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
 * Map of monitored PIDs.
 * Key = u32 pid (tgid).  Value = u8 fd_mask: bit 0 = fd 1, bit 1 = fd 2.
 * Populated by userspace for the selected root process; propagated to
 * children automatically by trace_sched_process_fork in kernel space.
 * Entries are removed by trace_sched_process_exit so dead PIDs never
 * accumulate.  Size matches the kernel default pid_max of 32768.
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 32768);
    __type(key, __u32);
    __type(value, __u8);
} monitored_pids SEC(".maps");

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
 * Per-CPU scratch map to stash the struct sock* pointer between
 * tcp_recvmsg entry and kretprobe exit.
 *
 * Key = tid (u32), value = sock pointer (u64).
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u64);
} recvmsg_args SEC(".maps");

/*
 * Helper: read the 4-tuple from struct sock using BPF field offsets.
 * struct sock → __sk_common (type struct sock_common) at offset 0:
 *   skc_daddr      at offset 0   (u32) – remote IPv4
 *   skc_rcv_saddr  at offset 4   (u32) – local IPv4
 *   skc_dport      at offset 12  (u16) – remote port (network order)
 *   skc_num        at offset 14  (u16) – local port (host order)
 *
 * We use bpf_probe_read_kernel to be safe across kernel versions.
 */
static __always_inline void read_sock_tuple(void *sk,
                                            __u32 *laddr, __u32 *raddr,
                                            __u16 *lport, __u16 *rport)
{
    bpf_probe_read_kernel(raddr, sizeof(*raddr), sk);          /* skc_daddr      @0  */
    bpf_probe_read_kernel(laddr, sizeof(*laddr), sk + 4);      /* skc_rcv_saddr  @4  */
    bpf_probe_read_kernel(rport, sizeof(*rport), sk + 12);     /* skc_dport      @12 */
    bpf_probe_read_kernel(lport, sizeof(*lport), sk + 14);     /* skc_num        @14 */
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

    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!mask) return 0;
    /* Check fd against mask: bit0=fd1, bit1=fd2 */
    __u32 fd = (__u32)ctx->fd;
    if (fd == 1 && !(*mask & 1)) return 0;
    if (fd == 2 && !(*mask & 2)) return 0;
    if (fd != 1 && fd != 2) return 0;

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

    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!mask) return 0;
    __u32 fd = (__u32)ctx->fd;
    if (fd == 1 && !(*mask & 1)) return 0;
    if (fd == 2 && !(*mask & 2)) return 0;
    if (fd != 1 && fd != 2) return 0;

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

    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!mask) return 0;
    __u32 fd = (__u32)ctx->fd;
    if (fd == 1 && !(*mask & 1)) return 0;
    if (fd == 2 && !(*mask & 2)) return 0;
    if (fd != 1 && fd != 2) return 0;

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

    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!mask) return 0;
    __u32 fd = (__u32)ctx->fd;
    if (fd == 1 && !(*mask & 1)) return 0;
    if (fd == 2 && !(*mask & 2)) return 0;
    if (fd != 1 && fd != 2) return 0;

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

    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!mask) return 0;
    __u32 fd = (__u32)ctx->fd;
    if (fd == 1 && !(*mask & 1)) return 0;
    if (fd == 2 && !(*mask & 2)) return 0;
    if (fd != 1 && fd != 2) return 0;

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

    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!mask) return 0;
    __u32 fd = (__u32)ctx->fd;
    if (fd == 1 && !(*mask & 1)) return 0;
    if (fd == 2 && !(*mask & 2)) return 0;
    if (fd != 1 && fd != 2) return 0;

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

    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!mask) return 0;
    __u32 fd = (__u32)ctx->fd_out;
    if (fd == 1 && !(*mask & 1)) return 0;
    if (fd == 2 && !(*mask & 2)) return 0;
    if (fd != 1 && fd != 2) return 0;

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

    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &pid);
    if (!mask) return 0;
    __u32 fd = (__u32)ctx->out_fd;
    if (fd == 1 && !(*mask & 1)) return 0;
    if (fd == 2 && !(*mask & 2)) return 0;
    if (fd != 1 && fd != 2) return 0;

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

/* ── tcp_sendmsg / tcp_recvmsg tracepoints ───────────────────── */

/*
 * kprobe context: tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
 * We use a kprobe on tcp_sendmsg to capture the 'size' argument
 * (the number of bytes the application is sending).
 *
 * Similarly for tcp_recvmsg we use a kretprobe to get the return
 * value (number of bytes received, or negative error).
 *
 * Using fentry/fexit (BPF trampoline) for better performance when
 * available, falling back to kprobe-style attachment.  libbpf will
 * auto-select the best attachment type for these program types.
 */

SEC("kprobe/tcp_sendmsg")
int trace_tcp_sendmsg(struct pt_regs *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    void *sk   = (void *)PT_REGS_PARM1(ctx);
    __u32 size = (__u32)PT_REGS_PARM3(ctx);

    struct fdmon_bpf_event_hdr ev = {};
    ev.type       = FDMON_BPF_NET_SEND;
    ev.pid        = (__u32)(pid_tgid >> 32);
    ev.tid        = (__u32)pid_tgid;
    ev.fd         = -1;
    ev.timestamp  = bpf_ktime_get_ns();
    ev.net.bytes  = size;
    read_sock_tuple(sk, &ev.net.laddr, &ev.net.raddr,
                    &ev.net.lport, &ev.net.rport);

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          &ev, sizeof(ev));
    return 0;
}

