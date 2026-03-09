/*
 * proc_icon.c – Process icon resolution for the process tree.
 *
 * See proc_icon.h for the full description of the priority chain.
 *
 * Implementation notes
 * ────────────────────
 * The cache key is the process comm name (max 15 chars on Linux).
 * Steam processes use key "steam:<appid>" to avoid collisions.
 *
 * The desktop index maps lowercase executable basename → icon name
 * and is built once at startup from all *.desktop files found in
 * XDG_DATA_DIRS/applications/.  The scan is O(N) directory reads
 * with minimal heap pressure: we only keep entries where Icon != Exec.
 *
 * Priority chain (first hit wins):
 *   1. GTK theme: gtk_icon_theme_load_icon(comm, size, flags)
 *   2. GTK theme: gtk_icon_theme_load_icon(desktop_index[comm], size, flags)
 *   3. Steam art: art_load_async("file://.../librarycache/<id>_icon.jpg")
 *      Fallback: art_load_async(".../library_600x900.jpg")
 *
 * Negative results are cached as a NULL pixbuf stored under a
 * special sentinel pointer (NEGATIVE_CACHE_SENTINEL) in the GHashTable
 * so that NULL values are distinguishable from "not yet looked up".
 *
 * art_load_async() is used for Steam art only.  GTK theme lookups are
 * synchronous (they access a warm in-memory cache inside GtkIconTheme).
 */

#include "proc_icon.h"
#include "../log.h"

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#include <pwd.h>

/* ── sentinel for negative cache entries ─────────────────────── */

/* A unique pointer value that means "probed, found nothing".
 * Stored in the hash table instead of NULL so we can distinguish
 * "not yet looked up" (key absent) from "looked up, miss" (key → sentinel). */
static GdkPixbuf *const MISS = (GdkPixbuf *)(uintptr_t)1;

/* ── internal context ─────────────────────────────────────────── */

struct proc_icon_ctx {
    int           icon_size;   /* target pixel size for scaling          */
    GHashTable   *cache;       /* comm/key → GdkPixbuf* or MISS          */
    GHashTable   *desk_index;  /* lowercase comm → strdup'd icon name    */
};

/* ── desktop file index ───────────────────────────────────────── */

/*
 * Extract "Icon=" and "Exec=" values from a single .desktop file.
 * Returns 1 if both were found.  Writes lowercase comm-basename into
 * `out_exec` and the raw icon name into `out_icon`.
 */
static int parse_desktop_file(const char *path,
                               char *out_exec, size_t exec_sz,
                               char *out_icon, size_t icon_sz)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    out_exec[0] = '\0';
    out_icon[0] = '\0';

    char line[1024];
    int in_entry = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (line[0] == '[') {
            in_entry = (strncmp(line, "[Desktop Entry]", 15) == 0);
            continue;
        }
        if (!in_entry) continue;

        if (strncmp(line, "Icon=", 5) == 0 && out_icon[0] == '\0') {
            const char *val = line + 5;
            size_t vlen = strlen(val);
            if (vlen >= icon_sz) vlen = icon_sz - 1;
            memcpy(out_icon, val, vlen);
            out_icon[vlen] = '\0';
        } else if (strncmp(line, "Exec=", 5) == 0 && out_exec[0] == '\0') {
            /* Exec may be: /full/path [opts] OR just basename [opts] */
            const char *exec_val = line + 5;

            /* Take the first token (the executable path/name) */
            char token[512];
            size_t t = 0;
            for (; exec_val[t] && exec_val[t] != ' ' && exec_val[t] != '\t'
                              && exec_val[t] != '%' && t + 1 < sizeof(token); t++)
                token[t] = exec_val[t];
            token[t] = '\0';

            /* Use basename */
            const char *base = strrchr(token, '/');
            base = base ? base + 1 : token;

            /* lowercase */
            size_t bi;
            for (bi = 0; bi < exec_sz - 1 && base[bi]; bi++)
                out_exec[bi] = (char)g_ascii_tolower((guchar)base[bi]);
            out_exec[bi] = '\0';
        }

        if (out_exec[0] && out_icon[0]) break;  /* got both — done */
    }
    fclose(f);
    return (out_exec[0] != '\0' && out_icon[0] != '\0');
}

/*
 * Scan a single applications/ directory and insert entries into the index.
 */
static void scan_applications_dir(GHashTable *index, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t nlen = strlen(de->d_name);
        if (nlen < 8) continue;   /* shorter than "a.desktop" */
        if (strcmp(de->d_name + nlen - 8, ".desktop") != 0) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);

        char exec_name[256], icon_name[256];
        if (!parse_desktop_file(path, exec_name, sizeof(exec_name),
                                                 icon_name, sizeof(icon_name)))
            continue;

        /* Only store if icon differs from exec name (otherwise the theme
         * lookup by comm already covers it) */
        if (strcmp(exec_name, icon_name) == 0)
            continue;

        /* Don't overwrite an existing entry (first-seen wins, matching
         * XDG_DATA_DIRS precedence since we scan higher-priority dirs first) */
        if (!g_hash_table_contains(index, exec_name)) {
            g_hash_table_insert(index,
                                g_strdup(exec_name),
                                g_strdup(icon_name));
        }
    }
    closedir(d);
}

