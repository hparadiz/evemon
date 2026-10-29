/* Translate pinned nvtop 3.3.2's multi-vendor backend model into Evemon's
 * event-bus GPU snapshot ABI. */

#include "gpu_nvtop_adapter.h"
#include "../ipc/query.h"
#include "../ipc/query_gpu.h"

#include "list.h"
#include "nvtop/extract_gpuinfo.h"
#include "nvtop/extract_gpuinfo_common.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct evemon_gpu_backend {
    struct list_head devices;
    unsigned         device_count;
    int              initialized;
    int              remote;
};

static const char *backend_driver_name(const char *vendor)
{
    if (!vendor) return "nvtop";
    if (strcmp(vendor, "NVIDIA") == 0) return "NVML";
    if (strcmp(vendor, "AMD") == 0) return "amdgpu";
    if (strcmp(vendor, "Intel") == 0) return "i915/xe";
    if (strcmp(vendor, "apple") == 0) return "Metal";
    if (strcmp(vendor, "Ascend") == 0) return "DCMI";
    if (strcmp(vendor, "TPU") == 0) return "libtpuinfo";
    if (strcmp(vendor, "RK-NPU") == 0) return "rknpu";
    if (strcmp(vendor, "METAX") == 0) return "MXSML";
    if (strcmp(vendor, "ENFLAME") == 0) return "EFML";
    return vendor;
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void copy_bounded_text(char *dst, size_t dst_size,
                              const char *src, size_t src_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t length = strnlen(src, src_size);
    if (length >= dst_size) length = dst_size - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static void copy_device(evemon_gpu_device_t *out,
                        const struct gpu_info *device)
{
    const struct gpuinfo_static_info *st = &device->static_info;
    const struct gpuinfo_dynamic_info *dyn = &device->dynamic_info;
    const char *vendor = device->vendor ? device->vendor->name : "GPU";

    memset(out, 0, sizeof(*out));
    copy_text(out->vendor, sizeof(out->vendor), vendor);
    copy_text(out->driver, sizeof(out->driver),
              backend_driver_name(vendor));
    copy_bounded_text(out->pdev, sizeof(out->pdev), device->pdev, PDEV_LEN);
    if (GPUINFO_STATIC_FIELD_VALID(st, device_name))
        copy_bounded_text(out->name, sizeof(out->name), st->device_name,
                          MAX_DEVICE_NAME);
    else if (out->pdev[0])
        snprintf(out->name, sizeof(out->name), "%s %s", vendor,
                 out->pdev);
    else
        snprintf(out->name, sizeof(out->name), "%s device", vendor);

    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, gpu_util_rate)) {
        out->utilization_percent = dyn->gpu_util_rate;
        out->valid |= EVEMON_GPU_VALID_UTILIZATION;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, total_memory)) {
        uint64_t used = 0;
        int have_used = 0;
        if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, used_memory)) {
            used = dyn->used_memory;
            have_used = 1;
        } else if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, free_memory) &&
                   dyn->free_memory <= dyn->total_memory) {
            used = dyn->total_memory - dyn->free_memory;
            have_used = 1;
        }
        if (have_used) {
            out->memory_total_bytes = dyn->total_memory;
            out->memory_used_bytes = used;
            out->valid |= EVEMON_GPU_VALID_MEMORY;
        }
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, gpu_temp)) {
        out->temperature_c = dyn->gpu_temp;
        out->valid |= EVEMON_GPU_VALID_TEMPERATURE;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, power_draw)) {
        out->power_mw = dyn->power_draw;
        out->valid |= EVEMON_GPU_VALID_POWER;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, power_draw_max)) {
        out->power_limit_mw = dyn->power_draw_max;
        out->valid |= EVEMON_GPU_VALID_POWER_LIMIT;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, fan_speed)) {
        out->fan_percent = dyn->fan_speed;
        out->valid |= EVEMON_GPU_VALID_FAN_PERCENT;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, fan_rpm)) {
        out->fan_rpm = dyn->fan_rpm;
        out->valid |= EVEMON_GPU_VALID_FAN_RPM;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, encoder_rate)) {
        out->encoder_percent = dyn->encoder_rate;
        out->valid |= EVEMON_GPU_VALID_ENCODER;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, gpu_clock_speed)) {
        out->core_clock_mhz = dyn->gpu_clock_speed;
        if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, gpu_clock_speed_max))
            out->core_clock_max_mhz = dyn->gpu_clock_speed_max;
        out->valid |= EVEMON_GPU_VALID_CORE_CLOCK;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, mem_clock_speed)) {
        out->memory_clock_mhz = dyn->mem_clock_speed;
        if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, mem_clock_speed_max))
            out->memory_clock_max_mhz = dyn->mem_clock_speed_max;
        out->valid |= EVEMON_GPU_VALID_MEM_CLOCK;
    }
    if (GPUINFO_DYNAMIC_FIELD_VALID(dyn, pcie_link_gen) &&
        GPUINFO_DYNAMIC_FIELD_VALID(dyn, pcie_link_width)) {
        out->pcie_generation = dyn->pcie_link_gen;
        out->pcie_width = dyn->pcie_link_width;
        out->valid |= EVEMON_GPU_VALID_PCIE_LINK;
    }
}

