#include "../evemon_plugin.h"
#include <gtksourceview/gtksource.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *_wm_ui_deps[] = { "org.evemon.write_monitor", NULL };

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.write_monitor_ui",
    "Write Log",
    "1.0",
    EVEMON_ROLE_PROCESS,
    "org.evemon.write_monitor", NULL
);

/* ── Per-line gutter data ─────────────────────────────────────── */

typedef struct {
    pid_t  pid;
    pid_t  ppid;
    char   name[32];  /* process name */
    char   ts[16];    /* "HH:MM:SS.mmm\0" */
} gutter_entry_t;

/* ── Custom GtkSourceGutterRenderer ────────────────────────────────────
 *
 * We subclass GtkSourceGutterRendererText so GTK handles sizing
 * and queuing redraws.  We override ::query_data to fill in the
 * text for each visible line from our parallel gutter_entries array.
 */

#define WM_TYPE_GUTTER_RENDERER     (wm_gutter_renderer_get_type())
#define WM_GUTTER_RENDERER(obj)     (G_TYPE_CHECK_INSTANCE_CAST((obj), WM_TYPE_GUTTER_RENDERER, WmGutterRenderer))

typedef struct _WmGutterRenderer      WmGutterRenderer;
typedef struct _WmGutterRendererClass WmGutterRendererClass;

struct _WmGutterRenderer {
    GtkSourceGutterRendererText parent;
    /* pointer back to our plugin context's entry array */
    gutter_entry_t **entries;
    size_t          *count;
};

struct _WmGutterRendererClass {
    GtkSourceGutterRendererTextClass parent_class;
};

G_DEFINE_TYPE(WmGutterRenderer, wm_gutter_renderer, GTK_SOURCE_TYPE_GUTTER_RENDERER_TEXT)

static void wm_gutter_renderer_query_data(GtkSourceGutterRenderer *renderer,
                                          GtkTextIter             *start,
                                          GtkTextIter             *end,
                                          GtkSourceGutterRendererState state)
{
    (void)end; (void)state;
    WmGutterRenderer *self = WM_GUTTER_RENDERER(renderer);

    int line = gtk_text_iter_get_line(start);
    gutter_entry_t *ent = NULL;
    if (self->entries && self->count && (size_t)line < *self->count)
        ent = &(*self->entries)[line];

    char markup[384];
    if (ent && ent->ts[0]) {
        /* "name  pid │ HH:MM:SS.mmm" */
        char name_trunc[17];
        strncpy(name_trunc, ent->name[0] ? ent->name : "[kernel]",
                sizeof(name_trunc) - 1);
        name_trunc[sizeof(name_trunc) - 1] = '\0';
        snprintf(markup, sizeof(markup),
                 "<span foreground=\"#7a8499\">%-16s</span>"
                 "<span foreground=\"#2e3a4a\">  </span>"
                 "<span foreground=\"#556070\">%7d</span>"
                 "<span foreground=\"#3d4a5c\"> \xe2\x94\x82 </span>"
                 "<span foreground=\"#7a8499\">%s</span>",
                 name_trunc, (int)ent->pid, ent->ts);
    } else {
        snprintf(markup, sizeof(markup), " ");
    }

    gtk_source_gutter_renderer_text_set_markup(
        GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer), markup, -1);
}

static void wm_gutter_renderer_class_init(WmGutterRendererClass *klass)
{
    GtkSourceGutterRendererClass *renderer_class =
        GTK_SOURCE_GUTTER_RENDERER_CLASS(klass);
    renderer_class->query_data = wm_gutter_renderer_query_data;
}

static void wm_gutter_renderer_init(WmGutterRenderer *self)
{
    (void)self;
}

static WmGutterRenderer *wm_gutter_renderer_new(gutter_entry_t **entries,
                                                 size_t          *count)
{
    WmGutterRenderer *r = g_object_new(WM_TYPE_GUTTER_RENDERER, NULL);
    r->entries = entries;
    r->count   = count;
    return r;
}

/* ── Plugin context ───────────────────────────────────────────── */

