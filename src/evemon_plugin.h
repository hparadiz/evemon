/*
 * evemon_plugin.h – Public plugin ABI header.
 *
 * This is the ONLY header third-party plugin authors need.  It defines
 * the stable contract between evemon and dynamically-loaded .so plugins.
 *
 * Plugins compile to standalone shared objects:
 *   gcc -shared -fPIC -o evemon_myplugin.so myplugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 *
 * Drop the .so into the plugins/ directory and restart evemon.
 *
 * ABI rules:
 *   - New fields may be APPENDED to evemon_proc_data_t (minor change).
 *   - New bits may be added to evemon_data_needs_t (minor change).
 *   - Removing or reordering fields bumps evemon_PLUGIN_ABI_VERSION.
 *   - Old plugins that don't know about new fields keep working.
 */

#ifndef evemon_PLUGIN_H
#define evemon_PLUGIN_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * GTK-dependent types.  Core (toolkit-free) code defines EVEMON_NO_GTK
 * before including this header; GtkWidget and GdkPixbuf fields are
 * replaced with void* so the header compiles without GTK headers.
 * GTK plugins and UI code include gtk/gtk.h first and get real types.
 */
#ifdef EVEMON_NO_GTK
typedef void  GtkWidget;
typedef void  GdkPixbuf;
typedef void  GtkDrawingArea;
#else
#include <gtk/gtk.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── ABI version ─────────────────────────────────────────────── */

#define evemon_PLUGIN_ABI_VERSION  1

/* ── Event bus types ──────────────────────────────────────────── */

/*
 * Event types for the publish/subscribe event bus.
 * New event types may be appended (minor ABI change).
 */
typedef enum {
    EVEMON_EVENT_PROCESS_SELECTED,     /* payload: pid_t*                */
    EVEMON_EVENT_ALBUM_ART_UPDATED,    /* payload: evemon_album_art_payload_t* */
    EVEMON_EVENT_AUDIO_STATE_CHANGED,  /* payload: reserved (NULL)       */
    EVEMON_EVENT_PID_EXITED,           /* payload: pid_t*                */
    EVEMON_EVENT_JSON_SNAPSHOT,        /* payload: evemon_json_payload_t**/
    EVEMON_EVENT_FD_WRITE,            /* payload: evemon_fd_write_payload_t* */
    EVEMON_EVENT_CHILD_EXEC,           /* payload: evemon_exec_payload_t*    */
    EVEMON_EVENT_CUSTOM                /* payload: user-defined          */
} evemon_event_type_t;

/* Event structure passed to subscribers */
typedef struct {
    evemon_event_type_t  type;
    void                *payload;     /* type-specific, see enum above   */
} evemon_event_t;

/* Subscriber callback signature */
typedef void (*evemon_event_cb)(const evemon_event_t *event,
                                void *user_data);

/* ── Plugin role ─────────────────────────────────────────────── */

/*
 * Every plugin declares one of three roles:
 *
 *   EVEMON_ROLE_PROCESS  – process-centric UI plugin; provides a
 *                          GtkWidget tab shown when a process is
 *                          selected.  This is the default (= 0) so
 *                          that old plugins compiled before roles
 *                          were introduced keep working.
 *
 *   EVEMON_ROLE_SERVICE  – headless, auto-activated at load time;
 *                          no UI widget.  Provides reusable host
 *                          services to other plugins via the event
 *                          bus (e.g. audio metadata, JSON snapshots).
 *
 *   EVEMON_ROLE_SYSTEM   – always-active UI plugin shown in a
 *                          dedicated system dock area, independent
 *                          of the selected process.
 */
typedef enum {
    EVEMON_ROLE_PROCESS = 0,          /* per-process tab UI             */
    EVEMON_ROLE_SERVICE = 1,          /* headless auto-activated service */
    EVEMON_ROLE_SYSTEM  = 2           /* always-active system dock UI   */
} evemon_plugin_role_t;

/* Legacy aliases — kept for source compatibility with old plugins */
#define EVEMON_PLUGIN_UI       EVEMON_ROLE_PROCESS
#define EVEMON_PLUGIN_HEADLESS EVEMON_ROLE_SERVICE

/* ── Event payloads ──────────────────────────────────────────── */

/*
 * Payload for EVEMON_EVENT_ALBUM_ART_UPDATED.
 * Published by the audio service plugin when album art changes.
 * The pixbuf is ref'd by the publisher; subscribers must ref it
 * if they want to keep it beyond the callback.
 */
