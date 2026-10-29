#include "client_session.h"
#include "../remote/adb_tunnel.h"
#include "../ipc/query_events.h"
#include "../core/settings.h"
#include "../remote/ssh_tunnel.h"
#include "../plugins/event_bus.h"
#include "runtime.h"
#include <errno.h>
#include <unistd.h>

struct client_session {
    monitor_state_t state;
    query_connection_t *connection;
    query_server_t *server;
    pthread_t monitor;
    int monitor_started;
    ssh_tunnel_t *tunnel;
    adb_tunnel_t *adb;
    char *host, *socket;
    int run_daemon;
    unsigned idle_minutes;
};
static client_session_t *current, *pending;
static GPtrArray *attempts;
static const char *restart_local_ui;
static client_session_t *session_new(void)
{
    client_session_t *s = g_new0(client_session_t, 1);
    if (monitor_state_init(&s->state)) { g_free(s); return NULL; }
    return s;
}
void client_session_discard(client_session_t *s)
{
    if (!s) return;
    if (s->monitor_started) {
        pthread_mutex_lock(&s->state.lock);
        s->state.running = 0;
        pthread_cond_broadcast(&s->state.updated);
        pthread_mutex_unlock(&s->state.lock);
    }
    query_server_stop(s->server);
    if (s->monitor_started) pthread_join(s->monitor, NULL);
    query_connection_stop(s->connection);
    fdmon_destroy(s->state.fdmon);
    ssh_tunnel_close(s->tunnel);
    adb_tunnel_close(s->adb);
    monitor_state_destroy(&s->state);
    g_free(s->host); g_free(s->socket); g_free(s);
}
static int session_connect(client_session_t *s, const char *path)
{
    s->connection = query_connection_prepare(&s->state, path);
    if (!s->connection) return -1;
    s->state.fdmon = query_fdmon_prepare(&s->state, path);
    return s->state.fdmon ? 0 : -1;
}
static void session_activate(client_session_t *s)
{
    if (s->connection) {
        query_connection_activate(s->connection);
        query_fdmon_activate(s->state.fdmon);
    } else {
        query_host_activate_local();
    }
    settings_select_host(query_is_foreign_host() ? query_host_name() : NULL);
    s->state.preselect_pid = settings_get()->preselected_pid;
    ssh_tunnel_activate(s->tunnel);
    adb_tunnel_activate(s->adb);
}
int client_session_start(const char *path, const char *serve_path, unsigned interval)
{
    client_session_t *s = session_new();
    if (!s) return -1;
    if (path) {
        if (session_connect(s, path)) goto fail;
    } else {
        session_activate(s);
        s->state.interval_ms = interval;
        s->state.fdmon = fdmon_create(FDMON_BACKEND_AUTO);
        s->state.connected = 1;
        if (serve_path) {
            s->server = query_server_start(&s->state, serve_path);
            if (!s->server) goto fail;
        } else {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            s->state.evemon_instance = (uint64_t)now.tv_sec * 1000000000u +
                                       (uint64_t)now.tv_nsec;
        }
        int rc = pthread_create(&s->monitor, NULL, monitor_thread, &s->state);
        if (rc) { errno = rc; goto fail; }
        s->monitor_started = 1;
    }
    current = s;
    if (path) session_activate(s);
    return 0;
fail: {
    int error = errno;
    client_session_discard(s); errno = error; return -1;
    }
}
monitor_state_t *client_session_state(void) { return current ? &current->state : NULL; }
typedef struct {
    char *host, *socket;
    int run_daemon;
    unsigned idle_minutes;
    GAsyncReadyCallback callback;
    gpointer data;
} destination_t;
static void destination_free(gpointer opaque)
{
    destination_t *d = opaque; g_free(d->host); g_free(d->socket); g_free(d);
}
static client_session_t *prepare_remote_attempt(const char *host,
    const char *socket, int run_daemon, unsigned idle_minutes, int adb,
    GCancellable *cancel, GError **error)
{
    client_session_t *s = session_new();
    if (!s) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Could not allocate an Evemon connection."); return NULL; }
    s->host = g_strdup(host); s->socket = g_strdup(socket);
    s->run_daemon = run_daemon;
    s->idle_minutes = idle_minutes;
    const char *path;
    if (adb) {
        s->adb = adb_tunnel_open(host, socket, run_daemon,
                                 idle_minutes * 60, cancel, error);
        if (!s->adb) goto fail;
        path = adb_tunnel_path(s->adb);
    } else {
        s->tunnel = ssh_tunnel_open(host, socket, run_daemon,
                                    idle_minutes * 60, cancel, error);
        if (!s->tunnel) goto fail;
        path = ssh_tunnel_path(s->tunnel);
    }
    if (session_connect(s, path)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                    "Could not connect to Evemon on %s: %s", host, g_strerror(errno));
        goto fail;
    }
    /* A HELLO alone is insufficient: require a complete decoded process
     * snapshot so a successful switch cannot silently open an empty model. */
    while (!g_cancellable_is_cancelled(cancel)) {
        pthread_mutex_lock(&s->state.lock);
        int ready = s->state.connected && s->state.snapshot_complete && s->state.generation;
        int connected = s->state.connected;
        pthread_mutex_unlock(&s->state.lock);
        if (ready) return s;
        if (!connected) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                        "Evemon on %s disconnected before sending a process snapshot.", host);
            goto fail;
        }
        g_usleep(20000);
    }
    g_cancellable_set_error_if_cancelled(cancel, error);
