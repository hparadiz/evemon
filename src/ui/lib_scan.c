/*
 * lib_scan.c – shared library / DLL browser for the sidebar.
 *
 * Reads /proc/<pid>/maps, extracts loaded shared libraries from the
 * executable (r-x) code mappings, and presents them in a categorised
 * GtkTreeStore.
 *
 * Categories:
 *   - System        — libraries under /usr/lib, /lib, /nix/store, etc.
 *   - Application   — libraries shipped with the application
 *   - Wine built-in — Wine's own reimplementations of Windows DLLs
 *   - Windows DLLs  — real Windows .dll files running under Wine/Proton
 *   - Runtime       — ld-linux, vDSO, libc, libm, libpthread, etc.
 *
 * For Wine/Proton processes the scanner understands the prefix layout
 * and classifies DLLs from the game directory vs. system vs. Proton
 * dist separately.
 */

#include "ui_internal.h"
#include "../steam.h"

#include <unistd.h>

/* ── category labels ─────────────────────────────────────────── */

const char *lib_cat_label[LIB_CAT_COUNT] = {
    [LIB_CAT_RUNTIME]      = "Runtime",
    [LIB_CAT_SYSTEM]       = "System Libraries",
    [LIB_CAT_APPLICATION]  = "Application Libraries",
    [LIB_CAT_WINE_BUILTIN] = "Wine / Proton Built-in",
    [LIB_CAT_WINDOWS_DLL]  = "Windows DLLs",
    [LIB_CAT_OTHER]        = "Other",
};

/* ── helpers ─────────────────────────────────────────────────── */

