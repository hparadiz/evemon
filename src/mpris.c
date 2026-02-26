/*
 * mpris.c – MPRIS2 media metadata provider for evemon.
 *
 * Connects to the D-Bus session bus, lists all org.mpris.MediaPlayer2.*
 * bus names, resolves each to a PID via org.freedesktop.DBus.GetConnectionUnixProcessID,
 * then reads the org.mpris.MediaPlayer2.Player properties (PlaybackStatus,
 * Metadata, Volume, Position, etc.) for players matching the target PID.
 *
 * Uses GDBus synchronous calls — safe from the broker worker thread.
 * Every call is bounded by short timeouts to avoid blocking if a
 * player is hung.
 *
 * Graceful failure:
 *   - D-Bus unavailable → returns -1 (empty results).
 *   - Player doesn't respond → skipped silently.
 *   - Missing properties → fields left empty / -1.
 *   - Running as root → attempts to find user session bus.
 */

#include "mpris.h"

/* from main.c */
extern int evemon_debug;

#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>

/* D-Bus call timeout in milliseconds — short to avoid blocking */
#define MPRIS_DBUS_TIMEOUT_MS  500

/* ── session bus connection helper ───────────────────────────── */

/*
 * When running as root (sudo, pkexec, kdesu), the session bus address
 * points at root's bus or is unset.  We need to find the real user's
 * session bus.
 */
