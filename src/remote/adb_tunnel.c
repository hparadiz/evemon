#include "adb_tunnel.h"
#include "../platform/desktop_session.h"
#include "../ipc/ipc_wire.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef EVEMON_ANDROID_SHA256
#define EVEMON_ANDROID_SHA256 ""
#endif

#define ANDROID_ARCHIVE "evemon-arm64.tar.gz"
#define ANDROID_EVEMON_BASE "/data/local/tmp/evemon"
#define ANDROID_EVEMON_PREVIOUS "/data/local/tmp/evemon.previous"
#define ANDROID_ARCHIVE_MAX_SIZE (64u * 1024u * 1024u)

struct adb_tunnel {
    char *serial, *directory, *path, *local_spec;
};

static void adb_child_setup(gpointer parent)
{
    desktop_child_use_user(NULL);
    if (setsid() < 0) _exit(126);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != GPOINTER_TO_INT(parent)) _exit(1);
}

static GPtrArray *adb_arguments(const char *serial)
{
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(args, g_strdup("adb"));
    if (serial && *serial) {
        g_ptr_array_add(args, g_strdup("-s"));
        g_ptr_array_add(args, g_strdup(serial));
    }
    return args;
}

static gboolean adb_execute(const char *serial, const char *const *tail,
                            GCancellable *cancel, char **output,
                            GError **error)
{
    GPtrArray *args = adb_arguments(serial);
    for (size_t i = 0; tail[i]; ++i)
        g_ptr_array_add(args, g_strdup(tail[i]));
    g_ptr_array_add(args, NULL);
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_child_setup(launcher, adb_child_setup,
                                          GINT_TO_POINTER(getpid()), NULL);
    GSubprocess *process = g_subprocess_launcher_spawnv(
        launcher, (const char *const *)args->pdata, error);
    g_object_unref(launcher);
    g_ptr_array_free(args, TRUE);
    if (!process) return FALSE;

    char *out = NULL, *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8(process, NULL, cancel,
                                                &out, &err, error);
    if (!ok) {
        const char *identifier = g_subprocess_get_identifier(process);
        if (identifier)
            kill(-(pid_t)g_ascii_strtoll(identifier, NULL, 10), SIGKILL);
        g_subprocess_force_exit(process);
        g_subprocess_wait(process, NULL, NULL);
    } else if (!g_subprocess_get_successful(process)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ADB%s%s failed: %s", serial && *serial ? " device " : "",
                    serial && *serial ? serial : "",
                    err && *err ? g_strstrip(err) : "command failed");
        ok = FALSE;
    }
    if (ok && output) {
        g_strstrip(out);
        *output = g_steal_pointer(&out);
    }
    g_free(out);
    g_free(err);
    g_object_unref(process);
    return ok;
}

static gboolean serial_is_safe(const char *serial)
{
    return serial && *serial && serial[0] != '-' && strlen(serial) <= 128 &&
        strspn(serial,
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-") ==
        strlen(serial);
}

static char *resolve_serial(const char *requested, GCancellable *cancel,
                            GError **error)
{
    if (requested && *requested && strcmp(requested, "auto")) {
        if (!serial_is_safe(requested)) {
            g_set_error_literal(error, G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT, "Enter a valid ADB device serial.");
            return NULL;
        }
        const char *command[] = {"get-state", NULL};
        char *state = NULL;
        if (!adb_execute(requested, command, cancel, &state, error)) return NULL;
        gboolean ready = !strcmp(state, "device");
        if (!ready)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "ADB device %s is %s.", requested,
                        *state ? state : "unavailable");
        g_free(state);
        return ready ? g_strdup(requested) : NULL;
    }
    const char *command[] = {"get-serialno", NULL};
    char *serial = NULL;
    if (!adb_execute(NULL, command, cancel, &serial, error)) return NULL;
    if (!serial_is_safe(serial) || !strcmp(serial, "unknown")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
            "ADB did not identify exactly one ready device; pass its serial.");
        g_clear_pointer(&serial, g_free);
    }
    return serial;
}

static gboolean remote_socket_is_safe(const char *path)
{
    if (!path || !*path) return TRUE;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
        strpbrk(path, ":\r\n")) return FALSE;
    if (path[0] == '/') return TRUE;
    return path[0] == '@' && path[1] &&
        strspn(path + 1,
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
        strlen(path + 1);
}

