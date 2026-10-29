/*
 * gpu_plugin.c – Event-driven GPU dashboard for the System Panel.
 */

#include "evemon_plugin.h"
#include "../core/settings.h"
#include "gpu_nvtop_adapter.h"

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.gpu",
    "GPU",
    "2.0",
    EVEMON_ROLE_SYSTEM,
    NULL
);

static gint gpu_instance_claimed;

enum {
    METRIC_GPU_LOAD,
    METRIC_MEMORY,
    METRIC_TEMPERATURE,
    METRIC_POWER,
    METRIC_CORE_CLOCK,
    METRIC_MEMORY_CLOCK,
    METRIC_COUNT
};

#define GPU_HISTORY_LEN 120
#define GPU_CHART_FONT_BASE 14.0
#define GPU_CHART_FONT_TITLE 18.0
#define GPU_CHART_FONT_HOVER GPU_CHART_FONT_BASE

enum {
    CHART_GPU_LOAD,
    CHART_MEMORY,
    CHART_TEMPERATURE,
    CHART_POWER,
    CHART_CORE_CLOCK,
    CHART_MEMORY_CLOCK,
    CHART_PROCESS_COUNT,
    CHART_COUNT
};

typedef struct {
    const char *label;
    int         ticks;
    const char *window;
} gpu_resolution_t;

static const gpu_resolution_t gpu_resolutions[] = {
    { "1 sec",  1,   "2 min" },
    { "5 sec",  5,   "10 min" },
    { "30 sec", 30,  "60 min" },
    { "5 min",  300, "10 hr" },
};

#define GPU_RESOLUTION_COUNT ((int)G_N_ELEMENTS(gpu_resolutions))
#define GPU_RESOLUTION_DEFAULT 0

typedef struct gpu_ui_ctx gpu_ui_ctx_t;

typedef struct {
    gpu_ui_ctx_t *owner;
    GtkWidget    *area;
    int           tooltip_x;
} gpu_chart_t;

typedef struct {
    GtkWidget *value;
    GtkWidget *bar;
} metric_widget_t;

struct gpu_ui_ctx {
    const evemon_host_services_t *host;
    int                           subscription_id;
    GThread                      *sample_thread;
    gint                          stopping;
    evemon_gpu_backend_t         *backend;
    GtkWidget                    *root;
    GtkWidget                    *device_combo;
    GtkWidget                    *device_name;
    GtkWidget                    *device_detail;
    GtkWidget                    *resolution_combo;
    metric_widget_t               metrics[METRIC_COUNT];
    gpu_chart_t                    chart;
    double                         history[CHART_COUNT][GPU_HISTORY_LEN];
    uint8_t                        history_valid[CHART_COUNT][GPU_HISTORY_LEN];
    double                         scale_max[CHART_COUNT];
    double                         accumulator[CHART_COUNT];
    unsigned                       accumulator_count[CHART_COUNT];
    int                            history_head;
    int                            history_count;
    int                            resolution_index;
    int                            tick_accumulator;
    evemon_gpu_snapshot_t         snapshot;
    char                          selected_pdev[16];
    char                          history_pdev[16];
    int                           updating_combo;
};

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static gpointer gpu_sample_thread(gpointer opaque)
{
    gpu_ui_ctx_t *ctx = opaque;
    while (!g_atomic_int_get(&ctx->stopping)) {
        evemon_gpu_snapshot_t snapshot = {0};
        if (evemon_gpu_backend_snapshot(ctx->backend, &snapshot,
                                        monotonic_ns())) {
            if (ctx->host && ctx->host->publish) {
                evemon_event_t event = {
                    .type = EVEMON_EVENT_GPU_SNAPSHOT,
                    .payload = &snapshot,
                };
                ctx->host->publish(ctx->host->host_ctx, &event);
            }
            evemon_gpu_snapshot_clear(&snapshot);
        }

        for (int i = 0; i < 10 && !g_atomic_int_get(&ctx->stopping); i++)
            g_usleep(100000);
    }
    return NULL;
}

static void format_bytes(uint64_t bytes, char *buf, size_t bufsz)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < G_N_ELEMENTS(units)) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(buf, bufsz, "%" PRIu64 " %s", bytes, units[unit]);
    else
        snprintf(buf, bufsz, "%.1f %s", value, units[unit]);
}

static const char *chart_labels[CHART_COUNT] = {
    "GPU",
    "VRAM",
    "Temp",
    "Power",
    "Core Clock",
    "Memory Clock",
    "Processes",
};

static void format_chart_value(int chart, double value,
                               char *buf, size_t bufsz)
{
    switch (chart) {
    case CHART_GPU_LOAD:
        snprintf(buf, bufsz, "%.0f%%", value);
        break;
    case CHART_MEMORY:
        format_bytes(value > 0.0 ? (uint64_t)value : 0, buf, bufsz);
        break;
    case CHART_TEMPERATURE:
        snprintf(buf, bufsz, "%.0f °C", value);
        break;
    case CHART_POWER:
        snprintf(buf, bufsz, "%.1f W", value / 1000.0);
        break;
    case CHART_CORE_CLOCK:
    case CHART_MEMORY_CLOCK:
        snprintf(buf, bufsz, "%.0f MHz", value);
        break;
    case CHART_PROCESS_COUNT:
        snprintf(buf, bufsz, "%.0f", value);
        break;
    default:
        snprintf(buf, bufsz, "%.1f", value);
        break;
    }
}

