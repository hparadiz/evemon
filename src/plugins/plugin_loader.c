#include "../ipc/query.h"
/*
 * plugin_loader.c – Plugin discovery, loading, and lifecycle management.
 *
 * Scans a directory for .so files, calls dlopen/dlsym to load each one,
 * validates the ABI version, and manages the plugin instance registry.
 */

#include "plugin_loader.h"
#include "../platform/desktop_session.h"
#include "../core/settings.h"

#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

/* libelf — for manifest section inspection without dlopen */
#include <gelf.h>
#include <libelf.h>

static int plugin_built(const char *id)
{
    if (query_is_foreign_host() &&
        !(query_remote_capabilities() & IPC_CAP_PIPEWIRE) &&
        !strcmp(id, "org.evemon.pipewire")) return 0;
    if (query_is_foreign_host() && (!strcmp(id, "org.evemon.milkdrop") ||
                                  !strcmp(id, "org.evemon.audio_service") ||
                                  !strcmp(id, "org.evemon.gentoo"))) return 0;
    if (!EVEMON_WITH_GPU && !strcmp(id, "org.evemon.gpu")) return 0;
    if (!EVEMON_WITH_MILKDROP && !strcmp(id, "org.evemon.milkdrop")) return 0;
    if (!EVEMON_WITH_PIPEWIRE && (!strcmp(id, "org.evemon.pipewire") ||
                                  !strcmp(id, "org.evemon.audio_service"))) return 0;
    return 1;
}

/* ── Pre-load manifest reader ────────────────────────────────── */

int evemon_manifest_read(const char *so_path, evemon_plugin_manifest_t *out)
{
    if (!so_path || !out) return -1;

    /* One-time libelf version initialisation (idempotent) */
    elf_version(EV_CURRENT);

    int fd = open(so_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
    if (!elf) { close(fd); return -1; }

    /* Locate the section name string table index */
    size_t shstrndx = 0;
    if (elf_getshdrstrndx(elf, &shstrndx) != 0) {
        elf_end(elf); close(fd); return -1;
    }

    int found = 0;
    Elf_Scn *scn = NULL;
    while ((scn = elf_nextscn(elf, scn)) != NULL) {
        GElf_Shdr shdr;
        if (gelf_getshdr(scn, &shdr) != &shdr) continue;

        const char *sname = elf_strptr(elf, shstrndx, shdr.sh_name);
        if (!sname || strcmp(sname, ".evemon_manifest") != 0) continue;

        /* Found the section — copy raw bytes into *out */
        Elf_Data *data = elf_getdata(scn, NULL);
        if (!data || !data->d_buf ||
            data->d_size < sizeof(evemon_plugin_manifest_t)) {
            break;  /* section too small */
        }

        memcpy(out, data->d_buf, sizeof(evemon_plugin_manifest_t));
        found = 1;
        break;
    }

    elf_end(elf);
    close(fd);

    if (!found) return -1;

    /* Validate magic sentinel */
    if (out->magic != EVEMON_MANIFEST_MAGIC) return -2;

    /* Ensure all string fields are NUL-terminated (safety) */
    out->id     [sizeof(out->id)      - 1] = '\0';
    out->name   [sizeof(out->name)    - 1] = '\0';
    out->version[sizeof(out->version) - 1] = '\0';
    out->deps   [sizeof(out->deps)    - 1] = '\0';

    return 0;
}

const char *evemon_manifest_deps_next(const char **cursor)
{
    if (!cursor || !*cursor) return NULL;
    const char *p = *cursor;
    if (*p == '\0') return NULL;     /* double-NUL end-of-list */
    size_t len = strlen(p);
    *cursor = p + len + 1;           /* advance past this entry's NUL */
    return p;
}

/* ── Host-provided utility functions ─────────────────────────── */

/*
 * These are the implementations of the utility functions declared in
 * evemon_plugin.h.  They are resolved by plugins via dlopen(RTLD_GLOBAL)
 * because the host binary is linked with -rdynamic.
 */

/* Forward declarations from ui_internal.h / ui.c */
extern void format_memory(long kb, char *buf, size_t bufsz);
extern void format_fuzzy_time(time_t epoch, char *buf, size_t bufsz);

void evemon_format_memory(long kb, char *buf, size_t bufsz)
{
    format_memory(kb, buf, bufsz);
}

void evemon_format_fuzzy_time(time_t epoch, char *buf, size_t bufsz)
{
    format_fuzzy_time(epoch, buf, bufsz);
}

int evemon_net_io_get(const evemon_proc_data_t *data, pid_t tgid,
                      uint64_t *send_bytes, uint64_t *recv_bytes)
{
    if (!data || !data->fdmon) {
        if (send_bytes) *send_bytes = 0;
        if (recv_bytes) *recv_bytes = 0;
        return -1;
    }
    /* Forward to the real fdmon implementation.
     * We include fdmon.h to get the function signature. */
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
                                       const evemon_host_services_t *svc)
{
    reg->host_services = svc;
}

void plugin_registry_destroy(plugin_registry_t *reg)
{
    /* Pass 1: call every plugin's destroy() callback and free descriptors.
     * We must finish ALL destroy() calls before any dlclose(), because a
     * plugin's destroy function may reside in a .so shared with (or
     * depended on by) another plugin.                                    */
    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (inst->plugin && inst->plugin->destroy)
            inst->plugin->destroy(inst->plugin->plugin_ctx);
        /* Free the heap-allocated plugin descriptor (C1) */
        free(inst->plugin);
        inst->plugin = NULL;
    }

    /* Pass 2: close dlopen handles, deduplicating to avoid double-close (M4) */
    void **closed_handles = calloc(reg->count, sizeof(void *));
    size_t nclosed = 0;

    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (inst->handle) {
            int already = 0;
            if (closed_handles) {
                for (size_t j = 0; j < nclosed; j++)
                    if (closed_handles[j] == inst->handle)
                        { already = 1; break; }
            }
            if (!already) {
                dlclose(inst->handle);
                if (closed_handles)
                    closed_handles[nclosed++] = inst->handle;
            }
        }
    }
    free(closed_handles);
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
    inst->pinned      = 0;
    inst->instance_id = g_next_instance_id++;
    return inst;
}

