#include "../ipc/query.h"
#include "pipewire_internal.h"

/* ── UTF-8 helpers ───────────────────────────────────────────── */

/*
 * Sanitise a PipeWire metadata string for GTK/Pango.
 * Returns a heap-allocated valid-UTF-8 string (caller frees with g_free).
 * - Replaces invalid byte sequences with U+FFFD.
 * - Returns "" for NULL input.
 */
char *utf8_sanitize(const char *raw)
{
    if (!raw || !raw[0]) return g_strdup("");
    if (g_utf8_validate(raw, -1, NULL)) return g_strdup(raw);
    return g_utf8_make_valid(raw, -1);
}

/* ── BPM detection ───────────────────────────────────────────── */

/*
 * Onset-based BPM detector using autocorrelation.
 *
 * We sample the peak meter levels at ~50ms intervals (same rate as
 * the existing meter timer).  From the combined L+R energy we derive
 * an onset envelope by computing the positive first derivative
 * (energy increase = onset).  An autocorrelation over the onset
 * history reveals periodic peaks whose lag corresponds to the beat
 * period.
 *
 * BPM range:  60–200 BPM (lags 300ms – 1000ms at 50ms resolution).
 * History:    ~10 seconds (200 samples at 50ms).
 * Output:     smoothed BPM value updated once per second.
 */


/*
 * Track-wide BPM histogram — accumulates weighted votes across an
 * entire track.  Each 1-BPM-wide bin from 60..199 collects confidence-
 * weighted votes from every autocorrelation cycle.  The bin with the
 * highest total weight is the dominant track tempo.
 */


/*
 * Reset the BPM detector — called on track change or seek.
 * Clears all history so the new track starts with a clean slate.
 */
void bpm_reset(bpm_state_t *bs)
{
    memset(bs->energy, 0, sizeof(bs->energy));
    memset(bs->onset, 0, sizeof(bs->onset));
    bs->head           = 0;
    bs->filled         = 0;
    bs->bpm            = 0.0f;
    bs->confidence     = 0.0f;
    bs->update_counter = 0;
    memset(bs->histo, 0, sizeof(bs->histo));
    bs->vote_count     = 0;
    bs->track_bpm      = 0.0f;
}

/*
 * Feed a new energy sample (0..1 combined L+R peak) into the detector.
 * Called from the meter timer at ~50ms intervals.
 *
 * Silent frames (energy below the onset threshold) are skipped entirely:
 * they cannot contribute an onset, and accumulating them would waste
 * autocorrelation CPU time and grow vote_count without bound for long
 * paused/silent tracks.
 */
