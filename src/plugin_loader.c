/*
 * plugin_loader.c – Plugin discovery, loading, and lifecycle management.
 *
 * Scans a directory for .so files, calls dlopen/dlsym to load each one,
 * validates the ABI version, and manages the plugin instance registry.
 */

#include "plugin_loader.h"

#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Host-provided utility functions ─────────────────────────── */

/*
 * These are the implementations of the utility functions declared in
 * allmon_plugin.h.  They are resolved by plugins via dlopen(RTLD_GLOBAL)
 * because the host binary is linked with -rdynamic.
 */

/* Forward declarations from ui_internal.h / ui.c */
extern void format_memory(long kb, char *buf, size_t bufsz);
extern void format_fuzzy_time(time_t epoch, char *buf, size_t bufsz);

void allmon_format_memory(long kb, char *buf, size_t bufsz)
{
    format_memory(kb, buf, bufsz);
}

void allmon_format_fuzzy_time(time_t epoch, char *buf, size_t bufsz)
{
    format_fuzzy_time(epoch, buf, bufsz);
}

int allmon_net_io_get(const allmon_proc_data_t *data, pid_t tgid,
                      uint64_t *send_bytes, uint64_t *recv_bytes)
{
    if (!data || !data->fdmon) {
        if (send_bytes) *send_bytes = 0;
        if (recv_bytes) *recv_bytes = 0;
        return -1;
    }
    /* Forward to the real fdmon implementation.
     * We include fdmon.h to get the function signature. */
    extern int fdmon_net_io_get(const void *ctx, pid_t tgid,
                                uint64_t *send_bytes, uint64_t *recv_bytes);
    return fdmon_net_io_get(data->fdmon, tgid, send_bytes, recv_bytes);
}

/* ── Instance ID counter ─────────────────────────────────────── */

static int g_next_instance_id = 1;

/* ── Registry management ─────────────────────────────────────── */

void plugin_registry_init(plugin_registry_t *reg)
{
    memset(reg, 0, sizeof(*reg));
}

void plugin_registry_set_host_services(plugin_registry_t *reg,
                                       const allmon_host_services_t *svc)
{
    reg->host_services = svc;
}

void plugin_registry_destroy(plugin_registry_t *reg)
{
    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (inst->plugin && inst->plugin->destroy)
            inst->plugin->destroy(inst->plugin->plugin_ctx);
        if (inst->handle)
            dlclose(inst->handle);
    }
    free(reg->instances);
    memset(reg, 0, sizeof(*reg));
}

static plugin_instance_t *registry_alloc(plugin_registry_t *reg)
{
    if (reg->count >= reg->capacity) {
        size_t newcap = reg->capacity ? reg->capacity * 2 : 16;
        plugin_instance_t *tmp = realloc(reg->instances,
                                         newcap * sizeof(plugin_instance_t));
        if (!tmp) return NULL;
        reg->instances = tmp;
        reg->capacity  = newcap;
    }
    plugin_instance_t *inst = &reg->instances[reg->count++];
    memset(inst, 0, sizeof(*inst));
    inst->tracked_pid = 0;
    inst->pinned      = FALSE;
    inst->instance_id = g_next_instance_id++;
    return inst;
}

/* ── Plugin loading ──────────────────────────────────────────── */

/*
 * Load a single .so plugin file.
 * Returns 0 on success, -1 on failure.
 */
static int load_single_plugin(plugin_registry_t *reg, const char *path)
{
    /* Use RTLD_NOW to catch unresolved symbols immediately,
     * and RTLD_LOCAL so plugin symbols don't pollute the global namespace */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "allmon: plugin %s: %s\n", path, dlerror());
        return -1;
    }

    allmon_plugin_init_fn init_fn =
        (allmon_plugin_init_fn)dlsym(handle, "allmon_plugin_init");
    if (!init_fn) {
        fprintf(stderr, "allmon: plugin %s: no allmon_plugin_init symbol\n",
                path);
        dlclose(handle);
        return -1;
    }

    allmon_plugin_t *plugin = init_fn();
    if (!plugin) {
        fprintf(stderr, "allmon: plugin %s: init returned NULL\n", path);
        dlclose(handle);
        return -1;
    }

    if (plugin->abi_version != ALLMON_PLUGIN_ABI_VERSION) {
        fprintf(stderr, "allmon: plugin %s: ABI version %d (expected %d)\n",
                path, plugin->abi_version, ALLMON_PLUGIN_ABI_VERSION);
        dlclose(handle);
        return -1;
    }

    if (!plugin->create_widget || !plugin->update) {
        fprintf(stderr, "allmon: plugin %s: missing required callbacks\n",
                path);
        dlclose(handle);
        return -1;
    }

    /* Create the first instance */
    plugin_instance_t *inst = registry_alloc(reg);
    if (!inst) {
        dlclose(handle);
        return -1;
    }

    inst->plugin = plugin;
    inst->handle = handle;

    /* Call create_widget on the GTK main thread (we are on it) */
    inst->widget = plugin->create_widget(plugin->plugin_ctx);
    if (!inst->widget) {
        fprintf(stderr, "allmon: plugin %s: create_widget returned NULL\n",
                path);
        reg->count--;  /* undo the alloc */
        dlclose(handle);
        return -1;
    }

    /* Inject host services if the plugin wants them */
    if (plugin->activate && reg->host_services)
        plugin->activate(plugin->plugin_ctx, reg->host_services);

    /* Update combined needs */
    reg->combined_needs |= plugin->data_needs;

    fprintf(stdout, "allmon: loaded plugin \"%s\" (%s) v%s from %s\n",
            plugin->name ? plugin->name : "?",
            plugin->id   ? plugin->id   : "?",
            plugin->version ? plugin->version : "?",
            path);

    return 0;
}

