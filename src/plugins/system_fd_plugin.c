/*
 * system_fd_plugin.c – File Descriptors plugin for the System Panel.
 *
 * Identical to fd_plugin.c but hardcoded to PID 1 (systemd/init)
 * and registered as EVEMON_ROLE_SYSTEM so it appears in the System Panel.
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <malloc.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.system_fd",
    "Files",
    "1.0",
    EVEMON_ROLE_SYSTEM,
    NULL
);

/* ── category definitions ────────────────────────────────────── */

enum {
    FD_CAT_FILES,
    FD_CAT_DEVICES,
    FD_CAT_NET_SOCKETS,
    FD_CAT_UNIX_SOCKETS,
    FD_CAT_OTHER_SOCKETS,
    FD_CAT_PIPES,
    FD_CAT_EVENTS,
    FD_CAT_OTHER,
    FD_CAT_COUNT
};

static const char *cat_labels[FD_CAT_COUNT] = {
    [FD_CAT_FILES]         = "Files",
    [FD_CAT_DEVICES]       = "Devices",
    [FD_CAT_NET_SOCKETS]   = "Network Sockets",
    [FD_CAT_UNIX_SOCKETS]  = "Unix Sockets",
    [FD_CAT_OTHER_SOCKETS] = "Other Sockets",
    [FD_CAT_PIPES]         = "Pipes & FIFOs",
    [FD_CAT_EVENTS]        = "Events & Timers",
    [FD_CAT_OTHER]         = "Other",
};

enum {
    COL_TYPE_MARKUP,
    COL_NAME_MARKUP,
    COL_PATH,        /* raw path for detail lookup */
    NUM_COLS
};

enum {
    DCOL_MARKUP,
    DCOL_NUM_COLS
};

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    GtkWidget      *vbox;
    GtkWidget      *scroll;
    GtkListStore   *store;
    GtkTreeView    *view;
    GtkWidget      *chk_desc;
    GtkWidget      *chk_dedup;
    GtkWidget      *search_entry;
    char            filter_text[256];
    pid_t           last_pid;
    gboolean        include_desc;
    gboolean        merge_dup;
    /* detail panel */
    GtkWidget      *detail_scroll;
    GtkListStore   *detail_store;
    GtkTreeView    *detail_view;
    char            selected_path[512];
    /* snapshot of all raw fds for detail lookup */
    evemon_fd_t    *snap_fds;
    size_t          snap_fd_count;
} fd_ctx_t;

/* ── classification ──────────────────────────────────────────── */

static int classify_fd(const char *path)
{
    if (!path || !path[0]) return FD_CAT_OTHER;

    if (strncmp(path, "socket:[", 8) == 0) return FD_CAT_NET_SOCKETS;
    if (strncmp(path, "/dev/", 5) == 0)    return FD_CAT_DEVICES;
    if (strncmp(path, "pipe:[", 6) == 0)   return FD_CAT_PIPES;
    if (strstr(path, "anon_inode:"))        return FD_CAT_EVENTS;

    if (path[0] == '/') return FD_CAT_FILES;

    return FD_CAT_OTHER;
}

static gboolean fd_should_show(const evemon_fd_t *fd)
{
    if (!fd || !fd->path[0])
        return FALSE;
    if (strstr(fd->path, " (deleted)") || strstr(fd->path, "(deleted)"))
        return FALSE;

    int cat = classify_fd(fd->path);
    return cat == FD_CAT_FILES || cat == FD_CAT_DEVICES;
}

static void fd_release_data(fd_ctx_t *ctx)
{
    gtk_list_store_clear(ctx->store);
    gtk_list_store_clear(ctx->detail_store);
    ctx->selected_path[0] = '\0';
    g_free(ctx->snap_fds);
    ctx->snap_fds = NULL;
    ctx->snap_fd_count = 0;
    malloc_trim(0);
}

/* ── signal callbacks ────────────────────────────────────────── */

