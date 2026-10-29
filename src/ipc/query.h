#ifndef EVEMON_QUERY_H
#define EVEMON_QUERY_H
#include "../core/proc.h"
#include "ipc_wire.h"
typedef struct query_server query_server_t;
typedef struct query_connection query_connection_t;
/* Persistent, versioned binary Unix stream connections. See docs/protocol.md. */
int query_default_path(char *path, size_t capacity);
query_server_t *query_server_start(monitor_state_t *state, const char *path);
void query_server_stop(query_server_t *server);
unsigned query_server_client_count(const query_server_t *server);
int query_client(const char *path, const char *request, int binary);
int query_snapshot_encode(monitor_state_t *state, uint16_t type, pid_t pid, ipc_buf_t *out);
int query_snapshot_decode(ipc_buf_t *in, monitor_state_t *state, int status_only);
query_connection_t *query_connection_start(monitor_state_t *state, const char *path);
/* Prepare an independent client without changing the active GUI's host. */
query_connection_t *query_connection_prepare(monitor_state_t *state, const char *path);
void query_connection_activate(query_connection_t *connection);
void query_connection_stop(query_connection_t *connection);
/* Set before starting GUI workers; never fall back to local collection. */
void query_set_remote(const char *path);
const char *query_remote_path(void);
uint32_t query_remote_capabilities(void);
int query_exchange(const char *path, uint16_t type, const ipc_buf_t *request, ipc_buf_t *response);
/* Host identity changes only after the previous frontend's workers stop. */
typedef struct { char name[256]; int foreign; } query_host_t;
int query_host_read(const char *path, query_host_t *host);
void query_host_activate(const query_host_t *host);
void query_host_activate_local(void);
int query_host_init(const char *path);
int query_is_foreign_host(void);
const char *query_host_name(void);
void query_host_encode(ipc_buf_t *out);
int query_signal(const proc_entry_t *target, int signal_number);
int query_signal_path(const char *path, const proc_entry_t *target, int signal_number);
void query_signal_dispatch(ipc_buf_t *in, ipc_buf_t *out, uint64_t instance);
/* Snapshot cache only: no GUI-thread filesystem or network access. */
int query_cached_process(pid_t pid, proc_entry_t *entry, char *name, size_t size);
#endif
