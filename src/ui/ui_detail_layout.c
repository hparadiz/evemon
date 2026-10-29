#include "ui_gtk_private.h"

/* ── selection / detail panel interaction ─────────────────────── */

void gtk_ui_on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    ui_ctx_t *ctx = (ui_ctx_t *)data;
    if (ctx->shutting_down) return;

    proc_detail_update(ctx);

    /* Immediately push the new PID to all follow-selection plugin instances
     * and kick a broker gather so audio/PipeWire plugins respond without
     * waiting for the next 1-second gtk_ui_on_refresh tick. */
    plugin_registry_t *preg = ctx->plugin_registry;
    if (preg && preg->count > 0) {
        pid_t sel_pid = 0;
        if (sel) {
            GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
            if (rows) {
                GtkTreePath *cpath =
                    gtk_tree_model_sort_convert_path_to_child_path(
                        ctx->sort_model, (GtkTreePath *)rows->data);
                GtkTreeModel *cmodel =
                    gtk_tree_model_sort_get_model(ctx->sort_model);
                GtkTreeIter citer;
                if (cpath && gtk_tree_model_get_iter(cmodel, &citer, cpath)) {
                    gint v = 0;
                    gtk_tree_model_get(cmodel, &citer, COL_PID, &v, -1);
                    sel_pid = (pid_t)v;
                }
                if (cpath) gtk_tree_path_free(cpath);
                g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
            }
        }

        for (size_t i = 0; i < preg->count; i++) {
            plugin_instance_t *inst = &preg->instances[i];
            /* SYSTEM plugins always track PID 1; never follow selection */
            if (inst->plugin && inst->plugin->role == EVEMON_ROLE_SYSTEM) continue;
            if (!inst->pinned && inst->is_active)
                plugin_instance_set_pid(inst, sel_pid, FALSE);
        }

        if (sel_pid <= 0)
            plugin_dispatch_clear_all(preg);

        broker_start(preg, ctx->mon ? ctx->mon->fdmon : NULL);
    }
}

/* ── Collapsible section constants ────────────────────────────── */

/* Animation duration for section reveal transitions (ms) */

/* ── detail panel: process icon watermark ────────────────────── */

/*
 * Draw the Steam game logo (if any) as a wide, faint watermark across
 * the top of the panel, then draw the process icon in the top-right corner.
 */
gboolean gtk_ui_on_detail_icon_draw(GtkWidget *widget, cairo_t *cr,
                                     gpointer data)
{
    ui_ctx_t *ctx = data;

    int widget_w = gtk_widget_get_allocated_width(widget);
    int widget_h = gtk_widget_get_allocated_height(widget);

    /* ── Steam game logo: wide banner, top of panel ─────────── */
    if (ctx->detail_steam_icon_pb) {
        int src_w = gdk_pixbuf_get_width(ctx->detail_steam_icon_pb);
        int src_h = gdk_pixbuf_get_height(ctx->detail_steam_icon_pb);

        /* Scale to fill ~70 % of the panel width, max 320 px tall */
        int target_w = (int)(widget_w * 0.70);
        double scale  = (double)target_w / src_w;
        int draw_h    = (int)(src_h * scale);
        if (draw_h > 320) { scale = 320.0 / src_h; }
        int draw_w    = (int)(src_w * scale);
        draw_h        = (int)(src_h * scale);

        /* Right-align with 8 px margin, top 8 px */
        int x = widget_w - draw_w - 8;
        int y = 8;

        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, scale, scale);
        gdk_cairo_set_source_pixbuf(cr, ctx->detail_steam_icon_pb, 0, 0);
        cairo_paint_with_alpha(cr, 0.18);
        cairo_restore(cr);
    }

    /* ── Process icon: small, top-right corner ───────────────── */
    if (ctx->detail_icon_pb) {
        int src_w = gdk_pixbuf_get_width(ctx->detail_icon_pb);
        int src_h = gdk_pixbuf_get_height(ctx->detail_icon_pb);

        /* Target size: 96 px, or half the widget height, whichever is smaller */
        int target = MIN(96, widget_h / 2);
        if (target < 16) return FALSE;

        double scale = (double)target / MAX(src_w, src_h);
        int draw_w = (int)(src_w * scale);
        int draw_h = (int)(src_h * scale);
        (void)draw_h;

        /* Top-right corner with 8 px margin */
        int x = widget_w - draw_w - 8;
        int y = 8;

        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, scale, scale);
        gdk_cairo_set_source_pixbuf(cr, ctx->detail_icon_pb, 0, 0);
        cairo_paint_with_alpha(cr, 1);
        cairo_restore(cr);
    }

    return FALSE;   /* don't consume the event */
}

