/*
 * fd_scan.c – file-descriptor enumeration, classification, socket
 *             resolution, and async GTask-based background scanning.
 */

#include "ui_internal.h"
#include "../fdmon.h"

#include <unistd.h>

/* ── fd_list helpers ─────────────────────────────────────────── */

void fd_list_init(fd_list_t *l)
{
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

void fd_list_free(fd_list_t *l)
{
    free(l->entries);
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

void fd_list_push(fd_list_t *l, int fd, const char *path)
{
    if (l->count >= l->capacity) {
        size_t newcap = l->capacity ? l->capacity * 2 : 64;
        fd_entry_t *tmp = realloc(l->entries, newcap * sizeof(fd_entry_t));
        if (!tmp) return;   /* OOM – drop this fd entry */
        l->entries  = tmp;
        l->capacity = newcap;
    }
    l->entries[l->count].fd = fd;
    l->entries[l->count].net_sort_key = 0;
    snprintf(l->entries[l->count].path, sizeof(l->entries[0].path), "%s", path);
    l->count++;
}

/* ── category labels ─────────────────────────────────────────── */

const char *fd_cat_label[FD_CAT_COUNT] = {
    [FD_CAT_FILES]         = "Files",
    [FD_CAT_DEVICES]       = "Devices",
    [FD_CAT_NET_SOCKETS]   = "Network Sockets",
    [FD_CAT_UNIX_SOCKETS]  = "Unix Sockets",
    [FD_CAT_OTHER_SOCKETS] = "Other Sockets",
    [FD_CAT_PIPES]         = "Pipes",
    [FD_CAT_EVENTS]        = "Event/Signaling",
    [FD_CAT_OTHER]         = "Other",
};

/* ── socket table ────────────────────────────────────────────── */

static void sock_table_init(sock_table_t *t)
{
    t->entries  = NULL;
    t->count    = 0;
    t->capacity = 0;
}

void sock_table_free(sock_table_t *t)
{
    free(t->entries);
    t->entries  = NULL;
    t->count    = 0;
    t->capacity = 0;
}

static sock_info_t *sock_table_push(sock_table_t *t)
{
    if (t->count >= t->capacity) {
        size_t newcap = t->capacity ? t->capacity * 2 : 256;
        sock_info_t *tmp = realloc(t->entries, newcap * sizeof(sock_info_t));
        if (!tmp) return NULL;
        t->entries  = tmp;
        t->capacity = newcap;
    }
    memset(&t->entries[t->count], 0, sizeof(sock_info_t));
    return &t->entries[t->count++];
}

static const sock_info_t *sock_table_find(const sock_table_t *t,
                                           unsigned long inode)
{
    for (size_t i = 0; i < t->count; i++)
        if (t->entries[i].inode == inode) return &t->entries[i];
    return NULL;
}

/* ── /proc/net parsers ───────────────────────────────────────── */

static void parse_proc_net_inet(const char *path, sock_kind_t kind,
                                sock_table_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        unsigned long inode = 0;
        unsigned local_addr = 0, remote_addr = 0;
        unsigned local_port = 0, remote_port = 0;
        unsigned st;
        int n = sscanf(line,
            " %*d: %X:%X %X:%X %X %*X:%*X %*X:%*X %*X %*u %*u %lu",
            &local_addr, &local_port,
            &remote_addr, &remote_port,
            &st, &inode);
        (void)st;
        if (n >= 6 && inode > 0) {
            sock_info_t *s = sock_table_push(out);
            if (!s) continue;
            s->inode       = inode;
            s->kind        = kind;
            s->local_addr  = local_addr;
            s->local_port  = (uint16_t)local_port;
            s->remote_addr = remote_addr;
            s->remote_port = (uint16_t)remote_port;
        }
    }
    fclose(f);
}

static void parse_proc_net_inet6(const char *path, sock_kind_t kind,
                                 sock_table_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        char local_hex[33] = "", remote_hex[33] = "";
        unsigned local_port = 0, remote_port = 0;
        unsigned st;
        unsigned long inode = 0;
        int n = sscanf(line,
            " %*d: %32[0-9A-Fa-f]:%X %32[0-9A-Fa-f]:%X %X "
            "%*X:%*X %*X:%*X %*X %*u %*u %lu",
            local_hex, &local_port,
            remote_hex, &remote_port,
            &st, &inode);
        (void)st;
        if (n >= 6 && inode > 0) {
            sock_info_t *s = sock_table_push(out);
            if (!s) continue;
            s->inode       = inode;
            s->kind        = kind;
            s->local_port6 = (uint16_t)local_port;
            s->remote_port6= (uint16_t)remote_port;
            for (int w = 0; w < 4; w++) {
                uint32_t v;
                sscanf(local_hex + w * 8, "%8x", &v);
                memcpy(s->local_addr6 + w * 4, &v, 4);
                sscanf(remote_hex + w * 8, "%8x", &v);
                memcpy(s->remote_addr6 + w * 4, &v, 4);
            }
        }
    }
    fclose(f);
}

