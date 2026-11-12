/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fdmon_ebpf_kern.c – BPF programs for fd-event tracing.
 *
 * Compiled with clang to BPF bytecode at build time:
 *   clang -O2 -target bpf -c -o fdmon_ebpf_kern.o fdmon_ebpf_kern.c
 *
 * Attaches to three tracepoints:
 *   • syscalls/sys_enter_openat  – record that this tid is opening a file
 *   • syscalls/sys_exit_openat   – capture the returned fd number
 *   • syscalls/sys_enter_close   – capture close events
 *
 * Events are pushed to a BPF_MAP_TYPE_PERF_EVENT_ARRAY (or ringbuf)
 * for userspace consumption.
 *
 * We do NOT filter by PID in kernel space — that's done in the
 * userspace reader after receiving the event.  This keeps the BPF
 * program simple and avoids the need to update BPF maps when the
 * watched PID set changes.  The overhead of a few extra events
 * crossing the perf buffer is negligible compared to the syscall
 * overhead itself.
 */

#include <linux/bpf.h>
#include <linux/ptrace.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* ── event structure shared with userspace ───────────────────── */

#define FDMON_BPF_OPEN     1
#define FDMON_BPF_CLOSE    2
#define FDMON_BPF_NET_SEND 3
#define FDMON_BPF_NET_RECV 4
#define FDMON_BPF_PATH_MAX 256
/* write event type */
#define FDMON_BPF_WRITE 5
/*
 * Max bytes to copy from write() payload.
 * 4096 covers a full pipe buffer / typical stdio flush.
 * The event struct itself lives in a per-CPU scratch map to avoid
 * the 512-byte BPF stack limit.
 */
#define FDMON_BPF_WRITE_MAX 4096
/*
 * EXEC event: fired on sched_process_exec (after a successful execve).
 * Carries the new PID/TID so userspace can inspect /proc/<pid>/fd/1,2
 * and decide whether to auto-register those fds for write monitoring.
 * The `path` union member holds the executable filename (best effort).
 */
#define FDMON_BPF_EXEC  6

struct fdmon_bpf_event {
    __u32 type;       /* FDMON_BPF_OPEN / CLOSE / NET_SEND / NET_RECV */
    __u32 pid;        /* tgid (userspace "PID") */
    __u32 tid;        /* kernel tid */
    __s32 fd;         /* file descriptor number (unused for net events) */
    __u64 timestamp;  /* ktime_get_ns() */
    __u64 user_ptr;   /* pointer to user buffer (copied for userspace fallback) */
    __u32 user_count; /* original requested count */
    union {
        char  path[FDMON_BPF_PATH_MAX];
        struct {
            __u32 bytes;  /* bytes sent or received in this call */
            __u32 laddr;  /* local  IPv4 address (network order) */
            __u32 raddr;  /* remote IPv4 address (network order) */
            __u16 lport;  /* local  port (host order)            */
            __u16 rport;  /* remote port (network order)         */
            __u64 inode;  /* socket inode number (for inode-based matching) */
        } net;
        struct {
            __u32 len; /* number of bytes copied into data */
            char   data[FDMON_BPF_WRITE_MAX];
        } write;
    };
};

/*
 * Stack-safe event header for programs that don't carry write payload.
 * All non-write programs (open, close, net, exec) use this struct on
 * the BPF stack — it fits well within the 512-byte limit.
 * The `write_event_scratch` per-CPU map is used only for write programs.
 *
 * Userspace receives both types via the same perf buffer; the `type`
 * field distinguishes them.  For non-write events the `write` union
 * member is never referenced by userspace.
 */
struct fdmon_bpf_event_hdr {
    __u32 type;
    __u32 pid;
    __u32 tid;
    __s32 fd;
    __u64 timestamp;
    __u64 user_ptr;
    __u32 user_count;
    union {
        char  path[FDMON_BPF_PATH_MAX];
        struct {
            __u32 bytes;
            __u32 laddr;
            __u32 raddr;
            __u16 lport;
            __u16 rport;
            __u64 inode;  /* socket inode — primary match key for UDP/IPv6 */
        } net;
    };
};

/* ── maps ────────────────────────────────────────────────────── */

/* Perf event output map – one buffer per CPU. */
struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
} events SEC(".maps");

