/*
 * steam_map.h – per-PID Steam display label side-table.
 *
 * Stores Steam/Proton/Wine display labels keyed by PID in a separate
 * memory space, entirely outside the GtkTreeStore.  This keeps the
 * tree-store columns free of Steam-specific data while preserving all
 * functionality (cell rendering, icon matching, etc.).
 *
 * Thread-safety: all operations must be called from the GTK main thread.
 */

#ifndef STEAM_MAP_H
#define STEAM_MAP_H

#include <sys/types.h>
#include <stddef.h>

/* ── opaque handle ───────────────────────────────────────────── */

typedef struct steam_map steam_map_t;

/* ── lifecycle ───────────────────────────────────────────────── */

/*
 * steam_map_create – allocate and return a new empty map.
 * Returns NULL on OOM.
 */
steam_map_t *steam_map_create(void);

/*
 * steam_map_destroy – free all memory owned by the map.
 * After this call the pointer must not be used.
 */
void steam_map_destroy(steam_map_t *m);

/* ── operations ──────────────────────────────────────────────── */

/*
 * steam_map_set – associate `label` with `pid`.
 * Passing NULL or an empty string removes any existing entry.
 * The map stores an internal copy of the string.
 */
void steam_map_set(steam_map_t *m, pid_t pid, const char *label);

/*
 * steam_map_get – return the label for `pid`, or NULL if absent.
 * The returned pointer is valid until the next steam_map_set/remove
 * call for the same PID, or until steam_map_destroy().
 */
const char *steam_map_get(const steam_map_t *m, pid_t pid);

/*
 * steam_map_has_label – return non-zero if `pid` has a non-empty label.
 * Shorthand for (steam_map_get(m, pid) != NULL).
 */
int steam_map_has_label(const steam_map_t *m, pid_t pid);

/*
 * steam_map_remove – remove any entry for `pid`.
 */
void steam_map_remove(steam_map_t *m, pid_t pid);

#endif /* STEAM_MAP_H */
