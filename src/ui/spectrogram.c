/*
 * spectrogram.c – real-time audio spectrogram for a selected process.
 *
 * Captures audio from a PipeWire node (identified by PID → node lookup)
 * using pw_stream, computes an FFT, and renders a scrolling waterfall
 * spectrogram into a GtkDrawingArea via Cairo.
 *
 * Gated behind HAVE_PIPEWIRE (same flag as pipewire_scan.c).
 */

#ifdef HAVE_PIPEWIRE

#include "ui_internal.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/utils/defs.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

/* ── FFT parameters ──────────────────────────────────────────── */

#define FFT_SIZE        1024    /* must be power of 2               */
#define FFT_HALF        (FFT_SIZE / 2)
#define SAMPLE_RATE     48000
#define WATERFALL_COLS  400     /* horizontal pixels = time slices  */
#define WATERFALL_ROWS  FFT_HALF /* vertical pixels = freq bins     */

/* ── frequency cutoff detection ──────────────────────────────── */

/*
 * Known lossy codec frequency cutoffs (from spectral analysis guides).
 * Sorted highest-first so the first match wins.
 */
typedef struct {
    int    cutoff_hz;
    const char *label;
} codec_cutoff_t;

static const codec_cutoff_t codec_table[] = {
    { 18500, "Lossless"    },  /* 44.1→48k PW resample: ~2-3k rolloff */
    { 17500, "~MP3 320"    },  /* MP3 320 CBR: ~20.5k → ~17.5k post-PW */
    { 17000, "~MP3 256"    },  /* MP3 256 CBR                     */
    { 16500, "~MP3 V0"     },  /* LAME V0                         */
    { 16000, "~MP3 192"    },  /* MP3 192 CBR                     */
    { 15000, "~MP3 V2"     },  /* LAME V2                         */
    { 14000, "~Lossy 160"  },  /* Various ~160 kbps               */
    { 12500, "~MP3 128"    },  /* MP3 128 CBR                     */
    { 10000, "~AAC 96"     },  /* Low bitrate AAC                 */
    {  8000, "~Low bitrate"},  /* Very low quality                */
    {     0, NULL          },
};

/* ── radix-2 Cooley–Tukey FFT (in-place, decimation-in-time) ── */

static void fft_dit(float *re, float *im, int n)
{
    /* bit-reversal permutation */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    /* butterfly stages */
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wre = cosf(ang), wim = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cur_re = 1.0f, cur_im = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float tre = re[i + j + len / 2] * cur_re
                          - im[i + j + len / 2] * cur_im;
                float tim = re[i + j + len / 2] * cur_im
                          + im[i + j + len / 2] * cur_re;
                re[i + j + len / 2] = re[i + j] - tre;
                im[i + j + len / 2] = im[i + j] - tim;
                re[i + j] += tre;
                im[i + j] += tim;
                float nre = cur_re * wre - cur_im * wim;
                cur_im    = cur_re * wim + cur_im * wre;
                cur_re    = nre;
            }
        }
    }
}

/* Hann window */
static float hann_window[FFT_SIZE];
static int   hann_ready = 0;

static void ensure_hann(void)
{
    if (hann_ready) return;
    for (int i = 0; i < FFT_SIZE; i++)
        hann_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
    hann_ready = 1;
}

/* ── lock-free SPSC ring buffer for PCM samples ──────────────── */

#define RING_SIZE  (FFT_SIZE * 48)  /* ~1 s at 48 kHz — absorbs network burst gaps */

/*
 * Expected wall-clock time between waterfall columns.
 * Each column represents one FFT hop: FFT_SIZE/2 samples.
 * At 48000 Hz: 512 / 48000 ≈ 10.67 ms = 10667 µs.
 * Used to synthesise silence columns when the ring is empty.
 */
#define SPECTRO_COL_PERIOD_US  10667

typedef struct {
    float           buf[RING_SIZE];
    atomic_uint     write_pos;      /* written by PW thread (producer) */
    atomic_uint     read_pos;       /* written by GTK timer (consumer) */
} ring_buf_t;

/* ── spectrogram state ───────────────────────────────────────── */