typedef struct {
    GdkPixbuf  *pixbuf;               /* loaded art (may be NULL); cast to GdkPixbuf* in GTK code */
    pid_t       source_pid;           /* PID this art belongs to         */
    char        art_url[512];         /* source URL                      */
    char        track_title[256];     /* current track title             */
    char        track_artist[256];    /* current track artist            */
    char        track_album[256];     /* current track album             */
    char        playback_status[32];  /* "Playing", "Paused", "Stopped"  */
    int64_t     position_us;          /* playback position in µs         */
    int64_t     length_us;            /* track length in µs              */
    char        identity[128];        /* player identity                 */
} evemon_album_art_payload_t;

/*
 * Payload for EVEMON_EVENT_JSON_SNAPSHOT.
 * Published by the JSON service plugin each update cycle.
 * `json` is a heap-allocated NUL-terminated JSON string;
 * the publisher owns it — subscribers must strdup() if
 * they want to keep it beyond the callback.
 * `len` is strlen(json), provided for convenience.
 */
typedef struct {
    const char *json;                 /* heap-allocated JSON string       */
    size_t      len;                  /* strlen(json)                     */
    pid_t       source_pid;           /* PID this snapshot describes      */
} evemon_json_payload_t;

/* Payload for EVEMON_EVENT_FD_WRITE: copy of data passed to write().
 * `data` holds up to 4096 bytes of the write() buffer, matching the
 * BPF capture limit (FDMON_BPF_WRITE_MAX).  `len` is the number of
 * valid bytes.  Writes larger than 4096 bytes are truncated; `truncated`
 * is set non-zero when the original write was larger than what was captured. */
typedef struct {
    pid_t  pid;   /* tid value from fdmon event (per-call) */
    pid_t  tgid;  /* userspace TGID (process PID) */
    int    fd;    /* file descriptor number */
    size_t len;   /* number of valid bytes in data */
    int    truncated; /* non-zero if original write > len */
    char   data[4096];
} evemon_fd_write_payload_t;

/* Payload for EVEMON_EVENT_CHILD_EXEC: a process just called execve().
 * Published for every exec, not just orphan-mode processes.
 * Subscribers that are monitoring a parent PID can check ppid and
 * immediately register the new pid's fds before it writes anything. */
typedef struct {
    pid_t  pid;          /* new process image PID (tgid)  */
    pid_t  ppid;         /* parent PID at exec time (best-effort from /proc) */
    char   path[256];    /* executable path (best effort) */
} evemon_exec_payload_t;

/* ── Data needs bitmask ──────────────────────────────────────── */

typedef enum {
    evemon_NEED_FDS         = (1 << 0),  /* fd list + socket resolution       */
    evemon_NEED_ENV         = (1 << 1),  /* /proc/<pid>/environ               */
    evemon_NEED_MMAP        = (1 << 2),  /* /proc/<pid>/maps (all regions)    */
    evemon_NEED_SOCKETS     = (1 << 3),  /* net sockets with eBPF throughput  */
    evemon_NEED_CGROUP      = (1 << 4),  /* cgroup controller files           */
    evemon_NEED_STATUS      = (1 << 5),  /* raw /proc/<pid>/status            */
    evemon_NEED_LIBS        = (1 << 6),  /* shared library list (from maps)   */
    evemon_NEED_PIPEWIRE    = (1 << 7),  /* PipeWire graph snapshot           */
    evemon_NEED_DESCENDANTS = (1 << 8),  /* include descendant PIDs           */
    evemon_NEED_THREADS     = (1 << 9),  /* per-thread info from /proc/task   */
    evemon_NEED_MPRIS       = (1 << 10), /* MPRIS2 media player metadata      */
} evemon_data_needs_t;

/* ── Gathered data types ─────────────────────────────────────── */

/* A single file descriptor entry */
typedef struct {
    int       fd;
    char      path[512];
    char      desc[256];         /* resolved label (e.g. device name) */
    int       category;          /* fd_category_t value               */
    uint64_t  net_sort_key;      /* total send+recv bytes for sort    */
    pid_t     source_pid;        /* PID that owns this fd (for desc)  */
} evemon_fd_t;

/* A single environment variable */
typedef struct {
    const char *text;            /* "KEY=value"                       */
    int         category;        /* env_category_t value              */
} evemon_env_t;

/* A single memory mapping region */
typedef struct {
    const char *text;            /* display line                      */
    int         category;        /* mmap_category_t value             */
    size_t      size_kb;         /* region size in KiB                */
} evemon_mmap_t;

