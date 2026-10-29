#ifndef EVEMON_SSH_TUNNEL_H
#define EVEMON_SSH_TUNNEL_H
#include <gio/gio.h>
typedef struct ssh_tunnel ssh_tunnel_t;
/* Optionally installs and starts a private headless Evemon as the SSH login
 * user. No sudo or privilege escalation is attempted. */
ssh_tunnel_t *ssh_tunnel_open(const char *host, const char *socket,
                              int run_daemon, unsigned idle_seconds,
                              GCancellable *cancel, GError **error);
const char *ssh_tunnel_path(const ssh_tunnel_t *tunnel);
void ssh_tunnel_activate(ssh_tunnel_t *tunnel);
void ssh_tunnel_close(ssh_tunnel_t *tunnel);
#endif
