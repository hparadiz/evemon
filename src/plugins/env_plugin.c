/*
 * env_plugin.c – Environment Variables plugin for evemon.
 *
 * Displays the environment variables of a process, categorised into
 * PATH, Display, Locale, XDG, Steam, and Other.
 *
 * Build:
 *   gcc -shared -fPIC -o evemon_env.so env_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>

/* ── category definitions ────────────────────────────────────── */

enum {
    ENV_CAT_PATH,
    ENV_CAT_DISPLAY,
    ENV_CAT_LOCALE,
    ENV_CAT_XDG,
    ENV_CAT_STEAM,
    ENV_CAT_OTHER,
    ENV_CAT_COUNT
};

static const char *cat_labels[ENV_CAT_COUNT] = {
    [ENV_CAT_PATH]    = "Paths",
    [ENV_CAT_DISPLAY] = "Display & Session",
    [ENV_CAT_LOCALE]  = "Locale",
    [ENV_CAT_XDG]     = "XDG",
    [ENV_CAT_STEAM]   = "Steam / Proton / Wine",
    [ENV_CAT_OTHER]   = "Other",
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
    unsigned        collapsed;
    pid_t           last_pid;
} env_ctx_t;

/* ── classification ──────────────────────────────────────────── */

static int classify_env(const char *text)
{
    if (!text) return ENV_CAT_OTHER;

    /* Extract key (up to '=') */
    const char *eq = strchr(text, '=');
    size_t klen = eq ? (size_t)(eq - text) : strlen(text);

    /* Path-related */
    if ((klen == 4 && strncmp(text, "PATH", 4) == 0) ||
        strncmp(text, "LD_LIBRARY_PATH", 15) == 0 ||
        strncmp(text, "LD_PRELOAD", 10) == 0 ||
        strncmp(text, "PYTHONPATH", 10) == 0 ||
        strncmp(text, "MANPATH", 7) == 0)
        return ENV_CAT_PATH;

    /* Display/session */
    if (strncmp(text, "DISPLAY=", 8) == 0 ||
        strncmp(text, "WAYLAND_DISPLAY", 15) == 0 ||
        strncmp(text, "DBUS_", 5) == 0 ||
        strncmp(text, "SESSION_", 8) == 0 ||
        strncmp(text, "DESKTOP_", 8) == 0 ||
        strncmp(text, "TERM=", 5) == 0 ||
        strncmp(text, "COLORTERM=", 10) == 0 ||
        strncmp(text, "VTE_", 4) == 0 ||
        strncmp(text, "GTK_", 4) == 0 ||
        strncmp(text, "QT_", 3) == 0 ||
        strncmp(text, "GDK_", 4) == 0 ||
        strncmp(text, "XCURSOR_", 8) == 0)
        return ENV_CAT_DISPLAY;

    /* Locale */
    if (strncmp(text, "LANG=", 5) == 0 ||
        strncmp(text, "LC_", 3) == 0 ||
        strncmp(text, "LANGUAGE=", 9) == 0)
        return ENV_CAT_LOCALE;

    /* XDG */
    if (strncmp(text, "XDG_", 4) == 0)
        return ENV_CAT_XDG;

    /* Steam / Proton / Wine */
    if (strncmp(text, "STEAM_", 6) == 0 ||
        strncmp(text, "SteamApp", 8) == 0 ||
        strncmp(text, "PROTON_", 7) == 0 ||
        strncmp(text, "WINE", 4) == 0 ||
        strncmp(text, "DXVK_", 5) == 0 ||
        strncmp(text, "VKD3D_", 6) == 0 ||
        strncmp(text, "MANGOHUD", 8) == 0 ||
        strncmp(text, "PRESSURE_VESSEL", 15) == 0 ||
        strncmp(text, "WINEPREFIX", 10) == 0)
        return ENV_CAT_STEAM;

    return ENV_CAT_OTHER;
}

/* ── markup ──────────────────────────────────────────────────── */

