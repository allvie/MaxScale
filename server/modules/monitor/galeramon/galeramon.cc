/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file galera_mon.c - A MySQL Galera cluster monitor
 */

#define MXS_MODULE_NAME "galeramon"

#include "galeramon.hh"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql.h>
#include <mysqld_error.h>
#include <maxscale/alloc.h>
#include <maxscale/config.hh>
#include <maxscale/dcb.hh>
#include <maxscale/modinfo.h>
#include <maxscale/mysql_utils.hh>
#include <maxscale/secrets.h>

#define DONOR_NODE_NAME_MAX_LEN 60
#define DONOR_LIST_SET_VAR      "SET GLOBAL wsrep_sst_donor = \""

using maxscale::MXS_MONITORED_SERVER;

/** Log a warning when a bad 'wsrep_local_index' is found */
static bool warn_erange_on_local_index = true;

static MXS_MONITORED_SERVER* set_cluster_master(MXS_MONITORED_SERVER*, MXS_MONITORED_SERVER*, int);
static void                  disableMasterFailback(void*, int);
static int                   compare_node_index(const void*, const void*);
static int                   compare_node_priority(const void*, const void*);
static bool                  using_xtrabackup(MXS_MONITORED_SERVER* database, const char* server_string);

GaleraMonitor::GaleraMonitor(const std::string& name, const std::string& module)
    : MonitorWorkerSimple(name, module)
    , m_disableMasterFailback(0)
    , m_availableWhenDonor(0)
    , m_disableMasterRoleSetting(0)
    , m_root_node_as_master(false)
    , m_use_priority(false)
    , m_set_donor_nodes(false)
    , m_log_no_members(false)
    , m_cluster_size(0)
{
}

GaleraMonitor::~GaleraMonitor()
{
}

// static
GaleraMonitor* GaleraMonitor::create(const std::string& name, const std::string& module)
{
    return new GaleraMonitor(name, module);
}

void GaleraMonitor::diagnostics(DCB* dcb) const
{
    dcb_printf(dcb, "Master Failback:\t%s\n", (m_disableMasterFailback == 1) ? "off" : "on");
    dcb_printf(dcb, "Available when Donor:\t%s\n", (m_availableWhenDonor == 1) ? "on" : "off");
    dcb_printf(dcb,
               "Master Role Setting Disabled:\t%s\n",
               m_disableMasterRoleSetting ? "on" : "off");
    dcb_printf(dcb, "Set wsrep_sst_donor node list:\t%s\n", (m_set_donor_nodes == 1) ? "on" : "off");
    if (!m_cluster_uuid.empty())
    {
        dcb_printf(dcb, "Galera Cluster UUID:\t%s\n", m_cluster_uuid.c_str());
        dcb_printf(dcb, "Galera Cluster size:\t%d\n", m_cluster_size);
    }
    else
    {
        dcb_printf(dcb, "Galera Cluster NOT set:\tno member nodes\n");
    }
}

json_t* GaleraMonitor::diagnostics_json() const
{
    json_t* rval = MonitorWorker::diagnostics_json();
    json_object_set_new(rval, "disable_master_failback", json_boolean(m_disableMasterFailback));
    json_object_set_new(rval, "disable_master_role_setting", json_boolean(m_disableMasterRoleSetting));
    json_object_set_new(rval, "root_node_as_master", json_boolean(m_root_node_as_master));
    json_object_set_new(rval, "use_priority", json_boolean(m_use_priority));
    json_object_set_new(rval, "set_donor_nodes", json_boolean(m_set_donor_nodes));

    if (!m_cluster_uuid.empty())
    {
        json_object_set_new(rval, "cluster_uuid", json_string(m_cluster_uuid.c_str()));
        json_object_set_new(rval, "cluster_size", json_integer(m_cluster_size));
    }

    return rval;
}

