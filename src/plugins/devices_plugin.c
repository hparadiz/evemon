/*
 * devices_plugin.c – Devices tab for allmon.
 *
 * Shows device files (/dev/) open by the process with resolved
 * hardware names (GPU model, sound card, input device, etc.).
 * Device name resolution is performed centrally by the broker via
 * label_device(), so the desc field arrives pre-populated.
 *
 * Categories:
 *   GPU / DRI, Sound / Audio, Input, Block Storage,
 *   Terminals & PTYs, Other Devices
 *
 * Build:
 *   gcc -shared -fPIC -o allmon_devices.so devices_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../allmon_plugin.h"
#include <string.h>
#include <stdio.h>

/* ── category definitions ────────────────────────────────────── */

enum {
    DEV_CAT_GPU,
    DEV_CAT_SOUND,
    DEV_CAT_INPUT,
    DEV_CAT_VIDEO,
    DEV_CAT_BLOCK,
    DEV_CAT_TERMINAL,
    DEV_CAT_OTHER,
    DEV_CAT_COUNT
};

static const char *cat_labels[DEV_CAT_COUNT] = {
    [DEV_CAT_GPU]      = "GPU / DRI",
    [DEV_CAT_SOUND]    = "Sound / Audio",
    [DEV_CAT_INPUT]    = "Input Devices",
    [DEV_CAT_VIDEO]    = "Video / Camera",
    [DEV_CAT_BLOCK]    = "Block Storage",
    [DEV_CAT_TERMINAL] = "Terminals & PTYs",
    [DEV_CAT_OTHER]    = "Other Devices",
};

static const char *cat_icons[DEV_CAT_COUNT] = {
    [DEV_CAT_GPU]      = "🎮",
    [DEV_CAT_SOUND]    = "🔊",
    [DEV_CAT_INPUT]    = "⌨",
    [DEV_CAT_VIDEO]    = "📷",
    [DEV_CAT_BLOCK]    = "💾",
    [DEV_CAT_TERMINAL] = "🖥",
    [DEV_CAT_OTHER]    = "⚙",
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
    GtkWidget      *empty_label;
    GtkWidget      *stack;        /* switches between tree and empty label */
    unsigned        collapsed;    /* bitmask: 1 << cat */
    pid_t           last_pid;
} devices_ctx_t;

/* ── classification ──────────────────────────────────────────── */

static int classify_device(const char *path)
{
    if (!path || strncmp(path, "/dev/", 5) != 0) return -1;

    const char *after = path + 5;

    /* GPU / DRI */
    if (strncmp(after, "dri/", 4) == 0)    return DEV_CAT_GPU;
    if (strncmp(after, "nvidia", 6) == 0)  return DEV_CAT_GPU;

    /* Sound */
    if (strncmp(after, "snd/", 4) == 0)    return DEV_CAT_SOUND;

    /* Input */
    if (strncmp(after, "input/", 6) == 0)  return DEV_CAT_INPUT;

    /* Video / Camera */
    if (strncmp(after, "video", 5) == 0)   return DEV_CAT_VIDEO;

    /* Block storage */
    if (strncmp(after, "sd", 2) == 0 ||
        strncmp(after, "nvme", 4) == 0 ||
        strncmp(after, "vd", 2) == 0 ||
        strncmp(after, "xvd", 3) == 0 ||
        strncmp(after, "dm-", 3) == 0 ||
        strncmp(after, "mapper/", 7) == 0 ||
        strncmp(after, "loop", 4) == 0)
        return DEV_CAT_BLOCK;

    /* Terminals & PTYs */
    if (strncmp(after, "pts/", 4) == 0 ||
        strncmp(after, "tty", 3) == 0 ||
        strcmp(after, "ptmx") == 0 ||
        strcmp(after, "console") == 0)
        return DEV_CAT_TERMINAL;

    return DEV_CAT_OTHER;
}

/* ── signal callbacks ────────────────────────────────────────── */

static void on_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                             GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    devices_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < DEV_CAT_COUNT)
        ctx->collapsed |= (1u << cat);
}