static void bpm_feed(bpm_state_t *bs, float energy)
{
    /* Gate: ignore silence so we don't process or vote on empty audio */
    if (energy < BPM_ONSET_THRESH * 0.5f)
        return;

    int prev = (bs->head + BPM_HIST_LEN - 1) % BPM_HIST_LEN;
    float prev_e = (bs->filled > 0) ? bs->energy[prev] : 0.0f;

    bs->energy[bs->head] = energy;

    /* Onset = positive energy derivative (half-wave rectified) */
    float diff = energy - prev_e;
    bs->onset[bs->head] = (diff > BPM_ONSET_THRESH) ? diff : 0.0f;

    bs->head = (bs->head + 1) % BPM_HIST_LEN;
    if (bs->filled < BPM_HIST_LEN) bs->filled++;

    /* Only recompute BPM every ~1 second (20 samples) */
    bs->update_counter++;
    if (bs->update_counter < 20) return;
    bs->update_counter = 0;

    /* Need at least 4 seconds of data for reliable detection */
    if (bs->filled < 80) return;

    /*
     * Autocorrelation of the onset envelope.
     * We scan lags corresponding to 60–200 BPM:
     *   200 BPM = 300ms  → lag  6 samples (300/50)
     *   60  BPM = 1000ms → lag 20 samples (1000/50)
     */
    int min_lag = (int)(60000.0f / (BPM_MAX * BPM_SAMPLE_MS));  /* 6  */
    int max_lag = (int)(60000.0f / (BPM_MIN * BPM_SAMPLE_MS));  /* 20 */
    if (max_lag >= bs->filled / 2) max_lag = bs->filled / 2 - 1;
    if (min_lag < 1) min_lag = 1;
    if (max_lag <= min_lag) return;

    int    n = bs->filled;
    float  best_corr = 0.0f;
    int    best_lag  = 0;

    /* Compute energy of the onset signal for normalisation */
    float norm = 0.0f;
    for (int i = 0; i < n; i++) {
        int idx = (bs->head + BPM_HIST_LEN - n + i) % BPM_HIST_LEN;
        norm += bs->onset[idx] * bs->onset[idx];
    }
    if (norm < 1e-8f) return;  /* silence — no onsets at all */

    for (int lag = min_lag; lag <= max_lag; lag++) {
        float sum = 0.0f;
        int count = n - lag;
        for (int i = 0; i < count; i++) {
            int idx_a = (bs->head + BPM_HIST_LEN - n + i) % BPM_HIST_LEN;
            int idx_b = (idx_a + lag) % BPM_HIST_LEN;
            sum += bs->onset[idx_a] * bs->onset[idx_b];
        }
        float corr = sum / norm;
        if (corr > best_corr) {
            best_corr = corr;
            best_lag  = lag;
        }
    }

    bs->confidence = best_corr;

    if (best_lag > 0 && best_corr >= BPM_CONFIDENCE_MIN) {
        float period_ms = (float)best_lag * BPM_SAMPLE_MS;
        float raw_bpm = 60000.0f / period_ms;

        /* Clamp to valid range */
        if (raw_bpm < BPM_MIN) raw_bpm *= 2.0f;
        if (raw_bpm > BPM_MAX) raw_bpm *= 0.5f;

        if (bs->bpm < 1.0f)
            bs->bpm = raw_bpm;   /* first reading — snap */
        else
            bs->bpm = bs->bpm * (1.0f - BPM_SMOOTH) + raw_bpm * BPM_SMOOTH;

        /* ── Accumulate into track-wide histogram ────────────── */
        /* Cast a confidence-weighted vote into the 1-BPM-wide bin.
         * Over a full track, the dominant tempo accumulates the
         * most weight regardless of intros/bridges/breakdowns. */
        int bin = (int)(raw_bpm + 0.5f) - BPM_HISTO_BASE;
        if (bin >= 0 && bin < BPM_HISTO_BINS) {
            bs->histo[bin] += best_corr;
            bs->vote_count++;
        }

        /* Recompute track BPM from the histogram peak.
         * Use a 3-bin weighted average around the peak for
         * sub-BPM precision (e.g. 127.3 instead of 127). */
        if (bs->vote_count >= 3) {
            int best_bin = 0;
            float best_w = 0.0f;
            for (int b = 0; b < BPM_HISTO_BINS; b++) {
                if (bs->histo[b] > best_w) {
                    best_w = bs->histo[b];
                    best_bin = b;
                }
            }
            float w_sum = bs->histo[best_bin];
            float bpm_sum = (float)(best_bin + BPM_HISTO_BASE) * w_sum;
            if (best_bin > 0) {
                w_sum   += bs->histo[best_bin - 1];
                bpm_sum += (float)(best_bin - 1 + BPM_HISTO_BASE)
                           * bs->histo[best_bin - 1];
            }
            if (best_bin < BPM_HISTO_BINS - 1) {
                w_sum   += bs->histo[best_bin + 1];
                bpm_sum += (float)(best_bin + 1 + BPM_HISTO_BASE)
                           * bs->histo[best_bin + 1];
            }
            bs->track_bpm = (w_sum > 0.0f) ? bpm_sum / w_sum : 0.0f;

            /*
             * Normalise the histogram periodically to prevent unbounded
             * accumulation on long tracks (hours of continuous playback).
             * Once vote_count reaches the cap, divide all bins by the
             * peak bin weight so relative proportions are preserved but
             * magnitudes stay near 1.0.  This keeps float precision
             * intact and ensures vote_count never overflows an int.
             */
            if (bs->vote_count >= BPM_HISTO_VOTE_CAP && best_w > 0.0f) {
                for (int b = 0; b < BPM_HISTO_BINS; b++)
                    bs->histo[b] /= best_w;
                /* Reset vote_count to half the cap so we normalise again
                 * after another half-cap's worth of new votes, not immediately. */
                bs->vote_count = BPM_HISTO_VOTE_CAP / 2;
            }
        }
    }
}

