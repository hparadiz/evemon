/*
 * ui.c – GTK3 process-tree UI entry point and event handlers.
 *
 * Displays processes in a hierarchical GtkTreeView (like Sysinternals
 * Process Explorer / procmon on Windows).  Each process is nested under
 * its parent so you can expand/collapse entire process subtrees.
 *
 * A GLib timeout fires every ~1 s, grabs the latest snapshot from the
 * monitor thread, rebuilds the GtkTreeStore, and re-expands any rows
 * the user had open.
 */

#include "ui_internal.h"

#include "../plugin_loader.h"
#include "../plugin_broker.h"

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <utmpx.h>

/* Forward declarations for filter helpers */
static void rebuild_filter_store(ui_ctx_t *ctx);
static void sync_filter_store(ui_ctx_t *ctx);
static void switch_to_real_store(ui_ctx_t *ctx);

/* Forward declaration for expand helper */
static void expand_respecting_collapsed_recurse(ui_ctx_t *ctx,
                                                 GtkTreeModel *model,
                                                 GtkTreeIter *parent);

/* ── pid_set helpers ─────────────────────────────────────────── */

/*
 * set_process_tree_node – record the expand/collapse state for a PID.
 *
 * pinned_pid = the PID of the pinned root that owns this subtree,
 *              or PTREE_UNPINNED (-1) for the main (unpinned) tree.
 * pid        = the actual process PID whose state we are recording.
 * state      = PTREE_COLLAPSED (1) or PTREE_EXPANDED (0).
 *
 * The (pinned_pid, pid) pair forms the composite key, so the same
 * PID can have independent expand/collapse state in multiple pinned
 * subtrees without collision.
 *
 * If the entry already exists its state is updated in place;
 * otherwise a new entry is appended.
 */
void set_process_tree_node(ptree_node_set_t *s, pid_t pinned_pid,
                           pid_t pid, int state)
{
    /* Update in place if the (pinned_pid, pid) pair already exists */
    for (size_t i = 0; i < s->count; i++) {
        if (s->pinned_pids[i] == pinned_pid && s->pids[i] == pid) {
            s->states[i] = state;
            return;
        }
    }

    /* Append new entry */
    if (s->count >= s->capacity) {
        size_t newcap = s->capacity ? s->capacity * 2 : 64;
        pid_t *tp = realloc(s->pinned_pids, newcap * sizeof(pid_t));
        if (!tp) return;   /* OOM – silently drop */
        s->pinned_pids = tp;
        pid_t *tk = realloc(s->pids, newcap * sizeof(pid_t));
        if (!tk) return;
        s->pids = tk;
        int *ts = realloc(s->states, newcap * sizeof(int));
        if (!ts) return;
        s->states   = ts;
        s->capacity = newcap;
    }
    s->pinned_pids[s->count] = pinned_pid;
    s->pids       [s->count] = pid;
    s->states     [s->count] = state;
    s->count++;
}

/*
 * get_process_tree_node – query the expand/collapse state for a PID.
 *
 * pinned_pid = the PID of the pinned root that owns this subtree,
 *              or PTREE_UNPINNED (-1) for the main (unpinned) tree.
 * pid        = the actual process PID to query.
 *
 * Returns the stored state (PTREE_COLLAPSED or PTREE_EXPANDED).
 * If the (pinned_pid, pid) pair has never been recorded, returns
 * PTREE_EXPANDED (the default: rows start expanded).
 */
int get_process_tree_node(const ptree_node_set_t *s, pid_t pinned_pid,
                          pid_t pid)
{
    for (size_t i = 0; i < s->count; i++)
        if (s->pinned_pids[i] == pinned_pid && s->pids[i] == pid)
            return s->states[i];
    return PTREE_EXPANDED;   /* default */
}

/* ── formatting helpers ──────────────────────────────────────── */

/* Format a KiB value into a human-readable string. */
void format_memory(long kb, char *buf, size_t bufsz)
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

/* Format an elapsed-seconds value into a human-friendly "ago" string. */
void format_fuzzy_time(time_t epoch, char *buf, size_t bufsz)
{
    if (epoch <= 0) { snprintf(buf, bufsz, "–"); return; }

    time_t now = time(NULL);
    long diff = (long)(now - epoch);
    if (diff < 0) { snprintf(buf, bufsz, "just now"); return; }

    if (diff < 60)
        snprintf(buf, bufsz, "%lds ago", diff);
    else if (diff < 3600)
        snprintf(buf, bufsz, "%ldm %lds ago", diff / 60, diff % 60);
    else if (diff < 86400) {
        long h = diff / 3600;
        long m = (diff % 3600) / 60;
        snprintf(buf, bufsz, "%ldh %ldm ago", h, m);
    } else if (diff < 86400 * 30L) {
        long d = diff / 86400;
        long h = (diff % 86400) / 3600;
        snprintf(buf, bufsz, "%ldd %ldh ago", d, h);
    } else if (diff < 86400 * 365L) {
        long d = diff / 86400;
        snprintf(buf, bufsz, "%ldd ago", d);
    } else {
        long y = diff / (86400 * 365L);
        long d = (diff % (86400 * 365L)) / 86400;
        snprintf(buf, bufsz, "%ldy %ldd ago", y, d);
    }
}

/* ── find a row by PID (recursive) ────────────────────────────── */

/*
 * Walk the tree model to find the row whose COL_PID equals `target`.
 * Returns TRUE and fills `result` if found, FALSE otherwise.
 */
gboolean find_iter_by_pid(GtkTreeModel *model, GtkTreeIter *parent,
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

/*
 * Walk the tree model to find the row whose COL_PID equals `target`
 * AND whose COL_PINNED_ROOT equals `pinned_root`.  This disambiguates
 * between the pinned copy and the original tree row for the same PID.
 * Returns TRUE and fills `result` if found, FALSE otherwise.
 */
static gboolean find_iter_by_pid_and_pinned_root(GtkTreeModel *model,
                                                  GtkTreeIter  *parent,
                                                  pid_t target,
                                                  pid_t pinned_root,
                                                  GtkTreeIter  *result)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint pid, pr;
        gtk_tree_model_get(model, &iter,
                           COL_PID, &pid,
                           COL_PINNED_ROOT, &pr, -1);
        if ((pid_t)pid == target && (pid_t)pr == pinned_root) {
            *result = iter;
            return TRUE;
        }
        if (find_iter_by_pid_and_pinned_root(model, &iter, target,
                                              pinned_root, result))
            return TRUE;
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}

/* ── middle-click autoscroll (browser-style) ─────────────────── */

/*
 * get_row_pinned_root – given a store iter, return its COL_PINNED_ROOT
 * value.  For rows in the normal tree this is PTREE_UNPINNED (-1);
 * for rows inside a pinned subtree it is the PID of the pinned root.
 *
 * This works by reading COL_PINNED_ROOT directly from the row –
 * every row in a pinned subtree is stamped with the same pinned root.
 */
static pid_t get_row_pinned_root(GtkTreeModel *model, GtkTreeIter *iter)
{
    gint pr = (gint)PTREE_UNPINNED;
    gtk_tree_model_get(model, iter, COL_PINNED_ROOT, &pr, -1);
    return (pid_t)pr;
}

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
    if (!ctx->autoscroll || ctx->shutting_down) return G_SOURCE_REMOVE;

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

/* ── right-click: end process / end process tree ─────────────── */

/*
 * Collect all descendant PIDs of `parent_pid` from the current tree model.
 * Uses the tree structure (not /proc) so it matches what the user sees.
 */
static void collect_tree_descendants(GtkTreeModel *model, GtkTreeIter *parent,
                                    pid_t **out, size_t *cnt, size_t *cap)
{
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(model, &child, parent);
    while (valid) {
        gint pid;
        gtk_tree_model_get(model, &child, COL_PID, &pid, -1);
        if (*cnt >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            pid_t *tmp = realloc(*out, *cap * sizeof(pid_t));
            if (!tmp) return;
            *out = tmp;
        }
        (*out)[(*cnt)++] = (pid_t)pid;
        collect_tree_descendants(model, &child, out, cnt, cap);
        valid = gtk_tree_model_iter_next(model, &child);
    }
}

static void on_end_process(GtkMenuItem *item, gpointer data)
{
    (void)item;
    pid_t pid = GPOINTER_TO_INT(data);
    if (pid > 1)
        kill(pid, SIGTERM);
}

/* Send an arbitrary signal to a process. */
typedef struct {
    pid_t pid;
    int   signo;
} send_signal_data_t;

static void on_send_signal(GtkMenuItem *item, gpointer data)
{
    (void)item;
    send_signal_data_t *d = data;
    if (d->pid > 1)
        kill(d->pid, d->signo);
    free(d);
}

/* Send an arbitrary signal to a process tree (children first). */
typedef struct {
    ui_ctx_t *ctx;
    pid_t     pid;
    int       signo;
} send_signal_tree_data_t;

static void on_send_signal_tree(GtkMenuItem *item, gpointer data)
{
    (void)item;
    send_signal_tree_data_t *d = data;
    GtkTreeModel *model = GTK_TREE_MODEL(d->ctx->store);

    GtkTreeIter iter;
    if (find_iter_by_pid(model, NULL, d->pid, &iter)) {
        pid_t *kids = NULL;
        size_t nkids = 0, cap = 0;
        collect_tree_descendants(model, &iter, &kids, &nkids, &cap);
        for (size_t i = nkids; i > 0; i--) {
            if (kids[i - 1] > 1)
                kill(kids[i - 1], d->signo);
        }
        free(kids);
    }
    if (d->pid > 1)
        kill(d->pid, d->signo);
    free(d);
}

typedef struct {
    ui_ctx_t *ctx;
    pid_t     pid;
} end_tree_data_t;

static void on_end_process_tree(GtkMenuItem *item, gpointer data)
{
    (void)item;
    end_tree_data_t *d = data;
    GtkTreeModel *model = GTK_TREE_MODEL(d->ctx->store);

    GtkTreeIter iter;
    if (!find_iter_by_pid(model, NULL, d->pid, &iter)) {
        free(d);
        return;
    }

    /* Kill children first (bottom-up is more graceful) */
    pid_t *kids = NULL;
    size_t nkids = 0, cap = 0;
    collect_tree_descendants(model, &iter, &kids, &nkids, &cap);

    /* Kill descendants in reverse order (deepest first) */
    for (size_t i = nkids; i > 0; i--) {
        if (kids[i - 1] > 1)
            kill(kids[i - 1], SIGTERM);
    }
    free(kids);

    /* Kill the root process last */
    if (d->pid > 1)
        kill(d->pid, SIGTERM);

    free(d);
}

static void on_copy_command(GtkMenuItem *item, gpointer data)
{
    (void)item;
    const char *cmdline = data;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, cmdline, -1);
}

/* ── pin / unpin helpers ─────────────────────────────────────── */

static gboolean pid_is_pinned(const ui_ctx_t *ctx, pid_t pid)
{
    for (size_t i = 0; i < ctx->pinned_count; i++)
        if (ctx->pinned_pids[i] == pid) return TRUE;
    return FALSE;
}

static void pin_pid(ui_ctx_t *ctx, pid_t pid)
{
    if (pid_is_pinned(ctx, pid)) return;
    if (ctx->pinned_count >= ctx->pinned_capacity) {
        size_t newcap = ctx->pinned_capacity ? ctx->pinned_capacity * 2 : 16;
        pid_t *tmp = realloc(ctx->pinned_pids, newcap * sizeof(pid_t));
        if (!tmp) return;
        ctx->pinned_pids     = tmp;
        ctx->pinned_capacity = newcap;
    }
    ctx->pinned_pids[ctx->pinned_count++] = pid;
}

static void unpin_pid(ui_ctx_t *ctx, pid_t pid)
{
    for (size_t i = 0; i < ctx->pinned_count; i++) {
        if (ctx->pinned_pids[i] == pid) {
            ctx->pinned_pids[i] = ctx->pinned_pids[--ctx->pinned_count];
            return;
        }
    }
}

typedef struct {
    ui_ctx_t *ctx;
    pid_t     pid;
} pin_toggle_data_t;

static void on_toggle_pin(GtkMenuItem *item, gpointer data)
{
    (void)item;
    pin_toggle_data_t *d = data;
    if (pid_is_pinned(d->ctx, d->pid))
        unpin_pid(d->ctx, d->pid);
    else
        pin_pid(d->ctx, d->pid);
    free(d);
}

static void show_process_context_menu(ui_ctx_t *ctx, GdkEventButton *ev,
                                     pid_t pid, const char *name,
                                     const char *cmdline)
{
    GtkWidget *menu = gtk_menu_new();

    /* ── Pin / Unpin toggle (at the very top) ── */
    {
        gboolean pinned = pid_is_pinned(ctx, pid);
        char pin_label[320];
        snprintf(pin_label, sizeof(pin_label), "%s (%s, pid %d)",
                 pinned ? "Unpin Process" : "Pin Process", name, pid);
        GtkWidget *mi_pin = gtk_menu_item_new_with_label(pin_label);
        pin_toggle_data_t *pd = malloc(sizeof(*pd));
        if (pd) {
            pd->ctx = ctx;
            pd->pid = pid;
            g_signal_connect(mi_pin, "activate",
                             G_CALLBACK(on_toggle_pin), pd);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_pin);
    }

    /* ── separator ── */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    /* ── Copy Command ── */
    GtkWidget *mi_copy = gtk_menu_item_new_with_label("Copy Command");
    if (cmdline && cmdline[0]) {
        /* Store a copy of the cmdline string as data on the menu item
         * so it survives until the callback fires. */
        gchar *cmd_dup = g_strdup(cmdline);
        g_signal_connect(mi_copy, "activate",
                         G_CALLBACK(on_copy_command), cmd_dup);
        g_object_set_data_full(G_OBJECT(mi_copy), "cmd",
                               cmd_dup, g_free);
    } else {
        gtk_widget_set_sensitive(mi_copy, FALSE);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_copy);

    /* ── separator ── */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    char label1[320];
    snprintf(label1, sizeof(label1), "End Process (%s, pid %d)", name, pid);
    GtkWidget *mi1 = gtk_menu_item_new_with_label(label1);
    g_signal_connect(mi1, "activate", G_CALLBACK(on_end_process),
                     GINT_TO_POINTER(pid));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi1);

    char label2[320];
    snprintf(label2, sizeof(label2), "End Process Tree (%s, pid %d)", name, pid);
    GtkWidget *mi2 = gtk_menu_item_new_with_label(label2);
    end_tree_data_t *d = malloc(sizeof(*d));
    if (d) {
        d->ctx = ctx;
        d->pid = pid;
        g_signal_connect(mi2, "activate", G_CALLBACK(on_end_process_tree), d);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi2);

    /* ── Send Signal submenu ── */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    static const struct { const char *label; int signo; } signals[] = {
        { "SIGTERM (15) – Terminate",  SIGTERM },
        { "SIGKILL (9)  – Kill",       SIGKILL },
        { "SIGSTOP (19) – Stop",       SIGSTOP },
        { "SIGCONT (18) – Continue",   SIGCONT },
        { NULL, 0 },   /* separator */
        { "SIGINT  (2)  – Interrupt",  SIGINT  },
        { "SIGHUP  (1)  – Hangup",    SIGHUP  },
        { "SIGUSR1 (10)",             SIGUSR1 },
        { "SIGUSR2 (12)",             SIGUSR2 },
        { NULL, 0 },   /* separator */
        { "SIGABRT (6)  – Abort",     SIGABRT },
        { "SIGQUIT (3)  – Quit",      SIGQUIT },
    };

    {
        GtkWidget *sig_menu = gtk_menu_new();
        GtkWidget *sig_item = gtk_menu_item_new_with_label("Send Signal");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(sig_item), sig_menu);

        for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++) {
            if (!signals[i].label) {
                gtk_menu_shell_append(GTK_MENU_SHELL(sig_menu),
                                      gtk_separator_menu_item_new());
                continue;
            }
            GtkWidget *mi = gtk_menu_item_new_with_label(signals[i].label);
            send_signal_data_t *sd = malloc(sizeof(*sd));
            if (sd) {
                sd->pid   = pid;
                sd->signo = signals[i].signo;
                g_signal_connect(mi, "activate",
                                 G_CALLBACK(on_send_signal), sd);
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(sig_menu), mi);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), sig_item);
    }

    /* ── Send Signal to Tree submenu ── */
    {
        GtkWidget *sig_tree_menu = gtk_menu_new();
        GtkWidget *sig_tree_item = gtk_menu_item_new_with_label(
            "Send Signal to Tree");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(sig_tree_item), sig_tree_menu);

        for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++) {
            if (!signals[i].label) {
                gtk_menu_shell_append(GTK_MENU_SHELL(sig_tree_menu),
                                      gtk_separator_menu_item_new());
                continue;
            }
            GtkWidget *mi = gtk_menu_item_new_with_label(signals[i].label);
            send_signal_tree_data_t *sd = malloc(sizeof(*sd));
            if (sd) {
                sd->ctx   = ctx;
                sd->pid   = pid;
                sd->signo = signals[i].signo;
                g_signal_connect(mi, "activate",
                                 G_CALLBACK(on_send_signal_tree), sd);
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(sig_tree_menu), mi);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), sig_tree_item);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *ev,
                                gpointer data)
{
    ui_ctx_t *ctx = data;

    if (ev->button == 3) {   /* right-click */
        GtkTreePath *path = NULL;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                         (gint)ev->x, (gint)ev->y,
                                         &path, NULL, NULL, NULL)) {
            /* Select the row under the cursor */
            GtkTreeSelection *sel = gtk_tree_view_get_selection(
                GTK_TREE_VIEW(widget));
            gtk_tree_selection_select_path(sel, path);

            GtkTreeIter sort_iter;
            if (gtk_tree_model_get_iter(
                    GTK_TREE_MODEL(ctx->sort_model),
                    &sort_iter, path)) {
                /* sort iter → underlying store iter */
                GtkTreeIter child_iter;
                gtk_tree_model_sort_convert_iter_to_child_iter(
                    ctx->sort_model, &child_iter, &sort_iter);
                GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                    ctx->sort_model);
                gint pid = 0;
                gchar *name = NULL;
                gchar *cmdline = NULL;
                gtk_tree_model_get(child_model, &child_iter,
                                   COL_PID, &pid, COL_NAME, &name,
                                   COL_CMDLINE, &cmdline, -1);
                show_process_context_menu(ctx, ev, (pid_t)pid,
                                         name ? name : "?",
                                         cmdline ? cmdline : "");
                g_free(name);
                g_free(cmdline);
            }
            gtk_tree_path_free(path);
        }
        return TRUE;
    }

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
    ui_ctx_t *ctx = data;

    /*
     * When the user collapses a parent, GTK cascades and emits
     * row-collapsed for every expanded descendant too.  We must
     * ignore those cascading signals – only record the row the
     * user actually collapsed.  A cascading collapse is detected
     * by checking whether the row's parent is still expanded in
     * the view; if it isn't, this is a side-effect, not a user action.
     */
    if (gtk_tree_path_get_depth(path) > 1) {
        GtkTreePath *parent_path = gtk_tree_path_copy(path);
        gtk_tree_path_up(parent_path);
        gboolean parent_expanded = gtk_tree_view_row_expanded(view,
                                                              parent_path);
        gtk_tree_path_free(parent_path);
        if (!parent_expanded)
            return;   /* cascade – ignore */
    }

    gint pid;
    /* iter is from the sort model; convert sort → underlying store */
    GtkTreeIter child_iter;
    gtk_tree_model_sort_convert_iter_to_child_iter(
        ctx->sort_model, &child_iter, iter);
    GtkTreeModel *child_model = gtk_tree_model_sort_get_model(ctx->sort_model);
    gtk_tree_model_get(child_model, &child_iter, COL_PID, &pid, -1);

    /* Determine whether this row is inside a pinned subtree */
    pid_t pinned_root = get_row_pinned_root(child_model, &child_iter);
    set_process_tree_node(&ctx->ptree_nodes, pinned_root, (pid_t)pid,
                          PTREE_COLLAPSED);

    //fprintf(stdout, "evemon: collapsed PID %d\n", pid);
}