typedef struct {
    const evemon_host_services_t *hsvc;
    GtkWidget        *root;
    GtkWidget        *combo;
    GtkWidget        *check_descendants;
    GtkWidget        *check_gutter;
    GtkWidget        *view;          /* GtkScrolledWindow */
    GtkSourceView    *source_view;   /* the GtkSourceView inside */
    GtkTextBuffer    *buf;
    GtkTextMark      *end_mark;      /* gravity=right mark pinned to buffer end */
    pid_t             current_pid;
    int               subscribed_fd_stdout;
    int               subscribed_fd_stderr;
    int               include_descendants;
    /* Tracked descendant PIDs */
    pid_t            *desc_pids;
    size_t            desc_count;
    size_t            desc_cap;
    int               sub_pid_select_id;
    int               sub_fd_write_id;
    /* Gutter renderer + data */
    WmGutterRenderer *gutter_renderer;
    gutter_entry_t   *gutter_entries; /* one per line in buf */
    size_t            gutter_count;
    size_t            gutter_cap;
    int               show_gutter;
    /* Auto-scroll */
    GtkWidget        *check_autoscroll;
    int               auto_scroll;      /* 1 = follow tail, 0 = paused */
    /* Kernel filter */
    GtkWidget        *check_exclude_kernel;
    int               exclude_kernel;   /* 1 = drop writes from pid 0 / ppid 0 kernel threads */
} wm_ui_ctx_t;

/* ── Direct /proc lookup ────────────────────────────────────────── */

static void proc_lookup(pid_t pid, char *name_out, size_t name_sz,
                        pid_t *ppid_out)
{
    name_out[0] = '\0';
    *ppid_out   = 0;

    /* name from /proc/PID/comm */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(name_out, (int)name_sz, f)) {
            /* strip trailing newline */
            size_t l = strlen(name_out);
            if (l > 0 && name_out[l - 1] == '\n') name_out[l - 1] = '\0';
        }
        fclose(f);
    }

    /* ppid from /proc/PID/status line "PPid:\t<n>" */
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    f = fopen(path, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "PPid:\t", 6) == 0) {
                *ppid_out = (pid_t)atoi(line + 6);
                break;
            }
        }
        fclose(f);
    }
}

/* ── Gutter entry helpers ─────────────────────────────────────── */

static void gutter_entries_clear(wm_ui_ctx_t *c)
{
    c->gutter_count = 0;
}

