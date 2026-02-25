/*
 * libs_plugin.c – Shared Libraries / DLLs plugin for evemon.
 *
 * Displays loaded shared libraries from /proc/<pid>/maps (r-x segments),
 * categorised into Runtime, System, Application, Wine Built-in,
 * Windows DLLs, and Other.
 *
 * Build:
 *   gcc -shared -fPIC -o evemon_libs.so libs_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>

/* ── UTF-8 helpers ───────────────────────────────────────────── */

/*
 * Sanitise a raw string for GTK/Pango.
 * Returns a heap-allocated valid-UTF-8 string (caller frees with g_free).
 * - Replaces invalid byte sequences with U+FFFD.
 * - Returns "" for NULL input.
 */
static char *utf8_sanitize(const char *raw)
{
    if (!raw || !raw[0]) return g_strdup("");
    if (g_utf8_validate(raw, -1, NULL)) return g_strdup(raw);
    return g_utf8_make_valid(raw, -1);
}

/* ── category definitions ────────────────────────────────────── */

enum {
    LIB_CAT_RUNTIME,
    LIB_CAT_SYSTEM,
    LIB_CAT_APPLICATION,
    LIB_CAT_WINE_BUILTIN,
    LIB_CAT_WINDOWS_DLL,
    LIB_CAT_OTHER,
    LIB_CAT_COUNT
};

static const char *cat_labels[LIB_CAT_COUNT] = {
    [LIB_CAT_RUNTIME]      = "Runtime",
    [LIB_CAT_SYSTEM]       = "System Libraries",
    [LIB_CAT_APPLICATION]  = "Application Libraries",
    [LIB_CAT_WINE_BUILTIN] = "Wine / Proton Built-in",
    [LIB_CAT_WINDOWS_DLL]  = "Windows DLLs",
    [LIB_CAT_OTHER]        = "Other",
};

enum { COL_TEXT, COL_MARKUP, COL_CAT, NUM_COLS };

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    unsigned        collapsed;
    pid_t           last_pid;
} lib_ctx_t;

/* ── classification ──────────────────────────────────────────── */

static int is_runtime_lib(const char *name)
{
    static const char *rt[] = {
        "ld-linux", "ld-musl", "libc.so", "libc-",
        "libm.so", "libm-", "libdl.so", "libdl-",
        "libpthread.so", "libpthread-", "librt.so", "librt-",
        "libresolv.so", "libnss_", "libgcc_s.so", "libstdc++.so",
        NULL
    };
    for (int i = 0; rt[i]; i++)
        if (strncmp(name, rt[i], strlen(rt[i])) == 0) return 1;
    return 0;
}

static int classify_lib(const char *path, const char *name)
{
    if (strstr(path, "x86_64-windows") || strstr(path, "i386-windows") ||
        strstr(path, "i686-windows"))
        return LIB_CAT_WINDOWS_DLL;

    size_t nlen = strlen(name);
    if (nlen >= 4) {
        const char *ext = name + nlen - 4;
        if (strcasecmp(ext, ".dll") == 0 || strcasecmp(ext, ".drv") == 0 ||
            strcasecmp(ext, ".exe") == 0) {
            if (strstr(path, "/wine/") || strstr(path, "/dist/") ||
                strstr(path, "/files/"))
                return LIB_CAT_WINE_BUILTIN;
            return LIB_CAT_WINDOWS_DLL;
        }
    }

    if (strstr(path, "x86_64-unix") || strstr(path, "i386-unix") ||
        strstr(path, "i686-unix"))
        return LIB_CAT_WINE_BUILTIN;

    if (strstr(path, "/wine/") && strstr(name, ".so"))
        return LIB_CAT_WINE_BUILTIN;

    if (is_runtime_lib(name))
        return LIB_CAT_RUNTIME;

    if (strncmp(path, "/usr/lib", 8) == 0 ||
        strncmp(path, "/usr/local/lib", 14) == 0 ||
        strncmp(path, "/lib/", 5) == 0 ||
        strncmp(path, "/lib64/", 7) == 0 ||
        strncmp(path, "/nix/store", 10) == 0 ||
        strstr(path, "steam-runtime") ||
        strstr(path, "SteamLinuxRuntime") ||
        strstr(path, "pressure-vessel"))
        return LIB_CAT_SYSTEM;

    return LIB_CAT_APPLICATION;
}

