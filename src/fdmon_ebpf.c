/*
 * fdmon_ebpf.c – eBPF backend for the fd-monitor abstraction.
 *
 * Loads the pre-compiled BPF object (fdmon_ebpf_kern.o), attaches
 * tracepoints, and reads events from the perf buffer into the
 * shared ring in fdmon_ctx.
 *
 * If libbpf or kernel support is unavailable at runtime, init
 * returns -1 and the caller falls back to fanotify.
 */

#include "fdmon_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

/* ── BPF event structure (must match fdmon_ebpf_kern.c) ──────── */

#define FDMON_BPF_OPEN     1
#define FDMON_BPF_CLOSE    2
#define FDMON_BPF_NET_SEND 3
#define FDMON_BPF_NET_RECV 4
#define FDMON_BPF_PATH_MAX 256

struct fdmon_bpf_event {
    uint32_t type;
    uint32_t pid;
    uint32_t tid;
    int32_t  fd;
    uint64_t timestamp;
    union {
        char     path[FDMON_BPF_PATH_MAX];
        struct {
            uint32_t bytes;
            uint32_t laddr;
            uint32_t raddr;
            uint16_t lport;
            uint16_t rport;
        } net;
    };
};

/* ── internal state ──────────────────────────────────────────── */

typedef struct {
    struct bpf_object     *obj;
    struct bpf_link       *link_enter_openat;
    struct bpf_link       *link_exit_openat;
    struct bpf_link       *link_enter_close;
    struct bpf_link       *link_tcp_sendmsg;
    struct bpf_link       *link_tcp_recvmsg_entry;
    struct bpf_link       *link_tcp_recvmsg_ret;
    struct perf_buffer    *pb;
    pthread_t              reader_thread;
    atomic_int             running;
} ebpf_state_t;

/* ── extern declarations ─────────────────────────────────────── */

/*
 * submit_event is defined in fdmon.c (non-static) and declared in
 * fdmon_internal.h.  struct fdmon_ctx is fully defined there too,
 * so we can access ctx->ebpf_state directly.
 */



/* ── perf buffer callback ────────────────────────────────────── */

static void handle_event(void *cookie, int cpu, void *data, __u32 size)
{
    (void)cpu;
    fdmon_ctx_t *ctx = cookie;

    if (size < sizeof(struct fdmon_bpf_event))
        return;

    const struct fdmon_bpf_event *ev = data;

    if (ev->type == FDMON_BPF_NET_SEND || ev->type == FDMON_BPF_NET_RECV) {
        /* Network I/O event: accumulate bytes per TGID and per socket */
        submit_net_event(ctx, (pid_t)ev->pid, ev->net.bytes,
                         ev->type == FDMON_BPF_NET_SEND);
        submit_sock_event(ctx, (pid_t)ev->pid,
                          ev->net.laddr, ev->net.lport,
                          ev->net.raddr, ev->net.rport,
                          ev->net.bytes,
                          ev->type == FDMON_BPF_NET_SEND);
        return;
    }

    fdmon_event_type_t type;
    if (ev->type == FDMON_BPF_OPEN)
        type = FDMON_OPEN;
    else if (ev->type == FDMON_BPF_CLOSE)
        type = FDMON_CLOSE;
    else
        return;

    /* submit_event handles PID filtering. */
    submit_event(ctx, type, (pid_t)ev->tid, (pid_t)ev->pid,
                 ev->fd, ev->path);
}

static void handle_lost(void *cookie, int cpu, __u64 count)
{
    (void)cookie; (void)cpu; (void)count;
    /* Lost events — could bump a counter here. */
}

/* ── reader thread ───────────────────────────────────────────── */

static void *ebpf_reader_thread(void *arg)
{
    ebpf_state_t *st = arg;
    while (atomic_load_explicit(&st->running, memory_order_relaxed)) {
        /* poll for 200ms then check running flag */
        perf_buffer__poll(st->pb, 200);
    }
    return NULL;
}

/* ── suppress libbpf log noise on probe ──────────────────────── */

static int silent_print(enum libbpf_print_level level,
                        const char *fmt, va_list ap)
{
    (void)level; (void)fmt; (void)ap;
    return 0;
}

/* ── path to the BPF object file ─────────────────────────────── */

/*
 * We look for the compiled BPF object in several locations:
 *  1. Next to the executable (build/ during development)
 *  2. EVEMON_LIBDIR (installed system path)
 */
static int find_bpf_object(char *buf, size_t bufsz)
{
    /* 1. Try next to the executable */
    ssize_t n = readlink("/proc/self/exe", buf, bufsz - 1);
    if (n > 0) {
        buf[n] = '\0';
        char *slash = strrchr(buf, '/');
        if (slash) {
            slash[1] = '\0';
            size_t remaining = bufsz - (size_t)(slash + 1 - buf);
            if (remaining >= sizeof("fdmon_ebpf_kern.o")) {
                strcat(buf, "fdmon_ebpf_kern.o");
                if (access(buf, R_OK) == 0)
                    return 0;
            }
        }
    }

    /* 2. Try the compiled-in install path */
#ifdef EVEMON_LIBDIR
    snprintf(buf, bufsz, "%s/fdmon_ebpf_kern.o", EVEMON_LIBDIR);
    if (access(buf, R_OK) == 0)
        return 0;
#endif

    return -1;
}

/* ── public init / destroy ───────────────────────────────────── */

