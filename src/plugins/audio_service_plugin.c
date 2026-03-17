/*
 * audio_service_plugin.c – Media Meta service plugin for evemon.
 *
 * Headless service plugin that owns album art loading and MPRIS metadata
 * aggregation, publishing results via the event bus so that UI plugins
 * (pipewire_plugin, milkdrop_plugin) can subscribe instead of each
 * maintaining their own art loader.
 *
 * Responsibilities:
 *   - Receive MPRIS metadata via the standard update() callback
 *   - Select the best MPRIS player (smart scoring)
 *   - Load album art asynchronously (via art_loader)
 *   - Publish EVEMON_EVENT_ALBUM_ART_UPDATED with the loaded pixbuf
 *     and metadata to all subscribers
 *
 * Role: EVEMON_ROLE_SERVICE (headless, auto-activated, no UI tab)
 */

#include "../evemon_plugin.h"
#include "../art_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.audio_service",
    "Media Meta",
    "1.0",
    EVEMON_ROLE_SERVICE,
    NULL
);

/* ── Guard for async art load callbacks ──────────────────────── */

/*
 * Reference-counted guard that outlives the plugin context.
 * Async art_load callbacks hold a pointer to this guard instead
 * of directly to audio_svc_ctx_t.  When svc_destroy() frees the
 * context, it marks the guard as dead.  The callback checks the
 * flag before touching any context fields.
 *
 * Without this, destroying a plugin instance while an async HTTP
 * art fetch is in-flight results in use-after-free (the GIO
 * callback fires after free(ctx) → segfault).  g_cancellable_cancel
 * sets a flag but does NOT prevent already-dispatched callbacks
 * from running.
 */
typedef struct {
    int                   ref_count;   /* manual refcount                  */
    int                   dead;        /* set TRUE when ctx is freed       */
    void                 *ctx;         /* back-pointer (NULL when dead)    */
} art_load_guard_t;

static art_load_guard_t *art_load_guard_new(void *ctx)
{
    art_load_guard_t *g = calloc(1, sizeof(art_load_guard_t));
    if (!g) return NULL;
    g->ref_count = 1;
    g->dead      = 0;
    g->ctx       = ctx;
    return g;
}

static art_load_guard_t *art_load_guard_ref(art_load_guard_t *g)
{
    if (g) g->ref_count++;
    return g;
}

static void art_load_guard_unref(art_load_guard_t *g)
{
    if (!g) return;
    if (--g->ref_count <= 0)
        free(g);
}

/* ── Per-instance context ────────────────────────────────────── */

typedef struct {
    const evemon_host_services_t *host;

    /* Cached MPRIS state to detect changes */
    char        art_url_cached[512];
    char        track_title[256];
    char        track_artist[256];
    char        track_album[256];
    char        playback_status[32];
    char        identity[128];
    int64_t     position_us;
    int64_t     length_us;

    /* Tracked PID (set each update cycle) */
    pid_t       tracked_pid;

    /* PID that was active when the current art load was initiated.
     * Used to discard stale art load completions after PID change. */
    pid_t       art_load_pid;

    /* Loaded album art */
    GdkPixbuf  *art_pixbuf;
    GCancellable *art_cancel;

    /* Guard for async art load callbacks (prevents use-after-free) */
    art_load_guard_t *guard;
} audio_svc_ctx_t;

/* ── Art load callback ───────────────────────────────────────── */

/*
 * Called when async art loading completes.  user_data is an
 * art_load_guard_t* (NOT the context directly) — the guard
 * lets us detect if the plugin was destroyed while the load
 * was in-flight.
 */