typedef struct {
    /* PipeWire connection (owned by the PW thread) */
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct pw_stream      *stream;
    struct spa_hook        stream_listener;

    /* lock-free ring buffer: PW process callback → GTK timer */
    ring_buf_t             ring;

    /* Waterfall image: each column is one time slice, scrolls left */
    float                  waterfall[WATERFALL_COLS][WATERFALL_ROWS];
    int                    wf_write_col;    /* next column to write     */

    /* Pre-rendered Cairo image surface for fast blitting */
    cairo_surface_t       *wf_surface;
    int                    wf_surface_w;
    int                    wf_surface_h;

    /* FFT scratch */
    float                  fft_re[FFT_SIZE];
    float                  fft_im[FFT_SIZE];

    /* Target node */
    uint32_t               target_node_id;

    /* GTK redraw timer */
    guint                  timer_id;

    /* UI widgets (not owned) */
    GtkDrawingArea        *draw_area;
    ui_ctx_t              *ui_ctx;

    /* Running flag */
    int                    running;

    /* Colour theme (index into spectro_theme_t) */
    spectro_theme_t        theme;

    /* Signal handler id for the "draw" callback (so we can disconnect) */
    gulong                 draw_handler_id;

    /* Wall-clock timestamp (g_get_monotonic_time µs) of the last waterfall
     * column write — real or synthesised.  Used to keep the waterfall
     * scrolling at a constant rate when network audio arrives as bursts. */
    gint64                 last_col_us;

    /* ── frequency cutoff detection ──────────────────────────── */
    float                  cutoff_freq;     /* detected cutoff in Hz    */
    int                    cutoff_bin;      /* FFT bin index of cutoff  */
    char                   cutoff_label[64];/* codec label string       */
    float                  cutoff_ema;      /* EMA-smoothed cutoff bin  */
    int                    detect_counter;  /* frames since last detect */
    /* Producer idle tracking — avoids false silence on PW quantum gaps */
    unsigned               last_wp;         /* wp seen in previous tick  */
    int                    producer_idle;   /* consecutive idle tick count*/
} spectro_state_t;

/* ── frequency cutoff detection ───────────────────────────────
 *
 * Scan the most recent waterfall columns from the top (Nyquist)
 * downward to find the highest bin where average energy is above a
 * noise-floor threshold.  That is the "cliff" – the point where
 * the lossy codec's low-pass filter kicks in.
 *
 * We use an exponential moving average to smooth the result and
 * avoid jitter.
 * ──────────────────────────────────────────────────────────── */

#define CUTOFF_SCAN_COLS   60      /* look at the last N time slices   */
#define CUTOFF_NOISE_FLOOR 0.06f   /* normalised dB threshold (~-75 dB)*/
#define CUTOFF_EMA_ALPHA   0.08f   /* smoothing factor (lower = slower)*/
#define CUTOFF_DETECT_INTERVAL 8   /* run detection every N FFT frames */

static void detect_cutoff(spectro_state_t *st)
{
    /*
     * Average each frequency bin across the most recent columns.
     */
    int start_col = st->wf_write_col - CUTOFF_SCAN_COLS;
    if (start_col < 0) start_col += WATERFALL_COLS;

    /* Scan from top bin down; find the highest with any significant energy.
     * The "cliff" is a hard cutoff — if *any* column has content at a bin,
     * the codec hasn't filtered it, so we use max, not mean. */
    int top_bin = 0;
    for (int bin = FFT_HALF - 1; bin >= 0; bin--) {
        float peak = 0.0f;
        int   col = start_col;
        for (int c = 0; c < CUTOFF_SCAN_COLS; c++) {
            if (st->waterfall[col][bin] > peak)
                peak = st->waterfall[col][bin];
            col = (col + 1) % WATERFALL_COLS;
        }
        if (peak > CUTOFF_NOISE_FLOOR) {
            top_bin = bin;
            break;
        }
    }

    /* Apply EMA smoothing to the detected bin. */
    if (st->cutoff_ema < 1.0f)
        st->cutoff_ema = (float)top_bin;        /* first time: seed      */
    else
        st->cutoff_ema += CUTOFF_EMA_ALPHA * ((float)top_bin - st->cutoff_ema);

    int smoothed_bin = (int)(st->cutoff_ema + 0.5f);
    float freq = (float)smoothed_bin * (float)SAMPLE_RATE / (float)FFT_SIZE;

    st->cutoff_bin  = smoothed_bin;
    st->cutoff_freq = freq;

    /* Map to a codec label. */
    const char *lbl = "Unknown";
    for (int i = 0; codec_table[i].label; i++) {
        if (freq >= (float)codec_table[i].cutoff_hz) {
            lbl = codec_table[i].label;
            break;
        }
    }
    snprintf(st->cutoff_label, sizeof(st->cutoff_label),
             "%s  (%.1f kHz)", lbl, freq / 1000.0f);
}

/* ── PipeWire stream process callback ────────────────────────── */

