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
    GtkCssProvider     *fd_css;
    int                 font_size;
    gboolean            auto_font;

    gboolean            follow_selection;

    /* sidebar */
    GtkWidget          *sidebar;
    GtkCheckMenuItem   *sidebar_menu_item;
    GtkWidget          *sidebar_grid;

    /* sidebar section collapse state & containers */
    gboolean            sb_info_collapsed;
    GtkWidget          *sb_info_content;     /* process info grid          */
    GtkWidget          *sb_info_header_arrow; /* ▼/▶ indicator             */

    gboolean            sb_fd_collapsed;
    GtkWidget          *sb_fd_content;       /* fd scroll + toggles box    */
    GtkWidget          *sb_fd_header_arrow;

    gboolean            sb_env_collapsed;
    GtkWidget          *sb_env_content;      /* env scroll                 */
    GtkWidget          *sb_env_header_arrow;

    gboolean            sb_mmap_collapsed;
    GtkWidget          *sb_mmap_content;     /* mmap scroll                */
    GtkWidget          *sb_mmap_header_arrow;

#ifdef HAVE_PIPEWIRE
    gboolean            sb_pw_collapsed;
    GtkWidget          *sb_pw_content;       /* pw scroll                  */
    GtkWidget          *sb_pw_header_arrow;

    gboolean            sb_spectro_collapsed;
    GtkWidget          *sb_spectro_content;
    GtkWidget          *sb_spectro_section;   /* entire section (show/hide) */
    GtkWidget          *sb_spectro_header_arrow;
    gboolean            sb_spectro_user_shown; /* user explicitly opened    */
#endif
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

    /* Steam/Proton metadata in sidebar */
    GtkLabel           *sb_steam_game;
    GtkLabel           *sb_steam_appid;
    GtkLabel           *sb_steam_proton;
    GtkLabel           *sb_steam_runtime;
    GtkLabel           *sb_steam_compat;
    GtkLabel           *sb_steam_gamedir;
    GtkWidget          *sb_steam_frame;     /* container to show/hide */

    /* file descriptor list in sidebar */
    GtkTreeStore       *fd_store;
    GtkTreeView        *fd_view;
    GtkWidget          *fd_desc_toggle;
    gboolean            fd_include_desc;
    GtkWidget          *fd_group_dup_toggle;
    gboolean            fd_group_dup_active;
    unsigned            fd_collapsed;      /* bitmask: 1 << cat */
    pid_t               fd_last_pid;

    /* environment variable list in sidebar */
    GtkTreeStore       *env_store;
    GtkTreeView        *env_view;
    GtkCssProvider     *env_css;
    unsigned            env_collapsed;     /* bitmask: 1 << env_cat */
    pid_t               env_last_pid;
    guint               env_generation;
    GCancellable       *env_cancel;

    /* memory map list in sidebar */
    GtkTreeStore       *mmap_store;
    GtkTreeView        *mmap_view;
    GtkCssProvider     *mmap_css;
    unsigned            mmap_collapsed;    /* bitmask: 1 << mmap_cat */
    pid_t               mmap_last_pid;
    guint               mmap_generation;
    GCancellable       *mmap_cancel;

    /* shared library / DLL list in sidebar */
    GtkTreeStore       *lib_store;
    GtkTreeView        *lib_view;
    GtkCssProvider     *lib_css;
    unsigned            lib_collapsed;     /* bitmask: 1 << lib_cat */
    pid_t               lib_last_pid;
    guint               lib_generation;
    GCancellable       *lib_cancel;

    /* lib section collapse/expand */
    gboolean            sb_lib_collapsed;
    GtkWidget          *sb_lib_content;
    GtkWidget          *sb_lib_header_arrow;
    GtkWidget          *sb_lib_scroll;

    /* network sockets in sidebar (dedicated section) */
    GtkTreeStore       *net_store;
    GtkTreeView        *net_view;
    GtkCssProvider     *net_css;
    pid_t               net_last_pid;
    guint               net_generation;
    GCancellable       *net_cancel;
    GtkWidget          *net_desc_toggle;
    gboolean            net_include_desc;

    /* net section collapse/expand */
    gboolean            sb_net_collapsed;
    GtkWidget          *sb_net_content;
    GtkWidget          *sb_net_header_arrow;
    GtkWidget          *sb_net_scroll;

