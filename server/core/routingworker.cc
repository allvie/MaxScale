/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/routingworker.hh>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <sstream>

#include <maxbase/atomic.hh>
#include <maxbase/semaphore.hh>
#include <maxbase/alloc.h>
#include <maxscale/cn_strings.hh>
#include <maxscale/config.hh>
#include <maxscale/clock.h>
#include <maxscale/limits.h>
#include <maxscale/mainworker.hh>
#include <maxscale/json_api.hh>
#include <maxscale/utils.hh>
#include <maxscale/statistics.hh>

#include "internal/modules.hh"
#include "internal/poll.hh"
#include "internal/server.hh"
#include "internal/service.hh"
#include "internal/session.hh"

#define WORKER_ABSENT_ID -1

using maxbase::Semaphore;
using maxbase::Worker;
using maxbase::WorkerLoad;
using maxscale::RoutingWorker;
using maxscale::Closer;
using std::vector;
using std::stringstream;

namespace
{

/**
 * Unit variables.
 */
struct this_unit
{
    bool            initialized;        // Whether the initialization has been performed.
    int             nWorkers;           // How many routing workers there are.
    RoutingWorker** ppWorkers;          // Array of routing worker instances.
    int             next_worker_id;     // Next worker id
    int  epoll_listener_fd; // Shared epoll descriptor for listening descriptors.
    int  id_main_worker;    // The id of the worker running in the main thread.
    int  id_min_worker;     // The smallest routing worker id.
    int  id_max_worker;     // The largest routing worker id.
    bool running;           // True if worker threads are running
} this_unit =
{
    false,              // initialized
    0,                  // nWorkers
    NULL,               // ppWorkers
    0,                  // next_worker_id
    -1,                 // epoll_listener_fd
    WORKER_ABSENT_ID,   // id_main_worker
    WORKER_ABSENT_ID,   // id_min_worker
    WORKER_ABSENT_ID,   // id_max_worker
    false,
};

int next_worker_id()
{
    return mxb::atomic::add(&this_unit.next_worker_id, 1, mxb::atomic::RELAXED);
}

thread_local struct this_thread
{
    int current_worker_id;      // The worker id of the current thread
} this_thread =
{
    WORKER_ABSENT_ID
};

/**
 * Calls thread_init on all loaded modules.
 *
 * @return True, if all modules were successfully initialized.
 */
bool modules_thread_init()
{
    bool initialized = false;

    MXS_MODULE_ITERATOR i = mxs_module_iterator_get(NULL);
    MXS_MODULE* module = NULL;

    while ((module = mxs_module_iterator_get_next(&i)) != NULL)
    {
        if (module->thread_init)
        {
            int rc = (module->thread_init)();

            if (rc != 0)
            {
                break;
            }
        }
    }

    if (module)
    {
        // If module is non-NULL it means that the initialization failed for
        // that module. We now need to call finish on all modules that were
        // successfully initialized.
        MXS_MODULE* failed_module = module;
        i = mxs_module_iterator_get(NULL);

        while ((module = mxs_module_iterator_get_next(&i)) != failed_module)
        {
            if (module->thread_finish)
            {
                (module->thread_finish)();
            }
        }
    }
    else
    {
        initialized = true;
    }

    return initialized;
}

/**
 * Calls thread_finish on all loaded modules.
 */
void modules_thread_finish()
{
    MXS_MODULE_ITERATOR i = mxs_module_iterator_get(NULL);
    MXS_MODULE* module = NULL;

    while ((module = mxs_module_iterator_get_next(&i)) != NULL)
    {
        if (module->thread_finish)
        {
            (module->thread_finish)();
        }
    }
}
}

