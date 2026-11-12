#ifndef EVEMON_IPC_WIRE_H
#define EVEMON_IPC_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

/* EVM1, big endian, no native structs or pointers on the wire. */
#define IPC_MAGIC 0x45564d31u
#define IPC_VERSION 1
#define IPC_HEADER_SIZE 16
#define IPC_LIMIT (32u * 1024u * 1024u)
#define IPC_REQUEST_LIMIT 4096u
#define IPC_TIMEOUT_MS 5000
/* A package lookup may read thousands of dpkg file lists from a cold disk. */
#define IPC_PACKAGE_TIMEOUT_MS 60000
#define IPC_SYSTEM_DIRECTORY "/run/evemon"
#define IPC_SYSTEM_SOCKET IPC_SYSTEM_DIRECTORY "/evemon.sock"
#define IPC_REPLY 0x8000
enum { IPC_HELLO = 1, IPC_STATUS, IPC_SNAPSHOT, IPC_PROCESS,
       IPC_SUBSCRIBE, IPC_INSPECT, IPC_CONTROL, IPC_EVENTS, IPC_GPU, IPC_HOST, IPC_SIGNAL, IPC_ABOUT, IPC_DPKG,
       IPC_CHECKSUM,
       IPC_ERROR = 0xffff };
enum { IPC_BAD_REQUEST = 1, IPC_NOT_READY, IPC_NOT_FOUND, IPC_UNSUPPORTED,
       IPC_INTERNAL, IPC_BUSY };
enum { IPC_CAP_SNAPSHOT = 1, IPC_CAP_INSPECT = 2, IPC_CAP_PIPEWIRE = 4,
       IPC_CAP_MPRIS = 8, IPC_CAP_EVENTS = 16, IPC_CAP_GPU = 32, IPC_CAP_SIGNAL = 64, IPC_CAP_ABOUT = 128,
       IPC_CAP_DPKG = 256 };
typedef struct {
    unsigned char *data;
    size_t len, cap, pos;
    int error;
} ipc_buf_t;
typedef struct { uint16_t type; uint32_t id, size; } ipc_header_t;
void ipc_free(ipc_buf_t *b);
void ipc_put_bytes(ipc_buf_t *b, const void *p, size_t n);
void ipc_put_u32(ipc_buf_t *b, uint32_t v);
void ipc_put_u64(ipc_buf_t *b, uint64_t v);
void ipc_put_f64(ipc_buf_t *b, double v);
void ipc_put_string(ipc_buf_t *b, const char *s);
uint32_t ipc_get_u32(ipc_buf_t *b);
uint64_t ipc_get_u64(ipc_buf_t *b);
double ipc_get_f64(ipc_buf_t *b);
char *ipc_get_string(ipc_buf_t *b);
void ipc_get_fixed(ipc_buf_t *b, char *s, size_t capacity);
int ipc_done(const ipc_buf_t *b);
void ipc_header_write(unsigned char *out, uint16_t type, uint32_t id, uint32_t size);
int ipc_header_read(const unsigned char *in, ipc_header_t *h);
int64_t ipc_now_ms(void);
/* Nonblocking socket + a single deadline for the entire frame. */
/* @name selects Linux's abstract namespace; peer UID checks still apply. */
int ipc_address(const char *path, struct sockaddr_un *address, socklen_t *size);
int ipc_connect(const char *path);
/* Register only private SSH sockets owned by the invoking desktop user. */
void ipc_desktop_socket(const char *path, int allow);
int ipc_send(int fd, uint16_t type, uint32_t id, const ipc_buf_t *body);
int ipc_receive(int fd, ipc_header_t *h, ipc_buf_t *body, int wake_fd);
/* As ipc_receive, with a caller-chosen deadline for the whole frame. */
int ipc_receive_wait(int fd, ipc_header_t *h, ipc_buf_t *body, int wake_fd, int timeout_ms);
int ipc_hello(int fd, uint32_t *capabilities, uint32_t *backend);
/* As ipc_hello, also returning the peer's version number. */
int ipc_version(int fd, uint32_t *capabilities, uint32_t *backend, uint64_t *build);
/* This executable's version number: "major.minor.patch" stored as
 * major * 10000 + minor * 100 + patch, so 0.0.2 is 2 and 0.2.0 is 200. */
uint64_t ipc_build(void);
/* Asks the peer for the CRC32 of its executable.  Evemons that predate the
 * request answer IPC_UNSUPPORTED, which fails here with errno ENOTSUP; any
 * other errno is a transport or protocol failure and says nothing about the
 * peer's build. */
int ipc_checksum(int fd, uint32_t *checksum);
/* CRC32 of this executable; 0 when it cannot be read. */
uint32_t ipc_self_checksum(void);
#endif