static void on_row_expanded(GtkTreeView *view,
                             GtkTreeIter *iter,
                             GtkTreePath *path,
                             gpointer     data)
{
    ui_ctx_t *ctx = data;
    gint pid;
    /* iter is from the sort model; convert sort → underlying store */
    GtkTreeIter child_iter;
    gtk_tree_model_sort_convert_iter_to_child_iter(
        ctx->sort_model, &child_iter, iter);
    GtkTreeModel *child_model = gtk_tree_model_sort_get_model(ctx->sort_model);
    gtk_tree_model_get(child_model, &child_iter, COL_PID, &pid, -1);

    /* Determine whether this row is inside a pinned subtree */
    pid_t pinned_root = get_row_pinned_root(child_model, &child_iter);
    set_process_tree_node(&ctx->ptree_nodes, pinned_root, (pid_t)pid,
                          PTREE_EXPANDED);

    //fprintf(stdout, "evemon: expanded PID %d\n", pid);

    /*
     * When GTK expands a row it reveals immediate children in their
     * default (collapsed) visual state.  Consult our source of truth
     * and re-expand any children that should be expanded.
     */
    GtkTreeModel *sort_model = gtk_tree_view_get_model(view);
    GtkTreeIter sort_child;
    gboolean valid = gtk_tree_model_iter_children(sort_model, &sort_child,
                                                  iter);
    while (valid) {
        if (gtk_tree_model_iter_has_child(sort_model, &sort_child)) {
            GtkTreeIter store_child;
            gtk_tree_model_sort_convert_iter_to_child_iter(
                ctx->sort_model, &store_child, &sort_child);
            gint child_pid;
            gtk_tree_model_get(child_model, &store_child,
                               COL_PID, &child_pid, -1);

            if (get_process_tree_node(&ctx->ptree_nodes, pinned_root,
                                      (pid_t)child_pid) == PTREE_EXPANDED) {
                GtkTreePath *child_path = gtk_tree_model_get_path(
                    sort_model, &sort_child);
                if (child_path) {
                    gtk_tree_view_expand_row(view, child_path, FALSE);
                    gtk_tree_path_free(child_path);
                }
            }
        }
        valid = gtk_tree_model_iter_next(sort_model, &sort_child);
    }
}

/* ── selection / sidebar interaction ─────────────────────────── */

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)sel;
    sidebar_update((ui_ctx_t *)data);
}

/* ── Collapsible section constants ────────────────────────────── */
#define SECTION_ARROW_EXPANDED  "▼"

/* Animation duration for section reveal transitions (ms) */
#define SECTION_TRANSITION_MS  200

/* ── detail panel: toggle visibility ─────────────────────────── */

static void detail_panel_relayout(ui_ctx_t *ctx);

static void on_toggle_detail_panel(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    if (gtk_check_menu_item_get_active(item)) {
        gtk_widget_show_all(ctx->detail_panel);
    } else {
        gtk_widget_hide(ctx->detail_panel);
    }
}

/* ── detail panel: change dock position ──────────────────────── */

typedef struct {
    ui_ctx_t        *ctx;
    panel_position_t pos;
} panel_pos_data_t;

static void on_panel_position_changed(GtkCheckMenuItem *item, gpointer data)
{
    panel_pos_data_t *d = data;
    if (!gtk_check_menu_item_get_active(item))
        return;   /* ignore radio deactivation */
    if (d->ctx->detail_panel_pos == d->pos)
        return;   /* no change */
    d->ctx->detail_panel_pos = d->pos;
    detail_panel_relayout(d->ctx);
}

/*
 * Reparent the detail panel into the correct paned position.
 *
 * The layout hierarchy is:
 *   content_box (vbox):
 *     [0] menubar
 *     [1] outer_paned (vertical or horizontal depending on position)
 *           pack1 = tree area (hpaned containing tree_overlay + sidebar)
 *           pack2 = detail_panel
 *     [2] status_ebox
 *
 * When the position changes, we destroy the old paned, create a new one
 * with the correct orientation, and re-pack everything.
 */
static void detail_panel_relayout(ui_ctx_t *ctx)
{
    gboolean was_visible = gtk_widget_get_visible(ctx->detail_panel);

    /* Remove detail_panel and hpaned from their current parent.
     * g_object_ref ensures they survive the container removal. */
    g_object_ref(ctx->detail_panel);
    g_object_ref(ctx->hpaned);

    GtkWidget *old_paned = ctx->detail_paned;
    if (old_paned) {
        /* The old paned is child [1] of content_box */
        gtk_container_remove(GTK_CONTAINER(old_paned), ctx->hpaned);
        gtk_container_remove(GTK_CONTAINER(old_paned), ctx->detail_panel);
        gtk_container_remove(GTK_CONTAINER(ctx->content_box), old_paned);
    }

    /* Create a new paned with the correct orientation */
    GtkOrientation orient;
    gboolean panel_is_child1;  /* TRUE = panel is pack1 (top/left) */

    switch (ctx->detail_panel_pos) {
    case PANEL_POS_TOP:
        orient = GTK_ORIENTATION_VERTICAL;
        panel_is_child1 = TRUE;
        break;
    case PANEL_POS_BOTTOM:
        orient = GTK_ORIENTATION_VERTICAL;
        panel_is_child1 = FALSE;
        break;
    case PANEL_POS_LEFT:
        orient = GTK_ORIENTATION_HORIZONTAL;
        panel_is_child1 = TRUE;
        break;
    case PANEL_POS_RIGHT:
        orient = GTK_ORIENTATION_HORIZONTAL;
        panel_is_child1 = FALSE;
        break;
    default:
        orient = GTK_ORIENTATION_VERTICAL;
        panel_is_child1 = FALSE;
        break;
    }

    GtkWidget *new_paned = gtk_paned_new(orient);

    if (panel_is_child1) {
        gtk_paned_pack1(GTK_PANED(new_paned), ctx->detail_panel, FALSE, FALSE);
        gtk_paned_pack2(GTK_PANED(new_paned), ctx->hpaned, TRUE, FALSE);
    } else {
        gtk_paned_pack1(GTK_PANED(new_paned), ctx->hpaned, TRUE, FALSE);
        gtk_paned_pack2(GTK_PANED(new_paned), ctx->detail_panel, FALSE, FALSE);
    }

    /* Set a reasonable default position for the divider */
    if (orient == GTK_ORIENTATION_VERTICAL) {
        /* For top/bottom, give the panel ~250px */
        GtkAllocation alloc;
        gtk_widget_get_allocation(ctx->content_box, &alloc);
        int total_h = alloc.height > 100 ? alloc.height : 700;
        int panel_h = 250;
        if (panel_is_child1)
            gtk_paned_set_position(GTK_PANED(new_paned), panel_h);
        else
            gtk_paned_set_position(GTK_PANED(new_paned), total_h - panel_h);
    } else {
        /* For left/right, give the panel ~350px */
        GtkAllocation alloc;
        gtk_widget_get_allocation(ctx->content_box, &alloc);
        int total_w = alloc.width > 100 ? alloc.width : 1100;
        int panel_w = 350;
        if (panel_is_child1)
            gtk_paned_set_position(GTK_PANED(new_paned), panel_w);
        else
            gtk_paned_set_position(GTK_PANED(new_paned), total_w - panel_w);
    }

    ctx->detail_paned = new_paned;

    g_object_unref(ctx->detail_panel);
    g_object_unref(ctx->hpaned);

    /* Insert the new paned into content_box at position [1]
     * (between menubar [0] and status [2]).  pack_start appends,
     * so we use gtk_box_reorder_child to place it correctly. */
    gtk_box_pack_start(GTK_BOX(ctx->content_box), new_paned, TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(ctx->content_box), new_paned, 1);

    gtk_widget_show_all(new_paned);

    if (!was_visible)
        gtk_widget_hide(ctx->detail_panel);
}


/* ── double-click: open sidebar for the activated row ─────────── */

static void on_row_activated(GtkTreeView       *view,
                             GtkTreePath       *path,
                             GtkTreeViewColumn *col,
                             gpointer           data)
{
    (void)view; (void)path; (void)col;
    ui_ctx_t *ctx = data;

    /* Double-click opens the detail panel (plugin tabs), not the sidebar */
    if (!gtk_widget_get_visible(ctx->detail_panel)) {
        gtk_check_menu_item_set_active(ctx->detail_panel_menu_item, TRUE);
    }
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

/* ── highlight animation timer ────────────────────────────────── */

/*
 * Check whether any row in the store has an active highlight
 * (born or died timestamp that hasn't expired yet).
 */
static gboolean store_has_active_highlights(GtkTreeModel *model,
                                            GtkTreeIter  *parent,
                                            gint64        now)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint64 born = 0, died = 0;
        gtk_tree_model_get(model, &iter,
                           COL_HIGHLIGHT_BORN, &born,
                           COL_HIGHLIGHT_DIED, &died, -1);
        if ((born > 0 && now - born < HIGHLIGHT_FADE_US) ||
            (died > 0 && now - died < HIGHLIGHT_FADE_US))
            return TRUE;
        if (store_has_active_highlights(model, &iter, now))
            return TRUE;
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}

/* Timer callback: redraw the tree while highlights are active */
static gboolean on_highlight_tick(gpointer data)
{
    ui_ctx_t *ctx = data;

    if (ctx->shutting_down) {
        ctx->highlight_timer = 0;
        return G_SOURCE_REMOVE;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    gint64 now = (gint64)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;

    GtkTreeModel *model = gtk_tree_view_get_model(ctx->view);
    GtkTreeModel *underlying = model;
    if (GTK_IS_TREE_MODEL_SORT(model))
        underlying = gtk_tree_model_sort_get_model(GTK_TREE_MODEL_SORT(model));

    if (store_has_active_highlights(underlying, NULL, now)) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->view));
        return G_SOURCE_CONTINUE;
    }

    /* No more active highlights — stop the timer */
    ctx->highlight_timer = 0;
    return G_SOURCE_REMOVE;
}

