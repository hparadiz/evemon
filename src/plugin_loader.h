/*
 * plugin_loader.h – Internal plugin loader API.
 *
 * Handles dlopen/dlsym discovery of .so plugins, manages the plugin
 * registry, and dispatches lifecycle callbacks.  Not part of the
 * public plugin ABI — only used internally by the host.
 */

#ifndef ALLMON_PLUGIN_LOADER_H
#define ALLMON_PLUGIN_LOADER_H

#include "allmon_plugin.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Plugin instance ─────────────────────────────────────────── */

/*
 * A single running instance of a plugin.  One plugin type can have
 * multiple instances (e.g. two "Network" tabs watching different PIDs).
 */
typedef struct {
    allmon_plugin_t  *plugin;        /* plugin descriptor (from .so)    */
    void             *handle;        /* dlopen handle                   */
    GtkWidget        *widget;        /* root widget returned by create  */
    pid_t             tracked_pid;   /* PID this instance is watching   */
    gboolean          pinned;        /* TRUE = pinned, FALSE = follows  */
    int               instance_id;   /* unique instance counter         */
} plugin_instance_t;

/* ── Plugin registry ─────────────────────────────────────────── */

typedef struct {
    plugin_instance_t *instances;    /* dynamic array                   */
    size_t             count;
    size_t             capacity;

    /* Combined data needs of all loaded instances (OR'd together) */
    allmon_data_needs_t combined_needs;

    /* Host services table — injected by the host, passed to plugins */
    const allmon_host_services_t *host_services;
} plugin_registry_t;

/*
 * Initialise the plugin registry.
 */
void plugin_registry_init(plugin_registry_t *reg);

/*
 * Set the host services table for the registry.
 * Must be called before plugin_loader_scan() so that plugins
 * receive services via their activate() callback.
 */
void plugin_registry_set_host_services(plugin_registry_t *reg,
                                       const allmon_host_services_t *svc);

/*
 * Free all resources in the registry (calls destroy on each instance).
 */
void plugin_registry_destroy(plugin_registry_t *reg);

/*
 * Scan a directory for .so files and load each as a plugin.
 * Calls allmon_plugin_init() and create_widget() for each.
 *
 * Returns the number of plugins successfully loaded.
 */
int plugin_loader_scan(plugin_registry_t *reg, const char *dir);

/*
 * Create a new instance of an already-loaded plugin type (by id).
 * Returns the instance index, or -1 on failure.
 */
int plugin_instance_create(plugin_registry_t *reg, const char *plugin_id);

/*
 * Destroy a specific plugin instance by index.
 */
void plugin_instance_destroy(plugin_registry_t *reg, int instance_idx);

/*
 * Recalculate the combined_needs bitmask from all active instances.
 */
void plugin_registry_recalc_needs(plugin_registry_t *reg);

/*
 * Set the tracked PID for an instance.  If pinned is FALSE, the
 * instance follows the tree selection.
 */
void plugin_instance_set_pid(plugin_instance_t *inst, pid_t pid,
                             gboolean pinned);

/*
 * Dispatch update() to all instances tracking a given PID.
 * Called from the main thread after the broker gathers data.
 */
void plugin_dispatch_update(plugin_registry_t *reg, pid_t pid,
                            const allmon_proc_data_t *data);

/*
 * Dispatch clear() to all instances tracking a given PID.
 */
void plugin_dispatch_clear(plugin_registry_t *reg, pid_t pid);

/*
 * Dispatch clear() to ALL instances (e.g. when no process selected).
 */
void plugin_dispatch_clear_all(plugin_registry_t *reg);

/*
 * Collect the set of unique PIDs being tracked by all instances.
 * Caller provides output buffer; returns the count of unique PIDs.
 */
size_t plugin_collect_tracked_pids(const plugin_registry_t *reg,
                                   pid_t *out, size_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* ALLMON_PLUGIN_LOADER_H */