/* ── Plugin loading ──────────────────────────────────────────── */

static int plugin_directory_is_trusted(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode) || !desktop_session_trusts_owner(st.st_uid) ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) {
        fprintf(stderr,
                "evemon: refusing plugin directory %s "
                "(owner uid %u, mode %04o)\n",
                dir, (unsigned)st.st_uid,
                (unsigned)(st.st_mode & 07777));
        return 0;
    }
    return 1;
}

static int plugin_file_is_trusted(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        !desktop_session_trusts_owner(st.st_uid) ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) {
        if (lstat(path, &st) == 0) {
            fprintf(stderr,
                    "evemon: refusing plugin %s "
                    "(owner uid %u, mode %04o)\n",
                    path, (unsigned)st.st_uid,
                    (unsigned)(st.st_mode & 07777));
        }
        return 0;
    }
    return 1;
}

/*
 * Load a single .so plugin file.
 * Returns 0 on success, -1 on failure.
 */
static int load_single_plugin(plugin_registry_t *reg, const char *path)
{
    if (!plugin_file_is_trusted(path))
        return -1;
    evemon_plugin_manifest_t manifest;
    if (evemon_manifest_read(path, &manifest) == 0 && !plugin_built(manifest.id)) return 0;
    /* Use RTLD_NOW to catch unresolved symbols immediately,
     * and RTLD_LOCAL so plugin symbols don't pollute the global namespace */
    /* Registered GTK GTypes retain class/vtable addresses for the process
     * lifetime, including after a host's widgets and contexts are gone. */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!handle) {
        fprintf(stderr, "evemon: plugin %s: %s\n", path, dlerror());
        return -1;
    }

    evemon_plugin_init_fn init_fn =
        (evemon_plugin_init_fn)dlsym(handle, "evemon_plugin_init");
    if (!init_fn) {
        fprintf(stderr, "evemon: plugin %s: no evemon_plugin_init symbol\n",
                path);
        dlclose(handle);
        return -1;
    }

    evemon_plugin_t *plugin = init_fn();
    if (!plugin) {
        fprintf(stderr, "evemon: plugin %s: init returned NULL\n", path);
        dlclose(handle);
        return -1;
    }

    if (plugin->abi_version != evemon_PLUGIN_ABI_VERSION) {
        fprintf(stderr, "evemon: plugin %s: ABI version %d (expected %d)\n",
                path, plugin->abi_version, evemon_PLUGIN_ABI_VERSION);
        dlclose(handle);
        return -1;
    }

    /* Check settings: skip plugins the user has disabled */
    if (plugin->id && (!plugin_built(plugin->id) || !settings_plugin_enabled(plugin->id))) {
        fprintf(stdout, "evemon: skipping disabled plugin \"%s\" (%s)\n",
                plugin->name ? plugin->name : "?",
                plugin->id);
        if (plugin->destroy)
            plugin->destroy(plugin->plugin_ctx);
        else
            free(plugin);
        dlclose(handle);
        return 0;  /* not an error — intentionally skipped */
    }

    /* UI plugins must provide create_widget and update.
     * Headless plugins (services) may omit create_widget. */
    /* SERVICE role plugins need at minimum an activate callback
     * to receive host services (including the event bus). */
    if (plugin->role == EVEMON_ROLE_SERVICE) {
        /* Service plugins need at minimum an activate callback
         * to receive host services (including the event bus). */
        if (!plugin->activate) {
            fprintf(stderr,
                    "evemon: plugin %s: service plugin missing activate()\n",
                    path);
            dlclose(handle);
            return -1;
        }
    } else {
        if (!plugin->create_widget || !plugin->update) {
            fprintf(stderr, "evemon: plugin %s: missing required callbacks\n",
                    path);
            dlclose(handle);
            return -1;
        }
    }

    /* Create the first instance */
    plugin_instance_t *inst = registry_alloc(reg);
    if (!inst) {
        dlclose(handle);
        return -1;
    }

    inst->plugin    = plugin;
    inst->handle    = handle;
    inst->is_active = 1;  /* default: lives in a notebook tab */
    snprintf(inst->so_path, sizeof(inst->so_path), "%s", path);

    /* SYSTEM role plugins are always-active and track PID 1 (init/systemd) */
    if (plugin->role == EVEMON_ROLE_SYSTEM)
        inst->tracked_pid = 1;

    if (plugin->role == EVEMON_ROLE_SERVICE) {
        /* Service plugins have no widget */
        inst->widget = NULL;
    } else {
        /* Call create_widget on the GTK main thread (we are on it) */
        inst->widget = plugin->create_widget(plugin->plugin_ctx);
        if (!inst->widget) {
            fprintf(stderr,
                    "evemon: plugin %s: create_widget returned NULL\n",
                    path);
            reg->count--;  /* undo the alloc */
            dlclose(handle);
            return -1;
        }
    }

    /* Inject host services if the plugin wants them */
    if (plugin->activate && reg->host_services)
        plugin->activate(plugin->plugin_ctx, reg->host_services);

    /* Update combined needs */
    reg->combined_needs |= plugin->data_needs;

    fprintf(stdout, "evemon: loaded %s plugin \"%s\" (%s) v%s from %s\n",
            plugin->role == EVEMON_ROLE_SERVICE ? "service" :
            plugin->role == EVEMON_ROLE_SYSTEM  ? "system"  : "process",
            plugin->name ? plugin->name : "?",
            plugin->id   ? plugin->id   : "?",
            plugin->version ? plugin->version : "?",
            path);

    return 0;
}

