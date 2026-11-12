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
#include "desktop_session.h"
#include "../ipc/query_events.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
/* For publishing write events directly to the event bus */
#include "../plugins/evemon_plugin.h"
#include "../plugins/event_bus.h"

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
            uint64_t inode;  /* socket inode — primary match key */
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

/*
 * One write-capture registration in the monitored_pids map, keyed by pid.
 * A write to fd 1 (bit 0) or fd 2 (bit 1) is captured when any of the four
 * masks has the fd's bit.  Userspace owns `own` and `root`; the kernel fills
 * `inh` and `origin` when a registered process forks.  Must match
 * fdmon_ebpf_kern.c.
 */
struct fdmon_watch {
    __u8  own;       /* registered for this process alone                 */
    __u8  root;      /* registered for this process and what it forks     */
    __u8  inh;       /* copied from the parent at fork                    */
    __u8  orphan;    /* automatically registered non-TTY output */
    __u32 origin;    /* registered process `inh` was copied from, or 0    */
};

/* Kernel structure offsets for the BPF programs.  Must match
 * fdmon_ebpf_kern.c. */
struct fdmon_config {
    __u32 sock_inode_known;
    __u32 sk_socket_off;     /* struct sock          :: sk_socket */
    __u32 socket_file_off;   /* struct socket        :: file      */
    __u32 file_inode_off;    /* struct file          :: f_inode   */
    __u32 inode_ino_off;     /* struct inode         :: i_ino     */
    __u32 tuple_known;
    __u32 sk_common_off;
    __u32 daddr_off, saddr_off, dport_off, num_off;
    __u32 group_live_known;
    __u32 task_signal_off;   /* struct task_struct   :: signal    */
    __u32 signal_live_off;   /* struct signal_struct :: live      */
};

typedef struct {
    struct bpf_object     *obj;
    /* Open/close tracing: attached only while a PID group is watched. */
    struct bpf_link       *link_enter_openat;
    struct bpf_link       *link_exit_openat;
    struct bpf_link       *link_enter_close;
    /* Write capture: attached only while something is registered. */
    struct bpf_link       *link_enter_write;
    struct bpf_link       *link_enter_writev;
    struct bpf_link       *link_enter_pwrite64;
    struct bpf_link       *link_enter_sendto;
    struct bpf_link       *link_enter_sendmsg;
    struct bpf_link       *link_enter_sendmmsg;
    struct bpf_link       *link_enter_splice;
    struct bpf_link       *link_enter_sendfile;
    /* Network byte accounting: an entry kprobe and a kretprobe per function. */
    struct bpf_link       *link_tcp_sendmsg_entry;
    struct bpf_link       *link_tcp_sendmsg_ret;
    struct bpf_link       *link_tcp_recvmsg_entry;
    struct bpf_link       *link_tcp_recvmsg_ret;
    struct bpf_link       *link_udp_sendmsg_entry;
    struct bpf_link       *link_udp_sendmsg_ret;
    struct bpf_link       *link_udp_recvmsg_entry;
    struct bpf_link       *link_udp_recvmsg_ret;
    /* udpv6_sendmsg / udpv6_recvmsg — separate kernel functions for IPv6 UDP */
    struct bpf_link       *link_udpv6_sendmsg_entry;
    struct bpf_link       *link_udpv6_sendmsg_ret;
    struct bpf_link       *link_udpv6_recvmsg_entry;
    struct bpf_link       *link_udpv6_recvmsg_ret;
    /* sched_process_exec: exec events, orphan-stdout detection */
    struct bpf_link       *link_sched_exec;
    /* task_newtask: copy a registration to the child at fork */
    struct bpf_link       *link_task_newtask;
    /* sched_process_exit: drop the registrations of tasks that are gone */
    struct bpf_link       *link_sched_exit;
    struct perf_buffer    *pb;
    pthread_t              reader_thread;
    atomic_int             running;
    int                    monitored_pids_map_fd;
    /* When non-zero, auto-register fd 1/2 of any newly exec'd process
     * whose stdout/stderr is not a TTY (orphan stdout capture mode). */
    atomic_int             orphan_stdout_mode;
    /* Serialises every read-modify-write of monitored_pids, the sweep over
     * it, and attaching or detaching the on-demand probes. */
    pthread_mutex_t        lock;
    /* A registration was narrowed or withdrawn: its inherited copies in the
     * map are stale until the reader thread sweeps. */
    atomic_int             sweep_pending;
} ebpf_state_t;

