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
    COL_START_TIME,    /* epoch seconds (gint64) for sorting   */
    COL_START_TIME_TEXT,/* formatted start-time string          */
    COL_CONTAINER,     /* container runtime label (string)      */
    COL_SERVICE,       /* systemd unit / openrc service name    */
    COL_CWD,
    COL_CMDLINE,
    NUM_COLS
};

/* ── pid_set: tracks user-collapsed PIDs ─────────────────────── */

typedef struct {
    pid_t *pids;
    size_t count;
    size_t capacity;
} pid_set_t;

void pid_set_add(pid_set_t *s, pid_t pid);
void pid_set_remove(pid_set_t *s, pid_t pid);
int  pid_set_contains(const pid_set_t *s, pid_t pid);

/* ── per-UI state ────────────────────────────────────────────── */

typedef struct {
    monitor_state_t    *mon;
    GtkTreeStore       *store;
    GtkTreeView        *view;
    GtkLabel           *status_label;
    GtkLabel           *status_right;
    GtkScrolledWindow  *scroll;
    pid_set_t           collapsed;
    GtkWidget          *menubar;
    GtkWidget          *tree;
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
    GtkLabel           *sb_pid;
    GtkLabel           *sb_ppid;
    GtkLabel           *sb_user;
    GtkLabel           *sb_name;
    GtkLabel           *sb_cpu;
    GtkLabel           *sb_rss;
    GtkLabel           *sb_group_rss;
    GtkLabel           *sb_group_cpu;
    GtkLabel           *sb_start_time;
    GtkLabel           *sb_container;
    GtkLabel           *sb_service;
    GtkLabel           *sb_cwd;
    GtkLabel           *sb_cmdline;

    /* file descriptor list in sidebar */
    GtkTreeStore       *fd_store;
    GtkTreeView        *fd_view;
    GtkWidget          *fd_desc_toggle;
    gboolean            fd_include_desc;
    GtkWidget          *fd_group_dup_toggle;
    gboolean            fd_group_dup_active;
    unsigned            fd_collapsed;      /* bitmask: 1 << cat */
    pid_t               fd_last_pid;

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

    /* name-filter (Ctrl+F / Meta+F) */
    GtkWidget          *filter_entry;
    GtkTreeStore       *filter_store;   /* shadow store for filtered view   */
    GtkTreeModelSort   *sort_model;
    GtkTreeViewColumn  *name_col;
    char                filter_text[256];
    guint               filter_hide_timer; /* auto-hide after idle (0=none) */
} ui_ctx_t;

/* ── fd types ────────────────────────────────────────────────── */

typedef struct {
    int   fd;
    char  path[512];
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

/* ── device labelling ────────────────────────────────────────── */

void label_device(const char *path, char *desc, size_t descsz);

/* ── tree store operations ───────────────────────────────────── */

void update_store(GtkTreeStore *store, GtkTreeView *view,
                  const proc_entry_t *entries, size_t count,
                  const pid_set_t *collapsed);

void populate_store_initial(GtkTreeStore *store, GtkTreeView *view,
                            const proc_entry_t *entries, size_t count);

long compute_group_rss(GtkTreeStore *store, GtkTreeIter *parent);
long compute_group_cpu(GtkTreeStore *store, GtkTreeIter *parent);

/* ── helpers shared across modules ───────────────────────────── */

void     format_memory(long kb, char *buf, size_t bufsz);
void     format_fuzzy_time(time_t epoch, char *buf, size_t bufsz);
gboolean find_iter_by_pid(GtkTreeModel *model, GtkTreeIter *parent,
                          pid_t target, GtkTreeIter *result);

/* ── sidebar ─────────────────────────────────────────────────── */

void sidebar_update(ui_ctx_t *ctx);

/* sidebar signal callbacks (connected by ui.c, defined in sidebar.c) */
void on_fd_desc_toggled(GtkToggleButton *btn, gpointer data);
void on_fd_group_dup_toggled(GtkToggleButton *btn, gpointer data);
void on_fd_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data);
void on_fd_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                        GtkTreePath *path, gpointer data);

/* ── cleanup (fix 6) ─────────────────────────────────────────── */

void ui_ctx_destroy(ui_ctx_t *ctx);

#endif /* UI_INTERNAL_H */
