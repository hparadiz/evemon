/*
 * plugin_loader.h – Internal plugin loader API.
 *
 * Handles dlopen/dlsym discovery of .so plugins, manages the plugin
 * registry, and dispatches lifecycle callbacks.  Not part of the
 * public plugin ABI — only used internally by the host.
 */

#ifndef evemon_PLUGIN_LOADER_H
#define evemon_PLUGIN_LOADER_H

#include "evemon_plugin.h"

#include <stddef.h>

/* When compiling without GTK (core library), gboolean is just int */
#ifdef EVEMON_NO_GTK
#ifndef gboolean
typedef int gboolean;
#endif
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Plugin instance ─────────────────────────────────────────── */

/*
 * A single running instance of a plugin.  One plugin type can have
 * multiple instances (e.g. two "Network" tabs watching different PIDs).
 */
typedef struct {
    evemon_plugin_t  *plugin;        /* plugin descriptor (from .so)    */
    void             *handle;        /* dlopen handle                   */
    void             *widget;        /* root widget returned by create_widget() */
    pid_t             tracked_pid;   /* PID this instance is watching   */
    int               pinned;        /* non-zero = pinned, 0 = follows selection */
    int               instance_id;   /* unique instance counter         */
    char              so_path[4096]; /* absolute path to the .so file   */
    int               is_active;     /* non-zero = in a notebook tab, 0 = floating window */
} plugin_instance_t;

/* ── Plugin registry ─────────────────────────────────────────── */

typedef struct {
    plugin_instance_t *instances;    /* dynamic array                   */
    size_t             count;
    size_t             capacity;

    /* Combined data needs of all loaded instances (OR'd together) */
    evemon_data_needs_t combined_needs;

    /* Host services table — injected by the host, passed to plugins */
    const evemon_host_services_t *host_services;
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
                                       const evemon_host_services_t *svc);

/*
 * Free all resources in the registry (calls destroy on each instance).
 */
void plugin_registry_destroy(plugin_registry_t *reg);

/*
 * Scan a directory for .so files and load each as a plugin.
 * Calls evemon_plugin_init() and create_widget() for each.
 *
 * Returns the number of plugins successfully loaded.
 */
int plugin_loader_scan(plugin_registry_t *reg, const char *dir);

/*
 * Create a new instance of an already-loaded plugin type (by id).
 * Returns the new instance's unique instance_id, or -1 on failure.
 */
int plugin_instance_create(plugin_registry_t *reg, const char *plugin_id);

/*
 * Find an instance by its unique instance_id.
 * Returns the array index, or -1 if not found.
 */
int plugin_registry_find_by_id(const plugin_registry_t *reg, int instance_id);

/*
 * Destroy a specific plugin instance by its unique instance_id.
 * Returns 0 on success, -1 if the instance was not found.
 */
int plugin_instance_destroy(plugin_registry_t *reg, int instance_id);

/*
 * Recalculate the combined_needs bitmask from all active instances.
 */
void plugin_registry_recalc_needs(plugin_registry_t *reg);

/*
 * Reload a single plugin .so by path: destroys every instance loaded
 * from that path, dlcloses the handle, then re-loads the file.
 * Returns the number of new instances created (1 on success, 0 on
 * failure). The caller is responsible for rebuilding any UI tabs.
 */
int plugin_reload(plugin_registry_t *reg, const char *so_path);

/*
 * Set the tracked PID for an instance.  If pinned is FALSE, the
 * instance follows the tree selection.
 */
void plugin_instance_set_pid(plugin_instance_t *inst, pid_t pid,
                             int pinned);

/*
 * Notify a plugin instance whether it lives in a notebook tab (active=TRUE)
 * or a standalone floating window (active=FALSE).  Calls plugin->set_active()
 * if the plugin provides it.  Safe to call with any inst pointer.
 */
void plugin_instance_set_active(plugin_instance_t *inst, int active);

/*
 * Dispatch update() to all instances tracking a given PID.
 * Called from the main thread after the broker gathers data.
 */
void plugin_dispatch_update(plugin_registry_t *reg, pid_t pid,
                            const evemon_proc_data_t *data);

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

/* ── Asynchronous plugin scanning ───────────────────────────── */

/*
 * Callback fired on the GTK main thread for each plugin that has been
 * successfully loaded from its .so file.
 *
 *   reg      – the registry (same pointer passed to scan_async)
 *   inst_id  – instance_id of the newly added plugin instance
 *   user_data – opaque pointer from plugin_loader_scan_async()
 *
 * The callback must call create_widget() and add the widget to the
 * notebook (or perform any other GTK-thread work).  The plugin
 * descriptor and handle are already stored in the registry; only
 * the widget creation and host-services injection are deferred.
 */
typedef void (*plugin_loaded_cb)(plugin_registry_t *reg,
                                 int inst_id,
                                 void *user_data);

/*
 * Callback fired on the GTK main thread once all .so files in the
 * directory have been processed (successfully or not).
 *
 *   n_loaded  – number of plugins successfully loaded
 *   user_data – opaque pointer from plugin_loader_scan_async()
 */
typedef void (*plugin_scan_done_cb)(int n_loaded, void *user_data);

/*
 * Function type for posting a callback + data onto the main thread.
 * The GTK frontend wires this to g_idle_add(); other frontends use
 * their own main-loop dispatch mechanism.
 *
 *   func      – function to call on the main thread; returns 0 to
 *               remove (one-shot), non-zero to keep scheduling.
 *   data      – opaque pointer forwarded to func.
 */
typedef void (*plugin_post_fn)(int (*func)(void *), void *data);

/*
 * Scan `dir` for plugins asynchronously in a background pthread.
 *
 * For each successfully loaded plugin, `on_loaded` is posted to the
 * main thread via `post_fn`.  When the scan completes, `on_done` is
 * posted to the main thread.
 *
 * Thread-safety: only one async scan may run at a time.  The caller
 * must not call plugin_loader_scan() or access plugin widget fields
 * until `on_done` has fired.
 *
 * Returns 0 on success (thread started), -1 on failure.
 */
int plugin_loader_scan_async(plugin_registry_t  *reg,
                             const char         *dir,
                             plugin_post_fn      post_fn,
                             plugin_loaded_cb    on_loaded,
                             plugin_scan_done_cb on_done,
                             void               *user_data);

#ifdef __cplusplus
}
#endif

#endif /* evemon_PLUGIN_LOADER_H */
