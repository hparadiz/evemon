/*
 * steam.h – Steam / Proton / Wine process metadata detection.
 *
 * Detects when a process belongs to a Steam game launch tree (reaper,
 * pressure-vessel, proton, wine, etc.) and resolves:
 *   - Steam App ID
 *   - Game name (from appmanifest_<appid>.acf)
 *   - Proton version (from the compat tool's "version" file)
 *   - Proton dist directory
 *   - Steam compatdata path (per-game Wine prefix)
 *   - Game install directory
 *   - Steam Runtime layer (e.g. "sniper", "soldier", "scout")
 *
 * Also detects standalone Wine processes (Lutris, Bottles, raw wine
 * invocations) by checking comm/cmdline before touching /proc/environ.
 * These processes have is_wine=1, is_steam=0 and a best-effort
 * compat_data path read from the WINEPREFIX env var.
 */

#ifndef evemon_STEAM_H
#define evemon_STEAM_H

#include <sys/types.h>

#define STEAM_NAME_MAX      256
#define STEAM_PATH_MAX      1024
#define STEAM_VERSION_MAX   128

typedef struct {
    int     is_steam;                          /* non-zero if Steam-related         */
    int     is_wine;                           /* non-zero if Wine/Proton process   */
    char    app_id[32];                        /* SteamAppId / STEAM_COMPAT_APP_ID  */
    char    game_name[STEAM_NAME_MAX];         /* resolved from appmanifest         */
    char    proton_version[STEAM_VERSION_MAX]; /* e.g. "experimental-9.0-20250212"  */
    char    proton_dist[STEAM_PATH_MAX];       /* path to Proton dist/              */
    char    compat_data[STEAM_PATH_MAX];       /* STEAM_COMPAT_DATA_PATH/WINEPREFIX */
    char    game_dir[STEAM_PATH_MAX];          /* game install directory             */
    char    runtime_layer[128];                /* "sniper", "soldier", etc.          */
    char    display_label[STEAM_NAME_MAX];     /* pre-formatted display string       */
} steam_info_t;

/*
 * Check once whether Steam is installed on this system.
 *
 * Probes the standard Linux Steam locations:
 *   ~/.local/share/Steam/ubuntu12_32/steam  (native install, Flatpak)
 *   ~/.steam/steam/ubuntu12_32/steam        (legacy symlink tree)
 *   /usr/share/steam/ubuntu12_32/steam      (distro package)
 *
 * The result is cached after the first call; subsequent calls are O(1).
 * Returns 1 if Steam appears to be installed, 0 otherwise.
 *
 * When this returns 0 the entire Steam detection pass in the monitor
 * thread is skipped, saving the per-process /proc/<pid>/environ reads.
 */
int steam_is_available(void);

/*
 * Probe comm/cmdline (and if necessary /proc/<pid>/environ) to detect
 * Steam-related and standalone Wine processes.
 * Returns a heap-allocated steam_info_t if the process is Steam or Wine,
 * or NULL if it is not.  Caller takes ownership of the returned pointer.
 *
 * Detection is two-tier:
 *   Fast path – comm/cmdline heuristics (no I/O): identifies wine, wine64,
 *     wineserver, winedevice.exe, wineloader, wineboot.exe, explorer.exe
 *     and similar Wine process names without reading any files.
 *   Slow path – /proc/<pid>/environ: used for Steam launcher processes and
 *     for Wine processes to retrieve WINEPREFIX / Steam compat vars.
 *
 * `comm` is the process name (from /proc/<pid>/comm).
 * `cmdline` is the full command line (may be NULL).
 *
 * If `parent_steam` is non-NULL, the child inherits the parent's
 * metadata (children of reaper share the same Steam context).
 */
steam_info_t *steam_detect(pid_t pid, const char *comm, const char *cmdline,
                           const steam_info_t *parent_steam);

#endif /* evemon_STEAM_H */