/* Append `n` identical entries (same pid + ts) for a single event chunk. */
static void gutter_entries_append(wm_ui_ctx_t *c, size_t n,
                                  pid_t pid, pid_t ppid,
                                  const char *name, const char *ts)
{
    if (n == 0) return;
    size_t needed = c->gutter_count + n;
    if (needed > c->gutter_cap) {
        size_t new_cap = c->gutter_cap ? c->gutter_cap * 2 : 64;
        while (new_cap < needed) new_cap *= 2;
        gutter_entry_t *tmp = realloc(c->gutter_entries,
                                      new_cap * sizeof(gutter_entry_t));
        if (!tmp) return;
        c->gutter_entries = tmp;
        c->gutter_cap     = new_cap;
    }
    for (size_t i = 0; i < n; ++i) {
        gutter_entry_t *e = &c->gutter_entries[c->gutter_count++];
        e->pid  = pid;
        e->ppid = ppid;
        e->name[0] = '\0';
        if (name) {
            strncpy(e->name, name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
        }
        strncpy(e->ts, ts, sizeof(e->ts) - 1);
        e->ts[sizeof(e->ts) - 1] = '\0';
    }
}

/* ── Write event ──────────────────────────────────────────────── */

static void on_fd_write_event(const evemon_event_t *ev, void *user_data)
{
    if (!ev || ev->type != EVEMON_EVENT_FD_WRITE) return;
    wm_ui_ctx_t *c = user_data;
    if (!c || !c->buf) return;
    evemon_fd_write_payload_t *p = (evemon_fd_write_payload_t *)ev->payload;
    if (!p || p->len == 0) return;

    int fd_subscribed = (p->fd == 1 && c->subscribed_fd_stdout) ||
                        (p->fd == 2 && c->subscribed_fd_stderr);
    if (!fd_subscribed) return;
    if (c->current_pid == 0) return;

    int pid_match = (p->tgid == c->current_pid);
    if (!pid_match && c->include_descendants)
        pid_match = 1;
    if (!pid_match) {
        for (size_t i = 0; i < c->desc_count; ++i)
            if (c->desc_pids[i] == p->tgid) { pid_match = 1; break; }
    }
    if (!pid_match) return;

    /* Resolve process name + ppid now so we can apply the kernel filter
     * before doing any allocation or buffer work. Kernel threads have
     * ppid == 0 (their parent is the idle/swapper process). */
    char   entry_name[32];
    pid_t  entry_ppid;
    proc_lookup(p->tgid, entry_name, sizeof(entry_name), &entry_ppid);

    /* Kernel threads: ppid == 0 (true kernel threads), or comm wrapped in
     * brackets e.g. "[kworker/0:1]" (reparented ones whose ppid is now 1). */
    if (c->exclude_kernel && (entry_ppid == 0 || entry_name[0] == '[')) return;

    /* Sanitise: strip ANSI escapes, keep printable ASCII + \n + \t */
    size_t src_len = p->len;
    if (src_len > sizeof(p->data)) src_len = sizeof(p->data);

    char *out = malloc(src_len + 1);
    if (!out) return;

    size_t out_len = 0;
    int in_escape = 0;
    for (size_t i = 0; i < src_len; ++i) {
        unsigned char ch = (unsigned char)p->data[i];
        if (in_escape == 1) {
            in_escape = (ch == '[') ? 2 : 0;
            continue;
        }
        if (in_escape == 2) {
            if (ch >= 0x40 && ch <= 0x7E) in_escape = 0;
            continue;
        }
        if (ch == 0x1B) { in_escape = 1; continue; }
        if (ch == '\n' || ch == '\t' || (ch >= 32 && ch < 127))
            out[out_len++] = (char)ch;
    }
    out[out_len] = '\0';

    if (out_len > 0) {
        /* Count newlines → number of complete lines to record in gutter */
        size_t newlines = 0;
        for (size_t i = 0; i < out_len; ++i)
            if (out[i] == '\n') newlines++;

        /* Timestamp for this event */
        char ts_buf[16];
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_info;
        localtime_r(&ts.tv_sec, &tm_info);
        int ts_len = (int)strftime(ts_buf, sizeof(ts_buf), "%H:%M:%S", &tm_info);
        snprintf(ts_buf + ts_len, sizeof(ts_buf) - (size_t)ts_len,
                 ".%03ld", ts.tv_nsec / 1000000L);

        /* Append text to main buffer */
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(c->buf, &end);
        gtk_text_buffer_insert(c->buf, &end, out, (gint)out_len);

        /* Record one gutter entry per newline */
        gutter_entries_append(c, newlines, p->tgid, entry_ppid,
                              entry_name[0] ? entry_name : NULL, ts_buf);

        /* Invalidate the gutter so it redraws with new entries */
        if (c->gutter_renderer)
            gtk_source_gutter_renderer_queue_draw(
                GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer));

        /* Auto-scroll */
        if (c->auto_scroll) {
            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(c->source_view),
                                         c->end_mark, 0.0, TRUE, 0.0, 1.0);
        }
    }

    free(out);
}

/* ── Subscription helpers ─────────────────────────────────────── */

static int subscribe_to_fd(wm_ui_ctx_t *c, pid_t pid, int fd)
{
    if (!c || !c->hsvc || !c->hsvc->monitor_fd_subscribe) return -1;
    return c->hsvc->monitor_fd_subscribe(c->hsvc->host_ctx, pid, fd);
}

static void unsubscribe_from_fd(wm_ui_ctx_t *c, pid_t pid, int fd)
{
    if (!c || !c->hsvc || !c->hsvc->monitor_fd_unsubscribe) return;
    c->hsvc->monitor_fd_unsubscribe(c->hsvc->host_ctx, pid, fd);
}

