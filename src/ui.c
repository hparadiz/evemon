/*
 * ui.c – GTK3 process-tree UI.
 *
 * Displays processes in a hierarchical GtkTreeView (like Sysinternals
 * Process Explorer / procmon on Windows).  Each process is nested under
 * its parent so you can expand/collapse entire process subtrees.
 *
 * A GLib timeout fires every ~1 s, grabs the latest snapshot from the
 * monitor thread, rebuilds the GtkTreeStore, and re-expands any rows
 * the user had open.
 */

#include "proc.h"
#include "profile.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utmpx.h>

/* ── tree-store column indices ───────────────────────────────── */
enum {
    COL_PID,
    COL_PPID,
    COL_USER,
    COL_NAME,
    COL_CPU,          /* raw CPU% × 10000 as int for sorting */
    COL_CPU_TEXT,     /* formatted CPU% string for display  */
    COL_RSS,          /* raw KiB value for sorting   */
    COL_RSS_TEXT,     /* formatted string for display */
    COL_GROUP_RSS,    /* sum of self + children RSS (KiB) for sorting */
    COL_GROUP_RSS_TEXT,/* formatted group RSS string for display       */
    COL_GROUP_CPU,     /* sum of self + children CPU% × 10000 for sorting */
    COL_GROUP_CPU_TEXT,/* formatted group CPU% string for display       */
    COL_CWD,
    COL_CMDLINE,
    NUM_COLS
};

/* ── per-UI state passed through the timeout ─────────────────── */

/*
 * Track PIDs the user has manually collapsed.  Default behaviour is
 * expand-all; any PID in this set stays collapsed across refreshes.
 */
typedef struct {
    pid_t *pids;
    size_t count;
    size_t capacity;
} pid_set_t;

static void pid_set_add(pid_set_t *s, pid_t pid)
{
    /* avoid duplicates */
    for (size_t i = 0; i < s->count; i++)
        if (s->pids[i] == pid) return;

    if (s->count >= s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 64;
        s->pids = realloc(s->pids, s->capacity * sizeof(pid_t));
    }
    s->pids[s->count++] = pid;
}

static void pid_set_remove(pid_set_t *s, pid_t pid)
{
    for (size_t i = 0; i < s->count; i++) {
        if (s->pids[i] == pid) {
            s->pids[i] = s->pids[--s->count];
            return;
        }
    }
}

static int pid_set_contains(const pid_set_t *s, pid_t pid)
{
    for (size_t i = 0; i < s->count; i++)
        if (s->pids[i] == pid) return 1;
    return 0;
}

typedef struct {
    monitor_state_t    *mon;
    GtkTreeStore       *store;
    GtkTreeView        *view;
    GtkLabel           *status_label;
    GtkLabel           *status_right;  /* right side: uptime / users / load   */
    GtkScrolledWindow  *scroll;
    pid_set_t           collapsed;     /* PIDs the user has manually collapsed */
    GtkWidget          *menubar;       /* toggleable menu bar                 */
    GtkWidget          *tree;          /* the GtkTreeView widget              */
    GtkCssProvider     *css;           /* live CSS provider for font changes  */
    int                 font_size;     /* current font size in pt             */
    gboolean            auto_font;     /* auto-scale font with window size    */

    /* scroll-follow: track selected row only after sort click */
    gboolean            follow_selection;  /* TRUE = keep selected row in view */

    /* middle-click autoscroll state */
    gboolean            autoscroll;      /* TRUE while middle-button held  */
    double              anchor_x;        /* root coords of initial click   */
    double              anchor_y;
    double              velocity_x;      /* current scroll speed (px/tick) */
    double              velocity_y;
    guint               scroll_timer;    /* g_timeout source ID, 0 = none  */

} ui_ctx_t;

/* ── helpers ─────────────────────────────────────────────────── */

/* Format a KiB value into a human-readable string. */
static void format_memory(long kb, char *buf, size_t bufsz)
{
    if (kb <= 0)
        snprintf(buf, bufsz, "–");
    else if (kb < 1024)
        snprintf(buf, bufsz, "%ld KiB", kb);
    else if (kb < 1024 * 1024)
        snprintf(buf, bufsz, "%.1f MiB", (double)kb / 1024.0);
    else
        snprintf(buf, bufsz, "%.2f GiB", (double)kb / (1024.0 * 1024.0));
}

/* ── hash table for PID lookups ──────────────────────────────── */

#define HT_SIZE 8192

typedef struct { pid_t pid; size_t idx; int used; } ht_entry_t;

static void ht_insert(ht_entry_t *ht, pid_t pid, size_t idx)
{
    unsigned h = (unsigned)pid % HT_SIZE;
    while (ht[h].used)
        h = (h + 1) % HT_SIZE;
    ht[h].pid  = pid;
    ht[h].idx  = idx;
    ht[h].used = 1;
}

static size_t ht_find(const ht_entry_t *ht, pid_t pid)
{
    unsigned h = (unsigned)pid % HT_SIZE;
    for (int k = 0; k < HT_SIZE; k++) {
        if (!ht[h].used) return (size_t)-1;
        if (ht[h].pid == pid) return ht[h].idx;
        h = (h + 1) % HT_SIZE;
    }
    return (size_t)-1;
}

/* ── iter map: PID → GtkTreeIter for existing rows ───────────── */

typedef struct { pid_t pid; GtkTreeIter iter; } iter_map_entry_t;

typedef struct {
    iter_map_entry_t *entries;
    size_t            count;
    size_t            capacity;
} iter_map_t;

