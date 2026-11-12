#ifndef EVEMON_QUERY_DPKG_H
#define EVEMON_QUERY_DPKG_H
#include "ipc_wire.h"
#include "../plugins/dpkg_lookup.h"

/* Evemon side.  PID 0 encodes only the distribution label.  Returns 0 or an
 * IPC error code (IPC_INTERNAL when cancelled). */
int query_dpkg_encode(pid_t pid, ipc_buf_t *out, dpkg_cancel_fn cancel, void *user_data);
/* `res` may be NULL to read only the label. */
int query_dpkg_decode(ipc_buf_t *in, char *label, size_t label_size, dpkg_result_t *res);
/* Client side; blocks for up to the IPC timeouts (IPC_PACKAGE_TIMEOUT_MS for a
 * lookup), never call on a GUI thread.  A readable wake_fd (-1 for none)
 * abandons the wait with errno ECANCELED. */
int query_dpkg_fetch(const char *path, pid_t pid, char *label, size_t label_size,
                     dpkg_result_t *res, int wake_fd);

/* Distribution label of the active remote dpkg host, read while connecting. */
void query_dpkg_set_remote_label(const char *label);
const char *query_dpkg_remote_label(void);
#endif