static void subscribe_descendant(wm_ui_ctx_t *c, pid_t dpid)
{
    for (size_t i = 0; i < c->desc_count; ++i)
        if (c->desc_pids[i] == dpid) return;

    if (c->subscribed_fd_stdout) subscribe_to_fd(c, dpid, 1);
    if (c->subscribed_fd_stderr) subscribe_to_fd(c, dpid, 2);

    if (c->desc_count >= c->desc_cap) {
        size_t new_cap = c->desc_cap ? c->desc_cap * 2 : 8;
        pid_t *tmp = realloc(c->desc_pids, new_cap * sizeof(pid_t));
        if (!tmp) return;
        c->desc_pids = tmp;
        c->desc_cap  = new_cap;
    }
    c->desc_pids[c->desc_count++] = dpid;
}

static void unsubscribe_all_descendants(wm_ui_ctx_t *c)
{
    for (size_t i = 0; i < c->desc_count; ++i) {
        if (c->subscribed_fd_stdout) unsubscribe_from_fd(c, c->desc_pids[i], 1);
        if (c->subscribed_fd_stderr) unsubscribe_from_fd(c, c->desc_pids[i], 2);
    }
    c->desc_count = 0;
}

static void apply_selection(wm_ui_ctx_t *c, int idx)
{
    if (!c || c->current_pid == 0) return;
    pid_t pid = c->current_pid;
    int want_stdout = (idx == 1 || idx == 3);
    int want_stderr = (idx == 2 || idx == 3);

    if (want_stdout && !c->subscribed_fd_stdout) {
        if (subscribe_to_fd(c, pid, 1) == 0) c->subscribed_fd_stdout = 1;
    } else if (!want_stdout && c->subscribed_fd_stdout) {
        unsubscribe_from_fd(c, pid, 1);
        c->subscribed_fd_stdout = 0;
    }
    if (want_stderr && !c->subscribed_fd_stderr) {
        if (subscribe_to_fd(c, pid, 2) == 0) c->subscribed_fd_stderr = 1;
    } else if (!want_stderr && c->subscribed_fd_stderr) {
        unsubscribe_from_fd(c, pid, 2);
        c->subscribed_fd_stderr = 0;
    }
}

static void clear_current_pid(wm_ui_ctx_t *c)
{
    if (!c || c->current_pid == 0) return;
    if (c->subscribed_fd_stdout) {
        unsubscribe_from_fd(c, c->current_pid, 1);
        c->subscribed_fd_stdout = 0;
    }
    if (c->subscribed_fd_stderr) {
        unsubscribe_from_fd(c, c->current_pid, 2);
        c->subscribed_fd_stderr = 0;
    }
    unsubscribe_all_descendants(c);
    if (c->hsvc && c->hsvc->monitor_unwatch_children)
        c->hsvc->monitor_unwatch_children(c->hsvc->host_ctx, c->current_pid);
    c->current_pid = 0;
}

static void activate_pid(wm_ui_ctx_t *c, pid_t pid)
{
    if (!c || pid == 0) return;
    c->current_pid = pid;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(c->combo));
    apply_selection(c, idx);
    if (c->hsvc && c->hsvc->monitor_watch_children) {
        int mask = (c->subscribed_fd_stdout ? 1 : 0) |
                   (c->subscribed_fd_stderr ? 2 : 0);
        if (mask)
            c->hsvc->monitor_watch_children(c->hsvc->host_ctx, pid, mask);
    }
}

/* ── Signal handlers ──────────────────────────────────────────── */

static void on_combo_changed(GtkComboBox *combo, gpointer user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c || c->current_pid == 0) return;
    int idx = gtk_combo_box_get_active(combo);
    apply_selection(c, idx);
    if (c->hsvc && c->hsvc->monitor_watch_children) {
        int mask = (c->subscribed_fd_stdout ? 1 : 0) |
                   (c->subscribed_fd_stderr ? 2 : 0);
        if (mask)
            c->hsvc->monitor_watch_children(c->hsvc->host_ctx, c->current_pid, mask);
        else
            c->hsvc->monitor_unwatch_children(c->hsvc->host_ctx, c->current_pid);
    }
}

