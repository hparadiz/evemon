#ifndef EVEMON_CLIENT_SESSION_H
#define EVEMON_CLIENT_SESSION_H
#include "../ipc/query.h"
#include <gio/gio.h>
typedef struct client_session client_session_t;
int client_session_start(const char *path, const char *serve_path, unsigned interval);
int client_session_start_remote(const char *host, const char *socket,
                                int run_daemon, unsigned idle_minutes,
                                GError **error);
int client_session_start_adb(const char *serial, const char *socket,
                             int run_daemon, unsigned idle_minutes,
                             GError **error);
void client_session_restart_arguments(void);
/* Restart locally after the current frontend and connection are closed. */
void client_session_disconnect(const char *frontend);
monitor_state_t *client_session_state(void);
/* Background preparation never changes active query identity or settings. */
void client_session_prepare(const char *host, const char *socket,
                            int run_daemon, unsigned idle_minutes,
                            GCancellable *cancel,
                            GAsyncReadyCallback callback, gpointer data);
client_session_t *client_session_finish(GAsyncResult *result, GError **error);
void client_session_discard(client_session_t *session);
void client_session_queue(client_session_t *session);
/* Call only after all frontend plugins and workers have been destroyed. */
int client_session_commit(void);
void client_session_close(void);
#endif