static char *env_to_markup(const char *text)
{
    const char *eq = strchr(text, '=');
    if (!eq) {
        return g_markup_escape_text(text, -1);
    }

    char *key_esc = g_markup_escape_text(text, (gssize)(eq - text));
    char *val_esc = g_markup_escape_text(eq + 1, -1);

    /* Truncate very long values for display (M1: UTF-8 safe) */
    char *markup;
    size_t val_len = g_utf8_strlen(val_esc, -1);
    if (val_len > 120) {
        gchar *trunc = g_utf8_substring(val_esc, 0, 120);
        markup = g_strdup_printf(
            "<b>%s</b>=<span foreground=\"#88aa88\">%s…</span>",
            key_esc, trunc);
        g_free(trunc);
    } else {
        markup = g_strdup_printf(
            "<b>%s</b>=<span foreground=\"#88aa88\">%s</span>",
            key_esc, val_esc);
    }

    g_free(key_esc);
    g_free(val_esc);
    return markup;
}

/* ── signal callbacks ────────────────────────────────────────── */

static void on_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                             GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    env_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < ENV_CAT_COUNT)
        ctx->collapsed |= (1u << cat);
}

static void on_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                            GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    env_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < ENV_CAT_COUNT)
        ctx->collapsed &= ~(1u << cat);
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *env_create_widget(void *opaque)
{
    env_ctx_t *ctx = opaque;

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
        "Env", cell, "markup", COL_MARKUP, NULL);
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

static void env_update(void *opaque, const evemon_proc_data_t *data)
{
    env_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    /* Categorise all env vars */
    size_t cat_count[ENV_CAT_COUNT] = {0};

    typedef struct { const char *text; int cat; } ent_t;
    size_t total = data->env_count;
    ent_t *ents = g_new0(ent_t, total > 0 ? total : 1);

    for (size_t i = 0; i < total; i++) {
        ents[i].text = data->envs[i].text;
        ents[i].cat  = classify_env(data->envs[i].text);
        cat_count[ents[i].cat]++;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeIter cat_iters[ENV_CAT_COUNT];
    gboolean    cat_exists[ENV_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cid = -1;
            gtk_tree_model_get(model, &top, COL_CAT, &cid, -1);
            if (cid >= 0 && cid < ENV_CAT_COUNT) {
                cat_iters[cid] = top;
                cat_exists[cid] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    for (int c = 0; c < ENV_CAT_COUNT; c++) {
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    for (int c = 0; c < ENV_CAT_COUNT; c++) {
        if (cat_count[c] == 0) continue;

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s (%zu)", cat_labels[c], cat_count[c]);
        char *hdr_esc = g_markup_escape_text(hdr, -1);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            gtk_tree_store_append(ctx->store, &parent, NULL);
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr, COL_MARKUP, hdr_esc,
                               COL_CAT, (gint)c, -1);
            cat_exists[c] = TRUE;
            cat_iters[c] = parent;
        } else {
            parent = cat_iters[c];
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr, COL_MARKUP, hdr_esc, -1);
        }
        g_free(hdr_esc);

        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);

        for (size_t i = 0; i < total; i++) {
            if (ents[i].cat != c) continue;

            char *markup = env_to_markup(ents[i].text);

            if (child_valid) {
                gtk_tree_store_set(ctx->store, &child,
                                   COL_TEXT, ents[i].text,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1, -1);
                child_valid = gtk_tree_model_iter_next(model, &child);
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
    g_free(ents);
}

static void env_clear(void *opaque)
{
    env_ctx_t *ctx = opaque;
    gtk_tree_store_clear(ctx->store);
    ctx->last_pid = 0;
}

static void env_destroy(void *opaque)
{
    free(opaque);
}

/* ── plugin descriptor ───────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    env_ctx_t *ctx = calloc(1, sizeof(env_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Environment Variables",
        .id            = "org.evemon.env",
        .version       = "1.0",
        .data_needs    = evemon_NEED_ENV,
        .plugin_ctx    = ctx,
        .create_widget = env_create_widget,
        .update        = env_update,
        .clear         = env_clear,
        .destroy       = env_destroy,
    };

    return p;
}
