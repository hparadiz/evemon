/*
 * ui.c – GTK3 process-tree UI entry point and event handlers.
 *
 * Displays processes in a hierarchical GtkTreeView.
 * Each process is nested under its parent so you can expand/collapse
 * entire process subtrees.
 *
 * A GLib timeout fires every ~1 s, grabs the latest snapshot from the
 * monitor thread, rebuilds the GtkTreeStore, and re-expands any rows
 * the user had open.
 */

#include "ui_internal.h"

#include "../plugin_loader.h"
#include "../plugin_broker.h"
#include "../event_bus.h"
#include "../fdmon_internal.h"
#include "../settings.h"

#ifdef HAVE_PIPEWIRE
#include "pipewire_graph.h"
#endif

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <utmpx.h>
#include <pwd.h>

/* Host helper: request eBPF monitor for a (pid,fd) pair */
static int host_monitor_fd_subscribe(void *host_ctx, pid_t pid, int fd)
{
    if (!host_ctx) return -1;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return -1;
    /* Ensure write probe is attached */
    if (fdmon_write_enable(ctx->mon->fdmon) != 0)
        return -1;
    return fdmon_add_pid_fd(ctx->mon->fdmon, pid, fd);
}

static void host_monitor_fd_unsubscribe(void *host_ctx, pid_t pid, int fd)
{
    if (!host_ctx) return;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return;
    fdmon_remove_pid_fd(ctx->mon->fdmon, pid, fd);
}

static int host_monitor_watch_children(void *host_ctx, pid_t pid, int fd_mask)
{
    if (!host_ctx) return -1;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return -1;
    /* Ensure the write probes and exec tracepoint are attached */
    if (fdmon_write_enable(ctx->mon->fdmon) != 0) return -1;
    return fdmon_watch_parent_fds(ctx->mon->fdmon, pid, fd_mask);
}

static void host_monitor_unwatch_children(void *host_ctx, pid_t pid)
{
    if (!host_ctx) return;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return;
    fdmon_unwatch_parent_fds(ctx->mon->fdmon, pid);
}

static int host_orphan_capture_enable(void *host_ctx)
{
    if (!host_ctx) return -1;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return -1;
    return fdmon_orphan_stdout_enable(ctx->mon->fdmon);
}

static void host_orphan_capture_disable(void *host_ctx)
{
    if (!host_ctx) return;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return;
    fdmon_orphan_stdout_disable(ctx->mon->fdmon);
}


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
pid_t get_row_pinned_root(GtkTreeModel *model, GtkTreeIter *iter)
{
    gint pr = (gint)PTREE_UNPINNED;
    gtk_tree_model_get(model, iter, COL_PINNED_ROOT, &pr, -1);
    return (pid_t)pr;
}

void stop_autoscroll(ui_ctx_t *ctx)
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
gboolean autoscroll_tick(gpointer data)
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

/* pin/unpin helpers and pinned_panel_create/destroy → see ui_pinned.c */

/* ── Show Audio Processes Only toggle ──────────────────────────── */

static void on_toggle_audio_only(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->show_audio_only = gtk_check_menu_item_get_active(item);
    settings_get()->show_audio_only = ctx->show_audio_only;
    settings_save();

    if (ctx->show_audio_only) {
        /* Build a filtered store showing only audio processes */
        rebuild_audio_filter_store(ctx);
    } else {
        /* If a name filter is active, rebuild that; otherwise switch
         * back to the unfiltered real store. */
        if (ctx->filter_text[0] != '\0')
            rebuild_filter_store(ctx);
        else
            switch_to_real_store(ctx);
    }
}


void show_process_context_menu(ui_ctx_t *ctx, GdkEventButton *ev,
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

    /* ── Show Audio Processes Only toggle ── */
    if (ctx->has_audio_plugin) {
        GtkWidget *mi_audio = gtk_check_menu_item_new_with_label(
            "Show Audio Processes Only");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi_audio),
                                       ctx->show_audio_only);
        g_signal_connect(mi_audio, "toggled",
                         G_CALLBACK(on_toggle_audio_only), ctx);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_audio);
    }

    /* ── Open Plugin as Window submenu ── */
    show_open_plugin_as_window_menu(ctx, menu, pid, name);

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

/* ── selection / detail panel interaction ─────────────────────── */

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)sel;
    proc_detail_update((ui_ctx_t *)data);
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
    gboolean active = gtk_check_menu_item_get_active(item);
    settings_get()->detail_panel_open = active;
    settings_save();
    if (active) {
        gtk_widget_show_all(ctx->detail_vbox);

        /* Only show the main (unpinned) detail panel if a process
         * is actually selected in the tree view.  Pinned panels
         * remain visible regardless. */
        GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
        GList *rows = sel ? gtk_tree_selection_get_selected_rows(sel, NULL) : NULL;
        if (!rows) {
            gtk_widget_hide(ctx->detail_panel);
        } else {
            g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
            /* Restore proc info tray state after show_all */
            if (ctx->proc_info_collapsed) {
                gtk_revealer_set_reveal_child(
                    GTK_REVEALER(ctx->proc_info_revealer), FALSE);
                gtk_widget_show(GTK_WIDGET(ctx->proc_info_summary));
            } else {
                gtk_widget_hide(GTK_WIDGET(ctx->proc_info_summary));
            }
        }
    } else {
        gtk_widget_hide(ctx->detail_vbox);
    }
}

/* ── process info tray: collapse / expand ────────────────────── */