/* A single shared library */
typedef struct {
    char      path[512];         /* full path to .so/.dll             */
    char      name[256];         /* basename                          */
    char      version[64];       /* version string (may be empty)     */
    char      origin[128];       /* short origin label (may be empty) */
    int       category;          /* lib_category_t value              */
    size_t    size_kb;           /* total code mapping size in KiB    */
} evemon_lib_t;

/* A single network socket with throughput */
typedef struct {
    char      desc[512];         /* "TCP 1.2.3.4:80 → 5.6.7.8:443"  */
    uint64_t  send_delta;        /* bytes sent since last snapshot    */
    uint64_t  recv_delta;        /* bytes received                    */
    uint64_t  total;             /* send + recv (sort key)            */
    pid_t     source_pid;        /* PID that owns this socket         */
} evemon_socket_t;

/* A PipeWire graph node */
typedef struct {
    uint32_t    id;              /* PipeWire node ID                  */
    uint32_t    client_id;       /* owning client object ID           */
    pid_t       pid;             /* application.process.id or 0       */
    char        app_name[128];   /* application.name                  */
    char        node_name[128];  /* node.name                         */
    char        node_desc[256];  /* node.description                  */
    char        media_class[64]; /* media.class (Stream/Output/Audio) */
    char        media_name[256]; /* media.name (tab title, song)      */
} evemon_pw_node_t;

/* A PipeWire graph link */
typedef struct {
    uint32_t  output_node;       /* source node ID                    */
    uint32_t  output_port;       /* source port ID                    */
    uint32_t  input_node;        /* sink node ID                      */
    uint32_t  input_port;        /* sink port ID                      */
} evemon_pw_link_t;

/* A PipeWire graph port */
typedef struct {
    uint32_t  id;                /* PipeWire port ID                  */
    uint32_t  node_id;           /* owning node ID                    */
    char      port_name[128];    /* port.name                         */
    char      direction[8];      /* "in" or "out"                     */
    char      format_dsp[64];    /* format.dsp                        */
} evemon_pw_port_t;

/* A single thread (task) within a process */
typedef struct {
    pid_t       tid;              /* thread ID (= /proc/<pid>/task/<tid>)  */
    char        name[64];         /* thread name from /proc/.../comm       */
    char        state;            /* 'R','S','D','Z','T','t','X','I' etc   */
    int         priority;         /* scheduling priority (field 18 of stat)*/
    int         nice;             /* nice value (field 19 of stat)         */
    int         processor;        /* last CPU core (field 39 of stat)      */
    unsigned long long utime;     /* user ticks (field 14)                 */
    unsigned long long stime;     /* system ticks (field 15)               */
    unsigned long long starttime; /* start time in jiffies (field 22)      */
    double      cpu_percent;      /* CPU% since last snapshot              */
    unsigned long long voluntary_ctxt_switches;   /* from status           */
    unsigned long long nonvoluntary_ctxt_switches; /* from status          */
} evemon_thread_t;

/* MPRIS media player metadata (from D-Bus session bus) */
typedef struct {
    char      identity[128];        /* player name ("Spotify", "Firefox") */
    char      desktop_entry[128];   /* .desktop entry name               */
    pid_t     pid;                  /* player process ID                 */
    char      bus_name[256];        /* D-Bus bus name                    */

    char      playback_status[32];  /* "Playing", "Paused", "Stopped"    */
    char      loop_status[32];      /* "None", "Track", "Playlist"       */
    int       shuffle;              /* 0 or 1                            */
    double    volume;               /* 0.0–1.0                           */
    double    rate;                 /* playback rate (1.0 = normal)      */
    int64_t   position_us;          /* playback position in µs           */

    char      track_title[256];     /* xesam:title                       */
    char      track_artist[256];    /* xesam:artist (comma-joined)       */
    char      track_album[256];     /* xesam:album                       */
    char      track_album_artist[256]; /* xesam:albumArtist              */
    char      art_url[512];         /* mpris:artUrl (file:// or http)    */
    char      track_id[256];        /* mpris:trackid                     */
    char      genre[128];           /* xesam:genre (comma-joined)        */
    int64_t   length_us;            /* mpris:length in µs (-1=unknown)   */
    int       track_number;         /* xesam:trackNumber (-1=unknown)    */
    int       disc_number;          /* xesam:discNumber (-1=unknown)     */
    char      content_type[64];     /* xesam:contentCreated              */
    char      url[512];             /* xesam:url (stream URL)            */

    int       can_play;
    int       can_pause;
    int       can_seek;
    int       can_go_next;
    int       can_go_previous;
    int       can_control;
} evemon_mpris_player_t;

