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
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
/* For publishing write events directly to the event bus */
#include "evemon_plugin.h"
#include "event_bus.h"

/* ── BPF event structure (must match fdmon_ebpf_kern.c) ──────── */

#define FDMON_BPF_OPEN     1
#define FDMON_BPF_CLOSE    2
#define FDMON_BPF_NET_SEND 3
#define FDMON_BPF_NET_RECV 4
#define FDMON_BPF_PATH_MAX 256
/* write event type and payload size (must match kernel) */
#define FDMON_BPF_WRITE 5
#define FDMON_BPF_WRITE_MAX 4096
/*
 * EXEC event: fired by sched_process_exec tracepoint after a successful
 * execve().  Used for orphan-stdout detection: userspace inspects
 * /proc/<pid>/fd/1 (and /2) and auto-registers them if they are not TTYs.
 */
#define FDMON_BPF_EXEC  6

struct fdmon_bpf_event {
    uint32_t type;
    uint32_t pid;
    uint32_t tid;
    int32_t  fd;
    uint64_t timestamp;
    uint64_t user_ptr;
    uint32_t user_count;
    union {
        char     path[FDMON_BPF_PATH_MAX];
        struct {
            uint32_t bytes;
            uint32_t laddr;
            uint32_t raddr;
            uint16_t lport;
            uint16_t rport;
        } net;
        struct {
            uint32_t len;
            char     data[FDMON_BPF_WRITE_MAX];
        } write;
    };
};

/*
 * Byte offset of the union payload within fdmon_bpf_event.
 * open/close/net/exec events are emitted as fdmon_bpf_event_hdr (kernel
 * side), which is this fixed header + the path/net union member.
 * write events are emitted from the per-CPU scratch map and carry
 * write.len + write.data[0..len-1] beyond the fixed header.
 */
#define FDMON_BPF_HDR_SIZE       ((uint32_t)offsetof(struct fdmon_bpf_event, path))
/* Minimum bytes needed to read write.len safely. */
#define FDMON_BPF_WRITE_HDR_SIZE ((uint32_t)offsetof(struct fdmon_bpf_event, write.data))

typedef struct {
    struct bpf_object     *obj;
    struct bpf_link       *link_enter_openat;
    struct bpf_link       *link_exit_openat;
    struct bpf_link       *link_enter_close;
    struct bpf_link       *link_enter_write;
    struct bpf_link       *link_enter_writev;
    struct bpf_link       *link_enter_pwrite64;
    struct bpf_link       *link_enter_sendto;
    struct bpf_link       *link_enter_sendmsg;
    struct bpf_link       *link_enter_sendmmsg;
    struct bpf_link       *link_enter_splice;
    struct bpf_link       *link_enter_sendfile;
    struct bpf_link       *link_tcp_sendmsg;
    struct bpf_link       *link_tcp_recvmsg_entry;
    struct bpf_link       *link_tcp_recvmsg_ret;
    /* sched_process_exec tracepoint for orphan-stdout detection */
    struct bpf_link       *link_sched_exec;
    /* sched_process_fork: inherit monitored fds into child immediately */
    struct bpf_link       *link_sched_fork;
    struct perf_buffer    *pb;
    pthread_t              reader_thread;
    atomic_int             running;
    int                    monitored_pids_map_fd;
    /* When non-zero, auto-register fd 1/2 of any newly exec'd process
     * whose stdout/stderr is not a TTY (orphan stdout capture mode). */
    atomic_int             orphan_stdout_mode;
    /*
     * Watched-parent table: when a process in this table exec()s a child,
     * we immediately insert the child's entry into monitored_pids IN the
     * reader thread — no GTK idle dispatch, no race.
     * fd_mask: bit 0 = fd 1, bit 1 = fd 2.
     */
#define WATCHED_PARENT_MAX 64
    struct {
        pid_t pid;
        int   fd_mask;   /* bit0 = fd1, bit1 = fd2 */
    } watched_parents[WATCHED_PARENT_MAX];
    int             watched_parent_count;
    pthread_mutex_t watched_parent_lock;
} ebpf_state_t;

/* ── extern declarations ─────────────────────────────────────── */

