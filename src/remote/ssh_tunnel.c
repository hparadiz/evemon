#include "ssh_tunnel.h"
#include "../platform/desktop_session.h"
#include "../ipc/ipc_wire.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

struct ssh_tunnel {
    char *directory, *path, *log, **argv, *host, *remote_socket;
    GPid pid;
    guint timer;
    GMutex pid_lock;
    GCancellable *cancel;
    gulong cancel_handler;
    gint64 spawned_at, retry_at;
    unsigned failures;
};
static const char *ssh_options[] = {
    "ssh", "-o", "ConnectTimeout=3", "-o", "ConnectionAttempts=1",
    "-o", "ControlMaster=no", "-o", "ControlPersist=no", "-o", "ControlPath=none",
    "-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=2", NULL
};
static void ssh_child_setup(gpointer parent)
{
    desktop_child_use_user(NULL);
    /* A GUI launched from a terminal must not let SSH open /dev/tty for a
     * passphrase. Own a detached session; the PID still names our process group
     * for cancellation, including askpass children. */
    if (setsid() < 0) _exit(126);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != GPOINTER_TO_INT(parent)) _exit(1);
}
static GPtrArray *ssh_arguments(void)
{
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; ssh_options[i]; ++i) g_ptr_array_add(args, g_strdup(ssh_options[i]));
    return args;
}
static int archive_name_is_safe(const char *name)
{
    return name && *name && name[0] != '-' && !strchr(name, '/') &&
        strspn(name,
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
        strlen(name);
}
static char *adjacent_file(const char *executable, const char *name)
{
    char *directory = g_path_get_dirname(executable);
    char *path = g_build_filename(directory, name, NULL);
    g_free(directory);
    if (access(path, R_OK) == 0) return path;
    g_free(path);
    return NULL;
}
static gboolean remote_bundle_paths(char **executable, char **bpf,
                                    GError **error)
{
    char path[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0 || (size_t)length >= sizeof(path) - 1) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Cannot locate this Evemon executable: %s",
                    g_strerror(errno));
        return FALSE;
    }
    path[length] = 0;
    *executable = g_canonicalize_filename(path, NULL);

    char *exe_name = g_path_get_basename(*executable);
    gboolean valid = archive_name_is_safe(exe_name);
    g_free(exe_name);
    if (!valid || access(*executable, R_OK)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Evemon's executable is unavailable.");
        g_clear_pointer(executable, g_free);
        return FALSE;
    }

    *bpf = adjacent_file(*executable, "fdmon_ebpf_kern.o");
#ifdef EVEMON_LIBDIR
    if (!*bpf) {
        char *installed = g_build_filename(EVEMON_LIBDIR,
                                            "fdmon_ebpf_kern.o", NULL);
        if (access(installed, R_OK) == 0) *bpf = installed;
        else g_free(installed);
    }