int fdmon_ebpf_init(struct fdmon_ctx *ctx)
{
    /* Suppress libbpf errors during probe — we fall back gracefully. */
    libbpf_print_fn_t old_print = libbpf_set_print(silent_print);

    int ret = -1;
    ebpf_state_t *st = calloc(1, sizeof(*st));
    if (!st) goto out;

    /* Find the BPF object. */
    char obj_path[PATH_MAX];
    if (find_bpf_object(obj_path, sizeof(obj_path)) != 0)
        goto fail;

    /* Open and load the BPF object. */
    st->obj = bpf_object__open(obj_path);
    if (!st->obj || libbpf_get_error(st->obj)) {
        st->obj = NULL;
        goto fail;
    }

    if (bpf_object__load(st->obj) != 0)
        goto fail;

    /* Find and attach programs. */
    struct bpf_program *prog;

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_openat");
    if (!prog) goto fail;
    st->link_enter_openat = bpf_program__attach(prog);
    if (!st->link_enter_openat || libbpf_get_error(st->link_enter_openat)) {
        st->link_enter_openat = NULL;
        goto fail;
    }

    prog = bpf_object__find_program_by_name(st->obj, "trace_exit_openat");
    if (!prog) goto fail;
    st->link_exit_openat = bpf_program__attach(prog);
    if (!st->link_exit_openat || libbpf_get_error(st->link_exit_openat)) {
        st->link_exit_openat = NULL;
        goto fail;
    }

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_close");
    if (!prog) goto fail;
    st->link_enter_close = bpf_program__attach(prog);
    if (!st->link_enter_close || libbpf_get_error(st->link_enter_close)) {
        st->link_enter_close = NULL;
        goto fail;
    }

    /* Attach tcp_sendmsg kprobe (optional – don't fail if unavailable) */
    prog = bpf_object__find_program_by_name(st->obj, "trace_tcp_sendmsg");
    if (prog) {
        st->link_tcp_sendmsg = bpf_program__attach(prog);
        if (!st->link_tcp_sendmsg || libbpf_get_error(st->link_tcp_sendmsg))
            st->link_tcp_sendmsg = NULL;
    }

    /* Attach tcp_recvmsg kprobe entry (stashes sock pointer) */
    prog = bpf_object__find_program_by_name(st->obj, "trace_tcp_recvmsg");
    if (prog) {
        st->link_tcp_recvmsg_entry = bpf_program__attach(prog);
        if (!st->link_tcp_recvmsg_entry || libbpf_get_error(st->link_tcp_recvmsg_entry))
            st->link_tcp_recvmsg_entry = NULL;
    }

    /* Attach tcp_recvmsg kretprobe (captures bytes + 4-tuple) */
    prog = bpf_object__find_program_by_name(st->obj, "trace_tcp_recvmsg_ret");
    if (prog) {
        st->link_tcp_recvmsg_ret = bpf_program__attach(prog);
        if (!st->link_tcp_recvmsg_ret || libbpf_get_error(st->link_tcp_recvmsg_ret))
            st->link_tcp_recvmsg_ret = NULL;
    }

    /* Open the perf buffer on the "events" map. */
    int map_fd = bpf_object__find_map_fd_by_name(st->obj, "events");
    if (map_fd < 0) goto fail;

    st->pb = perf_buffer__new(map_fd, 64 /* pages per cpu */,
                              handle_event, handle_lost, ctx, NULL);
    if (!st->pb || libbpf_get_error(st->pb)) {
        st->pb = NULL;
        goto fail;
    }

    /* Start reader thread. */
    atomic_store_explicit(&st->running, 1, memory_order_relaxed);
    if (pthread_create(&st->reader_thread, NULL,
                       ebpf_reader_thread, st) != 0)
        goto fail;

    /* Store state in ctx directly. */
    ctx->ebpf_state = st;

    ret = 0;
    goto out;

fail:
    if (st) {
        if (st->pb)               perf_buffer__free(st->pb);
        if (st->link_tcp_recvmsg_ret)   bpf_link__destroy(st->link_tcp_recvmsg_ret);
        if (st->link_tcp_recvmsg_entry) bpf_link__destroy(st->link_tcp_recvmsg_entry);
        if (st->link_tcp_sendmsg) bpf_link__destroy(st->link_tcp_sendmsg);
        if (st->link_enter_close) bpf_link__destroy(st->link_enter_close);
        if (st->link_exit_openat) bpf_link__destroy(st->link_exit_openat);
        if (st->link_enter_openat)bpf_link__destroy(st->link_enter_openat);
        if (st->obj)              bpf_object__close(st->obj);
        free(st);
    }

out:
    libbpf_set_print(old_print);
    return ret;
}

void fdmon_ebpf_destroy(struct fdmon_ctx *ctx)
{
    if (!ctx || !ctx->ebpf_state) return;

    ebpf_state_t *st = ctx->ebpf_state;

    atomic_store_explicit(&st->running, 0, memory_order_relaxed);
    pthread_join(st->reader_thread, NULL);

    if (st->pb)                     perf_buffer__free(st->pb);
    if (st->link_tcp_recvmsg_ret)   bpf_link__destroy(st->link_tcp_recvmsg_ret);
    if (st->link_tcp_recvmsg_entry) bpf_link__destroy(st->link_tcp_recvmsg_entry);
    if (st->link_tcp_sendmsg)       bpf_link__destroy(st->link_tcp_sendmsg);
    if (st->link_enter_close) bpf_link__destroy(st->link_enter_close);
    if (st->link_exit_openat) bpf_link__destroy(st->link_exit_openat);
    if (st->link_enter_openat)bpf_link__destroy(st->link_enter_openat);
    if (st->obj)              bpf_object__close(st->obj);
    free(st);
}
