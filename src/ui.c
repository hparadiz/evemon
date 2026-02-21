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
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <utmpx.h>
#include <arpa/inet.h>
#include <inttypes.h>

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
    COL_START_TIME,    /* epoch seconds (gint64) for sorting   */
    COL_START_TIME_TEXT,/* formatted start-time string          */
    COL_CONTAINER,     /* container runtime label (string)      */
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

    /* sidebar (detail panel for selected process) */
    GtkWidget          *sidebar;           /* the outer frame/scrolled widget  */
    GtkCheckMenuItem   *sidebar_menu_item; /* View → Sidebar toggle            */
    GtkWidget          *sidebar_grid;      /* the GtkGrid with key-value rows  */
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
    GtkLabel           *sb_cwd;
    GtkLabel           *sb_cmdline;

    /* file descriptor list in sidebar */
    GtkTreeStore       *fd_store;          /* tree store for fd list       */
    GtkTreeView        *fd_view;           /* tree view for fd list        */
    GtkWidget          *fd_desc_toggle;    /* checkbox: include descendants*/
    gboolean            fd_include_desc;   /* current toggle state         */
    unsigned            fd_collapsed;      /* bitmask: 1 << cat if user collapsed */
    pid_t               fd_last_pid;       /* PID shown last update (reset on change) */

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

/* Format an elapsed-seconds value into a human-friendly "ago" string. */
static void format_fuzzy_time(time_t epoch, char *buf, size_t bufsz)
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

/* ── file-descriptor enumeration from /proc ──────────────────── */

typedef struct {
    int   fd;
    char  path[512];
} fd_entry_t;

typedef struct {
    fd_entry_t *entries;
    size_t      count;
    size_t      capacity;
} fd_list_t;

