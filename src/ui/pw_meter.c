/*
 * pw_meter.c – real-time L/R peak level meters for PipeWire audio nodes.
 *
 * Maintains a set of passive stereo capture streams (one per audio
 * output node) that compute peak levels in their PW process callbacks.
 * A GTK timer reads the levels and updates the tree store.
 *
 * Also provides a custom GtkCellRenderer that draws inline L/R
 * meter bars with green→yellow→red colour gradient.
 *
 * Gated behind HAVE_PIPEWIRE.
 */

#ifdef HAVE_PIPEWIRE

#include "ui_internal.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

/* ── per-node meter stream ───────────────────────────────────── */

#define METER_MAX_NODES   64
#define METER_DECAY       0.85f   /* peak hold decay per tick (~50ms) */

typedef struct {
    uint32_t           node_id;
    struct pw_stream  *stream;
    struct spa_hook    listener;

    /* Written by PW RT thread, read by GTK timer */
    atomic_int         peak_l;    /* 0..1000 */
    atomic_int         peak_r;    /* 0..1000 */

    /* Accumulation within one process cycle */
    float              frame_peak_l;
    float              frame_peak_r;
} meter_node_t;

typedef struct {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;

    meter_node_t           nodes[METER_MAX_NODES];
    size_t                 node_count;

    /* Smoothed display values (updated by GTK timer) */
    float                  display_l[METER_MAX_NODES];
    float                  display_r[METER_MAX_NODES];
} pw_meter_state_t;

/* ── PW stream process callback (RT thread) ──────────────────── */

static void meter_on_process(void *data)
{
    meter_node_t *mn = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(mn->stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (!buf || !buf->datas[0].data)
        goto done;

    uint32_t n_bytes = buf->datas[0].chunk ? buf->datas[0].chunk->size : 0;
    if (n_bytes == 0)
        goto done;

    {
        float *ch0 = buf->datas[0].data;
        float *ch1 = (buf->n_datas >= 2 && buf->datas[1].data)
                     ? buf->datas[1].data : ch0;
        uint32_t n_samples = n_bytes / sizeof(float);

        float pk_l = 0.0f, pk_r = 0.0f;
        for (uint32_t i = 0; i < n_samples; i++) {
            float al = fabsf(ch0[i]);
            float ar = fabsf(ch1[i]);
            if (al > pk_l) pk_l = al;
            if (ar > pk_r) pk_r = ar;
        }

        /* Clamp to [0, 1] */
        if (pk_l > 1.0f) pk_l = 1.0f;
        if (pk_r > 1.0f) pk_r = 1.0f;

        /* Store as 0..1000 for atomic int transport */
        int il = (int)(pk_l * 1000.0f + 0.5f);
        int ir = (int)(pk_r * 1000.0f + 0.5f);

        /* Keep the max of the current stored peak (avoids flicker
         * when the GTK timer hasn't consumed the previous peak yet) */
        int old_l = atomic_load_explicit(&mn->peak_l, memory_order_relaxed);
        int old_r = atomic_load_explicit(&mn->peak_r, memory_order_relaxed);
        if (il > old_l)
            atomic_store_explicit(&mn->peak_l, il, memory_order_relaxed);
        if (ir > old_r)
            atomic_store_explicit(&mn->peak_r, ir, memory_order_relaxed);
    }

done:
    pw_stream_queue_buffer(mn->stream, b);
}

static void meter_on_state(void *data,
                           enum pw_stream_state old,
                           enum pw_stream_state state,
                           const char *error)
{
    (void)data; (void)old; (void)state; (void)error;
}

static const struct pw_stream_events meter_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process       = meter_on_process,
    .state_changed = meter_on_state,
};

/* ── start / stop ────────────────────────────────────────────── */

void pw_meter_stop(ui_ctx_t *ctx)
{
    if (ctx->pw_meter_timer) {
        g_source_remove(ctx->pw_meter_timer);
        ctx->pw_meter_timer = 0;
    }

    pw_meter_state_t *ms = ctx->pw_meter;
    if (!ms) return;

    if (ms->loop) {
        pw_thread_loop_stop(ms->loop);

        for (size_t i = 0; i < ms->node_count; i++) {
            if (ms->nodes[i].stream) {
                pw_stream_disconnect(ms->nodes[i].stream);
                pw_stream_destroy(ms->nodes[i].stream);
            }
        }
        if (ms->core)
            pw_core_disconnect(ms->core);
        if (ms->context)
            pw_context_destroy(ms->context);
        pw_thread_loop_destroy(ms->loop);
    }

    free(ms);
    ctx->pw_meter = NULL;
}

