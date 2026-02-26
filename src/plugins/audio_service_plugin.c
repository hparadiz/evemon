/*
 * audio_service_plugin.c – Headless audio service plugin for evemon.
 *
 * This is the first headless (service) plugin.  It owns all album art
 * loading and MPRIS metadata aggregation, publishing results via the
 * event bus so that UI plugins (pipewire_plugin, milkdrop_plugin) can
 * subscribe instead of each maintaining their own art loader.
 *
 * Responsibilities:
 *   - Receive MPRIS metadata via the standard update() callback
 *   - Select the best MPRIS player (smart scoring)
 *   - Load album art asynchronously (via art_loader)
 *   - Publish EVEMON_EVENT_ALBUM_ART_UPDATED with the loaded pixbuf
 *     and metadata to all subscribers
 *
 * This plugin has kind = EVEMON_PLUGIN_HEADLESS, so:
 *   - No create_widget() — no tab in the notebook
 *   - Auto-activated at load time
 *   - Participates in the broker data pipeline (NEED_MPRIS + NEED_PIPEWIRE)
 */

#include "../evemon_plugin.h"
#include "../art_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* Loaded album art */
    GdkPixbuf  *art_pixbuf;
    GCancellable *art_cancel;
} audio_svc_ctx_t;

/* ── Art load callback ───────────────────────────────────────── */

static void on_art_loaded(GdkPixbuf *pixbuf, void *user_data)
{
    audio_svc_ctx_t *ctx = user_data;

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

        if (strcmp(p->playback_status, "Playing") == 0) score += 100;
        else if (strcmp(p->playback_status, "Paused") == 0) score += 50;
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
        art_load_async(best->art_url, on_art_loaded, ctx,
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
        on_art_loaded(NULL, ctx);  /* publish cleared state */
    } else {
        /* Same URL — re-publish with current pixbuf + updated metadata
         * (position/status may have changed even if art hasn't). */
        if (ctx->host && ctx->host->publish) {
            evemon_album_art_payload_t payload;
            memset(&payload, 0, sizeof(payload));
            payload.pixbuf = ctx->art_pixbuf;
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
    svc_clear(opaque);
    free(ctx);
}

/* ── Plugin init ─────────────────────────────────────────────── */

evemon_plugin_t *evemon_plugin_init(void)
{
    audio_svc_ctx_t *ctx = calloc(1, sizeof(audio_svc_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    p->abi_version   = evemon_PLUGIN_ABI_VERSION;
    p->name          = "Audio Service";
    p->id            = "org.evemon.audio_service";
    p->version       = "1.0";
    p->data_needs    = evemon_NEED_MPRIS | evemon_NEED_PIPEWIRE;
    p->plugin_ctx    = ctx;
    p->kind          = EVEMON_PLUGIN_HEADLESS;

    /* Headless plugins have no create_widget */
    p->create_widget = NULL;
    p->update        = svc_update;
    p->clear         = svc_clear;
    p->destroy       = svc_destroy;
    p->activate      = svc_activate;

    return p;
}