static void fd_list_init(fd_list_t *l)
{
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void fd_list_free(fd_list_t *l)
{
    free(l->entries);
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void fd_list_push(fd_list_t *l, int fd, const char *path)
{
    if (l->count >= l->capacity) {
        l->capacity = l->capacity ? l->capacity * 2 : 64;
        l->entries = realloc(l->entries, l->capacity * sizeof(fd_entry_t));
    }
    l->entries[l->count].fd = fd;
    snprintf(l->entries[l->count].path, sizeof(l->entries[0].path), "%s", path);
    l->count++;
}

/* ── fd classification ────────────────────────────────────────── */

typedef enum {
    FD_CAT_FILES,           /* regular file paths (not /dev/)           */
    FD_CAT_DEVICES,         /* /dev/ paths                              */
    FD_CAT_NET_SOCKETS,     /* TCP/UDP network sockets                  */
    FD_CAT_UNIX_SOCKETS,    /* Unix domain sockets                      */
    FD_CAT_OTHER_SOCKETS,   /* netlink, packet, or unknown sockets      */
    FD_CAT_PIPES,           /* pipe:[...]                               */
    FD_CAT_EVENTS,          /* anon_inode:[eventfd|eventpoll|signalfd|timerfd] */
    FD_CAT_OTHER,           /* anything else                            */
    FD_CAT_COUNT
} fd_category_t;

/* Columns in the fd tree store */
enum {
    FD_COL_TEXT,    /* display string (category header or path) */
    FD_COL_CAT,     /* category id (int); -1 for child rows     */
    FD_NUM_COLS
};

static const char *fd_cat_label[FD_CAT_COUNT] = {
    [FD_CAT_FILES]         = "Files",
    [FD_CAT_DEVICES]       = "Devices",
    [FD_CAT_NET_SOCKETS]   = "Network Sockets",
    [FD_CAT_UNIX_SOCKETS]  = "Unix Sockets",
    [FD_CAT_OTHER_SOCKETS] = "Other Sockets",
    [FD_CAT_PIPES]         = "Pipes",
    [FD_CAT_EVENTS]        = "Event/Signaling",
    [FD_CAT_OTHER]         = "Other",
};

/* ── /proc/net socket inode resolution ────────────────────────── */

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
    /* For TCP/UDP: local and remote addr+port */
    uint32_t      local_addr;       /* IPv4 host order */
    uint16_t      local_port;
    uint32_t      remote_addr;
    uint16_t      remote_port;
    /* For TCP6/UDP6: full 128-bit addrs */
    unsigned char local_addr6[16];
    unsigned char remote_addr6[16];
    uint16_t      local_port6;
    uint16_t      remote_port6;
    /* For Unix: path (may be empty for abstract/unnamed) */
    char          unix_path[256];
    int           unix_type;        /* SOCK_STREAM=1, SOCK_DGRAM=2 */
} sock_info_t;

typedef struct {
    sock_info_t *entries;
    size_t       count;
    size_t       capacity;
} sock_table_t;

static void sock_table_init(sock_table_t *t)
{
    t->entries  = NULL;
    t->count    = 0;
    t->capacity = 0;
}

static void sock_table_free(sock_table_t *t)
{
    free(t->entries);
    t->entries  = NULL;
    t->count    = 0;
    t->capacity = 0;
}

static sock_info_t *sock_table_push(sock_table_t *t)
{
    if (t->count >= t->capacity) {
        t->capacity = t->capacity ? t->capacity * 2 : 256;
        t->entries = realloc(t->entries, t->capacity * sizeof(sock_info_t));
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

/*
 * Parse /proc/net/tcp or /proc/net/udp (IPv4).
 * Columns: sl local_address rem_address st ... inode ...
 */
static void parse_proc_net_inet(const char *path, sock_kind_t kind,
                                sock_table_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    /* skip header line */
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

/*
 * Parse /proc/net/tcp6 or /proc/net/udp6 (IPv6).
 * Addresses are 32 hex chars (4 × 32-bit words in host order).
 */
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
            s->inode       = inode;
            s->kind        = kind;
            s->local_port6 = (uint16_t)local_port;
            s->remote_port6= (uint16_t)remote_port;
            /* Decode 32-char hex into 16 bytes.
             * The kernel stores each 32-bit word in host byte order,
             * so we read 4 words and memcpy them. */
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

/*
 * Parse /proc/net/unix.
 * Columns: Num RefCount Protocol Flags Type St Inode Path
 */
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
        /* Num: RefCount: Protocol: Flags: Type: St: Inode [Path] */
        int n = sscanf(line, "%*p: %*X %*X %*X %X %*X %lu %255[^\n]",
                       &type, &inode, path);
        if (n >= 2 && inode > 0) {
            sock_info_t *s = sock_table_push(out);
            s->inode     = inode;
            s->kind      = SOCK_KIND_UNIX;
            s->unix_type = (int)type;
            /* trim leading space from path */
            char *p = path;
            while (*p == ' ') p++;
            snprintf(s->unix_path, sizeof(s->unix_path), "%s", p);
        }
    }
    fclose(f);
}

/* Build a complete socket table from all /proc/net sources. */
static void sock_table_build(sock_table_t *out)
{
    sock_table_init(out);
    parse_proc_net_inet("/proc/net/tcp",  SOCK_KIND_TCP,  out);
    parse_proc_net_inet("/proc/net/udp",  SOCK_KIND_UDP,  out);
    parse_proc_net_inet6("/proc/net/tcp6", SOCK_KIND_TCP6, out);
    parse_proc_net_inet6("/proc/net/udp6", SOCK_KIND_UDP6, out);
    parse_proc_net_unix(out);
}

/* Format an IPv4 address (already in network byte order from kernel). */
static void format_ipv4(uint32_t addr, char *buf, size_t bufsz)
{
    inet_ntop(AF_INET, &addr, buf, (socklen_t)bufsz);
}

/* Format an IPv6 address.  Collapse to IPv4 if it's a mapped address. */
static void format_ipv6(const unsigned char *addr, char *buf, size_t bufsz)
{
    /* Check for ::ffff:x.x.x.x (IPv4-mapped) */
    static const unsigned char mapped_prefix[12] =
        {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
    if (memcmp(addr, mapped_prefix, 12) == 0) {
        inet_ntop(AF_INET, addr + 12, buf, (socklen_t)bufsz);
    } else {
        inet_ntop(AF_INET6, addr, buf, (socklen_t)bufsz);
    }
}

/*
 * Given a socket inode, look it up and write a human-readable
 * description into `desc`.  Returns the appropriate category.
 */
static fd_category_t resolve_socket(unsigned long inode,
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

static fd_category_t classify_fd(const char *path)
{
    if (strncmp(path, "socket:", 7) == 0)
        return FD_CAT_OTHER_SOCKETS;   /* placeholder; resolved later */
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

/*
 * ── sysfs device name resolution ────────────────────────────────
 *
 * For devices that can be identified through sysfs, we read the actual
 * hardware name once and cache it.  For PCI devices (GPUs) we also
 * look up the vendor:device pair in the pci.ids database.
 *
 * Each helper reads a single small sysfs file — one open+read+close.
 * Results are cached in a simple linear table so each sysfs path is
 * read at most once per allmon session.
 */

/* ── tiny name cache ──────────────────────────────────────────── */

#define DEV_CACHE_MAX 256

typedef struct {
    char key[128];          /* sysfs lookup key (e.g. "renderD128") */
    char name[256];         /* resolved human name */
} dev_cache_entry_t;

static dev_cache_entry_t g_dev_cache[DEV_CACHE_MAX];
static size_t            g_dev_cache_count = 0;

static const char *dev_cache_get(const char *key)
{
    for (size_t i = 0; i < g_dev_cache_count; i++)
        if (strcmp(g_dev_cache[i].key, key) == 0)
            return g_dev_cache[i].name;
    return NULL;
}

static const char *dev_cache_put(const char *key, const char *name)
{
    if (g_dev_cache_count >= DEV_CACHE_MAX) return name;
    dev_cache_entry_t *e = &g_dev_cache[g_dev_cache_count++];
    snprintf(e->key,  sizeof(e->key),  "%s", key);
    snprintf(e->name, sizeof(e->name), "%s", name);
    return e->name;
}

/* Read a single-line sysfs file into buf, stripping trailing newline.
 * Returns 1 on success, 0 on failure. */
static int read_sysfs_line(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    buf[0] = '\0';
    if (fgets(buf, (int)bufsz, f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == ' '))
            buf[--len] = '\0';
    }
    fclose(f);
    return buf[0] != '\0';
}

/* ── PCI IDs database lookup ──────────────────────────────────── */

/*
 * Lazily load and search /usr/share/hwdata/pci.ids for a vendor:device
 * pair.  The file format is:
 *
 *   <vendor_hex>  <vendor_name>
 *   \t<device_hex>  <device_name>
 *
 * We do a linear scan — the file is ~1 MB and we only search it once
 * per unique PCI ID then cache the result.
 */
static const char *pci_ids_paths[] = {
    "/usr/share/hwdata/pci.ids",
    "/usr/share/misc/pci.ids",
    "/usr/local/share/hwdata/pci.ids",
    NULL
};

static int lookup_pci_id(unsigned vendor, unsigned device,
                         char *vname, size_t vnsz,
                         char *dname, size_t dnsz)
{
    FILE *f = NULL;
    for (const char **p = pci_ids_paths; *p; p++) {
        f = fopen(*p, "r");
        if (f) break;
    }
    if (!f) return 0;

    char line[512];
    int found_vendor = 0;
    vname[0] = '\0';
    dname[0] = '\0';

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        if (!found_vendor) {
            /* Top-level vendor line: "1002  AMD/ATI" */
            if (line[0] != '\t') {
                unsigned v;
                if (sscanf(line, "%x", &v) == 1 && v == vendor) {
                    found_vendor = 1;
                    char *nm = strchr(line, ' ');
                    if (nm) {
                        while (*nm == ' ') nm++;
                        snprintf(vname, vnsz, "%s", nm);
                        size_t len = strlen(vname);
                        while (len > 0 && (vname[len-1]=='\n'||vname[len-1]==' '))
                            vname[--len] = '\0';
                    }
                }
            }
        } else {
            /* We're inside our vendor block */
            if (line[0] != '\t') break;   /* next vendor — stop */
            if (line[0] == '\t' && line[1] != '\t') {
                unsigned d;
                if (sscanf(line + 1, "%x", &d) == 1 && d == device) {
                    char *nm = strchr(line + 1, ' ');
                    if (nm) {
                        while (*nm == ' ') nm++;
                        snprintf(dname, dnsz, "%s", nm);
                        size_t len = strlen(dname);
                        while (len > 0 && (dname[len-1]=='\n'||dname[len-1]==' '))
                            dname[--len] = '\0';
                    }
                    fclose(f);
                    return 1;
                }
            }
        }
    }
    fclose(f);
    return found_vendor;   /* at least got vendor name */
}

/* ── resolve DRI device name via sysfs + pci.ids ────────────── */

static const char *resolve_dri_name(const char *dri_name)
{
    const char *cached = dev_cache_get(dri_name);
    if (cached) return cached;

    /* Read PCI_ID from /sys/class/drm/<dri_name>/device/uevent */
    char uevent_path[256];
    snprintf(uevent_path, sizeof(uevent_path),
             "/sys/class/drm/%s/device/uevent", dri_name);

    unsigned vendor = 0, device = 0;
    FILE *f = fopen(uevent_path, "r");
    if (!f) return NULL;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "PCI_ID=%x:%x", &vendor, &device) == 2)
            break;
    }
    fclose(f);

    if (vendor == 0) return NULL;

    char vname[128] = "", dname[192] = "";
    lookup_pci_id(vendor, device, vname, sizeof(vname), dname, sizeof(dname));

    char result[256];
    if (dname[0])
        snprintf(result, sizeof(result), "%s", dname);
    else if (vname[0])
        snprintf(result, sizeof(result), "%s [%04x:%04x]", vname, vendor, device);
    else
        return NULL;

    return dev_cache_put(dri_name, result);
}