/*
 * Build the desktop index from all XDG_DATA_DIRS/applications/ dirs.
 * Higher-priority directories (earlier in XDG_DATA_DIRS) are scanned first.
 */
static GHashTable *build_desktop_index(void)
{
    GHashTable *idx = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);

    /* XDG data dirs: $XDG_DATA_HOME/applications first, then XDG_DATA_DIRS */
    const char *data_home = g_get_user_data_dir();
    if (data_home) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/applications", data_home);
        scan_applications_dir(idx, path);
    }

    const gchar * const *dirs = g_get_system_data_dirs();
    for (int i = 0; dirs && dirs[i]; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/applications", dirs[i]);
        scan_applications_dir(idx, path);
    }

    return idx;
}

/* ── GTK theme lookup (synchronous, warm cache) ───────────────── */

/*
 * Try to load an icon by name from the default GTK icon theme.
 * Returns a new GdkPixbuf scaled to `size` (caller owns), or NULL.
 */
static GdkPixbuf *theme_load(const char *icon_name, int size)
{
    if (!icon_name || !icon_name[0]) return NULL;

    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GError *err = NULL;
    GdkPixbuf *pb = gtk_icon_theme_load_icon(
        theme, icon_name, size,
        GTK_ICON_LOOKUP_USE_BUILTIN | GTK_ICON_LOOKUP_FORCE_SIZE |
        GTK_ICON_LOOKUP_GENERIC_FALLBACK,
        &err);
    if (err) {
        g_error_free(err);
        return NULL;
    }
    return pb;   /* already at `size` due to FORCE_SIZE */
}

/* ── Steam art async loading (GIO file:// only) ──────────────── */

typedef struct {
    proc_icon_ctx_t *ictx;
    char             key[64];     /* "steam:<appid>" */
    proc_icon_cb_t   cb;
    void            *userdata;
    GInputStream    *stream;
} steam_art_req_t;

static void on_steam_pixbuf_ready(GObject *source, GAsyncResult *res,
                                  gpointer data)
{
    (void)source;
    steam_art_req_t *req = data;

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_stream_finish(res, &err);
    if (err) { g_error_free(err); pb = NULL; }

    if (req->stream) { g_object_unref(req->stream); req->stream = NULL; }

    if (pb) {
        int w = gdk_pixbuf_get_width(pb);
        int h = gdk_pixbuf_get_height(pb);
        GdkPixbuf *scaled = pb;
        if (w != req->ictx->icon_size || h != req->ictx->icon_size) {
            scaled = gdk_pixbuf_scale_simple(
                pb, req->ictx->icon_size, req->ictx->icon_size,
                GDK_INTERP_BILINEAR);
            g_object_unref(pb);
        }
        g_hash_table_insert(req->ictx->cache, g_strdup(req->key), scaled);
        req->cb(req->key, scaled, req->userdata);
    } else {
        g_hash_table_insert(req->ictx->cache, g_strdup(req->key), MISS);
        req->cb(req->key, NULL, req->userdata);
    }
    free(req);
}

static void on_steam_file_read(GObject *source, GAsyncResult *res,
                               gpointer data)
{
    steam_art_req_t *req = data;
    GFile *file = G_FILE(source);

    GError *err = NULL;
    GFileInputStream *fis = g_file_read_finish(file, res, &err);
    g_object_unref(file);

    if (!fis) {
        if (err) g_error_free(err);
        g_hash_table_insert(req->ictx->cache, g_strdup(req->key), MISS);
        req->cb(req->key, NULL, req->userdata);
        free(req);
        return;
    }

    req->stream = G_INPUT_STREAM(fis);
    gdk_pixbuf_new_from_stream_async(req->stream, NULL,
                                     on_steam_pixbuf_ready, req);
}

/*
 * Search candidate home directories for Steam librarycache art.
 * Tries `home` first, then (if nothing found) every uid≥1000 user
 * from /etc/passwd — needed when running as root (sudo) where
 * g_get_home_dir() returns /root instead of the real user's home.
 */
/*
 * Fire an async load for a known art file path.
 * Returns TRUE and takes ownership of the request if the file exists.
 */