static void on_proc_info_tray_toggle(GtkButton *btn, gpointer data)
{
    (void)btn;
    ui_ctx_t *ctx = data;
    ctx->proc_info_collapsed = !ctx->proc_info_collapsed;
    settings_get()->proc_info_open = !ctx->proc_info_collapsed;
    settings_save();

    gtk_revealer_set_reveal_child(
        GTK_REVEALER(ctx->proc_info_revealer), !ctx->proc_info_collapsed);

    /* Arrow: ◀ means "collapse left", ▶ means "expand right" */
    gtk_button_set_label(GTK_BUTTON(ctx->proc_info_toggle),
                         ctx->proc_info_collapsed ? "▶" : "◀");
    gtk_widget_set_tooltip_text(ctx->proc_info_toggle,
                                ctx->proc_info_collapsed
                                    ? "Show process info"
                                    : "Hide process info");

    /* Show/hide the compact inline summary */
    if (ctx->proc_info_collapsed)
        gtk_widget_show(GTK_WIDGET(ctx->proc_info_summary));
    else
        gtk_widget_hide(GTK_WIDGET(ctx->proc_info_summary));
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
    settings_get()->detail_panel_position = (int)d->pos;
    settings_save();
}

/*
 * Reparent the detail panel into the correct paned position.
 *
 * The layout hierarchy is:
 *   content_box (vbox):
 *     [0] menubar
 *     [1] outer_paned (vertical or horizontal depending on position)
 *           pack1 = tree area (hpaned containing tree_overlay)
 *           pack2 = detail_panel
 *     [2] status_ebox
 *
 * When the position changes, we destroy the old paned, create a new one
 * with the correct orientation, and re-pack everything.
 */
static void detail_panel_relayout(ui_ctx_t *ctx)
{
    gboolean was_visible = gtk_widget_get_visible(ctx->detail_vbox);

    /* Remove detail_vbox and hpaned from their current parent.
     * g_object_ref ensures they survive the container removal. */
    g_object_ref(ctx->detail_vbox);
    g_object_ref(ctx->hpaned);

    GtkWidget *old_paned = ctx->detail_paned;
    if (old_paned) {
        /* The old paned is child [1] of content_box */
        gtk_container_remove(GTK_CONTAINER(old_paned), ctx->hpaned);
        gtk_container_remove(GTK_CONTAINER(old_paned), ctx->detail_vbox);
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
        gtk_paned_pack1(GTK_PANED(new_paned), ctx->detail_vbox, FALSE, FALSE);
        gtk_paned_pack2(GTK_PANED(new_paned), ctx->hpaned, TRUE, FALSE);
    } else {
        gtk_paned_pack1(GTK_PANED(new_paned), ctx->hpaned, TRUE, FALSE);
        gtk_paned_pack2(GTK_PANED(new_paned), ctx->detail_vbox, FALSE, FALSE);
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

    g_object_unref(ctx->detail_vbox);
    g_object_unref(ctx->hpaned);

    /* Insert the new paned into content_box at position [1]
     * (between menubar [0] and status [2]).  pack_start appends,
     * so we use gtk_box_reorder_child to place it correctly. */
    gtk_box_pack_start(GTK_BOX(ctx->content_box), new_paned, TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(ctx->content_box), new_paned, 1);

    gtk_widget_show_all(new_paned);

    /* Hide the main (unpinned) detail panel if nothing is selected */
    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
        GList *rows = sel ? gtk_tree_selection_get_selected_rows(sel, NULL) : NULL;
        if (!rows) {
            gtk_widget_hide(ctx->detail_panel);
        } else {
            g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
            /* Restore proc info tray state after show_all */
            if (ctx->proc_info_collapsed) {
                gtk_revealer_set_reveal_child(
                    GTK_REVEALER(ctx->proc_info_revealer), FALSE);
                gtk_widget_show(GTK_WIDGET(ctx->proc_info_summary));
            } else {
                gtk_widget_hide(GTK_WIDGET(ctx->proc_info_summary));
            }
        }
    }

    if (!was_visible)
        gtk_widget_hide(ctx->detail_vbox);
}


/* ── double-click: open detail panel for the activated row ────── */

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

void on_sort_column_changed(GtkTreeSortable *sortable, gpointer data)
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

/* Update both halves of the status bar from current system state. */
static void update_status_bar(ui_ctx_t *ctx, size_t count)
{
    /* System info (right side) */
    {
        double uptime_secs = 0;
        FILE *f = fopen("/proc/uptime", "r");
        if (f) { int r_ = fscanf(f, "%lf", &uptime_secs); (void)r_; fclose(f); }
        int up_days  = (int)(uptime_secs / 86400);
        int up_hours = (int)((long)uptime_secs % 86400) / 3600;
        int up_mins  = (int)((long)uptime_secs % 3600) / 60;

        int nusers = 0;
        setutxent();
        struct utmpx *ut;
        while ((ut = getutxent()) != NULL)
            if (ut->ut_type == USER_PROCESS) nusers++;
        endutxent();

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

    /* Process/memory/CPU summary (left side) */
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

            /* Select the preselected PID from settings (default: PID 1) */
            {
                pid_t sel_pid = settings_get()->preselected_pid;
                if (sel_pid > 0)
                    goto_pid(ctx, sel_pid);
            }

            /* Honour show_audio_only from settings on first load */
            if (ctx->show_audio_only)
                rebuild_audio_filter_store(ctx);

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
    if (ctx->show_audio_only)
        sync_audio_filter_store(ctx);
    else if (ctx->filter_text[0] != '\0')
        sync_filter_store(ctx);

    /* Kick the highlight fade timer if any rows are newly born/dying */
    ensure_highlight_timer(ctx);

    /* Refresh the set of PIDs with active PipeWire audio streams
     * so the tree can show 🔊 next to audio-using processes. */
    audio_pids_refresh(ctx);

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

    /* Update the process detail panel for the selected process */
    proc_detail_update(ctx);

    /* Update pinned detail panels' header labels */
    pinned_panels_update(ctx);

    /* ── Plugin broker dispatch ──────────────────────────────── */
    /* After the detail panel updates, kick off the plugin data broker.
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

            /* Update follow-selection instances to the selected PID.
             * Skip pinned and floating-window instances — they track
             * a fixed PID and must not follow the tree selection. */
            for (size_t i = 0; i < preg->count; i++) {
                plugin_instance_t *inst = &preg->instances[i];
                if (!inst->pinned && inst->is_active)
                    plugin_instance_set_pid(inst, sel_pid, FALSE);
            }

            /* If nothing is selected, clear all plugins and
             * hide the main (unpinned) detail panel. */
            if (sel_pid <= 0) {
                plugin_dispatch_clear_all(preg);
                if (gtk_widget_get_visible(ctx->detail_panel))
                    gtk_widget_hide(ctx->detail_panel);
            }

            /* Start the broker gather.  When sel_pid <= 0 the broker
             * still runs with zero tracked PIDs so it can take a
             * PipeWire graph snapshot and deliver system-wide audio
             * PIDs for the 🔊 tree-view icons. */
            broker_start(preg, ctx->mon ? ctx->mon->fdmon : NULL);
        }
    }

    update_status_bar(ctx, count);

    { proc_snapshot_t tmp = { local, count }; proc_snapshot_free(&tmp); }
    return G_SOURCE_CONTINUE;

