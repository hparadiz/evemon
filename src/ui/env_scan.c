/*
 * env_scan.c – on-demand /proc/<pid>/environ reading and display.
 *
 * Reads the NUL-delimited environment block for the selected process,
 * classifies variables into logical categories, and populates a
 * GtkTreeStore in the sidebar.  The read runs in a GTask worker
 * thread so it never blocks the UI, matching the fd_scan pattern.
 */

#include "ui_internal.h"

#include <unistd.h>

/* ── category labels ─────────────────────────────────────────── */

const char *env_cat_label[ENV_CAT_COUNT] = {
    [ENV_CAT_PATH]    = "Paths",
    [ENV_CAT_DISPLAY] = "Display / Session",
    [ENV_CAT_LOCALE]  = "Locale",
    [ENV_CAT_XDG]     = "XDG",
    [ENV_CAT_STEAM]   = "Steam / Proton",
    [ENV_CAT_OTHER]   = "Other",
};

/* ── classification ──────────────────────────────────────────── */

/* Well-known path-related variable names. */
static const char *path_keys[] = {
    "PATH", "LD_LIBRARY_PATH", "LD_PRELOAD", "MANPATH",
    "PYTHONPATH", "PERL5LIB", "GEM_PATH", "GOPATH",
    "CARGO_HOME", "RUSTUP_HOME", "NODE_PATH", "CLASSPATH",
    "PKG_CONFIG_PATH", "CMAKE_PREFIX_PATH", "ACLOCAL_PATH",
    NULL
};

/* Display / session variable prefixes and exact names. */
static const char *display_keys[] = {
    "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS",
    "SESSION_MANAGER", "DESKTOP_SESSION", "GDMSESSION",
    "XAUTHORITY", "XDG_SESSION_TYPE", "XDG_SESSION_CLASS",
    "XDG_CURRENT_DESKTOP", "QT_QPA_PLATFORM",
    "GDK_BACKEND", "SDL_VIDEODRIVER", "PULSE_SERVER",
    "PIPEWIRE_REMOTE", "SSH_AUTH_SOCK", "SSH_AGENT_PID",
    "GPG_AGENT_INFO", "TERM", "COLORTERM", "TERM_PROGRAM",
    NULL
};

static int key_in_list(const char *key, size_t klen, const char **list)
{
    for (const char **p = list; *p; p++) {
        size_t plen = strlen(*p);
        if (klen == plen && memcmp(key, *p, klen) == 0)
            return 1;
    }
    return 0;
}

static env_category_t classify_env(const char *key, size_t klen)
{
    /* Steam / Proton / Wine */
    if ((klen >= 6 && memcmp(key, "STEAM_", 6) == 0) ||
        (klen >= 7 && memcmp(key, "PROTON_", 7) == 0) ||
        (klen >= 4 && memcmp(key, "WINE", 4) == 0) ||
        (klen >= 8 && memcmp(key, "SteamApp", 8) == 0) ||
        (klen >= 10 && memcmp(key, "DXVK_", 5) == 0) ||
        (klen >= 4  && memcmp(key, "VKD3D", 5) == 0) ||
        (klen == 16 && memcmp(key, "STEAM_COMPAT_APP", 16) == 0) ||
        (klen >= 15 && memcmp(key, "PRESSURE_VESSEL", 15) == 0))
        return ENV_CAT_STEAM;

    /* XDG (but not the display/session ones handled below) */
    if (klen >= 4 && memcmp(key, "XDG_", 4) == 0) {
        /* Some XDG_ vars are really session vars */
        if (key_in_list(key, klen, display_keys))
            return ENV_CAT_DISPLAY;
        return ENV_CAT_XDG;
    }

    /* Locale */
    if ((klen == 4 && memcmp(key, "LANG", 4) == 0) ||
        (klen >= 3 && memcmp(key, "LC_", 3) == 0) ||
        (klen == 8 && memcmp(key, "LANGUAGE", 8) == 0))
        return ENV_CAT_LOCALE;

    /* Paths */
    if (key_in_list(key, klen, path_keys))
        return ENV_CAT_PATH;

    /* Display / session */
    if (key_in_list(key, klen, display_keys))
        return ENV_CAT_DISPLAY;

    return ENV_CAT_OTHER;
}

/* ── environ entry ───────────────────────────────────────────── */

typedef struct {
    char *text;              /* "KEY=value" (heap) */
    env_category_t cat;
} env_entry_t;

typedef struct {
    env_entry_t *entries;
    size_t       count;
    size_t       capacity;
} env_list_t;

