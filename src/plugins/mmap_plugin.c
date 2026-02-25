/*
 * mmap_plugin.c – Memory Maps plugin for evemon.
 *
 * Displays /proc/<pid>/maps regions categorised into Code, Data,
 * Read-Only, Heap, Stack, vDSO, Anonymous, and Other.
 *
 * Build:
 *   gcc -shared -fPIC -o evemon_mmap.so mmap_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>

/* ── category definitions ────────────────────────────────────── */

enum {
    MMAP_CAT_CODE,
    MMAP_CAT_DATA,
    MMAP_CAT_RODATA,
    MMAP_CAT_HEAP,
    MMAP_CAT_STACK,
    MMAP_CAT_VDSO,
    MMAP_CAT_ANON,
    MMAP_CAT_OTHER,
    MMAP_CAT_COUNT
};

static const char *cat_labels[MMAP_CAT_COUNT] = {
    [MMAP_CAT_CODE]   = "Code (r-x)",
    [MMAP_CAT_DATA]   = "Data (rw-)",
    [MMAP_CAT_RODATA] = "Read-Only (r--)",
    [MMAP_CAT_HEAP]   = "Heap",
    [MMAP_CAT_STACK]  = "Stack",
    [MMAP_CAT_VDSO]   = "vDSO / vvar",
    [MMAP_CAT_ANON]   = "Anonymous",
    [MMAP_CAT_OTHER]  = "Other",
};

enum { COL_TEXT, COL_MARKUP, COL_CAT, NUM_COLS };

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    unsigned        collapsed;
    pid_t           last_pid;
} mmap_ctx_t;

/* ── classification ──────────────────────────────────────────── */

static int classify_mmap(const char *line)
{
    if (!line) return MMAP_CAT_OTHER;

    /* Parse permissions: "addr_start-addr_end perms ..." */
    const char *p = line;
    while (*p && *p != ' ') p++;   /* skip address range */
    while (*p == ' ') p++;          /* skip spaces */

    char perms[5] = "----";
    if (p[0] && p[1] && p[2] && p[3]) {
        memcpy(perms, p, 4);
    }

    /* Find pathname at end of line */
    const char *pathname = NULL;
    {
        const char *pp = line;
        int fields = 0;
        while (fields < 5 && *pp) {
            while (*pp && *pp != ' ' && *pp != '\t') pp++;
            while (*pp == ' ' || *pp == '\t') pp++;
            fields++;
        }
        if (*pp)
            pathname = pp;
    }

    if (pathname) {
        if (strcmp(pathname, "[heap]") == 0)            return MMAP_CAT_HEAP;
        if (strncmp(pathname, "[stack", 6) == 0)       return MMAP_CAT_STACK;
        if (strcmp(pathname, "[vdso]") == 0 ||
            strcmp(pathname, "[vvar]") == 0 ||
            strcmp(pathname, "[vsyscall]") == 0)        return MMAP_CAT_VDSO;
    }

    /* r-xp = code */
    if (perms[0] == 'r' && perms[2] == 'x')
        return MMAP_CAT_CODE;

    /* rw-p with pathname = data; rw-p without = anon */
    if (perms[0] == 'r' && perms[1] == 'w') {
        if (pathname && pathname[0] != '\0' && pathname[0] != '[')
            return MMAP_CAT_DATA;
        return MMAP_CAT_ANON;
    }

    /* r--p with pathname = read-only data */
    if (perms[0] == 'r' && perms[1] == '-' && perms[2] == '-') {
        if (pathname && pathname[0] != '\0' && pathname[0] != '[')
            return MMAP_CAT_RODATA;
        return MMAP_CAT_OTHER;
    }

    return MMAP_CAT_OTHER;
}

/* ── size formatting ─────────────────────────────────────────── */

static void format_size(size_t kb, char *buf, size_t bufsz)
{
    if (kb >= 1048576)
        snprintf(buf, bufsz, "%.1f GiB", (double)kb / 1048576.0);
    else if (kb >= 1024)
        snprintf(buf, bufsz, "%.1f MiB", (double)kb / 1024.0);
    else
        snprintf(buf, bufsz, "%zu KiB", kb);
}

