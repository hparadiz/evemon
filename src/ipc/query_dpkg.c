#include "query_dpkg.h"
#include "query.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum { DPKG_HAS_EXE = 1, DPKG_FOUND = 2, DPKG_SNAP = 4, DPKG_ESSENTIAL = 8, DPKG_PROTECTED = 16 };

/* Field order of a found record; both sides share these buffer sizes. */
#define DPKG_STRINGS(X) \
    X(name) X(version) X(arch) X(source) X(origin) X(section) X(priority) \
    X(maintainer) X(original_maintainer) X(homepage) X(license) X(multi_arch) \
    X(size_str) X(installed_str) X(summary) X(description) X(depends) \
    X(revision) X(base) X(grade) X(confinement)

int query_dpkg_encode(pid_t pid, ipc_buf_t *out, dpkg_cancel_fn cancel, void *user_data)
{
    ipc_put_string(out, dpkg_distro_label());
    if (!pid) {
        ipc_put_u32(out, 0); ipc_put_u32(out, (uint32_t)-1);
        return out->error ? IPC_INTERNAL : 0;
    }
    dpkg_result_t *res = g_new0(dpkg_result_t, 1);
    if (!dpkg_lookup_pid(pid, res, cancel, user_data)) { g_free(res); return IPC_INTERNAL; }
    uint32_t flags = (res->has_exe ? DPKG_HAS_EXE : 0) | (res->found ? DPKG_FOUND : 0) |
                     (res->snap ? DPKG_SNAP : 0) | (res->essential ? DPKG_ESSENTIAL : 0) |
                     (res->protected_pkg ? DPKG_PROTECTED : 0);
    ipc_put_u32(out, flags);
    ipc_put_u32(out, (uint32_t)res->automatic);
    if (res->found) {
#define PUT(field) ipc_put_string(out, res->field);
        DPKG_STRINGS(PUT)
#undef PUT
    }
    g_free(res);
    return out->error ? IPC_INTERNAL : 0;
}

int query_dpkg_decode(ipc_buf_t *in, char *label, size_t label_size, dpkg_result_t *res)
{
    char scratch[64];
    if (!label) { label = scratch; label_size = sizeof(scratch); }
    ipc_get_fixed(in, label, label_size);
    uint32_t flags = ipc_get_u32(in);
    int32_t automatic = (int32_t)ipc_get_u32(in);
    if (in->error || (flags & ~31u) || automatic < -1 || automatic > 1 ||
        ((flags & DPKG_FOUND) && !(flags & DPKG_HAS_EXE)))
        goto invalid;
    if (res) {
        memset(res, 0, sizeof(*res));
        res->has_exe = !!(flags & DPKG_HAS_EXE);
        res->found = !!(flags & DPKG_FOUND);
        res->snap = !!(flags & DPKG_SNAP);
        res->essential = !!(flags & DPKG_ESSENTIAL);
        res->protected_pkg = !!(flags & DPKG_PROTECTED);
        res->automatic = automatic;
    }
    if (flags & DPKG_FOUND) {
        dpkg_result_t *dst = res ? res : g_new0(dpkg_result_t, 1);
#define GET(field) ipc_get_fixed(in, dst->field, sizeof(dst->field));
        DPKG_STRINGS(GET)
#undef GET
        if (!res) g_free(dst);
    }
    if (!ipc_done(in)) goto invalid;
    return 0;
invalid:
    errno = EPROTO; return -1;
}

int query_dpkg_fetch(const char *path, pid_t pid, char *label, size_t label_size,
                     dpkg_result_t *res, int wake_fd)
{
    ipc_buf_t request = {0}, response = {0};
    ipc_put_u32(&request, (uint32_t)pid);
    /* The label is answered inline; a lookup waits for the package lane. */
    int rc = query_exchange_wait(path, IPC_DPKG, &request, &response, wake_fd,
                                 pid ? IPC_PACKAGE_TIMEOUT_MS : IPC_TIMEOUT_MS);
    if (!rc) rc = query_dpkg_decode(&response, label, label_size, res);
    int saved = errno;
    ipc_free(&request); ipc_free(&response);
    errno = saved; return rc;
}

static char remote_label[64];
void query_dpkg_set_remote_label(const char *label)
{
    snprintf(remote_label, sizeof(remote_label), "%s", label ? label : "");
}
const char *query_dpkg_remote_label(void) { return remote_label[0] ? remote_label : NULL; }