/* Keep GPU history visually consistent with Network's chart: the same dark
 * canvas and colours sampled from the active spectrogram theme. */
static void chart_sample_colour(float mag, int theme,
                                double *r_out, double *g_out, double *b_out)
{
    float r = 0.0f, g = 0.0f, b = 0.0f;
    mag = CLAMP(mag, 0.0f, 1.0f);
    switch (theme) {
    default:
    case 0:
        if (mag < 0.25f) {
            float t = mag / 0.25f;
            b = 0.2f + 0.6f * t;
        } else if (mag < 0.50f) {
            float t = (mag - 0.25f) / 0.25f;
            g = t; b = 0.8f * (1.0f - t) + 0.2f * t;
        } else if (mag < 0.75f) {
            float t = (mag - 0.50f) / 0.25f;
            r = t; g = 1.0f; b = 0.2f * (1.0f - t);
        } else {
            float t = (mag - 0.75f) / 0.25f;
            r = 1.0f; g = 1.0f; b = t;
        }
        break;
    case 1:
        if (mag < 0.25f) {
            r = 0.5f * mag / 0.25f;
        } else if (mag < 0.50f) {
            r = 0.5f + 0.5f * (mag - 0.25f) / 0.25f;
        } else if (mag < 0.75f) {
            r = 1.0f; g = 0.5f * (mag - 0.50f) / 0.25f;
        } else {
            float t = (mag - 0.75f) / 0.25f;
            r = 1.0f; g = 0.5f + 0.5f * t; b = t;
        }
        break;
    case 2:
        if (mag < 0.33f) {
            float t = mag / 0.33f;
            r = 0.15f * t; b = 0.5f * t;
        } else if (mag < 0.66f) {
            float t = (mag - 0.33f) / 0.33f;
            r = 0.15f + 0.15f * t; g = 0.5f * t;
            b = 0.5f + 0.5f * t;
        } else {
            float t = (mag - 0.66f) / 0.34f;
            r = 0.30f + 0.70f * t; g = 0.5f + 0.5f * t; b = 1.0f;
        }
        break;
    case 3:
        r = g = b = mag;
        break;
    case 4:
        if (mag < 0.30f) {
            float t = mag / 0.30f;
            r = 0.6f * t; b = 0.6f * t;
        } else if (mag < 0.55f) {
            float t = (mag - 0.30f) / 0.25f;
            r = 0.6f + 0.4f * t; b = 0.6f - 0.3f * t;
        } else if (mag < 0.80f) {
            float t = (mag - 0.55f) / 0.25f;
            r = 1.0f - 0.8f * t; g = 0.7f * t; b = 0.3f + 0.7f * t;
        } else {
            float t = (mag - 0.80f) / 0.20f;
            r = 0.2f + 0.8f * t; g = 0.7f + 0.3f * t; b = 1.0f;
        }
        break;
    case 5:
        if (mag < 0.25f) {
            float t = mag / 0.25f;
            r = 0.04f + 0.10f * t; g = 0.02f + 0.03f * t;
            b = 0.18f + 0.32f * t;
        } else if (mag < 0.50f) {
            float t = (mag - 0.25f) / 0.25f;
            r = 0.14f + 0.56f * t; g = 0.05f; b = 0.50f + 0.10f * t;
        } else if (mag < 0.75f) {
            float t = (mag - 0.50f) / 0.25f;
            r = 0.70f + 0.10f * t; g = 0.05f + 0.65f * t;
            b = 0.60f + 0.30f * t;
        } else {
            float t = (mag - 0.75f) / 0.25f;
            r = 0.80f + 0.20f * t; g = 0.70f + 0.30f * t;
            b = 0.90f + 0.10f * t;
        }
        break;
    }

    float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
    if (luminance < 0.35f && luminance > 0.0f) {
        float scale = 0.35f / luminance;
        r = MIN(r * scale, 1.0f);
        g = MIN(g * scale, 1.0f);
        b = MIN(b * scale, 1.0f);
    } else if (luminance <= 0.0f) {
        r = g = b = 0.55f;
    }
    *r_out = r;
    *g_out = g;
    *b_out = b;
}

static void chart_colour(int chart, double *r, double *g, double *b)
{
    evemon_settings_t *settings = evemon_settings_get();
    int theme = settings ? settings->spectro_theme : 0;
    static const float low[6] = { 0.30f, 0.35f, 0.30f, 0.40f, 0.28f, 0.35f };
    static const float high[6] = { 0.95f, 0.95f, 0.95f, 0.95f, 0.95f, 0.95f };
    int selected = theme >= 0 && theme <= 5 ? theme : 0;
    float position = (float)chart / (float)(CHART_COUNT - 1);
    chart_sample_colour(low[selected] + (high[selected] - low[selected]) * position,
                        selected, r, g, b);
}

static void chart_text(cairo_t *cr, const char *text, double x, double y,
                       double size, const char *family,
                       double r, double g, double b, double alpha,
                       int *width_out, int *height_out)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, family);
    pango_font_description_set_absolute_size(font, (int)(size * PANGO_SCALE));
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_text(layout, text, -1);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    if (width_out) *width_out = width;
    if (height_out) *height_out = height;
}

