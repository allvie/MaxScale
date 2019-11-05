/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-11-05
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

#include <maxscale/ccdefs.hh>
#include <unordered_set>
#include <maxscale/buffer.hh>
#include <maxscale/filter.hh>
#include "cache.hh"
#include "cachefilter.hh"
#include "cache_storage_api.hh"

class CacheFilterSession : public maxscale::FilterSession
{
    CacheFilterSession(const CacheFilterSession&);
    CacheFilterSession& operator=(const CacheFilterSession&);

public:
    enum cache_session_state_t
    {
        CACHE_EXPECTING_RESPONSE,       // A select has been sent, and we are waiting for the response.
        CACHE_EXPECTING_NOTHING,        // We are not expecting anything from the server.
        CACHE_EXPECTING_USE_RESPONSE,   // A "USE DB" was issued.
        CACHE_STORING_RESPONSE,         // A select has been sent, and we are storing the data.
        CACHE_IGNORING_RESPONSE,        // We are not interested in the data received from the server.
    };

    /**
     * Releases all resources held by the session cache.
     */
    ~CacheFilterSession();

    /**
     * Creates a CacheFilterSession instance.
     *
     * @param pCache     Pointer to the cache instance to which this session cache
     *                   belongs. Must remain valid for the lifetime of the CacheFilterSession
     *                   instance being created.
     * @param pSession   Pointer to the session this session cache instance is
     *                   specific for. Must remain valid for the lifetime of the CacheFilterSession
     *                   instance being created.
     *
     * @return A new instance or NULL if memory allocation fails.
     */
    static CacheFilterSession* create(Cache* pCache, MXS_SESSION* pSession, SERVICE* pService);

    /**
     * The session has been closed.
     */
    void close();

    /**
     * A request on its way to a backend is delivered to this function.
     *
     * @param pPacket  Buffer containing an MySQL protocol packet.
     */
    int routeQuery(GWBUF* pPacket);

    /**
     * A response on its way to the client is delivered to this function.
     *
     * @param pData Response data.
     */
    int clientReply(GWBUF* pPacket, const mxs::ReplyRoute& down, const mxs::Reply& reply);

    /**
     * Print diagnostics of the session cache.
     */
    void diagnostics(DCB* dcb);

    /**
     * Print diagnostics of the session cache.
     */
    json_t* diagnostics_json() const;

private:
    void handle_expecting_nothing(const mxs::Reply& reply);
    void handle_expecting_use_response(const mxs::Reply& reply);
    void handle_storing_response(const mxs::Reply& reply);
    void handle_ignoring_response();

    void send_upstream();

    void reset_response_state();

    bool log_decisions() const
    {
        return m_pCache->config().debug.is_set(CACHE_DEBUG_DECISIONS);
    }

    void store_result();

    enum cache_action_t
    {
        CACHE_IGNORE           = 0,
        CACHE_USE              = 1,
        CACHE_POPULATE         = 2,
        CACHE_USE_AND_POPULATE = (CACHE_USE | CACHE_POPULATE)
    };

    static bool should_use(cache_action_t action)
    {
        return action & CACHE_USE ? true : false;
    }

    static bool should_populate(cache_action_t action)
    {
        return action & CACHE_POPULATE ? true : false;
    }

    cache_action_t get_cache_action(GWBUF* pPacket);

    void update_table_names(GWBUF* pPacket);

    enum routing_action_t
    {
        ROUTING_ABORT,      /**< Abort normal routing activity, data is coming from cache. */
        ROUTING_CONTINUE,   /**< Continue normal routing activity. */
    };

    routing_action_t route_COM_QUERY(GWBUF* pPacket);
    routing_action_t route_SELECT(cache_action_t action, const CacheRules& rules, GWBUF* pPacket);

    char* set_cache_populate(const char* zName,
                             const char* pValue_begin,
                             const char* pValue_end);
    char* set_cache_use(const char* zName,
                        const char* pValue_begin,
                        const char* pValue_end);
    char* set_cache_soft_ttl(const char* zName,
                             const char* pValue_begin,
                             const char* pValue_end);
    char* set_cache_hard_ttl(const char* zName,
                             const char* pValue_begin,
                             const char* pValue_end);

    static char* set_cache_populate(void* pContext,
                                    const char* zName,
                                    const char* pValue_begin,
                                    const char* pValue_end);
    static char* set_cache_use(void* pContext,
                               const char* zName,
                               const char* pValue_begin,
                               const char* pValue_end);
    static char* set_cache_soft_ttl(void* pContext,
                                    const char* zName,
                                    const char* pValue_begin,
                                    const char* pValue_end);
    static char* set_cache_hard_ttl(void* pContext,
                                    const char* zName,
                                    const char* pValue_begin,
                                    const char* pValue_end);

private:
    CacheFilterSession(MXS_SESSION* pSession, SERVICE* pService, Cache* pCache, char* zDefaultDb);

private:
    using Tables = std::unordered_set<std::string>;

    cache_session_state_t m_state;          /**< What state is the session in, what data is expected. */
    Cache*                m_pCache;         /**< The cache instance the session is associated with. */
    GWBUF*                m_res;            /**< The response buffer. */
    GWBUF*                m_next_response;  /**< The next response routed to the client. */
    CACHE_KEY             m_key;            /**< Key storage. */
    char*                 m_zDefaultDb;     /**< The default database. */
    char*                 m_zUseDb;         /**< Pending default database. Needs server response. */
    bool                  m_refreshing;     /**< Whether the session is updating a stale cache entry. */
    bool                  m_is_read_only;   /**< Whether the current trx has been read-only in pratice. */
    bool                  m_use;            /**< Whether the cache should be used in this session. */
    bool                  m_populate;       /**< Whether the cache should be populated in this session. */
    uint32_t              m_soft_ttl;       /**< The soft TTL used in the session. */
    uint32_t              m_hard_ttl;       /**< The hard TTL used in the session. */
    bool                  m_invalidate;     /**< Whether invalidation should be performed. */
    bool                  m_invalidate_now; /**< Should invalidation be done at next response. */
    Tables                m_tables;         /**< Tables selected or modified. */
    bool                  m_clear_cache;    /**< Whether the entire cache should be cleared. */
    bool                  m_user_specific;  /**< Whether a user specific cache should be used. */
};
