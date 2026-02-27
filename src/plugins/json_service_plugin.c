/*
 * json_service_plugin.c – Headless JSON relay service for evemon.
 *
 * Provides the EVEMON_EVENT_JSON_SNAPSHOT event bus channel.
 * This plugin does NOT serialise data itself — that is the
 * responsibility of each individual plugin that wants JSON output.
 *
 * Plugins produce JSON strings and publish them via the host
 * event bus using EVEMON_EVENT_JSON_SNAPSHOT.  This service
 * plugin simply establishes the channel and provides jansson
 * as the shared JSON library dependency.
 *
 * Uses jansson (libjansson) — a small, well-tested, widely-packaged
 * C JSON library.  Plugins that link against this .so get jansson
 * symbols for free.
 *
 *   Arch:   pacman -S jansson
 *   Debian: apt install libjansson-dev
 *   Fedora: dnf install jansson-devel
 *
 * This plugin has kind = EVEMON_PLUGIN_HEADLESS:
 *   - No create_widget() — no tab in the notebook
 *   - Auto-activated at load time
 *   - Requests NO data needs — purely a relay service
 */

#include "../evemon_plugin.h"

#include <jansson.h>
#include <stdlib.h>

/* ── Per-instance context ────────────────────────────────────── */

typedef struct {
    const evemon_host_services_t *host;
} json_svc_ctx_t;

/* ── Plugin callbacks ────────────────────────────────────────── */

static void json_activate(void *opaque,
                           const evemon_host_services_t *services)
{
    json_svc_ctx_t *ctx = opaque;
    ctx->host = services;
}

static void json_update(void *opaque, const evemon_proc_data_t *data)
{
    (void)opaque;
    (void)data;
    /* Nothing to do — individual plugins publish their own JSON. */
}

static void json_clear(void *opaque)
{
    (void)opaque;
}

static void json_destroy(void *opaque)
{
    free(opaque);
}

/* ── Plugin init ─────────────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    json_svc_ctx_t *ctx = calloc(1, sizeof(json_svc_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    p->abi_version   = evemon_PLUGIN_ABI_VERSION;
    p->name          = "JSON Service";
    p->id            = "org.evemon.json_service";
    p->version       = "1.0";
    p->kind          = EVEMON_PLUGIN_HEADLESS;

    /*
     * No data needs — we are purely a relay.  Individual plugins
     * serialise their own data and publish JSON snapshots via the
     * event bus.
     */
    p->data_needs    = 0;

    p->plugin_ctx    = ctx;
    p->create_widget = NULL;
    p->update        = json_update;
    p->clear         = json_clear;
    p->destroy       = json_destroy;
    p->activate      = json_activate;

    return p;
}
