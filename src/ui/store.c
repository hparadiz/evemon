/*
 * store.c – GTK-free process data layer.
 *
 * Owns the lifecycle of proc_record_t entries: diff-update against an
 * incoming proc_entry_t snapshot, ALIVE/DYING/KILLED state transitions,
 * strtab-based string storage, and compaction of expired records.
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

/*
 * re_push_entry_strings – push a record's strings into the store strtab
 * from a source strtab (may be different from s->strtab).
 * Used to remap snapshot-strtab offsets into store-strtab offsets.
 */
static void re_push_entry_strings(proc_store_t *s, proc_record_t *rec,
                                   const proc_entry_t *src,
                                   const proc_strtab_t *src_strtab)
{
    rec->entry = *src;

    rec->entry.name_off      = strtab_push(&s->strtab, PROC_STR(src_strtab, src->name_off));
    rec->entry.cmd_off       = strtab_push(&s->strtab, PROC_STR(src_strtab, src->cmd_off));
    rec->entry.user_off      = strtab_push(&s->strtab, PROC_STR(src_strtab, src->user_off));
    rec->entry.cwd_off       = strtab_push(&s->strtab, PROC_STR(src_strtab, src->cwd_off));
    rec->entry.container_off = strtab_push(&s->strtab, PROC_STR(src_strtab, src->container_off));
    rec->entry.service_off   = strtab_push(&s->strtab, PROC_STR(src_strtab, src->service_off));

    rec->entry.name_len      = (str_len_t)strlen(PROC_STR(&s->strtab, rec->entry.name_off));
    rec->entry.cmd_len       = (str_len_t)strlen(PROC_STR(&s->strtab, rec->entry.cmd_off));
    rec->entry.user_len      = (str_len_t)strlen(PROC_STR(&s->strtab, rec->entry.user_off));
    rec->entry.cwd_len       = (str_len_t)strlen(PROC_STR(&s->strtab, rec->entry.cwd_off));
    rec->entry.container_len = (str_len_t)strlen(PROC_STR(&s->strtab, rec->entry.container_off));
    rec->entry.service_len   = (str_len_t)strlen(PROC_STR(&s->strtab, rec->entry.service_off));
}

/*
 * store_copy_entry – copy from a snapshot entry into the store,
 * remapping string offsets through the snapshot's strtab.
 * Also deep-copies steam extras into s->extras.
 */
static void store_copy_entry(proc_store_t *s, proc_record_t *rec,
                              const proc_entry_t *src,
                              const proc_snapshot_t *snap)
{
    re_push_entry_strings(s, rec, src, &snap->strtab);

    /* Handle steam extras */
    rec->entry.extra_idx = UINT32_MAX;
    if (src->extra_idx != UINT32_MAX && src->extra_idx < snap->extras_len &&
        snap->extras[src->extra_idx].steam) {
        if (s->extras_len >= s->extras_cap) {
            uint32_t newcap = s->extras_cap ? s->extras_cap * 2 : 64;
            proc_extra_t *tmp = (proc_extra_t *)realloc(
                s->extras, newcap * sizeof(proc_extra_t));
            if (tmp) {
                s->extras     = tmp;
                s->extras_cap = newcap;
            }
        }
        if (s->extras_len < s->extras_cap) {
            steam_info_t *si = (steam_info_t *)malloc(sizeof(steam_info_t));
            if (si) {
                *si = *snap->extras[src->extra_idx].steam;
                s->extras[s->extras_len].steam = si;
                rec->entry.extra_idx = s->extras_len;
                s->extras_len++;
            }
        }
    }
}


/* ── public API ──────────────────────────────────────────────── */

void proc_store_init(proc_store_t *s)
{
    memset(s, 0, sizeof(*s));
    s->ht      = calloc(STORE_HT_SIZE, sizeof(store_ht_entry_t));
    s->snap_ht = calloc(STORE_HT_SIZE, sizeof(store_ht_entry_t));
    strtab_init(&s->strtab, 65536);
    s->extras     = NULL;
    s->extras_len = 0;
    s->extras_cap = 0;
    /* OOM on ht/snap_ht is fatal; callers check s->ht != NULL */
}

void proc_store_destroy(proc_store_t *s)
{
    /* Free steam extras */
    if (s->extras) {
        for (uint32_t i = 0; i < s->extras_len; i++)
            free(s->extras[i].steam);
        free(s->extras);
    }
    strtab_free(&s->strtab);
    free(s->records);
    free(s->ht);
    free(s->snap_ht);
    s->records    = NULL;
    s->ht         = NULL;
    s->snap_ht    = NULL;
    s->extras     = NULL;
    s->extras_len = 0;
    s->extras_cap = 0;
    s->count      = 0;
    s->capacity   = 0;
}