bool GaleraMonitor::configure(const MXS_CONFIG_PARAMETER* params)
{
    if (!MonitorWorkerSimple::configure(params))
    {
        return false;
    }

    m_disableMasterFailback = params->get_bool("disable_master_failback");
    m_availableWhenDonor = params->get_bool("available_when_donor");
    m_disableMasterRoleSetting = params->get_bool("disable_master_role_setting");
    m_root_node_as_master = params->get_bool("root_node_as_master");
    m_use_priority = params->get_bool("use_priority");
    m_set_donor_nodes = params->get_bool("set_donor_nodes");
    m_log_no_members = true;

    /* Reset all data in the hashtable */
    m_info.clear();

    return true;
}

bool GaleraMonitor::has_sufficient_permissions()
{
    return test_permissions("SHOW STATUS LIKE 'wsrep_local_state'");
}

void GaleraMonitor::update_server_status(MXS_MONITORED_SERVER* monitored_server)
{
    MYSQL_ROW row;
    MYSQL_RES* result;

    /* get server version string */
    mxs_mysql_update_server_version(monitored_server->server, monitored_server->con);
    auto server_string = monitored_server->server->version_string();

    /* Check if the the Galera FSM shows this node is joined to the cluster */
    const char* cluster_member =
        "SHOW STATUS WHERE Variable_name IN"
        " ('wsrep_cluster_state_uuid',"
        " 'wsrep_cluster_size',"
        " 'wsrep_local_index',"
        " 'wsrep_local_state')";

    if (mxs_mysql_query(monitored_server->con, cluster_member) == 0
        && (result = mysql_store_result(monitored_server->con)) != NULL)
    {
        if (mysql_field_count(monitored_server->con) < 2)
        {
            mysql_free_result(result);
            MXS_ERROR("Unexpected result for \"%s\". "
                      "Expected 2 columns. MySQL Version: %s",
                      cluster_member,
                      server_string.c_str());
            return;
        }
        GaleraNode info = {};
        while ((row = mysql_fetch_row(result)))
        {
            if (strcmp(row[0], "wsrep_cluster_size") == 0)
            {
                info.cluster_size = atoi(row[1]);
            }

            if (strcmp(row[0], "wsrep_local_index") == 0)
            {
                char* endchar;
                long local_index = strtol(row[1], &endchar, 10);
                if (*endchar != '\0'
                    || (errno == ERANGE && (local_index == LONG_MAX || local_index == LONG_MIN)))
                {
                    if (warn_erange_on_local_index)
                    {
                        MXS_WARNING("Invalid 'wsrep_local_index' on server '%s': %s",
                                    monitored_server->server->name(),
                                    row[1]);
                        warn_erange_on_local_index = false;
                    }
                    local_index = -1;
                    /* Force joined = 0 */
                    info.joined = 0;
                }

                info.local_index = local_index;
            }

            mxb_assert(row[0] && row[1]);

            if (strcmp(row[0], "wsrep_local_state") == 0)
            {
                if (strcmp(row[1], "4") == 0)
                {
                    info.joined = 1;
                }
                /* Check if the node is a donor and is using xtrabackup, in this case it can stay alive */
                else if (strcmp(row[1], "2") == 0 && m_availableWhenDonor == 1
                         && using_xtrabackup(monitored_server, server_string.c_str()))
                {
                    info.joined = 1;
                }
                else
                {
                    /* Force joined = 0 */
                    info.joined = 0;
                }

                info.local_state = atoi(row[1]);
            }

            if (strcmp(row[0], "wsrep_cluster_state_uuid") == 0 && row[1] && *row[1])
            {
                info.cluster_uuid = row[1];
            }
        }

        monitored_server->server->node_id = info.joined ? info.local_index : -1;

        m_info[monitored_server] = info;

        mysql_free_result(result);
    }
    else
    {
        monitored_server->mon_report_query_error();
    }
}

void GaleraMonitor::pre_tick()
{
    // Clear the info before monitoring to make sure it's up to date
    m_info.clear();
}