static void populate_detail(fd_ctx_t *ctx, const char *path)
{
    gtk_list_store_clear(ctx->detail_store);
    if (!path || !path[0]) return;

    for (size_t i = 0; i < ctx->snap_fd_count; i++) {
        if (strcmp(ctx->snap_fds[i].path, path) != 0) continue;

        pid_t spid = ctx->snap_fds[i].source_pid;
        int   fdn  = ctx->snap_fds[i].fd;

        /* read /proc/<pid>/comm for process name */
        char comm[64] = "";
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)spid);
        FILE *f = fopen(comm_path, "r");
        if (f) {
            if (fgets(comm, sizeof(comm), f)) {
                size_t len = strlen(comm);
                if (len > 0 && comm[len-1] == '\n') comm[len-1] = '\0';
            }
            fclose(f);
        }
        if (!comm[0]) snprintf(comm, sizeof(comm), "%d", (int)spid);

        char *fd_esc   = g_markup_escape_text(comm, -1);
        char *markup = g_strdup_printf(
            "<span foreground=\"#6699cc\">%d</span>  %s"
            "  <span foreground=\"#888888\">(%d)</span>",
            fdn, fd_esc, (int)spid);
        g_free(fd_esc);

        GtkTreeIter it;
        gtk_list_store_append(ctx->detail_store, &it);
        gtk_list_store_set(ctx->detail_store, &it, DCOL_MARKUP, markup, -1);
        g_free(markup);
    }
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    fd_ctx_t *ctx = data;
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) {
        gtk_list_store_clear(ctx->detail_store);
        ctx->selected_path[0] = '\0';
        return;
    }

    gchar *path = NULL;
    gtk_tree_model_get(model, &iter, COL_PATH, &path, -1);
    if (path) {
        g_strlcpy(ctx->selected_path, path, sizeof(ctx->selected_path));
        g_free(path);
    }
    populate_detail(ctx, ctx->selected_path);
}

static void on_child_visible_notify(GObject *obj, GParamSpec *pspec,
                                    gpointer data)
{
    (void)obj;
    (void)pspec;
    fd_ctx_t *ctx = data;
    if (!gtk_widget_get_child_visible(ctx->vbox))
        fd_release_data(ctx);
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *fd_create_widget(void *opaque)
{
    fd_ctx_t *ctx = opaque;

    ctx->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    g_signal_connect(ctx->vbox, "notify::child-visible",
                     G_CALLBACK(on_child_visible_notify), ctx);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 4);
    gtk_widget_set_margin_top(hbox, 2);
    gtk_widget_set_margin_bottom(hbox, 2);

    ctx->chk_desc = gtk_check_button_new_with_label("Include Descendants");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->chk_desc),
                                  ctx->include_desc);
    gtk_widget_set_no_show_all(ctx->chk_desc, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), ctx->chk_desc, FALSE, FALSE, 0);

    ctx->chk_dedup = gtk_check_button_new_with_label("Merge Duplicates");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->chk_dedup),
                                  ctx->merge_dup);
    gtk_widget_set_no_show_all(ctx->chk_dedup, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), ctx->chk_dedup, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(ctx->vbox), hbox, FALSE, FALSE, 0);

    ctx->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->search_entry), "Filter\u2026");
    gtk_widget_set_margin_start(ctx->search_entry, 4);
    gtk_widget_set_margin_end(ctx->search_entry, 4);
    gtk_widget_set_margin_bottom(ctx->search_entry, 2);
    gtk_box_pack_start(GTK_BOX(ctx->vbox), ctx->search_entry, FALSE, FALSE, 0);

    ctx->store = gtk_list_store_new(NUM_COLS,
                                    G_TYPE_STRING,   /* COL_TYPE_MARKUP */
                                    G_TYPE_STRING,   /* COL_NAME_MARKUP */
                                    G_TYPE_STRING);  /* COL_PATH        */

    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    g_object_unref(ctx->store);

    gtk_tree_view_set_headers_visible(ctx->view, TRUE);
    gtk_tree_view_set_enable_search(ctx->view, FALSE);

    /* Type column */
    GtkCellRenderer *type_cell = gtk_cell_renderer_text_new();
    g_object_set(type_cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *type_col = gtk_tree_view_column_new_with_attributes(
        "Type", type_cell, "markup", COL_TYPE_MARKUP, NULL);
    gtk_tree_view_column_set_resizable(type_col, TRUE);
    gtk_tree_view_column_set_min_width(type_col, 80);
    gtk_tree_view_append_column(ctx->view, type_col);

    /* Name column */
    GtkCellRenderer *name_cell = gtk_cell_renderer_text_new();
    g_object_set(name_cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes(
        "Name", name_cell, "markup", COL_NAME_MARKUP, NULL);
    gtk_tree_view_column_set_resizable(name_col, TRUE);
    gtk_tree_view_column_set_expand(name_col, TRUE);
    gtk_tree_view_append_column(ctx->view, name_col);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);

    /* ── detail list (right pane) ── */
    ctx->detail_store = gtk_list_store_new(DCOL_NUM_COLS, G_TYPE_STRING);
    ctx->detail_view  = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->detail_store)));
    g_object_unref(ctx->detail_store);
    gtk_tree_view_set_headers_visible(ctx->detail_view, TRUE);
    gtk_tree_view_set_enable_search(ctx->detail_view, FALSE);

    GtkCellRenderer *dcell = gtk_cell_renderer_text_new();
    g_object_set(dcell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *dcol = gtk_tree_view_column_new_with_attributes(
        "FD  Process (PID)", dcell, "markup", DCOL_MARKUP, NULL);
    gtk_tree_view_append_column(ctx->detail_view, dcol);

    ctx->detail_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->detail_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->detail_scroll),
                      GTK_WIDGET(ctx->detail_view));
    gtk_widget_set_vexpand(ctx->detail_scroll, TRUE);

    /* ── hpaned joining both ── */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hpaned), ctx->scroll, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(hpaned), ctx->detail_scroll, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(hpaned), 340);

    gtk_box_pack_start(GTK_BOX(ctx->vbox), hpaned, TRUE, TRUE, 0);

    gtk_widget_show_all(ctx->vbox);

    return ctx->vbox;
}