static void on_process(void *data)
{
    spectro_state_t *st = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(st->stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (!buf || buf->n_datas < 1 || !buf->datas[0].data)
        goto done;
    if (!buf->datas[0].chunk || buf->datas[0].chunk->size == 0)
        goto done;

    {
        float *samples = buf->datas[0].data;
        uint32_t n_bytes = buf->datas[0].chunk->size;
        uint32_t n_samples = n_bytes / sizeof(float);

        /*
         * Skip completely silent buffers — don't waste ring space or
         * trigger FFT work for tracks that are paused / in silence
         * sections.  A peak scan is cheap relative to the FFT.
         * Threshold is well below the noise floor of any real signal.
         */
        float peak = 0.0f;
        for (uint32_t i = 0; i < n_samples; i++) {
            float a = samples[i] < 0.0f ? -samples[i] : samples[i];
            if (a > peak) peak = a;
        }
        if (peak < 1e-7f)
            goto done;

        /*
         * Lock-free SPSC write: only the producer updates write_pos.
         * We read read_pos to check for overflow, but never modify it.
         */
        unsigned wp = atomic_load_explicit(&st->ring.write_pos, memory_order_relaxed);
        unsigned rp = atomic_load_explicit(&st->ring.read_pos, memory_order_acquire);

        for (uint32_t i = 0; i < n_samples; i++) {
            /* Drop oldest samples if ring is full (consumer too slow) */
            if (wp - rp >= RING_SIZE)
                rp = wp - RING_SIZE + 1;
            st->ring.buf[wp % RING_SIZE] = samples[i];
            wp++;
        }
        atomic_store_explicit(&st->ring.write_pos, wp, memory_order_release);
    }

done:
    pw_stream_queue_buffer(st->stream, b);
}

/* ── stream state callback ───────────────────────────────────── */

static void on_stream_state_changed(void *data,
                                    enum pw_stream_state old,
                                    enum pw_stream_state state,
                                    const char *error)
{
    (void)data; (void)old; (void)state; (void)error;
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process       = on_process,
    .state_changed = on_stream_state_changed,
};

/* ── colour themes ───────────────────────────────────────────── */
/* spectro_theme_t enum is declared in ui_internal.h */

/* ── colour mapping (magnitude → packed ARGB pixel) ──────────── */

static uint32_t mag_to_argb_themed(float mag, spectro_theme_t theme)
{
    if (mag < 0.0f) mag = 0.0f;
    if (mag > 1.0f) mag = 1.0f;

    float r = 0.0f, g = 0.0f, b = 0.0f;

    switch (theme) {

    default:
    case SPECTRO_THEME_CLASSIC:
        if (mag < 0.25f) {
            float t = mag / 0.25f;
            r = 0.0f; g = 0.0f; b = 0.2f + 0.6f * t;
        } else if (mag < 0.50f) {
            float t = (mag - 0.25f) / 0.25f;
            r = 0.0f; g = t; b = 0.8f * (1.0f - t) + 0.2f * t;
        } else if (mag < 0.75f) {
            float t = (mag - 0.50f) / 0.25f;
            r = t; g = 1.0f; b = 0.2f * (1.0f - t);
        } else {
            float t = (mag - 0.75f) / 0.25f;
            r = 1.0f; g = 1.0f; b = t;
        }
        break;

    case SPECTRO_THEME_HEAT:
        /* black → dark red → red → orange → yellow → white */
        if (mag < 0.25f) {
            float t = mag / 0.25f;
            r = 0.5f * t; g = 0.0f; b = 0.0f;
        } else if (mag < 0.50f) {
            float t = (mag - 0.25f) / 0.25f;
            r = 0.5f + 0.5f * t; g = 0.0f; b = 0.0f;
        } else if (mag < 0.75f) {
            float t = (mag - 0.50f) / 0.25f;
            r = 1.0f; g = 0.5f * t; b = 0.0f;
        } else {
            float t = (mag - 0.75f) / 0.25f;
            r = 1.0f; g = 0.5f + 0.5f * t; b = t;
        }
        break;

    case SPECTRO_THEME_COOL:
        /* black → deep indigo → violet → cyan → white */
        if (mag < 0.33f) {
            float t = mag / 0.33f;
            r = 0.15f * t; g = 0.0f; b = 0.5f * t;
        } else if (mag < 0.66f) {
            float t = (mag - 0.33f) / 0.33f;
            r = 0.15f + 0.15f * t; g = 0.5f * t; b = 0.5f + 0.5f * t;
        } else {
            float t = (mag - 0.66f) / 0.34f;
            r = 0.30f + 0.70f * t; g = 0.5f + 0.5f * t; b = 1.0f;
        }
        break;

    case SPECTRO_THEME_GREYSCALE:
        r = g = b = mag;
        break;

    case SPECTRO_THEME_NEON:
        /* black → magenta → hot pink → cyan → white */
        if (mag < 0.30f) {
            float t = mag / 0.30f;
            r = 0.6f * t; g = 0.0f; b = 0.6f * t;
        } else if (mag < 0.55f) {
            float t = (mag - 0.30f) / 0.25f;
            r = 0.6f + 0.4f * t; g = 0.0f; b = 0.6f - 0.3f * t;
        } else if (mag < 0.80f) {
            float t = (mag - 0.55f) / 0.25f;
            r = 1.0f - 0.8f * t; g = 0.7f * t; b = 0.3f + 0.7f * t;
        } else {
            float t = (mag - 0.80f) / 0.20f;
            r = 0.2f + 0.8f * t; g = 0.7f + 0.3f * t; b = 1.0f;
        }
        break;

    case SPECTRO_THEME_VIRIDIS: {
        /* Vaporwave: dark navy → deep purple → hot pink → cyan → white */
        if (mag < 0.25f) {
            float t = mag / 0.25f;
            r = 0.04f + 0.10f * t; g = 0.02f + 0.03f * t; b = 0.18f + 0.32f * t;
        } else if (mag < 0.50f) {
            float t = (mag - 0.25f) / 0.25f;
            r = 0.14f + 0.56f * t; g = 0.05f + 0.00f * t; b = 0.50f + 0.10f * t;
        } else if (mag < 0.75f) {
            float t = (mag - 0.50f) / 0.25f;
            r = 0.70f + 0.10f * t; g = 0.05f + 0.65f * t; b = 0.60f + 0.30f * t;
        } else {
            float t = (mag - 0.75f) / 0.25f;
            r = 0.80f + 0.20f * t; g = 0.70f + 0.30f * t; b = 0.90f + 0.10f * t;
        }
        break;
    }
    }

    uint32_t ri = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t gi = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t bi = (uint32_t)(b * 255.0f + 0.5f);
    if (ri > 255) ri = 255;
    if (gi > 255) gi = 255;
    if (bi > 255) bi = 255;
    return (0xFFu << 24) | (ri << 16) | (gi << 8) | bi;
}

/* ── rebuild the waterfall image surface from the float data ─── */

static void rebuild_wf_surface(spectro_state_t *st, int w, int h)
{
    if (!st->wf_surface || st->wf_surface_w != w || st->wf_surface_h != h) {
        if (st->wf_surface)
            cairo_surface_destroy(st->wf_surface);
        st->wf_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        st->wf_surface_w = w;
        st->wf_surface_h = h;
    }

    cairo_surface_flush(st->wf_surface);
    uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(st->wf_surface);
    int stride = cairo_image_surface_get_stride(st->wf_surface) / 4; /* in uint32s */

    for (int c = 0; c < WATERFALL_COLS; c++) {
        int src_col = (st->wf_write_col + c) % WATERFALL_COLS;
        /* Map pixel columns for this waterfall column */
        int px_start = c * w / WATERFALL_COLS;
        int px_end   = (c + 1) * w / WATERFALL_COLS;

        for (int r = 0; r < WATERFALL_ROWS; r++) {
            uint32_t argb = mag_to_argb_themed(st->waterfall[src_col][r], st->theme);
            /* Low frequencies at bottom */
            int py_start = h - (r + 1) * h / WATERFALL_ROWS;
            int py_end   = h - r * h / WATERFALL_ROWS;

            for (int py = py_start; py < py_end; py++) {
                uint32_t *row = pixels + py * stride;
                for (int px = px_start; px < px_end; px++)
                    row[px] = argb;
            }
        }
    }

    cairo_surface_mark_dirty(st->wf_surface);
}

/* ── GTK drawing ─────────────────────────────────────────────── */

static gboolean on_spectro_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    spectro_state_t *st = data;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    if (w <= 0 || h <= 0) return FALSE;

    /* Rebuild the surface if needed, then blit in one call */
    rebuild_wf_surface(st, w, h);
    cairo_set_source_surface(cr, st->wf_surface, 0, 0);
    cairo_paint(cr);

    /* Overlay: frequency labels on the left edge */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.6);
    cairo_select_font_face(cr, "Monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.0);

    int freq_labels[] = { 100, 500, 1000, 2000, 5000, 10000, 20000 };
    int n_labels = sizeof(freq_labels) / sizeof(freq_labels[0]);

    for (int i = 0; i < n_labels; i++) {
        int freq = freq_labels[i];
        if (freq > SAMPLE_RATE / 2) break;
        double bin = (double)freq * FFT_SIZE / SAMPLE_RATE;
        double y = h - (bin / FFT_HALF) * h;
        if (y < 10 || y > h - 5) continue;

        char label[32];
        if (freq >= 1000)
            snprintf(label, sizeof(label), "%dk", freq / 1000);
        else
            snprintf(label, sizeof(label), "%d", freq);

        cairo_move_to(cr, 3, y - 2);
        cairo_show_text(cr, label);
    }

    /* ── Cutoff frequency line and codec label ──────────────── */
    if (st->cutoff_freq > 0.0f && st->cutoff_label[0]) {
        double bin = (double)st->cutoff_freq * FFT_SIZE / SAMPLE_RATE;
        double y_cut = h - (bin / FFT_HALF) * h;

        if (y_cut > 2 && y_cut < h - 2) {
            /* Dashed horizontal line */
            double dashes[] = { 4.0, 3.0 };
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_set_source_rgba(cr, 1.0, 0.85, 0.0, 0.8);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 0, y_cut);
            cairo_line_to(cr, w, y_cut);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
        }

        /* Label in the top-right corner */
        cairo_select_font_face(cr, "Monospace",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, st->cutoff_label, &ext);
        double lx = w - ext.width - 6;
        double ly = 14;

        /* Background pill for readability */
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
        cairo_rectangle(cr, lx - 4, ly - ext.height - 2,
                        ext.width + 8, ext.height + 6);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 1.0, 0.85, 0.0, 0.95);
        cairo_move_to(cr, lx, ly);
        cairo_show_text(cr, st->cutoff_label);
    }

    return FALSE;
}