/*
 * Per-CPU scratch map to stash the filename pointer between
 * sys_enter_openat and sys_exit_openat.
 *
 * Key = tid (u32), value = pointer to filename in user memory.
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 8192);
    __type(key, __u32);
    __type(value, __u64);
} open_args SEC(".maps");

/*
 * Write-capture registrations.  Key = u32 pid (tgid).  In each mask bit 0 is
 * fd 1 and bit 1 is fd 2; a write is captured when any mask has its bit.
 *
 *   own     set by userspace for this process alone
 *   root    set by userspace: this process and everything it forks
 *   inh     copied from the parent by trace_task_newtask at fork
 *   origin  the registered process `inh` was copied from (0 = none)
 *
 * The masks are kept apart so userspace can withdraw one registration without
 * disturbing another, and can find what a withdrawn one had propagated
 * instead of flushing the whole map.  trace_sched_process_exit removes
 * entries, so the map only ever holds live tasks; it is preallocated, which
 * is why it has a fixed size instead of one entry per possible pid.
 * Must match struct fdmon_watch in fdmon_ebpf.c.
 */
struct fdmon_watch {
    __u8  own;
    __u8  root;
    __u8  inh;
    __u8  orphan;    /* automatically registered non-TTY output */
    __u32 origin;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);
    __type(value, struct fdmon_watch);
} monitored_pids SEC(".maps");

/* Nonzero when writes of process `pid` to `fd` are being captured. */
static __always_inline int fd_is_watched(__u32 pid, __u32 fd)
{
    if (fd != 1 && fd != 2)
        return 0;
    struct fdmon_watch *w = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!w)
        return 0;
    return ((w->own | w->root | w->inh | w->orphan) >> (fd - 1)) & 1;
}

/*
 * Kernel structure offsets, filled in by userspace from the running kernel's
 * BTF before any probe is attached.  A zero `*_known` flag means the lookup
 * failed and the fields it guards must not be used.
 * Must match struct fdmon_config in fdmon_ebpf.c.
 */
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

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct fdmon_config);
} fdmon_config SEC(".maps");

/*
 * Per-CPU scratch space for write events.
 * Using a PERCPU_ARRAY with a single slot (key=0) gives us a
 * per-CPU heap allocation of sizeof(fdmon_bpf_event), which can be
 * as large as we like — unlike the 512-byte BPF stack.
 * Each CPU builds its event here, then calls bpf_perf_event_output.
 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct fdmon_bpf_event);
} write_event_scratch SEC(".maps");

/*
 * The struct sock* of a recvmsg or sendmsg call in progress, stashed by the
 * entry kprobe for the kretprobe that learns how many bytes really moved.
 *
 * Key = tid (u32), value = sock pointer (u64).  The return probe always
 * removes its entry, and so does the thread's exit, so a call that returned
 * nothing or was never seen returning cannot use a slot up for good.
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u64);
} recvmsg_args SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u64);
} sendmsg_args SEC(".maps");

/*
 * Read the IPv4 tuple using the running kernel's BTF offsets. Unknown
 * layouts leave the tuple zeroed; per-process byte accounting still works.
 */
static __always_inline void read_sock_tuple(void *sk,
                                            __u32 *laddr, __u32 *raddr,
                                            __u16 *lport, __u16 *rport)
{
    __u32 zero = 0;
    struct fdmon_config *cfg = bpf_map_lookup_elem(&fdmon_config, &zero);
    if (!cfg || !cfg->tuple_known) return;
    void *common = sk + cfg->sk_common_off;
    bpf_probe_read_kernel(raddr, sizeof(*raddr), common + cfg->daddr_off);
    bpf_probe_read_kernel(laddr, sizeof(*laddr), common + cfg->saddr_off);
    bpf_probe_read_kernel(rport, sizeof(*rport), common + cfg->dport_off);
    bpf_probe_read_kernel(lport, sizeof(*lport), common + cfg->num_off);
}

/*
 * Read the socket inode number from struct sock:
 *
 *   sock->sk_socket (struct socket *) ->file (struct file *)
 *       ->f_inode (struct inode *) ->i_ino
 *
 * These members sit at different offsets in every kernel build, so the
 * offsets come from fdmon_config, which userspace fills from the running
 * kernel's BTF.  Without them the inode is reported as 0 and userspace
 * matches the connection by its address 4-tuple instead.
 * Returns 0 on any probe failure (verifier sees the early-out).
 */
static __always_inline __u64 read_sock_inode(void *sk)
{
    void *sk_socket = NULL;
    void *file      = NULL;
    void *inode     = NULL;
    __u64 ino       = 0;
    __u32 zero      = 0;

    struct fdmon_config *cfg = bpf_map_lookup_elem(&fdmon_config, &zero);
    if (!cfg || !cfg->sock_inode_known)
        return 0;

    if (bpf_probe_read_kernel(&sk_socket, sizeof(sk_socket),
                               sk + cfg->sk_socket_off) != 0 || !sk_socket)
        return 0;
    if (bpf_probe_read_kernel(&file, sizeof(file),
                               sk_socket + cfg->socket_file_off) != 0 || !file)
        return 0;
    if (bpf_probe_read_kernel(&inode, sizeof(inode),
                               file + cfg->file_inode_off) != 0 || !inode)
        return 0;
    bpf_probe_read_kernel(&ino, sizeof(ino), inode + cfg->inode_ino_off);
    return ino;
}