/* cgroup resource limits */
typedef struct {
    char      path[512];         /* cgroup v2 path                    */
    int64_t   mem_current;       /* memory.current (bytes, -1=N/A)    */
    int64_t   mem_max;           /* memory.max (bytes, -1=unlimited)  */
    int64_t   mem_high;          /* memory.high (bytes, -1=N/A)       */
    int64_t   swap_max;          /* memory.swap.max (bytes, -1=N/A)   */
    int64_t   cpu_quota;         /* cpu.max quota µs (-1=N/A)         */
    int64_t   cpu_period;        /* cpu.max period µs (-1=N/A)        */
    int64_t   pids_current;      /* pids.current (-1=N/A)             */
    int64_t   pids_max;          /* pids.max (-1=N/A)                 */
    char      io_max[256];       /* io.max line (empty=none)          */
} evemon_cgroup_t;

/*
 * The gathered data bundle — passed to plugin update() callbacks.
 *
 * Only fields corresponding to the plugin's data_needs are populated.
 * Other fields are zero/NULL.  The pointer is valid ONLY for the
 * duration of the update() call — do not stash it.
 */
typedef struct {
    /* Identity (always populated) */
    pid_t             pid;
    pid_t             ppid;
    const char       *name;
    const char       *cmdline;
    const char       *user;
    const char       *cwd;
    double            cpu_percent;
    long              rss_kb;

    /* Descendant PIDs (if evemon_NEED_DESCENDANTS) */
    const pid_t      *descendant_pids;
    size_t            descendant_count;

    /* File descriptors (if evemon_NEED_FDS) */
    const evemon_fd_t     *fds;
    size_t                 fd_count;

    /* Environment variables (if evemon_NEED_ENV) */
    const evemon_env_t    *envs;
    size_t                 env_count;

    /* Memory mappings (if evemon_NEED_MMAP) */
    const evemon_mmap_t   *mmaps;
    size_t                 mmap_count;

    /* Shared libraries (if evemon_NEED_LIBS) */
    const evemon_lib_t    *libs;
    size_t                 lib_count;

    /* Network sockets with throughput (if evemon_NEED_SOCKETS) */
    const evemon_socket_t *sockets;
    size_t                 socket_count;

    /* cgroup limits (if evemon_NEED_CGROUP) */
    const evemon_cgroup_t *cgroup;

    /* PipeWire graph (if evemon_NEED_PIPEWIRE) */
    const evemon_pw_node_t   *pw_nodes;
    size_t                    pw_node_count;
    const evemon_pw_link_t   *pw_links;
    size_t                    pw_link_count;
    const evemon_pw_port_t   *pw_ports;
    size_t                    pw_port_count;

    /* Threads (if evemon_NEED_THREADS) */
    const evemon_thread_t *threads;
    size_t                 thread_count;

    /* MPRIS media metadata (if evemon_NEED_MPRIS) */
    const evemon_mpris_player_t *mpris_players;
    size_t                       mpris_player_count;

    /* Raw file contents (if corresponding NEED flag set) */
    const char       *raw_cgroup;    /* /proc/<pid>/cgroup content     */
    const char       *raw_status;    /* /proc/<pid>/status content     */
    const char       *raw_maps;      /* /proc/<pid>/maps content       */

    /* eBPF network counters — host-provided, no plugin syscalls */
    const void       *fdmon;         /* opaque, use evemon_net_io_get() */
} evemon_proc_data_t;

/* ── Host services ────────────────────────────────────────────── */

/*
 * Function pointers provided by the host to plugins.  Injected via
 * the activate() callback after load.  Plugins that need real-time
 * host services (e.g. PipeWire audio meters, spectrogram) call
 * these instead of reimplementing the functionality themselves.
 *
 * All functions must be called from the GTK main thread.
 * The opaque `host_ctx` must be passed as the first argument.
 */
