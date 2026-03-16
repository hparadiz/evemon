/*
 * pipewire_scan.c – PipeWire audio connection scanner for the detail panel.
 *
 * Connects to the PipeWire daemon via libpipewire, enumerates the
 * object graph (nodes, ports, links), and for a given PID shows:
 *   - Which audio streams the process owns (output / input)
 *   - What sink or source device each stream is connected to
 *
 * The scan runs in a GTask worker thread (matching the fd_scan /
 * env_scan / mmap_scan pattern) so it never blocks the GTK main loop.
 *
 * Gated behind HAVE_PIPEWIRE — if not defined, pipewire_scan_start()
 * is a no-op stub (see ui_internal.h).
 *
 * Graph enumeration model
 * -----------------------
 * A persistent background thread (pw_watcher) keeps a live PipeWire
 * connection and maintains a continuously-updated pw_graph_t under a
 * mutex.  pw_snapshot() merely locks the mutex and copies the live
 * graph instead of doing a full connect-enumerate-disconnect cycle on
 * every broker poll.  This reduces first-display latency from
 * ~200-500 ms (two IPC roundtrips per cycle) to <1 ms.
 */

#ifdef HAVE_PIPEWIRE

#include "ui_internal.h"
#include "pipewire_graph.h"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static void pw_graph_init(pw_graph_t *g)
{
    memset(g, 0, sizeof(*g));
}

void pw_graph_free(pw_graph_t *g)
{
    free(g->nodes);
    free(g->ports);
    free(g->links);
    free(g->clients);
    memset(g, 0, sizeof(*g));
}

static pw_snap_node_t *pw_graph_push_node(pw_graph_t *g)
{
    if (g->node_count >= g->node_cap) {
        size_t nc = g->node_cap ? g->node_cap * 2 : 128;
        pw_snap_node_t *tmp = realloc(g->nodes, nc * sizeof(*tmp));
        if (!tmp) return NULL;
        g->nodes    = tmp;
        g->node_cap = nc;
    }
    pw_snap_node_t *n = &g->nodes[g->node_count++];
    memset(n, 0, sizeof(*n));
    return n;
}

static pw_snap_port_t *pw_graph_push_port(pw_graph_t *g)
{
    if (g->port_count >= g->port_cap) {
        size_t nc = g->port_cap ? g->port_cap * 2 : 256;
        pw_snap_port_t *tmp = realloc(g->ports, nc * sizeof(*tmp));
        if (!tmp) return NULL;
        g->ports    = tmp;
        g->port_cap = nc;
    }
    pw_snap_port_t *p = &g->ports[g->port_count++];
    memset(p, 0, sizeof(*p));
    return p;
}

static pw_snap_link_t *pw_graph_push_link(pw_graph_t *g)
{
    if (g->link_count >= g->link_cap) {
        size_t nc = g->link_cap ? g->link_cap * 2 : 128;
        pw_snap_link_t *tmp = realloc(g->links, nc * sizeof(*tmp));
        if (!tmp) return NULL;
        g->links    = tmp;
        g->link_cap = nc;
    }
    pw_snap_link_t *l = &g->links[g->link_count++];
    memset(l, 0, sizeof(*l));
    return l;
}

static pw_snap_client_t *pw_graph_push_client(pw_graph_t *g)
{
    if (g->client_count >= g->client_cap) {
        size_t nc = g->client_cap ? g->client_cap * 2 : 64;
        pw_snap_client_t *tmp = realloc(g->clients, nc * sizeof(*tmp));
        if (!tmp) return NULL;
        g->clients    = tmp;
        g->client_cap = nc;
    }
    pw_snap_client_t *c = &g->clients[g->client_count++];
    memset(c, 0, sizeof(*c));
    return c;
}

static const pw_snap_node_t *pw_graph_find_node(const pw_graph_t *g,
                                                 uint32_t id)
{
    for (size_t i = 0; i < g->node_count; i++)
        if (g->nodes[i].id == id) return &g->nodes[i];
    return NULL;
}

/* ── dict helper ─────────────────────────────────────────────── */

static const char *dict_get(const struct spa_dict *d, const char *key)
{
    if (!d) return NULL;
    return spa_dict_lookup(d, key);
}

/*
 * Ensure a NUL-terminated buffer doesn't end with a truncated
 * multi-byte UTF-8 sequence (as can happen after snprintf).
 */
static void utf8_safe_truncate(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return;
    buf[bufsz - 1] = '\0';
    size_t len = strlen(buf);
    if (len == 0) return;

    /* Walk backwards past continuation bytes (10xxxxxx) */
    size_t pos = len;
    while (pos > 0 && ((unsigned char)buf[pos - 1] & 0xC0) == 0x80)
        pos--;

    if (pos == 0) { buf[0] = '\0'; return; }

    unsigned char lead = (unsigned char)buf[pos - 1];
    int expected;
    if (lead < 0x80)      expected = 1;
    else if (lead < 0xC0) { buf[pos - 1] = '\0'; return; }
    else if (lead < 0xE0) expected = 2;
    else if (lead < 0xF0) expected = 3;
    else                  expected = 4;

    size_t actual = len - (pos - 1);
    if (actual < (size_t)expected)
        buf[pos - 1] = '\0';   /* incomplete sequence — remove it */
}

static void dict_copy(const struct spa_dict *d, const char *key,
                      char *buf, size_t bufsz)
{
    const char *v = dict_get(d, key);
    if (v) {
        snprintf(buf, bufsz, "%s", v);
        utf8_safe_truncate(buf, bufsz);
    } else {
        buf[0] = '\0';
    }
}

/*
 * Safe integer parsers for untrusted PipeWire property strings (FIND-3).
 * atoi() silently overflows and has no error detection; these helpers
 * use strtol/strtoul with range validation and return 0 on any error.
 */
static pid_t parse_pid(const char *s)
{
    if (!s || !*s) return 0;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v <= 0 || v > (long)INT_MAX) return 0;
    return (pid_t)v;
}

static uint32_t parse_u32(const char *s)
{
    if (!s || !*s) return 0;
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v > (unsigned long)UINT32_MAX) return 0;
    return (uint32_t)v;
}

/* ── Persistent PipeWire watcher ────────────────────────────── */

/*
 * The watcher runs in its own pthread with its own pw_main_loop.
 * It keeps a single PipeWire connection alive and updates g_live_graph
 * in response to registry add/remove events.
 *
 * pw_snapshot() locks g_watcher_mutex, deep-copies g_live_graph,
 * and returns immediately — no IPC roundtrip needed.
 *
 * Lifecycle:
 *   pw_watcher_start()  — called once at startup (from ui_gtk.c)
 *   pw_watcher_stop()   — called at shutdown
 *   pw_snapshot()       — fast copy under mutex
 */