/*
 * proc_store_update
 *
 * Algorithm:
 *
 *  A) Rebuild the store strtab and extras: reset both, then populate
 *     from scratch as entries are updated or inserted (clear+repopulate
 *     strategy avoids fragmentation).
 *
 *  B) Build a temporary hash of the incoming snapshot (snap_ht).
 *
 *  C) Walk the store's current records:
 *     - If a record is ALIVE or DYING and the PID is still in the
 *       snapshot → refresh its data, mark ALIVE.
 *     - If a record is ALIVE and the PID is absent from the snapshot
 *       → mark DYING, stamp died_at_us.
 *     - If a record is DYING and the grace period has expired → mark
 *       KILLED (compacted in step E).
 *     - KILLED records are left alone here (already awaiting compaction).
 *
 *  D) Walk the snapshot; any PID not already in the store → append new
 *     ALIVE record.
 *
 *  E) Compact: remove KILLED records from the array and rebuild the ht.
 */
void proc_store_update(proc_store_t *s,
                       const proc_snapshot_t *snap,
                       int64_t mono_now_us)
{
    if (!s->ht || !s->snap_ht) return;   /* OOM during init – skip */
    if (!snap) return;

    const proc_entry_t *entries = snap->entries;
    uint32_t            count   = snap->len;

    /* ── A: save old strtab for DYING record re-push ──────────
     * We must snapshot the current store strtab *before* the reset
     * so that DYING records' existing offsets remain readable while
     * we repopulate this tick. */
    proc_strtab_t old_strtab = { NULL, 0, 0 };
    if (s->strtab.len > 1) {
        old_strtab.buf = (char *)malloc(s->strtab.len);
        if (old_strtab.buf) {
            memcpy(old_strtab.buf, s->strtab.buf, s->strtab.len);
            old_strtab.len = s->strtab.len;
            old_strtab.cap = s->strtab.len;
        }
    }

    /* Reset the store strtab and extras so we can repopulate cleanly */
    strtab_reset(&s->strtab);
    for (uint32_t i = 0; i < s->extras_len; i++)
        free(s->extras[i].steam);
    s->extras_len = 0;

    /* ── B: build snapshot hash ──────────────────────────────── */
    store_ht_entry_t *snap_ht = s->snap_ht;
    memset(snap_ht, 0, STORE_HT_SIZE * sizeof(store_ht_entry_t));
    for (uint32_t i = 0; i < count; i++)
        ht_insert(snap_ht, entries[i].pid, i);

    /* ── C: update / age existing records ───────────────────── */
    for (size_t i = 0; i < s->count; i++) {
        proc_record_t *rec = &s->records[i];
        if (rec->status == PROC_KILLED) continue;

        size_t sidx = ht_find(snap_ht, rec->entry.pid);

        if (sidx != (size_t)-1) {
            /* PID still alive — refresh from snapshot */
            store_copy_entry(s, rec, &entries[sidx], snap);
            rec->status     = PROC_ALIVE;
            rec->died_at_us = 0;
        } else {
            /* PID gone — re-push strings from the old strtab so they
             * stay valid in the freshly-reset store strtab. */
            proc_strtab_t *src_strtab = old_strtab.buf ? &old_strtab : &s->strtab;
            re_push_entry_strings(s, rec, &rec->entry, src_strtab);
            rec->entry.extra_idx = UINT32_MAX;   /* steam not preserved for dying */

            if (rec->status == PROC_ALIVE) {
                rec->status     = PROC_DYING;
                rec->died_at_us = mono_now_us;
            } else if (rec->status == PROC_DYING) {
                if (mono_now_us - rec->died_at_us > STORE_KILL_DELAY_US)
                    rec->status = PROC_KILLED;
            }
        }
    }

    /* Free the old strtab snapshot */
    free(old_strtab.buf);

    /* ── D: append brand-new PIDs ─────────────────────────── */
    for (uint32_t i = 0; i < count; i++) {
        if (ht_find(s->ht, entries[i].pid) != (size_t)-1)
            continue;

        if (s->count >= s->capacity) {
            size_t newcap = s->capacity ? s->capacity * 2 : 256;
            proc_record_t *tmp = realloc(s->records,
                                         newcap * sizeof(proc_record_t));
            if (!tmp) continue;
            s->records  = tmp;
            s->capacity = newcap;
        }

        proc_record_t *rec = &s->records[s->count];
        memset(rec, 0, sizeof(*rec));
        store_copy_entry(s, rec, &entries[i], snap);
        rec->status     = PROC_ALIVE;
        rec->died_at_us = 0;

        ht_insert(s->ht, entries[i].pid, s->count);
        s->count++;
    }

    /* ── E: compact KILLED records ──────────────────────────── */
    int need_compact = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->records[i].status == PROC_KILLED) { need_compact = 1; break; }
    }

    if (need_compact) {
        size_t w = 0;
        for (size_t r = 0; r < s->count; r++) {
            if (s->records[r].status == PROC_KILLED)
                continue;
            if (w != r)
                s->records[w] = s->records[r];
            w++;
        }
        s->count = w;

        ht_clear(s->ht);
        for (size_t i = 0; i < s->count; i++)
            ht_insert(s->ht, s->records[i].entry.pid, i);

        if (s->capacity > 256 && s->count < s->capacity / 2) {
            size_t newcap = s->count < 128 ? 256 : s->count * 2;
            proc_record_t *tmp = realloc(s->records,
                                          newcap * sizeof(proc_record_t));
            if (tmp) {
                s->records  = tmp;
                s->capacity = newcap;
            }
        }
    }

    (void)store_mono_now_us;
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
