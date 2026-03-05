/*
 * proc_detail.c – process detail panel: show selected-process info.
 *
 * The detail panel contains the "Process Info" section with
 * basic metadata (PID, name, CPU%, RSS, etc.), Steam/Proton info,
 * and cgroup limits.  The old per-section data scanners (FD, env,
 * mmap, libs, network) have been retired in favour of the plugin
 * tab system in the detail panel.
 *
 * Performance note
 * ────────────────
 * proc_detail_update() is split into two phases so the panel appears
 * instantly on selection:
 *
 *   Phase 1 (synchronous, fast):
 *     – gtk_widget_show_all for the panel reveal
 *     – all gtk_label_set_text calls (PID, CPU, RSS, …)
 *     – hides the Steam/cgroup frames immediately
 *
 *   Phase 2 (deferred, via g_idle_add):
 *     – steam_detect + ancestor walk (reads /proc/environ)
 *     – cgroup_scan_start (reads /proc/cgroup + /sys/fs/cgroup/)
 *
 * Phase 2 only runs when the main loop is otherwise idle, so GTK gets
 * to paint the panel with basic info before any file I/O happens.
 * A generation counter (detail_gen) is incremented on each selection
 * change; the idle callback aborts if the generation has moved on
 * (i.e. the user already clicked somewhere else).
 */

#include "ui_internal.h"
#include "../steam.h"
#include "../fdmon.h"
#include <time.h>

extern struct timespec evemon_start_time;

/* ── deferred Steam/cgroup update context ─────────────────────── */

typedef struct {
    ui_ctx_t  *ctx;
    pid_t      pid;
    gchar     *name;
    gchar     *cmdline;
    gchar     *steam_label;
    guint      generation;   /* ctx->detail_gen snapshot at enqueue time */
} detail_deferred_t;

/* Generation counter – bumped on every selection change */
static guint detail_gen = 0;

static gboolean detail_deferred_cb(gpointer data)
{
    detail_deferred_t *d = data;
    ui_ctx_t *ctx = d->ctx;

    /* Abort if the UI is shutting down or the user selected a different process */
    if (ctx->shutting_down || d->generation != detail_gen) {
        g_free(d->name);
        g_free(d->cmdline);
        g_free(d->steam_label);
        free(d);
        return G_SOURCE_REMOVE;
    }

    /* ── Steam / Proton metadata ─────────────────────────────── */
    {
        gboolean is_steam = (d->steam_label && d->steam_label[0]);
        steam_info_t *si = NULL;

        if (is_steam && d->pid > 0) {
            si = steam_detect(d->pid, d->name, d->cmdline, NULL);

            if (!si) {
                /* Walk ancestors in the tree model to find Steam env vars */
                GtkTreeModel *child_model =
                    gtk_tree_model_sort_get_model(ctx->sort_model);
                GtkTreeIter ancestor_iter;
                /* Find the row for d->pid */
                if (find_iter_by_pid(child_model, NULL,
                                     d->pid, &ancestor_iter)) {
                    while (!si) {
                        GtkTreeIter parent_iter;
                        if (!gtk_tree_model_iter_parent(child_model,
                                                        &parent_iter,
                                                        &ancestor_iter))
                            break;
                        ancestor_iter = parent_iter;
                        gint anc_pid = 0;
                        gchar *anc_name = NULL, *anc_cmd = NULL;
                        gtk_tree_model_get(child_model, &ancestor_iter,
                                           COL_PID,     &anc_pid,
                                           COL_NAME,    &anc_name,
                                           COL_CMDLINE, &anc_cmd, -1);
                        steam_info_t *parent_si =
                            steam_detect((pid_t)anc_pid,
                                         anc_name, anc_cmd, NULL);
                        g_free(anc_name);
                        g_free(anc_cmd);
                        if (parent_si) {
                            si = steam_detect(d->pid, d->name,
                                              d->cmdline, parent_si);
                            if (!si)
                                si = parent_si;
                            else
                                free(parent_si);
                        }
                    }
                }
            }
        }

        if (si) {
            gtk_label_set_text(ctx->sb_steam_game,
                               si->game_name[0]      ? si->game_name      : "–");
            gtk_label_set_text(ctx->sb_steam_appid,
                               si->app_id[0]         ? si->app_id         : "–");
            gtk_label_set_text(ctx->sb_steam_proton,
                               si->proton_version[0] ? si->proton_version : "–");
            gtk_label_set_text(ctx->sb_steam_runtime,
                               si->runtime_layer[0]  ? si->runtime_layer  : "–");
            gtk_label_set_text(ctx->sb_steam_compat,
                               si->compat_data[0]    ? si->compat_data    : "–");
            gtk_label_set_text(ctx->sb_steam_gamedir,
                               si->game_dir[0]       ? si->game_dir       : "–");
            gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->sb_steam_gamedir),
                                        si->game_dir[0] ? si->game_dir : NULL);
            gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->sb_steam_compat),
                                        si->compat_data[0] ? si->compat_data : NULL);
            gtk_widget_set_no_show_all(ctx->sb_steam_frame, FALSE);
            gtk_widget_show_all(ctx->sb_steam_frame);
            gtk_widget_set_no_show_all(ctx->sb_steam_frame, TRUE);
            free(si);
        } else {
            gtk_widget_hide(ctx->sb_steam_frame);
        }
    }

    /* ── cgroup limits ───────────────────────────────────────── */
    if (d->pid > 0)
        cgroup_scan_start(ctx, d->pid);

    g_free(d->name);
    g_free(d->cmdline);
    g_free(d->steam_label);
    free(d);
    return G_SOURCE_REMOVE;
}