static double chart_peak(const gpu_ui_ctx_t *ctx, int chart)
{
    double peak = ctx->scale_max[chart];
    for (int i = 0; i < ctx->history_count; i++) {
        int index = (ctx->history_head - ctx->history_count + i
                     + GPU_HISTORY_LEN) % GPU_HISTORY_LEN;
        if (ctx->history_valid[chart][index] &&
            ctx->history[chart][index] > peak)
            peak = ctx->history[chart][index];
    }
    if (peak <= 0.0) peak = 1.0;
    if (ctx->scale_max[chart] <= 0.0) peak *= 1.10;
    return peak;
}

static gboolean on_chart_motion(GtkWidget *widget, GdkEventMotion *event,
                                gpointer opaque)
{
    gpu_chart_t *chart = opaque;
    chart->tooltip_x = (int)event->x;
    gtk_widget_queue_draw(widget);
    return FALSE;
}

static gboolean on_chart_leave(GtkWidget *widget, GdkEventCrossing *event,
                               gpointer opaque)
{
    (void)event;
    gpu_chart_t *chart = opaque;
    chart->tooltip_x = -1;
    gtk_widget_queue_draw(widget);
    return FALSE;
}

static gboolean on_chart_draw(GtkWidget *widget, cairo_t *cr, gpointer opaque)
{
    gpu_chart_t *chart = opaque;
    gpu_ui_ctx_t *ctx = chart->owner;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    if (width <= 0 || height <= 0) return FALSE;

    cairo_set_source_rgb(cr, 0.06, 0.06, 0.09);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);

    const int left = 54;
    const int right = 8;
    const int top = 34;
    const int bottom = 82;
    int plot_width = width - left - right;
    int plot_height = height - top - bottom;
    if (plot_width < 16 || plot_height < 16) return FALSE;

    chart_text(cr, "GPU History", 8.0, 5.0, GPU_CHART_FONT_TITLE, "Sans",
               0.92, 0.92, 0.95, 1.0, NULL, NULL);
    const char *scale_note = "normalized per metric";
    int scale_note_width = 0;
    chart_text(cr, scale_note, 0.0, -100.0, GPU_CHART_FONT_BASE, "Sans",
               0.62, 0.62, 0.68, 0.9, &scale_note_width, NULL);
    chart_text(cr, scale_note, width - scale_note_width - 8.0, 7.0,
               GPU_CHART_FONT_BASE, "Sans", 0.62, 0.62, 0.68, 0.9,
               NULL, NULL);

    for (int grid = 0; grid <= 4; grid++) {
        double x = left + (double)plot_width * grid / 4.0;
        double y = top + (double)plot_height * grid / 4.0;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, left, y + 0.5);
        cairo_line_to(cr, left + plot_width, y + 0.5);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 0.5, top);
        cairo_line_to(cr, x + 0.5, top + plot_height);
        cairo_stroke(cr);
    }

    chart_text(cr, "100%", 4.0, top - 6.0, GPU_CHART_FONT_BASE, "Monospace",
               0.70, 0.70, 0.74, 0.9, NULL, NULL);
    chart_text(cr, "50%", 4.0, top + plot_height / 2.0 - 6.0,
               GPU_CHART_FONT_BASE, "Monospace",
               0.70, 0.70, 0.74, 0.9, NULL, NULL);
    chart_text(cr, "0", 4.0, top + plot_height - 8.0,
               GPU_CHART_FONT_BASE, "Monospace",
               0.70, 0.70, 0.74, 0.9, NULL, NULL);

    int count = ctx->history_count;
    if (count > 0) {
        double step = (double)plot_width / (double)GPU_HISTORY_LEN;
        double x_offset = left + (double)(GPU_HISTORY_LEN - count) * step;
        cairo_save(cr);
        cairo_rectangle(cr, left, top, plot_width, plot_height);
        cairo_clip(cr);
        for (int kind = 0; kind < CHART_COUNT; kind++) {
            double peak = chart_peak(ctx, kind);
            double line_r, line_g, line_b;
            chart_colour(kind, &line_r, &line_g, &line_b);
            gboolean path_open = FALSE;
            cairo_new_path(cr);
            for (int i = 0; i < count; i++) {
                int index = (ctx->history_head - count + i + GPU_HISTORY_LEN)
                            % GPU_HISTORY_LEN;
                if (!ctx->history_valid[kind][index]) {
                    if (path_open) {
                        cairo_set_source_rgba(cr, line_r, line_g, line_b, 0.95);
                        cairo_set_line_width(cr, 1.5);
                        cairo_stroke(cr);
                        path_open = FALSE;
                    }
                    continue;
                }
                double normalized = ctx->history[kind][index] / peak;
                normalized = CLAMP(normalized, 0.0, 1.0);
                double x = x_offset + i * step + step * 0.5;
                double y = top + plot_height - normalized * plot_height;
                if (!path_open) {
                    cairo_move_to(cr, x, y);
                    path_open = TRUE;
                } else {
                    cairo_line_to(cr, x, y);
                }
            }
            if (path_open) {
                cairo_set_source_rgba(cr, line_r, line_g, line_b, 0.95);
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
            }
        }
        cairo_restore(cr);

        if (chart->tooltip_x >= left &&
            chart->tooltip_x < left + plot_width) {
            int slot = (int)((chart->tooltip_x - left) / step);
            int first_slot = GPU_HISTORY_LEN - count;
            slot = CLAMP(slot, first_slot, GPU_HISTORY_LEN - 1);
            int age = slot - first_slot;
            int index = (ctx->history_head - count + age + GPU_HISTORY_LEN)
                        % GPU_HISTORY_LEN;
            double hairline_x = left + (slot + 0.5) * step;
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.28);
            cairo_move_to(cr, hairline_x, top);
            cairo_line_to(cr, hairline_x, top + plot_height);
            cairo_stroke(cr);

            char lines[CHART_COUNT + 1][64];
            int seconds_ago = (count - 1 - age)
                              * gpu_resolutions[ctx->resolution_index].ticks;
            if (seconds_ago == 0)
                snprintf(lines[0], sizeof(lines[0]), "now");
            else
                snprintf(lines[0], sizeof(lines[0]), "-%d s", seconds_ago);
            int box_width = 0;
            int line_height = 0;
            chart_text(cr, lines[0], 0.0, -100.0,
                       GPU_CHART_FONT_HOVER, "Monospace",
                       1.0, 1.0, 1.0, 1.0, &box_width, &line_height);
            for (int kind = 0; kind < CHART_COUNT; kind++) {
                char value[32] = "—";
                if (ctx->history_valid[kind][index])
                    format_chart_value(kind, ctx->history[kind][index],
                                       value, sizeof(value));
                snprintf(lines[kind + 1], sizeof(lines[kind + 1]),
                         "%-12s %s", chart_labels[kind], value);
                int text_width = 0;
                chart_text(cr, lines[kind + 1], 0.0, -100.0,
                           GPU_CHART_FONT_HOVER, "Monospace",
                           1.0, 1.0, 1.0, 1.0,
                           &text_width, NULL);
                if (text_width > box_width) box_width = text_width;
            }
            double box_x = hairline_x + 7.0;
            if (box_x + box_width + 12.0 > width)
                box_x = hairline_x - box_width - 19.0;
            double box_y = top + 6.0;
            double box_height = (CHART_COUNT + 1) * line_height + 8.0;
            cairo_set_source_rgba(cr, 0.02, 0.02, 0.04, 0.94);
            cairo_rectangle(cr, box_x, box_y, box_width + 12.0, box_height);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.28);
            cairo_rectangle(cr, box_x + 0.5, box_y + 0.5,
                            box_width + 11.0, box_height - 1.0);
            cairo_stroke(cr);
            chart_text(cr, lines[0], box_x + 6.0, box_y + 4.0,
                       GPU_CHART_FONT_HOVER, "Monospace",
                       0.95, 0.95, 0.97, 1.0,
                       NULL, NULL);
            for (int kind = 0; kind < CHART_COUNT; kind++) {
                double line_r, line_g, line_b;
                chart_colour(kind, &line_r, &line_g, &line_b);
                chart_text(cr, lines[kind + 1], box_x + 6.0,
                           box_y + 4.0 + (kind + 1) * line_height,
                           GPU_CHART_FONT_HOVER, "Monospace",
                           line_r, line_g, line_b, 1.0,
                           NULL, NULL);
            }
        }
    }

    chart_text(cr, gpu_resolutions[ctx->resolution_index].window,
               left, top + plot_height + 2.0,
               GPU_CHART_FONT_BASE, "Monospace",
               0.65, 0.65, 0.69, 0.9, NULL, NULL);
    int now_width = 0;
    chart_text(cr, "now", 0.0, -100.0, GPU_CHART_FONT_BASE, "Monospace",
               0.65, 0.65, 0.69, 0.9, &now_width, NULL);
    chart_text(cr, "now", left + plot_width - now_width,
               top + plot_height + 2.0, GPU_CHART_FONT_BASE, "Monospace",
               0.65, 0.65, 0.69, 0.9, NULL, NULL);

    double legend_width = (double)plot_width / 4.0;
    for (int kind = 0; kind < CHART_COUNT; kind++) {
        int row = kind / 4;
        int column = kind % 4;
        double x = left + column * legend_width;
        double y = top + plot_height + 25.0 + row * 22.0;
        double line_r, line_g, line_b;
        chart_colour(kind, &line_r, &line_g, &line_b);
        cairo_set_source_rgba(cr, line_r, line_g, line_b, 0.95);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, x, y + 5.0);
        cairo_line_to(cr, x + 12.0, y + 5.0);
        cairo_stroke(cr);
        chart_text(cr, chart_labels[kind], x + 16.0, y,
                   GPU_CHART_FONT_BASE, "Monospace",
                   line_r, line_g, line_b, 0.95, NULL, NULL);
    }
    return FALSE;
}