namespace maxscale
{

RoutingWorker::PersistentEntry::PersistentEntry(BackendDCB* pDcb)
    : m_created(time(nullptr))
    , m_pDcb(pDcb)
{
    mxb_assert(m_pDcb);
}

RoutingWorker::PersistentEntry::~PersistentEntry()
{
    mxb_assert(!m_pDcb);
}

RoutingWorker::DCBHandler::DCBHandler(RoutingWorker* pOwner)
    : m_owner(*pOwner)
{
}

// Any activity on a backend DCB that is in the persistent pool, will
// cause the dcb to be evicted.
void RoutingWorker::DCBHandler::ready_for_reading(DCB* pDcb)
{
    m_owner.evict_dcb(static_cast<BackendDCB*>(pDcb));
}

void RoutingWorker::DCBHandler::write_ready(DCB* pDcb)
{
    m_owner.evict_dcb(static_cast<BackendDCB*>(pDcb));
}

void RoutingWorker::DCBHandler::error(DCB* pDcb)
{
    m_owner.evict_dcb(static_cast<BackendDCB*>(pDcb));
}

void RoutingWorker::DCBHandler::hangup(DCB* pDcb)
{
    m_owner.evict_dcb(static_cast<BackendDCB*>(pDcb));
}


RoutingWorker::RoutingWorker(mxb::WatchdogNotifier* pNotifier)
    : mxb::WatchedWorker(pNotifier)
    , m_id(next_worker_id())
    , m_pool_handler(this)
{
    MXB_POLL_DATA::handler = &RoutingWorker::epoll_instance_handler;
    MXB_POLL_DATA::owner = this;
}

RoutingWorker::~RoutingWorker()
{
}

// static
bool RoutingWorker::init(mxb::WatchdogNotifier* pNotifier)
{
    mxb_assert(!this_unit.initialized);

    this_unit.epoll_listener_fd = epoll_create(MAX_EVENTS);

    if (this_unit.epoll_listener_fd != -1)
    {
        int nWorkers = config_threadcount();
        RoutingWorker** ppWorkers = new(std::nothrow) RoutingWorker* [MXS_MAX_THREADS]();       // 0-inited
                                                                                                // array

        if (ppWorkers)
        {
            int id_main_worker = WORKER_ABSENT_ID;
            int id_min_worker = INT_MAX;
            int id_max_worker = INT_MIN;

            int i;
            for (i = 0; i < nWorkers; ++i)
            {
                RoutingWorker* pWorker = RoutingWorker::create(pNotifier, this_unit.epoll_listener_fd);

                if (pWorker)
                {
                    int id = pWorker->id();

                    // The first created worker will be the main worker.
                    if (id_main_worker == WORKER_ABSENT_ID)
                    {
                        id_main_worker = id;
                    }

                    if (id < id_min_worker)
                    {
                        id_min_worker = id;
                    }

                    if (id > id_max_worker)
                    {
                        id_max_worker = id;
                    }

                    ppWorkers[i] = pWorker;
                }
                else
                {
                    for (int j = i - 1; j >= 0; --j)
                    {
                        delete ppWorkers[j];
                    }

                    delete[] ppWorkers;
                    ppWorkers = NULL;
                    break;
                }
            }

            if (ppWorkers)
            {
                this_unit.ppWorkers = ppWorkers;
                this_unit.nWorkers = nWorkers;
                this_unit.id_main_worker = id_main_worker;
                this_unit.id_min_worker = id_min_worker;
                this_unit.id_max_worker = id_max_worker;

                this_unit.initialized = true;
            }
        }
        else
        {
            MXS_OOM();
            close(this_unit.epoll_listener_fd);
        }
    }
    else
    {
        MXS_ALERT("Could not allocate an epoll instance.");
    }

    return this_unit.initialized;
}

void RoutingWorker::finish()
{
    mxb_assert(this_unit.initialized);

    for (int i = this_unit.id_max_worker; i >= this_unit.id_min_worker; --i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        delete pWorker;
        this_unit.ppWorkers[i] = NULL;
    }

    delete[] this_unit.ppWorkers;
    this_unit.ppWorkers = NULL;

    close(this_unit.epoll_listener_fd);
    this_unit.epoll_listener_fd = 0;

    this_unit.initialized = false;
}

// static
bool RoutingWorker::add_shared_fd(int fd, uint32_t events, MXB_POLL_DATA* pData)
{
    bool rv = true;

    // This must be level-triggered. Since this is intended for listening
    // sockets and each worker will call accept() just once before going
    // back the epoll_wait(), using EPOLLET would mean that if there are
    // more clients to be accepted than there are threads returning from
    // epoll_wait() for an event, then some clients would be accepted only
    // when a new client has connected, thus causing a new EPOLLIN event.
    events &= ~EPOLLET;

    struct epoll_event ev;

    ev.events = events;
    ev.data.ptr = pData;

    // The main worker takes ownership of all shared fds
    pData->owner = RoutingWorker::get(RoutingWorker::MAIN);

    if (epoll_ctl(this_unit.epoll_listener_fd, EPOLL_CTL_ADD, fd, &ev) != 0)
    {
        Worker::resolve_poll_error(fd, errno, EPOLL_CTL_ADD);
        rv = false;
    }

    return rv;
}

// static
bool RoutingWorker::remove_shared_fd(int fd)
{
    bool rv = true;

    struct epoll_event ev = {};

    if (epoll_ctl(this_unit.epoll_listener_fd, EPOLL_CTL_DEL, fd, &ev) != 0)
    {
        Worker::resolve_poll_error(fd, errno, EPOLL_CTL_DEL);
        rv = false;
    }

    return rv;
}

bool mxs_worker_should_shutdown(MXB_WORKER* pWorker)
{
    return static_cast<RoutingWorker*>(pWorker)->should_shutdown();
}

RoutingWorker* RoutingWorker::get(int worker_id)
{
    mxb_assert(this_unit.initialized);

    if (worker_id == MAIN)
    {
        worker_id = this_unit.id_main_worker;
    }

    mxb_assert((worker_id >= this_unit.id_min_worker) && (worker_id <= this_unit.id_max_worker));

    return this_unit.ppWorkers[worker_id];
}

RoutingWorker* RoutingWorker::get_current()
{
    RoutingWorker* pWorker = NULL;

    int worker_id = get_current_id();

    if (worker_id != WORKER_ABSENT_ID)
    {
        pWorker = RoutingWorker::get(worker_id);
    }

    return pWorker;
}

int RoutingWorker::get_current_id()
{
    return this_thread.current_worker_id;
}

// static
bool RoutingWorker::start_workers()
{
    bool rv = true;

    for (int i = this_unit.id_min_worker; i <= this_unit.id_max_worker; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (!pWorker->start())
        {
            MXS_ALERT("Could not start routing worker %d of %d.", i, config_threadcount());
            rv = false;
            // At startup, so we don't even try to clean up.
            break;
        }
    }

    if (rv)
    {
        this_unit.running = true;
    }

    return rv;
}

// static
bool RoutingWorker::is_running()
{
    return this_unit.running;
}

// static
void RoutingWorker::join_workers()
{
    for (int i = this_unit.id_min_worker; i <= this_unit.id_max_worker; i++)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        pWorker->join();
    }

