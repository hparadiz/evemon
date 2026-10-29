#include "desktop_session.h"
#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int initialized;
static uid_t session_uid;
static gid_t session_gid;
static char session_user[256];
static gid_t *session_groups;
static int session_group_count;
static const char *session_keys[] = {
    "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_SESSION_ID",
    "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE",
    "SSH_AUTH_SOCK", "SSH_ASKPASS", "SSH_ASKPASS_REQUIRE"
};

static const char *process_env(const char *environment, size_t length, const char *key)
{
    size_t key_length = strlen(key);
    for (size_t offset = 0; offset < length; ) {
        const char *entry = environment + offset;
        size_t size = strnlen(entry, length - offset);
        if (size == length - offset) break;
        if (size > key_length && !memcmp(entry, key, key_length) && entry[key_length] == '=')
            return entry + key_length + 1;
        offset += size + 1;
    }
    return NULL;
}

static pid_t process_parent(pid_t pid, uid_t *uid)
{
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *status = fopen(path, "r");
    if (!status) return 0;
    unsigned parent = 0, owner = (uid_t)-1;
    while (fgets(line, sizeof(line), status)) {
        if (sscanf(line, "PPid: %u", &parent) == 1) continue;
        if (sscanf(line, "Uid: %u", &owner) == 1) *uid = owner;
    }
    fclose(status);
    return parent == (unsigned)pid ? 0 : (pid_t)parent;
}

static gboolean user_agent(const char *path, uid_t uid)
{
    struct stat st;
    return path && *path && !stat(path, &st) && S_ISSOCK(st.st_mode) && st.st_uid == uid;
}

static void restore_session_value(const char *key, const char *value, uid_t uid)
{
    const char *current = getenv(key);
    if (current && *current) return;
    if (!value || !*value) return;
    if (!strcmp(key, "SSH_AUTH_SOCK") && !user_agent(value, uid)) return;
    setenv(key, value, 1);
}

static void restore_ancestor_environment(uid_t uid)
{
    /* A shell or elevation launcher is stronger evidence than an arbitrary
     * process on the desktop. Recover only session/SSH keys, never a complete
     * environment (loader settings, credentials, etc.). Nearest ancestor wins. */
    pid_t pid = getppid();
    for (unsigned depth = 0; pid > 1 && depth < 64; ++depth) {
        uid_t owner = (uid_t)-1;
        pid_t parent = process_parent(pid, &owner);
        if (owner == uid) {
            char path[64], *environment = NULL;
            gsize length = 0;
            snprintf(path, sizeof(path), "/proc/%ld/environ", (long)pid);
            if (g_file_get_contents(path, &environment, &length, NULL)) {
                for (size_t i = 0; i < G_N_ELEMENTS(session_keys); ++i)
                    restore_session_value(session_keys[i], process_env(environment, length, session_keys[i]), uid);
                restore_session_value("DBUS_SESSION_BUS_ADDRESS",
                    process_env(environment, length, "DBUS_SESSION_BUS_ADDRESS"), uid);
            }
            g_free(environment);
        }
        pid = parent;
    }
}