static GtkWidget *chart_card_new(gpu_ui_ctx_t *ctx)
{
    gpu_chart_t *chart = &ctx->chart;
    chart->owner = ctx;
    chart->tooltip_x = -1;

    GtkWidget *frame = gtk_frame_new(NULL);
    chart->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(chart->area, 360, 300);
    gtk_widget_set_hexpand(chart->area, TRUE);
    gtk_widget_set_vexpand(chart->area, TRUE);
    gtk_widget_add_events(chart->area,
                          GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(chart->area, "draw", G_CALLBACK(on_chart_draw), chart);
    g_signal_connect(chart->area, "motion-notify-event",
                     G_CALLBACK(on_chart_motion), chart);
    g_signal_connect(chart->area, "leave-notify-event",
                     G_CALLBACK(on_chart_leave), chart);
    gtk_container_add(GTK_CONTAINER(frame), chart->area);
    return frame;
}

static void reset_history(gpu_ui_ctx_t *ctx)
{
    memset(ctx->history, 0, sizeof(ctx->history));
    memset(ctx->history_valid, 0, sizeof(ctx->history_valid));
    memset(ctx->scale_max, 0, sizeof(ctx->scale_max));
    memset(ctx->accumulator, 0, sizeof(ctx->accumulator));
    memset(ctx->accumulator_count, 0, sizeof(ctx->accumulator_count));
    ctx->scale_max[CHART_GPU_LOAD] = 100.0;
    ctx->scale_max[CHART_TEMPERATURE] = 110.0;
    ctx->history_head = 0;
    ctx->history_count = 0;
    ctx->tick_accumulator = 0;
    if (ctx->chart.area)
        gtk_widget_queue_draw(ctx->chart.area);
}

static void add_history_sample(gpu_ui_ctx_t *ctx)
{
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(ctx->device_combo));
    if (active < 0 || (uint32_t)active >= ctx->snapshot.device_count) {
        if (ctx->history_pdev[0]) {
            ctx->history_pdev[0] = '\0';
            reset_history(ctx);
        }
        return;
    }

    const evemon_gpu_device_t *gpu = &ctx->snapshot.devices[active];
    if (strcmp(ctx->history_pdev, gpu->pdev) != 0) {
        reset_history(ctx);
        snprintf(ctx->history_pdev, sizeof(ctx->history_pdev), "%s", gpu->pdev);
    }

    double values[CHART_COUNT] = {
        gpu->utilization_percent,
        (double)gpu->memory_used_bytes,
        gpu->temperature_c,
        gpu->power_mw,
        gpu->core_clock_mhz,
        gpu->memory_clock_mhz,
        0.0,
    };
    gboolean valid[CHART_COUNT] = {
        (gpu->valid & EVEMON_GPU_VALID_UTILIZATION) != 0,
        (gpu->valid & EVEMON_GPU_VALID_MEMORY) != 0,
        (gpu->valid & EVEMON_GPU_VALID_TEMPERATURE) != 0,
        (gpu->valid & EVEMON_GPU_VALID_POWER) != 0,
        (gpu->valid & EVEMON_GPU_VALID_CORE_CLOCK) != 0,
        (gpu->valid & EVEMON_GPU_VALID_MEM_CLOCK) != 0,
        TRUE,
    };
    for (uint32_t i = 0; i < ctx->snapshot.process_count; i++)
        if (ctx->snapshot.processes[i].device_index == (uint32_t)active)
            values[CHART_PROCESS_COUNT] += 1.0;

    if (valid[CHART_MEMORY] && gpu->memory_total_bytes > 0)
        ctx->scale_max[CHART_MEMORY] = (double)gpu->memory_total_bytes;
    if ((gpu->valid & EVEMON_GPU_VALID_POWER_LIMIT) && gpu->power_limit_mw > 0)
        ctx->scale_max[CHART_POWER] = gpu->power_limit_mw;
    if (valid[CHART_CORE_CLOCK] && gpu->core_clock_max_mhz > 0)
        ctx->scale_max[CHART_CORE_CLOCK] = gpu->core_clock_max_mhz;
    if (valid[CHART_MEMORY_CLOCK] && gpu->memory_clock_max_mhz > 0)
        ctx->scale_max[CHART_MEMORY_CLOCK] = gpu->memory_clock_max_mhz;

    for (int i = 0; i < CHART_COUNT; i++) {
        if (!valid[i]) continue;
        if (i == CHART_PROCESS_COUNT) {
            if (ctx->accumulator_count[i] == 0 ||
                values[i] > ctx->accumulator[i])
                ctx->accumulator[i] = values[i];
            ctx->accumulator_count[i] = 1;
        } else {
            ctx->accumulator[i] += values[i];
            ctx->accumulator_count[i]++;
        }
    }

    ctx->tick_accumulator++;
    int ticks = gpu_resolutions[ctx->resolution_index].ticks;
    if (ctx->tick_accumulator < ticks) return;

    for (int i = 0; i < CHART_COUNT; i++) {
        if (ctx->accumulator_count[i] > 0) {
            ctx->history[i][ctx->history_head] =
                ctx->accumulator[i] / ctx->accumulator_count[i];
            ctx->history_valid[i][ctx->history_head] = 1;
        } else {
            ctx->history[i][ctx->history_head] = 0.0;
            ctx->history_valid[i][ctx->history_head] = 0;
        }
    }
    ctx->history_head = (ctx->history_head + 1) % GPU_HISTORY_LEN;
    if (ctx->history_count < GPU_HISTORY_LEN) ctx->history_count++;
    ctx->tick_accumulator = 0;
    memset(ctx->accumulator, 0, sizeof(ctx->accumulator));
    memset(ctx->accumulator_count, 0, sizeof(ctx->accumulator_count));
    if (ctx->chart.area)
        gtk_widget_queue_draw(ctx->chart.area);
}