/* ── GTK timer: consume ring buffer → FFT → update waterfall ── */

#define MAX_FFTS_PER_TICK  4   /* cap work per frame to stay responsive */
/*
 * Minimum consecutive ticks with wp unchanged before synthesising silence.
 * 10 ticks × 16 ms ≈ 160 ms — safely exceeds the largest realistic
 * PipeWire quantum (4096 frames at 48 kHz ≈ 85 ms).
 */
#define SILENCE_IDLE_TICKS 10

static gboolean spectro_tick(gpointer data)
{
    spectro_state_t *st = data;
    if (!st->running) return G_SOURCE_REMOVE;

    ensure_hann();

    unsigned wp = atomic_load_explicit(&st->ring.write_pos, memory_order_acquire);
    unsigned rp = atomic_load_explicit(&st->ring.read_pos, memory_order_relaxed);
    unsigned avail = wp - rp;

    /*
     * Track how many consecutive ticks the producer hasn't advanced wp.
     * Short gaps are normal PipeWire quantum delivery; gaps longer than
     * SILENCE_IDLE_TICKS ticks mean the stream is truly paused/stopped.
     */
    if (wp != st->last_wp) {
        st->last_wp       = wp;
        st->producer_idle = 0;
    } else if (st->producer_idle < INT_MAX) {
        st->producer_idle++;
    }

    /*
     * If there is a partial ring fragment (0 < avail < FFT_SIZE) that
     * the producer hasn't topped up for SILENCE_IDLE_TICKS ticks, the
     * stream has truly stopped.  Discard it so silence synthesis below
     * can proceed (otherwise avail never reaches zero).
     */
    if (avail > 0 && avail < (unsigned)FFT_SIZE
                  && st->producer_idle >= SILENCE_IDLE_TICKS) {
        rp   += avail;
        avail = 0;
        atomic_store_explicit(&st->ring.read_pos, rp, memory_order_release);
    }

    /*
     * If we've fallen far behind, skip ahead so we only process
     * the most recent data (keeps the display in real-time).
     * With ~16ms ticks, ~768 samples arrive per tick (48kHz mono);
     * that's about 1-2 FFT hops, so a modest cap is enough.
     */
    if (avail > (unsigned)(FFT_SIZE * MAX_FFTS_PER_TICK + FFT_SIZE)) {
        rp = wp - FFT_SIZE * MAX_FFTS_PER_TICK;
        avail = wp - rp;
    }

    /* Process up to MAX_FFTS_PER_TICK windows */
    int did_fft = 0;
    int fft_count = 0;
    while (avail >= (unsigned)FFT_SIZE && fft_count < MAX_FFTS_PER_TICK) {
        /* Peek at the peak magnitude of this window before doing the FFT.
         * If the window is silent (e.g. paused playback, empty stream),
         * advance the read pointer but skip the FFT and waterfall write
         * entirely — no point computing or rendering silence. */
        float window_peak = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            float s = st->ring.buf[(rp + i) % RING_SIZE];
            if (s < 0.0f) s = -s;
            if (s > window_peak) window_peak = s;
        }
        rp += FFT_SIZE / 2;    /* 50% overlap */
        avail = wp - rp;

        if (window_peak < 1e-7f) {
            /* Silent window — write a zero-magnitude column so the visual
             * timeline stays uniform (silence gaps in the audio are real
             * events that should appear as silence in the waterfall). */
            memset(st->waterfall[st->wf_write_col], 0,
                   FFT_HALF * sizeof(float));
            st->wf_write_col = (st->wf_write_col + 1) % WATERFALL_COLS;
            did_fft = 1;
            fft_count++;
            continue;
        }

        /* Copy windowed samples (no lock needed — SPSC ring) */
        for (int i = 0; i < FFT_SIZE; i++) {
            st->fft_re[i] = st->ring.buf[(rp - FFT_SIZE / 2 + i) % RING_SIZE] * hann_window[i];
            st->fft_im[i] = 0.0f;
        }

        fft_dit(st->fft_re, st->fft_im, FFT_SIZE);

        /* Compute magnitude in dB, normalised to [0, 1] */
        float *col = st->waterfall[st->wf_write_col % WATERFALL_COLS];
        for (int i = 0; i < FFT_HALF; i++) {
            float mag = sqrtf(st->fft_re[i] * st->fft_re[i] +
                              st->fft_im[i] * st->fft_im[i]);
            float db = 20.0f * log10f(fmaxf(mag / FFT_SIZE, 1e-10f));
            float norm = (db + 80.0f) / 80.0f;
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
            col[i] = norm;
        }
        st->wf_write_col = (st->wf_write_col + 1) % WATERFALL_COLS;
        did_fft = 1;
        fft_count++;
    }
    atomic_store_explicit(&st->ring.read_pos, rp, memory_order_release);

    gint64 now_us = g_get_monotonic_time();

    if (did_fft) {
        /* Real columns written — record wall-clock time so silence
         * synthesis knows when to resume from. */
        st->last_col_us = now_us;

        /* Run cutoff detection periodically to avoid wasting cycles */
        st->detect_counter += fft_count;
        if (st->detect_counter >= CUTOFF_DETECT_INTERVAL) {
            st->detect_counter = 0;
            detect_cutoff(st);
        }
        if (st->draw_area)
            gtk_widget_queue_draw(GTK_WIDGET(st->draw_area));
    } else if (st->last_col_us > 0
               && avail == 0
               && st->producer_idle >= SILENCE_IDLE_TICKS) {
        /*
         * Ring is empty and the producer has been idle long enough that
         * this is a real pause, not a PipeWire inter-quantum gap.
         * Synthesise silence columns so the waterfall keeps scrolling
         * instead of freezing.
         *
         * We advance last_col_us by exactly SPECTRO_COL_PERIOD_US per
         * synthesised column (rather than snapping to now_us) so there
         * is no accumulated drift when bursts resume.
         */
        int64_t elapsed_us = now_us - st->last_col_us;
        int cols_due = (int)(elapsed_us / SPECTRO_COL_PERIOD_US);
        if (cols_due > MAX_FFTS_PER_TICK)
            cols_due = MAX_FFTS_PER_TICK;
        for (int c = 0; c < cols_due; c++) {
            memset(st->waterfall[st->wf_write_col], 0,
                   FFT_HALF * sizeof(float));
            st->wf_write_col = (st->wf_write_col + 1) % WATERFALL_COLS;
            st->last_col_us += SPECTRO_COL_PERIOD_US;
        }
        if (cols_due > 0) {
            st->detect_counter += cols_due;
            if (st->detect_counter >= CUTOFF_DETECT_INTERVAL) {
                st->detect_counter = 0;
                detect_cutoff(st);
            }
            if (st->draw_area)
                gtk_widget_queue_draw(GTK_WIDGET(st->draw_area));
        }
    }

    return G_SOURCE_CONTINUE;
}

