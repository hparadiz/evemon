/*
 * pipewire_graph.h – PipeWire graph snapshot types and function.
 *
 * Exposes pw_snapshot() so the plugin broker can gather PipeWire
 * graph data for plugins.  The snapshot connects to PipeWire,
 * enumerates all objects, then disconnects — returning ownership
 * of the data to the caller.
 */

#pragma once

#ifdef HAVE_PIPEWIRE

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* ── Graph snapshot types ────────────────────────────────────── */

typedef struct {
    uint32_t id;
    uint32_t client_id;
    pid_t    pid;
    char     app_name[128];
    char     node_name[128];
    char     node_desc[256];
    char     media_class[64];
    char     media_name[256];
} pw_snap_node_t;

typedef struct {
    uint32_t id;
    pid_t    pid;
} pw_snap_client_t;

typedef struct {
    uint32_t id;
    uint32_t node_id;
    char     port_name[128];
    char     port_alias[256];
    char     direction[8];
    char     format_dsp[64];
} pw_snap_port_t;

typedef struct {
    uint32_t id;
    uint32_t output_node_id;
    uint32_t output_port_id;
    uint32_t input_node_id;
    uint32_t input_port_id;
} pw_snap_link_t;

typedef struct {
    pw_snap_node_t   *nodes;
    size_t            node_count;
    size_t            node_cap;

    pw_snap_port_t   *ports;
    size_t            port_count;
    size_t            port_cap;

    pw_snap_link_t   *links;
    size_t            link_count;
    size_t            link_cap;

    pw_snap_client_t *clients;
    size_t            client_count;
    size_t            client_cap;
} pw_graph_t;

/*
 * Take a snapshot of the entire PipeWire graph.
 *
 * Connects to PipeWire, enumerates all nodes/ports/links/clients,
 * resolves node PIDs via client objects, then disconnects.
 * Returns 0 on success, -1 on failure.
 *
 * Caller must free the graph with pw_graph_free().
 * Safe to call from any thread (creates its own PW main loop).
 */
int  pw_snapshot(pw_graph_t *out);
void pw_graph_free(pw_graph_t *g);

/*
 * Persistent watcher thread — keeps a live PipeWire connection so
 * pw_snapshot() is a fast mutex-copy instead of a full IPC reconnect.
 * Call pw_watcher_start() once before the first pw_snapshot().
 */
void pw_watcher_start(void);
void pw_watcher_stop(void);

#endif /* HAVE_PIPEWIRE */
