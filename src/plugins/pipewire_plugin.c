/*
 * pipewire_plugin.c – PipeWire Audio Connections plugin for evemon.
 *
 * Displays PipeWire audio nodes and connections for a process:
 * categorised into Audio Output, Audio Input, Video, MIDI, and Other.
 *
 * Features:
 *   - Real-time L/R peak level meters (via host services)
 *   - Embedded scrolling FFT spectrogram (via host services)
 *   - Codec detection overlay on spectrogram (lossless/MP3/etc.)
 *   - Double-click a stream to switch spectrogram target
 *   - Proper node labelling: media_name → node_desc → app_name → node_name
 *
 * NOTE: This plugin requests evemon_NEED_PIPEWIRE.  The broker
 * will populate pw_nodes/pw_links from a PipeWire graph snapshot.
 * Real-time audio features (meters, spectrogram) are provided by
 * the host via evemon_host_services_t callbacks.
 *
 * Build:
 *   gcc -shared -fPIC -o evemon_pipewire_plugin.so pipewire_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>

/* ── UTF-8 helpers ───────────────────────────────────────────── */

/*
 * Sanitise a PipeWire metadata string for GTK/Pango.
 * Returns a heap-allocated valid-UTF-8 string (caller frees with g_free).
 * - Replaces invalid byte sequences with U+FFFD.
 * - Returns "" for NULL input.
 */
static char *utf8_sanitize(const char *raw)
{
    if (!raw || !raw[0]) return g_strdup("");
    if (g_utf8_validate(raw, -1, NULL)) return g_strdup(raw);
    return g_utf8_make_valid(raw, -1);
}

/* ── categories ──────────────────────────────────────────────── */

enum {
    PW_CAT_OUTPUT,
    PW_CAT_INPUT,
    PW_CAT_VIDEO,
    PW_CAT_MIDI,
    PW_CAT_OTHER,
    PW_CAT_COUNT
};

/* Category labels — currently unused but kept for reference. */
/* static const char *cat_labels[PW_CAT_COUNT] = {
    [PW_CAT_OUTPUT] = "Audio Output",
    [PW_CAT_INPUT]  = "Audio Input",
    [PW_CAT_VIDEO]  = "Video",
    [PW_CAT_MIDI]   = "MIDI",
    [PW_CAT_OTHER]  = "Other",
}; */

enum {
    COL_TEXT,       /* plain text (display line)                */
    COL_MARKUP,     /* Pango markup for display                 */
    COL_CAT,        /* category (-1 for leaf rows)              */
    COL_NODE_ID,    /* PipeWire node ID (uint, leaf rows)       */
    COL_LEVEL_L,    /* left  peak level, 0..1000 (int)          */
    COL_LEVEL_R,    /* right peak level, 0..1000 (int)          */
    NUM_COLS
};

/* ── custom GtkCellRenderer for L/R meter bars ───────────────── */

#define PW_TYPE_CELL_RENDERER_METER (pw_cell_renderer_meter_get_type())

typedef struct _PwCellRendererMeter      PwCellRendererMeter;
typedef struct _PwCellRendererMeterClass PwCellRendererMeterClass;

struct _PwCellRendererMeter {
    GtkCellRenderer parent;
    gint level_l;
    gint level_r;
};

struct _PwCellRendererMeterClass {
    GtkCellRendererClass parent_class;
};

enum {
    PROP_0_METER,
    PROP_LEVEL_L,
    PROP_LEVEL_R,
};

G_DEFINE_TYPE(PwCellRendererMeter, pw_cell_renderer_meter,
              GTK_TYPE_CELL_RENDERER)