static void iter_map_add(iter_map_t *m, pid_t pid, GtkTreeIter *iter)
{
    if (m->count >= m->capacity) {
        m->capacity = m->capacity ? m->capacity * 2 : 256;
        m->entries = realloc(m->entries, m->capacity * sizeof(iter_map_entry_t));
    }
    m->entries[m->count].pid  = pid;
    m->entries[m->count].iter = *iter;
    m->count++;
}

static GtkTreeIter *iter_map_find(iter_map_t *m, pid_t pid)
{
    for (size_t i = 0; i < m->count; i++)
        if (m->entries[i].pid == pid) return &m->entries[i].iter;
    return NULL;
}

/* ── collect existing tree rows into iter_map (recursive) ────── */

static void collect_iters(GtkTreeModel *model, GtkTreeIter *parent,
                          iter_map_t *map)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint pid;
        gtk_tree_model_get(model, &iter, COL_PID, &pid, -1);
        iter_map_add(map, (pid_t)pid, &iter);
        collect_iters(model, &iter, map);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

/* ── remove dead rows (recursive, bottom-up) ─────────────────── */

static void remove_dead_rows(GtkTreeStore *store, GtkTreeIter *parent,
                             const ht_entry_t *new_ht)
{
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);

    while (valid) {
        /* Recurse into children first (bottom-up removal) */
        remove_dead_rows(store, &iter, new_ht);

        gint pid;
        gtk_tree_model_get(model, &iter, COL_PID, &pid, -1);

        if (ht_find(new_ht, (pid_t)pid) == (size_t)-1) {
            /* Process gone – remove row (invalidates iter, returns next) */
            valid = gtk_tree_store_remove(store, &iter);
        } else {
            valid = gtk_tree_model_iter_next(model, &iter);
        }
    }
}

/* ── set row data from a proc_entry ──────────────────────────── */

static void set_row_data(GtkTreeStore *store, GtkTreeIter *iter,
                         const proc_entry_t *e)
{
    char rss_text[64];
    format_memory(e->mem_rss_kb, rss_text, sizeof(rss_text));

    char cpu_text[32];
    if (e->cpu_percent < 0.05)
        snprintf(cpu_text, sizeof(cpu_text), "0.0%%");
    else
        snprintf(cpu_text, sizeof(cpu_text), "%.1f%%", e->cpu_percent);

    gtk_tree_store_set(store, iter,
                       COL_PID,      (gint)e->pid,
                       COL_PPID,     (gint)e->ppid,
                       COL_USER,     e->user,
                       COL_NAME,     e->name,
                       COL_CPU,      (gint)(e->cpu_percent * 10000),
                       COL_CPU_TEXT, cpu_text,
                       COL_RSS,      (gint)(e->mem_rss_kb),
                       COL_RSS_TEXT, rss_text,
                       COL_GROUP_RSS,      (gint)0,
                       COL_GROUP_RSS_TEXT, "–",
                       COL_GROUP_CPU,      (gint)0,
                       COL_GROUP_CPU_TEXT, "0.0%",
                       COL_CWD,      e->cwd,
                       COL_CMDLINE,  e->cmdline,
                       -1);
}

/* ── find the parent iter for a given ppid ───────────────────── */

static GtkTreeIter *find_parent_iter(iter_map_t *map, pid_t ppid, pid_t self_pid)
{
    if (ppid <= 0 || ppid == self_pid)
        return NULL;
    return iter_map_find(map, ppid);
}

/*
 * Incremental update: diff the new snapshot against the existing tree.
 *   1. Remove rows for processes that no longer exist.
 *   2. Update existing rows in-place.
 *   3. Insert new processes under the correct parent.
 *
 * This avoids clearing the store, so there's no visual flash, and
 * scroll position / expand state / selection are all preserved.
 */
static void update_store(GtkTreeStore       *store,
                         GtkTreeView        *view,
                         const proc_entry_t *entries,
                         size_t              count,
                         const pid_set_t    *collapsed)
{
    /* Build hash of new snapshot: PID → index */
    ht_entry_t *new_ht = calloc(HT_SIZE, sizeof(ht_entry_t));
    if (!new_ht) return;
    for (size_t i = 0; i < count; i++)
        ht_insert(new_ht, entries[i].pid, i);

    /* Phase 1: Remove dead rows */
    remove_dead_rows(store, NULL, new_ht);

    /* Phase 2: Collect remaining existing rows */
    iter_map_t existing = { NULL, 0, 0 };
    collect_iters(GTK_TREE_MODEL(store), NULL, &existing);

    /* Build a hash of existing PIDs for quick "already exists?" check */
    ht_entry_t *old_ht = calloc(HT_SIZE, sizeof(ht_entry_t));
    if (!old_ht) { free(new_ht); free(existing.entries); return; }
    for (size_t i = 0; i < existing.count; i++)
        ht_insert(old_ht, existing.entries[i].pid, i);

    /* Phase 3: Update existing rows in-place */
    for (size_t i = 0; i < existing.count; i++) {
        pid_t pid = existing.entries[i].pid;
        size_t sidx = ht_find(new_ht, pid);
        if (sidx != (size_t)-1)
            set_row_data(store, &existing.entries[i].iter, &entries[sidx]);
    }

    /* Phase 4: Insert new processes.
     * We need to insert parents before children, so we use the same
     * ancestor-stack approach as before. */
    int *inserted = calloc(count, sizeof(int));
    if (!inserted) { free(new_ht); free(old_ht); free(existing.entries); return; }

    /* Mark already-existing entries as inserted */
    for (size_t i = 0; i < count; i++) {
        if (ht_find(old_ht, entries[i].pid) != (size_t)-1)
            inserted[i] = 1;
    }

    pid_t stack[64];
    int sp;

    for (size_t i = 0; i < count; i++) {
        if (inserted[i]) continue;

        /* Build ancestor stack */
        sp = 0;
        size_t cur = i;
        while (!inserted[cur]) {
            if (sp >= 64) break;
            stack[sp++] = entries[cur].pid;

            pid_t pp = entries[cur].ppid;
            size_t pidx = ht_find(new_ht, pp);
            if (pidx == (size_t)-1 || pidx == cur) break;
            if (inserted[pidx]) break;
            cur = pidx;
        }

        /* Pop stack: insert outermost ancestor first */
        while (sp > 0) {
            pid_t p = stack[--sp];
            size_t sidx = ht_find(new_ht, p);
            if (sidx == (size_t)-1 || inserted[sidx]) continue;

            const proc_entry_t *e = &entries[sidx];

            /* Find parent iter – check both existing map and freshly
             * inserted entries (which we add to existing as we go). */
            GtkTreeIter *parent_iter = find_parent_iter(&existing,
                                                         e->ppid, e->pid);

            GtkTreeIter new_iter;
            gtk_tree_store_append(store, &new_iter, parent_iter);
            set_row_data(store, &new_iter, e);

            /* Add to existing map so children can find us */
            iter_map_add(&existing, e->pid, &new_iter);
            inserted[sidx] = 1;

            /* Expand new row unless user has it collapsed */
            if (!pid_set_contains(collapsed, e->pid) && parent_iter) {
                GtkTreePath *path = gtk_tree_model_get_path(
                    GTK_TREE_MODEL(store), parent_iter);
                gtk_tree_view_expand_row(view, path, FALSE);
                gtk_tree_path_free(path);
            }
        }
    }

    free(new_ht);
    free(old_ht);
    free(existing.entries);
    free(inserted);
}