/* ── start / stop ────────────────────────────────────────────── */

static void spectro_stop_internal(spectro_state_t *st)
{
    if (!st) return;

    st->running = 0;

    if (st->timer_id) {
        g_source_remove(st->timer_id);
        st->timer_id = 0;
    }

    /* Disconnect our draw handler from the plugin's drawing area
     * BEFORE freeing the state, so no dangling-pointer callbacks. */
    if (st->draw_handler_id && st->draw_area &&
        GTK_IS_WIDGET(st->draw_area)) {
        g_signal_handler_disconnect(st->draw_area, st->draw_handler_id);
        st->draw_handler_id = 0;
    }

    if (st->loop) {
        pw_thread_loop_stop(st->loop);

        if (st->stream) {
            pw_stream_disconnect(st->stream);
            pw_stream_destroy(st->stream);
            st->stream = NULL;
        }
        if (st->core) {
            pw_core_disconnect(st->core);
            st->core = NULL;
        }
        if (st->context) {
            pw_context_destroy(st->context);
            st->context = NULL;
        }
        pw_thread_loop_destroy(st->loop);
        st->loop = NULL;
    }

    if (st->wf_surface) {
        cairo_surface_destroy(st->wf_surface);
        st->wf_surface = NULL;
    }
}

/* ── per-instance lookup helpers ──────────────────────────────── */

