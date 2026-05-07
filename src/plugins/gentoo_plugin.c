/*
 * gentoo_plugin.c – Gentoo/Portage package info plugin for evemon.
 *
 * For the selected process, reads /proc/<pid>/exe to get the installed
 * binary path, then scans the Portage package database at /var/db/pkg/
 * to find which ebuild owns that binary.  Displays:
 *
 *   • Package atom (category/pkgname-version)
 *   • Repository, SLOT, EAPI, License, Homepage, KEYWORDS
 *   • Installed size and build timestamp
 *   • USE flags: colour-coded active (green), default-on but disabled
 *     (orange), and inactive (grey) flags from IUSE
 *   • Runtime dependencies (RDEPEND)
 *
 * Load-gated: evemon_plugin_init() returns NULL on systems where
 * /var/db/pkg/ does not exist, so the plugin is silently skipped on
 * non-Gentoo machines and does not appear as a tab at all.
 *
 * Async model — the GTK main thread is NEVER blocked by Portage IO:
 *
 *   • A single worker thread serializes all /proc and /var/db/pkg work.
 *   • Process selection is coalesced to the latest PID, so rapid UI changes
 *     cannot spawn parallel full-database scans.
 *   • The worker keeps a small executable lookup cache.  It never builds a
 *     whole-system reverse index at startup.
 *   • cancel_scan() is O(1): it bumps an atomic generation counter.  Stale
 *     worker results and queued idle callbacks discard themselves.
 *
 * Build (picked up automatically by the Makefile — no extra flags):
 *   gcc -shared -fPIC -o build/plugins/evemon_gentoo_plugin.so \
 *       src/plugins/gentoo_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.gentoo",
    "Gentoo / Portage",
    "1.0",
    EVEMON_ROLE_PROCESS,
    NULL
);

/* ── Portage scan result ────────────────────────────────────────── */

#define USE_BUF_SIZE  8192
#define DEP_BUF_SIZE  16384

typedef struct {
    int  found;
    char cat[256];
    char pvr[256];
    char pkg_atom[512];          /* "x11-terms/alacritty-0.13.2"        */
    char slot[64];
    char repo[128];
    char license[512];
    char homepage[512];
    char keywords[512];
    char eapi[16];
    char use_flags[USE_BUF_SIZE];   /* active USE flags at install time   */
    char iuse_flags[USE_BUF_SIZE];  /* all ebuild USE flags (may have +/) */
    char rdepend[DEP_BUF_SIZE];
    char size_str[64];
    char build_time_str[64];
} portage_result_t;

/* ── Scan gate ──────────────────────────────────────────────────────
 *
 * Shared, ref-counted sentinel used to coordinate between the GTK main
 * thread, the Portage worker, and queued idle callbacks.
 *
 * Lifetime:
 *   • Created in gentoo_create_widget(); owned (ref=1) by gentoo_ctx_t.
 *   • Each posted g_idle_add payload acquires a ref for its scan_idle_t.
 *   • When a thread or idle drops its last ref the gate is freed.
 *
 * Invariant: if (gate->destroying == 0) then gate->ctx is a valid
 *             gentoo_ctx_t pointer.
 * This is guaranteed because gentoo_destroy() sets gate->destroying BEFORE
 * freeing ctx, and the GTK main loop is single-threaded — an idle that sees
 * destroying==0 cannot be racing with destroy().
 */
typedef struct {
    gint   ref;          /* atomic reference count                             */
    gint   destroying;   /* atomic bool — set before ctx is freed              */
    gint   gen;          /* atomic generation counter — bumped on each cancel  */
    void  *ctx;          /* gentoo_ctx_t* — valid while !destroying            */
} scan_gate_t;

static scan_gate_t *gate_new(void)
{
    scan_gate_t *g = g_slice_new0(scan_gate_t);
    g_atomic_int_set(&g->ref, 1);
    return g;
}
static void gate_ref  (scan_gate_t *g) { g_atomic_int_inc(&g->ref); }
static void gate_unref(scan_gate_t *g)
{
    if (g_atomic_int_dec_and_test(&g->ref))
        g_slice_free(scan_gate_t, g);
}

