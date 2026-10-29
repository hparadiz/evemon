#ifndef EVEMON_MILKDROP_INTERNAL_H
#define EVEMON_MILKDROP_INTERNAL_H
/*
 * milkdrop_plugin.c – OpenGL MilkDrop .milk preset visualiser for evemon.
 *
 * Uses GtkGLArea + OpenGL 3.3 core for GPU-accelerated rendering:
 *   - Warp feedback loop rendered as a textured mesh with per-vertex UVs
 *   - Post-processing (gamma, brighten, darken, solarize, invert) via shader
 *   - Video echo via shader
 *   - Waves/shapes/borders drawn with GL primitives
 *
 * CPU-side expression evaluator drives per-frame/per-pixel mesh parameters.
 * Audio from PipeWire via host services API.
 *
 * Build:  gcc -shared -fPIC -o evemon_milkdrop_plugin.so milkdrop_plugin.c \
 *             $(pkg-config --cflags --libs gtk+-3.0 epoxy) -lm
 */

#include "evemon_plugin.h"
#ifdef EVEMON_QT_MILKDROP
#include <glib.h>
#include <pango/pangocairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "../qt/milkdrop_keys.h"
#include "../qt/milkdrop_cairo.inc"
#endif
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <float.h>
#include <epoxy/gl.h>

/* from main.c – resolved at dlopen time (-rdynamic) */
#include "../core/log.h"

/* ── compile-time knobs ──────────────────────────────────────── */

#define MD_FFT_SIZE     1024
#define MD_FFT_HALF     (MD_FFT_SIZE / 2)
#define MD_WAVEFORM_N   576
#define MD_TEX_W        1024
#define MD_TEX_H        768
#define MESH_W          48
#define MESH_H          36
#define NUM_WAVE_MODES  8
#define RING_SIZE       (MD_FFT_SIZE * 8)

#define EXPR_MAX_TOKENS 2048
#define EXPR_MAX_VARS   192
#define MAX_EXPR_LINES  64
/* MilkDrop expression lines in real presets are rarely > 200 chars; 512 is
 * a safe upper bound and halves the per-preset expression-array footprint
 * compared to the previous 1024 (23 arrays × 32 KB saved = ~736 KB per
 * md_preset_t → ~2.1 MB for the three inline preset copies in md_ctx_t). */
#define MAX_LINE_LEN    512

#define MAX_CUSTOM_WAVES  4
#define MAX_CUSTOM_SHAPES 4
#define NUM_Q_VAR  32
#define NUM_T_VAR  8

#ifndef EVEMON_DATADIR
#define EVEMON_DATADIR "/usr/local/share/evemon"
#endif

#define PRESET_DIR      EVEMON_DATADIR "/milkdrop/presets"
#define MD_FAV_BASENAME "milkdrop_favorites.ini"

#define MD_FAV_SLOTS   16

/* max vertices for wave/shape drawing */
#define MAX_WAVE_VERTS  2048

/* ── SPSC ring buffer ────────────────────────────────────────── */