/* ── tracepoint context structures (from format files) ───────── */

/*
 * These match the tracepoint format exactly.
 * We only declare the fields we actually read.
 */

struct sys_enter_openat_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* 8 bytes padding for alignment to offset 16 */
    long           dfd;
    const char    *filename;
    long           flags;
    long           mode;
};

struct sys_exit_openat_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* padding */
    long           ret;
};

struct sys_enter_close_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* padding */
    unsigned long  fd;
};

/* ── programs ────────────────────────────────────────────────── */

SEC("tracepoint/syscalls/sys_enter_openat")
int trace_enter_openat(struct sys_enter_openat_args *ctx)
{
    __u32 tid = (__u32)bpf_get_current_pid_tgid();
    __u64 fname_ptr = (__u64)ctx->filename;

    /* Stash the filename pointer so sys_exit can read it. */
    bpf_map_update_elem(&open_args, &tid, &fname_ptr, BPF_ANY);
    return 0;
}

SEC("tracepoint/syscalls/sys_exit_openat")
int trace_exit_openat(struct sys_exit_openat_args *ctx)
{
    __u32 tid = (__u32)bpf_get_current_pid_tgid();

    /* Retrieve stashed filename pointer. */
    __u64 *fname_ptr = bpf_map_lookup_elem(&open_args, &tid);
    if (!fname_ptr) return 0;

    __u64 saved_ptr = *fname_ptr;
    bpf_map_delete_elem(&open_args, &tid);

    /* Only emit event if open succeeded (ret >= 0). */
    long fd = ctx->ret;
    if (fd < 0) return 0;

    __u64 pid_tgid = bpf_get_current_pid_tgid();

    struct fdmon_bpf_event_hdr ev = {};
    ev.type      = FDMON_BPF_OPEN;
    ev.pid       = (__u32)(pid_tgid >> 32);  /* tgid = userspace PID */
    ev.tid       = (__u32)pid_tgid;
    ev.fd        = (__s32)fd;
    ev.timestamp = bpf_ktime_get_ns();
    ev.user_ptr  = 0;
    ev.user_count = 0;

    /* Read the filename from user memory (best effort). */
    bpf_probe_read_user_str(ev.path, sizeof(ev.path), (void *)saved_ptr);

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          &ev, sizeof(ev));
    return 0;
}

SEC("tracepoint/syscalls/sys_enter_close")
int trace_enter_close(struct sys_enter_close_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    struct fdmon_bpf_event_hdr ev = {};
    ev.type      = FDMON_BPF_CLOSE;
    ev.pid       = (__u32)(pid_tgid >> 32);
    ev.tid       = (__u32)pid_tgid;
    ev.fd        = (__s32)ctx->fd;
    ev.timestamp = bpf_ktime_get_ns();
    ev.path[0]   = '\0';

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          &ev, sizeof(ev));
    return 0;
}

/* Capture write() calls (entry) and emit payload for monitored pid/fd */
struct sys_enter_write_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* Each syscall arg occupies an 8-byte slot in the tracepoint context
     * (pt_regs layout), regardless of the declared C type. */
    unsigned long  fd;
    unsigned long  buf;
    unsigned long  count;
};

SEC("tracepoint/syscalls/sys_enter_write")
int trace_enter_write(struct sys_enter_write_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);

    __u32 fd = (__u32)ctx->fd;
    if (!fd_is_watched(pid, fd)) return 0;

    unsigned long cnt = ctx->count;
    if (cnt == 0)
        return 0;

    /* Use per-CPU scratch to avoid the 512-byte BPF stack limit. */
    __u32 zero = 0;
    struct fdmon_bpf_event *ev = bpf_map_lookup_elem(&write_event_scratch, &zero);
    if (!ev) return 0;

    ev->type      = FDMON_BPF_WRITE;
    ev->pid       = pid;
    ev->tid       = (__u32)pid_tgid;
    ev->fd        = (__s32)fd;
    ev->timestamp = bpf_ktime_get_ns();
    ev->user_ptr   = ctx->buf;
    ev->user_count = (__u32)(cnt > 0xffffffff ? 0xffffffff : cnt);

    __u32 to_copy = (__u32)(cnt > FDMON_BPF_WRITE_MAX ? FDMON_BPF_WRITE_MAX : cnt);
    if (to_copy > 0) {
        int _res = bpf_probe_read_user(ev->write.data, to_copy, (void *)ctx->buf);
        ev->write.len = (_res == 0) ? to_copy : 0;
    } else {
        ev->write.len = 0;
    }

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          ev, sizeof(struct fdmon_bpf_event));
    return 0;
}

