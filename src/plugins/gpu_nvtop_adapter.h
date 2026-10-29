#ifndef EVEMON_GPU_NVTOP_ADAPTER_H
#define EVEMON_GPU_NVTOP_ADAPTER_H

#include "evemon_plugin.h"

#include <stdint.h>

typedef struct evemon_gpu_backend evemon_gpu_backend_t;

evemon_gpu_backend_t *evemon_gpu_backend_new(void);
void evemon_gpu_backend_free(evemon_gpu_backend_t *backend);

/* Initializes every backend compiled into pinned nvtop and returns the number
 * of devices discovered across all vendors. */
uint32_t evemon_gpu_backend_init(evemon_gpu_backend_t *backend);

/* Refreshes device and process data, allocating an exact-size snapshot. */
int evemon_gpu_backend_snapshot(evemon_gpu_backend_t *backend,
                                evemon_gpu_snapshot_t *snapshot,
                                uint64_t monotonic_ns);

void evemon_gpu_snapshot_clear(evemon_gpu_snapshot_t *snapshot);

#endif