/*
 * submit_event is defined in fdmon.c (non-static) and declared in
 * fdmon_internal.h.  struct fdmon_ctx is fully defined there too,
 * so we can access ctx->ebpf_state directly.
 */



/* ── orphan stdout helpers ───────────────────────────────────── */

/*
 * Return 1 if /proc/<pid>/fd/<fd> resolves to a path that is NOT a
 * character device (which would indicate a real TTY or /dev/null).
 * We consider it "orphaned" if:
 *   - The symlink resolves to a pipe ("pipe:[...]")
 *   - The symlink resolves to a socket ("socket:[...]")
 *   - The symlink resolves to a regular file (absolute path, no major:minor)
 *   - The target is /dev/null
 *
 * In practice: if the resolved path starts with "pipe:[", "socket:[",
 * or is an absolute path that is NOT under /dev (i.e. not a TTY),
 * we consider stdout orphaned and capture it.
 *
 * /dev/null specifically: we DO want to capture writes going there
 * because those are the "lost" outputs we care about most.
 */
static int fd_is_orphaned_output(pid_t pid, int fd)
{
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/fd/%d", (int)pid, fd);

    char target[256];
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0)
        return 0;   /* can't read = process already gone or no permission */
    target[n] = '\0';

    /* Pipes and sockets are always orphaned outputs */
    if (strncmp(target, "pipe:[", 6) == 0)
        return 1;
    if (strncmp(target, "socket:[", 8) == 0)
        return 0;   /* sockets handled by net path, not here */

    /* /dev/null is definitely an orphaned output */
    if (strcmp(target, "/dev/null") == 0)
        return 1;

    /* /dev/pts/N or /dev/tty* → real terminal, skip */
    if (strncmp(target, "/dev/pts/", 9) == 0)
        return 0;
    if (strncmp(target, "/dev/tty", 8) == 0)
        return 0;
    if (strncmp(target, "/dev/console", 12) == 0)
        return 0;

    /* Any other /dev/ node is a device — skip (could be a PTY master etc.) */
    if (strncmp(target, "/dev/", 5) == 0)
        return 0;

    /* Regular file path (logs, etc.) — capture it */
    if (target[0] == '/')
        return 1;

    /* Anything else (anon_inode, etc.) — skip */
    return 0;
}

/*
 * Try to auto-register fd 1 and fd 2 of a newly exec'd process for
 * write monitoring if orphan_stdout_mode is active.
 * Called from handle_event on a FDMON_BPF_EXEC event.
 * st->monitored_pids_map_fd must be valid.
 */
static void maybe_register_orphan_fds(ebpf_state_t *st, pid_t pid)
{
    if (st->monitored_pids_map_fd < 0)
        return;

    __u8 mask = 0;
    for (int fd = 1; fd <= 2; fd++) {
        if (fd_is_orphaned_output(pid, fd))
            mask |= (__u8)(1 << (fd - 1));
    }
    if (!mask) return;

    __u32 key = (uint32_t)pid;
    __u8 val  = mask;
    bpf_map_update_elem(st->monitored_pids_map_fd,
                         &key, &val, BPF_ANY);
}

/* ── perf buffer callback ────────────────────────────────────── */

