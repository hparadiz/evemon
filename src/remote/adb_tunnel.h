#ifndef EVEMON_ADB_TUNNEL_H
#define EVEMON_ADB_TUNNEL_H
#include <gio/gio.h>

typedef struct adb_tunnel adb_tunnel_t;

adb_tunnel_t *adb_tunnel_open(const char *serial, const char *socket,
                              int run_daemon, unsigned idle_seconds,
                              GCancellable *cancel, GError **error);
const char *adb_tunnel_path(const adb_tunnel_t *tunnel);
void adb_tunnel_activate(adb_tunnel_t *tunnel);
void adb_tunnel_close(adb_tunnel_t *tunnel);

#endif