/* ── resolve sound device name via sysfs ─────────────────────── */

static const char *resolve_snd_name(const char *snd_node)
{
    const char *cached = dev_cache_get(snd_node);
    if (cached) return cached;

    /* Extract card number from node name (e.g. "controlC3" → "3") */
    const char *p = snd_node;
    int card_num = -1;
    while (*p) {
        if (*p == 'C' || *p == 'D') {
            char *end;
            long n = strtol(p + 1, &end, 10);
            if (end != p + 1) { card_num = (int)n; break; }
        }
        p++;
    }
    if (card_num < 0) return NULL;

    char id_path[128];
    snprintf(id_path, sizeof(id_path), "/sys/class/sound/card%d/id", card_num);

    char name[128] = "";
    if (!read_sysfs_line(id_path, name, sizeof(name))) return NULL;

    return dev_cache_put(snd_node, name);
}

/* ── resolve input device name via sysfs ─────────────────────── */

static const char *resolve_input_name(const char *input_node)
{
    const char *cached = dev_cache_get(input_node);
    if (cached) return cached;

    char name_path[128];
    snprintf(name_path, sizeof(name_path),
             "/sys/class/input/%s/device/name", input_node);

    char name[256] = "";
    if (!read_sysfs_line(name_path, name, sizeof(name))) return NULL;

    return dev_cache_put(input_node, name);
}

/* ── resolve block device model via sysfs ────────────────────── */

static const char *resolve_block_name(const char *blk_name)
{
    const char *cached = dev_cache_get(blk_name);
    if (cached) return cached;

    /* Strip partition suffix: nvme0n1p2 → nvme0n1, sda1 → sda */
    char base[64];
    snprintf(base, sizeof(base), "%s", blk_name);
    size_t len = strlen(base);

    if (strncmp(base, "nvme", 4) == 0) {
        /* nvme0n1p2 → nvme0n1: strip pN suffix */
        char *pp = strrchr(base, 'p');
        if (pp && pp > base + 4 && pp[-1] >= '0' && pp[-1] <= '9'
            && pp[1] >= '0' && pp[1] <= '9')
            *pp = '\0';
    } else {
        /* sda1 → sda: strip trailing digits */
        while (len > 0 && base[len-1] >= '0' && base[len-1] <= '9')
            base[--len] = '\0';
    }

    char model_path[128];
    snprintf(model_path, sizeof(model_path),
             "/sys/block/%s/device/model", base);

    char model[128] = "";
    if (!read_sysfs_line(model_path, model, sizeof(model))) return NULL;

    return dev_cache_put(blk_name, model);
}

/* ── resolve video4linux device name via sysfs ───────────────── */

static const char *resolve_video_name(const char *v4l_node)
{
    const char *cached = dev_cache_get(v4l_node);
    if (cached) return cached;

    char name_path[128];
    snprintf(name_path, sizeof(name_path),
             "/sys/class/video4linux/%s/name", v4l_node);

    char name[256] = "";
    if (!read_sysfs_line(name_path, name, sizeof(name))) return NULL;

    return dev_cache_put(v4l_node, name);
}

/*
 * Label a /dev/ path with a human-readable device description.
 * For devices whose hardware identity can be resolved via sysfs or
 * the PCI IDs database, the actual name is shown.  Otherwise a
 * generic category label is used.  Results are cached.
 */