#endif
    return TRUE;
}
static char *remote_bootstrap_command(const char *socket,
                                      unsigned idle_seconds,
                                      const char *exe_name)
{
    char *quoted_socket = g_shell_quote(socket ? socket : "");
    char *quoted_exe = g_shell_quote(exe_name);
    char *command = g_strdup_printf(
        "set -eu;umask 077;"
        "base=$HOME/.cache/evemon/remote;"
        "mkdir -p \"$base\";chmod 700 \"$base\";"
        "tmp=$(mktemp -d \"$base/.install.XXXXXX\");"
        "trap 'rm -rf \"$tmp\"' EXIT HUP INT TERM;"
        "tar -xf - -C \"$tmp\";"
        "exe=$tmp/%s;"
        "test -f \"$exe\" && ! test -L \"$exe\";"
        "chmod 700 \"$exe\";"
        "mv -f \"$exe\" \"$base/evemon\";"
        /* An unmatched glob is an error in a zsh login shell; sh passes it on. */
        "sh -c 'rm -f \"$0\"/libevemon_core.so*' \"$base\";"
        "if test -f \"$tmp/fdmon_ebpf_kern.o\" && ! test -L \"$tmp/fdmon_ebpf_kern.o\";then "
          "chmod 600 \"$tmp/fdmon_ebpf_kern.o\";"
          "mv -f \"$tmp/fdmon_ebpf_kern.o\" \"$base/fdmon_ebpf_kern.o\";"
        "else rm -f \"$base/fdmon_ebpf_kern.o\";fi;"
        "socket=%s;"
        "if test -z \"$socket\";then "
          "socket=$(\"$base/evemon\" --print-socket);"
        "fi;"
        "check(){ \"$base/evemon\" --socket \"$socket\" --query status >/dev/null 2>&1;};"
        "if ! check;then "
          "log=$base/daemon.log;: >\"$log\";"
          "if test %u -gt 0;then "
            "nohup \"$base/evemon\" --headless --socket \"$socket\" --idle-exit %u </dev/null >>\"$log\" 2>&1 &"
          "else "
            "nohup \"$base/evemon\" --headless --socket \"$socket\" </dev/null >>\"$log\" 2>&1 &"
          "fi;"
          "daemon=$!;i=0;"
          "while test $i -lt 100 && ! check;do "
            "if ! kill -0 $daemon 2>/dev/null;then tail -n 20 \"$log\" >&2;exit 47;fi;"
            "sleep .05;i=$((i+1));"
          "done;"
        "fi;"
        "if ! check;then tail -n 20 \"$base/daemon.log\" >&2;exit 47;fi;"
        "printf '%%s\\n' \"$socket\"",
        quoted_exe, quoted_socket, idle_seconds, idle_seconds);
    g_free(quoted_socket); g_free(quoted_exe);
    return command;
}
static char *remote_ensure_command(const char *socket, unsigned idle_seconds,
                                   const char *executable, gboolean supervise)
{
    char *quoted_socket = g_shell_quote(socket ? socket : "");
    char *quoted_exe = executable ? g_shell_quote(executable)
                                  : g_strdup("\"$base/evemon\"");
    char *command = g_strdup_printf(
        "set -eu;umask 077;"
        "base=$HOME/.cache/evemon/remote;socket=%s;exe=%s;"
        "if test -z \"$socket\";then socket=$(\"$exe\" --print-socket);fi;"
        "check(){ \"$exe\" --socket \"$socket\" --query status >/dev/null 2>&1;};"
        "if ! check;then "
          "mkdir -p \"$base\";"
          "log=$base/daemon.log;: >\"$log\";"
          "if test %u -gt 0;then "
            "nohup \"$exe\" --headless --socket \"$socket\" --idle-exit %u </dev/null >>\"$log\" 2>&1 &"
          "else "
            "nohup \"$exe\" --headless --socket \"$socket\" </dev/null >>\"$log\" 2>&1 &"
          "fi;"
          "daemon=$!;i=0;"
          "while test $i -lt 100 && ! check;do "
            "if ! kill -0 $daemon 2>/dev/null;then tail -n 20 \"$log\" >&2;exit 47;fi;"
            "sleep .05;i=$((i+1));"
          "done;"
        "fi;"
        "if ! check;then tail -n 20 \"$base/daemon.log\" >&2;exit 47;fi;"
        /* Keep the remote command tied to this forwarding session.  If the
         * daemon disappears, SSH exits and the local supervisor re-runs this
         * command, restoring both the daemon and the tunnel. */
        "%s",
        quoted_socket, quoted_exe, idle_seconds, idle_seconds,
        supervise ? "while sleep 10;do check || exit 47;done"
                  : "printf '%s\\n' \"$socket\"");
    g_free(quoted_socket); g_free(quoted_exe);
    return command;
}
/* Shell: set the positional parameters to the PIDs of the Evemons this client
 * installed (remote_bootstrap_command's copy under $base) that serve $socket.
 * A system service, a GUI, a packaged install or another user's Evemon never
 * matches.  Positional parameters, because a zsh login shell does not split
 * an unquoted list. */
#define REMOTE_MANAGED_PIDS \
    "set --\n" \
    "for p in /proc/[0-9]*; do\n" \
    "  { read -r comm <\"$p/comm\"; } 2>/dev/null || continue\n" \
    "  test \"$comm\" = evemon || continue\n" \
    "  tr '\\0' '\\n' <\"$p/cmdline\" 2>/dev/null | grep -Fqx -- --headless || continue\n" \
    "  test \"$p/exe\" -ef \"$base/evemon\" ||\n" \
    "    case $(readlink \"$p/exe\" 2>/dev/null) in\n" \
    "      \"$base/evemon (deleted)\") ;;\n" \
    "      *) continue ;;\n" \
    "    esac\n" \
    "  tr '\\0' '\\n' <\"$p/cmdline\" 2>/dev/null | grep -Fqx -- \"$socket\" || continue\n" \
    "  set -- \"$@\" \"${p#/proc/}\"\n" \
    "done\n"
/* Prints "R" and the installed copy's mtime when the Evemon serving `socket`
 * is one this client may replace; otherwise "K" (keep): another architecture,
 * a build that already failed to run there, or somebody else's Evemon. */
