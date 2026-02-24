#ifndef ALLMON_PROFILE_H
#define ALLMON_PROFILE_H

/*
 * profile.h – lightweight profiler for timing code sections.
 *
 * Usage:
 *   profile_init();                       // call once at startup
 *
 *   PROFILE_BEGIN(snapshot_build);         // start a named timer
 *   ... expensive work ...
 *   PROFILE_END(snapshot_build);           // stop & record elapsed time
 *
 *   profile_dump(stderr);                  // print summary to a stream
 *   profile_get(name, &last, &avg, &max); // query a timer programmatically
 *   profile_reset();                       // clear all recorded data
 */

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

/* ── configuration ───────────────────────────────────────────── */
#define PROFILE_MAX_TIMERS  32
#define PROFILE_NAME_MAX    64

/* ── internal data ───────────────────────────────────────────── */

typedef struct {
    char     name[PROFILE_NAME_MAX];
    double   last_ms;      /* most recent measurement (ms) */
    double   total_ms;     /* cumulative total (ms)        */
    double   max_ms;       /* worst case (ms)              */
    size_t   hits;         /* number of measurements       */
} profile_timer_t;

typedef struct {
    profile_timer_t timers[PROFILE_MAX_TIMERS];
    size_t          count;
    pthread_mutex_t lock;
} profile_state_t;

/* Single global profiler instance (defined in profile.c) */
extern profile_state_t g_profile;

/* ── helpers (static, header-only) ───────────────────────────── */

static inline void profile_init(void)
{
    memset(&g_profile, 0, sizeof(g_profile));
    pthread_mutex_init(&g_profile.lock, NULL);
}

static inline void profile_reset(void)
{
    pthread_mutex_lock(&g_profile.lock);
    for (size_t i = 0; i < g_profile.count; i++) {
        g_profile.timers[i].last_ms  = 0.0;
        g_profile.timers[i].total_ms = 0.0;
        g_profile.timers[i].max_ms   = 0.0;
        g_profile.timers[i].hits     = 0;
    }
    pthread_mutex_unlock(&g_profile.lock);
}

/* Find or create a named timer slot; returns its index or -1 on overflow. */
static inline int profile__slot(const char *name)
{
    /* Fast path: look for existing entry */
    for (size_t i = 0; i < g_profile.count; i++) {
        if (strcmp(g_profile.timers[i].name, name) == 0)
            return (int)i;
    }
    /* Slow path: allocate a new slot */
    if (g_profile.count >= PROFILE_MAX_TIMERS)
        return -1;
    int idx = (int)g_profile.count++;
    strncpy(g_profile.timers[idx].name, name, PROFILE_NAME_MAX - 1);
    g_profile.timers[idx].name[PROFILE_NAME_MAX - 1] = '\0';
    return idx;
}

static inline void profile__record(const char *name, double elapsed_ms)
{
    pthread_mutex_lock(&g_profile.lock);
    int idx = profile__slot(name);
    if (idx >= 0) {
        profile_timer_t *t = &g_profile.timers[idx];
        t->last_ms   = elapsed_ms;
        t->total_ms += elapsed_ms;
        t->hits++;
        if (elapsed_ms > t->max_ms)
            t->max_ms = elapsed_ms;
    }
    pthread_mutex_unlock(&g_profile.lock);
}

/* Query a timer by name.  Returns 0 on success, -1 if not found. */
static inline int profile_get(const char *name,
                               double *last_ms,
                               double *avg_ms,
                               double *max_ms)
{
    pthread_mutex_lock(&g_profile.lock);
    for (size_t i = 0; i < g_profile.count; i++) {
        if (strcmp(g_profile.timers[i].name, name) == 0) {
            profile_timer_t *t = &g_profile.timers[i];
            if (last_ms) *last_ms = t->last_ms;
            if (avg_ms)  *avg_ms  = t->hits ? t->total_ms / (double)t->hits : 0.0;
            if (max_ms)  *max_ms  = t->max_ms;
            pthread_mutex_unlock(&g_profile.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_profile.lock);
    return -1;
}

/* Dump all timers to a FILE stream. */
static inline void profile_dump(FILE *fp)
{
    pthread_mutex_lock(&g_profile.lock);
    fprintf(fp, "%-24s  %8s  %8s  %8s  %8s\n",
            "TIMER", "LAST(ms)", "AVG(ms)", "MAX(ms)", "HITS");
    fprintf(fp, "%-24s  %8s  %8s  %8s  %8s\n",
            "------------------------", "--------", "--------", "--------", "--------");
    for (size_t i = 0; i < g_profile.count; i++) {
        profile_timer_t *t = &g_profile.timers[i];
        double avg = t->hits ? t->total_ms / (double)t->hits : 0.0;
        fprintf(fp, "%-24s  %8.2f  %8.2f  %8.2f  %8zu\n",
                t->name, t->last_ms, avg, t->max_ms, t->hits);
    }
    pthread_mutex_unlock(&g_profile.lock);
}

/* ── convenience macros ──────────────────────────────────────── */

/* Returns elapsed time in milliseconds between two timespecs. */
static inline double profile__diff_ms(struct timespec *a, struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) * 1000.0
         + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

#define PROFILE_BEGIN(label)                                     \
    struct timespec _prof_##label##_start;                       \
    clock_gettime(CLOCK_MONOTONIC, &_prof_##label##_start)

#define PROFILE_END(label)                                       \
    do {                                                         \
        struct timespec _prof_##label##_end;                     \
        clock_gettime(CLOCK_MONOTONIC, &_prof_##label##_end);   \
        double _prof_##label##_ms = profile__diff_ms(            \
            &_prof_##label##_start, &_prof_##label##_end);      \
        profile__record(#label, _prof_##label##_ms);             \
    } while (0)

#endif /* ALLMON_PROFILE_H */
