/*
 * steam.c – detect Steam / Proton metadata for a process.
 *
 * When a process belongs to a Steam game launch tree, its /proc/<pid>/environ
 * contains several telltale variables set by the Steam client and Proton:
 *
 *   SteamAppId              – the numeric app ID (e.g. "1422450" = Deadlock)
 *   STEAM_COMPAT_APP_ID     – same, set by pressure-vessel
 *   STEAM_COMPAT_DATA_PATH  – per-game Wine prefix (compatdata/<appid>)
 *   STEAM_COMPAT_TOOL_PATHS – colon-separated list of compat tool dirs
 *                             (first entry is usually the Proton directory)
 *   STEAM_COMPAT_CLIENT_INSTALL_PATH – Steam installation root
 *   STEAM_COMPAT_LIBRARY_PATHS       – additional Steam library folders
 *
 * We resolve the game name by reading the corresponding
 *   <steamapps>/appmanifest_<appid>.acf
 * file (Valve KeyValues format — we only need the "name" field).
 *
 * We resolve the Proton version by reading the "version" file in the
 * first STEAM_COMPAT_TOOL_PATHS entry.
 */

#include "steam.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>

/* ── helpers ─────────────────────────────────────────────────── */

/*
 * Read /proc/<pid>/environ in larger chunks for processes with big
 * environments.  Searches for multiple keys at once.
 */
typedef struct {
    const char *key;
    char       *buf;
    size_t      bufsz;
    int         found;
} env_query_t;

static void read_env_vars_bulk(pid_t pid, env_query_t *queries, int nqueries)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    /* Read up to 64 KiB of environment */
    size_t cap = 65536;
    char *data = malloc(cap);
    if (!data) { fclose(f); return; }

    size_t total = fread(data, 1, cap - 1, f);
    fclose(f);

    if (total == 0) { free(data); return; }
    data[total] = '\0';

    int remaining = nqueries;
    size_t pos = 0;
    while (pos < total && remaining > 0) {
        const char *entry = data + pos;
        size_t elen = strnlen(entry, total - pos);

        for (int i = 0; i < nqueries; i++) {
            if (queries[i].found)
                continue;
            size_t klen = strlen(queries[i].key);
            if (elen > klen && entry[klen] == '=' &&
                memcmp(entry, queries[i].key, klen) == 0) {
                const char *val = entry + klen + 1;
                snprintf(queries[i].buf, queries[i].bufsz, "%s", val);
                queries[i].found = 1;
                remaining--;
            }
        }
        pos += elen + 1;
    }

    free(data);
}

/*
 * Parse a Valve KeyValues ".acf" file to extract the "name" field.
 * This is a very minimal parser: looks for a line matching
 *   "name"    "<value>"
 */
static int parse_acf_name(const char *acf_path, char *name, size_t namesz)
{
    FILE *f = fopen(acf_path, "r");
    if (!f)
        return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Skip leading whitespace/tabs */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Match "name" (case insensitive) */
        if (*p != '"') continue;
        p++;

        if (strncasecmp(p, "name\"", 5) != 0)
            continue;
        p += 5;

        /* Skip whitespace between key and value */
        while (*p == ' ' || *p == '\t') p++;

        if (*p != '"') continue;
        p++;

        /* Extract value until closing quote */
        const char *end = strchr(p, '"');
        if (!end) continue;

        size_t vlen = (size_t)(end - p);
        if (vlen >= namesz) vlen = namesz - 1;
        memcpy(name, p, vlen);
        name[vlen] = '\0';
        fclose(f);
        return 1;
    }

    fclose(f);
    return 0;
}

/*
 * Read the Proton "version" file from a Proton dist directory.
 * The file typically contains a single line like:
 *   experimental-9.0-20250212
 */