/* Start the highlight timer if not already running */
static void ensure_highlight_timer(ui_ctx_t *ctx)
{
    if (ctx->highlight_timer == 0)
        ctx->highlight_timer = g_timeout_add(33, on_highlight_tick, ctx);
                                              /* ~30 fps for smooth fade */
}

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
        if (local) {
            memcpy(local, ctx->mon->snapshot.entries,
                   count * sizeof(proc_entry_t));
            /* Deep-copy heap-allocated steam pointers so the UI's copy
             * is independent of the monitor's snapshot lifecycle. */
            for (size_t i = 0; i < count; i++) {
                if (local[i].steam) {
                    steam_info_t *copy = malloc(sizeof(steam_info_t));
                    if (copy)
                        memcpy(copy, local[i].steam, sizeof(steam_info_t));
                    local[i].steam = copy;
                }
            }
        }
    }
    pthread_mutex_unlock(&ctx->mon->lock);

    if (!running || ctx->shutting_down) {
        if (!running)
            gtk_main_quit();
        proc_snapshot_t tmp = { local, count };
        proc_snapshot_free(&tmp);
        return G_SOURCE_REMOVE;
    }

    if (!local)
        return G_SOURCE_CONTINUE;

    /* Block collapse/expand and selection-changed signals during
     * programmatic changes.  Without blocking selection-changed,
     * remove_pinned_rows would destroy the selected pinned row and
     * GTK would auto-select a nearby row (e.g. init/kthreadd). */
    g_signal_handlers_block_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_block_by_func(ctx->view, on_row_expanded,  ctx);
    GtkTreeSelection *tree_sel = gtk_tree_view_get_selection(ctx->view);
    g_signal_handlers_block_by_func(tree_sel, on_selection_changed, ctx);

    /*
     * Always remember the selected PID and its pinned-root so we can
     * re-select the correct copy (pinned vs original) after the store
     * update.  Pinned rows are destroyed and recreated every tick, so
     * we must restore selection even when follow_selection is off.
     */
    pid_t    sel_pid         = 0;
    pid_t    sel_pinned_root = PTREE_UNPINNED;
    gboolean have_sel        = FALSE;

    {
        GList *rows = gtk_tree_selection_get_selected_rows(tree_sel, NULL);
        if (rows) {
            GtkTreePath *sel_path = rows->data;
            /* sel_path is a sort-model path; sort → underlying store */
            GtkTreePath *store_path = gtk_tree_model_sort_convert_path_to_child_path(
                ctx->sort_model, sel_path);
            GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                ctx->sort_model);
            GtkTreeIter sel_iter;
            if (store_path &&
                gtk_tree_model_get_iter(child_model,
                                       &sel_iter, store_path)) {
                gint pid, pr;
                gtk_tree_model_get(child_model, &sel_iter,
                                   COL_PID, &pid,
                                   COL_PINNED_ROOT, &pr, -1);
                sel_pid         = (pid_t)pid;
                sel_pinned_root = (pid_t)pr;
                have_sel = TRUE;
            }
            if (store_path)
                gtk_tree_path_free(store_path);
            g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
        }
    }

    PROFILE_BEGIN(ui_render);

    if (g_first_refresh) {
        /* First time: full populate + expand all */
        populate_store_initial(ctx->store, ctx->view, local, count);
        g_first_refresh = 0;

        /* Switch from fast startup poll to the normal 1-second interval */
        if (ctx->initial_refresh) {
            ctx->initial_refresh = FALSE;
            g_timeout_add(1000, on_refresh, ctx);
            /* Update status bar with first data before we drop this source */
            goto finish;
        }
    } else {
        /* Remove pinned copies before the incremental update so that
         * duplicate PIDs don't confuse the diff algorithm. */
        remove_pinned_rows(ctx->store);
        /* Incremental: update in-place, no clear, no flash */
        update_store(ctx->store, ctx->view, local, count);
    }

    /* Rebuild pinned subtrees (copies of pinned PIDs at the top) */
    rebuild_pinned_rows(ctx->store,
                        ctx->pinned_pids, ctx->pinned_count);

    /* Expand pinned subtrees according to stored collapse/expand state.
     * Pinned rows are rebuilt from scratch every tick, so they default
     * to collapsed in the view.  Walk top-level pinned rows in the
     * sort model and apply the per-pinned-root expand state. */
    if (ctx->pinned_count > 0) {
        GtkTreeModel *sort = GTK_TREE_MODEL(ctx->sort_model);
        GtkTreeIter sort_iter;
        gboolean valid = gtk_tree_model_get_iter_first(sort, &sort_iter);
        while (valid) {
            GtkTreeIter store_iter;
            gtk_tree_model_sort_convert_iter_to_child_iter(
                ctx->sort_model, &store_iter, &sort_iter);
            GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                ctx->sort_model);
            gint pr = 0;
            gtk_tree_model_get(child_model, &store_iter,
                               COL_PINNED_ROOT, &pr, -1);
            if (pr != (gint)PTREE_UNPINNED) {
                /* This is a pinned top-level row – expand it and
                 * its children according to stored state. */
                GtkTreePath *sort_path = gtk_tree_model_get_path(
                    sort, &sort_iter);
                if (sort_path) {
                    gint pid;
                    gtk_tree_model_get(child_model, &store_iter,
                                       COL_PID, &pid, -1);
                    pid_t pinned_root = (pid_t)pr;
                    if (get_process_tree_node(&ctx->ptree_nodes,
                                              pinned_root, (pid_t)pid)
                        != PTREE_COLLAPSED) {
                        gtk_tree_view_expand_row(ctx->view, sort_path,
                                                 FALSE);
                        /* Recurse: expand children respecting state */
                        expand_respecting_collapsed_recurse(
                            ctx, sort, &sort_iter);
                    }
                    gtk_tree_path_free(sort_path);
                }
            }
            valid = gtk_tree_model_iter_next(sort, &sort_iter);
        }
    }

    /* Recompute group totals (self + all descendants) */
    compute_group_rss(ctx->store, NULL);
    compute_group_cpu(ctx->store, NULL);

    /* Sync the filtered shadow store if a filter is active.
     * This updates data in-place, preserving view state (expand,
     * selection, scroll).  A full rebuild only happens if the set
     * of matching processes has structurally changed. */
    if (ctx->filter_text[0] != '\0')
        sync_filter_store(ctx);

    /* Kick the highlight fade timer if any rows are newly born/dying */
    ensure_highlight_timer(ctx);

    PROFILE_END(ui_render);

    /*
     * If there was a selection, find the row by PID (stable across
     * re-sorts) and use GTK's scroll_to_cell to place it at the same
     * viewport fraction as before.  This avoids manual bin-window
     * coordinate arithmetic which is unreliable across re-sorts.
     */
    if (have_sel && sel_pid > 0) {
        /* Find the PID+pinned_root in whichever store the sort model
         * is wrapping.  This ensures that when a pinned copy was
         * selected we re-select the pinned copy, not the original
         * tree row (which has the same PID but a different
         * COL_PINNED_ROOT).  Fall back to a plain PID match if the
         * exact copy is gone (e.g. process exited). */
        GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
            ctx->sort_model);
        GtkTreeIter found_iter;
        if (find_iter_by_pid_and_pinned_root(child_model, NULL,
                             sel_pid, sel_pinned_root, &found_iter) ||
            find_iter_by_pid(child_model, NULL,
                             sel_pid, &found_iter)) {
            GtkTreePath *child_path = gtk_tree_model_get_path(
                child_model, &found_iter);
            if (child_path) {
                GtkTreePath *sort_path =
                    gtk_tree_model_sort_convert_child_path_to_path(
                        ctx->sort_model, child_path);
                if (sort_path) {
                    /* Re-select (sort may have moved the iter) */
                    GtkTreeSelection *sel =
                        gtk_tree_view_get_selection(ctx->view);
                    gtk_tree_selection_select_path(sel, sort_path);
                    gtk_tree_path_free(sort_path);
                }
                gtk_tree_path_free(child_path);
            }
        }
    }

    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);
    g_signal_handlers_unblock_by_func(tree_sel, on_selection_changed, ctx);

    /* Update the sidebar detail panel for the selected process */
    sidebar_update(ctx);

    /* ── Plugin broker dispatch ──────────────────────────────── */
    /* After the sidebar updates, kick off the plugin data broker.
     * Non-pinned instances follow the tree selection. */
    {
        plugin_registry_t *preg = ctx->plugin_registry;
        if (preg && preg->count > 0) {
            /* Determine the currently selected PID */
            pid_t sel_pid = 0;
            GtkTreeSelection *psel =
                gtk_tree_view_get_selection(ctx->view);
            if (psel) {
                GList *prows =
                    gtk_tree_selection_get_selected_rows(psel, NULL);
                if (prows) {
                    GtkTreePath *pp = prows->data;
                    GtkTreePath *cpath =
                        gtk_tree_model_sort_convert_path_to_child_path(
                            ctx->sort_model, pp);
                    GtkTreeModel *cmodel =
                        gtk_tree_model_sort_get_model(ctx->sort_model);
                    GtkTreeIter citer;
                    if (cpath && gtk_tree_model_get_iter(cmodel, &citer,
                                                         cpath)) {
                        gint v = 0;
                        gtk_tree_model_get(cmodel, &citer,
                                           COL_PID, &v, -1);
                        sel_pid = (pid_t)v;
                    }
                    if (cpath) gtk_tree_path_free(cpath);
                    g_list_free_full(prows,
                                     (GDestroyNotify)gtk_tree_path_free);
                }
            }

            /* Update follow-selection instances to the selected PID */
            for (size_t i = 0; i < preg->count; i++) {
                plugin_instance_t *inst = &preg->instances[i];
                if (!inst->pinned)
                    plugin_instance_set_pid(inst, sel_pid, FALSE);
            }

            /* If nothing is selected, clear all plugins */
            if (sel_pid <= 0) {
                plugin_dispatch_clear_all(preg);
            } else {
                /* Start the broker gather (cancels any in-flight cycle) */
                broker_start(preg, ctx->mon ? ctx->mon->fdmon : NULL);
            }
        }
    }

    /* Update system info (right side of status bar) */
    {
        /* uptime from /proc/uptime */
        double uptime_secs = 0;
        FILE *f = fopen("/proc/uptime", "r");
        if (f) { int r_ = fscanf(f, "%lf", &uptime_secs); (void)r_; fclose(f); }
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
        if (f) { int r_ = fscanf(f, "%lf %lf %lf", &load1, &load5, &load15); (void)r_; fclose(f); }

        char sysinfo[256];
        snprintf(sysinfo, sizeof(sysinfo),
                 "up %dd %dh %dm  |  %d user%s  |  load: %.2f %.2f %.2f ",
                 up_days, up_hours, up_mins,
                 nusers, nusers == 1 ? "" : "s",
                 load1, load5, load15);
        gtk_label_set_text(ctx->status_right, sysinfo);
    }

    /* Update status bar (left side) */
    long mem_total_kb = 0, mem_avail_kb = 0;
    {
        FILE *mf = fopen("/proc/meminfo", "r");
        if (mf) {
            char line[256];
            while (fgets(line, sizeof(line), mf)) {
                if (sscanf(line, "MemTotal: %ld kB", &mem_total_kb) == 1)
                    continue;
                if (sscanf(line, "MemAvailable: %ld kB", &mem_avail_kb) == 1)
                    continue;
            }
            fclose(mf);
        }
    }
    long mem_used_kb = mem_total_kb - mem_avail_kb;
    if (mem_used_kb < 0) mem_used_kb = 0;

    char mem_used_buf[32], mem_total_buf[32];
    format_memory(mem_used_kb, mem_used_buf, sizeof(mem_used_buf));
    format_memory(mem_total_kb, mem_total_buf, sizeof(mem_total_buf));

    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 1;

    double ui_last = 0, ui_avg = 0, ui_max = 0;
    profile_get("ui_render", &ui_last, &ui_avg, &ui_max);

    char status[512];
    snprintf(status, sizeof(status),
             " %zu processes  |  %ld CPUs  |  memory: %s / %s  |  "
             "render: %.1f ms (avg %.1f, max %.1f)",
             count, ncpus,
             mem_used_buf, mem_total_buf,
             ui_last, ui_avg, ui_max);
    gtk_label_set_text(ctx->status_label, status);

    { proc_snapshot_t tmp = { local, count }; proc_snapshot_free(&tmp); }
    return G_SOURCE_CONTINUE;

finish:
    /* Unblock signals that were blocked above */
    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);
    g_signal_handlers_unblock_by_func(tree_sel, on_selection_changed, ctx);

    /* First refresh done – finish status update, then remove the fast timer */
    {
        long mem_total_kb = 0, mem_avail_kb = 0;
        {
            FILE *mf = fopen("/proc/meminfo", "r");
            if (mf) {
                char line[256];
                while (fgets(line, sizeof(line), mf)) {
                    if (sscanf(line, "MemTotal: %ld kB", &mem_total_kb) == 1)
                        continue;
                    if (sscanf(line, "MemAvailable: %ld kB", &mem_avail_kb) == 1)
                        continue;
                }
                fclose(mf);
            }
        }
        long mem_used_kb = mem_total_kb - mem_avail_kb;
        if (mem_used_kb < 0) mem_used_kb = 0;

        char mem_used_buf[32], mem_total_buf[32];
        format_memory(mem_used_kb, mem_used_buf, sizeof(mem_used_buf));
        format_memory(mem_total_kb, mem_total_buf, sizeof(mem_total_buf));

        long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpus < 1) ncpus = 1;

        double ui_last = 0, ui_avg = 0, ui_max = 0;
        profile_get("ui_render", &ui_last, &ui_avg, &ui_max);

        char status[512];
        snprintf(status, sizeof(status),
                 " %zu processes  |  %ld CPUs  |  memory: %s / %s  |  "
                 "render: %.1f ms (avg %.1f, max %.1f)",
                 count, ncpus,
                 mem_used_buf, mem_total_buf,
                 ui_last, ui_avg, ui_max);
        gtk_label_set_text(ctx->status_label, status);
    }
    { proc_snapshot_t tmp = { local, count }; proc_snapshot_free(&tmp); }
    return G_SOURCE_REMOVE;
}

/* ── menu bar actions ─────────────────────────────────────────── */

static void on_menu_exit(GtkMenuItem *item, gpointer data)
{
    (void)item;
    GtkWidget *window = data;
    gtk_widget_destroy(window);
}

/* ── GTK theme discovery & selection ──────────────────────────── */

static int theme_name_cmp(const void *a, const void *b)
{
    return g_ascii_strcasecmp(*(const char **)a, *(const char **)b);
}

/*
 * Collect available GTK3 theme names from the standard search paths.
 * A directory is a valid theme if it contains gtk-3.0/gtk.css.
 * Returns a NULL-terminated array of strdup'd names (caller frees).
 */
static char **collect_gtk_themes(int *out_count)
{
    /* Candidate directories: ~/.themes, ~/.local/share/themes,
     * $XDG_DATA_DIRS/themes (default /usr/share/themes) */
    const char *home = g_get_home_dir();
    char buf[PATH_MAX];

    /* Build a list of base directories to scan */
    const char *bases[16];
    int nbases = 0;

    static char home_themes[PATH_MAX];
    static char home_local[PATH_MAX];
    snprintf(home_themes, sizeof(home_themes), "%s/.themes", home);
    snprintf(home_local,  sizeof(home_local),  "%s/.local/share/themes", home);
    bases[nbases++] = home_themes;
    bases[nbases++] = home_local;

    /* Parse XDG_DATA_DIRS (colon-separated) */
    const char *xdg = g_getenv("XDG_DATA_DIRS");
    if (!xdg || !xdg[0])
        xdg = "/usr/local/share:/usr/share";

    char *xdg_copy = strdup(xdg);
    char *saveptr = NULL;
    for (char *tok = strtok_r(xdg_copy, ":", &saveptr);
         tok && nbases < 14;
         tok = strtok_r(NULL, ":", &saveptr)) {
        /* We'll construct the full path later with /themes appended */
        bases[nbases++] = tok;  /* points into xdg_copy, alive until free */
    }

    /* Collect theme names into a dynamic array */
    int cap = 64, count = 0;
    char **names = malloc(cap * sizeof(char *));

    for (int i = 0; i < nbases; i++) {
        /* For XDG_DATA_DIRS entries, append /themes */
        const char *dir;
        char full[PATH_MAX];
        if (i < 2) {
            dir = bases[i];  /* already has /themes */
        } else {
            snprintf(full, sizeof(full), "%s/themes", bases[i]);
            dir = full;
        }

        DIR *d = opendir(dir);
        if (!d) continue;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            /* Check for gtk-3.0/gtk.css inside the theme dir */
            int n = snprintf(buf, sizeof(buf), "%s/%s/gtk-3.0/gtk.css",
                             dir, ent->d_name);
            if (n < 0 || (size_t)n >= sizeof(buf))
                continue;  /* path too long – skip */
            struct stat st;
            if (stat(buf, &st) != 0 || !S_ISREG(st.st_mode))
                continue;

            /* Skip duplicates */
            gboolean dup = FALSE;
            for (int j = 0; j < count; j++) {
                if (strcmp(names[j], ent->d_name) == 0) {
                    dup = TRUE;
                    break;
                }
            }
            if (dup) continue;

            if (count + 1 >= cap) {
                cap *= 2;
                char **tmp = realloc(names, cap * sizeof(char *));
                if (!tmp) break;   /* OOM – stop collecting */
                names = tmp;
            }
            names[count++] = strdup(ent->d_name);
        }
        closedir(d);
    }
    free(xdg_copy);

    /* Sort alphabetically */
    if (count > 1)
        qsort(names, count, sizeof(char *), theme_name_cmp);

    names[count] = NULL;
    if (out_count) *out_count = count;
    return names;
}

static void on_theme_selected(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    if (!gtk_check_menu_item_get_active(item))
        return;  /* ignore deactivation of the old radio item */

    const char *name = gtk_menu_item_get_label(GTK_MENU_ITEM(item));
    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-theme-name", name, NULL);
}

/*
 * Build a "Theme" submenu with radio items for each discovered GTK3 theme.
 * The currently active theme gets the radio bullet.
 */
static GtkWidget *build_theme_submenu(void)
{
    GtkWidget *menu = gtk_menu_new();

    /* Get current theme name */
    GtkSettings *settings = gtk_settings_get_default();
    char *current = NULL;
    g_object_get(settings, "gtk-theme-name", &current, NULL);

    int count = 0;
    char **themes = collect_gtk_themes(&count);

    GSList *group = NULL;
    for (int i = 0; i < count; i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_label(group, themes[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));

        if (current && strcmp(themes[i], current) == 0)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);

        g_signal_connect(item, "toggled", G_CALLBACK(on_theme_selected), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        free(themes[i]);
    }
    free(themes);
    g_free(current);

    return menu;
}

/* ── neofetch-style About dialog (from about.inc) ────────────── */

/*
 * Read a value from /etc/os-release.  Looks for lines like:
 *   KEY="value"   or   KEY=value
 * Returns a strdup'd string (caller frees), or NULL if not found.
 */
static char *read_os_release_field(const char *key)
{
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) return NULL;

    size_t klen = strlen(key);
    char line[512];
    char *result = NULL;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *val = line + klen + 1;
            /* Strip trailing newline */
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r'))
                val[--vlen] = '\0';
            /* Strip surrounding quotes */
            if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
                val[vlen - 1] = '\0';
                val++;
            }
            result = strdup(val);
            break;
        }
    }
    fclose(f);
    return result;
}

#include "about.inc"

/* ── font helpers ─────────────────────────────────────────────── */

#define FONT_SIZE_MIN  6
#define FONT_SIZE_MAX  30
#define FONT_SIZE_DEFAULT 9

static void reload_font_css(ui_ctx_t *ctx)
{
    char buf[256];

    /* Main process tree */
    snprintf(buf, sizeof(buf),
             "treeview { font-family: Monospace; font-size: %dpt; }",
             ctx->font_size);
    gtk_css_provider_load_from_data(ctx->css, buf, -1, NULL);

    /* Sidebar labels and frame title */
    if (ctx->sidebar_css) {
        snprintf(buf, sizeof(buf),
                 "frame, label, checkbutton { font-size: %dpt; }",
                 ctx->font_size);
        gtk_css_provider_load_from_data(ctx->sidebar_css, buf, -1, NULL);
    }
}

/* ── desktop-environment–aware modifier detection ─────────────── */

/*
 * Detect whether the user's desktop shortcuts use Meta (Super) instead
 * of Ctrl as the primary application-shortcut modifier.
 *
 * Strategy (checked in order):
 *
 *  1. KDE  – parse ~/.config/kdeglobals [Shortcuts] section.
 *            If Copy=Meta+C (or similar), the user has macOS-style
 *            bindings → use GDK_META_MASK.
 *
 *  2. GTK 3 – parse ~/.config/gtk-3.0/settings.ini for
 *             gtk-key-theme-name.  "Mac" or "Emacs" themes remap
 *             shortcuts, but in practice GTK apps always use Ctrl
 *             unless the KDE portal overrides it (handled by #1).
 *
 *  3. GTK 2 – ~/.gtkrc-2.0 gtk-key-theme-name, same idea.
 *
 *  4. Fallback – Ctrl (the universal default).
 */