/* Extract just the filename from a full path */
static const char *basename_ptr(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/*
 * Try to extract a version string from a .so filename.
 * e.g. "libfoo.so.1.2.3" → "1.2.3"
 *      "libfoo.so"       → "" (no version)
 *      "ld-linux-x86-64.so.2" → "2"
 */
static void extract_so_version(const char *filename, char *ver, size_t versz)
{
    ver[0] = '\0';

    /* Find ".so." — version digits follow */
    const char *p = strstr(filename, ".so.");
    if (p) {
        p += 4;  /* skip ".so." */
        snprintf(ver, versz, "%s", p);
        return;
    }

    /* Check for ".so" at the end — no embedded version */
    size_t len = strlen(filename);
    if (len >= 3 && strcmp(filename + len - 3, ".so") == 0)
        return;  /* no version */
}

/*
 * Try to extract a version-like string from a Windows DLL path.
 * We look for version patterns in the directory path, e.g.:
 *   .../Proton - Experimental/files/lib64/wine/x86_64-windows/foo.dll
 *   .../steamapps/common/GameName/bin/foo.dll
 * Not much version info in the filename itself for DLLs.
 */
static void extract_dll_version(const char *fullpath, char *ver, size_t versz)
{
    ver[0] = '\0';

    /* For DLLs, version info is rarely in the filename. We could
     * try to read the PE version resource, but that's expensive.
     * For now, leave blank — the path context is more useful. */
    (void)fullpath;
    (void)versz;
}

/*
 * Determine a short "origin" label from the full path to give context
 * about where the library came from.
 */
static void extract_origin(const char *fullpath, char *origin, size_t osz)
{
    origin[0] = '\0';

    /* Wine/Proton paths */
    const char *p;
    if ((p = strstr(fullpath, "/dist/lib")) != NULL ||
        (p = strstr(fullpath, "/files/lib")) != NULL) {
        /* Proton's own libraries */
        /* Try to find the Proton version dir name */
        /* e.g. .../Proton - Experimental/files/lib64/... */
        const char *common = strstr(fullpath, "/common/");
        if (common) {
            common += 8;
            const char *slash = strchr(common, '/');
            if (slash) {
                size_t len = (size_t)(slash - common);
                if (len >= osz) len = osz - 1;
                memcpy(origin, common, len);
                origin[len] = '\0';
                return;
            }
        }
        snprintf(origin, osz, "Proton");
        return;
    }

    if (strstr(fullpath, "/wine/") || strstr(fullpath, "/Wine/")) {
        /* System wine or prefix wine libs */
        if (strstr(fullpath, "x86_64-windows") ||
            strstr(fullpath, "i386-windows") ||
            strstr(fullpath, "x86_64-unix") ||
            strstr(fullpath, "i386-unix"))
            snprintf(origin, osz, "Wine");
        else
            snprintf(origin, osz, "Wine");
        return;
    }

    /* Steam runtime */
    if (strstr(fullpath, "steam-runtime") ||
        strstr(fullpath, "SteamLinuxRuntime") ||
        strstr(fullpath, "pressure-vessel")) {
        if (strstr(fullpath, "sniper"))
            snprintf(origin, osz, "Runtime (sniper)");
        else if (strstr(fullpath, "soldier"))
            snprintf(origin, osz, "Runtime (soldier)");
        else if (strstr(fullpath, "scout"))
            snprintf(origin, osz, "Runtime (scout)");
        else
            snprintf(origin, osz, "Steam Runtime");
        return;
    }

    /* Game directory */
    if (strstr(fullpath, "/steamapps/common/")) {
        const char *common = strstr(fullpath, "/common/");
        if (common) {
            common += 8;
            const char *slash = strchr(common, '/');
            if (slash) {
                size_t len = (size_t)(slash - common);
                if (len >= osz) len = osz - 1;
                memcpy(origin, common, len);
                origin[len] = '\0';
                return;
            }
        }
    }

    /* compatdata = Wine prefix (game-specific overrides) */
    if (strstr(fullpath, "/compatdata/")) {
        snprintf(origin, osz, "Wine Prefix");
        return;
    }

    /* System paths */
    if (strncmp(fullpath, "/usr/lib", 8) == 0 ||
        strncmp(fullpath, "/lib", 4) == 0 ||
        strncmp(fullpath, "/nix/store", 10) == 0)
        return;  /* leave blank for system libs — it's obvious */
}

/* ── library entry ───────────────────────────────────────────── */

typedef struct {
    char  name[256];          /* library filename (e.g. "libfoo.so.1.2") */
    char  version[64];        /* extracted version string                */
    char  origin[128];        /* short origin label                      */
    char  fullpath[512];      /* full filesystem path                    */
    lib_category_t cat;
    size_t size_kb;           /* total code mapping size in KiB          */
} lib_entry_t;

typedef struct {
    lib_entry_t *entries;
    size_t       count;
    size_t       capacity;
} lib_list_t;

static void lib_list_init(lib_list_t *l)
{
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void lib_list_free(lib_list_t *l)
{
    free(l->entries);
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void lib_list_push(lib_list_t *l, const lib_entry_t *e)
{
    if (l->count >= l->capacity) {
        size_t newcap = l->capacity ? l->capacity * 2 : 64;
        lib_entry_t *tmp = realloc(l->entries, newcap * sizeof(lib_entry_t));
        if (!tmp) return;
        l->entries  = tmp;
        l->capacity = newcap;
    }
    l->entries[l->count++] = *e;
}

/* Check if we already have this library (merge sizes for multi-segment mappings) */
static lib_entry_t *lib_list_find(lib_list_t *l, const char *fullpath)
{
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->entries[i].fullpath, fullpath) == 0)
            return &l->entries[i];
    }
    return NULL;
}

/* ── classification ──────────────────────────────────────────── */

static int is_runtime_lib(const char *name)
{
    /* Core runtime libraries that every process loads */
    static const char *runtime_names[] = {
        "ld-linux",
        "ld-musl",
        "libc.so",
        "libc-",
        "libm.so",
        "libm-",
        "libdl.so",
        "libdl-",
        "libpthread.so",
        "libpthread-",
        "librt.so",
        "librt-",
        "libresolv.so",
        "libnss_",
        "libgcc_s.so",
        "libstdc++.so",
        NULL
    };

    for (int i = 0; runtime_names[i]; i++) {
        if (strncmp(name, runtime_names[i], strlen(runtime_names[i])) == 0)
            return 1;
    }
    return 0;
}

