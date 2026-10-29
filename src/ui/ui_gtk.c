#include "ui_gtk_private.h"

#include "ui_build.h"
static gboolean close_requested(GtkWidget *window, GdkEvent *event, gpointer data)
{
    (void)window; (void)event; (void)data;
    gtk_main_quit();
    return TRUE; /* Keep widgets alive until their plugin workers have stopped. */
}

/* ── UI thread ───────────────────────────────────────────────── */

void *ui_thread(void *arg)
{
    static gboolean have_geometry, maximized;
    static gint width, height, x, y;
    static ui_build_t builder;
    memset(&builder, 0, sizeof(builder));
    ui_build_t *b = &builder;
    b->mon = arg;
    gtk_build_tree(b);
    gtk_build_sidebar(b);
    gtk_build_menus(b);
    gtk_build_bind(b);
    if (have_geometry) {
        gtk_window_resize(GTK_WINDOW(b->window), width, height);
        gtk_window_move(GTK_WINDOW(b->window), x, y);
        if (maximized) gtk_window_maximize(GTK_WINDOW(b->window));
    }
    g_signal_connect(b->window, "delete-event", G_CALLBACK(close_requested), NULL);
    /* ── show & run ──────────────────────────────────────────── */
    gtk_widget_show_all(b->window);
    gtk_widget_hide(b->menubar);        /* hidden by default; toggle via status-bar right-click */

    /* Detail panel: honour settings (default = hidden) */
    if (settings_get()->detail_panel_open) {
        gtk_check_menu_item_set_active(b->ctx.detail_panel_menu_item, TRUE);
        /* proc info tray state */
        if (b->ctx.proc_info_collapsed) {
            gtk_revealer_set_reveal_child(
                GTK_REVEALER(b->ctx.proc_info_revealer), FALSE);
            gtk_button_set_label(GTK_BUTTON(b->ctx.proc_info_toggle), "▶");
            gtk_widget_show(GTK_WIDGET(b->ctx.proc_info_summary));
        }
    } else {
        gtk_widget_hide(b->detail_vbox);
    }

    /* System panel: honour settings (default = hidden, only if plugins loaded) */
    if (b->ctx.system_panel) {
        if (settings_get()->system_panel_open && b->ctx.system_panel_has_plugins) {
            gtk_check_menu_item_set_active(b->ctx.system_panel_menu_item, TRUE);
        } else {
            gtk_widget_hide(b->ctx.system_panel);
        }
    }
    gtk_main();

    gtk_window_get_size(GTK_WINDOW(b->window), &width, &height);
    gtk_window_get_position(GTK_WINDOW(b->window), &x, &y);
    maximized = gtk_window_is_maximized(GTK_WINDOW(b->window));
    have_geometry = TRUE;

    ui_ctx_destroy(&b->ctx);
    g_object_unref(b->sort_model);
    g_object_unref(b->store);
    g_object_unref(b->accel_group);
    g_object_unref(b->css); g_object_unref(b->filt_css);
    g_object_unref(b->pid_entry_css); g_object_unref(b->sidebar_css);
    return NULL;
}

/* ── Fix 6: cleanup ──────────────────────────────────────────── */

