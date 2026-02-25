/*
 * devices.c – sysfs device name resolution and caching.
 *
 * Resolves /dev/ paths to human-readable hardware names by querying
 * sysfs attributes and the PCI IDs database.  Results are cached in
 * a simple linear table so each sysfs path is read at most once per
 * evemon session.
 */

#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── tiny name cache ──────────────────────────────────────────── */

#define DEV_CACHE_MAX 256

typedef struct {
    char key[128];
    char name[256];
} dev_cache_entry_t;

static dev_cache_entry_t g_dev_cache[DEV_CACHE_MAX];
static size_t            g_dev_cache_count = 0;

static const char *dev_cache_get(const char *key)
{
    for (size_t i = 0; i < g_dev_cache_count; i++)
        if (strcmp(g_dev_cache[i].key, key) == 0)
            return g_dev_cache[i].name;
    return NULL;
}

static const char *dev_cache_put(const char *key, const char *name)
{
    if (g_dev_cache_count >= DEV_CACHE_MAX) return name;
    dev_cache_entry_t *e = &g_dev_cache[g_dev_cache_count++];
    snprintf(e->key,  sizeof(e->key),  "%s", key);
    snprintf(e->name, sizeof(e->name), "%s", name);
    return e->name;
}

/* Read a single-line sysfs file into buf, stripping trailing newline.
 * Returns 1 on success, 0 on failure. */
static int read_sysfs_line(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    buf[0] = '\0';
    if (fgets(buf, (int)bufsz, f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == ' '))
            buf[--len] = '\0';
    }
    fclose(f);
    return buf[0] != '\0';
}

/* ── PCI IDs database lookup ──────────────────────────────────── */

static const char *pci_ids_paths[] = {
    "/usr/share/hwdata/pci.ids",
    "/usr/share/misc/pci.ids",
    "/usr/local/share/hwdata/pci.ids",
    NULL
};

static int lookup_pci_id(unsigned vendor, unsigned device,
                         char *vname, size_t vnsz,
                         char *dname, size_t dnsz)
{
    FILE *f = NULL;
    for (const char **p = pci_ids_paths; *p; p++) {
        f = fopen(*p, "r");
        if (f) break;
    }
    if (!f) return 0;

    char line[512];
    int found_vendor = 0;
    vname[0] = '\0';
    dname[0] = '\0';

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        if (!found_vendor) {
            if (line[0] != '\t') {
                unsigned v;
                if (sscanf(line, "%x", &v) == 1 && v == vendor) {
                    found_vendor = 1;
                    char *nm = strchr(line, ' ');
                    if (nm) {
                        while (*nm == ' ') nm++;
                        snprintf(vname, vnsz, "%s", nm);
                        size_t len = strlen(vname);
                        while (len > 0 && (vname[len-1]=='\n'||vname[len-1]==' '))
                            vname[--len] = '\0';
                    }
                }
            }
        } else {
            if (line[0] != '\t') break;
            if (line[0] == '\t' && line[1] != '\t') {
                unsigned d;
                if (sscanf(line + 1, "%x", &d) == 1 && d == device) {
                    char *nm = strchr(line + 1, ' ');
                    if (nm) {
                        while (*nm == ' ') nm++;
                        snprintf(dname, dnsz, "%s", nm);
                        size_t len = strlen(dname);
                        while (len > 0 && (dname[len-1]=='\n'||dname[len-1]==' '))
                            dname[--len] = '\0';
                    }
                    fclose(f);
                    return 1;
                }
            }
        }
    }
    fclose(f);
    return found_vendor;
}

/* ── resolve DRI device name via sysfs + pci.ids ────────────── */

static const char *resolve_dri_name(const char *dri_name)
{
    const char *cached = dev_cache_get(dri_name);
    if (cached) return cached;

    char uevent_path[256];
    snprintf(uevent_path, sizeof(uevent_path),
             "/sys/class/drm/%s/device/uevent", dri_name);

    unsigned vendor = 0, device = 0;
    FILE *f = fopen(uevent_path, "r");
    if (!f) return NULL;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "PCI_ID=%x:%x", &vendor, &device) == 2)
            break;
    }
    fclose(f);

    if (vendor == 0) return NULL;

    char vname[128] = "", dname[192] = "";
    lookup_pci_id(vendor, device, vname, sizeof(vname), dname, sizeof(dname));

    char result[256];
    if (dname[0])
        snprintf(result, sizeof(result), "%s", dname);
    else if (vname[0])
        snprintf(result, sizeof(result), "%s [%04x:%04x]", vname, vendor, device);
    else
        return NULL;

    return dev_cache_put(dri_name, result);
}

/* ── resolve sound device name via sysfs ─────────────────────── */