static int is_windows_path(const char *path)
{
    return (strstr(path, "x86_64-windows") != NULL ||
            strstr(path, "i386-windows") != NULL ||
            strstr(path, "i686-windows") != NULL);
}

static int is_wine_unix_lib(const char *path)
{
    return (strstr(path, "x86_64-unix") != NULL ||
            strstr(path, "i386-unix") != NULL ||
            strstr(path, "i686-unix") != NULL);
}

static int has_so_extension(const char *name)
{
    /* Match *.so or *.so.* */
    const char *p = strstr(name, ".so");
    if (!p) return 0;
    char after = p[3];
    return (after == '\0' || after == '.');
}

static int has_dll_extension(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return 0;
    const char *ext = name + len - 4;
    return (strcasecmp(ext, ".dll") == 0 ||
            strcasecmp(ext, ".drv") == 0 ||
            strcasecmp(ext, ".exe") == 0 ||
            strcasecmp(ext, ".sys") == 0 ||
            strcasecmp(ext, ".ocx") == 0);
}

static lib_category_t classify_lib(const char *fullpath, const char *name)
{
    /* Windows DLLs in Wine's windows-side directories */
    if (is_windows_path(fullpath))
        return LIB_CAT_WINDOWS_DLL;

    /* DLL/EXE file extensions (in game directories or wine prefixes) */
    if (has_dll_extension(name)) {
        if (strstr(fullpath, "/wine/") ||
            strstr(fullpath, "/dist/") ||
            strstr(fullpath, "/files/"))
            return LIB_CAT_WINE_BUILTIN;
        return LIB_CAT_WINDOWS_DLL;
    }

    /* Wine's Unix-side built-in .so wrappers */
    if (is_wine_unix_lib(fullpath))
        return LIB_CAT_WINE_BUILTIN;

    /* Wine-specific .so files (ntdll.so, wineboot.so, etc.) */
    if (strstr(fullpath, "/wine/") && has_so_extension(name))
        return LIB_CAT_WINE_BUILTIN;

    /* Runtime / linker libraries */
    if (is_runtime_lib(name))
        return LIB_CAT_RUNTIME;

    /* System libraries in well-known system paths */
    if (strncmp(fullpath, "/usr/lib", 8) == 0 ||
        strncmp(fullpath, "/usr/local/lib", 14) == 0 ||
        strncmp(fullpath, "/lib/", 5) == 0 ||
        strncmp(fullpath, "/lib64/", 7) == 0 ||
        strncmp(fullpath, "/nix/store", 10) == 0)
        return LIB_CAT_SYSTEM;

    /* Steam runtime paths → system-equivalent */
    if (strstr(fullpath, "steam-runtime") ||
        strstr(fullpath, "SteamLinuxRuntime") ||
        strstr(fullpath, "pressure-vessel"))
        return LIB_CAT_SYSTEM;

    /* Everything else is application-specific */
    return LIB_CAT_APPLICATION;
}

/* Sort: by category, then alphabetically by name within category */
static int lib_entry_cmp(const void *a, const void *b)
{
    const lib_entry_t *ea = a;
    const lib_entry_t *eb = b;
    if (ea->cat != eb->cat)
        return (int)ea->cat - (int)eb->cat;
    return strcasecmp(ea->name, eb->name);
}

/* ── /proc/<pid>/maps reader ─────────────────────────────────── */

static void format_lib_size(size_t kb, char *buf, size_t bufsz)
{
    if (kb >= 1048576)
        snprintf(buf, bufsz, "%.1f GiB", (double)kb / 1048576.0);
    else if (kb >= 1024)
        snprintf(buf, bufsz, "%.1f MiB", (double)kb / 1024.0);
    else
        snprintf(buf, bufsz, "%zu KiB", kb);
}