int plugin_loader_scan(plugin_registry_t *reg, const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp) {
        /* Not an error — plugins dir may not exist yet */
        return 0;
    }

    int loaded = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        /* Only load .so files */
        size_t len = strlen(de->d_name);
        if (len < 4 || strcmp(de->d_name + len - 3, ".so") != 0)
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);

        if (load_single_plugin(reg, path) == 0)
            loaded++;
    }

    closedir(dp);
    return loaded;
}

/* ── Instance management ─────────────────────────────────────── */

int plugin_instance_create(plugin_registry_t *reg, const char *plugin_id)
{
    /* Find an existing instance with this plugin id to get the handle */
    allmon_plugin_t *plugin = NULL;
    void *handle = NULL;

    for (size_t i = 0; i < reg->count; i++) {
        if (reg->instances[i].plugin &&
            reg->instances[i].plugin->id &&
            strcmp(reg->instances[i].plugin->id, plugin_id) == 0) {
            plugin = reg->instances[i].plugin;
            handle = reg->instances[i].handle;
            break;
        }
    }

    if (!plugin) return -1;

    /* Re-call init to get a fresh plugin descriptor for the new instance */
    allmon_plugin_init_fn init_fn =
        (allmon_plugin_init_fn)dlsym(handle, "allmon_plugin_init");
    if (!init_fn) return -1;

    allmon_plugin_t *new_plugin = init_fn();
    if (!new_plugin) return -1;

    plugin_instance_t *inst = registry_alloc(reg);
    if (!inst) return -1;

    inst->plugin = new_plugin;
    inst->handle = handle;  /* shared handle — don't dlclose twice */

    inst->widget = new_plugin->create_widget(new_plugin->plugin_ctx);
    if (!inst->widget) {
        reg->count--;
        return -1;
    }

    /* Inject host services if the plugin wants them */
    if (new_plugin->activate && reg->host_services)
        new_plugin->activate(new_plugin->plugin_ctx, reg->host_services);

    reg->combined_needs |= new_plugin->data_needs;

    return (int)(reg->count - 1);
}

void plugin_instance_destroy(plugin_registry_t *reg, int instance_idx)
{
    if (instance_idx < 0 || (size_t)instance_idx >= reg->count)
        return;

    plugin_instance_t *inst = &reg->instances[instance_idx];
    if (inst->plugin && inst->plugin->destroy)
        inst->plugin->destroy(inst->plugin->plugin_ctx);

    /* Don't dlclose here — other instances may share the handle.
     * The handle is closed in plugin_registry_destroy(). */
    inst->plugin = NULL;
    inst->widget = NULL;
    inst->handle = NULL;

    /* Compact: shift remaining instances down */
    for (size_t i = (size_t)instance_idx; i + 1 < reg->count; i++)
        reg->instances[i] = reg->instances[i + 1];
    reg->count--;

    plugin_registry_recalc_needs(reg);
}

void plugin_registry_recalc_needs(plugin_registry_t *reg)
{
    reg->combined_needs = 0;
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->instances[i].plugin)
            reg->combined_needs |= reg->instances[i].plugin->data_needs;
    }
}

void plugin_instance_set_pid(plugin_instance_t *inst, pid_t pid,
                             gboolean pinned)
{
    inst->tracked_pid = pid;
    inst->pinned      = pinned;
}

/* ── Dispatch callbacks ──────────────────────────────────────── */

void plugin_dispatch_update(plugin_registry_t *reg, pid_t pid,
                            const allmon_proc_data_t *data)
{
    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (!inst->plugin) continue;
        if (inst->tracked_pid != pid) continue;
        if (inst->plugin->update)
            inst->plugin->update(inst->plugin->plugin_ctx, data);
    }
}

void plugin_dispatch_clear(plugin_registry_t *reg, pid_t pid)
{
    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (!inst->plugin) continue;
        if (inst->tracked_pid != pid) continue;
        if (inst->plugin->clear)
            inst->plugin->clear(inst->plugin->plugin_ctx);
    }
}

void plugin_dispatch_clear_all(plugin_registry_t *reg)
{
    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (!inst->plugin) continue;
        if (inst->plugin->clear)
            inst->plugin->clear(inst->plugin->plugin_ctx);
    }
}

size_t plugin_collect_tracked_pids(const plugin_registry_t *reg,
                                   pid_t *out, size_t max_out)
{
    size_t n = 0;
    for (size_t i = 0; i < reg->count && n < max_out; i++) {
        const plugin_instance_t *inst = &reg->instances[i];
        if (!inst->plugin || inst->tracked_pid <= 0)
            continue;

        /* Check for duplicates */
        gboolean dup = FALSE;
        for (size_t j = 0; j < n; j++) {
            if (out[j] == inst->tracked_pid) {
                dup = TRUE;
                break;
            }
        }
        if (!dup)
            out[n++] = inst->tracked_pid;
    }
    return n;
}