void GaleraMonitor::post_tick()
{
    int is_cluster = 0;

    /* Try to set a Galera cluster based on
     * UUID and cluster_size each node reports:
     * no multiple clusters UUID are allowed.
     */
    set_galera_cluster();

    /*
     * Let's select a master server:
     * it could be the candidate master following MXS_MIN(node_id) rule or
     * the server that was master in the previous monitor polling cycle
     * Decision depends on master_stickiness value set in configuration
     */

    /* get the candidate master, following MXS_MIN(node_id) rule */
    MXS_MONITORED_SERVER* candidate_master = get_candidate_master();

    m_master = set_cluster_master(m_master, candidate_master, m_disableMasterFailback);

    for (auto ptr : m_servers)
    {
        const int repl_bits = (SERVER_SLAVE | SERVER_MASTER | SERVER_MASTER_STICKINESS);
        if ((ptr->pending_status & SERVER_JOINED) && !m_disableMasterRoleSetting)
        {
            if (ptr != m_master)
            {
                /* set the Slave role and clear master stickiness */
                ptr->clear_pending_status(repl_bits);
                ptr->set_pending_status(SERVER_SLAVE);
            }
            else
            {
                if (candidate_master
                    && m_master->server->node_id != candidate_master->server->node_id)
                {
                    /* set master role and master stickiness */
                    ptr->clear_pending_status(repl_bits);
                    ptr->set_pending_status(SERVER_MASTER | SERVER_MASTER_STICKINESS);
                }
                else
                {
                    /* set master role and clear master stickiness */
                    ptr->clear_pending_status(repl_bits);
                    ptr->set_pending_status(SERVER_MASTER);
                }
            }

            is_cluster++;
        }
        else
        {
            ptr->clear_pending_status(repl_bits);
            ptr->set_pending_status(0);
        }
    }

    if (is_cluster == 0 && m_log_no_members)
    {
        MXS_ERROR("There are no cluster members");
        m_log_no_members = false;
    }
    else
    {
        if (is_cluster > 0 && m_log_no_members == 0)
        {
            MXS_NOTICE("Found cluster members");
            m_log_no_members = true;
        }
    }

    /* Set the global var "wsrep_sst_donor"
     * with a sorted list of "wsrep_node_name" for slave nodes
     */
    if (m_set_donor_nodes)
    {
        update_sst_donor_nodes(is_cluster);
    }
}

static bool using_xtrabackup(MXS_MONITORED_SERVER* database, const char* server_string)
{
    bool rval = false;
    MYSQL_RES* result;

    if (mxs_mysql_query(database->con, "SHOW VARIABLES LIKE 'wsrep_sst_method'") == 0
        && (result = mysql_store_result(database->con)) != NULL)
    {
        if (mysql_field_count(database->con) < 2)
        {
            mysql_free_result(result);
            MXS_ERROR("Unexpected result for \"SHOW VARIABLES LIKE "
                      "'wsrep_sst_method'\". Expected 2 columns."
                      " MySQL Version: %s",
                      server_string);
        }

        MYSQL_ROW row;

        while ((row = mysql_fetch_row(result)))
        {
            if (row[1] && (strcmp(row[1], "xtrabackup") == 0
                           || strcmp(row[1], "mariabackup") == 0
                           || strcmp(row[1], "xtrabackup-v2") == 0))
            {
                rval = true;
            }
        }
        mysql_free_result(result);
    }
    else
    {
        database->mon_report_query_error();
    }

    return rval;
}

/**
 * get candidate master from all nodes
 *
 * The current available rule: get the server with MXS_MIN(node_id)
 * node_id comes from 'wsrep_local_index' variable
 *
 * @param   servers The monitored servers list
 * @return  The candidate master on success, NULL on failure
 */
