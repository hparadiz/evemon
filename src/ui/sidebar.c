/*
 * sidebar.c – detail panel: show selected-process info & fd tree.
 */

#include "ui_internal.h"

/* ── sidebar: update detail panel from selection ─────────────── */

void sidebar_update(ui_ctx_t *ctx)
{
    if (!gtk_widget_get_visible(ctx->sidebar))
        return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    if (!sel) return;

    GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
    if (!rows) {
        /* No selection – clear all labels */
        gtk_label_set_text(ctx->sb_pid,       "–");
        gtk_label_set_text(ctx->sb_ppid,      "–");
        gtk_label_set_text(ctx->sb_user,      "–");
        gtk_label_set_text(ctx->sb_name,      "–");
        gtk_label_set_text(ctx->sb_cpu,       "–");
        gtk_label_set_text(ctx->sb_rss,       "–");
        gtk_label_set_text(ctx->sb_group_rss, "–");
        gtk_label_set_text(ctx->sb_group_cpu, "–");
        gtk_label_set_text(ctx->sb_start_time, "–");
        gtk_label_set_text(ctx->sb_container, "–");
        gtk_label_set_text(ctx->sb_service,   "–");
        gtk_label_set_text(ctx->sb_cwd,       "–");
        gtk_label_set_text(ctx->sb_cmdline,   "–");
        gtk_tree_store_clear(ctx->fd_store);
        return;
    }

    GtkTreePath *path = rows->data;
    /* path is from the sort model (the view's model); sort → underlying store */
    GtkTreePath *store_path = gtk_tree_model_sort_convert_path_to_child_path(
        ctx->sort_model, path);
    GtkTreeModel *child_model = gtk_tree_model_sort_get_model(ctx->sort_model);
    GtkTreeIter iter;
    if (!store_path ||
        !gtk_tree_model_get_iter(child_model, &iter, store_path)) {
        if (store_path) gtk_tree_path_free(store_path);
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
        return;
    }
    gtk_tree_path_free(store_path);

    gint pid = 0, ppid = 0, cpu_raw = 0, rss = 0, grp_rss = 0, grp_cpu = 0;
    gint64 start_epoch = 0;
    gchar *user = NULL, *name = NULL, *cpu_text = NULL;
    gchar *rss_text = NULL, *grp_rss_text = NULL, *grp_cpu_text = NULL;
    gchar *start_time_text = NULL, *container = NULL, *service = NULL,
          *cwd = NULL, *cmdline = NULL;

    gtk_tree_model_get(child_model, &iter,
                       COL_PID,            &pid,
                       COL_PPID,           &ppid,
                       COL_USER,           &user,
                       COL_NAME,           &name,
                       COL_CPU_TEXT,       &cpu_text,
                       COL_RSS_TEXT,       &rss_text,
                       COL_GROUP_RSS_TEXT, &grp_rss_text,
                       COL_GROUP_CPU_TEXT, &grp_cpu_text,
                       COL_START_TIME,     &start_epoch,
                       COL_START_TIME_TEXT, &start_time_text,
                       COL_CONTAINER,      &container,
                       COL_SERVICE,        &service,
                       COL_CWD,           &cwd,
                       COL_CMDLINE,        &cmdline,
                       -1);
    (void)cpu_raw; (void)rss; (void)grp_rss; (void)grp_cpu;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", pid);
    gtk_label_set_text(ctx->sb_pid, buf);

    snprintf(buf, sizeof(buf), "%d", ppid);
    gtk_label_set_text(ctx->sb_ppid, buf);

    gtk_label_set_text(ctx->sb_user,      user      ? user      : "–");
    gtk_label_set_text(ctx->sb_name,      name      ? name      : "–");
    gtk_label_set_text(ctx->sb_cpu,       cpu_text  ? cpu_text  : "–");
    gtk_label_set_text(ctx->sb_rss,       rss_text  ? rss_text  : "–");
    gtk_label_set_text(ctx->sb_group_rss, grp_rss_text ? grp_rss_text : "–");
    gtk_label_set_text(ctx->sb_group_cpu, grp_cpu_text ? grp_cpu_text : "–");
    if (start_time_text && start_epoch > 0) {
        char fuzzy[64];
        format_fuzzy_time((time_t)start_epoch, fuzzy, sizeof(fuzzy));
        char combined[192];
        snprintf(combined, sizeof(combined), "%s (%s)", start_time_text, fuzzy);
        gtk_label_set_text(ctx->sb_start_time, combined);
    } else {
        gtk_label_set_text(ctx->sb_start_time, start_time_text ? start_time_text : "–");
    }
    gtk_label_set_text(ctx->sb_container, (container && container[0]) ? container : "–");
    gtk_label_set_text(ctx->sb_service,   (service && service[0])     ? service   : "–");
    gtk_label_set_text(ctx->sb_cwd,       cwd       ? cwd       : "–");
    gtk_label_set_text(ctx->sb_cmdline,   cmdline   ? cmdline   : "–");

    /* ── populate file descriptor tree (async, off main thread) ── */
    fd_scan_start(ctx, (pid_t)pid);

    g_free(user); g_free(name); g_free(cpu_text);
    g_free(rss_text); g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(start_time_text); g_free(container); g_free(service);
    g_free(cwd); g_free(cmdline);

    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
}

void on_fd_desc_toggled(GtkToggleButton *btn, gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->fd_include_desc = gtk_toggle_button_get_active(btn);
    gtk_tree_store_clear(ctx->fd_store);
    sidebar_update(ctx);
}

void on_fd_group_dup_toggled(GtkToggleButton *btn, gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->fd_group_dup_active = gtk_toggle_button_get_active(btn);
    gtk_tree_store_clear(ctx->fd_store);
    sidebar_update(ctx);
}

void on_fd_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->fd_store), iter,
                       FD_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < FD_CAT_COUNT)
        ctx->fd_collapsed |= (1u << cat_id);
}

void on_fd_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                        GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->fd_store), iter,
                       FD_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < FD_CAT_COUNT)
        ctx->fd_collapsed &= ~(1u << cat_id);
}

gboolean on_fd_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)data;

    if (ev->keyval != GDK_KEY_Return && ev->keyval != GDK_KEY_KP_Enter)
        return FALSE;

    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return FALSE;

    /* Only act on top-level category rows (FD_COL_CAT >= 0) */
    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, FD_COL_CAT, &cat_id, -1);
    if (cat_id < 0)
        return FALSE;

    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    if (!path)
        return FALSE;

    if (gtk_tree_view_row_expanded(view, path))
        gtk_tree_view_collapse_row(view, path);
    else
        gtk_tree_view_expand_row(view, path, FALSE);

    gtk_tree_path_free(path);
    return TRUE;
}