static void on_resolution_changed(GtkComboBox *combo, gpointer opaque)
{
    gpu_ui_ctx_t *ctx = opaque;
    int index = gtk_combo_box_get_active(combo);
    if (index < 0 || index >= GPU_RESOLUTION_COUNT)
        index = GPU_RESOLUTION_DEFAULT;
    if (index == ctx->resolution_index) return;
    ctx->resolution_index = index;
    reset_history(ctx);
}

static void metric_set(metric_widget_t *metric, const char *text,
                       double fraction, int valid)
{
    gtk_label_set_text(GTK_LABEL(metric->value), valid ? text : "—");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(metric->bar),
                                  valid ? CLAMP(fraction, 0.0, 1.0) : 0.0);
    gtk_widget_set_sensitive(metric->bar, valid);
}

static GtkWidget *metric_card_new(const char *title, metric_widget_t *metric)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    gtk_container_add(GTK_CONTAINER(frame), box);

    GtkWidget *title_label = gtk_label_new(title);
    gtk_widget_set_halign(title_label, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(title_label),
                                "dim-label");

    metric->value = gtk_label_new("—");
    gtk_widget_set_halign(metric->value, GTK_ALIGN_START);

    metric->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(metric->bar, 130, 4);
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(metric->bar), FALSE);

    gtk_box_pack_start(GTK_BOX(box), title_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), metric->value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), metric->bar, FALSE, FALSE, 0);
    return frame;
}