int plugin_loader_scan(plugin_registry_t *reg, const char *dir)
{
    if (!plugin_directory_is_trusted(dir))
        return 0;

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
        int n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;

        if (load_single_plugin(reg, path) == 0)
            loaded++;
    }

    closedir(dp);
    return loaded;
}

/* ── Asynchronous plugin scan ────────────────────────────────── */

/*
 * One pending plugin result, heap-allocated by the worker thread and
 * freed after the main-thread idle callback has processed it.
 */
static pthread_t g_scan_thread;
static int g_scan_joinable;
static atomic_int g_scan_cancelled;
void plugin_loader_cancel_scan(void)
{
    atomic_store(&g_scan_cancelled, 1);
    if (g_scan_joinable) {
        pthread_join(g_scan_thread, NULL);
        g_scan_joinable = 0;
    }
}

typedef struct {
    evemon_plugin_t *plugin;
    void *handle;
    char path[4096];
    plugin_registry_t  *reg;
    int                 inst_id;   /* -1 = scan-done sentinel */
    int                 n_loaded;  /* only valid for sentinel  */
    plugin_loaded_cb    on_loaded;
    plugin_scan_done_cb on_done;
    void               *user_data;
} async_plugin_result_t;

static int async_loaded_idle(void *data)
{
    async_plugin_result_t *r = data;
    if (r->inst_id == -1) {
        if (r->on_done) r->on_done(r->n_loaded, r->user_data);
    } else {
        int enabled = !atomic_load(&g_scan_cancelled) &&
                      (!r->plugin->id || settings_plugin_enabled(r->plugin->id));
        plugin_instance_t *inst = enabled ? registry_alloc(r->reg) : NULL;
        if (inst) {
            inst->plugin = r->plugin;
            inst->handle = r->handle;
            inst->is_active = 1;
            snprintf(inst->so_path, sizeof(inst->so_path), "%s", r->path);
            if (r->plugin->role == EVEMON_ROLE_SYSTEM) inst->tracked_pid = 1;
            r->reg->combined_needs |= r->plugin->data_needs;
            if (r->on_loaded) r->on_loaded(r->reg, inst->instance_id, r->user_data);
        } else {
            if (r->plugin->destroy) r->plugin->destroy(r->plugin->plugin_ctx);
            free(r->plugin);
            dlclose(r->handle);
        }
    }
    free(r);
    return 0;  /* G_SOURCE_REMOVE */
}

