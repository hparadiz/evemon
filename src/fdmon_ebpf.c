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

#define FDMON_BPF_OPEN  1
#define FDMON_BPF_CLOSE 2
#define FDMON_BPF_PATH_MAX 256

struct fdmon_bpf_event {
    uint32_t type;
    uint32_t pid;
    uint32_t tid;
    int32_t  fd;
    uint64_t timestamp;
    char     path[FDMON_BPF_PATH_MAX];
};

/* ── internal state ──────────────────────────────────────────── */

typedef struct {
    struct bpf_object     *obj;
    struct bpf_link       *link_enter_openat;
    struct bpf_link       *link_exit_openat;
    struct bpf_link       *link_enter_close;
    struct perf_buffer    *pb;
    pthread_t              reader_thread;
    volatile int           running;
} ebpf_state_t;

/* ── extern declarations ─────────────────────────────────────── */

/*
 * submit_event is defined in fdmon.c (non-static) and declared in
 * fdmon_internal.h.  struct fdmon_ctx is fully defined there too,
 * so we can access ctx->ebpf_state directly.
 */

/* Global context pointer — one eBPF backend per process is fine. */
static fdmon_ctx_t *g_ebpf_ctx;

/* ── perf buffer callback ────────────────────────────────────── */

static void handle_event(void *cookie, int cpu, void *data, __u32 size)
{
    (void)cpu;
    (void)cookie;

    if (size < sizeof(struct fdmon_bpf_event))
        return;

    const struct fdmon_bpf_event *ev = data;

    fdmon_event_type_t type;
    if (ev->type == FDMON_BPF_OPEN)
        type = FDMON_OPEN;
    else if (ev->type == FDMON_BPF_CLOSE)
        type = FDMON_CLOSE;
    else
        return;

    /* submit_event handles PID filtering. */
    submit_event(g_ebpf_ctx, type, (pid_t)ev->tid, (pid_t)ev->pid,
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
    while (st->running) {
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
 * We look for the compiled BPF object next to the executable.
 * The Makefile places it in the build/ directory alongside allmon.
 */
static int find_bpf_object(char *buf, size_t bufsz)
{
    /* Try next to the executable */
    ssize_t n = readlink("/proc/self/exe", buf, bufsz - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';

    /* Strip the executable name, append our object name. */
    char *slash = strrchr(buf, '/');
    if (!slash) return -1;
    slash[1] = '\0';

    size_t remaining = bufsz - (size_t)(slash + 1 - buf);
    if (remaining < sizeof("fdmon_ebpf_kern.o"))
        return -1;
    strcat(buf, "fdmon_ebpf_kern.o");

    /* Check it exists. */
    if (access(buf, R_OK) != 0)
        return -1;

    return 0;
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

    /* Open the perf buffer on the "events" map. */
    int map_fd = bpf_object__find_map_fd_by_name(st->obj, "events");
    if (map_fd < 0) goto fail;

    g_ebpf_ctx = ctx;

    st->pb = perf_buffer__new(map_fd, 64 /* pages per cpu */,
                              handle_event, handle_lost, NULL, NULL);
    if (!st->pb || libbpf_get_error(st->pb)) {
        st->pb = NULL;
        goto fail;
    }

    /* Start reader thread. */
    st->running = 1;
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

    st->running = 0;
    pthread_join(st->reader_thread, NULL);

    if (st->pb)               perf_buffer__free(st->pb);
    if (st->link_enter_close) bpf_link__destroy(st->link_enter_close);
    if (st->link_exit_openat) bpf_link__destroy(st->link_exit_openat);
    if (st->link_enter_openat)bpf_link__destroy(st->link_enter_openat);
    if (st->obj)              bpf_object__close(st->obj);
    free(st);
}
