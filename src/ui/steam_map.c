/*
 * steam_map.c – per-PID Steam display label side-table.
 *
 * Uses open-addressing linear-probe hash table keyed by pid_t.
 * Bucket count is always a power of two; grows at 75% load.
 */

#include "steam_map.h"

#include <stdlib.h>
#include <string.h>

/* ── internal bucket ─────────────────────────────────────────── */

typedef struct {
    pid_t  pid;
    char  *label;   /* heap-allocated; NULL means empty bucket */
    int    used;    /* 1 = occupied, 0 = empty                  */
} sm_bucket_t;

struct steam_map {
    sm_bucket_t *buckets;
    size_t       cap;     /* always a power of two */
    size_t       count;   /* number of occupied entries */
};

#define SM_INITIAL_CAP 128
#define SM_LOAD_NUM     3
#define SM_LOAD_DEN     4   /* grow when count > cap * 3/4 */

/* ── helpers ─────────────────────────────────────────────────── */

static size_t sm_hash(pid_t pid, size_t cap)
{
    /* Fibonacci-mix for reasonable distribution */
    size_t h = (size_t)(unsigned)pid * 2654435761UL;
    return h & (cap - 1);
}

/* Insert into a pre-sized bucket array (no resize); caller ensures
 * there is enough space and that pid is not already present. */
static void sm_raw_insert(sm_bucket_t *buckets, size_t cap,
                          pid_t pid, char *label)
{
    size_t h = sm_hash(pid, cap);
    for (size_t k = 0; k < cap; k++) {
        sm_bucket_t *b = &buckets[(h + k) & (cap - 1)];
        if (!b->used) {
            b->pid   = pid;
            b->label = label;
            b->used  = 1;
            return;
        }
    }
    /* Should never reach here if caller guarantees load < 100% */
    free(label);
}

static int sm_grow(steam_map_t *m)
{
    size_t newcap = m->cap ? m->cap * 2 : SM_INITIAL_CAP;
    sm_bucket_t *nb = calloc(newcap, sizeof(sm_bucket_t));
    if (!nb) return 0;

    for (size_t i = 0; i < m->cap; i++) {
        sm_bucket_t *b = &m->buckets[i];
        if (b->used)
            sm_raw_insert(nb, newcap, b->pid, b->label);
    }

    free(m->buckets);
    m->buckets = nb;
    m->cap     = newcap;
    return 1;
}

/* Find the bucket for pid, or NULL if not present. */
static sm_bucket_t *sm_find(const steam_map_t *m, pid_t pid)
{
    if (!m->cap) return NULL;
    size_t h = sm_hash(pid, m->cap);
    for (size_t k = 0; k < m->cap; k++) {
        sm_bucket_t *b = &m->buckets[(h + k) & (m->cap - 1)];
        if (!b->used) return NULL;
        if (b->pid == pid) return b;
    }
    return NULL;
}

/* ── public API ──────────────────────────────────────────────── */

steam_map_t *steam_map_create(void)
{
    steam_map_t *m = calloc(1, sizeof(steam_map_t));
    return m;   /* buckets/cap/count all zero-initialised */
}

void steam_map_destroy(steam_map_t *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->buckets[i].used)
            free(m->buckets[i].label);
    }
    free(m->buckets);
    free(m);
}

void steam_map_set(steam_map_t *m, pid_t pid, const char *label)
{
    if (!m) return;

    /* Empty label → remove */
    if (!label || !label[0]) {
        steam_map_remove(m, pid);
        return;
    }

    sm_bucket_t *b = sm_find(m, pid);
    if (b) {
        /* Update existing entry */
        char *copy = strdup(label);
        if (!copy) return;
        free(b->label);
        b->label = copy;
        return;
    }

    /* New entry — grow if needed */
    if (!m->cap || m->count * SM_LOAD_DEN >= m->cap * SM_LOAD_NUM) {
        if (!sm_grow(m)) return;   /* OOM: silently skip */
    }

    char *copy = strdup(label);
    if (!copy) return;

    sm_raw_insert(m->buckets, m->cap, pid, copy);
    m->count++;
}

const char *steam_map_get(const steam_map_t *m, pid_t pid)
{
    if (!m) return NULL;
    sm_bucket_t *b = sm_find(m, pid);
    return b ? b->label : NULL;
}

int steam_map_has_label(const steam_map_t *m, pid_t pid)
{
    return steam_map_get(m, pid) != NULL;
}

void steam_map_remove(steam_map_t *m, pid_t pid)
{
    if (!m || !m->cap) return;
    sm_bucket_t *b = sm_find(m, pid);
    if (!b) return;

    free(b->label);
    b->label = NULL;
    b->used  = 0;
    m->count--;

    /*
     * Rehash the cluster that follows to fix up displaced entries.
     * Without this, entries inserted after a collision would become
     * unreachable once the slot they probed through is freed.
     */
    size_t h = (size_t)(b - m->buckets);
    for (size_t k = 1; k < m->cap; k++) {
        size_t ni = (h + k) & (m->cap - 1);
        sm_bucket_t *nb = &m->buckets[ni];
        if (!nb->used) break;
        /* Remove and re-insert to restore probe invariant */
        pid_t   rpid   = nb->pid;
        char   *rlabel = nb->label;
        nb->used  = 0;
        nb->label = NULL;
        m->count--;
        sm_raw_insert(m->buckets, m->cap, rpid, rlabel);
        m->count++;
    }
}
