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
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* ── event structure shared with userspace ───────────────────── */

#define FDMON_BPF_OPEN  1
#define FDMON_BPF_CLOSE 2
#define FDMON_BPF_PATH_MAX 256

struct fdmon_bpf_event {
    __u32 type;       /* FDMON_BPF_OPEN or FDMON_BPF_CLOSE */
    __u32 pid;        /* tgid (userspace "PID") */
    __u32 tid;        /* kernel tid */
    __s32 fd;         /* file descriptor number */
    __u64 timestamp;  /* ktime_get_ns() */
    char  path[FDMON_BPF_PATH_MAX];
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

    struct fdmon_bpf_event ev = {};
    ev.type      = FDMON_BPF_OPEN;
    ev.pid       = (__u32)(pid_tgid >> 32);  /* tgid = userspace PID */
    ev.tid       = (__u32)pid_tgid;
    ev.fd        = (__s32)fd;
    ev.timestamp = bpf_ktime_get_ns();

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

    struct fdmon_bpf_event ev = {};
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

char _license[] SEC("license") = "GPL";