static pthread_mutex_t  g_watcher_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pw_graph_t       g_live_graph;           /* protected by g_watcher_mutex */
static int              g_live_ready     = 0;   /* set after first full enumeration */
static pthread_t        g_watcher_tid    = 0;
static int              g_watcher_stop   = 0;

/* Watcher-internal connection state (accessed only on watcher thread) */

/*
 * Each bound watcher node is individually heap-allocated so that
 * realloc of the pointer array never invalidates the embedded spa_hook
 * that PipeWire's listener list holds a pointer to, and so that
 * user_data passed to pw_proxy_add_object_listener is stable.
 */
typedef struct pw_watcher_t pw_watcher_t;
typedef struct {
    struct pw_proxy *proxy;
    struct spa_hook  listener;
    size_t           node_index;  /* index into watcher->graph.nodes[] */
    uint32_t         pw_id;       /* PipeWire object ID (stable, for removal) */
    pw_watcher_t    *owner;       /* back-pointer to the watcher */
} pw_watcher_bound_t;

struct pw_watcher_t {
    struct pw_main_loop *loop;
    struct pw_context   *context;
    struct pw_core      *core;
    struct pw_registry  *registry;
    struct spa_hook      registry_listener;
    struct spa_hook      core_listener;
    int                  sync_seq;
    int                  phase;          /* 0 = initial enum, 1 = post-info */
    int                  initial_done;   /* 1 after first full sync */

    /* Live graph built on the watcher thread, swapped into g_live_graph
     * atomically under g_watcher_mutex once the initial sync is complete.
     * Subsequent add/remove events update g_live_graph directly. */
    pw_graph_t           graph;          /* work-in-progress during phase 0/1 */

    /* Bound-node proxies for receiving pw_node_info.
     * Array of pointers — each entry is individually heap-allocated
     * so that realloc never moves the spa_hook or user_data pointer. */
    pw_watcher_bound_t **bound;
    size_t               bound_count;
    size_t               bound_cap;

    /* When a new node arrives after initial_done, we issue a sync and
     * defer watcher_publish() until the sync completes.  This mirrors
     * the startup two-phase approach and guarantees that node_info
     * (with media_class / pid) has been delivered before we publish. */
    int                  pending_sync;  /* seq of outstanding post-init sync, or 0 */
};

/* Forward declarations */
static void watcher_on_global(void *data, uint32_t id,
                               uint32_t permissions, const char *type,
                               uint32_t version,
                               const struct spa_dict *props);
static void watcher_on_global_remove(void *data, uint32_t id);
static void watcher_on_core_done(void *data, uint32_t id, int seq);
static void watcher_on_core_error(void *data, uint32_t id, int seq,
                                   int res, const char *message);
static void watcher_on_node_info(void *data,
                                  const struct pw_node_info *info);

static const struct pw_registry_events g_watcher_reg_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global        = watcher_on_global,
    .global_remove = watcher_on_global_remove,
};
static const struct pw_core_events g_watcher_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done  = watcher_on_core_done,
    .error = watcher_on_core_error,
};
static const struct pw_node_events g_watcher_node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = watcher_on_node_info,
};

/* Deep-copy src into dst.  Returns 0 on success, -1 if any allocation
 * fails (dst is left zeroed on failure — no partial state).  dst must
 * be zeroed or freed before calling.  FIND-4: atomic commit pattern so
 * callers can detect and handle failure rather than silently getting an
 * incomplete graph. */
static int pw_graph_copy(pw_graph_t *dst, const pw_graph_t *src)
{
    pw_graph_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    if (src->node_count) {
        tmp.nodes = malloc(src->node_count * sizeof(*tmp.nodes));
        if (!tmp.nodes) goto fail;
        memcpy(tmp.nodes, src->nodes, src->node_count * sizeof(*tmp.nodes));
        tmp.node_count = src->node_count;
        tmp.node_cap   = src->node_count;
    }
    if (src->port_count) {
        tmp.ports = malloc(src->port_count * sizeof(*tmp.ports));
        if (!tmp.ports) goto fail;
        memcpy(tmp.ports, src->ports, src->port_count * sizeof(*tmp.ports));
        tmp.port_count = src->port_count;
        tmp.port_cap   = src->port_count;
    }
    if (src->link_count) {
        tmp.links = malloc(src->link_count * sizeof(*tmp.links));
        if (!tmp.links) goto fail;
        memcpy(tmp.links, src->links, src->link_count * sizeof(*tmp.links));
        tmp.link_count = src->link_count;
        tmp.link_cap   = src->link_count;
    }
    if (src->client_count) {
        tmp.clients = malloc(src->client_count * sizeof(*tmp.clients));
        if (!tmp.clients) goto fail;
        memcpy(tmp.clients, src->clients,
               src->client_count * sizeof(*tmp.clients));
        tmp.client_count = src->client_count;
        tmp.client_cap   = src->client_count;
    }

    *dst = tmp;
    return 0;

fail:
    pw_graph_free(&tmp);
    memset(dst, 0, sizeof(*dst));
    return -1;
}

/* Resolve node PIDs from client objects (in-place on graph). */
static void pw_graph_resolve_pids(pw_graph_t *g)
{
    for (size_t i = 0; i < g->node_count; i++) {
        pw_snap_node_t *n = &g->nodes[i];
        if (n->pid != 0 || n->client_id == 0) continue;
        for (size_t j = 0; j < g->client_count; j++) {
            if (g->clients[j].id == n->client_id) {
                n->pid = g->clients[j].pid;
                break;
            }
        }
    }
}

/* Publish the watcher's work graph into g_live_graph under the mutex. */
static void watcher_publish(pw_watcher_t *w)
{
    pw_graph_resolve_pids(&w->graph);

    pw_graph_t copy;
    if (pw_graph_copy(&copy, &w->graph) != 0) {
        evemon_log(LOG_ERROR, "[pw_watcher] pw_graph_copy failed (OOM) "
                   "— skipping publish");
        return;
    }

    pthread_mutex_lock(&g_watcher_mutex);
    pw_graph_free(&g_live_graph);
    g_live_graph = copy;
    g_live_ready = 1;
    pthread_mutex_unlock(&g_watcher_mutex);
}