static void pw_meter_get_property(GObject *obj, guint id,
                                  GValue *val, GParamSpec *pspec)
{
    PwCellRendererMeter *self = (PwCellRendererMeter *)obj;
    switch (id) {
    case PROP_LEVEL_L: g_value_set_int(val, self->level_l); break;
    case PROP_LEVEL_R: g_value_set_int(val, self->level_r); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void pw_meter_set_property(GObject *obj, guint id,
                                  const GValue *val, GParamSpec *pspec)
{
    PwCellRendererMeter *self = (PwCellRendererMeter *)obj;
    switch (id) {
    case PROP_LEVEL_L: self->level_l = g_value_get_int(val); break;
    case PROP_LEVEL_R: self->level_r = g_value_get_int(val); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

#define METER_BAR_WIDTH   60
#define METER_BAR_HEIGHT   5
#define METER_GAP          1
#define METER_TOTAL_HEIGHT (METER_BAR_HEIGHT * 2 + METER_GAP)

static void meter_get_preferred_width(GtkCellRenderer *cell,
                                      GtkWidget *widget,
                                      gint *minimum, gint *natural)
{
    (void)cell; (void)widget;
    if (minimum) *minimum = METER_BAR_WIDTH + 4;
    if (natural) *natural = METER_BAR_WIDTH + 4;
}

static void meter_get_preferred_height(GtkCellRenderer *cell,
                                       GtkWidget *widget,
                                       gint *minimum, gint *natural)
{
    (void)cell; (void)widget;
    if (minimum) *minimum = METER_TOTAL_HEIGHT + 2;
    if (natural) *natural = METER_TOTAL_HEIGHT + 2;
}

static void meter_color(float level, float *r, float *g, float *b)
{
    if (level < 0.6f) {
        float t = level / 0.6f;
        *r = t; *g = 0.8f; *b = 0.1f;
    } else {
        float t = (level - 0.6f) / 0.4f;
        *r = 0.9f + 0.1f * t;
        *g = 0.8f * (1.0f - t);
        *b = 0.05f;
    }
}

static void meter_render(GtkCellRenderer *cell, cairo_t *cr,
                         GtkWidget *widget,
                         const GdkRectangle *bg_area,
                         const GdkRectangle *cell_area,
                         GtkCellRendererState flags)
{
    (void)widget; (void)bg_area; (void)flags;
    PwCellRendererMeter *self = (PwCellRendererMeter *)cell;

    float fl = self->level_l / 1000.0f;
    float fr = self->level_r / 1000.0f;
    if (fl < 0.0f) fl = 0.0f;
    if (fl > 1.0f) fl = 1.0f;
    if (fr < 0.0f) fr = 0.0f;
    if (fr > 1.0f) fr = 1.0f;
    if (fl < 0.001f && fr < 0.001f) return;

    int x = cell_area->x + 2;
    int y = cell_area->y + (cell_area->height - METER_TOTAL_HEIGHT) / 2;
    int w = METER_BAR_WIDTH;

    /* Background (dark track) */
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, x, y, w, METER_BAR_HEIGHT);
    cairo_rectangle(cr, x, y + METER_BAR_HEIGHT + METER_GAP,
                    w, METER_BAR_HEIGHT);
    cairo_fill(cr);

    /* Left channel bar */
    if (fl > 0.001f) {
        int bw = (int)(fl * w + 0.5f);
        if (bw > w) bw = w;
        for (int px = 0; px < bw; px++) {
            float seg = (float)px / (float)w;
            float r, g, b;
            meter_color(seg, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_rectangle(cr, x + px, y, 1, METER_BAR_HEIGHT);
            cairo_fill(cr);
        }
    }

    /* Right channel bar */
    if (fr > 0.001f) {
        int bw = (int)(fr * w + 0.5f);
        if (bw > w) bw = w;
        int ry = y + METER_BAR_HEIGHT + METER_GAP;
        for (int px = 0; px < bw; px++) {
            float seg = (float)px / (float)w;
            float r, g, b;
            meter_color(seg, &r, &g, &b);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_rectangle(cr, x + px, ry, 1, METER_BAR_HEIGHT);
            cairo_fill(cr);
        }
    }
}

static void pw_cell_renderer_meter_class_init(PwCellRendererMeterClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    GtkCellRendererClass *cell_class = GTK_CELL_RENDERER_CLASS(klass);
    obj_class->get_property = pw_meter_get_property;
    obj_class->set_property = pw_meter_set_property;
    cell_class->get_preferred_width  = meter_get_preferred_width;
    cell_class->get_preferred_height = meter_get_preferred_height;
    cell_class->render               = meter_render;
    g_object_class_install_property(obj_class, PROP_LEVEL_L,
        g_param_spec_int("level-l", "Level L", "Left peak level",
                         0, 1000, 0, G_PARAM_READWRITE));
    g_object_class_install_property(obj_class, PROP_LEVEL_R,
        g_param_spec_int("level-r", "Level R", "Right peak level",
                         0, 1000, 0, G_PARAM_READWRITE));
}

static void pw_cell_renderer_meter_init(PwCellRendererMeter *self)
{
    self->level_l = 0;
    self->level_r = 0;
}

static GtkCellRenderer *pw_meter_renderer_new(void)
{
    return g_object_new(PW_TYPE_CELL_RENDERER_METER, NULL);
}

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    /* Connection list */
    GtkWidget      *main_box;
    GtkWidget      *scroll;
    GtkListStore   *store;
    GtkTreeView    *view;
    GtkCssProvider *css;
    pid_t           last_pid;

    /* Spectrogram (embedded drawing area) */
    GtkWidget      *spectro_section;
    GtkWidget      *spectro_draw;
    gboolean        spectro_shown;

    /* Host services (injected by activate()) */
    const evemon_host_services_t *host;

    /* Meter update timer */
    guint           meter_timer;

    /* Audio output node tracking */
    uint32_t        audio_node_ids[64];
    size_t          audio_node_count;

    /* MPRIS media metadata display */
    GtkWidget      *media_section;      /* container for now-playing */
    GtkWidget      *media_art_image;    /* GtkImage for album art    */
    GtkWidget      *media_title_label;
    GtkWidget      *media_artist_label;
    GtkWidget      *media_album_label;
    GtkWidget      *media_status_label;
    GtkWidget      *media_position_label;
    GdkPixbuf      *art_pixbuf;         /* cached album art pixbuf   */
    char            art_url_cached[512]; /* URL of cached art         */

    /* Event bus subscription ID (for cleanup in destroy) */
    int             event_sub_id;
} pw_ctx_t;

/* ── classification ──────────────────────────────────────────── */

static int classify_node(const char *media_class)
{
    if (!media_class || !media_class[0]) return PW_CAT_OTHER;
    if (strstr(media_class, "Audio") && strstr(media_class, "Output"))
        return PW_CAT_OUTPUT;
    if (strstr(media_class, "Audio") && strstr(media_class, "Input"))
        return PW_CAT_INPUT;
    if (strstr(media_class, "Audio") && strstr(media_class, "Sink"))
        return PW_CAT_OUTPUT;
    if (strstr(media_class, "Audio") && strstr(media_class, "Source"))
        return PW_CAT_INPUT;
    if (strstr(media_class, "Video"))  return PW_CAT_VIDEO;
    if (strstr(media_class, "Midi"))   return PW_CAT_MIDI;
    return PW_CAT_OTHER;
}

/* ── helpers ─────────────────────────────────────────────────── */

/*
 * Best human-readable label for a node.
 * For streams: prefer media_name (song title, tab name),
 * then node_desc, then app_name, then node_name.
 */
static const char *node_label(const evemon_pw_node_t *n)
{
    if (n->media_name[0])  return n->media_name;
    if (n->node_desc[0])   return n->node_desc;
    if (n->app_name[0])    return n->app_name;
    if (n->node_name[0])   return n->node_name;
    return "(unknown)";
}

/*
 * Return true if a PipeWire node belongs to evemon itself
 * (meter capture, spectrogram capture, or the monitor process).
 * Checks every name field so we never display our own plumbing.
 */
static int is_evemon_node(const evemon_pw_node_t *n)
{
    if (!n) return 0;
    if (strstr(n->node_name,  "evemon")) return 1;
    if (strstr(n->app_name,   "evemon")) return 1;
    if (strstr(n->node_desc,  "evemon")) return 1;
    if (strstr(n->media_name, "evemon")) return 1;
    return 0;
}

static const evemon_pw_node_t *find_node(const evemon_proc_data_t *data,
                                          uint32_t id)
{
    for (size_t i = 0; i < data->pw_node_count; i++)
        if (data->pw_nodes[i].id == id) return &data->pw_nodes[i];
    return NULL;
}

/* ── markup ──────────────────────────────────────────────────── */

/*
 * Build Pango markup from pre-split left/right labels and an arrow.
 * Both labels are escaped for XML safety.  If right is NULL,
 * treat the text as a single un-split label.
 */
static char *pw_build_markup(const char *left_raw,
                             const char *right_raw,
                             const char *arrow_utf8)
{
    if (!right_raw || !arrow_utf8) {
        /* No connection — just escape the whole string */
        char *safe = utf8_sanitize(left_raw);
        char *m = g_markup_escape_text(safe, -1);
        g_free(safe);
        return m;
    }

    char *safe_l = utf8_sanitize(left_raw);
    char *safe_r = utf8_sanitize(right_raw);
    char *esc_l  = g_markup_escape_text(safe_l, -1);
    char *esc_r  = g_markup_escape_text(safe_r, -1);

    char *m = g_strdup_printf(
        "<b>%s</b> %s <span foreground=\"#6699cc\">%s</span>",
        esc_l, arrow_utf8, esc_r);

    g_free(esc_l); g_free(esc_r);
    g_free(safe_l); g_free(safe_r);
    return m;
}

/* ── signal callbacks ────────────────────────────────────────── */

/*
 * Double-click a row to switch spectrogram target.
 */
static void on_row_activated(GtkTreeView *view, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    pw_ctx_t *ctx = data;

    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;

    guint node_id = 0;
    gtk_tree_model_get(model, &iter, COL_NODE_ID, &node_id, -1);
    if (node_id == 0) return;

    ctx->spectro_shown = TRUE;

    if (ctx->host && ctx->host->spectro_start) {
        ctx->host->spectro_start(ctx->host->host_ctx,
                                 GTK_DRAWING_AREA(ctx->spectro_draw),
                                 (uint32_t)node_id);
        gtk_widget_set_no_show_all(ctx->spectro_section, FALSE);
        gtk_widget_show_all(ctx->spectro_section);
        gtk_widget_set_no_show_all(ctx->spectro_section, TRUE);
    }
}

/* ── spectrogram placeholder draw ────────────────────────────── */

static gboolean on_spectro_draw(GtkWidget *widget, cairo_t *cr,
                                gpointer data)
{
    pw_ctx_t *ctx = data;
    uint32_t target = 0;
    if (ctx->host && ctx->host->spectro_get_target)
        target = ctx->host->spectro_get_target(ctx->host->host_ctx,
                     GTK_DRAWING_AREA(ctx->spectro_draw));

    if (target == 0) {
        int h = gtk_widget_get_allocated_height(widget);
        cairo_set_source_rgb(cr, 0.05, 0.05, 0.1);
        cairo_paint(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.3);
        cairo_select_font_face(cr, "Sans",
                               CAIRO_FONT_SLANT_ITALIC,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_move_to(cr, 8, h / 2.0 + 4);
        cairo_show_text(cr, "Double-click an audio stream to show spectrogram");
        return FALSE;
    }
    return FALSE;
}

/* ── meter timer callback ────────────────────────────────────── */

static gboolean meter_tick(gpointer data)
{
    pw_ctx_t *ctx = data;
    if (!ctx->store || !GTK_IS_TREE_MODEL(ctx->store) ||
        !ctx->host || !ctx->host->pw_meter_read)
        return G_SOURCE_CONTINUE;

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, NULL);
    int any_changed = 0;

    while (valid) {
        guint nid = 0;
        gtk_tree_model_get(model, &iter, COL_NODE_ID, &nid, -1);
        if (nid != 0) {
            int ll = 0, lr = 0;
            ctx->host->pw_meter_read(ctx->host->host_ctx,
                                     nid, &ll, &lr);
            gtk_list_store_set(ctx->store, &iter,
                               COL_LEVEL_L, ll, COL_LEVEL_R, lr, -1);
            if (ll > 0 || lr > 0) any_changed = 1;
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }

    if (any_changed)
        gtk_widget_queue_draw(GTK_WIDGET(ctx->view));

    return G_SOURCE_CONTINUE;
}

/* ── MPRIS media metadata display ─────────────────────────────── */

/*
 * Format microseconds to "M:SS" or "H:MM:SS".
 */
static void format_duration(int64_t us, char *buf, size_t bufsz)
{
    if (us < 0) { buf[0] = '\0'; return; }
    int64_t total_s = us / 1000000;
    int h = (int)(total_s / 3600);
    int m = (int)((total_s % 3600) / 60);
    int s = (int)(total_s % 60);
    if (h > 0)
        snprintf(buf, bufsz, "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, bufsz, "%d:%02d", m, s);
}

/* ── async art load callback (pipewire) ─────────────────────── */

/* Scale a pixbuf to fit within max_size, preserving aspect ratio. */
static GdkPixbuf *scale_art_pixbuf(GdkPixbuf *src, int max_size)
{
    if (!src) return NULL;
    int w = gdk_pixbuf_get_width(src);
    int h = gdk_pixbuf_get_height(src);
    if (w <= max_size && h <= max_size) return g_object_ref(src);
    double scale = (double)max_size / (w > h ? w : h);
    int nw = (int)(w * scale);
    int nh = (int)(h * scale);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    return gdk_pixbuf_scale_simple(src, nw, nh, GDK_INTERP_BILINEAR);
}

static void pw_on_art_loaded(GdkPixbuf *pixbuf, void *user_data)
{
    pw_ctx_t *ctx = user_data;
    if (ctx->art_pixbuf)
        g_object_unref(ctx->art_pixbuf);
    ctx->art_pixbuf = pixbuf ? g_object_ref(pixbuf) : NULL;
    /* Scale and update the GtkImage widget */
    if (ctx->art_pixbuf && ctx->media_art_image) {
        GdkPixbuf *scaled = scale_art_pixbuf(ctx->art_pixbuf, 64);
        gtk_image_set_from_pixbuf(GTK_IMAGE(ctx->media_art_image), scaled);
        if (scaled) g_object_unref(scaled);
    } else if (ctx->media_art_image) {
        gtk_image_clear(GTK_IMAGE(ctx->media_art_image));
    }
}

/*
 * Event bus callback: receives album art + metadata from the
 * audio_service headless plugin.
 */
static void pw_on_album_art_event(const evemon_event_t *event,
                                  void *user_data)
{
    pw_ctx_t *ctx = user_data;
    const evemon_album_art_payload_t *art = event->payload;
    if (!art) return;

    /* If this instance has been cleared (no process selected), ignore
     * all events — the widgets may be in an undefined state and we
     * have nothing meaningful to display. */
    if (ctx->last_pid <= 0) return;

    /* Only accept events for the PID this plugin instance is tracking.
     * Without this check, when multiple processes have audio playing,
     * metadata from process B would overwrite process A's display. */
    if (art->source_pid > 0 && art->source_pid != ctx->last_pid)
        return;

    /* Defensive: verify widgets are still alive before touching them.
     * After pw_destroy the widget pointers are stale; the event bus
     * unsubscribe should prevent us from reaching here, but belt and
     * suspenders never hurt. */
    if (!ctx->media_title_label || !GTK_IS_LABEL(ctx->media_title_label))
        return;

    /* Update album art */
    pw_on_art_loaded(art->pixbuf, ctx);
    snprintf(ctx->art_url_cached, sizeof(ctx->art_url_cached),
             "%s", art->art_url);

    /* Update MPRIS metadata labels from the event payload */
    if (art->track_title[0]) {
        char markup[512];
        char *esc = g_markup_escape_text(art->track_title, -1);
        snprintf(markup, sizeof(markup), "<b>%s</b>", esc);
        g_free(esc);
        if (GTK_IS_LABEL(ctx->media_title_label))
            gtk_label_set_markup(GTK_LABEL(ctx->media_title_label), markup);
    } else {
        if (GTK_IS_LABEL(ctx->media_title_label))
            gtk_label_set_text(GTK_LABEL(ctx->media_title_label), "");
    }

    if (art->track_artist[0]) {
        char *esc = g_markup_escape_text(art->track_artist, -1);
        char markup[512];
        snprintf(markup, sizeof(markup),
                 "<span foreground=\"#8899bb\">%s</span>", esc);
        g_free(esc);
        if (GTK_IS_LABEL(ctx->media_artist_label))
            gtk_label_set_markup(GTK_LABEL(ctx->media_artist_label), markup);
    } else {
        if (GTK_IS_LABEL(ctx->media_artist_label))
            gtk_label_set_text(GTK_LABEL(ctx->media_artist_label), "");
    }

    if (art->track_album[0]) {
        char *esc = g_markup_escape_text(art->track_album, -1);
        char markup[512];
        snprintf(markup, sizeof(markup),
                 "<span foreground=\"#778899\"><i>%s</i></span>", esc);
        g_free(esc);
        if (GTK_IS_LABEL(ctx->media_album_label))
            gtk_label_set_markup(GTK_LABEL(ctx->media_album_label), markup);
    } else {
        if (GTK_IS_LABEL(ctx->media_album_label))
            gtk_label_set_text(GTK_LABEL(ctx->media_album_label), "");
    }

    /* Playback status with icon */
    {
        const char *icon = "\u23f9";
        if (strcmp(art->playback_status, "Playing") == 0) icon = "\u25b6";
        else if (strcmp(art->playback_status, "Paused") == 0) icon = "\u23f8";

        char status[128];
        if (art->identity[0])
            snprintf(status, sizeof(status), "%s %s",
                     icon, art->identity);
        else
            snprintf(status, sizeof(status), "%s %s",
                     icon, art->playback_status);
        if (GTK_IS_LABEL(ctx->media_status_label))
            gtk_label_set_text(GTK_LABEL(ctx->media_status_label), status);
    }

    /* Position / Duration */
    {
        char pos_buf[32] = "", len_buf[32] = "", disp[80] = "";
        format_duration(art->position_us, pos_buf, sizeof(pos_buf));
        format_duration(art->length_us, len_buf, sizeof(len_buf));
        if (pos_buf[0] && len_buf[0])
            snprintf(disp, sizeof(disp), "%s / %s", pos_buf, len_buf);
        else if (len_buf[0])
            snprintf(disp, sizeof(disp), "/ %s", len_buf);
        else if (pos_buf[0])
            snprintf(disp, sizeof(disp), "%s", pos_buf);
        if (GTK_IS_LABEL(ctx->media_position_label))
            gtk_label_set_text(GTK_LABEL(ctx->media_position_label), disp);
    }

    /* Show/hide the section based on whether we have metadata */
    if (!ctx->media_section || !GTK_IS_WIDGET(ctx->media_section))
        return;
    if (art->track_title[0] || art->playback_status[0]) {
        gtk_widget_set_no_show_all(ctx->media_section, FALSE);
        gtk_widget_show_all(ctx->media_section);
        gtk_widget_set_no_show_all(ctx->media_section, TRUE);
    } else {
        gtk_widget_hide(ctx->media_section);
    }
}

/*
 * Update the Now Playing section with MPRIS metadata.
 *
 * Album art loading and MPRIS label updates are now handled by the
 * event bus (pw_on_album_art_event).  This function just ensures
 * the media section visibility is correct when no MPRIS data is
 * available.
 */
static void pw_update_mpris(pw_ctx_t *ctx, const evemon_proc_data_t *data)
{
    if (!data->mpris_players || data->mpris_player_count == 0) {
        /* No MPRIS data — hide the section */
        if (ctx->media_section)
            gtk_widget_hide(ctx->media_section);
        return;
    }
    /* The media section is shown/hidden by pw_on_album_art_event
     * when events arrive from the audio service plugin. */
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *pw_create_widget(void *opaque)
{
    pw_ctx_t *ctx = opaque;

    ctx->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* ── Connection tree ──────────────────────────────────────── */
    ctx->store = gtk_list_store_new(NUM_COLS,
                                    G_TYPE_STRING, G_TYPE_STRING,
                                    G_TYPE_INT, G_TYPE_UINT,
                                    G_TYPE_INT, G_TYPE_INT);
    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    /* NOTE: we keep our own ref to ctx->store (don't unref here)
     * so that the meter timer can safely access it.  We release
     * the ref in pw_destroy(). */

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Audio", cell, "markup", COL_MARKUP, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(ctx->view, col);

    GtkCellRenderer *meter_r = pw_meter_renderer_new();
    GtkTreeViewColumn *meter_col = gtk_tree_view_column_new_with_attributes(
        "Level", meter_r,
        "level-l", COL_LEVEL_L,
        "level-r", COL_LEVEL_R,
        NULL);
    gtk_tree_view_column_set_fixed_width(meter_col, 66);
    gtk_tree_view_column_set_sizing(meter_col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_append_column(ctx->view, meter_col);

    ctx->css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(ctx->css,
        "treeview { font-family: Monospace; font-size: 8pt; }", -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(GTK_WIDGET(ctx->view)),
        GTK_STYLE_PROVIDER(ctx->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);

    g_signal_connect(ctx->view, "row-activated",
                     G_CALLBACK(on_row_activated), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);

    gtk_box_pack_start(GTK_BOX(ctx->main_box), ctx->scroll, TRUE, TRUE, 0);

    /* ── Now Playing section (MPRIS metadata) ─────────────────── */
    {
        GtkWidget *media_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *media_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(media_label),
            "<small><b>Now Playing</b></small>");
        gtk_label_set_xalign(GTK_LABEL(media_label), 0.0f);
        gtk_box_pack_start(GTK_BOX(media_box), media_label,
                           FALSE, FALSE, 2);

        GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        /* Album art — fixed 64×64, no expand */
        ctx->media_art_image = gtk_image_new();
        gtk_widget_set_size_request(ctx->media_art_image, 64, 64);
        gtk_widget_set_halign(ctx->media_art_image, GTK_ALIGN_START);
        gtk_widget_set_valign(ctx->media_art_image, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(info_box), ctx->media_art_image,
                           FALSE, FALSE, 4);

        /* Text info */
        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);

        ctx->media_title_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_title_label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(ctx->media_title_label),
                                PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(text_box), ctx->media_title_label,
                           FALSE, FALSE, 0);

        ctx->media_artist_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_artist_label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(ctx->media_artist_label),
                                PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(text_box), ctx->media_artist_label,
                           FALSE, FALSE, 0);

        ctx->media_album_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_album_label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(ctx->media_album_label),
                                PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(text_box), ctx->media_album_label,
                           FALSE, FALSE, 0);

        GtkWidget *status_pos_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        ctx->media_status_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_status_label), 0.0f);
        gtk_box_pack_start(GTK_BOX(status_pos_box), ctx->media_status_label,
                           FALSE, FALSE, 0);
        ctx->media_position_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_position_label), 0.0f);
        gtk_box_pack_start(GTK_BOX(status_pos_box), ctx->media_position_label,
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), status_pos_box,
                           FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(info_box), text_box, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(media_box), info_box, FALSE, FALSE, 2);

        /* Apply monospace style to match the tree */
        GtkCssProvider *media_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(media_css,
            "label { font-family: Sans; font-size: 8pt; }", -1, NULL);
        GtkWidget *labels[] = { ctx->media_title_label,
                                ctx->media_artist_label,
                                ctx->media_album_label,
                                ctx->media_status_label,
                                ctx->media_position_label };
        for (int i = 0; i < 5; i++)
            gtk_style_context_add_provider(
                gtk_widget_get_style_context(labels[i]),
                GTK_STYLE_PROVIDER(media_css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(media_css);

        ctx->media_section = media_box;
        gtk_widget_set_no_show_all(ctx->media_section, TRUE);
        /* Pack at the end so it stays fixed at the bottom */
        gtk_box_pack_end(GTK_BOX(ctx->main_box), ctx->media_section,
                         FALSE, FALSE, 4);
    }

    /* ── Spectrogram section ──────────────────────────────────── */
    ctx->spectro_draw = gtk_drawing_area_new();
    gtk_widget_set_size_request(ctx->spectro_draw, -1, 180);
    gtk_widget_set_hexpand(ctx->spectro_draw, TRUE);

    GtkWidget *spectro_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(spectro_frame), ctx->spectro_draw);

    GtkWidget *spectro_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *spectro_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(spectro_label),
        "<small><b>Audio Spectrogram</b></small>");
    gtk_label_set_xalign(GTK_LABEL(spectro_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(spectro_box), spectro_label,
                       FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(spectro_box), spectro_frame,
                       FALSE, FALSE, 0);

    ctx->spectro_section = spectro_box;

    g_signal_connect(ctx->spectro_draw, "draw",
                     G_CALLBACK(on_spectro_draw), ctx);

    gtk_widget_set_no_show_all(ctx->spectro_section, TRUE);
    /* Pack at end (above Now Playing) so the tree list gets grow space */
    gtk_box_pack_end(GTK_BOX(ctx->main_box), ctx->spectro_section,
                     FALSE, FALSE, 4);

    gtk_widget_show_all(ctx->main_box);

    return ctx->main_box;
}