/* ── Per-scan heap-allocated result posted via g_idle_add ─────────── */
typedef struct {
    scan_gate_t      *gate;   /* ref — holds ctx alive while !destroying */
    gint              gen;    /* generation at which this scan was launched */
    portage_result_t *res;    /* heap-allocated; idle either applies or frees */
} scan_idle_t;

typedef struct {
    gboolean found;
    char cat[256];
    char pvr[256];
} pkg_ref_t;

/* ── Plugin instance context ────────────────────────────────────── */

typedef struct {
    /* ── widgets ── */
    GtkWidget     *scroll;
    GtkWidget     *root_box;
    GtkWidget     *status_label;   /* placeholder / "Searching…" */

    GtkWidget     *info_box;       /* shown only when a match is found */
    GtkWidget     *pkg_grid;
    GtkLabel      *atom_val;
    GtkLabel      *repo_val;
    GtkLabel      *slot_val;
    GtkLabel      *eapi_val;
    GtkLabel      *license_val;
    GtkLabel      *homepage_val;
    GtkLabel      *keywords_val;
    GtkLabel      *size_val;
    GtkLabel      *buildtime_val;

    GtkWidget     *use_frame;
    GtkWidget     *use_flow;       /* GtkFlowBox of per-flag GtkLabels   */

    GtkWidget     *rdep_frame;
    GtkTextBuffer *rdep_buf;

    /* ── state ── */
    pid_t          last_pid;

    /* ── async scan gate (shared with worker and idle callbacks) ── */
    scan_gate_t   *gate;

    GMutex         worker_lock;
    GCond          worker_cond;
    GThread       *worker;
    gboolean       worker_ready;
    gboolean       worker_stop;
    gboolean       worker_pending;
    pid_t          worker_pid;

    GHashTable    *lookup_cache;  /* exe path -> pkg_ref_t, including misses */
} gentoo_ctx_t;

/* ── Portage database helpers ───────────────────────────────────── */

/*
 * Read one file from /var/db/pkg/<cat>/<pvr>/<file> into buf.
 * Strips trailing whitespace.  On failure buf is set to "".
 */
static void portage_read(const char *cat, const char *pvr, const char *file,
                          char *buf, size_t bufsz)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/var/db/pkg/%s/%s/%s", cat, pvr, file);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return; }
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && (unsigned char)buf[n - 1] <= ' ')
        buf[--n] = '\0';
}

static void read_build_time(const char *cat, const char *pvr,
                              char *buf, size_t bufsz)
{
    char raw[32];
    portage_read(cat, pvr, "BUILD_TIME", raw, sizeof(raw));
    if (!raw[0]) { snprintf(buf, bufsz, "—"); return; }
    time_t t = (time_t)atoll(raw);
    if (t <= 0) { snprintf(buf, bufsz, "—"); return; }
    struct tm tm_buf;
    struct tm *tm = localtime_r(&t, &tm_buf);
    if (!tm) { snprintf(buf, bufsz, "—"); return; }
    strftime(buf, bufsz, "%Y-%m-%d %H:%M", tm);
}

static void read_size(const char *cat, const char *pvr,
                       char *buf, size_t bufsz)
{
    char raw[32];
    portage_read(cat, pvr, "SIZE", raw, sizeof(raw));
    if (!raw[0]) { snprintf(buf, bufsz, "—"); return; }
    long long sz = atoll(raw);
    if      (sz <= 0)           snprintf(buf, bufsz, "—");
    else if (sz < 1024)         snprintf(buf, bufsz, "%lld B",   sz);
    else if (sz < 1048576)      snprintf(buf, bufsz, "%.1f KiB", (double)sz / 1024.0);
    else if (sz < 1073741824)   snprintf(buf, bufsz, "%.1f MiB", (double)sz / 1048576.0);
    else                        snprintf(buf, bufsz, "%.2f GiB", (double)sz / 1073741824.0);
}