/* ── process detail panel: update from selection ─────────────── */

void proc_detail_update(ui_ctx_t *ctx)
{
    /* Bump generation so any in-flight deferred idle is cancelled */
    detail_gen++;

    /* One-shot startup timing log */
    {
        static gboolean logged = FALSE;
        if (!logged) {
            logged = TRUE;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double uptime = (double)(now.tv_sec  - evemon_start_time.tv_sec) +
                            (double)(now.tv_nsec - evemon_start_time.tv_nsec) / 1e9;
            evemon_log(LOG_DEBUG, "[evemon] detail panel first updated %.3f s after startup", uptime);
        }
    }

    /* Only update if the detail panel area is toggled on by the user */
    if (!gtk_widget_get_visible(ctx->detail_vbox))
        return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    if (!sel) return;

    GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
    if (!rows) {
        /* No selection – hide the main (unpinned) detail panel */
        gtk_widget_hide(ctx->detail_panel);
        gtk_label_set_text(ctx->sb_pid,       "–");
        gtk_label_set_text(ctx->sb_ppid,      "–");
        gtk_label_set_text(ctx->sb_user,      "–");
        gtk_label_set_text(ctx->sb_name,      "–");
        gtk_label_set_text(ctx->sb_cpu,       "–");
        gtk_label_set_text(ctx->sb_rss,       "–");
        gtk_label_set_text(ctx->sb_group_rss, "–");
        gtk_label_set_text(ctx->sb_group_cpu, "–");
        gtk_label_set_text(ctx->sb_io_read,   "–");
        gtk_label_set_text(ctx->sb_io_write,  "–");
        gtk_label_set_text(ctx->sb_net_send,  "–");
        gtk_label_set_text(ctx->sb_net_recv,  "–");
        gtk_label_set_text(ctx->sb_start_time, "–");
        gtk_label_set_text(ctx->sb_container, "–");
        gtk_label_set_text(ctx->sb_service,   "–");
        gtk_label_set_text(ctx->sb_cwd,       "–");
        gtk_label_set_text(ctx->sb_cmdline,   "–");
        gtk_widget_hide(ctx->sb_steam_frame);
        gtk_widget_hide(ctx->sb_cgroup_frame);
        return;
    }

    /* ── Phase 1: reveal panel and paint all cheap labels ─────── */

    /* A process is selected – ensure the main detail panel is visible */
    if (gtk_widget_get_visible(ctx->detail_vbox) &&
        !gtk_widget_get_visible(ctx->detail_panel)) {
        gtk_widget_show_all(ctx->detail_panel);
        /* Restore proc info tray state after show_all */
        if (ctx->proc_info_collapsed) {
            gtk_revealer_set_reveal_child(
                GTK_REVEALER(ctx->proc_info_revealer), FALSE);
            gtk_widget_show(GTK_WIDGET(ctx->proc_info_summary));
        } else {
            gtk_widget_hide(GTK_WIDGET(ctx->proc_info_summary));
        }
    }

    GtkTreePath *path = rows->data;
    /* path is from the sort model (the view's model); sort → underlying store */
    GtkTreePath *store_path = gtk_tree_model_sort_convert_path_to_child_path(
        ctx->sort_model, path);
    GtkTreeModel *child_model = gtk_tree_model_sort_get_model(ctx->sort_model);
    GtkTreeIter iter;
    if (!store_path ||
        !gtk_tree_model_get_iter(child_model, &iter, store_path)) {
        if (store_path) gtk_tree_path_free(store_path);
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
        return;
    }
    gtk_tree_path_free(store_path);

    gint pid = 0, ppid = 0, cpu_raw = 0, rss = 0, grp_rss = 0, grp_cpu = 0;
    gint64 start_epoch = 0;
    gchar *user = NULL, *name = NULL, *cpu_text = NULL;
    gchar *rss_text = NULL, *grp_rss_text = NULL, *grp_cpu_text = NULL;
    gchar *io_read_text = NULL, *io_write_text = NULL;
    gchar *start_time_text = NULL, *container = NULL, *service = NULL,
          *cwd = NULL, *cmdline = NULL, *steam_label = NULL;

    gtk_tree_model_get(child_model, &iter,
                       COL_PID,            &pid,
                       COL_PPID,           &ppid,
                       COL_USER,           &user,
                       COL_NAME,           &name,
                       COL_CPU_TEXT,       &cpu_text,
                       COL_RSS_TEXT,       &rss_text,
                       COL_GROUP_RSS_TEXT, &grp_rss_text,
                       COL_GROUP_CPU_TEXT, &grp_cpu_text,
                       COL_IO_READ_RATE_TEXT,  &io_read_text,
                       COL_IO_WRITE_RATE_TEXT, &io_write_text,
                       COL_START_TIME,     &start_epoch,
                       COL_START_TIME_TEXT, &start_time_text,
                       COL_CONTAINER,      &container,
                       COL_SERVICE,        &service,
                       COL_CWD,           &cwd,
                       COL_CMDLINE,        &cmdline,
                       COL_STEAM_LABEL,    &steam_label,
                       -1);
    (void)cpu_raw; (void)rss; (void)grp_rss; (void)grp_cpu;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", pid);
    gtk_label_set_text(ctx->sb_pid, buf);

    snprintf(buf, sizeof(buf), "%d", ppid);
    gtk_label_set_text(ctx->sb_ppid, buf);

    gtk_label_set_text(ctx->sb_user,      user      ? user      : "–");
    gtk_label_set_text(ctx->sb_name,      name      ? name      : "–");
    gtk_label_set_text(ctx->sb_cpu,       cpu_text  ? cpu_text  : "–");
    gtk_label_set_text(ctx->sb_rss,       rss_text  ? rss_text  : "–");
    gtk_label_set_text(ctx->sb_group_rss, grp_rss_text ? grp_rss_text : "–");
    gtk_label_set_text(ctx->sb_group_cpu, grp_cpu_text ? grp_cpu_text : "–");
    gtk_label_set_text(ctx->sb_io_read,   io_read_text  ? io_read_text  : "–");
    gtk_label_set_text(ctx->sb_io_write,  io_write_text ? io_write_text : "–");

    /* Update the compact inline summary (shown when tray is collapsed) */
    {
        char summary[256];
        snprintf(summary, sizeof(summary), "%d  %s  %s  %s",
                 pid,
                 name      ? name      : "–",
                 cpu_text  ? cpu_text  : "–",
                 rss_text  ? rss_text  : "–");
        gtk_label_set_text(ctx->proc_info_summary, summary);
    }

    /* Per-PID network throughput from eBPF tcp_sendmsg/tcp_recvmsg */
    {
        fdmon_ctx_t *fdmon = ctx->mon ? ctx->mon->fdmon : NULL;
        uint64_t send_b = 0, recv_b = 0;
        if (fdmon && pid > 0)
            fdmon_net_io_get(fdmon, (pid_t)pid, &send_b, &recv_b);

        /* Format as rate (interval ≈ 2 s from monitor thread) */
        char net_buf[64];
        double interval = 2.0;
        #define FMT_NET_RATE(bytes) do { \
            double _r = (interval > 0.01) ? (double)(bytes) / interval \
                                          : (double)(bytes); \
            if (_r < 1.0) \
                snprintf(net_buf, sizeof(net_buf), "0 B/s"); \
            else if (_r < 1024.0) \
                snprintf(net_buf, sizeof(net_buf), "%.0f B/s", _r); \
            else if (_r < 1024.0 * 1024.0) \
                snprintf(net_buf, sizeof(net_buf), "%.1f KiB/s", \
                         _r / 1024.0); \
            else if (_r < 1024.0 * 1024.0 * 1024.0) \
                snprintf(net_buf, sizeof(net_buf), "%.1f MiB/s", \
                         _r / (1024.0 * 1024.0)); \
            else \
                snprintf(net_buf, sizeof(net_buf), "%.2f GiB/s", \
                         _r / (1024.0 * 1024.0 * 1024.0)); \
        } while (0)

        FMT_NET_RATE(send_b);
        gtk_label_set_text(ctx->sb_net_send, net_buf);
        FMT_NET_RATE(recv_b);
        gtk_label_set_text(ctx->sb_net_recv, net_buf);
        #undef FMT_NET_RATE
    }

    if (start_time_text && start_epoch > 0) {
        char fuzzy[64];
        format_fuzzy_time((time_t)start_epoch, fuzzy, sizeof(fuzzy));
        char combined[192];
        snprintf(combined, sizeof(combined), "%s (%s)", start_time_text, fuzzy);
        gtk_label_set_text(ctx->sb_start_time, combined);
    } else {
        gtk_label_set_text(ctx->sb_start_time, start_time_text ? start_time_text : "–");
    }
    gtk_label_set_text(ctx->sb_container, (container && container[0]) ? container : "–");
    gtk_label_set_text(ctx->sb_service,   (service && service[0])     ? service   : "–");
    gtk_label_set_text(ctx->sb_cwd,       cwd       ? cwd       : "–");
    gtk_label_set_text(ctx->sb_cmdline,   cmdline   ? cmdline   : "–");

    /* Hide Steam frame immediately; the deferred idle will re-show it.
     * The cgroup frame is left as-is: cgroup_scan_complete will hide or
     * update it once the async read finishes, avoiding a hide→show blink
     * on every refresh tick. */
    gtk_widget_hide(ctx->sb_steam_frame);

    /* ── Phase 2: enqueue deferred Steam + cgroup work ────────── */
    if (pid > 0) {
        detail_deferred_t *d = malloc(sizeof(*d));
        if (d) {
            d->ctx        = ctx;
            d->pid        = (pid_t)pid;
            d->name       = g_strdup(name);
            d->cmdline    = g_strdup(cmdline);
            d->steam_label = g_strdup(steam_label);
            d->generation = detail_gen;
            g_idle_add(detail_deferred_cb, d);
        }
    }

    g_free(user); g_free(name); g_free(cpu_text);
    g_free(rss_text); g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(io_read_text); g_free(io_write_text);
    g_free(start_time_text); g_free(container); g_free(service);
    g_free(cwd); g_free(cmdline); g_free(steam_label);

    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
}

