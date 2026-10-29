/*
 * settings.c – Load/save evemon user settings as JSON.
 *
 * File location: ~/.config/evemon/settings.json
 *
 * Uses jansson for JSON parsing and serialisation.  The settings
 * file is human-editable (pretty-printed) and forward-compatible:
 * unknown keys are silently ignored on load, and missing keys use
 * built-in defaults.
 *
 * Example settings.json:
 *
 *   {
 *     "theme": "Adwaita-dark",
 *     "font_size": 9,
 *     "detail_panel_open": false,
 *     "proc_info_open": true,
 *     "detail_panel_position": 0,
 *     "preselected_pid": 1,
 *     "columns": ["PID", "Name", "CPU%", "Memory (RSS)", "Command"],
 *     "plugins": [
 *       { "id": "org.evemon.env",     "enabled": true  },
 *       { "id": "org.evemon.milkdrop","enabled": false }
 *     ],
 *     "hotkey_mode": "gtk",
 *     "custom_hotkeys": [
 *       { "action": "filter",   "binding": "Ctrl+F" },
 *       { "action": "goto_pid", "binding": "Ctrl+G" }
 *     ]
 *   }
 */

#include "settings.h"
#include "../platform/desktop_session.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>

/* ── Singleton ───────────────────────────────────────────────── */

static evemon_settings_t g_settings;
static int               g_settings_loaded = 0;
static char              g_settings_path[1024];
static char              g_settings_host[256];

static void read_process_view(evemon_settings_t *s, json_t *root)
{
    json_t *v = json_object_get(root, "preselected_pid");
    if (json_is_integer(v) && json_integer_value(v) >= 0)
        s->preselected_pid = (pid_t)json_integer_value(v);
    if (json_is_boolean(v = json_object_get(root, "show_audio_only")))
        s->show_audio_only = json_boolean_value(v);
    if (json_is_boolean(v = json_object_get(root, "show_cpu_intensive")))
        s->show_cpu_intensive = json_boolean_value(v);
    if (json_is_boolean(v = json_object_get(root, "show_gpu_only")))
        s->show_gpu_only = json_boolean_value(v);
    if (s->show_gpu_only) {
        s->show_cpu_intensive = false;
        s->show_audio_only = false;
    } else if (s->show_cpu_intensive) s->show_audio_only = false;
}

/* ── Defaults ────────────────────────────────────────────────── */

static void settings_set_defaults(evemon_settings_t *s)
{
    memset(s, 0, sizeof(*s));

    s->theme[0]              = '\0';     /* empty = use system theme    */
    s->font_size             = 9;
    s->detail_panel_open     = false;
    s->proc_info_open        = true;
    s->detail_panel_position = 0;        /* bottom                      */
    s->preselected_pid       = 1;        /* init                        */
    s->show_audio_only       = false;
    s->show_cpu_intensive    = false;
    s->show_gpu_only         = false;
    s->system_panel_open        = false;
    s->system_panel_position    = 0;        /* bottom                      */
    s->column_count          = 0;        /* empty = show all defaults   */
    s->plugin_count          = 0;        /* empty = load all            */
    s->hotkey_mode           = HOTKEY_MODE_GTK;
    s->custom_hotkey_count   = 0;
}

/* ── Path helpers ────────────────────────────────────────────── */

static const char *get_config_dir(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        return xdg;

    /* Fallback: ~/.config */
    static char buf[512];
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    snprintf(buf, sizeof(buf), "%s/.config", home);
    return buf;
}

static void build_settings_path(void)
{
    if (g_settings_path[0]) return;
    snprintf(g_settings_path, sizeof(g_settings_path),
             "%s/evemon/settings.json", get_config_dir());
}

const char *settings_path(void)
{
    build_settings_path();
    return g_settings_path;
}

/* ── Load ────────────────────────────────────────────────────── */