/* ── markup ──────────────────────────────────────────────────── */

/*
 * Render a /proc/<pid>/maps line with highlighted fields:
 *   addr range in grey, perms in bold, size in blue, pathname plain.
 *
 * Raw format: "addr_start-addr_end perms offset dev inode  pathname"
 * Display:    "<grey>addr-addr</grey>  <b>perms</b>  <blue>size</blue>  pathname"
 */
static char *mmap_to_markup(const char *line, size_t size_kb)
{
    char sz[32];
    format_size(size_kb, sz, sizeof(sz));

    /* Parse: addr_start-addr_end perms offset dev inode [pathname] */
    unsigned long addr_start = 0, addr_end = 0;
    char perms[8] = "----";
    unsigned long offset = 0;
    char dev[16] = "";
    unsigned long inode = 0;

    int n = sscanf(line, "%lx-%lx %4s %lx %15s %lu",
                   &addr_start, &addr_end, perms, &offset, dev, &inode);
    if (n < 5) {
        /* Can't parse — fall back to raw display */
        char *esc = g_markup_escape_text(line, -1);
        char *markup = g_strdup_printf(
            "%s  <span foreground=\"#6699cc\">%s</span>", esc, sz);
        g_free(esc);
        return markup;
    }

    /* Extract pathname: skip past the 5th field (inode) */
    const char *pathname = "";
    {
        const char *p = line;
        int fields = 0;
        while (fields < 5 && *p) {
            while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' ' || *p == '\t') p++;
            fields++;
        }
        if (*p)
            pathname = p;
    }

    char addr_buf[48];
    snprintf(addr_buf, sizeof(addr_buf), "%lx-%lx", addr_start, addr_end);

    char *addr_esc = g_markup_escape_text(addr_buf, -1);
    char *perms_esc = g_markup_escape_text(perms, -1);
    char *path_esc = g_markup_escape_text(pathname, -1);

    char *markup;
    if (pathname[0])
        markup = g_strdup_printf(
            "<span foreground=\"#888888\">%s</span>  "
            "<b>%s</b>  "
            "<span foreground=\"#6699cc\">%s</span>  "
            "%s",
            addr_esc, perms_esc, sz, path_esc);
    else
        markup = g_strdup_printf(
            "<span foreground=\"#888888\">%s</span>  "
            "<b>%s</b>  "
            "<span foreground=\"#6699cc\">%s</span>",
            addr_esc, perms_esc, sz);

    g_free(addr_esc);
    g_free(perms_esc);
    g_free(path_esc);
    return markup;
}

/* ── signal callbacks ────────────────────────────────────────── */

static void on_row_collapsed(GtkTreeView *v, GtkTreeIter *it,
                             GtkTreePath *p, gpointer data)
{
    (void)v; (void)p;
    mmap_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), it, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < MMAP_CAT_COUNT) ctx->collapsed |= (1u << cat);
}

static void on_row_expanded(GtkTreeView *v, GtkTreeIter *it,
                            GtkTreePath *p, gpointer data)
{
    (void)v; (void)p;
    mmap_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), it, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < MMAP_CAT_COUNT) ctx->collapsed &= ~(1u << cat);
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *mmap_create_widget(void *opaque)
{
    mmap_ctx_t *ctx = opaque;

    ctx->store = gtk_tree_store_new(NUM_COLS,
                                    G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    g_object_unref(ctx->store);

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);
    gtk_tree_view_set_enable_search(ctx->view, FALSE);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Map", cell, "markup", COL_MARKUP, NULL);
    gtk_tree_view_append_column(ctx->view, col);

    g_signal_connect(ctx->view, "row-collapsed",
                     G_CALLBACK(on_row_collapsed), ctx);
    g_signal_connect(ctx->view, "row-expanded",
                     G_CALLBACK(on_row_expanded), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);
    gtk_widget_show_all(ctx->scroll);

    return ctx->scroll;
}