/*
 * Find the spectro instance for a given draw area.
 * Returns the index into ctx->spectro_instances[], or -1 if not found.
 */
static int spectro_find(ui_ctx_t *ctx, GtkWidget *draw_area)
{
    for (size_t i = 0; i < ctx->spectro_count; i++) {
        if (ctx->spectro_instances[i].draw_area == draw_area)
            return (int)i;
    }
    return -1;
}

void spectrogram_stop(ui_ctx_t *ctx, GtkDrawingArea *draw_area)
{
    int idx = spectro_find(ctx, GTK_WIDGET(draw_area));
    if (idx < 0) return;

    spectro_state_t *st = ctx->spectro_instances[idx].state;
    spectro_stop_internal(st);
    free(st);

    /* Compact: move last entry into this slot */
    size_t last = ctx->spectro_count - 1;
    if ((size_t)idx < last)
        ctx->spectro_instances[idx] = ctx->spectro_instances[last];
    ctx->spectro_instances[last].draw_area = NULL;
    ctx->spectro_instances[last].state = NULL;
    ctx->spectro_count--;
}

void spectrogram_start_for_node(ui_ctx_t *ctx, GtkDrawingArea *draw_area,
                                uint32_t node_id)
{
    if (!draw_area) return;

    /* If we're already capturing this exact node on this draw area, do nothing */
    int idx = spectro_find(ctx, GTK_WIDGET(draw_area));
    if (idx >= 0) {
        spectro_state_t *old = ctx->spectro_instances[idx].state;
        if (old->target_node_id == node_id && old->running)
            return;
        /* Stop existing capture on this draw area */
        spectrogram_stop(ctx, draw_area);
    }

    if (node_id == 0) return;

    if (ctx->spectro_count >= SPECTRO_MAX_INSTANCES) return;

    spectro_state_t *st = calloc(1, sizeof(*st));
    if (!st) return;

    st->target_node_id = node_id;
    st->draw_area      = draw_area;
    st->ui_ctx         = ctx;
    st->running        = 1;

    /* PIPEWIRE_REMOTE is set once at startup in pw_watcher_start()
     * before any PipeWire threads are spawned (FIND-2). */

    pw_init(NULL, NULL);

    /*
     * Use pw_thread_loop so the PW processing runs on its own
     * real-time thread, separate from both GTK and the GTask pool.
     */
    st->loop = pw_thread_loop_new("evemon-spectro", NULL);
    if (!st->loop) goto fail;

    st->context = pw_context_new(pw_thread_loop_get_loop(st->loop), NULL, 0);
    if (!st->context) goto fail;

    pw_thread_loop_start(st->loop);
    pw_thread_loop_lock(st->loop);

    st->core = pw_context_connect(st->context, NULL, 0);
    if (!st->core) {
        pw_thread_loop_unlock(st->loop);
        goto fail;
    }

    /*
     * Build the audio format pod: F32 mono.
     * Don't force a specific sample rate — let PipeWire negotiate
     * the target node's native rate (e.g. 44100 for Spotify,
     * 48000 for most games).  We use SAMPLE_RATE only for FFT
     * frequency labelling; the actual rate doesn't affect the
     * spectrogram quality since we window in the sample domain.
     */
    uint8_t pod_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    struct spa_audio_info_raw raw_info = SPA_AUDIO_INFO_RAW_INIT(
        .format   = SPA_AUDIO_FORMAT_F32,
        .channels = 1
    );
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw_info);

    /* Create the capture stream.
     *
     * We pass the target node ID directly to pw_stream_connect(),
     * which tells PipeWire to connect us to that specific node.
     * AUTOCONNECT lets PipeWire handle format negotiation and
     * graph routing properly (including sample rate conversion
     * when the target uses e.g. 44100 Hz).
     *
     * node.passive=true ensures we don't affect the target's
     * scheduling — we're a passive observer.
     *
     * stream.capture.sink is NOT set, so we capture directly from
     * the target stream node's output ports, not from the mixed
     * sink monitor.
     */
    st->stream = pw_stream_new(
        st->core, "evemon-spectrogram",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,        "Audio",
            PW_KEY_MEDIA_CATEGORY,    "Capture",
            PW_KEY_MEDIA_ROLE,        "DSP",
            PW_KEY_NODE_PASSIVE,      "true",
            NULL));

    if (!st->stream) {
        pw_thread_loop_unlock(st->loop);
        goto fail;
    }

    pw_stream_add_listener(st->stream, &st->stream_listener,
                           &stream_events, st);

    /*
     * Connect with AUTOCONNECT to the specific target node ID.
     * PipeWire will create the links from the target's output ports
     * to our input ports, handling any necessary format conversion
     * (sample rate, channel count, etc.).
     *
     * By passing the target node's PipeWire object ID as the second
     * argument, pw_stream_connect directs the autoconnect to that
     * specific node rather than the default sink/source.
     */
    int ret = pw_stream_connect(st->stream,
                                PW_DIRECTION_INPUT,
                                st->target_node_id,
                                PW_STREAM_FLAG_AUTOCONNECT |
                                PW_STREAM_FLAG_MAP_BUFFERS |
                                PW_STREAM_FLAG_RT_PROCESS,
                                params, 1);

    if (ret < 0) {
        pw_thread_loop_unlock(st->loop);
        goto fail;
    }

    /*
     * With AUTOCONNECT + target node ID, PipeWire handles the
     * link creation.  No need for registry-based manual linking.
     */

    pw_thread_loop_unlock(st->loop);

    /* Start the GTK redraw timer at ~60 fps for smooth scrolling */
    st->timer_id = g_timeout_add(16, spectro_tick, st);

    /* Register in the per-instance array */
    size_t slot = ctx->spectro_count++;
    ctx->spectro_instances[slot].draw_area = GTK_WIDGET(draw_area);
    ctx->spectro_instances[slot].state = st;

    /* Connect our draw handler to the plugin's drawing area.
     * The plugin may have its own placeholder handler; ours renders
     * the actual waterfall and returns FALSE so both can coexist.
     * Store the handler ID so spectrogram_stop can disconnect it. */
    st->draw_handler_id = g_signal_connect(st->draw_area, "draw",
                                           G_CALLBACK(on_spectro_draw), st);

    return;