static void label_device(const char *path, char *desc, size_t descsz)
{
    const char *after_dev = path + 5;   /* skip "/dev/" */

    /* ── null / zero / full / random ─────────────────────────── */
    if (strcmp(after_dev, "null") == 0) {
        snprintf(desc, descsz, "%s  (null sink)", path); return;
    }
    if (strcmp(after_dev, "zero") == 0) {
        snprintf(desc, descsz, "%s  (zero source)", path); return;
    }
    if (strcmp(after_dev, "full") == 0) {
        snprintf(desc, descsz, "%s  (always-full sink)", path); return;
    }
    if (strcmp(after_dev, "random") == 0 ||
        strcmp(after_dev, "urandom") == 0) {
        snprintf(desc, descsz, "%s  (random number generator)", path); return;
    }

    /* ── terminals & PTYs ────────────────────────────────────── */
    if (strncmp(after_dev, "pts/", 4) == 0) {
        snprintf(desc, descsz, "%s  (pseudo-terminal)", path); return;
    }
    if (strcmp(after_dev, "ptmx") == 0) {
        snprintf(desc, descsz, "%s  (PTY multiplexer)", path); return;
    }
    if (strncmp(after_dev, "tty", 3) == 0) {
        if (after_dev[3] == '\0')
            snprintf(desc, descsz, "%s  (controlling terminal)", path);
        else if (after_dev[3] == 'S')
            snprintf(desc, descsz, "%s  (serial port)", path);
        else
            snprintf(desc, descsz, "%s  (virtual console)", path);
        return;
    }
    if (strcmp(after_dev, "console") == 0) {
        snprintf(desc, descsz, "%s  (system console)", path); return;
    }

    /* ── shared memory ───────────────────────────────────────── */
    if (strncmp(after_dev, "shm/", 4) == 0) {
        snprintf(desc, descsz, "%s  (shared memory)", path); return;
    }

    /* ── GPU / DRI — resolve actual GPU name from sysfs + pci.ids ── */
    if (strncmp(after_dev, "dri/", 4) == 0) {
        const char *dri_node = after_dev + 4;   /* "renderD128" or "card0" */
        const char *hw_name = resolve_dri_name(dri_node);
        const char *type = strncmp(dri_node, "renderD", 7) == 0
                           ? "GPU render" : "GPU";
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s: %s)", path, type, hw_name);
        else
            snprintf(desc, descsz, "%s  (%s node)", path, type);
        return;
    }
    if (strncmp(after_dev, "nvidia", 6) == 0) {
        snprintf(desc, descsz, "%s  (NVIDIA GPU)", path); return;
    }

    /* ── sound — resolve card name from sysfs ────────────────── */
    if (strncmp(after_dev, "snd/", 4) == 0) {
        const char *snd_node = after_dev + 4;
        const char *hw_name = resolve_snd_name(snd_node);
        const char *type;
        if (strncmp(snd_node, "control", 7) == 0)     type = "audio control";
        else if (strncmp(snd_node, "pcm", 3) == 0)    type = "audio PCM";
        else if (strncmp(snd_node, "timer", 5) == 0)   type = "audio timer";
        else if (strncmp(snd_node, "seq", 3) == 0)     type = "MIDI sequencer";
        else                                            type = "audio";
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s: %s)", path, type, hw_name);
        else
            snprintf(desc, descsz, "%s  (%s)", path, type);
        return;
    }

    /* ── input devices — resolve name from sysfs ─────────────── */
    if (strncmp(after_dev, "input/", 6) == 0) {
        const char *inp_node = after_dev + 6;
        const char *hw_name = resolve_input_name(inp_node);
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s)", path, hw_name);
        else if (strncmp(inp_node, "event", 5) == 0)
            snprintf(desc, descsz, "%s  (input event)", path);
        else if (strncmp(inp_node, "mouse", 5) == 0)
            snprintf(desc, descsz, "%s  (mouse)", path);
        else if (strncmp(inp_node, "js", 2) == 0)
            snprintf(desc, descsz, "%s  (joystick)", path);
        else
            snprintf(desc, descsz, "%s  (input device)", path);
        return;
    }

    /* ── video / camera — resolve name from sysfs ────────────── */
    if (strncmp(after_dev, "video", 5) == 0) {
        const char *v4l_node = after_dev;   /* "video0" */
        const char *hw_name = resolve_video_name(v4l_node);
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s)", path, hw_name);
        else
            snprintf(desc, descsz, "%s  (video/camera)", path);
        return;
    }

    /* ── FUSE ────────────────────────────────────────────────── */
    if (strcmp(after_dev, "fuse") == 0) {
        snprintf(desc, descsz, "%s  (FUSE filesystem)", path); return;
    }

    /* ── block devices — resolve model from sysfs ────────────── */
    if (strncmp(after_dev, "sd", 2) == 0 ||
        strncmp(after_dev, "nvme", 4) == 0 ||
        strncmp(after_dev, "vd", 2) == 0 ||
        strncmp(after_dev, "xvd", 3) == 0) {
        const char *hw_name = resolve_block_name(after_dev);
        if (hw_name)
            snprintf(desc, descsz, "%s  (%s)", path, hw_name);
        else
            snprintf(desc, descsz, "%s  (block storage)", path);
        return;
    }
    if (strncmp(after_dev, "dm-", 3) == 0 ||
        strncmp(after_dev, "mapper/", 7) == 0) {
        snprintf(desc, descsz, "%s  (device-mapper)", path); return;
    }
    if (strncmp(after_dev, "loop", 4) == 0) {
        snprintf(desc, descsz, "%s  (loop device)", path); return;
    }

    /* ── network ─────────────────────────────────────────────── */
    if (strncmp(after_dev, "net/", 4) == 0) {
        snprintf(desc, descsz, "%s  (network device)", path); return;
    }
    if (strcmp(after_dev, "rfkill") == 0) {
        snprintf(desc, descsz, "%s  (RF kill switch)", path); return;
    }

    /* ── hardware RNG ────────────────────────────────────────── */
    if (strncmp(after_dev, "hwrng", 5) == 0) {
        snprintf(desc, descsz, "%s  (hardware RNG)", path); return;
    }

    /* ── KVM ─────────────────────────────────────────────────── */
    if (strcmp(after_dev, "kvm") == 0) {
        snprintf(desc, descsz, "%s  (KVM hypervisor)", path); return;
    }

    /* ── misc / VFIO / iommu ─────────────────────────────────── */
    if (strncmp(after_dev, "vfio/", 5) == 0) {
        snprintf(desc, descsz, "%s  (VFIO passthrough)", path); return;
    }
    if (strncmp(after_dev, "hugepages", 9) == 0) {
        snprintf(desc, descsz, "%s  (huge pages)", path); return;
    }
    if (strncmp(after_dev, "usb", 3) == 0 ||
        strncmp(after_dev, "bus/usb", 7) == 0) {
        snprintf(desc, descsz, "%s  (USB device)", path); return;
    }

    /* ── fallback: unknown device ────────────────────────────── */
    snprintf(desc, descsz, "%s", path);
}