/*
 * Recursively compute the group RSS for every row in the tree.
 * Group RSS = own RSS + sum of all descendants' RSS.
 * Returns the group total for the subtree rooted at `parent`.
 */
static long compute_group_rss(GtkTreeStore *store, GtkTreeIter *parent)
{
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(model, &child, parent);

    while (valid) {
        compute_group_rss(store, &child);
        valid = gtk_tree_model_iter_next(model, &child);
    }

    /* Now every child has its group RSS computed.  Sum them. */
    if (!parent) return 0;   /* top-level call, nothing to store */

    gint own_rss = 0;
    gtk_tree_model_get(model, parent, COL_RSS, &own_rss, -1);

    long total = (long)own_rss;
    valid = gtk_tree_model_iter_children(model, &child, parent);
    while (valid) {
        gint child_group;
        gtk_tree_model_get(model, &child, COL_GROUP_RSS, &child_group, -1);
        total += (long)child_group;
        valid = gtk_tree_model_iter_next(model, &child);
    }

    char grp_text[64];
    format_memory(total, grp_text, sizeof(grp_text));
    gtk_tree_store_set(store, parent,
                       COL_GROUP_RSS,      (gint)total,
                       COL_GROUP_RSS_TEXT, grp_text,
                       -1);
    return total;
}

/*
 * Recursively compute the group CPU% for every row in the tree.
 * Group CPU% = own CPU% + sum of all descendants' CPU%.
 * Works with the raw ×10000 int values for precision; formats text.
 * Returns the group total (×10000) for the subtree rooted at `parent`.
 */
static long compute_group_cpu(GtkTreeStore *store, GtkTreeIter *parent)
{
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(model, &child, parent);

    while (valid) {
        compute_group_cpu(store, &child);
        valid = gtk_tree_model_iter_next(model, &child);
    }

    /* Now every child has its group CPU computed.  Sum them. */
    if (!parent) return 0;   /* top-level call, nothing to store */

    gint own_cpu = 0;
    gtk_tree_model_get(model, parent, COL_CPU, &own_cpu, -1);

    long total = (long)own_cpu;
    valid = gtk_tree_model_iter_children(model, &child, parent);
    while (valid) {
        gint child_group;
        gtk_tree_model_get(model, &child, COL_GROUP_CPU, &child_group, -1);
        total += (long)child_group;
        valid = gtk_tree_model_iter_next(model, &child);
    }

    char grp_text[32];
    double pct = total / 10000.0;
    if (pct < 0.05)
        snprintf(grp_text, sizeof(grp_text), "0.0%%");
    else
        snprintf(grp_text, sizeof(grp_text), "%.1f%%", pct);
    gtk_tree_store_set(store, parent,
                       COL_GROUP_CPU,      (gint)total,
                       COL_GROUP_CPU_TEXT, grp_text,
                       -1);
    return total;
}

/*
 * Full populate for the initial load (tree is empty).
 * Uses the same ancestor-stack insertion as before.
 */
static void populate_store_initial(GtkTreeStore       *store,
                                   GtkTreeView        *view,
                                   const proc_entry_t *entries,
                                   size_t              count)
{
    if (count == 0) return;

    ht_entry_t *ht = calloc(HT_SIZE, sizeof(ht_entry_t));
    if (!ht) return;
    for (size_t i = 0; i < count; i++)
        ht_insert(ht, entries[i].pid, i);

    int          *inserted = calloc(count, sizeof(int));
    GtkTreeIter  *iters    = calloc(count, sizeof(GtkTreeIter));
    if (!inserted || !iters) {
        free(ht); free(inserted); free(iters);
        return;
    }

    pid_t stack[64];
    int sp;

    for (size_t i = 0; i < count; i++) {
        if (inserted[i]) continue;

        sp = 0;
        size_t cur = i;
        while (!inserted[cur]) {
            if (sp >= 64) break;
            stack[sp++] = entries[cur].pid;
            pid_t pp = entries[cur].ppid;
            size_t pidx = ht_find(ht, pp);
            if (pidx == (size_t)-1 || pidx == cur) break;
            if (inserted[pidx]) break;
            cur = pidx;
        }

        while (sp > 0) {
            pid_t p = stack[--sp];
            size_t sidx = ht_find(ht, p);
            if (sidx == (size_t)-1 || inserted[sidx]) continue;

            const proc_entry_t *e = &entries[sidx];
            GtkTreeIter *parent_iter = NULL;
            size_t pidx = ht_find(ht, e->ppid);
            if (pidx != (size_t)-1 && inserted[pidx] && pidx != sidx)
                parent_iter = &iters[pidx];

            gtk_tree_store_append(store, &iters[sidx], parent_iter);
            set_row_data(store, &iters[sidx], e);
            inserted[sidx] = 1;
        }
    }

    free(ht);
    free(inserted);
    free(iters);

    /* Expand everything on first load */
    gtk_tree_view_expand_all(view);
}

