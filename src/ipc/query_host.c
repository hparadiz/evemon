#include "query.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static char evemon_host[256];
static int foreign_host;

static void host_identity(char *boot, size_t boot_size, char *ns, size_t ns_size)
{
    boot[0] = ns[0] = 0;
    FILE *file = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (file) {
        if (fgets(boot, boot_size, file)) boot[strcspn(boot, "\r\n")] = 0;
        fclose(file);
    }
    ssize_t n = readlink("/proc/self/ns/pid", ns, ns_size - 1);
    if (n > 0) ns[n] = 0;
}
void query_host_encode(ipc_buf_t *out)
{
    char host[256] = {0}, boot[64], ns[64];
    gethostname(host, sizeof(host) - 1);
    host_identity(boot, sizeof(boot), ns, sizeof(ns));
    ipc_put_string(out, host); ipc_put_string(out, boot); ipc_put_string(out, ns);
    ipc_put_u32(out, getuid());
}
int query_host_read(const char *path, query_host_t *host)
{
    ipc_buf_t request = {0}, response = {0};
    if (query_exchange(path, IPC_HOST, &request, &response)) { ipc_free(&response); return -1; }
    char boot[64], ns[64], local_boot[64], local_ns[64];
    ipc_get_fixed(&response, host->name, sizeof(host->name));
    ipc_get_fixed(&response, boot, sizeof(boot)); ipc_get_fixed(&response, ns, sizeof(ns));
    uint32_t uid = ipc_get_u32(&response);
    int valid = ipc_done(&response) && host->name[0];
    ipc_free(&response);
    if (!valid) { errno = EPROTO; return -1; }
    host_identity(local_boot, sizeof(local_boot), local_ns, sizeof(local_ns));
    host->foreign = !boot[0] || !ns[0] || strcmp(boot, local_boot) || strcmp(ns, local_ns) || uid != getuid();
    return 0;
}
void query_host_activate(const query_host_t *host)
{
    snprintf(evemon_host, sizeof(evemon_host), "%s", host->name);
    foreign_host = host->foreign;
    if (foreign_host)
        fprintf(stderr, "evemon: evemon host %s (remote)\n", evemon_host);
}
void query_host_activate_local(void)
{
    query_host_t host = {0};
    if (gethostname(host.name, sizeof(host.name) - 1) != 0)
        snprintf(host.name, sizeof(host.name), "localhost");
    query_host_activate(&host);
}
int query_host_init(const char *path)
{
    query_host_t host = {0};
    if (query_host_read(path, &host)) return -1;
    query_host_activate(&host);
    return 0;
}
int query_is_foreign_host(void) { return foreign_host; }
const char *query_host_name(void) { return evemon_host; }

int query_signal(const proc_entry_t *target, int signal_number)
{
    return query_signal_path(query_remote_path(), target, signal_number);
}
static uint64_t process_start_ticks(pid_t pid);
static int deliver_signal(pid_t pid, int signal_number, uint64_t start_ticks)
{
    /* Hold the process identity across /proc validation and signal delivery.
     * Never fall back to kill(pid), including on older kernels. */
    int fd = syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) return errno;
    int error = 0;
    if (process_start_ticks(pid) != start_ticks) error = ESTALE;
    else if (syscall(SYS_pidfd_send_signal, fd, signal_number, NULL, 0))
        error = errno;
    close(fd);
    return error;
}
int query_signal_path(const char *path, const proc_entry_t *target, int signal_number)
{
    if (!target || target->pid <= 1 || signal_number <= 0 ||
        signal_number >= NSIG || !target->start_ticks) {
        errno = EINVAL;
        return -1;
    }
    if (!path || !*path) {
        int error = deliver_signal(target->pid, signal_number,
                                   target->start_ticks);
        if (error) { errno = error; return -1; }
        return 0;
    }
    ipc_buf_t request = {0}, response = {0};
    ipc_put_u32(&request, target->pid); ipc_put_u32(&request, signal_number);
    ipc_put_u64(&request, target->start_ticks); ipc_put_u64(&request, target->evemon_instance);
    int rc = query_exchange(path, IPC_SIGNAL, &request, &response);
    if (!rc) {
        uint32_t error = ipc_get_u32(&response);
        if (!ipc_done(&response) || error > 4095) error = EPROTO;
        if (error) { errno = error; rc = -1; }
    }
    int saved = errno;
    ipc_free(&request); ipc_free(&response); errno = saved; return rc;
}
static uint64_t process_start_ticks(pid_t pid)
{
    char path[64], line[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char *read = fgets(line, sizeof(line), file); fclose(file);
    char *p = read ? strrchr(line, ')') : NULL;
    if (!p) return 0;
    char *save = NULL, *field = strtok_r(p + 1, " ", &save);
    for (int i = 3; i < 22 && field; ++i) field = strtok_r(NULL, " ", &save);
    return field ? strtoull(field, NULL, 10) : 0;
}
void query_signal_dispatch(ipc_buf_t *in, ipc_buf_t *out, uint64_t instance)
{
    uint32_t pid = ipc_get_u32(in), signo = ipc_get_u32(in);
    uint64_t ticks = ipc_get_u64(in), expected_instance = ipc_get_u64(in);
    int error = 0;
    if (!ipc_done(in) || pid <= 1 || pid > INT_MAX || signo >= NSIG || !ticks)
        error = EINVAL;
    else if (expected_instance != instance) error = ESTALE;
    else error = deliver_signal((pid_t)pid, (int)signo, ticks);
    ipc_put_u32(out, error);
}