static GDBusConnection *mpris_connect_session_bus(void)
{
    GError *err = NULL;
    GDBusConnection *conn = NULL;
    int is_root = (getuid() == 0);

    /*
     * Detect the real user UID when running as root.
     * sudo sets SUDO_UID, polkit/pkexec sets PKEXEC_UID, but KDE's
     * kdesu/kdesudo sets neither.  We check multiple sources:
     *   1. SUDO_UID
     *   2. PKEXEC_UID
     *   3. Scan /run/user/ for a non-root UID directory
     *   4. loginctl / XDG_SESSION_ID owner
     */
    const char *uid_str = NULL;
    uid_t target_uid = 0;

    if (is_root) {
        uid_str = getenv("SUDO_UID");
        if (!uid_str) uid_str = getenv("PKEXEC_UID");
        if (uid_str)
            target_uid = (uid_t)atoi(uid_str);
    }

    /* If root but no UID env var (kdesu, kdesudo, etc.),
     * find a real user by scanning /run/user/ */
    if (is_root && target_uid == 0) {
        DIR *d = opendir("/run/user");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.' || strcmp(de->d_name, "0") == 0)
                    continue;
                uid_t u = (uid_t)atoi(de->d_name);
                if (u > 0) {
                    target_uid = u;
                    break;
                }
            }
            closedir(d);
        }
    }

    /*
     * When running as root, g_bus_get_sync() returns root's own
     * session bus which has no user MPRIS players.
     * Skip it and go straight to the real user's bus.
     */
    if (!is_root) {
        conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
        if (conn) {
            if (evemon_debug)
                fprintf(stderr, "[MPRIS DEBUG] using default session bus\n");
            return conn;
        }
        g_clear_error(&err);
        return NULL;
    }

    /* We're root — need to find the user's bus */
    if (target_uid == 0)
        return NULL;   /* couldn't determine any real user */

    char bus_addr[512] = {0};
    char uid_buf[32];
    snprintf(uid_buf, sizeof(uid_buf), "%u", (unsigned)target_uid);
    uid_str = uid_buf;

    /* Strategy 1: try /run/user/<uid>/bus */
    if (uid_str) {
        char probe[256];
        snprintf(probe, sizeof(probe), "/run/user/%s/bus", uid_str);
        if (access(probe, F_OK) == 0) {
            snprintf(bus_addr, sizeof(bus_addr),
                     "unix:path=%s", probe);
        }
    }

    /* Strategy 2: scan /run/user/ for a non-root bus socket */
    if (!bus_addr[0]) {
        DIR *d = opendir("/run/user");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.' || strcmp(de->d_name, "0") == 0)
                    continue;
                char probe[256];
                snprintf(probe, sizeof(probe),
                         "/run/user/%s/bus", de->d_name);
                if (access(probe, F_OK) == 0) {
                    snprintf(bus_addr, sizeof(bus_addr),
                             "unix:path=%s", probe);
                    if (!target_uid)
                        target_uid = (uid_t)atoi(de->d_name);
                    break;
                }
            }
            closedir(d);
        }
    }

    /* Strategy 3: read DBUS_SESSION_BUS_ADDRESS from a process owned
     * by the target user.  KDE Plasma (and some other DEs) use a
     * /tmp/dbus-XXXX socket instead of /run/user/<uid>/bus. */
    if (!bus_addr[0] && target_uid > 0) {
        DIR *d = opendir("/proc");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] < '1' || de->d_name[0] > '9')
                    continue;

                /* Check owner via /proc/<pid>/status */
                char stat_path[128];
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
                if (file_uid != target_uid)
                    continue;

                /* Read environ for DBUS_SESSION_BUS_ADDRESS */
                char env_path[128];
                snprintf(env_path, sizeof(env_path),
                         "/proc/%s/environ", de->d_name);
                FILE *ef = fopen(env_path, "r");
                if (!ef) continue;

                /* environ is NUL-separated */
                char envbuf[8192];
                size_t n = fread(envbuf, 1, sizeof(envbuf) - 1, ef);
                fclose(ef);
                envbuf[n] = '\0';

                const char *needle = "DBUS_SESSION_BUS_ADDRESS=";
                size_t nlen = strlen(needle);
                for (size_t i = 0; i < n; ) {
                    const char *var = envbuf + i;
                    size_t vlen = strlen(var);
                    if (strncmp(var, needle, nlen) == 0) {
                        snprintf(bus_addr, sizeof(bus_addr),
                                 "%s", var + nlen);
                        break;
                    }
                    i += vlen + 1;
                }
                if (bus_addr[0]) break;
            }
            closedir(d);
        }
    }

    if (!bus_addr[0])
        return NULL;

    if (evemon_debug)
        fprintf(stderr, "[MPRIS DEBUG] connecting to bus: %s (as uid %u)\n",
                bus_addr, (unsigned)target_uid);

    /*
     * D-Bus EXTERNAL auth checks the Unix socket peer credentials.
     * When we're root connecting to a user bus, auth fails because
     * UID 0 ≠ UID 1000.  Temporarily drop effective UID to the
     * target user so the EXTERNAL handshake succeeds.
     */
    uid_t saved_euid = geteuid();
    if (target_uid > 0 && saved_euid == 0)
        seteuid(target_uid);

    conn = g_dbus_connection_new_for_address_sync(
        bus_addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);

    /* Restore root */
    if (geteuid() != saved_euid)
        seteuid(saved_euid);

    if (!conn) {
        if (evemon_debug)
            fprintf(stderr, "[MPRIS DEBUG] bus connect FAILED: %s\n",
                    err ? err->message : "(unknown)");
        g_clear_error(&err);
        return NULL;
    }

    if (evemon_debug)
        fprintf(stderr, "[MPRIS DEBUG] bus connect OK\n");
    return conn;
}

/* ── helper: get PID for a bus name ──────────────────────────── */

static pid_t mpris_get_bus_pid(GDBusConnection *conn, const char *bus_name)
{
    GError *err = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "GetConnectionUnixProcessID",
        g_variant_new("(s)", bus_name),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_DBUS_TIMEOUT_MS,
        NULL, &err);

    if (!result) {
        g_clear_error(&err);
        return 0;
    }

    guint32 pid = 0;
    g_variant_get(result, "(u)", &pid);
    g_variant_unref(result);
    return (pid_t)pid;
}

/* ── helper: get a single property from an interface ─────────── */

static GVariant *mpris_get_property(GDBusConnection *conn,
                                     const char *bus_name,
                                     const char *iface,
                                     const char *prop)
{
    GError *err = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        conn,
        bus_name,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", iface, prop),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_DBUS_TIMEOUT_MS,
        NULL, &err);

    if (!result) {
        g_clear_error(&err);
        return NULL;
    }

    GVariant *val = NULL;
    g_variant_get(result, "(v)", &val);
    g_variant_unref(result);
    return val;  /* caller must unref */
}