/* ── middle-click autoscroll (browser-style) ─────────────────── */

/* Dead-zone radius as a fraction of the smaller window dimension.
 * e.g. 0.03 = 3% of min(width, height).                          */
#define AUTOSCROLL_DEADZONE_FRAC 0.03

/* How often the scroll timer fires (ms). */
#define AUTOSCROLL_INTERVAL 16   /* ~60 fps */

/* Logarithmic speed factor. */
#define AUTOSCROLL_SCALE    12.0

static void stop_autoscroll(ui_ctx_t *ctx)
{
    ctx->autoscroll = FALSE;
    ctx->velocity_x = ctx->velocity_y = 0;

    if (ctx->scroll_timer) {
        g_source_remove(ctx->scroll_timer);
        ctx->scroll_timer = 0;
    }

    GdkDisplay *display = gdk_display_get_default();
    GdkSeat    *seat    = gdk_display_get_default_seat(display);
    gdk_seat_ungrab(seat);
}

/* Timer callback – apply velocity each tick. */
static gboolean autoscroll_tick(gpointer data)
{
    ui_ctx_t *ctx = data;
    if (!ctx->autoscroll) return G_SOURCE_REMOVE;

    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(ctx->scroll);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(ctx->scroll);

    double hval = gtk_adjustment_get_value(hadj) + ctx->velocity_x;
    double vval = gtk_adjustment_get_value(vadj) + ctx->velocity_y;

    double hmax = gtk_adjustment_get_upper(hadj) - gtk_adjustment_get_page_size(hadj);
    double vmax = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj);

    if (hval < 0) hval = 0;
    if (hval > hmax) hval = hmax;
    if (vval < 0) vval = 0;
    if (vval > vmax) vval = vmax;

    gtk_adjustment_set_value(hadj, hval);
    gtk_adjustment_set_value(vadj, vval);

    return G_SOURCE_CONTINUE;
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *ev,
                                gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    if (ev->button == 2) {   /* middle button */
        ctx->autoscroll = TRUE;
        ctx->follow_selection = FALSE;   /* user is scrolling manually */
        ctx->anchor_x   = ev->x_root;
        ctx->anchor_y   = ev->y_root;
        ctx->velocity_x = 0;
        ctx->velocity_y = 0;

        /* Grab pointer + show all-scroll cursor */
        GdkDisplay *display = gdk_display_get_default();
        GdkSeat    *seat    = gdk_display_get_default_seat(display);
        GdkWindow  *win     = gtk_widget_get_window(widget);
        GdkCursor  *cursor  = gdk_cursor_new_from_name(display, "all-scroll");

        gdk_seat_grab(seat, win, GDK_SEAT_CAPABILITY_POINTER,
                      TRUE, cursor, (GdkEvent *)ev, NULL, NULL);
        if (cursor) g_object_unref(cursor);

        /* Start the scroll timer */
        ctx->scroll_timer = g_timeout_add(AUTOSCROLL_INTERVAL,
                                          autoscroll_tick, ctx);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *ev,
                                  gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    if (ev->button == 2 && ctx->autoscroll) {
        stop_autoscroll(ctx);
        return TRUE;
    }
    return FALSE;
}

/*
 * As the mouse moves away from the anchor, compute a velocity whose
 * magnitude scales logarithmically with distance.  Inside a small
 * dead-zone around the anchor the velocity is zero.
 */
static gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *ev,
                                 gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;
    if (!ctx->autoscroll) return FALSE;

    double raw_dx = ev->x_root - ctx->anchor_x;
    double raw_dy = ev->y_root - ctx->anchor_y;
    double dist   = sqrt(raw_dx * raw_dx + raw_dy * raw_dy);

    /* Compute dead-zone from the smaller window dimension */
    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    int win_w = gtk_widget_get_allocated_width(toplevel);
    int win_h = gtk_widget_get_allocated_height(toplevel);
    double deadzone = AUTOSCROLL_DEADZONE_FRAC * (win_w < win_h ? win_w : win_h);
    if (deadzone < 8.0) deadzone = 8.0;   /* sensible minimum */

    if (dist < deadzone) {
        ctx->velocity_x = 0;
        ctx->velocity_y = 0;
    } else {
        /* Logarithmic ramp: speed = scale * log(1 + dist_beyond_deadzone) */
        double beyond = dist - deadzone;
        double speed  = AUTOSCROLL_SCALE * log(1.0 + beyond);
        /* Split into X / Y components proportionally */
        ctx->velocity_x = speed * (raw_dx / dist);
        ctx->velocity_y = speed * (raw_dy / dist);
    }
    return TRUE;
}

/* ── cancel autoscroll on window focus loss ───────────────────── */

static gboolean on_focus_out(GtkWidget *widget, GdkEventFocus *ev,
                             gpointer data)
{
    (void)widget; (void)ev;
    ui_ctx_t *ctx = data;

    if (ctx->autoscroll)
        stop_autoscroll(ctx);
    return FALSE;
}