static void env_list_init(env_list_t *l)
{
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void env_list_free(env_list_t *l)
{
    for (size_t i = 0; i < l->count; i++)
        free(l->entries[i].text);
    free(l->entries);
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void env_list_push(env_list_t *l, const char *text, env_category_t cat)
{
    if (l->count >= l->capacity) {
        size_t newcap = l->capacity ? l->capacity * 2 : 128;
        env_entry_t *tmp = realloc(l->entries, newcap * sizeof(env_entry_t));
        if (!tmp) return;
        l->entries  = tmp;
        l->capacity = newcap;
    }
    l->entries[l->count].text = strdup(text);
    l->entries[l->count].cat  = cat;
    l->count++;
}

/* Sort by category first, then alphabetically within each category. */
static int env_entry_cmp(const void *a, const void *b)
{
    const env_entry_t *ea = a;
    const env_entry_t *eb = b;
    if (ea->cat != eb->cat)
        return (int)ea->cat - (int)eb->cat;
    return strcmp(ea->text, eb->text);
}

/* ── /proc/<pid>/environ reader ──────────────────────────────── */

/*
 * Read /proc/<pid>/environ into a categorised list.
 * The file is NUL-delimited: KEY1=val1\0KEY2=val2\0...
 * We read in chunks to handle arbitrarily large environments
 * without a fixed upper bound.
 */
static void read_environ(pid_t pid, env_list_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);

    FILE *f = fopen(path, "r");
    if (!f) return;

    /*
     * Read the entire file into a dynamic buffer.  We can't use
     * fgets because the "lines" are NUL-delimited, not newline.
     */
    size_t bufsz = 8192, used = 0;
    char *buf = malloc(bufsz);
    if (!buf) { fclose(f); return; }

    for (;;) {
        size_t n = fread(buf + used, 1, bufsz - used, f);
        used += n;
        if (n == 0) break;
        if (used >= bufsz) {
            bufsz *= 2;
            /* Cap at 4 MiB to avoid runaway reads */
            if (bufsz > 4 * 1024 * 1024) break;
            char *tmp = realloc(buf, bufsz);
            if (!tmp) break;
            buf = tmp;
        }
    }
    fclose(f);

    /* Walk NUL-delimited entries */
    const char *p = buf;
    const char *end = buf + used;
    while (p < end) {
        size_t len = strnlen(p, (size_t)(end - p));
        if (len == 0) { p++; continue; }

        /* Find the '=' separator to extract the key */
        const char *eq = memchr(p, '=', len);
        size_t klen = eq ? (size_t)(eq - p) : len;

        env_category_t cat = classify_env(p, klen);
        env_list_push(out, p, cat);

        p += len + 1;
    }

    free(buf);
}

/* ── Pango markup for env entries ────────────────────────────── */

/*
 * Render "KEY=value" with the key in bold.
 * The value is escaped for Pango markup safety.
 */
static char *env_to_markup(const char *text)
{
    const char *eq = strchr(text, '=');
    if (!eq) {
        char *escaped = g_markup_escape_text(text, -1);
        return escaped;
    }

    char *key_escaped = g_markup_escape_text(text, (gssize)(eq - text));
    char *val_escaped = g_markup_escape_text(eq + 1, -1);

    char *markup = g_strdup_printf("<b>%s</b>=%s", key_escaped, val_escaped);

    g_free(key_escaped);
    g_free(val_escaped);
    return markup;
}

/* ── async GTask ─────────────────────────────────────────────── */

typedef struct {
    pid_t        pid;
    guint        generation;
    env_list_t   buckets[ENV_CAT_COUNT];
    ui_ctx_t    *ctx;
} env_scan_task_t;

static void env_scan_task_free(env_scan_task_t *t)
{
    if (!t) return;
    for (int c = 0; c < ENV_CAT_COUNT; c++)
        env_list_free(&t->buckets[c]);
    free(t);
}

static void env_scan_thread_func(GTask        *task,
                                 gpointer      source_object,
                                 gpointer      task_data,
                                 GCancellable *cancellable)
{
    (void)source_object;
    env_scan_task_t *t = task_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    env_list_t all;
    env_list_init(&all);
    read_environ(t->pid, &all);

    if (g_cancellable_is_cancelled(cancellable)) {
        env_list_free(&all);
        return;
    }

    /* Sort and split into per-category buckets */
    if (all.count > 1)
        qsort(all.entries, all.count, sizeof(env_entry_t), env_entry_cmp);

    for (int c = 0; c < ENV_CAT_COUNT; c++)
        env_list_init(&t->buckets[c]);

    for (size_t i = 0; i < all.count; i++) {
        env_category_t cat = all.entries[i].cat;
        env_list_push(&t->buckets[cat], all.entries[i].text, cat);
    }

    env_list_free(&all);

    g_task_return_boolean(task, TRUE);
}

static void env_scan_complete(GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
    (void)source_object;
    ui_ctx_t *ctx = user_data;

    GTask *task = G_TASK(result);
    env_scan_task_t *t = g_task_get_task_data(task);

    if (!t || t->generation != ctx->env_generation)
        return;

    if (g_task_had_error(task))
        return;

    /* Save scroll position */
    GtkAdjustment *env_vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(gtk_widget_get_parent(
            GTK_WIDGET(ctx->env_view))));
    double env_scroll_pos = gtk_adjustment_get_value(env_vadj);

    GtkTreeModel *env_model = GTK_TREE_MODEL(ctx->env_store);

    /* Index existing category rows */
    GtkTreeIter cat_iters[ENV_CAT_COUNT];
    gboolean    cat_exists[ENV_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(env_model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(env_model, &top, ENV_COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < ENV_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(env_model, &top);
        }
    }

    /* Remove empty categories */
    for (int c = 0; c < ENV_CAT_COUNT; c++) {
        if (cat_exists[c] && t->buckets[c].count == 0) {
            gtk_tree_store_remove(ctx->env_store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
    for (int c = 0; c < ENV_CAT_COUNT; c++) {
        if (t->buckets[c].count == 0) continue;

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s (%zu)",
                 env_cat_label[c], t->buckets[c].count);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_append(ctx->env_store, &parent, NULL);
            gtk_tree_store_set(ctx->env_store, &parent,
                               ENV_COL_TEXT, hdr,
                               ENV_COL_MARKUP, hdr_escaped,
                               ENV_COL_CAT, (gint)c, -1);
            g_free(hdr_escaped);
            cat_exists[c] = TRUE;
            cat_iters[c]  = parent;
        } else {
            parent = cat_iters[c];
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_set(ctx->env_store, &parent,
                               ENV_COL_TEXT, hdr,
                               ENV_COL_MARKUP, hdr_escaped, -1);
            g_free(hdr_escaped);
        }

        /* Update child rows in place, adding/removing as needed */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            env_model, &child, &parent);
        size_t bi = 0;

        while (bi < t->buckets[c].count && child_valid) {
            char *markup = env_to_markup(t->buckets[c].entries[bi].text);
            gtk_tree_store_set(ctx->env_store, &child,
                               ENV_COL_TEXT, t->buckets[c].entries[bi].text,
                               ENV_COL_MARKUP, markup,
                               ENV_COL_CAT, (gint)-1, -1);
            g_free(markup);
            bi++;
            child_valid = gtk_tree_model_iter_next(env_model, &child);
        }

        /* Append new rows */
        while (bi < t->buckets[c].count) {
            GtkTreeIter new_child;
            char *markup = env_to_markup(t->buckets[c].entries[bi].text);
            gtk_tree_store_append(ctx->env_store, &new_child, &parent);
            gtk_tree_store_set(ctx->env_store, &new_child,
                               ENV_COL_TEXT, t->buckets[c].entries[bi].text,
                               ENV_COL_MARKUP, markup,
                               ENV_COL_CAT, (gint)-1, -1);
            g_free(markup);
            bi++;
        }

        /* Remove excess rows */
        while (child_valid) {
            child_valid = gtk_tree_store_remove(ctx->env_store, &child);
        }

        /* Restore expand/collapse state */
        GtkTreePath *cat_path = gtk_tree_model_get_path(
            env_model, &cat_iters[c]);
        if (ctx->env_collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->env_view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->env_view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    gtk_adjustment_set_value(env_vadj, env_scroll_pos);
}

/* ── public API ──────────────────────────────────────────────── */

void env_scan_start(ui_ctx_t *ctx, pid_t pid)
{
    /* Cancel any in-flight scan */
    if (ctx->env_cancel) {
        g_cancellable_cancel(ctx->env_cancel);
        g_object_unref(ctx->env_cancel);
    }
    ctx->env_cancel = g_cancellable_new();
    ctx->env_generation++;

    /* Reset collapse state when switching to a different process */
    if (pid != ctx->env_last_pid) {
        ctx->env_collapsed = 0;
        ctx->env_last_pid  = pid;
    }

    env_scan_task_t *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->pid        = pid;
    t->generation = ctx->env_generation;
    t->ctx        = ctx;

    GTask *task = g_task_new(NULL, ctx->env_cancel, env_scan_complete, ctx);
    g_task_set_task_data(task, t, (GDestroyNotify)env_scan_task_free);
    g_task_run_in_thread(task, env_scan_thread_func);
    g_object_unref(task);
}

/* ── signal callbacks ────────────────────────────────────────── */

void on_env_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->env_store), iter,
                       ENV_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < ENV_CAT_COUNT)
        ctx->env_collapsed |= (1u << cat_id);
}

void on_env_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->env_store), iter,
                       ENV_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < ENV_CAT_COUNT)
        ctx->env_collapsed &= ~(1u << cat_id);
}

gboolean on_env_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
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

    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, ENV_COL_CAT, &cat_id, -1);
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