MXS_MONITORED_SERVER* GaleraMonitor::get_candidate_master()
{
    MXS_MONITORED_SERVER* candidate_master = NULL;
    long min_id = -1;
    int minval = INT_MAX;
    int currval;
    /* set min_id to the lowest value of moitor_servers->server->node_id */
    for (auto moitor_servers : m_servers)
    {
        if (!moitor_servers->server->is_in_maint()
            && (moitor_servers->pending_status & SERVER_JOINED))
        {
            if (m_use_priority)
            {
                std::string buf = moitor_servers->server->get_custom_parameter("priority");
                if (!buf.empty())
                {
                    /** The server has a priority  */
                    if ((currval = atoi(buf.c_str())) > 0)
                    {
                        /** The priority is valid */
                        if (currval < minval && currval > 0)
                        {
                            minval = currval;
                            candidate_master = moitor_servers;
                        }
                    }
                }
            }
            else if (moitor_servers->server->node_id >= 0
                     && (!m_use_priority || candidate_master == NULL))
            {
                // Server priorities are not in use or no candidate has been found
                if (min_id < 0 || moitor_servers->server->node_id < min_id)
                {
                    min_id = moitor_servers->server->node_id;
                    candidate_master = moitor_servers;
                }
            }
        }
    }

    if (!m_use_priority && !m_disableMasterFailback
        && m_root_node_as_master && min_id > 0)
    {
        /** The monitor couldn't find the node with wsrep_local_index of 0.
         * This means that we can't connect to the root node of the cluster.
         *
         * If the node is down, the cluster would recalculate the index values
         * and we would find it. In this case, we just can't connect to it.
         */

        candidate_master = NULL;
    }

    return candidate_master;
}

/**
 * set the master server in the cluster
 *
 * master could be the last one from previous monitor cycle Iis running) or
 * the candidate master.
 * The selection is based on the configuration option mapped to master_stickiness
 * The candidate master may change over time due to
 * 'wsrep_local_index' value change in the Galera Cluster
 * Enabling master_stickiness will avoid master change unless a failure is spotted
 *
 * @param   current_master Previous master server
 * @param   candidate_master The candidate master server accordingly to the selection rule
 * @return  The  master node pointer (could be NULL)
 */
static MXS_MONITORED_SERVER* set_cluster_master(MXS_MONITORED_SERVER* current_master,
                                                MXS_MONITORED_SERVER* candidate_master,
                                                int master_stickiness)
{
    /*
     * if current master is not set or master_stickiness is not enable
     * just return candidate_master.
     */
    if (current_master == NULL || master_stickiness == 0)
    {
        return candidate_master;
    }
    else
    {
        /*
         * if current_master is still a cluster member use it
         *
         */
        if ((current_master->pending_status & SERVER_JOINED)
            && (!current_master->server->is_in_maint()))
        {
            return current_master;
        }
        else
        {
            return candidate_master;
        }
    }
}

/**
 * Set the global variable wsrep_sst_donor in the cluster
 *
 * The monitor user must have the privileges for setting global vars.
 *
 * Galera monitor fetches from each joined slave node the var 'wsrep_node_name'
 * A list of nodes is automatically build and it's sorted by wsrep_local_index DESC
 * or by priority ASC if use_priority option is set.
 *
 * The list is then added to SET GLOBAL VARIABLE wrep_sst_donor =
 * The variable must be sent to all slave nodes.
 *
 * All slave nodes have a sorted list of nodes tht can be used as donor nodes.
 *
 * If there is only one node the funcion returns,
 *
 * @param   mon        The monitor handler
 * @param   is_cluster The number of joined nodes
 */
