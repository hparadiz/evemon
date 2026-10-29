#ifndef EVEMON_QUERY_EVENTS_H
#define EVEMON_QUERY_EVENTS_H
#include "../core/proc.h"
#include "ipc_wire.h"
typedef struct query_events query_events_t;
query_events_t *query_events_start(fdmon_ctx_t *fdmon);
void query_events_stop(query_events_t *events);
/* Each persistent control connection owns its registrations. */
int query_controls_replace(query_events_t *events, uint64_t owner, ipc_buf_t *request);
void query_controls_drop(query_events_t *events, uint64_t owner);
uint64_t query_events_latest(query_events_t *events);
void query_events_encode(query_events_t *events, uint64_t *cursor, ipc_buf_t *out);
int query_events_dispatch(ipc_buf_t *in);
/* A GUI-side fdmon facade: no tracing attachments, counters from the stream. */
fdmon_ctx_t *query_fdmon_create(monitor_state_t *state, const char *path);
fdmon_ctx_t *query_fdmon_prepare(monitor_state_t *state, const char *path);
void query_fdmon_activate(fdmon_ctx_t *ctx);
void query_fdmon_destroy(fdmon_ctx_t *ctx);
int query_fdmon_control(fdmon_ctx_t *ctx, unsigned kind, pid_t pid, int value, int remove);
int query_fdmon_backend(const fdmon_ctx_t *ctx);
int query_fdmon_net(const fdmon_ctx_t *ctx, pid_t pid, uint64_t *sent, uint64_t *received);
#endif