/* Cached /home/$USER/ prefix for sort prioritisation. */
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

/*
 * Sort priority for fd paths:
 *   0 = /home/$USER/ paths  (highest, sort first)
 *   1 = regular filesystem paths (start with '/')
 *   2 = everything else (pipes, sockets, anon_inode, etc.)
 * Within each tier, sort alphabetically.
 */
static int fd_path_tier(const char *path)
{
    size_t hlen;
    const char *hpfx = get_home_prefix(&hlen);
    if (hlen > 0 && strncmp(path, hpfx, hlen) == 0)
        return 0;
    /* Real filesystem paths start with '/' followed by a normal directory
     * component.  Pseudo-paths like /memfd:, /dmabuf: also start with '/'
     * but have a colon in the first component – push those to "other". */
    if (path[0] == '/') {
        const char *colon = strchr(path + 1, ':');
        const char *slash = strchr(path + 1, '/');
        /* If there's a colon before the first slash (or no slash at all),
         * this is a pseudo-path like /memfd:foo or /dmabuf:bar. */
        if (colon && (!slash || colon < slash))
            return 2;
        return 1;
    }
    return 2;
}

static int fd_entry_path_cmp(const void *a, const void *b)
{
    const char *pa = ((const fd_entry_t *)a)->path;
    const char *pb = ((const fd_entry_t *)b)->path;

    int ta = fd_path_tier(pa);
    int tb = fd_path_tier(pb);
    if (ta != tb)
        return ta - tb;

    return strcmp(pa, pb);
}

/* Read all open fds for a single PID from /proc/<pid>/fd/. */
static void read_pid_fds(pid_t pid, fd_list_t *out)
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

/*
 * Collect all children (recursive) of `parent_pid` from the tree store
 * and read their fds too.
 */