static GdkModifierType detect_shortcut_modifier(void)
{
    /* ── 1. KDE: ~/.config/kdeglobals [Shortcuts] ─────────────── */
    const char *config_home = g_getenv("XDG_CONFIG_HOME");
    char path[PATH_MAX];
    if (config_home && config_home[0])
        snprintf(path, sizeof(path), "%s/kdeglobals", config_home);
    else
        snprintf(path, sizeof(path), "%s/.config/kdeglobals",
                 g_get_home_dir());

    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[512];
        gboolean in_shortcuts = FALSE;
        while (fgets(line, sizeof(line), fp)) {
            /* strip trailing whitespace */
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'
                           || line[len - 1] == ' '))
                line[--len] = '\0';

            if (line[0] == '[') {
                in_shortcuts = (g_ascii_strcasecmp(line, "[Shortcuts]") == 0);
                continue;
            }
            if (!in_shortcuts)
                continue;

            /* Look for  Copy=Meta+C  or  Paste=Meta+V  etc. */
            if (g_str_has_prefix(line, "Copy=") ||
                g_str_has_prefix(line, "Paste=") ||
                g_str_has_prefix(line, "Cut=") ||
                g_str_has_prefix(line, "SelectAll=")) {
                const char *val = strchr(line, '=');
                if (val && g_ascii_strncasecmp(val + 1, "Meta+", 5) == 0) {
                    fclose(fp);
                    return GDK_META_MASK;
                }
                /* Explicitly set to something other than Meta → Ctrl */
                if (val && val[1] != '\0') {
                    fclose(fp);
                    return GDK_CONTROL_MASK;
                }
            }
        }
        fclose(fp);
    }

    /* ── 2. GTK 3: ~/.config/gtk-3.0/settings.ini ────────────── */
    if (config_home && config_home[0])
        snprintf(path, sizeof(path), "%s/gtk-3.0/settings.ini", config_home);
    else
        snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini",
                 g_get_home_dir());

    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *theme = g_key_file_get_string(kf, "Settings",
                                            "gtk-key-theme-name", NULL);
        if (theme) {
            /* "Mac" key theme remaps shortcuts to Meta */
            gboolean is_mac = (g_ascii_strcasecmp(theme, "Mac") == 0);
            g_free(theme);
            g_key_file_free(kf);
            return is_mac ? GDK_META_MASK : GDK_CONTROL_MASK;
        }
    }
    g_key_file_free(kf);

    /* ── 3. GTK 2: ~/.gtkrc-2.0 ──────────────────────────────── */
    snprintf(path, sizeof(path), "%s/.gtkrc-2.0", g_get_home_dir());
    fp = fopen(path, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            /* gtk-key-theme-name="Mac" */
            if (strstr(line, "gtk-key-theme-name")) {
                gboolean is_mac = (strcasestr(line, "\"Mac\"") != NULL);
                fclose(fp);
                return is_mac ? GDK_META_MASK : GDK_CONTROL_MASK;
            }
        }
        fclose(fp);
    }

    /* ── 4. Fallback: Ctrl (universal default) ────────────────── */
    return GDK_CONTROL_MASK;
}

/* ── name-filter helpers ──────────────────────────────────────── */

/*
 * Return TRUE if the row's COL_NAME contains the filter text
 * (case-insensitive substring match).
 */
static gboolean row_name_matches(GtkTreeModel *model,
                                 GtkTreeIter  *iter,
                                 const char   *filter_lower)
{
    gchar *name = NULL;
    gtk_tree_model_get(model, iter, COL_NAME, &name, -1);
    if (!name)
        return FALSE;
    gchar *name_down = g_utf8_strdown(name, -1);
    gboolean match = (strstr(name_down, filter_lower) != NULL);
    g_free(name_down);
    g_free(name);
    return match;
}

/*
 * Deep-copy a subtree from `src` into `dst` under `dst_parent`.
 * Copies the row at `src_iter` and all its children, recursively.
 */
static void copy_subtree(GtkTreeStore *dst, GtkTreeIter *dst_parent,
                         GtkTreeModel *src, GtkTreeIter *src_iter)
{
    GtkTreeIter dst_iter;
    gtk_tree_store_append(dst, &dst_iter, dst_parent);

    /* Copy all column values */
    gint pid, ppid, cpu, rss, grp_rss, grp_cpu, pinned_root;
    gint io_read_rate, io_write_rate;
    gint64 start_time, hl_born, hl_died;
    gchar *user = NULL, *name = NULL, *cpu_text = NULL, *rss_text = NULL;
    gchar *grp_rss_text = NULL, *grp_cpu_text = NULL;
    gchar *io_read_text = NULL, *io_write_text = NULL;
    gchar *start_text = NULL, *container = NULL, *service = NULL,
          *cwd = NULL, *cmdline = NULL, *steam_label = NULL;
    gchar *spark_data = NULL;
    gint spark_peak;

    gtk_tree_model_get(src, src_iter,
        COL_PID, &pid, COL_PPID, &ppid, COL_USER, &user, COL_NAME, &name,
        COL_CPU, &cpu, COL_CPU_TEXT, &cpu_text,
        COL_RSS, &rss, COL_RSS_TEXT, &rss_text,
        COL_GROUP_RSS, &grp_rss, COL_GROUP_RSS_TEXT, &grp_rss_text,
        COL_GROUP_CPU, &grp_cpu, COL_GROUP_CPU_TEXT, &grp_cpu_text,
        COL_IO_READ_RATE, &io_read_rate, COL_IO_READ_RATE_TEXT, &io_read_text,
        COL_IO_WRITE_RATE, &io_write_rate, COL_IO_WRITE_RATE_TEXT, &io_write_text,
        COL_START_TIME, &start_time, COL_START_TIME_TEXT, &start_text,
        COL_CONTAINER, &container, COL_SERVICE, &service,
        COL_CWD, &cwd, COL_CMDLINE, &cmdline,
        COL_STEAM_LABEL, &steam_label,
        COL_IO_SPARKLINE, &spark_data,
        COL_IO_SPARKLINE_PEAK, &spark_peak,
        COL_HIGHLIGHT_BORN, &hl_born, COL_HIGHLIGHT_DIED, &hl_died,
        COL_PINNED_ROOT, &pinned_root,
        -1);

    gtk_tree_store_set(dst, &dst_iter,
        COL_PID, pid, COL_PPID, ppid, COL_USER, user, COL_NAME, name,
        COL_CPU, cpu, COL_CPU_TEXT, cpu_text,
        COL_RSS, rss, COL_RSS_TEXT, rss_text,
        COL_GROUP_RSS, grp_rss, COL_GROUP_RSS_TEXT, grp_rss_text,
        COL_GROUP_CPU, grp_cpu, COL_GROUP_CPU_TEXT, grp_cpu_text,
        COL_IO_READ_RATE, io_read_rate, COL_IO_READ_RATE_TEXT, io_read_text,
        COL_IO_WRITE_RATE, io_write_rate, COL_IO_WRITE_RATE_TEXT, io_write_text,
        COL_START_TIME, start_time, COL_START_TIME_TEXT, start_text,
        COL_CONTAINER, container, COL_SERVICE, service,
        COL_CWD, cwd, COL_CMDLINE, cmdline,
        COL_STEAM_LABEL, steam_label,
        COL_IO_SPARKLINE, spark_data,
        COL_IO_SPARKLINE_PEAK, spark_peak,
        COL_HIGHLIGHT_BORN, hl_born, COL_HIGHLIGHT_DIED, hl_died,
        COL_PINNED_ROOT, pinned_root,
        -1);

    g_free(user); g_free(name); g_free(cpu_text); g_free(rss_text);
    g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(io_read_text); g_free(io_write_text); g_free(start_text);
    g_free(container); g_free(service); g_free(cwd); g_free(cmdline);
    g_free(steam_label); g_free(spark_data);

    /* Recurse into children */
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(src, &child, src_iter);
    while (valid) {
        copy_subtree(dst, &dst_iter, src, &child);
        valid = gtk_tree_model_iter_next(src, &child);
    }
}

/*
 * Walk the real store and find rows whose name matches the filter.
 * For each match, copy the entire subtree (the match + all its
 * descendants) into the filter_store as a new top-level root.
 *
 * Skips rows that are descendants of an already-matched ancestor
 * (they're already included via copy_subtree).
 */
static void find_and_copy_matches(GtkTreeStore *dst, GtkTreeModel *src,
                                   GtkTreeIter *parent,
                                   const char *filter_lower,
                                   gboolean ancestor_matched)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(src, &iter, parent);
    while (valid) {
        gboolean self_matches = row_name_matches(src, &iter, filter_lower);

        if (ancestor_matched) {
            /* Already copied as part of a parent's subtree – skip */
        } else if (self_matches) {
            /* This row matches: copy it + full subtree as a top-level root */
            copy_subtree(dst, NULL, src, &iter);
        } else {
            /* No match here – recurse to look for matches deeper */
            find_and_copy_matches(dst, src, &iter, filter_lower, FALSE);
        }

        valid = gtk_tree_model_iter_next(src, &iter);
    }
}

/*
 * Update the column values of a single filter_store row from the
 * corresponding row in the real store (found by PID).
 */
static void sync_row_from_real(GtkTreeStore *fs, GtkTreeIter *fs_iter,
                               GtkTreeModel *real, GtkTreeIter *real_iter)
{
    gint pid, ppid, cpu, rss, grp_rss, grp_cpu;
    gint io_read_rate, io_write_rate;
    gint64 start_time, hl_born, hl_died;
    gchar *user = NULL, *name = NULL, *cpu_text = NULL, *rss_text = NULL;
    gchar *grp_rss_text = NULL, *grp_cpu_text = NULL;
    gchar *io_read_text = NULL, *io_write_text = NULL;
    gchar *start_text = NULL, *container = NULL, *service = NULL,
          *cwd = NULL, *cmdline = NULL, *steam_label = NULL;
    gchar *spark_data = NULL;
    gint spark_peak;

    gtk_tree_model_get(real, real_iter,
        COL_PID, &pid, COL_PPID, &ppid, COL_USER, &user, COL_NAME, &name,
        COL_CPU, &cpu, COL_CPU_TEXT, &cpu_text,
        COL_RSS, &rss, COL_RSS_TEXT, &rss_text,
        COL_GROUP_RSS, &grp_rss, COL_GROUP_RSS_TEXT, &grp_rss_text,
        COL_GROUP_CPU, &grp_cpu, COL_GROUP_CPU_TEXT, &grp_cpu_text,
        COL_IO_READ_RATE, &io_read_rate, COL_IO_READ_RATE_TEXT, &io_read_text,
        COL_IO_WRITE_RATE, &io_write_rate, COL_IO_WRITE_RATE_TEXT, &io_write_text,
        COL_START_TIME, &start_time, COL_START_TIME_TEXT, &start_text,
        COL_CONTAINER, &container, COL_SERVICE, &service,
        COL_CWD, &cwd, COL_CMDLINE, &cmdline,
        COL_STEAM_LABEL, &steam_label,
        COL_IO_SPARKLINE, &spark_data,
        COL_IO_SPARKLINE_PEAK, &spark_peak,
        COL_HIGHLIGHT_BORN, &hl_born, COL_HIGHLIGHT_DIED, &hl_died,
        -1);

    /* Preserve the pinned_root value already in the filter store row
     * (it was set when the row was created and should not change). */
    gint pinned_root;
    gtk_tree_model_get(GTK_TREE_MODEL(fs), fs_iter,
                       COL_PINNED_ROOT, &pinned_root, -1);

    gtk_tree_store_set(fs, fs_iter,
        COL_PID, pid, COL_PPID, ppid, COL_USER, user, COL_NAME, name,
        COL_CPU, cpu, COL_CPU_TEXT, cpu_text,
        COL_RSS, rss, COL_RSS_TEXT, rss_text,
        COL_GROUP_RSS, grp_rss, COL_GROUP_RSS_TEXT, grp_rss_text,
        COL_GROUP_CPU, grp_cpu, COL_GROUP_CPU_TEXT, grp_cpu_text,
        COL_IO_READ_RATE, io_read_rate, COL_IO_READ_RATE_TEXT, io_read_text,
        COL_IO_WRITE_RATE, io_write_rate, COL_IO_WRITE_RATE_TEXT, io_write_text,
        COL_START_TIME, start_time, COL_START_TIME_TEXT, start_text,
        COL_CONTAINER, container, COL_SERVICE, service,
        COL_CWD, cwd, COL_CMDLINE, cmdline,
        COL_STEAM_LABEL, steam_label,
        COL_IO_SPARKLINE, spark_data,
        COL_IO_SPARKLINE_PEAK, spark_peak,
        COL_HIGHLIGHT_BORN, hl_born, COL_HIGHLIGHT_DIED, hl_died,
        COL_PINNED_ROOT, pinned_root,
        -1);

    g_free(user); g_free(name); g_free(cpu_text); g_free(rss_text);
    g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(io_read_text); g_free(io_write_text); g_free(start_text);
    g_free(container); g_free(service); g_free(cwd); g_free(cmdline);
    g_free(steam_label); g_free(spark_data);
}

/*
 * Recursively update all rows in the filter_store from the real store.
 * Returns FALSE if any PID in the filter_store is missing from the real
 * store (i.e. a process has exited and the structure needs a rebuild).
 */
static gboolean sync_filter_rows(GtkTreeStore *fs, GtkTreeIter *fs_parent,
                                  GtkTreeModel *real)
{
    GtkTreeIter fs_iter;
    gboolean valid = fs_parent
        ? gtk_tree_model_iter_children(GTK_TREE_MODEL(fs), &fs_iter, fs_parent)
        : gtk_tree_model_get_iter_first(GTK_TREE_MODEL(fs), &fs_iter);

    while (valid) {
        gint pid;
        gtk_tree_model_get(GTK_TREE_MODEL(fs), &fs_iter, COL_PID, &pid, -1);

        GtkTreeIter real_iter;
        if (!find_iter_by_pid(real, NULL, (pid_t)pid, &real_iter))
            return FALSE;  /* process vanished – need full rebuild */

        sync_row_from_real(fs, &fs_iter, real, &real_iter);

        /* Recurse into children */
        if (!sync_filter_rows(fs, &fs_iter, real))
            return FALSE;

        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(fs), &fs_iter);
    }
    return TRUE;
}

/*
 * Count how many top-level matches the current filter would produce
 * from the real store.  Used to detect when a new process appears
 * that matches the filter (structural change requiring rebuild).
 */
static int count_filter_matches(GtkTreeModel *real, GtkTreeIter *parent,
                                 const char *filter_lower,
                                 gboolean ancestor_matched)
{
    int count = 0;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(real, &iter, parent);
    while (valid) {
        gboolean self_matches = row_name_matches(real, &iter, filter_lower);
        if (ancestor_matched) {
            /* This row is part of a matched parent's subtree */
            count++;
            count += count_filter_matches(real, &iter, filter_lower, TRUE);
        } else if (self_matches) {
            count++;
            /* Children are part of this subtree, count them too */
            count += count_filter_matches(real, &iter, filter_lower, TRUE);
        } else {
            count += count_filter_matches(real, &iter, filter_lower, FALSE);
        }
        valid = gtk_tree_model_iter_next(real, &iter);
    }
    return count;
}

/*
 * Count total rows in the filter store (all levels).
 */
static int count_store_rows(GtkTreeModel *model, GtkTreeIter *parent)
{
    int count = 0;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        count++;
        count += count_store_rows(model, &iter);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return count;
}

/*
 * Incremental sync of the filter store: update column values in-place
 * from the real store.  If the structure has changed (process died or
 * a new process matches), fall back to a full rebuild.
 *
 * This preserves the view's expand/collapse state, selection, and
 * scroll position across normal refresh ticks.
 */
static void sync_filter_store(ui_ctx_t *ctx)
{
    if (!ctx->filter_store || ctx->filter_text[0] == '\0')
        return;

    GtkTreeModel *real = GTK_TREE_MODEL(ctx->store);

    /* Quick structural check: has the total number of matching rows changed? */
    gchar *filter_lower = g_utf8_strdown(ctx->filter_text, -1);
    int expected = count_filter_matches(real, NULL, filter_lower, FALSE);
    int current  = count_store_rows(GTK_TREE_MODEL(ctx->filter_store), NULL);
    g_free(filter_lower);

    if (expected != current) {
        /* Structure changed – fall back to full rebuild */
        rebuild_filter_store(ctx);
        return;
    }

    /* Structure is stable – just update column values in place */
    if (!sync_filter_rows(ctx->filter_store, NULL, real)) {
        /* A PID vanished mid-walk – fall back to rebuild */
        rebuild_filter_store(ctx);
    }
}

/*
 * Register inverted sort functions on a sort model.  Called each time
 * we create a new GtkTreeModelSort (which happens when switching
 * between the real store and the filter store).
 */
static void register_sort_funcs(GtkTreeModelSort *sm);

/*
 * Expand every row in the tree view EXCEPT those whose PID appears
 * in the user-collapsed set.  This preserves manual collapse state
 * across model swaps.
 */