/* ── helper: get all properties from an interface ────────────── */

static GVariant *mpris_get_all_properties(GDBusConnection *conn,
                                           const char *bus_name,
                                           const char *iface)
{
    GError *err = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        conn,
        bus_name,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", iface),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_DBUS_TIMEOUT_MS,
        NULL, &err);

    if (!result) {
        g_clear_error(&err);
        return NULL;
    }

    GVariant *dict = NULL;
    g_variant_get(result, "(@a{sv})", &dict);
    g_variant_unref(result);
    return dict;  /* caller must unref */
}

/* ── helper: extract string from a{sv} dict ──────────────────── */

static void dict_get_string(GVariant *dict, const char *key,
                             char *buf, size_t bufsz)
{
    buf[0] = '\0';
    GVariant *val = g_variant_lookup_value(dict, key, NULL);
    if (!val) return;

    if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        const char *s = g_variant_get_string(val, NULL);
        if (s) snprintf(buf, bufsz, "%s", s);
    }
    g_variant_unref(val);
}

static int dict_get_bool(GVariant *dict, const char *key, int def)
{
    GVariant *val = g_variant_lookup_value(dict, key,
                                            G_VARIANT_TYPE_BOOLEAN);
    if (!val) return def;
    int r = g_variant_get_boolean(val) ? 1 : 0;
    g_variant_unref(val);
    return r;
}

static double dict_get_double(GVariant *dict, const char *key, double def)
{
    GVariant *val = g_variant_lookup_value(dict, key,
                                            G_VARIANT_TYPE_DOUBLE);
    if (!val) return def;
    double r = g_variant_get_double(val);
    g_variant_unref(val);
    return r;
}

static int64_t dict_get_int64(GVariant *dict, const char *key, int64_t def)
{
    GVariant *val = g_variant_lookup_value(dict, key,
                                            G_VARIANT_TYPE_INT64);
    if (!val) return def;
    int64_t r = g_variant_get_int64(val);
    g_variant_unref(val);
    return r;
}

static int32_t dict_get_int32(GVariant *dict, const char *key, int32_t def)
{
    GVariant *val = g_variant_lookup_value(dict, key,
                                            G_VARIANT_TYPE_INT32);
    if (!val) return def;
    int32_t r = g_variant_get_int32(val);
    g_variant_unref(val);
    return r;
}

/* ── helper: extract string array as comma-joined ────────────── */

static void metadata_get_string_list(GVariant *metadata, const char *key,
                                      char *buf, size_t bufsz)
{
    buf[0] = '\0';
    GVariant *val = g_variant_lookup_value(metadata, key, NULL);
    if (!val) return;

    /* Could be as (array of strings) or s (single string) */
    if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING_ARRAY)) {
        gsize n = 0;
        const gchar **arr = g_variant_get_strv(val, &n);
        size_t pos = 0;
        for (gsize i = 0; i < n && pos < bufsz - 1; i++) {
            if (i > 0 && pos < bufsz - 3) {
                buf[pos++] = ',';
                buf[pos++] = ' ';
            }
            size_t rem = bufsz - pos - 1;
            size_t len = strlen(arr[i]);
            if (len > rem) len = rem;
            memcpy(buf + pos, arr[i], len);
            pos += len;
        }
        buf[pos] = '\0';
        g_free(arr);
    } else if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        const char *s = g_variant_get_string(val, NULL);
        if (s) snprintf(buf, bufsz, "%s", s);
    }
    g_variant_unref(val);
}

/* ── helper: extract string from metadata ────────────────────── */

static void metadata_get_string(GVariant *metadata, const char *key,
                                 char *buf, size_t bufsz)
{
    buf[0] = '\0';
    GVariant *val = g_variant_lookup_value(metadata, key, NULL);
    if (!val) return;

    if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        const char *s = g_variant_get_string(val, NULL);
        if (s) snprintf(buf, bufsz, "%s", s);
    } else if (g_variant_is_of_type(val, G_VARIANT_TYPE_OBJECT_PATH)) {
        const char *s = g_variant_get_string(val, NULL);
        if (s) snprintf(buf, bufsz, "%s", s);
    }
    g_variant_unref(val);
}