    this_unit.running = false;
}

RoutingWorker::SessionsById& RoutingWorker::session_registry()
{
    return m_sessions;
}

void RoutingWorker::destroy(DCB* pDcb)
{
    mxb_assert(pDcb->owner == this);

    m_zombies.push_back(pDcb);
}

/**
 * Close sessions that have been idle or write to the socket has taken for too long.
 *
 * If the time since a session last sent data is greater than the set connection_timeout
 * value in the service, it is disconnected. If the time since last write
 * to the socket is greater net_write_timeout the session is also disconnected.
 * The timeouts are disabled by default.
 */
void RoutingWorker::process_timeouts()
{
    if (mxs_clock() >= m_next_timeout_check)
    {
        /** Because the resolutions of the timeouts is one second, we only need to
         * check them once per second. One heartbeat is 100 milliseconds. */
        m_next_timeout_check = mxs_clock() + 10;

        for (DCB* pDcb : m_dcbs)
        {
            if (pDcb->role() == DCB::Role::CLIENT && pDcb->state() == DCB::State::POLLING)
            {
                SERVICE* service = pDcb->session()->service;

                if (service->config().conn_idle_timeout)
                {
                    int64_t idle = mxs_clock() - pDcb->last_read();
                    // Multiply by 10 to match conn_idle_timeout resolution
                    // to the 100 millisecond tics.
                    int64_t timeout = service->config().conn_idle_timeout * 10;

                    if (idle > timeout)
                    {
                        MXS_WARNING("Timing out '%s'@%s, idle for %.1f seconds",
                                    pDcb->session()->user().c_str(),
                                    pDcb->remote().c_str(),
                                    (float)idle / 10.f);
                        pDcb->session()->close_reason = SESSION_CLOSE_TIMEOUT;
                        pDcb->trigger_hangup_event();
                    }
                }

                if (service->config().net_write_timeout && pDcb->writeq_len() > 0)
                {
                    int64_t idle = mxs_clock() - pDcb->last_write();
                    // Multiply by 10 to match net_write_timeout resolution
                    // to the 100 millisecond tics.
                    if (idle > pDcb->service()->config().net_write_timeout * 10)
                    {
                        MXS_WARNING("network write timed out for '%s'@%s.",
                                    pDcb->session()->user().c_str(),
                                    pDcb->remote().c_str());
                        pDcb->session()->close_reason = SESSION_CLOSE_TIMEOUT;
                        pDcb->trigger_hangup_event();
                    }
                }
            }
        }
    }
}

void RoutingWorker::delete_zombies()
{
    // An algorithm cannot be used, as the final closing of a DCB may cause
    // other DCBs to be registered in the zombie queue.

    while (!m_zombies.empty())
    {
        DCB* pDcb = m_zombies.back();
        m_zombies.pop_back();

        DCB::Manager::call_destroy(pDcb);
    }
}

void RoutingWorker::add(DCB* pDcb)
{
    MXB_AT_DEBUG(auto rv = ) m_dcbs.insert(pDcb);
    mxb_assert(rv.second);
}

void RoutingWorker::remove(DCB* pDcb)
{
    auto it = m_dcbs.find(pDcb);
    mxb_assert(it != m_dcbs.end());
    m_dcbs.erase(it);
}

BackendDCB* RoutingWorker::get_backend_dcb(SERVER* pS, MXS_SESSION* pSession, mxs::Component* pUpstream)
{
    Server* pServer = static_cast<Server*>(pS);

    BackendDCB* pDcb = nullptr;

    if (pServer->persistent_conns_enabled() && pServer->is_running())
    {
        pDcb = get_backend_dcb_from_pool(pS, pSession, pUpstream);
    }

    if (!pDcb)
    {
        pDcb = BackendDCB::connect(pServer, pSession, this, pUpstream);
    }

    return pDcb;
}

BackendDCB* RoutingWorker::get_backend_dcb_from_pool(SERVER* pS,
                                                     MXS_SESSION* pSession,
                                                     mxs::Component* pUpstream)
{
    Server* pServer = static_cast<Server*>(pS);

    mxb_assert(pServer);

    BackendDCB* pDcb = nullptr;

    evict_dcbs(pServer, Evict::EXPIRED);

    PersistentEntries& persistent_entries = m_persistent_entries_by_server[pServer];

    while (!pDcb && !persistent_entries.empty())
    {
        pDcb = persistent_entries.front().release_dcb();
        persistent_entries.pop_front();
        mxb::atomic::add(&pServer->pool_stats.n_persistent, -1);

        // Put back the origininal handler.
        pDcb->set_handler(pDcb->protocol());
        auto ses = static_cast<Session*>(pSession);
        ses->link_backend_dcb(pDcb);

        if (pDcb->protocol()->reuse_connection(pDcb, pUpstream))
        {
            mxb::atomic::add(&pServer->pool_stats.n_from_pool, 1, mxb::atomic::RELAXED);
            mxb::atomic::add(&pServer->stats().n_current, 1, mxb::atomic::RELAXED);
        }
        else
        {
            MXS_WARNING("Failed to reuse a persistent connection.");
            m_evicting = true;

            if (pDcb->state() == DCB::State::POLLING)
            {
                pDcb->disable_events();
                pDcb->shutdown();
            }

            DCB::close(pDcb);
            pDcb = nullptr;

            m_evicting = false;
        }
    }

    if (pDcb)
    {
        // Put the dcb back to the regular book-keeping.
        mxb_assert(m_dcbs.find(pDcb) == m_dcbs.end());
        m_dcbs.insert(pDcb);
    }

    return pDcb;
}