static void on_check_descendants_toggled(GtkToggleButton *btn, gpointer user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c) return;
    c->include_descendants = gtk_toggle_button_get_active(btn);
    if (!c->include_descendants)
        unsubscribe_all_descendants(c);
}

static void on_process_selected(const evemon_event_t *ev, void *user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c) return;
    pid_t sel = ev->payload ? *(pid_t *)ev->payload : 0;
    if (c->current_pid == sel) return;
    clear_current_pid(c);
    if (c->buf)
        gtk_text_buffer_set_text(c->buf, "", 0);
    gutter_entries_clear(c);
    c->current_pid = sel;
    if (sel != 0) {
        int idx = c->combo ? gtk_combo_box_get_active(GTK_COMBO_BOX(c->combo)) : 0;
        apply_selection(c, idx);
        if (c->hsvc && c->hsvc->monitor_watch_children) {
            int mask = (c->subscribed_fd_stdout ? 1 : 0) |
                       (c->subscribed_fd_stderr ? 2 : 0);
            if (mask)
                c->hsvc->monitor_watch_children(c->hsvc->host_ctx, sel, mask);
        }
    }
}

static void on_check_gutter_toggled(GtkToggleButton *btn, gpointer user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c || !c->gutter_renderer) return;
    c->show_gutter = gtk_toggle_button_get_active(btn);
    gtk_source_gutter_renderer_set_visible(
        GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer), c->show_gutter);
}

static void on_check_autoscroll_toggled(GtkToggleButton *btn, gpointer user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c) return;
    c->auto_scroll = gtk_toggle_button_get_active(btn);
    /* If re-enabled, jump to bottom immediately */
    if (c->auto_scroll && c->buf) {
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(c->source_view),
                                     c->end_mark, 0.0, TRUE, 0.0, 1.0);
    }
}

static void on_check_exclude_kernel_toggled(GtkToggleButton *btn, gpointer user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c) return;
    c->exclude_kernel = gtk_toggle_button_get_active(btn);
}

/* ── Widget creation ─────────────────────────────────────────── */

static gboolean on_scroll_event(GtkWidget *widget, GdkEventScroll *ev,
                                 gpointer user_data)
{
    (void)widget;
    wm_ui_ctx_t *c = user_data;
    if (!c) return GDK_EVENT_PROPAGATE;

    gboolean scrolling_up = (ev->direction == GDK_SCROLL_UP) ||
                            (ev->direction == GDK_SCROLL_SMOOTH && ev->delta_y < 0);
    if (scrolling_up && c->auto_scroll) {
        c->auto_scroll = 0;
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(c->check_autoscroll), FALSE);
    }
    return GDK_EVENT_PROPAGATE;
}

