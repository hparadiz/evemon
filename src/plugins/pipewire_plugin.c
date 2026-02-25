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

static const char *cat_labels[PW_CAT_COUNT] = {
    [PW_CAT_OUTPUT] = "Audio Output",
    [PW_CAT_INPUT]  = "Audio Input",
    [PW_CAT_VIDEO]  = "Video",
    [PW_CAT_MIDI]   = "MIDI",
    [PW_CAT_OTHER]  = "Other",
};

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
    if (fl < 0.0f) fl = 0.0f; if (fl > 1.0f) fl = 1.0f;
    if (fr < 0.0f) fr = 0.0f; if (fr > 1.0f) fr = 1.0f;
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
    /* Connection tree */
    GtkWidget      *main_box;
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    GtkCssProvider *css;
    unsigned        collapsed;
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

static void on_row_collapsed(GtkTreeView *v, GtkTreeIter *it,
                             GtkTreePath *p, gpointer data)
{
    (void)v; (void)p;
    pw_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), it, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < PW_CAT_COUNT) ctx->collapsed |= (1u << cat);
}

static void on_row_expanded(GtkTreeView *v, GtkTreeIter *it,
                            GtkTreePath *p, gpointer data)
{
    (void)v; (void)p;
    pw_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), it, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < PW_CAT_COUNT) ctx->collapsed &= ~(1u << cat);
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *ev,
                             gpointer data)
{
    (void)data;
    if (ev->keyval != GDK_KEY_Return && ev->keyval != GDK_KEY_KP_Enter)
        return FALSE;

    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return FALSE;

    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, COL_CAT, &cat_id, -1);
    if (cat_id < 0) return FALSE;

    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    if (!path) return FALSE;

    if (gtk_tree_view_row_expanded(view, path))
        gtk_tree_view_collapse_row(view, path);
    else
        gtk_tree_view_expand_row(view, path, FALSE);

    gtk_tree_path_free(path);
    return TRUE;
}

/*
 * Double-click a leaf row to switch spectrogram target.
 */
static void on_row_activated(GtkTreeView *view, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    pw_ctx_t *ctx = data;

    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;

    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, COL_CAT, &cat_id, -1);
    if (cat_id >= 0) return;

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
        target = ctx->host->spectro_get_target(ctx->host->host_ctx);

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
    GtkTreeIter parent;
    gboolean pvalid = gtk_tree_model_iter_children(model, &parent, NULL);
    int any_changed = 0;

    while (pvalid) {
        GtkTreeIter child;
        gboolean cv = gtk_tree_model_iter_children(model, &child, &parent);
        while (cv) {
            guint nid = 0;
            gtk_tree_model_get(model, &child, COL_NODE_ID, &nid, -1);
            if (nid != 0) {
                int ll = 0, lr = 0;
                ctx->host->pw_meter_read(ctx->host->host_ctx,
                                         nid, &ll, &lr);
                gtk_tree_store_set(ctx->store, &child,
                                   COL_LEVEL_L, ll, COL_LEVEL_R, lr, -1);
                if (ll > 0 || lr > 0) any_changed = 1;
            }
            cv = gtk_tree_model_iter_next(model, &child);
        }
        pvalid = gtk_tree_model_iter_next(model, &parent);
    }

    if (any_changed)
        gtk_widget_queue_draw(GTK_WIDGET(ctx->view));

    return G_SOURCE_CONTINUE;
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *pw_create_widget(void *opaque)
{
    pw_ctx_t *ctx = opaque;

    ctx->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* ── Connection tree ──────────────────────────────────────── */
    ctx->store = gtk_tree_store_new(NUM_COLS,
                                    G_TYPE_STRING, G_TYPE_STRING,
                                    G_TYPE_INT, G_TYPE_UINT,
                                    G_TYPE_INT, G_TYPE_INT);
    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    /* NOTE: we keep our own ref to ctx->store (don't unref here)
     * so that the meter timer can safely access it.  We release
     * the ref in pw_destroy(). */

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);
    gtk_tree_view_set_enable_tree_lines(ctx->view, TRUE);

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

    g_signal_connect(ctx->view, "row-collapsed",
                     G_CALLBACK(on_row_collapsed), ctx);
    g_signal_connect(ctx->view, "row-expanded",
                     G_CALLBACK(on_row_expanded), ctx);
    g_signal_connect(ctx->view, "key-press-event",
                     G_CALLBACK(on_key_press), ctx);
    g_signal_connect(ctx->view, "row-activated",
                     G_CALLBACK(on_row_activated), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);

    gtk_box_pack_start(GTK_BOX(ctx->main_box), ctx->scroll, TRUE, TRUE, 0);

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
    gtk_box_pack_start(GTK_BOX(ctx->main_box), ctx->spectro_section,
                       FALSE, FALSE, 4);

    gtk_widget_show_all(ctx->main_box);

    return ctx->main_box;
}

static void pw_activate(void *opaque,
                        const evemon_host_services_t *services)
{
    pw_ctx_t *ctx = opaque;
    ctx->host = services;
    /* Meter timer is started lazily in pw_update() once we have audio nodes,
     * to avoid running before the tree store is fully ready. */
}