static void copy_process(evemon_gpu_process_t *out,
                         const struct gpu_process *process,
                         uint32_t device_index)
{
    memset(out, 0, sizeof(*out));
    out->device_index = device_index;
    out->pid = process->pid;
    if (GPUINFO_PROCESS_FIELD_VALID(process, gpu_usage)) {
        out->utilization_percent = process->gpu_usage;
        out->valid |= EVEMON_GPU_PROCESS_VALID_UTILIZATION;
    }
    if (GPUINFO_PROCESS_FIELD_VALID(process, gpu_memory_usage)) {
        out->memory_bytes = process->gpu_memory_usage;
        out->valid |= EVEMON_GPU_PROCESS_VALID_MEMORY;
    }
    if (GPUINFO_PROCESS_FIELD_VALID(process, encode_usage)) {
        out->encode_percent = process->encode_usage;
        out->valid |= EVEMON_GPU_PROCESS_VALID_ENCODE;
    }
    if (GPUINFO_PROCESS_FIELD_VALID(process, decode_usage)) {
        out->decode_percent = process->decode_usage;
        out->valid |= EVEMON_GPU_PROCESS_VALID_DECODE;
    }
    if (GPUINFO_PROCESS_FIELD_VALID(process, cmdline) && process->cmdline)
        copy_text(out->name, sizeof(out->name), process->cmdline);
    else
        snprintf(out->name, sizeof(out->name), "pid %d", (int)process->pid);
    if (GPUINFO_PROCESS_FIELD_VALID(process, user_name) && process->user_name)
        copy_text(out->user, sizeof(out->user), process->user_name);
}

static int process_compare(const void *lhs, const void *rhs)
{
    const evemon_gpu_process_t *a = lhs;
    const evemon_gpu_process_t *b = rhs;
    if (a->utilization_percent != b->utilization_percent)
        return a->utilization_percent < b->utilization_percent ? 1 : -1;
    if (a->memory_bytes != b->memory_bytes)
        return a->memory_bytes < b->memory_bytes ? 1 : -1;
    if (a->pid != b->pid) return a->pid > b->pid ? 1 : -1;
    if (a->device_index != b->device_index)
        return a->device_index > b->device_index ? 1 : -1;
    return 0;
}

evemon_gpu_backend_t *evemon_gpu_backend_new(void)
{
    evemon_gpu_backend_t *backend = calloc(1, sizeof(*backend));
    if (backend) INIT_LIST_HEAD(&backend->devices);
    return backend;
}

void evemon_gpu_backend_free(evemon_gpu_backend_t *backend)
{
    if (!backend) return;
    if (backend->initialized && !backend->remote)
        gpuinfo_shutdown_info_extraction(&backend->devices);
    free(backend);
}