static const char *resolve_snd_name(const char *snd_node)
{
    const char *cached = dev_cache_get(snd_node);
    if (cached) return cached;

    const char *p = snd_node;
    int card_num = -1;
    while (*p) {
        if (*p == 'C' || *p == 'D') {
            char *end;
            long n = strtol(p + 1, &end, 10);
            if (end != p + 1) { card_num = (int)n; break; }
        }
        p++;
    }
    if (card_num < 0) return NULL;

    char id_path[128];
    snprintf(id_path, sizeof(id_path), "/sys/class/sound/card%d/id", card_num);

    char name[128] = "";
    if (!read_sysfs_line(id_path, name, sizeof(name))) return NULL;

    return dev_cache_put(snd_node, name);
}

/* ── resolve input device name via sysfs ─────────────────────── */

static const char *resolve_input_name(const char *input_node)
{
    const char *cached = dev_cache_get(input_node);
    if (cached) return cached;

    char name_path[128];
    snprintf(name_path, sizeof(name_path),
             "/sys/class/input/%s/device/name", input_node);

    char name[256] = "";
    if (!read_sysfs_line(name_path, name, sizeof(name))) return NULL;

    return dev_cache_put(input_node, name);
}

/* ── resolve block device model via sysfs ────────────────────── */

static const char *resolve_block_name(const char *blk_name)
{
    const char *cached = dev_cache_get(blk_name);
    if (cached) return cached;

    char base[64];
    snprintf(base, sizeof(base), "%s", blk_name);
    size_t len = strlen(base);

    if (strncmp(base, "nvme", 4) == 0) {
        char *pp = strrchr(base, 'p');
        if (pp && pp > base + 4 && pp[-1] >= '0' && pp[-1] <= '9'
            && pp[1] >= '0' && pp[1] <= '9')
            *pp = '\0';
    } else {
        while (len > 0 && base[len-1] >= '0' && base[len-1] <= '9')
            base[--len] = '\0';
    }

    char model_path[128];
    snprintf(model_path, sizeof(model_path),
             "/sys/block/%s/device/model", base);

    char model[128] = "";
    if (!read_sysfs_line(model_path, model, sizeof(model))) return NULL;

    return dev_cache_put(blk_name, model);
}

/* ── resolve video4linux device name via sysfs ───────────────── */

static const char *resolve_video_name(const char *v4l_node)
{
    const char *cached = dev_cache_get(v4l_node);
    if (cached) return cached;

    char name_path[128];
    snprintf(name_path, sizeof(name_path),
             "/sys/class/video4linux/%s/name", v4l_node);

    char name[256] = "";
    if (!read_sysfs_line(name_path, name, sizeof(name))) return NULL;

    return dev_cache_put(v4l_node, name);
}

/* ── public: label a /dev/ path ──────────────────────────────── */

