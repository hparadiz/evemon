/*
 * main.c – evemon entry point.
 *
 * The monitor thread runs in the background scanning /proc.
 * The GTK3 UI runs on the main thread (required by GTK).
 */

#include "log.h"
#include "proc.h"
#include "profile.h"
#include "fdmon.h"
#include "settings.h"

#include <gtk/gtk.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

/* Monotonic timestamp (seconds) recorded at program startup */
struct timespec evemon_start_time;

/* Set by --safe-mode: blocks plugin loading on startup. */
int evemon_safe_mode = 0;

/* Global so the signal handler can reach it. */
static monitor_state_t g_state;

/*
 * Shutdown flag set atomically from the signal handler.
 * The monitor thread and UI poll this to know when to exit.
 * Using sig_atomic_t avoids calling pthread_mutex_lock() from
 * a signal handler, which is undefined behaviour per POSIX and
 * deadlocks if the signal fires while the lock is already held.
 */
static volatile sig_atomic_t g_shutdown_requested = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

/*
 * GLib idle callback that checks the atomic flag and performs the
 * actual (thread-safe) shutdown from the main loop context.
 */
static gboolean check_shutdown(gpointer data)
{
    (void)data;
    if (g_shutdown_requested) {
        pthread_mutex_lock(&g_state.lock);
        g_state.running = 0;
        pthread_cond_broadcast(&g_state.updated);
        pthread_mutex_unlock(&g_state.lock);

        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char *argv[])
{
    clock_gettime(CLOCK_MONOTONIC, &evemon_start_time);

    /* ── CLI argument parsing ──────────────────────────────────── */
    static const struct option long_opts[] = {
        { "debug",       no_argument,       NULL, 'd' },
        { "debug-audio", no_argument,       NULL, 'a' },
        { "pid",         required_argument, NULL, 'p' },
        { "safe-mode",   no_argument,       NULL, 's' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    pid_t cli_pid = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "dap:sh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd':
            evemon_debug = 1;
            break;
        case 'a':
            evemon_debug_audio = 1;
            break;
        case 'p':
            cli_pid = (pid_t)atoi(optarg);
            if (cli_pid <= 0) {
                fprintf(stderr, "evemon: invalid PID '%s'\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 's':
            evemon_safe_mode = 1;
            break;
        case 'h':
            printf("Usage: evemon [OPTIONS]\n"
                   "\n"
                   "Options:\n"
                   "  -d, --debug        Enable verbose debug logging\n"
                   "  -a, --debug-audio  Enable audio/PipeWire debug logging\n"
                   "  -p, --pid <PID>    Pre-select a process by PID on startup\n"
                   "  -s, --safe-mode    Start without loading any plugins\n"
                   "  -h, --help         Show this help message\n");
            return EXIT_SUCCESS;
        default:
            fprintf(stderr, "evemon: unknown option. Try --help.\n");
            return EXIT_FAILURE;
        }
    }

    /* Collapse consumed args so GTK only sees the remainder */
    argc -= optind;
    argv += optind;

    #define STARTUP_TS(label) do { \
        struct timespec _ts; \
        clock_gettime(CLOCK_MONOTONIC, &_ts); \
        double _e = (double)(_ts.tv_sec  - evemon_start_time.tv_sec) + \
                    (double)(_ts.tv_nsec - evemon_start_time.tv_nsec) / 1e9; \
        evemon_log(LOG_DEBUG, "[startup] %+7.3f s  %s", _e, label); \
    } while (0)

    STARTUP_TS("begin");
    profile_init();
    STARTUP_TS("profile_init done");
    FcInit();
    STARTUP_TS("FcInit done");
    g_set_prgname("evemon");
    gtk_init(&argc, &argv);
    STARTUP_TS("gtk_init done");

    if (monitor_state_init(&g_state) != 0) {
        evemon_log(LOG_ERROR, "evemon: failed to initialise state");
        return EXIT_FAILURE;
    }

    /* Fast-path the detail panel for the preselected PID.
     * CLI --pid takes precedence over the saved setting. */
    if (cli_pid > 0)
        settings_get()->preselected_pid = cli_pid;
    g_state.preselect_pid = settings_get()->preselected_pid;

    /* Create the eBPF fd/network monitor (best-effort: NULL on failure) */
    g_state.fdmon = fdmon_create(FDMON_BACKEND_AUTO);
    STARTUP_TS("fdmon_create done");

    /* Handle Ctrl-C gracefully */
    struct sigaction sa = { .sa_handler = on_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Start monitor in a background thread */
    pthread_t mon_tid;
    if (pthread_create(&mon_tid, NULL, monitor_thread, &g_state) != 0) {
        evemon_log(LOG_ERROR, "evemon: failed to start monitor thread");
        monitor_state_destroy(&g_state);
        return EXIT_FAILURE;
    }

    /*
     * Poll the atomic shutdown flag from the GTK main loop.
     * This bridges the async-signal-safe flag into the thread-safe
     * mutex/cond world without calling pthread functions from a
     * signal handler.
     */
    g_timeout_add(50, check_shutdown, NULL);

    STARTUP_TS("ui_thread start");

    /* Run GTK UI on the main thread (ui_thread calls gtk_main) */
    ui_thread(&g_state);

    /* Signal monitor to stop and wait for it */
    pthread_mutex_lock(&g_state.lock);
    g_state.running = 0;
    pthread_cond_broadcast(&g_state.updated);
    pthread_mutex_unlock(&g_state.lock);

    pthread_join(mon_tid, NULL);
    if (g_state.fdmon)
        fdmon_destroy(g_state.fdmon);
    monitor_state_destroy(&g_state);

    return EXIT_SUCCESS;
}