uint32_t evemon_gpu_backend_init(evemon_gpu_backend_t *backend)
{
    if (backend && query_remote_path()) {
        backend->remote = 1; backend->initialized = 1;
        if (!(query_remote_capabilities() & IPC_CAP_GPU)) return 0;
        /* A compiled backend is not proof that this evemon found a GPU.
         * Only a successful snapshot with zero devices disables the plugin;
         * a transient transport failure must leave its normal retry running. */
        evemon_gpu_snapshot_t snapshot = {0};
        if (!evemon_gpu_backend_snapshot(backend, &snapshot, 0)) return 1;
        backend->device_count = snapshot.device_count;
        evemon_gpu_snapshot_clear(&snapshot);
        if (!backend->device_count)
            fputs("evemon: evemon detected no supported GPUs; GPU plugin disabled for this session\n", stderr);
        return backend->device_count;
    }
    if (!backend) return 0;
    if (backend->initialized && !backend->remote) return backend->device_count;

    INIT_LIST_HEAD(&backend->devices);
    backend->device_count = 0;
    if (!gpuinfo_init_info_extraction(&backend->device_count,
                                      &backend->devices) ||
        backend->device_count == 0)
        return 0;

    backend->initialized = 1;
    gpuinfo_populate_static_infos(&backend->devices);
    return backend->device_count;
}

void evemon_gpu_snapshot_clear(evemon_gpu_snapshot_t *snapshot)
{
    if (!snapshot) return;
    free(snapshot->devices);
    free(snapshot->processes);
    memset(snapshot, 0, sizeof(*snapshot));
}

int evemon_gpu_backend_snapshot(evemon_gpu_backend_t *backend,
                                evemon_gpu_snapshot_t *snapshot,
                                uint64_t monotonic_ns)
{
    if (backend && backend->remote) {
        ipc_buf_t request = {0}, response = {0};
        int rc = query_exchange(query_remote_path(), IPC_GPU, &request, &response);
        if (!rc) rc = query_gpu_decode(&response, snapshot);
        if (!rc) snapshot->monotonic_ns = monotonic_ns;
        ipc_free(&response); return rc == 0;
    }

    if (!backend || !backend->initialized || !snapshot) return 0;

    memset(snapshot, 0, sizeof(*snapshot));
    gpuinfo_refresh_dynamic_info(&backend->devices);
    gpuinfo_refresh_processes(&backend->devices);
    gpuinfo_utilisation_rate(&backend->devices);
    gpuinfo_fix_dynamic_info_from_process_info(&backend->devices);

    size_t device_count = 0;
    size_t process_count = 0;
    struct gpu_info *device;
    list_for_each_entry(device, &backend->devices, list) {
        if (device_count == UINT32_MAX ||
            process_count > UINT32_MAX - device->processes_count)
            return 0;
        device_count++;
        process_count += device->processes_count;
    }

    if (device_count > 0) {
        snapshot->devices = calloc(device_count, sizeof(*snapshot->devices));
        if (!snapshot->devices) return 0;
    }
    if (process_count > 0) {
        snapshot->processes = calloc(process_count,
                                     sizeof(*snapshot->processes));
        if (!snapshot->processes) {
            evemon_gpu_snapshot_clear(snapshot);
            return 0;
        }
    }

    snapshot->monotonic_ns = monotonic_ns;
    snapshot->device_count = (uint32_t)device_count;
    snapshot->process_count = (uint32_t)process_count;

    uint32_t device_index = 0;
    uint32_t process_index = 0;
    list_for_each_entry(device, &backend->devices, list) {
        copy_device(&snapshot->devices[device_index], device);
        for (unsigned i = 0; i < device->processes_count; i++)
            copy_process(&snapshot->processes[process_index++],
                         &device->processes[i], device_index);
        device_index++;
    }

    if (snapshot->process_count > 1)
        qsort(snapshot->processes, snapshot->process_count,
              sizeof(*snapshot->processes), process_compare);
    return 1;
}
