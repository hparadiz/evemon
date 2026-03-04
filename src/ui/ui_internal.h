/*
 * ui_internal.h – shared types and declarations for the ui/ module.
 *
 * This header is private to the UI subsystem.  Only files under src/ui/
 * should include it.
 */
#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "../proc.h"
#include "../profile.h"
#include "../evemon_plugin.h"
#include "../settings.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <time.h>

/* ── tree-store column indices ───────────────────────────────── */
enum {
    COL_PID,
    COL_PPID,
    COL_USER,
    COL_NAME,
    COL_CPU,           /* raw CPU% × 10000 as int for sorting */
    COL_CPU_TEXT,      /* formatted CPU% string for display    */
    COL_RSS,           /* raw KiB value for sorting            */
    COL_RSS_TEXT,      /* formatted string for display         */
    COL_GROUP_RSS,     /* sum of self + children RSS (KiB)     */
    COL_GROUP_RSS_TEXT,/* formatted group RSS string            */
    COL_GROUP_CPU,     /* sum of self + children CPU% × 10000  */
    COL_GROUP_CPU_TEXT,/* formatted group CPU% string           */
    COL_IO_READ_RATE,  /* raw disk read  rate in bytes/sec for sorting  */
    COL_IO_READ_RATE_TEXT, /* formatted disk read rate string              */
    COL_IO_WRITE_RATE, /* raw disk write rate in bytes/sec for sorting  */
    COL_IO_WRITE_RATE_TEXT,/* formatted disk write rate string             */
    COL_START_TIME,    /* epoch seconds (gint64) for sorting   */
    COL_START_TIME_TEXT,/* formatted start-time string          */
    COL_CONTAINER,     /* container runtime label (string)      */
    COL_SERVICE,       /* systemd unit / openrc service name    */
    COL_CWD,
    COL_CMDLINE,
    COL_STEAM_LABEL,   /* Steam/Proton display label (string) */
    COL_IO_SPARKLINE,  /* packed sparkline data (string of semicolon-sep floats) */
    COL_IO_SPARKLINE_PEAK, /* current I/O peak × 1000 for glow animation (int) */
    COL_HIGHLIGHT_BORN,/* monotonic µs when row first appeared (gint64, 0=none)  */
    COL_HIGHLIGHT_DIED,/* monotonic µs when process vanished (gint64, 0=alive)   */
    COL_PINNED_ROOT,   /* pid_t of the pinned root, or -1 for normal tree */
    NUM_COLS
};

/* ── process tree node state tracking ─────────────────────────── */

/* Highlight fade duration (microseconds).  New/dying rows are
 * highlighted for this many µs before the colour fades completely. */
#define HIGHLIGHT_FADE_US  (2 * 1000000LL)   /* 2 seconds */

/* Node states (stored per-PID in the set) */
#define PTREE_EXPANDED  0
#define PTREE_COLLAPSED 1

/*
 * Pinned-PID sentinel: used as the pinned_pid value for rows that
 * belong to the main (unpinned) process tree.
 */
#define PTREE_UNPINNED  ((pid_t)-1)

typedef struct {
    pid_t *pinned_pids; /* owning pinned-process PID (PTREE_UNPINNED for main tree) */
    pid_t *pids;        /* the actual process PID                                  */
    int   *states;      /* PTREE_COLLAPSED or PTREE_EXPANDED                       */
    size_t count;
    size_t capacity;
} ptree_node_set_t;

void set_process_tree_node(ptree_node_set_t *s, pid_t pinned_pid,
                           pid_t pid, int state);
int  get_process_tree_node(const ptree_node_set_t *s, pid_t pinned_pid,
                           pid_t pid);

/* ── pinned process detail panel ───────────────────────────────── */

/*
 * Per-pinned-process detail panel.  Each pinned PID gets a complete
 * clone of the main detail panel: collapsible process-info tray
 * with all metadata labels, a full plugin notebook (including
 * headless audio service + milkdrop), and independent data tracking.
 */
typedef struct pinned_panel pinned_panel_t;
struct pinned_panel {
    pid_t              pid;             /* pinned PID                     */

    /* GTK widget hierarchy */
    GtkWidget         *outer_frame;     /* top-level container in pinned_box */
    GtkWidget         *detail_hpaned;   /* horizontal paned: tray | notebook */