/* ── detail panel: toggle visibility ─────────────────────────── */

void gtk_ui_detail_panel_relayout(ui_ctx_t *ctx);

void gtk_ui_on_toggle_detail_panel(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    gboolean active = gtk_check_menu_item_get_active(item);
    settings_get()->detail_panel_open = active;
    settings_save();
    if (active) {
        gtk_widget_show_all(ctx->detail_vbox);

        /* Only show the main (unpinned) detail panel if a process
         * is actually selected in the tree view.  Pinned panels
         * remain visible regardless. */
        GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
        GList *rows = sel ? gtk_tree_selection_get_selected_rows(sel, NULL) : NULL;
        if (!rows) {
            gtk_widget_hide(ctx->detail_panel);
        } else {
            g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
            /* Restore proc info tray state after show_all */
            if (ctx->proc_info_revealer && ctx->proc_info_summary) {
                if (ctx->proc_info_collapsed) {
                    gtk_revealer_set_reveal_child(
                        GTK_REVEALER(ctx->proc_info_revealer), FALSE);
                    gtk_widget_show(GTK_WIDGET(ctx->proc_info_summary));
                } else {
                    gtk_widget_hide(GTK_WIDGET(ctx->proc_info_summary));
                }
            }
        }
    } else {
        gtk_widget_hide(ctx->detail_vbox);
    }
}

/* ── process info tray: collapse / expand ────────────────────── */

void gtk_ui_on_proc_info_tray_toggle(GtkButton *btn, gpointer data)
{
    (void)btn;
    ui_ctx_t *ctx = data;
    ctx->proc_info_collapsed = !ctx->proc_info_collapsed;
    settings_get()->proc_info_open = !ctx->proc_info_collapsed;
    settings_save();

    gtk_revealer_set_reveal_child(
        GTK_REVEALER(ctx->proc_info_revealer), !ctx->proc_info_collapsed);

    /* Arrow: ◀ means "collapse left", ▶ means "expand right" */
    gtk_button_set_label(GTK_BUTTON(ctx->proc_info_toggle),
                         ctx->proc_info_collapsed ? "▶" : "◀");
    gtk_widget_set_tooltip_text(ctx->proc_info_toggle,
                                ctx->proc_info_collapsed
                                    ? "Show process info"
                                    : "Hide process info");

    /* Show/hide the compact inline summary */
    if (ctx->proc_info_collapsed)
        gtk_widget_show(GTK_WIDGET(ctx->proc_info_summary));
    else
        gtk_widget_hide(GTK_WIDGET(ctx->proc_info_summary));
}

/* ── detail panel: change dock position ──────────────────────── */

static int panel_handle_size(GtkWidget *paned, GtkOrientation orientation)
{
    GdkWindow *handle = gtk_paned_get_handle_window(GTK_PANED(paned));
    if (!handle) return 0;
    return orientation == GTK_ORIENTATION_VERTICAL
        ? gdk_window_get_height(handle) : gdk_window_get_width(handle);
}