static void collect_descendant_fds(GtkTreeModel *model, GtkTreeIter *parent,
                                   fd_list_t *out)
{
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(model, &child, parent);
    while (valid) {
        gint pid;
        gtk_tree_model_get(model, &child, COL_PID, &pid, -1);
        read_pid_fds((pid_t)pid, out);
        collect_descendant_fds(model, &child, out);
        valid = gtk_tree_model_iter_next(model, &child);
    }
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

    char start_text[64] = "–";
    if (e->start_time > 0) {
        time_t t = (time_t)e->start_time;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(start_text, sizeof(start_text), "%Y-%m-%d %H:%M:%S", &tm);
    }

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
                       COL_START_TIME,      (gint64)e->start_time,
                       COL_START_TIME_TEXT, start_text,
                       COL_CONTAINER,  e->container[0] ? e->container : "",
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

/* ── sidebar: update detail panel from selection ─────────────── */

static void sidebar_update(ui_ctx_t *ctx)
{
    if (!gtk_widget_get_visible(ctx->sidebar))
        return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    if (!sel) return;

    GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
    if (!rows) {
        /* No selection – clear all labels */
        gtk_label_set_text(ctx->sb_pid,       "–");
        gtk_label_set_text(ctx->sb_ppid,      "–");
        gtk_label_set_text(ctx->sb_user,      "–");
        gtk_label_set_text(ctx->sb_name,      "–");
        gtk_label_set_text(ctx->sb_cpu,       "–");
        gtk_label_set_text(ctx->sb_rss,       "–");
        gtk_label_set_text(ctx->sb_group_rss, "–");
        gtk_label_set_text(ctx->sb_group_cpu, "–");
        gtk_label_set_text(ctx->sb_start_time, "–");
        gtk_label_set_text(ctx->sb_container, "–");
        gtk_label_set_text(ctx->sb_cwd,       "–");
        gtk_label_set_text(ctx->sb_cmdline,   "–");
        gtk_tree_store_clear(ctx->fd_store);
        return;
    }

    GtkTreePath *path = rows->data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store), &iter, path)) {
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
        return;
    }

    gint pid = 0, ppid = 0, cpu_raw = 0, rss = 0, grp_rss = 0, grp_cpu = 0;
    gint64 start_epoch = 0;
    gchar *user = NULL, *name = NULL, *cpu_text = NULL;
    gchar *rss_text = NULL, *grp_rss_text = NULL, *grp_cpu_text = NULL;
    gchar *start_time_text = NULL, *container = NULL, *cwd = NULL, *cmdline = NULL;

    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &iter,
                       COL_PID,            &pid,
                       COL_PPID,           &ppid,
                       COL_USER,           &user,
                       COL_NAME,           &name,
                       COL_CPU_TEXT,       &cpu_text,
                       COL_RSS_TEXT,       &rss_text,
                       COL_GROUP_RSS_TEXT, &grp_rss_text,
                       COL_GROUP_CPU_TEXT, &grp_cpu_text,
                       COL_START_TIME,     &start_epoch,
                       COL_START_TIME_TEXT, &start_time_text,
                       COL_CONTAINER,      &container,
                       COL_CWD,           &cwd,
                       COL_CMDLINE,        &cmdline,
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
    gtk_label_set_text(ctx->sb_cwd,       cwd       ? cwd       : "–");
    gtk_label_set_text(ctx->sb_cmdline,   cmdline   ? cmdline   : "–");

    /* ── populate file descriptor tree (incremental) ────────── */
    {
        /* If the selected PID changed, reset collapse state */
        if ((pid_t)pid != ctx->fd_last_pid) {
            ctx->fd_collapsed = 0;
            ctx->fd_last_pid  = (pid_t)pid;
        }

        fd_list_t fds;
        fd_list_init(&fds);

        read_pid_fds((pid_t)pid, &fds);

        if (ctx->fd_include_desc) {
            GtkTreeIter fd_iter;
            if (find_iter_by_pid(GTK_TREE_MODEL(ctx->store), NULL,
                                (pid_t)pid, &fd_iter)) {
                collect_descendant_fds(GTK_TREE_MODEL(ctx->store),
                                       &fd_iter, &fds);
            }
        }

        /* Build socket inode table for resolution */
        sock_table_t socktbl;
        sock_table_build(&socktbl);

        /* Bucket fds by category */
        fd_list_t buckets[FD_CAT_COUNT];
        for (int c = 0; c < FD_CAT_COUNT; c++)
            fd_list_init(&buckets[c]);

        for (size_t i = 0; i < fds.count; i++) {
            fd_category_t cat = classify_fd(fds.entries[i].path);
            /* For sockets, resolve the inode to get the real
             * sub-category (net/unix/other) and a descriptive path. */
            if (cat == FD_CAT_OTHER_SOCKETS &&
                strncmp(fds.entries[i].path, "socket:[", 8) == 0) {
                unsigned long inode = strtoul(
                    fds.entries[i].path + 8, NULL, 10);
                char desc[512];
                cat = resolve_socket(inode, &socktbl,
                                     desc, sizeof(desc));
                fd_list_push(&buckets[cat], fds.entries[i].fd, desc);
            } else if (cat == FD_CAT_DEVICES) {
                char desc[512];
                label_device(fds.entries[i].path, desc, sizeof(desc));
                fd_list_push(&buckets[cat], fds.entries[i].fd, desc);
            } else {
                fd_list_push(&buckets[cat], fds.entries[i].fd,
                             fds.entries[i].path);
            }
        }
        sock_table_free(&socktbl);
        for (int c = 0; c < FD_CAT_COUNT; c++) {
            if (buckets[c].count > 1)
                qsort(buckets[c].entries, buckets[c].count,
                      sizeof(fd_entry_t), fd_entry_path_cmp);
        }

        /* Save scroll position */
        GtkAdjustment *fd_vadj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(gtk_widget_get_parent(
                GTK_WIDGET(ctx->fd_view))));
        double fd_scroll_pos = gtk_adjustment_get_value(fd_vadj);

        GtkTreeModel *fd_model = GTK_TREE_MODEL(ctx->fd_store);

        /*
         * Incremental update: walk existing top-level rows by category id.
         * Build a map of existing category iters.
         */
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

        /* Remove categories that are now empty */
        for (int c = 0; c < FD_CAT_COUNT; c++) {
            if (cat_exists[c] && buckets[c].count == 0) {
                gtk_tree_store_remove(ctx->fd_store, &cat_iters[c]);
                cat_exists[c] = FALSE;
            }
        }

        /* Add or update each non-empty category */
        for (int c = 0; c < FD_CAT_COUNT; c++) {
            if (buckets[c].count == 0) continue;

            char hdr[128];
            snprintf(hdr, sizeof(hdr), "%s (%zu)",
                     fd_cat_label[c], buckets[c].count);

            GtkTreeIter parent;
            if (!cat_exists[c]) {
                /* New category – append and expand by default */
                gtk_tree_store_append(ctx->fd_store, &parent, NULL);
                gtk_tree_store_set(ctx->fd_store, &parent,
                                   FD_COL_TEXT, hdr,
                                   FD_COL_CAT, (gint)c, -1);
                cat_exists[c] = TRUE;
                cat_iters[c]  = parent;
            } else {
                parent = cat_iters[c];
                /* Update header text (count may have changed) */
                gtk_tree_store_set(ctx->fd_store, &parent,
                                   FD_COL_TEXT, hdr, -1);
            }

            /*
             * Sync children: walk existing children and the new sorted
             * bucket in parallel.  Update in-place where possible,
             * insert/remove to match.
             */
            GtkTreeIter child;
            gboolean child_valid = gtk_tree_model_iter_children(
                fd_model, &child, &parent);
            size_t bi = 0;

            while (bi < buckets[c].count && child_valid) {
                /* Update existing child row */
                gtk_tree_store_set(ctx->fd_store, &child,
                                   FD_COL_TEXT, buckets[c].entries[bi].path,
                                   FD_COL_CAT, (gint)-1, -1);
                bi++;
                child_valid = gtk_tree_model_iter_next(fd_model, &child);
            }

            /* Append any remaining new entries */
            while (bi < buckets[c].count) {
                GtkTreeIter new_child;
                gtk_tree_store_append(ctx->fd_store, &new_child, &parent);
                gtk_tree_store_set(ctx->fd_store, &new_child,
                                   FD_COL_TEXT, buckets[c].entries[bi].path,
                                   FD_COL_CAT, (gint)-1, -1);
                bi++;
            }

            /* Remove surplus old children */
            while (child_valid) {
                child_valid = gtk_tree_store_remove(ctx->fd_store, &child);
            }

            /* Expand/collapse based on saved user state */
            GtkTreePath *cat_path = gtk_tree_model_get_path(
                fd_model, &cat_iters[c]);
            if (ctx->fd_collapsed & (1u << c))
                gtk_tree_view_collapse_row(ctx->fd_view, cat_path);
            else
                gtk_tree_view_expand_row(ctx->fd_view, cat_path, FALSE);
            gtk_tree_path_free(cat_path);
        }

        /* Restore scroll position */
        gtk_adjustment_set_value(fd_vadj, fd_scroll_pos);

        for (int c = 0; c < FD_CAT_COUNT; c++)
            fd_list_free(&buckets[c]);
        fd_list_free(&fds);
    }

    g_free(user); g_free(name); g_free(cpu_text);
    g_free(rss_text); g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(start_time_text); g_free(container); g_free(cwd); g_free(cmdline);

    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
}

