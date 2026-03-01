/*
 * event_bus.c – Lightweight publish/subscribe event bus for evemon.
 *
 * Implementation:
 *   - Static array of subscribers (no heap churn for subscribe/unsub).
 *   - Linear scan on publish (simple, cache-friendly, correct).
 *   - Main-thread dispatch via g_idle_add when published off-thread.
 *   - No DBus, no GLib signals, no string-based topic routing.
 *
 * Capacity: up to 128 concurrent subscriptions.  This is more than
 * enough for the current plugin set and leaves headroom for growth.
 */

#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Subscription table ──────────────────────────────────────── */

#define MAX_SUBSCRIPTIONS 128

typedef struct {
    int                   id;        /* subscription ID (>0), 0 = free slot */
    evemon_event_type_t   type;
    evemon_event_cb       cb;
    void                 *user_data;
} subscription_t;

static subscription_t g_subs[MAX_SUBSCRIPTIONS];
static int            g_next_sub_id = 1;
static pthread_mutex_t g_bus_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Init / Destroy ──────────────────────────────────────────── */

void evemon_event_bus_init(void)
{
    pthread_mutex_lock(&g_bus_lock);
    memset(g_subs, 0, sizeof(g_subs));
    g_next_sub_id = 1;
    pthread_mutex_unlock(&g_bus_lock);
}

void evemon_event_bus_destroy(void)
{
    pthread_mutex_lock(&g_bus_lock);
    memset(g_subs, 0, sizeof(g_subs));
    pthread_mutex_unlock(&g_bus_lock);
}

/* ── Subscribe / Unsubscribe ─────────────────────────────────── */

int evemon_event_bus_subscribe(evemon_event_type_t type,
                               evemon_event_cb cb,
                               void *user_data)
{
    if (!cb) return 0;

    pthread_mutex_lock(&g_bus_lock);

    int id = 0;
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        if (g_subs[i].id == 0) {
            g_subs[i].id        = g_next_sub_id++;
            g_subs[i].type      = type;
            g_subs[i].cb        = cb;
            g_subs[i].user_data = user_data;
            id = g_subs[i].id;
            break;
        }
    }

    pthread_mutex_unlock(&g_bus_lock);

    if (id == 0)
        fprintf(stderr, "evemon: event bus full, cannot subscribe\n");

    return id;
}

void evemon_event_bus_unsubscribe(int subscription_id)
{
    if (subscription_id <= 0) return;

    pthread_mutex_lock(&g_bus_lock);
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        if (g_subs[i].id == subscription_id) {
            memset(&g_subs[i], 0, sizeof(g_subs[i]));
            break;
        }
    }
    pthread_mutex_unlock(&g_bus_lock);
}

/* ── Publish ─────────────────────────────────────────────────── */

/*
 * Context for deferred (idle) dispatch when publish is called
 * from a non-main thread.
 */
typedef struct {
    evemon_event_t  event;
    /* Deep-copied payload for ALBUM_ART_UPDATED */
    void           *owned_payload;
} idle_dispatch_t;

static void idle_dispatch_free(idle_dispatch_t *d)
{
    if (d->owned_payload) {
        if (d->event.type == EVEMON_EVENT_ALBUM_ART_UPDATED) {
            evemon_album_art_payload_t *art = d->owned_payload;
            if (art->pixbuf)
                g_object_unref(art->pixbuf);
            free(art);
        } else if (d->event.type == EVEMON_EVENT_FD_WRITE) {
            /* owned_payload is evemon_fd_write_payload_t */
            free(d->owned_payload);
        } else if (d->event.type == EVEMON_EVENT_CHILD_EXEC) {
            free(d->owned_payload);
        } else if (d->event.type == EVEMON_EVENT_JSON_SNAPSHOT) {
            evemon_json_payload_t *jp = d->owned_payload;
            free((void *)jp->json);
            free(jp);
        } else {
            free(d->owned_payload);
        }
    }
    free(d);
}