static void watcher_on_node_info(void *data,
                                  const struct pw_node_info *info)
{
    if (!data || !info) return;

    /* user_data is the stable pw_watcher_bound_t*, not a raw node pointer */
    pw_watcher_bound_t *wb = data;
    if (!wb->owner || wb->node_index >= wb->owner->graph.node_count)
        return;
    pw_snap_node_t *n = &wb->owner->graph.nodes[wb->node_index];

    /*
     * Extract properties whenever info->props is non-NULL, regardless of
     * change_mask.  The old guard `!(change_mask & PW_NODE_CHANGE_MASK_PROPS)`
     * caused new runtime nodes to be permanently missed: PipeWire sometimes
     * delivers the first node_info for a newly-bound proxy without setting
     * PW_NODE_CHANGE_MASK_PROPS even though info->props carries the full
     * property set (media.class, application.process.id, etc.).
     * DICT_COPY_IF_PRESENT only overwrites fields with non-empty values so
     * it is always safe to call — it never clobbers good data with empty strings.
     */
    if (info->props) {
        const struct spa_dict *props = info->props;

        const char *pid_str = dict_get(props, PW_KEY_APP_PROCESS_ID);
        if (!pid_str) pid_str = dict_get(props, "pipewire.sec.pid");
        if (pid_str && n->pid == 0) n->pid = parse_pid(pid_str);

#define DICT_COPY_IF_PRESENT(key, field) \
        do { \
            const char *_v = dict_get(props, key); \
            if (_v && _v[0]) { \
                snprintf(n->field, sizeof(n->field), "%s", _v); \
                utf8_safe_truncate(n->field, sizeof(n->field)); \
            } \
        } while (0)

        DICT_COPY_IF_PRESENT(PW_KEY_APP_NAME,         app_name);
        DICT_COPY_IF_PRESENT(PW_KEY_NODE_NAME,        node_name);
        DICT_COPY_IF_PRESENT(PW_KEY_NODE_DESCRIPTION, node_desc);
        DICT_COPY_IF_PRESENT(PW_KEY_MEDIA_CLASS,      media_class);
        DICT_COPY_IF_PRESENT(PW_KEY_MEDIA_NAME,       media_name);

#undef DICT_COPY_IF_PRESENT
    }

    evemon_log(LOG_AUDIO,
               "[pw_watcher] node_info: id=%u pid=%d app_name='%s' "
               "node_name='%s' node_desc='%s' media_class='%s' media_name='%s'",
               n->id, (int)n->pid,
               n->app_name, n->node_name, n->node_desc,
               n->media_class, n->media_name);

    /*
     * Don't publish here — the pending_sync path in watcher_on_core_done
     * will publish once the sync round-trip completes, guaranteeing that
     * ALL node_info callbacks for this batch have been delivered first.
     * (watcher_on_global issues pw_core_sync when it binds a new node.)
     */
}

static void watcher_on_global(void *data, uint32_t id,
                               uint32_t permissions, const char *type,
                               uint32_t version,
                               const struct spa_dict *props)
{
    (void)permissions;
    pw_watcher_t *w = data;
    if (!type) return;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        pw_snap_node_t *n = pw_graph_push_node(&w->graph);
        if (!n) return;
        n->id = id;
        if (props) {
            const char *pid_str = dict_get(props, PW_KEY_APP_PROCESS_ID);
            if (!pid_str) pid_str = dict_get(props, "pipewire.sec.pid");
            n->pid = parse_pid(pid_str);
            const char *cid = dict_get(props, PW_KEY_CLIENT_ID);
            n->client_id = parse_u32(cid);
            dict_copy(props, PW_KEY_APP_NAME,         n->app_name,    sizeof(n->app_name));
            dict_copy(props, PW_KEY_NODE_NAME,        n->node_name,   sizeof(n->node_name));
            dict_copy(props, PW_KEY_NODE_DESCRIPTION, n->node_desc,   sizeof(n->node_desc));
            dict_copy(props, PW_KEY_MEDIA_CLASS,      n->media_class, sizeof(n->media_class));
            dict_copy(props, PW_KEY_MEDIA_NAME,       n->media_name,  sizeof(n->media_name));
        }

        /* Bind to get full node info (PID, etc.).
         * Each entry is individually heap-allocated so that a later
         * realloc of the pointer array never moves the spa_hook that
         * PipeWire holds a pointer to, and so user_data stays valid. */
        if (w->bound_count >= w->bound_cap) {
            size_t nc = w->bound_cap ? w->bound_cap * 2 : 64;
            pw_watcher_bound_t **tmp = realloc(w->bound, nc * sizeof(*w->bound));
            if (!tmp) return;
            w->bound     = tmp;
            w->bound_cap = nc;
        }
        pw_watcher_bound_t *wb = calloc(1, sizeof(*wb));
        if (!wb) return;
        wb->node_index = w->graph.node_count - 1;  /* index of node just pushed */
        wb->pw_id      = id;                          /* PW object ID for removal */
        wb->owner      = w;
        wb->proxy      = pw_registry_bind(w->registry, id, type, version, 0);
        w->bound[w->bound_count++] = wb;
        if (wb->proxy) {
            pw_proxy_add_object_listener(wb->proxy,
                                         &wb->listener,
                                         &g_watcher_node_events, wb);
        }

        /* After initial enumeration, issue a sync so that node_info
         * (carrying media_class / pid) is guaranteed to arrive before
         * we publish.  This mirrors the startup two-phase approach.
         * Coalesce multiple rapid arrivals into a single sync by only
         * issuing one if none is already pending. */
        if (w->initial_done && !w->pending_sync)
            w->pending_sync = pw_core_sync(w->core, PW_ID_CORE, 0);

    } else if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        if (!props) return;
        pw_snap_client_t *c = pw_graph_push_client(&w->graph);
        if (!c) return;
        c->id = id;
        const char *pid_str = dict_get(props, PW_KEY_APP_PROCESS_ID);
        if (!pid_str) pid_str = dict_get(props, "pipewire.sec.pid");
        c->pid = parse_pid(pid_str);

        if (w->initial_done) watcher_publish(w);

    } else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        if (!props) return;
        pw_snap_port_t *p = pw_graph_push_port(&w->graph);
        if (!p) return;
        p->id = id;
        const char *nid = dict_get(props, PW_KEY_NODE_ID);
        p->node_id = parse_u32(nid);
        dict_copy(props, PW_KEY_PORT_NAME,      p->port_name,  sizeof(p->port_name));
        dict_copy(props, PW_KEY_PORT_ALIAS,     p->port_alias, sizeof(p->port_alias));
        dict_copy(props, PW_KEY_PORT_DIRECTION, p->direction,  sizeof(p->direction));
        dict_copy(props, PW_KEY_FORMAT_DSP,     p->format_dsp, sizeof(p->format_dsp));

    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        if (!props) return;
        pw_snap_link_t *l = pw_graph_push_link(&w->graph);
        if (!l) return;
        l->id = id;
        const char *v;
        v = dict_get(props, PW_KEY_LINK_OUTPUT_NODE);
        l->output_node_id = parse_u32(v);
        v = dict_get(props, "link.output.port");
        l->output_port_id = parse_u32(v);
        v = dict_get(props, PW_KEY_LINK_INPUT_NODE);
        l->input_node_id  = parse_u32(v);
        v = dict_get(props, "link.input.port");
        l->input_port_id  = parse_u32(v);

        if (w->initial_done) watcher_publish(w);
    }
}