static void handle_event(void *cookie, int cpu, void *data, __u32 size)
{
    (void)cpu;
    fdmon_ctx_t *ctx = cookie;

    if (size < FDMON_BPF_HDR_SIZE + 1)
        return;

    const struct fdmon_bpf_event *ev = data;

    if (ev->type == FDMON_BPF_WRITE) {
        /* Need at least write.len (u32) before we can inspect payload size. */
        if (size < FDMON_BPF_WRITE_HDR_SIZE)
            return;

        evemon_fd_write_payload_t payload;
        payload.pid       = (pid_t)ev->tid;
        payload.tgid      = (pid_t)ev->pid;
        payload.fd        = ev->fd;
        payload.truncated = 0;

        if (ev->write.len > 0) {
            payload.len = ev->write.len;
            if (payload.len > sizeof(payload.data))
                payload.len = sizeof(payload.data);
            /* Guard: don't read past the end of the perf buffer record. */
            uint32_t avail = (size > FDMON_BPF_WRITE_HDR_SIZE)
                             ? (size - FDMON_BPF_WRITE_HDR_SIZE) : 0;
            if (payload.len > avail)
                payload.len = avail;
            if (payload.len > 0)
                memcpy(payload.data, ev->write.data, payload.len);
            /* truncated if the original count exceeded what BPF copied */
            payload.truncated = (ev->user_count > 0 &&
                                 (uint32_t)payload.len < ev->user_count) ? 1 : 0;
        } else if (ev->user_ptr != 0 && ev->user_count > 0) {
            /* Try process_vm_readv fallback — capped at our payload buffer size.
             * Note: this races with the remote process reusing the buffer, so
             * it is best-effort only; the inline BPF copy is the reliable path. */
            size_t want = ev->user_count;
            if (want > sizeof(payload.data)) want = sizeof(payload.data);
            struct iovec local_iov  = { .iov_base = payload.data, .iov_len = want };
            struct iovec remote_iov = { .iov_base = (void *)(uintptr_t)ev->user_ptr,
                                        .iov_len  = want };
            ssize_t nread = process_vm_readv((pid_t)ev->pid, &local_iov, 1,
                                             &remote_iov, 1, 0);
            payload.len       = (nread > 0) ? (size_t)nread : 0;
            payload.truncated = (ev->user_count > (uint32_t)payload.len) ? 1 : 0;
        } else {
            payload.len       = 0;
            payload.truncated = 0;
        }

        evemon_event_t e = { .type = EVEMON_EVENT_FD_WRITE, .payload = &payload };
        evemon_event_bus_publish(&e);
        return;
    }

    if (ev->type == FDMON_BPF_EXEC) {
        ebpf_state_t *st = ctx->ebpf_state;
        pid_t new_pid = (pid_t)ev->pid;

        if (st && atomic_load_explicit(&st->orphan_stdout_mode,
                                       memory_order_relaxed)) {
            maybe_register_orphan_fds(st, new_pid);
        }

        /*
         * Inline (reader-thread) child-fd registration.
         * Read the ppid from /proc/<pid>/status — this is the fastest
         * reliable way to get it, and /proc is available immediately
         * after execve completes.  If the new process's ppid matches
         * any entry in watched_parents, insert its fds into the BPF map
         * right now, before returning.  No GTK dispatch, no race.
         */
        if (st && st->monitored_pids_map_fd >= 0 &&
            st->watched_parent_count > 0) {

            pid_t ppid = 0;
            {
                char spath[64];
                snprintf(spath, sizeof(spath), "/proc/%d/status", (int)new_pid);
                FILE *sf = fopen(spath, "r");
                if (sf) {
                    char line[128];
                    while (fgets(line, sizeof(line), sf)) {
                        if (strncmp(line, "PPid:", 5) == 0) {
                            ppid = (pid_t)atoi(line + 5);
                            break;
                        }
                    }
                    fclose(sf);
                }
            }

            if (ppid != 0) {
                int fd_mask = 0;
                pthread_mutex_lock(&st->watched_parent_lock);
                for (int i = 0; i < st->watched_parent_count; i++) {
                    if (st->watched_parents[i].pid == ppid) {
                        fd_mask = st->watched_parents[i].fd_mask;
                        break;
                    }
                }
                pthread_mutex_unlock(&st->watched_parent_lock);

                if (fd_mask) {
                    /* The fork BPF program already propagated this entry.
                     * But if exec replaced the process image (new command),
                     * we re-confirm the entry is present with the right mask. */
                    __u32 key = (uint32_t)new_pid;
                    __u8 val  = (__u8)fd_mask;
                    bpf_map_update_elem(st->monitored_pids_map_fd,
                                        &key, &val, BPF_ANY);
                }
            }
        }

        /* Publish CHILD_EXEC unconditionally so write-monitor subscribers
         * can immediately register the new process's fds in the BPF map
         * before it has a chance to call write(). */
        evemon_exec_payload_t xp;
        xp.pid  = (pid_t)ev->pid;
        xp.ppid = 0;
        /* Best-effort ppid from /proc/<pid>/status */
        {
            char spath[64];
            snprintf(spath, sizeof(spath), "/proc/%d/status", (int)ev->pid);
            FILE *sf = fopen(spath, "r");
            if (sf) {
                char line[128];
                while (fgets(line, sizeof(line), sf)) {
                    if (strncmp(line, "PPid:", 5) == 0) {
                        xp.ppid = (pid_t)atoi(line + 5);
                        break;
                    }
                }
                fclose(sf);
            }
        }
        /* Copy executable path from the BPF event header */
        strncpy(xp.path, ev->path, sizeof(xp.path) - 1);
        xp.path[sizeof(xp.path) - 1] = '\0';

        evemon_event_t xe = { .type = EVEMON_EVENT_CHILD_EXEC, .payload = &xp };
        evemon_event_bus_publish(&xe);
        return;
    }

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

    /*
     * Raise the locked-memory limit to unlimited so the BPF maps
     * (especially write_event_scratch at 4136 bytes × nCPUs) can be
     * pinned.  On kernels ≥ 5.11 this is handled automatically via
     * CAP_BPF, but setting it explicitly is always safe and ensures
     * correct behaviour on older kernels or when running as root.
     */
    {
        struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
        if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0)
            fprintf(stderr, "evemon: setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY) failed: %s\n",
                    strerror(errno));
    }

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

    /*
     * Sanity-check the scratch map value size before loading.
     * FDMON_BPF_WRITE_MAX was raised from 256 → 4096, making
     * sizeof(fdmon_bpf_event) = 4136.  The BPF verifier enforces
     * that every bpf_probe_read_user() stays within the map value.
     * If the struct layout in this file has drifted from what was
     * compiled into the .o, nothing will work.
     */
    {
        struct bpf_map *scratch =
            bpf_object__find_map_by_name(st->obj, "write_event_scratch");
        if (scratch) {
            __u32 vs = bpf_map__value_size(scratch);
            if (vs != (uint32_t)sizeof(struct fdmon_bpf_event)) {
                fprintf(stderr,
                    "evemon: BPF write_event_scratch value_size mismatch: "
                    ".o has %u bytes, userspace struct is %zu bytes. "
                    "Rebuild with matching FDMON_BPF_WRITE_MAX (%d).\n",
                    vs, sizeof(struct fdmon_bpf_event), FDMON_BPF_WRITE_MAX);
                goto fail;
            }
        }
    }

    /* Resize monitored_pids to match the live kernel pid_max so the map
     * can never be full as long as the kernel allows the PID to exist. */
    {
        __u32 pid_max = 32768; /* safe default */
        FILE *f = fopen("/proc/sys/kernel/pid_max", "r");
        if (f) {
            unsigned long v;
            if (fscanf(f, "%lu", &v) == 1 && v > 0 && v <= 4194304)
                pid_max = (__u32)v;
            fclose(f);
        }
        struct bpf_map *mpmap =
            bpf_object__find_map_by_name(st->obj, "monitored_pids");
        if (mpmap)
            bpf_map__set_max_entries(mpmap, pid_max);
    }

    if (bpf_object__load(st->obj) != 0) {
        fprintf(stderr,
            "evemon: BPF program load/verify failed (EACCES). "
            "This is likely a verifier bounds error in the writev/sendmsg "
            "write-capture programs. Write monitoring will be disabled. "
            "Check kernel version (≥5.8) and RLIMIT_MEMLOCK.\n");
        goto fail;
    }

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

    /* Attach sched_process_exec tracepoint (optional – for orphan-stdout mode).
     * Failure here is non-fatal; orphan_stdout_mode simply won't auto-fire. */
    prog = bpf_object__find_program_by_name(st->obj, "trace_sched_process_exec");
    if (prog) {
        st->link_sched_exec = bpf_program__attach(prog);
        if (!st->link_sched_exec || libbpf_get_error(st->link_sched_exec))
            st->link_sched_exec = NULL;
    }

    /* Attach sched_process_fork tracepoint (optional – for child fd inheritance).
     * Enables zero-latency fd registration when a monitored process forks. */
    prog = bpf_object__find_program_by_name(st->obj, "trace_sched_process_fork");
    if (prog) {
        st->link_sched_fork = bpf_program__attach(prog);
        if (!st->link_sched_fork || libbpf_get_error(st->link_sched_fork))
            st->link_sched_fork = NULL;
    }

    /* Open the perf buffer on the "events" map. */
    int map_fd = bpf_object__find_map_fd_by_name(st->obj, "events");
    if (map_fd < 0) goto fail;

    /* Find optional monitored_pids map for write filtering */
    int mpfd = bpf_object__find_map_fd_by_name(st->obj, "monitored_pids");
    if (mpfd >= 0)
        st->monitored_pids_map_fd = mpfd;
    else
        st->monitored_pids_map_fd = -1;

    st->pb = perf_buffer__new(map_fd, 64 /* pages per cpu */,
                              handle_event, handle_lost, ctx, NULL);
    if (!st->pb || libbpf_get_error(st->pb)) {
        st->pb = NULL;
        goto fail;
    }

    /* Start reader thread. */
    atomic_store_explicit(&st->running, 1, memory_order_relaxed);
    atomic_store_explicit(&st->orphan_stdout_mode, 0, memory_order_relaxed);
    st->watched_parent_count = 0;
    pthread_mutex_init(&st->watched_parent_lock, NULL);
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
        if (st->link_sched_fork)  bpf_link__destroy(st->link_sched_fork);
        if (st->link_sched_exec)  bpf_link__destroy(st->link_sched_exec);
        if (st->link_enter_sendfile)    bpf_link__destroy(st->link_enter_sendfile);
        if (st->link_enter_splice)      bpf_link__destroy(st->link_enter_splice);
        if (st->link_enter_sendmmsg)    bpf_link__destroy(st->link_enter_sendmmsg);
        if (st->link_enter_sendmsg)     bpf_link__destroy(st->link_enter_sendmsg);
        if (st->link_enter_sendto)      bpf_link__destroy(st->link_enter_sendto);
        if (st->link_enter_pwrite64)    bpf_link__destroy(st->link_enter_pwrite64);
        if (st->link_enter_writev)      bpf_link__destroy(st->link_enter_writev);
        if (st->link_enter_write)       bpf_link__destroy(st->link_enter_write);
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
    pthread_mutex_destroy(&st->watched_parent_lock);

    if (st->pb)                     perf_buffer__free(st->pb);
    if (st->link_tcp_recvmsg_ret)   bpf_link__destroy(st->link_tcp_recvmsg_ret);
    if (st->link_tcp_recvmsg_entry) bpf_link__destroy(st->link_tcp_recvmsg_entry);
    if (st->link_tcp_sendmsg)       bpf_link__destroy(st->link_tcp_sendmsg);
    if (st->link_sched_fork)        bpf_link__destroy(st->link_sched_fork);
    if (st->link_sched_exec)        bpf_link__destroy(st->link_sched_exec);
    if (st->link_enter_write)       bpf_link__destroy(st->link_enter_write);
    if (st->link_enter_writev)      bpf_link__destroy(st->link_enter_writev);
    if (st->link_enter_pwrite64)    bpf_link__destroy(st->link_enter_pwrite64);
    if (st->link_enter_sendto)      bpf_link__destroy(st->link_enter_sendto);
    if (st->link_enter_sendmsg)     bpf_link__destroy(st->link_enter_sendmsg);
    if (st->link_enter_sendmmsg)    bpf_link__destroy(st->link_enter_sendmmsg);
    if (st->link_enter_splice)      bpf_link__destroy(st->link_enter_splice);
    if (st->link_enter_sendfile)    bpf_link__destroy(st->link_enter_sendfile);
    if (st->link_enter_close) bpf_link__destroy(st->link_enter_close);
    if (st->link_exit_openat) bpf_link__destroy(st->link_exit_openat);
    if (st->link_enter_openat)bpf_link__destroy(st->link_enter_openat);
    if (st->obj)              bpf_object__close(st->obj);
    free(st);
}

