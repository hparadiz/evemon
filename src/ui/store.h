/*
 * store.h – GTK-free process data layer.
 *
 * Owns a flat array of proc_record_t entries, each wrapping a
 * proc_entry_t plus lifecycle metadata (ALIVE / DYING / KILLED).
 * Performs the diff / born / killed logic entirely in C without
 * touching any GTK types.
 *
 * tree.c consumes this layer and translates it to GtkTreeStore ops.
 */

#ifndef UI_STORE_H
#define UI_STORE_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

#include "../proc.h"   /* proc_entry_t, steam_info_t */

/* ── constants ──────────────────────────────────────────────── */

/*
 * How long (µs) a DYING record is kept before it is promoted to
 * KILLED and compacted out of the array.  tree.c uses this same
 * interval for its fade-out animation budget.
 */
#define STORE_KILL_DELAY_US  (2 * 1000000LL)   /* 2 seconds */

/* Hash table size — must be a power of two or at least larger than
 * the max expected process count.  8192 matches tree.c's HT_SIZE. */
#define STORE_HT_SIZE  8192

/* ── record lifecycle ────────────────────────────────────────── */

typedef enum {
    PROC_ALIVE,   /* process present in latest snapshot                  */
    PROC_DYING,   /* process gone; within STORE_KILL_DELAY_US grace window */
    PROC_KILLED,  /* grace period expired; record pending compaction      */
} proc_status_t;

/* ── internal hash-table entry ───────────────────────────────── */

typedef struct {
    pid_t  pid;
    size_t idx;   /* index into proc_store_t::records[] */
    int    used;
} store_ht_entry_t;

/* ── per-process record ──────────────────────────────────────── */

/*
 * proc_record_t embeds a *copy* of proc_entry_t (including a deep-copied
 * steam_info_t) so the store owns all memory and the caller's snapshot
 * can be freed independently after proc_store_update() returns.
 */
typedef struct {
    proc_entry_t   entry;       /* full copy of the process data            */
    steam_info_t   steam_copy;  /* inline copy of steam metadata (if any)   */
    proc_status_t  status;      /* ALIVE / DYING / KILLED                   */
    int64_t        died_at_us;  /* CLOCK_MONOTONIC µs when marked DYING (0 if ALIVE) */
} proc_record_t;

/* ── store ───────────────────────────────────────────────────── */

typedef struct {
    proc_record_t    *records;    /* flat array; capacity managed by store   */
    size_t            count;      /* number of live (non-KILLED) records      */
    size_t            capacity;   /* allocated slots in records[]             */

    store_ht_entry_t  ht[STORE_HT_SIZE]; /* PID → records[] index lookup     */
} proc_store_t;

/* ── API ─────────────────────────────────────────────────────── */

/*
 * proc_store_init – zero-initialise a store.
 * Must be called before the first proc_store_update().
 */
void proc_store_init(proc_store_t *s);

/*
 * proc_store_destroy – free all heap memory owned by the store.
 * After this call the store is unusable until re-initialised.
 */
void proc_store_destroy(proc_store_t *s);

/*
 * proc_store_update – diff a new snapshot into the store.
 *
 *  1. Entries present in the snapshot that are already in the store are
 *     updated in-place (status → ALIVE, data refreshed).
 *  2. Entries present in the snapshot but not in the store are appended
 *     (status = ALIVE).
 *  3. Entries in the store that are absent from the snapshot are
 *     transitioned:  ALIVE → DYING (stamp died_at_us).
 *  4. DYING entries whose grace period has expired are promoted to
 *     KILLED and compacted out of the array.
 *
 * `mono_now_us` must be the current CLOCK_MONOTONIC time in microseconds.
 */
void proc_store_update(proc_store_t *s,
                       const proc_entry_t *entries, size_t count,
                       int64_t mono_now_us);

/*
 * proc_store_foreach – iterate over all non-KILLED records.
 *
 * The callback receives a pointer to each record and the caller-supplied
 * `userdata`.  It must not call proc_store_update() or proc_store_destroy()
 * from within the callback.
 *
 * Records are visited in array order (insertion order for new entries).
 */
typedef void (*proc_store_cb_t)(const proc_record_t *rec, void *userdata);

void proc_store_foreach(const proc_store_t *s,
                        proc_store_cb_t cb, void *userdata);

#endif /* UI_STORE_H */