typedef struct {
    plugin_registry_t  *reg;
    char                dir[4096];
    plugin_post_fn      post_fn;
    plugin_loaded_cb    on_loaded;
    plugin_scan_done_cb on_done;
    void               *user_data;
} async_scan_args_t;

/*
 * Background pthread worker.  Scans the directory, dlopen-ing each .so
 * and calling evemon_plugin_init().  Widget creation is deliberately
 * deferred: each result is posted to the main thread via post_fn,
 * where on_loaded() will call create_widget() and add the tab.
 */
static void *async_scan_worker(void *arg)
{
    async_scan_args_t *a = arg;
    pthread_setname_np(pthread_self(), "ev-scan");

    int n_loaded = 0;

    if (!plugin_directory_is_trusted(a->dir))
        goto done;

    {
        DIR *dp = opendir(a->dir);
        if (!dp)
            goto done;

        struct dirent *de;
        while (!atomic_load(&g_scan_cancelled) && (de = readdir(dp)) != NULL) {
            size_t len = strlen(de->d_name);
            if (len < 4 || strcmp(de->d_name + len - 3, ".so") != 0)
                continue;

            char path[4096];
            int n = snprintf(path, sizeof(path), "%s/%s",
                             a->dir, de->d_name);
            if (n < 0 || (size_t)n >= sizeof(path) ||
                !plugin_file_is_trusted(path))
                continue;

            evemon_plugin_manifest_t manifest;
            if (evemon_manifest_read(path, &manifest) == 0 && !plugin_built(manifest.id)) continue;
            /* ── Step 1: dlopen + init (background thread) ────── */
            void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
            if (!handle) {
                fprintf(stderr, "evemon: plugin %s: %s\n", path, dlerror());
                continue;
            }

            evemon_plugin_init_fn init_fn =
                (evemon_plugin_init_fn)dlsym(handle, "evemon_plugin_init");
            if (!init_fn) {
                fprintf(stderr,
                        "evemon: plugin %s: no evemon_plugin_init\n", path);
                dlclose(handle);
                continue;
            }

            evemon_plugin_t *plugin = init_fn();
            if (!plugin) {
                fprintf(stderr,
                        "evemon: plugin %s: init returned NULL\n", path);
                dlclose(handle);
                continue;
            }

            if (plugin->abi_version != evemon_PLUGIN_ABI_VERSION) {
                fprintf(stderr,
                        "evemon: plugin %s: ABI version %d (expected %d)\n",
                        path, plugin->abi_version, evemon_PLUGIN_ABI_VERSION);
                if (plugin->destroy)
                    plugin->destroy(plugin->plugin_ctx);
                else
                    free(plugin);
                dlclose(handle);
                continue;
            }

            if (plugin->id && !plugin_built(plugin->id)) {
                fprintf(stdout,
                        "evemon: skipping disabled plugin \"%s\" (%s)\n",
                        plugin->name ? plugin->name : "?",
                        plugin->id);
                if (plugin->destroy)
                    plugin->destroy(plugin->plugin_ctx);
                else
                    free(plugin);
                dlclose(handle);
                continue;
            }

            /* Validate callbacks (service vs UI) */
            if (plugin->role == EVEMON_ROLE_SERVICE) {
                if (!plugin->activate) {
                    fprintf(stderr,
                            "evemon: plugin %s: service plugin missing "
                            "activate()\n", path);
                    if (plugin->destroy)
                        plugin->destroy(plugin->plugin_ctx);
                    else
                        free(plugin);
                    dlclose(handle);
                    continue;
                }
            } else {
                if (!plugin->create_widget || !plugin->update) {
                    fprintf(stderr,
                            "evemon: plugin %s: missing required callbacks\n",
                            path);
                    if (plugin->destroy)
                        plugin->destroy(plugin->plugin_ctx);
                    else
                        free(plugin);
                    dlclose(handle);
                    continue;
                }
            }

            /* Transfer ownership to the frontend; never mutate its registry here. */
            async_plugin_result_t *r = calloc(1, sizeof(*r));
            if (!r) {
                if (plugin->destroy) plugin->destroy(plugin->plugin_ctx);
                free(plugin);
                dlclose(handle);
                continue;
            }
            r->reg = a->reg;
            r->plugin = plugin;
            r->handle = handle;
            snprintf(r->path, sizeof(r->path), "%s", path);
            r->inst_id = 0;
            r->n_loaded  = 0;
            r->on_loaded = a->on_loaded;
            r->on_done   = a->on_done;
            r->user_data = a->user_data;
            a->post_fn(async_loaded_idle, r);
            n_loaded++;
        }
        closedir(dp);
    }

done:
    fprintf(stdout, "evemon: async scan done: %d plugin(s) from %s\n",
            n_loaded, a->dir);

    /* Post the scan-done sentinel */
    async_plugin_result_t *sentinel = calloc(1, sizeof(*sentinel));
    if (sentinel) {
        sentinel->reg       = a->reg;
        sentinel->inst_id   = -1;  /* sentinel */
        sentinel->n_loaded  = n_loaded;
        sentinel->on_loaded = a->on_loaded;
        sentinel->on_done   = a->on_done;
        sentinel->user_data = a->user_data;
        a->post_fn(async_loaded_idle, sentinel);
    }

    free(a);
    return NULL;
}