/* ── parse a single MPRIS player ─────────────────────────────── */

static int mpris_read_player(GDBusConnection *conn,
                              const char *bus_name,
                              pid_t player_pid,
                              evemon_mpris_player_t *out)
{
    memset(out, 0, sizeof(*out));
    out->pid = player_pid;
    out->length_us = -1;
    out->track_number = -1;
    out->disc_number = -1;
    snprintf(out->bus_name, sizeof(out->bus_name), "%s", bus_name);

    /* ── Read org.mpris.MediaPlayer2 (root interface) ────────── */
    GVariant *root = mpris_get_all_properties(conn, bus_name,
                                               "org.mpris.MediaPlayer2");
    if (root) {
        dict_get_string(root, "Identity",
                        out->identity, sizeof(out->identity));
        dict_get_string(root, "DesktopEntry",
                        out->desktop_entry, sizeof(out->desktop_entry));
        g_variant_unref(root);
    }

    /* ── Read org.mpris.MediaPlayer2.Player ──────────────────── */
    GVariant *player = mpris_get_all_properties(conn, bus_name,
                                                 "org.mpris.MediaPlayer2.Player");
    if (!player)
        return -1;

    dict_get_string(player, "PlaybackStatus",
                    out->playback_status, sizeof(out->playback_status));
    dict_get_string(player, "LoopStatus",
                    out->loop_status, sizeof(out->loop_status));
    out->shuffle   = dict_get_bool(player, "Shuffle", 0);
    out->volume    = dict_get_double(player, "Volume", -1.0);
    out->rate      = dict_get_double(player, "Rate", 1.0);
    out->position_us = dict_get_int64(player, "Position", 0);

    out->can_play       = dict_get_bool(player, "CanPlay", 0);
    out->can_pause      = dict_get_bool(player, "CanPause", 0);
    out->can_seek       = dict_get_bool(player, "CanSeek", 0);
    out->can_go_next    = dict_get_bool(player, "CanGoNext", 0);
    out->can_go_previous = dict_get_bool(player, "CanGoPrevious", 0);
    out->can_control    = dict_get_bool(player, "CanControl", 0);

    /* ── Parse Metadata dictionary ───────────────────────────── */
    GVariant *meta_var = g_variant_lookup_value(player, "Metadata", NULL);
    if (meta_var) {
        /* Metadata is a{sv} — may be wrapped in a variant */
        GVariant *metadata = meta_var;
        if (g_variant_is_of_type(meta_var, G_VARIANT_TYPE_VARIANT)) {
            metadata = g_variant_get_variant(meta_var);
        }

        if (g_variant_is_of_type(metadata, G_VARIANT_TYPE_VARDICT)) {
            metadata_get_string(metadata, "xesam:title",
                                out->track_title, sizeof(out->track_title));
            metadata_get_string_list(metadata, "xesam:artist",
                                     out->track_artist, sizeof(out->track_artist));
            metadata_get_string(metadata, "xesam:album",
                                out->track_album, sizeof(out->track_album));
            metadata_get_string_list(metadata, "xesam:albumArtist",
                                     out->track_album_artist,
                                     sizeof(out->track_album_artist));
            metadata_get_string(metadata, "mpris:artUrl",
                                out->art_url, sizeof(out->art_url));
            metadata_get_string(metadata, "mpris:trackid",
                                out->track_id, sizeof(out->track_id));
            metadata_get_string_list(metadata, "xesam:genre",
                                     out->genre, sizeof(out->genre));
            metadata_get_string(metadata, "xesam:url",
                                out->url, sizeof(out->url));
            metadata_get_string(metadata, "xesam:contentCreated",
                                out->content_type, sizeof(out->content_type));

            /* Length: mpris:length is int64 (µs) */
            GVariant *len_v = g_variant_lookup_value(metadata,
                                                      "mpris:length", NULL);
            if (len_v) {
                if (g_variant_is_of_type(len_v, G_VARIANT_TYPE_INT64))
                    out->length_us = g_variant_get_int64(len_v);
                else if (g_variant_is_of_type(len_v, G_VARIANT_TYPE_UINT64))
                    out->length_us = (int64_t)g_variant_get_uint64(len_v);
                else if (g_variant_is_of_type(len_v, G_VARIANT_TYPE_INT32))
                    out->length_us = (int64_t)g_variant_get_int32(len_v);
                g_variant_unref(len_v);
            }

            /* Track number */
            GVariant *tn = g_variant_lookup_value(metadata,
                                                   "xesam:trackNumber", NULL);
            if (tn) {
                if (g_variant_is_of_type(tn, G_VARIANT_TYPE_INT32))
                    out->track_number = g_variant_get_int32(tn);
                g_variant_unref(tn);
            }

            /* Disc number */
            GVariant *dn = g_variant_lookup_value(metadata,
                                                   "xesam:discNumber", NULL);
            if (dn) {
                if (g_variant_is_of_type(dn, G_VARIANT_TYPE_INT32))
                    out->disc_number = g_variant_get_int32(dn);
                g_variant_unref(dn);
            }
        }

        if (metadata != meta_var)
            g_variant_unref(metadata);
        g_variant_unref(meta_var);
    }

    g_variant_unref(player);
    return 0;
}