static void clear_metrics(gpu_ui_ctx_t *ctx)
{
    for (int i = 0; i < METRIC_COUNT; i++)
        metric_set(&ctx->metrics[i], "—", 0.0, 0);
}

static void render_device(gpu_ui_ctx_t *ctx)
{
    if (!ctx->root) return;

    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(ctx->device_combo));
    if (active < 0 || (uint32_t)active >= ctx->snapshot.device_count) {
        gtk_label_set_text(GTK_LABEL(ctx->device_name),
                           "No supported GPU or accelerator found");
        gtk_label_set_text(GTK_LABEL(ctx->device_detail),
                           "Waiting for a GPU snapshot from the service");
        clear_metrics(ctx);
        return;
    }

    const evemon_gpu_device_t *gpu = &ctx->snapshot.devices[active];
    snprintf(ctx->selected_pdev, sizeof(ctx->selected_pdev), "%s", gpu->pdev);
    gtk_label_set_text(GTK_LABEL(ctx->device_name), gpu->name);

    GString *detail = g_string_new(NULL);
    if (gpu->vendor[0]) g_string_append(detail, gpu->vendor);
    if (gpu->driver[0]) {
        if (detail->len) g_string_append(detail, "  •  ");
        g_string_append(detail, gpu->driver);
    }
    if (gpu->pdev[0]) {
        if (detail->len) g_string_append(detail, "  •  ");
        g_string_append(detail, gpu->pdev);
    }
    if (gpu->valid & EVEMON_GPU_VALID_PCIE_LINK) {
        if (detail->len) g_string_append(detail, "  •  ");
        if (gpu->pcie_generation)
            g_string_append_printf(detail, "PCIe Gen %u ×%u",
                                   gpu->pcie_generation, gpu->pcie_width);
        else
            g_string_append_printf(detail, "PCIe ×%u", gpu->pcie_width);
    }
    if (gpu->valid & EVEMON_GPU_VALID_FAN_PERCENT) {
        if (detail->len) g_string_append(detail, "  •  ");
        g_string_append_printf(detail, "Fan %u%%", gpu->fan_percent);
        if (gpu->valid & EVEMON_GPU_VALID_FAN_RPM)
            g_string_append_printf(detail, " / %u RPM", gpu->fan_rpm);
    } else if (gpu->valid & EVEMON_GPU_VALID_FAN_RPM) {
        if (detail->len) g_string_append(detail, "  •  ");
        g_string_append_printf(detail, "Fan %u RPM", gpu->fan_rpm);
    }
    gtk_label_set_text(GTK_LABEL(ctx->device_detail), detail->str);
    g_string_free(detail, TRUE);

    char value[128];
    snprintf(value, sizeof(value), "%u%%", gpu->utilization_percent);
    metric_set(&ctx->metrics[METRIC_GPU_LOAD], value,
               gpu->utilization_percent / 100.0,
               gpu->valid & EVEMON_GPU_VALID_UTILIZATION);

    char used[32], total[32];
    format_bytes(gpu->memory_used_bytes, used, sizeof(used));
    format_bytes(gpu->memory_total_bytes, total, sizeof(total));
    snprintf(value, sizeof(value), "%s / %s", used, total);
    double memory_fraction = gpu->memory_total_bytes > 0
        ? (double)gpu->memory_used_bytes / (double)gpu->memory_total_bytes : 0.0;
    metric_set(&ctx->metrics[METRIC_MEMORY], value, memory_fraction,
               gpu->valid & EVEMON_GPU_VALID_MEMORY);

    snprintf(value, sizeof(value), "%u °C", gpu->temperature_c);
    metric_set(&ctx->metrics[METRIC_TEMPERATURE], value,
               gpu->temperature_c / 110.0,
               gpu->valid & EVEMON_GPU_VALID_TEMPERATURE);

    if (gpu->valid & EVEMON_GPU_VALID_POWER_LIMIT)
        snprintf(value, sizeof(value), "%.1f / %.1f W",
                 gpu->power_mw / 1000.0, gpu->power_limit_mw / 1000.0);
    else
        snprintf(value, sizeof(value), "%.1f W", gpu->power_mw / 1000.0);
    double power_fraction = gpu->power_limit_mw > 0
        ? (double)gpu->power_mw / (double)gpu->power_limit_mw
        : gpu->power_mw / 500000.0;
    metric_set(&ctx->metrics[METRIC_POWER], value, power_fraction,
               gpu->valid & EVEMON_GPU_VALID_POWER);

    if (gpu->core_clock_max_mhz)
        snprintf(value, sizeof(value), "%u / %u MHz",
                 gpu->core_clock_mhz, gpu->core_clock_max_mhz);
    else
        snprintf(value, sizeof(value), "%u MHz", gpu->core_clock_mhz);
    double core_fraction = gpu->core_clock_max_mhz > 0
        ? (double)gpu->core_clock_mhz / gpu->core_clock_max_mhz : 0.0;
    metric_set(&ctx->metrics[METRIC_CORE_CLOCK], value, core_fraction,
               gpu->valid & EVEMON_GPU_VALID_CORE_CLOCK);

    if (gpu->memory_clock_max_mhz)
        snprintf(value, sizeof(value), "%u / %u MHz",
                 gpu->memory_clock_mhz, gpu->memory_clock_max_mhz);
    else
        snprintf(value, sizeof(value), "%u MHz", gpu->memory_clock_mhz);
    double clock_fraction = gpu->memory_clock_max_mhz > 0
        ? (double)gpu->memory_clock_mhz / gpu->memory_clock_max_mhz : 0.0;
    metric_set(&ctx->metrics[METRIC_MEMORY_CLOCK], value, clock_fraction,
               gpu->valid & EVEMON_GPU_VALID_MEM_CLOCK);
}

