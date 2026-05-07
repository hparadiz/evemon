/*
 * system_panel.c – System Plugin Panel implementation.
 */

#include "ui_internal.h"
#include "../settings.h"
#include "../plugin_loader.h"
#include "../plugin_broker.h"
#include <malloc.h>

static void system_panel_release_plugins(ui_ctx_t *ctx)
{
    plugin_registry_t *preg = ctx ? ctx->plugin_registry : NULL;
    if (!preg) return;

    for (size_t i = 0; i < preg->count; i++) {
        plugin_instance_t *inst = &preg->instances[i];
        if (!inst->plugin || inst->plugin->role != EVEMON_ROLE_SYSTEM)
            continue;
        if (inst->plugin->clear)
            inst->plugin->clear(inst->plugin->plugin_ctx);
    }

    malloc_trim(0);
}

static void system_panel_restart_broker(ui_ctx_t *ctx)
{
    plugin_registry_t *preg = ctx ? ctx->plugin_registry : NULL;
    if (!preg) return;
    broker_start(preg, ctx->mon ? ctx->mon->fdmon : NULL);
}

/* ── panel-level close button callback ─────────────────────────── */

/*
 * Single "✕" button sits to the right of the tab bar (as a notebook
 * action widget).  Clicking it hides the entire system panel, which
 * is the same as unchecking View → System Plugin Panel.
 */
static void on_system_panel_close(GtkButton *btn, gpointer data)
{
    (void)btn;
    ui_ctx_t *ctx = data;

    if (!ctx->system_panel_menu_item) return;

    /* Toggle via the menu item so settings + visibility stay in sync. */
    g_signal_handlers_block_by_func(ctx->system_panel_menu_item,
                                     G_CALLBACK(on_toggle_system_panel),
                                     ctx);
    gtk_check_menu_item_set_active(ctx->system_panel_menu_item, FALSE);
    g_signal_handlers_unblock_by_func(ctx->system_panel_menu_item,
                                       G_CALLBACK(on_toggle_system_panel),
                                       ctx);

    if (ctx->system_panel) {
        gtk_widget_hide(ctx->system_panel);
        system_panel_release_plugins(ctx);
        system_panel_restart_broker(ctx);
    }

    settings_get()->system_panel_open = false;
    settings_save();
}

/* ── system_panel_add_plugin ───────────────────────────────────────── */

void system_panel_add_plugin(ui_ctx_t *ctx, GtkWidget *widget,
                             const char *label, int inst_id)
{
    (void)inst_id;   /* no per-tab close button; inst_id is unused here */
    if (!ctx->system_panel_notebook || !widget) return;

    GtkWidget *tab_lbl = gtk_label_new(label);
    gtk_notebook_append_page(GTK_NOTEBOOK(ctx->system_panel_notebook),
                             widget, tab_lbl);
    ctx->system_panel_has_plugins = TRUE;
}

/* ── system_panel_build ────────────────────────────────────────────── */

GtkWidget *system_panel_build(ui_ctx_t *ctx)
{
    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_widget_set_size_request(notebook, -1, 200);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(notebook), "evemon-plugins");
    if (ctx->plugin_css)
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(notebook),
            GTK_STYLE_PROVIDER(ctx->plugin_css),
            GTK_STYLE_PROVIDER_PRIORITY_USER);

    /* ── Single close button at the right end of the tab bar ── */
    GtkWidget *close_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_btn, FALSE);
    gtk_widget_set_tooltip_text(close_btn, "Hide System Panel");
    gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);

    GtkWidget *close_lbl = gtk_label_new("✕");
    gtk_container_add(GTK_CONTAINER(close_btn), close_lbl);

    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "button { padding: 0 4px; min-height: 0; min-width: 0; }",
            -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(close_btn),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

    g_signal_connect(close_btn, "clicked",
                     G_CALLBACK(on_system_panel_close), ctx);

    gtk_widget_show_all(close_btn);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook),
                                   close_btn, GTK_PACK_END);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(frame), notebook);
    gtk_widget_set_size_request(frame, -1, 200);

    ctx->system_panel_notebook = notebook;
    ctx->system_panel          = frame;

    gtk_widget_set_no_show_all(frame, TRUE);
    gtk_widget_hide(frame);

    return frame;
}