static void watcher_on_global_remove(void *data, uint32_t id)
{
    pw_watcher_t *w = data;

    /* Remove from nodes */
    for (size_t i = 0; i < w->graph.node_count; i++) {
        if (w->graph.nodes[i].id == id) {
            size_t last = --w->graph.node_count;
            w->graph.nodes[i] = w->graph.nodes[last];
            /*
             * Fix up the bound[] array:
             *
             * 1. Remove the entry for the deleted node (identified by pw_id==id).
             *    Destroy the proxy so PipeWire stops delivering callbacks for it.
             *
             * 2. Update the entry for the moved node (it was at node_index==last;
             *    it now lives at graph index i).
             *
             * Doing both in a single pass is safe because pw_id is unique.
             */
            int removed = 0, moved = 0;
            for (size_t b = 0; b < w->bound_count && !(removed && moved); ) {
                pw_watcher_bound_t *wb = w->bound[b];
                if (!wb) { b++; continue; }

                if (!removed && wb->pw_id == id) {
                    /* This is the deleted node's proxy — tear it down */
                    if (wb->proxy)
                        pw_proxy_destroy(wb->proxy);
                    free(wb);
                    w->bound[b] = w->bound[--w->bound_count];
                    removed = 1;
                    /* Don't increment b — the slot now holds a different entry */
                    continue;
                }
                if (!moved && i != last && wb->node_index == last) {
                    /* This entry tracked the node that was at `last`; it moved to `i` */
                    wb->node_index = i;
                    moved = 1;
                }
                b++;
            }
            watcher_publish(w);
            return;
        }
    }
    /* Remove from links */
    for (size_t i = 0; i < w->graph.link_count; i++) {
        if (w->graph.links[i].id == id) {
            w->graph.links[i] = w->graph.links[--w->graph.link_count];
            watcher_publish(w);
            return;
        }
    }
    /* Remove from ports */
    for (size_t i = 0; i < w->graph.port_count; i++) {
        if (w->graph.ports[i].id == id) {
            w->graph.ports[i] = w->graph.ports[--w->graph.port_count];
            return;
        }
    }
    /* Remove from clients */
    for (size_t i = 0; i < w->graph.client_count; i++) {
        if (w->graph.clients[i].id == id) {
            w->graph.clients[i] = w->graph.clients[--w->graph.client_count];
            return;
        }
    }
}

static void watcher_on_core_done(void *data, uint32_t id, int seq)
{
    pw_watcher_t *w = data;
    if (id != PW_ID_CORE) return;

    /* Runtime new-node sync: node_info has now been delivered */
    if (w->initial_done && seq == w->pending_sync) {
        w->pending_sync = 0;
        watcher_publish(w);
        return;
    }

    if (seq != w->sync_seq) return;

    if (w->phase == 0) {
        /* All globals received; do a second sync for node info callbacks */
        w->phase    = 1;
        w->sync_seq = pw_core_sync(w->core, PW_ID_CORE, 0);
    } else {
        /* Initial enumeration complete — publish and mark ready */
        w->initial_done = 1;
        watcher_publish(w);
        /* Don't quit the loop — stay running for live updates */
    }
}

static void watcher_on_core_error(void *data, uint32_t id, int seq,
                                   int res, const char *message)
{
    (void)id; (void)seq; (void)res; (void)message;
    pw_watcher_t *w = data;
    /* On fatal error, quit so the thread can restart */
    pw_main_loop_quit(w->loop);
}

static void *pw_watcher_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ev-pw-watch");

    /* PIPEWIRE_REMOTE is resolved in pw_watcher_start() on the main
     * thread before this thread is spawned, so no setenv() here. */

    pw_init(NULL, NULL);

    pw_watcher_t w;
    memset(&w, 0, sizeof(w));
    pw_graph_init(&w.graph);

    w.loop = pw_main_loop_new(NULL);
    if (!w.loop) return NULL;

    w.context = pw_context_new(pw_main_loop_get_loop(w.loop), NULL, 0);
    if (!w.context) { pw_main_loop_destroy(w.loop); return NULL; }

    w.core = pw_context_connect(w.context, NULL, 0);
    if (!w.core) {
        pw_context_destroy(w.context);
        pw_main_loop_destroy(w.loop);
        return NULL;
    }

    pw_core_add_listener(w.core, &w.core_listener,
                         &g_watcher_core_events, &w);

    w.registry = pw_core_get_registry(w.core, PW_VERSION_REGISTRY, 0);
    if (!w.registry) {
        pw_core_disconnect(w.core);
        pw_context_destroy(w.context);
        pw_main_loop_destroy(w.loop);
        return NULL;
    }

    pw_registry_add_listener(w.registry, &w.registry_listener,
                             &g_watcher_reg_events, &w);

    w.phase    = 0;
    w.sync_seq = pw_core_sync(w.core, PW_ID_CORE, 0);

    /* Run until stop is requested or an error quits the loop */
    while (!g_watcher_stop)
        pw_main_loop_run(w.loop);

    /* Teardown */
    for (size_t i = 0; i < w.bound_count; i++) {
        if (w.bound[i] && w.bound[i]->proxy)
            pw_proxy_destroy(w.bound[i]->proxy);
        free(w.bound[i]);
    }
    free(w.bound);
    pw_graph_free(&w.graph);

    pw_proxy_destroy((struct pw_proxy *)w.registry);
    pw_core_disconnect(w.core);
    pw_context_destroy(w.context);
    pw_main_loop_destroy(w.loop);

    return NULL;
}

/*
 * Start the persistent watcher thread.
 * Called once from ui_gtk.c before gtk_main().
 *
 * PIPEWIRE_REMOTE is resolved here — on the main thread, before any
 * PipeWire threads are spawned — so that setenv() is never called
 * from multiple threads concurrently (POSIX data race on environ,
 * FIND-2 / security_audit_2026-03-04.md).
 */
