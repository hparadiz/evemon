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
 */

#ifdef HAVE_PIPEWIRE

#include "ui_internal.h"
#include "pipewire_graph.h"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
    n->pid = pid_str ? (pid_t)atoi(pid_str) : 0;

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
            n->pid = pid_str ? (pid_t)atoi(pid_str) : 0;

            const char *cid_str = dict_get(props, PW_KEY_CLIENT_ID);
            n->client_id = cid_str ? (uint32_t)atoi(cid_str) : 0;

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
        c->pid = pid_str ? (pid_t)atoi(pid_str) : 0;

    } else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        if (!props) return;
        pw_snap_port_t *p = pw_graph_push_port(&e->graph);
        if (!p) return;
        p->id = id;

        const char *nid = dict_get(props, PW_KEY_NODE_ID);
        p->node_id = nid ? (uint32_t)atoi(nid) : 0;

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
        l->output_node_id = v ? (uint32_t)atoi(v) : 0;
        v = dict_get(props, "link.output.port");
        l->output_port_id = v ? (uint32_t)atoi(v) : 0;
        v = dict_get(props, PW_KEY_LINK_INPUT_NODE);
        l->input_node_id = v ? (uint32_t)atoi(v) : 0;
        v = dict_get(props, "link.input.port");
        l->input_port_id = v ? (uint32_t)atoi(v) : 0;
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

    pw_enum_t e;
    memset(&e, 0, sizeof(e));
    pw_graph_init(&e.graph);

    /*
     * When running as root via sudo, pkexec, kdesu, or similar,
     * XDG_RUNTIME_DIR points at root's runtime dir (or is unset),
     * so pw_context_connect() can't find the real user's PipeWire
     * socket.  Detect this and set PIPEWIRE_REMOTE to the correct
     * path.
     *
     * Priority:
     *   1. PIPEWIRE_REMOTE already set → respect it.
     *   2. SUDO_UID     (set by sudo)
     *   3. PKEXEC_UID   (set by polkit/pkexec)
     *   4. Scan /run/user/<uid>/pipewire-0 for active sessions
     */
    char pw_remote_buf[256] = {0};
    if (getuid() == 0 && !getenv("PIPEWIRE_REMOTE")) {
        const char *uid_str = getenv("SUDO_UID");
        if (!uid_str) uid_str = getenv("PKEXEC_UID");

        if (uid_str) {
            snprintf(pw_remote_buf, sizeof(pw_remote_buf),
                     "/run/user/%s/pipewire-0", uid_str);
        } else {
            /* No hint — scan /run/user/ for the first pipewire socket.
             * This handles kdesu, gksu, and other su wrappers that
             * don't set a UID variable.                                */
            DIR *run_dir = opendir("/run/user");
            if (run_dir) {
                struct dirent *de;
                while ((de = readdir(run_dir)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    /* Skip UID 0 (root's own session) */
                    if (strcmp(de->d_name, "0") == 0) continue;
                    char probe[256];
                    snprintf(probe, sizeof(probe),
                             "/run/user/%s/pipewire-0", de->d_name);
                    if (access(probe, F_OK) == 0) {
                        snprintf(pw_remote_buf, sizeof(pw_remote_buf),
                                 "%s", probe);
                        break;
                    }
                }
                closedir(run_dir);
            }
        }
        if (pw_remote_buf[0])
            setenv("PIPEWIRE_REMOTE", pw_remote_buf, 1);
    }

    pw_init(NULL, NULL);

    e.loop = pw_main_loop_new(NULL);
    if (!e.loop)
        return -1;

    e.context = pw_context_new(pw_main_loop_get_loop(e.loop), NULL, 0);
    if (!e.context) {
        pw_main_loop_destroy(e.loop);
        return -1;
    }

    e.core = pw_context_connect(e.context, NULL, 0);
    if (!e.core) {
        pw_context_destroy(e.context);
        pw_main_loop_destroy(e.loop);
        return -1;
    }

    spa_zero(e.core_listener);
    pw_core_add_listener(e.core, &e.core_listener,
                         &core_events, &e);

    e.registry = pw_core_get_registry(e.core,
                                      PW_VERSION_REGISTRY, 0);
    if (!e.registry) {
        pw_core_disconnect(e.core);
        pw_context_destroy(e.context);
        pw_main_loop_destroy(e.loop);
        return -1;
    }

    spa_zero(e.registry_listener);
    pw_registry_add_listener(e.registry, &e.registry_listener,
                             &registry_events, &e);

    /* Flush: the sync callback fires after all pending globals
     * have been delivered.  on_core_done will issue a second sync
     * to let pw_node_info callbacks arrive, then quit.            */
    e.phase    = 0;
    e.sync_seq = pw_core_sync(e.core, PW_ID_CORE, 0);

    pw_main_loop_run(e.loop);

    /* Tear down bound node proxies */
    for (size_t i = 0; i < e.bound_count; i++) {
        if (e.bound[i] && e.bound[i]->proxy)
            pw_proxy_destroy(e.bound[i]->proxy);
        free(e.bound[i]);
    }
    free(e.bound);

    /* Tear down PipeWire connection */
    pw_proxy_destroy((struct pw_proxy *)e.registry);
    pw_core_disconnect(e.core);
    pw_context_destroy(e.context);
    pw_main_loop_destroy(e.loop);

    /*
     * Resolve node PIDs via client objects.  Native PipeWire clients
     * (e.g. mpv) don't set application.process.id on their nodes —
     * the PID only lives on the client object.  Cross-reference using
     * the node's client.id.
     */
    for (size_t i = 0; i < e.graph.node_count; i++) {
        pw_snap_node_t *n = &e.graph.nodes[i];
        if (n->pid != 0 || n->client_id == 0)
            continue;
        for (size_t j = 0; j < e.graph.client_count; j++) {
            if (e.graph.clients[j].id == n->client_id) {
                n->pid = e.graph.clients[j].pid;
                break;
            }
        }
    }

    *out = e.graph;   /* move ownership */
    return 0;
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
            uint32_t current_node = spectrogram_get_target_node(ctx);
            int found_current = 0;
            for (size_t i = 0; i < t->audio_node_count; i++) {
                if (t->audio_node_ids[i] == current_node) {
                    found_current = 1;
                    break;
                }
            }
            if (!found_current)
                spectrogram_start_for_node(ctx, t->audio_node_ids[0]);
        }

        /* Start peak meters for all audio output nodes */
        pw_meter_start(ctx, t->audio_node_ids, t->audio_node_count);
    } else {
        spectrogram_stop(ctx);
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
    spectrogram_start_for_node(ctx, (uint32_t)node_id);
}

#endif /* HAVE_PIPEWIRE */
