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

#include <gtk/gtk.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── ABI version ─────────────────────────────────────────────── */

#define evemon_PLUGIN_ABI_VERSION  1

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
} evemon_data_needs_t;

/* ── Gathered data types ─────────────────────────────────────── */

/* A single file descriptor entry */
typedef struct {
    int       fd;
    char      path[512];
    char      desc[256];         /* resolved label (e.g. device name) */
    int       category;          /* fd_category_t value               */
    uint64_t  net_sort_key;      /* total send+recv bytes for sort    */
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

    /* ── PipeWire spectrogram ─────────────────────────────── */

    /*
     * Start real-time FFT spectrogram capture for a PipeWire node.
     * The host creates a passive capture stream, computes FFT, and
     * renders a scrolling waterfall into the provided GtkDrawingArea.
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
     * Stop the spectrogram and release the capture stream.
     */
    void (*spectro_stop)(void *host_ctx);

    /*
     * Get the currently active spectrogram target node ID.
     * Returns 0 if no spectrogram is running.
     */
    uint32_t (*spectro_get_target)(void *host_ctx);

} evemon_host_services_t;

/* ── Plugin descriptor ───────────────────────────────────────── */

/*
 * Each .so exports a single function:
 *   evemon_plugin_t *evemon_plugin_init(void);
 *
 * The returned struct must be statically allocated (or heap-allocated
 * and never freed before the host calls destroy()).
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
     * Create the plugin's widget tree.  Called on the GTK main thread
     * once per instance.  The returned widget is placed by the host
     * into a tab/panel — the plugin has zero layout awareness.
     *
     * ctx = plugin_ctx from this struct.
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

} evemon_plugin_t;

/* ── Plugin init function signature ──────────────────────────── */

typedef evemon_plugin_t *(*evemon_plugin_init_fn)(void);

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

#ifdef __cplusplus
}
#endif

#endif /* evemon_PLUGIN_H */
