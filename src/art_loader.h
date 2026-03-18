/*
 * art_loader.h – Async album art loader for evemon plugins.
 *
 * Loads album art from any URI scheme supported by GIO/gvfs:
 *   - file:///path/to/image.jpg
 *   - https://i.scdn.co/image/...
 *   - data:image/png;base64,...
 *
 * Uses GIO async I/O so the GTK main thread never blocks.
 *
 * Usage:
 *   art_load_async("https://...", my_callback, user_data);
 *
 * The callback receives a GdkPixbuf* (caller owns) or NULL on error.
 * Safe to call from the GTK main thread only.
 *
 * Zero new deps — uses GIO (via GTK3) and GdkPixbuf (via GTK3).
 */

#ifndef EVEMON_ART_LOADER_H
#define EVEMON_ART_LOADER_H

#include <gtk/gtk.h>

/*
 * Callback signature.  `pixbuf` is the loaded image (caller takes
 * ownership and must g_object_unref) or NULL on failure.
 */
typedef void (*art_loaded_cb)(GdkPixbuf *pixbuf, void *user_data);

/*
 * Start an async art load.  `url` can be file://, http(s)://, or
 * data:image/...;base64,...
 *
 * `cb` is called on the GTK main thread when loading completes.
 * Returns a GCancellable* that the caller can use to cancel.
 * The caller must g_object_unref the cancellable when done.
 * Pass NULL for `cancel_out` if you don't need cancellation.
 */
void art_load_async(const char *url, art_loaded_cb cb, void *user_data,
                    GCancellable **cancel_out);

#endif /* EVEMON_ART_LOADER_H */