bool RoutingWorker::can_be_destroyed(BackendDCB* pDcb)
{
    bool rv = true;

    // Are dcbs being evicted from the pool?
    if (!m_evicting)
    {
        // No, so it can potentially be added to the pool.
        Server* pServer = static_cast<Server*>(pDcb->server());
        int persistpoolmax = pServer->persistpoolmax();

        if (pDcb->state() == DCB::State::POLLING
            && pDcb->protocol()->established()
            && pDcb->session()
            && session_valid_for_pool(pDcb->session())
            && persistpoolmax > 0
            && pServer->is_running()
            && !pDcb->hanged_up()
            && evict_dcbs(pServer, Evict::EXPIRED) < persistpoolmax)
        {
            if (mxb::atomic::add_limited(&pServer->pool_stats.n_persistent, 1, persistpoolmax))
            {
                pDcb->clear();
                // Change the handler to one that will close the DCB in case there
                // is any activity on it.
                pDcb->set_handler(&m_pool_handler);

                PersistentEntries& persistent_entries = m_persistent_entries_by_server[pServer];

                persistent_entries.emplace_back(pDcb);

                // Remove the dcb from the regular book-keeping.
                auto it = m_dcbs.find(pDcb);
                mxb_assert(it != m_dcbs.end());
                m_dcbs.erase(it);

                MXB_AT_DEBUG(int rc =)mxb::atomic::add(&pServer->stats().n_current, -1, mxb::atomic::RELAXED);
                mxb_assert(rc > 0);

                rv = false;
            }
        }
    }

    return rv;
}

void RoutingWorker::evict_dcbs(Evict evict)
{
    for (auto& i : m_persistent_entries_by_server)
    {
        evict_dcbs(i.first, evict);
    }
}

int RoutingWorker::evict_dcbs(SERVER* pS, Evict evict)
{
    mxb_assert(!m_evicting);

    m_evicting = true;

    int count = 0;

    time_t now = time(nullptr);

    Server* pServer = static_cast<Server*>(pS);
    PersistentEntries& persistent_entries = m_persistent_entries_by_server[pServer];

    vector<BackendDCB*> to_be_evicted;

    if (!(pServer->status() & SERVER_RUNNING))
    {
        // The server is not running => unconditionally evict all related dcbs.
        evict = Evict::ALL;
    }

    auto persistmaxtime = pServer->persistmaxtime();
    auto persistpoolmax = pServer->persistpoolmax();

    auto j = persistent_entries.begin();

    while (j != persistent_entries.end())
    {
        PersistentEntry& entry = *j;

        bool hanged_up = entry.hanged_up();
        bool expired = (evict == Evict::ALL) || (now - entry.created() > persistmaxtime);
        bool too_many = (count > persistpoolmax);

        if (hanged_up || expired || too_many)
        {
            to_be_evicted.push_back(entry.release_dcb());
            j = persistent_entries.erase(j);
            mxb::atomic::add(&pServer->pool_stats.n_persistent, -1);
        }
        else
        {
            ++count;
            ++j;
        }
    }

    pServer->persistmax = MXS_MAX(pServer->persistmax, count);

    for (BackendDCB* pDcb : to_be_evicted)
    {
        close_pooled_dcb(pDcb);
    }

    m_evicting = false;

    return count;
}

void RoutingWorker::evict_dcb(BackendDCB* pDcb)
{
    mxb_assert(!m_evicting);

    m_evicting = true;

    PersistentEntries& persistent_entries = m_persistent_entries_by_server[pDcb->server()];

    // TODO: An issue that we need to do a linear search?
    auto i = std::find_if(persistent_entries.begin(),
                          persistent_entries.end(),
                          [pDcb](const PersistentEntry& entry) {
                              return entry.dcb() == pDcb;
                          });

    mxb_assert(i != persistent_entries.end());

    i->release_dcb();
    persistent_entries.erase(i);

    close_pooled_dcb(pDcb);

    m_evicting = false;
}

void RoutingWorker::close_pooled_dcb(BackendDCB* pDcb)
{
    mxb_assert(m_evicting);

    // Put the DCB back into the regular book-keeping.
    mxb_assert(m_dcbs.find(pDcb) == m_dcbs.end());
    m_dcbs.insert(pDcb);

    if (pDcb->state() == DCB::State::POLLING)
    {
        pDcb->disable_events();
        pDcb->shutdown();
    }

    // This will cause can_be_destroyed() to be called. However, the dcb will
    // not be considered for the pool since m_evicting is currently true.
    DCB::close(pDcb);
}