static void read_libraries(pid_t pid, lib_list_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        unsigned long addr_start = 0, addr_end = 0;
        char perms[8] = "----";
        unsigned long offset = 0;
        char dev[16] = "";
        unsigned long inode = 0;

        int n = sscanf(line, "%lx-%lx %4s %lx %15s %lu",
                       &addr_start, &addr_end, perms, &offset, dev, &inode);
        if (n < 5) continue;

        /* Only consider executable mappings (code segments) */
        if (perms[2] != 'x') continue;

        /* Extract pathname */
        char pathname[512] = "";
        {
            const char *p = line;
            int fields = 0;
            /* Skip the first 4 whitespace-delimited tokens:
             *   (1) addr_start-addr_end
             *   (2) perms
             *   (3) offset
             *   (4) dev
             * Then skip the inode (token 5) separately, leaving p
             * at the pathname (or end of line for anonymous maps). */
            while (fields < 4 && *p) {
                while (*p && *p != ' ' && *p != '\t') p++;
                while (*p == ' ' || *p == '\t') p++;
                fields++;
            }
            /* p now points at the inode field */
            while (*p && *p != ' ' && *p != '\t') p++;  /* skip inode */
            while (*p == ' ' || *p == '\t') p++;
            if (*p)
                snprintf(pathname, sizeof(pathname), "%s", p);
        }

        /* Skip anonymous mappings and kernel pseudo-files */
        if (pathname[0] == '\0' || pathname[0] == '[')
            continue;

        /* Must be a shared library or DLL */
        const char *fname = basename_ptr(pathname);
        if (!has_so_extension(fname) && !has_dll_extension(fname))
            continue;

        size_t size_kb = (addr_end - addr_start) / 1024;

        /* Check if we already have this library (multi-segment) */
        lib_entry_t *existing = lib_list_find(out, pathname);
        if (existing) {
            existing->size_kb += size_kb;
            continue;
        }

        lib_entry_t ent;
        memset(&ent, 0, sizeof(ent));
        snprintf(ent.name, sizeof(ent.name), "%s", fname);
        snprintf(ent.fullpath, sizeof(ent.fullpath), "%s", pathname);
        ent.size_kb = size_kb;
        ent.cat = classify_lib(pathname, fname);

        if (has_so_extension(fname))
            extract_so_version(fname, ent.version, sizeof(ent.version));
        else
            extract_dll_version(pathname, ent.version, sizeof(ent.version));

        extract_origin(pathname, ent.origin, sizeof(ent.origin));

        lib_list_push(out, &ent);
    }
    fclose(f);
}

/* ── Pango markup for library entries ────────────────────────── */

static char *lib_to_markup(const lib_entry_t *e)
{
    char sz_buf[32];
    format_lib_size(e->size_kb, sz_buf, sizeof(sz_buf));

    char *name_esc  = g_markup_escape_text(e->name, -1);
    char *sz_esc    = g_markup_escape_text(sz_buf, -1);
    char *ver_esc   = e->version[0]
                        ? g_markup_escape_text(e->version, -1)
                        : NULL;
    char *origin_esc = e->origin[0]
                        ? g_markup_escape_text(e->origin, -1)
                        : NULL;

    /* Build markup: "name  ver  size  [origin]" */
    GString *s = g_string_new(NULL);

    /* Library name — bold */
    g_string_append_printf(s, "<b>%s</b>", name_esc);

    /* Version — subtle colour */
    if (ver_esc)
        g_string_append_printf(s,
            "  <span foreground=\"#88aa88\">%s</span>", ver_esc);

    /* Size — blue tint */
    g_string_append_printf(s,
        "  <span foreground=\"#6699cc\">%s</span>", sz_esc);

    /* Origin — dim */
    if (origin_esc)
        g_string_append_printf(s,
            "  <span foreground=\"#888888\">(%s)</span>", origin_esc);

    char *result = g_string_free(s, FALSE);

    g_free(name_esc);
    g_free(sz_esc);
    g_free(ver_esc);
    g_free(origin_esc);

    return result;
}

/* ── async GTask ─────────────────────────────────────────────── */