/* ── on_toggle_system_panel ────────────────────────────────────────── */

void on_toggle_system_panel(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    gboolean active = gtk_check_menu_item_get_active(item);

    settings_get()->system_panel_open = active;
    settings_save();

    if (!ctx->system_panel) return;

    if (active) {
        gtk_widget_set_no_show_all(ctx->system_panel, FALSE);
        gtk_widget_show_all(ctx->system_panel);
        gtk_widget_set_no_show_all(ctx->system_panel, TRUE);
        system_panel_restart_broker(ctx);
    } else {
        gtk_widget_hide(ctx->system_panel);
        system_panel_release_plugins(ctx);
        system_panel_restart_broker(ctx);
    }
}

/* ── system_panel_relayout ─────────────────────────────────────────── */

void system_panel_relayout(ui_ctx_t *ctx)
{
    if (!ctx->system_panel) return;

    g_object_ref(ctx->system_panel);
    g_object_ref(ctx->detail_paned);

    GtkWidget *old_paned = ctx->system_panel_paned;
    if (old_paned) {
        gtk_container_remove(GTK_CONTAINER(old_paned), ctx->detail_paned);
        gtk_container_remove(GTK_CONTAINER(old_paned), ctx->system_panel);
        gtk_container_remove(GTK_CONTAINER(ctx->content_box), old_paned);
    } else {
        /* First show: detail_paned is directly in content_box; pull it out. */
        gtk_container_remove(GTK_CONTAINER(ctx->content_box), ctx->detail_paned);
    }

    GtkOrientation orient;
    gboolean panel_is_child1;

    switch (ctx->system_panel_pos) {
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

    GtkWidget *new_paned = gtk_paned_new(orient);

    if (panel_is_child1) {
        gtk_paned_pack1(GTK_PANED(new_paned), ctx->system_panel, FALSE, FALSE);
        gtk_paned_pack2(GTK_PANED(new_paned), ctx->detail_paned, TRUE, FALSE);
    } else {
        gtk_paned_pack1(GTK_PANED(new_paned), ctx->detail_paned, TRUE, FALSE);
        gtk_paned_pack2(GTK_PANED(new_paned), ctx->system_panel, FALSE, FALSE);
    }

    if (orient == GTK_ORIENTATION_VERTICAL) {
        GtkAllocation alloc;
        gtk_widget_get_allocation(ctx->content_box, &alloc);
        int total_h = alloc.height > 100 ? alloc.height : 700;
        int panel_h = 200;
        gtk_paned_set_position(GTK_PANED(new_paned),
            panel_is_child1 ? panel_h : (total_h - panel_h));
    } else {
        GtkAllocation alloc;
        gtk_widget_get_allocation(ctx->content_box, &alloc);
        int total_w = alloc.width > 100 ? alloc.width : 1100;
        int panel_w = 320;
        gtk_paned_set_position(GTK_PANED(new_paned),
            panel_is_child1 ? panel_w : (total_w - panel_w));
    }

    ctx->system_panel_paned = new_paned;

    gtk_box_pack_start(GTK_BOX(ctx->content_box), new_paned, TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(ctx->content_box), new_paned, 1);

    /* Show the paned and its inner content, but preserve the panel's
     * current visibility — don't force it shown if it was hidden. */
    gboolean panel_was_visible = gtk_widget_get_visible(ctx->system_panel);
    gtk_widget_set_no_show_all(ctx->system_panel, TRUE);
    gtk_widget_show_all(new_paned);
    if (!panel_was_visible)
        gtk_widget_hide(ctx->system_panel);

    g_object_unref(ctx->system_panel);
    g_object_unref(ctx->detail_paned);
}