    /* Collapsible process info tray (clone of main panel) */
    GtkWidget         *sidebar_frame;   /* the frame holding sidebar_scroll  */
    GtkWidget         *proc_info_revealer;
    GtkWidget         *proc_info_toggle;
    GtkLabel          *proc_info_summary;
    gboolean           proc_info_collapsed;

    /* Process info labels (same fields as main ctx->sb_*) */
    GtkLabel          *sb_pid;
    GtkLabel          *sb_ppid;
    GtkLabel          *sb_user;
    GtkLabel          *sb_name;
    GtkLabel          *sb_cpu;
    GtkLabel          *sb_rss;
    GtkLabel          *sb_group_rss;
    GtkLabel          *sb_group_cpu;
    GtkLabel          *sb_io_read;
    GtkLabel          *sb_io_write;
    GtkLabel          *sb_net_send;
    GtkLabel          *sb_net_recv;
    GtkLabel          *sb_start_time;
    GtkLabel          *sb_container;
    GtkLabel          *sb_service;
    GtkLabel          *sb_cwd;
    GtkLabel          *sb_cmdline;

    /* Steam/Proton metadata */
    GtkLabel          *sb_steam_game;
    GtkLabel          *sb_steam_appid;
    GtkLabel          *sb_steam_proton;
    GtkLabel          *sb_steam_runtime;
    GtkLabel          *sb_steam_compat;
    GtkLabel          *sb_steam_gamedir;
    GtkWidget         *sb_steam_frame;

    /* cgroup resource limits */
    GtkLabel          *sb_cgroup_path;
    GtkLabel          *sb_cgroup_mem;
    GtkLabel          *sb_cgroup_mem_high;
    GtkWidget         *sb_cgroup_mem_high_key;
    GtkLabel          *sb_cgroup_swap;
    GtkWidget         *sb_cgroup_swap_key;
    GtkLabel          *sb_cgroup_cpu;
    GtkLabel          *sb_cgroup_pids;
    GtkLabel          *sb_cgroup_io;
    GtkWidget         *sb_cgroup_io_key;
    GtkWidget         *sb_cgroup_frame;

    /* Plugin system (independent per panel) */
    GtkWidget         *notebook;        /* plugin notebook for this PID   */
    int               *instance_ids;    /* plugin instance IDs we created */
    size_t             n_instances;     /* count of instance IDs          */
};

/* ── floating plugin window ───────────────────────────────────── */

/*
 * A single plugin instance opened as its own top-level window.
 * Created by right-click → "Open Plugin as Window".
 * Destroyed when the user closes the window.
 */
typedef struct {
    int        instance_id;   /* plugin_registry instance ID      */
    pid_t      pid;           /* PID the plugin is tracking       */
    GtkWidget *window;        /* the GtkWindow                    */
} plugin_window_t;

/* ── detail panel dock position ───────────────────────────────── */

typedef enum {
    PANEL_POS_BOTTOM,
    PANEL_POS_TOP,
    PANEL_POS_LEFT,
    PANEL_POS_RIGHT,
} panel_position_t;

/* ── per-UI state ────────────────────────────────────────────── */