/* ── signal handlers for user collapse / expand ──────────────── */

static void on_row_collapsed(GtkTreeView *view,
                              GtkTreeIter *iter,
                              GtkTreePath *path,
                              gpointer     data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint pid;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_PID, &pid, -1);
    pid_set_add(&ctx->collapsed, (pid_t)pid);
}

static void on_row_expanded(GtkTreeView *view,
                             GtkTreeIter *iter,
                             GtkTreePath *path,
                             gpointer     data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint pid;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_PID, &pid, -1);
    pid_set_remove(&ctx->collapsed, (pid_t)pid);
}

/* ── find a row by PID (recursive) ────────────────────────────── */

/*
 * Walk the tree model to find the row whose COL_PID equals `target`.
 * Returns TRUE and fills `result` if found, FALSE otherwise.
 */
static gboolean find_iter_by_pid(GtkTreeModel *model, GtkTreeIter *parent,
                                 pid_t target, GtkTreeIter *result)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint pid;
        gtk_tree_model_get(model, &iter, COL_PID, &pid, -1);
        if ((pid_t)pid == target) {
            *result = iter;
            return TRUE;
        }
        if (find_iter_by_pid(model, &iter, target, result))
            return TRUE;
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}

/* ── sort-click: enable follow-selection ──────────────────────── */

static void on_sort_column_changed(GtkTreeSortable *sortable, gpointer data)
{
    (void)sortable;
    ui_ctx_t *ctx = data;
    ctx->follow_selection = TRUE;
}

/* ── user scroll: disable follow-selection ───────────────────── */

static gboolean on_tree_scroll_event(GtkWidget *widget, GdkEventScroll *ev,
                                     gpointer data)
{
    (void)widget; (void)ev;
    ui_ctx_t *ctx = data;
    ctx->follow_selection = FALSE;
    return FALSE;   /* let GTK handle the scroll normally */
}

/* ── periodic refresh callback ───────────────────────────────── */

static int g_first_refresh = 1;

static gboolean on_refresh(gpointer data)
{
    ui_ctx_t *ctx = data;

    pthread_mutex_lock(&ctx->mon->lock);
    int running = ctx->mon->running;
    size_t count = ctx->mon->snapshot.count;

    proc_entry_t *local = NULL;
    if (count > 0) {
        local = malloc(count * sizeof(proc_entry_t));
        if (local)
            memcpy(local, ctx->mon->snapshot.entries,
                   count * sizeof(proc_entry_t));
    }
    pthread_mutex_unlock(&ctx->mon->lock);

    if (!running) {
        gtk_main_quit();
        free(local);
        return G_SOURCE_REMOVE;
    }

    if (!local)
        return G_SOURCE_CONTINUE;

    /* Block collapse/expand signals during programmatic changes */
    g_signal_handlers_block_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_block_by_func(ctx->view, on_row_expanded,  ctx);

    /*
     * If follow_selection is active, remember the selected PID and its
     * viewport-relative position so we can scroll back after the update.
     */
    pid_t    sel_pid    = 0;
    float    sel_align  = 0.0f;
    gboolean have_sel   = FALSE;

    if (ctx->follow_selection) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
        if (sel) {
            GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
            if (rows) {
                GtkTreePath *sel_path = rows->data;
                GtkTreeIter sel_iter;
                if (gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store),
                                           &sel_iter, sel_path)) {
                    gint pid;
                    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &sel_iter,
                                       COL_PID, &pid, -1);
                    sel_pid = (pid_t)pid;

                    GdkRectangle cell_rect;
                    gtk_tree_view_get_cell_area(ctx->view, sel_path, NULL, &cell_rect);

                    GdkRectangle vis_rect;
                    gtk_tree_view_get_visible_rect(ctx->view, &vis_rect);

                    double vp_y = (double)(cell_rect.y - vis_rect.y);
                    double vp_h = (double)vis_rect.height;
                    if (vp_h > 0)
                        sel_align = (float)(vp_y / vp_h);
                    if (sel_align < 0.0f) sel_align = 0.0f;
                    if (sel_align > 1.0f) sel_align = 1.0f;
                    have_sel = TRUE;
                }
                g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
            }
        }
    }

    PROFILE_BEGIN(ui_render);

    if (g_first_refresh) {
        /* First time: full populate + expand all */
        populate_store_initial(ctx->store, ctx->view, local, count);
        g_first_refresh = 0;
    } else {
        /* Incremental: update in-place, no clear, no flash */
        update_store(ctx->store, ctx->view, local, count, &ctx->collapsed);
    }

    /* Recompute group totals (self + all descendants) */
    compute_group_rss(ctx->store, NULL);
    compute_group_cpu(ctx->store, NULL);

    PROFILE_END(ui_render);

    /*
     * If there was a selection, find the row by PID (stable across
     * re-sorts) and use GTK's scroll_to_cell to place it at the same
     * viewport fraction as before.  This avoids manual bin-window
     * coordinate arithmetic which is unreliable across re-sorts.
     */
    if (have_sel && sel_pid > 0) {
        GtkTreeIter found_iter;
        if (find_iter_by_pid(GTK_TREE_MODEL(ctx->store), NULL,
                             sel_pid, &found_iter)) {
            GtkTreePath *found_path = gtk_tree_model_get_path(
                GTK_TREE_MODEL(ctx->store), &found_iter);
            if (found_path) {
                gtk_tree_view_scroll_to_cell(ctx->view, found_path,
                                             NULL, TRUE, sel_align, 0.0f);
                gtk_tree_path_free(found_path);
            }
        }
    }

    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);

    /* Update system info (right side of status bar) */
    {
        /* uptime from /proc/uptime */
        double uptime_secs = 0;
        FILE *f = fopen("/proc/uptime", "r");
        if (f) { fscanf(f, "%lf", &uptime_secs); fclose(f); }
        int up_days  = (int)(uptime_secs / 86400);
        int up_hours = (int)((long)uptime_secs % 86400) / 3600;
        int up_mins  = (int)((long)uptime_secs % 3600) / 60;

        /* logged-in users via utmpx */
        int nusers = 0;
        setutxent();
        struct utmpx *ut;
        while ((ut = getutxent()) != NULL)
            if (ut->ut_type == USER_PROCESS) nusers++;
        endutxent();

        /* load averages from /proc/loadavg */
        double load1 = 0, load5 = 0, load15 = 0;
        f = fopen("/proc/loadavg", "r");
        if (f) { fscanf(f, "%lf %lf %lf", &load1, &load5, &load15); fclose(f); }

        char sysinfo[256];
        snprintf(sysinfo, sizeof(sysinfo),
                 "up %dd %dh %dm  |  %d user%s  |  load: %.2f %.2f %.2f ",
                 up_days, up_hours, up_mins,
                 nusers, nusers == 1 ? "" : "s",
                 load1, load5, load15);
        gtk_label_set_text(ctx->status_right, sysinfo);
    }

    /* Update status bar (left side) */
    double snap_last = 0, snap_avg = 0, snap_max = 0;
    double ui_last = 0, ui_avg = 0, ui_max = 0;
    profile_get("snapshot_build", &snap_last, &snap_avg, &snap_max);
    profile_get("ui_render",     &ui_last,   &ui_avg,   &ui_max);

    char status[512];
    snprintf(status, sizeof(status),
             " %zu processes  |  snapshot: %.1f ms (avg %.1f, max %.1f)  |  "
             "render: %.1f ms (avg %.1f, max %.1f)",
             count,
             snap_last, snap_avg, snap_max,
             ui_last, ui_avg, ui_max);
    gtk_label_set_text(ctx->status_label, status);

    free(local);
    return G_SOURCE_CONTINUE;
}

