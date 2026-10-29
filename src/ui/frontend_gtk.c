#include "ui_internal.h"
#include "../app/runtime.h"
#include "../app/client_session.h"
#include <fontconfig/fontconfig.h>
void gtk_ui_blur_init(void);
static gboolean shutdown_poll(gpointer unused)
{
    (void)unused;
    if (evemon_shutdown_requested && gtk_main_level() > 0) gtk_main_quit();
    return G_SOURCE_CONTINUE;
}
int evemon_ui_run(monitor_state_t *state, int argc, char **argv)
{
    FcInit();
    g_set_prgname("evemon");
    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "evemon: GTK cannot open a display; use --headless\n");
        return 1;
    }
    gtk_ui_blur_init();
    guint timer = g_timeout_add(50, shutdown_poll, NULL);
    do {
        ui_thread(state);
        if (evemon_shutdown_requested || !client_session_commit()) break;
        state = client_session_state();
    } while (1);
    g_source_remove(timer);
    return 0;
}
