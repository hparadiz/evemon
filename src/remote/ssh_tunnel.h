#ifndef EVEMON_SSH_TUNNEL_H
#define EVEMON_SSH_TUNNEL_H
#include <gio/gio.h>
typedef struct ssh_tunnel ssh_tunnel_t;
/* Called before a step an ordinary connection does not need (a remote build
 * check, an installation, an upgrade): the caller should allow `seconds` more
 * before giving up on the attempt. */
typedef void (*ssh_tunnel_patience_fn)(gpointer data, unsigned seconds);
/* Optionally installs and starts a private headless Evemon as the SSH login
 * user. No sudo or privilege escalation is attempted. An Evemon installed
 * this way is replaced when this build is newer; any other Evemon found on
 * the host is used as it is. `patience` may be NULL. */
ssh_tunnel_t *ssh_tunnel_open(const char *host, const char *socket,
                              int run_daemon, unsigned idle_seconds,
                              ssh_tunnel_patience_fn patience,
                              gpointer patience_data,
                              GCancellable *cancel, GError **error);
const char *ssh_tunnel_path(const ssh_tunnel_t *tunnel);
void ssh_tunnel_activate(ssh_tunnel_t *tunnel);
void ssh_tunnel_close(ssh_tunnel_t *tunnel);
#endif
