#include "ui_internal.h"
#include "../app/runtime.h"
#include "../core/settings.h"

static void choose_frontend(GtkCheckMenuItem *item, gpointer data)
{
    if (!gtk_check_menu_item_get_active(item)) return;
    const char *name = g_object_get_data(G_OBJECT(item), "frontend");
    if (!strcmp(name, "gtk")) return;
    /* Keep the running interface checked, including after Cancel or a save error. */
    GSList *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
    for (GSList *p = group; p; p = p->next) {
        const char *choice = g_object_get_data(G_OBJECT(p->data), "frontend");
        if (choice && !strcmp(choice, "gtk"))
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(p->data), TRUE);
    }
    GtkWindow *parent = GTK_WINDOW(data);
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Switch to Qt?");
    gtk_window_set_title(GTK_WINDOW(dialog), "Switch interface");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
        "Evemon will restart to apply this change.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL,
                           "Restart", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT) return;
    if (evemon_set_preferred_ui(name)) {
        GtkWidget *error = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Could not save the interface preference.");
        gtk_dialog_run(GTK_DIALOG(error));
        gtk_widget_destroy(error);
        return;
    }
    evemon_request_restart();
    gtk_main_quit();
}
GtkWidget *build_frontend_submenu(GtkWidget *window)
{
    GtkWidget *menu = gtk_menu_new();
    GSList *group = NULL;
    const char *names[] = {"gtk", "qt"};
    for (size_t i = 0; i < G_N_ELEMENTS(names); ++i) {
        if (!evemon_ui_available(names[i])) continue;
        GtkWidget *item = gtk_radio_menu_item_new_with_label(group, !strcmp(names[i], "qt") ? "Qt" : "GTK");
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), !strcmp(names[i], "gtk"));
        g_object_set_data(G_OBJECT(item), "frontend", (gpointer)names[i]);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    /* Connect after the radio group has its initial state. */
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *p = items; p; p = p->next)
        g_signal_connect(p->data, "toggled", G_CALLBACK(choose_frontend), window);
    g_list_free(items);
    return menu;
}
