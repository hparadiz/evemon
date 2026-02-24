/*
 * sparkline.c – per-process I/O history sparkline custom cell renderer.
 *
 * Draws a tiny animated bar-chart sparkline in each tree row showing
 * the recent I/O rate history.  New samples slide in from the right
 * with a smooth animation, and each bar has a glow effect that pulses
 * when I/O is active.
 *
 * The sparkline data is stored as a packed string of semicolon-separated
 * float values in the tree store column COL_IO_SPARKLINE.  The renderer
 * parses this at draw time and renders a mini chart using Cairo.
 */

#include "ui_internal.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── sparkline custom cell renderer ──────────────────────────── */

#define SPARKLINE_TYPE_CELL_RENDERER (sparkline_cell_renderer_get_type())

typedef struct _SparklineCellRenderer      SparklineCellRenderer;
typedef struct _SparklineCellRendererClass  SparklineCellRendererClass;

struct _SparklineCellRenderer {
    GtkCellRenderer parent;
    gchar *data;           /* semicolon-separated float values       */
    gint   peak;           /* current peak value ×1000 for glow      */
};

struct _SparklineCellRendererClass {
    GtkCellRendererClass parent_class;
};

enum {
    PROP_SPARK_0,
    PROP_SPARK_DATA,
    PROP_SPARK_PEAK,
};

G_DEFINE_TYPE(SparklineCellRenderer, sparkline_cell_renderer, GTK_TYPE_CELL_RENDERER)

static void sparkline_cell_renderer_finalize(GObject *obj)
{
    SparklineCellRenderer *self = (SparklineCellRenderer *)obj;
    g_free(self->data);
    G_OBJECT_CLASS(sparkline_cell_renderer_parent_class)->finalize(obj);
}