/* ── markup helpers ───────────────────────────────────────────── */

static void format_lib_size(size_t kb, char *buf, size_t bufsz)
{
    if (kb >= 1048576)
        snprintf(buf, bufsz, "%.1f GiB", (double)kb / 1048576.0);
    else if (kb >= 1024)
        snprintf(buf, bufsz, "%.1f MiB", (double)kb / 1024.0);
    else if (kb > 0)
        snprintf(buf, bufsz, "%zu KiB", kb);
    else
        buf[0] = '\0';
}

static char *lib_to_markup(const evemon_lib_t *lib)
{
    char *safe_name = utf8_sanitize(lib->name);
    char *n_esc = g_markup_escape_text(safe_name, -1);
    g_free(safe_name);

    GString *s = g_string_new(NULL);

    /* Library name — bold */
    g_string_append_printf(s, "<b>%s</b>", n_esc);
    g_free(n_esc);

    /* Version — green tint */
    if (lib->version[0]) {
        char *safe_ver = utf8_sanitize(lib->version);
        char *v_esc = g_markup_escape_text(safe_ver, -1);
        g_free(safe_ver);
        g_string_append_printf(s,
            "  <span foreground=\"#88aa88\">%s</span>", v_esc);
        g_free(v_esc);
    }

    /* Size — blue tint */
    char sz_buf[32];
    format_lib_size(lib->size_kb, sz_buf, sizeof(sz_buf));
    if (sz_buf[0]) {
        char *sz_esc = g_markup_escape_text(sz_buf, -1);
        g_string_append_printf(s,
            "  <span foreground=\"#6699cc\">%s</span>", sz_esc);
        g_free(sz_esc);
    }

    /* Origin — dim grey */
    if (lib->origin[0]) {
        char *safe_origin = utf8_sanitize(lib->origin);
        char *o_esc = g_markup_escape_text(safe_origin, -1);
        g_free(safe_origin);
        g_string_append_printf(s,
            "  <span foreground=\"#888888\">(%s)</span>", o_esc);
        g_free(o_esc);
    }

    return g_string_free(s, FALSE);
}

/* ── signal callbacks ────────────────────────────────────────── */

static void on_row_collapsed(GtkTreeView *v, GtkTreeIter *it,
                             GtkTreePath *p, gpointer data)
{
    (void)v; (void)p;
    lib_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), it, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < LIB_CAT_COUNT) ctx->collapsed |= (1u << cat);
}

static void on_row_expanded(GtkTreeView *v, GtkTreeIter *it,
                            GtkTreePath *p, gpointer data)
{
    (void)v; (void)p;
    lib_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), it, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < LIB_CAT_COUNT) ctx->collapsed &= ~(1u << cat);
}