static gboolean fire_art_request(proc_icon_ctx_t *ictx,
                                 const char      *art_path,
                                 const char      *key,
                                 proc_icon_cb_t   cb,
                                 void            *userdata)
{
    struct stat st;
    if (stat(art_path, &st) != 0 || !S_ISREG(st.st_mode))
        return FALSE;

    steam_art_req_t *req = malloc(sizeof(steam_art_req_t));
    if (!req) return FALSE;
    req->ictx     = ictx;
    req->stream   = NULL;
    req->cb       = cb;
    req->userdata = userdata;
    snprintf(req->key, sizeof(req->key), "%s", key);

    GFile *f = g_file_new_for_path(art_path);
    g_file_read_async(f, G_PRIORITY_LOW, NULL, on_steam_file_read, req);
    return TRUE;
}

static gboolean try_steam_art_from_home(proc_icon_ctx_t *ictx,
                                        const char      *app_id,
                                        const char      *key,
                                        proc_icon_cb_t   cb,
                                        void            *userdata,
                                        const char      *home)
{
    static const char *steam_roots[] = {
        "/.steam/steam",
        "/.local/share/Steam",
        NULL
    };

    char art_path[PATH_MAX];

    for (int ri = 0; steam_roots[ri]; ri++) {
        char cache_dir[PATH_MAX];
        snprintf(cache_dir, sizeof(cache_dir),
                 "%s%s/appcache/librarycache", home, steam_roots[ri]);

        /* ── New layout: librarycache/<appid>/ directory ─────────
         *
         * The per-appid directory holds:
         *   <hash>.jpg          – 32×32 game icon  (only .jpg at top level)
         *   <hash>/library_capsule.jpg     – 300×450 portrait
         *   <hash>/library_hero.jpg        – 1920×620 hero
         *   <hash>/library_header.jpg      – 460×215 header
         *
         * We prefer the small icon first, then capsule as fallback.
         */
        char appid_dir[PATH_MAX];
        snprintf(appid_dir, sizeof(appid_dir), "%s/%s", cache_dir, app_id);

        struct stat dstat;
        if (stat(appid_dir, &dstat) == 0 && S_ISDIR(dstat.st_mode)) {
            /* Pass 1: find a .jpg directly in the appid dir (the icon) */
            DIR *d = opendir(appid_dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    size_t nl = strlen(de->d_name);
                    if (nl < 5) continue;
                    if (strcmp(de->d_name + nl - 4, ".jpg") != 0) continue;
                    /* Must be a regular file, not a directory */
                    snprintf(art_path, sizeof(art_path),
                             "%s/%s", appid_dir, de->d_name);
                    if (fire_art_request(ictx, art_path, key, cb, userdata)) {
                        closedir(d);
                        return TRUE;
                    }
                }
                closedir(d);
            }

            /* Pass 2: look for named files inside hash subdirs */
            static const char *subdir_names[] = {
                "library_capsule.jpg",
                "library_header.jpg",
                "library_hero.jpg",
                NULL
            };
            d = opendir(appid_dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    snprintf(art_path, sizeof(art_path),
                             "%s/%s", appid_dir, de->d_name);
                    struct stat sub;
                    if (stat(art_path, &sub) != 0 || !S_ISDIR(sub.st_mode))
                        continue;
                    for (int si = 0; subdir_names[si]; si++) {
                        char named[PATH_MAX];
                        snprintf(named, sizeof(named),
                                 "%s/%s", art_path, subdir_names[si]);
                        if (fire_art_request(ictx, named, key, cb, userdata)) {
                            closedir(d);
                            return TRUE;
                        }
                    }
                }
                closedir(d);
            }
        }

        /* ── Legacy flat layout: librarycache/<appid>_icon.jpg etc. ── */
        static const char *flat_suffixes[] = {
            "_icon.jpg",
            "_library_600x900.jpg",
            "_header.jpg",
            NULL
        };
        for (int si = 0; flat_suffixes[si]; si++) {
            char *p = g_strconcat(cache_dir, "/", app_id, flat_suffixes[si], NULL);
            gboolean hit = fire_art_request(ictx, p, key, cb, userdata);
            g_free(p);
            if (hit) return TRUE;
        }
    }
    return FALSE;
}

static void try_steam_art(proc_icon_ctx_t    *ictx,
                          const char         *app_id,
                          const char         *key,
                          proc_icon_cb_t      cb,
                          void               *userdata)
{
    /* Try the process-owner's home first */
    const char *home = g_get_home_dir();
    if (home && try_steam_art_from_home(ictx, app_id, key, cb, userdata, home))
        return;

    /*
     * Fallback: when running as root (sudo), g_get_home_dir() returns /root
     * but Steam lives under a real user's home.  Scan /etc/passwd for all
     * users with uid >= 1000.
     */
    {
        struct passwd *pw;
        setpwent();
        while ((pw = getpwent()) != NULL) {
            if (!pw->pw_dir || !pw->pw_dir[0]) continue;
            if (pw->pw_uid < 1000) continue;
            if (home && strcmp(pw->pw_dir, home) == 0) continue; /* already tried */
            if (try_steam_art_from_home(ictx, app_id, key, cb, userdata, pw->pw_dir)) {
                endpwent();
                return;
            }
        }
        endpwent();
    }

    /* No art file found */
    g_hash_table_insert(ictx->cache, g_strdup(key), MISS);
    cb(key, NULL, userdata);
}

