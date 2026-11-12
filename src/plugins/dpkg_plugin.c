/*
 * dpkg_plugin.c – Debian/Ubuntu package info plugin for evemon.
 *
 * For the selected process, reads /proc/<pid>/exe to get the running
 * binary, finds the dpkg package that owns it and displays:
 *
 *   • Package, version, architecture and source package
 *   • The APT repositories carrying the installed version, as apt-cache
 *     policy reports them (suite/component, PPA, or "local")
 *   • Section, priority, maintainer, homepage and license (from the
 *     package's machine-readable copyright file)
 *   • Installed size, install time and whether it was installed by hand
 *     or pulled in automatically as a dependency
 *   • Package flags (essential, protected, Multi-Arch, install reason)
 *   • Description and dependencies (Depends, Pre-Depends, Recommends,
 *     Suggests)
 *
 * Snaps (Firefox and others on Ubuntu) are recognized from their
 * /snap/<name>/<revision>/ path and described from meta/snap.yaml.
 *
 * The lookup itself lives in the core (dpkg_lookup.c).  On the monitored
 * host the worker calls it directly.  For a remote Evemon it sends a DPKG
 * request, and that Evemon runs the same lookup against its own /proc and
 * dpkg database (docs/protocol.md).
 *
 * Load-gated: evemon_plugin_init() returns NULL unless the monitored host
 * has a populated /var/lib/dpkg/status (a remote Evemon advertises this as
 * the DPKG capability), so the tab only appears for dpkg systems.
 *
 * Async model — identical to the Gentoo plugin; the GTK main thread never
 * blocks on package database or socket IO:
 *
 *   • A single worker thread serializes all lookups.
 *   • Process selection is coalesced to the latest PID.
 *   • cancel_scan() is O(1): it bumps an atomic generation counter.
 */

#include "../ipc/query.h"
#include "../ipc/query_dpkg.h"
#include "evemon_plugin.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* A failed remote lookup is retried this many times, a little later each time. */
#define DPKG_RETRIES 3

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.dpkg",
    "Debian / APT",
    "1.0",
    EVEMON_ROLE_PROCESS,
    NULL
);

/* ── Scan gate (shared with the worker and queued idle callbacks) ── */