static gboolean dispatch_on_main(gpointer data)
{
    idle_dispatch_t *d = data;

    /* Take a snapshot of matching subscribers under the lock,
     * then invoke them outside the lock to avoid deadlocks. */
    subscription_t matches[MAX_SUBSCRIPTIONS];
    int nmatches = 0;

    pthread_mutex_lock(&g_bus_lock);
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        if (g_subs[i].id != 0 && g_subs[i].type == d->event.type) {
            matches[nmatches++] = g_subs[i];
        }
    }
    pthread_mutex_unlock(&g_bus_lock);

    for (int i = 0; i < nmatches; i++) {
        matches[i].cb(&d->event, matches[i].user_data);
    }

    idle_dispatch_free(d);
    return G_SOURCE_REMOVE;
}

/*
 * Deep-copy the event payload onto the heap for deferred dispatch.
 */
static idle_dispatch_t *event_to_idle(const evemon_event_t *event)
{
    idle_dispatch_t *d = calloc(1, sizeof(idle_dispatch_t));
    if (!d) return NULL;

    d->event = *event;
    d->owned_payload = NULL;

    /* For known payload types, deep-copy so the caller's stack is safe */
    if (event->type == EVEMON_EVENT_ALBUM_ART_UPDATED && event->payload) {
        const evemon_album_art_payload_t *src = event->payload;
        evemon_album_art_payload_t *dst = calloc(1, sizeof(*dst));
        if (dst) {
            *dst = *src;
            if (dst->pixbuf)
                g_object_ref(dst->pixbuf);
            d->event.payload = dst;
            d->owned_payload = dst;
        }
    }
    /* FD write events: deep-copy fixed-size payload */
    else if (event->type == EVEMON_EVENT_FD_WRITE && event->payload) {
        const evemon_fd_write_payload_t *src = event->payload;
        evemon_fd_write_payload_t *dst = calloc(1, sizeof(*dst));
        if (dst) {
            *dst = *src;
            d->event.payload = dst;
            d->owned_payload = dst;
        }
    }
    /* CHILD_EXEC: deep-copy fixed-size payload */
    else if (event->type == EVEMON_EVENT_CHILD_EXEC && event->payload) {
        const evemon_exec_payload_t *src = event->payload;
        evemon_exec_payload_t *dst = malloc(sizeof(*dst));
        if (dst) {
            *dst = *src;
            d->event.payload = dst;
            d->owned_payload = dst;
        }
    }
    /* PROCESS_SELECTED: payload is a pid_t* — copy the value */
    else if (event->type == EVEMON_EVENT_PROCESS_SELECTED && event->payload) {
        pid_t *src = event->payload;
        pid_t *dst = malloc(sizeof(pid_t));
        if (dst) {
            *dst = *src;
            d->event.payload = dst;
            d->owned_payload = dst;
        }
    }
    /* JSON_SNAPSHOT: deep-copy the JSON string */
    else if (event->type == EVEMON_EVENT_JSON_SNAPSHOT && event->payload) {
        const evemon_json_payload_t *src = event->payload;
        evemon_json_payload_t *dst = calloc(1, sizeof(*dst));
        if (dst) {
            dst->source_pid = src->source_pid;
            dst->len        = src->len;
            char *jcopy     = malloc(src->len + 1);
            if (jcopy) {
                memcpy(jcopy, src->json, src->len + 1);
                dst->json = jcopy;
            }
            d->event.payload = dst;
            d->owned_payload = dst;
        }
    }

    return d;
}

void evemon_event_bus_publish(const evemon_event_t *event)
{
    if (!event) return;

    /*
     * Always marshal to idle for consistency and thread safety.
     * Even from the main thread, using g_idle_add ensures we don't
     * invoke subscribers while the publisher is in the middle of
     * modifying shared state.
     */
    idle_dispatch_t *d = event_to_idle(event);
    if (d)
        g_idle_add(dispatch_on_main, d);
}

/* ── Host-side wrappers for evemon_host_services_t ───────────── */

int host_event_subscribe(void *host_ctx,
                         evemon_event_type_t type,
                         evemon_event_cb cb,
                         void *user_data)
{
    (void)host_ctx;
    return evemon_event_bus_subscribe(type, cb, user_data);
}

void host_event_publish(void *host_ctx,
                        const evemon_event_t *event)
{
    (void)host_ctx;
    evemon_event_bus_publish(event);
}

void host_event_unsubscribe(void *host_ctx, int subscription_id)
{
    (void)host_ctx;
    evemon_event_bus_unsubscribe(subscription_id);
}