/* ── public API ──────────────────────────────────────────────── */

int mpris_scan_for_pid(pid_t pid,
                       const pid_t *desc_pids, size_t desc_count,
                       evemon_mpris_data_t *out)
{
    memset(out, 0, sizeof(*out));

    GDBusConnection *conn = mpris_connect_session_bus();
    if (!conn)
        return -1;

    /* List all bus names */
    GError *err = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        NULL,
        G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_DBUS_TIMEOUT_MS,
        NULL, &err);

    if (!result) {
        g_clear_error(&err);
        g_object_unref(conn);
        return -1;
    }

    GVariantIter *iter = NULL;
    g_variant_get(result, "(as)", &iter);

    const gchar *name;
    if (evemon_debug)
        fprintf(stderr, "[MPRIS DEBUG] scan for pid=%d, desc_count=%zu\n",
                (int)pid, desc_count);
    while (g_variant_iter_next(iter, "&s", &name)) {
        /* Filter for org.mpris.MediaPlayer2.* */
        if (!g_str_has_prefix(name, "org.mpris.MediaPlayer2."))
            continue;

        if (out->player_count >= EVEMON_MPRIS_MAX_PLAYERS)
            break;

        /* Get the PID of this bus name owner */
        pid_t bus_pid = mpris_get_bus_pid(conn, name);
        if (evemon_debug)
            fprintf(stderr, "[MPRIS DEBUG]   bus=%s  bus_pid=%d\n",
                    name, (int)bus_pid);
        if (bus_pid <= 0)
            continue;

        /* Check if this PID matches our target or any descendant */
        int match = (bus_pid == pid);
        if (!match && desc_pids) {
            for (size_t i = 0; i < desc_count; i++) {
                if (desc_pids[i] == bus_pid) {
                    match = 1;
                    break;
                }
            }
        }
        if (evemon_debug)
            fprintf(stderr, "[MPRIS DEBUG]   match=%d\n", match);
        if (!match)
            continue;

        /* Read the player's metadata */
        evemon_mpris_player_t *p = &out->players[out->player_count];
        if (mpris_read_player(conn, name, bus_pid, p) == 0) {
            if (evemon_debug)
                fprintf(stderr, "[MPRIS DEBUG]   -> identity='%s' title='%s' "
                        "artist='%s' album='%s' status='%s' art='%.80s'\n",
                        p->identity, p->track_title, p->track_artist,
                        p->track_album, p->playback_status, p->art_url);
            out->player_count++;
        }
    }

    g_variant_iter_free(iter);
    g_variant_unref(result);
    g_object_unref(conn);
    return 0;
}