typedef struct {
    monitor_state_t    *mon;
    GtkTreeStore       *store;
    GtkTreeView        *view;
    GtkLabel           *status_label;
    GtkLabel           *status_right;
    GtkScrolledWindow  *scroll;
    ptree_node_set_t    ptree_nodes;
    GtkWidget          *menubar;
    GtkWidget          *file_menu_item;
    GtkWidget          *tree;
    gboolean            alt_pressed;     /* bare Alt-tap detection */
    GtkCssProvider     *css;
    GtkCssProvider     *sidebar_css;
    GtkCssProvider     *plugin_css;
    GtkCssProvider     *filter_css;
    GtkCssProvider     *pid_css;
    int                 font_size;

    gboolean            follow_selection;

    /* process detail panel */
    GtkWidget          *sidebar;
    GtkWidget          *sidebar_grid;

    /* detail panel section collapse state & containers */
    gboolean            sb_info_collapsed;
    GtkWidget          *sb_info_content;     /* process info grid          */
    GtkWidget          *sb_info_header_arrow; /* ▼/▶ indicator             */

    GtkLabel           *sb_pid;
    GtkLabel           *sb_ppid;
    GtkLabel           *sb_user;
    GtkLabel           *sb_name;
    GtkLabel           *sb_cpu;
    GtkLabel           *sb_rss;
    GtkLabel           *sb_group_rss;
    GtkLabel           *sb_group_cpu;
    GtkLabel           *sb_io_read;
    GtkLabel           *sb_io_write;
    GtkLabel           *sb_net_send;
    GtkLabel           *sb_net_recv;
    GtkLabel           *sb_start_time;
    GtkLabel           *sb_container;
    GtkLabel           *sb_service;
    GtkLabel           *sb_cwd;
    GtkLabel           *sb_cmdline;

    /* Steam/Proton metadata in detail panel */
    GtkLabel           *sb_steam_game;
    GtkLabel           *sb_steam_appid;
    GtkLabel           *sb_steam_proton;
    GtkLabel           *sb_steam_runtime;
    GtkLabel           *sb_steam_compat;
    GtkLabel           *sb_steam_gamedir;
    GtkWidget          *sb_steam_frame;     /* container to show/hide */

    /* cgroup resource limits in detail panel */
    GtkLabel           *sb_cgroup_path;
    GtkLabel           *sb_cgroup_mem;
    GtkLabel           *sb_cgroup_mem_high;
    GtkWidget          *sb_cgroup_mem_high_key;  /* key label (hide/show) */
    GtkLabel           *sb_cgroup_swap;
    GtkWidget          *sb_cgroup_swap_key;
    GtkLabel           *sb_cgroup_cpu;
    GtkLabel           *sb_cgroup_pids;
    GtkLabel           *sb_cgroup_io;
    GtkWidget          *sb_cgroup_io_key;
    GtkWidget          *sb_cgroup_frame;     /* container to show/hide */
    guint               cgroup_generation;
    GCancellable       *cgroup_cancel;

#ifdef HAVE_PIPEWIRE
    /* PipeWire detail panel scan state (used by pipewire_scan.c) */
    GtkTreeStore       *pw_store;
    GtkTreeView        *pw_view;
    GtkCssProvider     *pw_css;
    unsigned            pw_collapsed;      /* bitmask: 1 << pw_cat */
    pid_t               pw_last_pid;
    guint               pw_generation;
    GCancellable       *pw_cancel;

    /* PipeWire host services (meter + spectrogram) */
    void               *pw_meter;          /* opaque pw_meter_state_t */
    guint               pw_meter_timer;    /* GTK timer for meter updates */

    /* Spectrogram instances (per-draw-area, no longer a singleton).
     * Each active spectrogram has its own PW capture stream, FFT state,
     * and waterfall image.  The array grows as needed. */
#define SPECTRO_MAX_INSTANCES 16
    struct {
        GtkWidget *draw_area;          /* key: GtkDrawingArea*       */
        void      *state;              /* spectro_state_t*           */
    } spectro_instances[SPECTRO_MAX_INSTANCES];
    size_t              spectro_count;
    gboolean            sb_spectro_user_shown; /* user explicitly showed spectrogram */
#endif

    /* middle-click autoscroll */
    gboolean            autoscroll;
    double              anchor_x;
    double              anchor_y;
    double              velocity_x;
    double              velocity_y;
    guint               scroll_timer;

    /* startup: fast-poll until first snapshot arrives */
    gboolean            initial_refresh;

    /* set when on_destroy fires so timers don't touch dead widgets */
    gboolean            shutting_down;

    /* audio PID set: PIDs with active PipeWire audio streams */
    pid_t              *audio_pids;
    size_t              audio_pid_count;
    size_t              audio_pid_cap;
    gboolean            has_audio_plugin;  /* TRUE if audio service plugin loaded */

    /* highlight animation timer: fires ~60 fps while rows are highlighted */
    guint               highlight_timer;

    /* name-filter (Ctrl+F / Meta+F) */
    GtkWidget          *filter_entry;
    GtkTreeStore       *filter_store;   /* shadow store for filtered view   */
    GtkTreeModelSort   *sort_model;
    GtkTreeViewColumn  *name_col;
    char                filter_text[256];
    guint               filter_hide_timer; /* auto-hide after idle (0=none) */
    gboolean            show_audio_only;  /* TRUE = show only audio processes */

    /* Go-to-PID (Ctrl+G) */
    GtkWidget          *pid_entry;
    GtkTreeViewColumn  *pid_col;

    /* pinned processes */
    pid_t              *pinned_pids;     /* dynamic array of pinned PIDs  */
    size_t              pinned_count;
    size_t              pinned_capacity;

    /* pinned detail panels (one per pinned PID) */
    pinned_panel_t     *pinned_panels;   /* dynamic array                 */
    size_t              pinned_panel_count;
    size_t              pinned_panel_cap;
    GtkWidget          *pinned_box;      /* GtkBox stacking pinned panels */

    /* plugin system */
    void               *plugin_registry; /* plugin_registry_t* (opaque to avoid header dep) */
    GtkWidget          *plugin_notebook; /* GtkNotebook holding plugin tabs */
    void               *plugin_broker;   /* broker state (opaque void*)     */
    char                plugin_dir[4096]; /* directory scanned for .so files */
    GtkWidget          *plugins_menu_item; /* top-level "Plugins" menu item  */

    /* floating plugin windows (right-click → Open Plugin as Window) */
    plugin_window_t    *plugin_windows;  /* dynamic array                  */
    size_t              plugin_window_count;
    size_t              plugin_window_cap;

    /* detail panel (detachable plugin notebook) */
    GtkWidget          *detail_panel;         /* the frame wrapping the notebook  */
    GtkWidget          *detail_vbox;          /* vbox: detail_panel + pinned_box  */
    panel_position_t    detail_panel_pos;      /* current dock position            */
    GtkCheckMenuItem   *detail_panel_menu_item;/* View → Detail Panel toggle       */
    GtkWidget          *detail_paned;          /* current GtkPaned holding panel   */
    GtkWidget          *main_content;          /* tree_overlay (stable reference)  */
    GtkWidget          *content_box;           /* vbox holding menubar+content+status */
    GtkWidget          *hpaned;                /* tree paned (tree | detail panel)  */
    GSList             *panel_pos_group;        /* radio group for position items   */

    /* collapsible process info tray */
    GtkWidget          *proc_info_revealer;   /* GtkRevealer wrapping sidebar_frame */
    GtkWidget          *proc_info_toggle;     /* toggle button (▶/◀)               */
    GtkLabel           *proc_info_summary;    /* compact summary shown when collapsed */
    gboolean            proc_info_collapsed;  /* TRUE when process info is hidden   */
} ui_ctx_t;