/* GTK timer: read peaks, apply decay, update tree store */
static gboolean meter_tick(gpointer data)
{
    ui_ctx_t *ctx = data;
    pw_meter_state_t *ms = ctx->pw_meter;
    if (!ms) return G_SOURCE_REMOVE;

    for (size_t i = 0; i < ms->node_count; i++) {
        meter_node_t *mn = &ms->nodes[i];

        /* Consume the peak (reset to 0 after reading) */
        int raw_l = atomic_exchange_explicit(&mn->peak_l, 0, memory_order_relaxed);
        int raw_r = atomic_exchange_explicit(&mn->peak_r, 0, memory_order_relaxed);

        float new_l = raw_l / 1000.0f;
        float new_r = raw_r / 1000.0f;

        /* Fast attack, slow decay */
        if (new_l >= ms->display_l[i])
            ms->display_l[i] = new_l;
        else
            ms->display_l[i] = ms->display_l[i] * METER_DECAY;

        if (new_r >= ms->display_r[i])
            ms->display_r[i] = new_r;
        else
            ms->display_r[i] = ms->display_r[i] * METER_DECAY;
    }

    /* Update tree store levels for all audio output leaf rows */
    GtkTreeModel *model = GTK_TREE_MODEL(ctx->pw_store);
    GtkTreeIter parent;
    gboolean pvalid = gtk_tree_model_iter_children(model, &parent, NULL);
    int any_changed = 0;

    while (pvalid) {
        GtkTreeIter child;
        gboolean cvalid = gtk_tree_model_iter_children(model, &child, &parent);
        while (cvalid) {
            guint nid = 0;
            gtk_tree_model_get(model, &child, PW_COL_NODE_ID, &nid, -1);
            if (nid != 0) {
                /* Find this node in our meter array */
                for (size_t i = 0; i < ms->node_count; i++) {
                    if (ms->nodes[i].node_id == nid) {
                        int ll = (int)(ms->display_l[i] * 1000.0f + 0.5f);
                        int lr = (int)(ms->display_r[i] * 1000.0f + 0.5f);
                        gtk_tree_store_set(ctx->pw_store, &child,
                                           PW_COL_LEVEL_L, ll,
                                           PW_COL_LEVEL_R, lr, -1);
                        any_changed = 1;
                        break;
                    }
                }
            }
            cvalid = gtk_tree_model_iter_next(model, &child);
        }
        pvalid = gtk_tree_model_iter_next(model, &parent);
    }

    if (any_changed)
        gtk_widget_queue_draw(GTK_WIDGET(ctx->pw_view));

    return G_SOURCE_CONTINUE;
}

void pw_meter_start(ui_ctx_t *ctx, const uint32_t *node_ids, size_t count)
{
    /* If already monitoring the same set of nodes, keep going */
    pw_meter_state_t *old = ctx->pw_meter;
    if (old && old->node_count == count) {
        int same = 1;
        for (size_t i = 0; i < count; i++) {
            int found = 0;
            for (size_t j = 0; j < old->node_count; j++) {
                if (old->nodes[j].node_id == node_ids[i]) { found = 1; break; }
            }
            if (!found) { same = 0; break; }
        }
        if (same) return;
    }

    pw_meter_stop(ctx);
    if (count == 0) return;
    if (count > METER_MAX_NODES) count = METER_MAX_NODES;

    pw_meter_state_t *ms = calloc(1, sizeof(*ms));
    if (!ms) return;

    /* Ensure PIPEWIRE_REMOTE is set */
    const char *sudo_uid = getenv("SUDO_UID");
    if (sudo_uid && !getenv("PIPEWIRE_REMOTE")) {
        char buf[256];
        snprintf(buf, sizeof(buf), "/run/user/%s/pipewire-0", sudo_uid);
        setenv("PIPEWIRE_REMOTE", buf, 1);
    }

    pw_init(NULL, NULL);

    ms->loop = pw_thread_loop_new("allmon-meter", NULL);
    if (!ms->loop) { free(ms); return; }

    ms->context = pw_context_new(pw_thread_loop_get_loop(ms->loop), NULL, 0);
    if (!ms->context) goto fail;

    pw_thread_loop_start(ms->loop);
    pw_thread_loop_lock(ms->loop);

    ms->core = pw_context_connect(ms->context, NULL, 0);
    if (!ms->core) {
        pw_thread_loop_unlock(ms->loop);
        goto fail;
    }

    /* Create one stereo capture stream per node */
    uint8_t pod_buf[1024];
    struct spa_pod_builder b;
    struct spa_audio_info_raw raw_info;
    const struct spa_pod *params[1];

    for (size_t i = 0; i < count; i++) {
        meter_node_t *mn = &ms->nodes[ms->node_count];
        mn->node_id = node_ids[i];
        atomic_store(&mn->peak_l, 0);
        atomic_store(&mn->peak_r, 0);

        char stream_name[64];
        snprintf(stream_name, sizeof(stream_name), "allmon-meter-%u", node_ids[i]);

        mn->stream = pw_stream_new(
            ms->core, stream_name,
            pw_properties_new(
                PW_KEY_MEDIA_TYPE,     "Audio",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE,     "DSP",
                PW_KEY_NODE_PASSIVE,   "true",
                PW_KEY_NODE_LATENCY,   "1024/48000",
                NULL));
        if (!mn->stream) continue;

        pw_stream_add_listener(mn->stream, &mn->listener,
                               &meter_stream_events, mn);

        /* F32 stereo, no forced rate */
        b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
        raw_info = SPA_AUDIO_INFO_RAW_INIT(
            .format   = SPA_AUDIO_FORMAT_F32,
            .channels = 2
        );
        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw_info);

        int ret = pw_stream_connect(mn->stream,
                                    PW_DIRECTION_INPUT,
                                    node_ids[i],
                                    PW_STREAM_FLAG_AUTOCONNECT |
                                    PW_STREAM_FLAG_MAP_BUFFERS |
                                    PW_STREAM_FLAG_RT_PROCESS,
                                    params, 1);
        if (ret < 0) {
            pw_stream_destroy(mn->stream);
            mn->stream = NULL;
            continue;
        }

        ms->node_count++;
    }

    pw_thread_loop_unlock(ms->loop);

    ctx->pw_meter = ms;

    /* Start a ~50ms GTK timer for smooth meter updates */
    ctx->pw_meter_timer = g_timeout_add(50, meter_tick, ctx);
    return;

