/*
 * fd_plugin.c – File Descriptors plugin for allmon.
 *
 * Displays the file descriptors open by a process, categorised into
 * Files, Devices, Net Sockets, Unix Sockets, Pipes, Events, Other.
 *
 * Build:
 *   gcc -shared -fPIC -o allmon_fd.so fd_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../allmon_plugin.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>

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
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    unsigned        collapsed;    /* bitmask: 1 << cat */
    pid_t           last_pid;
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

static char *fd_to_markup(int fd_num, const char *path)
{
    char *esc = g_markup_escape_text(path, -1);
    char *markup = g_strdup_printf(
        "<span foreground=\"#6699cc\">%d</span>  %s", fd_num, esc);
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

    ctx->store = gtk_tree_store_new(NUM_COLS,
                                    G_TYPE_STRING,   /* text   */
                                    G_TYPE_STRING,   /* markup */
                                    G_TYPE_INT);     /* cat    */

    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    g_object_unref(ctx->store);

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);
    gtk_tree_view_set_enable_search(ctx->view, FALSE);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
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
    gtk_widget_show_all(ctx->scroll);

    return ctx->scroll;
}

static void fd_update(void *opaque, const allmon_proc_data_t *data)
{
    fd_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    /* Bucket fds by category */
    size_t cat_count[FD_CAT_COUNT] = {0};
    typedef struct { int fd; const char *path; int cat; } fd_ent_t;

    size_t total = data->fd_count;
    fd_ent_t *ents = g_new0(fd_ent_t, total > 0 ? total : 1);

    for (size_t i = 0; i < total; i++) {
        ents[i].fd   = data->fds[i].fd;
        ents[i].path = data->fds[i].path;
        ents[i].cat  = classify_fd(data->fds[i].path);
        cat_count[ents[i].cat]++;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    /* Save scroll */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    /* Index existing category rows */
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

    /* Remove empty categories */
    for (int c = 0; c < FD_CAT_COUNT; c++) {
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
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

        /* Update children in place */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);
        size_t bi = 0;

        for (size_t i = 0; i < total; i++) {
            if (ents[i].cat != c) continue;

            char *markup = fd_to_markup(ents[i].fd, ents[i].path);

            if (child_valid) {
                gtk_tree_store_set(ctx->store, &child,
                                   COL_TEXT, ents[i].path,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
                child_valid = gtk_tree_model_iter_next(model, &child);
            } else {
                GtkTreeIter new_child;
                gtk_tree_store_append(ctx->store, &new_child, &parent);
                gtk_tree_store_set(ctx->store, &new_child,
                                   COL_TEXT, ents[i].path,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
            }
            g_free(markup);
            bi++;
        }

        /* Remove excess children */
        while (child_valid)
            child_valid = gtk_tree_store_remove(ctx->store, &child);

        /* Restore collapse state */
        GtkTreePath *cat_path = gtk_tree_model_get_path(model, &cat_iters[c]);
        if (ctx->collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);
    g_free(ents);
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

static allmon_plugin_t fd_plugin;

__attribute__((visibility("default")))
allmon_plugin_t *allmon_plugin_init(void)
{
    fd_ctx_t *ctx = calloc(1, sizeof(fd_ctx_t));
    if (!ctx) return NULL;

    fd_plugin = (allmon_plugin_t){
        .abi_version   = ALLMON_PLUGIN_ABI_VERSION,
        .name          = "File Descriptors",
        .id            = "org.allmon.fd",
        .version       = "1.0",
        .data_needs    = ALLMON_NEED_FDS | ALLMON_NEED_DESCENDANTS,
        .plugin_ctx    = ctx,
        .create_widget = fd_create_widget,
        .update        = fd_update,
        .clear         = fd_clear,
        .destroy       = fd_destroy,
    };

    return &fd_plugin;
}