/* ── menu bar actions ─────────────────────────────────────────── */

static void on_menu_exit(GtkMenuItem *item, gpointer data)
{
    (void)item;
    GtkWidget *window = data;
    gtk_widget_destroy(window);
}

static void on_menu_about(GtkMenuItem *item, gpointer data)
{
    (void)item;
    GtkWidget *window = data;

    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "allmon – Process Monitor\n"
        "Version 0.1.0\n\n"
        "A lightweight Linux process monitor\n"
        "with a hierarchical tree view.");
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* ── font helpers ─────────────────────────────────────────────── */

#define FONT_SIZE_MIN  6
#define FONT_SIZE_MAX  30
#define FONT_SIZE_DEFAULT 9

static void reload_font_css(ui_ctx_t *ctx)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "treeview { font-family: Monospace; font-size: %dpt; }",
             ctx->font_size);
    gtk_css_provider_load_from_data(ctx->css, buf, -1, NULL);
}

static void on_font_increase(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (ctx->font_size < FONT_SIZE_MAX) {
        ctx->font_size++;
        ctx->auto_font = FALSE;
        reload_font_css(ctx);
    }
}

static void on_font_decrease(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (ctx->font_size > FONT_SIZE_MIN) {
        ctx->font_size--;
        ctx->auto_font = FALSE;
        reload_font_css(ctx);
    }
}

static void on_font_auto_toggle(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->auto_font = gtk_check_menu_item_get_active(item);
}

/* Recompute the auto-scaled font size based on physical pixel height. */
static void recompute_auto_font(ui_ctx_t *ctx)
{
    if (!ctx->auto_font) return;

    GtkWidget *w = GTK_WIDGET(ctx->view);
    GtkAllocation alloc;
    gtk_widget_get_allocation(gtk_widget_get_toplevel(w), &alloc);

    /* Multiply by the scale factor to get real physical pixel height.
     * On a 2K monitor scale_factor is typically 1; on a 4K monitor it's 2. */
    int scale = gtk_widget_get_scale_factor(gtk_widget_get_toplevel(w));
    int phys_height = alloc.height * scale;

    /* Baseline: 9pt at 700 physical pixels. */
    int new_size = (int)(9.0 * phys_height / 700.0 + 0.5);
    if (new_size < FONT_SIZE_MIN) new_size = FONT_SIZE_MIN;
    if (new_size > FONT_SIZE_MAX) new_size = FONT_SIZE_MAX;
    if (new_size != ctx->font_size) {
        ctx->font_size = new_size;
        reload_font_css(ctx);
    }
}

static gboolean on_window_configure(GtkWidget *widget, GdkEventConfigure *ev,
                                    gpointer data)
{
    (void)widget;
    (void)ev;
    recompute_auto_font(data);
    return FALSE;
}

/* Fired when the window moves to a monitor with a different scale factor. */
static void on_scale_factor_changed(GObject *obj, GParamSpec *pspec,
                                    gpointer data)
{
    (void)obj;
    (void)pspec;
    recompute_auto_font(data);
}

/* ── status bar right-click context menu ──────────────────────── */

static void on_toggle_menubar(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (gtk_widget_get_visible(ctx->menubar))
        gtk_widget_hide(ctx->menubar);
    else
        gtk_widget_show_all(ctx->menubar);
}

static gboolean on_status_button_press(GtkWidget *widget, GdkEventButton *ev,
                                       gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    if (ev->button == 3) {   /* right-click */
        GtkWidget *menu = gtk_menu_new();

        gboolean visible = gtk_widget_get_visible(ctx->menubar);
        const char *label = visible ? "Hide Menubar" : "Show Menubar";

        GtkWidget *mi = gtk_menu_item_new_with_label(label);
        g_signal_connect(mi, "activate", G_CALLBACK(on_toggle_menubar), ctx);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
        return TRUE;
    }
    return FALSE;
}

