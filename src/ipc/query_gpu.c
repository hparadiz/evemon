#include "query_gpu.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
void query_gpu_encode(const evemon_gpu_snapshot_t *s, ipc_buf_t *b)
{
    ipc_put_u64(b, s->monotonic_ns);
    ipc_put_u32(b, s->device_count);
    for (uint32_t i = 0; i < s->device_count && !b->error; ++i) {
        const evemon_gpu_device_t *v = &s->devices[i];
        ipc_put_string(b, v->name);
        ipc_put_string(b, v->vendor);
        ipc_put_string(b, v->driver);
        ipc_put_string(b, v->pdev);
        ipc_put_u32(b, v->valid);
        ipc_put_u32(b, v->utilization_percent);
        ipc_put_u32(b, v->temperature_c);
        ipc_put_u32(b, v->power_mw);
        ipc_put_u32(b, v->power_limit_mw);
        ipc_put_u32(b, v->fan_percent);
        ipc_put_u32(b, v->fan_rpm);
        ipc_put_u32(b, v->core_clock_mhz);
        ipc_put_u32(b, v->core_clock_max_mhz);
        ipc_put_u32(b, v->memory_clock_mhz);
        ipc_put_u32(b, v->memory_clock_max_mhz);
        ipc_put_u32(b, v->pcie_generation);
        ipc_put_u32(b, v->pcie_width);
        ipc_put_u32(b, v->encoder_percent);
        ipc_put_u64(b, v->memory_total_bytes);
        ipc_put_u64(b, v->memory_used_bytes);
    }
    ipc_put_u32(b, s->process_count);
    for (uint32_t i = 0; i < s->process_count && !b->error; ++i) {
        const evemon_gpu_process_t *v = &s->processes[i];
        ipc_put_u32(b, v->device_index);
        ipc_put_u32(b, v->pid);
        ipc_put_u32(b, v->valid);
        ipc_put_u32(b, v->utilization_percent);
        ipc_put_u32(b, v->encode_percent);
        ipc_put_u32(b, v->decode_percent);
        ipc_put_u64(b, v->memory_bytes);
        ipc_put_string(b, v->name);
        ipc_put_string(b, v->user);
    }
}
int query_gpu_decode(ipc_buf_t *b, evemon_gpu_snapshot_t *out)
{
    evemon_gpu_snapshot_t s = {0};
    s.monotonic_ns = ipc_get_u64(b);
    s.device_count = ipc_get_u32(b);
    if (b->error || s.device_count > 65536 || s.device_count > (b->len - b->pos) / 88) goto invalid;
    s.devices = calloc(s.device_count ? s.device_count : 1, sizeof(*s.devices));
    if (!s.devices) goto invalid;
    for (uint32_t i = 0; i < s.device_count && !b->error; ++i) {
        evemon_gpu_device_t *v = &s.devices[i];
        ipc_get_fixed(b, v->name, sizeof(v->name));
        ipc_get_fixed(b, v->vendor, sizeof(v->vendor));
        ipc_get_fixed(b, v->driver, sizeof(v->driver));
        ipc_get_fixed(b, v->pdev, sizeof(v->pdev));
        v->valid = ipc_get_u32(b);
        v->utilization_percent = ipc_get_u32(b);
        v->temperature_c = ipc_get_u32(b);
        v->power_mw = ipc_get_u32(b);
        v->power_limit_mw = ipc_get_u32(b);
        v->fan_percent = ipc_get_u32(b);
        v->fan_rpm = ipc_get_u32(b);
        v->core_clock_mhz = ipc_get_u32(b);
        v->core_clock_max_mhz = ipc_get_u32(b);
        v->memory_clock_mhz = ipc_get_u32(b);
        v->memory_clock_max_mhz = ipc_get_u32(b);
        v->pcie_generation = ipc_get_u32(b);
        v->pcie_width = ipc_get_u32(b);
        v->encoder_percent = ipc_get_u32(b);
        v->memory_total_bytes = ipc_get_u64(b);
        v->memory_used_bytes = ipc_get_u64(b);
    }
    s.process_count = ipc_get_u32(b);
    if (b->error || s.process_count > 65536 || s.process_count > (b->len - b->pos) / 40) goto invalid;
    s.processes = calloc(s.process_count ? s.process_count : 1, sizeof(*s.processes));
    if (!s.processes) goto invalid;
    for (uint32_t i = 0; i < s.process_count && !b->error; ++i) {
        evemon_gpu_process_t *v = &s.processes[i];
        v->device_index = ipc_get_u32(b);
        v->pid = ipc_get_u32(b);
        v->valid = ipc_get_u32(b);
        v->utilization_percent = ipc_get_u32(b);
        v->encode_percent = ipc_get_u32(b);
        v->decode_percent = ipc_get_u32(b);
        v->memory_bytes = ipc_get_u64(b);
        ipc_get_fixed(b, v->name, sizeof(v->name));
        ipc_get_fixed(b, v->user, sizeof(v->user));
        if (v->device_index >= s.device_count) b->error = EPROTO;
    }
    if (!ipc_done(b)) goto invalid;
    *out = s; return 0;
invalid:
    free(s.devices); free(s.processes); errno = EPROTO; return -1;
}