/* ── extern declarations ─────────────────────────────────────── */

/*
 * submit_event is defined in fdmon.c (non-static) and declared in
 * fdmon_internal.h.  struct fdmon_ctx is fully defined there too,
 * so we can access ctx->ebpf_state directly.
 */



/* ── probes attached on demand ───────────────────────────────── */

static int silent_print(enum libbpf_print_level level,
                        const char *fmt, va_list ap);

static struct bpf_link *attach_program(ebpf_state_t *st, const char *name)
{
    struct bpf_program *prog = bpf_object__find_program_by_name(st->obj, name);
    if (!prog) return NULL;
    struct bpf_link *link = bpf_program__attach(prog);
    return !link || libbpf_get_error(link) ? NULL : link;
}

static void detach_link(struct bpf_link **link)
{
    if (*link) bpf_link__destroy(*link);
    *link = NULL;
}

/* An entry probe alone would only fill its stash map, so the return probe
 * goes on first and the pair is all or nothing. */
static void attach_pair(ebpf_state_t *st, const char *entry, const char *ret,
                        struct bpf_link **entry_link, struct bpf_link **ret_link)
{
    *ret_link   = attach_program(st, ret);
    *entry_link = *ret_link ? attach_program(st, entry) : NULL;
    if (!*entry_link) detach_link(ret_link);
}

static void open_probes_detach(ebpf_state_t *st)
{
    detach_link(&st->link_enter_close);
    detach_link(&st->link_exit_openat);
    detach_link(&st->link_enter_openat);
}

/* Every open and close on the machine becomes a perf event while these are
 * attached, so they are only attached while someone consumes the events. */
static int open_probes_attach(ebpf_state_t *st)
{
    if (st->link_enter_openat) return 0;
    st->link_enter_openat = attach_program(st, "trace_enter_openat");
    st->link_exit_openat  = attach_program(st, "trace_exit_openat");
    st->link_enter_close  = attach_program(st, "trace_enter_close");
    if (st->link_enter_openat && st->link_exit_openat && st->link_enter_close)
        return 0;
    open_probes_detach(st);
    return -1;
}

static void write_probes_detach(ebpf_state_t *st)
{
    detach_link(&st->link_enter_sendfile);
    detach_link(&st->link_enter_splice);
    detach_link(&st->link_enter_sendmmsg);
    detach_link(&st->link_enter_sendmsg);
    detach_link(&st->link_enter_sendto);
    detach_link(&st->link_enter_pwrite64);
    detach_link(&st->link_enter_writev);
    detach_link(&st->link_enter_write);
}

/* st->lock held.  trace_enter_write is required; kernels may lack some of
 * the other syscall tracepoints, and those are simply left out. */
static int write_probes_attach(ebpf_state_t *st)
{
    if (st->link_enter_write) return 0;
    libbpf_print_fn_t old_print = libbpf_set_print(silent_print);
    st->link_enter_write = attach_program(st, "trace_enter_write");
    if (st->link_enter_write) {
        st->link_enter_writev   = attach_program(st, "trace_enter_writev");
        st->link_enter_pwrite64 = attach_program(st, "trace_enter_pwrite64");
        st->link_enter_sendto   = attach_program(st, "trace_enter_sendto");
        st->link_enter_sendmsg  = attach_program(st, "trace_enter_sendmsg");
        st->link_enter_sendmmsg = attach_program(st, "trace_enter_sendmmsg");
        st->link_enter_splice   = attach_program(st, "trace_enter_splice");
        st->link_enter_sendfile = attach_program(st, "trace_enter_sendfile");
    }
    libbpf_set_print(old_print);
    return st->link_enter_write ? 0 : -1;
}