static gboolean bundle_checksum(const char *path, char digest[65],
                                GError **error)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Cannot open the Android Evemon archive: %s",
                    g_strerror(errno));
        return FALSE;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                    "Cannot inspect the Android Evemon archive: %s",
                    g_strerror(saved));
        return FALSE;
    }
    if (!S_ISREG(st.st_mode) || !desktop_session_trusts_owner(st.st_uid) ||
        (st.st_mode & (S_IWGRP | S_IWOTH)) || st.st_size <= 0 ||
        (uint64_t)st.st_size > ANDROID_ARCHIVE_MAX_SIZE) {
        close(fd);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "The Android Evemon archive is not a trusted regular file.");
        return FALSE;
    }
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    unsigned char buffer[65536];
    for (;;) {
        ssize_t size = read(fd, buffer, sizeof(buffer));
        if (size > 0) {
            g_checksum_update(sum, buffer, (gsize)size);
            continue;
        }
        if (!size) break;
        if (errno == EINTR) continue;
        int saved = errno;
        g_checksum_free(sum);
        close(fd);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                    "Cannot read the Android Evemon archive: %s",
                    g_strerror(saved));
        return FALSE;
    }
    g_strlcpy(digest, g_checksum_get_string(sum), 65);
    g_checksum_free(sum);
    close(fd);
    return TRUE;
}

static char *android_bundle_path(GError **error)
{
    if (!EVEMON_ANDROID_SHA256[0]) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "This Evemon build does not include Android ARM64 Evemon.");
        return NULL;
    }
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable,
                              sizeof(executable) - 1);
    char *relative = NULL;
    if (length > 0 && (size_t)length < sizeof(executable) - 1) {
        executable[length] = 0;
        char *directory = g_path_get_dirname(executable);
        relative = g_build_filename(directory, "share", "evemon", "android",
                                    ANDROID_ARCHIVE, NULL);
        g_free(directory);
    }
    char *installed = g_build_filename(EVEMON_DATADIR, "android",
                                       ANDROID_ARCHIVE, NULL);
    char *path = relative && access(relative, R_OK) == 0
               ? g_steal_pointer(&relative) : NULL;
    if (!path && access(installed, R_OK) == 0)
        path = g_steal_pointer(&installed);
    g_free(relative);
    g_free(installed);
    if (!path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "The bundled Android ARM64 Evemon is missing.");
        return NULL;
    }
    char digest[65];
    if (!bundle_checksum(path, digest, error)) {
        g_free(path);
        return NULL;
    }
    if (g_ascii_strcasecmp(digest, EVEMON_ANDROID_SHA256)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The bundled Android ARM64 Evemon failed verification.");
        g_free(path);
        return NULL;
    }
    return path;
}

static gboolean ensure_android_bundle(const char *serial,
                                      GCancellable *cancel,
                                      char **bundle_id, GError **error)
{
    char *archive = android_bundle_path(error);
    if (!archive) return FALSE;
    char *quoted_id = g_shell_quote(EVEMON_ANDROID_SHA256);
    char *probe = g_strdup_printf(
        "base=" ANDROID_EVEMON_BASE ";"
        "if test -f \"$base/bundle.id\" && "
          "test \"$(cat \"$base/bundle.id\")\" = %s && "
          "cd \"$base\" && sha256sum -c manifest.sha256 >/dev/null 2>&1;then "
          "printf 'current\\n';else printf 'install\\n';fi",
        quoted_id);
    const char *probe_shell[] = {"shell", probe, NULL};
    char *state = NULL;
    gboolean ok = adb_execute(serial, probe_shell, cancel, &state, error);
    g_free(probe);
    if (!ok) goto done;
    if (strcmp(state, "current")) {
        char short_id[17];
        g_strlcpy(short_id, EVEMON_ANDROID_SHA256, sizeof(short_id));
        char *remote_archive = g_strdup_printf(
            "/data/local/tmp/.evemon-upload-%s.tar.gz", short_id);
        const char *push[] = {"push", archive, remote_archive, NULL};
        ok = adb_execute(serial, push, cancel, NULL, error);
        if (ok) {
            char *stage = g_strdup_printf(
                "/data/local/tmp/.evemon-stage-%s", short_id);
            char *quoted_archive = g_shell_quote(remote_archive);
            char *quoted_stage = g_shell_quote(stage);
            char *install = g_strdup_printf(
                "set -eu;umask 077;archive=%s;stage=%s;"
                "base=" ANDROID_EVEMON_BASE ";old=" ANDROID_EVEMON_PREVIOUS ";"
                "cleanup(){ test -z \"${stage:-}\" || rm -rf \"$stage\";"
                  "test -z \"${archive:-}\" || rm -f \"$archive\";};"
                "trap cleanup EXIT HUP INT TERM;"
                "rm -rf \"$stage\";mkdir -m 700 \"$stage\";"
                "tar -xzf \"$archive\" -C \"$stage\";cd \"$stage\";"
                "test -f evemon && ! test -L evemon;"
                "test -f lib/ld-linux-aarch64.so.1 && ! test -L lib/ld-linux-aarch64.so.1;"
                "test -f manifest.sha256 && ! test -L manifest.sha256;"
                "sha256sum -c manifest.sha256 >/dev/null;"
                "chmod -R go-rwx \"$stage\";chmod 700 \"$stage\" \"$stage/evemon\" "
                  "\"$stage/lib\" \"$stage/lib/ld-linux-aarch64.so.1\";"
                "printf '%%s\\n' %s >\"$stage/bundle.id\";chmod 600 \"$stage/bundle.id\";"
                "rm -rf \"$old\";"
                "if test -e \"$base\";then mv \"$base\" \"$old\";fi;"
                "if ! mv \"$stage\" \"$base\";then "
                  "test ! -e \"$old\" || mv \"$old\" \"$base\";exit 1;fi;"
                "rm -f \"$archive\";stage=;archive=;",
                quoted_archive, quoted_stage, quoted_id);
            const char *install_shell[] = {"shell", install, NULL};
            ok = adb_execute(serial, install_shell, cancel, NULL, error);
            g_free(install);
            g_free(quoted_stage);
            g_free(quoted_archive);
            g_free(stage);
        }
        g_free(remote_archive);
    }
    if (ok) *bundle_id = g_strdup(EVEMON_ANDROID_SHA256);
done:
    g_free(state);
    g_free(quoted_id);
    g_free(archive);
    return ok;
}