void pw_watcher_start(void)
{
    if (g_watcher_tid) return;

    /* Resolve PIPEWIRE_REMOTE once on the main thread. */
    if (getuid() == 0 && !getenv("PIPEWIRE_REMOTE")) {
        char pw_remote_buf[256] = {0};
        const char *uid_str = getenv("SUDO_UID");
        if (!uid_str) uid_str = getenv("PKEXEC_UID");
        if (uid_str) {
            /* Validate that the UID string is purely numeric (OBS-A). */
            int is_numeric = 1;
            for (const char *p = uid_str; *p; p++) {
                if (*p < '0' || *p > '9') { is_numeric = 0; break; }
            }
            if (is_numeric)
                snprintf(pw_remote_buf, sizeof(pw_remote_buf),
                         "/run/user/%s/pipewire-0", uid_str);
        } else {
            DIR *run_dir = opendir("/run/user");
            if (run_dir) {
                struct dirent *de;
                while ((de = readdir(run_dir)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    if (strcmp(de->d_name, "0") == 0) continue;

                    /* Validate d_name is a purely numeric UID (FIND-1). */
                    int is_numeric = 1;
                    for (const char *p = de->d_name; *p; p++) {
                        if (*p < '0' || *p > '9') { is_numeric = 0; break; }
                    }
                    if (!is_numeric) continue;

                    /* d_name is purely numeric (validated above), so the
                     * path length is bounded: "/run/user/" (10) + UID
                     * digits (≤10 for 32-bit) + "/pipewire-0" (11) < 40.
                     * Use a larger intermediate buffer to silence the
                     * -Wformat-truncation false positive. */
                    char probe[PATH_MAX];
                    snprintf(probe, sizeof(probe),
                             "/run/user/%s/pipewire-0", de->d_name);

                    /* Verify ownership matches the directory UID (FIND-1). */
                    struct stat st;
                    if (stat(probe, &st) != 0) continue;
                    uid_t expected_uid = (uid_t)strtoul(de->d_name, NULL, 10);
                    if (st.st_uid != expected_uid) continue;

                    snprintf(pw_remote_buf, sizeof(pw_remote_buf),
                             "%s", probe);
                    break;
                }
                closedir(run_dir);
            }
        }
        if (pw_remote_buf[0])
            setenv("PIPEWIRE_REMOTE", pw_remote_buf, 1);
    }

    pw_graph_init(&g_live_graph);
    g_watcher_stop = 0;
    pthread_create(&g_watcher_tid, NULL, pw_watcher_thread, NULL);
}

/*
 * Stop the persistent watcher thread.
 * Called from ui_gtk.c on shutdown.
 */
void pw_watcher_stop(void)
{
    if (!g_watcher_tid) return;
    g_watcher_stop = 1;
    /* We can't easily signal the PW main loop from outside without
     * a pipewire reference, so just detach and let it exit on next
     * iteration check.  The process is shutting down anyway. */
    pthread_detach(g_watcher_tid);
    g_watcher_tid = 0;

    pthread_mutex_lock(&g_watcher_mutex);
    pw_graph_free(&g_live_graph);
    g_live_ready = 0;
    pthread_mutex_unlock(&g_watcher_mutex);
}

/* ── registry enumeration ────────────────────────────────────── */


/*
 * Bound-node tracking: we bind to every Node global so we can
 * receive its pw_node_info (which carries the full property set
 * including application.process.id).  After the first sync we
 * issue the binds, then do a second sync to collect the info
 * callbacks.
 */
typedef struct pw_bound_node pw_bound_node_t;
struct pw_bound_node {
    struct pw_proxy    *proxy;
    struct spa_hook     listener;
    size_t              node_index;  /* index into graph.nodes[] (resolved after enum) */
    struct pw_enum     *owner;      /* back-pointer               */
};

typedef struct pw_enum {
    pw_graph_t          graph;
    struct pw_main_loop *loop;
    struct pw_context   *context;
    struct pw_core      *core;
    struct pw_registry  *registry;
    struct spa_hook      registry_listener;
    struct spa_hook      core_listener;
    int                  sync_seq;
    int                  phase;     /* 0 = collecting globals, 1 = collecting info */

    pw_bound_node_t   **bound;      /* array of pointers to heap-allocated bound nodes */
    size_t              bound_count;
    size_t              bound_cap;
} pw_enum_t;

/* ── node info callback (phase 2) ────────────────────────────── */

static void on_node_info(void *data, const struct pw_node_info *info)
{
    pw_bound_node_t *bn = data;
    if (!bn->owner || bn->node_index >= bn->owner->graph.node_count)
        return;
    pw_snap_node_t  *n  = &bn->owner->graph.nodes[bn->node_index];

    if (!info || !info->props)
        return;

    const struct spa_dict *props = info->props;

    const char *pid_str = dict_get(props, PW_KEY_APP_PROCESS_ID);
    if (!pid_str)
        pid_str = dict_get(props, "pipewire.sec.pid");
    n->pid = parse_pid(pid_str);

    dict_copy(props, PW_KEY_APP_NAME,          n->app_name,    sizeof(n->app_name));
    dict_copy(props, PW_KEY_NODE_NAME,         n->node_name,   sizeof(n->node_name));
    dict_copy(props, PW_KEY_NODE_DESCRIPTION,  n->node_desc,   sizeof(n->node_desc));
    dict_copy(props, PW_KEY_MEDIA_CLASS,       n->media_class, sizeof(n->media_class));
    dict_copy(props, PW_KEY_MEDIA_NAME,        n->media_name,  sizeof(n->media_name));
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = on_node_info,
};

/* ── registry global callback (phase 1) ─────────────────────── */

static void on_global(void *data, uint32_t id,
                      uint32_t permissions, const char *type,
                      uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    pw_enum_t *e = data;

    if (!type)
        return;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        /* Create a skeleton node entry — full details filled by on_node_info */
        pw_snap_node_t *n = pw_graph_push_node(&e->graph);
        if (!n) return;
        n->id = id;

        /* Grab whatever the global props give us (often minimal) */
        if (props) {
            const char *pid_str = dict_get(props, PW_KEY_APP_PROCESS_ID);
            if (!pid_str)
                pid_str = dict_get(props, "pipewire.sec.pid");
            n->pid = parse_pid(pid_str);

            const char *cid_str = dict_get(props, PW_KEY_CLIENT_ID);
            n->client_id = parse_u32(cid_str);

            dict_copy(props, PW_KEY_APP_NAME,          n->app_name,    sizeof(n->app_name));
            dict_copy(props, PW_KEY_NODE_NAME,         n->node_name,   sizeof(n->node_name));
            dict_copy(props, PW_KEY_NODE_DESCRIPTION,  n->node_desc,   sizeof(n->node_desc));
            dict_copy(props, PW_KEY_MEDIA_CLASS,       n->media_class, sizeof(n->media_class));
            dict_copy(props, PW_KEY_MEDIA_NAME,        n->media_name,  sizeof(n->media_name));
        }

        /* Record that we need to bind this node later.
         * Each bound node is individually heap-allocated so that
         * realloc of the pointer array never invalidates the
         * embedded spa_hook that PipeWire's listener list references. */
        if (e->bound_count >= e->bound_cap) {
            size_t nc = e->bound_cap ? e->bound_cap * 2 : 64;
            pw_bound_node_t **tmp = realloc(e->bound, nc * sizeof(*tmp));
            if (!tmp) return;
            e->bound     = tmp;
            e->bound_cap = nc;
        }
        pw_bound_node_t *bn = calloc(1, sizeof(*bn));
        if (!bn) return;
        e->bound[e->bound_count++] = bn;
        bn->node_index = e->graph.node_count - 1;  /* index of the node we just pushed */
        bn->owner = e;
        bn->proxy = pw_registry_bind(e->registry, id,
                                     type, version, 0);
        if (bn->proxy) {
            pw_proxy_add_object_listener(bn->proxy,
                                         &bn->listener,
                                         &node_events, bn);
        }

    } else if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        if (!props) return;
        pw_snap_client_t *c = pw_graph_push_client(&e->graph);
        if (!c) return;
        c->id = id;

        const char *pid_str = dict_get(props, PW_KEY_APP_PROCESS_ID);
        if (!pid_str)
            pid_str = dict_get(props, "pipewire.sec.pid");
        c->pid = parse_pid(pid_str);

    } else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        if (!props) return;
        pw_snap_port_t *p = pw_graph_push_port(&e->graph);
        if (!p) return;
        p->id = id;

        const char *nid = dict_get(props, PW_KEY_NODE_ID);
        p->node_id = parse_u32(nid);

        dict_copy(props, PW_KEY_PORT_NAME,      p->port_name,  sizeof(p->port_name));
        dict_copy(props, PW_KEY_PORT_ALIAS,     p->port_alias, sizeof(p->port_alias));
        dict_copy(props, PW_KEY_PORT_DIRECTION, p->direction,  sizeof(p->direction));
        dict_copy(props, PW_KEY_FORMAT_DSP,     p->format_dsp, sizeof(p->format_dsp));

    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        if (!props) return;
        pw_snap_link_t *l = pw_graph_push_link(&e->graph);
        if (!l) return;
        l->id = id;

        const char *v;
        v = dict_get(props, PW_KEY_LINK_OUTPUT_NODE);
        l->output_node_id = parse_u32(v);
        v = dict_get(props, "link.output.port");
        l->output_port_id = parse_u32(v);
        v = dict_get(props, PW_KEY_LINK_INPUT_NODE);
        l->input_node_id = parse_u32(v);
        v = dict_get(props, "link.input.port");
        l->input_port_id = parse_u32(v);
    }
}