static void expand_respecting_collapsed_recurse(ui_ctx_t *ctx,
                                                 GtkTreeModel *model,
                                                 GtkTreeIter *parent)
{
    GtkTreeIter iter;
    gboolean valid = parent
        ? gtk_tree_model_iter_children(model, &iter, parent)
        : gtk_tree_model_get_iter_first(model, &iter);

    while (valid) {
        if (gtk_tree_model_iter_has_child(model, &iter)) {
            GtkTreeIter child_it;
            gtk_tree_model_sort_convert_iter_to_child_iter(
                ctx->sort_model, &child_it, &iter);
            GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                ctx->sort_model);
            gint pid;
            gtk_tree_model_get(child_model, &child_it, COL_PID, &pid, -1);

            /* Use the row's pinned root for collapse/expand lookup */
            pid_t pinned_root = get_row_pinned_root(child_model, &child_it);

            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            if (get_process_tree_node(&ctx->ptree_nodes, pinned_root,
                                      (pid_t)pid) == PTREE_COLLAPSED) {
                gtk_tree_view_collapse_row(ctx->view, path);
            } else {
                gtk_tree_view_expand_row(ctx->view, path, FALSE);
                /* Recurse into children */
                expand_respecting_collapsed_recurse(ctx, model, &iter);
            }
            gtk_tree_path_free(path);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

static void expand_respecting_collapsed(ui_ctx_t *ctx)
{
    GtkTreeModel *model = gtk_tree_view_get_model(ctx->view);
    if (!model) return;
    expand_respecting_collapsed_recurse(ctx, model, NULL);
}

/*
 * Rebuild the shadow filter_store from scratch and point the sort
 * model at it.  Called whenever the filter text changes (non-empty)
 * or when the underlying store is refreshed while a filter is active.
 *
 * The original store is never modified.
 */
static void rebuild_filter_store(ui_ctx_t *ctx)
{
    /* Remember the currently selected PID so we can re-select it
     * after the model swap (set_model clears the selection). */
    pid_t sel_pid = 0;
    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
        GtkTreeModel *old_model = NULL;
        GtkTreeIter sel_iter;
        if (sel && gtk_tree_selection_get_selected(sel, &old_model, &sel_iter)) {
            GtkTreeIter child_iter;
            gtk_tree_model_sort_convert_iter_to_child_iter(
                ctx->sort_model, &child_iter, &sel_iter);
            GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                ctx->sort_model);
            gint pid_val;
            gtk_tree_model_get(child_model, &child_iter, COL_PID, &pid_val, -1);
            sel_pid = (pid_t)pid_val;
        }
    }

    gchar *filter_lower = g_utf8_strdown(ctx->filter_text, -1);

    /* Create a fresh filter store */
    GtkTreeStore *fs = gtk_tree_store_new(NUM_COLS,
        G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
        G_TYPE_INT64, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_INT,
        G_TYPE_INT64, G_TYPE_INT64, G_TYPE_INT);

    find_and_copy_matches(fs, GTK_TREE_MODEL(ctx->store), NULL,
                          filter_lower, FALSE);
    g_free(filter_lower);

    /* Replace old filter_store */
    if (ctx->filter_store)
        g_object_unref(ctx->filter_store);
    ctx->filter_store = fs;

    /* Save / restore sort column across the switch */
    gint sort_col = GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID;
    GtkSortType sort_order = GTK_SORT_ASCENDING;
    gtk_tree_sortable_get_sort_column_id(
        GTK_TREE_SORTABLE(ctx->sort_model), &sort_col, &sort_order);

    /* Build a new sort model wrapping the filter store */
    GtkTreeModel *new_sort = gtk_tree_model_sort_new_with_model(
        GTK_TREE_MODEL(fs));
    ctx->sort_model = GTK_TREE_MODEL_SORT(new_sort);
    register_sort_funcs(ctx->sort_model);

    if (sort_col != GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID)
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(ctx->sort_model), sort_col, sort_order);

    gtk_tree_view_set_model(ctx->view, new_sort);

    g_signal_handlers_block_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_block_by_func(ctx->view, on_row_expanded,  ctx);
    expand_respecting_collapsed(ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);

    g_signal_connect(new_sort, "sort-column-changed",
                     G_CALLBACK(on_sort_column_changed), ctx);

    /* Re-select the previously-selected PID in the new model */
    if (sel_pid > 0) {
        GtkTreeIter found;
        if (find_iter_by_pid(GTK_TREE_MODEL(fs), NULL, sel_pid, &found)) {
            GtkTreePath *child_path = gtk_tree_model_get_path(
                GTK_TREE_MODEL(fs), &found);
            if (child_path) {
                GtkTreePath *sort_path =
                    gtk_tree_model_sort_convert_child_path_to_path(
                        ctx->sort_model, child_path);
                if (sort_path) {
                    GtkTreeSelection *sel =
                        gtk_tree_view_get_selection(ctx->view);
                    gtk_tree_selection_select_path(sel, sort_path);
                    gtk_tree_path_free(sort_path);
                }
                gtk_tree_path_free(child_path);
            }
        }
    }
}

/*
 * Switch the view back to the real (unfiltered) store.  Called when
 * the filter is cleared.
 */
static void switch_to_real_store(ui_ctx_t *ctx)
{
    if (ctx->filter_store) {
        g_object_unref(ctx->filter_store);
        ctx->filter_store = NULL;
    }

    gint sort_col = GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID;
    GtkSortType sort_order = GTK_SORT_ASCENDING;
    gtk_tree_sortable_get_sort_column_id(
        GTK_TREE_SORTABLE(ctx->sort_model), &sort_col, &sort_order);

    GtkTreeModel *new_sort = gtk_tree_model_sort_new_with_model(
        GTK_TREE_MODEL(ctx->store));
    ctx->sort_model = GTK_TREE_MODEL_SORT(new_sort);
    register_sort_funcs(ctx->sort_model);

    if (sort_col != GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID)
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(ctx->sort_model), sort_col, sort_order);

    gtk_tree_view_set_model(ctx->view, new_sort);

    g_signal_handlers_block_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_block_by_func(ctx->view, on_row_expanded,  ctx);
    expand_respecting_collapsed(ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);

    g_signal_connect(new_sort, "sort-column-changed",
                     G_CALLBACK(on_sort_column_changed), ctx);
}

/*
 * Position the filter entry in the overlay so it sits in the column
 * header row, right after the "Name" label text.
 *
 * We query the Name column's header button allocation to find where
 * the header text ends, then place the entry immediately to its right.
 */
static gboolean on_overlay_get_child_position(GtkOverlay   *overlay,
                                              GtkWidget    *child,
                                              GdkRectangle *alloc,
                                              gpointer      data)
{
    (void)overlay;
    ui_ctx_t *ctx = data;

    /* Determine which column header to anchor to */
    GtkTreeViewColumn *target_col = NULL;
    const char        *label_text = NULL;

    if (child == ctx->filter_entry) {
        target_col = ctx->name_col;
        label_text = "Name";
    } else if (child == ctx->pid_entry) {
        target_col = ctx->pid_col;
        label_text = "PID";
    } else {
        return FALSE;   /* let GTK handle other overlay children */
    }

    /* Get the header button widget for the target column */
    GtkWidget *btn = gtk_tree_view_column_get_button(target_col);
    if (!btn || !gtk_widget_get_realized(btn))
        return FALSE;

    /* Get the header button's allocation relative to the tree */
    GtkAllocation btn_alloc;
    gtk_widget_get_allocation(btn, &btn_alloc);

    /* Translate the button's coordinates into the overlay's coordinate space */
    int ox = 0, oy = 0;
    GtkWidget *overlay_widget = GTK_WIDGET(overlay);
    gtk_widget_translate_coordinates(btn, overlay_widget, 0, 0, &ox, &oy);

    /* Measure the label text width using the button's Pango context */
    PangoLayout *layout = gtk_widget_create_pango_layout(btn, label_text);
    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    g_object_unref(layout);

    /* Measure the natural size of the entry */
    GtkRequisition entry_nat;
    gtk_widget_get_preferred_size(child, NULL, &entry_nat);

    /* Right-align the entry within the column header.
     * Use the entry's natural width, but clamp to the available
     * space (column width minus label text minus padding). */
    int margin  = 4;  /* pixels from column right edge */
    int min_gap = 6;  /* minimum gap between label and entry */
    int entry_h = entry_nat.height;
    if (entry_h > btn_alloc.height - 2)
        entry_h = btn_alloc.height - 2;

    int col_right = ox + btn_alloc.width - margin;
    int entry_w   = entry_nat.width;
    int max_w     = col_right - (ox + text_w + min_gap);
    if (max_w < 30) max_w = 30;
    if (entry_w > max_w) entry_w = max_w;

    alloc->x      = col_right - entry_w;
    alloc->y      = oy + (btn_alloc.height - entry_h) / 2;
    alloc->width  = entry_w;
    alloc->height = entry_h;

    return TRUE;
}

/* ── auto-hide timer for an empty, visible filter entry ────── */

static gboolean on_filter_hide_timeout(gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->filter_hide_timer = 0;

    /* Only hide if still empty */
    if (ctx->filter_text[0] == '\0' && ctx->filter_entry &&
        gtk_widget_get_visible(ctx->filter_entry)) {
        gtk_widget_hide(ctx->filter_entry);
    }
    return G_SOURCE_REMOVE;
}

static void filter_cancel_hide_timer(ui_ctx_t *ctx)
{
    if (ctx->filter_hide_timer) {
        g_source_remove(ctx->filter_hide_timer);
        ctx->filter_hide_timer = 0;
    }
}

static void filter_schedule_hide(ui_ctx_t *ctx)
{
    filter_cancel_hide_timer(ctx);
    ctx->filter_hide_timer = g_timeout_add(5000, on_filter_hide_timeout, ctx);
}

static gboolean on_filter_entry_key_release(GtkWidget *widget,
                                            GdkEventKey *ev,
                                            gpointer data)
{
    ui_ctx_t *ctx = data;

    if (ev->keyval == GDK_KEY_Escape) {
        filter_cancel_hide_timer(ctx);
        gtk_entry_set_text(GTK_ENTRY(ctx->filter_entry), "");
        ctx->filter_text[0] = '\0';
        switch_to_real_store(ctx);
        gtk_widget_hide(ctx->filter_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
        return TRUE;
    }

    /* Ignore releases with Ctrl/Meta held — the window key-press
     * handler already takes care of Ctrl+F / Meta+F toggling. */
    guint state = ev->state & gtk_accelerator_get_default_mod_mask();
    if (state & (GDK_CONTROL_MASK | GDK_META_MASK))
        return FALSE;

    /* Update filter text and rebuild shadow store on every plain key release */
    const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
    snprintf(ctx->filter_text, sizeof(ctx->filter_text), "%s", text ? text : "");

    if (ctx->filter_text[0] != '\0') {
        filter_cancel_hide_timer(ctx);
        rebuild_filter_store(ctx);
    } else {
        switch_to_real_store(ctx);
        filter_schedule_hide(ctx);
    }

    return FALSE;
}

/* ── Go-to-PID entry helpers ──────────────────────────────────── */

/* Reject any non-digit characters typed into the PID entry. */
static void on_pid_entry_insert_text(GtkEditable *editable,
                                     const gchar *text,
                                     gint         length,
                                     gint        *position,
                                     gpointer     data)
{
    (void)editable; (void)position; (void)data;
    for (gint i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') {
            g_signal_stop_emission_by_name(editable, "insert-text");
            return;
        }
    }
}

/*
 * Navigate to a PID in the tree view.  Exact match only —
 * typing "2" matches PID 2, not PID 22 or 200.
 */
static void goto_pid(ui_ctx_t *ctx, pid_t target)
{
    GtkTreeModel *child = gtk_tree_model_sort_get_model(ctx->sort_model);
    GtkTreeIter child_iter;

    if (!find_iter_by_pid(child, NULL, target, &child_iter))
        return;

    GtkTreePath *child_path = gtk_tree_model_get_path(child, &child_iter);
    if (!child_path)
        return;

    GtkTreePath *sort_path =
        gtk_tree_model_sort_convert_child_path_to_path(
            ctx->sort_model, child_path);
    gtk_tree_path_free(child_path);
    if (!sort_path)
        return;

    /* Expand all ancestors so the row is visible */
    GtkTreePath *parent_path = gtk_tree_path_copy(sort_path);
    while (gtk_tree_path_up(parent_path) &&
           gtk_tree_path_get_depth(parent_path) > 0) {
        gtk_tree_view_expand_row(ctx->view, parent_path, FALSE);
    }
    gtk_tree_path_free(parent_path);

    /* Select and scroll to the row */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    gtk_tree_selection_select_path(sel, sort_path);
    gtk_tree_view_scroll_to_cell(ctx->view, sort_path,
                                 NULL, TRUE, 0.5f, 0.0f);
    gtk_tree_path_free(sort_path);
}

static gboolean on_pid_entry_key_release(GtkWidget *widget,
                                         GdkEventKey *ev,
                                         gpointer data)
{
    ui_ctx_t *ctx = data;

    if (ev->keyval == GDK_KEY_Escape) {
        gtk_entry_set_text(GTK_ENTRY(ctx->pid_entry), "");
        gtk_widget_hide(ctx->pid_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
        return TRUE;
    }

    /* Enter/Return → dismiss the entry, keep current selection */
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        gtk_entry_set_text(GTK_ENTRY(ctx->pid_entry), "");
        gtk_widget_hide(ctx->pid_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
        return TRUE;
    }

    /* Ignore modifier-only releases */
    guint state = ev->state & gtk_accelerator_get_default_mod_mask();
    if (state & (GDK_CONTROL_MASK | GDK_META_MASK))
        return FALSE;

    /* Live search: navigate on every keystroke (exact PID match) */
    const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
    if (!text || !text[0])
        return FALSE;

    char *end = NULL;
    long val = strtol(text, &end, 10);
    if (end == text || *end != '\0' || val <= 0)
        return FALSE;   /* not a clean integer yet */

    goto_pid(ctx, (pid_t)val);
    return FALSE;
}

/* ── keyboard shortcuts ──────────────────────────────────────── */

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *ev,
                             gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    static GdkModifierType mod = 0;
    if (mod == 0)
        mod = detect_shortcut_modifier();

    /* Mask out lock bits (Caps Lock, Num Lock, etc.) */
    guint state = ev->state & gtk_accelerator_get_default_mod_mask();

    /* Track bare Alt tap: set flag on Alt press, clear if any other
     * key is pressed while Alt is held (Alt+<key> is not a bare tap). */
    if (ev->keyval == GDK_KEY_Alt_L || ev->keyval == GDK_KEY_Alt_R) {
        ctx->alt_pressed = TRUE;
        return FALSE;
    }
    if (state & GDK_MOD1_MASK)
        ctx->alt_pressed = FALSE;

    /* Escape while sidebar has focus → return focus to the tree view */
    if (ev->keyval == GDK_KEY_Escape && state == 0) {
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(widget));
        if (focus && gtk_widget_get_visible(ctx->sidebar) &&
            gtk_widget_is_ancestor(focus, ctx->sidebar)) {
            gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
            return TRUE;
        }
    }

    /* Left / Right arrow → collapse / expand selected row in tree view */
    if (state == 0 &&
        (ev->keyval == GDK_KEY_Left || ev->keyval == GDK_KEY_Right)) {
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(widget));
        if (focus == GTK_WIDGET(ctx->view)) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
            GtkTreeModel *model = NULL;
            GtkTreeIter sort_iter;
            if (gtk_tree_selection_get_selected(sel, &model, &sort_iter)) {
                GtkTreePath *path = gtk_tree_model_get_path(model, &sort_iter);
                if (path) {
                    if (ev->keyval == GDK_KEY_Right) {
                        gtk_tree_view_expand_row(ctx->view, path, FALSE);
                    } else {
                        /* Left on an already-collapsed or leaf row →
                         * jump to the parent row instead. */
                        if (!gtk_tree_view_row_expanded(ctx->view, path) ||
                            !gtk_tree_model_iter_has_child(model, &sort_iter)) {
                            if (gtk_tree_path_up(path) &&
                                gtk_tree_path_get_depth(path) > 0) {
                                gtk_tree_view_set_cursor(ctx->view, path,
                                                         NULL, FALSE);
                            }
                        } else {
                            gtk_tree_view_collapse_row(ctx->view, path);
                        }
                    }
                    gtk_tree_path_free(path);
                }
            }
            return TRUE;
        }
    }

    /* Ctrl+F or Meta+F → toggle name filter (honour both modifiers) */
    if (ev->keyval == GDK_KEY_f &&
        (state == GDK_CONTROL_MASK || state == GDK_META_MASK)) {
        if (ctx->filter_entry) {
            if (gtk_widget_get_visible(ctx->filter_entry)) {
                filter_cancel_hide_timer(ctx);
                gtk_entry_set_text(GTK_ENTRY(ctx->filter_entry), "");
                ctx->filter_text[0] = '\0';
                switch_to_real_store(ctx);
                gtk_widget_hide(ctx->filter_entry);
                gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
            } else {
                filter_cancel_hide_timer(ctx);
                gtk_widget_show(ctx->filter_entry);
                gtk_widget_grab_focus(ctx->filter_entry);
            }
        }
        return TRUE;
    }

    /* Ctrl+G or Meta+G → toggle Go-to-PID entry */
    if (ev->keyval == GDK_KEY_g &&
        (state == GDK_CONTROL_MASK || state == GDK_META_MASK)) {
        if (ctx->pid_entry) {
            if (gtk_widget_get_visible(ctx->pid_entry)) {
                gtk_entry_set_text(GTK_ENTRY(ctx->pid_entry), "");
                gtk_widget_hide(ctx->pid_entry);
                gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
            } else {
                gtk_widget_show(ctx->pid_entry);
                gtk_widget_grab_focus(ctx->pid_entry);
            }
        }
        return TRUE;
    }

    if (state != (guint)mod)
        return FALSE;       /* modifier not held – not our shortcut */

    switch (ev->keyval) {
    case GDK_KEY_plus:       /* Ctrl + Shift + = (i.e. "+") */
    case GDK_KEY_equal:      /* Ctrl + =  (no shift needed) */
    case GDK_KEY_KP_Add:     /* Ctrl + numpad "+"           */
        if (ctx->font_size < FONT_SIZE_MAX) {
            ctx->font_size++;
            ctx->auto_font = FALSE;
            reload_font_css(ctx);
        }
        return TRUE;

    case GDK_KEY_minus:      /* Ctrl + -                    */
    case GDK_KEY_KP_Subtract:/* Ctrl + numpad "-"           */
        if (ctx->font_size > FONT_SIZE_MIN) {
            ctx->font_size--;
            ctx->auto_font = FALSE;
            reload_font_css(ctx);
        }
        return TRUE;

    case GDK_KEY_0:          /* Ctrl + 0  → reset font size */
    case GDK_KEY_KP_0:
        ctx->font_size = FONT_SIZE_DEFAULT;
        ctx->auto_font = FALSE;
        reload_font_css(ctx);
        return TRUE;

    case GDK_KEY_q:          /* Ctrl + Q  → quit            */
        gtk_main_quit();
        return TRUE;

    default:
        break;
    }

    return FALSE;
}