/* Helper small structs for vectored / message syscalls */
struct iovec_user {
    void *iov_base;
    unsigned long iov_len;
};

struct msghdr_user {
    void *msg_name;
    int    msg_namelen;
    struct iovec_user *msg_iov;
    unsigned long msg_iovlen;
    void *msg_control;
    unsigned long msg_controllen;
    unsigned int msg_flags;
};

/* sys_enter_writev: fd, iov, iovcnt */
struct sys_enter_writev_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* 8-byte slots for each syscall arg */
    unsigned long  fd;
    unsigned long  iov;   /* ptr to iovec array */
    unsigned long  iovcnt;
};

SEC("tracepoint/syscalls/sys_enter_writev")
int trace_enter_writev(struct sys_enter_writev_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);

    __u32 fd = (__u32)ctx->fd;
    if (!fd_is_watched(pid, fd)) return 0;

    if (!ctx->iov || ctx->iovcnt == 0) return 0;

    __u32 zero = 0;
    struct fdmon_bpf_event *ev = bpf_map_lookup_elem(&write_event_scratch, &zero);
    if (!ev) return 0;

    ev->type      = FDMON_BPF_WRITE;
    ev->pid       = pid;
    ev->tid       = (__u32)pid_tgid;
    ev->fd        = (__s32)fd;
    ev->timestamp = bpf_ktime_get_ns();
    ev->user_ptr  = 0;  /* multi-segment; no single pointer */
    ev->user_count = 0;
    ev->write.len = 0;

    /*
     * Copy data from the first iovec that has content.  We use a
     * single fixed-size bpf_probe_read_user so the verifier sees
     * constant offset (0) and constant size (FDMON_BPF_WRITE_MAX),
     * which it can trivially prove fits in write_event_scratch
     * (value_size = sizeof(struct fdmon_bpf_event) = 40 + 4096).
     *
     * Multi-segment writes: we still walk all iovecs to accumulate
     * user_count (total bytes the process intended to write), but
     * we only copy the first non-empty segment's data.
     */
    __u32 dest_off = 0;
    unsigned long iovcnt = ctx->iovcnt;
    if (iovcnt > 16) iovcnt = 16;

    for (unsigned long i = 0; i < 16; i++) {
        if (i >= iovcnt) break;

        struct iovec_user iov;
        if (bpf_probe_read_user(&iov, sizeof(iov),
                                (const void *)(ctx->iov + i * sizeof(iov))) != 0)
            break;
        if (!iov.iov_base || iov.iov_len == 0) continue;

        ev->user_count += (__u32)iov.iov_len;

        /* Only copy the first segment's bytes into the scratch buffer. */
        if (dest_off == 0) {
            __u32 to_copy = (__u32)(iov.iov_len > FDMON_BPF_WRITE_MAX
                                    ? FDMON_BPF_WRITE_MAX : iov.iov_len);
            if (bpf_probe_read_user(ev->write.data, FDMON_BPF_WRITE_MAX,
                                    iov.iov_base) == 0)
                dest_off = to_copy;
        }
    }

    if (dest_off == 0) return 0;
    ev->write.len = dest_off;

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          ev, sizeof(struct fdmon_bpf_event));
    return 0;
}

/* sys_enter_pwrite64: fd, buf, count, pos */
struct sys_enter_pwrite64_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* 8-byte slots for each syscall arg */
    unsigned long  fd;
    unsigned long  buf;   /* ptr to user buffer */
    unsigned long  count;
    unsigned long  pos;
};

SEC("tracepoint/syscalls/sys_enter_pwrite64")
int trace_enter_pwrite64(struct sys_enter_pwrite64_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);

    __u32 fd = (__u32)ctx->fd;
    if (!fd_is_watched(pid, fd)) return 0;

    unsigned long cnt = ctx->count;
    if (cnt == 0) return 0;

    __u32 zero = 0;
    struct fdmon_bpf_event *ev = bpf_map_lookup_elem(&write_event_scratch, &zero);
    if (!ev) return 0;

    ev->type      = FDMON_BPF_WRITE;
    ev->pid       = pid;
    ev->tid       = (__u32)pid_tgid;
    ev->fd        = (__s32)fd;
    ev->timestamp = bpf_ktime_get_ns();
    ev->user_ptr   = ctx->buf;
    ev->user_count = (__u32)(cnt > 0xffffffff ? 0xffffffff : cnt);

    __u32 to_copy = (__u32)(cnt > FDMON_BPF_WRITE_MAX ? FDMON_BPF_WRITE_MAX : cnt);
    if (to_copy > 0) {
        int _res = bpf_probe_read_user(ev->write.data, to_copy, (void *)ctx->buf);
        ev->write.len = (_res == 0) ? to_copy : 0;
    } else {
        ev->write.len = 0;
    }

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          ev, sizeof(struct fdmon_bpf_event));
    return 0;
}

