/* Toolkit-free launcher: choose one frontend before loading a GUI library. */
#include "runtime.h"
#include "../platform/desktop_session.h"
#include "../ipc/query.h"
#include "client_session.h"
#include "../ipc/query_events.h"
#include "../core/settings.h"
#include "../core/profile.h"
#include "../plugins/event_bus.h"
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void shutdown_signal(int signal)
{
    (void)signal;
    evemon_shutdown_requested = 1;
}
static int positive_number(const char *value, long maximum, long *result)
{
    char *end;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || !*value || *end || parsed < 1 || parsed > maximum) return -1;
    *result = parsed;
    return 0;
}
static void help(void)
{
    puts("Usage: evemon [OPTIONS]\n"
         "  --ui gtk|qt          Select the UI for this launch\n"
         "  --set-ui gtk|qt|headless  Save startup preference and exit\n"
         "  --list-uis           List compiled frontends\n"
         "  --headless           Run the monitor and local query service without a GUI\n"
         "  --connect PATH       Connect a GUI to an existing headless Evemon\n"
         "  --remote HOST        Connect a GUI to a remote Evemon over SSH\n"
         "  --android-adb-forward SERIAL  Launch and forward Evemon on Android (use auto for one device)\n"
         "  --remote-socket PATH Override the SSH/ADB Evemon socket\n"
         "  --remote-no-daemon   Do not install/start a private remote daemon\n"
         "  --remote-idle MIN    Stop an installed daemon after idle minutes (default 5)\n"
         "  --remote-no-idle     Keep an installed remote daemon running\n"
         "  --serve              Expose the managed Evemon instance at the shared socket\n"
         "  --socket PATH        Local socket (root default: " IPC_SYSTEM_SOCKET ")\n"
         "  --query status|snapshot|gpu|PID  Query the binary service, print readable output\n"
         "  --binary             With --query, write a framed binary response to stdout\n"
         "  --interval MS        Monitor sampling period, 1..3600000 ms (default 1000)\n"
         "  --idle-exit SEC      In headless mode, exit after this long without clients\n"
         "  -s, --safe-mode      Disable frontend plugins\n"
         "  -d, --debug          Verbose logs\n"
         "  -a, --debug-audio    Audio debug logs\n"
         "  -h, --help           Show this help\n"
         "Queries return the latest snapshot immediately; polling does not trigger a scan.\n"
         "Unprivileged Evemon instances use a private per-user runtime socket.\n"
         "The UI preference is applied at the next start; switching requires a restart.");
}

static int frontend_module_is_trusted(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & (S_IWGRP | S_IWOTH)))
        return 0;
    return desktop_session_trusts_owner(st.st_uid);
}

