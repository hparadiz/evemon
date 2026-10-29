#include "ui_gtk_private.h"
#include "../platform/desktop_session.h"

static const char *desktop_open_extra_env[] = {
    "XDG_SESSION_TYPE",
    "GNOME_DESKTOP_SESSION_ID",
    "MATE_DESKTOP_SESSION_ID",
    "LXQT_SESSION_CONFIG",
    "DESKTOP",
    "BROWSER",
    "SWAYSOCK",
    "HYPRLAND_INSTANCE_SIGNATURE",
    NULL
};

typedef struct {
    gboolean initialized;
    gboolean usable;
    uid_t uid;
    gid_t gid;
    char *home;
    char *user;
    char *runtime_dir;
    char *bus_addr;
    char *display;
    char *wayland_display;
    char *xauthority;
    char *desktop;
    char *session;
    char *extra[8];
} desktop_open_context_t;

static desktop_open_context_t desktop_open_ctx = {0};

/* ── proc_meta event callback: update the Software section ────── */

static uid_t desktop_user_uid(void) { return desktop_session_uid(); }

static char *desktop_user_env(uid_t uid, const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return NULL;

    char needle[128];
    snprintf(needle, sizeof(needle), "%s=", name);
    size_t nlen = strlen(needle);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9')
            continue;

        char stat_path[512];
        snprintf(stat_path, sizeof(stat_path),
                 "/proc/%s/status", de->d_name);
        FILE *sf = fopen(stat_path, "r");
        if (!sf) continue;

        uid_t file_uid = (uid_t)-1;
        char line[256];
        while (fgets(line, sizeof(line), sf)) {
            unsigned ruid;
            if (sscanf(line, "Uid:\t%u", &ruid) == 1) {
                file_uid = (uid_t)ruid;
                break;
            }
        }
        fclose(sf);
        if (file_uid != uid)
            continue;

        char env_path[512];
        snprintf(env_path, sizeof(env_path),
                 "/proc/%s/environ", de->d_name);
        FILE *ef = fopen(env_path, "r");
        if (!ef) continue;

        char envbuf[65536];
        size_t n = fread(envbuf, 1, sizeof(envbuf) - 1, ef);
        fclose(ef);
        envbuf[n] = '\0';

        for (size_t i = 0; i < n; ) {
            const char *var = envbuf + i;
            size_t vlen = strlen(var);
            if (strncmp(var, needle, nlen) == 0) {
                char *value = g_strdup(var + nlen);
                closedir(d);
                return value;
            }
            i += vlen + 1;
        }
    }

    closedir(d);
    return NULL;
}

static char *dup_preferred_env(uid_t uid, const char *name,
                               gboolean proc_first)
{
    const char *current = getenv(name);
    char *from_proc = desktop_user_env(uid, name);
    const char *value = NULL;

    if (proc_first)
        value = (from_proc && from_proc[0]) ? from_proc : current;
    else
        value = (current && current[0]) ? current : from_proc;

    char *copy = (value && value[0]) ? g_strdup(value) : NULL;
    g_free(from_proc);
    return copy;
}