/* ── PipeWire audio connection scanning & host services ───────── */

#ifdef HAVE_PIPEWIRE

enum {
    PW_COL_TEXT,          /* plain text (display line)           */
    PW_COL_MARKUP,        /* Pango markup for display            */
    PW_COL_CAT,           /* category (-1 for leaf rows)         */
    PW_COL_NODE_ID,       /* PipeWire node ID (uint, leaf rows)  */
    PW_COL_LEVEL_L,       /* left  peak level, 0..1000 (int)     */
    PW_COL_LEVEL_R,       /* right peak level, 0..1000 (int)     */
    PW_NUM_COLS
};

void pipewire_scan_start(ui_ctx_t *ctx, pid_t pid);

/* peak meter – real-time L/R level monitoring for audio nodes */
void pw_meter_start(ui_ctx_t *ctx, const uint32_t *node_ids, size_t count);
void pw_meter_stop(ui_ctx_t *ctx);
void pw_meter_remove_nodes(ui_ctx_t *ctx, const uint32_t *node_ids,
                           size_t count);
void pw_meter_read(ui_ctx_t *ctx, uint32_t node_id, int *level_l, int *level_r);
GtkCellRenderer *pw_cell_renderer_meter_new(void);

/* spectrogram – real-time audio FFT visualisation */
typedef enum {
    SPECTRO_THEME_CLASSIC = 0,
    SPECTRO_THEME_HEAT,
    SPECTRO_THEME_COOL,
    SPECTRO_THEME_GREYSCALE,
    SPECTRO_THEME_NEON,
    SPECTRO_THEME_VIRIDIS,      /* vaporwave: navy → purple → pink → cyan → white */
    SPECTRO_NUM_THEMES
} spectro_theme_t;

void spectrogram_start_for_node(ui_ctx_t *ctx, GtkDrawingArea *draw_area,
                                uint32_t node_id);
void spectrogram_stop(ui_ctx_t *ctx, GtkDrawingArea *draw_area);
uint32_t spectrogram_get_target_node(ui_ctx_t *ctx, GtkDrawingArea *draw_area);
void spectrogram_set_theme(ui_ctx_t *ctx, GtkDrawingArea *draw_area,
                           spectro_theme_t theme);
gboolean spectrogram_on_draw(GtkWidget *widget, cairo_t *cr, gpointer data);