#define VAR_HT_SIZE 256                  /* must be power of two        */
#define VAR_HT_MASK (VAR_HT_SIZE - 1)
#define MD_EVAL_MAX_DEPTH 128
#define MESH_VERT_COUNT  ((MESH_W+1)*(MESH_H+1))
#define MESH_FLOAT_COUNT (MESH_VERT_COUNT * 4)
#define SHAPE_SCRATCH_VERTS  (100 + 2)   /* sides(≤100) + 2 for fan */
typedef struct {
    float       buf[2][RING_SIZE];
    atomic_uint write_pos;
    atomic_uint read_pos;
} md_ring_buf_t;
typedef struct {
    float wave[2][MD_WAVEFORM_N];
    float spec[MD_FFT_HALF];
    float imm[3], avg[3], long_avg[3];
    float imm_rel[3], avg_rel[3];
} md_sound_t;
enum {
    TOK_NUM, TOK_VAR, TOK_FUNC,
    TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_MOD,
    TOK_ASSIGN, TOK_SEMI, TOK_LPAREN, TOK_RPAREN, TOK_COMMA,
    TOK_EOF
};
typedef struct { int type; float num_val; char str_val[64]; } md_token_t;
typedef struct { char name[64]; float value; } md_var_t;
typedef struct {
    md_var_t vars[EXPR_MAX_VARS];
    int      var_count;
    uint8_t  ht[VAR_HT_SIZE];  /* index+1 into vars[], 0 = empty */
} md_var_ctx_t;
typedef struct {
    md_token_t tokens[EXPR_MAX_TOKENS];
    int        count, cur;
    int        depth;  /* H3: recursion depth limit for expression evaluator */
} md_lexer_t;
typedef struct {
    int enabled, samples, sep, bSpectrum;
    int bUseDots, bDrawThick, bAdditive;
    float scaling, smoothing, r, g, b, a;
    char per_frame[MAX_EXPR_LINES][MAX_LINE_LEN]; int per_frame_count;
    char per_point[MAX_EXPR_LINES][MAX_LINE_LEN]; int per_point_count;
    char init_code[MAX_EXPR_LINES][MAX_LINE_LEN]; int init_count;
} md_custom_wave_t;
typedef struct {
    int enabled, sides, additive, thickOutline, textured, num_inst;
    float x, y, rad, ang;
    float r, g, b, a, r2, g2, b2, a2;
    float border_r, border_g, border_b, border_a;
    float tex_ang, tex_zoom;
    char per_frame[MAX_EXPR_LINES][MAX_LINE_LEN]; int per_frame_count;
    char init_code[MAX_EXPR_LINES][MAX_LINE_LEN]; int init_count;
} md_custom_shape_t;
typedef struct {
    char filepath[1024], name[256];
    float fGammaAdj, fDecay, fVideoEchoZoom, fVideoEchoAlpha;
    int   nVideoEchoOrientation, nWaveMode;
    int   bAdditiveWaves, bWaveDots, bWaveThick;
    int   bModWaveAlphaByVolume, bMaximizeWaveColor;
    int   bTexWrap, bDarkenCenter;
    int   bBrighten, bDarken, bSolarize, bInvert;
    float fWaveAlpha, fWaveScale, fWaveSmoothing, fWaveParam;
    float fModWaveAlphaStart, fModWaveAlphaEnd;
    float fWarpAnimSpeed, fWarpScale, fZoomExponent, fShader;
    float zoom, rot, cx, cy, dx, dy, warp, sx, sy;
    float wave_r, wave_g, wave_b, wave_x, wave_y;
    float ob_size, ob_r, ob_g, ob_b, ob_a;
    float ib_size, ib_r, ib_g, ib_b, ib_a;
    float nMotionVectorsX, nMotionVectorsY;
    float mv_dx, mv_dy, mv_l, mv_r, mv_g, mv_b, mv_a;
    char  per_frame[MAX_EXPR_LINES][MAX_LINE_LEN];      int per_frame_count;
    char  per_pixel[MAX_EXPR_LINES][MAX_LINE_LEN];      int per_pixel_count;
    char  per_frame_init[MAX_EXPR_LINES][MAX_LINE_LEN]; int per_frame_init_count;
    md_custom_wave_t  waves[MAX_CUSTOM_WAVES];
    md_custom_shape_t shapes[MAX_CUSTOM_SHAPES];
} md_preset_t;
typedef struct {
    char   filepath[1024];
    /* all scalar fields serialised by md_favorites_save / load */
    float  fGammaAdj, fDecay, fVideoEchoZoom, fVideoEchoAlpha;
    int    nVideoEchoOrientation, nWaveMode;
    int    bAdditiveWaves, bWaveDots, bWaveThick;
    int    bModWaveAlphaByVolume, bMaximizeWaveColor;
    int    bTexWrap, bDarkenCenter;
    int    bBrighten, bDarken, bSolarize, bInvert;
    float  fWaveAlpha, fWaveScale, fWaveSmoothing, fWaveParam;
    float  fModWaveAlphaStart, fModWaveAlphaEnd;
    float  fWarpAnimSpeed, fWarpScale, fZoomExponent, fShader;
    float  zoom, rot, cx, cy, dx, dy, warp, sx, sy;
    float  wave_r, wave_g, wave_b, wave_x, wave_y;
    float  ob_size, ob_r, ob_g, ob_b, ob_a;
    float  ib_size, ib_r, ib_g, ib_b, ib_a;
    float  nMotionVectorsX, nMotionVectorsY;
    float  mv_dx, mv_dy, mv_l, mv_r, mv_g, mv_b, mv_a;
} md_fav_snapshot_t;
typedef struct {
    int               used;            /* 1 if this slot has data       */
    char              name[256];       /* display name (preset name)    */
    md_fav_snapshot_t snap;            /* scalar-only parameter snapshot */
    float             q_vars[NUM_Q_VAR]; /* q-var state at save time    */
} md_fav_slot_t;
typedef struct { char **paths, **names; int count, capacity; } md_preset_lib_t;
typedef struct { float x, y, r, g, b, a; } color_vert_t;
typedef struct {
    /* FBOs for feedback loop */
    GLuint fbo[2], fbo_tex[2];
    int    cur_fbo;

    /* shader programs */
    GLuint prog_warp, prog_post, prog_echo, prog_darken;
    GLuint prog_color, prog_blit;

    /* VAOs + VBOs */
    GLuint quad_vao, quad_vbo;
    GLuint mesh_vao, mesh_vbo, mesh_ebo;
    GLuint wave_vao, wave_vbo;

    int    mesh_index_count;
    GLuint gtk_fbo;  /* GtkGLArea's own FBO (not 0!) */
    int    gl_ready;

    /* current FBO texture dimensions (changes on fullscreen toggle) */
    int    tex_w, tex_h;

    /* ── persistent scratch buffers (allocated once, reused every frame) */
    /* Warp mesh vertex data: avoids per-frame malloc in gl_warp_blit */
    float        *mesh_verts;    /* MESH_FLOAT_COUNT floats */
    /* Shape fill/border scratch: avoids per-call malloc in gl_draw_shape */
    color_vert_t *shape_scratch; /* SHAPE_SCRATCH_VERTS verts */
    /* Wave vertex scratch: avoids 48 KB stack alloc per wave draw call */
    color_vert_t *wave_verts;    /* MAX_WAVE_VERTS verts */

    /* ── cached uniform locations (looked up once after shader link) ── */
    /* prog_warp */
    GLint  u_warp_tex,   u_warp_decay;
    /* prog_post */
    GLint  u_post_tex,   u_post_gamma, u_post_brighten, u_post_darken;
    GLint  u_post_solarize, u_post_invert;
    /* prog_echo */
    GLint  u_echo_tex,   u_echo_alpha, u_echo_zoom, u_echo_orient;
    /* prog_darken */
    GLint  u_darken_aspect;
    /* prog_blit */
    GLint  u_blit_tex;
} md_gl_t;
typedef struct {
    GtkWidget                    *main_box, *gl_area;
    const evemon_host_services_t *host;

    md_gl_t   gl;
    int       cur_fbo;
    md_sound_t sound;
    md_ring_buf_t ring;

    uint32_t  audio_node_ids[64];
    size_t    audio_node_count;
    uint32_t  old_audio_node_ids[64]; /* previous cycle IDs for change detection */
    size_t    old_audio_node_count;

    md_preset_lib_t lib;
    int       lib_loaded;

    md_preset_t preset, next_preset;
    int       preset_idx, preset_loaded;
    float     blend_progress;
    int       blending;
    float     blend_duration;

    md_var_ctx_t pf_vars;
    int       pf_init_done;
    float     q_vars[NUM_Q_VAR];

    float     warp_time, frame_time, total_time;
    int       frame_count;
    guint     audio_timer;
    guint     redraw_timer;
    gboolean  running;

    float     beat_acc;
    int       beat_holdoff;
    int       auto_preset, locked;

    char      info_text[512];
    int       info_frames;

    pid_t     last_pid;
    int       capture_started;

    gint64    last_frame_time;

    /* audio smoothing for bursty network streams */
    float     smooth_peak[2];    /* envelope-followed peak L/R */
    float     smooth_target[2];  /* latest raw peak target */
    float     peak_vel[2];       /* velocity for critically-damped spring */
    float     synth_phase;       /* continuous synth oscillator phase */
    gint64    last_audio_tick;   /* timestamp of last audio_tick */
    gint64    last_peak_time;    /* when we last got a non-zero peak */

    /* hotkey state */
    int       show_help;         /* F1: help overlay visible */
    int       show_preset_name;  /* F4: persistent preset name */
    int       show_fps;          /* F5: FPS counter */
    int       show_rating;       /* F6: show preset rating */
    int       show_nowplaying;   /* M: now-playing detail level
                                  *   0=off, 1=title only,
                                  *   2=title+artist+album,
                                  *   3=full (+ art + timer) */
    int       sequential_order;  /* R: sequential vs random */
    int       effects_enabled;   /* F11: all effects on/off */
    float     preset_duration;   /* seconds before auto-advance */
    float     preset_timer;      /* accumulator for auto-advance */
    float     blend_time_user;   /* B: user-configured blend time */
    int       prev_preset_idx;   /* backspace: previous preset */
    int       wave_thick;        /* override wave thick toggle */
    int       wave_dots;         /* override wave dots toggle */
    int       wave_additive;     /* override wave additive toggle */
    int       darken_center_ovr; /* D: toggle darken center */
    float     fps_accum;         /* for FPS calculation */
    int       fps_frames;
    float     fps_display;       /* last computed FPS value */
    md_preset_t *saved_preset;   /* for undo mashup — heap-allocated on demand */
    int       has_saved_preset;  /* whether saved_preset is valid */

    /* fullscreen */
    GtkWidget *fs_window;        /* fullscreen GtkWindow (NULL = embedded) */
    GtkWidget *fs_overlay;       /* the GtkOverlay we can reparent */
    GtkWidget *fs_info_area;     /* the Cairo overlay drawing area */
    GtkWidget *embed_parent;     /* original parent container */
    int       fullscreen;        /* 1 when in fullscreen mode */
    int       fs_width, fs_height; /* fullscreen monitor resolution */
    int       pending_fbo_w, pending_fbo_h; /* deferred FBO resize (0 = none) */
    int       fs_toggle_pending; /* guard: toggling already in progress */

    /* audio source display */
    char      audio_app_name[128];  /* e.g. "Firefox" */
    char      audio_media_name[256]; /* e.g. "YouTube - Song Title" */
    char      process_name[128];    /* monitored process name */

    /* MPRIS metadata (richer than PipeWire media_name) */
    char      mpris_title[256];
    char      mpris_artist[256];
    char      mpris_album[256];
    char      mpris_status[32];     /* "Playing", "Paused", "Stopped" */
    int64_t   mpris_position_us;
    int64_t   mpris_length_us;
    int       mpris_available;      /* 1 if we have MPRIS data */

    /* Album art for overlay rendering */
    char      mpris_art_url[512];   /* cached art URL (detect changes) */
    GdkPixbuf *mpris_art_pixbuf;    /* loaded album art (any size)     */

    /* Event bus subscription ID (for cleanup in destroy) */
    int       event_sub_id;

    /* per-instance debug/startup counters (were incorrectly file-static) */
    int       first_render_done;  /* 0 until first frame is drawn */
    int       dbg_frame_count;    /* counts early debug frames */

    /* Set by the host: TRUE when this instance lives in a notebook tab.
     * Used by md_update/md_clear to decide whether to show/hide the tab. */
    int       is_active;

    /* ── favorites ─────────────────────────────────────────── */
    md_fav_slot_t favorites[MD_FAV_SLOTS];
    int           show_fav_menu;       /* Ctrl+M: favorites overlay visible */
    int           fav_menu_sel;        /* cursor row in favorites menu      */
    int           fav_cycle;           /* Ctrl+F: cycle only favorites mode */
    int           fav_cycle_idx;       /* current position in fav cycle     */

    /* ── pre-lexed expression cache (re-lex only on preset change) ── */
    /*
     * md_lexer_t is ~148 KB (2048 tokens × 72 bytes).  Allocating it on
     * the stack inside the per-pixel mesh loop (1,813 calls/frame) causes
     * ~268 MB/s of stack traffic and forces the kernel to grow the stack
     * segment without ever shrinking it.  We pre-lex each program once
     * per preset switch and replay the token stream each frame/vertex.
     */
    int           preset_serial;     /* bumped every time a new preset loads  */
    int           lex_serial;        /* serial at which the lexers were built  */

    /* pre-lexed programs for the active preset */
    md_lexer_t   *lex_per_frame_init; /* per_frame_init code  */
    md_lexer_t   *lex_per_frame;      /* per_frame code        */
    md_lexer_t   *lex_per_pixel;      /* per_pixel (warp mesh) */
    /* custom waves: per-point and per-frame for each wave slot */
    md_lexer_t   *lex_wave_pf[MAX_CUSTOM_WAVES];
    md_lexer_t   *lex_wave_pp[MAX_CUSTOM_WAVES];
    /* custom shapes: per-frame for each shape slot */
    md_lexer_t   *lex_shape_pf[MAX_CUSTOM_SHAPES];

    /* persistent concat buffers — avoid 64 KB stack allocs each frame */
    char          code_buf[MAX_EXPR_LINES * MAX_LINE_LEN];
} md_ctx_t;
void fft_dit(float *re, float *im, int n);
void ensure_hann(void);
void ensure_equalize(void);
float *md_var_get(md_var_ctx_t *vc, const char *name);
void md_var_set(md_var_ctx_t *vc, const char *name, float val);
float md_var_read(md_var_ctx_t *vc, const char *name);
void md_lex(md_lexer_t *lex, const char *src);
float md_eval_precompiled(md_lexer_t *l, md_var_ctx_t *vc);
void md_concat_lines(const char lines[][MAX_LINE_LEN], int n,
                             char *out, int maxlen);