void GaleraMonitor::update_sst_donor_nodes(int is_cluster)
{
    MXS_MONITORED_SERVER* ptr;
    MYSQL_ROW row;
    MYSQL_RES* result;
    bool ignore_priority = true;

    if (is_cluster == 1)
    {
        return;     // Only one server in the cluster: update_sst_donor_nodes is not performed
    }

    unsigned int found_slaves = 0;
    MXS_MONITORED_SERVER* node_list[is_cluster - 1];
    /* Donor list size = DONOR_LIST_SET_VAR + n_hosts * max_host_len + n_hosts + 1 */

    char* donor_list = static_cast<char*>(MXS_CALLOC(1,
                                                     strlen(DONOR_LIST_SET_VAR)
                                                     + is_cluster * DONOR_NODE_NAME_MAX_LEN
                                                     + is_cluster + 1));

    if (donor_list == NULL)
    {
        MXS_ERROR("can't execute update_sst_donor_nodes() due to memory allocation error");
        return;
    }

    strcpy(donor_list, DONOR_LIST_SET_VAR);

    /* Create an array of slave nodes */
    for (auto ptr : m_servers)
    {
        if ((ptr->pending_status & SERVER_JOINED) && (ptr->pending_status & SERVER_SLAVE))
        {
            node_list[found_slaves] = (MXS_MONITORED_SERVER*)ptr;
            found_slaves++;

            /* Check the server parameter "priority"
             * If no server has "priority" set, then
             * the server list will be order by default method.
             */

            if (m_use_priority && !ptr->server->get_custom_parameter("priority").empty())
            {
                ignore_priority = false;
            }
        }
    }

    /* Set order type */
    bool sort_order = (!ignore_priority) && (int)m_use_priority;

    /* Sort the array */
    qsort(node_list,
          found_slaves,
          sizeof(MXS_MONITORED_SERVER*),
          sort_order ? compare_node_priority : compare_node_index);

    /* Select nodename from each server and append it to node_list */
    for (unsigned int k = 0; k < found_slaves; k++)
    {
        MXS_MONITORED_SERVER* ptr = node_list[k];

        /* Get the Galera node name */
        if (mxs_mysql_query(ptr->con, "SHOW VARIABLES LIKE 'wsrep_node_name'") == 0
            && (result = mysql_store_result(ptr->con)) != NULL)
        {
            if (mysql_field_count(ptr->con) < 2)
            {
                mysql_free_result(result);
                MXS_ERROR("Unexpected result for \"SHOW VARIABLES LIKE 'wsrep_node_name'\". "
                          "Expected 2 columns");
                return;
            }

            while ((row = mysql_fetch_row(result)))
            {
                strncat(donor_list, row[1], DONOR_NODE_NAME_MAX_LEN);
                strcat(donor_list, ",");
            }

            mysql_free_result(result);
        }
        else
        {
            ptr->mon_report_query_error();
        }
    }

    int donor_list_size = strlen(donor_list);
    if (donor_list[donor_list_size - 1] == ',')
    {
        donor_list[donor_list_size - 1] = '\0';
    }

    strcat(donor_list, "\"");

    /* Set now rep_sst_donor in each slave node */
    for (unsigned int k = 0; k < found_slaves; k++)
    {
        MXS_MONITORED_SERVER* ptr = node_list[k];
        if (mxs_mysql_query(ptr->con, donor_list) != 0)
        {
            ptr->mon_report_query_error();
        }
    }

    MXS_FREE(donor_list);
}

/**
 * Compare routine for slave nodes sorted by 'wsrep_local_index'
 *
 * The default order is DESC.
 *
 * Nodes with lowest 'wsrep_local_index' value
 * are at the end of the list.
 *
 * @param   a        Pointer to array value
 * @param   b        Pointer to array value
 * @return  A number less than, threater than or equal to 0
 */

static int compare_node_index(const void* a, const void* b)
{
    const MXS_MONITORED_SERVER* s_a = *(MXS_MONITORED_SERVER* const*)a;
    const MXS_MONITORED_SERVER* s_b = *(MXS_MONITORED_SERVER* const*)b;

    // Order is DESC: b - a
    return s_b->server->node_id - s_a->server->node_id;
}

/**
 * Compare routine for slave nodes sorted by node priority
 *
 * The default order is DESC.
 *
 * Some special cases, i.e: no give priority, or 0 value
 * are handled.
 *
 * Note: the master selection algorithm is:
 * node with lowest priority value and > 0
 *
 * This sorting function will add master candidates
 * at the end of the list.
 *
 * @param   a        Pointer to array value
 * @param   b        Pointer to array value
 * @return  A number less than, threater than or equal to 0
 */