static char *android_bootstrap_command(const char *socket, int run_daemon,
                                       unsigned idle_seconds,
                                       const char *bundle_id)
{
    char *quoted_socket = g_shell_quote(socket ? socket : "");
    char tag[17] = "";
    if (bundle_id && strlen(bundle_id) >= 16) memcpy(tag, bundle_id, 16);
    char *quoted_tag = g_shell_quote(tag);
    char *command = g_strdup_printf(
        "set -eu;umask 077;"
        "if test -x /data/local/tmp/evemon/evemon && test -x /data/local/tmp/evemon/lib/ld-linux-aarch64.so.1;then "
          "base=/data/local/tmp/evemon;"
        "elif test -x /data/local/tmp/evemon-test/evemon && test -x /data/local/tmp/evemon-test/lib/ld-linux-aarch64.so.1;then "
          "base=/data/local/tmp/evemon-test;"
        "else echo 'Android Evemon is not installed under /data/local/tmp' >&2;exit 44;fi;"
        "loader=$base/lib/ld-linux-aarch64.so.1;"
        "if test %d -ne 0;then "
          "chmod 700 \"$base\" \"$base/evemon\" \"$loader\";"
          "chmod -R go-rwx \"$base/lib\";"
          "test ! -f \"$base/fdmon_ebpf_kern.o\" || chmod 600 \"$base/fdmon_ebpf_kern.o\";"
        "fi;"
        "socket=%s;tag=%s;"
        "if test -z \"$socket\";then "
          "if test -n \"$tag\";then socket=@evemon-$(id -u)-$tag;"
          "else socket=@evemon-$(id -u);fi;"
        "fi;"
        "run(){ HOME=\"$base\" XDG_CONFIG_HOME=\"$base/config\" XDG_RUNTIME_DIR=\"$base\" "
          "\"$loader\" --library-path \"$base/lib\" \"$base/evemon\" \"$@\";};"
        "check(){ run --socket \"$socket\" --query status >/dev/null 2>&1;};"
        "if test %d -eq 0;then check || exit 44;"
        "elif ! check;then "
          "log=$base/evemon.log;: >\"$log\";"
          "if test %u -gt 0;then "
            "nohup setsid env HOME=\"$base\" XDG_CONFIG_HOME=\"$base/config\" XDG_RUNTIME_DIR=\"$base\" "
              "\"$loader\" --library-path \"$base/lib\" \"$base/evemon\" --headless --socket \"$socket\" --idle-exit %u "
              "</dev/null >>\"$log\" 2>&1 &"
          "else "
            "nohup setsid env HOME=\"$base\" XDG_CONFIG_HOME=\"$base/config\" XDG_RUNTIME_DIR=\"$base\" "
              "\"$loader\" --library-path \"$base/lib\" \"$base/evemon\" --headless --socket \"$socket\" "
              "</dev/null >>\"$log\" 2>&1 &"
          "fi;"
          "daemon=$!;printf '%%s\\n' \"$daemon\" >\"$base/evemon.pid\";chmod 600 \"$base/evemon.pid\" \"$log\";i=0;"
          "while test $i -lt 100 && ! check;do "
            "if ! kill -0 $daemon 2>/dev/null;then tail -n 20 \"$log\" >&2;exit 47;fi;"
            "sleep .05;i=$((i+1));"
          "done;"
        "fi;"
        "if ! check;then tail -n 20 \"$base/evemon.log\" >&2;exit 47;fi;"
        "printf '%%s\\n' \"$socket\"",
        run_daemon, quoted_socket, quoted_tag, run_daemon,
        idle_seconds, idle_seconds);
    g_free(quoted_socket);
    g_free(quoted_tag);
    return command;
}