/* Forward declaration */
static gboolean apply_scan_result(gpointer data);

static gboolean worker_should_cancel(gentoo_ctx_t *ctx, gint gen)
{
    gboolean cancel;
    g_mutex_lock(&ctx->worker_lock);
    cancel = ctx->worker_stop || ctx->worker_pending;
    g_mutex_unlock(&ctx->worker_lock);
    return cancel || g_atomic_int_get(&ctx->gate->gen) != gen;
}

static int contents_has_exe(gentoo_ctx_t *ctx, gint gen,
                            const char *cat, const char *pvr,
                            const char *exe_path, gboolean *cancelled)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/var/db/pkg/%s/%s/CONTENTS", cat, pvr);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[8192];
    size_t elen = strlen(exe_path);
    unsigned nlines = 0;
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        if ((++nlines & 127u) == 0 && worker_should_cancel(ctx, gen)) {
            *cancelled = TRUE;
            break;
        }

        const char *p = NULL;
        if      (strncmp(line, "obj ", 4) == 0) p = line + 4;
        else if (strncmp(line, "sym ", 4) == 0) p = line + 4;
        if (!p) continue;

        if (strncmp(p, exe_path, elen) == 0) {
            char c = p[elen];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0') {
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

static gboolean find_package_for_exe(gentoo_ctx_t *ctx, gint gen,
                                     const char *exe_path, pkg_ref_t *out)
{
    memset(out, 0, sizeof(*out));

    DIR *catdir = opendir("/var/db/pkg");
    if (!catdir)
        return TRUE;

    gboolean completed = TRUE;
    struct dirent *c;
    while (!worker_should_cancel(ctx, gen) && (c = readdir(catdir))) {
        if (c->d_name[0] == '.') continue;

        char cat_path[PATH_MAX];
        snprintf(cat_path, sizeof(cat_path), "/var/db/pkg/%s", c->d_name);
        struct stat st;
        if (stat(cat_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        DIR *pkgdir = opendir(cat_path);
        if (!pkgdir) continue;

        struct dirent *p;
        while (!worker_should_cancel(ctx, gen) && (p = readdir(pkgdir))) {
            if (p->d_name[0] == '.') continue;

            gboolean cancelled = FALSE;
            if (contents_has_exe(ctx, gen, c->d_name, p->d_name,
                                 exe_path, &cancelled)) {
                out->found = TRUE;
                snprintf(out->cat, sizeof(out->cat), "%s", c->d_name);
                snprintf(out->pvr, sizeof(out->pvr), "%s", p->d_name);
                closedir(pkgdir);
                closedir(catdir);
                return TRUE;
            }
            if (cancelled) {
                completed = FALSE;
                break;
            }
        }
        closedir(pkgdir);
        if (!completed)
            break;
    }
    if (worker_should_cancel(ctx, gen))
        completed = FALSE;

    closedir(catdir);
    return completed;
}

static void read_package_metadata(const pkg_ref_t *ref, portage_result_t *res)
{
    if (!ref->found)
        return;

    snprintf(res->cat, sizeof(res->cat), "%s", ref->cat);
    snprintf(res->pvr, sizeof(res->pvr), "%s", ref->pvr);
    snprintf(res->pkg_atom, sizeof(res->pkg_atom), "%s/%s", ref->cat, ref->pvr);

    portage_read(ref->cat, ref->pvr, "SLOT",
                 res->slot,       sizeof(res->slot));
    portage_read(ref->cat, ref->pvr, "repository",
                 res->repo,       sizeof(res->repo));
    portage_read(ref->cat, ref->pvr, "LICENSE",
                 res->license,    sizeof(res->license));
    portage_read(ref->cat, ref->pvr, "HOMEPAGE",
                 res->homepage,   sizeof(res->homepage));
    portage_read(ref->cat, ref->pvr, "KEYWORDS",
                 res->keywords,   sizeof(res->keywords));
    portage_read(ref->cat, ref->pvr, "USE",
                 res->use_flags,  sizeof(res->use_flags));
    portage_read(ref->cat, ref->pvr, "IUSE",
                 res->iuse_flags, sizeof(res->iuse_flags));
    portage_read(ref->cat, ref->pvr, "RDEPEND",
                 res->rdepend,    sizeof(res->rdepend));
    portage_read(ref->cat, ref->pvr, "EAPI",
                 res->eapi,       sizeof(res->eapi));
    read_build_time(ref->cat, ref->pvr,
                    res->build_time_str, sizeof(res->build_time_str));
    read_size(ref->cat, ref->pvr, res->size_str, sizeof(res->size_str));
    res->found = 1;
}

static void resolve_proc_exe(pid_t pid, char *exe_path, size_t exe_path_sz)
{
    exe_path[0] = '\0';

    char proc_exe[64];
    snprintf(proc_exe, sizeof(proc_exe), "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(proc_exe, exe_path, exe_path_sz - 1);
    if (n <= 0)
        return;

    exe_path[n] = '\0';
    static const char deleted_sfx[] = " (deleted)";
    size_t elen = (size_t)n, dlen = sizeof(deleted_sfx) - 1;
    if (elen > dlen && strcmp(exe_path + elen - dlen, deleted_sfx) == 0)
        exe_path[elen - dlen] = '\0';
}

static gpointer portage_worker_thread(gpointer data)
{
    gentoo_ctx_t *ctx = data;

    for (;;) {
        pid_t pid;
        gint gen;

        g_mutex_lock(&ctx->worker_lock);
        while (!ctx->worker_stop && !ctx->worker_pending)
            g_cond_wait(&ctx->worker_cond, &ctx->worker_lock);
        if (ctx->worker_stop) {
            g_mutex_unlock(&ctx->worker_lock);
            break;
        }
        pid = ctx->worker_pid;
        gen = g_atomic_int_get(&ctx->gate->gen);
        ctx->worker_pending = FALSE;
        g_mutex_unlock(&ctx->worker_lock);

        char exe_path[PATH_MAX];
        resolve_proc_exe(pid, exe_path, sizeof(exe_path));

        portage_result_t *res = g_slice_new0(portage_result_t);
        if (exe_path[0]) {
            pkg_ref_t *ref = g_hash_table_lookup(ctx->lookup_cache, exe_path);
            if (!ref) {
                pkg_ref_t found_ref;
                if (find_package_for_exe(ctx, gen, exe_path, &found_ref)) {
                    ref = g_new(pkg_ref_t, 1);
                    *ref = found_ref;
                    g_hash_table_insert(ctx->lookup_cache, g_strdup(exe_path), ref);
                }
            }
            if (ref && !worker_should_cancel(ctx, gen))
                read_package_metadata(ref, res);
        }

        if (!worker_should_cancel(ctx, gen) &&
            !g_atomic_int_get(&ctx->gate->destroying) &&
            g_atomic_int_get(&ctx->gate->gen) == gen) {
            scan_idle_t *idle = g_slice_new(scan_idle_t);
            idle->gate = ctx->gate;
            gate_ref(ctx->gate);
            idle->gen = gen;
            idle->res = res;
            g_idle_add(apply_scan_result, idle);
        } else {
            g_slice_free(portage_result_t, res);
        }
    }
    return NULL;
}

/* ── Cancel/coalesce the current scan (non-blocking, O(1)) ───────── */

/*
 * Bumps the gate's generation counter, which atomically invalidates any
 * in-flight worker result and any queued idle callback for the old generation.
 */
static void cancel_scan(gentoo_ctx_t *ctx)
{
    g_atomic_int_inc(&ctx->gate->gen);
}

/* ── USE-flag flow box ───────────────────────────────────────────── */

/*
 * Rebuild the GtkFlowBox with one label per IUSE flag.
 *
 * Colour coding (Pango markup):
 *   green  bold     – flag is in USE (was active at install time)
 *   orange small    – flag has IUSE default-on (+prefix) but was not set
 *   grey   small    – flag is default-off and was not set
 */
static void rebuild_use_flags(gentoo_ctx_t *ctx, const portage_result_t *res)
{
    /* Remove previous children */
    GList *children = gtk_container_get_children(GTK_CONTAINER(ctx->use_flow));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    if (!res->iuse_flags[0]) return;

    /* Build a hash-set of the active USE flags for O(1) lookup */
    GHashTable *active = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    {
        char *copy = g_strdup(res->use_flags);
        char *save = NULL;
        for (char *t = strtok_r(copy, " \t\n", &save);
             t; t = strtok_r(NULL, " \t\n", &save))
            g_hash_table_add(active, g_strdup(t));
        g_free(copy);
    }

    /* Walk IUSE maintaining the ebuild-declared order */
    char *copy = g_strdup(res->iuse_flags);
    char *save = NULL;
    for (char *t = strtok_r(copy, " \t\n", &save);
         t; t = strtok_r(NULL, " \t\n", &save)) {

        const char *flag  = t;
        gboolean    def_on = FALSE;
        if      (flag[0] == '+') { flag++; def_on = TRUE; }
        else if (flag[0] == '-') { flag++; }

        gboolean is_set = g_hash_table_contains(active, flag);

        char markup[256];
        if (is_set)
            /* green + bold — enabled at install time */
            snprintf(markup, sizeof(markup),
                     "<b><span foreground='#4CAF50'>%s</span></b>", flag);
        else if (def_on)
            /* orange — ebuild default-on but disabled at install time */
            snprintf(markup, sizeof(markup),
                     "<span foreground='#FF8C00' size='small'>-%s</span>", flag);
        else
            /* grey — default-off and not set */
            snprintf(markup, sizeof(markup),
                     "<span foreground='#888888' size='small'>-%s</span>", flag);

        GtkWidget *lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl), markup);
        gtk_widget_set_tooltip_text(lbl,
            is_set  ? "Enabled at install time" :
            def_on  ? "Default-on in ebuild; disabled at install" :
                      "Not enabled at install time");

        gtk_container_add(GTK_CONTAINER(ctx->use_flow), lbl);
    }
    g_free(copy);
    g_hash_table_destroy(active);

    gtk_widget_show_all(ctx->use_flow);
}

/* ── Apply result on the GTK main thread (g_idle_add callback) ─── */

static gboolean apply_scan_result(gpointer data)
{
    scan_idle_t *idle = data;

    /*
     * Gate check — two cases discard the result without touching ctx:
     *   1. gate->destroying == 1: ctx has been freed.
     *   2. gate->gen != idle->gen: a newer scan has invalidated this one.
     * Both checks are atomic reads; no mutex needed.
     */
    if (g_atomic_int_get(&idle->gate->destroying) ||
        g_atomic_int_get(&idle->gate->gen) != idle->gen) {
        g_slice_free(portage_result_t, idle->res);
        gate_unref(idle->gate);
        g_slice_free(scan_idle_t, idle);
        return G_SOURCE_REMOVE;
    }

    /* ctx is alive — the invariant holds because destroying == 0
     * and we are on the single-threaded GTK main loop.
     */
    gentoo_ctx_t     *ctx = idle->gate->ctx;
    const portage_result_t *res = idle->res;

    if (!res->found) {
        gtk_label_set_text(GTK_LABEL(ctx->status_label),
                           "Not found in the Portage package database.");
        gtk_widget_show(ctx->status_label);
        gtk_widget_hide(ctx->info_box);
        g_slice_free(portage_result_t, idle->res);
        gate_unref(idle->gate);
        g_slice_free(scan_idle_t, idle);
        return G_SOURCE_REMOVE;
    }

    /* Populate package info labels */
    gtk_label_set_text(ctx->atom_val,
                       res->pkg_atom[0]       ? res->pkg_atom       : "—");
    gtk_label_set_text(ctx->repo_val,
                       res->repo[0]           ? res->repo           : "—");
    gtk_label_set_text(ctx->slot_val,
                       res->slot[0]           ? res->slot           : "0");
    gtk_label_set_text(ctx->eapi_val,
                       res->eapi[0]           ? res->eapi           : "—");
    gtk_label_set_text(ctx->license_val,
                       res->license[0]        ? res->license        : "—");
    gtk_label_set_text(ctx->keywords_val,
                       res->keywords[0]       ? res->keywords       : "—");
    gtk_label_set_text(ctx->size_val,
                       res->size_str[0]       ? res->size_str       : "—");
    gtk_label_set_text(ctx->buildtime_val,
                       res->build_time_str[0] ? res->build_time_str : "—");

    if (res->homepage[0]) {
        gtk_label_set_text(ctx->homepage_val, res->homepage);
        gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->homepage_val), res->homepage);
    } else {
        gtk_label_set_text(ctx->homepage_val, "—");
    }

    /* USE flags section */
    if (res->iuse_flags[0]) {
        rebuild_use_flags(ctx, res);
        gtk_widget_show(ctx->use_frame);
    } else {
        gtk_widget_hide(ctx->use_frame);
    }

    /* RDEPEND section */
    if (res->rdepend[0]) {
        gtk_text_buffer_set_text(ctx->rdep_buf, res->rdepend, -1);
        gtk_widget_show(ctx->rdep_frame);
    } else {
        gtk_widget_hide(ctx->rdep_frame);
    }

    gtk_widget_hide(ctx->status_label);
    gtk_widget_show(ctx->info_box);

    g_slice_free(portage_result_t, idle->res);
    gate_unref(idle->gate);
    g_slice_free(scan_idle_t, idle);
    return G_SOURCE_REMOVE;
}