static void pw_activate(void *opaque,
                        const evemon_host_services_t *services)
{
    pw_ctx_t *ctx = opaque;
    ctx->host = services;
    /* Subscribe to album art events from the audio service plugin */
    if (services->subscribe)
        ctx->event_sub_id = services->subscribe(services->host_ctx,
                            EVEMON_EVENT_ALBUM_ART_UPDATED,
                            pw_on_album_art_event, ctx);
}

static void pw_update(void *opaque, const evemon_proc_data_t *data)
{
    pw_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->last_pid = data->pid;
        ctx->spectro_shown = FALSE;
        gtk_list_store_clear(ctx->store);
        ctx->audio_node_count = 0;
    }

    if (!data->pw_nodes || data->pw_node_count == 0) {
        /* Keep the last known good display — don't clear the store.
         * PipeWire snapshots are inherently racy; an empty result
         * on one cycle doesn't mean the streams are gone.  The store
         * is cleared explicitly on PID change (above) and in pw_clear(). */
        return;
    }

    /* Build display entries.
     * We store left/right labels separately so that markup generation
     * never has to search for arrow characters inside metadata strings
     * (which can legitimately contain → or ↔ in song titles, etc.).
     * Using heap-allocated sanitised UTF-8 strings avoids truncation
     * of multi-byte sequences that plagued the old 512-byte buffer.
     */
    typedef struct {
        char *left;          /* sanitised UTF-8 (g_free) */
        char *right;         /* sanitised UTF-8 or NULL  */
        const char *arrow;   /* static UTF-8 arrow or NULL */
        int cat;
        uint32_t node_id;
    } ent_t;
    size_t ent_cap = 64, ent_count = 0;
    ent_t *ents = calloc(ent_cap, sizeof(ent_t));
    if (!ents) return;

    ctx->audio_node_count = 0;

    for (size_t ni = 0; ni < data->pw_node_count; ni++) {
        const evemon_pw_node_t *node = &data->pw_nodes[ni];
        if (node->pid != data->pid) continue;

        /* Skip evemon's own capture/meter/spectrogram streams. */
        if (is_evemon_node(node)) continue;

        int cat = classify_node(node->media_class);
        const char *self = node_label(node);

        /* Track audio output nodes for meters/spectrogram */
        if (strstr(node->media_class, "Stream") &&
            strstr(node->media_class, "Output") &&
            strstr(node->media_class, "Audio")) {
            if (ctx->audio_node_count < 64)
                ctx->audio_node_ids[ctx->audio_node_count++] = node->id;
        }

        /* Find linked peers */
        uint32_t peers[32]; size_t np = 0;
        for (size_t li = 0; li < data->pw_link_count; li++) {
            const evemon_pw_link_t *lk = &data->pw_links[li];
            uint32_t peer_id = 0;
            if (lk->output_node == node->id)
                peer_id = lk->input_node;
            else if (lk->input_node == node->id)
                peer_id = lk->output_node;
            else continue;
            if (peer_id == 0 || peer_id == node->id) continue;
            int dup = 0;
            for (size_t k = 0; k < np; k++)
                if (peers[k] == peer_id) { dup = 1; break; }
            if (!dup && np < 32) peers[np++] = peer_id;
        }

        /* Filter out evemon's own nodes from the peer list so our
         * meter/spectrogram capture streams never show as connections. */
        size_t real_np = 0;
        for (size_t pi = 0; pi < np; pi++) {
            const evemon_pw_node_t *peer = find_node(data, peers[pi]);
            if (is_evemon_node(peer)) continue;
            peers[real_np++] = peers[pi];
        }
        np = real_np;

        if (np == 0) {
            if (ent_count >= ent_cap) {
                ent_cap *= 2;
                ent_t *tmp = realloc(ents, ent_cap * sizeof(ent_t));
                if (!tmp) break;
                ents = tmp;
            }
            ents[ent_count].left  = g_strdup_printf("%s  (not connected)", self);
            ents[ent_count].right = NULL;
            ents[ent_count].arrow = NULL;
            ents[ent_count].cat = cat;
            ents[ent_count].node_id = node->id;
            ent_count++;
        } else {
            for (size_t pi = 0; pi < np; pi++) {
                if (ent_count >= ent_cap) {
                    ent_cap *= 2;
                    ent_t *tmp = realloc(ents, ent_cap * sizeof(ent_t));
                    if (!tmp) break;
                    ents = tmp;
                }
                const evemon_pw_node_t *peer =
                    find_node(data, peers[pi]);
                const char *peer_lbl =
                    peer ? node_label(peer) : "(unknown)";

                if (cat == PW_CAT_OUTPUT) {
                    ents[ent_count].left  = utf8_sanitize(self);
                    ents[ent_count].right = utf8_sanitize(peer_lbl);
                    ents[ent_count].arrow = "\xe2\x86\x92";  /* → */
                } else if (cat == PW_CAT_INPUT) {
                    ents[ent_count].left  = utf8_sanitize(peer_lbl);
                    ents[ent_count].right = utf8_sanitize(self);
                    ents[ent_count].arrow = "\xe2\x86\x92";  /* → */
                } else {
                    ents[ent_count].left  = utf8_sanitize(self);
                    ents[ent_count].right = utf8_sanitize(peer_lbl);
                    ents[ent_count].arrow = "\xe2\x86\x94";  /* ↔ */
                }
                ents[ent_count].cat = cat;
                ents[ent_count].node_id = node->id;
                ent_count++;
            }
        }
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    /* Remember which node_id is currently selected so we can
     * re-select it after the store is rebuilt. */
    uint32_t sel_node_id = 0;
    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
        GtkTreeIter sel_iter;
        if (gtk_tree_selection_get_selected(sel, NULL, &sel_iter)) {
            guint nid = 0;
            gtk_tree_model_get(model, &sel_iter, COL_NODE_ID, &nid, -1);
            sel_node_id = nid;
        }
    }

    /* Update the flat list in place: reuse existing rows, append
     * new ones, remove excess. */
    GtkTreeIter iter;
    gboolean iter_valid = gtk_tree_model_iter_children(model, &iter, NULL);
    size_t ei = 0;

    while (ei < ent_count) {
        char *markup = pw_build_markup(ents[ei].left,
                                       ents[ei].right,
                                       ents[ei].arrow);
        char *plain;
        if (ents[ei].right && ents[ei].arrow)
            plain = g_strdup_printf("%s %s %s",
                ents[ei].left, ents[ei].arrow, ents[ei].right);
        else
            plain = g_strdup(ents[ei].left);

        if (iter_valid) {
            gtk_list_store_set(ctx->store, &iter,
                COL_TEXT, plain,
                COL_MARKUP, markup,
                COL_CAT, (gint)ents[ei].cat,
                COL_NODE_ID, (guint)ents[ei].node_id, -1);
            iter_valid = gtk_tree_model_iter_next(model, &iter);
        } else {
            GtkTreeIter new_iter;
            gtk_list_store_append(ctx->store, &new_iter);
            gtk_list_store_set(ctx->store, &new_iter,
                COL_TEXT, plain,
                COL_MARKUP, markup,
                COL_CAT, (gint)ents[ei].cat,
                COL_NODE_ID, (guint)ents[ei].node_id,
                COL_LEVEL_L, (gint)0,
                COL_LEVEL_R, (gint)0, -1);
        }
        g_free(markup);
        g_free(plain);
        ei++;
    }

    /* Remove excess rows */
    while (iter_valid)
        iter_valid = gtk_list_store_remove(ctx->store, &iter);

    gtk_adjustment_set_value(vadj, scroll_pos);

    /* Re-select the previously selected node by node_id. */
    if (sel_node_id != 0) {
        GtkTreeIter si;
        gboolean sv = gtk_tree_model_iter_children(model, &si, NULL);
        while (sv) {
            guint nid = 0;
            gtk_tree_model_get(model, &si, COL_NODE_ID, &nid, -1);
            if (nid == sel_node_id) {
                GtkTreeSelection *sel =
                    gtk_tree_view_get_selection(ctx->view);
                gtk_tree_selection_select_iter(sel, &si);
                break;
            }
            sv = gtk_tree_model_iter_next(model, &si);
        }
    }

    for (size_t i = 0; i < ent_count; i++) {
        g_free(ents[i].left);
        g_free(ents[i].right);
    }
    free(ents);

    /* ── Start/update peak meters and spectrogram ─────────────── */
    if (ctx->host) {
        if (ctx->audio_node_count > 0) {
            if (ctx->host->pw_meter_start)
                ctx->host->pw_meter_start(ctx->host->host_ctx,
                    ctx->audio_node_ids, ctx->audio_node_count);

            /* Start meter timer lazily on first data */
            if (!ctx->meter_timer)
                ctx->meter_timer = g_timeout_add(50, meter_tick, ctx);

            if (ctx->spectro_shown && ctx->host->spectro_start) {
                uint32_t current = 0;
                if (ctx->host->spectro_get_target)
                    current = ctx->host->spectro_get_target(
                        ctx->host->host_ctx,
                        GTK_DRAWING_AREA(ctx->spectro_draw));
                int found = 0;
                for (size_t i = 0; i < ctx->audio_node_count; i++)
                    if (ctx->audio_node_ids[i] == current)
                        { found = 1; break; }
                if (!found)
                    ctx->host->spectro_start(ctx->host->host_ctx,
                        GTK_DRAWING_AREA(ctx->spectro_draw),
                        ctx->audio_node_ids[0]);
            }
        } else {
            /* No audio output nodes for this process — stop our own
             * spectrogram (per-draw-area, safe for other instances).
             * Don't call pw_meter_stop or pw_meter_remove_nodes since
             * the meter is shared; stale streams read silence and are
             * cleaned up in pw_destroy(). */
            if (ctx->spectro_shown && ctx->spectro_draw &&
                ctx->host->spectro_stop)
                ctx->host->spectro_stop(ctx->host->host_ctx,
                    GTK_DRAWING_AREA(ctx->spectro_draw));
        }
    }

    /* ── Update MPRIS metadata display ────────────────────────── */
    pw_update_mpris(ctx, data);
}