/* ── dynamic stdout/stderr (write) control ─────────────────── */

int fdmon_write_enable(struct fdmon_ctx *ctx)
{
    if (!ctx || !ctx->ebpf_state) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;
    if (st->link_enter_write)
        return 0; /* already attached */

    /* Attach all write-like tracepoint programs */
    struct bpf_program *prog = NULL;
    /* Suppress libbpf prints while attaching optional programs to avoid
     * noisy kernel-specific tracepoint errors (some kernels lack certain
     * syscall tracepoints).  We still treat the core `trace_enter_write`
     * program as required; other programs are optional. */
    libbpf_print_fn_t old_print = libbpf_set_print(silent_print);

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_write");
    if (!prog) {
        libbpf_set_print(old_print);
        goto fail_attach;
    }
    st->link_enter_write = bpf_program__attach(prog);
    if (!st->link_enter_write || libbpf_get_error(st->link_enter_write)) {
        libbpf_set_print(old_print);
        goto fail_attach;
    }

    /* For the remaining programs treat attach failures as non-fatal
     * (kernel may not expose these tracepoints).  If attach fails we
     * clear the link pointer and continue. */
    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_writev");
    if (prog) {
        struct bpf_link *l = bpf_program__attach(prog);
        if (!l || libbpf_get_error(l)) l = NULL;
        st->link_enter_writev = l;
    }

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_pwrite64");
    if (prog) {
        struct bpf_link *l = bpf_program__attach(prog);
        if (!l || libbpf_get_error(l)) l = NULL;
        st->link_enter_pwrite64 = l;
    }

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_sendto");
    if (prog) {
        struct bpf_link *l = bpf_program__attach(prog);
        if (!l || libbpf_get_error(l)) l = NULL;
        st->link_enter_sendto = l;
    }

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_sendmsg");
    if (prog) {
        struct bpf_link *l = bpf_program__attach(prog);
        if (!l || libbpf_get_error(l)) l = NULL;
        st->link_enter_sendmsg = l;
    }

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_sendmmsg");
    if (prog) {
        struct bpf_link *l = bpf_program__attach(prog);
        if (!l || libbpf_get_error(l)) l = NULL;
        st->link_enter_sendmmsg = l;
    }

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_splice");
    if (prog) {
        struct bpf_link *l = bpf_program__attach(prog);
        if (!l || libbpf_get_error(l)) l = NULL;
        st->link_enter_splice = l;
    }

    prog = bpf_object__find_program_by_name(st->obj, "trace_enter_sendfile");
    if (prog) {
        struct bpf_link *l = bpf_program__attach(prog);
        if (!l || libbpf_get_error(l)) l = NULL;
        st->link_enter_sendfile = l;
    }

    /* restore libbpf print handler */
    libbpf_set_print(old_print);

        /* Diagnostic: report which optional probes attached */
        return 0;

fail_attach:
    /* clean up any links attached so far */
    if (st->link_enter_sendfile)    { bpf_link__destroy(st->link_enter_sendfile); st->link_enter_sendfile = NULL; }
    if (st->link_enter_splice)      { bpf_link__destroy(st->link_enter_splice); st->link_enter_splice = NULL; }
    if (st->link_enter_sendmmsg)    { bpf_link__destroy(st->link_enter_sendmmsg); st->link_enter_sendmmsg = NULL; }
    if (st->link_enter_sendmsg)     { bpf_link__destroy(st->link_enter_sendmsg); st->link_enter_sendmsg = NULL; }
    if (st->link_enter_sendto)      { bpf_link__destroy(st->link_enter_sendto); st->link_enter_sendto = NULL; }
    if (st->link_enter_pwrite64)    { bpf_link__destroy(st->link_enter_pwrite64); st->link_enter_pwrite64 = NULL; }
    if (st->link_enter_writev)      { bpf_link__destroy(st->link_enter_writev); st->link_enter_writev = NULL; }
    if (st->link_enter_write)       { bpf_link__destroy(st->link_enter_write); st->link_enter_write = NULL; }
    return -1;
}

