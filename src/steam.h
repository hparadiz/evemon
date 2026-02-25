/*
 * steam.h – Steam / Proton process metadata detection.
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
 */

#ifndef evemon_STEAM_H
#define evemon_STEAM_H

#include <sys/types.h>

#define STEAM_NAME_MAX      256
#define STEAM_PATH_MAX      1024
#define STEAM_VERSION_MAX   128

typedef struct {
    int     is_steam;                          /* non-zero if Steam-related         */
    char    app_id[32];                        /* SteamAppId / STEAM_COMPAT_APP_ID  */
    char    game_name[STEAM_NAME_MAX];         /* resolved from appmanifest         */
    char    proton_version[STEAM_VERSION_MAX]; /* e.g. "experimental-9.0-20250212"  */
    char    proton_dist[STEAM_PATH_MAX];       /* path to Proton dist/              */
    char    compat_data[STEAM_PATH_MAX];       /* STEAM_COMPAT_DATA_PATH            */
    char    game_dir[STEAM_PATH_MAX];          /* game install directory             */
    char    runtime_layer[128];                /* "sniper", "soldier", etc.          */
    char    display_label[STEAM_NAME_MAX];     /* pre-formatted display string       */
} steam_info_t;

/*
 * Probe /proc/<pid>/environ for Steam-related environment variables.
 * Returns a heap-allocated steam_info_t if the process is Steam-related,
 * or NULL if it is not.  Caller takes ownership of the returned pointer.
 *
 * `comm` is the process name (from /proc/<pid>/comm) used to decide
 * whether to probe at all (avoids reading environ for every process).
 *
 * If `parent_steam` is non-NULL, the child inherits the parent's
 * metadata (children of reaper share the same Steam context).
 */
steam_info_t *steam_detect(pid_t pid, const char *comm, const char *cmdline,
                           const steam_info_t *parent_steam);

#endif /* evemon_STEAM_H */