static char *remote_upgrade_check_command(const char *socket,
                                          const char *architecture,
                                          uint32_t checksum)
{
    char *quoted_socket = g_shell_quote(socket);
    char *command = g_strdup_printf(
        "base=$HOME/.cache/evemon/remote\nsocket=%s\n"
        "test \"$(uname -m)\" = %s || { echo K; exit 0; }\n"
        "test \"$(cat \"$base/unusable\" 2>/dev/null)\" != %u || { echo K; exit 0; }\n"
        REMOTE_MANAGED_PIDS
        "test $# -gt 0 || { echo K; exit 0; }\n"
        "printf 'R\\n%%s\\n' \"$(stat -c %%Y \"$base/evemon\" 2>/dev/null || echo 0)\"\n",
        quoted_socket, architecture, checksum);
    g_free(quoted_socket);
    return command;
}
/* Reads the archive on stdin and swaps it in for the installed copy.  The
 * running Evemon is stopped only after the new build has proved it can run on
 * that host, and the previous build is put back if the new one will not
 * serve.  Prints "U" and the socket after an upgrade, "K" when the previous
 * Evemon was kept. */
static char *remote_upgrade_command(const char *socket, unsigned idle_seconds,
                                    const char *exe_name, uint32_t checksum)
{
    char *quoted_socket = g_shell_quote(socket);
    char *quoted_exe = g_shell_quote(exe_name);
    char *command = g_strdup_printf(
        "set -u\numask 077\n"
        "base=$HOME/.cache/evemon/remote\n"
        "socket=%s\nname=%s\nchecksum=%u\nidle_seconds=%u\n"
        "log=$base/daemon.log\n"
        /* Drain the upload before reporting that the running copy stays. */
        "keep() { cat >/dev/null 2>&1; echo K; exit 0; }\n"
        "check() { \"$base/evemon\" --socket \"$socket\" --query status >/dev/null 2>&1; }\n"
        "start() {\n"
        "  set -- --headless --socket \"$socket\"\n"
        "  if test \"$idle_seconds\" -gt 0; then set -- \"$@\" --idle-exit \"$idle_seconds\"; fi\n"
        "  : >\"$log\"\n"
        "  nohup \"$base/evemon\" \"$@\" </dev/null >>\"$log\" 2>&1 &\n"
        "  daemon=$!\n"
        "  i=0\n"
        "  while test $i -lt 100 && ! check; do\n"
        "    kill -0 \"$daemon\" 2>/dev/null || return 1\n"
        "    sleep .05\n"
        "    i=$((i+1))\n"
        "  done\n"
        "  check\n"
        "}\n"
        /* Unpack and validate before stopping any managed daemon. */
        "mkdir -p \"$base\" || keep\n"
        "tmp=$(mktemp -d \"$base/.install.XXXXXX\") || keep\n"
        "trap 'rm -rf \"$tmp\"' EXIT\n"
        "trap 'exit 1' HUP INT TERM\n"
        "tar -xf - -C \"$tmp\" || keep\n"
        "exe=$tmp/$name\n"
        "{ test -f \"$exe\" && ! test -L \"$exe\" && chmod 700 \"$exe\"; } || keep\n"
        "if ! \"$exe\" --print-socket >/dev/null 2>&1; then\n"
        "  echo \"$checksum\" >\"$base/unusable\"\n"
        "  keep\n"
        "fi\n"
        REMOTE_MANAGED_PIDS
        "test $# -gt 0 || keep\n"
        /* From here the swap finishes even if this SSH session is cut off. */
        "trap '' HUP INT TERM PIPE\n"
        "kill \"$@\" 2>/dev/null\n"
        "i=0\n"
        "while test $i -lt 60; do\n"
        "  alive=\n"
        "  for p in \"$@\"; do kill -0 \"$p\" 2>/dev/null && alive=1; done\n"
        "  test -n \"$alive\" || break\n"
        "  sleep .05\n"
        "  i=$((i+1))\n"
        "done\n"
        "if test -n \"$alive\"; then kill -9 \"$@\" 2>/dev/null; sleep .2; fi\n"
        /* Back up both files, then install the validated upload. */
        "for file in evemon fdmon_ebpf_kern.o; do\n"
        "  rm -f \"$base/$file.previous\"\n"
        "  if test -f \"$base/$file\"; then mv -f \"$base/$file\" \"$base/$file.previous\"; fi\n"
        "done\n"
        "mv -f \"$exe\" \"$base/evemon\"\n"
        "sh -c 'rm -f \"$0\"/libevemon_core.so*' \"$base\"\n"
        "if test -f \"$tmp/fdmon_ebpf_kern.o\" && ! test -L \"$tmp/fdmon_ebpf_kern.o\"; then\n"
        "  chmod 600 \"$tmp/fdmon_ebpf_kern.o\"\n"
        "  mv -f \"$tmp/fdmon_ebpf_kern.o\" \"$base/fdmon_ebpf_kern.o\"\n"
        "fi\n"
        "if start; then\n"
        "  rm -f \"$base/evemon.previous\" \"$base/fdmon_ebpf_kern.o.previous\" \"$base/unusable\"\n"
        "  printf 'U\\n%%s\\n' \"$socket\"\n"
        "  exit 0\n"
        "fi\n"
        /* The new build will not serve here: put the previous one back. */
        "tail -n 20 \"$log\" >&2\n"
        "kill \"$daemon\" 2>/dev/null\n"
        "echo \"$checksum\" >\"$base/unusable\"\n"
        "test -f \"$base/evemon.previous\" || exit 47\n"
        "rm -f \"$base/fdmon_ebpf_kern.o\"\n"
        "for file in evemon fdmon_ebpf_kern.o; do\n"
        "  if test -f \"$base/$file.previous\"; then mv -f \"$base/$file.previous\" \"$base/$file\"; fi\n"
        "done\n"
        "start || exit 47\n"
        "echo K\n"
        "exit 0\n",
        quoted_socket, quoted_exe, checksum, idle_seconds);
    g_free(quoted_socket); g_free(quoted_exe);
    return command;
}
static gboolean remote_command(const char *host, const char *command,
                                GCancellable *cancel, char **out, GError **error)
{
    GPtrArray *args = ssh_arguments();
    g_ptr_array_add(args, g_strdup(host));
    g_ptr_array_add(args, g_strdup(command));
    g_ptr_array_add(args, NULL);
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_setenv(launcher, "SSH_ASKPASS_REQUIRE", "force", TRUE);
    g_subprocess_launcher_set_child_setup(launcher, ssh_child_setup,
                                          GINT_TO_POINTER(getpid()), NULL);
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher,
        (const char *const *)args->pdata, error);
    g_object_unref(launcher); g_ptr_array_free(args, TRUE);
    if (!process) return FALSE;
    char *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8(process, NULL, cancel, out, &err, error);
    if (!ok) {
        const char *identifier = g_subprocess_get_identifier(process);
        if (identifier) kill(-(pid_t)g_ascii_strtoll(identifier, NULL, 10), SIGKILL);
        g_subprocess_force_exit(process); g_subprocess_wait(process, NULL, NULL);
    } else if (!g_subprocess_get_successful(process)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Could not connect to Evemon on %s: %s", host,
                    err && *err ? g_strstrip(err) : "remote command failed");
        ok = FALSE;
    }
    g_free(err); g_object_unref(process);
    return ok;
}