static gboolean initialize_panel_sizes(gpointer data)
{
    ui_ctx_t *ctx = data;
    int width = gtk_widget_get_allocated_width(ctx->detail_paned);
    int height = gtk_widget_get_allocated_height(ctx->detail_paned);
    /* Size the outer system split first, then use its remaining space for
     * the detail split. These are initial positions, never resize limits. */
    if (ctx->system_panel && gtk_widget_get_mapped(ctx->system_panel)) {
        GtkWidget *paned = ctx->system_panel_paned;
        GtkOrientation orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(paned));
        width = gtk_widget_get_allocated_width(paned);
        height = gtk_widget_get_allocated_height(paned);
        gboolean vertical = orientation == GTK_ORIENTATION_VERTICAL;
        int extent = MAX(0, (vertical ? height : width) - panel_handle_size(paned, orientation));
        int size = vertical ? gtk_widget_get_allocated_height(ctx->system_panel)
                            : gtk_widget_get_allocated_width(ctx->system_panel);
        if (!g_object_get_data(G_OBJECT(ctx->system_panel), "initial-size-applied")) {
            size = MIN(vertical ? 200 : 320, extent / 2);
            gtk_paned_set_position(GTK_PANED(paned),
                gtk_paned_get_child1(GTK_PANED(paned)) == ctx->system_panel ? size : extent - size);
            g_object_set_data(G_OBJECT(ctx->system_panel), "initial-size-applied", GINT_TO_POINTER(1));
        }
        if (vertical) height = MAX(0, extent - size);
        else width = MAX(0, extent - size);
    }
    if (gtk_widget_get_mapped(ctx->detail_vbox) &&
        !g_object_get_data(G_OBJECT(ctx->detail_vbox), "initial-size-applied")) {
        GtkWidget *paned = ctx->detail_paned;
        GtkOrientation orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(paned));
        gboolean vertical = orientation == GTK_ORIENTATION_VERTICAL;
        int extent = MAX(0, (vertical ? height : width) - panel_handle_size(paned, orientation));
        int size = MIN(vertical ? 250 : 350, extent / 2);
        gtk_paned_set_position(GTK_PANED(paned),
            gtk_paned_get_child1(GTK_PANED(paned)) == ctx->detail_vbox ? size : extent - size);
        g_object_set_data(G_OBJECT(ctx->detail_vbox), "initial-size-applied", GINT_TO_POINTER(1));
    }
    return G_SOURCE_REMOVE;
}

void gtk_ui_on_panel_map(GtkWidget *panel, gpointer data)
{
    if (!g_object_get_data(G_OBJECT(panel), "initial-size-applied"))
        g_idle_add(initialize_panel_sizes, data);
}

void gtk_ui_on_panel_position_changed(GtkCheckMenuItem *item, gpointer data)
{
    panel_pos_data_t *d = data;
    if (!gtk_check_menu_item_get_active(item))
        return;   /* ignore radio deactivation */

    if (d->is_sys) {
        /* System plugin panel */
        if (d->ctx->system_panel_pos == d->pos)
            return;
        d->ctx->system_panel_pos = d->pos;
        /* Only relayout if the panel is currently visible (has a paned).
         * If it's hidden, the new position will be applied next time it shows. */
        if (d->ctx->system_panel_paned)
            system_panel_relayout(d->ctx);
        settings_get()->system_panel_position = (int)d->pos;
        settings_save();
    } else {
        /* Detail panel */
        if (d->ctx->detail_panel_pos == d->pos)
            return;   /* no change */
        d->ctx->detail_panel_pos = d->pos;
        gtk_ui_detail_panel_relayout(d->ctx);
        settings_get()->detail_panel_position = (int)d->pos;
        settings_save();
    }
}

/*
 * Reparent the detail panel into the correct paned position.
 *
 * detail_paned remains inside system_panel_paned after the system panel
 * is created. Reorient that existing splitter in place: replacing it in
 * content_box would detach the process view from the system divider.
 * The process view absorbs allocation changes; the detail panel keeps
 * its size while the system divider is dragged.
 */