static void rebuild_device_combo(gpu_ui_ctx_t *ctx)
{
    int wanted = 0;
    if (ctx->selected_pdev[0]) {
        for (uint32_t i = 0; i < ctx->snapshot.device_count; i++)
            if (strcmp(ctx->snapshot.devices[i].pdev,
                       ctx->selected_pdev) == 0) {
                wanted = (int)i;
                break;
            }
    }

    ctx->updating_combo = 1;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ctx->device_combo));
    for (uint32_t i = 0; i < ctx->snapshot.device_count; i++) {
        const evemon_gpu_device_t *gpu = &ctx->snapshot.devices[i];
        char id[16];
        char label[256];
        snprintf(id, sizeof(id), "%u", i);
        snprintf(label, sizeof(label), "%s — %s", gpu->vendor, gpu->name);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ctx->device_combo),
                                  id, label);
    }
    if (ctx->snapshot.device_count > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->device_combo), wanted);
    gtk_widget_set_sensitive(ctx->device_combo,
                             ctx->snapshot.device_count > 1);
    ctx->updating_combo = 0;
}

static void on_device_changed(GtkComboBox *combo, gpointer opaque)
{
    (void)combo;
    gpu_ui_ctx_t *ctx = opaque;
    if (!ctx->updating_combo) {
        render_device(ctx);
        add_history_sample(ctx);
    }
}

static void on_gpu_snapshot(const evemon_event_t *event, void *opaque)
{
    gpu_ui_ctx_t *ctx = opaque;
    if (!event || event->type != EVEMON_EVENT_GPU_SNAPSHOT || !event->payload)
        return;

    const evemon_gpu_snapshot_t *src = event->payload;
    evemon_gpu_device_t *devices = NULL;
    evemon_gpu_process_t *processes = NULL;
    if (src->device_count > 0) {
        devices = g_try_malloc_n(src->device_count, sizeof(*devices));
        if (!devices) return;
        memcpy(devices, src->devices,
               (size_t)src->device_count * sizeof(*devices));
    }
    if (src->process_count > 0) {
        processes = g_try_malloc_n(src->process_count, sizeof(*processes));
        if (!processes) {
            g_free(devices);
            return;
        }
        memcpy(processes, src->processes,
               (size_t)src->process_count * sizeof(*processes));
    }
    g_free(ctx->snapshot.devices);
    g_free(ctx->snapshot.processes);
    ctx->snapshot = *src;
    ctx->snapshot.devices = devices;
    ctx->snapshot.processes = processes;
    if (!ctx->root || !gtk_widget_get_mapped(ctx->root)) return;
    rebuild_device_combo(ctx);
    render_device(ctx);
    add_history_sample(ctx);
}