/* sys_enter_sendto: fd, buf, len, flags, dest_addr */
struct sys_enter_sendto_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* 8-byte slots for each syscall arg */
    unsigned long  fd;
    unsigned long  buff;  /* ptr to user buffer (kernel calls it 'buff') */
    unsigned long  len;
    unsigned long  flags;
    unsigned long  addr;
    unsigned long  addrlen;
};

SEC("tracepoint/syscalls/sys_enter_sendto")
int trace_enter_sendto(struct sys_enter_sendto_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);

    __u32 fd = (__u32)ctx->fd;
    if (!fd_is_watched(pid, fd)) return 0;

    unsigned long cnt = ctx->len;
    if (cnt == 0) return 0;

    __u32 zero = 0;
    struct fdmon_bpf_event *ev = bpf_map_lookup_elem(&write_event_scratch, &zero);
    if (!ev) return 0;

    ev->type      = FDMON_BPF_WRITE;
    ev->pid       = pid;
    ev->tid       = (__u32)pid_tgid;
    ev->fd        = (__s32)fd;
    ev->timestamp = bpf_ktime_get_ns();
    ev->user_ptr   = ctx->buff;
    ev->user_count = (__u32)(cnt > 0xffffffff ? 0xffffffff : cnt);

    __u32 to_copy = (__u32)(cnt > FDMON_BPF_WRITE_MAX ? FDMON_BPF_WRITE_MAX : cnt);
    if (to_copy > 0) {
        int _res = bpf_probe_read_user(ev->write.data, to_copy, (void *)ctx->buff);
        ev->write.len = (_res == 0) ? to_copy : 0;
    } else {
        ev->write.len = 0;
    }

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          ev, sizeof(struct fdmon_bpf_event));
    return 0;
}

/* sys_enter_sendmsg: fd, msg, flags */
struct sys_enter_sendmsg_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* 8-byte slots for each syscall arg */
    unsigned long  fd;
    unsigned long  msg;   /* ptr to msghdr */
    unsigned long  flags;
};

SEC("tracepoint/syscalls/sys_enter_sendmsg")
int trace_enter_sendmsg(struct sys_enter_sendmsg_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);

    __u32 fd = (__u32)ctx->fd;
    if (!fd_is_watched(pid, fd)) return 0;

    struct msghdr_user msgh;
    if (bpf_probe_read_user(&msgh, sizeof(msgh), (void *)ctx->msg) != 0)
        return 0;
    if (!msgh.msg_iov || msgh.msg_iovlen == 0)
        return 0;

    __u32 zero = 0;
    struct fdmon_bpf_event *ev = bpf_map_lookup_elem(&write_event_scratch, &zero);
    if (!ev) return 0;

    ev->type      = FDMON_BPF_WRITE;
    ev->pid       = pid;
    ev->tid       = (__u32)pid_tgid;
    ev->fd        = (__s32)fd;
    ev->timestamp = bpf_ktime_get_ns();
    ev->user_ptr  = 0;
    ev->user_count = 0;
    ev->write.len = 0;

    /* Walk all iovecs of the msghdr, same as writev (cap at 16). */
    unsigned long iovcnt = msgh.msg_iovlen;
    if (iovcnt > 16) iovcnt = 16;
    __u32 dest_off = 0;

    for (unsigned long i = 0; i < 16; i++) {
        if (i >= iovcnt) break;
        struct iovec_user iov;
        if (bpf_probe_read_user(&iov, sizeof(iov),
                                (const void *)((unsigned long)msgh.msg_iov +
                                               i * sizeof(iov))) != 0)
            break;
        if (!iov.iov_base || iov.iov_len == 0) continue;

        ev->user_count += (__u32)iov.iov_len;

        if (dest_off == 0) {
            __u32 to_copy = (__u32)(iov.iov_len > FDMON_BPF_WRITE_MAX
                                    ? FDMON_BPF_WRITE_MAX : iov.iov_len);
            if (bpf_probe_read_user(ev->write.data, FDMON_BPF_WRITE_MAX,
                                    iov.iov_base) == 0)
                dest_off = to_copy;
        }
    }

    if (dest_off == 0) return 0;
    ev->write.len = dest_off;

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          ev, sizeof(struct fdmon_bpf_event));
    return 0;
}