SEC("kprobe/tcp_recvmsg")
int trace_tcp_recvmsg(struct pt_regs *ctx)
{
    __u32 tid = (__u32)bpf_get_current_pid_tgid();
    __u64 sk  = (__u64)PT_REGS_PARM1(ctx);
    bpf_map_update_elem(&recvmsg_args, &tid, &sk, BPF_ANY);
    return 0;
}

SEC("kretprobe/tcp_recvmsg")
int trace_tcp_recvmsg_ret(struct pt_regs *ctx)
{
    int ret = (int)PT_REGS_RC(ctx);
    if (ret <= 0)
        return 0;  /* error or no data – skip */

    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 tid = (__u32)pid_tgid;

    /* Retrieve the stashed sock pointer */
    __u64 *sk_ptr = bpf_map_lookup_elem(&recvmsg_args, &tid);
    if (!sk_ptr) return 0;
    void *sk = (void *)*sk_ptr;
    bpf_map_delete_elem(&recvmsg_args, &tid);

    struct fdmon_bpf_event_hdr ev = {};
    ev.type       = FDMON_BPF_NET_RECV;
    ev.pid        = (__u32)(pid_tgid >> 32);
    ev.tid        = (__u32)pid_tgid;
    ev.fd         = -1;
    ev.timestamp  = bpf_ktime_get_ns();
    ev.net.bytes  = (__u32)ret;
    read_sock_tuple(sk, &ev.net.laddr, &ev.net.raddr,
                    &ev.net.lport, &ev.net.rport);

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          &ev, sizeof(ev));
    return 0;
}

/* ── sched_process_exit tracepoint ──────────────────────────── */

/*
 * Format (from /sys/kernel/debug/tracing/events/sched/sched_process_exit):
 *   common header  (8 bytes)
 *   char comm[16]; (offset 8)
 *   pid_t pid;     (offset 24)
 *   int prio;      (offset 28)
 *
 * pid here is the thread pid.  For the group leader (main thread) pid == tgid,
 * which is what we store in monitored_pids, so we can safely delete it.
 */
struct sched_process_exit_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    char           comm[16];
    __s32          pid;
    int            prio;
};

SEC("tracepoint/sched/sched_process_exit")
int trace_sched_process_exit(struct sched_process_exit_args *ctx)
{
    /* Only remove on the group-leader exit (pid == tgid).  Thread exits
     * fire with pid == tid != tgid and should not remove the entry. */
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 tgid = (__u32)(pid_tgid >> 32);
    __u32 pid  = (__u32)(pid_tgid & 0xffffffff);
    if (pid != tgid) return 0;

    bpf_map_delete_elem(&monitored_pids, &tgid);
    return 0;
}

/* ── sched_process_exec tracepoint ───────────────────────────── */

/*
 * Fired after a successful execve() replaces the process image.
 * The tracepoint format (from /sys/kernel/debug/tracing/events/sched/
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
    /* __data_loc encoding: high 16 bits = offset from struct start,
     * low 16 bits = length. */
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

    /* Decode __data_loc: offset is upper 16 bits of the field. */
    __u16 fname_offset = (__u16)((ctx->__data_loc_filename >> 16) & 0xffff);
    /* The filename string lives at (ctx + fname_offset). */
    void *fname_ptr = (void *)ctx + fname_offset;
    bpf_probe_read_kernel_str(ev.path, sizeof(ev.path), fname_ptr);

    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
                          &ev, sizeof(ev));
    return 0;
}

/* ── sched_process_fork: inherit monitored fds ───────────────── */

/*
 * Fired immediately when fork()/clone() creates a new process.
 * The child inherits the parent's file descriptors — if the parent is
 * being monitored for fd 1 or fd 2, register the same fds for the
 * child RIGHT NOW, in kernel space, with zero latency.
 *
 * This closes the race for workers that fork() and write() without
 * ever calling execve() — and also covers the execve() case because
 * the child is registered at fork time, before it even runs.
 */
struct sched_process_fork_args {
    unsigned short common_type;
    unsigned char  common_flags;
    unsigned char  common_preempt_count;
    int            common_pid;
    char           parent_comm[16]; /* offset 8  */
    __u32          parent_pid;      /* offset 24 */
    char           child_comm[16];  /* offset 28 */
    __u32          child_pid;       /* offset 44 */
};

SEC("tracepoint/sched/sched_process_fork")
int trace_sched_process_fork(struct sched_process_fork_args *ctx)
{
    __u32 parent = ctx->parent_pid;
    __u32 child  = ctx->child_pid;

    /* If the parent is monitored, register the child with the same fd mask.
     * The child inherits the parent's fds, so the same mask applies. */
    __u8 *mask = bpf_map_lookup_elem(&monitored_pids, &parent);
    if (!mask) return 0;

    __u8 val = *mask;
    bpf_map_update_elem(&monitored_pids, &child, &val, BPF_ANY);

    return 0;
}

char _license[] SEC("license") = "GPL";