void gtk_ui_desktop_open_context_init_once(void)
{
    if (desktop_open_ctx.initialized)
        return;
    desktop_open_ctx.initialized = TRUE;

    uid_t uid = desktop_user_uid();
    if (uid == (uid_t)-1 || uid == 0)
        return;

    struct passwd *pw = getpwuid(uid);
    if (!pw)
        return;

    char runtime_dir[64];
    char fallback_bus_addr[96];
    snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%lu",
             (unsigned long)uid);
    snprintf(fallback_bus_addr, sizeof(fallback_bus_addr),
             "unix:path=%s/bus", runtime_dir);

    desktop_open_ctx.uid = uid;
    desktop_open_ctx.gid = pw->pw_gid;
    desktop_open_ctx.home = g_strdup(pw->pw_dir);
    desktop_open_ctx.user = g_strdup(pw->pw_name);
    desktop_open_ctx.runtime_dir = g_strdup(runtime_dir);
    desktop_open_ctx.bus_addr = NULL;
    char *bus_path = g_build_filename(runtime_dir, "bus", NULL);
    if (g_file_test(bus_path, G_FILE_TEST_EXISTS))
        desktop_open_ctx.bus_addr = g_strdup(fallback_bus_addr);
    g_free(bus_path);
    if (!desktop_open_ctx.bus_addr)
        desktop_open_ctx.bus_addr = dup_preferred_env(uid, "DBUS_SESSION_BUS_ADDRESS", TRUE);
    if (!desktop_open_ctx.bus_addr)
        desktop_open_ctx.bus_addr = g_strdup(fallback_bus_addr);

    gboolean proc_first = (geteuid() == 0);
    desktop_open_ctx.display = dup_preferred_env(uid, "DISPLAY", proc_first);
    desktop_open_ctx.wayland_display = dup_preferred_env(uid, "WAYLAND_DISPLAY", proc_first);
    desktop_open_ctx.xauthority = dup_preferred_env(uid, "XAUTHORITY", proc_first);
    desktop_open_ctx.desktop = dup_preferred_env(uid, "XDG_CURRENT_DESKTOP", proc_first);
    desktop_open_ctx.session = dup_preferred_env(uid, "DESKTOP_SESSION", proc_first);

    for (int i = 0; desktop_open_extra_env[i]; i++)
        desktop_open_ctx.extra[i] = dup_preferred_env(uid, desktop_open_extra_env[i], proc_first);

    if (!desktop_open_ctx.xauthority) {
        char *xa = g_build_filename(desktop_open_ctx.home, ".Xauthority", NULL);
        if (g_file_test(xa, G_FILE_TEST_EXISTS))
            desktop_open_ctx.xauthority = g_strdup(xa);
        g_free(xa);
    }

    desktop_open_ctx.usable = TRUE;
}

static void desktop_open_context_apply(void)
{
    desktop_open_context_t *c = &desktop_open_ctx;

    setenv("HOME", c->home, 1);
    setenv("USER", c->user, 1);
    setenv("LOGNAME", c->user, 1);
    setenv("XDG_RUNTIME_DIR", c->runtime_dir, 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", c->bus_addr, 1);
    if (c->display) setenv("DISPLAY", c->display, 1);
    if (c->wayland_display) setenv("WAYLAND_DISPLAY", c->wayland_display, 1);
    if (c->desktop) setenv("XDG_CURRENT_DESKTOP", c->desktop, 1);
    if (c->session) setenv("DESKTOP_SESSION", c->session, 1);
    if (c->xauthority) setenv("XAUTHORITY", c->xauthority, 1);
    for (int i = 0; desktop_open_extra_env[i]; i++) {
        if (c->extra[i])
            setenv(desktop_open_extra_env[i], c->extra[i], 1);
    }
}

static void on_metadata_opener_exit(GPid pid, gint status, gpointer data)
{
    (void)status;
    (void)data;
    g_spawn_close_pid(pid);
}

static gboolean try_portal_open_direct(const char *url)
{
    if (!desktop_open_ctx.bus_addr || !desktop_open_ctx.bus_addr[0])
        return FALSE;

    GError *err = NULL;
    GDBusConnection *conn = g_dbus_connection_new_for_address_sync(
        desktop_open_ctx.bus_addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    if (!conn) {
        g_clear_error(&err);
        return FALSE;
    }

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));

    GVariant *reply = g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.OpenURI",
        "OpenURI",
        g_variant_new("(ssa{sv})", "", url, &options),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        2500,
        NULL,
        &err);

    g_object_unref(conn);
    if (!reply) {
        g_clear_error(&err);
        return FALSE;
    }

    g_variant_unref(reply);
    return TRUE;
}

static gboolean try_default_uri_handler_direct(const char *url)
{
    GError *err = NULL;
    gboolean opened = g_app_info_launch_default_for_uri(url, NULL, &err);
    if (!opened)
        g_clear_error(&err);
    return opened;
}