static void on_art_loaded(GdkPixbuf *pixbuf, void *user_data)
{
    art_load_guard_t *guard = user_data;

    /* If the plugin instance has been destroyed while we were
     * loading, the context is freed — discard everything. */
    if (!guard || guard->dead) {
        if (pixbuf)
            g_object_unref(pixbuf);
        art_load_guard_unref(guard);
        return;
    }

    audio_svc_ctx_t *ctx = guard->ctx;
    art_load_guard_unref(guard);   /* release the load's ref */

    /* If the tracked PID changed since we kicked off this art load,
     * the result is stale — discard it to prevent publishing art
     * for the wrong process (which can cause segfaults if the old
     * process's plugin instance has already been torn down). */
    if (ctx->art_load_pid != 0 && ctx->art_load_pid != ctx->tracked_pid) {
        if (pixbuf)
            g_object_unref(pixbuf);
        if (ctx->art_cancel) {
            g_object_unref(ctx->art_cancel);
            ctx->art_cancel = NULL;
        }
        return;
    }

    /* Replace cached pixbuf */
    if (ctx->art_pixbuf)
        g_object_unref(ctx->art_pixbuf);
    ctx->art_pixbuf = pixbuf;  /* takes ownership (may be NULL) */

    if (ctx->art_cancel) {
        g_object_unref(ctx->art_cancel);
        ctx->art_cancel = NULL;
    }

    /* Publish the art update event */
    if (ctx->host && ctx->host->publish) {
        evemon_album_art_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        payload.pixbuf = ctx->art_pixbuf;  /* subscribers must ref if keeping */
        payload.source_pid = ctx->tracked_pid;
        snprintf(payload.art_url, sizeof(payload.art_url),
                 "%s", ctx->art_url_cached);
        snprintf(payload.track_title, sizeof(payload.track_title),
                 "%s", ctx->track_title);
        snprintf(payload.track_artist, sizeof(payload.track_artist),
                 "%s", ctx->track_artist);
        snprintf(payload.track_album, sizeof(payload.track_album),
                 "%s", ctx->track_album);
        snprintf(payload.playback_status, sizeof(payload.playback_status),
                 "%s", ctx->playback_status);
        snprintf(payload.identity, sizeof(payload.identity),
                 "%s", ctx->identity);
        payload.position_us = ctx->position_us;
        payload.length_us   = ctx->length_us;

        evemon_event_t event = {
            .type    = EVEMON_EVENT_ALBUM_ART_UPDATED,
            .payload = &payload
        };
        ctx->host->publish(ctx->host->host_ctx, &event);
    }
}

/* ── MPRIS player scoring (shared logic) ─────────────────────── */

/*
 * Smart player selection for multi-stream apps (Firefox, etc.).
 *
 * Scoring (highest total wins):
 *   +100 : PlaybackStatus == "Playing"
 *    +50 : PlaybackStatus == "Paused"
 *    +30 : MPRIS track_title matches a PipeWire media_name
 *           from one of our monitored audio nodes
 *    +20 : has a non-empty track_title
 *    +10 : has album art
 */
static const evemon_mpris_player_t *
select_best_player(const evemon_proc_data_t *data)
{
    if (!data->mpris_players || data->mpris_player_count == 0)
        return NULL;

    int best_score = -1;
    const evemon_mpris_player_t *best = NULL;

    for (size_t i = 0; i < data->mpris_player_count; i++) {
        const evemon_mpris_player_t *p = &data->mpris_players[i];
        int score = 0;

        /*
         * Strongly prefer players that are actually playing.
         * A "Playing" player should always win over "Paused" or
         * "Stopped", since with multiple audio processes the
         * user cares about the one producing sound right now.
         *
         * Use position_us > 0 as evidence the player has actually
         * started playback (not just declared "Playing" with no
         * content loaded).
         */
        if (strcmp(p->playback_status, "Playing") == 0) {
            score += 200;
            if (p->position_us > 0) score += 50;
        } else if (strcmp(p->playback_status, "Paused") == 0) {
            score += 50;
            if (p->position_us > 0) score += 10;
        }
        /* "Stopped" players get no playback bonus */

        if (p->track_title[0]) score += 20;
        if (p->art_url[0]) score += 10;

        /* Correlate with PipeWire streams */
        if (p->track_title[0] && data->pw_nodes) {
            for (size_t j = 0; j < data->pw_node_count; j++) {
                const evemon_pw_node_t *nd = &data->pw_nodes[j];
                if (nd->pid != data->pid) continue;
                if (!nd->media_name[0]) continue;
                if (strstr(nd->media_name, p->track_title) ||
                    strstr(p->track_title, nd->media_name)) {
                    score += 30;
                    break;
                }
            }
        }

        if (score > best_score) {
            best_score = score;
            best = p;
        }
    }

    return best ? best : &data->mpris_players[0];
}