void ui_ctx_destroy(ui_ctx_t *ctx)
{
    ctx->shutting_down = TRUE;
    plugin_loader_cancel_scan();
    broker_destroy();
    /* Drain ownership-transfer callbacks after both producers have stopped. */
    for (unsigned i = 0; i < 1024 && g_main_context_pending(NULL); ++i)
        g_main_context_iteration(NULL, FALSE);
    /* A host change reuses the main loop; no old timer may address its next
     * context. Includes the fast/steady refresh sources and input timers. */
    while (g_source_remove_by_user_data(ctx)) {}
    ctx->scroll_timer = ctx->highlight_timer = ctx->filter_hide_timer = 0;

    /* Cancel filter auto-hide timer */
    filter_cancel_hide_timer(ctx);

    /* Free the shadow filter store */
    if (ctx->filter_store) {
        g_object_unref(ctx->filter_store);
        ctx->filter_store = NULL;
    }

#ifdef HAVE_PIPEWIRE
    /* Stop all spectrogram instances and peak meters
     * (may have been started by PipeWire plugins via host services) */
    while (ctx->spectro_count > 0) {
        GtkDrawingArea *da = GTK_DRAWING_AREA(
            ctx->spectro_instances[0].draw_area);
        spectrogram_stop(ctx, da);
    }
    pw_meter_stop(ctx);
#endif

    evemon_event_bus_unsubscribe(ctx->gpu_subscription_id);
    ctx->gpu_subscription_id = 0;
    /* Destroy widgets while their plugin code and contexts are still alive:
     * unrealize handlers must run before plugin_registry_destroy/dlclose. */
    GtkWidget *window = gtk_widget_get_toplevel(GTK_WIDGET(ctx->view));
    g_signal_handlers_disconnect_by_func(window, G_CALLBACK(gtk_ui_on_destroy), ctx);
    gtk_widget_destroy(window);

    /* Stop autoscroll timer */
    if (ctx->scroll_timer) {
        g_source_remove(ctx->scroll_timer);
        ctx->scroll_timer = 0;
    }

    /* Stop highlight fade timer */
    if (ctx->highlight_timer) {
        g_source_remove(ctx->highlight_timer);
        ctx->highlight_timer = 0;
    }

    /* Free the process tree node set */
    free(ctx->ptree_nodes.pinned_pids);
    free(ctx->ptree_nodes.pids);
    free(ctx->ptree_nodes.states);
    ctx->ptree_nodes.pinned_pids = NULL;
    ctx->ptree_nodes.pids        = NULL;
    ctx->ptree_nodes.states      = NULL;
    ctx->ptree_nodes.count       = 0;
    ctx->ptree_nodes.capacity    = 0;

    /* Free the collapsed PID set */
    free(ctx->collapsed_pids.pids);
    ctx->collapsed_pids.pids     = NULL;
    ctx->collapsed_pids.count    = 0;
    ctx->collapsed_pids.capacity = 0;

    /* Cancel the broker FIRST — a worker thread may still be running
     * and its completion callback references ctx->audio_pids. */
    broker_destroy();

#ifdef HAVE_PIPEWIRE
    pw_watcher_stop();
#endif

    /* Free the audio PID set (safe now that the broker is torn down) */
    free(ctx->audio_pids);
    ctx->audio_pids      = NULL;
    ctx->audio_pid_count = 0;
    ctx->audio_pid_cap   = 0;

    /* Free Steam display label side-table */
    if (ctx->steam_map) {
        steam_map_destroy(ctx->steam_map);
        ctx->steam_map = NULL;
    }

    /* Free process icon cache */
    if (ctx->icon_ctx) {
        proc_icon_ctx_free(ctx->icon_ctx);
        ctx->icon_ctx = NULL;
    }
    if (ctx->icon_ctx_large) {
        proc_icon_ctx_free(ctx->icon_ctx_large);
        ctx->icon_ctx_large = NULL;
    }

    /* Free the pinned PIDs set */
    free(ctx->pinned_pids);
    ctx->pinned_pids     = NULL;
    ctx->pinned_count    = 0;
    ctx->pinned_capacity = 0;

    /* Free pinned detail panels (plugin instances freed by registry_destroy) */
    for (size_t i = 0; i < ctx->pinned_panel_count; i++)
        free(ctx->pinned_panels[i].instance_ids);
    free(ctx->pinned_panels);
    ctx->pinned_panels      = NULL;
    ctx->pinned_panel_count = 0;
    ctx->pinned_panel_cap   = 0;

    /* Close any floating plugin windows — destroy the GTK window (which
     * fires on_plugin_window_destroyed → plugin_instance_destroy) and
     * then free the tracking array.  We disconnect the signal first to
     * avoid a double-free: plugin_registry_destroy below will also
     * clean up any surviving instances. */
    for (size_t i = 0; i < ctx->plugin_window_count; i++) {
        if (ctx->plugin_windows[i].window) {
            g_signal_handlers_disconnect_by_func(
                ctx->plugin_windows[i].window,
                G_CALLBACK(on_plugin_window_delete), ctx);
            gtk_widget_destroy(ctx->plugin_windows[i].window);
        }
    }
    free(ctx->plugin_windows);
    ctx->plugin_windows      = NULL;
    ctx->plugin_window_count = 0;
    ctx->plugin_window_cap   = 0;

    /* Stop plugin workers and let subscribers detach while the event bus is
     * still alive.  The GPU service publishes from its evemon thread, so
     * destroying the bus first would race its final publish. */
    if (ctx->plugin_registry) {
        plugin_registry_destroy(ctx->plugin_registry);
        free(ctx->plugin_registry);
        ctx->plugin_registry = NULL;
    }
    g_free(ctx->gpu_snapshot.devices);
    g_free(ctx->gpu_snapshot.processes);
    memset(&ctx->gpu_snapshot, 0, sizeof(ctx->gpu_snapshot));
    ctx->plugin_notebook = NULL;
    if (ctx->plugin_css) {
        gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(ctx->plugin_css));
        g_object_unref(ctx->plugin_css); ctx->plugin_css = NULL;
    }

    /* Destroy the GTK-free process store */
    proc_store_destroy(&ctx->pstore);
}