static void close_open_command_console_fds(void)
{
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

static gboolean spawn_detached_open_command(const char *label, char **argv)
{
    int errpipe[2];
    if (pipe(errpipe) != 0)
        return FALSE;
    if (fcntl(errpipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        return FALSE;
    }

    (void)label;
    pid_t pid = fork();
    if (pid == 0) {
        close(errpipe[0]);
        close_open_command_console_fds();
        execvp(argv[0], argv);
        int saved_errno = errno;
        (void)write(errpipe[1], &saved_errno, sizeof(saved_errno));
        _exit(127);
    }

    close(errpipe[1]);
    if (pid < 0) {
        close(errpipe[0]);
        return FALSE;
    }

    int exec_errno = 0;
    ssize_t n = read(errpipe[0], &exec_errno, sizeof(exec_errno));
    close(errpipe[0]);

    if (n == 0)
        return TRUE;

    waitpid(pid, NULL, 0);
    return FALSE;
}

static char *browser_arg_with_url(const char *arg, const char *url,
                                  gboolean *used_placeholder)
{
    const char *placeholder = strstr(arg, "%s");
    if (!placeholder)
        return g_strdup(arg);

    if (used_placeholder)
        *used_placeholder = TRUE;

    GString *out = g_string_new_len(arg, placeholder - arg);
    g_string_append(out, url);
    g_string_append(out, placeholder + 2);
    return g_string_free(out, FALSE);
}

static gboolean try_browser_env_open(const char *url)
{
    const char *browser = getenv("BROWSER");
    if (!browser || !browser[0])
        return FALSE;

    char **choices = g_strsplit(browser, ":", 0);
    for (int i = 0; choices && choices[i]; i++) {
        char *choice = g_strstrip(choices[i]);
        if (!choice[0])
            continue;

        int argc = 0;
        char **parsed = NULL;
        GError *err = NULL;
        if (!g_shell_parse_argv(choice, &argc, &parsed, &err)) {
            g_clear_error(&err);
            continue;
        }

        gboolean used_placeholder = FALSE;
        char **argv = g_new0(char *, argc + 2);
        for (int j = 0; j < argc; j++)
            argv[j] = browser_arg_with_url(parsed[j], url,
                                           &used_placeholder);
        if (!used_placeholder)
            argv[argc] = g_strdup(url);

        g_strfreev(parsed);
        gboolean opened = spawn_detached_open_command("BROWSER", argv);
        g_strfreev(argv);
        if (opened) {
            g_strfreev(choices);
            return TRUE;
        }
    }

    g_strfreev(choices);
    return FALSE;
}

static gboolean try_known_browser_open(const char *url)
{
    const char *browsers[] = {
        "firefox",
        "librewolf",
        "floorp",
        "chromium",
        "google-chrome",
        "brave-browser",
        "vivaldi",
        "epiphany",
        "microsoft-edge",
        NULL
    };

    for (int i = 0; browsers[i]; i++) {
        char *argv[] = { (char *)browsers[i], (char *)url, NULL };
        if (spawn_detached_open_command(browsers[i], argv))
            return TRUE;
    }

    return FALSE;
}

static void open_url_as_desktop_user(const char *url)
{
    if (!url || !url[0])
        return;
    if (!desktop_open_ctx.usable)
        return;

    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0)
            g_child_watch_add((GPid)pid, on_metadata_opener_exit, NULL);
        return;
    }

    desktop_open_context_apply();

    if (geteuid() == 0) {
        if (initgroups(desktop_open_ctx.user, desktop_open_ctx.gid) != 0 ||
            setgid(desktop_open_ctx.gid) != 0 ||
            setuid(desktop_open_ctx.uid) != 0)
            _exit(127);
    }

    setsid();

    if (try_default_uri_handler_direct(url))
        _exit(0);

    if (try_portal_open_direct(url))
        _exit(0);

    if (try_browser_env_open(url))
        _exit(0);

    if (try_known_browser_open(url))
        _exit(0);

    _exit(127);
}