static const char *executable_architecture(void)
{
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "i686";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    return NULL;
#endif
}
/* Sends this build to the host.  A fresh install returns the socket of the
 * Evemon it started; an upgrade returns remote_upgrade_command's report. */
static gboolean bootstrap_remote(const char *host, const char *socket,
                                 unsigned idle_seconds, gboolean upgrade,
                                 GCancellable *cancel, char **remote_path,
                                 GError **error)
{
    char *executable = NULL, *bpf = NULL;
    char *exe_dir = NULL, *exe_name = NULL;
    char *bpf_dir = NULL, *bpf_name = NULL;
    gboolean ok = FALSE;
    if (!remote_bundle_paths(&executable, &bpf, error))
        return FALSE;

    exe_dir = g_path_get_dirname(executable);
    exe_name = g_path_get_basename(executable);
    bpf_dir = bpf ? g_path_get_dirname(bpf) : NULL;
    bpf_name = bpf ? g_path_get_basename(bpf) : NULL;
    const char *tar = access("/usr/bin/tar", X_OK) == 0
                    ? "/usr/bin/tar" : "/bin/tar";

    int archive[2];
    if (pipe2(archive, O_CLOEXEC)) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Cannot prepare remote Evemon archive: %s",
                    g_strerror(errno));
        goto fail_paths;
    }
    GPtrArray *tar_args = g_ptr_array_new_with_free_func(g_free);
    const char *fixed_args[] = {tar, "-chf", "-", "-C", exe_dir,
                                exe_name, NULL};
    for (size_t i = 0; fixed_args[i]; ++i)
        g_ptr_array_add(tar_args, g_strdup(fixed_args[i]));
    if (bpf) {
        g_ptr_array_add(tar_args, g_strdup("-C"));
        g_ptr_array_add(tar_args, g_strdup(bpf_dir));
        g_ptr_array_add(tar_args, g_strdup(bpf_name));
    }
    g_ptr_array_add(tar_args, NULL);
    GSubprocessLauncher *tar_launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_take_stdout_fd(tar_launcher, archive[1]);
    GSubprocess *archiver = g_subprocess_launcher_spawnv(tar_launcher,
        (const char *const *)tar_args->pdata, error);
    g_object_unref(tar_launcher); g_ptr_array_free(tar_args, TRUE);
    if (!archiver) { close(archive[0]); goto fail_paths; }

    char *command = upgrade
        ? remote_upgrade_command(socket, idle_seconds, exe_name,
                                 ipc_self_checksum())
        : remote_bootstrap_command(socket, idle_seconds, exe_name);
    GPtrArray *args = ssh_arguments();
    g_ptr_array_add(args, g_strdup(host));
    g_ptr_array_add(args, command);
    g_ptr_array_add(args, NULL);
    GSubprocessLauncher *ssh_launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_take_stdin_fd(ssh_launcher, archive[0]);
    g_subprocess_launcher_setenv(ssh_launcher, "SSH_ASKPASS_REQUIRE", "force", TRUE);
    g_subprocess_launcher_set_child_setup(ssh_launcher, ssh_child_setup,
                                          GINT_TO_POINTER(getpid()), NULL);
    GSubprocess *process = g_subprocess_launcher_spawnv(ssh_launcher,
        (const char *const *)args->pdata, error);
    g_object_unref(ssh_launcher); g_ptr_array_free(args, TRUE);
    if (!process) {
        g_subprocess_force_exit(archiver);
        g_subprocess_wait(archiver, NULL, NULL);
        g_object_unref(archiver);
        goto fail_paths;
    }

    char *out = NULL, *err = NULL, *tar_err = NULL;
    ok = g_subprocess_communicate_utf8(process, NULL, cancel,
                                       &out, &err, error);
    if (!ok) {
        const char *identifier = g_subprocess_get_identifier(process);
        if (identifier)
            kill(-(pid_t)g_ascii_strtoll(identifier, NULL, 10), SIGKILL);
        g_subprocess_force_exit(process);
        g_subprocess_wait(process, NULL, NULL);
    }
    gboolean tar_ok = g_subprocess_communicate_utf8(
        archiver, NULL, NULL, NULL, &tar_err, NULL) &&
        g_subprocess_get_successful(archiver);
    if (ok && (!g_subprocess_get_successful(process) || !tar_ok)) {
        const char *detail = err && *err ? g_strstrip(err)
                           : tar_err && *tar_err ? g_strstrip(tar_err)
                           : "remote installation failed";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Could not install or start Evemon on %s: %s",
                    host, detail);
        ok = FALSE;
    }
    if (ok && upgrade) {
        if (!out) out = g_strdup("");
    } else if (ok && out) {
        g_strstrip(out);
        if (out[0] != '/' ||
            strlen(out) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
            strpbrk(out, ":\r\n")) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "SSH returned an invalid Evemon socket path.");
            ok = FALSE;
        }
    } else if (ok) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "SSH returned no Evemon socket path.");
        ok = FALSE;
    }
    if (ok) *remote_path = g_steal_pointer(&out);
    g_free(out); g_free(err); g_free(tar_err);
    g_object_unref(process); g_object_unref(archiver);