#ifdef HAVE_PIPEWIRE
    /* PipeWire audio connections in sidebar */
    GtkTreeStore       *pw_store;
    GtkTreeView        *pw_view;
    GtkCssProvider     *pw_css;
    unsigned            pw_collapsed;      /* bitmask: 1 << cat */
    pid_t               pw_last_pid;
    guint               pw_generation;
    GCancellable       *pw_cancel;
    void               *pw_meter;          /* opaque pw_meter_state_t */
    guint               pw_meter_timer;    /* GTK timer for meter updates */

    /* Spectrogram (real-time FFT visualisation of audio stream) */
    GtkWidget          *spectro_draw;      /* GtkDrawingArea         */
    void               *spectro;           /* opaque spectro_state_t */
#endif

    /* middle-click autoscroll */
    gboolean            autoscroll;
    double              anchor_x;
    double              anchor_y;
    double              velocity_x;
    double              velocity_y;
    guint               scroll_timer;

    /* async fd scan state */
    guint               fd_generation;
    GCancellable       *fd_cancel;

    /* startup: fast-poll until first snapshot arrives */
    gboolean            initial_refresh;

    /* highlight animation timer: fires ~60 fps while rows are highlighted */
    guint               highlight_timer;

    /* name-filter (Ctrl+F / Meta+F) */
    GtkWidget          *filter_entry;
    GtkTreeStore       *filter_store;   /* shadow store for filtered view   */
    GtkTreeModelSort   *sort_model;
    GtkTreeViewColumn  *name_col;
    char                filter_text[256];
    guint               filter_hide_timer; /* auto-hide after idle (0=none) */

    /* Go-to-PID (Ctrl+G) */
    GtkWidget          *pid_entry;
    GtkTreeViewColumn  *pid_col;

    /* scroll widgets for drag-to-resize (stored for window-resize clamping) */
    GtkWidget          *sb_fd_scroll;
    GtkWidget          *sb_env_scroll;
    GtkWidget          *sb_mmap_scroll;
#ifdef HAVE_PIPEWIRE
    GtkWidget          *sb_pw_scroll;
#endif

    /* pinned processes */
    pid_t              *pinned_pids;     /* dynamic array of pinned PIDs  */
    size_t              pinned_count;
    size_t              pinned_capacity;
} ui_ctx_t;

/* ── fd types ────────────────────────────────────────────────── */

typedef struct {
    int   fd;
    char  path[512];
    uint64_t net_sort_key;   /* total send+recv bytes for network sort (0 = unsorted) */
} fd_entry_t;

typedef struct {
    fd_entry_t *entries;
    size_t      count;
    size_t      capacity;
} fd_list_t;

void fd_list_init(fd_list_t *l);
void fd_list_free(fd_list_t *l);
void fd_list_push(fd_list_t *l, int fd, const char *path);

/* ── fd classification ───────────────────────────────────────── */

typedef enum {
    FD_CAT_FILES,
    FD_CAT_DEVICES,
    FD_CAT_NET_SOCKETS,
    FD_CAT_UNIX_SOCKETS,
    FD_CAT_OTHER_SOCKETS,
    FD_CAT_PIPES,
    FD_CAT_EVENTS,
    FD_CAT_OTHER,
    FD_CAT_COUNT
} fd_category_t;

enum {
    FD_COL_TEXT,
    FD_COL_MARKUP,
    FD_COL_CAT,
    FD_NUM_COLS
};

extern const char *fd_cat_label[FD_CAT_COUNT];

/* ── socket table ────────────────────────────────────────────── */

typedef enum {
    SOCK_KIND_UNKNOWN,
    SOCK_KIND_TCP,
    SOCK_KIND_TCP6,
    SOCK_KIND_UDP,
    SOCK_KIND_UDP6,
    SOCK_KIND_UNIX,
} sock_kind_t;