typedef struct {
    gint   ref;          /* atomic reference count                             */
    gint   destroying;   /* atomic bool — set before ctx is freed              */
    gint   gen;          /* atomic generation counter — bumped on each cancel  */
    void  *ctx;          /* dpkg_ctx_t* — valid while !destroying              */
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

typedef struct {
    scan_gate_t   *gate;
    gint           gen;
    int            error;    /* errno of a failed remote request, else 0 */
    dpkg_result_t *res;
} scan_idle_t;

/* ── Plugin instance context ────────────────────────────────────── */

#define DPKG_ROWS         12
#define DPKG_DEP_COLUMNS  4    /* Pre-Depends, Depends, Recommends, Suggests */

typedef struct {
    GtkWidget     *scroll;
    GtkWidget     *root_box;
    GtkWidget     *status_label;

    GtkWidget     *info_box;
    GtkLabel      *name_label;
    GtkLabel      *detail_label;     /* version · architecture             */
    GtkLabel      *flags_label;
    GtkLabel      *summary_label;
    GtkLabel      *row_key[DPKG_ROWS];
    GtkLabel      *row_val[DPKG_ROWS];
    GtkWidget     *desc_column;
    GtkLabel      *desc_label;
    GtkWidget     *dep_column[DPKG_DEP_COLUMNS];
    GtkLabel      *dep_title[DPKG_DEP_COLUMNS];
    GtkLabel      *dep_items[DPKG_DEP_COLUMNS];

    pid_t          last_pid;
    scan_gate_t   *gate;

    GMutex         worker_lock;
    GCond          worker_cond;
    GThread       *worker;
    gboolean       worker_ready;
    gboolean       worker_stop;
    gboolean       worker_pending;
    pid_t          worker_pid;

    gboolean       remote;       /* ask the monitored host's Evemon         */
    char           remote_path[108];
    int            wake[2];      /* readable = abandon the remote request   */
    int            failures;     /* failed remote lookups of last_pid       */
    gint64         retry_at;     /* monotonic time of the next attempt, or 0 */
} dpkg_ctx_t;

static gboolean apply_scan_result(gpointer data);

typedef struct {
    dpkg_ctx_t *ctx;
    gint        gen;
} cancel_check_t;

static gboolean worker_should_cancel(gpointer user_data)
{
    cancel_check_t *check = user_data;
    gboolean cancel;
    g_mutex_lock(&check->ctx->worker_lock);
    cancel = check->ctx->worker_stop || check->ctx->worker_pending;
    g_mutex_unlock(&check->ctx->worker_lock);
    return cancel || g_atomic_int_get(&check->ctx->gate->gen) != check->gen;
}

static gpointer dpkg_worker_thread(gpointer data)
{
    dpkg_ctx_t *ctx = data;

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

        cancel_check_t check = { ctx, gen };
        dpkg_result_t *res = g_slice_new0(dpkg_result_t);
        gboolean completed = TRUE;
        int error = 0;
        if (ctx->remote) {
            /* Forget wake-ups meant for earlier requests.  A cancellation that
             * raced with this drain has already moved the generation on. */
            char stale[64];
            while (ctx->wake[0] >= 0 && read(ctx->wake[0], stale, sizeof(stale)) > 0) {}
            if (worker_should_cancel(&check))
                completed = FALSE;
            else if (query_dpkg_fetch(ctx->remote_path, pid, NULL, 0, res, ctx->wake[0]))
                error = errno ? errno : EPROTO;
        } else {
            completed = dpkg_lookup_pid(pid, res, worker_should_cancel, &check);
        }

        if (completed && !worker_should_cancel(&check) &&
            !g_atomic_int_get(&ctx->gate->destroying)) {
            scan_idle_t *idle = g_slice_new(scan_idle_t);
            idle->gate = ctx->gate;
            gate_ref(ctx->gate);
            idle->gen = gen;
            idle->error = error;
            idle->res = res;
            g_idle_add(apply_scan_result, idle);
        } else {
            g_slice_free(dpkg_result_t, res);
        }
    }
    return NULL;
}

static void cancel_scan(dpkg_ctx_t *ctx)
{
    g_atomic_int_inc(&ctx->gate->gen);
    /* A remote request can wait a long time on a cold package database. */
    if (ctx->wake[1] >= 0) {
        char wake = 1;
        ssize_t written = write(ctx->wake[1], &wake, 1);
        (void)written;
    }
}

/* ── Presentation ────────────────────────────────────────────────── */

static void set_row(dpkg_ctx_t *ctx, int *row, const char *key, const char *value)
{
    if (*row >= DPKG_ROWS) return;
    gtk_label_set_text(ctx->row_key[*row], key);
    gtk_label_set_text(ctx->row_val[*row], value && value[0] ? value : "—");
    gtk_widget_show(GTK_WIDGET(ctx->row_key[*row]));
    gtk_widget_show(GTK_WIDGET(ctx->row_val[*row]));
    (*row)++;
}

static void set_markup(GtkLabel *label, const char *format, const char *text)
{
    gchar *escaped = g_markup_escape_text(text, -1);
    gchar *markup = g_strdup_printf(format, escaped);
    gtk_label_set_markup(label, markup);
    g_free(markup);
    g_free(escaped);
}

typedef enum { FLAG_PLAIN, FLAG_GOOD, FLAG_NOTICE } flag_tone_t;

/* Flags sit inline in the header; explanations go in its tooltip. */
static void add_flag(GString *markup, GString *tips, const char *text,
                     flag_tone_t tone, const char *tooltip)
{
    gchar *escaped = g_markup_escape_text(text, -1);
    if (markup->len)
        g_string_append(markup, "<span foreground='#888888'> · </span>");
    if (tone == FLAG_GOOD)
        g_string_append_printf(markup, "<b><span foreground='#4CAF50'>%s</span></b>", escaped);
    else if (tone == FLAG_NOTICE)
        g_string_append_printf(markup, "<b><span foreground='#FF8C00'>%s</span></b>", escaped);
    else
        g_string_append_printf(markup, "<span foreground='#888888'>%s</span>", escaped);
    g_free(escaped);
    g_string_append_printf(tips, "%s%s: %s", tips->len ? "\n" : "", text, tooltip);
}