static int settings_load_from_file(evemon_settings_t *s, const char *path)
{
    settings_set_defaults(s);

    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        /* File doesn't exist → not an error, just use defaults */
        if (errno == ENOENT)
            return 0;
        fprintf(stderr, "evemon: settings parse error: %s (line %d)\n",
                err.text, err.line);
        return -1;
    }

    if (!json_is_object(root)) {
        json_decref(root);
        return -1;
    }

    /* ── Scalars ─────────────────────────────────────────────── */

    json_t *v;

    if ((v = json_object_get(root, "frontend")) && json_is_string(v))
        snprintf(s->frontend, sizeof(s->frontend), "%s", json_string_value(v));

    if ((v = json_object_get(root, "theme")) && json_is_string(v))
        snprintf(s->theme, sizeof(s->theme), "%s", json_string_value(v));

    if ((v = json_object_get(root, "font_size")) && json_is_integer(v)) {
        int fs = (int)json_integer_value(v);
        if (fs >= 6 && fs <= 30) s->font_size = fs;
    }

    if ((v = json_object_get(root, "detail_panel_open")) && json_is_boolean(v))
        s->detail_panel_open = json_boolean_value(v);

    if ((v = json_object_get(root, "proc_info_open")) && json_is_boolean(v))
        s->proc_info_open = json_boolean_value(v);

    if ((v = json_object_get(root, "detail_panel_position")) && json_is_integer(v)) {
        int pos = (int)json_integer_value(v);
        if (pos >= 0 && pos <= 3) s->detail_panel_position = pos;
    }

    read_process_view(s, root);
    if (g_settings_host[0]) {
        s->preselected_pid = 1;
        s->show_audio_only = s->show_cpu_intensive = s->show_gpu_only = false;
        read_process_view(s, json_object_get(json_object_get(root, "hosts"), g_settings_host));
    }

    if ((v = json_object_get(root, "spectro_theme")) && json_is_integer(v)) {
        int t = (int)json_integer_value(v);
        if (t >= 0 && t < 16) s->spectro_theme = t;
    }

    if ((v = json_object_get(root, "system_panel_open")) && json_is_boolean(v))
        s->system_panel_open = json_boolean_value(v);

    if ((v = json_object_get(root, "system_panel_position")) && json_is_integer(v)) {
        int pos = (int)json_integer_value(v);
        if (pos >= 0 && pos <= 3) s->system_panel_position = pos;
    }

    /* ── Columns ─────────────────────────────────────────────── */

    json_t *cols = json_object_get(root, "columns");
    if (cols && json_is_array(cols)) {
        size_t n = json_array_size(cols);
        if (n > SETTINGS_MAX_COLUMNS) n = SETTINGS_MAX_COLUMNS;
        s->column_count = 0;
        for (size_t i = 0; i < n; i++) {
            json_t *c = json_array_get(cols, i);
            if (json_is_string(c)) {
                snprintf(s->columns[s->column_count],
                         SETTINGS_COL_NAME_MAX, "%s",
                         json_string_value(c));
                s->column_count++;
            }
        }
    }

    /* ── Plugins ─────────────────────────────────────────────── */

    json_t *plugins = json_object_get(root, "plugins");
    if (plugins && json_is_array(plugins)) {
        size_t n = json_array_size(plugins);
        if (n > SETTINGS_MAX_PLUGINS) n = SETTINGS_MAX_PLUGINS;
        s->plugin_count = 0;
        for (size_t i = 0; i < n; i++) {
            json_t *p = json_array_get(plugins, i);
            if (!json_is_object(p)) continue;
            json_t *id = json_object_get(p, "id");
            json_t *en = json_object_get(p, "enabled");
            if (!id || !json_is_string(id)) continue;
            snprintf(s->plugins[s->plugin_count].id,
                     SETTINGS_PLUGIN_ID_MAX, "%s",
                     json_string_value(id));
            s->plugins[s->plugin_count].enabled =
                (en && json_is_boolean(en)) ? json_boolean_value(en) : true;
            s->plugin_count++;
        }
    }

    /* ── Hotkey mode ─────────────────────────────────────────── */

    if ((v = json_object_get(root, "hotkey_mode")) && json_is_string(v)) {
        const char *mode = json_string_value(v);
        if (strcmp(mode, "macos") == 0)
            s->hotkey_mode = HOTKEY_MODE_MACOS;
        else
            s->hotkey_mode = HOTKEY_MODE_GTK;
    }

    /* ── Custom hotkeys ──────────────────────────────────────── */

    json_t *hkeys = json_object_get(root, "custom_hotkeys");
    if (hkeys && json_is_array(hkeys)) {
        size_t n = json_array_size(hkeys);
        if (n > SETTINGS_MAX_HOTKEYS) n = SETTINGS_MAX_HOTKEYS;
        s->custom_hotkey_count = 0;
        for (size_t i = 0; i < n; i++) {
            json_t *h = json_array_get(hkeys, i);
            if (!json_is_object(h)) continue;
            json_t *act = json_object_get(h, "action");
            json_t *bnd = json_object_get(h, "binding");
            if (!act || !json_is_string(act)) continue;
            if (!bnd || !json_is_string(bnd)) continue;
            snprintf(s->custom_hotkeys[s->custom_hotkey_count].action,
                     SETTINGS_HOTKEY_MAX, "%s", json_string_value(act));
            snprintf(s->custom_hotkeys[s->custom_hotkey_count].binding,
                     SETTINGS_HOTKEY_MAX, "%s", json_string_value(bnd));
            s->custom_hotkey_count++;
        }
    }

    json_decref(root);
    return 0;
}