static gboolean open_metadata_label_url(GtkWidget *button,
                                         GtkLabel  *label,
                                         const char *source)
{
    static gint64 last_open_us;
    static char *last_open_url;

    const char *url = g_object_get_data(G_OBJECT(label), "evemon-url");
    if (!url || !url[0])
        return FALSE;

    gint64 now = g_get_monotonic_time();
    if (last_open_url && g_strcmp0(last_open_url, url) == 0 &&
        now - last_open_us < 250000)
        return TRUE;

    g_free(last_open_url);
    last_open_url = g_strdup(url);
    last_open_us = now;

    (void)button;
    (void)source;
    open_url_as_desktop_user(url);
    return TRUE;
}

static void on_metadata_link_clicked(GtkButton *button, gpointer data)
{
    open_metadata_label_url(GTK_WIDGET(button), GTK_LABEL(data), "clicked");
}

void gtk_ui_make_metadata_label_clickable(GtkWidget *button,
                                          GtkLabel  *label)
{
    static GtkCssProvider *plain_link_button_css;

    if (!plain_link_button_css) {
        plain_link_button_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(
            plain_link_button_css,
            ".evemon-metadata-link {"
            "  background: transparent;"
            "  border: none;"
            "  box-shadow: none;"
            "  padding: 0;"
            "  margin: 0;"
            "  min-height: 0;"
            "  color: inherit;"
            "}"
            ".evemon-metadata-link:hover,"
            ".evemon-metadata-link:active,"
            ".evemon-metadata-link:focus {"
            "  background: transparent;"
            "  border: none;"
            "  box-shadow: none;"
            "  color: inherit;"
            "}",
            -1, NULL);
    }

    gtk_label_set_selectable(label, FALSE);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(button), FALSE);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(button),
                                "evemon-metadata-link");
    gtk_style_context_add_provider(gtk_widget_get_style_context(button),
                                   GTK_STYLE_PROVIDER(plain_link_button_css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_signal_connect(button, "clicked",
                     G_CALLBACK(on_metadata_link_clicked), label);
}

static void set_link_label(GtkLabel *label, const char *url)
{
    if (!url || !url[0]) {
        gtk_label_set_text(label, "–");
        gtk_widget_set_tooltip_text(GTK_WIDGET(label), NULL);
        g_object_set_data(G_OBJECT(label), "evemon-url", NULL);
        return;
    }

    gtk_label_set_text(label, url);
    gtk_widget_set_tooltip_text(GTK_WIDGET(label), url);
    g_object_set_data_full(G_OBJECT(label), "evemon-url",
                           g_strdup(url), g_free);
}

static gboolean metadata_button_contains_event(GtkWidget *button,
                                               GdkEventButton *ev)
{
    if (!button || !gtk_widget_get_visible(button))
        return FALSE;

    gpointer user_data = NULL;
    GtkWidget *event_widget = NULL;
    if (ev->window)
        gdk_window_get_user_data(ev->window, &user_data);
    if (GTK_IS_WIDGET(user_data))
        event_widget = GTK_WIDGET(user_data);

    if (!event_widget)
        event_widget = gtk_get_event_widget((GdkEvent *)ev);
    if (!event_widget)
        return FALSE;

    gint x = 0, y = 0;
    if (!gtk_widget_translate_coordinates(event_widget, button,
                                          (gint)ev->x, (gint)ev->y,
                                          &x, &y))
        return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(button, &alloc);
    return x >= 0 && y >= 0 && x < alloc.width && y < alloc.height;
}

gboolean gtk_ui_metadata_link_from_main_click(ui_ctx_t *ctx,
                                              GdkEventButton *ev)
{
    if (!ctx || !ctx->sb_meta_frame ||
        !gtk_widget_get_visible(ctx->sb_meta_frame) ||
        ev->type != GDK_BUTTON_PRESS || ev->button != 1)
        return FALSE;

    struct {
        GtkWidget *button;
        GtkLabel  *label;
    } links[] = {
        { ctx->sb_meta_homepage_button,    ctx->sb_meta_homepage },
        { ctx->sb_meta_source_url_button,  ctx->sb_meta_source_url },
        { ctx->sb_meta_funding_url_button, ctx->sb_meta_funding_url },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(links); i++) {
        if (!metadata_button_contains_event(links[i].button, ev))
            continue;

        return open_metadata_label_url(links[i].button, links[i].label,
                                       "main-click");
    }

    return FALSE;
}

void gtk_ui_on_main_client_event_after(GtkWidget *widget,
                                       GdkEvent  *event,
                                       gpointer   data)
{
    (void)widget;
    if (event->type == GDK_BUTTON_PRESS)
        gtk_ui_metadata_link_from_main_click(data, (GdkEventButton *)event);
}

void gtk_ui_on_proc_meta(const evemon_event_t *ev, void *ud)
{
    ui_ctx_t *ctx = ud;
    const evemon_proc_meta_t *m = ev->payload;
    if (!m || !ctx->sb_meta_frame) return;
    if (!m->matched) {
        gtk_widget_hide(ctx->sb_meta_frame);
        return;
    }
#define LBL(field, src) gtk_label_set_text(ctx->field, (src)[0] ? (src) : "–")
    LBL(sb_meta_organization, m->organization);
    LBL(sb_meta_license,      m->primary_license);
    LBL(sb_meta_summary,      m->summary);
#undef LBL
    set_link_label(ctx->sb_meta_homepage,    m->homepage);
    set_link_label(ctx->sb_meta_source_url,  m->source_url);
    set_link_label(ctx->sb_meta_funding_url, m->funding_url);
    gtk_widget_set_no_show_all(ctx->sb_meta_frame, FALSE);
    gtk_widget_show_all(ctx->sb_meta_frame);
    gtk_widget_set_no_show_all(ctx->sb_meta_frame, TRUE);
}

/* ── menu bar actions ─────────────────────────────────────────── */

void gtk_ui_on_menu_exit(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    gtk_main_quit();
}

void gtk_ui_on_menu_restart(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    evemon_request_restart();
    gtk_main_quit();
}

static int find_desktop_bin(const char *name, char *out, size_t outsz)
{
    const char *dirs[] = {
        "/usr/bin", "/usr/local/bin",
        "/usr/libexec/kf6", "/usr/libexec/kf5",
        "/usr/lib64/libexec/kf6", "/usr/lib64/libexec/kf5",
        "/usr/local/libexec/kf6", "/usr/local/libexec/kf5",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        snprintf(out, outsz, "%s/%s", dirs[i], name);
        if (access(out, X_OK) == 0)
            return 1;
    }
    return 0;
}

/*
 * Plasma can be restarted with stale WAYLAND_DISPLAY/QT_QPA_PLATFORM
 * values, which its children then inherit.  GTK has already connected to
 * the display successfully, so use that live connection as the authority
 * for the elevater and the restarted process.
 */
static void repair_elevation_environment(void)
{
    if (desktop_open_ctx.usable && desktop_open_ctx.uid == getuid()) {
        setenv("XDG_RUNTIME_DIR", desktop_open_ctx.runtime_dir, 1);
        setenv("DBUS_SESSION_BUS_ADDRESS", desktop_open_ctx.bus_addr, 1);
    }

    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;

    const char *name = gdk_display_get_name(display);
    if (!name || !name[0])
        return;

#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(display)) {
        setenv("WAYLAND_DISPLAY", name, 1);
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);
        return;
    }
#endif
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(display)) {
        setenv("DISPLAY", name, 1);
        setenv("GDK_BACKEND", "x11", 1);
        setenv("QT_QPA_PLATFORM", "xcb", 1);
        if (desktop_open_ctx.xauthority)
            setenv("XAUTHORITY", desktop_open_ctx.xauthority, 1);
    }
#endif
}

void gtk_ui_on_menu_restart_as_admin(GtkMenuItem *item, gpointer data)
{
    (void)item;
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0) return;
    executable[length] = 0;
    repair_elevation_environment();
    if (desktop_start_elevated(executable, "gtk")) {
        GtkWidget *error = gtk_message_dialog_new(GTK_WINDOW(data), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Could not start /usr/bin/kdesu.");
        gtk_dialog_run(GTK_DIALOG(error)); gtk_widget_destroy(error);
    }
}