/* ── public API ───────────────────────────────────────────────── */

proc_icon_ctx_t *proc_icon_ctx_new(int icon_size)
{
    proc_icon_ctx_t *ctx = calloc(1, sizeof(proc_icon_ctx_t));
    if (!ctx) return NULL;

    ctx->icon_size  = icon_size > 0 ? icon_size : PROC_ICON_SIZE;
    ctx->cache      = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    ctx->desk_index = build_desktop_index();

    if (evemon_debug)
        fprintf(stderr, "[PROC_ICON] desktop index: %u entries\n",
                g_hash_table_size(ctx->desk_index));

    return ctx;
}

void proc_icon_ctx_free(proc_icon_ctx_t *ctx)
{
    if (!ctx) return;

    /* Unref all cached pixbufs, then destroy the tables */
    proc_icon_invalidate(ctx);
    g_hash_table_destroy(ctx->cache);
    g_hash_table_destroy(ctx->desk_index);
    free(ctx);
}

void proc_icon_invalidate(proc_icon_ctx_t *ctx)
{
    if (!ctx) return;

    /* Walk the cache and unref all real pixbufs */
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, ctx->cache);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        if (val && val != MISS)
            g_object_unref((GdkPixbuf *)val);
    }
    g_hash_table_remove_all(ctx->cache);
}

GdkPixbuf *proc_icon_get_cached(proc_icon_ctx_t *ctx, const char *comm)
{
    if (!ctx || !comm) return NULL;
    gpointer val = g_hash_table_lookup(ctx->cache, comm);
    if (!val || val == MISS) return NULL;
    return (GdkPixbuf *)val;
}

void proc_icon_lookup_async(proc_icon_ctx_t    *ctx,
                            const char         *comm,
                            const steam_info_t *steam,
                            proc_icon_cb_t      cb,
                            void               *userdata)
{
    if (!ctx || !comm || !cb) return;

    /* Build lookup key.  Steam processes get a per-app-id key so we
     * can show game-specific art rather than a generic Steam icon. */
    char key[96];
    if (steam && steam->is_steam && steam->app_id[0])
        snprintf(key, sizeof(key), "steam:%s", steam->app_id);
    else
        snprintf(key, sizeof(key), "%s", comm);

    /* ── Cache hit ──────────────────────────────────────────── */
    gpointer cached = g_hash_table_lookup(ctx->cache, key);
    if (cached != NULL) {
        /* Either a real pixbuf or the MISS sentinel */
        cb(key, (cached == MISS) ? NULL : (GdkPixbuf *)cached, userdata);
        return;
    }

    /* ── Priority 1: GTK icon theme by comm name ────────────── */
    {
        /* Try lowercase comm first, then as-is */
        char lc[64];
        size_t ci;
        for (ci = 0; ci < sizeof(lc) - 1 && comm[ci]; ci++)
            lc[ci] = (char)g_ascii_tolower((guchar)comm[ci]);
        lc[ci] = '\0';

        GdkPixbuf *pb = theme_load(lc, ctx->icon_size);
        if (!pb && strcmp(lc, comm) != 0)
            pb = theme_load(comm, ctx->icon_size);

        if (pb) {
            g_hash_table_insert(ctx->cache, g_strdup(key), pb);
            cb(key, pb, userdata);
            return;
        }
    }

    /* ── Priority 2: desktop index → GTK icon theme ─────────── */
    {
        char lc[64];
        size_t ci;
        for (ci = 0; ci < sizeof(lc) - 1 && comm[ci]; ci++)
            lc[ci] = (char)g_ascii_tolower((guchar)comm[ci]);
        lc[ci] = '\0';

        const char *icon_name = g_hash_table_lookup(ctx->desk_index, lc);
        if (icon_name) {
            GdkPixbuf *pb = theme_load(icon_name, ctx->icon_size);
            if (pb) {
                g_hash_table_insert(ctx->cache, g_strdup(key), pb);
                cb(key, pb, userdata);
                return;
            }
        }
    }

    /* ── Priority 3: Steam librarycache art ──────────────────── */
    if (steam && steam->is_steam && steam->app_id[0]) {
        /* try_steam_art handles async load and calls cb when done */
        try_steam_art(ctx, steam->app_id, key, cb, userdata);
        return;
    }

    /* ── Miss ────────────────────────────────────────────────── */
    g_hash_table_insert(ctx->cache, g_strdup(key), MISS);
    cb(key, NULL, userdata);
}