void gtk_ui_detail_panel_relayout(ui_ctx_t *ctx)
{
    gboolean was_visible = gtk_widget_get_visible(ctx->detail_vbox);

    /* Remove detail_vbox and hpaned from their current parent.
     * g_object_ref ensures they survive the container removal. */
    g_object_ref(ctx->detail_vbox);
    g_object_ref(ctx->hpaned);

    GtkWidget *paned = ctx->detail_paned;
    gtk_container_remove(GTK_CONTAINER(paned), ctx->hpaned);
    gtk_container_remove(GTK_CONTAINER(paned), ctx->detail_vbox);

    /* Preserve this splitter's parent and allocation. */
    GtkOrientation orient;
    gboolean panel_is_child1;  /* TRUE = panel is pack1 (top/left) */

    switch (ctx->detail_panel_pos) {
    case PANEL_POS_TOP:
        orient = GTK_ORIENTATION_VERTICAL;
        panel_is_child1 = TRUE;
        break;
    case PANEL_POS_BOTTOM:
        orient = GTK_ORIENTATION_VERTICAL;
        panel_is_child1 = FALSE;
        break;
    case PANEL_POS_LEFT:
        orient = GTK_ORIENTATION_HORIZONTAL;
        panel_is_child1 = TRUE;
        break;
    case PANEL_POS_RIGHT:
        orient = GTK_ORIENTATION_HORIZONTAL;
        panel_is_child1 = FALSE;
        break;
    default:
        orient = GTK_ORIENTATION_VERTICAL;
        panel_is_child1 = FALSE;
        break;
    }

    gtk_orientable_set_orientation(GTK_ORIENTABLE(paned), orient);

    if (panel_is_child1) {
        gtk_paned_pack1(GTK_PANED(paned), ctx->detail_vbox, FALSE, TRUE);
        gtk_paned_pack2(GTK_PANED(paned), ctx->hpaned, TRUE, TRUE);
    } else {
        gtk_paned_pack1(GTK_PANED(paned), ctx->hpaned, TRUE, TRUE);
        gtk_paned_pack2(GTK_PANED(paned), ctx->detail_vbox, FALSE, TRUE);
    }

    /* Set a reasonable default position for the divider */
    if (orient == GTK_ORIENTATION_VERTICAL) {
        /* For top/bottom, give the panel ~250px */
        GtkAllocation alloc;
        gtk_widget_get_allocation(paned, &alloc);
        int total_h = alloc.height > 100 ? alloc.height : 700;
        int panel_h = 250;
        if (panel_is_child1)
            gtk_paned_set_position(GTK_PANED(paned), panel_h);
        else
            gtk_paned_set_position(GTK_PANED(paned), total_h - panel_h);
    } else {
        /* For left/right, give the panel ~350px */
        GtkAllocation alloc;
        gtk_widget_get_allocation(paned, &alloc);
        int total_w = alloc.width > 100 ? alloc.width : 1100;
        int panel_w = 350;
        if (panel_is_child1)
            gtk_paned_set_position(GTK_PANED(paned), panel_w);
        else
            gtk_paned_set_position(GTK_PANED(paned), total_w - panel_w);
    }

    g_object_unref(ctx->detail_vbox);
    g_object_unref(ctx->hpaned);

    gtk_widget_show_all(paned);

    /* Hide the main (unpinned) detail panel if nothing is selected */
    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
        GList *rows = sel ? gtk_tree_selection_get_selected_rows(sel, NULL) : NULL;
        if (!rows) {
            gtk_widget_hide(ctx->detail_panel);
        } else {
            g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
            /* Restore proc info tray state after show_all */
            if (ctx->proc_info_revealer && ctx->proc_info_summary) {
                if (ctx->proc_info_collapsed) {
                    gtk_revealer_set_reveal_child(
                        GTK_REVEALER(ctx->proc_info_revealer), FALSE);
                    gtk_widget_show(GTK_WIDGET(ctx->proc_info_summary));
                } else {
                    gtk_widget_hide(GTK_WIDGET(ctx->proc_info_summary));
                }
            }
        }
    }

    if (!was_visible)
        gtk_widget_hide(ctx->detail_vbox);
}


/* ── double-click: open detail panel for the activated row ────── */

void gtk_ui_on_row_activated(GtkTreeView       *view,
                             GtkTreePath       *path,
                             GtkTreeViewColumn *col,
                             gpointer           data)
{
    (void)view; (void)path; (void)col;
    ui_ctx_t *ctx = data;

    /* Double-click opens the detail panel (plugin tabs), not the sidebar */
    if (!gtk_widget_get_visible(ctx->detail_panel)) {
        gtk_check_menu_item_set_active(ctx->detail_panel_menu_item, TRUE);
    }
}

/* ── sort-click: enable follow-selection ──────────────────────── */

void on_sort_column_changed(GtkTreeSortable *sortable, gpointer data)
{
    (void)sortable;
    ui_ctx_t *ctx = data;
    ctx->follow_selection = TRUE;
}

/* ── user scroll: disable follow-selection ───────────────────── */

gboolean gtk_ui_on_tree_scroll_event(GtkWidget *widget, GdkEventScroll *ev,
                                     gpointer data)
{
    (void)widget; (void)ev;
    ui_ctx_t *ctx = data;
    ctx->follow_selection = FALSE;
    return FALSE;   /* let GTK handle the scroll normally */
}