bool RoutingWorker::pre_run()
{
    this_thread.current_worker_id = m_id;

    bool rv = modules_thread_init() && qc_thread_init(QC_INIT_SELF);

    if (!rv)
    {
        MXS_ERROR("Could not perform thread initialization for all modules. Thread exits.");
        this_thread.current_worker_id = WORKER_ABSENT_ID;
    }

    return rv;
}

void RoutingWorker::post_run()
{
    evict_dcbs(Evict::ALL);

    modules_thread_finish();
    qc_thread_end(QC_INIT_SELF);
    // TODO: Add service_thread_finish().
    this_thread.current_worker_id = WORKER_ABSENT_ID;
}

/**
 * Creates a worker instance.
 * - Allocates the structure.
 * - Creates a pipe.
 * - Adds the read descriptor to the polling mechanism.
 *
 * @param pNotifier          The watchdog notifier.
 * @param epoll_listener_fd  The file descriptor of the epoll set to which listening
 *                           sockets will be placed.
 *
 * @return A worker instance if successful, otherwise NULL.
 */
// static
RoutingWorker* RoutingWorker::create(mxb::WatchdogNotifier* pNotifier, int epoll_listener_fd)
{
    RoutingWorker* pThis = new(std::nothrow) RoutingWorker(pNotifier);

    if (pThis)
    {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        MXB_POLL_DATA* pData = pThis;
        ev.data.ptr = pData;    // Necessary for pointer adjustment, otherwise downcast will not work.

        // The shared epoll instance descriptor is *not* added using EPOLLET (edge-triggered)
        // because we want it to be level-triggered. That way, as long as there is a single
        // active (accept() can be called) listening socket, epoll_wait() will return an event
        // for it. It must be like that because each worker will call accept() just once before
        // calling epoll_wait() again. The end result is that as long as the load of different
        // workers is roughly the same, the client connections will be distributed evenly across
        // the workers. If the load is not the same, then a worker with less load will get more
        // clients that a worker with more load.
        if (epoll_ctl(pThis->m_epoll_fd, EPOLL_CTL_ADD, epoll_listener_fd, &ev) == 0)
        {
            MXS_INFO("Epoll instance for listening sockets added to worker epoll instance.");
        }
        else
        {
            MXS_ERROR("Could not add epoll instance for listening sockets to "
                      "epoll instance of worker: %s",
                      mxs_strerror(errno));
            delete pThis;
            pThis = NULL;
        }
    }
    else
    {
        MXS_OOM();
    }

    return pThis;
}

void RoutingWorker::epoll_tick()
{
    process_timeouts();

    delete_zombies();

    for (auto& func : m_epoll_tick_funcs)
    {
        func();
    }
}

/**
 * Callback for events occurring on the shared epoll instance.
 *
 * @param pData   Will point to a Worker instance.
 * @param wid     The worker id.
 * @param events  The events.
 *
 * @return What actions were performed.
 */
// static
uint32_t RoutingWorker::epoll_instance_handler(MXB_POLL_DATA* pData, MXB_WORKER* pWorker, uint32_t events)
{
    RoutingWorker* pThis = static_cast<RoutingWorker*>(pData);
    mxb_assert(pThis == pWorker);

    return pThis->handle_epoll_events(events);
}

/**
 * Handler for events occurring in the shared epoll instance.
 *
 * @param events  The events.
 *
 * @return What actions were performed.
 */
uint32_t RoutingWorker::handle_epoll_events(uint32_t events)
{
    struct epoll_event epoll_events[1];

    // We extract just one event
    int nfds = epoll_wait(this_unit.epoll_listener_fd, epoll_events, 1, 0);

    uint32_t actions = MXB_POLL_NOP;

    if (nfds == -1)
    {
        MXS_ERROR("epoll_wait failed: %s", mxs_strerror(errno));
    }
    else if (nfds == 0)
    {
        MXS_DEBUG("No events for worker %d.", m_id);
    }
    else
    {
        MXS_DEBUG("1 event for worker %d.", m_id);
        MXB_POLL_DATA* pData = static_cast<MXB_POLL_DATA*>(epoll_events[0].data.ptr);

        actions = pData->handler(pData, this, epoll_events[0].events);
    }

    return actions;
}

// static
size_t RoutingWorker::broadcast(Task* pTask, Semaphore* pSem)
{
    // No logging here, function must be signal safe.
    size_t n = 0;

    int nWorkers = this_unit.next_worker_id;
    for (int i = 0; i < nWorkers; ++i)
    {
        Worker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->execute(pTask, pSem, EXECUTE_AUTO))
        {
            ++n;
        }
    }

    return n;
}

// static
size_t RoutingWorker::broadcast(std::unique_ptr<DisposableTask> sTask)
{
    DisposableTask* pTask = sTask.release();
    Worker::inc_ref(pTask);

    size_t n = 0;

    int nWorkers = this_unit.next_worker_id;
    for (int i = 0; i < nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->post_disposable(pTask, EXECUTE_AUTO))
        {
            ++n;
        }
    }

    Worker::dec_ref(pTask);

    return n;
}