static void *load_frontend(const char *name)
{
    char executable[PATH_MAX], path[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    void *module = NULL;
    if (size > 0) {
        executable[size] = 0;
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = 0;
            int n = snprintf(path, sizeof(path), "%s/evemon-ui-%s.so", executable, name);
            if (n > 0 && n < (int)sizeof(path)) {
                int trusted = frontend_module_is_trusted(path);
                if (trusted)
                    module = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
                if (!module && trusted) {
                    fprintf(stderr, "evemon: %s\n", dlerror());
                    return NULL;
                }
            }
        }
    }
    if (!module) {
        snprintf(path, sizeof(path), "%s/evemon-ui-%s.so", EVEMON_LIBDIR, name);
        if (frontend_module_is_trusted(path)) {
            module = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
            if (!module) fprintf(stderr, "evemon: %s\n", dlerror());
        } else {
            fprintf(stderr, "evemon: no trusted %s frontend module found\n",
                    name);
        }
    }
    return module;
}
int main(int argc, char **argv)
{
    desktop_session_init();
    clock_gettime(CLOCK_MONOTONIC, &evemon_start_time);
    /* Restart drops an explicit UI override so the new saved choice takes effect. */
    evemon_restart_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (!evemon_restart_argv) return 1;
    int restart_count = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--ui")) { ++i; continue; }
        if (!strncmp(argv[i], "--ui=", 5)) continue;
        evemon_restart_argv[restart_count++] = argv[i];
    }
    const char *ui = NULL, *preference = NULL, *request = NULL, *socket_path = NULL, *connect_path = NULL;
    const char *remote_host = NULL, *remote_socket = NULL, *adb_serial = NULL;
    int remote_run_daemon = 1, print_socket = 0;
    long remote_idle_minutes = 5;
    long interval = 1000, idle_exit = 0;
    int serve = 0, binary = 0;
    enum { OPT_UI = 256, OPT_SET_UI, OPT_LIST, OPT_HEADLESS, OPT_SERVE,
        OPT_SOCKET, OPT_QUERY, OPT_INTERVAL, OPT_IDLE_EXIT, OPT_CONNECT,
        OPT_BINARY, OPT_REMOTE, OPT_REMOTE_SOCKET, OPT_REMOTE_NO_DAEMON,
        OPT_REMOTE_IDLE, OPT_REMOTE_NO_IDLE, OPT_PRINT_SOCKET,
        OPT_ANDROID_ADB_FORWARD };
    const struct option options[] = {
        {"ui", required_argument, NULL, OPT_UI}, {"set-ui", required_argument, NULL, OPT_SET_UI},
        {"list-uis", no_argument, NULL, OPT_LIST}, {"headless", no_argument, NULL, OPT_HEADLESS},
        {"serve", no_argument, NULL, OPT_SERVE}, {"socket", required_argument, NULL, OPT_SOCKET},
        {"connect", required_argument, NULL, OPT_CONNECT}, {"binary", no_argument, NULL, OPT_BINARY},
        {"remote", required_argument, NULL, OPT_REMOTE}, {"remote-socket", required_argument, NULL, OPT_REMOTE_SOCKET},
        {"android-adb-forward", required_argument, NULL, OPT_ANDROID_ADB_FORWARD},
        {"remote-no-daemon", no_argument, NULL, OPT_REMOTE_NO_DAEMON},
        {"remote-idle", required_argument, NULL, OPT_REMOTE_IDLE},
        {"remote-no-idle", no_argument, NULL, OPT_REMOTE_NO_IDLE},
        {"print-socket", no_argument, NULL, OPT_PRINT_SOCKET},
        {"query", required_argument, NULL, OPT_QUERY}, {"interval", required_argument, NULL, OPT_INTERVAL},
        {"idle-exit", required_argument, NULL, OPT_IDLE_EXIT},
        {"debug", no_argument, NULL, 'd'}, {"debug-audio", no_argument, NULL, 'a'},
        {"safe-mode", no_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    int option;
    while ((option = getopt_long(argc, argv, "dash", options, NULL)) != -1) {
        switch (option) {
        case OPT_UI: ui = optarg; break;
        case OPT_SET_UI: preference = optarg; break;
        case OPT_LIST:
            for (const char **p = (const char *[]){"gtk", "qt", "headless", NULL}; *p; ++p)
                if (evemon_ui_available(*p)) puts(*p);
            return 0;
        case OPT_HEADLESS: ui = "headless"; break;
        case OPT_SERVE: serve = 1; break;
        case OPT_SOCKET: socket_path = optarg; break;
        case OPT_QUERY: request = optarg; break;
        case OPT_CONNECT: connect_path = optarg; break;
        case OPT_REMOTE: remote_host = optarg; break;
        case OPT_ANDROID_ADB_FORWARD: adb_serial = optarg; break;
        case OPT_REMOTE_SOCKET: remote_socket = optarg; break;
        case OPT_REMOTE_NO_DAEMON: remote_run_daemon = 0; break;
        case OPT_REMOTE_IDLE:
            if (positive_number(optarg, 10080, &remote_idle_minutes)) { fputs("evemon: invalid remote idle timeout\n", stderr); return 2; }
            break;
        case OPT_REMOTE_NO_IDLE: remote_idle_minutes = 0; break;
        case OPT_PRINT_SOCKET: print_socket = 1; break;
        case OPT_BINARY: binary = 1; break;
        case OPT_INTERVAL:
            if (positive_number(optarg, 3600000, &interval)) { fputs("evemon: invalid interval\n", stderr); return 2; }
            break;
        case OPT_IDLE_EXIT:
            if (positive_number(optarg, 604800, &idle_exit)) { fputs("evemon: invalid idle timeout\n", stderr); return 2; }
            break;
        case 's': evemon_safe_mode = 1; break;
        case 'd': evemon_debug = 1; break;
        case 'a': evemon_debug_audio = 1; break;
        case 'h': help(); return 0;
        default: return 2;
        }
    }
    if (optind != argc) { fputs("evemon: unexpected positional argument\n", stderr); return 2; }
    int remote_targets = !!remote_host + !!adb_serial;
    if (remote_targets > 1) {
        fputs("evemon: choose either --remote or --android-adb-forward\n", stderr); return 2;
    }
    if ((remote_socket && !remote_targets) ||
        (remote_targets && (connect_path || serve || socket_path || request))) {
        fputs("evemon: remote transports require a GUI and cannot be combined with --connect, --serve, --socket or --query\n", stderr); return 2;
    }
    if ((!remote_run_daemon || remote_idle_minutes != 5) && !remote_targets) {
        fputs("evemon: remote daemon options require --remote or --android-adb-forward\n", stderr); return 2;
    }
    if (print_socket) {
        char path[108];
        if (query_default_path(path, sizeof(path))) { perror("evemon: socket path"); return 1; }
        puts(path);
        return 0;
    }
    if (preference) {
        if (evemon_set_preferred_ui(preference)) { perror("evemon: cannot save frontend preference"); return 1; }
        printf("Startup UI: %s (restart running instances to apply)\n", preference);
        return 0;
    }
    char default_socket[108], query[64];
    if (request) {
        if (strcmp(request, "status") && strcmp(request, "snapshot") && strcmp(request, "gpu")) {
            long query_pid;
            if (positive_number(request, INT_MAX, &query_pid)) { fputs("evemon: query must be status, snapshot, gpu, or a PID\n", stderr); return 2; }
            snprintf(query, sizeof(query), "process %ld", query_pid);
            request = query;
        }
        if (!socket_path) {
            if (query_default_path(default_socket, sizeof(default_socket))) { perror("evemon: socket path"); return 1; }
            socket_path = default_socket;
        }
        if (query_client(socket_path, request, binary)) { perror("evemon: query failed"); return 1; }
        return 0;
    }
    if (binary) { fputs("evemon: --binary requires --query\n", stderr); return 2; }
    evemon_settings_t *settings = settings_get();
    if (!ui) {
        ui = settings->frontend[0] ? settings->frontend : evemon_default_ui();
        if (!evemon_ui_available(ui)) {
            fprintf(stderr, "evemon: saved UI '%s' is unavailable; using %s\n", ui, evemon_default_ui());
            ui = evemon_default_ui();
        }
    }
    if (!evemon_ui_available(ui)) { fprintf(stderr, "evemon: UI '%s' was not built; see --list-uis\n", ui); return 2; }
    int headless = !strcmp(ui, "headless");
    if (idle_exit && !headless) { fputs("evemon: --idle-exit requires --headless\n", stderr); return 2; }
    if (headless && (connect_path || remote_targets)) { fputs("evemon: remote connections require a GUI\n", stderr); return 2; }
    if (connect_path && (serve || socket_path)) { fputs("evemon: --connect cannot be combined with --serve or --socket\n", stderr); return 2; }
    if (!headless && socket_path && !serve) { fputs("evemon: --socket requires --serve for a local GUI\n", stderr); return 2; }
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = shutdown_signal;
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    profile_init();
    monitor_state_t state = {0};
    int state_initialized = 0;
    query_server_t *server = NULL;
    pthread_t monitor;
    int monitor_started = 0, result = 0;
    evemon_event_bus_init();
    if (headless) {
        if (monitor_state_init(&state)) { result = 1; goto cleanup; }
        state_initialized = 1;
        state.interval_ms = (unsigned)interval;
        state.preselect_pid = settings->preselected_pid;
        state.fdmon = fdmon_create(FDMON_BACKEND_AUTO);
        if (!socket_path) {
            if (query_default_path(default_socket, sizeof(default_socket))) { perror("evemon: socket path"); result = 1; goto cleanup; }
            socket_path = default_socket;
        }
        server = query_server_start(&state, socket_path);
        if (!server) { perror("evemon: query service"); result = 1; goto cleanup; }
        fprintf(stderr, "evemon: binary socket %s\n", socket_path);
        result = pthread_create(&monitor, NULL, monitor_thread, &state);
        if (result) { result = 1; goto cleanup; }
        monitor_started = 1;
    } else {
        if (serve && !socket_path) {
            if (query_default_path(default_socket, sizeof(default_socket))) { perror("evemon: socket path"); result = 1; goto cleanup; }
            socket_path = default_socket;
        }
        GError *error = NULL;
        if (adb_serial) {
            if (client_session_start_adb(adb_serial, remote_socket,
                                         remote_run_daemon,
                                         (unsigned)remote_idle_minutes,
                                         &error)) {
                fprintf(stderr, "evemon: %s\n", error->message); g_clear_error(&error); result = 1; goto cleanup;
            }
        } else if (remote_host) {
            if (client_session_start_remote(remote_host, remote_socket,
                                            remote_run_daemon,
                                            (unsigned)remote_idle_minutes,
                                            &error)) {
                fprintf(stderr, "evemon: %s\n", error->message); g_clear_error(&error); result = 1; goto cleanup;
            }
        } else if (client_session_start(connect_path, serve ? socket_path : NULL,
                                        interval)) {
            perror("evemon: connect"); result = 1; goto cleanup;
        }
        /* Qt restores its own per-host selection. */
        if (!strcmp(ui, "qt")) client_session_state()->preselect_pid = 0;
    }
    void *module = headless ? NULL : load_frontend(ui);
    if (!headless && !module) { result = 1; goto cleanup; }
    evemon_ui_run_fn run = module ? (evemon_ui_run_fn)dlsym(module, "evemon_ui_run") : NULL;
    if (module && !run) { fprintf(stderr, "evemon: invalid frontend module: %s\n", dlerror()); result = 1; goto cleanup; }
    if (headless) {
        int64_t idle_since = ipc_now_ms();
        while (!evemon_shutdown_requested) {
            for (int i = 0; i < 64 && g_main_context_pending(NULL); ++i) g_main_context_iteration(NULL, FALSE);
            int64_t now = ipc_now_ms();
            if (!idle_exit || query_server_client_count(server) > 0)
                idle_since = now;
            else if (now - idle_since >= idle_exit * 1000)
                break;
            g_usleep(50000);
        }
    } else {
        int ui_argc = 1;
        char *ui_argv[] = {argv[0], NULL};
        result = run(client_session_state(), ui_argc, ui_argv);
    }
cleanup:
    if (evemon_restart_requested) client_session_restart_arguments();
    if (state_initialized) {
        pthread_mutex_lock(&state.lock);
        state.running = 0;
        pthread_cond_broadcast(&state.updated);
        pthread_mutex_unlock(&state.lock);
        query_server_stop(server);
        if (monitor_started) pthread_join(monitor, NULL);
        fdmon_destroy(state.fdmon);
        monitor_state_destroy(&state);
    } else {
        client_session_close();
    }
    evemon_event_bus_destroy();
    /* Keep the chosen module loaded through process exit: toolkit globals and
     * plugin teardown may still own function pointers into it. */
    if (evemon_restart_requested) {
        execv("/proc/self/exe", evemon_restart_argv);
        perror("evemon: restart");
        return 1;
    }
    free(evemon_restart_argv);
    return result;
}