typedef struct {
    void *host_ctx;   /* opaque — pass to every call below */

    /* ── PipeWire peak meters ──────────────────────────────── */

    /*
     * Start real-time L/R peak level monitoring for a set of
     * PipeWire audio output nodes.  The host creates passive
     * capture streams and updates levels at ~50 ms intervals.
     *
     * node_ids: array of PW node IDs to monitor.
     * count:    number of entries in node_ids.
     *
     * Call with count=0 to stop all meters.
     * Replaces any previously running meters.
     */
    void (*pw_meter_start)(void *host_ctx,
                           const uint32_t *node_ids, size_t count);

    /*
     * Stop all PipeWire peak meters.
     */
    void (*pw_meter_stop)(void *host_ctx);

    /*
     * Read the current smoothed peak levels for a specific node.
     * Returns levels in 0..1000 range (0 = silence, 1000 = clipping).
     * Returns 0,0 if the node is not being monitored.
     */
    void (*pw_meter_read)(void *host_ctx, uint32_t node_id,
                          int *level_l, int *level_r);

    /*
     * Remove specific node streams from the shared meter.
     * Only the streams matching the given node IDs are torn down;
     * all other streams remain intact.  This allows plugins to
     * clean up their own audio nodes without nuking streams that
     * belong to other plugin instances.
     */
    void (*pw_meter_remove_nodes)(void *host_ctx,
                                  const uint32_t *node_ids, size_t count);

    /* ── PipeWire spectrogram ─────────────────────────────── */

    /*
     * Start real-time FFT spectrogram capture for a PipeWire node.
     * The host creates a passive capture stream, computes FFT, and
     * renders a scrolling waterfall into the provided GtkDrawingArea.
     *
     * Each draw_area gets its own independent spectrogram instance.
     * Multiple plugins can have simultaneous spectrograms.
     *
     * draw_area: the GtkDrawingArea to render the spectrogram into.
     *            The plugin owns this widget; the host just draws.
     * node_id:   PipeWire node ID to capture from.
     *
     * Call with node_id=0 to stop.
     */
    void (*spectro_start)(void *host_ctx, GtkDrawingArea *draw_area,
                          uint32_t node_id);

    /*
     * Stop the spectrogram for a specific draw area and release
     * the capture stream.  Pass the same draw_area used in
     * spectro_start().
     */
    void (*spectro_stop)(void *host_ctx, GtkDrawingArea *draw_area);

    /*
     * Get the currently active spectrogram target node ID for a
     * specific draw area.  Returns 0 if no spectrogram is running
     * for that area.
     */
    uint32_t (*spectro_get_target)(void *host_ctx,
                                   GtkDrawingArea *draw_area);

    /*
     * Set the colour theme for a spectrogram draw area.
     * theme_index maps to the spectro_theme_t enum (see ui_internal.h):
     *   0 = Classic, 1 = Heat, 2 = Cool, 3 = Greyscale,
     *   4 = Neon,    5 = Viridis
     * No-op if no spectrogram is running for that area.
     */
    void (*spectro_set_theme)(void *host_ctx,
                              GtkDrawingArea *draw_area,
                              unsigned theme_index);

    /*
     * Set the charting (spectrogram) colour theme globally — applies to
     * all active spectrogram instances, persists the choice in settings,
     * and syncs the View → Appearance → Charting Theme menu radio items.
     * theme_index maps to spectro_theme_t (0=Classic … 5=Vaporwave).
     */
    void (*set_charting_theme)(void *host_ctx, unsigned theme_index);

    /*
     * Register a callback to be invoked whenever the charting theme
     * changes (e.g. from the menu bar).  This lets plugin UIs keep
     * their own theme selector in sync with the global setting.
     * Pass NULL to unregister.  Only one callback per plugin instance.
     *
     * cb:        called with (plugin_ctx, theme_index) on the GTK main thread.
     * plugin_ctx: opaque pointer passed back as the first argument to cb.
     */
    void (*charting_theme_notify)(void *host_ctx,
                                  void (*cb)(void *plugin_ctx,
                                             unsigned theme_index),
                                  void *plugin_ctx);

    /* ── Event bus ─────────────────────────────────────────── */

    /*
     * Subscribe to events of a given type.  The callback is
     * always invoked on the GTK main thread.
     *
     * Returns a subscription ID (>0) that can be passed to
     * unsubscribe(), or 0 on failure.
     */
    int (*subscribe)(void *host_ctx,
                     evemon_event_type_t type,
                     evemon_event_cb cb,
                     void *user_data);

    /*
     * Publish an event.  Thread-safe: if called off the main
     * thread, dispatch is marshalled via g_idle_add.
     * The payload is deep-copied for known event types.
     */
    void (*publish)(void *host_ctx, const evemon_event_t *event);

    /*
     * Remove a subscription by its ID.  Pass the ID returned
     * by subscribe().
     */
    void (*unsubscribe)(void *host_ctx, int subscription_id);

    /* Host helpers: request monitoring of a specific pid+fd pair
     * via the eBPF backend. Returns 0 on success, -1 on failure. */
    int  (*monitor_fd_subscribe)(void *host_ctx, pid_t pid, int fd);
    void (*monitor_fd_unsubscribe)(void *host_ctx, pid_t pid, int fd);

    /* Register a parent PID so any child it exec()s has its fd 1/2
     * inserted into the BPF map immediately (in the reader thread,
     * before the child can write).  fd_mask: bit0=fd1, bit1=fd2.
     * Call unwatch when no longer monitoring that parent. */
    int  (*monitor_watch_children)(void *host_ctx, pid_t pid, int fd_mask);
    void (*monitor_unwatch_children)(void *host_ctx, pid_t pid);

    /* Enable/disable orphan-capture mode: automatically intercept writes
     * from any newly exec'd process whose stdout/stderr is not a TTY
     * (cron jobs, systemd services, pipes, etc.). */
    int  (*orphan_capture_enable)(void *host_ctx);
    void (*orphan_capture_disable)(void *host_ctx);

    /* ── Plugin-in-window launcher ─────────────────────────── */

    /*
     * Open a fresh instance of any registered plugin in its own
     * floating GtkWindow, tracking `pid`.
     *
     * plugin_id:  reverse-DNS plugin identifier, e.g. "org.evemon.milkdrop"
     * pid:        process to monitor in the new window
     * proc_name:  human-readable process name (used in the window title)
     *
     * No-op if the plugin_id is not loaded.
     */
    void (*open_plugin_window)(void *host_ctx,
                               const char *plugin_id,
                               pid_t pid,
                               const char *proc_name);

} evemon_host_services_t;