static void on_global_remove(void *data, uint32_t id)
{
    (void)data; (void)id;
    /* We only do a single snapshot pass — removals don't matter. */
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global        = on_global,
    .global_remove = on_global_remove,
};

static void on_core_done(void *data, uint32_t id, int seq)
{
    pw_enum_t *e = data;
    if (id == PW_ID_CORE && seq == e->sync_seq) {
        if (e->phase == 0) {
            /*
             * Phase 0 complete: all globals enumerated and node
             * proxies bound.  Issue a second sync so that the
             * pw_node_info callbacks have time to arrive.
             */
            e->phase    = 1;
            e->sync_seq = pw_core_sync(e->core, PW_ID_CORE, 0);
        } else {
            /* Phase 1 complete: all info received, we're done. */
            pw_main_loop_quit(e->loop);
        }
    }
}

static void on_core_error(void *data, uint32_t id, int seq,
                          int res, const char *message)
{
    (void)id; (void)seq; (void)res; (void)message;
    pw_enum_t *e = data;
    pw_main_loop_quit(e->loop);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done  = on_core_done,
    .error = on_core_error,
};

/*
 * Snapshot the entire PipeWire graph.  Connects, registers for
 * globals, issues a sync, runs the loop until the sync completes,
 * then tears down.  Returns 0 on success, -1 on failure.
 *
 * Must be called from a thread that is NOT the PipeWire main loop
 * (i.e. from our GTask worker thread).
 */
int pw_snapshot(pw_graph_t *out)
{
    pw_graph_init(out);

    pthread_mutex_lock(&g_watcher_mutex);
    if (!g_live_ready) {
        pthread_mutex_unlock(&g_watcher_mutex);
        return -1;   /* watcher not ready yet — caller will retry next cycle */
    }
    int rc = pw_graph_copy(out, &g_live_graph);
    pthread_mutex_unlock(&g_watcher_mutex);
    return rc;
}

/* ── per-PID result building ─────────────────────────────────── */

/*
 * Categories for the PipeWire detail panel tree.
 */
typedef enum {
    PW_CAT_OUTPUT,    /* streams sending audio out (playback)   */
    PW_CAT_INPUT,     /* streams capturing audio in (record)    */
    PW_CAT_VIDEO,     /* video streams                          */
    PW_CAT_MIDI,      /* MIDI streams                           */
    PW_CAT_OTHER,     /* anything else                          */
    PW_CAT_COUNT
} pw_scan_category_t;

static const char *pw_cat_label[PW_CAT_COUNT] = {
    [PW_CAT_OUTPUT] = "Audio Output",
    [PW_CAT_INPUT]  = "Audio Input",
    [PW_CAT_VIDEO]  = "Video",
    [PW_CAT_MIDI]   = "MIDI",
    [PW_CAT_OTHER]  = "Other",
};

typedef struct {
    char text[512];
    pw_scan_category_t cat;
    uint32_t node_id;    /* PipeWire node ID (for spectrogram selection) */
} pw_result_entry_t;

typedef struct {
    pw_result_entry_t *entries;
    size_t count;
    size_t capacity;
} pw_result_list_t;

static void pw_result_init(pw_result_list_t *l) { memset(l, 0, sizeof(*l)); }

static void pw_result_free(pw_result_list_t *l)
{
    free(l->entries);
    memset(l, 0, sizeof(*l));
}

static void pw_result_push(pw_result_list_t *l, pw_scan_category_t cat,
                           const char *text, uint32_t node_id)
{
    if (l->count >= l->capacity) {
        size_t nc = l->capacity ? l->capacity * 2 : 32;
        pw_result_entry_t *tmp = realloc(l->entries, nc * sizeof(*tmp));
        if (!tmp) return;
        l->entries  = tmp;
        l->capacity = nc;
    }
    pw_result_entry_t *e = &l->entries[l->count++];
    e->cat = cat;
    e->node_id = node_id;
    snprintf(e->text, sizeof(e->text), "%s", text);
}

static pw_scan_category_t classify_media_class(const char *mc)
{
    if (!mc || !mc[0]) return PW_CAT_OTHER;
    if (strstr(mc, "Audio") && strstr(mc, "Output"))  return PW_CAT_OUTPUT;
    if (strstr(mc, "Audio") && strstr(mc, "Input"))   return PW_CAT_INPUT;
    if (strstr(mc, "Audio") && strstr(mc, "Sink"))    return PW_CAT_OUTPUT;
    if (strstr(mc, "Audio") && strstr(mc, "Source"))  return PW_CAT_INPUT;
    if (strstr(mc, "Video"))                          return PW_CAT_VIDEO;
    if (strstr(mc, "Midi"))                           return PW_CAT_MIDI;
    return PW_CAT_OTHER;
}

