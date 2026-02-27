/*
 * settings.h – Persistent user settings for evemon.
 *
 * Settings are stored as a JSON file at:
 *   ~/.config/evemon/settings.json
 *
 * The settings module provides a global singleton that is loaded once
 * at startup and saved on each change.  All fields have sensible
 * defaults so a missing or empty file is perfectly valid.
 */

#ifndef EVEMON_SETTINGS_H
#define EVEMON_SETTINGS_H

#include <sys/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ──────────────────────────────────────────────────── */

#define SETTINGS_MAX_COLUMNS   32
#define SETTINGS_MAX_PLUGINS   64
#define SETTINGS_MAX_HOTKEYS   64
#define SETTINGS_COL_NAME_MAX  64
#define SETTINGS_PLUGIN_ID_MAX 128
#define SETTINGS_THEME_MAX     128
#define SETTINGS_HOTKEY_MAX    128

/* ── Hotkey mode ─────────────────────────────────────────────── */

typedef enum {
    HOTKEY_MODE_GTK   = 0,   /* Ctrl-based shortcuts (default)  */
    HOTKEY_MODE_MACOS = 1,   /* Meta/Super-based shortcuts      */
} hotkey_mode_t;

/* ── Plugin enable/disable entry ─────────────────────────────── */

typedef struct {
    char  id[SETTINGS_PLUGIN_ID_MAX];   /* reverse-DNS plugin id      */
    bool  enabled;                       /* false = skip at load time  */
} settings_plugin_t;

/* ── Custom hotkey binding ───────────────────────────────────── */

typedef struct {
    char  action[SETTINGS_HOTKEY_MAX];   /* e.g. "filter", "goto_pid" */
    char  binding[SETTINGS_HOTKEY_MAX];  /* e.g. "Ctrl+F", "Meta+G"  */
} settings_hotkey_t;

/* ── Top-level settings struct ───────────────────────────────── */

typedef struct {
    /* Appearance */
    char          theme[SETTINGS_THEME_MAX];     /* GTK theme name       */
    int           font_size;                      /* tree/sidebar pt size */

    /* Layout */
    bool          detail_panel_open;              /* detail panel visible */
    bool          proc_info_open;                 /* side tray expanded   */
    int           detail_panel_position;          /* 0=bottom,1=top,2=left,3=right */

    /* Behaviour */
    pid_t         preselected_pid;                /* PID to select on startup (0 = none) */
    bool          show_audio_only;               /* filter tree to audio processes */

    /* Columns: ordered list of column names; visibility is implicit
     * (present = visible).  Empty array = show all defaults. */
    char          columns[SETTINGS_MAX_COLUMNS][SETTINGS_COL_NAME_MAX];
    int           column_count;

    /* Plugins: which plugins to load or skip */
    settings_plugin_t plugins[SETTINGS_MAX_PLUGINS];
    int               plugin_count;

    /* Hotkeys */
    hotkey_mode_t     hotkey_mode;
    settings_hotkey_t custom_hotkeys[SETTINGS_MAX_HOTKEYS];
    int               custom_hotkey_count;
} evemon_settings_t;

/* ── API ─────────────────────────────────────────────────────── */

/*
 * Get the global settings singleton.  On the first call, loads from
 * ~/.config/evemon/settings.json (or returns defaults if the file
 * doesn't exist).  Thread-safe after initial call from main thread.
 */
evemon_settings_t *settings_get(void);

/*
 * Reload settings from disk, replacing the current in-memory state.
 * Returns 0 on success, -1 on parse error (keeps previous settings).
 */
int settings_load(void);

/*
 * Save current in-memory settings to disk.
 * Creates ~/.config/evemon/ if it doesn't exist.
 * Returns 0 on success, -1 on I/O error.
 */
int settings_save(void);

/*
 * Return the settings file path (for diagnostics / UI display).
 * The returned pointer is to a static buffer — do not free.
 */
const char *settings_path(void);

/*
 * Check if a plugin id is enabled in settings.
 * Returns true if the plugin is not listed (default = enabled)
 * or if it is listed with enabled=true.
 */
bool settings_plugin_enabled(const char *plugin_id);

#ifdef __cplusplus
}
#endif

#endif /* EVEMON_SETTINGS_H */
