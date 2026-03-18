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

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.system_fd",
    "System FD",
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
    COL_TEXT,
    COL_MARKUP,
    COL_CAT,
    NUM_COLS
};

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    GtkWidget      *vbox;
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    GtkWidget      *chk_desc;
    GtkWidget      *chk_dedup;
    unsigned        collapsed;
    pid_t           last_pid;
    gboolean        include_desc;
    gboolean        merge_dup;
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

/* ── markup helper ───────────────────────────────────────────── */

static char *fd_to_markup(int fd_num, const char *path, const char *desc)
{
    const char *display = (desc && desc[0]) ? desc : path;
    char *esc = g_markup_escape_text(display, -1);
    char *markup = g_strdup_printf(
        "<span foreground=\"#6699cc\">%d</span>  %s", fd_num, esc);
    g_free(esc);
    return markup;
}

static char *fd_to_markup_grouped(const char *path, const char *desc,
                                  size_t count)
{
    const char *display = (desc && desc[0]) ? desc : path;
    char *esc = g_markup_escape_text(display, -1);
    char *markup = g_strdup_printf(
        "%s  <span foreground=\"#888888\">(%zu duplicates)</span>",
        esc, count);
    g_free(esc);
    return markup;
}

/* ── signal callbacks ────────────────────────────────────────── */

static void on_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                             GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    fd_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < FD_CAT_COUNT)
        ctx->collapsed |= (1u << cat);
}

static void on_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                            GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    fd_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < FD_CAT_COUNT)
        ctx->collapsed &= ~(1u << cat);
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *fd_create_widget(void *opaque)
{
    fd_ctx_t *ctx = opaque;

    ctx->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 4);
    gtk_widget_set_margin_top(hbox, 2);
    gtk_widget_set_margin_bottom(hbox, 2);

    ctx->chk_desc = gtk_check_button_new_with_label("Include Descendants");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->chk_desc),
                                  ctx->include_desc);
    gtk_box_pack_start(GTK_BOX(hbox), ctx->chk_desc, FALSE, FALSE, 0);

    ctx->chk_dedup = gtk_check_button_new_with_label("Merge Duplicates");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->chk_dedup),
                                  ctx->merge_dup);
    gtk_box_pack_start(GTK_BOX(hbox), ctx->chk_dedup, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(ctx->vbox), hbox, FALSE, FALSE, 0);

    ctx->store = gtk_tree_store_new(NUM_COLS,
                                    G_TYPE_STRING,
                                    G_TYPE_STRING,
                                    G_TYPE_INT);

    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    g_object_unref(ctx->store);

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);
    gtk_tree_view_set_enable_search(ctx->view, FALSE);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "FD", cell, "markup", COL_MARKUP, NULL);
    gtk_tree_view_append_column(ctx->view, col);

    g_signal_connect(ctx->view, "row-collapsed",
                     G_CALLBACK(on_row_collapsed), ctx);
    g_signal_connect(ctx->view, "row-expanded",
                     G_CALLBACK(on_row_expanded), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);

    gtk_box_pack_start(GTK_BOX(ctx->vbox), ctx->scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(ctx->vbox);

    return ctx->vbox;
}

/* ── display entry (for filtering + dedup) ──────────────────── */

typedef struct {
    int         fd;
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

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    ctx->include_desc = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->chk_desc));
    ctx->merge_dup = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->chk_dedup));

    size_t raw_total = data->fd_count;
    fd_display_t *raw = g_new0(fd_display_t, raw_total > 0 ? raw_total : 1);
    size_t raw_count = 0;

    for (size_t i = 0; i < raw_total; i++) {
        if (!ctx->include_desc && data->fds[i].source_pid != data->pid)
            continue;
        raw[raw_count].fd        = data->fds[i].fd;
        raw[raw_count].path      = data->fds[i].path;
        raw[raw_count].desc      = data->fds[i].desc;
        raw[raw_count].cat       = classify_fd(data->fds[i].path);
        raw[raw_count].dup_count = 1;
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

    size_t cat_count[FD_CAT_COUNT] = {0};
    for (size_t i = 0; i < display_count; i++)
        cat_count[display[i].cat]++;

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeIter cat_iters[FD_CAT_COUNT];
    gboolean    cat_exists[FD_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(model, &top, COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < FD_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    for (int c = 0; c < FD_CAT_COUNT; c++) {
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    for (int c = 0; c < FD_CAT_COUNT; c++) {
        if (cat_count[c] == 0) continue;

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s (%zu)", cat_labels[c], cat_count[c]);
        char *hdr_esc = g_markup_escape_text(hdr, -1);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            gtk_tree_store_append(ctx->store, &parent, NULL);
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr,
                               COL_MARKUP, hdr_esc,
                               COL_CAT, (gint)c, -1);
            cat_exists[c] = TRUE;
            cat_iters[c] = parent;
        } else {
            parent = cat_iters[c];
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr,
                               COL_MARKUP, hdr_esc, -1);
        }
        g_free(hdr_esc);

        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);

        for (size_t i = 0; i < display_count; i++) {
            if (display[i].cat != c) continue;

            char *markup;
            if (display[i].dup_count > 1)
                markup = fd_to_markup_grouped(display[i].path,
                                              display[i].desc,
                                              display[i].dup_count);
            else
                markup = fd_to_markup(display[i].fd, display[i].path,
                                      display[i].desc);

            if (child_valid) {
                gtk_tree_store_set(ctx->store, &child,
                                   COL_TEXT, display[i].path,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
                child_valid = gtk_tree_model_iter_next(model, &child);
            } else {
                GtkTreeIter new_child;
                gtk_tree_store_append(ctx->store, &new_child, &parent);
                gtk_tree_store_set(ctx->store, &new_child,
                                   COL_TEXT, display[i].path,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
            }
            g_free(markup);
        }

        while (child_valid)
            child_valid = gtk_tree_store_remove(ctx->store, &child);

        GtkTreePath *cat_path = gtk_tree_model_get_path(model, &cat_iters[c]);
        if (ctx->collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);
    g_free(display);
}

static void fd_clear(void *opaque)
{
    fd_ctx_t *ctx = opaque;
    gtk_tree_store_clear(ctx->store);
    ctx->last_pid = 0;
}

static void fd_destroy(void *opaque)
{
    fd_ctx_t *ctx = opaque;
    free(ctx);
}

/* ── plugin descriptor ───────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    fd_ctx_t *ctx = calloc(1, sizeof(fd_ctx_t));
    if (!ctx) return NULL;

    ctx->include_desc = TRUE;
    ctx->merge_dup    = FALSE;
    ctx->last_pid     = 1;   /* hardcoded to PID 1 */

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "System FD",
        .id            = "org.evemon.system_fd",
        .version       = "1.0",
        .data_needs    = evemon_NEED_FDS | evemon_NEED_DESCENDANTS,
        .plugin_ctx    = ctx,
        .create_widget = fd_create_widget,
        .update        = fd_update,
        .clear         = fd_clear,
        .destroy       = fd_destroy,
        .role          = EVEMON_ROLE_SYSTEM,
        .dependencies  = NULL,
    };

    return p;
}