/* ── categories ──────────────────────────────────────────────── */

/* ── custom GtkCellRenderer for L/R meter bars ───────────────── */

#ifdef EVEMON_QT_WIDGETS
GtkCellRenderer *pw_meter_renderer_new(void) { return gtk_cell_renderer_text_new(); }
#else
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

GtkCellRenderer *pw_meter_renderer_new(void)
{
    return g_object_new(PW_TYPE_CELL_RENDERER_METER, NULL);
}

#endif

/* ── per-instance state ──────────────────────────────────────── */


/* ── classification ──────────────────────────────────────────── */

int classify_node(const char *media_class)
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
const char *node_label(const evemon_pw_node_t *n)
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
int is_evemon_node(const evemon_pw_node_t *n)
{
    if (!n) return 0;
    if (strstr(n->node_name,  "evemon")) return 1;
    if (strstr(n->app_name,   "evemon")) return 1;
    if (strstr(n->node_desc,  "evemon")) return 1;
    if (strstr(n->media_name, "evemon")) return 1;
    return 0;
}

const evemon_pw_node_t *find_node(const evemon_proc_data_t *data,
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
char *pw_build_markup(const char *left_raw,
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
 * Right-click context menu on a tree-view row.
 * Offers "Open Milkdrop in Window" for audio output/input rows.
 */
typedef struct {
    pw_ctx_t *ctx;
    guint     node_id;
    pid_t     node_pid;   /* PID of the owning process for this node */
} pw_popup_data_t;

static void on_pw_open_milkdrop(GtkMenuItem *item, gpointer data)
{
    (void)item;
    pw_popup_data_t *d = data;
    pw_ctx_t *ctx = d->ctx;
    /* Use the owning process PID when available (e.g. when init/PID 1 is
     * selected and all streams are shown).  Falls back to last_pid for
     * ordinary single-process selections. */
    pid_t open_pid = (d->node_pid > 0) ? d->node_pid : ctx->last_pid;
    g_free(d);

    if (!ctx->host || !ctx->host->open_plugin_window) return;
    ctx->host->open_plugin_window(ctx->host->host_ctx,
                                  "org.evemon.milkdrop",
                                  open_pid,
                                  ctx->last_proc_name[0]
                                      ? ctx->last_proc_name : "audio");
}

gboolean on_pw_button_press(GtkWidget *widget, GdkEventButton *ev,
                                    gpointer data)
{
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;

    pw_ctx_t *ctx = data;
    GtkTreeView  *view = GTK_TREE_VIEW(widget);
    GtkTreeModel *model = gtk_tree_view_get_model(view);

    /* Find the row under the cursor */
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(view,
            (gint)ev->x, (gint)ev->y,
            &path, NULL, NULL, NULL))
        return FALSE;

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        gtk_tree_path_free(path);
        return FALSE;
    }
    gtk_tree_path_free(path);

    guint node_id = 0;
    gtk_tree_model_get(model, &iter, COL_NODE_ID, &node_id, -1);
    if (node_id == 0) return FALSE; /* header row, not a stream */

    /* Select the row so it's obvious what we right-clicked */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    gtk_tree_selection_select_iter(sel, &iter);

    /* Build popup */
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *mi_md = gtk_menu_item_new_with_label("Open Milkdrop in Window");
    pw_popup_data_t *pd = g_new0(pw_popup_data_t, 1);
    pd->ctx     = ctx;
    pd->node_id = node_id;
    /* Look up the owning process PID for this node from our tracked list */
    pd->node_pid = 0;
    for (size_t _i = 0; _i < ctx->audio_node_count; _i++) {
        if (ctx->audio_node_ids[_i] == node_id) {
            pd->node_pid = ctx->audio_node_pids[_i];
            break;
        }
    }
    g_signal_connect(mi_md, "activate",
                     G_CALLBACK(on_pw_open_milkdrop), pd);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_md);
    gtk_widget_set_sensitive(mi_md, !query_is_foreign_host());

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/*
 * Double-click a row to switch spectrogram target.
 */