static void set_flags(dpkg_ctx_t *ctx, const dpkg_result_t *res)
{
    GString *markup = g_string_new(NULL), *tips = g_string_new(NULL);
    if (res->snap) {
        gboolean strict = strcmp(res->confinement, "strict") == 0;
        add_flag(markup, tips, res->confinement, strict ? FLAG_GOOD : FLAG_NOTICE,
                 strict ? "Runs inside the snap sandbox"
                        : "Runs with full system access, outside the snap sandbox");
        if (res->grade[0])
            add_flag(markup, tips, res->grade, strcmp(res->grade, "stable") == 0 ? FLAG_PLAIN : FLAG_NOTICE,
                     "Publisher's quality grade for this revision");
    } else {
        if (res->automatic == 0)
            add_flag(markup, tips, "manually installed", FLAG_GOOD, "Installed on request, not as a dependency");
        else if (res->automatic == 1)
            add_flag(markup, tips, "automatic", FLAG_PLAIN,
                     "Installed as a dependency; apt autoremove removes it once nothing needs it");
        if (res->essential)
            add_flag(markup, tips, "essential", FLAG_NOTICE, "The system expects it to always be installed");
        if (res->protected_pkg)
            add_flag(markup, tips, "protected", FLAG_NOTICE, "Removal needs explicit confirmation");
        if (res->priority[0])
            add_flag(markup, tips, res->priority,
                     strcmp(res->priority, "required") == 0 || strcmp(res->priority, "important") == 0
                         ? FLAG_NOTICE : FLAG_PLAIN,
                     "Debian priority");
        if (res->multi_arch[0]) {
            char text[64];
            snprintf(text, sizeof(text), "Multi-Arch: %s", res->multi_arch);
            add_flag(markup, tips, text, FLAG_PLAIN, "How this package coexists with other architectures");
        }
    }
    gtk_label_set_markup(ctx->flags_label, markup->str);
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->flags_label), tips->len ? tips->str : NULL);
    if (markup->len) gtk_widget_show(GTK_WIDGET(ctx->flags_label));
    else gtk_widget_hide(GTK_WIDGET(ctx->flags_label));
    g_string_free(markup, TRUE);
    g_string_free(tips, TRUE);
}

/*
 * One column per relationship.  `depends` is laid out by dpkg_lookup.c as
 * "Title:" lines, each followed by "  item" lines.
 */
static void set_dependencies(dpkg_ctx_t *ctx, const char *depends)
{
    int column = 0;
    GString *items = NULL;
    gchar **lines = g_strsplit(depends, "\n", -1);
    for (gchar **line = lines; ; line++) {
        gboolean title = *line && **line && **line != ' ';
        if ((!*line || title) && items) {
            gtk_label_set_text(ctx->dep_items[column], items->str);
            gtk_widget_show(ctx->dep_column[column++]);
            g_string_free(items, TRUE);
            items = NULL;
        }
        if (!*line) break;
        if (title && column < DPKG_DEP_COLUMNS) {
            (*line)[strcspn(*line, ":")] = '\0';
            set_markup(ctx->dep_title[column], "<b>%s</b>", *line);
            items = g_string_new(NULL);
        } else if (items && *g_strstrip(*line)) {
            g_string_append_printf(items, "%s%s", items->len ? "\n" : "", *line);
        }
    }
    g_strfreev(lines);
    for (; column < DPKG_DEP_COLUMNS; column++)
        gtk_widget_hide(ctx->dep_column[column]);
}

static void free_idle(scan_idle_t *idle)
{
    g_slice_free(dpkg_result_t, idle->res);
    gate_unref(idle->gate);
    g_slice_free(scan_idle_t, idle);
}