static int read_proton_version(const char *proton_dir,
                               char *ver, size_t versz)
{
    char path[STEAM_PATH_MAX];
    snprintf(path, sizeof(path), "%s/version", proton_dir);

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    if (!fgets(ver, (int)versz, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    /* Strip trailing whitespace */
    size_t len = strlen(ver);
    while (len > 0 && (ver[len - 1] == '\n' || ver[len - 1] == '\r' ||
                       ver[len - 1] == ' '))
        ver[--len] = '\0';

    return (len > 0);
}

/*
 * Given a STEAM_COMPAT_TOOL_PATHS value (colon-separated), extract the
 * first entry (the Proton install dir) and try to read its version.
 * Also derives a human-friendly proton name from the path.
 */
static void resolve_proton_info(const char *tool_paths,
                                char *version, size_t versz,
                                char *dist_dir, size_t distsz)
{
    version[0] = '\0';
    dist_dir[0] = '\0';

    if (!tool_paths || !tool_paths[0])
        return;

    /* Extract first colon-delimited path */
    char first[STEAM_PATH_MAX];
    const char *colon = strchr(tool_paths, ':');
    size_t len = colon ? (size_t)(colon - tool_paths) : strlen(tool_paths);
    if (len >= sizeof(first)) len = sizeof(first) - 1;
    memcpy(first, tool_paths, len);
    first[len] = '\0';

    /* Remove trailing slash */
    while (len > 0 && first[len - 1] == '/')
        first[--len] = '\0';

    snprintf(dist_dir, distsz, "%s", first);

    /* Try to read version file */
    if (read_proton_version(first, version, versz))
        return;

    /* Fallback: derive version from directory name.
     * e.g. "/home/user/.steam/steam/steamapps/common/Proton - Experimental"
     *       → "Proton - Experimental" */
    const char *last_slash = strrchr(first, '/');
    if (last_slash)
        snprintf(version, versz, "%s", last_slash + 1);
}

/*
 * Resolve the game name from appmanifest files in Steam library folders.
 * Searches the standard locations:
 *   ~/.local/share/Steam/steamapps/
 *   ~/.steam/steam/steamapps/
 *   any library folder from STEAM_COMPAT_LIBRARY_PATHS
 */
static int resolve_game_name(const char *app_id, const char *compat_data_path,
                             char *name, size_t namesz,
                             char *game_dir, size_t game_dir_sz)
{
    name[0] = '\0';
    game_dir[0] = '\0';

    if (!app_id || !app_id[0])
        return 0;

    /* Build list of steamapps directories to search */
    const char *search_dirs[8];
    int ndirs = 0;

    /* Derive steamapps dir from STEAM_COMPAT_DATA_PATH if available.
     * e.g. /home/user/.local/share/Steam/steamapps/compatdata/1422450
     *    → /home/user/.local/share/Steam/steamapps/ */
    static char derived_dir[STEAM_PATH_MAX];
    if (compat_data_path && compat_data_path[0]) {
        snprintf(derived_dir, sizeof(derived_dir), "%s", compat_data_path);
        /* Walk up twice: strip /compatdata/<appid> */
        char *p = strrchr(derived_dir, '/');
        if (p) { *p = '\0'; p = strrchr(derived_dir, '/'); }
        if (p) { *p = '\0'; }
        search_dirs[ndirs++] = derived_dir;
    }

    /* Standard paths */
    static char std1[STEAM_PATH_MAX], std2[STEAM_PATH_MAX];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(std1, sizeof(std1), "%s/.local/share/Steam/steamapps", home);
        snprintf(std2, sizeof(std2), "%s/.steam/steam/steamapps", home);
        search_dirs[ndirs++] = std1;
        search_dirs[ndirs++] = std2;
    }

    /* Try each directory */
    char acf_path[STEAM_PATH_MAX];
    for (int i = 0; i < ndirs; i++) {
        snprintf(acf_path, sizeof(acf_path),
                 "%s/appmanifest_%s.acf", search_dirs[i], app_id);

        if (parse_acf_name(acf_path, name, namesz)) {
            /* Also resolve the game install directory from the same acf */
            char installdir[512] = "";
            /* Re-read the installdir field */
            FILE *f = fopen(acf_path, "r");
            if (f) {
                char line[1024];
                while (fgets(line, sizeof(line), f)) {
                    const char *p = line;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p != '"') continue;
                    p++;
                    if (strncasecmp(p, "installdir\"", 11) != 0) continue;
                    p += 11;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p != '"') continue;
                    p++;
                    const char *end = strchr(p, '"');
                    if (end) {
                        size_t vlen = (size_t)(end - p);
                        if (vlen >= sizeof(installdir))
                            vlen = sizeof(installdir) - 1;
                        memcpy(installdir, p, vlen);
                        installdir[vlen] = '\0';
                    }
                    break;
                }
                fclose(f);
            }
            if (installdir[0])
                snprintf(game_dir, game_dir_sz, "%s/common/%s",
                         search_dirs[i], installdir);
            return 1;
        }
    }

    return 0;
}