/* ── write-capture registrations ─────────────────────────────── */

/*
 * Change what userspace has registered for one process: OR `own_set` into
 * and clear `own_clear` from its own mask, and with `set_root` replace its
 * tree mask.  Inherited bits are the kernel's and are left alone; an entry
 * with no bits left is removed.  st->lock held.  Returns 0, or -1 with errno.
 */
static int watch_update(ebpf_state_t *st, pid_t pid, __u8 own_set,
                        __u8 own_clear, int set_root, __u8 root, int orphan)
{
    int fd = st->monitored_pids_map_fd;
    __u32 key = (__u32)pid;
    if (fd < 0) { errno = ENOENT; return -1; }

    /* The kernel adds an entry when the parent forks and removes it when
     * the task exits, either of which can happen between the lookup and
     * the update; EXIST/NOEXIST make that visible instead of overwriting
     * an inherited mask or bringing a dead task's entry back. */
    for (int attempt = 0; attempt < 4; ++attempt) {
        struct fdmon_watch w = {0};
        int existed = bpf_map_lookup_elem(fd, &key, &w) == 0;
        w.own = (__u8)((w.own | own_set) & ~own_clear);
        if (set_root) w.root = root;
        if (orphan >= 0) w.orphan = (__u8)orphan;
        if (!(w.own | w.root | w.inh | w.orphan)) {
            if (existed) bpf_map_delete_elem(fd, &key);
            return 0;
        }
        if (bpf_map_update_elem(fd, &key, &w, existed ? BPF_EXIST : BPF_NOEXIST) == 0) {
            if (!existed && kill(pid, 0) != 0 && errno == ESRCH)
                bpf_map_delete_elem(fd, &key);   /* it exited before we wrote */
            return 0;
        }
        if (errno != ENOENT && errno != EEXIST) return -1;
    }
    errno = EAGAIN;
    return -1;
}

/*
 * Userspace stand-in for trace_task_newtask on kernels where that program
 * could not be attached: when a child execs, give it its parent's masks the
 * way the kernel would have at fork.
 */
static void watch_inherit(ebpf_state_t *st, pid_t child, pid_t parent)
{
    int fd = st->monitored_pids_map_fd;
    __u32 parent_key = (__u32)parent, child_key = (__u32)child;
    struct fdmon_watch p, w = {0};
    if (fd < 0) return;

    pthread_mutex_lock(&st->lock);
    if (bpf_map_lookup_elem(fd, &parent_key, &p) == 0) {
        w.inh    = p.root | p.inh;
        w.origin = p.root ? parent_key : p.origin;
        if (w.inh && bpf_map_update_elem(fd, &child_key, &w, BPF_NOEXIST) == 0 &&
            kill(child, 0) != 0 && errno == ESRCH)
            bpf_map_delete_elem(fd, &child_key);
    }
    pthread_mutex_unlock(&st->lock);
}

typedef struct {
    __u32              pid;
    struct fdmon_watch w;
    int                changed;
} watch_row_t;

static int watch_row_order(const void *a, const void *b)
{
    __u32 x = ((const watch_row_t *)a)->pid, y = ((const watch_row_t *)b)->pid;
    return x < y ? -1 : x > y;
}

/*
 * Bring the inherited halves of the map back in line with the registrations
 * they were copied from.  The kernel copies a registered process's masks to
 * everything it forks; when userspace later narrows or withdraws that
 * registration, this pass narrows or removes the copies instead of flushing
 * the whole map, so other registrations are untouched.  It also drops entries
 * of tasks that no longer exist, and detaches the write probes once nothing
 * is left to capture.  st->lock held.
 */