void label_device(const char *path, char *desc, size_t descsz)
{
    const char *after_dev = path + 5;   /* skip "/dev/" */

    /* ── null / zero / full / random ─────────────────────────── */
    if (strcmp(after_dev, "null") == 0) {
        snprintf(desc, descsz, "%s  (null sink)", path); return;
    }
    if (strcmp(after_dev, "zero") == 0) {
        snprintf(desc, descsz, "%s  (zero source)", path); return;
    }
    if (strcmp(after_dev, "full") == 0) {
        snprintf(desc, descsz, "%s  (always-full sink)", path); return;
    }
    if (strcmp(after_dev, "random") == 0 ||
        strcmp(after_dev, "urandom") == 0) {
        snprintf(desc, descsz, "%s  (random number generator)", path); return;
    }

    /* ── terminals & PTYs ────────────────────────────────────── */
    if (strncmp(after_dev, "pts/", 4) == 0) {
        snprintf(desc, descsz, "%s  (pseudo-terminal)", path); return;
    }
    if (strcmp(after_dev, "ptmx") == 0) {
        snprintf(desc, descsz, "%s  (PTY multiplexer)", path); return;
    }
    if (strncmp(after_dev, "tty", 3) == 0) {
        if (after_dev[3] == '\0')
            snprintf(desc, descsz, "%s  (controlling terminal)", path);
        else if (after_dev[3] == 'S')
            snprintf(desc, descsz, "%s  (serial port)", path);
        else
            snprintf(desc, descsz, "%s  (virtual console)", path);
        return;
    }
    if (strcmp(after_dev, "console") == 0) {
        snprintf(desc, descsz, "%s  (system console)", path); return;
    }

    /* ── shared memory ───────────────────────────────────────── */
    if (strncmp(after_dev, "shm/", 4) == 0) {
        snprintf(desc, descsz, "%s  (shared memory)", path); return;
    }

    /* ── GPU / DRI ───────────────────────────────────────────── */
    if (strncmp(after_dev, "dri/", 4) == 0) {
        const char *dri_node = after_dev + 4;
        const char *hw_name = resolve_dri_name(dri_node);
        const char *type = strncmp(dri_node, "renderD", 7) == 0
                           ? "GPU render" : "GPU";
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s: %s)", path, type, hw_name);
        else
            snprintf(desc, descsz, "%s  (%s node)", path, type);
        return;
    }
    if (strncmp(after_dev, "nvidia", 6) == 0) {
        snprintf(desc, descsz, "%s  (NVIDIA GPU)", path); return;
    }

    /* ── sound ───────────────────────────────────────────────── */
    if (strncmp(after_dev, "snd/", 4) == 0) {
        const char *snd_node = after_dev + 4;
        const char *hw_name = resolve_snd_name(snd_node);
        const char *type;
        if (strncmp(snd_node, "control", 7) == 0)     type = "audio control";
        else if (strncmp(snd_node, "pcm", 3) == 0)    type = "audio PCM";
        else if (strncmp(snd_node, "timer", 5) == 0)   type = "audio timer";
        else if (strncmp(snd_node, "seq", 3) == 0)     type = "MIDI sequencer";
        else                                            type = "audio";
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s: %s)", path, type, hw_name);
        else
            snprintf(desc, descsz, "%s  (%s)", path, type);
        return;
    }

    /* ── input devices ───────────────────────────────────────── */
    if (strncmp(after_dev, "input/", 6) == 0) {
        const char *inp_node = after_dev + 6;
        const char *hw_name = resolve_input_name(inp_node);
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s)", path, hw_name);
        else if (strncmp(inp_node, "event", 5) == 0)
            snprintf(desc, descsz, "%s  (input event)", path);
        else if (strncmp(inp_node, "mouse", 5) == 0)
            snprintf(desc, descsz, "%s  (mouse)", path);
        else if (strncmp(inp_node, "js", 2) == 0)
            snprintf(desc, descsz, "%s  (joystick)", path);
        else
            snprintf(desc, descsz, "%s  (input device)", path);
        return;
    }

    /* ── video / camera ──────────────────────────────────────── */
    if (strncmp(after_dev, "video", 5) == 0) {
        const char *v4l_node = after_dev;
        const char *hw_name = resolve_video_name(v4l_node);
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s)", path, hw_name);
        else
            snprintf(desc, descsz, "%s  (video/camera)", path);
        return;
    }

    /* ── FUSE ────────────────────────────────────────────────── */
    if (strcmp(after_dev, "fuse") == 0) {
        snprintf(desc, descsz, "%s  (FUSE filesystem)", path); return;
    }

    /* ── block devices ───────────────────────────────────────── */
    if (strncmp(after_dev, "sd", 2) == 0 ||
        strncmp(after_dev, "nvme", 4) == 0 ||
        strncmp(after_dev, "vd", 2) == 0 ||
        strncmp(after_dev, "xvd", 3) == 0) {
        const char *hw_name = resolve_block_name(after_dev);
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s)", path, hw_name);
        else
            snprintf(desc, descsz, "%s  (block storage)", path);
        return;
    }
    if (strncmp(after_dev, "dm-", 3) == 0 ||
        strncmp(after_dev, "mapper/", 7) == 0) {
        snprintf(desc, descsz, "%s  (device-mapper)", path); return;
    }
    if (strncmp(after_dev, "loop", 4) == 0) {
        snprintf(desc, descsz, "%s  (loop device)", path); return;
    }

    /* ── network ─────────────────────────────────────────────── */
    if (strncmp(after_dev, "net/", 4) == 0) {
        snprintf(desc, descsz, "%s  (network device)", path); return;
    }
    if (strcmp(after_dev, "rfkill") == 0) {
        snprintf(desc, descsz, "%s  (RF kill switch)", path); return;
    }

    /* ── hardware RNG ────────────────────────────────────────── */
    if (strncmp(after_dev, "hwrng", 5) == 0) {
        snprintf(desc, descsz, "%s  (hardware RNG)", path); return;
    }

    /* ── KVM ─────────────────────────────────────────────────── */
    if (strcmp(after_dev, "kvm") == 0) {
        snprintf(desc, descsz, "%s  (KVM hypervisor)", path); return;
    }

    /* ── misc / VFIO / iommu ─────────────────────────────────── */
    if (strncmp(after_dev, "vfio/", 5) == 0) {
        snprintf(desc, descsz, "%s  (VFIO passthrough)", path); return;
    }
    if (strncmp(after_dev, "hugepages", 9) == 0) {
        snprintf(desc, descsz, "%s  (huge pages)", path); return;
    }
    if (strncmp(after_dev, "usb", 3) == 0 ||
        strncmp(after_dev, "bus/usb", 7) == 0) {
        snprintf(desc, descsz, "%s  (USB device)", path); return;
    }

    /* ── fallback ────────────────────────────────────────────── */
    snprintf(desc, descsz, "%s", path);
}