/* sys_enter_sendmmsg: fd, msgvec, vlen, flags */
struct sys_enter_sendmmsg_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* 8-byte slots for each syscall arg */
    unsigned long  fd;
    unsigned long  msgvec; /* ptr to mmsghdr array; too complex, skip payload */
    unsigned long  vlen;
    unsigned long  flags;
};

SEC("tracepoint/syscalls/sys_enter_sendmmsg")
int trace_enter_sendmmsg(struct sys_enter_sendmmsg_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);

    __u32 fd = (__u32)ctx->fd;
    if (!fd_is_watched(pid, fd)) return 0;

    /* msgvec is complex; emit a zero-payload write notification. */
    struct fdmon_bpf_event_hdr ev = {};
    ev.type      = FDMON_BPF_WRITE;
    ev.pid       = pid;
    ev.tid       = (__u32)pid_tgid;
    ev.fd        = (__s32)fd;
    ev.timestamp = bpf_ktime_get_ns();
    ev.user_ptr  = 0;
    ev.user_count = 0;
    /* path[0] == 0 signals zero-length write to userspace */

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &ev, sizeof(ev));
    return 0;
}

/* sys_enter_splice: off_in, fd_in, off_out, fd_out, len, flags */
struct sys_enter_splice_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* 8-byte slots for each syscall arg */
    unsigned long  fd_in;
    unsigned long  off_in;
    unsigned long  fd_out;
    unsigned long  off_out;
    unsigned long  len;
    unsigned long  flags;
};

SEC("tracepoint/syscalls/sys_enter_splice")
int trace_enter_splice(struct sys_enter_splice_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);

    __u32 fd = (__u32)ctx->fd_out;
    if (!fd_is_watched(pid, fd)) return 0;

    struct fdmon_bpf_event_hdr ev = {};
    ev.type      = FDMON_BPF_WRITE;
    ev.pid       = pid;
    ev.tid       = (__u32)pid_tgid;
    ev.fd        = (__s32)fd;
    ev.timestamp = bpf_ktime_get_ns();
    /* user_ptr=0 / user_count=0 signals no inline data to userspace */

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &ev, sizeof(ev));
    return 0;
}

/* sys_enter_sendfile: out_fd, in_fd, offset, count */
struct sys_enter_sendfile_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    int            __syscall_nr;
    /* 8-byte slots for each syscall arg */
    unsigned long  out_fd;
    unsigned long  in_fd;
    unsigned long  offset;
    unsigned long  count;
};

SEC("tracepoint/syscalls/sys_enter_sendfile")
int trace_enter_sendfile(struct sys_enter_sendfile_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);

    __u32 fd = (__u32)ctx->out_fd;
    if (!fd_is_watched(pid, fd)) return 0;

    struct fdmon_bpf_event_hdr ev = {};
    ev.type      = FDMON_BPF_WRITE;
    ev.pid       = pid;
    ev.tid       = (__u32)pid_tgid;
    ev.fd        = (__s32)fd;
    ev.timestamp = bpf_ktime_get_ns();
    ev.user_ptr  = 0;
    ev.user_count = 0;

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &ev, sizeof(ev));
    return 0;
}

/* ── socket send / receive byte accounting ───────────────────── */

/*
 * tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size) and
 * tcp_recvmsg(struct sock *sk, ...) return the number of bytes that actually
 * moved, or a negative error.  What an application asks to send is not what
 * a non-blocking socket accepts, so both directions count the return value:
 * the entry kprobe stashes the sock pointer and the kretprobe pairs it with
 * the result.  udp_sendmsg / udp_recvmsg and their IPv6 counterparts have the
 * same shape (PARM1 = sk, return value = bytes).
 */
#define DEFINE_SOCK_ENTRY_PROBE(name, section, stash)                   \
SEC(section)                                                            \
int name(struct pt_regs *ctx)                                           \
{                                                                       \
    __u32 tid = (__u32)bpf_get_current_pid_tgid();                     \
    __u64 sk  = (__u64)PT_REGS_PARM1(ctx);                             \
    bpf_map_update_elem(&stash, &tid, &sk, BPF_ANY);                   \
    return 0;                                                           \
}