static void pw_clear(void *opaque)
{
    pw_ctx_t *ctx = opaque;
    gtk_list_store_clear(ctx->store);
    ctx->last_pid = 0;
    ctx->audio_node_count = 0;
    /* Hide the media section so stale metadata isn't shown when
     * no process is selected. */
    if (ctx->media_section && GTK_IS_WIDGET(ctx->media_section))
        gtk_widget_hide(ctx->media_section);
    /* Clear album art */
    if (ctx->media_art_image && GTK_IS_IMAGE(ctx->media_art_image))
        gtk_image_clear(GTK_IMAGE(ctx->media_art_image));
    if (ctx->art_pixbuf) {
        g_object_unref(ctx->art_pixbuf);
        ctx->art_pixbuf = NULL;
    }
    ctx->art_url_cached[0] = '\0';
    /* Don't call pw_meter_stop here — the meter is shared. */
}

static void pw_destroy(void *opaque)
{
    pw_ctx_t *ctx = opaque;

    /* Unsubscribe from the event bus BEFORE freeing ctx,
     * otherwise deferred g_idle_add callbacks can dereference
     * the freed user_data pointer → segfault. */
    if (ctx->event_sub_id > 0 && ctx->host && ctx->host->unsubscribe) {
        ctx->host->unsubscribe(ctx->host->host_ctx, ctx->event_sub_id);
        ctx->event_sub_id = 0;
    }

    if (ctx->meter_timer) {
        g_source_remove(ctx->meter_timer);
        ctx->meter_timer = 0;
    }
    /* Don't call pw_meter_stop — the meter is shared across all plugin
     * instances.  We remove only our own nodes via pw_meter_remove_nodes
     * so other instances' streams remain intact. */
    if (ctx->host) {
        if (ctx->audio_node_count > 0 && ctx->host->pw_meter_remove_nodes)
            ctx->host->pw_meter_remove_nodes(ctx->host->host_ctx,
                ctx->audio_node_ids, ctx->audio_node_count);
        /* Stop our own spectrogram instance (per-draw-area, safe) */
        if (ctx->spectro_draw && ctx->host->spectro_stop)
            ctx->host->spectro_stop(ctx->host->host_ctx,
                GTK_DRAWING_AREA(ctx->spectro_draw));
    }
    if (ctx->store) {
        g_object_unref(ctx->store);
        ctx->store = NULL;
    }
    if (ctx->css) g_object_unref(ctx->css);
    if (ctx->art_pixbuf) g_object_unref(ctx->art_pixbuf);

    /* Null out widget pointers before free — defensive against
     * any stale references that might still try to touch them. */
    ctx->media_title_label    = NULL;
    ctx->media_artist_label   = NULL;
    ctx->media_album_label    = NULL;
    ctx->media_status_label   = NULL;
    ctx->media_position_label = NULL;
    ctx->media_art_image      = NULL;
    ctx->media_section        = NULL;

    free(ctx);
}

/* ── descriptor ──────────────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    pw_ctx_t *ctx = calloc(1, sizeof(pw_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "PipeWire Audio",
        .id            = "org.evemon.pipewire",
        .version       = "1.0",
        .data_needs    = evemon_NEED_PIPEWIRE | evemon_NEED_MPRIS,
        .plugin_ctx    = ctx,
        .create_widget = pw_create_widget,
        .update        = pw_update,
        .clear         = pw_clear,
        .destroy       = pw_destroy,
        .activate      = pw_activate,
    };

    return p;
}