static gboolean apply_scan_result(gpointer data)
{
    scan_idle_t *idle = data;

    if (g_atomic_int_get(&idle->gate->destroying) ||
        g_atomic_int_get(&idle->gate->gen) != idle->gen) {
        free_idle(idle);
        return G_SOURCE_REMOVE;
    }

    dpkg_ctx_t *ctx = idle->gate->ctx;
    const dpkg_result_t *res = idle->res;

    /* The remote Evemon keeps reading what a slow lookup needs, so a later
     * attempt usually succeeds; dpkg_update() starts it. */
    gboolean retry = idle->error && ++ctx->failures <= DPKG_RETRIES;
    ctx->retry_at = retry ? g_get_monotonic_time() + ctx->failures * 2 * G_TIME_SPAN_SECOND : 0;
    if (!idle->error) ctx->failures = 0;

    if (idle->error || !res->found) {
        char message[384];
        if (idle->error)
            snprintf(message, sizeof(message), "Could not query the dpkg database on %s: %s%s",
                     query_host_name(), g_strerror(idle->error), retry ? " — retrying…" : "");
        else
            snprintf(message, sizeof(message), "%s", !res->has_exe
                     ? "No executable to look up (kernel thread, exited, or not readable by Evemon)."
                     : "Not owned by any installed package or snap.");
        gtk_label_set_text(GTK_LABEL(ctx->status_label), message);
        gtk_widget_show(ctx->status_label);
        gtk_widget_hide(ctx->info_box);
        free_idle(idle);
        return G_SOURCE_REMOVE;
    }

    int row = 0;
    char text[512];
    set_markup(ctx->name_label, "<big><b>%s</b></big>", res->name);
    if (res->snap) {
        snprintf(text, sizeof(text), "%s · revision %s · snap", res->version, res->revision);
        set_row(ctx, &row, "Repository", res->origin);
        set_row(ctx, &row, "Base", res->base);
        set_row(ctx, &row, "Confinement", res->confinement);
        set_row(ctx, &row, "License", res->license);
        set_row(ctx, &row, "Snap size", res->size_str);
        set_row(ctx, &row, "Installed", res->installed_str);
    } else {
        snprintf(text, sizeof(text), "%s · %s", res->version, res->arch);
        if (strcmp(res->source, res->name) != 0)
            set_row(ctx, &row, "Source package", res->source);
        set_row(ctx, &row, "Repository", res->origin);
        set_row(ctx, &row, "Section", res->section);
        set_row(ctx, &row, "Installed by", res->automatic == 1 ? "Automatically, as a dependency"
                                          : res->automatic == 0 ? "Manually" : "—");
        set_row(ctx, &row, "Maintainer", res->maintainer);
        if (res->original_maintainer[0])
            set_row(ctx, &row, "Debian maintainer", res->original_maintainer);
        set_row(ctx, &row, "Homepage", res->homepage);
        set_row(ctx, &row, "License", res->license);
        set_row(ctx, &row, "Installed size", res->size_str);
        set_row(ctx, &row, "Installed", res->installed_str);
    }
    gtk_label_set_text(ctx->detail_label, text);
    for (int i = row; i < DPKG_ROWS; i++) {
        gtk_widget_hide(GTK_WIDGET(ctx->row_key[i]));
        gtk_widget_hide(GTK_WIDGET(ctx->row_val[i]));
    }

    set_flags(ctx, res);

    gtk_label_set_text(ctx->summary_label, res->summary);
    if (res->summary[0]) gtk_widget_show(GTK_WIDGET(ctx->summary_label));
    else gtk_widget_hide(GTK_WIDGET(ctx->summary_label));

    gtk_label_set_text(ctx->desc_label, res->description);
    if (res->description[0]) gtk_widget_show(ctx->desc_column);
    else gtk_widget_hide(ctx->desc_column);

    set_dependencies(ctx, res->snap ? "" : res->depends);

    gtk_widget_hide(ctx->status_label);
    gtk_widget_show(ctx->info_box);
    free_idle(idle);
    return G_SOURCE_REMOVE;
}

/* ── Widget construction ─────────────────────────────────────────── */

static GtkLabel *add_label(GtkWidget *box, gboolean dim, int wrap_chars)
{
    GtkWidget *label = gtk_label_new("");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    if (wrap_chars) {
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(label), wrap_chars);
    }
    if (dim)
        gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    return GTK_LABEL(label);
}

