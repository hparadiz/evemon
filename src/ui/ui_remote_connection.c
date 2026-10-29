#include "ui_internal.h"
#include "../app/client_session.h"
#include "../platform/desktop_session.h"

typedef struct {
    GtkWidget *dialog, *host, *socket;
    GtkWidget *run_daemon, *idle_shutdown, *idle_minutes;
    char *socket_hint;
    GCancellable *cancel;
    gboolean busy;
} remote_dialog_t;
static void dialog_free(gpointer opaque)
{
    remote_dialog_t *d = opaque;
    g_cancellable_cancel(d->cancel); g_object_unref(d->cancel);
    g_free(d->socket_hint); g_free(d);
}
static void connected(GObject *source, GAsyncResult *result, gpointer opaque)
{
    (void)source;
    GtkWidget *dialog = opaque;
    remote_dialog_t *d = g_object_get_data(G_OBJECT(dialog), "remote-dialog");
    GError *error = NULL;
    client_session_t *session = client_session_finish(result, &error);
    if (!g_cancellable_is_cancelled(d->cancel)) {
        if (session) {
            client_session_queue(session); session = NULL;
            gtk_widget_destroy(dialog); gtk_main_quit();
        } else {
            gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
            d->busy = FALSE;
        }
    }
    client_session_discard(session); g_clear_error(&error); g_object_unref(dialog);
}
static void destroyed(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    remote_dialog_t *d = opaque; g_cancellable_cancel(d->cancel);
}
static void response(GtkDialog *dialog, gint id, gpointer opaque)
{
    remote_dialog_t *d = opaque;
    if (id != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    if (d->busy) return;
    d->busy = TRUE;
    gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
    gboolean run_daemon = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(d->run_daemon));
    unsigned idle_minutes = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(d->idle_shutdown))
        ? (unsigned)gtk_spin_button_get_value_as_int(
              GTK_SPIN_BUTTON(d->idle_minutes))
        : 0;
    client_session_prepare(gtk_entry_get_text(GTK_ENTRY(d->host)),
                           gtk_entry_get_text(GTK_ENTRY(d->socket)),
                           run_daemon, idle_minutes,
                           d->cancel, connected, g_object_ref(dialog));
}
static void daemon_toggled(GtkToggleButton *button, gpointer opaque)
{
    remote_dialog_t *d = opaque;
    gboolean enabled = gtk_toggle_button_get_active(button);
    gtk_widget_set_sensitive(d->idle_shutdown, enabled);
    gtk_widget_set_sensitive(d->idle_minutes, enabled &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->idle_shutdown)));
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->socket),
        enabled ? d->socket_hint : IPC_SYSTEM_SOCKET);
}
static void idle_toggled(GtkToggleButton *button, gpointer opaque)
{
    remote_dialog_t *d = opaque;
    gtk_widget_set_sensitive(d->idle_minutes,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->run_daemon)) &&
        gtk_toggle_button_get_active(button));
}
void gtk_ui_connect_remote(GtkMenuItem *item, gpointer window)
{
    (void)item;
    remote_dialog_t *d = g_new0(remote_dialog_t, 1);
    d->cancel = g_cancellable_new();
    uid_t uid = desktop_session_uid();
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (uid == 0)
        d->socket_hint = g_strdup(IPC_SYSTEM_SOCKET);
    else if (runtime && *runtime)
        d->socket_hint = g_build_filename(runtime, "evemon", "query.sock", NULL);
    else
        d->socket_hint = g_strdup_printf("/tmp/evemon-%lu/query.sock",
                                         (unsigned long)uid);
    d->dialog = gtk_dialog_new_with_buttons("Connect to Remote", GTK_WINDOW(window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Cancel", GTK_RESPONSE_CANCEL,
        "Connect", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(d->dialog, "connectRemoteDialog");
    g_object_set_data_full(G_OBJECT(d->dialog), "remote-dialog", d, dialog_free);
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6); gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    d->host = gtk_entry_new(); d->socket = gtk_entry_new();
    gtk_widget_set_name(d->host, "connectRemoteHost"); gtk_widget_set_name(d->socket, "connectRemoteSocket");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->host), "Hostname, IP address, or user@host");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->socket), d->socket_hint);
    gtk_widget_set_tooltip_text(d->socket,
        "Blank uses a private socket for the SSH user when Run Daemon is enabled; otherwise it uses " IPC_SYSTEM_SOCKET ".");
    gtk_widget_set_hexpand(d->host, TRUE);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("SSH host"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), d->host, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Socket override (optional)"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), d->socket, 1, 1, 1, 1);
    d->run_daemon = gtk_check_button_new_with_label("Run Daemon on Host");
    d->idle_shutdown = gtk_check_button_new_with_label(
        "Self Shutdown Daemon after");
    d->idle_minutes = gtk_spin_button_new_with_range(1, 10080, 1);
    gtk_widget_set_name(d->run_daemon, "connectRemoteRunDaemon");
    gtk_widget_set_name(d->idle_shutdown, "connectRemoteIdleShutdown");
    gtk_widget_set_name(d->idle_minutes, "connectRemoteIdleMinutes");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->run_daemon), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->idle_shutdown), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->idle_minutes), 5);
    GtkWidget *idle_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(idle_row), d->idle_shutdown, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(idle_row), d->idle_minutes, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(idle_row), gtk_label_new("min"), FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), d->run_daemon, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), idle_row, 1, 3, 1, 1);
    g_signal_connect(d->run_daemon, "toggled", G_CALLBACK(daemon_toggled), d);
    g_signal_connect(d->idle_shutdown, "toggled", G_CALLBACK(idle_toggled), d);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d->dialog))), grid);
    gtk_dialog_set_default_response(GTK_DIALOG(d->dialog), GTK_RESPONSE_ACCEPT);
    gtk_entry_set_activates_default(GTK_ENTRY(d->host), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(d->socket), TRUE);
    g_signal_connect(d->dialog, "response", G_CALLBACK(response), d);
    g_signal_connect(d->dialog, "destroy", G_CALLBACK(destroyed), d);
    gtk_window_set_default_size(GTK_WINDOW(d->dialog), 460, -1);
    gtk_widget_show_all(d->dialog); gtk_widget_grab_focus(d->host);
}