fail:
    spectro_stop_internal(st);
    free(st);
}

/* ── draw signal (connected in ui.c) ─────────────────────────── */

uint32_t spectrogram_get_target_node(ui_ctx_t *ctx, GtkDrawingArea *draw_area)
{
    int idx = spectro_find(ctx, GTK_WIDGET(draw_area));
    if (idx < 0) return 0;
    spectro_state_t *st = ctx->spectro_instances[idx].state;
    return st->target_node_id;
}

void spectrogram_set_theme(ui_ctx_t *ctx, GtkDrawingArea *draw_area,
                           spectro_theme_t theme)
{
    int idx = spectro_find(ctx, GTK_WIDGET(draw_area));
    if (idx < 0) return;
    spectro_state_t *st = ctx->spectro_instances[idx].state;
    st->theme = theme;
    /* Force the surface to be rebuilt on the next draw tick */
    if (st->wf_surface) {
        cairo_surface_destroy(st->wf_surface);
        st->wf_surface = NULL;
        st->wf_surface_w = 0;
        st->wf_surface_h = 0;
    }
    if (draw_area && GTK_IS_WIDGET(draw_area))
        gtk_widget_queue_draw(GTK_WIDGET(draw_area));
}

gboolean spectrogram_on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    ui_ctx_t *ctx = data;
    int idx = spectro_find(ctx, widget);
    if (idx < 0) {
        /* No active capture — draw a dark background with hint text */
        (void)gtk_widget_get_allocated_width(widget);
        int h = gtk_widget_get_allocated_height(widget);
        cairo_set_source_rgb(cr, 0.05, 0.05, 0.1);
        cairo_paint(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.3);
        cairo_select_font_face(cr, "Sans",
                               CAIRO_FONT_SLANT_ITALIC,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_move_to(cr, 8, h / 2.0 + 4);
        cairo_show_text(cr, "No audio stream");
        return FALSE;
    }
    return on_spectro_draw(widget, cr, ctx->spectro_instances[idx].state);
}

#endif /* HAVE_PIPEWIRE */
