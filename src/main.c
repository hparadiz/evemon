/*
 * main.c – allmon entry point.
 *
 * The monitor thread runs in the background scanning /proc.
 * The GTK3 UI runs on the main thread (required by GTK).
 */

#include "proc.h"
#include "profile.h"

#include <gtk/gtk.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

/* Global so the signal handler can reach it. */
static monitor_state_t g_state;

static void on_sigint(int sig)
{
    (void)sig;

    pthread_mutex_lock(&g_state.lock);
    g_state.running = 0;
    pthread_cond_broadcast(&g_state.updated);   /* wake waiters */
    pthread_mutex_unlock(&g_state.lock);
}

int main(int argc, char *argv[])
{
    profile_init();
    FcInit();
    gtk_init(&argc, &argv);

    if (monitor_state_init(&g_state) != 0) {
        fprintf(stderr, "allmon: failed to initialise state\n");
        return EXIT_FAILURE;
    }

    /* Handle Ctrl-C gracefully */
    struct sigaction sa = { .sa_handler = on_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Start monitor in a background thread */
    pthread_t mon_tid;
    if (pthread_create(&mon_tid, NULL, monitor_thread, &g_state) != 0) {
        fprintf(stderr, "allmon: failed to start monitor thread\n");
        monitor_state_destroy(&g_state);
        return EXIT_FAILURE;
    }

    /* Run GTK UI on the main thread (ui_thread calls gtk_main) */
    ui_thread(&g_state);

    /* Signal monitor to stop and wait for it */
    pthread_mutex_lock(&g_state.lock);
    g_state.running = 0;
    pthread_cond_broadcast(&g_state.updated);
    pthread_mutex_unlock(&g_state.lock);

    pthread_join(mon_tid, NULL);
    monitor_state_destroy(&g_state);

    return EXIT_SUCCESS;
}