fail:
    if (ms->loop)
        pw_thread_loop_stop(ms->loop);
    if (ms->core)
        pw_core_disconnect(ms->core);
    if (ms->context)
        pw_context_destroy(ms->context);
    if (ms->loop)
        pw_thread_loop_destroy(ms->loop);
    free(ms);
}

void pw_meter_read(ui_ctx_t *ctx, uint32_t node_id,
                   int *level_l, int *level_r)
{
    *level_l = 0;
    *level_r = 0;
    pw_meter_state_t *ms = ctx->pw_meter;
    if (!ms) return;
    for (size_t i = 0; i < ms->node_count; i++) {
        if (ms->nodes[i].node_id == node_id) {
            *level_l = (int)(ms->display_l[i] * 1000.0f + 0.5f);
            *level_r = (int)(ms->display_r[i] * 1000.0f + 0.5f);
            return;
        }
    }
}

/* ── Custom GtkCellRenderer for L/R meter bars ───────────────── */

/*
 * PwCellRendererMeter – draws two horizontal bars (L and R)
 * with a green→yellow→red colour gradient.
 *
 * Properties:
 *   "level-l"  gint  0..1000  left channel level
 *   "level-r"  gint  0..1000  right channel level
 */

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

G_DEFINE_TYPE(PwCellRendererMeter, pw_cell_renderer_meter, GTK_TYPE_CELL_RENDERER)

static void pw_cell_renderer_meter_get_property(GObject *obj, guint id,
                                                 GValue *val, GParamSpec *pspec)
{
    PwCellRendererMeter *self = (PwCellRendererMeter *)obj;
    switch (id) {
    case PROP_LEVEL_L: g_value_set_int(val, self->level_l); break;
    case PROP_LEVEL_R: g_value_set_int(val, self->level_r); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void pw_cell_renderer_meter_set_property(GObject *obj, guint id,
                                                 const GValue *val,
                                                 GParamSpec *pspec)
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
    /* Green (0.0) → Yellow (0.6) → Red (1.0) */
    if (level < 0.6f) {
        float t = level / 0.6f;
        *r = t;
        *g = 0.8f;
        *b = 0.1f;
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

    /* Don't draw anything if both channels are silent */
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

        /* Gradient fill: draw in segments */
        for (int px = 0; px < bw; px++) {
            float seg_level = (float)px / (float)w;
            float r, g, b;
            meter_color(seg_level, &r, &g, &b);
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
            float seg_level = (float)px / (float)w;
            float r, g, b;
            meter_color(seg_level, &r, &g, &b);
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

    obj_class->get_property = pw_cell_renderer_meter_get_property;
    obj_class->set_property = pw_cell_renderer_meter_set_property;

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

GtkCellRenderer *pw_cell_renderer_meter_new(void)
{
    return g_object_new(PW_TYPE_CELL_RENDERER_METER, NULL);
}

#endif /* HAVE_PIPEWIRE */