static void watch_sweep(ebpf_state_t *st)
{
    int fd = st->monitored_pids_map_fd;
    if (fd < 0) return;

    size_t count = 0, cap = 256, steps = 0;
    watch_row_t *rows = malloc(cap * sizeof(*rows));
    if (!rows) return;

    /* get_next_key starts over from the first key when the key it is given
     * has just been removed, and tasks exit all the time: the walk may see a
     * key twice.  Duplicates are dropped below; `steps` bounds the walk. */
    __u32 key = 0, next = 0;
    int rc = bpf_map_get_next_key(fd, NULL, &next);
    while (rc == 0 && steps++ < (1u << 18)) {
        key = next;
        if (count == cap) {
            watch_row_t *grown = realloc(rows, 2 * cap * sizeof(*rows));
            if (!grown) break;
            rows = grown;
            cap *= 2;
        }
        rows[count].pid = key;
        rows[count].changed = 0;
        if (bpf_map_lookup_elem(fd, &key, &rows[count].w) == 0) count++;
        rc = bpf_map_get_next_key(fd, &key, &next);
    }
    int complete = rc != 0 && errno == ENOENT;   /* reached the end of the map */
    if (!complete) { free(rows); atomic_store(&st->sweep_pending, 1); return; }
    qsort(rows, count, sizeof(*rows), watch_row_order);
    size_t unique = 0;
    for (size_t i = 0; i < count; i++)
        if (!unique || rows[unique - 1].pid != rows[i].pid) rows[unique++] = rows[i];
    count = unique;

    /* Tasks whose exit went unseen, and thread ids retired by an exec. */
    for (size_t i = 0; i < count; i++) {
        if (rows[i].w.orphan && !atomic_load(&st->orphan_stdout_mode)) {
            rows[i].w.orphan = 0;
            rows[i].changed = 1;
        }
        if (kill((pid_t)rows[i].pid, 0) != 0 && errno == ESRCH) {
            rows[i].w = (struct fdmon_watch){0};
            rows[i].changed = 1;
        }
    }

    /* An heir's mask is whatever its origin captures now.  Origins can be
     * heirs of an outer registration themselves, hence the passes. */
    for (int pass = 0, moved = 1; moved && pass < 8; pass++) {
        moved = 0;
        for (size_t i = 0; i < count; i++) {
            watch_row_t *r = &rows[i];
            if (!r->w.inh && !r->w.origin) continue;
            watch_row_t probe = {.pid = r->w.origin};
            watch_row_t *o = r->w.origin != r->pid
                ? bsearch(&probe, rows, count, sizeof(*rows), watch_row_order) : NULL;
            __u8 inh = 0;
            __u32 origin = 0;
            if (o && o->w.root) {
                inh = o->w.root | o->w.inh;
                origin = o->pid;
            } else if (o && o->w.inh && o->w.origin) {
                /* Its origin is only an heir itself now: answer to the same
                 * registration that one does. */
                inh = o->w.inh;
                origin = o->w.origin;
            }
            if (!inh) origin = 0;
            if (inh != r->w.inh || origin != r->w.origin) {
                r->w.inh = inh;
                r->w.origin = origin;
                r->changed = moved = 1;
            }
        }
    }

    size_t remaining = 0;
    for (size_t i = 0; i < count; i++) {
        watch_row_t *r = &rows[i];
        if (!(r->w.own | r->w.root | r->w.inh | r->w.orphan)) {
            bpf_map_delete_elem(fd, &r->pid);
            continue;
        }
        remaining++;
        /* EXIST: a task that exited meanwhile must stay gone. */
        if (r->changed) bpf_map_update_elem(fd, &r->pid, &r->w, BPF_EXIST);
    }
    free(rows);

    if (complete && !remaining &&
        !atomic_load_explicit(&st->orphan_stdout_mode, memory_order_relaxed))
        write_probes_detach(st);
}

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

    pthread_mutex_lock(&st->lock);
    watch_update(st, pid, 0, 0, 0, 0, mask);
    pthread_mutex_unlock(&st->lock);
}