static void on_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                            GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    devices_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < DEV_CAT_COUNT)
        ctx->collapsed &= ~(1u << cat);
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *devices_create_widget(void *opaque)
{
    devices_ctx_t *ctx = opaque;

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
        "Device", cell, "markup", COL_MARKUP, NULL);
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

    ctx->empty_label = gtk_label_new("No device files open");
    gtk_widget_set_halign(ctx->empty_label, GTK_ALIGN_START);
    gtk_widget_set_valign(ctx->empty_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(ctx->empty_label, 8);
    gtk_widget_set_margin_top(ctx->empty_label, 8);

    ctx->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(ctx->stack), ctx->scroll, "tree");
    gtk_stack_add_named(GTK_STACK(ctx->stack), ctx->empty_label, "empty");
    gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "empty");

    gtk_widget_show_all(ctx->stack);

    return ctx->stack;
}

static void devices_update(void *opaque, const allmon_proc_data_t *data)
{
    devices_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    /* Bucket device FDs by category */
    typedef struct {
        int         fd;
        const char *path;
        const char *desc;
        int         cat;
    } dev_ent_t;

    size_t total = data->fd_count;
    size_t dev_total = 0;
    size_t cat_count[DEV_CAT_COUNT] = {0};

    dev_ent_t *ents = g_new0(dev_ent_t, total > 0 ? total : 1);

    for (size_t i = 0; i < total; i++) {
        int cat = classify_device(data->fds[i].path);
        if (cat < 0) continue;  /* not a /dev/ path */

        ents[dev_total].fd   = data->fds[i].fd;
        ents[dev_total].path = data->fds[i].path;
        ents[dev_total].desc = data->fds[i].desc;
        ents[dev_total].cat  = cat;
        cat_count[cat]++;
        dev_total++;
    }

    if (dev_total == 0) {
        gtk_tree_store_clear(ctx->store);
        gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "empty");
        g_free(ents);
        return;
    }

    gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "tree");

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    /* Save scroll position */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    /* Index existing category rows */
    GtkTreeIter cat_iters[DEV_CAT_COUNT];
    gboolean    cat_exists[DEV_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(model, &top, COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < DEV_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    /* Remove empty categories */
    for (int c = 0; c < DEV_CAT_COUNT; c++) {
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
    for (int c = 0; c < DEV_CAT_COUNT; c++) {
        if (cat_count[c] == 0) continue;

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s  %s (%zu)",
                 cat_icons[c], cat_labels[c], cat_count[c]);
        char *hdr_esc = g_markup_escape_text(hdr, -1);
        char *hdr_markup = g_strdup_printf("<b>%s</b>", hdr_esc);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            gtk_tree_store_append(ctx->store, &parent, NULL);
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr,
                               COL_MARKUP, hdr_markup,
                               COL_CAT, (gint)c, -1);
            cat_exists[c] = TRUE;
            cat_iters[c] = parent;
        } else {
            parent = cat_iters[c];
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr,
                               COL_MARKUP, hdr_markup, -1);
        }
        g_free(hdr_esc);
        g_free(hdr_markup);

        /* Update children in place */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);

        for (size_t i = 0; i < dev_total; i++) {
            if (ents[i].cat != c) continue;

            /* Build a rich markup line:
             *   fd N  /dev/path  (resolved description) */
            const char *display = (ents[i].desc && ents[i].desc[0])
                                  ? ents[i].desc : ents[i].path;
            char *esc = g_markup_escape_text(display, -1);
            char *markup = g_strdup_printf(
                "<span foreground=\"#6699cc\">%d</span>  %s",
                ents[i].fd, esc);
            g_free(esc);

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

static void devices_clear(void *opaque)
{
    devices_ctx_t *ctx = opaque;
    gtk_tree_store_clear(ctx->store);
    gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "empty");
    ctx->last_pid = 0;
}

static void devices_destroy(void *opaque)
{
    free(opaque);
}

/* ── plugin descriptor ───────────────────────────────────────── */

static allmon_plugin_t devices_plugin;

__attribute__((visibility("default")))
allmon_plugin_t *allmon_plugin_init(void)
{
    devices_ctx_t *ctx = calloc(1, sizeof(devices_ctx_t));
    if (!ctx) return NULL;

    devices_plugin = (allmon_plugin_t){
        .abi_version   = ALLMON_PLUGIN_ABI_VERSION,
        .name          = "Devices",
        .id            = "org.allmon.devices",
        .version       = "1.0",
        .data_needs    = ALLMON_NEED_FDS,
        .plugin_ctx    = ctx,
        .create_widget = devices_create_widget,
        .update        = devices_update,
        .clear         = devices_clear,
        .destroy       = devices_destroy,
    };

    return &devices_plugin;
}