/* A titled column in the body row; returns its content label. */
static GtkLabel *add_column(GtkWidget *row, GtkWidget **column_out, GtkLabel **title_out,
                            const char *title, int wrap_chars)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(column, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), column, FALSE, FALSE, 0);
    GtkLabel *heading = add_label(column, TRUE, 0);
    if (title) set_markup(heading, "<b>%s</b>", title);
    GtkLabel *content = add_label(column, FALSE, wrap_chars);
    gtk_label_set_selectable(content, TRUE);
    *column_out = column;
    if (title_out) *title_out = heading;
    return content;
}

static GtkWidget *dpkg_create_widget(void *opaque)
{
    dpkg_ctx_t *ctx = opaque;

    ctx->gate      = gate_new();
    ctx->gate->ctx = ctx;
    if (ctx->remote && pipe2(ctx->wake, O_CLOEXEC | O_NONBLOCK))
        ctx->wake[0] = ctx->wake[1] = -1;   /* requests then run to their timeout */
    g_mutex_init(&ctx->worker_lock);
    g_cond_init(&ctx->worker_cond);
    ctx->worker_ready = TRUE;
    ctx->worker = g_thread_new("dpkg-worker", dpkg_worker_thread, ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    ctx->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(ctx->root_box, 8);
    gtk_widget_set_margin_end(ctx->root_box, 8);
    gtk_widget_set_margin_top(ctx->root_box, 6);
    gtk_widget_set_margin_bottom(ctx->root_box, 6);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), ctx->root_box);

    ctx->status_label = GTK_WIDGET(add_label(ctx->root_box, TRUE, 0));
    gtk_label_set_text(GTK_LABEL(ctx->status_label), "Select a process to look up its package.");

    ctx->info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(ctx->root_box), ctx->info_box, FALSE, FALSE, 0);

    /* Header: name, version · architecture, flags, then the summary. */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(ctx->info_box), header, FALSE, FALSE, 0);
    ctx->name_label = add_label(header, FALSE, 0);
    gtk_label_set_selectable(ctx->name_label, TRUE);
    ctx->detail_label = add_label(header, TRUE, 0);
    gtk_label_set_selectable(ctx->detail_label, TRUE);
    ctx->flags_label = add_label(header, FALSE, 0);
    gtk_widget_set_margin_start(GTK_WIDGET(ctx->flags_label), 12);
    ctx->summary_label = add_label(ctx->info_box, TRUE, 120);

    /* Body: details, description and each relationship side by side. */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_box_pack_start(GTK_BOX(ctx->info_box), body, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_widget_set_valign(grid, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    for (int r = 0; r < DPKG_ROWS; r++) {
        GtkWidget *k = gtk_label_new("");
        gtk_widget_set_halign(k, GTK_ALIGN_START);
        gtk_widget_set_valign(k, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(k), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
        gtk_grid_attach(GTK_GRID(grid), k, 0, r, 1, 1);

        GtkWidget *v = gtk_label_new("");
        gtk_widget_set_halign(v, GTK_ALIGN_START);
        gtk_widget_set_valign(v, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(v), 0.0);
        gtk_label_set_selectable(GTK_LABEL(v), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(v), 52);
        gtk_grid_attach(GTK_GRID(grid), v, 1, r, 1, 1);

        ctx->row_key[r] = GTK_LABEL(k);
        ctx->row_val[r] = GTK_LABEL(v);
    }

    ctx->desc_label = add_column(body, &ctx->desc_column, NULL, "Description", 80);
    for (int c = 0; c < DPKG_DEP_COLUMNS; c++)
        ctx->dep_items[c] = add_column(body, &ctx->dep_column[c], &ctx->dep_title[c], NULL, 44);

    gtk_widget_show_all(ctx->scroll);
    gtk_widget_hide(ctx->info_box);

    return ctx->scroll;
}

/* ── Plugin callbacks ────────────────────────────────────────────── */

static void dpkg_update(void *opaque, const evemon_proc_data_t *data)
{
    dpkg_ctx_t *ctx = opaque;

    if (!ctx->scroll || !GTK_IS_WIDGET(ctx->scroll) ||
        !gtk_widget_get_mapped(ctx->scroll) ||
        !gtk_widget_get_child_visible(ctx->scroll)) return;
    gboolean retry = ctx->retry_at && g_get_monotonic_time() >= ctx->retry_at;
    if (data->pid == ctx->last_pid && !retry) return;

    if (data->pid != ctx->last_pid) ctx->failures = 0;
    ctx->retry_at = 0;
    ctx->last_pid = data->pid;
    cancel_scan(ctx);

    char searching[320];
    if (ctx->remote)
        snprintf(searching, sizeof(searching), "Searching the dpkg database on %s…", query_host_name());
    else
        snprintf(searching, sizeof(searching), "Searching the dpkg database…");
    gtk_label_set_text(GTK_LABEL(ctx->status_label), searching);
    gtk_widget_show(ctx->status_label);
    gtk_widget_hide(ctx->info_box);

    g_mutex_lock(&ctx->worker_lock);
    ctx->worker_pid = data->pid;
    ctx->worker_pending = TRUE;
    g_cond_signal(&ctx->worker_cond);
    g_mutex_unlock(&ctx->worker_lock);
}

static void dpkg_clear(void *opaque)
{
    dpkg_ctx_t *ctx = opaque;

    cancel_scan(ctx);
    ctx->last_pid = 0;
    ctx->failures = 0;
    ctx->retry_at = 0;

    gtk_label_set_text(GTK_LABEL(ctx->status_label), "Select a process to look up its package.");
    gtk_widget_show(ctx->status_label);
    gtk_widget_hide(ctx->info_box);
}

static int dpkg_wants_update(void *opaque)
{
    dpkg_ctx_t *ctx = opaque;
    return ctx->scroll &&
           GTK_IS_WIDGET(ctx->scroll) &&
           gtk_widget_get_mapped(ctx->scroll) &&
           gtk_widget_get_child_visible(ctx->scroll);
}

static void dpkg_destroy(void *opaque)
{
    dpkg_ctx_t *ctx = opaque;

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

    if (ctx->worker_ready) {
        g_cond_clear(&ctx->worker_cond);
        g_mutex_clear(&ctx->worker_lock);
    }

    if (ctx->wake[0] >= 0) close(ctx->wake[0]);
    if (ctx->wake[1] >= 0) close(ctx->wake[1]);

    if (ctx->gate)
        gate_unref(ctx->gate);

    g_slice_free(dpkg_ctx_t, ctx);
}

/* "Ubuntu / APT", "Debian / APT", "Linux Mint / APT" … from the monitored host's os-release. */
static const char *tab_title(gboolean remote)
{
    static char title[96];
    const char *distro = remote ? query_dpkg_remote_label() : dpkg_distro_label();
    snprintf(title, sizeof(title), "%s / APT", distro && distro[0] ? distro : "Debian");
    return title;
}

/* ── Plugin init — dpkg gate ─────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    /* Only for hosts with a populated dpkg database (Debian, Ubuntu and derivatives). */
    gboolean remote = query_is_foreign_host();
    if (remote ? !(query_remote_capabilities() & IPC_CAP_DPKG) || !query_remote_path()
               : !dpkg_available())
        return NULL;

    dpkg_ctx_t *ctx = g_slice_new0(dpkg_ctx_t);
    if (!ctx) return NULL;
    /* Capture the socket now; a host switch must never redirect this instance. */
    ctx->remote = remote;
    ctx->wake[0] = ctx->wake[1] = -1;
    if (remote)
        snprintf(ctx->remote_path, sizeof(ctx->remote_path), "%s", query_remote_path());

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { g_slice_free(dpkg_ctx_t, ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = tab_title(remote),
        .id            = "org.evemon.dpkg",
        .version       = "1.0",
        .data_needs    = 0,    /* the lookup reads /proc/<pid>/exe itself */
        .plugin_ctx    = ctx,
        .create_widget = dpkg_create_widget,
        .update        = dpkg_update,
        .clear         = dpkg_clear,
        .wants_update  = dpkg_wants_update,
        .destroy       = dpkg_destroy,
        .role          = EVEMON_ROLE_PROCESS,
        .dependencies  = NULL,
    };

    return p;
}