int plugin_loader_scan_async(plugin_registry_t  *reg,
                             const char         *dir,
                             plugin_post_fn      post_fn,
                             plugin_loaded_cb    on_loaded,
                             plugin_scan_done_cb on_done,
                             void               *user_data)
{
    if (!reg || !dir || !post_fn || g_scan_joinable) return -1;
    atomic_store(&g_scan_cancelled, 0);

    async_scan_args_t *a = malloc(sizeof(*a));
    if (!a) return -1;

    a->reg       = reg;
    a->post_fn   = post_fn;
    a->on_loaded = on_loaded;
    a->on_done   = on_done;
    a->user_data = user_data;
    snprintf(a->dir, sizeof(a->dir), "%s", dir);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int rc = pthread_create(&g_scan_thread, &attr, async_scan_worker, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(a);
        return -1;
    }
    g_scan_joinable = 1;
    return 0;
}

int plugin_instance_create(plugin_registry_t *reg, const char *plugin_id)
{
    /* Find an existing instance with this plugin id to get the handle */
    evemon_plugin_t *plugin = NULL;
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
    evemon_plugin_init_fn init_fn =
        (evemon_plugin_init_fn)dlsym(handle, "evemon_plugin_init");
    if (!init_fn) return -1;

    evemon_plugin_t *new_plugin = init_fn();
    if (!new_plugin) return -1;

    plugin_instance_t *inst = registry_alloc(reg);
    if (!inst) return -1;

    inst->plugin    = new_plugin;
    inst->handle    = handle;  /* shared handle — don't dlclose twice */
    inst->is_active = 1;  /* default: lives in a notebook tab */

    /* SYSTEM role plugins are always-active and track PID 1 (init/systemd) */
    if (new_plugin->role == EVEMON_ROLE_SYSTEM)
        inst->tracked_pid = 1;

    if (new_plugin->role == EVEMON_ROLE_SERVICE) {
        inst->widget = NULL;
    } else {
        inst->widget = new_plugin->create_widget(new_plugin->plugin_ctx);
        if (!inst->widget) {
            reg->count--;
            return -1;
        }
    }

    /* Inject host services if the plugin wants them */
    if (new_plugin->activate && reg->host_services)
        new_plugin->activate(new_plugin->plugin_ctx, reg->host_services);

    reg->combined_needs |= new_plugin->data_needs;

    return inst->instance_id;
}

int plugin_registry_find_by_id(const plugin_registry_t *reg, int instance_id)
{
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->instances[i].instance_id == instance_id)
            return (int)i;
    }
    return -1;
}

int plugin_instance_destroy(plugin_registry_t *reg, int instance_id)
{
    int instance_idx = plugin_registry_find_by_id(reg, instance_id);
    if (instance_idx < 0)
        return -1;

    plugin_instance_t *inst = &reg->instances[instance_idx];
    if (inst->plugin && inst->plugin->destroy)
        inst->plugin->destroy(inst->plugin->plugin_ctx);

    /* Free the heap-allocated plugin descriptor (C1) */
    free(inst->plugin);

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
    return 0;
}