/* ── window close handler ────────────────────────────────────── */

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    monitor_state_t *mon = data;

    pthread_mutex_lock(&mon->lock);
    mon->running = 0;
    pthread_cond_broadcast(&mon->updated);
    pthread_mutex_unlock(&mon->lock);

    gtk_main_quit();
}

/* ── inverted sort comparators ────────────────────────────────── */

/*
 * GTK's default sort-indicator arrows can feel backwards: the "up"
 * arrow (▲) means ascending (smallest first) and "down" (▼) means
 * descending.  Most users expect ▲ = highest first.  We fix this by
 * negating the comparison so GTK's "ascending" actually sorts
 * descending-by-value, making the arrow intuitive.
 */

static gint sort_int_inverted(GtkTreeModel *model,
                              GtkTreeIter  *a,
                              GtkTreeIter  *b,
                              gpointer      col_id_ptr)
{
    gint col = GPOINTER_TO_INT(col_id_ptr);
    gint va = 0, vb = 0;
    gtk_tree_model_get(model, a, col, &va, -1);
    gtk_tree_model_get(model, b, col, &vb, -1);
    /* Negate: GTK "ascending" will now show largest first. */
    return (va < vb) ? 1 : (va > vb) ? -1 : 0;
}

static gint sort_string_inverted(GtkTreeModel *model,
                                 GtkTreeIter  *a,
                                 GtkTreeIter  *b,
                                 gpointer      col_id_ptr)
{
    gint col = GPOINTER_TO_INT(col_id_ptr);
    gchar *sa = NULL, *sb = NULL;
    gtk_tree_model_get(model, a, col, &sa, -1);
    gtk_tree_model_get(model, b, col, &sb, -1);
    int cmp = 0;
    if (sa && sb)      cmp = g_utf8_collate(sa, sb);
    else if (sa)       cmp = 1;
    else if (sb)       cmp = -1;
    g_free(sa);
    g_free(sb);
    /* Negate for inverted arrow direction. */
    return -cmp;
}

/* ── public entry point ──────────────────────────────────────── */