/* Parent of a process, or 0 when it has already gone. */
static pid_t proc_ppid(pid_t pid)
{
    char path[64], line[128];
    pid_t ppid = 0;
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PPid:", 5) == 0) {
            ppid = (pid_t)atoi(line + 5);
            break;
        }
    }
    fclose(f);
    return ppid;
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
        /* /proc is the quickest reliable source for the parent, and is
         * there as soon as execve has completed. */
        pid_t ppid = proc_ppid(new_pid);

        if (st && atomic_load_explicit(&st->orphan_stdout_mode,
                                       memory_order_relaxed)) {
            maybe_register_orphan_fds(st, new_pid);
        }

        /* The kernel copies a registration to the child at fork.  Where
         * that program is not attached, do it here, in the reader thread,
         * before the new image has had much chance to write. */
        if (st && !st->link_task_newtask && ppid > 0)
            watch_inherit(st, new_pid, ppid);

        /* Publish CHILD_EXEC unconditionally so write-monitor subscribers
         * can immediately register the new process's fds in the BPF map
         * before it has a chance to call write(). */
        evemon_exec_payload_t xp;
        xp.pid  = new_pid;
        xp.ppid = ppid;
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
                          ev->net.inode,
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

/* While anything is being captured the registrations are re-checked this
 * often even if nobody asked, which also notices that the last one is gone. */
#define WATCH_SWEEP_SECONDS 5

static void *ebpf_reader_thread(void *arg)
{
    ebpf_state_t *st = arg;
    pthread_setname_np(pthread_self(), "ev-ebpf");
    struct timespec swept;
    clock_gettime(CLOCK_MONOTONIC, &swept);
    while (atomic_load_explicit(&st->running, memory_order_relaxed)) {
        /* poll for 200ms then check running flag */
        perf_buffer__poll(st->pb, 200);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int due = atomic_exchange_explicit(&st->sweep_pending, 0, memory_order_relaxed);
        pthread_mutex_lock(&st->lock);
        if (st->link_enter_write && now.tv_sec - swept.tv_sec >= WATCH_SWEEP_SECONDS)
            due = 1;
        if (due) {
            watch_sweep(st);
            swept = now;
        }
        pthread_mutex_unlock(&st->lock);
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
    struct stat st;
#define BPF_OBJECT_TRUSTED(path_)                                             \
    (lstat((path_), &st) == 0 && S_ISREG(st.st_mode) &&                       \
     !(st.st_mode & (S_IWGRP | S_IWOTH)) &&                                  \
     desktop_session_trusts_owner(st.st_uid))

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
                if (BPF_OBJECT_TRUSTED(buf))
                    return 0;
            }
        }
    }

    /* 2. Try the compiled-in install path */
#ifdef EVEMON_LIBDIR
    snprintf(buf, bufsz, "%s/fdmon_ebpf_kern.o", EVEMON_LIBDIR);
    if (BPF_OBJECT_TRUSTED(buf))
        return 0;
#endif

#undef BPF_OBJECT_TRUSTED

    return -1;
}

/* ── public init / destroy ───────────────────────────────────── */

/* Resolve named members, including those inside anonymous unions/structs. */
static int btf_member_offset(const struct btf *btf, int id, const char *name,
                              __u32 *offset, int depth)
{
    if (id <= 0 || depth > 8) return -1;
    id = btf__resolve_type(btf, (__u32)id);
    const struct btf_type *t = id > 0 ? btf__type_by_id(btf, (__u32)id) : NULL;
    if (!t || (!btf_is_struct(t) && !btf_is_union(t))) return -1;
    const struct btf_member *m = btf_members(t);
    for (unsigned i = 0; i < btf_vlen(t); i++) {
        const char *member = btf__name_by_offset(btf, m[i].name_off);
        __u32 bits = btf_member_bit_offset(t, i), nested = 0;
        if (bits % 8 || btf_member_bitfield_size(t, i)) continue;
        if (member && !strcmp(member, name)) {
            *offset = bits / 8;
            return 0;
        }
        if ((!member || !*member) &&
            !btf_member_offset(btf, m[i].type, name, &nested, depth + 1)) {
            *offset = bits / 8 + nested;
            return 0;
        }
    }
    return -1;
}

static int kernel_offset(const struct btf *btf, const char *type,
                          const char *member, __u32 *offset)
{
    return btf_member_offset(btf, btf__find_by_name_kind(btf, type, BTF_KIND_STRUCT),
                               member, offset, 0);
}