typedef struct {
    unsigned long inode;
    sock_kind_t   kind;
    uint32_t      local_addr;
    uint16_t      local_port;
    uint32_t      remote_addr;
    uint16_t      remote_port;
    unsigned char local_addr6[16];
    unsigned char remote_addr6[16];
    uint16_t      local_port6;
    uint16_t      remote_port6;
    char          unix_path[256];
    int           unix_type;
} sock_info_t;

typedef struct {
    sock_info_t *entries;
    size_t       count;
    size_t       capacity;
} sock_table_t;

void              sock_table_build(sock_table_t *out);
void              sock_table_free(sock_table_t *t);
fd_category_t     resolve_socket(unsigned long inode, const sock_table_t *tbl,
                                 char *desc, size_t descsz);
fd_category_t     classify_fd(const char *path);

/* ── /proc fd reading & sorting ──────────────────────────────── */

void read_pid_fds(pid_t pid, fd_list_t *out);
int  fd_entry_path_cmp(const void *a, const void *b);
int  strcmp_trimmed(const char *a, const char *b);
char *fd_path_to_markup(const char *path);

/* ── async fd scan ───────────────────────────────────────────── */

void fd_scan_start(ui_ctx_t *ctx, pid_t pid);

/* ── async network socket scan ───────────────────────────────── */

enum {
    NET_COL_TEXT,        /* plain text (display line)            */
    NET_COL_MARKUP,      /* Pango markup for display             */
    NET_COL_SORT_KEY,    /* sort key: total bytes (gint64)       */
    NET_NUM_COLS
};

void net_scan_start(ui_ctx_t *ctx, pid_t pid);
void on_net_desc_toggled(GtkToggleButton *btn, gpointer data);

/* sidebar signal callbacks for network tree */
void on_net_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data);
void on_net_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data);
gboolean on_net_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data);

/* ── environment variable scanning ───────────────────────────── */

typedef enum {
    ENV_CAT_PATH,        /* PATH, LD_LIBRARY_PATH, etc.         */
    ENV_CAT_DISPLAY,     /* DISPLAY, WAYLAND_DISPLAY, DBUS, etc */
    ENV_CAT_LOCALE,      /* LANG, LC_*, LANGUAGE                */
    ENV_CAT_XDG,         /* XDG_*                               */
    ENV_CAT_STEAM,       /* STEAM_*, PROTON_*, WINE*, SteamApp* */
    ENV_CAT_OTHER,       /* everything else                     */
    ENV_CAT_COUNT
} env_category_t;

enum {
    ENV_COL_TEXT,         /* plain text (KEY=value)              */
    ENV_COL_MARKUP,       /* Pango markup for display            */
    ENV_COL_CAT,          /* env_category_t (-1 for leaf rows)   */
    ENV_NUM_COLS
};

extern const char *env_cat_label[ENV_CAT_COUNT];

void env_scan_start(ui_ctx_t *ctx, pid_t pid);

/* sidebar signal callbacks for env tree (connected in ui.c) */
void on_env_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data);
void on_env_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data);
gboolean on_env_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data);

/* ── memory map scanning ─────────────────────────────────────── */

typedef enum {
    MMAP_CAT_CODE,       /* executable mappings (r-x)           */
    MMAP_CAT_DATA,       /* read-write file-backed              */
    MMAP_CAT_RODATA,     /* read-only file-backed               */
    MMAP_CAT_HEAP,       /* [heap]                              */
    MMAP_CAT_STACK,      /* [stack]                             */
    MMAP_CAT_VDSO,       /* [vdso], [vvar], [vsyscall]          */
    MMAP_CAT_ANON,       /* anonymous (no pathname)             */
    MMAP_CAT_OTHER,      /* everything else                     */
    MMAP_CAT_COUNT
} mmap_category_t;

enum {
    MMAP_COL_TEXT,        /* plain text (display line)           */
    MMAP_COL_MARKUP,      /* Pango markup for display            */
    MMAP_COL_CAT,         /* mmap_category_t (-1 for leaf rows)  */
    MMAP_NUM_COLS
};