void plugin_registry_recalc_needs(plugin_registry_t *reg)
{
    reg->combined_needs = 0;
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->instances[i].plugin)
            reg->combined_needs |= reg->instances[i].plugin->data_needs;
    }
}

evemon_data_needs_t plugin_registry_effective_needs(const plugin_registry_t *reg)
{
    evemon_data_needs_t needs = 0;
    for (size_t i = 0; i < reg->count; i++) {
        const plugin_instance_t *inst = &reg->instances[i];
        if (!inst->plugin) continue;
        if (inst->plugin->role != EVEMON_ROLE_SERVICE && !inst->widget)
            continue;
        /* No wants_update → always wants data */
        if (!inst->plugin->wants_update ||
            inst->plugin->wants_update(inst->plugin->plugin_ctx))
            needs |= inst->plugin->data_needs;
    }
    return needs;
}

int plugin_reload(plugin_registry_t *reg, const char *so_path)
{
    if (!reg || !so_path) return 0;

    /* Collect instance IDs that came from this .so, and the shared handle */
    int     ids[256];
    size_t  n_ids = 0;
    void   *handle = NULL;

    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (strcmp(inst->so_path, so_path) != 0) continue;
        if (n_ids < 256)
            ids[n_ids++] = inst->instance_id;
        if (!handle)
            handle = inst->handle;
    }

    /* Destroy all matching instances (plugin_instance_destroy compacts the
     * array each time, so iterate by id rather than by index) */
    for (size_t i = 0; i < n_ids; i++)
        plugin_instance_destroy(reg, ids[i]);

    /* Now dlclose the handle (no surviving instances share it any more) */
    if (handle)
        dlclose(handle);

    /* Re-load the .so as a fresh plugin */
    return (load_single_plugin(reg, so_path) == 0) ? 1 : 0;
}

void plugin_instance_set_pid(plugin_instance_t *inst, pid_t pid,
                             int pinned)
{
    inst->tracked_pid = pid;
    inst->pinned      = pinned;
}

void plugin_instance_set_active(plugin_instance_t *inst, int active)
{
    if (!inst) return;
    inst->is_active = active;
    if (inst->plugin && inst->plugin->set_active)
        inst->plugin->set_active(inst->plugin->plugin_ctx, active ? 1 : 0);
}

/* ── Dispatch callbacks ──────────────────────────────────────── */

void plugin_dispatch_update(plugin_registry_t *reg, pid_t pid,
                            const evemon_proc_data_t *data)
{
    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (!inst->plugin || !inst->plugin->update) continue;
        if (inst->plugin->role != EVEMON_ROLE_SERVICE && !inst->widget)
            continue;
        if (inst->tracked_pid != pid) continue;
        /* If the plugin opts in to demand-driven updates, ask it first */
        if (inst->plugin->wants_update &&
            !inst->plugin->wants_update(inst->plugin->plugin_ctx))
            continue;
        inst->plugin->update(inst->plugin->plugin_ctx, data);
    }
}

void plugin_dispatch_clear(plugin_registry_t *reg, pid_t pid)
{
    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (!inst->plugin || !inst->plugin->clear) continue;
        if (inst->tracked_pid != pid) continue;
        inst->plugin->clear(inst->plugin->plugin_ctx);
    }
}

void plugin_dispatch_clear_all(plugin_registry_t *reg)
{
    for (size_t i = 0; i < reg->count; i++) {
        plugin_instance_t *inst = &reg->instances[i];
        if (!inst->plugin || !inst->plugin->clear) continue;
        /* Skip pinned instances — they manage their own lifecycle
         * and their widgets may already be destroyed by GTK. */
        if (inst->pinned) continue;
        /* Skip floating-window instances — they track a fixed PID
         * independently of the tree selection and must not be cleared
         * when the user deselects a process. */
        if (!inst->is_active) continue;
        /* Skip SYSTEM role plugins — they always track PID 1 and
         * must never be cleared due to process selection changes. */
        if (inst->plugin->role == EVEMON_ROLE_SYSTEM) continue;
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
        if (inst->plugin->wants_update &&
            !inst->plugin->wants_update(inst->plugin->plugin_ctx))
            continue;

        /* Check for duplicates */
        int dup = 0;
        for (size_t j = 0; j < n; j++) {
            if (out[j] == inst->tracked_pid) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            out[n++] = inst->tracked_pid;
    }
    return n;
}