/* ── Plugin descriptor ───────────────────────────────────────── */

/*
 * Each .so exports a single function:
 *   evemon_plugin_t *evemon_plugin_init(void);
 *
 * The returned struct must be heap-allocated with calloc/malloc.
 * The host takes ownership and will free() it after calling destroy().
 * Do NOT return a pointer to a static variable — multiple instances
 * of the same plugin type require independent descriptors.
 */
typedef struct {
    /* ABI version — MUST equal evemon_PLUGIN_ABI_VERSION */
    int                 abi_version;

    /* Human-readable plugin name (e.g. "Environment Variables") */
    const char         *name;

    /* Reverse-DNS identifier (e.g. "org.evemon.env") */
    const char         *id;

    /* Plugin version string (informational, e.g. "1.0") */
    const char         *version;

    /* Bitmask of data the plugin needs from the broker */
    evemon_data_needs_t data_needs;

    /* Per-instance opaque context (set by the plugin in init) */
    void               *plugin_ctx;

    /*
     * Create the plugin's widget tree.  Called on the UI main thread
     * once per instance.  The returned widget is placed by the host
     * into a tab/panel — the plugin has zero layout awareness.
     *
     * ctx = plugin_ctx from this struct.
     * Returns a toolkit widget pointer (GtkWidget* for GTK frontend).
     * Headless/terminal plugins return NULL.
     */
    GtkWidget *(*create_widget)(void *ctx);

    /*
     * New data has arrived for the PID this instance is tracking.
     * Called on the GTK main thread.  `data` is valid only for the
     * duration of this call.
     */
    void (*update)(void *ctx, const evemon_proc_data_t *data);

    /*
     * The tracked PID has exited, or nothing is selected.
     * Clear the display.
     */
    void (*clear)(void *ctx);

    /*
     * The plugin instance is being destroyed (tab closed, shutdown).
     * Free any resources allocated in init/create_widget.
     */
    void (*destroy)(void *ctx);

    /*
     * (Optional) Called after load to inject host services.
     * Plugins that need host services (e.g. PipeWire meters)
     * should stash the pointer for later use.  The pointer
     * remains valid for the lifetime of the plugin instance.
     *
     * May be NULL if the plugin doesn't need host services.
     */
    void (*activate)(void *ctx, const evemon_host_services_t *services);

    /*
     * Plugin role — controls how the host treats this plugin.
     *
     *   EVEMON_ROLE_PROCESS  – per-process tab (default, = 0)
     *   EVEMON_ROLE_SERVICE  – headless, auto-activated service
     *   EVEMON_ROLE_SYSTEM   – always-active system dock panel
     *
     * Zero-initialised value maps to EVEMON_ROLE_PROCESS so that
     * plugins compiled against older ABI versions continue to work.
     */
    evemon_plugin_role_t role;

    /*
     * (Optional) NULL-terminated array of plugin IDs this plugin
     * depends on.  The host ensures all listed plugins are activated
     * before calling this plugin's activate() callback.
     *
     * Example:
     *   static const char *my_deps[] = {
     *       "org.evemon.audio_service", NULL
     *   };
     *   p->dependencies = my_deps;
     *
     * Set to NULL if the plugin has no dependencies.
     */
    const char **dependencies;

    /*
     * (Optional) Returns non-zero if this plugin should be shown as a tab.
     * Called on the GTK main thread when building the notebook.
     *
     * If NULL (e.g. plugins compiled against an older ABI), the plugin
     * is always considered available.  Return 0 to suppress the tab
     * entirely (e.g. a PipeWire plugin when PipeWire is not running).
     */
    int (*is_available)(void *ctx);

    /*
     * (Optional) Called by the host when the plugin's placement context
     * changes.  `active` is non-zero when the instance lives inside a
     * notebook tab (and should show/hide its tab label as needed), zero
     * when it lives in a standalone floating window (where there is no
     * tab to manipulate).
     *
     * Plugins that previously walked gtk_widget_get_parent() to discover
     * whether they were in a notebook should use this instead.  NULL is
     * safe — older plugins without this callback are always treated as
     * notebook-hosted (is_active = TRUE) so their behaviour is unchanged.
     */
    void (*set_active)(void *ctx, int active);

    /*
     * (Optional) Called by the broker before each gather cycle to ask
     * whether this instance currently wants to receive data.
     *
     * Return non-zero to receive update() as normal.
     * Return 0 to be skipped this cycle — the broker will not gather
     * the data your plugin needs and update() will not be called.
     *
     * NULL (not implemented) means “always send me data”, which is the
     * correct behaviour for plugins that need continuous data (headless
     * audio service, write monitor, floating windows, etc.).
     *
     * A typical implementation checks whether the plugin's widget is
     * currently mapped / visible:
     *
     *   int my_wants_update(void *ctx) {
     *       my_ctx_t *c = ctx;
     *       return gtk_widget_get_mapped(c->root_widget);
     *   }
     */
    int (*wants_update)(void *ctx);

} evemon_plugin_t;