/*
 * Get the best human-readable label for a node.
 * For streams: prefer media_name (tab title, song name),
 * then node_desc, then app_name, then node_name.
 */
static const char *node_label(const pw_snap_node_t *n)
{
    if (n->media_name[0])  return n->media_name;
    if (n->node_desc[0])   return n->node_desc;
    if (n->app_name[0])    return n->app_name;
    if (n->node_name[0])   return n->node_name;
    return "(unknown)";
}

/*
 * Build the per-PID result list from a graph snapshot.
 *
 * For each node owned by `pid`:
 *   1. Classify it (output/input/video/midi/other)
 *   2. Follow links to find what it's connected to
 *   3. Format a display string:
 *        "App Name → Sink Description (media class)"
 *      or for inputs:
 *        "Source Description → App Name (media class)"
 */
static void build_pid_results(const pw_graph_t *g, pid_t pid,
                              pw_result_list_t *out)
{
    for (size_t ni = 0; ni < g->node_count; ni++) {
        const pw_snap_node_t *node = &g->nodes[ni];
        if (node->pid != pid)
            continue;

        pw_scan_category_t cat = classify_media_class(node->media_class);
        const char *self_label = node_label(node);

        /* Collect unique peer node IDs this node links to.
         * For an output stream (Stream/Output/Audio), the stream
         * is the output side of the link → peer is the input side.
         * For an input stream (Stream/Input/Audio), the stream
         * is the input side → peer is the output side. */
        uint32_t peers[64];
        size_t npeer = 0;

        for (size_t li = 0; li < g->link_count; li++) {
            const pw_snap_link_t *lk = &g->links[li];
            uint32_t peer_id = 0;

            if (lk->output_node_id == node->id)
                peer_id = lk->input_node_id;
            else if (lk->input_node_id == node->id)
                peer_id = lk->output_node_id;
            else
                continue;

            if (peer_id == 0 || peer_id == node->id)
                continue;

            /* Dedup */
            int found = 0;
            for (size_t k = 0; k < npeer; k++)
                if (peers[k] == peer_id) { found = 1; break; }
            if (!found && npeer < 64)
                peers[npeer++] = peer_id;
        }

        if (npeer == 0) {
            /* Node exists but has no active links */
            char buf[512];
            snprintf(buf, sizeof(buf), "%s  (not connected)", self_label);
            pw_result_push(out, cat, buf, node->id);
        } else {
            for (size_t pi = 0; pi < npeer; pi++) {
                const pw_snap_node_t *peer = pw_graph_find_node(g, peers[pi]);
                const char *peer_label = peer ? node_label(peer) : "(unknown)";

                char buf[512];
                if (cat == PW_CAT_OUTPUT) {
                    /* Our stream → Sink */
                    snprintf(buf, sizeof(buf), "%s → %s",
                             self_label, peer_label);
                } else if (cat == PW_CAT_INPUT) {
                    /* Source → Our stream */
                    snprintf(buf, sizeof(buf), "%s → %s",
                             peer_label, self_label);
                } else {
                    snprintf(buf, sizeof(buf), "%s ↔ %s",
                             self_label, peer_label);
                }
                pw_result_push(out, cat, buf, node->id);
            }
        }
    }
}

/* ── Pango markup ────────────────────────────────────────────── */

static char *pw_entry_to_markup(const char *text)
{
    /* Highlight the arrow and the part after it in a colour */
    const char *arrow = strstr(text, " → ");
    if (!arrow)
        arrow = strstr(text, " ↔ ");

    if (arrow) {
        char *left  = g_markup_escape_text(text, (gssize)(arrow - text));
        char *right = g_markup_escape_text(arrow + strlen(" → "), -1);
        /* Pick a nice colour for the target device */
        char *markup = g_strdup_printf(
            "<b>%s</b> → <span foreground=\"#6699cc\">%s</span>",
            left, right);
        g_free(left);
        g_free(right);
        return markup;
    }

    return g_markup_escape_text(text, -1);
}

/* ── GTask async scan ────────────────────────────────────────── */

typedef struct {
    pid_t              pid;
    guint              generation;
    pw_result_list_t   buckets[PW_CAT_COUNT];
    uint32_t           audio_node_id;   /* first Stream/Output/Audio node for pid, or 0 */
    uint32_t           audio_node_ids[64]; /* all audio output node IDs for this pid */
    size_t             audio_node_count;
    ui_ctx_t          *ctx;
} pw_scan_task_t;

static void pw_scan_task_free(pw_scan_task_t *t)
{
    if (!t) return;
    for (int c = 0; c < PW_CAT_COUNT; c++)
        pw_result_free(&t->buckets[c]);
    free(t);
}

static void pw_scan_thread_func(GTask        *task,
                                gpointer      source_object,
                                gpointer      task_data,
                                GCancellable *cancellable)
{
    (void)source_object;
    pw_scan_task_t *t = task_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    pw_graph_t graph;
    if (pw_snapshot(&graph) != 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "PipeWire connection failed");
        return;
    }

    if (g_cancellable_is_cancelled(cancellable)) {
        pw_graph_free(&graph);
        return;
    }

    /* Build per-PID results */
    pw_result_list_t all;
    pw_result_init(&all);
    build_pid_results(&graph, t->pid, &all);

    /* Find all audio output nodes for the spectrogram */
    t->audio_node_id = 0;
    t->audio_node_count = 0;
    for (size_t i = 0; i < graph.node_count; i++) {
        if (graph.nodes[i].pid == t->pid &&
            strstr(graph.nodes[i].media_class, "Stream") &&
            strstr(graph.nodes[i].media_class, "Output") &&
            strstr(graph.nodes[i].media_class, "Audio")) {
            if (t->audio_node_count < 64)
                t->audio_node_ids[t->audio_node_count++] = graph.nodes[i].id;
            if (t->audio_node_id == 0)
                t->audio_node_id = graph.nodes[i].id;
        }
    }

    pw_graph_free(&graph);

    /* Split into per-category buckets */
    for (int c = 0; c < PW_CAT_COUNT; c++)
        pw_result_init(&t->buckets[c]);

    for (size_t i = 0; i < all.count; i++) {
        pw_scan_category_t cat = all.entries[i].cat;
        pw_result_push(&t->buckets[cat], cat, all.entries[i].text,
                       all.entries[i].node_id);
    }
    pw_result_free(&all);

    g_task_return_boolean(task, TRUE);
}