/* ── pinned detail panels: full update ────────────────────────── */

/*
 * Walk the tree store to find a PID, recursing into children.
 */
static gboolean pinned_find_pid_recurse(GtkTreeModel *model,
                                        GtkTreeIter  *iter,
                                        pid_t         target,
                                        GtkTreeIter  *out)
{
    do {
        gint pid = 0;
        gtk_tree_model_get(model, iter, COL_PID, &pid, -1);
        if ((pid_t)pid == target) {
            *out = *iter;
            return TRUE;
        }
        GtkTreeIter child;
        if (gtk_tree_model_iter_children(model, &child, iter)) {
            if (pinned_find_pid_recurse(model, &child, target, out))
                return TRUE;
        }
    } while (gtk_tree_model_iter_next(model, iter));
    return FALSE;
}

/*
 * Update all pinned panels' labels, steam/cgroup info, and net throughput.
 * This mirrors proc_detail_update() but operates on each pinned panel
 * independently rather than on the main detail panel selection.
 */
void pinned_panels_update(ui_ctx_t *ctx)
{
    if (!ctx->pinned_panel_count) return;

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    for (size_t i = 0; i < ctx->pinned_panel_count; i++) {
        pinned_panel_t *pp = &ctx->pinned_panels[i];

        /* Find this PID in the store */
        GtkTreeIter iter, found;
        gboolean ok = FALSE;
        if (gtk_tree_model_get_iter_first(model, &iter))
            ok = pinned_find_pid_recurse(model, &iter, pp->pid, &found);

        if (!ok) {
            /* Process exited — show stale summary, clear labels */
            char summary[128];
            snprintf(summary, sizeof(summary),
                     "📌 PID %d (exited)", (int)pp->pid);
            gtk_label_set_text(pp->proc_info_summary, summary);
            gtk_label_set_text(pp->sb_pid, "–");
            gtk_label_set_text(pp->sb_name, "(exited)");
            gtk_label_set_text(pp->sb_cpu, "–");
            gtk_label_set_text(pp->sb_rss, "–");
            gtk_widget_hide(pp->sb_steam_frame);
            gtk_widget_hide(pp->sb_cgroup_frame);
            continue;
        }

        /* Extract all columns */
        gint pid = 0, ppid = 0;
        gint64 start_epoch = 0;
        gchar *user = NULL, *name = NULL, *cpu_text = NULL;
        gchar *rss_text = NULL, *grp_rss_text = NULL, *grp_cpu_text = NULL;
        gchar *io_read_text = NULL, *io_write_text = NULL;
        gchar *start_time_text = NULL, *container = NULL, *service = NULL,
              *cwd = NULL, *cmdline = NULL, *steam_label = NULL;

        gtk_tree_model_get(model, &found,
                           COL_PID,            &pid,
                           COL_PPID,           &ppid,
                           COL_USER,           &user,
                           COL_NAME,           &name,
                           COL_CPU_TEXT,       &cpu_text,
                           COL_RSS_TEXT,       &rss_text,
                           COL_GROUP_RSS_TEXT, &grp_rss_text,
                           COL_GROUP_CPU_TEXT, &grp_cpu_text,
                           COL_IO_READ_RATE_TEXT,  &io_read_text,
                           COL_IO_WRITE_RATE_TEXT, &io_write_text,
                           COL_START_TIME,     &start_epoch,
                           COL_START_TIME_TEXT, &start_time_text,
                           COL_CONTAINER,      &container,
                           COL_SERVICE,        &service,
                           COL_CWD,           &cwd,
                           COL_CMDLINE,        &cmdline,
                           COL_STEAM_LABEL,    &steam_label,
                           -1);

        /* PID / PPID */
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", pid);
        gtk_label_set_text(pp->sb_pid, buf);
        snprintf(buf, sizeof(buf), "%d", ppid);
        gtk_label_set_text(pp->sb_ppid, buf);

        /* Basic labels */
        gtk_label_set_text(pp->sb_user,      user      ? user      : "–");
        gtk_label_set_text(pp->sb_name,      name      ? name      : "–");
        gtk_label_set_text(pp->sb_cpu,       cpu_text  ? cpu_text  : "–");
        gtk_label_set_text(pp->sb_rss,       rss_text  ? rss_text  : "–");
        gtk_label_set_text(pp->sb_group_rss, grp_rss_text ? grp_rss_text : "–");
        gtk_label_set_text(pp->sb_group_cpu, grp_cpu_text ? grp_cpu_text : "–");
        gtk_label_set_text(pp->sb_io_read,   io_read_text  ? io_read_text  : "–");
        gtk_label_set_text(pp->sb_io_write,  io_write_text ? io_write_text : "–");

        /* Inline summary */
        {
            char summary[256];
            snprintf(summary, sizeof(summary), "📌 %d  %s  %s  %s",
                     pid,
                     name     ? name     : "–",
                     cpu_text ? cpu_text : "–",
                     rss_text ? rss_text : "–");
            gtk_label_set_text(pp->proc_info_summary, summary);
        }

        /* Per-PID network throughput from eBPF */
        {
            fdmon_ctx_t *fdmon = ctx->mon ? ctx->mon->fdmon : NULL;
            uint64_t send_b = 0, recv_b = 0;
            if (fdmon && pid > 0)
                fdmon_net_io_get(fdmon, (pid_t)pid, &send_b, &recv_b);

            char net_buf[64];
            double interval = 2.0;
            #define FMT_NET_RATE(bytes) do { \
                double _r = (interval > 0.01) ? (double)(bytes) / interval \
                                              : (double)(bytes); \
                if (_r < 1.0) \
                    snprintf(net_buf, sizeof(net_buf), "0 B/s"); \
                else if (_r < 1024.0) \
                    snprintf(net_buf, sizeof(net_buf), "%.0f B/s", _r); \
                else if (_r < 1024.0 * 1024.0) \
                    snprintf(net_buf, sizeof(net_buf), "%.1f KiB/s", \
                             _r / 1024.0); \
                else if (_r < 1024.0 * 1024.0 * 1024.0) \
                    snprintf(net_buf, sizeof(net_buf), "%.1f MiB/s", \
                             _r / (1024.0 * 1024.0)); \
                else \
                    snprintf(net_buf, sizeof(net_buf), "%.2f GiB/s", \
                             _r / (1024.0 * 1024.0 * 1024.0)); \
            } while (0)

            FMT_NET_RATE(send_b);
            gtk_label_set_text(pp->sb_net_send, net_buf);
            FMT_NET_RATE(recv_b);
            gtk_label_set_text(pp->sb_net_recv, net_buf);
            #undef FMT_NET_RATE
        }

        /* Start time */
        if (start_time_text && start_epoch > 0) {
            char fuzzy[64];
            format_fuzzy_time((time_t)start_epoch, fuzzy, sizeof(fuzzy));
            char combined[192];
            snprintf(combined, sizeof(combined), "%s (%s)",
                     start_time_text, fuzzy);
            gtk_label_set_text(pp->sb_start_time, combined);
        } else {
            gtk_label_set_text(pp->sb_start_time,
                               start_time_text ? start_time_text : "–");
        }

        gtk_label_set_text(pp->sb_container,
                           (container && container[0]) ? container : "–");
        gtk_label_set_text(pp->sb_service,
                           (service && service[0]) ? service : "–");
        gtk_label_set_text(pp->sb_cwd,     cwd     ? cwd     : "–");
        gtk_label_set_text(pp->sb_cmdline, cmdline ? cmdline : "–");

        /* ── Steam / Proton ────────────────────────────────────── */
        {
            gboolean is_steam = (steam_label && steam_label[0]);
            steam_info_t *si = NULL;

            if (is_steam && pid > 0) {
                si = steam_detect((pid_t)pid, name, cmdline, NULL);
                if (!si) {
                    /* Walk ancestors to find Steam env vars */
                    GtkTreeIter ancestor = found;
                    while (!si) {
                        GtkTreeIter parent_iter;
                        if (!gtk_tree_model_iter_parent(model, &parent_iter,
                                                        &ancestor))
                            break;
                        ancestor = parent_iter;
                        gint anc_pid = 0;
                        gchar *anc_name = NULL, *anc_cmd = NULL;
                        gtk_tree_model_get(model, &ancestor,
                                           COL_PID, &anc_pid,
                                           COL_NAME, &anc_name,
                                           COL_CMDLINE, &anc_cmd, -1);
                        steam_info_t *parent_si =
                            steam_detect((pid_t)anc_pid, anc_name,
                                         anc_cmd, NULL);
                        g_free(anc_name);
                        g_free(anc_cmd);
                        if (parent_si) {
                            si = steam_detect((pid_t)pid, name, cmdline,
                                              parent_si);
                            if (!si)
                                si = parent_si;
                            else
                                free(parent_si);
                        }
                    }
                }
            }

            if (si) {
                gtk_label_set_text(pp->sb_steam_game,
                    si->game_name[0]      ? si->game_name      : "–");
                gtk_label_set_text(pp->sb_steam_appid,
                    si->app_id[0]         ? si->app_id         : "–");
                gtk_label_set_text(pp->sb_steam_proton,
                    si->proton_version[0] ? si->proton_version : "–");
                gtk_label_set_text(pp->sb_steam_runtime,
                    si->runtime_layer[0]  ? si->runtime_layer  : "–");
                gtk_label_set_text(pp->sb_steam_compat,
                    si->compat_data[0]    ? si->compat_data    : "–");
                gtk_label_set_text(pp->sb_steam_gamedir,
                    si->game_dir[0]       ? si->game_dir       : "–");
                gtk_widget_set_tooltip_text(
                    GTK_WIDGET(pp->sb_steam_gamedir),
                    si->game_dir[0] ? si->game_dir : NULL);
                gtk_widget_set_tooltip_text(
                    GTK_WIDGET(pp->sb_steam_compat),
                    si->compat_data[0] ? si->compat_data : NULL);
                gtk_widget_set_no_show_all(pp->sb_steam_frame, FALSE);
                gtk_widget_show_all(pp->sb_steam_frame);
                gtk_widget_set_no_show_all(pp->sb_steam_frame, TRUE);
                free(si);
            } else {
                gtk_widget_hide(pp->sb_steam_frame);
            }
        }

        /* ── cgroup limits (synchronous — fast reads) ──────────── */
        if (pid > 0) {
            cgroup_label_set_t ls = {
                .path         = pp->sb_cgroup_path,
                .mem          = pp->sb_cgroup_mem,
                .mem_high     = pp->sb_cgroup_mem_high,
                .mem_high_key = pp->sb_cgroup_mem_high_key,
                .swap         = pp->sb_cgroup_swap,
                .swap_key     = pp->sb_cgroup_swap_key,
                .cpu          = pp->sb_cgroup_cpu,
                .pids         = pp->sb_cgroup_pids,
                .io           = pp->sb_cgroup_io,
                .io_key       = pp->sb_cgroup_io_key,
                .frame        = pp->sb_cgroup_frame,
            };
            cgroup_update_labels((pid_t)pid, &ls);
        }

        g_free(user); g_free(name); g_free(cpu_text);
        g_free(rss_text); g_free(grp_rss_text); g_free(grp_cpu_text);
        g_free(io_read_text); g_free(io_write_text);
        g_free(start_time_text); g_free(container); g_free(service);
        g_free(cwd); g_free(cmdline); g_free(steam_label);
    }
}