finish:
    /* Unblock signals that were blocked above */
    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);
    g_signal_handlers_unblock_by_func(tree_sel, on_selection_changed, ctx);

    /* First refresh done – finish status update, then remove the fast timer */
    update_status_bar(ctx, count);
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

static void on_menu_restart(GtkMenuItem *item, gpointer data)
{
    (void)item;
    (void)data;
    /* Re-exec ourselves — picks up new plugin config on next load */
    extern char **environ;
    char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = '\0';
        char *argv[] = { path, NULL };
        execve(path, argv, environ);
    }
    /* execve only returns on failure — fall back to exit */
    gtk_main_quit();
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
    GtkSettings *gs = gtk_settings_get_default();
    g_object_set(gs, "gtk-theme-name", name, NULL);

    /* Persist theme choice */
    evemon_settings_t *s = settings_get();
    snprintf(s->theme, sizeof(s->theme), "%s", name);
    settings_save();
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

void reload_font_css(ui_ctx_t *ctx)
{
    char buf[256];

    /* Main process tree */
    snprintf(buf, sizeof(buf),
             "treeview { font-family: Monospace; font-size: %dpt; }",
             ctx->font_size);
    gtk_css_provider_load_from_data(ctx->css, buf, -1, NULL);

    /* Detail panel labels and frame title */
    if (ctx->sidebar_css) {
        snprintf(buf, sizeof(buf),
                 "frame, label, checkbutton { font-size: %dpt; }",
                 ctx->font_size);
        gtk_css_provider_load_from_data(ctx->sidebar_css, buf, -1, NULL);
    }

    /* Plugin notebook: tab labels + all plugin widget contents */
    if (ctx->plugin_css) {
        snprintf(buf, sizeof(buf),
                 ".evemon-plugins tab,"
                 ".evemon-plugins tab label,"
                 ".evemon-plugins treeview,"
                 ".evemon-plugins label,"
                 ".evemon-plugins checkbutton { font-size: %dpt; }",
                 ctx->font_size);
        gtk_css_provider_load_from_data(ctx->plugin_css, buf, -1, NULL);
    }

    /* Name-filter and PID search entries */
    if (ctx->filter_css) {
        snprintf(buf, sizeof(buf),
                 "entry {"
                 "  font-size: %dpt;"
                 "  min-height: 0;"
                 "  padding: 1px 4px;"
                 "  border-radius: 3px;"
                 "}", ctx->font_size);
        gtk_css_provider_load_from_data(ctx->filter_css, buf, -1, NULL);
    }
    if (ctx->pid_css) {
        snprintf(buf, sizeof(buf),
                 "entry {"
                 "  font-size: %dpt;"
                 "  min-height: 0;"
                 "  padding: 1px 4px;"
                 "  border-radius: 3px;"
                 "}", ctx->font_size);
        gtk_css_provider_load_from_data(ctx->pid_css, buf, -1, NULL);
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

static void on_font_increase(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (ctx->font_size < FONT_SIZE_MAX) {
        ctx->font_size++;
        reload_font_css(ctx);
        settings_get()->font_size = ctx->font_size;
        settings_save();
    }
}

static void on_font_decrease(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (ctx->font_size > FONT_SIZE_MIN) {
        ctx->font_size--;
        reload_font_css(ctx);
        settings_get()->font_size = ctx->font_size;
        settings_save();
    }
}

static void on_font_reset(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    ctx->font_size = FONT_SIZE_DEFAULT;
    reload_font_css(ctx);
    settings_get()->font_size = ctx->font_size;
    settings_save();
}

static void on_menu_filter(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (!ctx->filter_entry) return;
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

static void on_menu_goto_pid(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (!ctx->pid_entry) return;
    if (gtk_widget_get_visible(ctx->pid_entry)) {
        gtk_entry_set_text(GTK_ENTRY(ctx->pid_entry), "");
        gtk_widget_hide(ctx->pid_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
    } else {
        gtk_widget_show(ctx->pid_entry);
        gtk_widget_grab_focus(ctx->pid_entry);
    }
}

/* ── column visibility toggle ─────────────────────────────────── */

/*
 * Callback for View → Columns → <column name> check menu items.
 * Toggles the column's visibility and persists the set of visible
 * columns to settings.json.
 */
static void on_column_toggled(GtkCheckMenuItem *item, gpointer data)
{
    GtkTreeViewColumn *col = data;
    gboolean active = gtk_check_menu_item_get_active(item);
    gtk_tree_view_column_set_visible(col, active);

    /* Rebuild the settings columns[] list from the tree view */
    GtkTreeView *view = GTK_TREE_VIEW(
        gtk_tree_view_column_get_tree_view(col));
    GList *all_cols = gtk_tree_view_get_columns(view);

    evemon_settings_t *s = settings_get();
    s->column_count = 0;
    for (GList *c = all_cols; c; c = c->next) {
        GtkTreeViewColumn *tv_col = c->data;
        if (gtk_tree_view_column_get_visible(tv_col)) {
            const char *title = gtk_tree_view_column_get_title(tv_col);
            if (title && s->column_count < SETTINGS_MAX_COLUMNS) {
                snprintf(s->columns[s->column_count],
                         SETTINGS_COL_NAME_MAX, "%s", title);
                s->column_count++;
            }
        }
    }
    g_list_free(all_cols);
    settings_save();
}

/*
 * Build the View → Columns submenu with a check item for each
 * tree view column.  Visibility is initialised from the current
 * column state (which was set from settings at startup).
 */
static GtkWidget *build_columns_submenu(GtkTreeView *view)
{
    GtkWidget *menu = gtk_menu_new();
    GList *cols = gtk_tree_view_get_columns(view);

    for (GList *c = cols; c; c = c->next) {
        GtkTreeViewColumn *tv_col = c->data;
        const char *title = gtk_tree_view_column_get_title(tv_col);
        if (!title || title[0] == '\0') continue;

        GtkWidget *mi = gtk_check_menu_item_new_with_label(title);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi),
                                       gtk_tree_view_column_get_visible(tv_col));
        g_signal_connect(mi, "toggled",
                         G_CALLBACK(on_column_toggled), tv_col);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    g_list_free(cols);
    return menu;
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
void register_sort_funcs(GtkTreeModelSort *sm)
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

/* ── expand / collapse helpers ───────────────────────────────── */

void expand_respecting_collapsed_recurse(ui_ctx_t *ctx,
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

            pid_t pinned_root = get_row_pinned_root(child_model, &child_it);
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            if (get_process_tree_node(&ctx->ptree_nodes, pinned_root,
                                      (pid_t)pid) == PTREE_COLLAPSED) {
                gtk_tree_view_collapse_row(ctx->view, path);
            } else {
                gtk_tree_view_expand_row(ctx->view, path, FALSE);
                expand_respecting_collapsed_recurse(ctx, model, &iter);
            }
            gtk_tree_path_free(path);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

void expand_respecting_collapsed(ui_ctx_t *ctx)
{
    GtkTreeModel *model = gtk_tree_view_get_model(ctx->view);
    if (!model) return;
    expand_respecting_collapsed_recurse(ctx, model, NULL);
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
/* ── Audio PID probing ────────────────────────────────────────
 * Take a PipeWire graph snapshot and collect every PID that has
 * an active audio stream.  Called once per refresh tick when the
 * audio plugin is loaded.
 */
/*
 * Broker callback: receives audio PIDs extracted from the PipeWire
 * graph on the broker worker thread, delivered to us on the GTK
 * main thread.  This replaces the old approach of calling
 * pw_snapshot() directly on the main thread, which was unsafe
 * (raced with the broker's own pw_snapshot on the worker thread
 * via non-thread-safe pw_init / setenv calls → SIGSEGV).
 */
static void on_broker_audio_pids(const pid_t *pids, size_t count,
                                 void *data)
{
    ui_ctx_t *ctx = data;
    if (ctx->shutting_down)
        return;

    /* Grow the audio_pids array if needed */
    if (count > ctx->audio_pid_cap) {
        size_t new_cap = count < 64 ? 64 : count;
        pid_t *tmp = realloc(ctx->audio_pids, new_cap * sizeof(pid_t));
        if (!tmp) { ctx->audio_pid_count = 0; return; }
        ctx->audio_pids   = tmp;
        ctx->audio_pid_cap = new_cap;
    }
    memcpy(ctx->audio_pids, pids, count * sizeof(pid_t));
    ctx->audio_pid_count = count;
}

void audio_pids_refresh(ui_ctx_t *ctx)
{
    /* Audio PIDs are now updated asynchronously via the broker
     * callback (on_broker_audio_pids).  This function is kept
     * as a no-op placeholder for callers that still reference it. */
    (void)ctx;
}

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

    gboolean is_pinned = display && pid_is_pinned(ctx, (pid_t)pid);
    gboolean is_audio  = audio_pid_is_active(ctx, (pid_t)pid);

    if (is_pinned || is_audio) {
        char buf[512];
        const char *prefix = is_audio && is_pinned ? "\xF0\x9F\x94\x8A ➡ "
                           : is_audio              ? "\xF0\x9F\x94\x8A "
                           :                         "➡ ";
        snprintf(buf, sizeof(buf), "%s%s", prefix, display ? display : "");
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
 * These bridge from the plugin API to the detail panel's pw_meter.c
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

static void host_pw_meter_remove_nodes(void *host_ctx,
                                       const uint32_t *node_ids,
                                       size_t count)
{
    ui_ctx_t *c = host_ctx;
    pw_meter_remove_nodes(c, node_ids, count);
}

static void host_spectro_start(void *host_ctx, GtkDrawingArea *draw_area,
                               uint32_t node_id)
{
    ui_ctx_t *c = host_ctx;
    spectrogram_start_for_node(c, draw_area, node_id);
}

static void host_spectro_stop(void *host_ctx, GtkDrawingArea *draw_area)
{
    ui_ctx_t *c = host_ctx;
    spectrogram_stop(c, draw_area);
}

static uint32_t host_spectro_get_target(void *host_ctx,
                                        GtkDrawingArea *draw_area)
{
    ui_ctx_t *c = host_ctx;
    return spectrogram_get_target_node(c, draw_area);
}

static void host_spectro_set_theme(void *host_ctx,
                                   GtkDrawingArea *draw_area,
                                   unsigned theme_index)
{
    ui_ctx_t *c = host_ctx;
    spectrogram_set_theme(c, draw_area, (spectro_theme_t)theme_index);
}
#endif /* HAVE_PIPEWIRE */

/* Trampoline: open a plugin in a floating window (callable by plugins) */
static void host_open_plugin_window(void *host_ctx,
                                    const char *plugin_id,
                                    pid_t pid,
                                    const char *proc_name)
{
    ui_ctx_t *c = host_ctx;
    open_plugin_window(c, pid,
                       proc_name ? proc_name : "",
                       plugin_id);
}

/* ── UI thread ───────────────────────────────────────────────── */

void *ui_thread(void *arg)
{
    monitor_state_t *mon = (monitor_state_t *)arg;

    /* ── window ──────────────────────────────────────────────── */
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "ＥＶＥＭＯＮ");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);

    /* Accel group for displaying hotkey hints in menus.
     * The actual key handling is in on_key_press. */
    GtkAccelGroup *accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(window), accel_group);

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

    /* Apply column visibility from settings.
     * If columns[] is non-empty, only listed columns are visible. */
    {
        const evemon_settings_t *s = settings_get();
        if (s->column_count > 0) {
            GList *all = gtk_tree_view_get_columns(GTK_TREE_VIEW(tree));
            for (GList *c = all; c; c = c->next) {
                GtkTreeViewColumn *tv_col = c->data;
                const char *title = gtk_tree_view_column_get_title(tv_col);
                if (!title) continue;
                gboolean found = FALSE;
                for (int i = 0; i < s->column_count; i++) {
                    if (strcmp(s->columns[i], title) == 0) {
                        found = TRUE;
                        break;
                    }
                }
                gtk_tree_view_column_set_visible(tv_col, found);
            }
            g_list_free(all);
        }
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

    /* Compact CSS: small font, minimal padding so it fits the header row.
     * NOTE: don't unref – kept alive for dynamic font changes. */
    GtkCssProvider *filt_css = gtk_css_provider_new();
    {
        char fbuf[128];
        snprintf(fbuf, sizeof(fbuf),
                 "entry {"
                 "  font-size: %dpt;"
                 "  min-height: 0;"
                 "  padding: 1px 4px;"
                 "  border-radius: 3px;"
                 "}", FONT_SIZE_DEFAULT);
        gtk_css_provider_load_from_data(filt_css, fbuf, -1, NULL);
    }
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(name_filter_entry),
        GTK_STYLE_PROVIDER(filt_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

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

    /* NOTE: don't unref – kept alive for dynamic font changes. */
    GtkCssProvider *pid_entry_css = gtk_css_provider_new();
    {
        char pbuf[128];
        snprintf(pbuf, sizeof(pbuf),
                 "entry {"
                 "  font-size: %dpt;"
                 "  min-height: 0;"
                 "  padding: 1px 4px;"
                 "  border-radius: 3px;"
                 "}", FONT_SIZE_DEFAULT);
        gtk_css_provider_load_from_data(pid_entry_css, pbuf, -1, NULL);
    }
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(pid_entry),
        GTK_STYLE_PROVIDER(pid_entry_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    gtk_overlay_add_overlay(GTK_OVERLAY(tree_overlay), pid_entry);

    /* ── process detail panel ─────────────────────────────────── */

    /*
     * Helper: create a collapsible detail panel section.
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

    /* ── Pack detail panel content ───────────────────────────────── */
    gtk_box_pack_start(GTK_BOX(sidebar_vbox), info_section, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(sidebar_scroll), sidebar_vbox);

    GtkWidget *sidebar_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(sidebar_frame), sidebar_scroll);

    /* Live CSS provider for detail panel font size (cascades to all children) */
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

    GtkWidget *restart_item = gtk_menu_item_new_with_label("Restart");
    g_signal_connect(restart_item, "activate",
                     G_CALLBACK(on_menu_restart), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), restart_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());

    GtkWidget *exit_item = gtk_menu_item_new_with_label("Exit");
    gtk_widget_add_accelerator(exit_item, "activate", accel_group,
                               GDK_KEY_q, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
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

    /* Columns visibility submenu */
    GtkWidget *columns_item = gtk_menu_item_new_with_label("Columns");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(columns_item),
                              build_columns_submenu(GTK_TREE_VIEW(tree)));
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), columns_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          gtk_separator_menu_item_new());

    /* Filter & Go-to-PID with hotkey hints */
    GtkWidget *filter_item = gtk_menu_item_new_with_label("Filter by Name");
    gtk_widget_add_accelerator(filter_item, "activate", accel_group,
                               GDK_KEY_f, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), filter_item);

    GtkWidget *goto_pid_item = gtk_menu_item_new_with_label("Go to PID");
    gtk_widget_add_accelerator(goto_pid_item, "activate", accel_group,
                               GDK_KEY_g, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), goto_pid_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          gtk_separator_menu_item_new());

    GtkWidget *appear_menu = gtk_menu_new();
    GtkWidget *appear_item = gtk_menu_item_new_with_label("Appearance");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(appear_item), appear_menu);

    GtkWidget *font_inc = gtk_menu_item_new_with_label("Increase Font");
    gtk_widget_add_accelerator(font_inc, "activate", accel_group,
                               GDK_KEY_plus, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    GtkWidget *font_dec = gtk_menu_item_new_with_label("Decrease Font");
    gtk_widget_add_accelerator(font_dec, "activate", accel_group,
                               GDK_KEY_minus, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    GtkWidget *font_reset = gtk_menu_item_new_with_label("Reset Font");
    gtk_widget_add_accelerator(font_reset, "activate", accel_group,
                               GDK_KEY_0, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);

    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu), font_inc);
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu), font_dec);
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu), font_reset);

    /* Theme picker submenu */
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu),
                          gtk_separator_menu_item_new());
    GtkWidget *theme_item = gtk_menu_item_new_with_label("Theme");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(theme_item),
                              build_theme_submenu());
    gtk_menu_shell_append(GTK_MENU_SHELL(appear_menu), theme_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), appear_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_item);

    /* Plugins menu — rebuilt dynamically each time it opens */
    GtkWidget *plugins_menu = gtk_menu_new();
    GtkWidget *plugins_item = gtk_menu_item_new_with_label("Plugins");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(plugins_item), plugins_menu);
    /* on_plugins_menu_map needs ctx, but ctx isn't fully initialised yet;
     * we connect the signal after ctx is set up (see below). */
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), plugins_item);

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

    /* ── detail panel (wraps process info + plugin notebook) ──── */
    /* The detail panel contains a horizontal paned:
     *   pack1 = collapsible tray (toggle button + revealer(sidebar_frame))
     *   pack2 = plugin notebook (tabs for FD, Env, Mmap, Libs, Net, …)
     * The process info pane can be collapsed via the toggle button,
     * giving full width to the plugin notebook. */
    GtkWidget *detail_panel = gtk_frame_new(NULL);
    gtk_widget_set_size_request(detail_panel, -1, 200);

    /* Wrap sidebar_frame in a GtkRevealer for slide animation */
    GtkWidget *proc_info_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(proc_info_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(proc_info_revealer),
                                         SECTION_TRANSITION_MS);
    gtk_revealer_set_reveal_child(GTK_REVEALER(proc_info_revealer), TRUE);
    gtk_widget_set_size_request(sidebar_frame, 280, -1);
    gtk_container_add(GTK_CONTAINER(proc_info_revealer), sidebar_frame);

    /* Toggle button: thin handle on the LEFT to collapse/expand */
    GtkWidget *proc_info_toggle = gtk_button_new_with_label("◀");
    gtk_widget_set_size_request(proc_info_toggle, 16, -1);
    gtk_widget_set_tooltip_text(proc_info_toggle, "Hide process info");

    /* Compact inline summary label (vertical text, shown when collapsed) */
    GtkWidget *proc_info_summary_w = gtk_label_new("–");
    gtk_label_set_angle(GTK_LABEL(proc_info_summary_w), 90);
    gtk_label_set_ellipsize(GTK_LABEL(proc_info_summary_w), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(proc_info_summary_w, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(proc_info_summary_w, TRUE);
    gtk_widget_hide(proc_info_summary_w);  /* hidden until collapsed */

    {
        /* Minimal padding for toggle button + summary label */
        GtkCssProvider *tray_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(tray_css,
            "button { padding: 0; min-width: 14px; }"
            ".proc-info-summary { padding: 2px 0; font-size: 9pt; }",
            -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(proc_info_toggle),
            GTK_STYLE_PROVIDER(tray_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(proc_info_summary_w),
            GTK_STYLE_PROVIDER(tray_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(proc_info_summary_w),
            "proc-info-summary");
        g_object_unref(tray_css);
    }

    /* Tray container: [toggle] [summary] [revealer(sidebar_frame)] */
    GtkWidget *tray_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(tray_box), proc_info_toggle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tray_box), proc_info_summary_w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tray_box), proc_info_revealer, FALSE, FALSE, 0);

    /* Inner horizontal paned: tray (process info) | notebook */
    GtkWidget *detail_hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(detail_hpaned), tray_box, FALSE, FALSE);
    /* The notebook will be added to pack2 after plugin loading below. */
    gtk_container_add(GTK_CONTAINER(detail_panel), detail_hpaned);

    /* Box to hold pinned-process detail panels (stacked vertically) */
    GtkWidget *pinned_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Wrapper: detail_panel on top, pinned panels below, scrollable */
    GtkWidget *detail_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(detail_vbox), detail_panel, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(detail_vbox), pinned_box, FALSE, FALSE, 0);

    /* ── layout ──────────────────────────────────────────────── */
    /* Default: detail panel docked at bottom.
     * outer_paned (vertical): pack1 = hpaned, pack2 = detail_vbox */
    GtkWidget *outer_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(outer_paned), hpaned, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(outer_paned), detail_vbox, FALSE, FALSE);

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
    ctx.font_size    = settings_get()->font_size;
    ctx.ptree_nodes  = (ptree_node_set_t){ NULL, NULL, NULL, 0, 0 };
    ctx.follow_selection = FALSE;

    /* Name filter */
    ctx.filter_store = NULL;
    ctx.sort_model   = GTK_TREE_MODEL_SORT(sort_model);
    ctx.filter_entry = name_filter_entry;
    ctx.filter_css   = filt_css;
    ctx.name_col     = name_col;
    ctx.filter_text[0] = '\0';
    ctx.show_audio_only = settings_get()->show_audio_only;

    /* Go-to-PID */
    ctx.pid_entry = pid_entry;
    ctx.pid_css   = pid_entry_css;
    ctx.pid_col   = pid_col;

    /* Process detail panel */
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

    /* Steam/Proton detail */
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
    ctx.spectro_count  = 0;
    memset(ctx.spectro_instances, 0, sizeof(ctx.spectro_instances));