static int compare_node_priority(const void* a, const void* b)
{
    const MXS_MONITORED_SERVER* s_a = *(MXS_MONITORED_SERVER* const*)a;
    const MXS_MONITORED_SERVER* s_b = *(MXS_MONITORED_SERVER* const*)b;
    std::string pri_a = s_a->server->get_custom_parameter("priority");
    std::string pri_b = s_b->server->get_custom_parameter("priority");
    bool have_a = !pri_a.empty();
    bool have_b = !pri_b.empty();

    /**
     * Check priority parameter:
     *
     * Return a - b in case of issues
     */
    if (!have_a && have_b)
    {
        MXS_DEBUG("Server %s has no given priority. It will be at the beginning of the list",
                  s_a->server->name());
        return -(INT_MAX - 1);
    }
    else if (have_a && !have_b)
    {
        MXS_DEBUG("Server %s has no given priority. It will be at the beginning of the list",
                  s_b->server->name());
        return INT_MAX - 1;
    }
    else if (!have_a && !have_b)
    {
        MXS_DEBUG("Servers %s and %s have no given priority. They be at the beginning of the list",
                  s_a->server->name(),
                  s_b->server->name());
        return 0;
    }

    /* The given  priority is valid */
    int pri_val_a = atoi(pri_a.c_str());
    int pri_val_b = atoi(pri_b.c_str());

    /* Return a - b in case of issues */
    if ((pri_val_a < INT_MAX && pri_val_a > 0) && !(pri_val_b < INT_MAX && pri_val_b > 0))
    {
        return pri_val_a;
    }
    else if (!(pri_val_a < INT_MAX && pri_val_a > 0) && (pri_val_b < INT_MAX && pri_val_b > 0))
    {
        return -pri_val_b;
    }
    else if (!(pri_val_a < INT_MAX && pri_val_a > 0) && !(pri_val_b < INT_MAX && pri_val_b > 0))
    {
        return 0;
    }

    // The order is DESC: b -a
    return pri_val_b - pri_val_a;
}

/**
 * Only set the servers as joined if they are a part of the largest cluster
 */
void GaleraMonitor::set_galera_cluster()
{
    int cluster_size = 0;
    std::string cluster_uuid;

    for (auto it = m_info.begin(); it != m_info.end(); it++)
    {
        if (it->second.joined && it->second.cluster_size > cluster_size)
        {
            // Use the UUID of the largest cluster
            cluster_size = it->second.cluster_size;
            cluster_uuid = it->second.cluster_uuid;
        }
    }

    for (auto it = m_info.begin(); it != m_info.end(); it++)
    {
        if (it->second.joined)
        {
            it->first->set_pending_status(SERVER_JOINED);
        }
        else
        {
            it->first->clear_pending_status(SERVER_JOINED);
        }
    }
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    MXS_NOTICE("Initialise the MySQL Galera Monitor module.");

    static MXS_MODULE info =
    {
        MXS_MODULE_API_MONITOR,
        MXS_MODULE_GA,
        MXS_MONITOR_VERSION,
        "A Galera cluster monitor",
        "V2.0.0",
        MXS_NO_MODULE_CAPABILITIES,
        &maxscale::MonitorApi<GaleraMonitor>::s_api,
        NULL,                                       /* Process init. */
        NULL,                                       /* Process finish. */
        NULL,                                       /* Thread init. */
        NULL,                                       /* Thread finish. */
        {
            {"disable_master_failback",             MXS_MODULE_PARAM_BOOL,  "false"},
            {"available_when_donor",                MXS_MODULE_PARAM_BOOL,  "false"},
            {"disable_master_role_setting",         MXS_MODULE_PARAM_BOOL,  "false"},
            {"root_node_as_master",                 MXS_MODULE_PARAM_BOOL,  "false"},
            {"use_priority",                        MXS_MODULE_PARAM_BOOL,  "false"},
            {"set_donor_nodes",                     MXS_MODULE_PARAM_BOOL,  "false"},
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}