int settings_load(void)
{
    build_settings_path();
    /* Repair settings left root-owned by older elevated versions as well as
     * preserving ownership on new writes. Never follows a file symlink. */
    desktop_settings_owner(g_settings_path);
    return settings_load_from_file(&g_settings, g_settings_path);
}

int settings_select_host(const char *hostname)
{
    snprintf(g_settings_host, sizeof(g_settings_host), "%s", hostname ? hostname : "");
    int rc = settings_load();
    g_settings_loaded = 1;
    return rc;
}

/* ── Save ────────────────────────────────────────────────────── */

int settings_save(void)
{
    build_settings_path();

    /* Ensure ~/.config/evemon/ exists */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/evemon", get_config_dir());
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "evemon: cannot create %s: %s\n",
                dir, strerror(errno));
        return -1;
    }

    desktop_settings_owner(dir);

    const evemon_settings_t *s = &g_settings;

    json_t *root = json_object();
    if (!root) return -1;

    /* ── Scalars ─────────────────────────────────────────────── */

    if (s->frontend[0])
        json_object_set_new(root, "frontend", json_string(s->frontend));

    if (s->theme[0])
        json_object_set_new(root, "theme", json_string(s->theme));

    json_object_set_new(root, "font_size",
                        json_integer(s->font_size));
    json_object_set_new(root, "detail_panel_open",
                        json_boolean(s->detail_panel_open));
    json_object_set_new(root, "proc_info_open",
                        json_boolean(s->proc_info_open));
    json_object_set_new(root, "detail_panel_position",
                        json_integer(s->detail_panel_position));
    json_object_set_new(root, "preselected_pid",
                        json_integer((json_int_t)s->preselected_pid));
    json_object_set_new(root, "show_audio_only",
                        json_boolean(s->show_audio_only));
    json_object_set_new(root, "show_cpu_intensive",
                        json_boolean(s->show_cpu_intensive));
    json_object_set_new(root, "show_gpu_only",
                        json_boolean(s->show_gpu_only));
    /* Merge host views from disk so saving either frontend keeps the other
     * hosts and the local process filters intact. */
    json_t *previous = json_load_file(g_settings_path, 0, NULL);
    json_t *hosts = json_deep_copy(json_object_get(previous, "hosts"));
    if (!json_is_object(hosts)) { json_decref(hosts); hosts = json_object(); }
    if (g_settings_host[0]) {
        static const char *keys[] = {"preselected_pid", "show_audio_only", "show_cpu_intensive", "show_gpu_only"};
        json_t *view = json_object();
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            json_object_set(view, keys[i], json_object_get(root, keys[i]));
            json_t *local = json_object_get(previous, keys[i]);
            if (local) json_object_set(root, keys[i], local);
            else json_object_set_new(root, keys[i], i ? json_false() : json_integer(1));
        }
        json_object_set_new(hosts, g_settings_host, view);
    }
    if (json_object_size(hosts)) json_object_set(root, "hosts", hosts);
    json_decref(hosts); json_decref(previous);
    json_object_set_new(root, "spectro_theme",
                        json_integer(s->spectro_theme));
    json_object_set_new(root, "system_panel_open",
                        json_boolean(s->system_panel_open));
    json_object_set_new(root, "system_panel_position",
                        json_integer(s->system_panel_position));

    /* ── Columns ─────────────────────────────────────────────── */

    if (s->column_count > 0) {
        json_t *cols = json_array();
        for (int i = 0; i < s->column_count; i++)
            json_array_append_new(cols, json_string(s->columns[i]));
        json_object_set_new(root, "columns", cols);
    }

    /* ── Plugins ─────────────────────────────────────────────── */

    if (s->plugin_count > 0) {
        json_t *plugins = json_array();
        for (int i = 0; i < s->plugin_count; i++) {
            json_t *p = json_pack("{s:s, s:b}",
                "id",      s->plugins[i].id,
                "enabled", s->plugins[i].enabled);
            if (p) json_array_append_new(plugins, p);
        }
        json_object_set_new(root, "plugins", plugins);
    }

    /* ── Hotkey mode ─────────────────────────────────────────── */

    json_object_set_new(root, "hotkey_mode",
        json_string(s->hotkey_mode == HOTKEY_MODE_MACOS ? "macos" : "gtk"));

    /* ── Custom hotkeys ──────────────────────────────────────── */

    if (s->custom_hotkey_count > 0) {
        json_t *hkeys = json_array();
        for (int i = 0; i < s->custom_hotkey_count; i++) {
            json_t *h = json_pack("{s:s, s:s}",
                "action",  s->custom_hotkeys[i].action,
                "binding", s->custom_hotkeys[i].binding);
            if (h) json_array_append_new(hkeys, h);
        }
        json_object_set_new(root, "custom_hotkeys", hkeys);
    }

    /* Atomic replacement keeps restart from observing a partial settings file. */
    char temporary[sizeof(g_settings_path) + 16];
    snprintf(temporary, sizeof(temporary), "%s.XXXXXX", g_settings_path);
    int fd = mkstemp(temporary);
    int rc = -1;
    if (fd >= 0) {
        rc = 0;
        if (geteuid() == 0 && desktop_session_uid() != 0)
            rc = fchown(fd, desktop_session_uid(), desktop_session_gid());
        if (rc == 0) rc = json_dumpfd(root, fd, JSON_INDENT(2) | JSON_SORT_KEYS);
        if (rc == 0 && fsync(fd)) rc = -1;
        if (close(fd)) rc = -1;
        if (rc == 0 && rename(temporary, g_settings_path)) rc = -1;
        if (rc != 0) unlink(temporary);
    }
    json_decref(root);
    if (rc != 0) {
        fprintf(stderr, "evemon: failed to write %s\n", g_settings_path);
        return -1;
    }

    return 0;
}

