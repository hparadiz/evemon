#ifndef EVEMON_BROKER_DATA_H
#define EVEMON_BROKER_DATA_H
#include "plugin_broker.h"
#include "../ipc/ipc_wire.h"
struct broker_pid_data {
    pid_t                pid;
    evemon_proc_data_t   data;
    evemon_data_needs_t  needs;

    /* Owned storage (freed after dispatch) */
    evemon_fd_t         *fd_buf;
    size_t               fd_cap;

    evemon_env_t        *env_buf;
    char                *env_raw;       /* raw /proc/<pid>/environ */
    size_t               env_cap;

    evemon_mmap_t       *mmap_buf;
    char                *mmap_raw;      /* raw /proc/<pid>/maps    */
    size_t               mmap_cap;

    evemon_lib_t        *lib_buf;
    size_t               lib_cap;

    evemon_socket_t     *sock_buf;
    size_t               sock_cap;

    evemon_cgroup_t      cgroup_data;

    pid_t               *desc_pids;
    size_t               desc_count;
    size_t               desc_cap;

    evemon_thread_t     *thread_buf;
    size_t               thread_cap;

    char                *raw_status;
    char                *raw_maps;
    char                *raw_cgroup;

    /* PipeWire graph (if evemon_NEED_PIPEWIRE) */
    evemon_pw_node_t    *pw_nodes;
    size_t               pw_node_cap;
    evemon_pw_link_t    *pw_links;
    size_t               pw_link_cap;
    evemon_pw_port_t    *pw_ports;
    size_t               pw_port_cap;

    /* MPRIS media metadata */
    evemon_mpris_player_t *mpris_players;
    size_t                 mpris_player_count;
    int decoded; /* string ownership for binary client results */
};


void broker_pid_data_free(broker_pid_data_t *d);
broker_pid_data_t *broker_request(pid_t pid, unsigned needs, void *fdmon);
broker_pid_data_t *broker_gather(pid_t pid, unsigned needs, void *fdmon);
int query_inspect_encode(pid_t pid, unsigned needs, void *fdmon, ipc_buf_t *out);
broker_pid_data_t *query_inspect_decode(ipc_buf_t *in);
#endif
