#include "query_about.h"
#include "query.h"
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

static void file_field(const char *path, const char *key, char *value, size_t capacity)
{
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[1024]; size_t length = strlen(key);
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, key, length) || line[length] != '=') continue;
        char *text = g_strstrip(line + length + 1);
        size_t n = strlen(text);
        if (n >= 2 && ((text[0] == '"' && text[n - 1] == '"') ||
                       (text[0] == '\'' && text[n - 1] == '\''))) {
            text[n - 1] = 0; ++text;
        }
        g_strlcpy(value, text, capacity); break;
    }
    fclose(file);
}

static void os_field(const char *key, char *value, size_t capacity)
{
    file_field("/etc/os-release", key, value, capacity);
    if (!value[0]) file_field("/usr/lib/os-release", key, value, capacity);
}

static uint64_t sysfs_number(const char *directory, const char *field)
{
    char *path = g_build_filename(directory, field, NULL), *text = NULL;
    uint64_t value = 0;
    if (g_file_get_contents(path, &text, NULL, NULL)) value = g_ascii_strtoull(text, NULL, 0);
    g_free(text); g_free(path); return value;
}

static void pci_name(unsigned vendor, unsigned device, char *name, size_t capacity)
{
    static const char *paths[] = {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids", "/usr/share/pci.ids"};
    FILE *file = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(paths) && !file; ++i) file = fopen(paths[i], "r");
    snprintf(name, capacity, "PCI %04x:%04x", vendor, device);
    if (!file) return;
    char line[1024], vendor_name[256] = "";
    unsigned id; int offset;
    while (fgets(line, sizeof(line), file)) {
        if (g_ascii_isxdigit(line[0]) && sscanf(line, "%x %n", &id, &offset) == 1) {
            if (vendor_name[0]) break;
            if (id == vendor) g_strlcpy(vendor_name, g_strstrip(line + offset), sizeof(vendor_name));
        } else if (vendor_name[0] && line[0] == '\t' && line[1] != '\t' &&
                   sscanf(line + 1, "%x %n", &id, &offset) == 1 && id == device) {
            snprintf(name, capacity, "%s %s", vendor_name, g_strstrip(line + 1 + offset));
            fclose(file); return;
        }
    }
    if (vendor_name[0]) snprintf(name, capacity, "%s %04x", vendor_name, device);
    fclose(file);
}

void query_about_cpu_name(char *value, size_t capacity)
{
    FILE *file = fopen("/proc/cpuinfo", "r"); char line[1024];
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            if ((!strcmp(g_strstrip(line), "model name") || !strcmp(line, "Hardware")) &&
                *g_strstrip(colon + 1)) {
                g_strlcpy(value, colon + 1, capacity); fclose(file); return;
            }
        }
        fclose(file);
    }
    char *machine = NULL, *family = NULL;
    if (g_file_get_contents("/sys/devices/soc0/machine", &machine, NULL, NULL) && *g_strstrip(machine)) {
        g_file_get_contents("/sys/devices/soc0/family", &family, NULL, NULL);
        if (family && *g_strstrip(family) && !g_str_has_prefix(machine, family))
            snprintf(value, capacity, "%s %s", family, machine);
        else g_strlcpy(value, machine, capacity);
        g_free(machine); g_free(family); return;
    }
    g_free(machine); machine = NULL;
    if (g_file_get_contents("/sys/firmware/devicetree/base/model", &machine, NULL, NULL) && *g_strstrip(machine)) {
        g_strlcpy(value, machine, capacity); g_free(machine); return;
    }
    g_free(machine);
    struct utsname kernel;
    g_strlcpy(value, !uname(&kernel) ? kernel.machine : "Unavailable", capacity);
}

static void gpu_names(char *value, size_t capacity, const evemon_gpu_snapshot_t *gpu)
{
    if (gpu && gpu->device_count) {
        GString *names = g_string_new(NULL);
        for (uint32_t i = 0; i < gpu->device_count; ++i) {
            const evemon_gpu_device_t *device = &gpu->devices[i];
            if (!device->name[0]) continue;
            if (names->len) g_string_append(names, ", ");
            g_string_append(names, device->name);
            char *directory = g_build_filename("/sys/bus/pci/devices", device->pdev, NULL);
            uint64_t memory = sysfs_number(directory, "mem_info_vram_total");
            if (!memory) memory = sysfs_number(directory, "lmem_total_bytes");
            if (memory) g_string_append_printf(names, " (%.0f GiB)", memory / (1024.0 * 1024 * 1024));
            g_free(directory);
        }
        if (names->len) {
            g_strlcpy(value, names->str, capacity); g_string_free(names, TRUE); return;
        }
        g_string_free(names, TRUE);
    }
    GDir *devices = g_dir_open("/sys/bus/pci/devices", 0, NULL);
    if (!devices) { g_strlcpy(value, "Unavailable", capacity); return; }
    GString *names = g_string_new(NULL);
    const char *slot;
    while ((slot = g_dir_read_name(devices))) {
        char *directory = g_build_filename("/sys/bus/pci/devices", slot, NULL);
        if (sysfs_number(directory, "class") >> 16 == 3) {
            char name[512];
            pci_name(sysfs_number(directory, "vendor"), sysfs_number(directory, "device"), name, sizeof(name));
            if (names->len) g_string_append(names, ", ");
            g_string_append(names, name);
            uint64_t memory = sysfs_number(directory, "mem_info_vram_total");
            if (!memory) memory = sysfs_number(directory, "lmem_total_bytes");
            if (memory) g_string_append_printf(names, " (%.0f GiB)", memory / (1024.0 * 1024 * 1024));
        }
        g_free(directory);
        if (names->len >= capacity - 1) break;
    }
    g_dir_close(devices);
    g_strlcpy(value, names->len ? names->str : "None detected", capacity);
    g_string_free(names, TRUE);
}