void fdmon_write_disable(struct fdmon_ctx *ctx)
{
    if (!ctx || !ctx->ebpf_state) return;
    ebpf_state_t *st = ctx->ebpf_state;
    if (st->link_enter_sendfile)    { bpf_link__destroy(st->link_enter_sendfile); st->link_enter_sendfile = NULL; }
    if (st->link_enter_splice)      { bpf_link__destroy(st->link_enter_splice); st->link_enter_splice = NULL; }
    if (st->link_enter_sendmmsg)    { bpf_link__destroy(st->link_enter_sendmmsg); st->link_enter_sendmmsg = NULL; }
    if (st->link_enter_sendmsg)     { bpf_link__destroy(st->link_enter_sendmsg); st->link_enter_sendmsg = NULL; }
    if (st->link_enter_sendto)      { bpf_link__destroy(st->link_enter_sendto); st->link_enter_sendto = NULL; }
    if (st->link_enter_pwrite64)    { bpf_link__destroy(st->link_enter_pwrite64); st->link_enter_pwrite64 = NULL; }
    if (st->link_enter_writev)      { bpf_link__destroy(st->link_enter_writev); st->link_enter_writev = NULL; }
    if (st->link_enter_write)       { bpf_link__destroy(st->link_enter_write); st->link_enter_write = NULL; }
}

