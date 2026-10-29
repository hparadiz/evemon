#ifndef EVEMON_QUERY_GPU_H
#define EVEMON_QUERY_GPU_H
#include "ipc_wire.h"
#include "../plugins/evemon_plugin.h"
void query_gpu_encode(const evemon_gpu_snapshot_t *snapshot, ipc_buf_t *out);
int query_gpu_decode(ipc_buf_t *in, evemon_gpu_snapshot_t *snapshot);
#endif
