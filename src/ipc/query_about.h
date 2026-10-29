#ifndef EVEMON_QUERY_ABOUT_H
#define EVEMON_QUERY_ABOUT_H
#include "ipc_wire.h"
#include "../plugins/evemon_plugin.h"
#include <gio/gio.h>

typedef struct {
    char hostname[256], distro_id[64], distro_name[256], kernel[128];
    char user[128], shell[128], cpu[256], gpu[2048];
    uint32_t cpus;
    uint64_t uptime, memory_total, memory_available;
} query_about_t;

/* Evemon-side metadata; called only on the inspector worker. */
void query_about_encode(ipc_buf_t *out, const evemon_gpu_snapshot_t *gpu);
void query_about_cpu_name(char *value, size_t capacity);
int query_about_decode(ipc_buf_t *in, query_about_t *info);
/* Capture the socket now; completion must never substitute local host data. */
void query_about_async(const char *path, GAsyncReadyCallback callback, gpointer data);
query_about_t *query_about_finish(GAsyncResult *result, GError **error);
#endif