extern const char *mmap_cat_label[MMAP_CAT_COUNT];

void mmap_scan_start(ui_ctx_t *ctx, pid_t pid);

/* ── shared library / DLL scanning ────────────────────────────── */

typedef enum {
    LIB_CAT_RUNTIME,       /* ld-linux, libc, libm, libgcc_s, etc.  */
    LIB_CAT_SYSTEM,        /* system-installed .so files             */
    LIB_CAT_APPLICATION,   /* app-shipped .so files                  */
    LIB_CAT_WINE_BUILTIN,  /* Wine/Proton built-in DLLs (.so + .dll)*/
    LIB_CAT_WINDOWS_DLL,   /* real Windows .dll under Wine/Proton    */
    LIB_CAT_OTHER,         /* anything else                          */
    LIB_CAT_COUNT
} lib_category_t;

enum {
    LIB_COL_TEXT,          /* plain text (full path for leaves, header for cats) */
    LIB_COL_MARKUP,        /* Pango markup for display              */
    LIB_COL_CAT,           /* lib_category_t (-1 for leaf rows)     */
    LIB_NUM_COLS
};

extern const char *lib_cat_label[LIB_CAT_COUNT];

void lib_scan_start(ui_ctx_t *ctx, pid_t pid);

/* sidebar signal callbacks for library tree */
void on_lib_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data);
void on_lib_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data);
gboolean on_lib_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data);
gboolean on_lib_query_tooltip(GtkWidget *widget, gint x, gint y,
                              gboolean keyboard_mode, GtkTooltip *tooltip,
                              gpointer data);

/* ── PipeWire audio connection scanning ───────────────────────── */

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
void pw_meter_read(ui_ctx_t *ctx, uint32_t node_id, int *level_l, int *level_r);
GtkCellRenderer *pw_cell_renderer_meter_new(void);

/* spectrogram – real-time audio FFT visualisation */
void spectrogram_start_for_node(ui_ctx_t *ctx, uint32_t node_id);
void spectrogram_stop(ui_ctx_t *ctx);
uint32_t spectrogram_get_target_node(ui_ctx_t *ctx);
gboolean spectrogram_on_draw(GtkWidget *widget, cairo_t *cr, gpointer data);

/* sidebar signal callbacks for PipeWire tree (connected in ui.c) */
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

static inline void spectrogram_start_for_node(void *ctx, uint32_t node_id)
{ (void)ctx; (void)node_id; }
static inline void spectrogram_stop(void *ctx)
{ (void)ctx; }
static inline uint32_t spectrogram_get_target_node(void *ctx)
{ (void)ctx; return 0; }

#endif /* HAVE_PIPEWIRE */

/* sidebar signal callbacks for mmap tree (connected in ui.c) */
void on_mmap_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                           GtkTreePath *path, gpointer data);
void on_mmap_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data);
gboolean on_mmap_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data);

/* ── device labelling ────────────────────────────────────────── */

void label_device(const char *path, char *desc, size_t descsz);

/* ── tree store operations ───────────────────────────────────── */

void update_store(GtkTreeStore *store, GtkTreeView *view,
                  const proc_entry_t *entries, size_t count);

void populate_store_initial(GtkTreeStore *store, GtkTreeView *view,
                            const proc_entry_t *entries, size_t count);

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

/* ── sidebar ─────────────────────────────────────────────────── */

void sidebar_update(ui_ctx_t *ctx);

/* sidebar signal callbacks (connected by ui.c, defined in sidebar.c) */
void on_fd_desc_toggled(GtkToggleButton *btn, gpointer data);
void on_fd_group_dup_toggled(GtkToggleButton *btn, gpointer data);
void on_fd_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data);
void on_fd_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                        GtkTreePath *path, gpointer data);
gboolean on_fd_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data);

/* ── cleanup (fix 6) ─────────────────────────────────────────── */

void ui_ctx_destroy(ui_ctx_t *ctx);

/* ── I/O sparkline custom cell renderer ──────────────────────── */

GtkCellRenderer *sparkline_cell_renderer_new(void);

#endif /* UI_INTERNAL_H */