/* ── Alt tap: show menu bar & open File menu ─────────────────── */

static gboolean on_key_release(GtkWidget *widget, GdkEventKey *ev,
                               gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    if ((ev->keyval == GDK_KEY_Alt_L || ev->keyval == GDK_KEY_Alt_R) &&
        ctx->alt_pressed) {
        ctx->alt_pressed = FALSE;

        /* If the menu bar is hidden, show it first */
        if (!gtk_widget_get_visible(ctx->menubar))
            gtk_widget_show_all(ctx->menubar);

        /* Activate the File menu item (opens the dropdown) */
        gtk_menu_shell_select_item(GTK_MENU_SHELL(ctx->menubar),
                                   ctx->file_menu_item);
        return TRUE;
    }
    ctx->alt_pressed = FALSE;
    return FALSE;
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
    ui_ctx_t *ctx = data;
    monitor_state_t *mon = ctx->mon;

    ctx->shutting_down = TRUE;

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
 *
 * Pinned top-level rows (COL_PINNED_ROOT != PTREE_UNPINNED) always
 * sort above normal top-level rows.  Among pinned rows the normal
 * column sort order applies.
 */

/* Helper: compare two top-level iters by pinned status.
 *
 * Pinned rows must always appear visually above non-pinned rows,
 * regardless of the current sort column or direction.
 *
 * Because our sort functions invert the comparison (so GTK's
 * "ascending" displays largest first) AND GTK itself negates
 * the result when the direction is descending, the sign we
 * need to return flips depending on the current sort order.
 *
 * GTK "ascending" + our inversion  → return  1 means a BEFORE b
 * GTK "descending" + our inversion → return -1 means a BEFORE b
 *
 * Returns 0 when both rows have the same pinned status or when
 * the rows are not both top-level (children sort normally).
 */
static inline int pinned_cmp(GtkTreeModel *model,
                             GtkTreeIter  *a,
                             GtkTreeIter  *b)
{
    /* Check depth: only top-level rows participate in pinned ordering.
     * A quick way: see if the iter has a parent. */
    GtkTreeIter parent_a, parent_b;
    gboolean a_top = !gtk_tree_model_iter_parent(model, &parent_a, a);
    gboolean b_top = !gtk_tree_model_iter_parent(model, &parent_b, b);

    if (!a_top || !b_top)
        return 0;  /* children within a subtree: normal sort */

    gint pa = 0, pb = 0;
    gtk_tree_model_get(model, a, COL_PINNED_ROOT, &pa, -1);
    gtk_tree_model_get(model, b, COL_PINNED_ROOT, &pb, -1);

    gboolean a_pinned = (pa != (gint)PTREE_UNPINNED);
    gboolean b_pinned = (pb != (gint)PTREE_UNPINNED);

    if (a_pinned == b_pinned)
        return 0;  /* both pinned or both unpinned → normal sort */

    /* Determine effective sign: we need "pinned row first" in both
     * ascending and descending modes.
     *
     * Our sort funcs already invert (ascending → largest first).
     * GTK applies an extra negation when direction == descending.
     *
     *   ascending:  return  1 → GTK displays a before b  (inverted)
     *   descending: return -1 → GTK negates to 1 → a before b
     *
     * So: ascending needs +1 for "a first", descending needs -1.      */
    GtkSortType order = GTK_SORT_ASCENDING;
    gint sort_col = GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID;
    gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(model),
                                         &sort_col, &order);

    int sign = (order == GTK_SORT_ASCENDING) ? 1 : -1;

    return a_pinned ? sign : -sign;
}