void on_row_activated(GtkTreeView *view, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    pw_ctx_t *ctx = data;
    if (query_is_foreign_host()) return;

    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;

    guint node_id = 0;
    gtk_tree_model_get(model, &iter, COL_NODE_ID, &node_id, -1);
    if (node_id == 0) return;

    ctx->spectro_shown = TRUE;
    ctx->spectro_target_node = (uint32_t)node_id;
    ctx->spectro_miss_count = 0;

    if (ctx->host && ctx->host->spectro_start) {
        ctx->host->spectro_start(ctx->host->host_ctx,
                                 GTK_DRAWING_AREA(ctx->spectro_draw),
                                 (uint32_t)node_id);
        /* Re-apply the currently selected theme so switching sources
         * doesn't silently reset it to the default (Classic). */
        if (ctx->host->set_charting_theme && ctx->spectro_theme_combo) {
            int active = gtk_combo_box_get_active(
                GTK_COMBO_BOX(ctx->spectro_theme_combo));
            if (active >= 0)
                ctx->host->set_charting_theme(ctx->host->host_ctx,
                    (unsigned)active);
        } else if (ctx->host->spectro_set_theme && ctx->spectro_theme_combo) {
            int active = gtk_combo_box_get_active(
                GTK_COMBO_BOX(ctx->spectro_theme_combo));
            if (active >= 0)
                ctx->host->spectro_set_theme(ctx->host->host_ctx,
                    GTK_DRAWING_AREA(ctx->spectro_draw),
                    (unsigned)active);
        }
        gtk_widget_set_no_show_all(ctx->spectro_section, FALSE);
        gtk_widget_show_all(ctx->spectro_section);
        gtk_widget_set_no_show_all(ctx->spectro_section, TRUE);
    }
}

/* ── spectrogram theme selector callback ─────────────────────── */

void on_spectro_theme_changed(GtkComboBox *combo, gpointer data);

/* Called by the host when the charting theme changes externally (e.g. menu bar) */
void on_charting_theme_notify(void *plugin_ctx, unsigned theme_index)
{
    pw_ctx_t *ctx = plugin_ctx;
    if (!ctx->spectro_theme_combo) return;
    int current = gtk_combo_box_get_active(GTK_COMBO_BOX(ctx->spectro_theme_combo));
    if (current == (int)theme_index) return; /* already in sync */
    g_signal_handlers_block_by_func(
        ctx->spectro_theme_combo,
        G_CALLBACK(on_spectro_theme_changed), ctx);
    gtk_combo_box_set_active(
        GTK_COMBO_BOX(ctx->spectro_theme_combo), (gint)theme_index);
    g_signal_handlers_unblock_by_func(
        ctx->spectro_theme_combo,
        G_CALLBACK(on_spectro_theme_changed), ctx);
}