/* ── Singleton accessor ──────────────────────────────────────── */

evemon_settings_t *settings_get(void)
{
    if (!g_settings_loaded) {
        settings_load();
        g_settings_loaded = 1;
    }
    return &g_settings;
}

/* ── Re-exported symbols for plugin use ─────────────────────── */

/*
 * Thin wrappers so plugins loaded via dlopen (with RTLD_GLOBAL / -rdynamic)
 * can access the settings singleton without depending on settings.h types.
 * Plugins that include settings.h get the real typed API; these symbol
 * names are declared as void* / int in evemon_plugin.h for ABI-only callers.
 */
void *evemon_settings_get(void) { return settings_get(); }
int   evemon_settings_save(void) { return settings_save(); }

/* ── Plugin query ────────────────────────────────────────────── */

bool settings_plugin_enabled(const char *plugin_id)
{
    if (!plugin_id) return true;
    const evemon_settings_t *s = settings_get();
    for (int i = 0; i < s->plugin_count; i++) {
        if (strcmp(s->plugins[i].id, plugin_id) == 0)
            return s->plugins[i].enabled;
    }
    /* Not listed → enabled by default */
    return true;
}

void settings_plugin_set_enabled(const char *plugin_id, bool enabled)
{
    if (!plugin_id) return;
    evemon_settings_t *s = settings_get();

    /* Update existing entry if present */
    for (int i = 0; i < s->plugin_count; i++) {
        if (strcmp(s->plugins[i].id, plugin_id) == 0) {
            s->plugins[i].enabled = enabled;
            settings_save();
            return;
        }
    }

    /* Add new entry if there's room */
    if (s->plugin_count < SETTINGS_MAX_PLUGINS) {
        snprintf(s->plugins[s->plugin_count].id,
                 SETTINGS_PLUGIN_ID_MAX, "%s", plugin_id);
        s->plugins[s->plugin_count].enabled = enabled;
        s->plugin_count++;
        settings_save();
    }
}