static void configure_kernel_offsets(ebpf_state_t *st)
{
    struct fdmon_config cfg = {0};
    struct btf *btf = btf__load_vmlinux_btf();
    if (btf && !libbpf_get_error(btf)) {
        cfg.sock_inode_known =
            !kernel_offset(btf, "sock", "sk_socket", &cfg.sk_socket_off) &&
            !kernel_offset(btf, "socket", "file", &cfg.socket_file_off) &&
            !kernel_offset(btf, "file", "f_inode", &cfg.file_inode_off) &&
            !kernel_offset(btf, "inode", "i_ino", &cfg.inode_ino_off);
        cfg.tuple_known =
            !kernel_offset(btf, "sock", "__sk_common", &cfg.sk_common_off) &&
            !kernel_offset(btf, "sock_common", "skc_daddr", &cfg.daddr_off) &&
            !kernel_offset(btf, "sock_common", "skc_rcv_saddr", &cfg.saddr_off) &&
            !kernel_offset(btf, "sock_common", "skc_dport", &cfg.dport_off) &&
            !kernel_offset(btf, "sock_common", "skc_num", &cfg.num_off);
        cfg.group_live_known =
            !kernel_offset(btf, "task_struct", "signal", &cfg.task_signal_off) &&
            !kernel_offset(btf, "signal_struct", "live", &cfg.signal_live_off);
        btf__free(btf);
    }
    int fd = bpf_object__find_map_fd_by_name(st->obj, "fdmon_config");
    __u32 key = 0;
    if (fd >= 0) bpf_map_update_elem(fd, &key, &cfg, BPF_ANY);
}

static void ebpf_state_free(ebpf_state_t *st)
{
    if (!st) return;
    open_probes_detach(st);
    write_probes_detach(st);
    detach_link(&st->link_tcp_sendmsg_entry);
    detach_link(&st->link_tcp_sendmsg_ret);
    detach_link(&st->link_tcp_recvmsg_entry);
    detach_link(&st->link_tcp_recvmsg_ret);
    detach_link(&st->link_udp_sendmsg_entry);
    detach_link(&st->link_udp_sendmsg_ret);
    detach_link(&st->link_udp_recvmsg_entry);
    detach_link(&st->link_udp_recvmsg_ret);
    detach_link(&st->link_udpv6_sendmsg_entry);
    detach_link(&st->link_udpv6_sendmsg_ret);
    detach_link(&st->link_udpv6_recvmsg_entry);
    detach_link(&st->link_udpv6_recvmsg_ret);
    detach_link(&st->link_task_newtask);
    detach_link(&st->link_sched_exec);
    detach_link(&st->link_sched_exit);
    if (st->pb) perf_buffer__free(st->pb);
    if (st->obj) bpf_object__close(st->obj);
    pthread_mutex_destroy(&st->lock);
    free(st);
}