const char *adb_tunnel_path(const adb_tunnel_t *t) { return t->path; }
void adb_tunnel_activate(adb_tunnel_t *t) { (void)t; }

void adb_tunnel_close(adb_tunnel_t *t)
{
    if (!t) return;
    if (t->local_spec) {
        const char *command[] = {"forward", "--remove", t->local_spec, NULL};
        adb_execute(t->serial, command, NULL, NULL, NULL);
    }
    if (t->path) {
        ipc_desktop_socket(t->path, 0);
        unlink(t->path);
    }
    if (t->directory) rmdir(t->directory);
    g_free(t->serial);
    g_free(t->directory);
    g_free(t->path);
    g_free(t->local_spec);
    g_free(t);
}

adb_tunnel_t *adb_tunnel_open(const char *requested_serial,
                              const char *socket, int run_daemon,
                              unsigned idle_seconds, GCancellable *cancel,
                              GError **error)
{
    if (!remote_socket_is_safe(socket)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "The Android socket must be an absolute path or a safe @abstract name.");
        return NULL;
    }
    char *serial = resolve_serial(requested_serial, cancel, error);
    if (!serial) return NULL;
    char *bundle_id = NULL;
    if (run_daemon &&
        !ensure_android_bundle(serial, cancel, &bundle_id, error)) {
        g_free(serial);
        return NULL;
    }
    char *bootstrap = android_bootstrap_command(socket, run_daemon,
                                                idle_seconds, bundle_id);
    g_free(bundle_id);
    /* adb shell already evaluates one remote command string.  Adding
     * `sh -c` here would make the first word of that string the script and
     * execute the remainder in the outer Android shell. */
    const char *shell[] = {"shell", bootstrap, NULL};
    char *remote = NULL;
    if (!adb_execute(serial, shell, cancel, &remote, error)) {
        g_free(bootstrap);
        g_free(serial);
        return NULL;
    }
    g_free(bootstrap);
    if (!remote_socket_is_safe(remote) || !remote || !*remote) {
        char *escaped = g_strescape(remote ? remote : "", NULL);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "ADB returned an invalid Evemon socket path: %s",
                    escaped);
        g_free(escaped);
        g_free(remote);
        g_free(serial);
        return NULL;
    }

    adb_tunnel_t *t = g_new0(adb_tunnel_t, 1);
    t->serial = serial;
    t->directory = g_dir_make_tmp("evemon-adb-XXXXXX", error);
    if (!t->directory) {
        g_free(remote);
        adb_tunnel_close(t);
        return NULL;
    }
    if (geteuid() == 0 && chown(t->directory, desktop_session_uid(),
                                desktop_session_gid())) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "ADB directory: %s", g_strerror(errno));
        g_free(remote);
        adb_tunnel_close(t);
        return NULL;
    }
    t->path = g_build_filename(t->directory, "evemon.sock", NULL);
    t->local_spec = g_strdup_printf("localfilesystem:%s", t->path);
    char *remote_spec = remote[0] == '@'
        ? g_strdup_printf("localabstract:%s", remote + 1)
        : g_strdup_printf("localfilesystem:%s", remote);
    const char *forward[] = {"forward", "--no-rebind", t->local_spec,
                             remote_spec, NULL};
    gboolean forwarded = adb_execute(t->serial, forward, cancel, NULL, error);
    g_free(remote_spec);
    g_free(remote);
    if (!forwarded) {
        adb_tunnel_close(t);
        return NULL;
    }
    ipc_desktop_socket(t->path, 1);
    while (!cancel || !g_cancellable_is_cancelled(cancel)) {
        int fd = ipc_connect(t->path);
        if (fd >= 0) {
            int rc = ipc_hello(fd, NULL, NULL);
            close(fd);
            if (!rc) return t;
            break;
        }
        g_usleep(50000);
    }
    if (!cancel || !g_cancellable_set_error_if_cancelled(cancel, error))
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                    "No Android Evemon could be reached on ADB device %s.",
                    t->serial);
    adb_tunnel_close(t);
    return NULL;
}