static void parse_proc_net_unix(sock_table_t *out)
{
    FILE *f = fopen("/proc/net/unix", "r");
    if (!f) return;

    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        unsigned long inode = 0;
        unsigned type = 0;
        char path[256] = "";
        int n = sscanf(line, "%*p: %*X %*X %*X %X %*X %lu %255[^\n]",
                       &type, &inode, path);
        if (n >= 2 && inode > 0) {
            sock_info_t *s = sock_table_push(out);
            if (!s) continue;
            s->inode     = inode;
            s->kind      = SOCK_KIND_UNIX;
            s->unix_type = (int)type;
            char *p = path;
            while (*p == ' ') p++;
            snprintf(s->unix_path, sizeof(s->unix_path), "%s", p);
        }
    }
    fclose(f);
}

void sock_table_build(sock_table_t *out)
{
    sock_table_init(out);
    parse_proc_net_inet("/proc/net/tcp",  SOCK_KIND_TCP,  out);
    parse_proc_net_inet("/proc/net/udp",  SOCK_KIND_UDP,  out);
    parse_proc_net_inet6("/proc/net/tcp6", SOCK_KIND_TCP6, out);
    parse_proc_net_inet6("/proc/net/udp6", SOCK_KIND_UDP6, out);
    parse_proc_net_unix(out);
}

/* ── IP formatting ───────────────────────────────────────────── */

static void format_ipv4(uint32_t addr, char *buf, size_t bufsz)
{
    inet_ntop(AF_INET, &addr, buf, (socklen_t)bufsz);
}