#endif

    /* Pinned processes */
    ctx.pinned_pids     = NULL;
    ctx.pinned_count    = 0;
    ctx.pinned_capacity = 0;

    /* Pinned detail panels */
    ctx.pinned_panels      = NULL;
    ctx.pinned_panel_count = 0;
    ctx.pinned_panel_cap   = 0;
    ctx.pinned_box         = pinned_box;

    /* ── Plugin system ───────────────────────────────────────── */
    {
        /* Allocate the registry on the heap so we have a stable pointer */
        plugin_registry_t *preg = calloc(1, sizeof(plugin_registry_t));
        if (preg) {
            plugin_registry_init(preg);

            /* Initialise the event bus before loading any plugins */
            evemon_event_bus_init();

            /* Build host services table on the heap so it outlives this block */
            evemon_host_services_t *hsvc = calloc(1, sizeof(evemon_host_services_t));
            if (hsvc) {
                hsvc->host_ctx          = &ctx;
#ifdef HAVE_PIPEWIRE
                hsvc->pw_meter_start    = host_pw_meter_start;
                hsvc->pw_meter_stop     = host_pw_meter_stop;
                hsvc->pw_meter_read     = host_pw_meter_read;
                hsvc->pw_meter_remove_nodes = host_pw_meter_remove_nodes;
                hsvc->spectro_start     = host_spectro_start;
                hsvc->spectro_stop      = host_spectro_stop;
                hsvc->spectro_get_target = host_spectro_get_target;
                hsvc->spectro_set_theme  = host_spectro_set_theme;
#endif
                /* Event bus wiring */
                hsvc->subscribe         = host_event_subscribe;
                hsvc->publish           = host_event_publish;
                hsvc->unsubscribe       = host_event_unsubscribe;
                /* eBPF fd monitoring helpers */
                hsvc->monitor_fd_subscribe      = host_monitor_fd_subscribe;
                hsvc->monitor_fd_unsubscribe    = host_monitor_fd_unsubscribe;
                hsvc->monitor_watch_children    = host_monitor_watch_children;
                hsvc->monitor_unwatch_children  = host_monitor_unwatch_children;
                /* Orphan-capture (cron/daemon) mode */
                hsvc->orphan_capture_enable  = host_orphan_capture_enable;
                hsvc->orphan_capture_disable = host_orphan_capture_disable;
                hsvc->open_plugin_window     = host_open_plugin_window;
                plugin_registry_set_host_services(preg, hsvc);
            }

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

            /* Remember the plugin directory so the Plugins menu can
             * scan for new .so files at open time. */
            snprintf(ctx.plugin_dir, sizeof(ctx.plugin_dir),
                     "%s", plugin_dir);

            /* Detect whether any loaded plugin needs PipeWire data;
             * if so, enable audio-PID probing in the process tree. */
            ctx.has_audio_plugin = FALSE;
            for (size_t i = 0; i < preg->count; i++) {
                if (preg->instances[i].plugin &&
                    (preg->instances[i].plugin->data_needs &
                     evemon_NEED_PIPEWIRE)) {
                    ctx.has_audio_plugin = TRUE;
                    fprintf(stdout, "evemon: audio plugin detected "
                            "(%s) – enabling audio PID probing\n",
                            preg->instances[i].plugin->id);
                    break;
                }
            }

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

                /* Plugins listed here are appended at the very end,
                 * after the pass-2 "remaining" plugins, and are hidden
                 * by default (the plugin itself shows them on demand). */
                static const char *tab_order_last[] = {
                    "org.evemon.milkdrop",
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
                        /* Headless plugins have no tab */
                        if (inst->plugin->kind == EVEMON_PLUGIN_HEADLESS)
                            continue;
                        if (!PLUGIN_IS_AVAILABLE(inst))
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
                        plugin_instance_set_active(inst, TRUE);
                        added[i] = TRUE;
                    }
                }

                /* Pass 2: remaining plugins in load order (skip last-order) */
                for (size_t i = 0; i < preg->count; i++) {
                    if (added[i]) continue;
                    plugin_instance_t *inst = &preg->instances[i];
                    /* Headless plugins have no tab */
                    if (inst->plugin &&
                        inst->plugin->kind == EVEMON_PLUGIN_HEADLESS) {
                        added[i] = TRUE;
                        continue;
                    }
                    /* Skip plugins reserved for pass 3 (last) */
                    if (inst_is_last_order(preg->instances[i].plugin,
                                           tab_order_last))
                        continue;
                    if (!inst->widget) continue;
                    if (!PLUGIN_IS_AVAILABLE(inst)) continue;
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
                    plugin_instance_set_active(inst, TRUE);
                    added[i] = TRUE;
                }

                /* Pass 3: "load last" plugins, hidden by default */
                for (int o = 0; tab_order_last[o]; o++) {
                    for (size_t i = 0; i < preg->count; i++) {
                        if (added[i]) continue;
                        plugin_instance_t *inst = &preg->instances[i];
                        if (!inst->widget || !inst->plugin ||
                            !inst->plugin->id)
                            continue;
                        if (!PLUGIN_IS_AVAILABLE(inst))
                            continue;
                        if (strcmp(inst->plugin->id,
                                   tab_order_last[o]) != 0)
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
                        gtk_notebook_append_page(
                            GTK_NOTEBOOK(notebook),
                            inst->widget, label);
                        plugin_instance_set_active(inst, TRUE);
                        /* Hide the tab until the plugin activates it */
                        gtk_widget_set_no_show_all(inst->widget, TRUE);
                        gtk_widget_hide(inst->widget);
                        GtkWidget *tab_lbl = gtk_notebook_get_tab_label(
                            GTK_NOTEBOOK(notebook), inst->widget);
                        if (tab_lbl) {
                            gtk_widget_set_no_show_all(tab_lbl, TRUE);
                            gtk_widget_hide(tab_lbl);
                        }
                        added[i] = TRUE;
                    }
                }

                g_free(added);
            }

            /* The plugin notebook lives in the detail panel alongside
             * the process info.  It is NOT packed into sidebar_vbox. */

            ctx.plugin_registry = preg;
            ctx.plugin_notebook = notebook;

            /* Wire up the Plugins menu now that ctx is ready.
             * Use "show" (not "map") so it fires only when the user
             * actually opens the dropdown, not during show_all(window). */
            g_signal_connect(plugins_menu, "show",
                             G_CALLBACK(on_plugins_menu_map), &ctx);
            ctx.plugins_menu_item = plugins_item;

            /* Live CSS provider for plugin notebook font size */
            gtk_style_context_add_class(
                gtk_widget_get_style_context(notebook), "evemon-plugins");
            ctx.plugin_css = gtk_css_provider_new();
            {
                char pbuf[256];
                snprintf(pbuf, sizeof(pbuf),
                         ".evemon-plugins tab,"
                         ".evemon-plugins tab label,"
                         ".evemon-plugins treeview,"
                         ".evemon-plugins label,"
                         ".evemon-plugins checkbutton { font-size: %dpt; }",
                         settings_get()->font_size);
                gtk_css_provider_load_from_data(ctx.plugin_css, pbuf, -1, NULL);
            }
            gtk_style_context_add_provider_for_screen(
                gdk_screen_get_default(),
                GTK_STYLE_PROVIDER(ctx.plugin_css),
                GTK_STYLE_PROVIDER_PRIORITY_USER);
        } else {
            ctx.plugin_registry = NULL;
            ctx.plugin_notebook = NULL;
        }
        ctx.plugin_broker = NULL;  /* broker is stateless (module-level) */

        /* Register for audio PIDs delivered by the broker worker thread. */
        if (ctx.has_audio_plugin)
            broker_set_audio_callback(on_broker_audio_pids, &ctx);
    }

    /* ── Finish detail panel setup: put notebook into the hpaned ── */
    ctx.detail_panel          = detail_panel;
    ctx.detail_vbox           = detail_vbox;
    ctx.detail_panel_pos      = (panel_position_t)settings_get()->detail_panel_position;
    ctx.detail_panel_menu_item = GTK_CHECK_MENU_ITEM(detail_panel_toggle);
    ctx.detail_paned          = outer_paned;
    ctx.content_box           = vbox;
    ctx.hpaned                = hpaned;
    ctx.panel_pos_group       = pos_group;

    /* Collapsible process info tray */
    ctx.proc_info_revealer  = proc_info_revealer;
    ctx.proc_info_toggle    = proc_info_toggle;
    ctx.proc_info_summary   = GTK_LABEL(proc_info_summary_w);
    ctx.proc_info_collapsed = !settings_get()->proc_info_open;

    g_signal_connect(proc_info_toggle, "clicked",
                     G_CALLBACK(on_proc_info_tray_toggle), &ctx);

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
    g_signal_connect(font_inc,   "activate", G_CALLBACK(on_font_increase),    &ctx);
    g_signal_connect(font_dec,   "activate", G_CALLBACK(on_font_decrease),    &ctx);
    g_signal_connect(font_reset, "activate", G_CALLBACK(on_font_reset),       &ctx);
    g_signal_connect(filter_item,   "activate", G_CALLBACK(on_menu_filter),   &ctx);
    g_signal_connect(goto_pid_item, "activate", G_CALLBACK(on_menu_goto_pid), &ctx);
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

    /* Double-click a row to open the detail panel */
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

    /* ── Apply theme from settings ────────────────────────────── */
    {
        const evemon_settings_t *s = settings_get();
        if (s->theme[0]) {
            GtkSettings *gtk_s = gtk_settings_get_default();
            g_object_set(gtk_s, "gtk-theme-name", s->theme, NULL);
        }
    }

    /* ── Apply detail panel position layout before showing ────── */
    if (ctx.detail_panel_pos != PANEL_POS_BOTTOM)
        detail_panel_relayout(&ctx);

    /* ── show & run ──────────────────────────────────────────── */
    gtk_widget_show_all(window);
    gtk_widget_hide(menubar);        /* hidden by default; toggle via status-bar right-click */

    /* Detail panel: honour settings (default = hidden) */
    if (settings_get()->detail_panel_open) {
        gtk_check_menu_item_set_active(ctx.detail_panel_menu_item, TRUE);
        /* proc info tray state */
        if (ctx.proc_info_collapsed) {
            gtk_revealer_set_reveal_child(
                GTK_REVEALER(ctx.proc_info_revealer), FALSE);
            gtk_button_set_label(GTK_BUTTON(ctx.proc_info_toggle), "▶");
            gtk_widget_show(GTK_WIDGET(ctx.proc_info_summary));
        }
    } else {
        gtk_widget_hide(detail_vbox);
    }
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
    /* Stop all spectrogram instances and peak meters
     * (may have been started by PipeWire plugins via host services) */
    while (ctx->spectro_count > 0) {
        GtkDrawingArea *da = GTK_DRAWING_AREA(
            ctx->spectro_instances[0].draw_area);
        spectrogram_stop(ctx, da);
    }
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

    /* Cancel the broker FIRST — a worker thread may still be running
     * and its completion callback references ctx->audio_pids. */
    broker_destroy();

    /* Free the audio PID set (safe now that the broker is torn down) */
    free(ctx->audio_pids);
    ctx->audio_pids      = NULL;
    ctx->audio_pid_count = 0;
    ctx->audio_pid_cap   = 0;

    /* Free the pinned PIDs set */
    free(ctx->pinned_pids);
    ctx->pinned_pids     = NULL;
    ctx->pinned_count    = 0;
    ctx->pinned_capacity = 0;

    /* Free pinned detail panels (plugin instances freed by registry_destroy) */
    for (size_t i = 0; i < ctx->pinned_panel_count; i++)
        free(ctx->pinned_panels[i].instance_ids);
    free(ctx->pinned_panels);
    ctx->pinned_panels      = NULL;
    ctx->pinned_panel_count = 0;
    ctx->pinned_panel_cap   = 0;

    /* Close any floating plugin windows — destroy the GTK window (which
     * fires on_plugin_window_destroyed → plugin_instance_destroy) and
     * then free the tracking array.  We disconnect the signal first to
     * avoid a double-free: plugin_registry_destroy below will also
     * clean up any surviving instances. */
    for (size_t i = 0; i < ctx->plugin_window_count; i++) {
        if (ctx->plugin_windows[i].window) {
            g_signal_handlers_disconnect_by_func(
                ctx->plugin_windows[i].window,
                G_CALLBACK(on_plugin_window_delete), ctx);
            gtk_widget_destroy(ctx->plugin_windows[i].window);
        }
    }
    free(ctx->plugin_windows);
    ctx->plugin_windows      = NULL;
    ctx->plugin_window_count = 0;
    ctx->plugin_window_cap   = 0;

    /* Clean up plugin system */
    evemon_event_bus_destroy();
    if (ctx->plugin_registry) {
        plugin_registry_destroy(ctx->plugin_registry);
        free(ctx->plugin_registry);
        ctx->plugin_registry = NULL;
    }
    ctx->plugin_notebook = NULL;
}