/* ── Widget construction ─────────────────────────────────────────── */

static GtkLabel *add_row(GtkGrid *grid, int row, const char *key)
{
    GtkWidget *k = gtk_label_new(key);
    gtk_widget_set_halign(k, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(k), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
    gtk_grid_attach(grid, k, 0, row, 1, 1);

    GtkWidget *v = gtk_label_new("—");
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(v), 0.0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 60);
    gtk_grid_attach(grid, v, 1, row, 1, 1);

    return GTK_LABEL(v);
}

static GtkWidget *gentoo_create_widget(void *opaque)
{
    gentoo_ctx_t *ctx = opaque;

    /* Gate is created alongside the widget so the worker can start immediately */
    ctx->gate      = gate_new();
    ctx->gate->ctx = ctx;
    g_mutex_init(&ctx->worker_lock);
    g_cond_init(&ctx->worker_cond);
    ctx->lookup_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);
    ctx->worker_ready = TRUE;
    ctx->worker = g_thread_new("portage-worker", portage_worker_thread, ctx);

    /* Outer scrolled window */
    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    ctx->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(ctx->root_box, 8);
    gtk_widget_set_margin_end(ctx->root_box, 8);
    gtk_widget_set_margin_top(ctx->root_box, 6);
    gtk_widget_set_margin_bottom(ctx->root_box, 6);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), ctx->root_box);

    /* Status / placeholder label at the top */
    ctx->status_label = gtk_label_new(
        "Select a process to look up its Portage package.");
    gtk_widget_set_halign(ctx->status_label, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(ctx->status_label),
                                "dim-label");
    gtk_box_pack_start(GTK_BOX(ctx->root_box), ctx->status_label,
                       FALSE, FALSE, 0);

    /* ── Info box (hidden until a match is found) ── */
    ctx->info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(ctx->root_box), ctx->info_box, TRUE, TRUE, 0);

    /* Package metadata grid */
    ctx->pkg_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(ctx->pkg_grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(ctx->pkg_grid), 2);
    gtk_box_pack_start(GTK_BOX(ctx->info_box), ctx->pkg_grid, FALSE, FALSE, 0);

    int r = 0;
    ctx->atom_val      = add_row(GTK_GRID(ctx->pkg_grid), r++, "Package");
    ctx->repo_val      = add_row(GTK_GRID(ctx->pkg_grid), r++, "Repository");
    ctx->slot_val      = add_row(GTK_GRID(ctx->pkg_grid), r++, "SLOT");
    ctx->eapi_val      = add_row(GTK_GRID(ctx->pkg_grid), r++, "EAPI");
    ctx->license_val   = add_row(GTK_GRID(ctx->pkg_grid), r++, "License");
    ctx->homepage_val  = add_row(GTK_GRID(ctx->pkg_grid), r++, "Homepage");
    ctx->keywords_val  = add_row(GTK_GRID(ctx->pkg_grid), r++, "Keywords");
    ctx->size_val      = add_row(GTK_GRID(ctx->pkg_grid), r++, "Installed size");
    ctx->buildtime_val = add_row(GTK_GRID(ctx->pkg_grid), r++, "Built");

    /* ── USE flags section ── */
    ctx->use_frame = gtk_frame_new("USE flags");
    gtk_frame_set_shadow_type(GTK_FRAME(ctx->use_frame), GTK_SHADOW_NONE);
    gtk_box_pack_start(GTK_BOX(ctx->info_box), ctx->use_frame, FALSE, FALSE, 0);

    ctx->use_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ctx->use_flow),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(ctx->use_flow), 4);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ctx->use_flow), 16);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(ctx->use_flow), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(ctx->use_flow), 6);
    gtk_widget_set_margin_start(ctx->use_flow, 4);
    gtk_widget_set_margin_top(ctx->use_flow, 4);
    gtk_container_add(GTK_CONTAINER(ctx->use_frame), ctx->use_flow);

    /* ── RDEPEND section ── */
    ctx->rdep_frame = gtk_frame_new("Runtime Dependencies (RDEPEND)");
    gtk_frame_set_shadow_type(GTK_FRAME(ctx->rdep_frame), GTK_SHADOW_NONE);
    gtk_box_pack_start(GTK_BOX(ctx->info_box), ctx->rdep_frame, TRUE, TRUE, 0);

    GtkWidget *rdep_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rdep_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(rdep_scroll), 80);
    gtk_container_add(GTK_CONTAINER(ctx->rdep_frame), rdep_scroll);

    GtkWidget *rdep_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(rdep_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(rdep_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(rdep_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_margin_start(rdep_view, 4);
    gtk_widget_set_margin_top(rdep_view, 4);
    ctx->rdep_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(rdep_view));
    gtk_container_add(GTK_CONTAINER(rdep_scroll), rdep_view);

    /* Initial visibility */
    gtk_widget_show_all(ctx->scroll);
    gtk_widget_hide(ctx->info_box);
    gtk_widget_hide(ctx->use_frame);
    gtk_widget_hide(ctx->rdep_frame);

    return ctx->scroll;
}

/* ── update ──────────────────────────────────────────────────────── */

static void gentoo_update(void *opaque, const evemon_proc_data_t *data)
{
    gentoo_ctx_t *ctx = opaque;

    if (!ctx->scroll || !GTK_IS_WIDGET(ctx->scroll) ||
        !gtk_widget_get_mapped(ctx->scroll) ||
        !gtk_widget_get_child_visible(ctx->scroll)) return;
    if (data->pid == ctx->last_pid) return;  /* same PID — already displayed */

    ctx->last_pid = data->pid;

    /*
     * Invalidate any queued result, then coalesce the worker to the latest
     * selected PID.  The worker serializes all Portage IO and caches completed
     * executable lookups, so rapid selection changes cannot launch parallel scans.
     */
    cancel_scan(ctx);

    gtk_label_set_text(GTK_LABEL(ctx->status_label),
                       "Searching Portage database…");
    gtk_widget_show(ctx->status_label);
    gtk_widget_hide(ctx->info_box);

    g_mutex_lock(&ctx->worker_lock);
    ctx->worker_pid = data->pid;
    ctx->worker_pending = TRUE;
    g_cond_signal(&ctx->worker_cond);
    g_mutex_unlock(&ctx->worker_lock);
}

/* ── clear ───────────────────────────────────────────────────────── */

static void gentoo_clear(void *opaque)
{
    gentoo_ctx_t *ctx = opaque;

    cancel_scan(ctx);           /* non-blocking: bumps gen, nothing waits */
    ctx->last_pid = 0;

    gtk_label_set_text(GTK_LABEL(ctx->status_label),
                       "Select a process to look up its Portage package.");
    gtk_widget_show(ctx->status_label);
    gtk_widget_hide(ctx->info_box);
}

/* ── wants_update ────────────────────────────────────────────────── */

static int gentoo_wants_update(void *opaque)
{
    gentoo_ctx_t *ctx = opaque;
    return ctx->scroll &&
           GTK_IS_WIDGET(ctx->scroll) &&
           gtk_widget_get_mapped(ctx->scroll) &&
           gtk_widget_get_child_visible(ctx->scroll);
}

/* ── destroy ─────────────────────────────────────────────────────── */

/*
 * Destroy:
 *   1. Set gate->destroying = 1 BEFORE freeing ctx.  Any idle callback
 *      that fires afterwards will see this flag and will not dereference ctx.
 *   2. Bump gen (via cancel_scan) to invalidate any in-flight worker result.
 *   3. Drop the ctx's ref on the gate.  The gate itself may outlive ctx if
 *      an idle still holds a ref; it will see destroying==1 and skip ctx.
 */
static void gentoo_destroy(void *opaque)
{
    gentoo_ctx_t *ctx = opaque;

    if (ctx->gate) {
        g_atomic_int_set(&ctx->gate->destroying, 1);
        cancel_scan(ctx);
    }

    if (ctx->worker_ready && ctx->worker) {
        g_mutex_lock(&ctx->worker_lock);
        ctx->worker_stop = TRUE;
        g_cond_signal(&ctx->worker_cond);
        g_mutex_unlock(&ctx->worker_lock);
        g_thread_join(ctx->worker);
        ctx->worker = NULL;
    }

    if (ctx->lookup_cache)
        g_hash_table_destroy(ctx->lookup_cache);

    if (ctx->worker_ready) {
        g_cond_clear(&ctx->worker_cond);
        g_mutex_clear(&ctx->worker_lock);
    }

    if (ctx->gate)
        gate_unref(ctx->gate);   /* ctx drops its ref; queued idles may keep it alive */

    g_slice_free(gentoo_ctx_t, ctx);
}

/* ── Plugin init — Gentoo gate ───────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    /*
     * Load gate: only activate on Gentoo (or any system with a Portage
     * package database).  On Fedora, Debian, Arch, etc. /var/db/pkg/
     * will not exist and this plugin returns NULL — the loader skips it
     * without any error and no tab is added.
     */
    struct stat st;
    if (stat("/var/db/pkg", &st) != 0 || !S_ISDIR(st.st_mode))
        return NULL;

    gentoo_ctx_t *ctx = g_slice_new0(gentoo_ctx_t);
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { g_slice_free(gentoo_ctx_t, ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Gentoo / Portage",
        .id            = "org.evemon.gentoo",
        .version       = "1.0",
        .data_needs    = 0,    /* we read /proc/<pid>/exe ourselves */
        .plugin_ctx    = ctx,
        .create_widget = gentoo_create_widget,
        .update        = gentoo_update,
        .clear         = gentoo_clear,
        .wants_update  = gentoo_wants_update,
        .destroy       = gentoo_destroy,
        .role          = EVEMON_ROLE_PROCESS,
        .dependencies  = NULL,
    };

    return p;
}
