/*
 * devices_plugin.c – Devices tab for evemon.
 *
 * Shows device files (/dev/) open by the process with resolved
 * hardware names (GPU model, sound card, input device, etc.).
 * Device name resolution is performed centrally by the broker via
 * label_device(), so the desc field arrives pre-populated.
 *
 * Options:
 *   • Include Descendants – also show device FDs from child processes.
 *   • Merge Duplicates    – group identical device paths with a count.
 *
 * Categories:
 *   GPU / DRI, Sound / Audio, Input, Block Storage,
 *   Terminals & PTYs, Other Devices
 *
 * Build:
 *   gcc -shared -fPIC -o evemon_devices.so devices_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"
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
    GtkWidget      *vbox;         /* top-level container            */
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    GtkWidget      *empty_label;
    GtkWidget      *stack;        /* switches between tree and empty label */
    GtkWidget      *chk_desc;     /* "Include Descendants" checkbox */
    GtkWidget      *chk_dedup;    /* "Merge Duplicates" checkbox    */
    unsigned        collapsed;    /* bitmask: 1 << cat              */
    pid_t           last_pid;
    gboolean        include_desc; /* current toggle state           */
    gboolean        merge_dup;    /* current toggle state           */
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

    /* Top-level vbox: checkboxes + stack */
    ctx->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* Checkbox bar */
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

    /* Tree store + view */
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
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
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

    gtk_box_pack_start(GTK_BOX(ctx->vbox), ctx->stack, TRUE, TRUE, 0);

    gtk_widget_show_all(ctx->vbox);

    return ctx->vbox;
}

/* ── display entry (for filtering + dedup) ──────────────────── */

typedef struct {
    int         fd;
    const char *path;
    const char *desc;
    int         cat;
    size_t      dup_count;   /* 1 = unique, >1 = merged group */
} dev_display_t;

static int dev_path_cmp(const void *a, const void *b)
{
    const dev_display_t *ea = a, *eb = b;
    return strcmp(ea->path, eb->path);
}

static void devices_update(void *opaque, const evemon_proc_data_t *data)
{
    devices_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    /* Read current checkbox state */
    ctx->include_desc = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->chk_desc));
    ctx->merge_dup = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->chk_dedup));

    /* Phase 1: classify device FDs, optionally filter by source_pid */
    size_t total = data->fd_count;
    dev_display_t *raw = g_new0(dev_display_t, total > 0 ? total : 1);
    size_t raw_count = 0;

    for (size_t i = 0; i < total; i++) {
        int cat = classify_device(data->fds[i].path);
        if (cat < 0) continue;  /* not a /dev/ path */

        if (!ctx->include_desc && data->fds[i].source_pid != data->pid)
            continue;

        raw[raw_count].fd        = data->fds[i].fd;
        raw[raw_count].path      = data->fds[i].path;
        raw[raw_count].desc      = data->fds[i].desc;
        raw[raw_count].cat       = cat;
        raw[raw_count].dup_count = 1;
        raw_count++;
    }

    /* Phase 2: optionally merge duplicates */
    dev_display_t *display = raw;
    size_t display_count = raw_count;

    if (ctx->merge_dup && raw_count > 1) {
        qsort(raw, raw_count, sizeof(dev_display_t), dev_path_cmp);

        dev_display_t *merged = g_new0(dev_display_t, raw_count);
        size_t mi = 0;
        size_t i = 0;
        while (i < raw_count) {
            merged[mi] = raw[i];
            merged[mi].dup_count = 1;
            size_t j = i + 1;
            while (j < raw_count &&
                   merged[mi].cat == raw[j].cat &&
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

    /* Count per category */
    size_t cat_count[DEV_CAT_COUNT] = {0};
    for (size_t i = 0; i < display_count; i++)
        cat_count[display[i].cat]++;

    if (display_count == 0) {
        gtk_tree_store_clear(ctx->store);
        gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "empty");
        g_free(display);
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

        for (size_t i = 0; i < display_count; i++) {
            if (display[i].cat != c) continue;

            char *markup;
            if (display[i].dup_count > 1) {
                const char *d = (display[i].desc && display[i].desc[0])
                                ? display[i].desc : display[i].path;
                char *esc = g_markup_escape_text(d, -1);
                markup = g_strdup_printf(
                    "%s  <span foreground=\"#888888\">(%zu duplicates)</span>",
                    esc, display[i].dup_count);
                g_free(esc);
            } else {
                const char *d = (display[i].desc && display[i].desc[0])
                                ? display[i].desc : display[i].path;
                char *esc = g_markup_escape_text(d, -1);
                markup = g_strdup_printf(
                    "<span foreground=\"#6699cc\">%d</span>  %s",
                    display[i].fd, esc);
                g_free(esc);
            }

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
    g_free(display);
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

static evemon_plugin_t devices_plugin;

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    devices_ctx_t *ctx = calloc(1, sizeof(devices_ctx_t));
    if (!ctx) return NULL;

    ctx->include_desc = TRUE;
    ctx->merge_dup    = TRUE;

    devices_plugin = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Devices",
        .id            = "org.evemon.devices",
        .version       = "1.0",
        .data_needs    = evemon_NEED_FDS | evemon_NEED_DESCENDANTS,
        .plugin_ctx    = ctx,
        .create_widget = devices_create_widget,
        .update        = devices_update,
        .clear         = devices_clear,
        .destroy       = devices_destroy,
    };

    return &devices_plugin;
}
