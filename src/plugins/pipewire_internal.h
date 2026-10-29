#ifndef EVEMON_PIPEWIRE_INTERNAL_H
#define EVEMON_PIPEWIRE_INTERNAL_H
#include "evemon_plugin.h"
#include "../core/settings.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define BPM_HIST_LEN      200    /* ~10s at 50ms sample rate       */
#define BPM_SAMPLE_MS      50    /* matches meter_tick interval     */
#define BPM_MIN             60.0f
#define BPM_MAX            200.0f
#define BPM_SMOOTH          0.15f /* EMA smoothing (lower = slower)  */
#define BPM_ONSET_THRESH    0.02f /* minimum energy jump for onset   */
#define BPM_CONFIDENCE_MIN  0.10f /* min autocorr peak for valid BPM */
#define BPM_HISTO_BINS     140    /* one bin per BPM: 60..199        */
#define BPM_HISTO_BASE      60    /* lowest bin = 60 BPM             */
#define BPM_HISTO_VOTE_CAP  8000
#define N_SPECTRO_THEMES 6
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

typedef struct {
    /* ── 10-second trailing detector ─────────────────────────── */
    float  energy[BPM_HIST_LEN];   /* raw combined energy (0..1)     */
    float  onset[BPM_HIST_LEN];    /* onset envelope (half-wave rect)*/
    int    head;                    /* circular write index           */
    int    filled;                  /* samples collected so far       */
    float  bpm;                     /* current smoothed trailing BPM  */
    float  confidence;              /* autocorrelation peak strength  */
    int    update_counter;          /* counts samples to throttle     */

    /* ── Track-wide accumulator ──────────────────────────────── */
    float  histo[BPM_HISTO_BINS];  /* confidence-weighted vote bins   */
    int    vote_count;              /* total votes cast this track     */
    float  track_bpm;              /* dominant BPM across whole track  */
} bpm_state_t;
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
    GtkWidget      *spectro_theme_combo;  /* theme selector dropdown */
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

    /* BPM detection */
    bpm_state_t     bpm;
    GtkWidget      *bpm_label;          /* 10s trailing BPM              */
    GtkWidget      *bpm_track_label;    /* full-track accumulated BPM    */
    char            bpm_track_key[768]; /* "title\0artist" for change detect */
    int64_t         bpm_last_pos_us;    /* last position for seek detection  */

    /* Cached process name for use in popup menus / window titles */
    char            last_proc_name[256];

    /* Event bus subscription ID (for cleanup in destroy) */
    int             event_sub_id;

    /* Spectrogram node tracking */
    uint32_t        spectro_target_node;  /* user-selected PW node id (0 = none) */
    int             spectro_miss_count;   /* consecutive updates target was absent */

    /* Per-node PID, parallel to audio_node_ids */
    pid_t           audio_node_pids[64];
} pw_ctx_t;
char *utf8_sanitize(const char *raw);
void bpm_reset(bpm_state_t *bs);
GtkCellRenderer *pw_meter_renderer_new(void);
GtkCellRenderer *pw_meter_renderer_new(void);
int classify_node(const char *media_class);
const char *node_label(const evemon_pw_node_t *n);
int is_evemon_node(const evemon_pw_node_t *n);
const evemon_pw_node_t *find_node(const evemon_proc_data_t *data,
                                          uint32_t id);
char *pw_build_markup(const char *left_raw,
                             const char *right_raw,
                             const char *arrow_utf8);
gboolean on_pw_button_press(GtkWidget *widget, GdkEventButton *ev,
                                    gpointer data);
void on_row_activated(GtkTreeView *view, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer data);
void on_charting_theme_notify(void *plugin_ctx, unsigned theme_index);
void on_spectro_theme_changed(GtkComboBox *combo, gpointer data);
gboolean on_spectro_button_press(GtkWidget *widget,
                                        GdkEventButton *ev,
                                        gpointer data);
gboolean on_spectro_draw(GtkWidget *widget, cairo_t *cr,
                                gpointer data);
gboolean meter_tick(gpointer data);
void pw_on_album_art_event(const evemon_event_t *event,
                                  void *user_data);
void pw_update_mpris(pw_ctx_t *ctx, const evemon_proc_data_t *data);
void pw_destroy(void *opaque);
extern const char *spectro_theme_names[N_SPECTRO_THEMES];

#endif