void on_spectro_theme_changed(GtkComboBox *combo, gpointer data)
{
    pw_ctx_t *ctx = data;
    if (!ctx->host) return;
    int active = gtk_combo_box_get_active(combo);
    if (active < 0) return;
    if (ctx->host->set_charting_theme) {
        ctx->host->set_charting_theme(ctx->host->host_ctx, (unsigned)active);
    } else if (ctx->host->spectro_set_theme) {
        ctx->host->spectro_set_theme(ctx->host->host_ctx,
                                     GTK_DRAWING_AREA(ctx->spectro_draw),
                                     (unsigned)active);
        evemon_settings_t *s = settings_get();
        if (s) { s->spectro_theme = active; settings_save(); }
    }
}

const char *spectro_theme_names[] = {
    "Classic", "Heat", "Cool", "Greyscale", "Neon", "Vaporwave"
};

typedef struct {
    pw_ctx_t *ctx;
    int       theme_index;
} spectro_theme_popup_data_t;

static void on_spectro_theme_popup_selected(GtkCheckMenuItem *item, gpointer data)
{
    if (!gtk_check_menu_item_get_active(item))
        return;
    spectro_theme_popup_data_t *d = data;
    pw_ctx_t *ctx = d->ctx;

    /* Apply the theme directly — mirrors on_spectro_theme_changed */
    if (ctx->host) {
        if (ctx->host->set_charting_theme)
            ctx->host->set_charting_theme(ctx->host->host_ctx,
                                          (unsigned)d->theme_index);
        else if (ctx->host->spectro_set_theme)
            ctx->host->spectro_set_theme(ctx->host->host_ctx,
                                         GTK_DRAWING_AREA(ctx->spectro_draw),
                                         (unsigned)d->theme_index);
    }

    /* Sync the combo box without re-firing on_spectro_theme_changed */
    if (ctx->spectro_theme_combo) {
        g_signal_handlers_block_by_func(
            ctx->spectro_theme_combo,
            G_CALLBACK(on_spectro_theme_changed), ctx);
        gtk_combo_box_set_active(
            GTK_COMBO_BOX(ctx->spectro_theme_combo), d->theme_index);
        g_signal_handlers_unblock_by_func(
            ctx->spectro_theme_combo,
            G_CALLBACK(on_spectro_theme_changed), ctx);
    }
}