void *ui_thread(void *arg)
{
    monitor_state_t *mon = (monitor_state_t *)arg;

    /* ── window ──────────────────────────────────────────────── */
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "allmon – Process Monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), mon);

    /* ── tree store & view ───────────────────────────────────── */
    GtkTreeStore *store = gtk_tree_store_new(NUM_COLS,
                                             G_TYPE_INT,      /* PID          */
                                             G_TYPE_INT,      /* PPID         */
                                             G_TYPE_STRING,   /* USER         */
                                             G_TYPE_STRING,   /* NAME         */
                                             G_TYPE_INT,      /* CPU% × 10000*/
                                             G_TYPE_STRING,   /* CPU% text    */
                                             G_TYPE_INT,      /* RSS (KiB)    */
                                             G_TYPE_STRING,   /* RSS text     */
                                             G_TYPE_INT,      /* group RSS    */
                                             G_TYPE_STRING,   /* group RSS txt*/
                                             G_TYPE_INT,      /* group CPU%   */
                                             G_TYPE_STRING,   /* group CPU txt*/
                                             G_TYPE_STRING,   /* CWD          */
                                             G_TYPE_STRING);  /* CMDLINE      */

    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);   /* view holds a ref now */

    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(tree), TRUE);

    /* Columns */
    GtkCellRenderer *r;
    GtkTreeViewColumn *col;

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("PID", r,
                                                   "text", COL_PID, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_PID);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("PPID", r,
                                                   "text", COL_PPID, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_PPID);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("User", r,
                                                   "text", COL_USER, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_USER);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Name", r,
                                                   "text", COL_NAME, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_NAME);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 150);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0f, NULL);
    col = gtk_tree_view_column_new_with_attributes("CPU%", r,
                                                   "text", COL_CPU_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_CPU);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0f, NULL);   /* right-align numbers */
    col = gtk_tree_view_column_new_with_attributes("Memory (RSS)", r,
                                                   "text", COL_RSS_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_RSS);  /* sort by raw value */
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0f, NULL);
    col = gtk_tree_view_column_new_with_attributes("Group Memory (RSS)", r,
                                                   "text", COL_GROUP_RSS_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_GROUP_RSS);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 120);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0f, NULL);
    col = gtk_tree_view_column_new_with_attributes("Group CPU%", r,
                                                   "text", COL_GROUP_CPU_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_GROUP_CPU);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("CWD", r,
                                                   "text", COL_CWD, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_CWD);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 150);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Command", r,
                                                   "text", COL_CMDLINE, NULL);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 300);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    /* Register inverted sort functions so ▲ = largest/highest first.
     * Integer columns use sort_int_inverted; string columns use
     * sort_string_inverted.  The column index is passed as user-data. */
    GtkTreeSortable *sortable = GTK_TREE_SORTABLE(store);

    gtk_tree_sortable_set_sort_func(sortable, COL_PID,
        sort_int_inverted, GINT_TO_POINTER(COL_PID), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_PPID,
        sort_int_inverted, GINT_TO_POINTER(COL_PPID), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_USER,
        sort_string_inverted, GINT_TO_POINTER(COL_USER), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_NAME,
        sort_string_inverted, GINT_TO_POINTER(COL_NAME), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_CPU,
        sort_int_inverted, GINT_TO_POINTER(COL_CPU), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_RSS,
        sort_int_inverted, GINT_TO_POINTER(COL_RSS), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_GROUP_RSS,
        sort_int_inverted, GINT_TO_POINTER(COL_GROUP_RSS), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_GROUP_CPU,
        sort_int_inverted, GINT_TO_POINTER(COL_GROUP_CPU), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_CWD,
        sort_string_inverted, GINT_TO_POINTER(COL_CWD), NULL);

    /* Use a monospace font for the tree via CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "treeview { font-family: Monospace; font-size: 9pt; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(tree),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    /* NOTE: don't unref css – kept alive for dynamic font changes */

    /* ── scrolled window ─────────────────────────────────────── */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tree);

    /* ── menu bar (hidden by default) ─────────────────────────── */
    GtkWidget *menubar = gtk_menu_bar_new();

    /* File menu */
    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);

    GtkWidget *exit_item = gtk_menu_item_new_with_label("Exit");
    g_signal_connect(exit_item, "activate", G_CALLBACK(on_menu_exit), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), exit_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

    /* View menu → Appearance submenu */
    GtkWidget *view_menu = gtk_menu_new();
    GtkWidget *view_item = gtk_menu_item_new_with_label("View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);

    GtkWidget *appear_menu = gtk_menu_new();
    GtkWidget *appear_item = gtk_menu_item_new_with_label("Appearance");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(appear_item), appear_menu);

    GtkWidget *font_inc = gtk_menu_item_new_with_label("Increase Font");
    GtkWidget *font_dec = gtk_menu_item_new_with_label("Decrease Font");
    GtkWidget *font_auto = gtk_check_menu_item_new_with_label("Scale Font with Screen Size");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(font_auto), FALSE);

    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu), font_inc);
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu), font_dec);
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu),
                          gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu), font_auto);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), appear_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_item);

    /* Help menu */
    GtkWidget *help_menu = gtk_menu_new();
    GtkWidget *help_item = gtk_menu_item_new_with_label("Help");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);

    GtkWidget *about_item = gtk_menu_item_new_with_label("About");
    g_signal_connect(about_item, "activate", G_CALLBACK(on_menu_about), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_item);

    /* ── status bar (in event box for right-click) ───────────── */
    GtkWidget *status = gtk_label_new(" Loading…");
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);

    GtkWidget *status_right = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status_right), 1.0f);

    GtkWidget *status_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(status_hbox), status, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(status_hbox), status_right, FALSE, FALSE, 8);

    GtkWidget *status_ebox = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(status_ebox), status_hbox);
    gtk_widget_add_events(status_ebox, GDK_BUTTON_PRESS_MASK);

    /* ── layout ──────────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_ebox, FALSE, FALSE, 4);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* ── refresh timer ───────────────────────────────────────── */
    static ui_ctx_t ctx;
    ctx.mon          = mon;
    ctx.store        = store;
    ctx.view         = GTK_TREE_VIEW(tree);
    ctx.scroll       = GTK_SCROLLED_WINDOW(scroll);
    ctx.status_label = GTK_LABEL(status);
    ctx.status_right = GTK_LABEL(status_right);
    ctx.menubar      = menubar;
    ctx.tree         = tree;
    ctx.css          = css;
    ctx.font_size    = FONT_SIZE_DEFAULT;
    ctx.auto_font    = FALSE;
    ctx.collapsed    = (pid_set_t){ NULL, 0, 0 };
    ctx.follow_selection = FALSE;

    /* Font menu callbacks (need ctx address, so connect after ctx init) */
    g_signal_connect(font_inc,  "activate", G_CALLBACK(on_font_increase),    &ctx);
    g_signal_connect(font_dec,  "activate", G_CALLBACK(on_font_decrease),    &ctx);
    g_signal_connect(font_auto, "toggled",  G_CALLBACK(on_font_auto_toggle), &ctx);
    g_signal_connect(window,    "configure-event",
                     G_CALLBACK(on_window_configure), &ctx);
    g_signal_connect(window,    "notify::scale-factor",
                     G_CALLBACK(on_scale_factor_changed), &ctx);

    /* Right-click on status bar to toggle menu bar */
    g_signal_connect(status_ebox, "button-press-event",
                     G_CALLBACK(on_status_button_press), &ctx);

    /* Middle-click drag-to-scroll */
    gtk_widget_add_events(tree, GDK_BUTTON_PRESS_MASK
                              | GDK_BUTTON_RELEASE_MASK
                              | GDK_POINTER_MOTION_MASK);
    g_signal_connect(tree,   "button-press-event",   G_CALLBACK(on_button_press),   &ctx);
    g_signal_connect(tree,   "button-release-event", G_CALLBACK(on_button_release), &ctx);
    g_signal_connect(tree,   "motion-notify-event",  G_CALLBACK(on_motion_notify),  &ctx);
    g_signal_connect(window, "focus-out-event",      G_CALLBACK(on_focus_out),      &ctx);

    /* Track user collapse / expand actions */
    g_signal_connect(tree, "row-collapsed", G_CALLBACK(on_row_collapsed), &ctx);
    g_signal_connect(tree, "row-expanded",  G_CALLBACK(on_row_expanded),  &ctx);

    /* Enable follow-selection when user clicks a sort column */
    g_signal_connect(store, "sort-column-changed",
                     G_CALLBACK(on_sort_column_changed), &ctx);

    /* Disable follow-selection when user scrolls manually */
    gtk_widget_add_events(tree, GDK_SCROLL_MASK);
    g_signal_connect(tree, "scroll-event",
                     G_CALLBACK(on_tree_scroll_event), &ctx);

    g_timeout_add(1000, on_refresh, &ctx);

    /* ── show & run ──────────────────────────────────────────── */
    gtk_widget_show_all(window);
    gtk_widget_hide(menubar);   /* hidden by default; toggle via status-bar right-click */
    gtk_main();

    return NULL;
}