typedef struct {
    pid_t          pid;
    guint          generation;
    lib_list_t     buckets[LIB_CAT_COUNT];
    size_t         cat_count[LIB_CAT_COUNT];
    size_t         cat_total_kb[LIB_CAT_COUNT];
    ui_ctx_t      *ctx;
} lib_scan_task_t;

static void lib_scan_task_free(lib_scan_task_t *t)
{
    if (!t) return;
    for (int c = 0; c < LIB_CAT_COUNT; c++)
        lib_list_free(&t->buckets[c]);
    free(t);
}

static void lib_scan_thread_func(GTask        *task,
                                 gpointer      source_object,
                                 gpointer      task_data,
                                 GCancellable *cancellable)
{
    (void)source_object;
    lib_scan_task_t *t = task_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    lib_list_t all;
    lib_list_init(&all);
    read_libraries(t->pid, &all);

    if (g_cancellable_is_cancelled(cancellable)) {
        lib_list_free(&all);
        return;
    }

    /* Sort and split into per-category buckets */
    if (all.count > 1)
        qsort(all.entries, all.count, sizeof(lib_entry_t), lib_entry_cmp);

    for (int c = 0; c < LIB_CAT_COUNT; c++) {
        lib_list_init(&t->buckets[c]);
        t->cat_count[c]    = 0;
        t->cat_total_kb[c] = 0;
    }

    for (size_t i = 0; i < all.count; i++) {
        lib_category_t cat = all.entries[i].cat;
        lib_list_push(&t->buckets[cat], &all.entries[i]);
        t->cat_count[cat]++;
        t->cat_total_kb[cat] += all.entries[i].size_kb;
    }

    lib_list_free(&all);

    g_task_return_boolean(task, TRUE);
}