/* ── Plugin init function signature ──────────────────────────── */

typedef evemon_plugin_t *(*evemon_plugin_init_fn)(void);

/*
 * EVEMON_PLUGIN_MANIFEST – embedded flat-bitcode manifest.
 *
 * Place this macro once at file scope in your plugin source.
 *
 * The struct is stored verbatim in the ".evemon_manifest" ELF section
 * as a contiguous block of bytes with NO relocations — all fields are
 * fixed-size value types (char arrays, uint32_t).  This means the host
 * can read the manifest by mmapping the .so file and finding the
 * section directly, without dlopen(), without resolving relocations,
 * and without executing any plugin code.
 *
 * Layout (all fields are fixed-size, no pointers):
 *
 *   magic        – EVEMON_MANIFEST_MAGIC (0x45564D4E, "EVMN") sentinel
 *   abi_version  – evemon_PLUGIN_ABI_VERSION at compile time
 *   role         – evemon_plugin_role_t (uint32_t)
 *   id[]         – reverse-DNS identifier, NUL-terminated, 128 bytes
 *   name[]       – human-readable name, NUL-terminated, 128 bytes
 *   version[]    – version string (e.g. "1.0"), NUL-terminated, 32 bytes
 *   deps[]       – packed dependency IDs: each entry is a NUL-terminated
 *                  string; the list ends with an extra NUL byte (i.e.
 *                  two consecutive NUL bytes mark the end).  512 bytes.
 *                  Empty (deps[0] == 0) means no dependencies.
 *
 * Total struct size: 4+4+4+128+128+32+512 = 812 bytes.
 *
 * Usage:
 *
 *   EVEMON_PLUGIN_MANIFEST(
 *       "org.evemon.myplugin",   // id
 *       "My Plugin",             // name
 *       "1.0",                   // version
 *       EVEMON_ROLE_PROCESS,     // role
 *       "org.evemon.audio_service", NULL  // dependencies (NULL-terminated)
 *   );
 *
 *   // No-dependency form:
 *   EVEMON_PLUGIN_MANIFEST("org.evemon.env", "Environment Variables",
 *                          "1.0", EVEMON_ROLE_PROCESS, NULL);
 */

#define EVEMON_MANIFEST_MAGIC  UINT32_C(0x45564D4E)  /* "EVMN" */

/* Sizes of the fixed string fields in the manifest */
#define EVEMON_MANIFEST_ID_LEN      128
#define EVEMON_MANIFEST_NAME_LEN    128
#define EVEMON_MANIFEST_VER_LEN      32
#define EVEMON_MANIFEST_DEPS_LEN    512