#define DEFINE_SOCK_RETURN_PROBE(name, section, stash, event)           \
SEC(section)                                                            \
int name(struct pt_regs *ctx)                                           \
{                                                                       \
    __u64 pid_tgid = bpf_get_current_pid_tgid();                       \
    __u32 tid = (__u32)pid_tgid;                                        \
    __u64 *sk_ptr = bpf_map_lookup_elem(&stash, &tid);                 \
    if (!sk_ptr) return 0;                                              \
    void *sk = (void *)*sk_ptr;                                         \
    /* Release the slot whatever the call returned. */                  \
    bpf_map_delete_elem(&stash, &tid);                                  \
    int ret = (int)PT_REGS_RC(ctx);                                     \
    if (ret <= 0) return 0;   /* an error, or nothing moved */          \
    struct fdmon_bpf_event_hdr ev = {};                                 \
    ev.type      = event;                                               \
    ev.pid       = (__u32)(pid_tgid >> 32);                             \
    ev.tid       = tid;                                                 \
    ev.fd        = -1;                                                  \
    ev.timestamp = bpf_ktime_get_ns();                                  \
    ev.net.bytes = (__u32)ret;                                          \
    ev.net.inode = read_sock_inode(sk);                                 \
    read_sock_tuple(sk, &ev.net.laddr, &ev.net.raddr,                  \
                    &ev.net.lport, &ev.net.rport);                      \
    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,             \
                          &ev, sizeof(ev));                             \
    return 0;                                                           \
}

/* TCP, both address families */
DEFINE_SOCK_ENTRY_PROBE(trace_tcp_sendmsg, "kprobe/tcp_sendmsg", sendmsg_args)
DEFINE_SOCK_RETURN_PROBE(trace_tcp_sendmsg_ret, "kretprobe/tcp_sendmsg",
                         sendmsg_args, FDMON_BPF_NET_SEND)
DEFINE_SOCK_ENTRY_PROBE(trace_tcp_recvmsg, "kprobe/tcp_recvmsg", recvmsg_args)
DEFINE_SOCK_RETURN_PROBE(trace_tcp_recvmsg_ret, "kretprobe/tcp_recvmsg",
                         recvmsg_args, FDMON_BPF_NET_RECV)

/* IPv4 UDP */
DEFINE_SOCK_ENTRY_PROBE(trace_udp_sendmsg, "kprobe/udp_sendmsg", sendmsg_args)
DEFINE_SOCK_RETURN_PROBE(trace_udp_sendmsg_ret, "kretprobe/udp_sendmsg",
                         sendmsg_args, FDMON_BPF_NET_SEND)
DEFINE_SOCK_ENTRY_PROBE(trace_udp_recvmsg, "kprobe/udp_recvmsg", recvmsg_args)
DEFINE_SOCK_RETURN_PROBE(trace_udp_recvmsg_ret, "kretprobe/udp_recvmsg",
                         recvmsg_args, FDMON_BPF_NET_RECV)

/* IPv6 UDP — separate kernel functions */
DEFINE_SOCK_ENTRY_PROBE(trace_udpv6_sendmsg, "kprobe/udpv6_sendmsg", sendmsg_args)
DEFINE_SOCK_RETURN_PROBE(trace_udpv6_sendmsg_ret, "kretprobe/udpv6_sendmsg",
                         sendmsg_args, FDMON_BPF_NET_SEND)
DEFINE_SOCK_ENTRY_PROBE(trace_udpv6_recvmsg, "kprobe/udpv6_recvmsg", recvmsg_args)
DEFINE_SOCK_RETURN_PROBE(trace_udpv6_recvmsg_ret, "kretprobe/udpv6_recvmsg",
                         recvmsg_args, FDMON_BPF_NET_RECV)

/* ── sched_process_exit tracepoint ──────────────────────────── */

/*
 * Runs in the exiting thread.  Nothing is read from the record: its layout
 * differs between kernel versions, and everything needed is the current task.
 */
struct sched_process_exit_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
};

/*
 * Has the last thread of the current process gone?  do_exit() decrements
 * signal->live before this tracepoint fires, so zero means the whole thread
 * group is dead. Without these offsets, userspace reaps dead registrations.
 */
static __always_inline int thread_group_dead(__u32 tid, __u32 tgid)
{
    __u32 zero = 0;
    struct fdmon_config *cfg = bpf_map_lookup_elem(&fdmon_config, &zero);

    if (cfg && cfg->group_live_known) {
        void *task   = (void *)bpf_get_current_task();
        void *signal = NULL;
        int   live   = 1;

        if (bpf_probe_read_kernel(&signal, sizeof(signal),
                                  task + cfg->task_signal_off) == 0 && signal &&
            bpf_probe_read_kernel(&live, sizeof(live),
                                  signal + cfg->signal_live_off) == 0)
            return live <= 0;
    }
    (void)tid; (void)tgid;
    return 0; /* userspace sweeps dead PIDs when BTF is unavailable */
}