static void lib_scan_complete(GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
    (void)source_object;
    ui_ctx_t *ctx = user_data;

    GTask *task = G_TASK(result);
    lib_scan_task_t *t = g_task_get_task_data(task);

    if (!t || t->generation != ctx->lib_generation)
        return;

    if (g_task_had_error(task))
        return;
    /* Save scroll position */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(gtk_widget_get_parent(
            GTK_WIDGET(ctx->lib_view))));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->lib_store);

    /* Index existing category rows */
    GtkTreeIter cat_iters[LIB_CAT_COUNT];
    gboolean    cat_exists[LIB_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(model, &top, LIB_COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < LIB_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    /* Remove empty categories */
    for (int c = 0; c < LIB_CAT_COUNT; c++) {
        if (cat_exists[c] && t->buckets[c].count == 0) {
            gtk_tree_store_remove(ctx->lib_store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
    for (int c = 0; c < LIB_CAT_COUNT; c++) {
        if (t->buckets[c].count == 0) continue;

        char sz_buf[32];
        format_lib_size(t->cat_total_kb[c], sz_buf, sizeof(sz_buf));

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s — %zu libraries, %s",
                 lib_cat_label[c], t->cat_count[c], sz_buf);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_append(ctx->lib_store, &parent, NULL);
            gtk_tree_store_set(ctx->lib_store, &parent,
                               LIB_COL_TEXT, hdr,
                               LIB_COL_MARKUP, hdr_escaped,
                               LIB_COL_CAT, (gint)c, -1);
            g_free(hdr_escaped);
            cat_exists[c] = TRUE;
            cat_iters[c]  = parent;
        } else {
            parent = cat_iters[c];
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_set(ctx->lib_store, &parent,
                               LIB_COL_TEXT, hdr,
                               LIB_COL_MARKUP, hdr_escaped, -1);
            g_free(hdr_escaped);
        }

        /* Update child rows in place, adding/removing as needed */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);
        size_t bi = 0;

        while (bi < t->buckets[c].count && child_valid) {
            char *markup = lib_to_markup(&t->buckets[c].entries[bi]);
            gtk_tree_store_set(ctx->lib_store, &child,
                               LIB_COL_TEXT, t->buckets[c].entries[bi].name,
                               LIB_COL_MARKUP, markup,
                               LIB_COL_CAT, (gint)-1, -1);
            g_free(markup);

            /* Tooltip with full path */
            GtkTreePath *tp = gtk_tree_model_get_path(model, &child);
            (void)tp;  /* tooltips set via query-tooltip signal */
            gtk_tree_path_free(tp);

            bi++;
            child_valid = gtk_tree_model_iter_next(model, &child);
        }

        /* Append new rows */
        while (bi < t->buckets[c].count) {
            GtkTreeIter new_child;
            char *markup = lib_to_markup(&t->buckets[c].entries[bi]);
            gtk_tree_store_append(ctx->lib_store, &new_child, &parent);
            gtk_tree_store_set(ctx->lib_store, &new_child,
                               LIB_COL_TEXT, t->buckets[c].entries[bi].fullpath,
                               LIB_COL_MARKUP, markup,
                               LIB_COL_CAT, (gint)-1, -1);
            g_free(markup);
            bi++;
        }

        /* Remove excess rows */
        while (child_valid) {
            child_valid = gtk_tree_store_remove(ctx->lib_store, &child);
        }

        /* Restore expand/collapse state */
        GtkTreePath *cat_path = gtk_tree_model_get_path(
            model, &cat_iters[c]);
        if (ctx->lib_collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->lib_view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->lib_view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);
}

/* ── public API ──────────────────────────────────────────────── */

void lib_scan_start(ui_ctx_t *ctx, pid_t pid)
{
    /* Cancel any in-flight scan */
    if (ctx->lib_cancel) {
        g_cancellable_cancel(ctx->lib_cancel);
        g_object_unref(ctx->lib_cancel);
    }
    ctx->lib_cancel = g_cancellable_new();
    ctx->lib_generation++;

    /* Reset collapse state when switching to a different process */
    if (pid != ctx->lib_last_pid) {
        ctx->lib_collapsed = 0;
        ctx->lib_last_pid  = pid;
    }

    lib_scan_task_t *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->pid        = pid;
    t->generation = ctx->lib_generation;
    t->ctx        = ctx;

    GTask *task = g_task_new(NULL, ctx->lib_cancel, lib_scan_complete, ctx);
    g_task_set_task_data(task, t, (GDestroyNotify)lib_scan_task_free);
    g_task_run_in_thread(task, lib_scan_thread_func);
    g_object_unref(task);
}

/* ── signal callbacks ────────────────────────────────────────── */

void on_lib_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->lib_store), iter,
                       LIB_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < LIB_CAT_COUNT)
        ctx->lib_collapsed |= (1u << cat_id);
}

void on_lib_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->lib_store), iter,
                       LIB_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < LIB_CAT_COUNT)
        ctx->lib_collapsed &= ~(1u << cat_id);
}

gboolean on_lib_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
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
    gtk_tree_model_get(model, &iter, LIB_COL_CAT, &cat_id, -1);
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

/* ── tooltip callback: show full path on hover ───────────────── */

gboolean on_lib_query_tooltip(GtkWidget  *widget,
                              gint        x,
                              gint        y,
                              gboolean    keyboard_mode,
                              GtkTooltip *tooltip,
                              gpointer    data)
{
    (void)data;
    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeModel *model;
    GtkTreePath *path;
    GtkTreeIter iter;

    if (!gtk_tree_view_get_tooltip_context(view, &x, &y, keyboard_mode,
                                            &model, &path, &iter)) {
        return FALSE;
    }

    gint cat_id = -1;
    gchar *text = NULL;
    gtk_tree_model_get(model, &iter,
                       LIB_COL_CAT, &cat_id,
                       LIB_COL_TEXT, &text, -1);

    /* Only show tooltip for leaf rows (libraries, not category headers) */
    if (cat_id >= 0 || !text || !text[0]) {
        g_free(text);
        gtk_tree_path_free(path);
        return FALSE;
    }

    gtk_tooltip_set_text(tooltip, text);
    gtk_tree_view_set_tooltip_row(view, tooltip, path);

    g_free(text);
    gtk_tree_path_free(path);
    return TRUE;
}