fail_paths:
    g_free(executable); g_free(bpf);
    g_free(exe_dir); g_free(exe_name);
    g_free(bpf_dir); g_free(bpf_name);
    return ok;
}
/* PR_SET_PDEATHSIG follows the thread that forked, not the process.  A tunnel
 * is opened from a GTask pool thread, which GLib retires after some idle
 * seconds; an SSH child forked there is then signalled while its owner is
 * still running.  One thread that lives as long as the process forks every
 * long-lived tunnel instead. */
typedef struct {
    char **argv, **environment;
    int input, output, log;
    GPid pid;
    GError *error;
    gboolean ok, done;
} tunnel_spawn_t;
static GMutex spawn_lock;
static GCond spawn_changed;
static GQueue spawn_queue = G_QUEUE_INIT;
static gpointer spawn_worker(gpointer unused)
{
    (void)unused;
    g_mutex_lock(&spawn_lock);
    for (;;) {
        tunnel_spawn_t *request = g_queue_pop_head(&spawn_queue);
        if (!request) { g_cond_wait(&spawn_changed, &spawn_lock); continue; }
        g_mutex_unlock(&spawn_lock);
        gboolean ok = g_spawn_async_with_fds(NULL, request->argv, request->environment,
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, ssh_child_setup, GINT_TO_POINTER(getpid()),
            &request->pid, request->input, request->output, request->log, &request->error);
        g_mutex_lock(&spawn_lock);
        request->ok = ok; request->done = TRUE;
        g_cond_broadcast(&spawn_changed);
    }
    return NULL;
}
static gboolean spawn_tunnel(ssh_tunnel_t *t, GError **error)
{
    static gsize started;
    if (g_once_init_enter(&started)) {
        g_thread_unref(g_thread_new("ev-ssh-spawn", spawn_worker, NULL));
        g_once_init_leave(&started, 1);
    }
    int log = open(t->log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    gboolean ok = log >= 0 && null >= 0;
    char **environment = g_get_environ();
    environment = g_environ_setenv(environment, "SSH_ASKPASS_REQUIRE", "force", TRUE);
    if (ok) {
        tunnel_spawn_t request = {.argv = t->argv, .environment = environment,
                                  .input = null, .output = null, .log = log};
        g_mutex_lock(&spawn_lock);
        g_queue_push_tail(&spawn_queue, &request);
        g_cond_broadcast(&spawn_changed);
        while (!request.done) g_cond_wait(&spawn_changed, &spawn_lock);
        g_mutex_unlock(&spawn_lock);
        ok = request.ok;
        if (ok) {
            /* A cancellation may be reading the PID to stop the old child. */
            g_mutex_lock(&t->pid_lock); t->pid = request.pid; g_mutex_unlock(&t->pid_lock);
            t->spawned_at = g_get_monotonic_time();
        } else g_propagate_error(error, request.error);
    } else g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "SSH: %s", g_strerror(errno));
    if (log >= 0) close(log);
    if (null >= 0) close(null);
    g_strfreev(environment);
    return ok;
}
static gboolean tunnel_alive(ssh_tunnel_t *t)
{
    g_mutex_lock(&t->pid_lock);
    if (!t->pid) { g_mutex_unlock(&t->pid_lock); return FALSE; }
    int status;
    pid_t result = waitpid(t->pid, &status, WNOHANG);
    gboolean alive = !result || (result < 0 && errno == EINTR);
    if (!alive) { g_spawn_close_pid(t->pid); t->pid = 0; }
    g_mutex_unlock(&t->pid_lock);
    return alive;
}
static void cancel_tunnel(GCancellable *cancel, gpointer data)
{
    (void)cancel;
    ssh_tunnel_t *t = data;
    /* Closing the owned SSH process also wakes every in-flight HELLO, host
     * query and subscription waiting on its forwarded sockets. */
    g_mutex_lock(&t->pid_lock);
    if (t->pid > 0) kill(-t->pid, SIGKILL);
    g_mutex_unlock(&t->pid_lock);
}
static void detach_cancel(ssh_tunnel_t *t)
{
    if (t->cancel_handler) g_cancellable_disconnect(t->cancel, t->cancel_handler);
    t->cancel_handler = 0;
    g_clear_object(&t->cancel);
}
static gboolean supervise(gpointer data)
{
    ssh_tunnel_t *t = data;
    gint64 now = g_get_monotonic_time();
    if (!tunnel_alive(t)) {
        if (now < t->retry_at) return G_SOURCE_CONTINUE;
        unsigned delay = 1u << MIN(t->failures, 5u);
        if (t->failures < 5) t->failures++;
        t->retry_at = now + delay * G_TIME_SPAN_SECOND;
        unlink(t->path);
        GError *error = NULL;
        if (!spawn_tunnel(t, &error)) {
            g_warning("evemon: SSH reconnect: %s", error->message);
            g_clear_error(&error);
        }
    } else if (now - t->spawned_at >= 30 * G_TIME_SPAN_SECOND) t->failures = 0;
    return G_SOURCE_CONTINUE;
}
void ssh_tunnel_activate(ssh_tunnel_t *t)
{
    if (t) {
        detach_cancel(t);
        if (!t->timer) t->timer = g_timeout_add_seconds(1, supervise, t);
    }
}
const char *ssh_tunnel_path(const ssh_tunnel_t *t) { return t->path; }
void ssh_tunnel_close(ssh_tunnel_t *t)
{
    if (!t) return;
    detach_cancel(t);
    if (t->timer) g_source_remove(t->timer);
    if (tunnel_alive(t)) {
        kill(-t->pid, SIGTERM);
        for (int i = 0; i < 20 && tunnel_alive(t); ++i) g_usleep(10000);
        if (tunnel_alive(t)) {
            kill(-t->pid, SIGKILL);
            while (waitpid(t->pid, NULL, 0) < 0 && errno == EINTR) {}
            g_spawn_close_pid(t->pid);
        }
    }
    if (t->path) { ipc_desktop_socket(t->path, 0); unlink(t->path); }
    if (t->log) unlink(t->log);
    if (t->directory) rmdir(t->directory);
    g_free(t->path); g_free(t->log); g_free(t->directory); g_strfreev(t->argv);
    g_free(t->host); g_free(t->remote_socket);
    g_mutex_clear(&t->pid_lock); g_free(t);
}
/* True when the Evemon behind `fd` is another build of this version or an
 * older one.  A newer Evemon is never a candidate for replacement, and a
 * failed question says nothing about the answer. */