int fdmon_add_pid_fd(struct fdmon_ctx *ctx, pid_t pid, int fd)
{
    if (!ctx || !ctx->ebpf_state) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;
    if (st->monitored_pids_map_fd < 0) { errno = ENOENT; return -1; }
    if (fd != 1 && fd != 2) { errno = EINVAL; return -1; }

    __u32 key  = (uint32_t)pid;
    __u8  bit  = (__u8)(1 << (fd - 1));
    __u8  mask = 0;

    /* Read existing mask (if any) and OR in the new bit. */
    bpf_map_lookup_elem(st->monitored_pids_map_fd, &key, &mask);
    mask |= bit;

    int rc = bpf_map_update_elem(st->monitored_pids_map_fd, &key, &mask, BPF_ANY);
    if (rc != 0) {
        fprintf(stderr, "evemon: fdmon_add_pid_fd failed pid=%d fd=%d rc=%d errno=%d\n", (int)pid, fd, rc, errno);
        return -1;
    }
    return 0;
}

int fdmon_remove_pid_fd(struct fdmon_ctx *ctx, pid_t pid, int fd)
{
    if (!ctx || !ctx->ebpf_state) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;
    if (st->monitored_pids_map_fd < 0) { errno = ENOENT; return -1; }
    if (fd != 1 && fd != 2) { errno = EINVAL; return -1; }

    __u32 key = (uint32_t)pid;
    __u8  bit = (__u8)(1 << (fd - 1));
    __u8  mask = 0;

    if (bpf_map_lookup_elem(st->monitored_pids_map_fd, &key, &mask) != 0)
        return 0;  /* not present, nothing to do */

    mask &= ~bit;
    int rc;
    if (mask == 0)
        rc = bpf_map_delete_elem(st->monitored_pids_map_fd, &key);
    else
        rc = bpf_map_update_elem(st->monitored_pids_map_fd, &key, &mask, BPF_EXIST);

    if (rc != 0) {
        fprintf(stderr, "evemon: fdmon_remove_pid_fd failed pid=%d fd=%d rc=%d errno=%d\n", (int)pid, fd, rc, errno);
        return -1;
    }
    return 0;
}