static gint sort_int_inverted(GtkTreeModel *model,
                              GtkTreeIter  *a,
                              GtkTreeIter  *b,
                              gpointer      col_id_ptr)
{
    int pc = pinned_cmp(model, a, b);
    if (pc) return pc;

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
    int pc = pinned_cmp(model, a, b);
    if (pc) return pc;

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

static gint sort_int64_inverted(GtkTreeModel *model,
                               GtkTreeIter  *a,
                               GtkTreeIter  *b,
                               gpointer      col_id_ptr)
{
    int pc = pinned_cmp(model, a, b);
    if (pc) return pc;

    gint col = GPOINTER_TO_INT(col_id_ptr);
    gint64 va = 0, vb = 0;
    gtk_tree_model_get(model, a, col, &va, -1);
    gtk_tree_model_get(model, b, col, &vb, -1);
    return (va < vb) ? 1 : (va > vb) ? -1 : 0;
}

/*
 * Register all inverted sort functions on a GtkTreeModelSort.
 * Called every time we create a new sort model (when switching
 * between the real store and the filter shadow store).
 */
static void register_sort_funcs(GtkTreeModelSort *sm)
{
    GtkTreeSortable *sortable = GTK_TREE_SORTABLE(sm);

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
    gtk_tree_sortable_set_sort_func(sortable, COL_IO_READ_RATE,
        sort_int_inverted, GINT_TO_POINTER(COL_IO_READ_RATE), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_IO_WRITE_RATE,
        sort_int_inverted, GINT_TO_POINTER(COL_IO_WRITE_RATE), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_START_TIME,
        sort_int64_inverted, GINT_TO_POINTER(COL_START_TIME), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_CONTAINER,
        sort_string_inverted, GINT_TO_POINTER(COL_CONTAINER), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_SERVICE,
        sort_string_inverted, GINT_TO_POINTER(COL_SERVICE), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_STEAM_LABEL,
        sort_string_inverted, GINT_TO_POINTER(COL_STEAM_LABEL), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_CWD,
        sort_string_inverted, GINT_TO_POINTER(COL_CWD), NULL);
}

/* ── public entry point ──────────────────────────────────────── */

/*
 * Cell data function for row highlighting: computes a fading
 * background colour for newly spawned (green) or dying (red)
 * processes.  Applied to every column renderer so the entire row
 * is coloured.
 *
 * The sort model exposes the underlying store's data, so we read
 * COL_HIGHLIGHT_BORN / COL_HIGHLIGHT_DIED directly from `model`.
 */
static void highlight_cell_data_func(GtkTreeViewColumn *col,
                                     GtkCellRenderer   *cell,
                                     GtkTreeModel      *model,
                                     GtkTreeIter       *iter,
                                     gpointer           data)
{
    (void)col; (void)data;

    gint64 born = 0, died = 0;
    gtk_tree_model_get(model, iter,
                       COL_HIGHLIGHT_BORN, &born,
                       COL_HIGHLIGHT_DIED, &died, -1);

    /* Get current monotonic time */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    gint64 now = (gint64)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;

    GdkRGBA rgba = { 0, 0, 0, 0 };   /* fully transparent = no background */
    gboolean set_bg = FALSE;

    if (died > 0) {
        /* Dying process: red fade-out */
        gint64 age = now - died;
        if (age < HIGHLIGHT_FADE_US) {
            double alpha = 0.35 * (1.0 - (double)age / HIGHLIGHT_FADE_US);
            rgba = (GdkRGBA){ 0.95, 0.25, 0.20, alpha };
            set_bg = TRUE;
        }
    } else if (born > 0) {
        /* New process: green fade-out */
        gint64 age = now - born;
        if (age < HIGHLIGHT_FADE_US) {
            double alpha = 0.30 * (1.0 - (double)age / HIGHLIGHT_FADE_US);
            rgba = (GdkRGBA){ 0.20, 0.80, 0.30, alpha };
            set_bg = TRUE;
        }
    }

    if (set_bg)
        g_object_set(cell, "cell-background-rgba", &rgba, NULL);
    else
        g_object_set(cell, "cell-background-set", FALSE, NULL);
}

/*
 * Cell data function for the Name column: prepend "➡ " when the
 * process is pinned.  The process is considered "pinned" when its
 * own PID appears in the pinned set (regardless of whether the
 * current row is the pinned copy or the original tree entry).
 */
static void name_cell_data_func(GtkTreeViewColumn *col,
                                GtkCellRenderer   *cell,
                                GtkTreeModel      *model,
                                GtkTreeIter       *iter,
                                gpointer           data)
{
    ui_ctx_t *ctx = data;

    /* Apply highlight background (same logic as highlight_cell_data_func) */
    highlight_cell_data_func(col, cell, model, iter, NULL);

    gchar *name = NULL;
    gchar *steam_label = NULL;
    gint pid = 0;
    gtk_tree_model_get(model, iter,
                       COL_NAME, &name,
                       COL_PID, &pid,
                       COL_STEAM_LABEL, &steam_label,
                       -1);

    /* Prefer Steam display label (e.g. "reaper (Steam) · Deadlock [Proton ...]") */
    const char *display = name;
    if (steam_label && steam_label[0])
        display = steam_label;

    if (display && pid_is_pinned(ctx, (pid_t)pid)) {
        char buf[512];
        snprintf(buf, sizeof(buf), "➡ %s", display);
        g_object_set(cell, "text", buf, NULL);
    } else {
        g_object_set(cell, "text", display ? display : "", NULL);
    }
    g_free(name);
    g_free(steam_label);
}

/* ── Host service forwarding functions for PipeWire plugins ──── */
#ifdef HAVE_PIPEWIRE
/*
 * These bridge from the plugin API to the sidebar's pw_meter.c
 * and spectrogram.c implementations.  The host_ctx is a ui_ctx_t*.
 */
static void host_pw_meter_start(void *host_ctx,
                                const uint32_t *node_ids, size_t count)
{
    ui_ctx_t *c = host_ctx;
    pw_meter_start(c, node_ids, count);
}

static void host_pw_meter_stop(void *host_ctx)
{
    ui_ctx_t *c = host_ctx;
    pw_meter_stop(c);
}

static void host_pw_meter_read(void *host_ctx, uint32_t node_id,
                               int *level_l, int *level_r)
{
    ui_ctx_t *c = host_ctx;
    pw_meter_read(c, node_id, level_l, level_r);
}

static void host_spectro_start(void *host_ctx, GtkDrawingArea *draw_area,
                               uint32_t node_id)
{
    ui_ctx_t *c = host_ctx;
    /* Point the host's spectrogram draw widget at the plugin's area */
    c->spectro_draw = GTK_WIDGET(draw_area);
    spectrogram_start_for_node(c, node_id);
}

static void host_spectro_stop(void *host_ctx)
{
    ui_ctx_t *c = host_ctx;
    spectrogram_stop(c);
}

static uint32_t host_spectro_get_target(void *host_ctx)
{
    ui_ctx_t *c = host_ctx;
    return spectrogram_get_target_node(c);
}
#endif /* HAVE_PIPEWIRE */

void *ui_thread(void *arg)
{
    monitor_state_t *mon = (monitor_state_t *)arg;

    /* ── window ──────────────────────────────────────────────── */
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Everything Monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);

    /* Set the window icon from the embedded GResource PNG.
     *
     * The source image may be large and non-square, so we produce
     * square, RGBA-with-alpha versions at the standard icon sizes
     * that window managers request (16, 32, 48, 64, 128).
     * This avoids blurry stretching and preserves transparency.      */
    {
        GdkPixbuf *raw = gdk_pixbuf_new_from_resource(
            "/org/evemon/icon.png", NULL);
        if (raw) {
            static const int sizes[] = { 16, 32, 48, 64, 128 };
            GList *icon_list = NULL;

            for (int i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
                int sz = sizes[i];

                /* Scale preserving aspect ratio into a sz×sz box */
                int src_w = gdk_pixbuf_get_width(raw);
                int src_h = gdk_pixbuf_get_height(raw);
                double scale = (double)sz / (src_w > src_h ? src_w : src_h);
                int dst_w = (int)(src_w * scale + 0.5);
                int dst_h = (int)(src_h * scale + 0.5);
                if (dst_w < 1) dst_w = 1;
                if (dst_h < 1) dst_h = 1;

                GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
                    raw, dst_w, dst_h, GDK_INTERP_BILINEAR);
                if (!scaled) continue;

                /* Centre the scaled image on a transparent sz×sz canvas */
                GdkPixbuf *canvas = gdk_pixbuf_new(
                    GDK_COLORSPACE_RGB, TRUE, 8, sz, sz);
                gdk_pixbuf_fill(canvas, 0x00000000);   /* fully transparent */

                int off_x = (sz - dst_w) / 2;
                int off_y = (sz - dst_h) / 2;
                gdk_pixbuf_composite(scaled, canvas,
                                     off_x, off_y, dst_w, dst_h,
                                     off_x, off_y, 1.0, 1.0,
                                     GDK_INTERP_BILINEAR, 255);
                g_object_unref(scaled);

                icon_list = g_list_append(icon_list, canvas);
            }

            if (icon_list)
                gtk_window_set_icon_list(GTK_WINDOW(window), icon_list);

            g_list_free_full(icon_list, g_object_unref);
            g_object_unref(raw);
        }
    }

    /* NOTE: the "destroy" signal is connected later, after ctx is
     * initialised, so the callback can set ctx.shutting_down. */

    /* Reduce the heavy backdrop dimming that GTK themes apply when a
     * modal dialog (e.g. the About splash) steals focus.  We override
     * the :backdrop pseudo-class at the screen level so the main
     * window stays nearly fully opaque. */
    {
        GtkCssProvider *bd_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(bd_css,
            "window:backdrop { opacity: 0.92; }", -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(bd_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(bd_css);
    }

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
                                             G_TYPE_INT,      /* IO read rate */
                                             G_TYPE_STRING,   /* IO read text */
                                             G_TYPE_INT,      /* IO write rate*/
                                             G_TYPE_STRING,   /* IO write text*/
                                             G_TYPE_INT64,    /* start time   */
                                             G_TYPE_STRING,   /* start time txt*/
                                             G_TYPE_STRING,   /* container    */
                                             G_TYPE_STRING,   /* service      */
                                             G_TYPE_STRING,   /* CWD          */
                                             G_TYPE_STRING,   /* CMDLINE      */
                                             G_TYPE_STRING,   /* STEAM_LABEL  */
                                             G_TYPE_STRING,   /* IO_SPARKLINE */
                                             G_TYPE_INT,      /* IO_SPARKLINE_PEAK */
                                             G_TYPE_INT64,    /* HIGHLIGHT_BORN */
                                             G_TYPE_INT64,    /* HIGHLIGHT_DIED */
                                             G_TYPE_INT);     /* PINNED_ROOT  */

    /* Sort model wraps the store directly (no filter model – we use
     * a shadow store approach for filtering instead). */
    GtkTreeModel *sort_model = gtk_tree_model_sort_new_with_model(
        GTK_TREE_MODEL(store));

    GtkWidget *tree = gtk_tree_view_new_with_model(sort_model);
    /* Don't unref sort_model – kept in ctx */

    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(tree), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree), FALSE);

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
    GtkTreeViewColumn *pid_col = col;   /* save for Go-to-PID positioning */

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
    GtkTreeViewColumn *name_col = col;  /* save for filter positioning */

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
    g_object_set(r, "xalign", 1.0f, NULL);
    col = gtk_tree_view_column_new_with_attributes("Disk Read", r,
                                                   "text", COL_IO_READ_RATE_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_IO_READ_RATE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0f, NULL);
    col = gtk_tree_view_column_new_with_attributes("Disk Write", r,
                                                   "text", COL_IO_WRITE_RATE_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_IO_WRITE_RATE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    /* I/O Sparkline — custom cell renderer with animated bar chart */
    {
        GtkCellRenderer *spark_r = sparkline_cell_renderer_new();
        col = gtk_tree_view_column_new_with_attributes("I/O History", spark_r,
                                                       "spark-data", COL_IO_SPARKLINE,
                                                       "spark-peak", COL_IO_SPARKLINE_PEAK,
                                                       NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_min_width(col, 86);
        gtk_tree_view_column_set_fixed_width(col, 86);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
    }

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Start Time", r,
                                                   "text", COL_START_TIME_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_START_TIME);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 140);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Container", r,
                                                   "text", COL_CONTAINER, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_CONTAINER);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Service", r,
                                                   "text", COL_SERVICE, NULL);
    gtk_tree_view_column_set_sort_column_id(col, COL_SERVICE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, 120);
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

    /* Wire the highlight cell-data function to every column so that
     * newly spawned (green) and dying (red) rows get a full-row
     * background colour that fades over HIGHLIGHT_FADE_US.          */
    {
        GList *cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(tree));
        for (GList *c = cols; c; c = c->next) {
            GtkTreeViewColumn *tv_col = c->data;
            GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(tv_col));
            for (GList *r2 = renderers; r2; r2 = r2->next) {
                gtk_tree_view_column_set_cell_data_func(
                    tv_col, r2->data, highlight_cell_data_func, NULL, NULL);
            }
            g_list_free(renderers);
        }
        g_list_free(cols);
    }

    /* Register inverted sort functions so ▲ = largest/highest first. */
    register_sort_funcs(GTK_TREE_MODEL_SORT(sort_model));

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

    /* ── name-filter entry (overlaid on the column header) ────── */
    GtkWidget *name_filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(name_filter_entry), "filter…");
    gtk_entry_set_width_chars(GTK_ENTRY(name_filter_entry), 16);
    gtk_widget_set_no_show_all(name_filter_entry, TRUE);
    gtk_widget_set_valign(name_filter_entry, GTK_ALIGN_START);
    gtk_widget_set_halign(name_filter_entry, GTK_ALIGN_START);

    /* Compact CSS: small font, minimal padding so it fits the header row */
    {
        GtkCssProvider *filt_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(filt_css,
            "entry {"
            "  font-size: 8pt;"
            "  min-height: 0;"
            "  padding: 1px 4px;"
            "  border-radius: 3px;"
            "}", -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(name_filter_entry),
            GTK_STYLE_PROVIDER(filt_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(filt_css);
    }

    GtkWidget *tree_overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(tree_overlay), scroll);
    gtk_overlay_add_overlay(GTK_OVERLAY(tree_overlay), name_filter_entry);

    /* ── Go-to-PID entry (overlaid on the PID column header) ──── */
    GtkWidget *pid_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(pid_entry), "PID…");
    gtk_entry_set_width_chars(GTK_ENTRY(pid_entry), 8);
    gtk_entry_set_input_purpose(GTK_ENTRY(pid_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_no_show_all(pid_entry, TRUE);
    gtk_widget_set_valign(pid_entry, GTK_ALIGN_START);
    gtk_widget_set_halign(pid_entry, GTK_ALIGN_START);

    {
        GtkCssProvider *pid_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(pid_css,
            "entry {"
            "  font-size: 8pt;"
            "  min-height: 0;"
            "  padding: 1px 4px;"
            "  border-radius: 3px;"
            "}", -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(pid_entry),
            GTK_STYLE_PROVIDER(pid_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(pid_css);
    }

    gtk_overlay_add_overlay(GTK_OVERLAY(tree_overlay), pid_entry);

    /* ── sidebar (detail panel) ───────────────────────────────── */

    /*
     * Helper: create a collapsible sidebar section.
     *
     * Returns a GtkBox (vertical) containing:
     *   [0] separator
     *   [1] event-box header (double-click to toggle)
     *   [2] content_widget (shown/hidden on collapse)
     *
     * *out_arrow receives the arrow label widget (▼/▶).
     * The event-box has "sb-section-header" as widget name for CSS.
     */
    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sidebar_scroll, 240, -1);

    GtkWidget *sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(sidebar_vbox, 8);
    gtk_widget_set_margin_end(sidebar_vbox, 8);
    gtk_widget_set_margin_top(sidebar_vbox, 8);
    gtk_widget_set_margin_bottom(sidebar_vbox, 8);

    /* ── Process Info section (non-scrollable grid) ───────────── */
    GtkWidget *info_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(info_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(info_grid), 8);

    /* Helper macro to add a label row to the info grid */
    #define SIDEBAR_ROW(row, key_str, label_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_END);     \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(info_grid), _k, 0, row, 1, 1);         \
        gtk_grid_attach(GTK_GRID(info_grid), _v, 1, row, 1, 1);         \
        label_var = GTK_LABEL(_v);                                       \
    } while (0)

    GtkLabel *sb_pid, *sb_ppid, *sb_user, *sb_name;
    GtkLabel *sb_cpu, *sb_rss, *sb_group_rss, *sb_group_cpu;
    GtkLabel *sb_io_read, *sb_io_write;
    GtkLabel *sb_net_send, *sb_net_recv;
    GtkLabel *sb_start_time, *sb_container, *sb_service, *sb_cwd, *sb_cmdline;

    SIDEBAR_ROW(0,  "PID",             sb_pid);
    SIDEBAR_ROW(1,  "PPID",            sb_ppid);
    SIDEBAR_ROW(2,  "User",            sb_user);
    SIDEBAR_ROW(3,  "Name",            sb_name);
    SIDEBAR_ROW(4,  "CPU%",            sb_cpu);
    SIDEBAR_ROW(5,  "Memory (RSS)",    sb_rss);
    SIDEBAR_ROW(6,  "Group Memory",    sb_group_rss);
    SIDEBAR_ROW(7,  "Group CPU%",      sb_group_cpu);
    SIDEBAR_ROW(8,  "Disk Read",       sb_io_read);
    SIDEBAR_ROW(9,  "Disk Write",      sb_io_write);
    SIDEBAR_ROW(10, "Net Send",        sb_net_send);
    SIDEBAR_ROW(11, "Net Recv",        sb_net_recv);
    SIDEBAR_ROW(12, "Start Time",      sb_start_time);
    SIDEBAR_ROW(13, "Container",       sb_container);
    SIDEBAR_ROW(14, "Service",         sb_service);
    SIDEBAR_ROW(15, "CWD",            sb_cwd);
    SIDEBAR_ROW(16, "Command",         sb_cmdline);
    #undef SIDEBAR_ROW

    /* ── Steam / Proton metadata section ──────────────────────── */
    GtkWidget *steam_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *steam_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(steam_box), steam_sep, FALSE, FALSE, 0);

    GtkWidget *steam_header = gtk_label_new("Steam / Proton");
    gtk_label_set_xalign(GTK_LABEL(steam_header), 0.0f);
    {
        PangoAttrList *a = pango_attr_list_new();
        pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(steam_header), a);
        pango_attr_list_unref(a);
    }
    gtk_box_pack_start(GTK_BOX(steam_box), steam_header, FALSE, FALSE, 0);

    GtkWidget *steam_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(steam_grid), 8);
    gtk_box_pack_start(GTK_BOX(steam_box), steam_grid, FALSE, FALSE, 0);

    #define SIDEBAR_ROW_S(row, key_str, label_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_END);     \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(steam_grid), _k, 0, row, 1, 1);        \
        gtk_grid_attach(GTK_GRID(steam_grid), _v, 1, row, 1, 1);        \
        label_var = GTK_LABEL(_v);                                       \
    } while (0)

    GtkLabel *sb_steam_game, *sb_steam_appid, *sb_steam_proton;
    GtkLabel *sb_steam_runtime, *sb_steam_compat, *sb_steam_gamedir;

    SIDEBAR_ROW_S(0, "Game",            sb_steam_game);
    SIDEBAR_ROW_S(1, "App ID",          sb_steam_appid);
    SIDEBAR_ROW_S(2, "Proton",          sb_steam_proton);
    SIDEBAR_ROW_S(3, "Runtime",         sb_steam_runtime);
    SIDEBAR_ROW_S(4, "Compat Data",     sb_steam_compat);
    SIDEBAR_ROW_S(5, "Game Directory",  sb_steam_gamedir);
    #undef SIDEBAR_ROW_S

    gtk_widget_set_no_show_all(steam_box, TRUE);

    /* ── cgroup resource limits section (conditionally visible) ── */
    GtkWidget *cgroup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *cgroup_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(cgroup_box), cgroup_sep, FALSE, FALSE, 0);

    GtkWidget *cgroup_header = gtk_label_new("cgroup Limits");
    gtk_label_set_xalign(GTK_LABEL(cgroup_header), 0.0f);
    {
        PangoAttrList *a = pango_attr_list_new();
        pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(cgroup_header), a);
        pango_attr_list_unref(a);
    }
    gtk_box_pack_start(GTK_BOX(cgroup_box), cgroup_header, FALSE, FALSE, 0);

    GtkWidget *cgroup_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(cgroup_grid), 8);
    gtk_box_pack_start(GTK_BOX(cgroup_box), cgroup_grid, FALSE, FALSE, 0);

    /* Cgroup row helper macro (same pattern as SIDEBAR_ROW_S) */
    #define CGROUP_ROW(row, key_str, label_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_MIDDLE);  \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(cgroup_grid), _k, 0, row, 1, 1);       \
        gtk_grid_attach(GTK_GRID(cgroup_grid), _v, 1, row, 1, 1);       \
        label_var = GTK_LABEL(_v);                                       \
    } while (0)

    /* Variant that also stores the key widget for hide/show */
    #define CGROUP_ROW_K(row, key_str, label_var, key_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_MIDDLE);  \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(cgroup_grid), _k, 0, row, 1, 1);       \
        gtk_grid_attach(GTK_GRID(cgroup_grid), _v, 1, row, 1, 1);       \
        label_var = GTK_LABEL(_v);                                       \
        key_var = _k;                                                    \
    } while (0)

    GtkLabel *sb_cgroup_path, *sb_cgroup_mem;
    GtkLabel *sb_cgroup_mem_high, *sb_cgroup_swap;
    GtkLabel *sb_cgroup_cpu, *sb_cgroup_pids, *sb_cgroup_io;
    GtkWidget *sb_cgroup_mem_high_key, *sb_cgroup_swap_key, *sb_cgroup_io_key;

    CGROUP_ROW(0, "Path",     sb_cgroup_path);
    CGROUP_ROW(1, "Memory",   sb_cgroup_mem);
    CGROUP_ROW_K(2, "Mem High", sb_cgroup_mem_high, sb_cgroup_mem_high_key);
    CGROUP_ROW_K(3, "Swap",    sb_cgroup_swap, sb_cgroup_swap_key);
    CGROUP_ROW(4, "CPU",      sb_cgroup_cpu);
    CGROUP_ROW(5, "PIDs",     sb_cgroup_pids);
    CGROUP_ROW_K(6, "I/O",     sb_cgroup_io, sb_cgroup_io_key);
    #undef CGROUP_ROW
    #undef CGROUP_ROW_K

    gtk_widget_set_no_show_all(cgroup_box, TRUE);

    /*
     * ── Collapsible section header helper ───────────────────────
     *
     * Creates a horizontal box with an arrow label (▼/▶) and a bold
     * title, wrapped in a GtkEventBox that responds to double-click.
     * Returns the event box.  *out_arrow receives the arrow label.
     */
    /*
     * MAKE_SECTION: Build a complete collapsible section.
     *
     * section_var  – GtkWidget* to receive the outer section GtkBox
     * header_var   – GtkWidget* to receive the header event box
     * revealer_var – GtkWidget* to receive the GtkRevealer
     * arrow_var    – GtkWidget* to receive the arrow label
     * title        – section title string
     * content_w    – GtkWidget* of the content to place inside the revealer
     * expandable   – TRUE if this section should vexpand when open
     */
    #define MAKE_SECTION(section_var, header_var, revealer_var,         \
                         arrow_var, title, content_w, expandable)       \
    do {                                                                \
        /* Header: arrow + bold label in an event box */                \
        GtkWidget *_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);   \
        gtk_widget_set_margin_top(_hb, 2);                             \
        gtk_widget_set_margin_bottom(_hb, 2);                          \
        GtkWidget *_ar = gtk_label_new(SECTION_ARROW_EXPANDED);        \
        gtk_label_set_xalign(GTK_LABEL(_ar), 0.0f);                   \
        gtk_box_pack_start(GTK_BOX(_hb), _ar, FALSE, FALSE, 0);       \
        GtkWidget *_lbl = gtk_label_new(title);                        \
        gtk_label_set_xalign(GTK_LABEL(_lbl), 0.0f);                  \
        {                                                              \
            PangoAttrList *_a = pango_attr_list_new();                 \
            pango_attr_list_insert(_a,                                 \
                pango_attr_weight_new(PANGO_WEIGHT_BOLD));             \
            gtk_label_set_attributes(GTK_LABEL(_lbl), _a);            \
            pango_attr_list_unref(_a);                                 \
        }                                                              \
        gtk_box_pack_start(GTK_BOX(_hb), _lbl, TRUE, TRUE, 0);        \
        GtkWidget *_eb = gtk_event_box_new();                          \
        gtk_container_add(GTK_CONTAINER(_eb), _hb);                    \
        gtk_widget_add_events(_eb, GDK_BUTTON_PRESS_MASK              \
                                 | GDK_BUTTON_RELEASE_MASK            \
                                 | GDK_POINTER_MOTION_MASK);          \
        (arrow_var)  = _ar;                                            \
        (header_var) = _eb;                                            \
                                                                       \
        /* Revealer: smooth animated show/hide of content */           \
        GtkWidget *_rv = gtk_revealer_new();                           \
        gtk_revealer_set_transition_type(GTK_REVEALER(_rv),            \
            GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);                  \
        gtk_revealer_set_transition_duration(GTK_REVEALER(_rv),        \
            SECTION_TRANSITION_MS);                                    \
        gtk_revealer_set_reveal_child(GTK_REVEALER(_rv), TRUE);       \
        gtk_container_add(GTK_CONTAINER(_rv), (content_w));            \
        (revealer_var) = _rv;                                          \
                                                                       \
        /* Section box: separator + header + revealer */               \
        GtkWidget *_sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);    \
        gtk_box_pack_start(GTK_BOX(_sec),                              \
            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),             \
            FALSE, FALSE, 0);                                          \
        gtk_box_pack_start(GTK_BOX(_sec), _eb, FALSE, FALSE, 0);      \
        gtk_box_pack_start(GTK_BOX(_sec), _rv,                         \
            (expandable), (expandable), 0);                            \
        (section_var) = _sec;                                          \
    } while (0)

    /* ── Process Info collapsible section ─────────────────────── */
    /* Info section is special: always expanded, content = info_grid + steam_box
     * wrapped together in a vertical box.  We still use a revealer for
     * visual consistency when collapsing. */
    GtkWidget *info_content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(info_content_box), info_grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_content_box), steam_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_content_box), cgroup_box, FALSE, FALSE, 0);

    GtkWidget *info_section, *info_header_eb, *info_revealer, *info_arrow;
    MAKE_SECTION(info_section, info_header_eb, info_revealer,
                 info_arrow, "Process Info", info_content_box, FALSE);
    (void)info_revealer;
    gtk_widget_set_name(info_section, "info-section");
    /* Info always participates in equal vertical space sharing */
    gtk_widget_set_vexpand(info_section, TRUE);
    /* Info section is never collapsible – hide the arrow and header */
    gtk_widget_hide(info_arrow);
    gtk_widget_set_no_show_all(info_arrow, TRUE);
    /* Hide the "Process Info" header label and its separator/divider */
    gtk_widget_hide(info_header_eb);
    gtk_widget_set_no_show_all(info_header_eb, TRUE);
    {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(info_section));
        if (kids) {
            /* First child is the separator added by MAKE_SECTION */
            gtk_widget_hide(GTK_WIDGET(kids->data));
            gtk_widget_set_no_show_all(GTK_WIDGET(kids->data), TRUE);
            g_list_free(kids);
        }
    }

    #undef MAKE_SECTION

    /* ── Pack sidebar content ───────────────────────────────────── */
    gtk_box_pack_start(GTK_BOX(sidebar_vbox), info_section, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(sidebar_scroll), sidebar_vbox);

    GtkWidget *sidebar_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(sidebar_frame), sidebar_scroll);

    /* Live CSS provider for sidebar font size (cascades to all children) */
    GtkCssProvider *sidebar_css = gtk_css_provider_new();
    {
        char sbuf[128];
        snprintf(sbuf, sizeof(sbuf),
                 "frame, label, checkbutton { font-size: %dpt; }",
                 FONT_SIZE_DEFAULT);
        gtk_css_provider_load_from_data(sidebar_css, sbuf, -1, NULL);
    }
    gtk_style_context_add_provider(gtk_widget_get_style_context(sidebar_frame),
        GTK_STYLE_PROVIDER(sidebar_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* ── layout: tree is the main content ───────────────────── */
    GtkWidget *hpaned = tree_overlay;

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

    /* Detail Panel toggle */
    GtkWidget *detail_panel_toggle = gtk_check_menu_item_new_with_label("Detail Panel");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(detail_panel_toggle), FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), detail_panel_toggle);

    /* Detail Panel → Position submenu (radio items) */
    GtkWidget *panel_pos_menu = gtk_menu_new();
    GtkWidget *panel_pos_item = gtk_menu_item_new_with_label("Panel Position");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(panel_pos_item), panel_pos_menu);

    GSList *pos_group = NULL;
    static panel_pos_data_t pos_data_bottom, pos_data_top, pos_data_left, pos_data_right;

    GtkWidget *pos_bottom = gtk_radio_menu_item_new_with_label(pos_group, "Bottom");
    pos_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(pos_bottom));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(pos_bottom), TRUE);
    gtk_menu_shell_append(GTK_MENU_SHELL(panel_pos_menu), pos_bottom);

    GtkWidget *pos_top = gtk_radio_menu_item_new_with_label(pos_group, "Top");
    pos_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(pos_top));
    gtk_menu_shell_append(GTK_MENU_SHELL(panel_pos_menu), pos_top);

    GtkWidget *pos_left = gtk_radio_menu_item_new_with_label(pos_group, "Left");
    pos_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(pos_left));
    gtk_menu_shell_append(GTK_MENU_SHELL(panel_pos_menu), pos_left);

    GtkWidget *pos_right = gtk_radio_menu_item_new_with_label(pos_group, "Right");
    pos_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(pos_right));
    gtk_menu_shell_append(GTK_MENU_SHELL(panel_pos_menu), pos_right);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), panel_pos_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          gtk_separator_menu_item_new());

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

    /* Theme picker submenu */
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu),
                          gtk_separator_menu_item_new());
    GtkWidget *theme_item = gtk_menu_item_new_with_label("Theme");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(theme_item),
                              build_theme_submenu());
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu), theme_item);

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

    /* ── detail panel (wraps sidebar + plugin notebook) ──────── */
    /* The detail panel now contains a horizontal paned:
     *   pack1 = sidebar_frame  (Process Info / Steam / cgroup)
     *   pack2 = plugin notebook (tabs for FD, Env, Mmap, Libs, Net, …)
     * Opening the detail panel reveals both process info and plugin tabs. */
    GtkWidget *detail_panel = gtk_frame_new(NULL);
    gtk_widget_set_size_request(detail_panel, -1, 200);

    /* Inner horizontal paned: sidebar | notebook */
    GtkWidget *detail_hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_size_request(sidebar_frame, 280, -1);
    gtk_paned_pack1(GTK_PANED(detail_hpaned), sidebar_frame, FALSE, FALSE);
    /* The notebook will be added to pack2 after plugin loading below. */
    gtk_container_add(GTK_CONTAINER(detail_panel), detail_hpaned);

    /* ── layout ──────────────────────────────────────────────── */
    /* Default: detail panel docked at bottom.
     * outer_paned (vertical): pack1 = hpaned, pack2 = detail_panel */
    GtkWidget *outer_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(outer_paned), hpaned, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(outer_paned), detail_panel, FALSE, FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), outer_paned, TRUE, TRUE, 0);
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
    ctx.menubar        = menubar;
    ctx.file_menu_item = file_item;
    ctx.tree           = tree;
    ctx.alt_pressed    = FALSE;
    ctx.css          = css;
    ctx.sidebar_css  = sidebar_css;
    ctx.font_size    = FONT_SIZE_DEFAULT;
    ctx.auto_font    = FALSE;
    ctx.ptree_nodes  = (ptree_node_set_t){ NULL, NULL, NULL, 0, 0 };
    ctx.follow_selection = FALSE;

    /* Name filter */
    ctx.filter_store = NULL;
    ctx.sort_model   = GTK_TREE_MODEL_SORT(sort_model);
    ctx.filter_entry = name_filter_entry;
    ctx.name_col     = name_col;
    ctx.filter_text[0] = '\0';

    /* Go-to-PID */
    ctx.pid_entry = pid_entry;
    ctx.pid_col   = pid_col;

    /* Sidebar detail panel */
    ctx.sidebar            = sidebar_frame;
    ctx.sidebar_grid       = sidebar_vbox;

    /* Collapsible section state */
    ctx.sb_info_collapsed     = FALSE;
    ctx.sb_info_content       = info_content_box;
    ctx.sb_info_header_arrow  = info_arrow;

    ctx.sb_pid        = sb_pid;
    ctx.sb_ppid       = sb_ppid;
    ctx.sb_user       = sb_user;
    ctx.sb_name       = sb_name;
    ctx.sb_cpu        = sb_cpu;
    ctx.sb_rss        = sb_rss;
    ctx.sb_group_rss  = sb_group_rss;
    ctx.sb_group_cpu  = sb_group_cpu;
    ctx.sb_io_read    = sb_io_read;
    ctx.sb_io_write   = sb_io_write;
    ctx.sb_net_send   = sb_net_send;
    ctx.sb_net_recv   = sb_net_recv;
    ctx.sb_start_time = sb_start_time;
    ctx.sb_container  = sb_container;
    ctx.sb_service    = sb_service;
    ctx.sb_cwd        = sb_cwd;
    ctx.sb_cmdline    = sb_cmdline;

    /* Steam/Proton sidebar */
    ctx.sb_steam_game    = sb_steam_game;
    ctx.sb_steam_appid   = sb_steam_appid;
    ctx.sb_steam_proton  = sb_steam_proton;
    ctx.sb_steam_runtime = sb_steam_runtime;
    ctx.sb_steam_compat  = sb_steam_compat;
    ctx.sb_steam_gamedir = sb_steam_gamedir;
    ctx.sb_steam_frame   = steam_box;     /* show/hide entire Steam section */

    /* cgroup resource limits */
    ctx.sb_cgroup_path         = sb_cgroup_path;
    ctx.sb_cgroup_mem          = sb_cgroup_mem;
    ctx.sb_cgroup_mem_high     = sb_cgroup_mem_high;
    ctx.sb_cgroup_mem_high_key = sb_cgroup_mem_high_key;
    ctx.sb_cgroup_swap         = sb_cgroup_swap;
    ctx.sb_cgroup_swap_key     = sb_cgroup_swap_key;
    ctx.sb_cgroup_cpu          = sb_cgroup_cpu;
    ctx.sb_cgroup_pids         = sb_cgroup_pids;
    ctx.sb_cgroup_io           = sb_cgroup_io;
    ctx.sb_cgroup_io_key       = sb_cgroup_io_key;
    ctx.sb_cgroup_frame        = cgroup_box;