static void sparkline_get_property(GObject *obj, guint id,
                                   GValue *val, GParamSpec *pspec)
{
    SparklineCellRenderer *self = (SparklineCellRenderer *)obj;
    switch (id) {
    case PROP_SPARK_DATA: g_value_set_string(val, self->data); break;
    case PROP_SPARK_PEAK: g_value_set_int(val, self->peak);    break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void sparkline_set_property(GObject *obj, guint id,
                                   const GValue *val, GParamSpec *pspec)
{
    SparklineCellRenderer *self = (SparklineCellRenderer *)obj;
    switch (id) {
    case PROP_SPARK_DATA:
        g_free(self->data);
        self->data = g_value_dup_string(val);
        break;
    case PROP_SPARK_PEAK:
        self->peak = g_value_get_int(val);
        break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

/* ── dimensions ──────────────────────────────────────────────── */

#define SPARK_WIDTH       80    /* total width of the sparkline area  */
#define SPARK_HEIGHT      14    /* height of the chart area           */
#define SPARK_PAD         2     /* horizontal padding                 */
#define SPARK_BAR_GAP     1     /* gap between bars                   */

static void sparkline_get_preferred_width(GtkCellRenderer *cell,
                                          GtkWidget *widget,
                                          gint *minimum, gint *natural)
{
    (void)cell; (void)widget;
    if (minimum) *minimum = SPARK_WIDTH + SPARK_PAD * 2;
    if (natural) *natural = SPARK_WIDTH + SPARK_PAD * 2;
}

static void sparkline_get_preferred_height(GtkCellRenderer *cell,
                                           GtkWidget *widget,
                                           gint *minimum, gint *natural)
{
    (void)cell; (void)widget;
    if (minimum) *minimum = SPARK_HEIGHT + 2;
    if (natural) *natural = SPARK_HEIGHT + 2;
}

/* ── colour helpers ──────────────────────────────────────────── */

/*
 * Map a normalised level [0..1] to a colour:
 *   0.0 = cool blue/cyan
 *   0.5 = warm yellow/amber
 *   1.0 = hot red/white
 */
static void spark_color(float level, float *r, float *g, float *b)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    if (level < 0.35f) {
        /* Blue → Cyan */
        float t = level / 0.35f;
        *r = 0.15f + 0.05f * t;
        *g = 0.45f + 0.35f * t;
        *b = 0.85f - 0.15f * t;
    } else if (level < 0.65f) {
        /* Cyan → Yellow/Amber */
        float t = (level - 0.35f) / 0.30f;
        *r = 0.20f + 0.70f * t;
        *g = 0.80f + 0.10f * t;
        *b = 0.70f - 0.55f * t;
    } else {
        /* Amber → Red */
        float t = (level - 0.65f) / 0.35f;
        *r = 0.90f + 0.10f * t;
        *g = 0.90f - 0.65f * t;
        *b = 0.15f + 0.10f * t;
    }
}

/* ── parse sparkline data ────────────────────────────────────── */

#define SPARK_MAX_SAMPLES  IO_HISTORY_LEN

static int parse_sparkline(const char *data, float *out, int max)
{
    if (!data || !data[0])
        return 0;

    int n = 0;
    const char *p = data;
    while (*p && n < max) {
        char *end = NULL;
        float v = strtof(p, &end);
        if (end == p) break;
        out[n++] = v;
        if (*end == ';') end++;
        p = end;
    }
    return n;
}

/* ── render ──────────────────────────────────────────────────── */

static void sparkline_render(GtkCellRenderer *cell, cairo_t *cr,
                             GtkWidget *widget,
                             const GdkRectangle *bg_area,
                             const GdkRectangle *cell_area,
                             GtkCellRendererState flags)
{
    (void)widget; (void)bg_area; (void)flags;
    SparklineCellRenderer *self = (SparklineCellRenderer *)cell;

    float samples[SPARK_MAX_SAMPLES];
    int n = parse_sparkline(self->data, samples, SPARK_MAX_SAMPLES);

    /* Nothing to draw if no data or all zeros */
    if (n == 0) return;

    int all_zero = 1;
    for (int i = 0; i < n; i++) {
        if (samples[i] > 0.001f) { all_zero = 0; break; }
    }
    if (all_zero) return;

    /* Find the max value for normalisation.
     * Use a minimum floor so very low rates still show something. */
    float max_val = 1024.0f;   /* minimum 1 KiB/s floor */
    for (int i = 0; i < n; i++) {
        if (samples[i] > max_val)
            max_val = samples[i];
    }

    int x0 = cell_area->x + SPARK_PAD;
    int y0 = cell_area->y + (cell_area->height - SPARK_HEIGHT) / 2;
    int chart_w = SPARK_WIDTH;
    int chart_h = SPARK_HEIGHT;

    /* Compute bar width from number of samples */
    int bar_w = (chart_w - (n - 1) * SPARK_BAR_GAP) / n;
    if (bar_w < 2) bar_w = 2;

    /* Draw subtle dark background track */
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, 0.5);
    cairo_rectangle(cr, x0, y0, chart_w, chart_h);
    cairo_fill(cr);

    /* Draw each bar right-aligned (newest sample = rightmost bar) */
    int total_bar_w = n * bar_w + (n - 1) * SPARK_BAR_GAP;
    int offset_x = chart_w - total_bar_w;  /* right-align bars */
    if (offset_x < 0) offset_x = 0;

    /* Current peak for glow effect (normalised 0..1) */
    float peak_norm = self->peak / 1000.0f;
    if (peak_norm > 1.0f) peak_norm = 1.0f;

    for (int i = 0; i < n; i++) {
        float norm = samples[i] / max_val;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

        int bh = (int)(norm * chart_h + 0.5f);
        if (bh < 1 && samples[i] > 0.001f) bh = 1;  /* at least 1px for non-zero */
        if (bh > chart_h) bh = chart_h;

        int bx = x0 + offset_x + i * (bar_w + SPARK_BAR_GAP);
        int by = y0 + chart_h - bh;

        if (bh <= 0) continue;

        /* Bar fill: gradient from bottom to top */
        float r, g, b;
        spark_color(norm, &r, &g, &b);

        /* Glow: newest bars (i near n-1) pulse brighter when peak is high.
         * Apply a subtle brightness boost based on recency and peak. */
        float recency = (float)i / (float)(n > 1 ? n - 1 : 1);
        float glow = peak_norm * recency * 0.3f;

        /* Draw bar with a vertical gradient (lighter at top for 3D effect) */
        for (int py = 0; py < bh; py++) {
            float frac = (float)py / (float)(bh > 1 ? bh - 1 : 1);
            /* frac 0 = top of bar, 1 = bottom of bar */
            float bright = 1.0f + glow + 0.20f * (1.0f - frac);
            float dr = r * bright;
            float dg = g * bright;
            float db = b * bright;
            if (dr > 1.0f) dr = 1.0f;
            if (dg > 1.0f) dg = 1.0f;
            if (db > 1.0f) db = 1.0f;

            /* Alpha: slight fade for older samples */
            float alpha = 0.6f + 0.4f * recency;
            cairo_set_source_rgba(cr, dr, dg, db, alpha);
            cairo_rectangle(cr, bx, by + py, bar_w, 1);
            cairo_fill(cr);
        }

        /* Top edge highlight (bright line for polish) */
        if (bh > 2) {
            float hr, hg, hb;
            spark_color(norm, &hr, &hg, &hb);
            float highlight = 1.3f + glow;
            hr *= highlight; hg *= highlight; hb *= highlight;
            if (hr > 1.0f) hr = 1.0f;
            if (hg > 1.0f) hg = 1.0f;
            if (hb > 1.0f) hb = 1.0f;
            cairo_set_source_rgba(cr, hr, hg, hb, 0.7f + 0.3f * recency);
            cairo_rectangle(cr, bx, by, bar_w, 1);
            cairo_fill(cr);
        }
    }

    /* Subtle top border line for the chart area */
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.35, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x0 + chart_w, y0);
    cairo_stroke(cr);
}

/* ── GObject class setup ─────────────────────────────────────── */

static void sparkline_cell_renderer_class_init(SparklineCellRendererClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    GtkCellRendererClass *cell_class = GTK_CELL_RENDERER_CLASS(klass);

    obj_class->finalize     = sparkline_cell_renderer_finalize;
    obj_class->get_property = sparkline_get_property;
    obj_class->set_property = sparkline_set_property;

    cell_class->get_preferred_width  = sparkline_get_preferred_width;
    cell_class->get_preferred_height = sparkline_get_preferred_height;
    cell_class->render               = sparkline_render;

    g_object_class_install_property(obj_class, PROP_SPARK_DATA,
        g_param_spec_string("spark-data", "Sparkline Data",
                            "Semicolon-separated float values",
                            NULL, G_PARAM_READWRITE));
    g_object_class_install_property(obj_class, PROP_SPARK_PEAK,
        g_param_spec_int("spark-peak", "Sparkline Peak",
                         "Current peak value * 1000 for glow",
                         0, G_MAXINT, 0, G_PARAM_READWRITE));
}

static void sparkline_cell_renderer_init(SparklineCellRenderer *self)
{
    self->data = NULL;
    self->peak = 0;
}

GtkCellRenderer *sparkline_cell_renderer_new(void)
{
    return g_object_new(SPARKLINE_TYPE_CELL_RENDERER, NULL);
}