/* detail panel signal callbacks for PipeWire tree */
void on_pw_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data);
void on_pw_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                        GtkTreePath *path, gpointer data);
gboolean on_pw_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data);
void on_pw_row_activated(GtkTreeView *view, GtkTreePath *path,
                         GtkTreeViewColumn *col, gpointer data);

#else

static inline void pipewire_scan_start(void *ctx, pid_t pid)
{ (void)ctx; (void)pid; }

static inline void spectrogram_start_for_node(void *ctx,
                                              GtkDrawingArea *da,
                                              uint32_t node_id)
{ (void)ctx; (void)da; (void)node_id; }
static inline void spectrogram_stop(void *ctx, GtkDrawingArea *da)
{ (void)ctx; (void)da; }
static inline uint32_t spectrogram_get_target_node(void *ctx,
                                                   GtkDrawingArea *da)
{ (void)ctx; (void)da; return 0; }

#endif /* HAVE_PIPEWIRE */

/* ── cgroup detail panel scan ────────────────────────────────── */

void cgroup_scan_start(ui_ctx_t *ctx, pid_t pid);

/*
 * Synchronous cgroup update for pinned panels.
 * Reads cgroup limits for the given PID and writes results directly
 * to the provided labels (avoids needing full ui_ctx_t pointer).
 */
typedef struct {
    GtkLabel  *path;
    GtkLabel  *mem;
    GtkLabel  *mem_high;
    GtkWidget *mem_high_key;
    GtkLabel  *swap;
    GtkWidget *swap_key;
    GtkLabel  *cpu;
    GtkLabel  *pids;
    GtkLabel  *io;
    GtkWidget *io_key;
    GtkWidget *frame;       /* the cgroup section container */
} cgroup_label_set_t;

void cgroup_update_labels(pid_t pid, const cgroup_label_set_t *ls);

/* ── device labelling ────────────────────────────────────────── */

void label_device(const char *path, char *desc, size_t descsz);

/* ── tree store operations ───────────────────────────────────── */

void update_store(GtkTreeStore *store, GtkTreeView *view,
                  const proc_entry_t *entries, size_t count);

void populate_store_initial(GtkTreeStore *store, GtkTreeView *view,
                            const proc_entry_t *entries, size_t count,
                            pid_t preselect_pid, ui_ctx_t *ctx);

long compute_group_rss(GtkTreeStore *store, GtkTreeIter *parent);
long compute_group_cpu(GtkTreeStore *store, GtkTreeIter *parent);

void rebuild_pinned_rows(GtkTreeStore *store,
                         const pid_t *pinned_pids, size_t pinned_count);

void remove_pinned_rows(GtkTreeStore *store);

/* ── helpers shared across modules ───────────────────────────── */

void     format_memory(long kb, char *buf, size_t bufsz);
void     format_fuzzy_time(time_t epoch, char *buf, size_t bufsz);
gboolean find_iter_by_pid(GtkTreeModel *model, GtkTreeIter *parent,
                          pid_t target, GtkTreeIter *result);
void     collect_descendant_pids(GtkTreeModel *model, GtkTreeIter *parent,
                                pid_t **out, size_t *out_count,
                                size_t *out_cap);
pid_t    get_row_pinned_root(GtkTreeModel *model, GtkTreeIter *iter);

/* ── process detail panel ────────────────────────────────────── */

void proc_detail_update(ui_ctx_t *ctx);

/* ── pin toggle callback data ───────────────────────────────── */
typedef struct {
    ui_ctx_t *ctx;
    pid_t     pid;
} pin_toggle_data_t;

/* ── plugin availability macro ───────────────────────────────── */
#ifndef PLUGIN_IS_AVAILABLE
#define PLUGIN_IS_AVAILABLE(inst) \
    (!((inst)->plugin) || \
     !(inst)->plugin->is_available || \
     (inst)->plugin->is_available((inst)->plugin->plugin_ctx))
#endif

/* ── autoscroll constants ────────────────────────────────────── */
#define AUTOSCROLL_DEADZONE_FRAC 0.03
#define AUTOSCROLL_INTERVAL      16      /* ~60 fps */
#define AUTOSCROLL_SCALE         12.0

/* ── font size constants ─────────────────────────────────────── */
#define FONT_SIZE_MIN     6
#define FONT_SIZE_MAX     30
#define FONT_SIZE_DEFAULT 9

/* ── pinned detail panels (ui_pinned.c) ──────────────────────── */

/* Update all pinned panels' header labels from tree-model data */
void pinned_panels_update(ui_ctx_t *ctx);