static gboolean on_query_tooltip(GtkWidget *widget, gint x, gint y,
                                 gboolean kb, GtkTooltip *tip, gpointer data)
{
    (void)data;
    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeModel *model;
    GtkTreePath *path;
    GtkTreeIter iter;

    if (!gtk_tree_view_get_tooltip_context(view, &x, &y, kb,
                                            &model, &path, &iter))
        return FALSE;

    gint cat = -1;
    gchar *text = NULL;
    gtk_tree_model_get(model, &iter, COL_CAT, &cat, COL_TEXT, &text, -1);

    if (cat >= 0 || !text || !text[0]) {
        g_free(text);
        gtk_tree_path_free(path);
        return FALSE;
    }

    gtk_tooltip_set_text(tip, text);
    gtk_tree_view_set_tooltip_row(view, tip, path);
    g_free(text);
    gtk_tree_path_free(path);
    return TRUE;
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *lib_create_widget(void *opaque)
{
    lib_ctx_t *ctx = opaque;

    ctx->store = gtk_tree_store_new(NUM_COLS,
                                    G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    g_object_unref(ctx->store);

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);
    gtk_tree_view_set_enable_search(ctx->view, FALSE);
    gtk_widget_set_has_tooltip(GTK_WIDGET(ctx->view), TRUE);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Lib", cell, "markup", COL_MARKUP, NULL);
    gtk_tree_view_append_column(ctx->view, col);

    g_signal_connect(ctx->view, "row-collapsed",
                     G_CALLBACK(on_row_collapsed), ctx);
    g_signal_connect(ctx->view, "row-expanded",
                     G_CALLBACK(on_row_expanded), ctx);
    g_signal_connect(ctx->view, "query-tooltip",
                     G_CALLBACK(on_query_tooltip), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);
    gtk_widget_show_all(ctx->scroll);

    return ctx->scroll;
}

static void lib_update(void *opaque, const evemon_proc_data_t *data)
{
    lib_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    size_t total = data->lib_count;
    size_t cat_count[LIB_CAT_COUNT] = {0};

    /* Classify each library */
    int *cats = g_new0(int, total > 0 ? total : 1);
    size_t cat_kb[LIB_CAT_COUNT] = {0};
    for (size_t i = 0; i < total; i++) {
        cats[i] = classify_lib(data->libs[i].path, data->libs[i].name);
        cat_count[cats[i]]++;
        cat_kb[cats[i]] += data->libs[i].size_kb;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeIter cat_iters[LIB_CAT_COUNT];
    gboolean cat_exists[LIB_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean v = gtk_tree_model_iter_children(model, &top, NULL);
        while (v) {
            gint cid = -1;
            gtk_tree_model_get(model, &top, COL_CAT, &cid, -1);
            if (cid >= 0 && cid < LIB_CAT_COUNT) {
                cat_iters[cid] = top; cat_exists[cid] = TRUE;
            }
            v = gtk_tree_model_iter_next(model, &top);
        }
    }

    for (int c = 0; c < LIB_CAT_COUNT; c++)
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }

    for (int c = 0; c < LIB_CAT_COUNT; c++) {
        if (cat_count[c] == 0) continue;

        char sz_buf[32];
        format_lib_size(cat_kb[c], sz_buf, sizeof(sz_buf));

        char hdr[128];
        if (sz_buf[0])
            snprintf(hdr, sizeof(hdr), "%s — %zu libraries, %s",
                     cat_labels[c], cat_count[c], sz_buf);
        else
            snprintf(hdr, sizeof(hdr), "%s (%zu)", cat_labels[c], cat_count[c]);
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
        gboolean cv = gtk_tree_model_iter_children(model, &child, &parent);

        for (size_t i = 0; i < total; i++) {
            if (cats[i] != c) continue;

            const evemon_lib_t *lib = &data->libs[i];

            char *markup = lib_to_markup(lib);
            char *safe_path = utf8_sanitize(lib->path);

            if (cv) {
                gtk_tree_store_set(ctx->store, &child,
                                   COL_TEXT, safe_path,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
                cv = gtk_tree_model_iter_next(model, &child);
            } else {
                GtkTreeIter nc;
                gtk_tree_store_append(ctx->store, &nc, &parent);
                gtk_tree_store_set(ctx->store, &nc,
                                   COL_TEXT, safe_path,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
            }
            g_free(safe_path);
            g_free(markup);
        }

        while (cv)
            cv = gtk_tree_store_remove(ctx->store, &child);

        GtkTreePath *cp = gtk_tree_model_get_path(model, &cat_iters[c]);
        if (ctx->collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->view, cp);
        else
            gtk_tree_view_expand_row(ctx->view, cp, FALSE);
        gtk_tree_path_free(cp);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);
    g_free(cats);
}

static void lib_clear(void *opaque)
{
    lib_ctx_t *ctx = opaque;
    gtk_tree_store_clear(ctx->store);
    ctx->last_pid = 0;
}

static void lib_destroy(void *opaque) { free(opaque); }

/* ── descriptor ──────────────────────────────────────────────── */

static evemon_plugin_t lib_plugin;

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    lib_ctx_t *ctx = calloc(1, sizeof(lib_ctx_t));
    if (!ctx) return NULL;

    lib_plugin = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Shared Libraries",
        .id            = "org.evemon.libs",
        .version       = "1.0",
        .data_needs    = evemon_NEED_LIBS,
        .plugin_ctx    = ctx,
        .create_widget = lib_create_widget,
        .update        = lib_update,
        .clear         = lib_clear,
        .destroy       = lib_destroy,
    };

    return &lib_plugin;
}