static GtkWidget *gpu_create_widget(void *opaque)
{
    gpu_ui_ctx_t *ctx = opaque;

    GtkWidget *root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_set_border_width(GTK_CONTAINER(root), 8);
    ctx->root = root;

    GtkWidget *summary = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_size_request(summary, 520, -1);
    gtk_widget_set_margin_end(summary, 8);
    gtk_paned_pack1(GTK_PANED(root), summary, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(root), 520);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ctx->device_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(ctx->device_combo, 300, -1);
    g_signal_connect(ctx->device_combo, "changed",
                     G_CALLBACK(on_device_changed), ctx);
    gtk_box_pack_start(GTK_BOX(header), ctx->device_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(summary), header, FALSE, FALSE, 0);

    ctx->device_name = gtk_label_new("Waiting for GPU service…");
    gtk_widget_set_halign(ctx->device_name, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(ctx->device_name), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(summary), ctx->device_name, FALSE, FALSE, 0);

    ctx->device_detail = gtk_label_new("");
    gtk_widget_set_halign(ctx->device_detail, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(ctx->device_detail), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(ctx->device_detail),
                                "dim-label");
    gtk_box_pack_start(GTK_BOX(summary), ctx->device_detail, FALSE, FALSE, 0);

    GtkWidget *metrics_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(metrics_grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(metrics_grid), 5);
    static const char *metric_titles[METRIC_COUNT] = {
        "GPU Load", "VRAM", "Temperature", "Power", "Core Clock",
        "Memory Clock"
    };
    for (int i = 0; i < METRIC_COUNT; i++) {
        GtkWidget *card = metric_card_new(metric_titles[i], &ctx->metrics[i]);
        gtk_widget_set_hexpand(card, TRUE);
        gtk_grid_attach(GTK_GRID(metrics_grid), card, i % 3, i / 3, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(summary), metrics_grid, FALSE, FALSE, 0);

    GtkWidget *chart_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(chart_panel), 0);
    gtk_widget_set_margin_start(chart_panel, 8);

    GtkWidget *history_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *history_label = gtk_label_new("History");
    gtk_widget_set_halign(history_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(history_header), history_label, TRUE, TRUE, 0);

    ctx->resolution_combo = gtk_combo_box_text_new();
    for (int i = 0; i < GPU_RESOLUTION_COUNT; i++)
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(ctx->resolution_combo),
            gpu_resolutions[i].label);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->resolution_combo),
                             GPU_RESOLUTION_DEFAULT);
    g_signal_connect(ctx->resolution_combo, "changed",
                     G_CALLBACK(on_resolution_changed), ctx);
    gtk_box_pack_end(GTK_BOX(history_header), ctx->resolution_combo,
                     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(chart_panel), history_header, FALSE, FALSE, 0);

    GtkWidget *chart = chart_card_new(ctx);
    gtk_widget_set_hexpand(chart, TRUE);
    gtk_widget_set_vexpand(chart, TRUE);
    gtk_box_pack_start(GTK_BOX(chart_panel), chart, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(root), chart_panel, TRUE, FALSE);

    clear_metrics(ctx);
    reset_history(ctx);
    return root;
}

static void gpu_activate(void *opaque,
                         const evemon_host_services_t *services)
{
    gpu_ui_ctx_t *ctx = opaque;
    ctx->host = services;
    if (services && services->subscribe)
        ctx->subscription_id = services->subscribe(
            services->host_ctx, EVEMON_EVENT_GPU_SNAPSHOT,
            on_gpu_snapshot, ctx);
    if (services && services->publish && ctx->backend &&
        !ctx->sample_thread) {
        g_atomic_int_set(&ctx->stopping, 0);
        ctx->sample_thread = g_thread_new(
            "evemon-gpu", gpu_sample_thread, ctx);
    }
}

static void gpu_update(void *opaque, const evemon_proc_data_t *data)
{
    (void)opaque;
    (void)data;
    /* All GPU data arrives through EVEMON_EVENT_GPU_SNAPSHOT. */
}

static void gpu_clear(void *opaque)
{
    gpu_ui_ctx_t *ctx = opaque;
    g_free(ctx->snapshot.devices);
    g_free(ctx->snapshot.processes);
    memset(&ctx->snapshot, 0, sizeof(ctx->snapshot));
    ctx->selected_pdev[0] = '\0';
    ctx->history_pdev[0] = '\0';
    reset_history(ctx);
    if (!ctx->root) return;
    ctx->updating_combo = 1;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ctx->device_combo));
    ctx->updating_combo = 0;
    render_device(ctx);
}

static void gpu_destroy(void *opaque)
{
    gpu_ui_ctx_t *ctx = opaque;
    if (!ctx) return;
    g_atomic_int_set(&ctx->stopping, 1);
    if (ctx->sample_thread)
        g_thread_join(ctx->sample_thread);
    if (ctx->host && ctx->host->unsubscribe && ctx->subscription_id)
        ctx->host->unsubscribe(ctx->host->host_ctx, ctx->subscription_id);
    g_free(ctx->snapshot.devices);
    g_free(ctx->snapshot.processes);
    evemon_gpu_backend_free(ctx->backend);
    free(ctx);
    g_atomic_int_set(&gpu_instance_claimed, 0);
}

static int gpu_wants_update(void *opaque)
{
    gpu_ui_ctx_t *ctx = opaque;
    return ctx->root && GTK_IS_WIDGET(ctx->root) &&
           gtk_widget_get_mapped(ctx->root) &&
           gtk_widget_get_child_visible(ctx->root);
}

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    if (!g_atomic_int_compare_and_exchange(&gpu_instance_claimed, 0, 1))
        return NULL;

    gpu_ui_ctx_t *ctx = calloc(1, sizeof(*ctx));
    evemon_plugin_t *plugin = calloc(1, sizeof(*plugin));
    if (!ctx || !plugin) {
        free(ctx);
        free(plugin);
        g_atomic_int_set(&gpu_instance_claimed, 0);
        return NULL;
    }
    ctx->backend = evemon_gpu_backend_new();
    if (!ctx->backend || evemon_gpu_backend_init(ctx->backend) == 0) {
        evemon_gpu_backend_free(ctx->backend);
        free(ctx);
        free(plugin);
        g_atomic_int_set(&gpu_instance_claimed, 0);
        return NULL;
    }

    *plugin = (evemon_plugin_t){
        .abi_version = evemon_PLUGIN_ABI_VERSION,
        .name = "GPU",
        .id = "org.evemon.gpu",
        .version = "2.0",
        .data_needs = 0,
        .plugin_ctx = ctx,
        .create_widget = gpu_create_widget,
        .update = gpu_update,
        .clear = gpu_clear,
        .destroy = gpu_destroy,
        .activate = gpu_activate,
        .is_available = NULL,
        .role = EVEMON_ROLE_SYSTEM,
        .dependencies = NULL,
        .wants_update = gpu_wants_update,
    };
    return plugin;
}