/* ── orphan-stdout mode ──────────────────────────────────────── */

/*
 * Enable automatic capture of stdout/stderr for any newly exec'd process
 * whose output fd is NOT a TTY.  This catches cron jobs, systemd services,
 * and any process whose output would otherwise be silently discarded.
 *
 * Mechanism:
 *   1. The sched_process_exec BPF tracepoint fires on every successful
 *      execve().  It emits a FDMON_BPF_EXEC event to userspace.
 *   2. handle_event() sees the exec event and, if orphan_stdout_mode is
 *      set, calls maybe_register_orphan_fds() which inspects
 *      /proc/<pid>/fd/1 and /proc/<pid>/fd/2 via readlink().
 *   3. If the target is a pipe, regular file, or /dev/null (anything that
 *      is not a real TTY), a pid→fd_mask entry is inserted into the
 *      monitored_pids BPF map.
 *   4. From that point on, write()/writev()/pwrite64() calls from that
 *      process to those fds are captured and published on the event bus
 *      as EVEMON_EVENT_FD_WRITE events.
 *
 * Note: entries for short-lived processes accumulate in the BPF map.
 * Callers are responsible for periodically pruning dead PIDs via
 * fdmon_remove_pid_fd(), or rely on map eviction when it fills.
 */
int fdmon_orphan_stdout_enable(struct fdmon_ctx *ctx)
{
    if (!ctx || !ctx->ebpf_state) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;

    /* sched_process_exec tracepoint must be attached */
    if (!st->link_sched_exec) {
        fprintf(stderr, "evemon: orphan_stdout: sched_process_exec tracepoint not attached\n");
        errno = ENOTSUP;
        return -1;
    }

    /* Ensure the write tracepoints are live */
    if (!st->link_enter_write) {
        if (fdmon_write_enable(ctx) != 0)
            return -1;
    }

    atomic_store_explicit(&st->orphan_stdout_mode, 1, memory_order_relaxed);
    return 0;
}