/* ── display entry (for filtering + dedup) ──────────────────── */

typedef struct {
    int         fd;
    pid_t       source_pid;
    const char *path;
    const char *desc;
    int         cat;
    size_t      dup_count;
} fd_display_t;

static int path_cmp(const void *a, const void *b)
{
    const fd_display_t *ea = a, *eb = b;
    return strcmp(ea->path, eb->path);
}

static void fd_update(void *opaque, const evemon_proc_data_t *data)
{
    fd_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid)
        ctx->last_pid = data->pid;

    ctx->include_desc = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->chk_desc));
    ctx->merge_dup = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->chk_dedup));

    const char *filter_raw = gtk_entry_get_text(GTK_ENTRY(ctx->search_entry));
    g_strlcpy(ctx->filter_text, filter_raw ? filter_raw : "",
              sizeof(ctx->filter_text));
    gboolean has_filter = ctx->filter_text[0] != '\0';

    size_t raw_total = data->fd_count;
    fd_display_t *raw = g_new0(fd_display_t, raw_total > 0 ? raw_total : 1);
    size_t raw_count = 0;

    for (size_t i = 0; i < raw_total; i++) {
        if (!ctx->include_desc && data->fds[i].source_pid != data->pid)
            continue;
        if (!fd_should_show(&data->fds[i]))
            continue;
        if (has_filter) {
            const char *path = data->fds[i].path;
            const char *desc = data->fds[i].desc;
            if (!strcasestr(path, ctx->filter_text) &&
                !strcasestr(desc, ctx->filter_text))
                continue;
        }
        raw[raw_count].fd         = data->fds[i].fd;
        raw[raw_count].source_pid  = data->fds[i].source_pid;
        raw[raw_count].path        = data->fds[i].path;
        raw[raw_count].desc        = data->fds[i].desc;
        raw[raw_count].cat         = classify_fd(data->fds[i].path);
        raw[raw_count].dup_count   = 1;
        raw_count++;
    }

    fd_display_t *display = raw;
    size_t display_count = raw_count;

    if (ctx->merge_dup && raw_count > 1) {
        qsort(raw, raw_count, sizeof(fd_display_t), path_cmp);

        fd_display_t *merged = g_new0(fd_display_t, raw_count);
        size_t mi = 0;
        size_t i = 0;
        while (i < raw_count) {
            merged[mi] = raw[i];
            merged[mi].dup_count = 1;
            size_t j = i + 1;
            while (j < raw_count &&
                   merged[mi].cat == (int)raw[j].cat &&
                   strcmp(raw[i].path, raw[j].path) == 0) {
                merged[mi].dup_count++;
                j++;
            }
            mi++;
            i = j;
        }
        g_free(raw);
        display = merged;
        display_count = mi;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    /* Walk existing rows and update or remove them in-place */
    GtkTreeIter row;
    gboolean row_valid = gtk_tree_model_get_iter_first(model, &row);

    for (size_t i = 0; i < display_count; i++) {
        const char *type_label = cat_labels[display[i].cat];
        char *type_esc = g_markup_escape_text(type_label, -1);

        const char *name_raw = (display[i].desc && display[i].desc[0])
                               ? display[i].desc : display[i].path;
        char *name_esc = g_markup_escape_text(name_raw, -1);
        char *name_markup;
        if (display[i].dup_count > 1)
            name_markup = g_strdup_printf(
                "%s  <span foreground=\"#888888\">(%zu)</span>",
                name_esc, display[i].dup_count);
        else
            name_markup = g_strdup(name_esc);
        g_free(name_esc);

        if (row_valid) {
            gtk_list_store_set(ctx->store, &row,
                               COL_TYPE_MARKUP, type_esc,
                               COL_NAME_MARKUP, name_markup,
                               COL_PATH,        display[i].path, -1);
            row_valid = gtk_tree_model_iter_next(model, &row);
        } else {
            GtkTreeIter new_row;
            gtk_list_store_append(ctx->store, &new_row);
            gtk_list_store_set(ctx->store, &new_row,
                               COL_TYPE_MARKUP, type_esc,
                               COL_NAME_MARKUP, name_markup,
                               COL_PATH,        display[i].path, -1);
        }
        g_free(type_esc);
        g_free(name_markup);
    }

    /* Remove leftover rows */
    while (row_valid)
        row_valid = gtk_list_store_remove(ctx->store, &row);

    gtk_adjustment_set_value(vadj, scroll_pos);
    g_free(display);

    /* ── snapshot displayed fds for the detail panel ── */
    g_free(ctx->snap_fds);
    ctx->snap_fd_count = 0;
    ctx->snap_fds = g_new(evemon_fd_t, data->fd_count > 0 ? data->fd_count : 1);
    for (size_t i = 0; i < data->fd_count; i++) {
        if (!ctx->include_desc && data->fds[i].source_pid != data->pid)
            continue;
        if (!fd_should_show(&data->fds[i]))
            continue;
        if (has_filter &&
            !strcasestr(data->fds[i].path, ctx->filter_text) &&
            !strcasestr(data->fds[i].desc, ctx->filter_text))
            continue;
        ctx->snap_fds[ctx->snap_fd_count++] = data->fds[i];
    }

    /* refresh detail panel if a path is still selected */
    if (ctx->selected_path[0])
        populate_detail(ctx, ctx->selected_path);
}