// static
size_t RoutingWorker::broadcast(std::function<void ()> func,
                                mxb::Semaphore* pSem,
                                mxb::Worker::execute_mode_t mode)
{
    size_t n = 0;
    int nWorkers = this_unit.next_worker_id;

    for (int i = 0; i < nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->execute(func, pSem, mode))
        {
            ++n;
        }
    }

    return n;
}

// static
size_t RoutingWorker::execute_serially(Task& task)
{
    Semaphore sem;
    size_t n = 0;

    int nWorkers = this_unit.next_worker_id;
    for (int i = 0; i < nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->execute(&task, &sem, EXECUTE_AUTO))
        {
            sem.wait();
            ++n;
        }
    }

    return n;
}

// static
size_t RoutingWorker::execute_serially(std::function<void()> func)
{
    Semaphore sem;
    size_t n = 0;

    int nWorkers = this_unit.next_worker_id;
    for (int i = 0; i < nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->execute(func, &sem, EXECUTE_AUTO))
        {
            sem.wait();
            ++n;
        }
    }

    return n;
}

// static
size_t RoutingWorker::execute_concurrently(Task& task)
{
    Semaphore sem;
    return sem.wait_n(RoutingWorker::broadcast(&task, &sem));
}

// static
size_t RoutingWorker::execute_concurrently(std::function<void()> func)
{
    Semaphore sem;
    return sem.wait_n(RoutingWorker::broadcast(func, &sem, EXECUTE_AUTO));
}

// static
size_t RoutingWorker::broadcast_message(uint32_t msg_id, intptr_t arg1, intptr_t arg2)
{
    // NOTE: No logging here, this function must be signal safe.

    size_t n = 0;

    int nWorkers = this_unit.next_worker_id;
    for (int i = 0; i < nWorkers; ++i)
    {
        Worker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->post_message(msg_id, arg1, arg2))
        {
            ++n;
        }
    }

    return n;
}

// static
void RoutingWorker::shutdown_all()
{
    // NOTE: No logging here, this function must be signal safe.
    mxb_assert((this_unit.next_worker_id == 0) || (this_unit.ppWorkers != NULL));

    int nWorkers = this_unit.next_worker_id;
    for (int i = 0; i < nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        pWorker->shutdown();
    }
}

namespace
{

std::vector<Worker::STATISTICS> get_stats()
{
    std::vector<Worker::STATISTICS> rval;

    int nWorkers = this_unit.next_worker_id;

    for (int i = 0; i < nWorkers; ++i)
    {
        RoutingWorker* pWorker = RoutingWorker::get(i);
        mxb_assert(pWorker);

        rval.push_back(pWorker->statistics());
    }

    return rval;
}
}

// static
Worker::STATISTICS RoutingWorker::get_statistics()
{
    auto s = get_stats();

    STATISTICS cs;

    cs.n_read = mxs::sum(s, &STATISTICS::n_read);
    cs.n_write = mxs::sum(s, &STATISTICS::n_write);
    cs.n_error = mxs::sum(s, &STATISTICS::n_error);
    cs.n_hup = mxs::sum(s, &STATISTICS::n_hup);
    cs.n_accept = mxs::sum(s, &STATISTICS::n_accept);
    cs.n_polls = mxs::sum(s, &STATISTICS::n_polls);
    cs.n_pollev = mxs::sum(s, &STATISTICS::n_pollev);
    cs.evq_avg = mxs::avg(s, &STATISTICS::evq_avg);
    cs.evq_max = mxs::max(s, &STATISTICS::evq_max);
    cs.maxqtime = mxs::max(s, &STATISTICS::maxqtime);
    cs.maxexectime = mxs::max(s, &STATISTICS::maxexectime);
    cs.n_fds = mxs::sum_element(s, &STATISTICS::n_fds);
    cs.n_fds = mxs::min_element(s, &STATISTICS::n_fds);
    cs.n_fds = mxs::max_element(s, &STATISTICS::n_fds);
    cs.qtimes = mxs::avg_element(s, &STATISTICS::qtimes);
    cs.exectimes = mxs::avg_element(s, &STATISTICS::exectimes);

    return cs;
}

// static
int64_t RoutingWorker::get_one_statistic(POLL_STAT what)
{
    auto s = get_stats();

    int64_t rv = 0;

    switch (what)
    {
    case POLL_STAT_READ:
        rv = mxs::sum(s, &STATISTICS::n_read);
        break;

    case POLL_STAT_WRITE:
        rv = mxs::sum(s, &STATISTICS::n_write);
        break;

    case POLL_STAT_ERROR:
        rv = mxs::sum(s, &STATISTICS::n_error);
        break;

    case POLL_STAT_HANGUP:
        rv = mxs::sum(s, &STATISTICS::n_hup);
        break;

    case POLL_STAT_ACCEPT:
        rv = mxs::sum(s, &STATISTICS::n_accept);
        break;

    case POLL_STAT_EVQ_AVG:
        rv = mxs::avg(s, &STATISTICS::evq_avg);
        break;

    case POLL_STAT_EVQ_MAX:
        rv = mxs::max(s, &STATISTICS::evq_max);
        break;

    case POLL_STAT_MAX_QTIME:
        rv = mxs::max(s, &STATISTICS::maxqtime);
        break;

    case POLL_STAT_MAX_EXECTIME:
        rv = mxs::max(s, &STATISTICS::maxexectime);
        break;

    default:
        mxb_assert(!true);
    }

    return rv;
}