static GtkWidget *plugin_create_widget(void *ctx)
{
    wm_ui_ctx_t *c = ctx;
    if (!c) return NULL;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);

    /* Toolbar row */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl  = gtk_label_new("Monitor writes to:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);

    c->combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->combo), "None");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->combo), "STDOUT (fd 1)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->combo), "STDERR (fd 2)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->combo), "Both");
    gtk_combo_box_set_active(GTK_COMBO_BOX(c->combo), 0);
    gtk_box_pack_start(GTK_BOX(hbox), c->combo, FALSE, FALSE, 0);

    c->check_descendants = gtk_check_button_new_with_label("Include descendants");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->check_descendants), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), c->check_descendants, FALSE, FALSE, 4);

    c->check_gutter = gtk_check_button_new_with_label("Show gutter");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->check_gutter), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), c->check_gutter, FALSE, FALSE, 4);

    c->auto_scroll = 1;
    c->check_autoscroll = gtk_check_button_new_with_label("Auto-scroll");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->check_autoscroll), TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), c->check_autoscroll, FALSE, FALSE, 4);

    c->exclude_kernel = 1;
    c->check_exclude_kernel = gtk_check_button_new_with_label("Exclude kernel");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->check_exclude_kernel), TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), c->check_exclude_kernel, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(box), hbox, FALSE, FALSE, 0);

    /* Main GtkSourceView */
    GtkSourceBuffer *sbuf = gtk_source_buffer_new(NULL);
    c->source_view = GTK_SOURCE_VIEW(gtk_source_view_new_with_buffer(sbuf));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(c->source_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(c->source_view), GTK_WRAP_WORD_CHAR);
    gtk_source_view_set_show_line_numbers(c->source_view, FALSE);
    c->buf = GTK_TEXT_BUFFER(sbuf);

    /* Right-gravity mark at end of buffer — always stays at the true end
     * as text is inserted, so scroll_to_mark reaches the last pixel. */
    GtkTextIter buf_end;
    gtk_text_buffer_get_end_iter(c->buf, &buf_end);
    c->end_mark = gtk_text_buffer_create_mark(c->buf, NULL, &buf_end, FALSE);

    /* Custom gutter renderer - hidden by default */
    c->gutter_renderer = wm_gutter_renderer_new(&c->gutter_entries,
                                                &c->gutter_count);
    gtk_source_gutter_renderer_set_visible(
        GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer), FALSE);
    gtk_source_gutter_renderer_set_padding(
        GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer), 4, 0);
    /* Fix: set an explicit width so the gutter allocates space even before
     * the renderer becomes visible.  "1234567 23:59:59.999" ≈ 20 chars;
     * 145 px covers that comfortably at typical monospace sizes. */
    gtk_source_gutter_renderer_set_size(
        GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer), 260);

    /* Dark panel background — distinct from the text view body */
    GdkRGBA gutter_bg = { 0.118, 0.141, 0.188, 1.0 }; /* #1e2430 */
    gtk_source_gutter_renderer_set_background(
        GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer), &gutter_bg);
    gtk_source_gutter_renderer_set_alignment(
        GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer), 1.0f, 0.0f);
    gtk_source_gutter_renderer_set_alignment_mode(
        GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer),
        GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST);

    GtkSourceGutter *gutter =
        gtk_source_view_get_gutter(c->source_view, GTK_TEXT_WINDOW_LEFT);
    gtk_source_gutter_insert(gutter,
                             GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer),
                             0);

    c->view = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(c->view), GTK_WIDGET(c->source_view));
    gtk_widget_set_vexpand(c->view, TRUE);
    gtk_widget_set_hexpand(c->view, TRUE);
    gtk_box_pack_start(GTK_BOX(box), c->view, TRUE, TRUE, 0);

    g_signal_connect(c->combo, "changed",
                     G_CALLBACK(on_combo_changed), c);
    g_signal_connect(c->check_descendants, "toggled",
                     G_CALLBACK(on_check_descendants_toggled), c);
    g_signal_connect(c->check_gutter, "toggled",
                     G_CALLBACK(on_check_gutter_toggled), c);
    g_signal_connect(c->check_autoscroll, "toggled",
                     G_CALLBACK(on_check_autoscroll_toggled), c);
    g_signal_connect(c->check_exclude_kernel, "toggled",
                     G_CALLBACK(on_check_exclude_kernel_toggled), c);
    gtk_widget_add_events(GTK_WIDGET(c->source_view), GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(GTK_WIDGET(c->source_view), "scroll-event",
                     G_CALLBACK(on_scroll_event), c);

    c->root = box;
    return box;
}

/* ── Lifecycle ────────────────────────────────────────────────── */