/* ── Plugin callbacks ────────────────────────────────────────── */

static void svc_activate(void *opaque,
                         const evemon_host_services_t *services)
{
    audio_svc_ctx_t *ctx = opaque;
    ctx->host = services;
}

static void svc_update(void *opaque, const evemon_proc_data_t *data)
{
    audio_svc_ctx_t *ctx = opaque;
    ctx->tracked_pid = data->pid;

    const evemon_mpris_player_t *best = select_best_player(data);
    if (!best) {
        /* No MPRIS data — if we had art, publish a "cleared" event */
        if (ctx->art_url_cached[0] || ctx->art_pixbuf) {
            if (ctx->art_cancel) {
                g_cancellable_cancel(ctx->art_cancel);
                g_object_unref(ctx->art_cancel);
                ctx->art_cancel = NULL;
            }
            if (ctx->art_pixbuf) {
                g_object_unref(ctx->art_pixbuf);
                ctx->art_pixbuf = NULL;
            }
            ctx->art_url_cached[0] = '\0';
            ctx->track_title[0] = '\0';
            ctx->track_artist[0] = '\0';
            ctx->track_album[0] = '\0';
            ctx->playback_status[0] = '\0';
            ctx->identity[0] = '\0';
            ctx->position_us = 0;
            ctx->length_us = 0;

            if (ctx->host && ctx->host->publish) {
                evemon_album_art_payload_t payload;
                memset(&payload, 0, sizeof(payload));
                payload.source_pid = ctx->tracked_pid;
                evemon_event_t event = {
                    .type    = EVEMON_EVENT_ALBUM_ART_UPDATED,
                    .payload = &payload
                };
                ctx->host->publish(ctx->host->host_ctx, &event);
            }
        }
        return;
    }

    /* Cache metadata for publish */
    snprintf(ctx->track_title, sizeof(ctx->track_title),
             "%s", best->track_title);
    snprintf(ctx->track_artist, sizeof(ctx->track_artist),
             "%s", best->track_artist);
    snprintf(ctx->track_album, sizeof(ctx->track_album),
             "%s", best->track_album);
    snprintf(ctx->playback_status, sizeof(ctx->playback_status),
             "%s", best->playback_status);
    snprintf(ctx->identity, sizeof(ctx->identity),
             "%s", best->identity);
    ctx->position_us = best->position_us;
    ctx->length_us   = best->length_us;

    /* Album art — load only when URL changes */
    if (best->art_url[0] &&
        strcmp(best->art_url, ctx->art_url_cached) != 0) {
        /* New URL — cancel any in-flight load */
        if (ctx->art_cancel) {
            g_cancellable_cancel(ctx->art_cancel);
            g_object_unref(ctx->art_cancel);
            ctx->art_cancel = NULL;
        }
        snprintf(ctx->art_url_cached, sizeof(ctx->art_url_cached),
                 "%s", best->art_url);
        ctx->art_load_pid = ctx->tracked_pid;
        art_load_async(best->art_url, on_art_loaded,
                       art_load_guard_ref(ctx->guard),
                       &ctx->art_cancel);
    } else if (!best->art_url[0] && ctx->art_url_cached[0]) {
        /* Art URL cleared */
        if (ctx->art_cancel) {
            g_cancellable_cancel(ctx->art_cancel);
            g_object_unref(ctx->art_cancel);
            ctx->art_cancel = NULL;
        }
        if (ctx->art_pixbuf) {
            g_object_unref(ctx->art_pixbuf);
            ctx->art_pixbuf = NULL;
        }
        ctx->art_url_cached[0] = '\0';
        on_art_loaded(NULL, art_load_guard_ref(ctx->guard));  /* publish cleared state */
    } else {
        /* Same URL — re-publish with current pixbuf + updated metadata
         * (position/status may have changed even if art hasn't). */
        if (ctx->host && ctx->host->publish) {
            evemon_album_art_payload_t payload;
            memset(&payload, 0, sizeof(payload));
            payload.pixbuf = ctx->art_pixbuf;
            payload.source_pid = ctx->tracked_pid;
            snprintf(payload.art_url, sizeof(payload.art_url),
                     "%s", ctx->art_url_cached);
            snprintf(payload.track_title, sizeof(payload.track_title),
                     "%s", ctx->track_title);
            snprintf(payload.track_artist, sizeof(payload.track_artist),
                     "%s", ctx->track_artist);
            snprintf(payload.track_album, sizeof(payload.track_album),
                     "%s", ctx->track_album);
            snprintf(payload.playback_status, sizeof(payload.playback_status),
                     "%s", ctx->playback_status);
            snprintf(payload.identity, sizeof(payload.identity),
                     "%s", ctx->identity);
            payload.position_us = ctx->position_us;
            payload.length_us   = ctx->length_us;

            evemon_event_t event = {
                .type    = EVEMON_EVENT_ALBUM_ART_UPDATED,
                .payload = &payload
            };
            ctx->host->publish(ctx->host->host_ctx, &event);
        }
    }
}