int fdmon_ebpf_init(struct fdmon_ctx *ctx)
{
    libbpf_print_fn_t old_print = libbpf_set_print(silent_print);
    struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
    (void)setrlimit(RLIMIT_MEMLOCK, &rl);
    int ret = -1;
    ebpf_state_t *st = calloc(1, sizeof(*st));
    if (!st) goto out;
    pthread_mutex_init(&st->lock, NULL);
    st->monitored_pids_map_fd = -1;

    char obj_path[PATH_MAX];
    if (find_bpf_object(obj_path, sizeof(obj_path))) goto fail;
    st->obj = bpf_object__open(obj_path);
    if (!st->obj || libbpf_get_error(st->obj)) {
        st->obj = NULL;
        goto fail;
    }
    /* Reject stale objects before exchanging differently-sized map values. */
    struct bpf_map *scratch = bpf_object__find_map_by_name(st->obj, "write_event_scratch");
    struct bpf_map *watches = bpf_object__find_map_by_name(st->obj, "monitored_pids");
    struct bpf_map *config = bpf_object__find_map_by_name(st->obj, "fdmon_config");
    if (!scratch || bpf_map__value_size(scratch) != sizeof(struct fdmon_bpf_event) ||
        !watches || bpf_map__value_size(watches) != sizeof(struct fdmon_watch) ||
        !config || bpf_map__value_size(config) != sizeof(struct fdmon_config)) {
        fprintf(stderr, "evemon: BPF object layout mismatch; rebuild fdmon_ebpf_kern.o\n");
        goto fail;
    }
    if (bpf_object__load(st->obj)) goto fail;
    configure_kernel_offsets(st);
    st->monitored_pids_map_fd = bpf_map__fd(watches);

    /* Validate open/close support, then leave it idle until somebody watches. */
    if (open_probes_attach(st)) goto fail;
    open_probes_detach(st);
    st->link_sched_exit = attach_program(st, "trace_sched_process_exit");
    if (!st->link_sched_exit) goto fail;
    st->link_sched_exec = attach_program(st, "trace_sched_process_exec");
    st->link_task_newtask = attach_program(st, "trace_task_newtask");

    attach_pair(st, "trace_tcp_sendmsg", "trace_tcp_sendmsg_ret",
                &st->link_tcp_sendmsg_entry, &st->link_tcp_sendmsg_ret);
    attach_pair(st, "trace_tcp_recvmsg", "trace_tcp_recvmsg_ret",
                &st->link_tcp_recvmsg_entry, &st->link_tcp_recvmsg_ret);
    attach_pair(st, "trace_udp_sendmsg", "trace_udp_sendmsg_ret",
                &st->link_udp_sendmsg_entry, &st->link_udp_sendmsg_ret);
    attach_pair(st, "trace_udp_recvmsg", "trace_udp_recvmsg_ret",
                &st->link_udp_recvmsg_entry, &st->link_udp_recvmsg_ret);
    attach_pair(st, "trace_udpv6_sendmsg", "trace_udpv6_sendmsg_ret",
                &st->link_udpv6_sendmsg_entry, &st->link_udpv6_sendmsg_ret);
    attach_pair(st, "trace_udpv6_recvmsg", "trace_udpv6_recvmsg_ret",
                &st->link_udpv6_recvmsg_entry, &st->link_udpv6_recvmsg_ret);

    int map_fd = bpf_object__find_map_fd_by_name(st->obj, "events");
    if (map_fd < 0) goto fail;
    st->pb = perf_buffer__new(map_fd, 64, handle_event, handle_lost, ctx, NULL);
    if (!st->pb || libbpf_get_error(st->pb)) {
        st->pb = NULL;
        goto fail;
    }
    /* The callback can run as soon as the reader starts. */
    ctx->ebpf_state = st;
    atomic_store(&st->running, 1);
    if (pthread_create(&st->reader_thread, NULL, ebpf_reader_thread, st)) {
        ctx->ebpf_state = NULL;
        goto fail;
    }
    ret = 0;
    goto out;
fail:
    ebpf_state_free(st);
out:
    libbpf_set_print(old_print);
    return ret;
}

void fdmon_ebpf_destroy(struct fdmon_ctx *ctx)
{
    if (!ctx || !ctx->ebpf_state) return;
    ebpf_state_t *st = ctx->ebpf_state;
    atomic_store(&st->running, 0);
    pthread_join(st->reader_thread, NULL);
    ctx->ebpf_state = NULL;
    ebpf_state_free(st);
}

int fdmon_ebpf_open_enable(fdmon_ctx_t *ctx, int enabled)
{
    ebpf_state_t *st = ctx->ebpf_state;
    if (!st) { errno = ENOTSUP; return -1; }
    pthread_mutex_lock(&st->lock);
    int rc = 0;
    if (enabled) rc = open_probes_attach(st);
    else open_probes_detach(st);
    pthread_mutex_unlock(&st->lock);
    return rc;
}

int fdmon_write_enable(struct fdmon_ctx *ctx)
{
    if (ctx && ctx->remote) { if (query_fdmon_backend(ctx) == FDMON_BACKEND_EBPF) return 0; errno = ENOTSUP; return -1; }
    if (!ctx || !ctx->ebpf_state) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;
    pthread_mutex_lock(&st->lock);
    int rc = write_probes_attach(st);
    pthread_mutex_unlock(&st->lock);
    return rc;
}

