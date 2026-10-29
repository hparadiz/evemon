#include "ipc_wire.h"
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

void ipc_free(ipc_buf_t *b) { free(b->data); memset(b, 0, sizeof(*b)); }
void ipc_put_bytes(ipc_buf_t *b, const void *p, size_t n)
{
    if (b->error) return;
    if (n > IPC_LIMIT - b->len) { b->error = EOVERFLOW; return; }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        if (cap < b->len + n) cap = b->len + n;
        if (cap > IPC_LIMIT) cap = IPC_LIMIT;
        void *next = realloc(b->data, cap);
        if (!next) { b->error = ENOMEM; return; }
        b->data = next; b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
}
void ipc_put_u32(ipc_buf_t *b, uint32_t v)
{
    unsigned char p[4] = {v >> 24, v >> 16, v >> 8, v};
    ipc_put_bytes(b, p, sizeof(p));
}
void ipc_put_u64(ipc_buf_t *b, uint64_t v)
{
    ipc_put_u32(b, v >> 32); ipc_put_u32(b, (uint32_t)v);
}
void ipc_put_f64(ipc_buf_t *b, double v)
{
    uint64_t bits;
    _Static_assert(sizeof(double) == sizeof(bits), "wire requires IEEE binary64");
    memcpy(&bits, &v, sizeof(bits)); ipc_put_u64(b, bits);
}
void ipc_put_string(ipc_buf_t *b, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > IPC_LIMIT) { b->error = EOVERFLOW; return; }
    ipc_put_u32(b, (uint32_t)n); ipc_put_bytes(b, s, n);
}
uint32_t ipc_get_u32(ipc_buf_t *b)
{
    if (b->error || b->len - b->pos < 4) { b->error = EPROTO; return 0; }
    const unsigned char *p = b->data + b->pos; b->pos += 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint64_t ipc_get_u64(ipc_buf_t *b)
{
    uint64_t hi = ipc_get_u32(b), lo = ipc_get_u32(b);
    return hi << 32 | lo;
}
double ipc_get_f64(ipc_buf_t *b)
{
    uint64_t bits = ipc_get_u64(b); double v;
    memcpy(&v, &bits, sizeof(v));
    if (!isfinite(v)) { b->error = EPROTO; return 0; }
    return v;
}
static size_t string_size(ipc_buf_t *b)
{
    size_t n = ipc_get_u32(b);
    if (b->error || n > b->len - b->pos || (n && memchr(b->data + b->pos, 0, n))) {
        b->error = EPROTO; return 0;
    }
    return n;
}
char *ipc_get_string(ipc_buf_t *b)
{
    size_t n = string_size(b);
    if (b->error) return NULL;
    char *s = malloc(n + 1);
    if (!s) { b->error = ENOMEM; return NULL; }
    if (n) memcpy(s, b->data + b->pos, n);
    s[n] = 0; b->pos += n; return s;
}
void ipc_get_fixed(ipc_buf_t *b, char *s, size_t capacity)
{
    size_t n = string_size(b);
    if (b->error || n >= capacity) { b->error = EPROTO; return; }
    if (n) memcpy(s, b->data + b->pos, n);
    s[n] = 0; b->pos += n;
}
int ipc_done(const ipc_buf_t *b) { return !b->error && b->pos == b->len; }
void ipc_header_write(unsigned char *out, uint16_t type, uint32_t id, uint32_t size)
{
    ipc_buf_t b = {.data = out, .cap = IPC_HEADER_SIZE};
    ipc_put_u32(&b, IPC_MAGIC); ipc_put_u32(&b, (IPC_VERSION << 16) | type);
    ipc_put_u32(&b, id); ipc_put_u32(&b, size);
}
int ipc_header_read(const unsigned char *in, ipc_header_t *h)
{
    ipc_buf_t b = {.data = (unsigned char *)in, .len = IPC_HEADER_SIZE};
    uint32_t magic = ipc_get_u32(&b), version_type = ipc_get_u32(&b);
    h->type = version_type & 0xffff; h->id = ipc_get_u32(&b); h->size = ipc_get_u32(&b);
    if (magic != IPC_MAGIC || version_type >> 16 != IPC_VERSION || h->size > IPC_LIMIT) {
        errno = EPROTO; return -1;
    }
    return 0;
}
int64_t ipc_now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int wait_io(int fd, short events, int wake, int64_t deadline)
{
    for (;;) {
        int64_t left = deadline - ipc_now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p[2] = {{fd, events, 0}, {wake, POLLIN, 0}};
        int n = poll(p, 2, (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = ETIMEDOUT; return -1; }
        if (p[1].revents) { errno = ECANCELED; return -1; }
        if (p[0].revents & events) return 0;
        errno = ECONNRESET; return -1;
    }
}
int ipc_address(const char *path, struct sockaddr_un *address, socklen_t *size)
{
    size_t length = strlen(path);
    if (!length || (path[0] == '@' && length == 1)) { errno = EINVAL; return -1; }
    if (length >= sizeof(address->sun_path)) { errno = ENAMETOOLONG; return -1; }
    memset(address, 0, sizeof(*address)); address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, length);
    if (path[0] == '@') address->sun_path[0] = 0;
    *size = offsetof(struct sockaddr_un, sun_path) + length + (path[0] != '@');
    return 0;
}
#include "../platform/desktop_session.h"
#include <glib.h>
static GMutex desktop_sockets_lock;
static GHashTable *desktop_sockets;
void ipc_desktop_socket(const char *path, int allow)
{
    g_mutex_lock(&desktop_sockets_lock);
    if (!desktop_sockets) desktop_sockets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (allow) g_hash_table_add(desktop_sockets, g_strdup(path));
    else g_hash_table_remove(desktop_sockets, path);
    g_mutex_unlock(&desktop_sockets_lock);
}
int ipc_connect(const char *path)
{
    struct sockaddr_un address; socklen_t address_size;
    if (ipc_address(path, &address, &address_size)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int rc = connect(fd, (struct sockaddr *)&address, address_size);
    if (rc && errno == EINPROGRESS) {
        rc = wait_io(fd, POLLOUT, -1, ipc_now_ms() + IPC_TIMEOUT_MS);
        if (!rc) { int error; socklen_t len = sizeof(error);
            rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            if (!rc && error) { errno = error; rc = -1; }
        }
    }
    uid_t expected_uid = getuid();
    if (expected_uid == 0) {
        g_mutex_lock(&desktop_sockets_lock);
        if (desktop_sockets && g_hash_table_contains(desktop_sockets, path)) expected_uid = desktop_session_uid();
        g_mutex_unlock(&desktop_sockets_lock);
    }
    struct ucred cred; socklen_t size = sizeof(cred);
    if (!rc && (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) ||
        (cred.uid != expected_uid && !(path[0] != '@' && cred.uid == 0)))) {
        errno = EACCES; rc = -1;
    }
    if (rc) { int error = errno; close(fd); errno = error; return -1; }
    return fd;
}
static int transfer(int fd, void *data, size_t len, int sending, int wake, int64_t deadline)
{
    for (size_t pos = 0; pos < len;) {
        if (wait_io(fd, sending ? POLLOUT : POLLIN, wake, deadline)) return -1;
        ssize_t n = sending ? send(fd, (char *)data + pos, len - pos, MSG_NOSIGNAL)
                            : recv(fd, (char *)data + pos, len - pos, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        pos += (size_t)n;
    }
    return 0;
}
int ipc_send(int fd, uint16_t type, uint32_t id, const ipc_buf_t *body)
{
    if (body->error || body->len > IPC_LIMIT) { errno = body->error ? body->error : EOVERFLOW; return -1; }
    unsigned char header[IPC_HEADER_SIZE];
    ipc_header_write(header, type, id, (uint32_t)body->len);
    int64_t deadline = ipc_now_ms() + IPC_TIMEOUT_MS;
    if (transfer(fd, header, sizeof(header), 1, -1, deadline)) return -1;
    return transfer(fd, body->data, body->len, 1, -1, deadline);
}
int ipc_receive(int fd, ipc_header_t *h, ipc_buf_t *body, int wake_fd)
{
    unsigned char header[IPC_HEADER_SIZE];
    int64_t deadline = ipc_now_ms() + IPC_TIMEOUT_MS;
    if (transfer(fd, header, sizeof(header), 0, wake_fd, deadline) || ipc_header_read(header, h)) return -1;
    ipc_free(body);
    if (h->size) {
        body->data = malloc(h->size);
        if (!body->data) return -1;
        body->cap = body->len = h->size;
    }
    return transfer(fd, body->data, body->len, 0, wake_fd, deadline);
}
int ipc_hello(int fd, uint32_t *capabilities, uint32_t *backend)
{
    ipc_buf_t b = {0}; ipc_header_t h;
    if (ipc_send(fd, IPC_HELLO, 1, &b) || ipc_receive(fd, &h, &b, -1)) { ipc_free(&b); return -1; }
    uint32_t caps = ipc_get_u32(&b), max_size = ipc_get_u32(&b);
    (void)ipc_get_u64(&b); uint32_t kind = ipc_get_u32(&b);
    char version[64] = {0}; ipc_get_fixed(&b, version, sizeof(version));
    int valid = h.type == (IPC_HELLO | IPC_REPLY) && h.id == 1 && max_size <= IPC_LIMIT && ipc_done(&b);
    ipc_free(&b);
    if (!valid) { errno = EPROTO; return -1; }
    if (capabilities) *capabilities = caps;
    if (backend) *backend = kind;
    return 0;
}