static void format_ipv6(const unsigned char *addr, char *buf, size_t bufsz)
{
    static const unsigned char mapped_prefix[12] =
        {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
    if (memcmp(addr, mapped_prefix, 12) == 0) {
        inet_ntop(AF_INET, addr + 12, buf, (socklen_t)bufsz);
    } else {
        inet_ntop(AF_INET6, addr, buf, (socklen_t)bufsz);
    }
}

/* ── socket resolution ───────────────────────────────────────── */

fd_category_t resolve_socket(unsigned long inode,
                              const sock_table_t *tbl,
                              char *desc, size_t descsz)
{
    const sock_info_t *s = sock_table_find(tbl, inode);
    if (!s) {
        snprintf(desc, descsz, "socket:[%lu]", inode);
        return FD_CAT_OTHER_SOCKETS;
    }

    switch (s->kind) {
    case SOCK_KIND_TCP:
    case SOCK_KIND_UDP: {
        const char *proto = (s->kind == SOCK_KIND_TCP) ? "TCP" : "UDP";
        char laddr[INET_ADDRSTRLEN], raddr[INET_ADDRSTRLEN];
        format_ipv4(s->local_addr,  laddr, sizeof(laddr));
        format_ipv4(s->remote_addr, raddr, sizeof(raddr));
        if (s->remote_addr == 0 && s->remote_port == 0)
            snprintf(desc, descsz, "%s %s:%u (listening)",
                     proto, laddr, s->local_port);
        else
            snprintf(desc, descsz, "%s %s:%u → %s:%u",
                     proto, laddr, s->local_port,
                     raddr, s->remote_port);
        return FD_CAT_NET_SOCKETS;
    }
    case SOCK_KIND_TCP6:
    case SOCK_KIND_UDP6: {
        const char *proto = (s->kind == SOCK_KIND_TCP6) ? "TCP6" : "UDP6";
        char laddr[INET6_ADDRSTRLEN], raddr[INET6_ADDRSTRLEN];
        format_ipv6(s->local_addr6,  laddr, sizeof(laddr));
        format_ipv6(s->remote_addr6, raddr, sizeof(raddr));
        static const unsigned char zeroes[16] = {0};
        if (memcmp(s->remote_addr6, zeroes, 16) == 0 && s->remote_port6 == 0)
            snprintf(desc, descsz, "%s [%s]:%u (listening)",
                     proto, laddr, s->local_port6);
        else
            snprintf(desc, descsz, "%s [%s]:%u → [%s]:%u",
                     proto, laddr, s->local_port6,
                     raddr, s->remote_port6);
        return FD_CAT_NET_SOCKETS;
    }
    case SOCK_KIND_UNIX: {
        const char *type_str = (s->unix_type == 1) ? "stream" :
                               (s->unix_type == 2) ? "dgram"  : "";
        if (s->unix_path[0])
            snprintf(desc, descsz, "unix %s %s", type_str, s->unix_path);
        else
            snprintf(desc, descsz, "unix %s (unnamed inode %lu)",
                     type_str, inode);
        return FD_CAT_UNIX_SOCKETS;
    }
    default:
        snprintf(desc, descsz, "socket:[%lu]", inode);
        return FD_CAT_OTHER_SOCKETS;
    }
}

/* ── fd classification ───────────────────────────────────────── */

fd_category_t classify_fd(const char *path)
{
    if (strncmp(path, "socket:", 7) == 0)
        return FD_CAT_OTHER_SOCKETS;
    if (strncmp(path, "pipe:",   5) == 0)
        return FD_CAT_PIPES;
    if (strncmp(path, "anon_inode:", 11) == 0) {
        const char *name = path + 11;
        if (strstr(name, "eventfd")   ||
            strstr(name, "eventpoll") ||
            strstr(name, "signalfd")  ||
            strstr(name, "timerfd"))
            return FD_CAT_EVENTS;
        return FD_CAT_OTHER;
    }
    if (strncmp(path, "/dev/", 5) == 0)
        return FD_CAT_DEVICES;
    if (path[0] == '/')
        return FD_CAT_FILES;
    return FD_CAT_OTHER;
}

/* ── whitespace-trimmed comparison for dedup ─────────────────── */

/*
 * Compare two strings ignoring leading and trailing whitespace.
 * Returns 0 if the trimmed content is identical, non-zero otherwise.
 */
int strcmp_trimmed(const char *a, const char *b)
{
    /* Skip leading whitespace */
    while (*a && (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r')) a++;
    while (*b && (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r')) b++;

    /* Find end of non-whitespace content */
    const char *ea = a + strlen(a);
    while (ea > a && (ea[-1] == ' ' || ea[-1] == '\t' || ea[-1] == '\n' || ea[-1] == '\r')) ea--;

    const char *eb = b + strlen(b);
    while (eb > b && (eb[-1] == ' ' || eb[-1] == '\t' || eb[-1] == '\n' || eb[-1] == '\r')) eb--;

    size_t la = (size_t)(ea - a);
    size_t lb = (size_t)(eb - b);
    if (la != lb) return 1;
    return memcmp(a, b, la);
}

/* ── markup helper: render whitespace as grayed-out symbols ──── */

/*
 * Convert a path string to Pango markup.  Visible content is escaped
 * normally; leading/trailing whitespace is rendered as visible symbols
 * in a gray color:  '·' for space, '→' for tab, '↵' for newline/CR.
 */
char *fd_path_to_markup(const char *path)
{
    if (!path || !path[0])
        return g_strdup("–");

    const char *start = path;
    const char *end   = path + strlen(path);

    /* Find the trimmed core region */
    const char *core_start = start;
    while (core_start < end &&
           (*core_start == ' ' || *core_start == '\t' ||
            *core_start == '\n' || *core_start == '\r'))
        core_start++;

    const char *core_end = end;
    while (core_end > core_start &&
           (core_end[-1] == ' ' || core_end[-1] == '\t' ||
            core_end[-1] == '\n' || core_end[-1] == '\r'))
        core_end--;

    /* If no whitespace at edges, just escape the whole thing */
    if (core_start == start && core_end == end) {
        char *escaped = g_markup_escape_text(path, -1);
        return escaped;
    }

    GString *out = g_string_new(NULL);

    /* Render leading whitespace as grayed symbols */
    if (core_start > start) {
        g_string_append(out, "<span foreground=\"#888888\">");
        for (const char *p = start; p < core_start; p++) {
            switch (*p) {
            case ' ':  g_string_append(out, "·");  break;
            case '\t': g_string_append(out, "→");  break;
            case '\n': g_string_append(out, "↵");  break;
            case '\r': g_string_append(out, "↵");  break;
            default: {
                char *esc = g_markup_escape_text(p, 1);
                g_string_append(out, esc);
                g_free(esc);
                break;
            }
            }
        }
        g_string_append(out, "</span>");
    }

    /* Render the core content normally (escaped) */
    if (core_end > core_start) {
        char *escaped = g_markup_escape_text(core_start,
                                             (gssize)(core_end - core_start));
        g_string_append(out, escaped);
        g_free(escaped);
    }

    /* Render trailing whitespace as grayed symbols */
    if (core_end < end) {
        g_string_append(out, "<span foreground=\"#888888\">");
        for (const char *p = core_end; p < end; p++) {
            switch (*p) {
            case ' ':  g_string_append(out, "·");  break;
            case '\t': g_string_append(out, "→");  break;
            case '\n': g_string_append(out, "↵");  break;
            case '\r': g_string_append(out, "↵");  break;
            default: {
                char *esc = g_markup_escape_text(p, 1);
                g_string_append(out, esc);
                g_free(esc);
                break;
            }
            }
        }
        g_string_append(out, "</span>");
    }

    return g_string_free(out, FALSE);
}

/* ── fd path sorting ─────────────────────────────────────────── */

static const char *get_home_prefix(size_t *len_out)
{
    static char prefix[256] = "";
    static size_t plen = 0;
    static int inited = 0;
    if (!inited) {
        const char *user = getenv("USER");
        if (user && user[0])
            plen = (size_t)snprintf(prefix, sizeof(prefix),
                                    "/home/%s/", user);
        inited = 1;
    }
    *len_out = plen;
    return prefix;
}

static int fd_path_tier(const char *path)
{
    size_t hlen;
    const char *hpfx = get_home_prefix(&hlen);
    if (hlen > 0 && strncmp(path, hpfx, hlen) == 0)
        return 0;
    if (path[0] == '/') {
        const char *colon = strchr(path + 1, ':');
        const char *slash = strchr(path + 1, '/');
        if (colon && (!slash || colon < slash))
            return 2;
        return 1;
    }
    return 2;
}

int fd_entry_path_cmp(const void *a, const void *b)
{
    const char *pa = ((const fd_entry_t *)a)->path;
    const char *pb = ((const fd_entry_t *)b)->path;

    int ta = fd_path_tier(pa);
    int tb = fd_path_tier(pb);
    if (ta != tb)
        return ta - tb;

    return strcmp(pa, pb);
}

/* ── /proc fd reading ────────────────────────────────────────── */

void read_pid_fds(pid_t pid, fd_list_t *out)
{
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", (int)pid);

    DIR *dp = opendir(dirpath);
    if (!dp) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        int fd = atoi(de->d_name);

        char link[288];
        snprintf(link, sizeof(link), "/proc/%d/fd/%s", (int)pid, de->d_name);

        char target[512] = "";
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n > 0) target[n] = '\0';
        else       snprintf(target, sizeof(target), "(unreadable)");

        fd_list_push(out, fd, target);
    }
    closedir(dp);
}

/* ── async fd scan infrastructure ────────────────────────────── */

void collect_descendant_pids(GtkTreeModel *model, GtkTreeIter *parent,
                                    pid_t **out, size_t *out_count, size_t *out_cap)
{
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(model, &child, parent);
    while (valid) {
        gint pid;
        gtk_tree_model_get(model, &child, COL_PID, &pid, -1);
        if (*out_count >= *out_cap) {
            *out_cap = *out_cap ? *out_cap * 2 : 64;
            pid_t *tmp = realloc(*out, *out_cap * sizeof(pid_t));
            if (!tmp) return;
            *out = tmp;
        }
        (*out)[(*out_count)++] = (pid_t)pid;
        collect_descendant_pids(model, &child, out, out_count, out_cap);
        valid = gtk_tree_model_iter_next(model, &child);
    }
}

typedef struct {
    pid_t    pid;
    pid_t   *desc_pids;
    size_t   desc_count;
    guint    generation;
    fd_list_t buckets[FD_CAT_COUNT];
    ui_ctx_t *ctx;
    fdmon_ctx_t *fdmon;        /* for per-PID network throughput */
    uint64_t net_send_bytes;   /* delta send bytes this snapshot */
    uint64_t net_recv_bytes;   /* delta recv bytes this snapshot */
} fd_scan_task_t;

static void fd_scan_task_free(fd_scan_task_t *t)
{
    if (!t) return;
    free(t->desc_pids);
    for (int c = 0; c < FD_CAT_COUNT; c++)
        fd_list_free(&t->buckets[c]);
    free(t);
}

/* Sort network socket entries by traffic (highest first). */
static int fd_entry_net_sort_cmp(const void *a, const void *b)
{
    const fd_entry_t *ea = (const fd_entry_t *)a;
    const fd_entry_t *eb = (const fd_entry_t *)b;
    /* Descending by net_sort_key */
    if (eb->net_sort_key > ea->net_sort_key) return  1;
    if (eb->net_sort_key < ea->net_sort_key) return -1;
    /* Fall back to path comparison for stability */
    return strcmp(ea->path, eb->path);
}

static void fd_scan_thread_func(GTask        *task,
                                gpointer      source_object,
                                gpointer      task_data,
                                GCancellable *cancellable)
{
    (void)source_object;
    fd_scan_task_t *t = task_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    fd_list_t fds;
    fd_list_init(&fds);
    read_pid_fds(t->pid, &fds);

    for (size_t i = 0; i < t->desc_count; i++) {
        if (g_cancellable_is_cancelled(cancellable)) {
            fd_list_free(&fds);
            return;
        }
        read_pid_fds(t->desc_pids[i], &fds);
    }

    if (g_cancellable_is_cancelled(cancellable)) {
        fd_list_free(&fds);
        return;
    }

    sock_table_t socktbl;
    sock_table_build(&socktbl);

    for (int c = 0; c < FD_CAT_COUNT; c++)
        fd_list_init(&t->buckets[c]);

    for (size_t i = 0; i < fds.count; i++) {
        if (g_cancellable_is_cancelled(cancellable))
            break;

        fd_category_t cat = classify_fd(fds.entries[i].path);
        if (cat == FD_CAT_OTHER_SOCKETS &&
            strncmp(fds.entries[i].path, "socket:[", 8) == 0) {
            unsigned long inode = strtoul(
                fds.entries[i].path + 8, NULL, 10);
            char desc[512];
            cat = resolve_socket(inode, &socktbl, desc, sizeof(desc));
            fd_list_push(&t->buckets[cat], fds.entries[i].fd, desc);
        } else if (cat == FD_CAT_DEVICES) {
            char desc[512];
            label_device(fds.entries[i].path, desc, sizeof(desc));
            fd_list_push(&t->buckets[cat], fds.entries[i].fd, desc);
        } else {
            fd_list_push(&t->buckets[cat], fds.entries[i].fd,
                         fds.entries[i].path);
        }
    }
    sock_table_free(&socktbl);
    fd_list_free(&fds);

    /* Query per-PID network throughput from eBPF (if available) and
     * annotate all network socket entries with ↑/↓ rate strings.
     * The per-PID counters give us aggregate send/recv for the whole
     * process, not per-socket, so we store the total on every socket
     * entry and use it as the sort key.                             */
    {
        uint64_t send_b = 0, recv_b = 0;
        if (t->fdmon)
            fdmon_net_io_get(t->fdmon, t->pid, &send_b, &recv_b);
        t->net_send_bytes = send_b;
        t->net_recv_bytes = recv_b;

        uint64_t total_net = send_b + recv_b;
        /* Stamp the sort key on every network socket entry */
        for (size_t i = 0; i < t->buckets[FD_CAT_NET_SOCKETS].count; i++)
            t->buckets[FD_CAT_NET_SOCKETS].entries[i].net_sort_key = total_net;
    }

    for (int c = 0; c < FD_CAT_COUNT; c++) {
        if (t->buckets[c].count > 1) {
            if (c == FD_CAT_NET_SOCKETS)
                qsort(t->buckets[c].entries, t->buckets[c].count,
                      sizeof(fd_entry_t), fd_entry_net_sort_cmp);
            else
                qsort(t->buckets[c].entries, t->buckets[c].count,
                      sizeof(fd_entry_t), fd_entry_path_cmp);
        }
    }

    g_task_return_boolean(task, TRUE);
}

static void fd_scan_complete(GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
    (void)source_object;
    ui_ctx_t *ctx = user_data;

    GTask *task = G_TASK(result);
    fd_scan_task_t *t = g_task_get_task_data(task);

    if (!t || t->generation != ctx->fd_generation)
        return;

    if (g_task_had_error(task))
        return;

    GtkAdjustment *fd_vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(gtk_widget_get_parent(
            GTK_WIDGET(ctx->fd_view))));
    double fd_scroll_pos = gtk_adjustment_get_value(fd_vadj);

    GtkTreeModel *fd_model = GTK_TREE_MODEL(ctx->fd_store);

    GtkTreeIter cat_iters[FD_CAT_COUNT];
    gboolean    cat_exists[FD_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(fd_model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(fd_model, &top, FD_COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < FD_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(fd_model, &top);
        }
    }

    for (int c = 0; c < FD_CAT_COUNT; c++) {
        if (cat_exists[c] && t->buckets[c].count == 0) {
            gtk_tree_store_remove(ctx->fd_store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    for (int c = 0; c < FD_CAT_COUNT; c++) {
        if (t->buckets[c].count == 0) continue;

        /* Network sockets are shown in their own sidebar section now */
        if (c == FD_CAT_NET_SOCKETS) continue;

        char hdr[256];
        snprintf(hdr, sizeof(hdr), "%s (%zu)",
                 fd_cat_label[c], t->buckets[c].count);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_append(ctx->fd_store, &parent, NULL);
            gtk_tree_store_set(ctx->fd_store, &parent,
                               FD_COL_TEXT, hdr,
                               FD_COL_MARKUP, hdr_escaped,
                               FD_COL_CAT, (gint)c, -1);
            g_free(hdr_escaped);
            cat_exists[c] = TRUE;
            cat_iters[c]  = parent;
        } else {
            parent = cat_iters[c];
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_set(ctx->fd_store, &parent,
                               FD_COL_TEXT, hdr,
                               FD_COL_MARKUP, hdr_escaped, -1);
            g_free(hdr_escaped);
        }

        /* Build the display list: optionally group duplicates */
        fd_list_t display;
        fd_list_init(&display);

        if (ctx->fd_group_dup_active && t->buckets[c].count > 0) {
            /* Buckets are already sorted by path, so duplicates are adjacent.
             * Use strcmp_trimmed so paths differing only in leading/trailing
             * whitespace are grouped together.                              */
            size_t run_start = 0;
            while (run_start < t->buckets[c].count) {
                size_t run_end = run_start + 1;
                while (run_end < t->buckets[c].count &&
                       strcmp_trimmed(t->buckets[c].entries[run_start].path,
                                      t->buckets[c].entries[run_end].path) == 0)
                    run_end++;
                size_t run_len = run_end - run_start;
                if (run_len > 1) {
                    char grouped[600];
                    snprintf(grouped, sizeof(grouped), "%s (%zu duplicates)",
                             t->buckets[c].entries[run_start].path, run_len);
                    fd_list_push(&display, t->buckets[c].entries[run_start].fd,
                                 grouped);
                } else {
                    for (size_t j = run_start; j < run_end; j++)
                        fd_list_push(&display, t->buckets[c].entries[j].fd,
                                     t->buckets[c].entries[j].path);
                }
                run_start = run_end;
            }
        } else {
            for (size_t j = 0; j < t->buckets[c].count; j++)
                fd_list_push(&display, t->buckets[c].entries[j].fd,
                             t->buckets[c].entries[j].path);
        }

        /* Update the category header with the true fd count */
        {
            char hdr2[128];
            if (ctx->fd_group_dup_active && display.count != t->buckets[c].count)
                snprintf(hdr2, sizeof(hdr2), "%s (%zu, %zu unique)",
                         fd_cat_label[c], t->buckets[c].count, display.count);
            else
                snprintf(hdr2, sizeof(hdr2), "%s (%zu)",
                         fd_cat_label[c], t->buckets[c].count);
            char *hdr2_escaped = g_markup_escape_text(hdr2, -1);
            gtk_tree_store_set(ctx->fd_store, &parent,
                               FD_COL_TEXT, hdr2,
                               FD_COL_MARKUP, hdr2_escaped, -1);
            g_free(hdr2_escaped);
        }

        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            fd_model, &child, &parent);
        size_t bi = 0;

        while (bi < display.count && child_valid) {
            char *markup = fd_path_to_markup(display.entries[bi].path);
            gtk_tree_store_set(ctx->fd_store, &child,
                               FD_COL_TEXT, display.entries[bi].path,
                               FD_COL_MARKUP, markup,
                               FD_COL_CAT, (gint)-1, -1);
            g_free(markup);
            bi++;
            child_valid = gtk_tree_model_iter_next(fd_model, &child);
        }

        while (bi < display.count) {
            GtkTreeIter new_child;
            char *markup = fd_path_to_markup(display.entries[bi].path);
            gtk_tree_store_append(ctx->fd_store, &new_child, &parent);
            gtk_tree_store_set(ctx->fd_store, &new_child,
                               FD_COL_TEXT, display.entries[bi].path,
                               FD_COL_MARKUP, markup,
                               FD_COL_CAT, (gint)-1, -1);
            g_free(markup);
            bi++;
        }

        fd_list_free(&display);

        while (child_valid) {
            child_valid = gtk_tree_store_remove(ctx->fd_store, &child);
        }

        GtkTreePath *cat_path = gtk_tree_model_get_path(
            fd_model, &cat_iters[c]);
        if (ctx->fd_collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->fd_view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->fd_view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    gtk_adjustment_set_value(fd_vadj, fd_scroll_pos);
}

void fd_scan_start(ui_ctx_t *ctx, pid_t pid)
{
    if (ctx->fd_cancel) {
        g_cancellable_cancel(ctx->fd_cancel);
        g_object_unref(ctx->fd_cancel);
    }
    ctx->fd_cancel = g_cancellable_new();
    ctx->fd_generation++;

    if (pid != ctx->fd_last_pid) {
        ctx->fd_collapsed = 0;
        ctx->fd_last_pid  = pid;
    }

    fd_scan_task_t *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->pid        = pid;
    t->generation = ctx->fd_generation;
    t->ctx        = ctx;
    t->fdmon      = ctx->mon ? ctx->mon->fdmon : NULL;

    if (ctx->fd_include_desc) {
        GtkTreeIter fd_iter;
        if (find_iter_by_pid(GTK_TREE_MODEL(ctx->store), NULL,
                             pid, &fd_iter)) {
            size_t cap = 0;
            collect_descendant_pids(GTK_TREE_MODEL(ctx->store),
                                    &fd_iter,
                                    &t->desc_pids, &t->desc_count, &cap);
        }
    }

    GTask *task = g_task_new(NULL, ctx->fd_cancel, fd_scan_complete, ctx);
    g_task_set_task_data(task, t, (GDestroyNotify)fd_scan_task_free);
    g_task_run_in_thread(task, fd_scan_thread_func);
    g_object_unref(task);
}