gboolean on_spectro_button_press(GtkWidget *widget,
                                        GdkEventButton *ev,
                                        gpointer data)
{
    (void)widget;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;

    pw_ctx_t *ctx = data;
    int current = ctx->spectro_theme_combo
                  ? gtk_combo_box_get_active(
                        GTK_COMBO_BOX(ctx->spectro_theme_combo))
                  : 0;
    if (current < 0) current = 0;

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *header_mi = gtk_menu_item_new_with_label("Charting Theme");
    gtk_widget_set_sensitive(header_mi, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), header_mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    static spectro_theme_popup_data_t popup_data[N_SPECTRO_THEMES];
    GSList *group = NULL;
    for (int i = 0; i < N_SPECTRO_THEMES; i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_label(
            group, spectro_theme_names[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        if (i == current)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        popup_data[i] = (spectro_theme_popup_data_t){ ctx, i };
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_spectro_theme_popup_selected),
                         &popup_data[i]);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* ── spectrogram placeholder draw ────────────────────────────── */

gboolean on_spectro_draw(GtkWidget *widget, cairo_t *cr,
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

gboolean meter_tick(gpointer data)
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

    /* ── BPM detection: feed combined peak energy ─────────── */
    if (ctx->audio_node_count > 0 && ctx->host && ctx->host->pw_meter_read) {
        float max_e = 0.0f;
        for (size_t i = 0; i < ctx->audio_node_count; i++) {
            int ll = 0, lr = 0;
            ctx->host->pw_meter_read(ctx->host->host_ctx,
                                     ctx->audio_node_ids[i], &ll, &lr);
            float e = (ll + lr) / 2000.0f;  /* 0..1 combined */
            if (e > max_e) max_e = e;
        }
        bpm_feed(&ctx->bpm, max_e);

        /* Update trailing BPM label (10s window) */
        if (ctx->bpm_label && GTK_IS_LABEL(ctx->bpm_label)) {
            if (ctx->bpm.bpm >= BPM_MIN &&
                ctx->bpm.confidence >= BPM_CONFIDENCE_MIN) {
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "<span foreground=\"#cc88ff\">~<b>%.0f</b></span>",
                    ctx->bpm.bpm);
                gtk_label_set_markup(GTK_LABEL(ctx->bpm_label), buf);
            } else {
                gtk_label_set_markup(GTK_LABEL(ctx->bpm_label),
                    "<span foreground=\"#666666\">~\u2014</span>");
            }
        }

        /* Update track-wide BPM label (histogram accumulator) */
        if (ctx->bpm_track_label && GTK_IS_LABEL(ctx->bpm_track_label)) {
            if (ctx->bpm.track_bpm >= BPM_MIN && ctx->bpm.vote_count >= 3) {
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "<span foreground=\"#88ddff\"><b>%.0f</b> BPM</span>",
                    ctx->bpm.track_bpm);
                gtk_label_set_markup(GTK_LABEL(ctx->bpm_track_label), buf);
            } else {
                gtk_label_set_markup(GTK_LABEL(ctx->bpm_track_label),
                    "<span foreground=\"#666666\">\u2014 BPM</span>");
            }
        }
    }

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
 * Media Meta service plugin (org.evemon.audio_service).
 */
void pw_on_album_art_event(const evemon_event_t *event,
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

    /* ── Track change / seek detection for BPM reset ────────── */
    {
        /* Build a composite key from title+artist to detect track changes.
         * Using both fields handles cases like "Intro" appearing on
         * multiple albums by different artists. */
        char new_key[768];
        snprintf(new_key, sizeof(new_key), "%s\x01%s",
                 art->track_title, art->track_artist);

        if (new_key[0] && strcmp(new_key, ctx->bpm_track_key) != 0) {
            /* Track changed — reset both trailing and track-wide BPM */
            bpm_reset(&ctx->bpm);
            snprintf(ctx->bpm_track_key, sizeof(ctx->bpm_track_key),
                     "%s", new_key);
            ctx->bpm_last_pos_us = art->position_us;
            /* Show detecting state immediately */
            if (ctx->bpm_label && GTK_IS_LABEL(ctx->bpm_label))
                gtk_label_set_markup(GTK_LABEL(ctx->bpm_label),
                    "<span foreground=\"#666666\">~\u2014</span>");
            if (ctx->bpm_track_label && GTK_IS_LABEL(ctx->bpm_track_label))
                gtk_label_set_markup(GTK_LABEL(ctx->bpm_track_label),
                    "<span foreground=\"#666666\">\u2014 BPM</span>");
        } else if (art->position_us >= 0 && ctx->bpm_last_pos_us >= 0) {
            /* Seek detection: if position jumps backward by more than
             * 3 seconds, the user seeked — reset the detector since
             * the beat phase context has changed. */
            int64_t delta = art->position_us - ctx->bpm_last_pos_us;
            if (delta < -3000000) {  /* jumped back >3s */
                bpm_reset(&ctx->bpm);
                if (ctx->bpm_label && GTK_IS_LABEL(ctx->bpm_label))
                    gtk_label_set_markup(GTK_LABEL(ctx->bpm_label),
                        "<span foreground=\"#666666\">~\u2014</span>");
                if (ctx->bpm_track_label && GTK_IS_LABEL(ctx->bpm_track_label))
                    gtk_label_set_markup(GTK_LABEL(ctx->bpm_track_label),
                        "<span foreground=\"#666666\">\u2014 BPM</span>");
            }
        }
        ctx->bpm_last_pos_us = art->position_us;
    }

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
void pw_update_mpris(pw_ctx_t *ctx, const evemon_proc_data_t *data)
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

