/*
 * net_scan.c – dedicated network socket scanner for sidebar section.
 *
 * Enumerates network sockets for the selected process (and optionally
 * descendants), resolves addresses via /proc/net/{tcp,udp,tcp6,udp6},
 * queries per-connection throughput from the eBPF backend, and
 * displays results sorted by traffic (highest first).
 */

#include "ui_internal.h"
#include "../fdmon.h"

#include <unistd.h>

/* ── format helpers ──────────────────────────────────────────── */

static void format_rate(uint64_t bytes, double interval,
                        char *buf, size_t bufsz)
{
    double r = (interval > 0.01) ? (double)bytes / interval
                                 : (double)bytes;
    if (r < 1.0)
        snprintf(buf, bufsz, "0 B/s");
    else if (r < 1024.0)
        snprintf(buf, bufsz, "%.0f B/s", r);
    else if (r < 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f KiB/s", r / 1024.0);
    else if (r < 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f MiB/s", r / (1024.0 * 1024.0));
    else
        snprintf(buf, bufsz, "%.2f GiB/s", r / (1024.0 * 1024.0 * 1024.0));
}

/* ── net entry: one resolved socket with throughput ──────────── */

typedef struct {
    char     desc[512];       /* "TCP 1.2.3.4:80 → 5.6.7.8:443" */
    uint64_t send_delta;      /* bytes sent since last snapshot   */
    uint64_t recv_delta;      /* bytes received                   */
    uint64_t total;           /* send + recv (sort key)           */
} net_entry_t;

typedef struct {
    net_entry_t *entries;
    size_t       count;
    size_t       capacity;
} net_list_t;

static void net_list_init(net_list_t *l)
{
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void net_list_free(net_list_t *l)
{
    free(l->entries);
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static net_entry_t *net_list_push(net_list_t *l)
{
    if (l->count >= l->capacity) {
        size_t newcap = l->capacity ? l->capacity * 2 : 64;
        net_entry_t *tmp = realloc(l->entries, newcap * sizeof(net_entry_t));
        if (!tmp) return NULL;
        l->entries  = tmp;
        l->capacity = newcap;
    }
    memset(&l->entries[l->count], 0, sizeof(net_entry_t));
    return &l->entries[l->count++];
}

/* Sort descending by total traffic, then alphabetically. */
static int net_entry_cmp(const void *a, const void *b)
{
    const net_entry_t *ea = (const net_entry_t *)a;
    const net_entry_t *eb = (const net_entry_t *)b;
    if (eb->total > ea->total) return  1;
    if (eb->total < ea->total) return -1;
    return strcmp(ea->desc, eb->desc);
}

/* ── per-socket throughput matching ──────────────────────────── */

/*
 * Match a resolved socket (from /proc/net/tcp) against the per-socket
 * eBPF data (keyed by 4-tuple).
 *
 * For IPv4 sockets the match is straightforward: compare laddr:lport
 * with raddr:rport.  The BPF side stores lport in host order and
 * rport in network order (matching the kernel's sock_common layout).
 *
 * We get sock_info_t from the /proc/net parser, which stores ports
 * as host-order u16.  The rport from BPF is network order, so we
 * need to convert.
 */
static void match_sock_throughput(const sock_info_t *si,
                                  const fdmon_sock_io_t *socks,
                                  size_t sock_count,
                                  uint64_t *out_send, uint64_t *out_recv)
{
    *out_send = 0;
    *out_recv = 0;

    for (size_t i = 0; i < sock_count; i++) {
        const fdmon_sock_io_t *s = &socks[i];
        uint16_t bpf_rport_host = ntohs(s->rport);

        switch (si->kind) {
        case SOCK_KIND_TCP:
        case SOCK_KIND_UDP:
            if (si->local_addr  == s->laddr &&
                si->local_port  == s->lport &&
                si->remote_addr == s->raddr &&
                si->remote_port == bpf_rport_host) {
                *out_send = s->delta_send;
                *out_recv = s->delta_recv;
                return;
            }
            break;
        case SOCK_KIND_TCP6:
        case SOCK_KIND_UDP6: {
            /* Check if this is a v4-mapped v6 address */
            static const unsigned char mapped[12] =
                {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
            uint32_t la4, ra4;
            uint16_t lp, rp;
            if (memcmp(si->local_addr6, mapped, 12) == 0 &&
                memcmp(si->remote_addr6, mapped, 12) == 0) {
                memcpy(&la4, si->local_addr6 + 12, 4);
                memcpy(&ra4, si->remote_addr6 + 12, 4);
                lp = si->local_port6;
                rp = si->remote_port6;
                if (la4 == s->laddr && lp == s->lport &&
                    ra4 == s->raddr && rp == bpf_rport_host) {
                    *out_send = s->delta_send;
                    *out_recv = s->delta_recv;
                    return;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ── async scan task ─────────────────────────────────────────── */

typedef struct {
    pid_t          pid;
    pid_t         *desc_pids;
    size_t         desc_count;
    guint          generation;
    ui_ctx_t      *ctx;
    fdmon_ctx_t   *fdmon;
    net_list_t     results;
    /* aggregate per-PID totals for the header */
    uint64_t       total_send;
    uint64_t       total_recv;
} net_scan_task_t;

static void net_scan_task_free(net_scan_task_t *t)
{
    if (!t) return;
    net_list_free(&t->results);
    free(t->desc_pids);
    free(t);
}

/* Collect socket inodes from a single PID's /proc/<pid>/fd. */
static void collect_sock_inodes(pid_t pid,
                                unsigned long **inodes,
                                size_t *inode_count,
                                size_t *inode_cap)
{
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", (int)pid);
    DIR *dp = opendir(dirpath);
    if (!dp) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char link[288], target[512];
        snprintf(link, sizeof(link), "/proc/%d/fd/%s", (int)pid, de->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        if (strncmp(target, "socket:[", 8) != 0) continue;
        unsigned long ino = strtoul(target + 8, NULL, 10);
        if (ino == 0) continue;

        if (*inode_count >= *inode_cap) {
            *inode_cap = *inode_cap ? *inode_cap * 2 : 64;
            unsigned long *tmp = realloc(*inodes, *inode_cap * sizeof(**inodes));
            if (!tmp) continue;
            *inodes = tmp;
        }
        (*inodes)[(*inode_count)++] = ino;
    }
    closedir(dp);
}

static void net_scan_thread_func(GTask        *task,
                                 gpointer      source_object,
                                 gpointer      task_data,
                                 GCancellable *cancellable)
{
    (void)source_object;
    net_scan_task_t *t = task_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    /* 1. Enumerate the process's fds to find socket inodes */
    unsigned long *inodes = NULL;
    size_t inode_count = 0, inode_cap = 0;

    collect_sock_inodes(t->pid, &inodes, &inode_count, &inode_cap);

    /* Also collect from descendant PIDs */
    for (size_t d = 0; d < t->desc_count; d++) {
        if (g_cancellable_is_cancelled(cancellable)) {
            free(inodes);
            return;
        }
        collect_sock_inodes(t->desc_pids[d], &inodes, &inode_count, &inode_cap);
    }

    if (inode_count == 0) {
        free(inodes);
        net_list_init(&t->results);
        g_task_return_boolean(task, TRUE);
        return;
    }

    if (g_cancellable_is_cancelled(cancellable)) {
        free(inodes);
        return;
    }

    /* 2. Build the system socket table */
    sock_table_t socktbl;
    sock_table_build(&socktbl);

    /* 3. Get per-socket throughput from eBPF */
    fdmon_sock_io_t sock_io[1024];
    size_t sock_io_count = 1024;
    if (t->fdmon)
        fdmon_sock_io_list(t->fdmon, t->pid, sock_io, &sock_io_count);
    else
        sock_io_count = 0;

    /* Also collect per-socket throughput from descendant PIDs */
    for (size_t d = 0; d < t->desc_count && sock_io_count < 1024; d++) {
        fdmon_sock_io_t desc_io[256];
        size_t desc_count = 256;
        if (t->fdmon)
            fdmon_sock_io_list(t->fdmon, t->desc_pids[d],
                               desc_io, &desc_count);
        else
            desc_count = 0;
        for (size_t j = 0; j < desc_count && sock_io_count < 1024; j++)
            sock_io[sock_io_count++] = desc_io[j];
    }

    /* Get aggregate per-PID throughput */
    uint64_t pid_send = 0, pid_recv = 0;
    if (t->fdmon)
        fdmon_net_io_get(t->fdmon, t->pid, &pid_send, &pid_recv);
    /* Also sum descendant throughput */
    for (size_t d = 0; d < t->desc_count; d++) {
        uint64_t ds = 0, dr = 0;
        if (t->fdmon)
            fdmon_net_io_get(t->fdmon, t->desc_pids[d], &ds, &dr);
        pid_send += ds;
        pid_recv += dr;
    }
    t->total_send = pid_send;
    t->total_recv = pid_recv;

    net_list_init(&t->results);

    /* 4. For each socket inode, resolve and annotate */
    for (size_t i = 0; i < inode_count; i++) {
        if (g_cancellable_is_cancelled(cancellable)) break;

        /* Find in sock table */
        const sock_info_t *si = NULL;
        for (size_t j = 0; j < socktbl.count; j++) {
            if (socktbl.entries[j].inode == inodes[i]) {
                si = &socktbl.entries[j];
                break;
            }
        }
        if (!si) continue;

        /* Skip unix sockets – those stay in fd tree */
        if (si->kind == SOCK_KIND_UNIX) continue;

        /* Resolve to description string */
        char desc[512];
        switch (si->kind) {
        case SOCK_KIND_TCP:
        case SOCK_KIND_UDP: {
            const char *proto = (si->kind == SOCK_KIND_TCP) ? "TCP" : "UDP";
            char la[INET_ADDRSTRLEN], ra[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &si->local_addr, la, sizeof(la));
            inet_ntop(AF_INET, &si->remote_addr, ra, sizeof(ra));
            if (si->remote_addr == 0 && si->remote_port == 0)
                snprintf(desc, sizeof(desc), "%s %s:%u (listening)",
                         proto, la, si->local_port);
            else
                snprintf(desc, sizeof(desc), "%s %s:%u → %s:%u",
                         proto, la, si->local_port, ra, si->remote_port);
            break;
        }
        case SOCK_KIND_TCP6:
        case SOCK_KIND_UDP6: {
            const char *proto = (si->kind == SOCK_KIND_TCP6) ? "TCP6" : "UDP6";
            char la[INET6_ADDRSTRLEN], ra[INET6_ADDRSTRLEN];
            static const unsigned char mapped_pfx[12] =
                {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
            if (memcmp(si->local_addr6, mapped_pfx, 12) == 0)
                inet_ntop(AF_INET, si->local_addr6 + 12, la, sizeof(la));
            else
                inet_ntop(AF_INET6, si->local_addr6, la, sizeof(la));
            if (memcmp(si->remote_addr6, mapped_pfx, 12) == 0)
                inet_ntop(AF_INET, si->remote_addr6 + 12, ra, sizeof(ra));
            else
                inet_ntop(AF_INET6, si->remote_addr6, ra, sizeof(ra));

            static const unsigned char zeroes[16] = {0};
            if (memcmp(si->remote_addr6, zeroes, 16) == 0 && si->remote_port6 == 0)
                snprintf(desc, sizeof(desc), "%s [%s]:%u (listening)",
                         proto, la, si->local_port6);
            else
                snprintf(desc, sizeof(desc), "%s [%s]:%u → [%s]:%u",
                         proto, la, si->local_port6, ra, si->remote_port6);
            break;
        }
        default:
            snprintf(desc, sizeof(desc), "socket:[%lu]", inodes[i]);
            break;
        }

        /* Match per-socket throughput */
        uint64_t send_d = 0, recv_d = 0;
        match_sock_throughput(si, sock_io, sock_io_count,
                              &send_d, &recv_d);

        net_entry_t *ne = net_list_push(&t->results);
        if (!ne) continue;
        snprintf(ne->desc, sizeof(ne->desc), "%s", desc);
        ne->send_delta = send_d;
        ne->recv_delta = recv_d;
        ne->total      = send_d + recv_d;
    }

    free(inodes);
    sock_table_free(&socktbl);

    /* 5. Sort by traffic (descending) */
    if (t->results.count > 1)
        qsort(t->results.entries, t->results.count,
              sizeof(net_entry_t), net_entry_cmp);

    g_task_return_boolean(task, TRUE);
}

/* ── completion callback (runs on main thread) ───────────────── */

static void net_scan_complete(GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
    (void)source_object;
    ui_ctx_t *ctx = user_data;

    GTask *task = G_TASK(result);
    net_scan_task_t *t = g_task_get_task_data(task);

    if (!t || t->generation != ctx->net_generation)
        return;
    if (g_task_had_error(task))
        return;

    GtkTreeStore *store = ctx->net_store;
    GtkTreeModel *model = GTK_TREE_MODEL(store);

    /* Build the header text with aggregate throughput */
    char hdr[256];
    if (t->total_send > 0 || t->total_recv > 0) {
        char sbuf[32], rbuf[32];
        format_rate(t->total_send, 2.0, sbuf, sizeof(sbuf));
        format_rate(t->total_recv, 2.0, rbuf, sizeof(rbuf));
        snprintf(hdr, sizeof(hdr), "%zu connection%s  ↑%s  ↓%s",
                 t->results.count,
                 t->results.count == 1 ? "" : "s",
                 sbuf, rbuf);
    } else {
        snprintf(hdr, sizeof(hdr), "%zu connection%s",
                 t->results.count,
                 t->results.count == 1 ? "" : "s");
    }

    /* Update or create the header row */
    GtkTreeIter top;
    (void)top;  /* available for future header-row use */

    if (t->results.count == 0) {
        /* No network sockets — show a single "no connections" row */
        gtk_tree_store_clear(store);
        gtk_tree_store_append(store, &top, NULL);
        gtk_tree_store_set(store, &top,
                           NET_COL_TEXT, "No network sockets",
                           NET_COL_MARKUP, "<i>No network sockets</i>",
                           NET_COL_SORT_KEY, (gint64)0, -1);
        return;
    }

    /* Single-level list (no categories) — update in place */
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, NULL);
    size_t i = 0;

    while (i < t->results.count && valid) {
        const net_entry_t *ne = &t->results.entries[i];

        /* Build display text with throughput annotation */
        char display[640];
        if (ne->total > 0) {
            char sbuf[32], rbuf[32];
            format_rate(ne->send_delta, 2.0, sbuf, sizeof(sbuf));
            format_rate(ne->recv_delta, 2.0, rbuf, sizeof(rbuf));
            snprintf(display, sizeof(display), "%s  ↑%s ↓%s",
                     ne->desc, sbuf, rbuf);
        } else {
            snprintf(display, sizeof(display), "%s", ne->desc);
        }

        char *markup;
        if (ne->total > 0) {
            /* Highlight active connections */
            char *esc_desc = g_markup_escape_text(ne->desc, -1);
            char sbuf[32], rbuf[32];
            format_rate(ne->send_delta, 2.0, sbuf, sizeof(sbuf));
            format_rate(ne->recv_delta, 2.0, rbuf, sizeof(rbuf));
            markup = g_strdup_printf(
                "%s  <span foreground=\"#4488ff\">↑%s</span> "
                "<span foreground=\"#44cc44\">↓%s</span>",
                esc_desc, sbuf, rbuf);
            g_free(esc_desc);
        } else {
            markup = g_markup_escape_text(display, -1);
        }

        gtk_tree_store_set(store, &iter,
                           NET_COL_TEXT, display,
                           NET_COL_MARKUP, markup,
                           NET_COL_SORT_KEY, (gint64)ne->total, -1);
        g_free(markup);
        i++;
        valid = gtk_tree_model_iter_next(model, &iter);
    }

    /* Append remaining new entries */
    while (i < t->results.count) {
        const net_entry_t *ne = &t->results.entries[i];
        char display[640];
        char *markup;

        if (ne->total > 0) {
            char sbuf[32], rbuf[32];
            format_rate(ne->send_delta, 2.0, sbuf, sizeof(sbuf));
            format_rate(ne->recv_delta, 2.0, rbuf, sizeof(rbuf));
            snprintf(display, sizeof(display), "%s  ↑%s ↓%s",
                     ne->desc, sbuf, rbuf);
            char *esc_desc = g_markup_escape_text(ne->desc, -1);
            markup = g_strdup_printf(
                "%s  <span foreground=\"#4488ff\">↑%s</span> "
                "<span foreground=\"#44cc44\">↓%s</span>",
                esc_desc, sbuf, rbuf);
            g_free(esc_desc);
        } else {
            snprintf(display, sizeof(display), "%s", ne->desc);
            markup = g_markup_escape_text(display, -1);
        }

        GtkTreeIter new_iter;
        gtk_tree_store_append(store, &new_iter, NULL);
        gtk_tree_store_set(store, &new_iter,
                           NET_COL_TEXT, display,
                           NET_COL_MARKUP, markup,
                           NET_COL_SORT_KEY, (gint64)ne->total, -1);
        g_free(markup);
        i++;
    }

    /* Remove excess old rows */
    while (valid) {
        valid = gtk_tree_store_remove(store, &iter);
    }
}

/* ── public API ──────────────────────────────────────────────── */

void net_scan_start(ui_ctx_t *ctx, pid_t pid)
{
    if (ctx->net_cancel) {
        g_cancellable_cancel(ctx->net_cancel);
        g_object_unref(ctx->net_cancel);
    }
    ctx->net_cancel = g_cancellable_new();
    ctx->net_generation++;

    if (pid != ctx->net_last_pid)
        ctx->net_last_pid = pid;

    net_scan_task_t *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->pid        = pid;
    t->generation = ctx->net_generation;
    t->ctx        = ctx;
    t->fdmon      = ctx->mon ? ctx->mon->fdmon : NULL;

    if (ctx->net_include_desc) {
        GtkTreeIter net_iter;
        if (find_iter_by_pid(GTK_TREE_MODEL(ctx->store), NULL,
                             pid, &net_iter)) {
            size_t cap = 0;
            collect_descendant_pids(GTK_TREE_MODEL(ctx->store),
                                    &net_iter,
                                    &t->desc_pids, &t->desc_count, &cap);
        }
    }

    GTask *task = g_task_new(NULL, ctx->net_cancel, net_scan_complete, ctx);
    g_task_set_task_data(task, t, (GDestroyNotify)net_scan_task_free);
    g_task_run_in_thread(task, net_scan_thread_func);
    g_object_unref(task);
}

/* ── tree view signal callbacks ──────────────────────────────── */

void on_net_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data)
{
    (void)view; (void)iter; (void)path; (void)data;
}

void on_net_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data)
{
    (void)view; (void)iter; (void)path; (void)data;
}

gboolean on_net_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)widget; (void)ev; (void)data;
    return FALSE;
}

void on_net_desc_toggled(GtkToggleButton *btn, gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->net_include_desc = gtk_toggle_button_get_active(btn);
    gtk_tree_store_clear(ctx->net_store);
    sidebar_update(ctx);
}