static void on_fd_desc_toggled(GtkToggleButton *btn, gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->fd_include_desc = gtk_toggle_button_get_active(btn);
    ctx->fd_collapsed = 0;   /* reset collapse state on structural change */
    gtk_tree_store_clear(ctx->fd_store);
    sidebar_update(ctx);
}

static void on_fd_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                                GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->fd_store), iter,
                       FD_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < FD_CAT_COUNT)
        ctx->fd_collapsed |= (1u << cat_id);
}

static void on_fd_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                               GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->fd_store), iter,
                       FD_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < FD_CAT_COUNT)
        ctx->fd_collapsed &= ~(1u << cat_id);
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)sel;
    sidebar_update((ui_ctx_t *)data);
}

static void on_toggle_sidebar(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    if (gtk_check_menu_item_get_active(item)) {
        gtk_widget_show_all(ctx->sidebar);
        sidebar_update(ctx);
    } else {
        gtk_widget_hide(ctx->sidebar);
    }
}

/* ── double-click: open sidebar for the activated row ─────────── */

static void on_row_activated(GtkTreeView       *view,
                             GtkTreePath       *path,
                             GtkTreeViewColumn *col,
                             gpointer           data)
{
    (void)view; (void)path; (void)col;
    ui_ctx_t *ctx = data;

    if (!gtk_widget_get_visible(ctx->sidebar)) {
        /* Toggling the menu item fires the "toggled" signal, which
         * calls on_toggle_sidebar → show + update.                 */
        gtk_check_menu_item_set_active(ctx->sidebar_menu_item, TRUE);
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

    /* Update the sidebar detail panel for the selected process */
    sidebar_update(ctx);

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

static gint sort_int64_inverted(GtkTreeModel *model,
                               GtkTreeIter  *a,
                               GtkTreeIter  *b,
                               gpointer      col_id_ptr)
{
    gint col = GPOINTER_TO_INT(col_id_ptr);
    gint64 va = 0, vb = 0;
    gtk_tree_model_get(model, a, col, &va, -1);
    gtk_tree_model_get(model, b, col, &vb, -1);
    return (va < vb) ? 1 : (va > vb) ? -1 : 0;
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
                                             G_TYPE_INT64,    /* start time   */
                                             G_TYPE_STRING,   /* start time txt*/
                                             G_TYPE_STRING,   /* container    */
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
    gtk_tree_sortable_set_sort_func(sortable, COL_START_TIME,
        sort_int64_inverted, GINT_TO_POINTER(COL_START_TIME), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_CONTAINER,
        sort_string_inverted, GINT_TO_POINTER(COL_CONTAINER), NULL);
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

    /* ── sidebar (detail panel) ───────────────────────────────── */
    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sidebar_scroll, 240, -1);

    GtkWidget *sidebar_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(sidebar_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(sidebar_grid), 8);
    gtk_widget_set_margin_start(sidebar_grid, 8);
    gtk_widget_set_margin_end(sidebar_grid, 8);
    gtk_widget_set_margin_top(sidebar_grid, 8);
    gtk_widget_set_margin_bottom(sidebar_grid, 8);

    /* Helper macro to add a label row to the sidebar grid */
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
        gtk_grid_attach(GTK_GRID(sidebar_grid), _k, 0, row, 1, 1);      \
        gtk_grid_attach(GTK_GRID(sidebar_grid), _v, 1, row, 1, 1);      \
        label_var = GTK_LABEL(_v);                                       \
    } while (0)

    GtkLabel *sb_pid, *sb_ppid, *sb_user, *sb_name;
    GtkLabel *sb_cpu, *sb_rss, *sb_group_rss, *sb_group_cpu;
    GtkLabel *sb_start_time, *sb_container, *sb_cwd, *sb_cmdline;

    SIDEBAR_ROW(0,  "PID",             sb_pid);
    SIDEBAR_ROW(1,  "PPID",            sb_ppid);
    SIDEBAR_ROW(2,  "User",            sb_user);
    SIDEBAR_ROW(3,  "Name",            sb_name);
    SIDEBAR_ROW(4,  "CPU%",            sb_cpu);
    SIDEBAR_ROW(5,  "Memory (RSS)",    sb_rss);
    SIDEBAR_ROW(6,  "Group Memory",    sb_group_rss);
    SIDEBAR_ROW(7,  "Group CPU%",      sb_group_cpu);
    SIDEBAR_ROW(8,  "Start Time",      sb_start_time);
    SIDEBAR_ROW(9,  "Container",       sb_container);
    SIDEBAR_ROW(10, "CWD",            sb_cwd);
    SIDEBAR_ROW(11, "Command",         sb_cmdline);
    #undef SIDEBAR_ROW

    /* ── file descriptors section ─────────────────────────────── */
    GtkWidget *fd_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(sidebar_grid), fd_sep, 0, 12, 2, 1);

    /* Header label */
    GtkWidget *fd_header = gtk_label_new("Open File Descriptors");
    gtk_label_set_xalign(GTK_LABEL(fd_header), 0.0f);
    {
        PangoAttrList *a = pango_attr_list_new();
        pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(fd_header), a);
        pango_attr_list_unref(a);
    }
    gtk_grid_attach(GTK_GRID(sidebar_grid), fd_header, 0, 13, 2, 1);

    /* "Include descendants" toggle */
    GtkWidget *fd_desc_toggle = gtk_check_button_new_with_label(
        "Include descendant tree");
    gtk_grid_attach(GTK_GRID(sidebar_grid), fd_desc_toggle, 0, 14, 2, 1);

    /* Scrollable tree view for the fd list */
    GtkTreeStore *fd_store = gtk_tree_store_new(FD_NUM_COLS,
                                                G_TYPE_STRING,   /* FD_COL_TEXT */
                                                G_TYPE_INT);     /* FD_COL_CAT  */
    GtkWidget *fd_tree = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(fd_store));
    g_object_unref(fd_store);

    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(fd_tree), FALSE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(fd_tree), TRUE);

    GtkCellRenderer *fd_r = gtk_cell_renderer_text_new();
    g_object_set(fd_r, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    GtkTreeViewColumn *fd_col = gtk_tree_view_column_new_with_attributes(
        "Path", fd_r, "text", FD_COL_TEXT, NULL);
    gtk_tree_view_column_set_expand(fd_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(fd_tree), fd_col);

    /* Apply the same monospace CSS to the fd tree */
    GtkCssProvider *fd_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(fd_css,
        "treeview { font-family: Monospace; font-size: 8pt; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(fd_tree),
        GTK_STYLE_PROVIDER(fd_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(fd_css);

    /* Enable selection so user can copy paths */
    GtkTreeSelection *fd_sel = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(fd_tree));
    gtk_tree_selection_set_mode(fd_sel, GTK_SELECTION_SINGLE);

    GtkWidget *fd_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fd_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(fd_scroll, -1, 200);
    gtk_widget_set_vexpand(fd_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(fd_scroll), fd_tree);
    gtk_grid_attach(GTK_GRID(sidebar_grid), fd_scroll, 0, 15, 2, 1);

    gtk_container_add(GTK_CONTAINER(sidebar_scroll), sidebar_grid);

    GtkWidget *sidebar_frame = gtk_frame_new("Details");
    gtk_container_add(GTK_CONTAINER(sidebar_frame), sidebar_scroll);

    /* ── horizontal paned: tree | sidebar ─────────────────────── */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hpaned), scroll, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(hpaned), sidebar_frame, FALSE, FALSE);

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

    /* View menu → Sidebar toggle + Appearance submenu */
    GtkWidget *view_menu = gtk_menu_new();
    GtkWidget *view_item = gtk_menu_item_new_with_label("View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);

    GtkWidget *sidebar_toggle = gtk_check_menu_item_new_with_label("Sidebar");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(sidebar_toggle), FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), sidebar_toggle);
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
    gtk_box_pack_start(GTK_BOX(vbox), hpaned, TRUE, TRUE, 0);
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

    /* Sidebar detail panel */
    ctx.sidebar            = sidebar_frame;
    ctx.sidebar_menu_item  = GTK_CHECK_MENU_ITEM(sidebar_toggle);
    ctx.sidebar_grid       = sidebar_grid;
    ctx.sb_pid        = sb_pid;
    ctx.sb_ppid       = sb_ppid;
    ctx.sb_user       = sb_user;
    ctx.sb_name       = sb_name;
    ctx.sb_cpu        = sb_cpu;
    ctx.sb_rss        = sb_rss;
    ctx.sb_group_rss  = sb_group_rss;
    ctx.sb_group_cpu  = sb_group_cpu;
    ctx.sb_start_time = sb_start_time;
    ctx.sb_container  = sb_container;
    ctx.sb_cwd        = sb_cwd;
    ctx.sb_cmdline    = sb_cmdline;

    /* File descriptor list */
    ctx.fd_store        = fd_store;
    ctx.fd_view         = GTK_TREE_VIEW(fd_tree);
    ctx.fd_desc_toggle  = fd_desc_toggle;
    ctx.fd_include_desc = FALSE;
    ctx.fd_collapsed    = 0;
    ctx.fd_last_pid     = 0;

    /* Font menu callbacks (need ctx address, so connect after ctx init) */
    g_signal_connect(font_inc,  "activate", G_CALLBACK(on_font_increase),    &ctx);
    g_signal_connect(font_dec,  "activate", G_CALLBACK(on_font_decrease),    &ctx);
    g_signal_connect(font_auto, "toggled",  G_CALLBACK(on_font_auto_toggle), &ctx);
    g_signal_connect(sidebar_toggle, "toggled",
                     G_CALLBACK(on_toggle_sidebar), &ctx);
    g_signal_connect(fd_desc_toggle, "toggled",
                     G_CALLBACK(on_fd_desc_toggled), &ctx);
    g_signal_connect(fd_tree, "row-collapsed",
                     G_CALLBACK(on_fd_row_collapsed), &ctx);
    g_signal_connect(fd_tree, "row-expanded",
                     G_CALLBACK(on_fd_row_expanded), &ctx);
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

    /* Double-click a row to open the sidebar */
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), &ctx);

    /* Track user collapse / expand actions */
    g_signal_connect(tree, "row-collapsed", G_CALLBACK(on_row_collapsed), &ctx);
    g_signal_connect(tree, "row-expanded",  G_CALLBACK(on_row_expanded),  &ctx);

    /* Update sidebar immediately when selection changes (arrow keys, click) */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), &ctx);

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
    gtk_widget_hide(menubar);        /* hidden by default; toggle via status-bar right-click */
    gtk_widget_hide(sidebar_frame);  /* hidden by default; toggle via View → Sidebar */
    gtk_main();

    return NULL;
}