// static
bool RoutingWorker::get_qc_stats(int id, QC_CACHE_STATS* pStats)
{
    class Task : public Worker::Task
    {
    public:
        Task(QC_CACHE_STATS* pStats)
            : m_stats(*pStats)
        {
        }

        void execute(Worker&)
        {
            qc_get_cache_stats(&m_stats);
        }

    private:
        QC_CACHE_STATS& m_stats;
    };

    RoutingWorker* pWorker = RoutingWorker::get(id);

    if (pWorker)
    {
        Semaphore sem;
        Task task(pStats);
        pWorker->execute(&task, &sem, EXECUTE_AUTO);
        sem.wait();
    }

    return pWorker != nullptr;
}

// static
void RoutingWorker::get_qc_stats(std::vector<QC_CACHE_STATS>& all_stats)
{
    class Task : public Worker::Task
    {
    public:
        Task(std::vector<QC_CACHE_STATS>* pAll_stats)
            : m_all_stats(*pAll_stats)
        {
            m_all_stats.resize(config_threadcount());
        }

        void execute(Worker& worker)
        {
            int id = mxs::RoutingWorker::get_current_id();
            mxb_assert(id >= 0);

            QC_CACHE_STATS& stats = m_all_stats[id];

            qc_get_cache_stats(&stats);
        }

    private:
        std::vector<QC_CACHE_STATS>& m_all_stats;
    };

    Task task(&all_stats);
    mxs::RoutingWorker::execute_concurrently(task);
}

namespace
{

json_t* qc_stats_to_json(const char* zHost, int id, const QC_CACHE_STATS& stats)
{
    json_t* pStats = json_object();
    json_object_set_new(pStats, "size", json_integer(stats.size));
    json_object_set_new(pStats, "inserts", json_integer(stats.inserts));
    json_object_set_new(pStats, "hits", json_integer(stats.hits));
    json_object_set_new(pStats, "misses", json_integer(stats.misses));
    json_object_set_new(pStats, "evictions", json_integer(stats.evictions));

    json_t* pAttributes = json_object();
    json_object_set_new(pAttributes, "stats", pStats);

    json_t* pSelf = mxs_json_self_link(zHost, "qc_stats", std::to_string(id).c_str());

    json_t* pJson = json_object();
    json_object_set_new(pJson, CN_ID, json_string(std::to_string(id).c_str()));
    json_object_set_new(pJson, CN_TYPE, json_string("qc_stats"));
    json_object_set_new(pJson, CN_ATTRIBUTES, pAttributes);
    json_object_set_new(pJson, CN_LINKS, pSelf);

    return pJson;
}
}

// static
std::unique_ptr<json_t> RoutingWorker::get_qc_stats_as_json(const char* zHost, int id)
{
    std::unique_ptr<json_t> sStats;

    QC_CACHE_STATS stats;

    if (get_qc_stats(id, &stats))
    {
        json_t* pJson = qc_stats_to_json(zHost, id, stats);

        stringstream self;
        self << MXS_JSON_API_QC_STATS << id;

        sStats.reset(mxs_json_resource(zHost, self.str().c_str(), pJson));
    }

    return sStats;
}

// static
std::unique_ptr<json_t> RoutingWorker::get_qc_stats_as_json(const char* zHost)
{
    vector<QC_CACHE_STATS> all_stats;

    get_qc_stats(all_stats);

    std::unique_ptr<json_t> sAll_stats(json_array());

    int id = 0;
    for (const auto& stats : all_stats)
    {
        json_t* pJson = qc_stats_to_json(zHost, id, stats);

        json_array_append_new(sAll_stats.get(), pJson);
        ++id;
    }

    return std::unique_ptr<json_t>(mxs_json_resource(zHost, MXS_JSON_API_QC_STATS, sAll_stats.release()));
}

// static
RoutingWorker* RoutingWorker::pick_worker()
{
    static int id_generator = 0;
    int id = this_unit.id_min_worker
        + (mxb::atomic::add(&id_generator, 1, mxb::atomic::RELAXED) % this_unit.nWorkers);
    return get(id);
}

void maxscale::RoutingWorker::register_epoll_tick_func(std::function<void ()> func)
{
    m_epoll_tick_funcs.push_back(func);
}

}

size_t mxs_rworker_broadcast_message(uint32_t msg_id, intptr_t arg1, intptr_t arg2)
{
    return RoutingWorker::broadcast_message(msg_id, arg1, arg2);
}

bool mxs_rworker_register_session(MXS_SESSION* session)
{
    RoutingWorker* pWorker = RoutingWorker::get_current();
    mxb_assert(pWorker);
    return pWorker->session_registry().add(session);
}

bool mxs_rworker_deregister_session(MXS_SESSION* session)
{
    RoutingWorker* pWorker = RoutingWorker::get_current();
    mxb_assert(pWorker);
    return pWorker->session_registry().remove(session->id());
}

MXS_SESSION* mxs_rworker_find_session(uint64_t id)
{
    RoutingWorker* pWorker = RoutingWorker::get_current();
    mxb_assert(pWorker);
    return pWorker->session_registry().lookup(id);
}