static char *restore_desktop_environment(uid_t uid)
{
    /* Some desktops use a /tmp bus instead of /run/user/UID/bus. Recover only
     * from this user's desktop session, matching any inherited display IDs.
     * Multiple different matching buses are ambiguous; never pick one at random. */
    GDir *directory = g_dir_open("/proc", 0, NULL);
    if (!directory) return NULL;
    char *result = NULL;
    char *session_values[G_N_ELEMENTS(session_keys)] = {0};
    const char *entry;
    while ((entry = g_dir_read_name(directory))) {
        if (entry[0] < '1' || entry[0] > '9' || strspn(entry, "0123456789") != strlen(entry)) continue;
        char *path = g_build_filename("/proc", entry, NULL);
        struct stat st;
        if (stat(path, &st) || st.st_uid != uid) { g_free(path); continue; }
        char *comm_path = g_build_filename(path, "comm", NULL), *comm = NULL;
        g_file_get_contents(comm_path, &comm, NULL, NULL); g_free(comm_path);
        gboolean desktop = comm && (!strcmp(comm, "plasmashell\n") || !strcmp(comm, "gnome-shell\n") ||
            !strcmp(comm, "sway\n") || !strcmp(comm, "Hyprland\n") || !strcmp(comm, "xfce4-session\n") ||
            !strcmp(comm, "cinnamon\n") || !strcmp(comm, "mate-session\n") || !strcmp(comm, "lxqt-session\n"));
        g_free(comm);
        if (!desktop) { g_free(path); continue; }
        char *env_path = g_build_filename(path, "environ", NULL), *environment = NULL;
        gsize length = 0;
        g_file_get_contents(env_path, &environment, &length, NULL); g_free(env_path); g_free(path);
        if (!environment) continue;
        gboolean matches = TRUE;
        const char *keys[] = {"DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_ID"};
        for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i) {
            const char *current = getenv(keys[i]);
            if (current && *current && g_strcmp0(current, process_env(environment, length, keys[i]))) matches = FALSE;
        }
        const char *bus = process_env(environment, length, "DBUS_SESSION_BUS_ADDRESS");
        if (matches && bus && *bus) {
            gboolean conflict = result && strcmp(result, bus);
            for (size_t i = 0; result && i < G_N_ELEMENTS(session_keys); ++i)
                conflict |= g_strcmp0(session_values[i], process_env(environment, length, session_keys[i])) != 0;
            if (conflict) {
                for (size_t i = 0; i < G_N_ELEMENTS(session_keys); ++i) g_free(session_values[i]);
                g_free(result); g_free(environment); g_dir_close(directory); return NULL;
            }
            if (!result) {
                result = g_strdup(bus);
                for (size_t i = 0; i < G_N_ELEMENTS(session_keys); ++i)
                    session_values[i] = g_strdup(process_env(environment, length, session_keys[i]));
            }
        }
        g_free(environment);
    }
    g_dir_close(directory);
    for (size_t i = 0; i < G_N_ELEMENTS(session_keys); ++i) {
        restore_session_value(session_keys[i], session_values[i], uid);
        g_free(session_values[i]);
    }
    return result;
}

static uid_t parse_uid(const char *text)
{
    if (!text || !*text) return (uid_t)-1;
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value == 0 || value >= (uid_t)-1) return (uid_t)-1;
    return (uid_t)value;
}