static void plugin_activate(void *ctx, const evemon_host_services_t *services)
{
    wm_ui_ctx_t *c = ctx;
    if (!c) return;
    c->hsvc                 = services;
    c->current_pid          = 0;
    c->subscribed_fd_stdout = 0;
    c->subscribed_fd_stderr = 0;
    c->include_descendants  = 0;
    c->desc_pids            = NULL;
    c->desc_count           = 0;
    c->desc_cap             = 0;
    c->sub_pid_select_id    = 0;
    c->sub_fd_write_id      = 0;

    if (services && services->subscribe) {
        c->sub_pid_select_id = services->subscribe(services->host_ctx,
                                                   EVEMON_EVENT_PROCESS_SELECTED,
                                                   on_process_selected, c);
        c->sub_fd_write_id   = services->subscribe(services->host_ctx,
                                                   EVEMON_EVENT_FD_WRITE,
                                                   on_fd_write_event, c);
    }
}

static void plugin_destroy(void *ctx)
{
    wm_ui_ctx_t *c = ctx;
    if (!c) return;
    if (c->hsvc) {
        clear_current_pid(c);
        if (c->sub_pid_select_id && c->hsvc->unsubscribe)
            c->hsvc->unsubscribe(c->hsvc->host_ctx, c->sub_pid_select_id);
        if (c->sub_fd_write_id && c->hsvc->unsubscribe)
            c->hsvc->unsubscribe(c->hsvc->host_ctx, c->sub_fd_write_id);
    }
    free(c->desc_pids);
    free(c->gutter_entries);
    free(c);
}

static void plugin_update(void *ctx, const evemon_proc_data_t *data)
{
    wm_ui_ctx_t *c = ctx;
    if (!c || !data) return;
    pid_t sel = data->pid;
    if (c->current_pid != sel) {
        clear_current_pid(c);
        if (c->buf)
            gtk_text_buffer_set_text(c->buf, "", 0);
        gutter_entries_clear(c);
        c->current_pid = sel;
        if (sel != 0) {
            int idx = c->combo ? gtk_combo_box_get_active(GTK_COMBO_BOX(c->combo)) : 0;
            apply_selection(c, idx);
            if (c->hsvc && c->hsvc->monitor_watch_children) {
                int mask = (c->subscribed_fd_stdout ? 1 : 0) |
                           (c->subscribed_fd_stderr ? 2 : 0);
                if (mask)
                    c->hsvc->monitor_watch_children(c->hsvc->host_ctx, sel, mask);
            }
        }
    }
    if (c->include_descendants &&
        (c->subscribed_fd_stdout || c->subscribed_fd_stderr) &&
        data->descendant_pids && data->descendant_count > 0) {
        for (size_t i = 0; i < data->descendant_count; ++i)
            subscribe_descendant(c, data->descendant_pids[i]);
    }
}

static void plugin_clear(void *ctx)
{
    wm_ui_ctx_t *c = ctx;
    if (!c) return;
    clear_current_pid(c);
    if (c->buf)
        gtk_text_buffer_set_text(c->buf, "", 0);
    gutter_entries_clear(c);
    if (c->gutter_renderer)
        gtk_source_gutter_renderer_queue_draw(
            GTK_SOURCE_GUTTER_RENDERER(c->gutter_renderer));
}

static int plugin_wants_update(void *ctx)
{
    wm_ui_ctx_t *c = ctx;
    return c && c->root &&
           GTK_IS_WIDGET(c->root) &&
           gtk_widget_get_mapped(c->root) &&
           gtk_widget_get_child_visible(c->root);
}

/* ── Entry point ──────────────────────────────────────────────── */

evemon_plugin_t *evemon_plugin_init(void)
{
    evemon_plugin_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    wm_ui_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) { free(p); return NULL; }

    p->abi_version   = evemon_PLUGIN_ABI_VERSION;
    p->name          = "Write Log";
    p->id            = "org.evemon.write_monitor_ui";
    p->version       = "1.0";
    p->data_needs    = evemon_NEED_DESCENDANTS;
    p->plugin_ctx    = c;
    p->create_widget = plugin_create_widget;
    p->update        = plugin_update;
    p->clear         = plugin_clear;
    p->destroy       = plugin_destroy;
    p->activate      = plugin_activate;
    p->wants_update  = plugin_wants_update;
    p->role          = EVEMON_ROLE_PROCESS;
    p->dependencies  = _wm_ui_deps;

    return p;
}