fail:
    client_session_discard(s);
    return NULL;
}
typedef struct {
    GMutex lock;
    GCond changed;
    GCancellable *cancel;
    gint64 deadline;
    gboolean done, expired;
} connection_deadline_t;
static gpointer connection_deadline(gpointer data)
{
    connection_deadline_t *d = data;
    g_mutex_lock(&d->lock);
    while (!d->done && g_cond_wait_until(&d->changed, &d->lock, d->deadline)) {}
    d->expired = !d->done;
    g_mutex_unlock(&d->lock);
    if (d->expired) g_cancellable_cancel(d->cancel);
    return NULL;
}
static void cancel_attempt(GCancellable *unused, gpointer data)
{
    (void)unused; g_cancellable_cancel(data);
}
static client_session_t *prepare_transport(const char *host,
    const char *socket, int run_daemon, unsigned idle_minutes, int adb,
    unsigned timeout_seconds, GCancellable *cancel, GError **error)
{
    /* One deadline covers DNS, authentication/askpass, remote discovery,
     * forwarding, protocol negotiation and the first complete snapshot.
     * It also works for --remote, before any GUI main loop exists. */
    connection_deadline_t d = {.cancel = g_cancellable_new(),
        .deadline = g_get_monotonic_time() + timeout_seconds * G_TIME_SPAN_SECOND};
    g_mutex_init(&d.lock); g_cond_init(&d.changed);
    gulong handler = cancel ? g_cancellable_connect(cancel, G_CALLBACK(cancel_attempt), d.cancel, NULL) : 0;
    GThread *timer = g_thread_new("ev-connect-limit", connection_deadline, &d);
    GError *failure = NULL;
    client_session_t *s = prepare_remote_attempt(host, socket, run_daemon,
                                                 idle_minutes, adb, d.cancel,
                                                 &failure);
    g_mutex_lock(&d.lock); d.done = TRUE; g_cond_signal(&d.changed); g_mutex_unlock(&d.lock);
    g_thread_join(timer);
    if (handler) g_cancellable_disconnect(cancel, handler);
    if ((cancel && g_cancellable_is_cancelled(cancel)) || d.expired) {
        client_session_discard(s); s = NULL;
        g_clear_error(&failure);
        if (!(cancel && g_cancellable_set_error_if_cancelled(cancel, &failure)))
            g_set_error(&failure, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                        "Connection to %s timed out after %u seconds. Check the transport and Evemon, then retry.",
                        host, timeout_seconds);
    }
    g_object_unref(d.cancel); g_cond_clear(&d.changed); g_mutex_clear(&d.lock);
    if (failure) g_propagate_error(error, failure);
    return s;
}
static client_session_t *prepare_remote(const char *host, const char *socket,
    int run_daemon, unsigned idle_minutes, GCancellable *cancel,
    GError **error)
{
    return prepare_transport(host, socket, run_daemon, idle_minutes, 0, 5,
                             cancel, error);
}
static void prepare_worker(GTask *task, gpointer source, gpointer opaque, GCancellable *cancel)
{
    (void)source;
    destination_t *d = opaque;
    GError *error = NULL;
    client_session_t *s = prepare_remote(d->host, d->socket, d->run_daemon,
                                         d->idle_minutes, cancel, &error);
    if (s) { g_task_return_pointer(task, s, (GDestroyNotify)client_session_discard); return; }
    g_task_return_error(task, error);
}
int client_session_start_remote(const char *host, const char *socket,
                                int run_daemon, unsigned idle_minutes,
                                GError **error)
{
    client_session_t *s = prepare_remote(host, socket, run_daemon,
                                         idle_minutes, NULL, error);
    if (!s) return -1;
    current = s; session_activate(s);
    return 0;
}
int client_session_start_adb(const char *serial, const char *socket,
                             int run_daemon, unsigned idle_minutes,
                             GError **error)
{
    client_session_t *s = prepare_transport(serial, socket, run_daemon,
                                             idle_minutes, 1, 15, NULL,
                                             error);
    if (!s) return -1;
    current = s;
    session_activate(s);
    return 0;
}
void client_session_disconnect(const char *frontend)
{
    restart_local_ui = frontend;
    evemon_request_restart();
}
void client_session_restart_arguments(void)
{
    if (!current || (!restart_local_ui && !current->host)) return;
    int count = 0;
    while (evemon_restart_argv[count]) ++count;
    char **args = calloc((size_t)count + 10, sizeof(*args));
    if (!args) return;
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const char *arg = evemon_restart_argv[i];
        if (!strcmp(arg, "--serve")) continue;
        if (!strcmp(arg, "--connect") || !strcmp(arg, "--remote") ||
            !strcmp(arg, "--android-adb-forward") || !strcmp(arg, "--remote-socket") ||
            !strcmp(arg, "--remote-idle") ||
            !strcmp(arg, "--socket")) { ++i; continue; }
        if (g_str_has_prefix(arg, "--connect=") || g_str_has_prefix(arg, "--remote=") ||
            g_str_has_prefix(arg, "--android-adb-forward=") ||
            g_str_has_prefix(arg, "--remote-socket=") || g_str_has_prefix(arg, "--remote-idle=") ||
            !strcmp(arg, "--remote-no-daemon") || !strcmp(arg, "--remote-no-idle") ||
            g_str_has_prefix(arg, "--socket=")) continue;
        args[n++] = evemon_restart_argv[i];
    }
    if (restart_local_ui) {
        args[n++] = "--ui";
        args[n++] = g_strdup(restart_local_ui);
    } else {
        args[n++] = current->adb ? "--android-adb-forward" : "--remote";
        args[n++] = g_strdup(current->host);
        if (current->socket && *current->socket) { args[n++] = "--remote-socket"; args[n++] = g_strdup(current->socket); }
        if (!current->run_daemon) args[n++] = "--remote-no-daemon";
        if (current->run_daemon && current->idle_minutes == 0) {
            args[n++] = "--remote-no-idle";
        } else if (current->run_daemon && current->idle_minutes != 5) {
            args[n++] = "--remote-idle";
            args[n++] = g_strdup_printf("%u", current->idle_minutes);
        }
    }
    free(evemon_restart_argv); evemon_restart_argv = args;
}
static void prepared_ready(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)unused;
    GTask *task = G_TASK(result);
    destination_t *d = g_task_get_task_data(task);
    g_ptr_array_remove(attempts, g_task_get_cancellable(task));
    d->callback(source, result, d->data);
}
void client_session_prepare(const char *host, const char *socket,
                            int run_daemon, unsigned idle_minutes,
                            GCancellable *cancel,
                            GAsyncReadyCallback callback, gpointer data)
{
    destination_t *d = g_new0(destination_t, 1);
    d->host = g_strdup(host); d->socket = g_strdup(socket);
    d->run_daemon = run_daemon; d->idle_minutes = idle_minutes;
    d->callback = callback; d->data = data;
    if (!attempts) attempts = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(attempts, g_object_ref(cancel));
    GTask *task = g_task_new(NULL, cancel, prepared_ready, NULL);
    g_task_set_task_data(task, d, destination_free);
    g_task_run_in_thread(task, prepare_worker); g_object_unref(task);
}
client_session_t *client_session_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}
void client_session_queue(client_session_t *s)
{
    client_session_discard(pending); pending = s;
}
int client_session_commit(void)
{
    if (!pending) return 0;
    client_session_discard(current);
    /* Queued events from the old machine cannot reach new subscribers. */
    evemon_event_bus_destroy(); evemon_event_bus_init();
    current = pending; pending = NULL;
    session_activate(current);
    return 1;
}
void client_session_close(void)
{
    /* Reap cancelled SSH/askpass children before process exit or exec. */
    if (attempts) {
        for (guint i = 0; i < attempts->len; ++i) g_cancellable_cancel(g_ptr_array_index(attempts, i));
        while (attempts->len) g_main_context_iteration(NULL, TRUE);
        g_ptr_array_unref(attempts); attempts = NULL;
    }
    client_session_discard(pending); pending = NULL;
    client_session_discard(current); current = NULL;
}