typedef struct {
    uint32_t magic;                          /* EVEMON_MANIFEST_MAGIC          */
    uint32_t abi_version;                    /* evemon_PLUGIN_ABI_VERSION       */
    uint32_t role;                           /* evemon_plugin_role_t value      */
    char     id     [EVEMON_MANIFEST_ID_LEN];   /* plugin identifier (NUL-term) */
    char     name   [EVEMON_MANIFEST_NAME_LEN]; /* human-readable name          */
    char     version[EVEMON_MANIFEST_VER_LEN];  /* version string               */
    /*
     * deps: packed NUL-terminated strings, double-NUL terminated.
     * e.g. "org.evemon.audio_service\0org.evemon.other\0\0"
     * An empty deps field has deps[0] == '\0'.
     */
    char     deps   [EVEMON_MANIFEST_DEPS_LEN];
} evemon_plugin_manifest_t;

/*
 * _evemon_manifest_pack_deps – internal helper used by the macro.
 * Writes a packed double-NUL-terminated dep list into `buf`.
 * The varargs must be (const char*) values ending with NULL.
 * Not for direct use.
 */
static inline void _evemon_manifest_pack_deps(char *buf, size_t bufsz, ...)
    __attribute__((unused));
static inline void _evemon_manifest_pack_deps(char *buf, size_t bufsz, ...)
{
    /* va_list not available without stdarg.h — which is already pulled in
     * by stddef.h / stdint.h.  We use __builtin_va_* for portability. */
    __builtin_va_list ap;
    __builtin_va_start(ap, bufsz);
    size_t off = 0;
    const char *dep;
    while ((dep = __builtin_va_arg(ap, const char *)) != (const char *)0) {
        size_t dlen = 0;
        while (dep[dlen]) dlen++;
        if (off + dlen + 2 >= bufsz) break;  /* +2 for this NUL + terminator */
        for (size_t i = 0; i < dlen; i++) buf[off++] = dep[i];
        buf[off++] = '\0';
    }
    __builtin_va_end(ap);
    /* Ensure double-NUL termination (buf was already zero-filled by macro) */
    if (off < bufsz) buf[off] = '\0';
}

/*
 * EVEMON_PLUGIN_MANIFEST(id, name, version, role, dep0, dep1, ..., NULL)
 *
 * Emits the flat manifest into ".evemon_manifest".
 * The constructor__ priority 101 runs before C++ static init (102+) and
 * before the plugin's own constructors, filling in the deps field at
 * startup using the pack helper (avoids compound-literal VLA limits).
 */
#define EVEMON_PLUGIN_MANIFEST(_id, _name, _ver, _role, ...) \
    __attribute__((used, section(".evemon_manifest"))) \
    static evemon_plugin_manifest_t _evemon_manifest = { \
        .magic       = EVEMON_MANIFEST_MAGIC, \
        .abi_version = (uint32_t)evemon_PLUGIN_ABI_VERSION, \
        .role        = (uint32_t)(_role), \
        .id          = _id, \
        .name        = _name, \
        .version     = _ver, \
        .deps        = { 0 }, \
    }; \
    __attribute__((constructor(101), used)) \
    static void _evemon_manifest_init_deps(void) { \
        _evemon_manifest_pack_deps( \
            _evemon_manifest.deps, \
            sizeof(_evemon_manifest.deps), \
            __VA_ARGS__); \
    }

/* ── Host-provided utility functions ─────────────────────────── */

/*
 * These are real symbols exported by the evemon binary (linked with
 * -rdynamic).  Plugins can call them directly — they are resolved
 * automatically by dlopen with RTLD_GLOBAL.
 */

/* Query eBPF per-PID network throughput (reads from host's fdmon) */
int  evemon_net_io_get(const evemon_proc_data_t *data, pid_t tgid,
                       uint64_t *send_bytes, uint64_t *recv_bytes);

/* Format a KiB value to human-readable string (shared helper) */
void evemon_format_memory(long kb, char *buf, size_t bufsz);

/* Format an epoch timestamp to fuzzy "2h 15m ago" string */
void evemon_format_fuzzy_time(time_t epoch, char *buf, size_t bufsz);

/*
 * Persistent settings access.  Plugins may read/write evemon's
 * settings.json via these re-exported symbols.  The returned pointer
 * is the global singleton — modify fields then call settings_save().
 * Do not cache the pointer across calls.
 *
 * evemon_settings_t is declared in settings.h.  Plugins that need
 * field access must include settings.h alongside this header.
 */
void *evemon_settings_get(void);   /* returns evemon_settings_t* */
int   evemon_settings_save(void);

#ifdef __cplusplus
}
#endif

#endif /* evemon_PLUGIN_H */
