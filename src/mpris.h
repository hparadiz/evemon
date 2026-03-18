/*
 * mpris.h – MPRIS2 media metadata provider for evemon.
 *
 * Queries the D-Bus session bus for org.mpris.MediaPlayer2 instances,
 * resolves their PIDs, and exposes current playback metadata (track
 * title, artist, album, album art URL, playback status, position,
 * etc.) to any plugin that requests evemon_NEED_MPRIS.
 *
 * Uses GDBus (GIO) — already linked via GTK3, zero new deps.
 *
 * Thread safety: mpris_scan_for_pid() creates its own synchronous
 * GDBusConnection and is safe to call from the broker worker thread.
 * All returned data is owned by the caller (heap-allocated).
 *
 * Graceful degradation:
 *   - If D-Bus session bus is unreachable → returns empty results.
 *   - If no MPRIS players are registered → returns empty results.
 *   - If a player doesn't expose certain properties → fields are "".
 *   - Album art URL may be a file:// or http(s):// URL, or empty.
 */

#ifndef EVEMON_MPRIS_H
#define EVEMON_MPRIS_H

/*
 * Pull in evemon_mpris_player_t from the public plugin ABI header.
 * The struct is defined there so plugins can read the fields without
 * depending on this internal header.
 */
#include "evemon_plugin.h"

/* Maximum number of MPRIS players we track per PID */
#define EVEMON_MPRIS_MAX_PLAYERS  8

/* ── Aggregated MPRIS results for a PID ─────────────────────── */

typedef struct {
    evemon_mpris_player_t  players[EVEMON_MPRIS_MAX_PLAYERS];
    size_t                 player_count;
} evemon_mpris_data_t;

/*
 * Scan the D-Bus session bus for MPRIS players belonging to `pid`
 * (or any of its descendant PIDs in `desc_pids`).
 *
 * Fills `out` with metadata for all matching players.
 * Returns 0 on success, -1 if D-Bus is unavailable.
 *
 * Safe to call from any thread (creates its own GDBusConnection).
 * The `out` struct is fully self-contained — no pointers to free.
 *
 * If running as root (sudo), attempts to connect to the real user's
 * session bus via DBUS_SESSION_BUS_ADDRESS or /run/user/<uid>/bus.
 */
int mpris_scan_for_pid(pid_t pid,
                       const pid_t *desc_pids, size_t desc_count,
                       evemon_mpris_data_t *out);

#endif /* EVEMON_MPRIS_H */