/*
 * Detect the Steam runtime layer from STEAM_COMPAT_TOOL_PATHS or
 * from known path patterns.  The runtime is identified by its codename:
 *   "sniper" (3.0), "soldier" (2.0), "scout" (1.0)
 */
static void detect_runtime_layer(const char *tool_paths,
                                 const char *compat_data_path,
                                 char *layer, size_t layersz)
{
    layer[0] = '\0';

    const char *haystack = tool_paths;
    if (!haystack) haystack = compat_data_path;
    if (!haystack) return;

    /* Check the full tool_paths string for runtime keywords */
    if (tool_paths) {
        if (strstr(tool_paths, "sniper"))
            snprintf(layer, layersz, "Steam Linux Runtime 3.0 (sniper)");
        else if (strstr(tool_paths, "soldier"))
            snprintf(layer, layersz, "Steam Linux Runtime 2.0 (soldier)");
        else if (strstr(tool_paths, "scout") ||
                 strstr(tool_paths, "SteamLinuxRuntime"))
            snprintf(layer, layersz, "Steam Linux Runtime 1.0 (scout)");
    }
}

/*
 * Build a human-friendly display label for the process tree.
 * e.g. "reaper (Steam Runtime) Running Deadlock [Proton Exp 9.0]"
 */
static void build_display_label(const char *comm, const steam_info_t *info,
                                char *label, size_t labelsz)
{
    /* Start with the process name */
    int off = 0;

    if (info->game_name[0]) {
        off += snprintf(label + off, labelsz - off,
                        "%s (Steam)", comm);
        off += snprintf(label + off, labelsz - off,
                        " · %s", info->game_name);
    } else if (info->app_id[0]) {
        off += snprintf(label + off, labelsz - off,
                        "%s (Steam AppID %s)", comm, info->app_id);
    } else {
        off += snprintf(label + off, labelsz - off,
                        "%s (Steam)", comm);
    }

    if (info->proton_version[0]) {
        /* Shorten the version for display */
        off += snprintf(label + off, labelsz - off,
                        " [%s]", info->proton_version);
    }

    (void)off;
}

/* ── process names that indicate a Steam game launch tree ────── */

static int is_steam_launcher_process(const char *comm)
{
    if (!comm) return 0;

    /* Exact matches for known Steam launcher binaries */
    static const char *names[] = {
        "reaper",
        "pressure-ves",     /* pressure-vessel-wrap truncated to 15 chars in comm */
        "steam-runtime",    /* steam-runtime-launcher-interface-0 */
        "proton",
        "pv-bwrap",
        "steam-launch-",    /* steam-launch-wrapper */
        NULL
    };

    for (int i = 0; names[i]; i++) {
        if (strcmp(comm, names[i]) == 0 ||
            strncmp(comm, names[i], strlen(names[i])) == 0)
            return 1;
    }

    return 0;
}

/* ── public API ──────────────────────────────────────────────── */