static void mmap_update(void *opaque, const evemon_proc_data_t *data)
{
    mmap_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    size_t total = data->mmap_count;
    size_t cat_count[MMAP_CAT_COUNT] = {0};
    size_t cat_total_kb[MMAP_CAT_COUNT] = {0};

    typedef struct { const char *text; size_t size_kb; int cat; } ent_t;
    ent_t *ents = g_new0(ent_t, total > 0 ? total : 1);

    for (size_t i = 0; i < total; i++) {
        ents[i].text    = data->mmaps[i].text;
        ents[i].size_kb = data->mmaps[i].size_kb;
        ents[i].cat     = classify_mmap(data->mmaps[i].text);
        cat_count[ents[i].cat]++;
        cat_total_kb[ents[i].cat] += ents[i].size_kb;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeIter cat_iters[MMAP_CAT_COUNT];
    gboolean cat_exists[MMAP_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean v = gtk_tree_model_iter_children(model, &top, NULL);
        while (v) {
            gint cid = -1;
            gtk_tree_model_get(model, &top, COL_CAT, &cid, -1);
            if (cid >= 0 && cid < MMAP_CAT_COUNT) {
                cat_iters[cid] = top; cat_exists[cid] = TRUE;
            }
            v = gtk_tree_model_iter_next(model, &top);
        }
    }

    for (int c = 0; c < MMAP_CAT_COUNT; c++)
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }

    for (int c = 0; c < MMAP_CAT_COUNT; c++) {
        if (cat_count[c] == 0) continue;

        char sz[32]; format_size(cat_total_kb[c], sz, sizeof(sz));
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s — %zu regions, %s",
                 cat_labels[c], cat_count[c], sz);
        char *hdr_esc = g_markup_escape_text(hdr, -1);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            gtk_tree_store_append(ctx->store, &parent, NULL);
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr, COL_MARKUP, hdr_esc,
                               COL_CAT, (gint)c, -1);
            cat_exists[c] = TRUE; cat_iters[c] = parent;
        } else {
            parent = cat_iters[c];
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr, COL_MARKUP, hdr_esc, -1);
        }
        g_free(hdr_esc);

        GtkTreeIter child;
        gboolean child_v = gtk_tree_model_iter_children(model, &child, &parent);

        for (size_t i = 0; i < total; i++) {
            if (ents[i].cat != c) continue;
            char *markup = mmap_to_markup(ents[i].text, ents[i].size_kb);

            if (child_v) {
                gtk_tree_store_set(ctx->store, &child,
                                   COL_TEXT, ents[i].text,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
                child_v = gtk_tree_model_iter_next(model, &child);
            } else {
                GtkTreeIter nc;
                gtk_tree_store_append(ctx->store, &nc, &parent);
                gtk_tree_store_set(ctx->store, &nc,
                                   COL_TEXT, ents[i].text,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
            }
            g_free(markup);
        }

        while (child_v)
            child_v = gtk_tree_store_remove(ctx->store, &child);

        GtkTreePath *cp = gtk_tree_model_get_path(model, &cat_iters[c]);
        if (ctx->collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->view, cp);
        else
            gtk_tree_view_expand_row(ctx->view, cp, FALSE);
        gtk_tree_path_free(cp);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);
    g_free(ents);
}

static void mmap_clear(void *opaque)
{
    mmap_ctx_t *ctx = opaque;
    gtk_tree_store_clear(ctx->store);
    ctx->last_pid = 0;
}

static void mmap_destroy(void *opaque) { free(opaque); }

/* ── descriptor ──────────────────────────────────────────────── */

static evemon_plugin_t mmap_plugin;

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    mmap_ctx_t *ctx = calloc(1, sizeof(mmap_ctx_t));
    if (!ctx) return NULL;

    mmap_plugin = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Memory Maps",
        .id            = "org.evemon.mmap",
        .version       = "1.0",
        .data_needs    = evemon_NEED_MMAP,
        .plugin_ctx    = ctx,
        .create_widget = mmap_create_widget,
        .update        = mmap_update,
        .clear         = mmap_clear,
        .destroy       = mmap_destroy,
    };

    return &mmap_plugin;
}