void query_about_encode(ipc_buf_t *out, const evemon_gpu_snapshot_t *gpu)
{
    query_about_t a = {0};
    gethostname(a.hostname, sizeof(a.hostname) - 1);
    os_field("ID", a.distro_id, sizeof(a.distro_id));
    os_field("PRETTY_NAME", a.distro_name, sizeof(a.distro_name));
    if (!a.distro_name[0]) os_field("NAME", a.distro_name, sizeof(a.distro_name));
    if (!a.distro_id[0]) {
        char android_version[64] = "";
        file_field("/system/build.prop", "ro.build.version.release", android_version, sizeof(android_version));
        if (!android_version[0] && access("/system/bin/getprop", X_OK) == 0) {
            char *argv[] = { "/system/bin/getprop", "ro.build.version.release", NULL };
            char *value = NULL; int status = -1;
            if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                             &value, NULL, &status, NULL) && status == 0)
                g_strlcpy(android_version, g_strstrip(value), sizeof(android_version));
            g_free(value);
        }
        if (android_version[0]) {
            g_strlcpy(a.distro_id, "android", sizeof(a.distro_id));
            if (!a.distro_name[0]) snprintf(a.distro_name, sizeof(a.distro_name), "Android %s", android_version);
        } else g_strlcpy(a.distro_id, "linux", sizeof(a.distro_id));
    }
    if (!a.distro_name[0]) g_strlcpy(a.distro_name, "Linux", sizeof(a.distro_name));
    struct utsname kernel;
    if (!uname(&kernel)) {
        g_strlcpy(a.kernel, kernel.release, sizeof(a.kernel));
    }
    struct sysinfo info;
    if (!sysinfo(&info)) {
        a.uptime = info.uptime;
        a.memory_total = (uint64_t)info.totalram * info.mem_unit;
        a.memory_available = (uint64_t)info.freeram * info.mem_unit;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN); a.cpus = cpus > 0 ? cpus : 1;
    struct passwd user, *found = NULL; char buffer[16384];
    if (!getpwuid_r(getuid(), &user, buffer, sizeof(buffer), &found) && found) {
        g_strlcpy(a.user, user.pw_name, sizeof(a.user));
        const char *base = strrchr(user.pw_shell, '/');
        g_strlcpy(a.shell, base ? base + 1 : user.pw_shell, sizeof(a.shell));
    }
    if (!a.user[0]) snprintf(a.user, sizeof(a.user), "%lu", (unsigned long)getuid());
    query_about_cpu_name(a.cpu, sizeof(a.cpu));
    FILE *file = fopen("/proc/meminfo", "r"); char line[1024];
    if (file) {
        unsigned long long value;
        while (fgets(line, sizeof(line), file)) {
            if (sscanf(line, "MemTotal: %llu kB", &value) == 1) a.memory_total = value * 1024;
            if (sscanf(line, "MemAvailable: %llu kB", &value) == 1) a.memory_available = value * 1024;
        }
        fclose(file);
    }
    gpu_names(a.gpu, sizeof(a.gpu), gpu);
    ipc_put_string(out, a.hostname); ipc_put_string(out, a.distro_id);
    ipc_put_string(out, a.distro_name); ipc_put_string(out, a.kernel);
    ipc_put_string(out, a.user); ipc_put_string(out, a.shell);
    ipc_put_string(out, a.cpu); ipc_put_string(out, a.gpu);
    ipc_put_u32(out, a.cpus); ipc_put_u64(out, a.uptime);
    ipc_put_u64(out, a.memory_total); ipc_put_u64(out, a.memory_available);
}

int query_about_decode(ipc_buf_t *in, query_about_t *a)
{
    memset(a, 0, sizeof(*a));
#define FIELD(name) ipc_get_fixed(in, a->name, sizeof(a->name))
    FIELD(hostname); FIELD(distro_id); FIELD(distro_name); FIELD(kernel);
    FIELD(user); FIELD(shell); FIELD(cpu); FIELD(gpu);
#undef FIELD
    a->cpus = ipc_get_u32(in); a->uptime = ipc_get_u64(in);
    a->memory_total = ipc_get_u64(in); a->memory_available = ipc_get_u64(in);
    if (!ipc_done(in) || !a->hostname[0] || !a->cpus || a->memory_available > a->memory_total) {
        errno = EPROTO; return -1;
    }
    return 0;
}

static void about_worker(GTask *task, gpointer source, gpointer path, GCancellable *cancel)
{
    (void)source; (void)cancel;
    ipc_buf_t request = {0}, response = {0};
    query_about_t *info = g_new0(query_about_t, 1);
    int result = query_exchange(path, IPC_ABOUT, &request, &response);
    if (!result) result = query_about_decode(&response, info);
    int error = errno; ipc_free(&response);
    if (result) {
        g_free(info);
        g_task_return_new_error(task, G_IO_ERROR, g_io_error_from_errno(error),
                               "Could not load this machine's system information: %s", g_strerror(error));
    } else g_task_return_pointer(task, info, g_free);
}

void query_about_async(const char *path, GAsyncReadyCallback callback, gpointer data)
{
    GTask *task = g_task_new(NULL, NULL, callback, data);
    g_task_set_task_data(task, g_strdup(path), g_free);
    if (!(query_remote_capabilities() & IPC_CAP_ABOUT))
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "This Evemon version does not provide system information for About.");
    else g_task_run_in_thread(task, about_worker);
    g_object_unref(task);
}

query_about_t *query_about_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}