SEC("tracepoint/sched/sched_process_exit")
int trace_sched_process_exit(struct sched_process_exit_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 tgid = (__u32)(pid_tgid >> 32);
    __u32 tid  = (__u32)pid_tgid;

    /* A call this thread never returned from must not keep its slot. */
    bpf_map_delete_elem(&recvmsg_args, &tid);
    bpf_map_delete_elem(&sendmsg_args, &tid);
    bpf_map_delete_elem(&open_args, &tid);

    /* A thread created while its process was watched got an entry too. */
    if (tid != tgid)
        bpf_map_delete_elem(&monitored_pids, &tid);

    /* The process's entry goes with its last thread, not with its leader:
     * a leader can exit first, and an exec from another thread retires the
     * leader while the process lives on under the same pid. */
    if (bpf_map_lookup_elem(&monitored_pids, &tgid) &&
        thread_group_dead(tid, tgid))
        bpf_map_delete_elem(&monitored_pids, &tgid);
    return 0;
}

/* ── sched_process_exec tracepoint ───────────────────────────── */

/*
 * Fired after a successful execve() replaces the process image.
 * The tracepoint format (from /sys/kernel/tracing/events/sched/
 * sched_process_exec/format) contains:
 *
 *   common header (8 bytes)
 *   __data_loc char[] filename;  (offset 8,  size 4 – encoded location)
 *   pid_t       pid;             (offset 12, size 4)
 *   pid_t       old_pid;         (offset 16, size 4)
 *
 * We use __data_loc to locate the filename string within the record.
 */
struct sched_process_exec_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    /* __data_loc encoding: low 16 bits = offset from the start of the
     * record, high 16 bits = length. */
    __u32          __data_loc_filename;
    __u32          pid;
    __u32          old_pid;
};

SEC("tracepoint/sched/sched_process_exec")
int trace_sched_process_exec(struct sched_process_exec_args *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    struct fdmon_bpf_event_hdr ev = {};
    ev.type      = FDMON_BPF_EXEC;
    ev.pid       = (__u32)(pid_tgid >> 32);  /* tgid */
    ev.tid       = (__u32)pid_tgid;
    ev.fd        = -1;
    ev.timestamp = bpf_ktime_get_ns();
    ev.user_ptr  = 0;
    ev.user_count = 0;

    /* A thread other than the leader that execs takes over the leader's
     * pid; the entry made for it when it was created names no task now. */
    __u32 old_pid = ctx->old_pid;
    if (old_pid != ev.pid)
        bpf_map_delete_elem(&monitored_pids, &old_pid);

    /* The filename string lives at (ctx + offset). */
    __u16 fname_offset = (__u16)(ctx->__data_loc_filename & 0xffff);
    void *fname_ptr = (void *)ctx + fname_offset;
    bpf_probe_read_kernel_str(ev.path, sizeof(ev.path), fname_ptr);

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          &ev, sizeof(ev));
    return 0;
}

/* ── task_newtask: inherit monitored fds ─────────────────────── */

/*
 * Fired inside fork()/clone() in the parent, before the new task can run.
 * The child inherits the parent's file descriptors — if the parent's writes
 * to fd 1 or fd 2 are captured, register the same fds for the child RIGHT
 * NOW, in kernel space, with zero latency.
 *
 * This closes the race for workers that fork() and write() without ever
 * calling execve() — and also covers the execve() case because the child is
 * registered at fork time, before it even runs.
 *
 * Only `pid` is read from the record.  It has been the event's first field
 * for as long as the tracepoint has existed, whereas sched_process_fork
 * changed layout when its comm arrays became dynamic strings — and a
 * program that reads past the end of a record cannot be attached at all.
 * The parent is the current task, which also gets forks made by threads
 * other than the main one right.
 */
struct task_newtask_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    __s32          pid;   /* the new task; for a new thread, its tid */
};

SEC("tracepoint/task/task_newtask")
int trace_task_newtask(struct task_newtask_args *ctx)
{
    __u32 parent = (__u32)(bpf_get_current_pid_tgid() >> 32);
    __u32 child  = (__u32)ctx->pid;

    struct fdmon_watch *p = bpf_map_lookup_elem(&monitored_pids, &parent);
    if (!p) return 0;

    struct fdmon_watch w = {};
    w.inh = p->root | p->inh;
    if (!w.inh) return 0;
    /* Heirs answer to the nearest "this process and what it forks"
     * registration; failing that, to whatever their parent answers to. */
    w.origin = p->root ? parent : p->origin;

    bpf_map_update_elem(&monitored_pids, &child, &w, BPF_ANY);
    return 0;
}

char _license[] SEC("license") = "GPL";