void fdmon_orphan_stdout_disable(struct fdmon_ctx *ctx)
{
    if (!ctx || !ctx->ebpf_state) return;
    ebpf_state_t *st = ctx->ebpf_state;
    atomic_store_explicit(&st->orphan_stdout_mode, 0, memory_order_relaxed);
}

/*
 * Register a parent PID so that when any of its direct children call
 * execve(), their fd 1 and/or fd 2 are inserted into the BPF map
 * immediately in the reader thread — before the child can write.
 *
 * fd_mask: bit 0 = fd 1 (stdout), bit 1 = fd 2 (stderr).
 * This replaces any existing entry for the same pid.
 */
int fdmon_watch_parent_fds(struct fdmon_ctx *ctx, pid_t pid, int fd_mask)
{
    if (!ctx || !ctx->ebpf_state || pid <= 0) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;

    /* Ensure the sched_process_exec tracepoint is attached */
    if (!st->link_sched_exec) {
        errno = ENOTSUP;
        return -1;
    }

    /* Register the root pid directly in the BPF map so its own writes
     * (and those of already-forked children) are captured immediately,
     * without waiting for any exec/fork event. */
    if (st->monitored_pids_map_fd >= 0 && fd_mask) {
        __u32 key = (uint32_t)pid;
        __u8  val = (__u8)fd_mask;
        bpf_map_update_elem(st->monitored_pids_map_fd, &key, &val, BPF_ANY);
    }

    pthread_mutex_lock(&st->watched_parent_lock);

    /* Update existing entry if present */
    for (int i = 0; i < st->watched_parent_count; i++) {
        if (st->watched_parents[i].pid == pid) {
            st->watched_parents[i].fd_mask = fd_mask;
            pthread_mutex_unlock(&st->watched_parent_lock);
            return 0;
        }
    }

    /* Add new entry if there is room */
    if (st->watched_parent_count >= WATCHED_PARENT_MAX) {
        pthread_mutex_unlock(&st->watched_parent_lock);
        errno = ENOMEM;
        return -1;
    }
    st->watched_parents[st->watched_parent_count].pid     = pid;
    st->watched_parents[st->watched_parent_count].fd_mask = fd_mask;
    st->watched_parent_count++;

    pthread_mutex_unlock(&st->watched_parent_lock);
    return 0;
}

void fdmon_unwatch_parent_fds(struct fdmon_ctx *ctx, pid_t pid)
{
    if (!ctx || !ctx->ebpf_state || pid <= 0) return;
    ebpf_state_t *st = ctx->ebpf_state;

    /* Walk the entire monitored_pids map and delete every entry.
     * The fork BPF program will have propagated the root pid's entry to
     * every descendant, so we can't know which pids were added — the only
     * safe thing is to flush the whole map.
     *
     * bpf_map_get_next_key semantics: passing NULL gets the first key;
     * we collect all keys first, then delete, to avoid iterator invalidation. */
    if (st->monitored_pids_map_fd >= 0) {
        __u32 keys[8192];
        int   nkeys = 0;
        __u32 cur, next;
        int   rc = bpf_map_get_next_key(st->monitored_pids_map_fd, NULL, &next);
        while (rc == 0 && nkeys < (int)(sizeof(keys)/sizeof(keys[0]))) {
            cur = next;
            keys[nkeys++] = cur;
            rc = bpf_map_get_next_key(st->monitored_pids_map_fd, &cur, &next);
        }
        for (int i = 0; i < nkeys; i++)
            bpf_map_delete_elem(st->monitored_pids_map_fd, &keys[i]);
    }

    pthread_mutex_lock(&st->watched_parent_lock);
    for (int i = 0; i < st->watched_parent_count; i++) {
        if (st->watched_parents[i].pid == pid) {
            st->watched_parents[i] =
                st->watched_parents[--st->watched_parent_count];
            break;
        }
    }
    pthread_mutex_unlock(&st->watched_parent_lock);
}
