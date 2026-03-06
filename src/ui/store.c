/*
 * store.c – GTK-free process data layer.
 *
 * Owns the lifecycle of proc_record_t entries: diff-update against an
 * incoming proc_entry_t snapshot, ALIVE/DYING/KILLED state transitions,
 * deep-copy of steam_info_t, and compaction of expired records.
 *
 * No GTK headers are included here.
 */

#include "store.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── helpers ─────────────────────────────────────────────────── */

/* Monotonic time in microseconds. */
static int64_t store_mono_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* ── hash table operations ───────────────────────────────────── */

static void ht_clear(store_ht_entry_t *ht)
{
    memset(ht, 0, STORE_HT_SIZE * sizeof(store_ht_entry_t));
}

static void ht_insert(store_ht_entry_t *ht, pid_t pid, size_t idx)
{
    unsigned h = (unsigned)pid % STORE_HT_SIZE;
    for (int k = 0; k < STORE_HT_SIZE; k++) {
        if (!ht[h].used) {
            ht[h].pid  = pid;
            ht[h].idx  = idx;
            ht[h].used = 1;
            return;
        }
        h = (h + 1) % STORE_HT_SIZE;
    }
    /* Table full — silently drop.  With STORE_HT_SIZE = 8192 this only
     * occurs on systems with >8192 simultaneous processes. */
}

static size_t ht_find(const store_ht_entry_t *ht, pid_t pid)
{
    unsigned h = (unsigned)pid % STORE_HT_SIZE;
    for (int k = 0; k < STORE_HT_SIZE; k++) {
        if (!ht[h].used) return (size_t)-1;
        if (ht[h].pid == pid) return ht[h].idx;
        h = (h + 1) % STORE_HT_SIZE;
    }
    return (size_t)-1;
}

/* ── deep-copy helpers for proc_entry_t ─────────────────────── */

/*
 * Copy src into dst, replacing the steam pointer with an inline copy
 * stored in steam_out (or NULL if src->steam is NULL).
 */
static void copy_entry(proc_record_t *rec, const proc_entry_t *src)
{
    /* Free any existing heap cmdline before overwriting */
    free(rec->entry.cmdline_long);

    rec->entry = *src;   /* shallow copy — fixes up steam and cmdline_long below */

    /* Deep-copy the optional long cmdline */
    if (src->cmdline_long)
        rec->entry.cmdline_long = strdup(src->cmdline_long);
    else
        rec->entry.cmdline_long = NULL;

    if (src->steam) {
        rec->steam_copy  = *src->steam;          /* inline copy */
        rec->entry.steam = &rec->steam_copy;     /* point at our copy */
    } else {
        rec->entry.steam = NULL;
    }
}

/* ── public API ──────────────────────────────────────────────── */

void proc_store_init(proc_store_t *s)
{
    memset(s, 0, sizeof(*s));
}

void proc_store_destroy(proc_store_t *s)
{
    for (size_t i = 0; i < s->count; i++)
        free(s->records[i].entry.cmdline_long);
    free(s->records);
    s->records  = NULL;
    s->count    = 0;
    s->capacity = 0;
    ht_clear(s->ht);
}

/*
 * proc_store_update
 *
 * Algorithm:
 *
 *  A) Build a temporary hash of the incoming snapshot (snap_ht).
 *
 *  B) Walk the store's current records:
 *     - If a record is ALIVE or DYING and the PID is still in the
 *       snapshot → refresh its data, mark ALIVE.
 *     - If a record is ALIVE and the PID is absent from the snapshot
 *       → mark DYING, stamp died_at_us.
 *     - If a record is DYING and the grace period has expired → mark
 *       KILLED (compacted in step D).
 *     - KILLED records are left alone here (already awaiting compaction).
 *
 *  C) Walk the snapshot; any PID not already in the store → append new
 *     ALIVE record.
 *
 *  D) Compact: remove KILLED records from the array and rebuild the ht.
 */
void proc_store_update(proc_store_t *s,
                       const proc_entry_t *entries, size_t count,
                       int64_t mono_now_us)
{
    /* ── A: build snapshot hash ─────────────────────────────── */

    /* Stack-allocate when count is small, heap otherwise. */
    store_ht_entry_t *snap_ht = calloc(STORE_HT_SIZE, sizeof(store_ht_entry_t));
    if (!snap_ht) return;   /* OOM – skip update */

    for (size_t i = 0; i < count; i++)
        ht_insert(snap_ht, entries[i].pid, i);

    /* ── B: update / age existing records ──────────────────── */

    for (size_t i = 0; i < s->count; i++) {
        proc_record_t *rec = &s->records[i];
        if (rec->status == PROC_KILLED) continue;

        size_t sidx = ht_find(snap_ht, rec->entry.pid);

        if (sidx != (size_t)-1) {
            /* PID still alive — refresh data */
            copy_entry(rec, &entries[sidx]);
            rec->status      = PROC_ALIVE;
            rec->died_at_us  = 0;
        } else {
            /* PID gone */
            if (rec->status == PROC_ALIVE) {
                rec->status     = PROC_DYING;
                rec->died_at_us = mono_now_us;
            } else if (rec->status == PROC_DYING) {
                if (mono_now_us - rec->died_at_us > STORE_KILL_DELAY_US)
                    rec->status = PROC_KILLED;
            }
        }
    }

    /* ── C: append brand-new PIDs ───────────────────────────── */

    for (size_t i = 0; i < count; i++) {
        if (ht_find(s->ht, entries[i].pid) != (size_t)-1)
            continue;   /* already in the store */

        /* Grow the records array if needed (doubling strategy) */
        if (s->count >= s->capacity) {
            size_t newcap = s->capacity ? s->capacity * 2 : 256;
            proc_record_t *tmp = realloc(s->records,
                                         newcap * sizeof(proc_record_t));
            if (!tmp) continue;   /* OOM — skip this entry */
            s->records  = tmp;
            s->capacity = newcap;
        }

        proc_record_t *rec = &s->records[s->count];
        memset(rec, 0, sizeof(*rec));   /* zero before copy_entry's free() */
        copy_entry(rec, &entries[i]);
        rec->status     = PROC_ALIVE;
        rec->died_at_us = 0;

        ht_insert(s->ht, entries[i].pid, s->count);
        s->count++;
    }

    /* ── D: compact KILLED records ──────────────────────────── */

    /* Check whether any compaction is needed before touching memory. */
    int need_compact = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->records[i].status == PROC_KILLED) { need_compact = 1; break; }
    }

    if (need_compact) {
        size_t w = 0;
        for (size_t r = 0; r < s->count; r++) {
            if (s->records[r].status == PROC_KILLED) {
                free(s->records[r].entry.cmdline_long);
                continue;
            }
            if (w != r) s->records[w] = s->records[r];
            w++;
        }
        s->count = w;

        /* Rebuild hash table from scratch to fix up indices */
        ht_clear(s->ht);
        for (size_t i = 0; i < s->count; i++)
            ht_insert(s->ht, s->records[i].entry.pid, i);
    }

    free(snap_ht);

    (void)store_mono_now_us;   /* suppress unused-function warning if caller
                                * always passes mono_now_us explicitly */
}

void proc_store_foreach(const proc_store_t *s,
                        proc_store_cb_t cb, void *userdata)
{
    for (size_t i = 0; i < s->count; i++) {
        const proc_record_t *rec = &s->records[i];
        if (rec->status == PROC_KILLED) continue;
        cb(rec, userdata);
    }
}