int md_load_preset(md_preset_t *p, const char *filepath);
void md_scan_dir(md_preset_lib_t *lib, const char *dir, int depth);
void md_lib_free(md_preset_lib_t *lib);
void md_lib_shuffle(md_preset_lib_t *lib);
GLuint gl_create_program(const char *vs_src, const char *fs_src);
void md_lex_cache_free(md_ctx_t *ctx);
void md_lex_cache_rebuild(md_ctx_t *ctx);
void md_analyze_sound(md_ctx_t *ctx);
void md_setup_pf_vars(md_ctx_t *ctx, md_preset_t *p);
void md_readback_pf_vars(md_ctx_t *ctx);
void md_favorites_load(md_ctx_t *ctx);
void md_fav_save_current(md_ctx_t *ctx);
int md_fav_recall(md_ctx_t *ctx, int slot);
void md_fav_delete(md_ctx_t *ctx, int slot);
void md_fav_cycle_next(md_ctx_t *ctx, int delta);
void md_random_vivid_color(float sat_min, float val_min, float val_max,
                                   float *out_r, float *out_g, float *out_b);
void show_info(md_ctx_t *ctx, const char *fmt, ...);
void md_load_preset_idx(md_ctx_t *ctx, int idx, int blend);
void gl_draw_fullscreen_quad(md_ctx_t *ctx);
void md_render_frame(md_ctx_t *ctx);
void gl_init_resources(md_ctx_t *ctx);
#ifndef EVEMON_QT_MILKDROP
void on_realize(GtkGLArea *area, gpointer data);
#endif
#ifndef EVEMON_QT_MILKDROP
gboolean on_render(GtkGLArea *area, GdkGLContext *glctx, gpointer data);
#endif
#ifndef EVEMON_QT_MILKDROP
void on_unrealize(GtkGLArea *area, gpointer data);
#endif
#ifndef EVEMON_QT_MILKDROP
gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data);
#endif
#ifndef EVEMON_QT_MILKDROP
gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data);
#endif
#ifdef EVEMON_QT_MILKDROP
gboolean md_handle_key(md_ctx_t *ctx, unsigned keyval, unsigned state);
gboolean md_draw_overlay(md_ctx_t *ctx, cairo_t *cr, int ww, int hh);
#else
gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data);
gboolean on_overlay_draw(GtkWidget *w, cairo_t *cr, gpointer data);
#endif

#endif