static void pw_update(void *opaque, const evemon_proc_data_t *data)
{
    pw_ctx_t *ctx = opaque;

    if (!data->pw_nodes || data->pw_node_count == 0) {
        gtk_tree_store_clear(ctx->store);
        if (ctx->host) {
            if (ctx->host->pw_meter_stop)
                ctx->host->pw_meter_stop(ctx->host->host_ctx);
            if (ctx->host->spectro_stop)
                ctx->host->spectro_stop(ctx->host->host_ctx);
        }
        ctx->audio_node_count = 0;
        return;
    }

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
        ctx->spectro_shown = FALSE;
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

    /* Count per category */
    size_t cat_count[PW_CAT_COUNT] = {0};
    for (size_t i = 0; i < ent_count; i++)
        cat_count[ents[i].cat]++;

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeIter cat_iters[PW_CAT_COUNT];
    gboolean cat_exists[PW_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean v = gtk_tree_model_iter_children(model, &top, NULL);
        while (v) {
            gint cid = -1;
            gtk_tree_model_get(model, &top, COL_CAT, &cid, -1);
            if (cid >= 0 && cid < PW_CAT_COUNT) {
                cat_iters[cid] = top;
                cat_exists[cid] = TRUE;
            }
            v = gtk_tree_model_iter_next(model, &top);
        }
    }

    for (int c = 0; c < PW_CAT_COUNT; c++)
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }

    for (int c = 0; c < PW_CAT_COUNT; c++) {
        if (cat_count[c] == 0) continue;

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s (%zu)", cat_labels[c],
                 cat_count[c]);
        char *hdr_esc = g_markup_escape_text(hdr, -1);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            gtk_tree_store_append(ctx->store, &parent, NULL);
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr, COL_MARKUP, hdr_esc,
                               COL_CAT, (gint)c,
                               COL_NODE_ID, (guint)0,
                               COL_LEVEL_L, (gint)0,
                               COL_LEVEL_R, (gint)0, -1);
            cat_exists[c] = TRUE;
            cat_iters[c] = parent;
        } else {
            parent = cat_iters[c];
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr,
                               COL_MARKUP, hdr_esc, -1);
        }
        g_free(hdr_esc);

        GtkTreeIter child;
        gboolean cv = gtk_tree_model_iter_children(model, &child,
                                                   &parent);
        for (size_t i = 0; i < ent_count; i++) {
            if (ents[i].cat != c) continue;
            char *markup = pw_build_markup(ents[i].left,
                                           ents[i].right,
                                           ents[i].arrow);

            /* Build plain-text display string */
            char *plain;
            if (ents[i].right && ents[i].arrow)
                plain = g_strdup_printf("%s %s %s",
                    ents[i].left, ents[i].arrow, ents[i].right);
            else
                plain = g_strdup(ents[i].left);

            if (cv) {
                gtk_tree_store_set(ctx->store, &child,
                    COL_TEXT, plain,
                    COL_MARKUP, markup,
                    COL_CAT, (gint)-1,
                    COL_NODE_ID, (guint)ents[i].node_id, -1);
                cv = gtk_tree_model_iter_next(model, &child);
            } else {
                GtkTreeIter nc;
                gtk_tree_store_append(ctx->store, &nc, &parent);
                gtk_tree_store_set(ctx->store, &nc,
                    COL_TEXT, plain,
                    COL_MARKUP, markup,
                    COL_CAT, (gint)-1,
                    COL_NODE_ID, (guint)ents[i].node_id,
                    COL_LEVEL_L, (gint)0,
                    COL_LEVEL_R, (gint)0, -1);
            }
            g_free(markup);
            g_free(plain);
        }

        while (cv) cv = gtk_tree_store_remove(ctx->store, &child);

        GtkTreePath *cp = gtk_tree_model_get_path(model,
                                                   &cat_iters[c]);
        if (ctx->collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->view, cp);
        else
            gtk_tree_view_expand_row(ctx->view, cp, FALSE);
        gtk_tree_path_free(cp);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);
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
                        ctx->host->host_ctx);
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
            if (ctx->host->pw_meter_stop)
                ctx->host->pw_meter_stop(ctx->host->host_ctx);
            if (ctx->host->spectro_stop)
                ctx->host->spectro_stop(ctx->host->host_ctx);
        }
    }
}

static void pw_clear(void *opaque)
{
    pw_ctx_t *ctx = opaque;
    gtk_tree_store_clear(ctx->store);
    ctx->last_pid = 0;
    ctx->audio_node_count = 0;
    if (ctx->host) {
        if (ctx->host->pw_meter_stop)
            ctx->host->pw_meter_stop(ctx->host->host_ctx);
        if (ctx->host->spectro_stop)
            ctx->host->spectro_stop(ctx->host->host_ctx);
    }
}

static void pw_destroy(void *opaque)
{
    pw_ctx_t *ctx = opaque;
    if (ctx->meter_timer) {
        g_source_remove(ctx->meter_timer);
        ctx->meter_timer = 0;
    }
    if (ctx->host) {
        if (ctx->host->pw_meter_stop)
            ctx->host->pw_meter_stop(ctx->host->host_ctx);
        if (ctx->host->spectro_stop)
            ctx->host->spectro_stop(ctx->host->host_ctx);
    }
    if (ctx->store) {
        g_object_unref(ctx->store);
        ctx->store = NULL;
    }
    if (ctx->css) g_object_unref(ctx->css);
    free(ctx);
}

/* ── descriptor ──────────────────────────────────────────────── */

static evemon_plugin_t pw_plugin;

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    pw_ctx_t *ctx = calloc(1, sizeof(pw_ctx_t));
    if (!ctx) return NULL;

    pw_plugin = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "PipeWire Audio",
        .id            = "org.evemon.pipewire",
        .version       = "1.0",
        .data_needs    = evemon_NEED_PIPEWIRE,
        .plugin_ctx    = ctx,
        .create_widget = pw_create_widget,
        .update        = pw_update,
        .clear         = pw_clear,
        .destroy       = pw_destroy,
        .activate      = pw_activate,
    };

    return &pw_plugin;
}
