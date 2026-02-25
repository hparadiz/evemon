/*
 * main.c – evemon entry point.
 *
 * The monitor thread runs in the background scanning /proc.
 * The GTK3 UI runs on the main thread (required by GTK).
 */

#include "proc.h"
#include "profile.h"
#include "fdmon.h"

#include <gtk/gtk.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

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
    profile_init();
    FcInit();
    g_set_prgname("evemon");
    gtk_init(&argc, &argv);

    //fprintf(stdout, "evemon: starting up...\n");
    if (monitor_state_init(&g_state) != 0) {
        fprintf(stderr, "evemon: failed to initialise state\n");
        return EXIT_FAILURE;
    }

    /* Create the eBPF fd/network monitor (best-effort: NULL on failure) */
    g_state.fdmon = fdmon_create(FDMON_BACKEND_AUTO);

    /* Handle Ctrl-C gracefully */
    struct sigaction sa = { .sa_handler = on_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Start monitor in a background thread */
    pthread_t mon_tid;
    if (pthread_create(&mon_tid, NULL, monitor_thread, &g_state) != 0) {
        fprintf(stderr, "evemon: failed to start monitor thread\n");
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