#ifdef HAVE_PIPEWIRE
    /* Meter and spectrogram state are still used by host services */
    ctx.pw_meter       = NULL;
    ctx.pw_meter_timer = 0;
    ctx.spectro_draw   = NULL;
    ctx.spectro        = NULL;
#endif

    /* Pinned processes */
    ctx.pinned_pids     = NULL;
    ctx.pinned_count    = 0;
    ctx.pinned_capacity = 0;

    /* ── Plugin system ───────────────────────────────────────── */
    {
        /* Allocate the registry on the heap so we have a stable pointer */
        plugin_registry_t *preg = calloc(1, sizeof(plugin_registry_t));
        if (preg) {
            plugin_registry_init(preg);

#ifdef HAVE_PIPEWIRE
            /* Build host services table on the heap so it outlives this block */
            evemon_host_services_t *hsvc = calloc(1, sizeof(evemon_host_services_t));
            if (hsvc) {
                hsvc->host_ctx          = &ctx;
                hsvc->pw_meter_start    = host_pw_meter_start;
                hsvc->pw_meter_stop     = host_pw_meter_stop;
                hsvc->pw_meter_read     = host_pw_meter_read;
                hsvc->spectro_start     = host_spectro_start;
                hsvc->spectro_stop      = host_spectro_stop;
                hsvc->spectro_get_target = host_spectro_get_target;
                plugin_registry_set_host_services(preg, hsvc);
            }
#endif

            /* Resolve plugins directory.  Search order:
             *  1. <exe_dir>/plugins/    — for development (build/plugins/)
             *  2. EVEMON_LIBDIR/plugins — installed system path
             *  3. build/plugins          — last-ditch fallback              */
            char exe_path[4096], plugin_dir[4096];
            plugin_dir[0] = '\0';
            ssize_t exe_len = readlink("/proc/self/exe", exe_path,
                                        sizeof(exe_path) - 1);
            if (exe_len > 0) {
                exe_path[exe_len] = '\0';
                char *slash = strrchr(exe_path, '/');
                if (slash) *slash = '\0';
                snprintf(plugin_dir, sizeof(plugin_dir),
                         "%s/plugins", exe_path);
            }

            /* If the exe-relative dir doesn't exist (installed layout),
             * fall back to the compiled-in LIBDIR.                        */
            if (plugin_dir[0] == '\0' ||
                access(plugin_dir, F_OK) != 0) {
#ifdef EVEMON_LIBDIR
                snprintf(plugin_dir, sizeof(plugin_dir),
                         "%s/plugins", EVEMON_LIBDIR);
#else
                snprintf(plugin_dir, sizeof(plugin_dir), "build/plugins");
#endif
            }

            int nloaded = plugin_loader_scan(preg, plugin_dir);
            fprintf(stdout, "evemon: %d plugin(s) loaded from %s\n",
                    nloaded, plugin_dir);

            /* Create a GtkNotebook for plugin tabs (detail panel) */
            GtkWidget *notebook = gtk_notebook_new();
            gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
            gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);

            /* Add each plugin instance as a tab.
             * Preferred order: PipeWire Audio first, then Network,
             * then everything else in load order. */
            {
                static const char *tab_order[] = {
                    "org.evemon.pipewire",
                    "org.evemon.net",
                    NULL
                };

                /* Tab label overrides keyed by plugin id */
                static const struct { const char *id; const char *label; }
                tab_label_overrides[] = {
                    { "org.evemon.net", "Network" },
                    { NULL, NULL }
                };

                gboolean *added = g_new0(gboolean, preg->count);

                /* Pass 1: add plugins in the preferred order */
                for (int o = 0; tab_order[o]; o++) {
                    for (size_t i = 0; i < preg->count; i++) {
                        plugin_instance_t *inst = &preg->instances[i];
                        if (!inst->widget || !inst->plugin ||
                            !inst->plugin->id)
                            continue;
                        if (strcmp(inst->plugin->id, tab_order[o]) != 0)
                            continue;
                        const char *lbl = inst->plugin->name
                                        ? inst->plugin->name : "Plugin";
                        for (int k = 0; tab_label_overrides[k].id; k++) {
                            if (strcmp(inst->plugin->id,
                                       tab_label_overrides[k].id) == 0) {
                                lbl = tab_label_overrides[k].label;
                                break;
                            }
                        }
                        GtkWidget *label = gtk_label_new(lbl);
                        gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                                                 inst->widget, label);
                        added[i] = TRUE;
                    }
                }

                /* Pass 2: remaining plugins in load order */
                for (size_t i = 0; i < preg->count; i++) {
                    if (added[i]) continue;
                    plugin_instance_t *inst = &preg->instances[i];
                    if (!inst->widget) continue;
                    const char *lbl = (inst->plugin && inst->plugin->name)
                                    ? inst->plugin->name : "Plugin";
                    if (inst->plugin && inst->plugin->id) {
                        for (int k = 0; tab_label_overrides[k].id; k++) {
                            if (strcmp(inst->plugin->id,
                                       tab_label_overrides[k].id) == 0) {
                                lbl = tab_label_overrides[k].label;
                                break;
                            }
                        }
                    }
                    GtkWidget *label = gtk_label_new(lbl);
                    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                                             inst->widget, label);
                }

                g_free(added);
            }

            /* The plugin notebook lives in the detail panel (separate
             * from the sidebar).  It is NOT packed into sidebar_vbox. */

            ctx.plugin_registry = preg;
            ctx.plugin_notebook = notebook;
        } else {
            ctx.plugin_registry = NULL;
            ctx.plugin_notebook = NULL;
        }
        ctx.plugin_broker = NULL;  /* broker is stateless (module-level) */
    }

    /* ── Finish detail panel setup: put notebook into the hpaned ── */
    ctx.detail_panel          = detail_panel;
    ctx.detail_panel_pos      = PANEL_POS_BOTTOM;
    ctx.detail_panel_menu_item = GTK_CHECK_MENU_ITEM(detail_panel_toggle);
    ctx.detail_paned          = outer_paned;
    ctx.content_box           = vbox;
    ctx.hpaned                = hpaned;
    ctx.panel_pos_group       = pos_group;

    if (ctx.plugin_notebook) {
        gtk_paned_pack2(GTK_PANED(detail_hpaned), ctx.plugin_notebook,
                        TRUE, FALSE);
    }

    /* Set up the Name column cell data function so pinned processes
     * get the ➡ prefix.  We need ctx to be initialised first. */
    {
        GList *renderers = gtk_cell_layout_get_cells(
            GTK_CELL_LAYOUT(name_col));
        if (renderers) {
            GtkCellRenderer *name_r = renderers->data;
            gtk_tree_view_column_set_cell_data_func(
                name_col, name_r, name_cell_data_func, &ctx, NULL);
            g_list_free(renderers);
        }
    }

    /* Font menu callbacks (need ctx address, so connect after ctx init) */
    g_signal_connect(font_inc,  "activate", G_CALLBACK(on_font_increase),    &ctx);
    g_signal_connect(font_dec,  "activate", G_CALLBACK(on_font_decrease),    &ctx);
    g_signal_connect(font_auto, "toggled",  G_CALLBACK(on_font_auto_toggle), &ctx);
    g_signal_connect(detail_panel_toggle, "toggled",
                     G_CALLBACK(on_toggle_detail_panel), &ctx);

    /* Detail panel position radio items */
    pos_data_bottom = (panel_pos_data_t){ &ctx, PANEL_POS_BOTTOM };
    pos_data_top    = (panel_pos_data_t){ &ctx, PANEL_POS_TOP };
    pos_data_left   = (panel_pos_data_t){ &ctx, PANEL_POS_LEFT };
    pos_data_right  = (panel_pos_data_t){ &ctx, PANEL_POS_RIGHT };
    g_signal_connect(pos_bottom, "toggled",
                     G_CALLBACK(on_panel_position_changed), &pos_data_bottom);
    g_signal_connect(pos_top, "toggled",
                     G_CALLBACK(on_panel_position_changed), &pos_data_top);
    g_signal_connect(pos_left, "toggled",
                     G_CALLBACK(on_panel_position_changed), &pos_data_left);
    g_signal_connect(pos_right, "toggled",
                     G_CALLBACK(on_panel_position_changed), &pos_data_right);

    g_signal_connect(name_filter_entry, "key-release-event",
                     G_CALLBACK(on_filter_entry_key_release), &ctx);
    g_signal_connect(pid_entry, "key-release-event",
                     G_CALLBACK(on_pid_entry_key_release), &ctx);
    g_signal_connect(pid_entry, "insert-text",
                     G_CALLBACK(on_pid_entry_insert_text), NULL);
    g_signal_connect(tree_overlay, "get-child-position",
                     G_CALLBACK(on_overlay_get_child_position), &ctx);
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

    /* Global keyboard shortcuts (Ctrl+Plus / Ctrl+Minus / Ctrl+0 / Ctrl+Q) */
    g_signal_connect(window, "key-press-event",   G_CALLBACK(on_key_press),   &ctx);
    g_signal_connect(window, "key-release-event", G_CALLBACK(on_key_release), &ctx);

    /* Double-click a row to open the sidebar */
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), &ctx);

    /* Track user collapse / expand actions */
    g_signal_connect(tree, "row-collapsed", G_CALLBACK(on_row_collapsed), &ctx);
    g_signal_connect(tree, "row-expanded",  G_CALLBACK(on_row_expanded),  &ctx);

    /* Update sidebar immediately when selection changes (arrow keys, click) */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), &ctx);

    /* Enable follow-selection when user clicks a sort column */
    g_signal_connect(sort_model, "sort-column-changed",
                     G_CALLBACK(on_sort_column_changed), &ctx);

    /* Disable follow-selection when user scrolls manually */
    gtk_widget_add_events(tree, GDK_SCROLL_MASK);
    g_signal_connect(tree, "scroll-event",
                     G_CALLBACK(on_tree_scroll_event), &ctx);

    ctx.initial_refresh = TRUE;
    ctx.highlight_timer = 0;

    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), &ctx);
    g_timeout_add(50, on_refresh, &ctx);

    /* ── show & run ──────────────────────────────────────────── */
    gtk_widget_show_all(window);
    gtk_widget_hide(menubar);        /* hidden by default; toggle via status-bar right-click */
    /* sidebar_frame is now embedded inside detail_panel, so it becomes
     * visible/hidden together with the detail panel. */
    gtk_widget_hide(detail_panel);   /* hidden by default; toggle via View → Detail Panel */
    gtk_main();

    ui_ctx_destroy(&ctx);

    return NULL;
}

/* ── Fix 6: cleanup ──────────────────────────────────────────── */

void ui_ctx_destroy(ui_ctx_t *ctx)
{
    /* Cancel filter auto-hide timer */
    filter_cancel_hide_timer(ctx);

    /* Free the shadow filter store */
    if (ctx->filter_store) {
        g_object_unref(ctx->filter_store);
        ctx->filter_store = NULL;
    }

#ifdef HAVE_PIPEWIRE
    /* Stop the spectrogram capture and peak meters
     * (may have been started by the PipeWire plugin via host services) */
    spectrogram_stop(ctx);
    pw_meter_stop(ctx);
#endif

    /* Stop autoscroll timer */
    if (ctx->scroll_timer) {
        g_source_remove(ctx->scroll_timer);
        ctx->scroll_timer = 0;
    }

    /* Stop highlight fade timer */
    if (ctx->highlight_timer) {
        g_source_remove(ctx->highlight_timer);
        ctx->highlight_timer = 0;
    }

    /* Free the process tree node set */
    free(ctx->ptree_nodes.pinned_pids);
    free(ctx->ptree_nodes.pids);
    free(ctx->ptree_nodes.states);
    ctx->ptree_nodes.pinned_pids = NULL;
    ctx->ptree_nodes.pids        = NULL;
    ctx->ptree_nodes.states      = NULL;
    ctx->ptree_nodes.count       = 0;
    ctx->ptree_nodes.capacity    = 0;

    /* Free the pinned PIDs set */
    free(ctx->pinned_pids);
    ctx->pinned_pids     = NULL;
    ctx->pinned_count    = 0;
    ctx->pinned_capacity = 0;

    /* Clean up plugin system */
    broker_destroy();
    if (ctx->plugin_registry) {
        plugin_registry_destroy(ctx->plugin_registry);
        free(ctx->plugin_registry);
        ctx->plugin_registry = NULL;
    }
    ctx->plugin_notebook = NULL;
}