steam_info_t *steam_detect(pid_t pid, const char *comm, const char *cmdline,
                           const steam_info_t *parent_steam)
{
    /*
     * Fast-reject: kernel threads and early system processes (PID ≤ 10)
     * are never Steam processes.  Avoid opening /proc/<pid>/environ for
     * them at all — this is the most common case during the ancestor walk
     * for ordinary (non-Steam) processes selected in the tree.
     */
    if (pid <= 10)
        return NULL;

    /*
     * Strategy:
     *   1. If the process is a known Steam launcher name → probe its environ.
     *   2. If the parent was already identified as Steam → inherit metadata.
     *   3. Wine/Proton child processes inherit from parent too.
     */

    int should_probe = is_steam_launcher_process(comm);

    /* Check if cmdline contains steam/proton/pressure-vessel indicators */
    if (!should_probe && cmdline) {
        if (strstr(cmdline, "pressure-vessel") ||
            strstr(cmdline, "proton waitforexitandrun") ||
            strstr(cmdline, "proton run") ||
            strstr(cmdline, "steam-runtime-launcher-interface") ||
            strstr(cmdline, "SteamLinuxRuntime"))
            should_probe = 1;
    }

    /*
     * Fast-reject: if there's no parent Steam context AND this process
     * doesn't look like a Steam launcher, skip the /proc/environ read
     * entirely.  This avoids expensive I/O for every ancestor in the
     * tree walk when a non-Steam process is selected.
     */
    if (!should_probe && !parent_steam)
        return NULL;

    /* If we have Steam parent metadata, children inherit it */
    if (parent_steam && parent_steam->is_steam) {
        steam_info_t *out = malloc(sizeof(*out));
        if (!out) return NULL;
        memcpy(out, parent_steam, sizeof(*out));
        /* Rebuild the display label with this process's own name */
        build_display_label(comm, out, out->display_label,
                            sizeof(out->display_label));
        return out;
    }

    if (!should_probe)
        return NULL;

    /* ── Probe /proc/<pid>/environ for Steam variables ────────── */
    char app_id[32] = "";
    char compat_data[STEAM_PATH_MAX] = "";
    char tool_paths[STEAM_PATH_MAX] = "";
    char library_paths[STEAM_PATH_MAX] = "";

    env_query_t queries[] = {
        { "SteamAppId",                  app_id,        sizeof(app_id),        0 },
        { "STEAM_COMPAT_APP_ID",         app_id,        sizeof(app_id),        0 },
        { "STEAM_COMPAT_DATA_PATH",      compat_data,   sizeof(compat_data),   0 },
        { "STEAM_COMPAT_TOOL_PATHS",     tool_paths,    sizeof(tool_paths),    0 },
        { "STEAM_COMPAT_LIBRARY_PATHS",  library_paths, sizeof(library_paths), 0 },
    };
    int nq = (int)(sizeof(queries) / sizeof(queries[0]));

    read_env_vars_bulk(pid, queries, nq);

    /* If no Steam env vars found, this isn't a Steam process */
    if (!app_id[0] && !compat_data[0] && !tool_paths[0])
        return NULL;

    steam_info_t *out = calloc(1, sizeof(*out));
    if (!out) return NULL;

    out->is_steam = 1;
    snprintf(out->app_id, sizeof(out->app_id), "%s", app_id);
    snprintf(out->compat_data, sizeof(out->compat_data), "%s", compat_data);

    /* Resolve Proton version + dist path */
    resolve_proton_info(tool_paths,
                        out->proton_version, sizeof(out->proton_version),
                        out->proton_dist, sizeof(out->proton_dist));

    /* Resolve game name + install dir from appmanifest */
    resolve_game_name(app_id, compat_data,
                      out->game_name, sizeof(out->game_name),
                      out->game_dir, sizeof(out->game_dir));

    /* Detect runtime layer (sniper/soldier/scout) */
    detect_runtime_layer(tool_paths, compat_data,
                         out->runtime_layer, sizeof(out->runtime_layer));

    /* Build the display label */
    build_display_label(comm, out, out->display_label,
                        sizeof(out->display_label));

    return out;
}
