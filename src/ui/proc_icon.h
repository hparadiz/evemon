/*
 * proc_icon.h – Process icon resolution for the process tree.
 *
 * Resolves a GdkPixbuf icon for a process using a priority chain:
 *
 *   1. GTK icon theme (gtk_icon_theme_load_icon) keyed by comm name.
 *      The default theme reflects whatever the DM/desktop has installed
 *      in /usr/share/icons/hicolor (and user overrides).  Zero I/O after
 *      the theme object is warm — pure in-memory lookup.
 *
 *   2. XDG .desktop file index: a one-time scan of XDG_DATA_DIRS/applications/
 *      maps executable basename → Icon= key, then retried through the theme.
 *      Handles apps whose binary name differs from their icon name
 *      (e.g. "firefox" binary → "org.mozilla.firefox" icon).
 *
 *   3. Steam librarycache art: if steam metadata is present, loads
 *      ~/.steam/steam/appcache/librarycache/<appid>_icon.jpg (or _library_600x900.jpg
 *      as fallback) via the existing art_load_async() infrastructure.
 *
 * All results are scaled to PROC_ICON_SIZE × PROC_ICON_SIZE px immediately
 * and stored in a GHashTable<char* key → GdkPixbuf*>.  A NULL value in the
 * table means "already probed, nothing found" (negative cache).
 *
 * Thread safety: all functions must be called from the GTK main thread.
 */

#ifndef UI_PROC_ICON_H
#define UI_PROC_ICON_H

#include <gtk/gtk.h>
#include "../steam.h"

/* Pixel size of icons shown in the tree.  Matches a comfortable 16 px
 * default row height; callers may scale this with font size if needed. */
#define PROC_ICON_SIZE 16

/* Opaque icon-resolver state.  Create one per UI context. */
typedef struct proc_icon_ctx proc_icon_ctx_t;

/* Callback delivered on the GTK main thread when an icon is resolved.
 * `pixbuf` is owned by the icon cache — do NOT g_object_unref it.
 * `key`    is the same string passed to proc_icon_lookup_async(). */
typedef void (*proc_icon_cb_t)(const char *key,
                               GdkPixbuf  *pixbuf,   /* NULL = not found */
                               void       *userdata);

/*
 * proc_icon_ctx_new – create a new icon resolution context.
 *
 * Scans XDG_DATA_DIRS/applications/ for .desktop files and builds an
 * in-memory index (comm → icon name).  This scan is O(number of .desktop
 * files) and happens once at startup; it is cheap on any modern system.
 *
 * `icon_size` — desired pixel size (typically PROC_ICON_SIZE).
 */
proc_icon_ctx_t *proc_icon_ctx_new(int icon_size);

/*
 * proc_icon_ctx_free – destroy the context and release all cached pixbufs.
 */
void proc_icon_ctx_free(proc_icon_ctx_t *ctx);

/*
 * proc_icon_lookup_async – look up or schedule loading of the icon for
 * a process identified by `comm` (the /proc/<pid>/comm basename).
 *
 * `steam` may be NULL.  If non-NULL and is_steam != 0 the Steam art path
 * is tried as the final fallback.
 *
 * If the icon is already cached the callback is invoked synchronously
 * (before the function returns).  Otherwise it is invoked asynchronously
 * on the GTK main thread once loading completes.
 *
 * Negative cache entries (previous miss) trigger a synchronous NULL
 * callback without re-probing.
 */
void proc_icon_lookup_async(proc_icon_ctx_t      *ctx,
                            const char           *comm,
                            const steam_info_t   *steam,
                            proc_icon_cb_t        cb,
                            void                 *userdata);

/*
 * proc_icon_get_cached – synchronous cache-only lookup.
 * Returns the cached pixbuf (do NOT unref) or NULL if not yet resolved.
 * Does NOT trigger any loading or fire any callbacks.
 */
GdkPixbuf *proc_icon_get_cached(proc_icon_ctx_t *ctx, const char *comm);

/*
 * proc_icon_invalidate – flush the entire cache (e.g. on theme change).
 * All previously cached pixbufs are unref'd.  New lookups will re-probe.
 */
void proc_icon_invalidate(proc_icon_ctx_t *ctx);

#endif /* UI_PROC_ICON_H */