static void fd_clear(void *opaque)
{
    fd_ctx_t *ctx = opaque;
    fd_release_data(ctx);
    ctx->last_pid = 0;
}

static void fd_destroy(void *opaque)
{
    fd_ctx_t *ctx = opaque;
    g_free(ctx->snap_fds);
    free(ctx);
}

static int fd_wants_update(void *opaque)
{
    fd_ctx_t *ctx = opaque;
    return ctx->vbox &&
           GTK_IS_WIDGET(ctx->vbox) &&
           gtk_widget_get_mapped(ctx->vbox) &&
           gtk_widget_get_child_visible(ctx->vbox);
}

/* ── plugin descriptor ───────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    fd_ctx_t *ctx = calloc(1, sizeof(fd_ctx_t));
    if (!ctx) return NULL;

    ctx->include_desc = TRUE;
    ctx->merge_dup    = TRUE;
    ctx->last_pid     = 1;   /* hardcoded to PID 1 */

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Files",
        .id            = "org.evemon.system_fd",
        .version       = "1.0",
        .data_needs    = evemon_NEED_FDS | evemon_NEED_DESCENDANTS,
        .plugin_ctx    = ctx,
        .create_widget = fd_create_widget,
        .update        = fd_update,
        .clear         = fd_clear,
        .destroy       = fd_destroy,
        .wants_update  = fd_wants_update,
        .role          = EVEMON_ROLE_SYSTEM,
        .dependencies  = NULL,
    };

    return p;
}