static gboolean remote_build_differs(int fd, uint64_t version)
{
    uint32_t checksum = 0;
    if (version > ipc_build() || !ipc_self_checksum()) return FALSE;
    if (ipc_checksum(fd, &checksum)) return errno == ENOTSUP;
    return checksum != ipc_self_checksum();
}
/* Replaces the Evemon this client installed on the remote with this build
 * when that copy is older.  Anything else answering on the socket keeps
 * running untouched: a system service, a packaged or another user's Evemon,
 * a GUI.  Fails only when cancelled or when the host was left without a
 * working Evemon. */
static gboolean ssh_tunnel_upgrade(ssh_tunnel_t *t, uint64_t remote_version,
                                   unsigned idle_seconds,
                                   ssh_tunnel_patience_fn patience,
                                   gpointer patience_data,
                                   GCancellable *cancel, GError **error)
{
    const char *architecture = executable_architecture();
    char *executable = NULL, *bpf = NULL;
    if (!architecture || !remote_bundle_paths(&executable, &bpf, NULL)) return TRUE;
    struct stat local;
    gboolean readable = stat(executable, &local) == 0;
    g_free(executable); g_free(bpf);
    if (!readable) return TRUE;

    if (patience) patience(patience_data, 5);
    g_autofree char *check = remote_upgrade_check_command(
        t->remote_socket, architecture, ipc_self_checksum());
    g_autofree char *report = NULL;
    GError *failure = NULL;
    if (!remote_command(t->host, check, cancel, &report, &failure)) {
        if (g_cancellable_is_cancelled(cancel)) { g_propagate_error(error, failure); return FALSE; }
        /* Not knowing is no reason to refuse an Evemon that works. */
        g_clear_error(&failure);
        return TRUE;
    }
    if (!report || report[0] != 'R' || report[1] != '\n') return TRUE;
    /* Between two builds of one version the newer file wins, so clients with
     * different builds do not keep replacing each other's. */
    if (remote_version == ipc_build() &&
        g_ascii_strtoll(report + 2, NULL, 10) >= (gint64)local.st_mtime) return TRUE;

    if (patience) patience(patience_data, 60);
    g_autofree char *result = NULL;
    return bootstrap_remote(t->host, t->remote_socket, idle_seconds, TRUE,
                            cancel, &result, error);
}
ssh_tunnel_t *ssh_tunnel_open(const char *host, const char *socket,
                              int run_daemon, unsigned idle_seconds,
                              ssh_tunnel_patience_fn patience,
                              gpointer patience_data,
                              GCancellable *cancel, GError **error)
{
    if (!host || !*host || host[0] == '-' || strlen(host) > 255 ||
        strspn(host, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.@:%[]-") != strlen(host)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Enter an SSH host, IP address, or user@host.");
        return NULL;
    }
    if (socket && *socket && (socket[0] != '/' || strpbrk(socket, ":\r\n"))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The remote socket must be an absolute path without colons or newlines.");
        return NULL;
    }
    /* Discover a running instance before considering installation. The wire
     * protocol is portable; this executable and its dynamic loader are not. */
    g_autofree char *quoted = g_shell_quote(socket ? socket : "");
    const char *user_paths = "\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/evemon/query.sock\" \"/tmp/evemon-$(id -u)/query.sock\"";
    g_autofree char *paths = run_daemon
        ? g_strdup_printf("%s " IPC_SYSTEM_SOCKET, user_paths)
        : g_strdup_printf(IPC_SYSTEM_SOCKET " %s", user_paths);
    g_autofree char *command = g_strdup_printf(
        "p=%s;denied=;"
        "if test -n \"$p\";then set -- \"$p\";else set -- %s;fi;"
        "for p;do d=${p%%/*};"
          "if { test -d \"$d\" && ! test -x \"$d\"; } || "
             "{ test -S \"$p\" && { ! test -r \"$p\" || ! test -w \"$p\"; }; };then denied=$p;"
          "elif test -S \"$p\";then printf 'S\\n%%s\\n' \"$p\";exit 0;fi;"
        "done;"
        "if test -n \"$denied\";then printf 'D\\n%%s\\n' \"$denied\";exit 0;fi;"
        "if test %d -eq 1;then "
          "for exe in \"$HOME/.local/bin/evemon\" \"$(command -v evemon || :)\" "
             "/usr/local/bin/evemon /usr/bin/evemon /opt/evemon/bin/evemon \"$HOME/.cache/evemon/remote/evemon\";do "
            "if test -x \"$exe\" && \"$exe\" --print-socket >/dev/null 2>&1;then "
              "printf 'E\\n%%s\\n' \"$exe\";exit 0;fi;"
          "done;"
        "fi;printf 'M\\n';uname -m", quoted, paths, !!run_daemon);
    g_autofree char *probe = NULL;
    if (!remote_command(host, command, cancel, &probe, error)) return NULL;
    if (!probe || strlen(probe) < 3 || probe[1] != '\n') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "SSH returned invalid Evemon discovery information.");
        return NULL;
    }
    char *value = g_strchomp(probe + 2);
    char *out = NULL;
    g_autofree char *remote_executable = NULL;
    if (probe[0] == 'D') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Access denied to %s on %s. Your SSH account needs directory traversal and read/write socket access in the filesystem ACL.", value, host);
        return NULL;
    } else if (probe[0] == 'S') {
        out = g_strdup(value);
        run_daemon = 0; /* An existing instance owns its own lifecycle. */
    } else if (probe[0] == 'E' && run_daemon && value[0] == '/' && !strpbrk(value, "\r\n")) {
        remote_executable = g_strdup(value);
        g_autofree char *start = remote_ensure_command(socket, idle_seconds, remote_executable, FALSE);
        if (!remote_command(host, start, cancel, &out, error)) { g_free(out); return NULL; }
    } else if (probe[0] == 'M' && run_daemon) {
        const char *architecture = executable_architecture();
        if (!architecture || strcmp(architecture, value)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "%s requires a native %s Evemon executable. This %s build cannot be installed there. Install Evemon for that machine's architecture first.",
                        host, value, architecture ? architecture : "unsupported-architecture");
            return NULL;
        }
        /* Uploading and starting a build takes longer than connecting. */
        if (patience) patience(patience_data, 60);
        if (!bootstrap_remote(host, socket, idle_seconds, FALSE, cancel, &out, error)) return NULL;
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "No Evemon is available on %s%s%s.", host,
                    socket && *socket ? " at " : "", socket && *socket ? socket : "");
        return NULL;
    }
    if (out) g_strchomp(out);
    if (!out || out[0] != '/' ||
        strlen(out) >= sizeof(((struct sockaddr_un *)0)->sun_path) || strpbrk(out, ":\r\n")) {
        g_free(out);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "SSH returned an invalid Evemon socket path.");
        return NULL;
    }
    ssh_tunnel_t *t = g_new0(ssh_tunnel_t, 1);
    g_mutex_init(&t->pid_lock);
    t->directory = g_dir_make_tmp("evemon-remote-XXXXXX", error);
    if (!t->directory) { g_free(out); ssh_tunnel_close(t); return NULL; }
    /* SSH uses the desktop user's agent, keys, configuration and known_hosts,
     * including when this frontend was launched through kdesu. */
    if (geteuid() == 0 && chown(t->directory, desktop_session_uid(), desktop_session_gid())) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "SSH directory: %s", g_strerror(errno));
        g_free(out); ssh_tunnel_close(t); return NULL;
    }
    t->host = g_strdup(host);
    t->remote_socket = g_strdup(out);
    t->path = g_build_filename(t->directory, "evemon.sock", NULL);
    ipc_desktop_socket(t->path, 1);
    t->log = g_build_filename(t->directory, "ssh.log", NULL);
    GPtrArray *args = ssh_arguments();
    g_ptr_array_add(args, g_strdup(run_daemon ? "-T" : "-NT"));
    g_ptr_array_add(args, g_strdup("-o")); g_ptr_array_add(args, g_strdup("ExitOnForwardFailure=yes"));
    g_ptr_array_add(args, g_strdup("-L")); g_ptr_array_add(args, g_strdup_printf("%s:%s", t->path, out));
    g_ptr_array_add(args, g_strdup(host));
    if (run_daemon)
        g_ptr_array_add(args, remote_ensure_command(out, idle_seconds, remote_executable, TRUE));
    g_ptr_array_add(args, NULL);
    t->argv = (char **)g_ptr_array_free(args, FALSE); g_free(out);
    if (!spawn_tunnel(t, error)) { ssh_tunnel_close(t); return NULL; }
    if (cancel) {
        t->cancel = g_object_ref(cancel);
        t->cancel_handler = g_cancellable_connect(cancel, G_CALLBACK(cancel_tunnel), t, NULL);
    }
    gboolean compared = FALSE;
    while (tunnel_alive(t) && !g_cancellable_is_cancelled(cancel)) {
        int fd = ipc_connect(t->path);
        if (fd >= 0) {
            uint64_t version = 0;
            int rc = ipc_version(fd, NULL, NULL, &version);
            gboolean differs = !rc && !compared && remote_build_differs(fd, version);
            close(fd);
            if (differs) {
                compared = TRUE;
                if (!ssh_tunnel_upgrade(t, version, idle_seconds, patience, patience_data, cancel, error)) {
                    ssh_tunnel_close(t); return NULL;
                }
                /* A supervising tunnel may have taken the swap for a lost
                 * Evemon and exited; the next connection needs a new one. */
                if (!tunnel_alive(t)) {
                    unlink(t->path);
                    if (!spawn_tunnel(t, error)) { ssh_tunnel_close(t); return NULL; }
                }
                continue;
            }
            if (!rc) return t;
            break; /* A refused/broken Evemon is an error, not a retry loop. */
        }
        g_usleep(50000);
    }
    if (!g_cancellable_set_error_if_cancelled(cancel, error)) {
        char *detail = NULL;
        g_file_get_contents(t->log, &detail, NULL, NULL);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
            "No Evemon could be reached on %s.%s%s", host,
            detail && *detail ? " SSH: " : "", detail && *detail ? g_strstrip(detail) : "");
        g_free(detail);
    }
    ssh_tunnel_close(t); return NULL;
}