gboolean pid_is_pinned(const ui_ctx_t *ctx, pid_t pid);
void     pin_pid(ui_ctx_t *ctx, pid_t pid);
void     unpin_pid(ui_ctx_t *ctx, pid_t pid);
void     on_toggle_pin(GtkMenuItem *item, gpointer data);
void     pinned_panel_create(ui_ctx_t *ctx, pid_t pid);
void     pinned_panel_destroy(ui_ctx_t *ctx, pid_t pid);/* ── plugin window management (ui_plugins.c) ─────────────────── */

void     open_plugin_window(ui_ctx_t *ctx, pid_t pid,
                            const char *proc_name, const char *plugin_id);
gboolean on_plugin_window_delete(GtkWidget *window, GdkEvent *ev,
                                 gpointer data);
void     on_plugin_toggled(GtkCheckMenuItem *item, gpointer data);
void     on_plugins_menu_map(GtkWidget *menu, gpointer data);
int      inst_is_last_order(const evemon_plugin_t *p, const char *list[]);
void     show_open_plugin_as_window_menu(ui_ctx_t *ctx, GtkWidget *menu,
                                         pid_t pid, const char *name);

/* ── filter store (ui_filter.c) ──────────────────────────────── */

void     copy_subtree(GtkTreeStore *dst, GtkTreeIter *dst_parent,
                      GtkTreeModel *src, GtkTreeIter *src_iter);
void     rebuild_filter_store(ui_ctx_t *ctx);
void     sync_filter_store(ui_ctx_t *ctx);
void     switch_to_real_store(ui_ctx_t *ctx);
void     rebuild_audio_filter_store(ui_ctx_t *ctx);
void     sync_audio_filter_store(ui_ctx_t *ctx);
void     expand_respecting_collapsed(ui_ctx_t *ctx);
void     expand_respecting_collapsed_recurse(ui_ctx_t *ctx,
                                             GtkTreeModel *model,
                                             GtkTreeIter *parent);
void     register_sort_funcs(GtkTreeModelSort *sm);
void     on_sort_column_changed(GtkTreeSortable *sortable, gpointer data);
void     show_process_context_menu(ui_ctx_t *ctx, GdkEventButton *ev,
                                   pid_t pid, const char *name,
                                   const char *cmdline);
void     reload_font_css(ui_ctx_t *ctx);

/* ── input handlers (ui_input.c) ─────────────────────────────── */

void     filter_cancel_hide_timer(ui_ctx_t *ctx);
void     goto_pid(ui_ctx_t *ctx, pid_t pid);
void     stop_autoscroll(ui_ctx_t *ctx);
gboolean autoscroll_tick(gpointer data);

gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data);
gboolean on_key_release(GtkWidget *w, GdkEventKey *ev, gpointer data);
gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data);
gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data);
gboolean on_motion_notify(GtkWidget *w, GdkEventMotion *ev, gpointer data);
gboolean on_focus_out(GtkWidget *w, GdkEventFocus *ev, gpointer data);
gboolean on_filter_entry_key_release(GtkWidget *w, GdkEventKey *ev,
                                     gpointer data);
void     on_pid_entry_insert_text(GtkEditable *e, const gchar *text,
                                  gint length, gint *position, gpointer data);
gboolean on_pid_entry_key_release(GtkWidget *w, GdkEventKey *ev,
                                  gpointer data);
gboolean on_overlay_get_child_position(GtkOverlay *overlay, GtkWidget *child,
                                       GdkRectangle *alloc, gpointer data);
void     on_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data);
void     on_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data);

/* ── audio PID probing ────────────────────────────────────────── */

/* Refresh the set of PIDs with active audio streams (PipeWire). */
void audio_pids_refresh(ui_ctx_t *ctx);

/* Returns TRUE if the given PID has an active audio stream. */
static inline gboolean audio_pid_is_active(const ui_ctx_t *ctx, pid_t pid)
{
    for (size_t i = 0; i < ctx->audio_pid_count; i++)
        if (ctx->audio_pids[i] == pid) return TRUE;
    return FALSE;
}

/* ── cleanup (fix 6) ─────────────────────────────────────────── */

void ui_ctx_destroy(ui_ctx_t *ctx);

/* ── I/O sparkline custom cell renderer ──────────────────────── */

GtkCellRenderer *sparkline_cell_renderer_new(void);

#endif /* UI_INTERNAL_H */