static uid_t invoking_uid(void)
{
    if (geteuid() != 0) return getuid();
    if (getuid() != 0) return getuid();
    const char *keys[] = {"EVEMON_DESKTOP_UID", "SUDO_UID", "PKEXEC_UID", NULL};
    for (int i = 0; keys[i]; ++i) {
        uid_t uid = parse_uid(getenv(keys[i]));
        if (uid != (uid_t)-1 && getpwuid(uid)) return uid;
    }
    pid_t pid = getppid();
    for (unsigned depth = 0; pid > 1 && depth < 64; ++depth) {
        uid_t uid = (uid_t)-1;
        pid_t parent = process_parent(pid, &uid);
        if (uid != 0 && uid != (uid_t)-1 && getpwuid(uid)) return uid;
        pid = parent;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    if (runtime && !strncmp(runtime, "/run/user/", 10) &&
        lstat(runtime, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid != 0 &&
        st.st_uid == parse_uid(runtime + 10) && getpwuid(st.st_uid)) return st.st_uid;
    FILE *login = fopen("/proc/self/loginuid", "r");
    char value[32] = "";
    if (login) { if (!fgets(value, sizeof(value), login)) value[0] = 0; fclose(login); }
    value[strcspn(value, "\n")] = 0;
    uid_t uid = parse_uid(value);
    return uid != (uid_t)-1 && getpwuid(uid) ? uid : 0;
}

void desktop_session_init(void)
{
    if (initialized) return;
    initialized = 1;
    session_uid = invoking_uid();
    struct passwd *pw = getpwuid(session_uid);
    if (!pw) { session_uid = getuid(); session_gid = getgid(); return; }
    session_gid = pw->pw_gid;
    snprintf(session_user, sizeof(session_user), "%s", pw->pw_name);
    const char *current_shell = getenv("SHELL");
    if ((!current_shell || !*current_shell) && pw->pw_shell && *pw->pw_shell)
        setenv("SHELL", pw->pw_shell, 1);
    if (geteuid() == 0 && session_uid != 0) {
        /* Elevation often strips SSH_AUTH_SOCK while leaving enough identity
         * information to find the launching shell. Do not keep a root-owned
         * or stale agent when SSH will run as the original user. */
        if (!user_agent(getenv("SSH_AUTH_SOCK"), session_uid)) unsetenv("SSH_AUTH_SOCK");
        restore_ancestor_environment(session_uid);
        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1); setenv("LOGNAME", pw->pw_name, 1);
        if (pw->pw_shell && *pw->pw_shell)
            setenv("SHELL", pw->pw_shell, 1);
        const char *keys[] = {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"};
        const char *suffix[] = {".config", ".local/share", ".cache"};
        for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i) {
            const char *current = getenv(keys[i]);
            if (!current || !*current || !strncmp(current, "/root/", 6)) {
                char *path = g_build_filename(pw->pw_dir, suffix[i], NULL);
                setenv(keys[i], path, 1); g_free(path);
            }
        }
        int count = 0;
        getgrouplist(session_user, session_gid, NULL, &count);
        if (count > 0 && count <= NGROUPS_MAX) {
            session_groups = g_try_new(gid_t, count);
            if (session_groups && getgrouplist(session_user, session_gid, session_groups, &count) >= 0)
                session_group_count = count;
        }
    }
    char uid[32], runtime[64];
    snprintf(uid, sizeof(uid), "%lu", (unsigned long)session_uid);
    setenv("EVEMON_DESKTOP_UID", uid, 1);
    snprintf(runtime, sizeof(runtime), "/run/user/%s", uid);
    struct stat st;
    const char *current = getenv("XDG_RUNTIME_DIR");
    if ((!current || lstat(current, &st) || !S_ISDIR(st.st_mode) || st.st_uid != session_uid) &&
        lstat(runtime, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == session_uid)
        setenv("XDG_RUNTIME_DIR", runtime, 1);
    current = getenv("XDG_RUNTIME_DIR");
    if (current && geteuid() == 0 && session_uid != 0) {
        if (!getenv("PIPEWIRE_REMOTE")) {
            char *remote = g_build_filename(current, "pipewire-0", NULL);
            setenv("PIPEWIRE_REMOTE", remote, 1); g_free(remote);
        }
        // Detached elevation helpers may have no surviving user ancestor.
        // Recover missing keys only from that user's matching desktop session.
        char *address = restore_desktop_environment(session_uid);
        const char *inherited_bus = getenv("DBUS_SESSION_BUS_ADDRESS");
        if (!inherited_bus || !*inherited_bus) {
            if (!address) {
                char *socket = g_build_filename(current, "bus", NULL);
                if (!lstat(socket, &st) && S_ISSOCK(st.st_mode) && st.st_uid == session_uid)
                    address = g_strdup_printf("unix:path=%s", socket);
                g_free(socket);
            }
            if (address) setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
        }
        g_free(address);
    }
}

uid_t desktop_session_uid(void) { desktop_session_init(); return session_uid; }
gid_t desktop_session_gid(void) { desktop_session_init(); return session_gid; }
const char *desktop_session_user(void) { desktop_session_init(); return session_user; }
int desktop_session_trusts_owner(uid_t owner)
{
    desktop_session_init();
    uid_t effective = geteuid();
    return owner == 0 || owner == effective ||
           (effective == 0 && session_uid != 0 && owner == session_uid);
}

void desktop_child_use_user(void *unused)
{
    (void)unused;
    if (geteuid() != 0 || session_uid == 0) return;
    if (setgroups(session_group_count, session_groups) ||
        setgid(session_gid) || setuid(session_uid)) _exit(126);
}

char *desktop_elevation_command(const char *executable, const char *frontend)
{
    desktop_session_init();
    const char *keys[] = {
        "EVEMON_DESKTOP_UID", "HOME", "USER", "LOGNAME", "SHELL",
        "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
        "XDG_RUNTIME_DIR", "XDG_CURRENT_DESKTOP", "DESKTOP_SESSION",
        "XDG_SESSION_TYPE", "DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY",
        "XAUTHORITY", "SSH_AUTH_SOCK", "SSH_ASKPASS", "SSH_ASKPASS_REQUIRE",
        "QT_SCALE_FACTOR",
        "PIPEWIRE_REMOTE", "PIPEWIRE_RUNTIME_DIR", "PULSE_SERVER", "LANG", "LC_ALL", NULL
    };
    /* Start the privileged process from an allowlisted environment.  In
     * particular, never carry loader/module selectors or the caller's PATH
     * across the privilege boundary. */
    GString *command = g_string_new(
        "/usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin");
    for (int i = 0; keys[i]; ++i) {
        const char *value = getenv(keys[i]); if (!value) continue;
        char *assignment = g_strdup_printf("%s=%s", keys[i], value);
        char *quoted = g_shell_quote(assignment);
        g_string_append_c(command, ' '); g_string_append(command, quoted);
        g_free(quoted); g_free(assignment);
    }
    char *quoted = g_shell_quote(executable);
    g_string_append_printf(command, " %s", quoted); g_free(quoted);
    if (frontend && (!strcmp(frontend, "gtk") || !strcmp(frontend, "qt")))
        g_string_append_printf(command, " --ui %s", frontend);
    return g_string_free(command, FALSE);
}

int desktop_start_elevated(const char *executable, const char *frontend)
{
    char *command = desktop_elevation_command(executable, frontend);
    char *script_path = NULL;
    /* The installed phone verifier accepts at most 512 command characters.
     * Keep the full environment in a private, digest-bound launch script. */
    if (strlen(command) > 512) {
        script_path = g_strdup("/tmp/evemon-elevate-XXXXXX");
        int fd = g_mkstemp(script_path);
        if (fd < 0) { g_free(command); g_free(script_path); return -1; }
        char *quoted = g_shell_quote(script_path);
        char *script = g_strdup_printf("#!/bin/sh\n/bin/rm -f -- %s\nexec %s\n", quoted, command);
        size_t length = strlen(script), offset = 0;
        while (offset < length) {
            ssize_t n = write(fd, script + offset, length - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            offset += (size_t)n;
        }
        close(fd);
        if (offset != length) {
            unlink(script_path); g_free(script_path); g_free(quoted); g_free(script); g_free(command);
            return -1;
        }
        char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                     script, -1);
        char *quoted_digest = g_shell_quote(digest);
        g_free(command);
        /* The unprivileged caller owns script_path.  Copy it into a new
         * root-owned inode, verify that copy, then execute the same inode.
         * Checking one pathname and reopening it was a classic TOCTOU. */
        command = g_strdup_printf(
            "t=$(/usr/bin/mktemp /tmp/evemon-root.XXXXXX)||exit 1;"
            "/bin/cp -- %s \"$t\"&&"
            "printf '%%s  %%s\\n' %s \"$t\"|/usr/bin/sha256sum -c -&&"
            "/bin/sh \"$t\";s=$?;"
            "/bin/rm -f -- \"$t\" %s;exit $s",
            quoted, quoted_digest, quoted);
        g_free(quoted_digest); g_free(digest); g_free(quoted); g_free(script);
    }
    char *argv[] = {"/usr/bin/kdesu", "-n", "--noignorebutton", "-c", command, NULL};
    /* Preserve the original restart behavior: replace this session and all
     * its threads, rather than leave it running beside the new instance. */
    execv(argv[0], argv);
    int saved = errno;
    if (script_path) unlink(script_path);
    g_free(script_path);
    g_free(command);
    fprintf(stderr, "evemon: could not start /usr/bin/kdesu: %s\n", strerror(saved));
    errno = saved;
    return -1;
}

void desktop_settings_owner(const char *path)
{
    if (geteuid() != 0 || desktop_session_uid() == 0) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) &&
        (st.st_uid == 0 || st.st_uid == session_uid)) {
        if (fchown(fd, session_uid, session_gid))
            fprintf(stderr, "evemon: cannot preserve settings ownership: %s\n", strerror(errno));
    }
    close(fd);
}