static void pw_scan_complete(GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
    (void)source_object;
    ui_ctx_t *ctx = user_data;

    GTask *task = G_TASK(result);
    pw_scan_task_t *t = g_task_get_task_data(task);

    if (!t || t->generation != ctx->pw_generation)
        return;
    if (g_task_had_error(task))
        return;

    /* Save scroll position */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(gtk_widget_get_parent(
            GTK_WIDGET(ctx->pw_view))));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->pw_store);

    /* Index existing category rows */
    GtkTreeIter cat_iters[PW_CAT_COUNT];
    gboolean    cat_exists[PW_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(model, &top, PW_COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < PW_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    /* Remove empty categories */
    for (int c = 0; c < PW_CAT_COUNT; c++) {
        if (cat_exists[c] && t->buckets[c].count == 0) {
            gtk_tree_store_remove(ctx->pw_store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
    for (int c = 0; c < PW_CAT_COUNT; c++) {
        if (t->buckets[c].count == 0) continue;

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s (%zu)",
                 pw_cat_label[c], t->buckets[c].count);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_append(ctx->pw_store, &parent, NULL);
            gtk_tree_store_set(ctx->pw_store, &parent,
                               PW_COL_TEXT, hdr,
                               PW_COL_MARKUP, hdr_escaped,
                               PW_COL_CAT, (gint)c, -1);
            g_free(hdr_escaped);
            cat_exists[c] = TRUE;
            cat_iters[c]  = parent;
        } else {
            parent = cat_iters[c];
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_set(ctx->pw_store, &parent,
                               PW_COL_TEXT, hdr,
                               PW_COL_MARKUP, hdr_escaped, -1);
            g_free(hdr_escaped);
        }

        /* Update child rows in place, adding/removing as needed */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);
        size_t bi = 0;

        while (bi < t->buckets[c].count && child_valid) {
            char *markup = pw_entry_to_markup(t->buckets[c].entries[bi].text);
            gtk_tree_store_set(ctx->pw_store, &child,
                               PW_COL_TEXT, t->buckets[c].entries[bi].text,
                               PW_COL_MARKUP, markup,
                               PW_COL_CAT, (gint)-1,
                               PW_COL_NODE_ID, (guint)t->buckets[c].entries[bi].node_id, -1);
            g_free(markup);
            bi++;
            child_valid = gtk_tree_model_iter_next(model, &child);
        }

        /* Append new rows */
        while (bi < t->buckets[c].count) {
            GtkTreeIter new_child;
            char *markup = pw_entry_to_markup(t->buckets[c].entries[bi].text);
            gtk_tree_store_append(ctx->pw_store, &new_child, &parent);
            gtk_tree_store_set(ctx->pw_store, &new_child,
                               PW_COL_TEXT, t->buckets[c].entries[bi].text,
                               PW_COL_MARKUP, markup,
                               PW_COL_CAT, (gint)-1,
                               PW_COL_NODE_ID, (guint)t->buckets[c].entries[bi].node_id, -1);
            g_free(markup);
            bi++;
        }

        /* Remove excess rows */
        while (child_valid) {
            child_valid = gtk_tree_store_remove(ctx->pw_store, &child);
        }

        /* Restore expand/collapse state */
        GtkTreePath *cat_path = gtk_tree_model_get_path(
            model, &cat_iters[c]);
        if (ctx->pw_collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->pw_view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->pw_view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);

    /* Start (or stop) the spectrogram based on audio node availability.
     *
     * If we're already capturing one of this PID's audio nodes,
     * keep it (the user may have explicitly selected it).
     * Otherwise, auto-start on the first available audio output node.
     */
    if (t->audio_node_count > 0) {
        /* Only auto-start spectrogram if user has explicitly shown it */
        if (ctx->sb_spectro_user_shown) {
            uint32_t current_node = spectrogram_get_target_node(ctx, NULL);
            int found_current = 0;
            for (size_t i = 0; i < t->audio_node_count; i++) {
                if (t->audio_node_ids[i] == current_node) {
                    found_current = 1;
                    break;
                }
            }
            if (!found_current)
                spectrogram_start_for_node(ctx, NULL,
                                           t->audio_node_ids[0]);
        }

        /* Start peak meters for all audio output nodes */
        pw_meter_start(ctx, t->audio_node_ids, t->audio_node_count);
    } else {
        spectrogram_stop(ctx, NULL);
        pw_meter_stop(ctx);
    }
}

/* ── public API ──────────────────────────────────────────────── */

void pipewire_scan_start(ui_ctx_t *ctx, pid_t pid)
{
    /* Disabled: PipeWire audio is now handled entirely by the
     * PipeWire plugin (src/plugins/pipewire_plugin.c).  The old
     * detail panel scanner is kept compiled but not called to avoid
     * conflicting PipeWire sink creation. */
    (void)ctx;
    (void)pid;
}

/* ── signal callbacks ────────────────────────────────────────── */

void on_pw_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->pw_store), iter,
                       PW_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < PW_CAT_COUNT)
        ctx->pw_collapsed |= (1u << cat_id);
}

void on_pw_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                        GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->pw_store), iter,
                       PW_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < PW_CAT_COUNT)
        ctx->pw_collapsed &= ~(1u << cat_id);
}

gboolean on_pw_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)data;

    if (ev->keyval != GDK_KEY_Return && ev->keyval != GDK_KEY_KP_Enter)
        return FALSE;

    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return FALSE;

    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, PW_COL_CAT, &cat_id, -1);
    if (cat_id < 0)
        return FALSE;

    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    if (!path)
        return FALSE;

    if (gtk_tree_view_row_expanded(view, path))
        gtk_tree_view_collapse_row(view, path);
    else
        gtk_tree_view_expand_row(view, path, FALSE);

    gtk_tree_path_free(path);
    return TRUE;
}

/*
 * Double-click (or Enter on) a leaf row in the PipeWire tree to
 * switch the spectrogram to that specific audio stream.
 */
void on_pw_row_activated(GtkTreeView *view, GtkTreePath *path,
                         GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    ui_ctx_t *ctx = data;

    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;

    /* Only act on leaf rows (cat == -1) */
    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, PW_COL_CAT, &cat_id, -1);
    if (cat_id >= 0)
        return;   /* category header row — handled by expand/collapse */

    guint node_id = 0;
    gtk_tree_model_get(model, &iter, PW_COL_NODE_ID, &node_id, -1);
    if (node_id == 0)
        return;

    /* Mark that the user explicitly wants the spectrogram visible */
    ctx->sb_spectro_user_shown = TRUE;
    spectrogram_start_for_node(ctx, NULL, (uint32_t)node_id);
}

#endif /* HAVE_PIPEWIRE */