MXB_WORKER* mxs_rworker_get(int worker_id)
{
    return RoutingWorker::get(worker_id);
}

MXB_WORKER* mxs_rworker_get_current()
{
    return RoutingWorker::get_current();
}

int mxs_rworker_get_current_id()
{
    return RoutingWorker::get_current_id();
}

namespace
{

using namespace maxscale;

class WorkerInfoTask : public Worker::Task
{
public:
    WorkerInfoTask(const char* zHost, uint32_t nThreads)
        : m_zHost(zHost)
    {
        m_data.resize(nThreads);
    }

    void execute(Worker& worker)
    {
        RoutingWorker& rworker = static_cast<RoutingWorker&>(worker);

        json_t* pStats = json_object();
        const Worker::STATISTICS& s = rworker.statistics();
        json_object_set_new(pStats, "reads", json_integer(s.n_read));
        json_object_set_new(pStats, "writes", json_integer(s.n_write));
        json_object_set_new(pStats, "errors", json_integer(s.n_error));
        json_object_set_new(pStats, "hangups", json_integer(s.n_hup));
        json_object_set_new(pStats, "accepts", json_integer(s.n_accept));
        json_object_set_new(pStats, "avg_event_queue_length", json_integer(s.evq_avg));
        json_object_set_new(pStats, "max_event_queue_length", json_integer(s.evq_max));
        json_object_set_new(pStats, "max_exec_time", json_integer(s.maxexectime));
        json_object_set_new(pStats, "max_queue_time", json_integer(s.maxqtime));

        uint32_t nCurrent;
        uint64_t nTotal;
        rworker.get_descriptor_counts(&nCurrent, &nTotal);
        json_object_set_new(pStats, "current_descriptors", json_integer(nCurrent));
        json_object_set_new(pStats, "total_descriptors", json_integer(nTotal));

        json_t* load = json_object();
        json_object_set_new(load, "last_second", json_integer(rworker.load(Worker::Load::ONE_SECOND)));
        json_object_set_new(load, "last_minute", json_integer(rworker.load(Worker::Load::ONE_MINUTE)));
        json_object_set_new(load, "last_hour", json_integer(rworker.load(Worker::Load::ONE_HOUR)));
        json_object_set_new(pStats, "load", load);

        json_t* qc = qc_get_cache_stats_as_json();

        if (qc)
        {
            json_object_set_new(pStats, "query_classifier_cache", qc);
        }

        json_t* pAttr = json_object();
        json_object_set_new(pAttr, "stats", pStats);

        int idx = rworker.id();
        stringstream ss;
        ss << idx;

        json_t* pJson = json_object();
        json_object_set_new(pJson, CN_ID, json_string(ss.str().c_str()));
        json_object_set_new(pJson, CN_TYPE, json_string(CN_THREADS));
        json_object_set_new(pJson, CN_ATTRIBUTES, pAttr);
        json_object_set_new(pJson, CN_LINKS, mxs_json_self_link(m_zHost, CN_THREADS, ss.str().c_str()));

        mxb_assert((size_t)idx < m_data.size());
        m_data[idx] = pJson;
    }

    json_t* resource()
    {
        json_t* pArr = json_array();

        for (auto it = m_data.begin(); it != m_data.end(); it++)
        {
            json_array_append_new(pArr, *it);
        }

        return mxs_json_resource(m_zHost, MXS_JSON_API_THREADS, pArr);
    }

    json_t* resource(int id)
    {
        stringstream self;
        self << MXS_JSON_API_THREADS << id;
        return mxs_json_resource(m_zHost, self.str().c_str(), m_data[id]);
    }

private:
    vector<json_t*> m_data;
    const char*     m_zHost;
};

class FunctionTask : public Worker::DisposableTask
{
public:
    FunctionTask(std::function<void ()> cb)
        : m_cb(cb)
    {
    }

    void execute(Worker& worker)
    {
        m_cb();
    }

protected:
    std::function<void ()> m_cb;
};
}

size_t mxs_rworker_broadcast(void (* cb)(void* data), void* data)
{
    std::unique_ptr<FunctionTask> task(new FunctionTask([cb, data]() {
                                                            cb(data);
                                                        }));

    return RoutingWorker::broadcast(std::move(task));
}

json_t* mxs_rworker_to_json(const char* zHost, int id)
{
    Worker* target = RoutingWorker::get(id);
    WorkerInfoTask task(zHost, id + 1);
    Semaphore sem;

    target->execute(&task, &sem, Worker::EXECUTE_AUTO);
    sem.wait();

    return task.resource(id);
}

json_t* mxs_rworker_list_to_json(const char* host)
{
    WorkerInfoTask task(host, config_threadcount());
    RoutingWorker::execute_concurrently(task);
    return task.resource();
}

namespace
{

class WatchdogTask : public Worker::Task
{
public:
    WatchdogTask()
    {
    }

    void execute(Worker& worker)
    {
        // Success if this is called.
    }
};
}

void mxs_rworker_watchdog()
{
    MXS_INFO("MaxScale watchdog called.");
    WatchdogTask task;
    RoutingWorker::execute_concurrently(task);
}