static void svc_clear(void *opaque)
{
    audio_svc_ctx_t *ctx = opaque;

    if (ctx->art_cancel) {
        g_cancellable_cancel(ctx->art_cancel);
        g_object_unref(ctx->art_cancel);
        ctx->art_cancel = NULL;
    }
    if (ctx->art_pixbuf) {
        g_object_unref(ctx->art_pixbuf);
        ctx->art_pixbuf = NULL;
    }
    ctx->art_url_cached[0] = '\0';
    ctx->track_title[0] = '\0';
    ctx->track_artist[0] = '\0';
    ctx->track_album[0] = '\0';
    ctx->playback_status[0] = '\0';
    ctx->identity[0] = '\0';
    ctx->position_us = 0;
    ctx->length_us = 0;
}

static void svc_destroy(void *opaque)
{
    audio_svc_ctx_t *ctx = opaque;

    /*
     * Mark the guard dead FIRST — before cancelling or freeing anything.
     *
     * GLib async callbacks (GIO, libsoup) can already be queued in the
     * main-loop at the point we call g_cancellable_cancel().  Cancellation
     * sets a flag but does NOT dequeue already-dispatched idle sources, so
     * on_art_loaded() can fire after we return from svc_clear().
     *
     * By marking dead=1 before svc_clear(), any callback that was queued
     * before this point will see guard->dead == 1, skip accessing ctx,
     * and simply unref the guard (safe — guard is ref-counted and not
     * freed until the last reference drops).
     */
    if (ctx->guard) {
        ctx->guard->dead = 1;
        ctx->guard->ctx  = NULL;
    }

    /* Now safe to cancel in-flight loads and free GObject refs */
    svc_clear(opaque);

    /* Release the plugin's own guard ref.  If an async callback still
     * holds a ref (refcount > 1 before this unref), the guard stays
     * alive until that callback runs and unrefs it — no use-after-free. */
    if (ctx->guard) {
        art_load_guard_unref(ctx->guard);
        ctx->guard = NULL;
    }

    free(ctx);
}

/* ── Plugin init ─────────────────────────────────────────────── */

evemon_plugin_t *evemon_plugin_init(void)
{
    audio_svc_ctx_t *ctx = calloc(1, sizeof(audio_svc_ctx_t));
    if (!ctx) return NULL;

    ctx->guard = art_load_guard_new(ctx);
    if (!ctx->guard) { free(ctx); return NULL; }

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { art_load_guard_unref(ctx->guard); free(ctx); return NULL; }

    p->abi_version   = evemon_PLUGIN_ABI_VERSION;
    p->name          = "Media Meta";
    p->id            = "org.evemon.audio_service";
    p->version       = "1.0";
    p->data_needs    = evemon_NEED_MPRIS | evemon_NEED_PIPEWIRE;
    p->plugin_ctx    = ctx;
    p->role          = EVEMON_ROLE_SERVICE;
    p->dependencies  = NULL;

    /* Headless plugins have no create_widget */
    p->create_widget = NULL;
    p->update        = svc_update;
    p->clear         = svc_clear;
    p->destroy       = svc_destroy;
    p->activate      = svc_activate;

    return p;
}