void fdmon_write_disable(struct fdmon_ctx *ctx)
{
    if (!ctx || ctx->remote || !ctx->ebpf_state) return;
    ebpf_state_t *st = ctx->ebpf_state;
    pthread_mutex_lock(&st->lock);
    /* Other views may still own registrations. */
    watch_sweep(st);
    pthread_mutex_unlock(&st->lock);
}

int fdmon_add_pid_fd(struct fdmon_ctx *ctx, pid_t pid, int fd)
{
    if (ctx && ctx->remote) return query_fdmon_control(ctx, 1, pid, fd, 0);
    if (!ctx || !ctx->ebpf_state || pid <= 0 || (fd != 1 && fd != 2)) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;
    pthread_mutex_lock(&st->lock);
    int rc = write_probes_attach(st);
    if (!rc) rc = watch_update(st, pid, 1u << (fd - 1), 0, 0, 0, -1);
    pthread_mutex_unlock(&st->lock);
    return rc;
}

int fdmon_remove_pid_fd(struct fdmon_ctx *ctx, pid_t pid, int fd)
{
    if (ctx && ctx->remote) return query_fdmon_control(ctx, 1, pid, fd, 1);
    if (!ctx || !ctx->ebpf_state || pid <= 0 || (fd != 1 && fd != 2)) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;
    pthread_mutex_lock(&st->lock);
    int rc = watch_update(st, pid, 0, 1u << (fd - 1), 0, 0, -1);
    atomic_store(&st->sweep_pending, 1);
    pthread_mutex_unlock(&st->lock);
    return rc;
}

int fdmon_orphan_stdout_enable(struct fdmon_ctx *ctx)
{
    if (ctx && ctx->remote) return query_fdmon_control(ctx, 3, 0, 0, 0);
    if (!ctx || !ctx->ebpf_state) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;
    if (!st->link_sched_exec) { errno = ENOTSUP; return -1; }
    pthread_mutex_lock(&st->lock);
    int rc = write_probes_attach(st);
    if (!rc) atomic_store(&st->orphan_stdout_mode, 1);
    pthread_mutex_unlock(&st->lock);
    return rc;
}

void fdmon_orphan_stdout_disable(struct fdmon_ctx *ctx)
{
    if (ctx && ctx->remote) { query_fdmon_control(ctx, 3, 0, 0, 1); return; }
    if (!ctx || !ctx->ebpf_state) return;
    ebpf_state_t *st = ctx->ebpf_state;
    atomic_store(&st->orphan_stdout_mode, 0);
    atomic_store(&st->sweep_pending, 1);
}

int fdmon_watch_parent_fds(struct fdmon_ctx *ctx, pid_t pid, int fd_mask)
{
    if (ctx && ctx->remote) return query_fdmon_control(ctx, 2, pid, fd_mask, 0);
    if (!ctx || !ctx->ebpf_state || pid <= 0 || (fd_mask & ~3)) { errno = EINVAL; return -1; }
    ebpf_state_t *st = ctx->ebpf_state;
    if (!st->link_task_newtask && !st->link_sched_exec) { errno = ENOTSUP; return -1; }
    pthread_mutex_lock(&st->lock);
    int rc = fd_mask ? write_probes_attach(st) : 0;
    if (!rc) rc = watch_update(st, pid, 0, 0, 1, fd_mask, -1);
    if (!rc) watch_sweep(st);
    pthread_mutex_unlock(&st->lock);
    return rc;
}

void fdmon_unwatch_parent_fds(struct fdmon_ctx *ctx, pid_t pid)
{
    if (ctx && ctx->remote) { query_fdmon_control(ctx, 2, pid, 0, 1); return; }
    if (!ctx || !ctx->ebpf_state || pid <= 0) return;
    ebpf_state_t *st = ctx->ebpf_state;
    pthread_mutex_lock(&st->lock);
    watch_update(st, pid, 0, 0, 1, 0, -1);
    watch_sweep(st);
    pthread_mutex_unlock(&st->lock);
}
