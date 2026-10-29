#include "../ipc/query.h"
#include "../app/client_session.h"
#include "ui_gtk_private.h"

#include "ui_build.h"
void gtk_ui_connect_remote(GtkMenuItem *item, gpointer window);

static void disconnect_remote(GtkMenuItem *item, gpointer window)
{
    client_session_disconnect("gtk");
    gtk_ui_on_menu_restart(item, window);
}

void gtk_build_menus(ui_build_t *b)
{
    /* ── menu bar (hidden by default) ─────────────────────────── */
    b->menubar = gtk_menu_bar_new();

    /* File menu */
    b->file_menu = gtk_menu_new();
    b->file_item = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->file_item), b->file_menu);
    GtkWidget *connect_remote = gtk_menu_item_new_with_label("Connect to Remote…");
    g_signal_connect(connect_remote, "activate", G_CALLBACK(gtk_ui_connect_remote), b->window);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->file_menu), connect_remote);
    if (query_remote_path()) {
        GtkWidget *disconnect = gtk_menu_item_new_with_label("Disconnect");
        g_signal_connect(disconnect, "activate", G_CALLBACK(disconnect_remote), b->window);
        gtk_menu_shell_append(GTK_MENU_SHELL(b->file_menu), disconnect);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(b->file_menu), gtk_separator_menu_item_new());

    b->restart_item = gtk_menu_item_new_with_label("Restart");
    g_signal_connect(b->restart_item, "activate",
                     G_CALLBACK(gtk_ui_on_menu_restart), b->window);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->file_menu), b->restart_item);

    b->restart_admin_item = gtk_menu_item_new_with_label("Restart as Administrator");
    g_signal_connect(b->restart_admin_item, "activate",
                     G_CALLBACK(gtk_ui_on_menu_restart_as_admin), b->window);
    /* Grey it out when we're already root */
    if (geteuid() == 0 || query_is_foreign_host())
        gtk_widget_set_sensitive(b->restart_admin_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->file_menu), b->restart_admin_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->file_menu),
                          gtk_separator_menu_item_new());

    b->exit_item = gtk_menu_item_new_with_label("Exit");
    gtk_widget_add_accelerator(b->exit_item, "activate", b->accel_group,
                               GDK_KEY_q, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    g_signal_connect(b->exit_item, "activate", G_CALLBACK(gtk_ui_on_menu_exit), b->window);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->file_menu), b->exit_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->menubar), b->file_item);

    /* View menu → Appearance submenu */
    b->view_menu = gtk_menu_new();
    b->view_item = gtk_menu_item_new_with_label("View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->view_item), b->view_menu);

    /* Detail Panel toggle */
    b->detail_panel_toggle = gtk_check_menu_item_new_with_label("Detail Panel");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(b->detail_panel_toggle), FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->detail_panel_toggle);

    /* Detail Panel → Position submenu (radio items) */
    b->panel_pos_menu = gtk_menu_new();
    b->panel_pos_item = gtk_menu_item_new_with_label("Panel Position");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->panel_pos_item), b->panel_pos_menu);

    b->pos_group = NULL;


    b->pos_bottom = gtk_radio_menu_item_new_with_label(b->pos_group, "Bottom");
    b->pos_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(b->pos_bottom));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(b->pos_bottom), TRUE);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->panel_pos_menu), b->pos_bottom);

    b->pos_top = gtk_radio_menu_item_new_with_label(b->pos_group, "Top");
    b->pos_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(b->pos_top));
    gtk_menu_shell_append(GTK_MENU_SHELL(b->panel_pos_menu), b->pos_top);

    b->pos_left = gtk_radio_menu_item_new_with_label(b->pos_group, "Left");
    b->pos_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(b->pos_left));
    gtk_menu_shell_append(GTK_MENU_SHELL(b->panel_pos_menu), b->pos_left);

    b->pos_right = gtk_radio_menu_item_new_with_label(b->pos_group, "Right");
    b->pos_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(b->pos_right));
    gtk_menu_shell_append(GTK_MENU_SHELL(b->panel_pos_menu), b->pos_right);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->panel_pos_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu),
                          gtk_separator_menu_item_new());

    /* System Plugin Panel toggle */
    b->system_panel_toggle = gtk_check_menu_item_new_with_label(
        "System Plugin Panel");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(b->system_panel_toggle), FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->system_panel_toggle);

    /* System Plugin Panel → Position submenu (radio items) */
    b->system_panel_pos_menu = gtk_menu_new();
    b->system_panel_pos_item = gtk_menu_item_new_with_label(
        "System Panel Position");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->system_panel_pos_item),
                              b->system_panel_pos_menu);

    b->system_panel_pos_group = NULL;


    b->system_panel_pos_bottom = gtk_radio_menu_item_new_with_label(
        b->system_panel_pos_group, "Bottom");
    b->system_panel_pos_group = gtk_radio_menu_item_get_group(
        GTK_RADIO_MENU_ITEM(b->system_panel_pos_bottom));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(b->system_panel_pos_bottom), TRUE);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->system_panel_pos_menu), b->system_panel_pos_bottom);

    b->system_panel_pos_top = gtk_radio_menu_item_new_with_label(
        b->system_panel_pos_group, "Top");
    b->system_panel_pos_group = gtk_radio_menu_item_get_group(
        GTK_RADIO_MENU_ITEM(b->system_panel_pos_top));
    gtk_menu_shell_append(GTK_MENU_SHELL(b->system_panel_pos_menu), b->system_panel_pos_top);

    b->system_panel_pos_left = gtk_radio_menu_item_new_with_label(
        b->system_panel_pos_group, "Left");
    b->system_panel_pos_group = gtk_radio_menu_item_get_group(
        GTK_RADIO_MENU_ITEM(b->system_panel_pos_left));
    gtk_menu_shell_append(GTK_MENU_SHELL(b->system_panel_pos_menu), b->system_panel_pos_left);

    b->system_panel_pos_right = gtk_radio_menu_item_new_with_label(
        b->system_panel_pos_group, "Right");
    b->system_panel_pos_group = gtk_radio_menu_item_get_group(
        GTK_RADIO_MENU_ITEM(b->system_panel_pos_right));
    gtk_menu_shell_append(GTK_MENU_SHELL(b->system_panel_pos_menu), b->system_panel_pos_right);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->system_panel_pos_item);

    /* Columns visibility submenu */

    b->columns_item = gtk_menu_item_new_with_label("Columns");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->columns_item),
                              gtk_ui_build_columns_submenu(GTK_TREE_VIEW(b->tree),
                                                    b->gpu_column_menu_items));
    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->columns_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu),
                          gtk_separator_menu_item_new());

    /* Filter & Go-to-PID with hotkey hints */
    b->filter_item = gtk_menu_item_new_with_label("Filter by Name");
    gtk_widget_add_accelerator(b->filter_item, "activate", b->accel_group,
                               GDK_KEY_f, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->filter_item);

    b->goto_pid_item = gtk_menu_item_new_with_label("Go to PID");
    gtk_widget_add_accelerator(b->goto_pid_item, "activate", b->accel_group,
                               GDK_KEY_g, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->goto_pid_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu),
                          gtk_separator_menu_item_new());

    /* Show Audio Processes Only – greyed out until the audio plugin loads */
    b->audio_only_item = gtk_check_menu_item_new_with_label(
        "Show Audio Processes Only");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(b->audio_only_item),
                                   settings_get()->show_audio_only);
    gtk_widget_set_sensitive(b->audio_only_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->audio_only_item);

    /* Show GPU Processes Only – enabled only after a supported GPU plugin
     * instance passes detection and is loaded. */
    b->gpu_only_item = gtk_check_menu_item_new_with_label(
        "Show GPU Processes Only");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(b->gpu_only_item), FALSE);
    gtk_widget_set_sensitive(b->gpu_only_item, FALSE);
    gtk_widget_set_no_show_all(b->gpu_only_item, TRUE);
    gtk_widget_hide(b->gpu_only_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->gpu_only_item);

    b->cpu_intensive_item = gtk_check_menu_item_new_with_label(
        "Show CPU-Intensive Processes Only");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(b->cpu_intensive_item),
                                   settings_get()->show_cpu_intensive);
    gtk_widget_set_tooltip_text(
        b->cpu_intensive_item,
        "Show groups using at least half of one logical CPU and the busy subprocesses within them");
    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->cpu_intensive_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu),
                          gtk_separator_menu_item_new());

    b->appear_menu = gtk_menu_new();
    b->appear_item = gtk_menu_item_new_with_label("Appearance");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->appear_item), b->appear_menu);

    b->font_inc = gtk_menu_item_new_with_label("Increase Font");
    gtk_widget_add_accelerator(b->font_inc, "activate", b->accel_group,
                               GDK_KEY_plus, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    b->font_dec = gtk_menu_item_new_with_label("Decrease Font");
    gtk_widget_add_accelerator(b->font_dec, "activate", b->accel_group,
                               GDK_KEY_minus, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    b->font_reset = gtk_menu_item_new_with_label("Reset Font");
    gtk_widget_add_accelerator(b->font_reset, "activate", b->accel_group,
                               GDK_KEY_0, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->appear_menu), b->font_inc);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->appear_menu), b->font_dec);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->appear_menu), b->font_reset);

    /* Theme picker submenu */
    gtk_menu_shell_append(GTK_MENU_SHELL(b->appear_menu),
                          gtk_separator_menu_item_new());
    b->theme_item = gtk_menu_item_new_with_label("Theme");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->theme_item),
                              gtk_ui_build_theme_submenu());
    gtk_menu_shell_append(GTK_MENU_SHELL(b->appear_menu), b->theme_item);

    /* Charting Theme submenu (spectrogram colour) — signals connected
     * after ctx is initialised below (ctx address is stable: static). */
    b->charting_theme_item = gtk_menu_item_new_with_label("Charting Theme");
    gtk_menu_shell_append(GTK_MENU_SHELL(b->appear_menu), b->charting_theme_item);
    /* submenu and signal connections deferred until ctx is set up */

    gtk_menu_shell_append(GTK_MENU_SHELL(b->view_menu), b->appear_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->menubar), b->view_item);

    /* Plugins menu — rebuilt dynamically each time it opens */
    b->plugins_menu = gtk_menu_new();
    b->plugins_item = gtk_menu_item_new_with_label("Plugins");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->plugins_item), b->plugins_menu);
    /* on_plugins_menu_map needs ctx, but ctx isn't fully initialised yet;
     * we connect the signal after ctx is set up (see below). */
    gtk_menu_shell_append(GTK_MENU_SHELL(b->menubar), b->plugins_item);

    /* Help menu */
    b->help_menu = gtk_menu_new();
    b->help_item = gtk_menu_item_new_with_label("Help");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->help_item), b->help_menu);

    b->compat_item = gtk_menu_item_new_with_label("Compatibility…");
    g_signal_connect(b->compat_item, "activate",
                     G_CALLBACK(gtk_ui_on_menu_compatibility), b->window);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->help_menu), b->compat_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->help_menu),
                          gtk_separator_menu_item_new());

    b->about_item = gtk_menu_item_new_with_label("About");
    g_signal_connect(b->about_item, "activate", G_CALLBACK(gtk_ui_on_menu_about), b->window);
    gtk_menu_shell_append(GTK_MENU_SHELL(b->help_menu), b->about_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(b->menubar), b->help_item);

    if (evemon_ui_available("qt") && evemon_ui_available("gtk")) {
        gtk_menu_shell_append(GTK_MENU_SHELL(b->appear_menu), gtk_separator_menu_item_new());
        GtkWidget *interface_item = gtk_menu_item_new_with_label("Interface");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(interface_item), build_frontend_submenu(b->window));
        gtk_menu_shell_append(GTK_MENU_SHELL(b->appear_menu), interface_item);
    }

    /* ── status bar (in event box for right-click) ───────────── */
    b->status = gtk_label_new(" Loading…");
    gtk_label_set_xalign(GTK_LABEL(b->status), 0.0f);

    b->status_right = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(b->status_right), 1.0f);

    b->status_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(b->status_hbox), b->status, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(b->status_hbox), b->status_right, FALSE, FALSE, 8);

    b->status_ebox = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(b->status_ebox), b->status_hbox);
    gtk_widget_add_events(b->status_ebox, GDK_BUTTON_PRESS_MASK);

    /* ── detail panel (wraps process info + plugin notebook) ──── */
    /* The detail panel contains a horizontal paned:
     *   pack1 = collapsible tray (toggle button + revealer(sidebar_frame))
     *   pack2 = plugin notebook (tabs for FD, Env, Mmap, Libs, Net, …)
     * The process info pane can be collapsed via the toggle button,
     * giving full width to the plugin notebook. */
    b->detail_panel = gtk_frame_new(NULL);

    /* Wrap sidebar_frame in a GtkRevealer for slide animation */
    b->proc_info_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(b->proc_info_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(b->proc_info_revealer),
                                         SECTION_TRANSITION_MS);
    gtk_revealer_set_reveal_child(GTK_REVEALER(b->proc_info_revealer), TRUE);
    gtk_widget_set_size_request(b->sidebar_frame, 280, -1);
    gtk_container_add(GTK_CONTAINER(b->proc_info_revealer), b->sidebar_overlay);

    /* Toggle button: thin handle on the LEFT to collapse/expand */
    b->proc_info_toggle = gtk_button_new_with_label("◀");
    gtk_widget_set_size_request(b->proc_info_toggle, 16, -1);
    gtk_widget_set_tooltip_text(b->proc_info_toggle, "Hide process info");

    /* Compact inline summary label (vertical text, shown when collapsed) */
    b->proc_info_summary_w = gtk_label_new("–");
    gtk_label_set_angle(GTK_LABEL(b->proc_info_summary_w), 90);
    gtk_label_set_ellipsize(GTK_LABEL(b->proc_info_summary_w), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(b->proc_info_summary_w, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(b->proc_info_summary_w, TRUE);
    gtk_widget_hide(b->proc_info_summary_w);  /* hidden until collapsed */

    {
        /* Minimal padding for toggle button + summary label */
        GtkCssProvider *tray_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(tray_css,
            "button { padding: 0; min-width: 14px; }"
            ".proc-info-summary { padding: 2px 0; font-size: 9pt; }",
            -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(b->proc_info_toggle),
            GTK_STYLE_PROVIDER(tray_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(b->proc_info_summary_w),
            GTK_STYLE_PROVIDER(tray_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(b->proc_info_summary_w),
            "proc-info-summary");
        g_object_unref(tray_css);
    }

    /* Tray container: [toggle] [summary] [revealer(sidebar_frame)] */
    b->tray_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(b->tray_box), b->proc_info_toggle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->tray_box), b->proc_info_summary_w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->tray_box), b->proc_info_revealer, TRUE, TRUE, 0);

    /* Inner horizontal paned: tray (process info) | notebook */
    b->detail_hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(b->detail_hpaned), b->tray_box, FALSE, FALSE);
    /* The notebook will be added to pack2 after plugin loading below. */
    gtk_container_add(GTK_CONTAINER(b->detail_panel), b->detail_hpaned);

    /* Box to hold pinned-process detail panels (stacked vertically) */
    b->pinned_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Keep the content at its usable size inside a shrinkable viewport. */
    GtkWidget *detail_contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(detail_contents), b->detail_panel, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(detail_contents), b->pinned_box, FALSE, FALSE, 0);
    GtkWidget *detail_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detail_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(detail_scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(detail_scroll), detail_contents);
    b->detail_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(b->detail_vbox), detail_scroll, TRUE, TRUE, 0);

    /* ── layout ──────────────────────────────────────────────── */
    /* Default: detail panel docked at bottom.
     * outer_paned (vertical): pack1 = hpaned, pack2 = detail_vbox */
    b->outer_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(b->outer_paned), b->hpaned, TRUE, TRUE);
    gtk_paned_pack2(GTK_PANED(b->outer_paned), b->detail_vbox, FALSE, TRUE);

    /* The system panel paned (wrapping outer_paned + system panel) is created
     * lazily in system_panel_relayout() only when a SYSTEM plugin is shown.
     * Until then outer_paned sits directly in vbox with no wasted space. */

    b->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_events(b->vbox, GDK_BUTTON_PRESS_MASK);
    gtk_box_pack_start(GTK_BOX(b->vbox), b->menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->vbox), b->outer_paned, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(b->vbox), b->status_ebox, FALSE, FALSE, 4);
    gtk_container_add(GTK_CONTAINER(b->window), b->vbox);

}
