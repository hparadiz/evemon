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

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <float.h>
#include <epoxy/gl.h>

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
#define MAX_LINE_LEN    1024

#define MAX_CUSTOM_WAVES  4
#define MAX_CUSTOM_SHAPES 4
#define NUM_Q_VAR  32
#define NUM_T_VAR  8

#define PRESET_DIR \
    "/home/akujin/.local/share/Steam/steamapps/common/projectM/presets"

/* max vertices for wave/shape drawing */
#define MAX_WAVE_VERTS  2048

/* ── SPSC ring buffer ────────────────────────────────────────── */

typedef struct {
    float       buf[2][RING_SIZE];
    atomic_uint write_pos;
    atomic_uint read_pos;
} md_ring_buf_t;

/* ── FFT ─────────────────────────────────────────────────────── */

static void fft_dit(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wre = cosf(ang), wim = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cur_re = 1.0f, cur_im = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float tre = re[i+j+len/2]*cur_re - im[i+j+len/2]*cur_im;
                float tim = re[i+j+len/2]*cur_im + im[i+j+len/2]*cur_re;
                re[i+j+len/2] = re[i+j] - tre;
                im[i+j+len/2] = im[i+j] - tim;
                re[i+j] += tre; im[i+j] += tim;
                float nre = cur_re*wre - cur_im*wim;
                cur_im = cur_re*wim + cur_im*wre;
                cur_re = nre;
            }
        }
    }
}

static float g_hann[MD_FFT_SIZE];
static int   g_hann_ready;
static void ensure_hann(void)
{
    if (g_hann_ready) return;
    for (int i = 0; i < MD_FFT_SIZE; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f*(float)M_PI*i/(MD_FFT_SIZE-1)));
    g_hann_ready = 1;
}

static float g_equalize[MD_FFT_HALF];
static int   g_eq_ready;
static void ensure_equalize(void)
{
    if (g_eq_ready) return;
    for (int i = 0; i < MD_FFT_HALF; i++) {
        float x = (float)(MD_FFT_HALF - i) / (float)MD_FFT_HALF;
        g_equalize[i] = -0.02f * logf(x + 1e-10f);
    }
    g_eq_ready = 1;
}

typedef struct {
    float wave[2][MD_WAVEFORM_N];
    float spec[MD_FFT_HALF];
    float imm[3], avg[3], long_avg[3];
    float imm_rel[3], avg_rel[3];
} md_sound_t;

/* ══════════════════════════════════════════════════════════════
 *  EXPRESSION EVALUATOR  (unchanged from Cairo version)
 * ══════════════════════════════════════════════════════════════ */

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
} md_var_ctx_t;

static float *md_var_get(md_var_ctx_t *vc, const char *name)
{
    for (int i = 0; i < vc->var_count; i++)
        if (strcmp(vc->vars[i].name, name) == 0)
            return &vc->vars[i].value;
    if (vc->var_count < EXPR_MAX_VARS) {
        int i = vc->var_count++;
        strncpy(vc->vars[i].name, name, 63);
        vc->vars[i].name[63] = 0;
        vc->vars[i].value = 0.0f;
        return &vc->vars[i].value;
    }
    return NULL;
}

static void md_var_set(md_var_ctx_t *vc, const char *name, float val)
{ float *p = md_var_get(vc, name); if (p) *p = val; }

static float md_var_read(md_var_ctx_t *vc, const char *name)
{ float *p = md_var_get(vc, name); return p ? *p : 0.0f; }

typedef struct {
    md_token_t tokens[EXPR_MAX_TOKENS];
    int        count, cur;
    int        depth;  /* H3: recursion depth limit for expression evaluator */
} md_lexer_t;

#define MD_EVAL_MAX_DEPTH 128

static const char *g_func_names[] = {
    "sin","cos","tan","asin","acos","atan","atan2",
    "pow","log","log10","sqrt","abs","min","max",
    "sign","sqr","if","above","below","equal",
    "bnot","band","bor","rand","floor","ceil",
    "int","sigmoid","avg","exp","fmod",NULL
};

static void md_lex(md_lexer_t *lex, const char *src)
{
    lex->count = 0; lex->cur = 0;
    int pos = 0, len = (int)strlen(src);
    while (pos < len && lex->count < EXPR_MAX_TOKENS - 1) {
        char c = src[pos];
        if (c==' '||c=='\t'||c=='\r'||c=='\n') { pos++; continue; }
        if (c=='/' && pos+1<len && src[pos+1]=='/') {
            while (pos<len && src[pos]!='\n') pos++;
            continue;
        }
        md_token_t *t = &lex->tokens[lex->count];
        if (isdigit((unsigned char)c) ||
            (c=='.' && pos+1<len && isdigit((unsigned char)src[pos+1]))) {
            char buf[64]; int bi=0;
            while (pos<len && bi<62 && (isdigit((unsigned char)src[pos])||src[pos]=='.'))
                buf[bi++]=src[pos++];
            if (pos<len && (src[pos]=='e'||src[pos]=='E')) {
                buf[bi++]=src[pos++];
                if (pos<len && (src[pos]=='+'||src[pos]=='-')) buf[bi++]=src[pos++];
                while (pos<len && bi<62 && isdigit((unsigned char)src[pos]))
                    buf[bi++]=src[pos++];
            }
            buf[bi]=0; t->type=TOK_NUM; t->num_val=(float)atof(buf);
            lex->count++; continue;
        }
        if (isalpha((unsigned char)c)||c=='_') {
            char buf[64]; int bi=0;
            while (pos<len && bi<62 && (isalnum((unsigned char)src[pos])||src[pos]=='_'))
                buf[bi++]=src[pos++];
            buf[bi]=0;
            int sv=pos;
            while (sv<len && (src[sv]==' '||src[sv]=='\t')) sv++;
            int is_func=0;
            if (sv<len && src[sv]=='(') {
                for (int i=0; g_func_names[i]; i++)
                    if (!strcmp(buf,g_func_names[i])) { is_func=1; break; }
            }
            t->type = is_func ? TOK_FUNC : TOK_VAR;
            memcpy(t->str_val, buf, 63); t->str_val[63]=0;
            lex->count++; continue;
        }
        t->str_val[0]=c; t->str_val[1]=0;
        switch (c) {
        case '+': t->type=TOK_PLUS; break;   case '-': t->type=TOK_MINUS; break;
        case '*': t->type=TOK_MUL; break;    case '/': t->type=TOK_DIV; break;
        case '%': t->type=TOK_MOD; break;    case '=': t->type=TOK_ASSIGN; break;
        case ';': t->type=TOK_SEMI; break;   case '(': t->type=TOK_LPAREN; break;
        case ')': t->type=TOK_RPAREN; break; case ',': t->type=TOK_COMMA; break;
        default: pos++; continue;
        }
        pos++; lex->count++;
    }
    lex->tokens[lex->count].type = TOK_EOF;
}

static float md_eval_assign(md_lexer_t *l, md_var_ctx_t *vc);
static md_token_t *md_peek(md_lexer_t *l) { return &l->tokens[l->cur]; }
static md_token_t *md_adv(md_lexer_t *l) {
    md_token_t *t = &l->tokens[l->cur];
    if (t->type != TOK_EOF) l->cur++;
    return t;
}

static float md_eval_mul(md_lexer_t *l, md_var_ctx_t *vc);
static float md_eval_add(md_lexer_t *l, md_var_ctx_t *vc)
{
    float v = md_eval_mul(l, vc);
    while (md_peek(l)->type==TOK_PLUS || md_peek(l)->type==TOK_MINUS) {
        int op = md_adv(l)->type;
        float r = md_eval_mul(l, vc);
        v = (op==TOK_PLUS) ? v+r : v-r;
    }
    return v;
}

static float md_eval_primary(md_lexer_t *l, md_var_ctx_t *vc);
static float md_eval_unary(md_lexer_t *l, md_var_ctx_t *vc)
{
    if (md_peek(l)->type==TOK_MINUS) { md_adv(l); return -md_eval_unary(l,vc); }
    if (md_peek(l)->type==TOK_PLUS)  { md_adv(l); return  md_eval_unary(l,vc); }
    return md_eval_primary(l,vc);
}

static float md_eval_mul(md_lexer_t *l, md_var_ctx_t *vc)
{
    float v = md_eval_unary(l,vc);
    while (md_peek(l)->type==TOK_MUL||md_peek(l)->type==TOK_DIV||md_peek(l)->type==TOK_MOD) {
        int op = md_adv(l)->type;
        float r = md_eval_unary(l,vc);
        if (op==TOK_MUL) v*=r;
        else if (op==TOK_DIV) v = r!=0 ? v/r : 0;
        else v = r!=0 ? fmodf(v,r) : 0;
    }
    return v;
}

static float md_eval_primary(md_lexer_t *l, md_var_ctx_t *vc)
{
    /* H3: guard against unbounded recursion from malicious presets */
    if (l->depth >= MD_EVAL_MAX_DEPTH) return 0.0f;
    l->depth++;

    md_token_t *t = md_peek(l);
    float result = 0.0f;
    if (t->type==TOK_NUM) { md_adv(l); result = t->num_val; }
    else if (t->type==TOK_LPAREN) {
        md_adv(l); result=md_eval_assign(l,vc);
        if (md_peek(l)->type==TOK_RPAREN) md_adv(l);
    }
    else if (t->type==TOK_FUNC) {
        md_adv(l); char fn[64]; strncpy(fn,t->str_val,63); fn[63]=0;
        if (md_peek(l)->type==TOK_LPAREN) md_adv(l);
        float a[8]; int na=0;
        if (md_peek(l)->type!=TOK_RPAREN) {
            a[na++]=md_eval_assign(l,vc);
            while (md_peek(l)->type==TOK_COMMA && na<8) { md_adv(l); a[na++]=md_eval_assign(l,vc); }
        }
        if (md_peek(l)->type==TOK_RPAREN) md_adv(l);
        float x=na>0?a[0]:0, y=na>1?a[1]:0, z=na>2?a[2]:0;
        if (!strcmp(fn,"sin"))        result = sinf(x);
        else if (!strcmp(fn,"cos"))   result = cosf(x);
        else if (!strcmp(fn,"tan"))   result = tanf(x);
        else if (!strcmp(fn,"asin"))  result = asinf(fminf(fmaxf(x,-1),1));
        else if (!strcmp(fn,"acos"))  result = acosf(fminf(fmaxf(x,-1),1));
        else if (!strcmp(fn,"atan"))  result = atanf(x);
        else if (!strcmp(fn,"atan2")) result = atan2f(x,y);
        else if (!strcmp(fn,"pow"))   result = powf(fabsf(x)+1e-30f,y);
        else if (!strcmp(fn,"exp"))   result = expf(fminf(x,80.0f));
        else if (!strcmp(fn,"log"))   result = logf(fabsf(x)+1e-30f);
        else if (!strcmp(fn,"log10")) result = log10f(fabsf(x)+1e-30f);
        else if (!strcmp(fn,"sqrt"))  result = sqrtf(fabsf(x));
        else if (!strcmp(fn,"abs"))   result = fabsf(x);
        else if (!strcmp(fn,"min"))   result = fminf(x,y);
        else if (!strcmp(fn,"max"))   result = fmaxf(x,y);
        else if (!strcmp(fn,"sign"))  result = (x>0)?1:(x<0)?-1:0;
        else if (!strcmp(fn,"sqr"))   result = x*x;
        else if (!strcmp(fn,"floor")) result = floorf(x);
        else if (!strcmp(fn,"ceil"))  result = ceilf(x);
        else if (!strcmp(fn,"int"))   result = (float)(int)x;
        else if (!strcmp(fn,"fmod"))  result = y!=0?fmodf(x,y):0;
        else if (!strcmp(fn,"rand"))  result = (float)(g_random_int_range(0,10000))/10000.0f*(x>0?x:1);
        else if (!strcmp(fn,"sigmoid")) result = 1.0f/(1.0f+expf(-x*y));
        else if (!strcmp(fn,"avg"))   result = (x+y)*0.5f;
        else if (!strcmp(fn,"if"))    result = (fabsf(x)>0.00001f)?y:z;
        else if (!strcmp(fn,"above")) result = (x>y)?1:0;
        else if (!strcmp(fn,"below")) result = (x<y)?1:0;
        else if (!strcmp(fn,"equal")) result = (fabsf(x-y)<0.00001f)?1:0;
        else if (!strcmp(fn,"bnot"))  result = (fabsf(x)<0.00001f)?1:0;
        else if (!strcmp(fn,"band"))  result = (fabsf(x)>1e-5f&&fabsf(y)>1e-5f)?1:0;
        else if (!strcmp(fn,"bor"))   result = (fabsf(x)>1e-5f||fabsf(y)>1e-5f)?1:0;
    }
    else if (t->type==TOK_VAR) { md_adv(l); result = md_var_read(vc,t->str_val); }
    else if (t->type!=TOK_EOF) { md_adv(l); }

    l->depth--;
    return result;
}

static float md_eval_assign(md_lexer_t *l, md_var_ctx_t *vc)
{
    if (md_peek(l)->type==TOK_VAR) {
        int save = l->cur;
        md_token_t *vt = md_adv(l);
        if (md_peek(l)->type==TOK_ASSIGN) {
            md_adv(l); float v=md_eval_assign(l,vc);
            md_var_set(vc,vt->str_val,v); return v;
        }
        int op = md_peek(l)->type;
        if ((op==TOK_PLUS||op==TOK_MINUS||op==TOK_MUL||op==TOK_DIV) &&
            l->cur+1<=l->count && l->tokens[l->cur+1].type==TOK_ASSIGN) {
            md_adv(l); md_adv(l);
            float rhs=md_eval_assign(l,vc), cur=md_var_read(vc,vt->str_val), v;
            switch (op) {
            case TOK_PLUS:v=cur+rhs;break; case TOK_MINUS:v=cur-rhs;break;
            case TOK_MUL:v=cur*rhs;break;  case TOK_DIV:v=rhs?cur/rhs:0;break;
            default:v=rhs;break;
            }
            md_var_set(vc,vt->str_val,v); return v;
        }
        l->cur = save;
    }
    return md_eval_add(l,vc);
}

static float md_eval_program(md_lexer_t *l, md_var_ctx_t *vc)
{
    float r=0;
    while (md_peek(l)->type!=TOK_EOF) {
        r = md_eval_assign(l,vc);
        if (md_peek(l)->type==TOK_SEMI) md_adv(l);
    }
    return r;
}

static float md_eval(const char *expr, md_var_ctx_t *vc)
{
    if (!expr || !expr[0]) return 0;
    md_lexer_t lex; md_lex(&lex, expr);
    return md_eval_program(&lex, vc);
}

static void md_concat_lines(const char lines[][MAX_LINE_LEN], int n,
                             char *out, int maxlen)
{
    int pos=0; out[0]=0;
    for (int i=0; i<n; i++) {
        int ll=(int)strlen(lines[i]);
        if (pos+ll+2>=maxlen) break;
        memcpy(out+pos, lines[i], ll); pos+=ll; out[pos++]=';';
    }
    out[pos]=0;
}

/* ── custom wave / shape / preset structs ────────────────────── */

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

/* ── .milk parser ────────────────────────────────────────────── */

static void md_preset_defaults(md_preset_t *p)
{
    memset(p, 0, sizeof(*p));
    p->fGammaAdj=1; p->fDecay=0.98f; p->fVideoEchoZoom=2;
    p->bWaveThick=1; p->fWaveAlpha=0.8f; p->fWaveScale=1;
    p->fWaveSmoothing=0.75f; p->fWarpAnimSpeed=1; p->fWarpScale=1;
    p->fZoomExponent=1; p->zoom=1; p->cx=0.5f; p->cy=0.5f;
    p->sx=1; p->sy=1;
    p->wave_r=1; p->wave_g=1; p->wave_b=1;
    p->wave_x=0.5f; p->wave_y=0.5f;
    p->ob_size=0.01f; p->ib_size=0.01f;
    p->ib_r=p->ib_g=p->ib_b=0.25f;
    p->mv_r=p->mv_g=p->mv_b=1; p->mv_l=0.9f;
    p->fModWaveAlphaStart=0.75f; p->fModWaveAlphaEnd=0.95f;
    for (int i=0; i<MAX_CUSTOM_WAVES; i++) {
        p->waves[i].samples=512; p->waves[i].scaling=1;
        p->waves[i].smoothing=0.5f;
        p->waves[i].r=p->waves[i].g=p->waves[i].b=p->waves[i].a=1;
    }
    for (int i=0; i<MAX_CUSTOM_SHAPES; i++) {
        p->shapes[i].sides=4; p->shapes[i].x=p->shapes[i].y=0.5f;
        p->shapes[i].rad=0.1f; p->shapes[i].r=p->shapes[i].a=1;
        p->shapes[i].tex_zoom=1; p->shapes[i].num_inst=1;
        p->shapes[i].border_r=p->shapes[i].border_g=p->shapes[i].border_b=1;
        p->shapes[i].border_a=0.1f;
    }
}

static int md_load_preset(md_preset_t *p, const char *filepath)
{
    FILE *f = fopen(filepath,"r");
    if (!f) return -1;
    md_preset_defaults(p);
    strncpy(p->filepath, filepath, sizeof(p->filepath)-1);
    const char *slash=strrchr(filepath,'/');
    const char *base=slash?slash+1:filepath;
    strncpy(p->name, base, sizeof(p->name)-1);
    char *dot=strrchr(p->name,'.'); if (dot && !strcmp(dot,".milk")) *dot=0;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        int len=(int)strlen(line);
        while (len>0 && strchr("\r\n \t",line[len-1])) line[--len]=0;
        if (!len||line[0]=='['||line[0]=='#') continue;
        if (!strncmp(line,"MILKDROP",8)||!strncmp(line,"PSVERSION",9)) continue;
        char *eq=strchr(line,'='); if (!eq) continue;
        *eq=0; char *key=line, *val=eq+1;
        while (*key==' '||*key=='\t') key++;
        int klen=(int)strlen(key);
        while (klen>0 && (key[klen-1]==' '||key[klen-1]=='\t')) key[--klen]=0;

        #define PI(k,fld) if(!strcmp(key,k)){p->fld=atoi(val);continue;}
        #define PF(k,fld) if(!strcmp(key,k)){p->fld=(float)atof(val);continue;}
        PF("fGammaAdj",fGammaAdj) PF("fDecay",fDecay)
        PF("fVideoEchoZoom",fVideoEchoZoom) PF("fVideoEchoAlpha",fVideoEchoAlpha)
        PI("nVideoEchoOrientation",nVideoEchoOrientation)
        PI("nWaveMode",nWaveMode) PI("bAdditiveWaves",bAdditiveWaves)
        PI("bWaveDots",bWaveDots) PI("bWaveThick",bWaveThick)
        PI("bModWaveAlphaByVolume",bModWaveAlphaByVolume)
        PI("bMaximizeWaveColor",bMaximizeWaveColor)
        PI("bTexWrap",bTexWrap) PI("bDarkenCenter",bDarkenCenter)
        PI("bBrighten",bBrighten) PI("bDarken",bDarken)
        PI("bSolarize",bSolarize) PI("bInvert",bInvert)
        PF("fWaveAlpha",fWaveAlpha) PF("fWaveScale",fWaveScale)
        PF("fWaveSmoothing",fWaveSmoothing) PF("fWaveParam",fWaveParam)
        PF("fModWaveAlphaStart",fModWaveAlphaStart)
        PF("fModWaveAlphaEnd",fModWaveAlphaEnd)
        PF("fWarpAnimSpeed",fWarpAnimSpeed) PF("fWarpScale",fWarpScale)
        PF("fZoomExponent",fZoomExponent) PF("fShader",fShader)
        PF("zoom",zoom) PF("rot",rot) PF("cx",cx) PF("cy",cy)
        PF("dx",dx) PF("dy",dy) PF("warp",warp) PF("sx",sx) PF("sy",sy)
        PF("wave_r",wave_r) PF("wave_g",wave_g) PF("wave_b",wave_b)
        PF("wave_x",wave_x) PF("wave_y",wave_y)
        PF("ob_size",ob_size) PF("ob_r",ob_r) PF("ob_g",ob_g)
        PF("ob_b",ob_b) PF("ob_a",ob_a)
        PF("ib_size",ib_size) PF("ib_r",ib_r) PF("ib_g",ib_g)
        PF("ib_b",ib_b) PF("ib_a",ib_a)
        PF("nMotionVectorsX",nMotionVectorsX) PF("nMotionVectorsY",nMotionVectorsY)
        PF("mv_dx",mv_dx) PF("mv_dy",mv_dy) PF("mv_l",mv_l)
        PF("mv_r",mv_r) PF("mv_g",mv_g) PF("mv_b",mv_b) PF("mv_a",mv_a)
        #undef PI
        #undef PF

        { int n;
          if (sscanf(key,"per_frame_init_%d",&n)==1 && n>=1 && n-1<MAX_EXPR_LINES) {
              strncpy(p->per_frame_init[n-1],val,MAX_LINE_LEN-1);
              if (n>p->per_frame_init_count) p->per_frame_init_count=n;
              continue;
          }
          if (sscanf(key,"per_frame_%d",&n)==1 && n>=1 && n-1<MAX_EXPR_LINES) {
              strncpy(p->per_frame[n-1],val,MAX_LINE_LEN-1);
              if (n>p->per_frame_count) p->per_frame_count=n;
              continue;
          }
          if (sscanf(key,"per_pixel_%d",&n)==1 && n>=1 && n-1<MAX_EXPR_LINES) {
              strncpy(p->per_pixel[n-1],val,MAX_LINE_LEN-1);
              if (n>p->per_pixel_count) p->per_pixel_count=n;
              continue;
          }
        }

        for (int wi=0; wi<MAX_CUSTOM_WAVES; wi++) {
            char pfx[32]; md_custom_wave_t *w=&p->waves[wi];
            snprintf(pfx,sizeof(pfx),"wavecode_%d_",wi);
            if (!strncmp(key,pfx,strlen(pfx))) {
                char *pm=key+strlen(pfx);
                if (!strcmp(pm,"enabled")) w->enabled=atoi(val);
                else if (!strcmp(pm,"samples")) w->samples=atoi(val);
                else if (!strcmp(pm,"sep")) w->sep=atoi(val);
                else if (!strcmp(pm,"bSpectrum")) w->bSpectrum=atoi(val);
                else if (!strcmp(pm,"bUseDots")) w->bUseDots=atoi(val);
                else if (!strcmp(pm,"bDrawThick")) w->bDrawThick=atoi(val);
                else if (!strcmp(pm,"bAdditive")) w->bAdditive=atoi(val);
                else if (!strcmp(pm,"scaling")) w->scaling=(float)atof(val);
                else if (!strcmp(pm,"smoothing")) w->smoothing=(float)atof(val);
                else if (!strcmp(pm,"r")) w->r=(float)atof(val);
                else if (!strcmp(pm,"g")) w->g=(float)atof(val);
                else if (!strcmp(pm,"b")) w->b=(float)atof(val);
                else if (!strcmp(pm,"a")) w->a=(float)atof(val);
                goto next;
            }
            { int wn,nn;
              if (sscanf(key,"wave_%d_per_point%d",&wn,&nn)==2 && wn==wi
                  && nn>=1 && nn-1<MAX_EXPR_LINES) {
                  strncpy(w->per_point[nn-1],val,MAX_LINE_LEN-1);
                  if (nn>w->per_point_count) w->per_point_count=nn;
                  goto next;
              }
              if (sscanf(key,"wave_%d_per_frame%d",&wn,&nn)==2 && wn==wi
                  && nn>=1 && nn-1<MAX_EXPR_LINES) {
                  strncpy(w->per_frame[nn-1],val,MAX_LINE_LEN-1);
                  if (nn>w->per_frame_count) w->per_frame_count=nn;
                  goto next;
              }
              if (sscanf(key,"wave_%d_init%d",&wn,&nn)==2 && wn==wi
                  && nn>=1 && nn-1<MAX_EXPR_LINES) {
                  strncpy(w->init_code[nn-1],val,MAX_LINE_LEN-1);
                  if (nn>w->init_count) w->init_count=nn;
                  goto next;
              }
            }
        }
        for (int si=0; si<MAX_CUSTOM_SHAPES; si++) {
            char pfx[32]; md_custom_shape_t *s=&p->shapes[si];
            snprintf(pfx,sizeof(pfx),"shapecode_%d_",si);
            if (!strncmp(key,pfx,strlen(pfx))) {
                char *pm=key+strlen(pfx);
                if (!strcmp(pm,"enabled")) s->enabled=atoi(val);
                else if (!strcmp(pm,"sides")) s->sides=atoi(val);
                else if (!strcmp(pm,"additive")) s->additive=atoi(val);
                else if (!strcmp(pm,"thickOutline")) s->thickOutline=atoi(val);
                else if (!strcmp(pm,"textured")) s->textured=atoi(val);
                else if (!strcmp(pm,"num_inst")) s->num_inst=atoi(val);
                else if (!strcmp(pm,"x")) s->x=(float)atof(val);
                else if (!strcmp(pm,"y")) s->y=(float)atof(val);
                else if (!strcmp(pm,"rad")) s->rad=(float)atof(val);
                else if (!strcmp(pm,"ang")) s->ang=(float)atof(val);
                else if (!strcmp(pm,"r")) s->r=(float)atof(val);
                else if (!strcmp(pm,"g")) s->g=(float)atof(val);
                else if (!strcmp(pm,"b")) s->b=(float)atof(val);
                else if (!strcmp(pm,"a")) s->a=(float)atof(val);
                else if (!strcmp(pm,"r2")) s->r2=(float)atof(val);
                else if (!strcmp(pm,"g2")) s->g2=(float)atof(val);
                else if (!strcmp(pm,"b2")) s->b2=(float)atof(val);
                else if (!strcmp(pm,"a2")) s->a2=(float)atof(val);
                else if (!strcmp(pm,"border_r")) s->border_r=(float)atof(val);
                else if (!strcmp(pm,"border_g")) s->border_g=(float)atof(val);
                else if (!strcmp(pm,"border_b")) s->border_b=(float)atof(val);
                else if (!strcmp(pm,"border_a")) s->border_a=(float)atof(val);
                else if (!strcmp(pm,"tex_ang")) s->tex_ang=(float)atof(val);
                else if (!strcmp(pm,"tex_zoom")) s->tex_zoom=(float)atof(val);
                goto next;
            }
            { int sn,nn;
              if (sscanf(key,"shape_%d_per_frame%d",&sn,&nn)==2 && sn==si
                  && nn>=1 && nn-1<MAX_EXPR_LINES) {
                  strncpy(s->per_frame[nn-1],val,MAX_LINE_LEN-1);
                  if (nn>s->per_frame_count) s->per_frame_count=nn;
                  goto next;
              }
              if (sscanf(key,"shape_%d_init%d",&sn,&nn)==2 && sn==si
                  && nn>=1 && nn-1<MAX_EXPR_LINES) {
                  strncpy(s->init_code[nn-1],val,MAX_LINE_LEN-1);
                  if (nn>s->init_count) s->init_count=nn;
                  goto next;
              }
            }
        }
next:;
    }
    fclose(f); return 0;
}

/* ── preset library ──────────────────────────────────────────── */

typedef struct { char **paths, **names; int count, capacity; } md_preset_lib_t;

static void md_lib_add(md_preset_lib_t *lib, const char *path, const char *name)
{
    if (lib->count>=lib->capacity) {
        lib->capacity = lib->capacity?lib->capacity*2:1024;
        lib->paths=realloc(lib->paths,lib->capacity*sizeof(char*));
        lib->names=realloc(lib->names,lib->capacity*sizeof(char*));
    }
    lib->paths[lib->count]=strdup(path);
    char *dn=strdup(name); char *dt=strrchr(dn,'.');
    if (dt && !strcmp(dt,".milk")) *dt=0;
    lib->names[lib->count]=dn; lib->count++;
}

static void md_scan_dir(md_preset_lib_t *lib, const char *dir, int depth)
{
    if (depth>10) return;
    DIR *d=opendir(dir); if (!d) return;
    struct dirent *ent;
    while ((ent=readdir(d))) {
        if (ent->d_name[0]=='.') continue;
        char fp[2048]; snprintf(fp,sizeof(fp),"%s/%s",dir,ent->d_name);
        struct stat st; if (stat(fp,&st)) continue;
        if (S_ISDIR(st.st_mode)) md_scan_dir(lib,fp,depth+1);
        else if (S_ISREG(st.st_mode)) {
            int nl=(int)strlen(ent->d_name);
            if (nl>5 && !strcmp(ent->d_name+nl-5,".milk"))
                md_lib_add(lib,fp,ent->d_name);
        }
    }
    closedir(d);
}

static void md_lib_free(md_preset_lib_t *lib)
{
    for (int i=0;i<lib->count;i++) { free(lib->paths[i]); free(lib->names[i]); }
    free(lib->paths); free(lib->names); memset(lib,0,sizeof(*lib));
}

static void md_lib_shuffle(md_preset_lib_t *lib)
{
    for (int i=lib->count-1;i>0;i--) {
        int j=g_random_int_range(0,i+1);
        char *tp=lib->paths[i]; lib->paths[i]=lib->paths[j]; lib->paths[j]=tp;
        char *tn=lib->names[i]; lib->names[i]=lib->names[j]; lib->names[j]=tn;
    }
}

/* ══════════════════════════════════════════════════════════════
 *  OPENGL SHADER HELPERS
 * ══════════════════════════════════════════════════════════════ */

static GLuint gl_compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s,512,NULL,log);
        fprintf(stderr,"milkdrop: shader compile error: %s\n",log);
        glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint gl_link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p,512,NULL,log);
        fprintf(stderr,"milkdrop: program link error: %s\n",log);
        glDeleteProgram(p); return 0;
    }
    return p;
}

static GLuint gl_create_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = gl_compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = gl_compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }
    GLuint p = gl_link_program(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

/* ── GLSL shader sources ─────────────────────────────────────── */

/* Warp mesh: vertex has position + UV (warped by CPU-side expression eval) */
static const char *WARP_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos*2.0-1.0, 0.0, 1.0);\n"
    "  vUV = aUV;\n"
    "}\n";

static const char *WARP_FS =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uDecay;\n"
    "void main() {\n"
    "  vec3 c = texture(uTex, clamp(vUV, 0.0, 1.0)).rgb;\n"
    "  fragColor = vec4(clamp(c * uDecay, 0.0, 1.0), 1.0);\n"
    "}\n";

/* Fullscreen quad (for post-process, composite, echo) */
static const char *QUAD_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos*2.0-1.0, 0.0, 1.0);\n"
    "  vUV = aPos;\n"
    "}\n";

/* Post-process: gamma, brighten, darken, solarize, invert */
static const char *POST_FS =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uGamma;\n"
    "uniform int uBrighten, uDarken, uSolarize, uInvert;\n"
    "void main() {\n"
    "  vec3 c = clamp(texture(uTex, vUV).rgb, 0.0, 1.0);\n"
    "  if (uGamma > 0.01 && abs(uGamma - 1.0) > 0.01)\n"
    "    c = pow(c, vec3(1.0 / uGamma));\n"
    "  if (uBrighten != 0) c = sqrt(clamp(c,0.0,1.0));\n"
    "  if (uDarken != 0) c = c*c;\n"
    "  if (uSolarize != 0) { c = clamp(c,0.0,1.0); c = c*(1.0-c)*4.0; }\n"
    "  if (uInvert != 0) c = 1.0 - clamp(c,0.0,1.0);\n"
    "  fragColor = vec4(clamp(c,0.0,1.0), 1.0);\n"
    "}\n";

/* Video echo */
static const char *ECHO_FS =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uAlpha;\n"
    "uniform float uZoom;\n"
    "uniform int uOrient;\n"
    "void main() {\n"
    "  vec2 uv = vUV;\n"
    "  if (uOrient==1||uOrient==3) uv.x = 1.0-uv.x;\n"
    "  if (uOrient==2||uOrient==3) uv.y = 1.0-uv.y;\n"
    "  uv = (uv - 0.5) / uZoom + 0.5;\n"
    "  vec3 c = texture(uTex, uv).rgb;\n"
    "  fragColor = vec4(c, uAlpha);\n"
    "}\n";

/* Darken center radial gradient */
static const char *DARKEN_FS =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform float uAspect;\n"
    "void main() {\n"
    "  vec2 d = (vUV - 0.5) * vec2(uAspect, 1.0);\n"
    "  float r = length(d) * 2.857;\n" /* 1/0.35 */
    "  float a = 0.65 * clamp(1.0 - r, 0.0, 1.0);\n"
    "  fragColor = vec4(0.0, 0.0, 0.0, a);\n"
    "}\n";

/* Simple color shader for waves/shapes/borders */
static const char *COLOR_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec4 aColor;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos*2.0-1.0, 0.0, 1.0);\n"
    "  vColor = aColor;\n"
    "}\n";

static const char *COLOR_FS =
    "#version 330 core\n"
    "in vec4 vColor;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = vColor; }\n";

/* Passthrough blit (display final to screen) */
static const char *BLIT_FS =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex;\n"
    "void main() { fragColor = vec4(texture(uTex, vUV).rgb, 1.0); }\n";

/* ── per-instance GL state ───────────────────────────────────── */

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
} md_gl_t;

/* ── per-instance plugin state ───────────────────────────────── */

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
    md_preset_t saved_preset;    /* for undo mashup */
    int       has_saved_preset;  /* whether saved_preset is valid */

    /* fullscreen */
    GtkWidget *fs_window;        /* fullscreen GtkWindow (NULL = embedded) */
    GtkWidget *fs_overlay;       /* the GtkOverlay we can reparent */
    GtkWidget *fs_info_area;     /* the Cairo overlay drawing area */
    GtkWidget *embed_parent;     /* original parent container */
    int       fullscreen;        /* 1 when in fullscreen mode */
    int       fs_width, fs_height; /* fullscreen monitor resolution */

    /* audio source display */
    char      audio_app_name[128];  /* e.g. "Firefox" */
    char      audio_media_name[256]; /* e.g. "YouTube - Song Title" */
    char      process_name[128];    /* monitored process name */
} md_ctx_t;

/* ── sound analysis ──────────────────────────────────────────── */

static void md_analyze_sound(md_ctx_t *ctx)
{
    md_sound_t *snd = &ctx->sound;
    ensure_hann(); ensure_equalize();
    unsigned wp = atomic_load_explicit(&ctx->ring.write_pos, memory_order_acquire);
    unsigned rp = atomic_load_explicit(&ctx->ring.read_pos,  memory_order_relaxed);
    unsigned avail = wp - rp;
    if (avail < (unsigned)MD_WAVEFORM_N)
        memset(snd->wave, 0, sizeof(snd->wave));
    else {
        if (avail > RING_SIZE/2) rp = wp - MD_WAVEFORM_N;
        for (int i=0; i<MD_WAVEFORM_N; i++) {
            snd->wave[0][i] = ctx->ring.buf[0][(rp+i)%RING_SIZE];
            snd->wave[1][i] = ctx->ring.buf[1][(rp+i)%RING_SIZE];
        }
        rp += MD_WAVEFORM_N/2;
        atomic_store_explicit(&ctx->ring.read_pos, rp, memory_order_release);
    }
    float fft_re[MD_FFT_SIZE], fft_im[MD_FFT_SIZE];
    memset(fft_re,0,sizeof(fft_re)); memset(fft_im,0,sizeof(fft_im));
    for (int i=0; i<MD_WAVEFORM_N && i<MD_FFT_SIZE; i++)
        fft_re[i] = snd->wave[0][i] * g_hann[i];
    fft_dit(fft_re, fft_im, MD_FFT_SIZE);
    for (int i=0; i<MD_FFT_HALF; i++) {
        float mag = sqrtf(fft_re[i]*fft_re[i]+fft_im[i]*fft_im[i])
                    / (float)MD_FFT_SIZE * (1+g_equalize[i]);
        snd->spec[i] = mag;
    }
    int bands[3][2] = {{0,85},{86,170},{171,255}};
    for (int b=0; b<3; b++) {
        float sum=0;
        int lo=bands[b][0], hi=(int)fminf(bands[b][1], MD_FFT_HALF-1);
        for (int i=lo; i<=hi; i++) sum += snd->spec[i];
        snd->imm[b] = sum/(hi-lo+1);
    }
    float dt = ctx->frame_time*30;
    if (dt<0.01f) dt=0.01f;
    if (dt>2) dt=2;
    for (int b=0; b<3; b++) {
        float rate = snd->imm[b]>snd->avg[b] ? 0.2f : 0.5f;
        snd->avg[b] += (1-powf(1-rate,dt))*(snd->imm[b]-snd->avg[b]);
        snd->long_avg[b] += (1-powf(0.992f,dt))*(snd->imm[b]-snd->long_avg[b]);
        if (snd->long_avg[b]>0.001f) {
            snd->imm_rel[b] = snd->imm[b]/snd->long_avg[b];
            snd->avg_rel[b] = snd->avg[b]/snd->long_avg[b];
        } else { snd->imm_rel[b]=1; snd->avg_rel[b]=1; }
    }
}

/* ── per-frame variable setup ────────────────────────────────── */

static void md_setup_pf_vars(md_ctx_t *ctx, md_preset_t *p)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    vc->var_count = 0;
    md_var_set(vc,"time",ctx->total_time);
    md_var_set(vc,"fps",1.0f/(ctx->frame_time+0.0001f));
    md_var_set(vc,"frame",(float)ctx->frame_count);
    md_var_set(vc,"progress",fmodf(ctx->total_time*0.1f,1));
    md_var_set(vc,"bass",ctx->sound.imm_rel[0]);
    md_var_set(vc,"mid",ctx->sound.imm_rel[1]);
    md_var_set(vc,"treb",ctx->sound.imm_rel[2]);
    md_var_set(vc,"bass_att",ctx->sound.avg_rel[0]);
    md_var_set(vc,"mid_att",ctx->sound.avg_rel[1]);
    md_var_set(vc,"treb_att",ctx->sound.avg_rel[2]);
    md_var_set(vc,"meshx",(float)MESH_W);
    md_var_set(vc,"meshy",(float)MESH_H);
    md_var_set(vc,"pixelsx",(float)ctx->gl.tex_w);
    md_var_set(vc,"pixelsy",(float)ctx->gl.tex_h);
    md_var_set(vc,"aspectx",(float)ctx->gl.tex_w/ctx->gl.tex_h);
    md_var_set(vc,"aspecty",1);
    md_var_set(vc,"zoom",p->zoom); md_var_set(vc,"zoomexp",p->fZoomExponent);
    md_var_set(vc,"rot",p->rot); md_var_set(vc,"warp",p->warp);
    md_var_set(vc,"cx",p->cx); md_var_set(vc,"cy",p->cy);
    md_var_set(vc,"dx",p->dx); md_var_set(vc,"dy",p->dy);
    md_var_set(vc,"sx",p->sx); md_var_set(vc,"sy",p->sy);
    md_var_set(vc,"decay",p->fDecay);
    md_var_set(vc,"wave_a",p->fWaveAlpha);
    md_var_set(vc,"wave_r",p->wave_r); md_var_set(vc,"wave_g",p->wave_g);
    md_var_set(vc,"wave_b",p->wave_b);
    md_var_set(vc,"wave_x",p->wave_x); md_var_set(vc,"wave_y",p->wave_y);
    md_var_set(vc,"wave_mode",(float)p->nWaveMode);
    md_var_set(vc,"wave_mystery",p->fWaveParam);
    md_var_set(vc,"wave_usedots",(float)p->bWaveDots);
    md_var_set(vc,"wave_thick",(float)p->bWaveThick);
    md_var_set(vc,"wave_additive",(float)p->bAdditiveWaves);
    md_var_set(vc,"wave_brighten",(float)p->bMaximizeWaveColor);
    md_var_set(vc,"echo_zoom",p->fVideoEchoZoom);
    md_var_set(vc,"echo_alpha",p->fVideoEchoAlpha);
    md_var_set(vc,"echo_orient",(float)p->nVideoEchoOrientation);
    md_var_set(vc,"gamma",p->fGammaAdj);
    md_var_set(vc,"darken_center",(float)p->bDarkenCenter);
    md_var_set(vc,"wrap",(float)p->bTexWrap);
    md_var_set(vc,"invert",(float)p->bInvert);
    md_var_set(vc,"brighten",(float)p->bBrighten);
    md_var_set(vc,"darken",(float)p->bDarken);
    md_var_set(vc,"solarize",(float)p->bSolarize);
    md_var_set(vc,"ob_size",p->ob_size);
    md_var_set(vc,"ob_r",p->ob_r); md_var_set(vc,"ob_g",p->ob_g);
    md_var_set(vc,"ob_b",p->ob_b); md_var_set(vc,"ob_a",p->ob_a);
    md_var_set(vc,"ib_size",p->ib_size);
    md_var_set(vc,"ib_r",p->ib_r); md_var_set(vc,"ib_g",p->ib_g);
    md_var_set(vc,"ib_b",p->ib_b); md_var_set(vc,"ib_a",p->ib_a);
    md_var_set(vc,"mv_x",p->nMotionVectorsX);
    md_var_set(vc,"mv_y",p->nMotionVectorsY);
    md_var_set(vc,"mv_dx",p->mv_dx); md_var_set(vc,"mv_dy",p->mv_dy);
    md_var_set(vc,"mv_l",p->mv_l);
    md_var_set(vc,"mv_r",p->mv_r); md_var_set(vc,"mv_g",p->mv_g);
    md_var_set(vc,"mv_b",p->mv_b); md_var_set(vc,"mv_a",p->mv_a);
    for (int i=0; i<NUM_Q_VAR; i++) {
        char nm[8]; snprintf(nm,sizeof(nm),"q%d",i+1);
        md_var_set(vc, nm, ctx->q_vars[i]);
    }
}

static void md_readback_pf_vars(md_ctx_t *ctx)
{
    for (int i=0; i<NUM_Q_VAR; i++) {
        char nm[8]; snprintf(nm,sizeof(nm),"q%d",i+1);
        ctx->q_vars[i] = md_var_read(&ctx->pf_vars, nm);
    }
}

/* ── beat detection ──────────────────────────────────────────── */

static int md_detect_beat(md_ctx_t *ctx)
{
    if (--ctx->beat_holdoff > 0) return 0;
    if (ctx->sound.imm_rel[0] > 2.2f) { ctx->beat_holdoff = 30; return 1; }
    return 0;
}

static void show_info(md_ctx_t *ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    vsnprintf(ctx->info_text, sizeof(ctx->info_text), fmt, ap);
    va_end(ap); ctx->info_frames = 180;
}

static void md_load_preset_idx(md_ctx_t *ctx, int idx, int blend)
{
    if (!ctx->lib.count) return;
    idx = ((idx%ctx->lib.count)+ctx->lib.count)%ctx->lib.count;
    md_preset_t *tgt = blend ? &ctx->next_preset : &ctx->preset;
    if (md_load_preset(tgt, ctx->lib.paths[idx])) {
        idx = (idx+1)%ctx->lib.count;
        if (md_load_preset(tgt, ctx->lib.paths[idx])) return;
    }
    ctx->preset_idx = idx;
    if (blend) {
        ctx->blending=1; ctx->blend_progress=0; ctx->blend_duration=2.5f;
    } else {
        ctx->preset_loaded=1; ctx->pf_init_done=0;
        memset(ctx->q_vars,0,sizeof(ctx->q_vars));
    }
}

/* ══════════════════════════════════════════════════════════════
 *  GL RENDERING
 * ══════════════════════════════════════════════════════════════ */

static void gl_draw_fullscreen_quad(md_ctx_t *ctx)
{
    glBindVertexArray(ctx->gl.quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

/* Build the warp mesh: compute per-vertex warped UVs on CPU,
 * upload as vertex data, GPU does the textured draw + decay. */
static void gl_warp_blit(md_ctx_t *ctx, md_preset_t *p)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    float zoom  = md_var_read(vc,"zoom");
    float zexp  = md_var_read(vc,"zoomexp");
    float rot   = md_var_read(vc,"rot");
    float wrp   = md_var_read(vc,"warp");
    float cx    = md_var_read(vc,"cx");
    float cy    = md_var_read(vc,"cy");
    float d_x   = md_var_read(vc,"dx");
    float d_y   = md_var_read(vc,"dy");
    float s_x   = md_var_read(vc,"sx");
    float s_y   = md_var_read(vc,"sy");
    float decay = md_var_read(vc,"decay");
    /* Clamp decay to prevent energy accumulation (white-out).
     * Values above 1.0 from per-frame code would amplify the
     * feedback loop.  Cap at 1.0 and floor at 0. */
    if (decay > 1.0f) decay = 1.0f;
    if (decay < 0.0f) decay = 0.0f;
    int   wrap  = (int)md_var_read(vc,"wrap");

    float wt = ctx->warp_time;
    float ws_inv = 1.0f / (p->fWarpScale + 0.01f);
    float f[4];
    f[0]=11.68f+4*cosf(wt*1.413f+10); f[1]=8.77f+3*cosf(wt*1.113f+7);
    f[2]=10.54f+3*cosf(wt*0.786f+3);  f[3]=11.49f+4*cosf(wt*0.893f+5);
    float ax = 1, ay = (float)ctx->gl.tex_w/ctx->gl.tex_h;

    int has_pp = p->per_pixel_count > 0;
    char pp_code[MAX_EXPR_LINES * MAX_LINE_LEN];
    if (has_pp) md_concat_lines(p->per_pixel, p->per_pixel_count, pp_code, sizeof(pp_code));

    /* Compute warped UV for each mesh vertex */
    int nv = (MESH_W+1)*(MESH_H+1);
    float *verts = malloc(nv * 4 * sizeof(float)); /* x,y, u,v per vertex */

    for (int my = 0; my <= MESH_H; my++) {
        for (int mx = 0; mx <= MESH_W; mx++) {
            float nx = (float)mx / MESH_W;
            float ny = (float)my / MESH_H;

            /* per-pixel vars */
            float pz=zoom, pe=zexp, pr=rot, pw=wrp;
            float pcx=cx, pcy=cy, pdx=d_x, pdy=d_y, psx=s_x, psy=s_y;

            if (has_pp) {
                float x2=nx*2-1, y2=ny*2-1;
                float rad = sqrtf(x2*x2*ax*ax + y2*y2*ay*ay);
                float ang = atan2f(y2*ay, x2*ax);
                md_var_ctx_t pp_vc;
                memcpy(&pp_vc, vc, sizeof(md_var_ctx_t));
                md_var_set(&pp_vc,"x",nx); md_var_set(&pp_vc,"y",ny);
                md_var_set(&pp_vc,"rad",rad); md_var_set(&pp_vc,"ang",ang);
                md_eval(pp_code, &pp_vc);
                pz=md_var_read(&pp_vc,"zoom"); pe=md_var_read(&pp_vc,"zoomexp");
                pr=md_var_read(&pp_vc,"rot");  pw=md_var_read(&pp_vc,"warp");
                pcx=md_var_read(&pp_vc,"cx");  pcy=md_var_read(&pp_vc,"cy");
                pdx=md_var_read(&pp_vc,"dx");  pdy=md_var_read(&pp_vc,"dy");
                psx=md_var_read(&pp_vc,"sx");  psy=md_var_read(&pp_vc,"sy");
            }

            float x = nx*2-1, y = ny*2-1;
            float rad = sqrtf(x*x*ax*ax + y*y*ay*ay);
            float fz = powf(pz, powf(pe, rad*2-1));
            float u =  x*ax*0.5f/fz + 0.5f;
            float v = -y*ay*0.5f/fz + 0.5f;
            u = (u-pcx)/psx + pcx;
            v = (v-pcy)/psy + pcy;
            float ws2 = pw*0.0035f;
            u += ws2*sinf(wt*0.333f + ws_inv*(x*f[0]-y*f[3]));
            v += ws2*cosf(wt*0.375f - ws_inv*(x*f[2]+y*f[1]));
            u += ws2*cosf(wt*0.753f - ws_inv*(x*f[1]-y*f[2]));
            v += ws2*sinf(wt*0.825f + ws_inv*(x*f[0]+y*f[3]));
            float u2=u-pcx, v2=v-pcy;
            float cr2=cosf(pr), sr2=sinf(pr);
            u = u2*cr2-v2*sr2+pcx;
            v = u2*sr2+v2*cr2+pcy;
            u -= pdx; v -= pdy;

            int idx = (my*(MESH_W+1)+mx)*4;
            verts[idx+0] = nx;   /* screen position */
            verts[idx+1] = ny;
            verts[idx+2] = u;    /* warped UV */
            verts[idx+3] = v;
        }
    }

    /* upload and draw */
    GLuint src_tex = ctx->gl.fbo_tex[1 - ctx->cur_fbo];
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->gl.fbo[ctx->cur_fbo]);
    glViewport(0, 0, ctx->gl.tex_w, ctx->gl.tex_h);

    glUseProgram(ctx->gl.prog_warp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    GLint wrapMode = wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_warp, "uTex"), 0);
    glUniform1f(glGetUniformLocation(ctx->gl.prog_warp, "uDecay"),
                fminf(fmaxf(decay, 0.0f), 1.0f));

    glBindVertexArray(ctx->gl.mesh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->gl.mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, nv*4*sizeof(float), verts, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                          (void*)(2*sizeof(float)));
    /* re-bind EBO so glDrawElements can find the indices */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->gl.mesh_ebo);

    glDrawElements(GL_TRIANGLES, ctx->gl.mesh_index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    free(verts);
}

/* Draw darken-center effect */
static void gl_darken_center(md_ctx_t *ctx)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ctx->gl.prog_darken);
    glUniform1f(glGetUniformLocation(ctx->gl.prog_darken, "uAspect"),
                (float)ctx->gl.tex_w/ctx->gl.tex_h);
    gl_draw_fullscreen_quad(ctx);
    glDisable(GL_BLEND);
}

/* Upload wave/shape vertex data and draw */
typedef struct { float x, y, r, g, b, a; } color_vert_t;

static void gl_draw_colored_lines(md_ctx_t *ctx, color_vert_t *v, int count,
                                   GLenum mode, float width, int additive)
{
    if (count < 2 && mode != GL_POINTS) return;
    glEnable(GL_BLEND);
    if (additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else          glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(width);
    glUseProgram(ctx->gl.prog_color);
    glBindVertexArray(ctx->gl.wave_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->gl.wave_vbo);
    glBufferData(GL_ARRAY_BUFFER, count*sizeof(color_vert_t), v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(color_vert_t), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(color_vert_t),
                          (void*)(2*sizeof(float)));
    glDrawArrays(mode, 0, count);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

/* Draw a filled polygon with gradient (for custom shapes) */
static void gl_draw_shape(md_ctx_t *ctx, float sx, float sy, float sr, float sa,
                           int sides, float r1, float g1, float b1, float a1,
                           float r2, float g2, float b2, float a2,
                           float br, float bg, float bb, float ba,
                           int additive, int thick)
{
    if (sides < 3) sides = 3;
    if (sides > 100) sides = 100;
    float w = (float)ctx->gl.tex_w, h = (float)ctx->gl.tex_h;
    float px = sx/w, py = sy/h;
    float rx = sr/w, ry = sr/h;

    /* fill: triangle fan with center + rim */
    int nv = sides + 2;
    color_vert_t *fv = malloc(nv * sizeof(color_vert_t));
    fv[0] = (color_vert_t){px, py, r1, g1, b1, fminf(a1,1)};
    for (int i = 0; i <= sides; i++) {
        float angle = sa + (float)i/sides * 2*(float)M_PI;
        fv[i+1] = (color_vert_t){
            px + rx*cosf(angle), py + ry*sinf(angle),
            r2, g2, b2, fminf(a2,1)
        };
    }

    glEnable(GL_BLEND);
    if (additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else          glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ctx->gl.prog_color);
    glBindVertexArray(ctx->gl.wave_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->gl.wave_vbo);
    glBufferData(GL_ARRAY_BUFFER, nv*sizeof(color_vert_t), fv, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(color_vert_t), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(color_vert_t),
                          (void*)(2*sizeof(float)));
    glDrawArrays(GL_TRIANGLE_FAN, 0, nv);
    free(fv);

    /* border */
    if (ba > 0.001f) {
        int nb = sides + 1;
        color_vert_t *bv = malloc(nb * sizeof(color_vert_t));
        for (int i = 0; i <= sides; i++) {
            float angle = sa + (float)(i%sides)/sides * 2*(float)M_PI;
            bv[i] = (color_vert_t){
                px + rx*cosf(angle), py + ry*sinf(angle),
                br, bg, bb, fminf(ba,1)
            };
        }
        glLineWidth(thick?2.5f:1.0f);
        glBufferData(GL_ARRAY_BUFFER, nb*sizeof(color_vert_t), bv, GL_STREAM_DRAW);
        glDrawArrays(GL_LINE_STRIP, 0, nb);
        free(bv);
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

/* ── built-in waveform drawing ───────────────────────────────── */

static void gl_draw_builtin_wave(md_ctx_t *ctx)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    md_sound_t *snd = &ctx->sound;
    float wa = md_var_read(vc,"wave_a");
    if (wa < 0.001f) return;

    int mode = (int)md_var_read(vc,"wave_mode") % NUM_WAVE_MODES;
    float wr = md_var_read(vc,"wave_r");
    float wg = md_var_read(vc,"wave_g");
    float wb = md_var_read(vc,"wave_b");
    float ws = ctx->preset.fWaveScale * 0.5f;
    int thick = (int)md_var_read(vc,"wave_thick");
    int add   = (int)md_var_read(vc,"wave_additive");

    if ((int)md_var_read(vc,"wave_brighten")) {
        float mx = fmaxf(wr,fmaxf(wg,wb));
        if (mx>0.01f) { wr/=mx; wg/=mx; wb/=mx; }
    }
    if (ctx->preset.bModWaveAlphaByVolume) {
        float vol = (snd->imm[0]+snd->imm[1]+snd->imm[2])*0.33f;
        float vr = vol/(snd->long_avg[0]+0.001f);
        float am = (vr-ctx->preset.fModWaveAlphaStart) /
                   (ctx->preset.fModWaveAlphaEnd-ctx->preset.fModWaveAlphaStart+0.001f);
        wa *= fminf(fmaxf(am,0),1);
    }
    /* Clamp wave colors and alpha to prevent additive white-out */
    wr = fminf(fmaxf(wr,0),1); wg = fminf(fmaxf(wg,0),1);
    wb = fminf(fmaxf(wb,0),1); wa = fminf(fmaxf(wa,0),1);

    float W=(float)ctx->gl.tex_w, H=(float)ctx->gl.tex_h;
    float cx_w = md_var_read(vc,"wave_x");
    float cy_w = 1.0f - md_var_read(vc,"wave_y");
    int n = MD_WAVEFORM_N;

    color_vert_t verts[MAX_WAVE_VERTS];
    int nv = 0;

    switch (mode) {
    case 0: { /* circular */
        float br = 0.25f;
        for (int i=0; i<=n && nv<MAX_WAVE_VERTS; i++) {
            int idx=i%n;
            float angle=(float)idx/n*2*(float)M_PI;
            float s=(snd->wave[0][idx]+snd->wave[1][idx])*0.5f;
            float r=br*(1+s*ws);
            float px=cx_w+r*cosf(angle)*(H/W), py=cy_w+r*sinf(angle);
            verts[nv++]=(color_vert_t){px,py,wr,wg,wb,fminf(wa,1)};
        }
    } break;
    case 1: { /* XY oscilloscope */
        for (int i=0; i<n && nv<MAX_WAVE_VERTS; i++) {
            float ang=snd->wave[0][i]*(float)M_PI*ws;
            float rad=fmaxf(0,0.2f+snd->wave[1][i]*ws*0.5f);
            float px=cx_w+rad*cosf(ang)*0.3f*(H/W);
            float py=cy_w+rad*sinf(ang)*0.3f;
            verts[nv++]=(color_vert_t){px,py,wr,wg,wb,fminf(wa,1)};
        }
    } break;
    case 2: { /* Lissajous */
        for (int i=0; i<n && nv<MAX_WAVE_VERTS; i++) {
            float px=cx_w+snd->wave[0][i]*ws*0.4f*(H/W);
            float py=cy_w+snd->wave[1][i]*ws*0.4f;
            verts[nv++]=(color_vert_t){px,py,wr,wg,wb,fminf(wa,1)};
        }
    } break;
    case 3: { /* volume spiro */
        float trsq=snd->avg_rel[2]*snd->avg_rel[2];
        float a2=wa*fminf(trsq*0.5f,1);
        for (int i=0; i<n && nv<MAX_WAVE_VERTS; i++) {
            float px=cx_w+snd->wave[0][i]*ws*0.4f*(H/W);
            float py=cy_w+snd->wave[1][i]*ws*0.4f;
            verts[nv++]=(color_vert_t){px,py,wr,wg,wb,fminf(a2,1)};
        }
    } break;
    case 4: { /* horizontal */
        for (int i=0; i<n && nv<MAX_WAVE_VERTS; i++) {
            float px=(float)i/(n-1);
            float s=(snd->wave[0][i]+snd->wave[1][i])*0.5f;
            float py=cy_w+s*ws*0.5f;
            verts[nv++]=(color_vert_t){px,py,wr,wg,wb,fminf(wa,1)};
        }
    } break;
    case 5: { /* explosive */
        float t=ctx->warp_time*0.5f;
        for (int i=0; i<n && nv<MAX_WAVE_VERTS; i++) {
            float fr=(float)i/(n-1);
            float angle=fr*(float)M_PI*6+t;
            float s=(snd->wave[0][i]+snd->wave[1][i])*0.5f;
            float r=(0.3f+0.3f*sinf(fr*(float)M_PI*2+t))+s*ws*0.3f;
            float px=cx_w+r*cosf(angle)*0.35f*(H/W);
            float py=cy_w+r*sinf(angle)*0.35f;
            verts[nv++]=(color_vert_t){px,py,wr,wg,wb,fminf(wa,1)};
        }
    } break;
    case 6: { /* dual */
        float yo=0.25f;
        for (int i=0; i<n && nv<MAX_WAVE_VERTS; i++) {
            float px=(float)i/(n-1);
            float py=cy_w-yo+snd->wave[0][i]*ws*0.3f;
            verts[nv++]=(color_vert_t){px,py,wr,wg,wb,fminf(wa,1)};
        }
        gl_draw_colored_lines(ctx, verts, nv,
                              GL_LINE_STRIP, thick?2.5f:1.2f, add);
        nv=0;
        for (int i=0; i<n && nv<MAX_WAVE_VERTS; i++) {
            float px=(float)i/(n-1);
            float py=cy_w+yo+snd->wave[1][i]*ws*0.3f;
            verts[nv++]=(color_vert_t){px,py,wr,wg,wb,fminf(wa,1)};
        }
    } break;
    case 7: { /* spectrum bars */
        int bins=128;
        for (int i=0; i<bins && i<MD_FFT_HALF; i++) {
            float mag=fminf(snd->spec[i]*ws*20, 1);
            float fr=(float)i/bins;
            float x0=(float)i/bins, x1=(float)(i+1)/bins;
            float y0=1.0f, y1=1.0f-mag*0.8f;
            float cr=1-fr, cg=sinf(fr*(float)M_PI), cb=fr;
            /* two triangles per bar */
            if (nv+6 <= MAX_WAVE_VERTS) {
                verts[nv++]=(color_vert_t){x0,y0,cr,cg,cb,fminf(wa,1)};
                verts[nv++]=(color_vert_t){x1,y0,cr,cg,cb,fminf(wa,1)};
                verts[nv++]=(color_vert_t){x0,y1,cr,cg,cb,fminf(wa,1)};
                verts[nv++]=(color_vert_t){x0,y1,cr,cg,cb,fminf(wa,1)};
                verts[nv++]=(color_vert_t){x1,y0,cr,cg,cb,fminf(wa,1)};
                verts[nv++]=(color_vert_t){x1,y1,cr,cg,cb,fminf(wa,1)};
            }
        }
        gl_draw_colored_lines(ctx, verts, nv, GL_TRIANGLES, 1, add);
        return;
    }
    }
    gl_draw_colored_lines(ctx, verts, nv, GL_LINE_STRIP, thick?2.5f:1.2f, add);
}

/* ── custom waves ────────────────────────────────────────────── */

static void gl_draw_custom_waves(md_ctx_t *ctx)
{
    md_sound_t *snd = &ctx->sound;
    for (int wi=0; wi<MAX_CUSTOM_WAVES; wi++) {
        md_custom_wave_t *cw = &ctx->preset.waves[wi];
        if (!cw->enabled || !cw->per_point_count) continue;
        char pp_code[MAX_EXPR_LINES*MAX_LINE_LEN];
        md_concat_lines(cw->per_point, cw->per_point_count, pp_code, sizeof(pp_code));
        if (!pp_code[0]) continue;
        int samples=cw->samples;
        if (samples<2) samples=2;
        if (samples>MD_WAVEFORM_N) samples=MD_WAVEFORM_N;
        char pf_code[MAX_EXPR_LINES*MAX_LINE_LEN];
        md_concat_lines(cw->per_frame, cw->per_frame_count, pf_code, sizeof(pf_code));
        md_var_ctx_t wvc;
        memcpy(&wvc, &ctx->pf_vars, sizeof(md_var_ctx_t));
        md_var_set(&wvc,"r",cw->r); md_var_set(&wvc,"g",cw->g);
        md_var_set(&wvc,"b",cw->b); md_var_set(&wvc,"a",cw->a);
        md_var_set(&wvc,"samples",(float)samples);
        if (pf_code[0]) md_eval(pf_code, &wvc);
        float br=md_var_read(&wvc,"r"), bg=md_var_read(&wvc,"g");
        float bb=md_var_read(&wvc,"b"), ba=md_var_read(&wvc,"a");
        samples=(int)md_var_read(&wvc,"samples");
        if (samples<2) samples=2;
        if (samples>MD_WAVEFORM_N) samples=MD_WAVEFORM_N;

        color_vert_t verts[MAX_WAVE_VERTS];
        int nv=0;
        for (int i=0; i<samples && nv<MAX_WAVE_VERTS; i++) {
            md_var_ctx_t ppv;
            memcpy(&ppv, &wvc, sizeof(md_var_ctx_t));
            float sf=(float)i/(samples-1);
            float v1 = cw->bSpectrum ? snd->spec[i%MD_FFT_HALF]*cw->scaling
                                     : snd->wave[0][i%MD_WAVEFORM_N]*cw->scaling;
            float v2 = cw->bSpectrum ? snd->spec[i%MD_FFT_HALF]*cw->scaling
                                     : snd->wave[1][i%MD_WAVEFORM_N]*cw->scaling;
            md_var_set(&ppv,"sample",sf);
            md_var_set(&ppv,"value1",v1); md_var_set(&ppv,"value2",v2);
            md_var_set(&ppv,"x",0.5f+v1*0.5f); md_var_set(&ppv,"y",0.5f+v2*0.5f);
            md_var_set(&ppv,"r",br); md_var_set(&ppv,"g",bg);
            md_var_set(&ppv,"b",bb); md_var_set(&ppv,"a",ba);
            md_eval(pp_code, &ppv);
            float px=md_var_read(&ppv,"x"), py=1.0f-md_var_read(&ppv,"y");
            verts[nv++]=(color_vert_t){
                px, py,
                md_var_read(&ppv,"r"), md_var_read(&ppv,"g"),
                md_var_read(&ppv,"b"), fminf(md_var_read(&ppv,"a"),1)
            };
        }
        GLenum mode = cw->bUseDots ? GL_POINTS : GL_LINE_STRIP;
        gl_draw_colored_lines(ctx, verts, nv, mode,
                              cw->bDrawThick?2.5f:1.2f, cw->bAdditive);
    }
}

/* ── custom shapes ───────────────────────────────────────────── */

static void gl_draw_custom_shapes(md_ctx_t *ctx)
{
    for (int si=0; si<MAX_CUSTOM_SHAPES; si++) {
        md_custom_shape_t *cs = &ctx->preset.shapes[si];
        if (!cs->enabled) continue;
        char pf_code[MAX_EXPR_LINES*MAX_LINE_LEN];
        md_concat_lines(cs->per_frame, cs->per_frame_count, pf_code, sizeof(pf_code));
        int inst=cs->num_inst;
        if (inst<1) inst=1;
        if (inst>1024) inst=1024;
        for (int ii=0; ii<inst; ii++) {
            md_var_ctx_t svc;
            memcpy(&svc, &ctx->pf_vars, sizeof(md_var_ctx_t));
            md_var_set(&svc,"x",cs->x); md_var_set(&svc,"y",cs->y);
            md_var_set(&svc,"rad",cs->rad); md_var_set(&svc,"ang",cs->ang);
            md_var_set(&svc,"sides",(float)cs->sides);
            md_var_set(&svc,"r",cs->r); md_var_set(&svc,"g",cs->g);
            md_var_set(&svc,"b",cs->b); md_var_set(&svc,"a",cs->a);
            md_var_set(&svc,"r2",cs->r2); md_var_set(&svc,"g2",cs->g2);
            md_var_set(&svc,"b2",cs->b2); md_var_set(&svc,"a2",cs->a2);
            md_var_set(&svc,"border_r",cs->border_r);
            md_var_set(&svc,"border_g",cs->border_g);
            md_var_set(&svc,"border_b",cs->border_b);
            md_var_set(&svc,"border_a",cs->border_a);
            md_var_set(&svc,"additive",(float)cs->additive);
            md_var_set(&svc,"thick",(float)cs->thickOutline);
            md_var_set(&svc,"instance",(float)ii);
            md_var_set(&svc,"instances",(float)inst);
            if (pf_code[0]) md_eval(pf_code, &svc);
            float sx=md_var_read(&svc,"x")*(float)ctx->gl.tex_w;
            float sy=(1-md_var_read(&svc,"y"))*(float)ctx->gl.tex_h;
            float sr=md_var_read(&svc,"rad")*fminf(ctx->gl.tex_w,ctx->gl.tex_h)*0.5f;
            float sa=md_var_read(&svc,"ang");
            int sides=(int)md_var_read(&svc,"sides");
            gl_draw_shape(ctx, sx, sy, sr, sa, sides,
                md_var_read(&svc,"r"), md_var_read(&svc,"g"),
                md_var_read(&svc,"b"), md_var_read(&svc,"a"),
                md_var_read(&svc,"r2"), md_var_read(&svc,"g2"),
                md_var_read(&svc,"b2"), md_var_read(&svc,"a2"),
                md_var_read(&svc,"border_r"), md_var_read(&svc,"border_g"),
                md_var_read(&svc,"border_b"), md_var_read(&svc,"border_a"),
                (int)md_var_read(&svc,"additive"),
                (int)md_var_read(&svc,"thick"));
        }
    }
}

/* ── borders ─────────────────────────────────────────────────── */

static void gl_draw_borders(md_ctx_t *ctx)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    float oba = md_var_read(vc,"ob_a");
    if (oba > 0.001f) {
        float s = md_var_read(vc,"ob_size");
        color_vert_t bv[5];
        float cr=md_var_read(vc,"ob_r"), cg=md_var_read(vc,"ob_g"),
              cb=md_var_read(vc,"ob_b");
        bv[0]=(color_vert_t){0,0,cr,cg,cb,oba};
        bv[1]=(color_vert_t){1,0,cr,cg,cb,oba};
        bv[2]=(color_vert_t){1,1,cr,cg,cb,oba};
        bv[3]=(color_vert_t){0,1,cr,cg,cb,oba};
        bv[4]=bv[0];
        gl_draw_colored_lines(ctx, bv, 5, GL_LINE_STRIP,
                              s*fminf(ctx->gl.tex_w,ctx->gl.tex_h)*2, 0);
    }
    float iba = md_var_read(vc,"ib_a");
    if (iba > 0.001f) {
        float obs = md_var_read(vc,"ob_size");
        float ibs = md_var_read(vc,"ib_size");
        float inset = obs*2+ibs;
        float cr=md_var_read(vc,"ib_r"), cg=md_var_read(vc,"ib_g"),
              cb=md_var_read(vc,"ib_b");
        color_vert_t bv[5];
        bv[0]=(color_vert_t){inset,inset,cr,cg,cb,iba};
        bv[1]=(color_vert_t){1-inset,inset,cr,cg,cb,iba};
        bv[2]=(color_vert_t){1-inset,1-inset,cr,cg,cb,iba};
        bv[3]=(color_vert_t){inset,1-inset,cr,cg,cb,iba};
        bv[4]=bv[0];
        gl_draw_colored_lines(ctx, bv, 5, GL_LINE_STRIP,
                              ibs*fminf(ctx->gl.tex_w,ctx->gl.tex_h)*2, 0);
    }
}

/* ── motion vectors ──────────────────────────────────────────── */

static void gl_draw_motion_vectors(md_ctx_t *ctx)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    float ma=md_var_read(vc,"mv_a"), ml=md_var_read(vc,"mv_l");
    if (ma<0.001f || ml<0.001f) return;
    int nvx=(int)md_var_read(vc,"mv_x"), nvy=(int)md_var_read(vc,"mv_y");
    if (nvx<=0||nvy<=0) return;
    float cr=md_var_read(vc,"mv_r"), cg=md_var_read(vc,"mv_g"),
          cb=md_var_read(vc,"mv_b");
    float len=ml*0.025f;
    float mvdx=md_var_read(vc,"mv_dx"), mvdy=md_var_read(vc,"mv_dy");
    color_vert_t verts[MAX_WAVE_VERTS];
    int nv=0;
    for (int y=0; y<nvy; y++)
        for (int x=0; x<nvx; x++) {
            if (nv+2>MAX_WAVE_VERTS) break;
            float fx=(x+0.5f)/nvx, fy=(y+0.5f)/nvy;
            verts[nv++]=(color_vert_t){fx,fy,cr,cg,cb,ma};
            verts[nv++]=(color_vert_t){fx+mvdx*len+len,fy+mvdy*len,cr,cg,cb,ma};
        }
    gl_draw_colored_lines(ctx, verts, nv, GL_LINES, fminf(ml,1), 0);
}

/* ── video echo pass ─────────────────────────────────────────── */

static void gl_video_echo(md_ctx_t *ctx)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    float ea = md_var_read(vc,"echo_alpha");
    if (ea < 0.01f) return;
    float ez = md_var_read(vc,"echo_zoom");
    int eo = (int)md_var_read(vc,"echo_orient");
    /* Clamp echo alpha to prevent energy accumulation */
    if (ea > 1.0f) ea = 1.0f;

    /* We need to read from the current in-progress FBO but also
     * write to it.  Copy current → other, then blend other back
     * onto current with the echo transform.  This matches real
     * MilkDrop where echo operates on the current composite. */
    int other = 1 - ctx->cur_fbo;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->gl.fbo[ctx->cur_fbo]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->gl.fbo[other]);
    glBlitFramebuffer(0,0,ctx->gl.tex_w,ctx->gl.tex_h,
                      0,0,ctx->gl.tex_w,ctx->gl.tex_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    /* Now blend the copy (other) back onto current with echo params */
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->gl.fbo[ctx->cur_fbo]);
    glViewport(0,0,ctx->gl.tex_w,ctx->gl.tex_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ctx->gl.prog_echo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->gl.fbo_tex[other]);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_echo, "uTex"), 0);
    glUniform1f(glGetUniformLocation(ctx->gl.prog_echo, "uAlpha"), ea);
    glUniform1f(glGetUniformLocation(ctx->gl.prog_echo, "uZoom"), ez);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_echo, "uOrient"), eo);
    gl_draw_fullscreen_quad(ctx);
    glDisable(GL_BLEND);
}

/* ── post-process pass ───────────────────────────────────────── */

static void gl_post_process(md_ctx_t *ctx)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    float gam = md_var_read(vc,"gamma");
    int br=(int)md_var_read(vc,"brighten"), dk=(int)md_var_read(vc,"darken");
    int sol=(int)md_var_read(vc,"solarize"), inv=(int)md_var_read(vc,"invert");
    if (!br && !dk && !sol && !inv && fabsf(gam-1.0f)<0.01f) return;

    /* We need to read from current FBO and write back to it.
     * Blit current → other, then post-process other → current. */
    int other = 1 - ctx->cur_fbo;

    /* copy current to other */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->gl.fbo[ctx->cur_fbo]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->gl.fbo[other]);
    glBlitFramebuffer(0,0,ctx->gl.tex_w,ctx->gl.tex_h,
                      0,0,ctx->gl.tex_w,ctx->gl.tex_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    /* now post-process from other → current */
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->gl.fbo[ctx->cur_fbo]);
    glViewport(0,0,ctx->gl.tex_w,ctx->gl.tex_h);
    glUseProgram(ctx->gl.prog_post);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->gl.fbo_tex[other]);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_post,"uTex"),0);
    glUniform1f(glGetUniformLocation(ctx->gl.prog_post,"uGamma"),gam);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_post,"uBrighten"),br);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_post,"uDarken"),dk);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_post,"uSolarize"),sol);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_post,"uInvert"),inv);
    gl_draw_fullscreen_quad(ctx);
}

/* ── main render frame ───────────────────────────────────────── */

static void md_render_frame(md_ctx_t *ctx)
{
    md_preset_t *p = &ctx->preset;
    ctx->warp_time  += ctx->frame_time * p->fWarpAnimSpeed;
    ctx->total_time += ctx->frame_time;
    ctx->frame_count++;

    /* FPS tracking */
    ctx->fps_accum += ctx->frame_time;
    ctx->fps_frames++;
    if (ctx->fps_accum >= 1.0f) {
        ctx->fps_display = (float)ctx->fps_frames / ctx->fps_accum;
        ctx->fps_accum = 0; ctx->fps_frames = 0;
    }

    if (ctx->blending) {
        float bd = ctx->blend_time_user > 0.01f ? ctx->blend_time_user
                                                  : ctx->blend_duration;
        ctx->blend_progress += ctx->frame_time / bd;
        if (ctx->blend_progress >= 1) {
            ctx->blending=0; ctx->preset=ctx->next_preset;
            ctx->pf_init_done=0; memset(ctx->q_vars,0,sizeof(ctx->q_vars));
        }
    }

    /* auto-advance: beat-driven or timer-driven */
    if (!ctx->locked) {
        if (ctx->auto_preset && md_detect_beat(ctx))
            md_load_preset_idx(ctx, ctx->preset_idx+1, 1);
        if (ctx->preset_duration > 0 && !ctx->auto_preset) {
            ctx->preset_timer += ctx->frame_time;
            if (ctx->preset_timer >= ctx->preset_duration) {
                ctx->preset_timer = 0;
                ctx->prev_preset_idx = ctx->preset_idx;
                md_load_preset_idx(ctx, ctx->preset_idx+1, 1);
            }
        }
    }

    md_setup_pf_vars(ctx, p);

    if (!ctx->pf_init_done && p->per_frame_init_count>0) {
        char code[MAX_EXPR_LINES*MAX_LINE_LEN];
        md_concat_lines(p->per_frame_init, p->per_frame_init_count, code, sizeof(code));
        md_eval(code, &ctx->pf_vars);
        md_readback_pf_vars(ctx); ctx->pf_init_done=1;
    }
    if (p->per_frame_count>0) {
        char code[MAX_EXPR_LINES*MAX_LINE_LEN];
        md_concat_lines(p->per_frame, p->per_frame_count, code, sizeof(code));
        md_eval(code, &ctx->pf_vars);
        md_readback_pf_vars(ctx);
    }

    /* 1. warp blit (renders into cur_fbo from 1-cur_fbo) */
    gl_warp_blit(ctx, p);

    if (ctx->effects_enabled) {
        /* 2. darken center */
        if ((int)md_var_read(&ctx->pf_vars,"darken_center"))
            gl_darken_center(ctx);

        /* 3. custom shapes */
        gl_draw_custom_shapes(ctx);

        /* 4. built-in wave */
        gl_draw_builtin_wave(ctx);

        /* 5. custom waves */
        gl_draw_custom_waves(ctx);

        /* 6. borders + motion vectors */
        gl_draw_borders(ctx);
        gl_draw_motion_vectors(ctx);

        /* 7. video echo */
        gl_video_echo(ctx);

        /* 8. post-process */
        gl_post_process(ctx);
    }

    /* 9. swap */
    ctx->cur_fbo = 1 - ctx->cur_fbo;
}

/* ══════════════════════════════════════════════════════════════
 *  GTK GL AREA CALLBACKS
 * ══════════════════════════════════════════════════════════════ */

static void gl_init_resources(md_ctx_t *ctx)
{
    md_gl_t *g = &ctx->gl;

    /* compile shaders */
    g->prog_warp   = gl_create_program(WARP_VS, WARP_FS);
    g->prog_post   = gl_create_program(QUAD_VS, POST_FS);
    g->prog_echo   = gl_create_program(QUAD_VS, ECHO_FS);
    g->prog_darken = gl_create_program(QUAD_VS, DARKEN_FS);
    g->prog_color  = gl_create_program(COLOR_VS, COLOR_FS);
    g->prog_blit   = gl_create_program(QUAD_VS, BLIT_FS);

    /* fullscreen quad VAO */
    float quad[] = {0,0, 1,0, 0,1, 1,1};
    glGenVertexArrays(1, &g->quad_vao);
    glGenBuffers(1, &g->quad_vbo);
    glBindVertexArray(g->quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindVertexArray(0);

    /* mesh VAO + EBO */
    glGenVertexArrays(1, &g->mesh_vao);
    glGenBuffers(1, &g->mesh_vbo);
    glGenBuffers(1, &g->mesh_ebo);

    /* generate mesh indices */
    int ni = MESH_W * MESH_H * 6;
    g->mesh_index_count = ni;
    GLuint *idx = malloc(ni * sizeof(GLuint));
    int ii = 0;
    for (int y=0; y<MESH_H; y++) {
        for (int x=0; x<MESH_W; x++) {
            int tl = y*(MESH_W+1)+x;
            int tr = tl+1;
            int bl = tl+(MESH_W+1);
            int br2 = bl+1;
            idx[ii++]=tl; idx[ii++]=bl; idx[ii++]=tr;
            idx[ii++]=tr; idx[ii++]=bl; idx[ii++]=br2;
        }
    }
    glBindVertexArray(g->mesh_vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->mesh_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni*sizeof(GLuint), idx, GL_STATIC_DRAW);
    glBindVertexArray(0);
    free(idx);

    /* wave VAO (dynamic) */
    glGenVertexArrays(1, &g->wave_vao);
    glGenBuffers(1, &g->wave_vbo);

    /* FBOs */
    glGenFramebuffers(2, g->fbo);
    glGenTextures(2, g->fbo_tex);
    g->tex_w = MD_TEX_W;
    g->tex_h = MD_TEX_H;
    for (int i=0; i<2; i++) {
        glBindTexture(GL_TEXTURE_2D, g->fbo_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g->tex_w, g->tex_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g->fbo_tex[i], 0);
        /* clear to black */
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Query the FBO that GtkGLArea set up for us */
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&g->gtk_fbo);
    fprintf(stderr,"milkdrop: GtkGLArea FBO = %u\n", g->gtk_fbo);

    g->gl_ready = 1;
    fprintf(stderr,"milkdrop: GL init OK  warp=%u post=%u echo=%u darken=%u color=%u blit=%u\n",
            g->prog_warp, g->prog_post, g->prog_echo, g->prog_darken,
            g->prog_color, g->prog_blit);
    GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    fprintf(stderr,"milkdrop: FBO status=0x%x (want 0x%x)\n", fbs, GL_FRAMEBUFFER_COMPLETE);
}

static gboolean redraw_tick(gpointer data)
{
    md_ctx_t *ctx = data;
    if (ctx->gl_area && gtk_widget_get_realized(ctx->gl_area))
        gtk_widget_queue_draw(ctx->gl_area);
    return G_SOURCE_CONTINUE;
}

static void on_realize(GtkGLArea *area, gpointer data)
{
    (void)area;
    md_ctx_t *ctx = data;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) {
        fprintf(stderr,"milkdrop: GtkGLArea error on realize\n");
        return;
    }
    gl_init_resources(ctx);
    /* 60 Hz redraw timer so rendering keeps going */
    g_timeout_add(16, redraw_tick, ctx);
}

static gboolean on_render(GtkGLArea *area, GdkGLContext *glctx, gpointer data)
{
    (void)glctx;
    md_ctx_t *ctx = data;
    if (!ctx->gl.gl_ready) return FALSE;

    int ww = gtk_widget_get_allocated_width(GTK_WIDGET(area));
    int hh = gtk_widget_get_allocated_height(GTK_WIDGET(area));
    if (ww<=0||hh<=0) return FALSE;

    /* GtkGLArea binds its own FBO before calling render — capture it */
    GLuint gtk_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&gtk_fbo);

    if (!ctx->running || !ctx->audio_node_count) {
        /* Just clear to black — info text drawn via Cairo overlay */
        glBindFramebuffer(GL_FRAMEBUFFER, gtk_fbo);
        glViewport(0,0,ww,hh);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        return TRUE;
    }

    /* compute dt */
    gint64 now = g_get_monotonic_time();
    if (ctx->last_frame_time == 0) ctx->last_frame_time = now;
    float dt = (float)(now - ctx->last_frame_time) / 1000000.0f;
    ctx->last_frame_time = now;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.1f)   dt = 0.1f;
    ctx->frame_time = dt;

    md_analyze_sound(ctx);
    md_render_frame(ctx);

    static int first_render = 1;
    if (first_render) {
        fprintf(stderr,"milkdrop: first render frame, preset='%s' cur_fbo=%d\n",
                ctx->preset.name, ctx->cur_fbo);
        first_render = 0;
    }

    /* blit FBO result to GtkGLArea's framebuffer (NOT FBO 0!) */
    /* md_render_frame rendered into cur_fbo THEN swapped, so the
     * just-finished frame is at 1-cur_fbo (the pre-swap cur_fbo). */
    int display_fbo = 1 - ctx->cur_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, gtk_fbo);
    glViewport(0, 0, ww, hh);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(ctx->gl.prog_blit);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->gl.fbo_tex[display_fbo]);
    glUniform1i(glGetUniformLocation(ctx->gl.prog_blit,"uTex"),0);
    gl_draw_fullscreen_quad(ctx);

    static int dbg_count = 0;
    if (dbg_count++ < 5) {
        GLenum err = glGetError();
        fprintf(stderr, "milkdrop: frame %d  gtk_fbo=%u display_fbo=%d tex=%u err=0x%x\n",
                dbg_count, gtk_fbo, display_fbo, ctx->gl.fbo_tex[display_fbo], err);
    }

    /* request next frame */
    gtk_widget_queue_draw(GTK_WIDGET(area));
    return TRUE;
}

static void on_unrealize(GtkGLArea *area, gpointer data)
{
    (void)area;
    md_ctx_t *ctx = data;
    md_gl_t *g = &ctx->gl;
    if (!g->gl_ready) return;
    gtk_gl_area_make_current(area);
    glDeleteProgram(g->prog_warp); glDeleteProgram(g->prog_post);
    glDeleteProgram(g->prog_echo); glDeleteProgram(g->prog_darken);
    glDeleteProgram(g->prog_color); glDeleteProgram(g->prog_blit);
    glDeleteVertexArrays(1, &g->quad_vao); glDeleteBuffers(1, &g->quad_vbo);
    glDeleteVertexArrays(1, &g->mesh_vao); glDeleteBuffers(1, &g->mesh_vbo);
    glDeleteBuffers(1, &g->mesh_ebo);
    glDeleteVertexArrays(1, &g->wave_vao); glDeleteBuffers(1, &g->wave_vbo);
    glDeleteFramebuffers(2, g->fbo); glDeleteTextures(2, g->fbo_tex);
    g->gl_ready = 0;
}

/* ── mashup helpers ───────────────────────────────────────────── */

/* Mini-mashup: randomize wave colors and motion from another preset */
static void md_mini_mashup(md_ctx_t *ctx, int save)
{
    if (ctx->lib.count < 2) return;
    if (save && !ctx->has_saved_preset) {
        ctx->saved_preset = ctx->preset;
        ctx->has_saved_preset = 1;
    }
    int donor = g_random_int_range(0, ctx->lib.count);
    md_preset_t tmp; memset(&tmp, 0, sizeof(tmp));
    if (md_load_preset(&tmp, ctx->lib.paths[donor])) return;
    /* mix in wave colors, motion, and a few interesting params */
    ctx->preset.wave_r = tmp.wave_r; ctx->preset.wave_g = tmp.wave_g;
    ctx->preset.wave_b = tmp.wave_b;
    ctx->preset.dx = tmp.dx; ctx->preset.dy = tmp.dy;
    ctx->preset.rot = tmp.rot; ctx->preset.sx = tmp.sx; ctx->preset.sy = tmp.sy;
    show_info(ctx, "Mini-mashup from: %s", tmp.name);
}

/* Deep mashup: replace warp, shapes, and post-process from donor */
static void md_deep_mashup(md_ctx_t *ctx, int save)
{
    if (ctx->lib.count < 2) return;
    if (save && !ctx->has_saved_preset) {
        ctx->saved_preset = ctx->preset;
        ctx->has_saved_preset = 1;
    }
    int donor = g_random_int_range(0, ctx->lib.count);
    md_preset_t tmp; memset(&tmp, 0, sizeof(tmp));
    if (md_load_preset(&tmp, ctx->lib.paths[donor])) return;
    /* deep: take warp, decay, echo, post-process, shapes */
    ctx->preset.fDecay = tmp.fDecay;
    ctx->preset.fVideoEchoZoom = tmp.fVideoEchoZoom;
    ctx->preset.fVideoEchoAlpha = tmp.fVideoEchoAlpha;
    ctx->preset.nVideoEchoOrientation = tmp.nVideoEchoOrientation;
    ctx->preset.warp = tmp.warp; ctx->preset.fWarpScale = tmp.fWarpScale;
    ctx->preset.zoom = tmp.zoom; ctx->preset.rot = tmp.rot;
    ctx->preset.cx = tmp.cx; ctx->preset.cy = tmp.cy;
    ctx->preset.dx = tmp.dx; ctx->preset.dy = tmp.dy;
    ctx->preset.sx = tmp.sx; ctx->preset.sy = tmp.sy;
    ctx->preset.bBrighten = tmp.bBrighten; ctx->preset.bDarken = tmp.bDarken;
    ctx->preset.bSolarize = tmp.bSolarize; ctx->preset.bInvert = tmp.bInvert;
    ctx->preset.fGammaAdj = tmp.fGammaAdj;
    for (int i = 0; i < MAX_CUSTOM_SHAPES; i++)
        ctx->preset.shapes[i] = tmp.shapes[i];
    /* keep per-pixel code from original for stability */
    show_info(ctx, "Deep mashup from: %s", tmp.name);
}

static void md_undo_mashup(md_ctx_t *ctx)
{
    if (!ctx->has_saved_preset) { show_info(ctx, "No mashup to undo"); return; }
    ctx->preset = ctx->saved_preset;
    ctx->has_saved_preset = 0;
    show_info(ctx, "Mashup undone");
}

/* ── signal handlers ─────────────────────────────────────────── */

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    md_ctx_t *ctx = data;
    if (ev->type==GDK_2BUTTON_PRESS && ev->button==1) {
        int idx=g_random_int_range(0,ctx->lib.count?ctx->lib.count:1);
        md_load_preset_idx(ctx, idx, 1);
        show_info(ctx,"[%d/%d] %s",ctx->preset_idx+1,ctx->lib.count,
                  ctx->lib.names[ctx->preset_idx]);
        return TRUE;
    }
    gtk_widget_grab_focus(w); return FALSE;
}

/* ── fullscreen support ──────────────────────────────────────── */

/* Forward declarations */
static void md_toggle_fullscreen(md_ctx_t *ctx);
static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data);

/* Key handler for the fullscreen window itself */
static gboolean on_fs_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    /* Forward to the main key handler (which knows about F and Escape) */
    return on_key_press(w, ev, data);
}

/* Resize FBO textures to match the current rendering resolution */
static void md_resize_fbos(md_ctx_t *ctx, int w, int h)
{
    md_gl_t *g = &ctx->gl;
    if (!g->gl_ready) return;

    g->tex_w = w;
    g->tex_h = h;
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g->fbo_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[i]);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fprintf(stderr, "milkdrop: FBO resized to %dx%d\n", w, h);
}

static void md_toggle_fullscreen(md_ctx_t *ctx)
{
    if (!ctx->fs_overlay) return;

    if (!ctx->fullscreen) {
        /* ── enter fullscreen ───────────────────────────────── */

        /* Find monitor resolution */
        GdkScreen *screen = gtk_widget_get_screen(ctx->gl_area);
        GdkDisplay *display = gdk_screen_get_display(screen);
        GdkWindow *gdk_win = gtk_widget_get_window(ctx->gl_area);
        GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, gdk_win);
        GdkRectangle geom;
        gdk_monitor_get_geometry(monitor, &geom);
        ctx->fs_width = geom.width;
        ctx->fs_height = geom.height;

        /* Create fullscreen window */
        ctx->fs_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(ctx->fs_window), "MilkDrop");
        gtk_window_set_decorated(GTK_WINDOW(ctx->fs_window), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(ctx->fs_window), TRUE);
        gtk_widget_add_events(ctx->fs_window, GDK_KEY_PRESS_MASK);
        g_signal_connect(ctx->fs_window, "key-press-event",
                         G_CALLBACK(on_fs_key_press), ctx);

        /* Prevent the window's destroy from killing the plugin */
        g_signal_connect(ctx->fs_window, "delete-event",
                         G_CALLBACK(gtk_widget_hide_on_delete), NULL);

        /* Reparent the overlay (which contains GL area + info) */
        g_object_ref(ctx->fs_overlay);
        gtk_container_remove(GTK_CONTAINER(ctx->embed_parent), ctx->fs_overlay);
        gtk_container_add(GTK_CONTAINER(ctx->fs_window), ctx->fs_overlay);
        g_object_unref(ctx->fs_overlay);

        gtk_widget_show_all(ctx->fs_window);
        gtk_window_fullscreen(GTK_WINDOW(ctx->fs_window));
        gtk_widget_grab_focus(ctx->gl_area);

        ctx->fullscreen = 1;

        /* Resize FBOs to native resolution for crisp rendering */
        gtk_gl_area_make_current(GTK_GL_AREA(ctx->gl_area));
        md_resize_fbos(ctx, ctx->fs_width, ctx->fs_height);

        show_info(ctx, "Fullscreen %dx%d  (F or Esc to exit)",
                  ctx->fs_width, ctx->fs_height);
    } else {
        /* ── leave fullscreen ───────────────────────────────── */

        /* Reparent overlay back to embedded container */
        g_object_ref(ctx->fs_overlay);
        gtk_container_remove(GTK_CONTAINER(ctx->fs_window), ctx->fs_overlay);
        gtk_box_pack_start(GTK_BOX(ctx->embed_parent), ctx->fs_overlay,
                           TRUE, TRUE, 0);
        g_object_unref(ctx->fs_overlay);

        gtk_widget_show_all(ctx->embed_parent);
        gtk_widget_grab_focus(ctx->gl_area);

        /* Destroy fullscreen window */
        gtk_widget_destroy(ctx->fs_window);
        ctx->fs_window = NULL;
        ctx->fullscreen = 0;

        /* Resize FBOs back to the compile-time default */
        gtk_gl_area_make_current(GTK_GL_AREA(ctx->gl_area));
        md_resize_fbos(ctx, MD_TEX_W, MD_TEX_H);

        show_info(ctx, "Windowed mode");
    }
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w; md_ctx_t *ctx=data;
    int shift = (ev->state & GDK_SHIFT_MASK) != 0;
    int ctrl  = (ev->state & GDK_CONTROL_MASK) != 0;
    (void)ctrl;

    switch (ev->keyval) {

    /* ── SPACE: soft cut to next preset ──────────────────────── */
    case GDK_KEY_space:
        if (shift) {
            /* Shift+SPACE: hard cut to next */
            ctx->prev_preset_idx = ctx->preset_idx;
            md_load_preset_idx(ctx, ctx->preset_idx + 1, 0);
            show_info(ctx, "[HARD] [%d/%d] %s", ctx->preset_idx+1,
                      ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        } else {
            ctx->prev_preset_idx = ctx->preset_idx;
            md_load_preset_idx(ctx, ctx->preset_idx + 1, 1);
            show_info(ctx, "[%d/%d] %s", ctx->preset_idx+1,
                      ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        }
        return TRUE;

    /* ── H: hard cut to random preset ────────────────────────── */
    case GDK_KEY_h: case GDK_KEY_H: {
        ctx->prev_preset_idx = ctx->preset_idx;
        int idx = g_random_int_range(0, ctx->lib.count ? ctx->lib.count : 1);
        md_load_preset_idx(ctx, idx, 0);
        show_info(ctx, "[HARD] [%d/%d] %s", ctx->preset_idx+1,
                  ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        return TRUE;
    }

    /* ── BACKSPACE: go back to previous preset ───────────────── */
    case GDK_KEY_BackSpace:
        if (ctx->prev_preset_idx >= 0 && ctx->prev_preset_idx < ctx->lib.count) {
            int tmp = ctx->preset_idx;
            md_load_preset_idx(ctx, ctx->prev_preset_idx, 1);
            ctx->prev_preset_idx = tmp;
            show_info(ctx, "[BACK] [%d/%d] %s", ctx->preset_idx+1,
                      ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        }
        return TRUE;

    /* ── N: next preset (soft cut) ───────────────────────────── */
    case GDK_KEY_n: case GDK_KEY_N:
        if (shift) {
            /* Shift+N: show/hide info text */
            ctx->show_preset_name = !ctx->show_preset_name;
            show_info(ctx, "Preset name: %s",
                      ctx->show_preset_name ? "ON" : "OFF");
        } else {
            ctx->prev_preset_idx = ctx->preset_idx;
            md_load_preset_idx(ctx, ctx->preset_idx + 1, 1);
            show_info(ctx, "[%d/%d] %s", ctx->preset_idx+1,
                      ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        }
        return TRUE;

    /* ── P: previous preset ──────────────────────────────────── */
    case GDK_KEY_p: case GDK_KEY_P:
        ctx->prev_preset_idx = ctx->preset_idx;
        md_load_preset_idx(ctx, ctx->preset_idx - 1, 1);
        show_info(ctx, "[%d/%d] %s", ctx->preset_idx+1,
                  ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        return TRUE;

    /* ── R: toggle random/sequential ─────────────────────────── */
    case GDK_KEY_r: case GDK_KEY_R:
        if (shift) {
            /* Shift+R: random jump */
            int idx = g_random_int_range(0, ctx->lib.count ? ctx->lib.count : 1);
            ctx->prev_preset_idx = ctx->preset_idx;
            md_load_preset_idx(ctx, idx, 1);
            show_info(ctx, "[RANDOM] [%d/%d] %s", ctx->preset_idx+1,
                      ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        } else {
            ctx->sequential_order = !ctx->sequential_order;
            show_info(ctx, "Order: %s",
                      ctx->sequential_order ? "SEQUENTIAL" : "RANDOM");
            if (!ctx->sequential_order) md_lib_shuffle(&ctx->lib);
        }
        return TRUE;

    /* ── L / ` / ~: lock/unlock preset ───────────────────────── */
    case GDK_KEY_l: case GDK_KEY_L:
    case GDK_KEY_grave: case GDK_KEY_asciitilde:
        ctx->locked = !ctx->locked;
        show_info(ctx, "Preset %s",
                  ctx->locked ? "LOCKED" : "UNLOCKED");
        return TRUE;

    /* ── A: auto-cycle / Shift+A: mini-mashup ────────────────── */
    case GDK_KEY_a:
        md_mini_mashup(ctx, 1);
        return TRUE;
    case GDK_KEY_A:
        md_undo_mashup(ctx);
        return TRUE;

    /* ── Z: deep-mashup / Shift+Z: undo ──────────────────────── */
    case GDK_KEY_z:
        md_deep_mashup(ctx, 1);
        return TRUE;
    case GDK_KEY_Z:
        md_undo_mashup(ctx);
        return TRUE;

    /* ── C: randomize wave color / Shift+C: undo ─────────────── */
    case GDK_KEY_c:
        if (!ctx->has_saved_preset) {
            ctx->saved_preset = ctx->preset;
            ctx->has_saved_preset = 1;
        }
        ctx->preset.wave_r = (float)(g_random_int_range(0,1000))/999.0f;
        ctx->preset.wave_g = (float)(g_random_int_range(0,1000))/999.0f;
        ctx->preset.wave_b = (float)(g_random_int_range(0,1000))/999.0f;
        show_info(ctx, "Wave color randomized (%.2f, %.2f, %.2f)",
                  ctx->preset.wave_r, ctx->preset.wave_g, ctx->preset.wave_b);
        return TRUE;
    case GDK_KEY_C:
        md_undo_mashup(ctx);
        return TRUE;

    /* ── D: toggle darken center ─────────────────────────────── */
    case GDK_KEY_d: case GDK_KEY_D:
        ctx->preset.bDarkenCenter = !ctx->preset.bDarkenCenter;
        show_info(ctx, "Darken center: %s",
                  ctx->preset.bDarkenCenter ? "ON" : "OFF");
        return TRUE;

    /* ── W: cycle wave mode ──────────────────────────────────── */
    case GDK_KEY_w: case GDK_KEY_W: {
        int m = (ctx->preset.nWaveMode + 1) % NUM_WAVE_MODES;
        ctx->preset.nWaveMode = m;
        static const char *mn[] = {"Circular","XY Scope","Lissajous",
            "Volume Spiro","Horizontal","Explosive","Dual","Spectrum"};
        show_info(ctx, "Wave: %s", mn[m]);
        return TRUE;
    }

    /* ── S: shuffle library ──────────────────────────────────── */
    case GDK_KEY_s: case GDK_KEY_S:
        md_lib_shuffle(&ctx->lib);
        show_info(ctx, "Library shuffled (%d presets)", ctx->lib.count);
        return TRUE;

    /* ── B / Shift+B: blend time adjust ──────────────────────── */
    case GDK_KEY_b:
        ctx->blend_time_user += 0.5f;
        if (ctx->blend_time_user > 10.0f) ctx->blend_time_user = 10.0f;
        show_info(ctx, "Blend time: %.1fs", ctx->blend_time_user);
        return TRUE;
    case GDK_KEY_B:
        ctx->blend_time_user -= 0.5f;
        if (ctx->blend_time_user < 0.0f) ctx->blend_time_user = 0.0f;
        show_info(ctx, "Blend time: %.1fs", ctx->blend_time_user);
        return TRUE;

    /* ── U / Shift+U: flip motion / lock direction ───────────── */
    case GDK_KEY_u:
        ctx->preset.dx = -ctx->preset.dx;
        ctx->preset.dy = -ctx->preset.dy;
        ctx->preset.rot = -ctx->preset.rot;
        show_info(ctx, "Motion direction flipped");
        return TRUE;
    case GDK_KEY_U:
        ctx->preset.dx = 0; ctx->preset.dy = 0;
        ctx->preset.rot = 0;
        show_info(ctx, "Motion locked (zeroed)");
        return TRUE;

    /* ── O: reload current preset from disk ──────────────────── */
    case GDK_KEY_o: case GDK_KEY_O:
        if (ctx->preset_loaded && ctx->preset_idx >= 0 &&
            ctx->preset_idx < ctx->lib.count) {
            md_load_preset_idx(ctx, ctx->preset_idx, 0);
            ctx->pf_init_done = 0;
            memset(ctx->q_vars, 0, sizeof(ctx->q_vars));
            show_info(ctx, "Reloaded: %s", ctx->lib.names[ctx->preset_idx]);
        }
        return TRUE;

    /* ── T: toggle wave thick ────────────────────────────────── */
    case GDK_KEY_t: case GDK_KEY_T:
        ctx->preset.bWaveThick = !ctx->preset.bWaveThick;
        show_info(ctx, "Wave thick: %s",
                  ctx->preset.bWaveThick ? "ON" : "OFF");
        return TRUE;

    /* ── I: toggle invert ────────────────────────────────────── */
    case GDK_KEY_i: case GDK_KEY_I:
        ctx->preset.bInvert = !ctx->preset.bInvert;
        show_info(ctx, "Invert: %s", ctx->preset.bInvert ? "ON" : "OFF");
        return TRUE;

    /* ── G: toggle gamma (cycle 1.0 → 2.0 → 3.0 → 1.0) ─────── */
    case GDK_KEY_g: case GDK_KEY_G: {
        float g = ctx->preset.fGammaAdj;
        if (g < 1.5f) g = 2.0f;
        else if (g < 2.5f) g = 3.0f;
        else g = 1.0f;
        ctx->preset.fGammaAdj = g;
        show_info(ctx, "Gamma: %.1f", g);
        return TRUE;
    }

    /* ── +/-: adjust preset duration ─────────────────────────── */
    case GDK_KEY_plus: case GDK_KEY_KP_Add: case GDK_KEY_equal:
        ctx->preset_duration += 5.0f;
        if (ctx->preset_duration > 120.0f) ctx->preset_duration = 120.0f;
        show_info(ctx, "Auto-advance: %.0fs", ctx->preset_duration);
        return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        ctx->preset_duration -= 5.0f;
        if (ctx->preset_duration < 5.0f) ctx->preset_duration = 5.0f;
        show_info(ctx, "Auto-advance: %.0fs", ctx->preset_duration);
        return TRUE;

    /* ── F / Escape: fullscreen toggle ────────────────────────── */
    case GDK_KEY_f: case GDK_KEY_F:
        md_toggle_fullscreen(ctx);
        return TRUE;
    case GDK_KEY_Escape:
        if (ctx->fullscreen) { md_toggle_fullscreen(ctx); return TRUE; }
        return FALSE;

    /* ── F1: help overlay ────────────────────────────────────── */
    case GDK_KEY_F1:
        ctx->show_help = !ctx->show_help;
        return TRUE;

    /* ── F4: toggle persistent preset name ───────────────────── */
    case GDK_KEY_F4:
        ctx->show_preset_name = !ctx->show_preset_name;
        show_info(ctx, "Preset name: %s",
                  ctx->show_preset_name ? "ALWAYS" : "AUTO");
        return TRUE;

    /* ── F5: toggle FPS display ──────────────────────────────── */
    case GDK_KEY_F5:
        ctx->show_fps = !ctx->show_fps;
        show_info(ctx, "FPS display: %s", ctx->show_fps ? "ON" : "OFF");
        return TRUE;

    /* ── F8: toggle auto-cycle on beat ───────────────────────── */
    case GDK_KEY_F8:
        ctx->auto_preset = !ctx->auto_preset;
        show_info(ctx, "Beat auto-cycle: %s",
                  ctx->auto_preset ? "ON" : "OFF");
        return TRUE;

    /* ── F9: toggle solarize ─────────────────────────────────── */
    case GDK_KEY_F9:
        ctx->preset.bSolarize = !ctx->preset.bSolarize;
        show_info(ctx, "Solarize: %s",
                  ctx->preset.bSolarize ? "ON" : "OFF");
        return TRUE;

    /* ── F10: toggle brighten ────────────────────────────────── */
    case GDK_KEY_F10:
        ctx->preset.bBrighten = !ctx->preset.bBrighten;
        show_info(ctx, "Brighten: %s",
                  ctx->preset.bBrighten ? "ON" : "OFF");
        return TRUE;

    /* ── F11: toggle all effects ─────────────────────────────── */
    case GDK_KEY_F11:
        ctx->effects_enabled = !ctx->effects_enabled;
        show_info(ctx, "Effects: %s",
                  ctx->effects_enabled ? "ON" : "OFF");
        return TRUE;

    /* ── 0-9: rate the current preset (just info) ────────────── */
    case GDK_KEY_0: case GDK_KEY_1: case GDK_KEY_2: case GDK_KEY_3:
    case GDK_KEY_4: case GDK_KEY_5: case GDK_KEY_6: case GDK_KEY_7:
    case GDK_KEY_8: case GDK_KEY_9:
        show_info(ctx, "Rated preset %d/10 (not saved)",
                  ev->keyval - GDK_KEY_0);
        return TRUE;

    default: break;
    }
    return FALSE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)w; md_ctx_t *ctx=data;
    if (ev->direction==GDK_SCROLL_UP) md_load_preset_idx(ctx,ctx->preset_idx+1,1);
    else if (ev->direction==GDK_SCROLL_DOWN) md_load_preset_idx(ctx,ctx->preset_idx-1,1);
    else return FALSE;
    show_info(ctx,"[%d/%d] %s",ctx->preset_idx+1,ctx->lib.count,
              ctx->lib.names[ctx->preset_idx]);
    return TRUE;
}

/* ── audio ───────────────────────────────────────────────────── */

static void md_start_capture(md_ctx_t *ctx)
{
    if (!ctx->host || !ctx->audio_node_count) return;
    if (ctx->host->pw_meter_start)
        ctx->host->pw_meter_start(ctx->host->host_ctx,
                                   ctx->audio_node_ids, ctx->audio_node_count);
    ctx->capture_started=1; ctx->running=1;
    if (!ctx->preset_loaded && ctx->lib.count>0) {
        md_load_preset_idx(ctx, g_random_int_range(0,ctx->lib.count), 0);
        show_info(ctx,"[%d/%d] %s",ctx->preset_idx+1,ctx->lib.count,
                  ctx->lib.names[ctx->preset_idx]);
    }
    /* kick off rendering by requesting a draw */
    if (ctx->gl_area) gtk_widget_queue_draw(ctx->gl_area);
}

static void md_stop_capture(md_ctx_t *ctx)
{
    ctx->running=0; ctx->capture_started=0;
    if (ctx->host && ctx->host->pw_meter_stop)
        ctx->host->pw_meter_stop(ctx->host->host_ctx);
}

static gboolean audio_tick(gpointer data)
{
    md_ctx_t *ctx = data;
    if (!ctx->running || !ctx->host || !ctx->host->pw_meter_read)
        return G_SOURCE_CONTINUE;

    /* compute delta time for this tick */
    gint64 now = g_get_monotonic_time();
    float dt = 0.020f;
    if (ctx->last_audio_tick > 0)
        dt = (float)(now - ctx->last_audio_tick) / 1000000.0f;
    ctx->last_audio_tick = now;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.1f)   dt = 0.1f;

    /* read raw peaks from PipeWire */
    float raw[2] = {0, 0};
    for (size_t i = 0; i < ctx->audio_node_count; i++) {
        int ll = 0, lr = 0;
        ctx->host->pw_meter_read(ctx->host->host_ctx,
                                  ctx->audio_node_ids[i], &ll, &lr);
        float fl = ll / 1000.f, fr = lr / 1000.f;
        if (fl > raw[0]) raw[0] = fl;
        if (fr > raw[1]) raw[1] = fr;
    }

    /* update targets when we get a real reading */
    if (raw[0] > 0.001f || raw[1] > 0.001f) {
        ctx->smooth_target[0] = raw[0];
        ctx->smooth_target[1] = raw[1];
        ctx->last_peak_time = now;
    } else {
        /* if no new peak for a while, gently decay the target toward zero */
        float since = (float)(now - ctx->last_peak_time) / 1000000.0f;
        if (since > 0.08f) { /* 80ms grace period for network jitter */
            float fade = expf(-2.5f * (since - 0.08f));
            ctx->smooth_target[0] *= fade;
            ctx->smooth_target[1] *= fade;
        }
    }

    /* critically-damped spring follower for each channel:
     * fast attack (jump up to target quickly), slower release */
    for (int ch = 0; ch < 2; ch++) {
        float tgt = ctx->smooth_target[ch];
        float cur = ctx->smooth_peak[ch];
        float vel = ctx->peak_vel[ch];

        if (tgt > cur) {
            /* attack: fast spring (omega ~40) */
            float omega = 40.0f;
            float diff = tgt - cur;
            float spring = omega * omega * diff;
            float damp = 2.0f * omega * vel;
            vel += (spring - damp) * dt;
            cur += vel * dt;
            if (cur > tgt) { cur = tgt; vel = 0; }
        } else {
            /* release: gentler spring (omega ~8) for smooth decay */
            float omega = 8.0f;
            float diff = tgt - cur;
            float spring = omega * omega * diff;
            float damp = 2.0f * omega * vel;
            vel += (spring - damp) * dt;
            cur += vel * dt;
            if (cur < 0) { cur = 0; vel = 0; }
        }

        ctx->smooth_peak[ch] = cur;
        ctx->peak_vel[ch] = vel;
    }

    float pl = ctx->smooth_peak[0];
    float pr = ctx->smooth_peak[1];

    /* synthesize waveform from smoothed peaks */
    unsigned wp = atomic_load_explicit(&ctx->ring.write_pos, memory_order_relaxed);
    int samples_per_tick = MD_WAVEFORM_N / 4;
    float phase_inc = dt * 120.0f; /* base frequency scales with real time */
    for (int i = 0; i < samples_per_tick; i++) {
        float t = (float)i / samples_per_tick;
        float p = ctx->synth_phase + phase_inc * t;
        float noise = ((float)(g_random_int_range(0, 1000)) / 1000.f - 0.5f) * 0.25f;
        float wl = pl * (sinf(p * 4.0f) * 0.45f +
                         sinf(p * 9.13f) * 0.25f +
                         sinf(p * 17.7f) * 0.12f +
                         noise * (0.1f + pl * 0.2f));
        float wr = pr * (sinf(p * 4.0f + 0.5f) * 0.45f +
                         sinf(p * 9.13f + 0.3f) * 0.25f +
                         sinf(p * 17.7f + 0.7f) * 0.12f +
                         noise * (0.1f + pr * 0.2f));
        unsigned idx = wp % RING_SIZE;
        ctx->ring.buf[0][idx] = wl;
        ctx->ring.buf[1][idx] = wr;
        wp++;
    }
    ctx->synth_phase += phase_inc;
    /* keep phase from growing unbounded */
    if (ctx->synth_phase > 1000.0f * (float)M_PI)
        ctx->synth_phase -= 1000.0f * (float)M_PI;
    atomic_store_explicit(&ctx->ring.write_pos, wp, memory_order_release);
    return G_SOURCE_CONTINUE;
}

/* ── plugin callbacks ────────────────────────────────────────── */

/* Draw info overlay using Cairo on top of GL */
static gboolean on_overlay_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    md_ctx_t *ctx = data;
    int ww = gtk_widget_get_allocated_width(w);
    int hh = gtk_widget_get_allocated_height(w);

    /* Clear to fully transparent so GL area shows through */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (!ctx->running && !ctx->audio_node_count) {
        cairo_set_source_rgba(cr,1,1,1,0.3);
        cairo_select_font_face(cr,"Sans",CAIRO_FONT_SLANT_ITALIC,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr,12);
        cairo_move_to(cr,12,hh/2.0-16);
        cairo_show_text(cr,"MilkDrop — waiting for audio stream");
        cairo_set_font_size(cr,9);
        cairo_move_to(cr,12,hh/2.0+4);
        cairo_show_text(cr,"Select a process with PipeWire audio output");
        cairo_move_to(cr,12,hh/2.0+20);
        char buf[80]; snprintf(buf,sizeof(buf),"%d presets loaded from disk",
                               ctx->lib.count);
        cairo_show_text(cr, buf);
    }

    /* ── help overlay (F1) ───────────────────────────────────── */
    if (ctx->show_help) {
        /* semi-transparent dark background */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.82);
        cairo_rectangle(cr, 0, 0, ww, hh);
        cairo_fill(cr);

        cairo_select_font_face(cr, "Monospace", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13);
        cairo_set_source_rgba(cr, 1, 0.9, 0.4, 1);
        cairo_move_to(cr, 16, 28);
        cairo_show_text(cr, "MilkDrop Hotkeys (F1 to close)");

        cairo_select_font_face(cr, "Monospace", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10);

        static const char *help[] = {
            "SPACE        Soft cut to next preset",
            "Shift+SPACE  Hard cut to next preset",
            "BACKSPACE    Go back to previous preset",
            "H            Hard cut to random preset",
            "N            Next preset (soft cut)",
            "P            Previous preset (soft cut)",
            "Shift+N      Toggle preset name display",
            "",
            "R            Toggle random/sequential order",
            "Shift+R      Random jump (soft cut)",
            "L / ` / ~    Lock/unlock current preset",
            "S            Shuffle preset library",
            "",
            "A            Mini-mashup (randomize colors+motion)",
            "Shift+A      Undo mashup",
            "Z            Deep mashup (warp+shapes+echo)",
            "Shift+Z      Undo mashup",
            "C            Randomize wave colors",
            "Shift+C      Undo color randomization",
            "",
            "W            Cycle wave mode",
            "D            Toggle darken center",
            "T            Toggle wave thick",
            "I            Toggle invert",
            "G            Cycle gamma (1→2→3→1)",
            "U            Flip motion direction",
            "Shift+U      Zero out motion",
            "O            Reload current preset from disk",
            "",
            "B / Shift+B  Increase/decrease blend time",
            "+/-          Increase/decrease auto-advance time",
            "0-9          Rate preset (display only)",
            "",
            "F1           This help screen",
            "F4           Toggle persistent preset name",
            "F5           Toggle FPS display",
            "F8           Toggle beat-driven auto-cycle",
            "F9           Toggle solarize",
            "F10          Toggle brighten",
            "F11          Toggle all effects",
            "",
            "F            Toggle fullscreen",
            "Escape       Exit fullscreen",
            "",
            "Scroll       Next/previous preset",
            "Double-click Random preset",
            NULL
        };

        float y = 48;
        cairo_set_source_rgba(cr, 0.85, 0.9, 1.0, 0.95);
        for (int i = 0; help[i]; i++) {
            if (help[i][0] == '\0') { y += 6; continue; }
            cairo_move_to(cr, 24, y);
            cairo_show_text(cr, help[i]);
            y += 14;
            if (y > hh - 16) break;
        }
    }

    /* ── FPS counter (F5) ────────────────────────────────────── */
    if (ctx->show_fps && ctx->preset_loaded) {
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "%.1f fps", ctx->fps_display);
        cairo_select_font_face(cr, "Monospace", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10);
        /* shadow */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_move_to(cr, ww - 81, 15);
        cairo_show_text(cr, fps_buf);
        /* text */
        cairo_set_source_rgba(cr, 0.2, 1.0, 0.2, 0.8);
        cairo_move_to(cr, ww - 82, 14);
        cairo_show_text(cr, fps_buf);
    }

    /* ── status indicators (lock, auto, blend) ───────────────── */
    if (ctx->preset_loaded) {
        float sx = 6, sy = hh - 50;
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 9);
        if (ctx->locked) {
            cairo_set_source_rgba(cr, 1, 0.3, 0.3, 0.7);
            cairo_move_to(cr, sx, sy);
            cairo_show_text(cr, "LOCKED");
            sy -= 13;
        }
        if (ctx->auto_preset) {
            cairo_set_source_rgba(cr, 0.3, 1, 0.3, 0.7);
            cairo_move_to(cr, sx, sy);
            cairo_show_text(cr, "AUTO");
            sy -= 13;
        }
        if (ctx->blending) {
            cairo_set_source_rgba(cr, 0.5, 0.7, 1, 0.6);
            cairo_move_to(cr, sx, sy);
            char bb[32]; snprintf(bb, sizeof(bb), "BLEND %.0f%%",
                                  ctx->blend_progress * 100);
            cairo_show_text(cr, bb);
        }
    }

    /* ── info toast (bottom bar) ─────────────────────────────── */
    if (ctx->info_frames > 0) {
        ctx->info_frames--;
        float a = ctx->info_frames>60 ? 0.9f : 0.9f*ctx->info_frames/60.0f;
        cairo_set_source_rgba(cr,0,0,0,a*0.7);
        cairo_rectangle(cr,0,hh-36,ww,36); cairo_fill(cr);
        cairo_set_source_rgba(cr,1,1,1,a);
        cairo_select_font_face(cr,"Sans",CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr,11);
        cairo_move_to(cr,8,hh-14);
        cairo_show_text(cr,ctx->info_text);
    }

    /* ── preset name (top-left, always or faded) ─────────────── */
    if (ctx->preset_loaded) {
        float name_alpha = ctx->show_preset_name ? 0.6f : 0.2f;
        cairo_set_source_rgba(cr, 1, 1, 1, name_alpha);
        cairo_select_font_face(cr,"Sans",CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr,9);
        cairo_move_to(cr,6,14);
        char dn[80]; strncpy(dn,ctx->preset.name,70); dn[70]=0;
        if (strlen(ctx->preset.name)>70) strcat(dn,"...");
        cairo_show_text(cr,dn);
    }

    /* ── audio source text (top-right) ───────────────────────── */
    if (ctx->running && ctx->audio_node_count > 0) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9);

        /* build the audio source string */
        char src[384];
        if (ctx->audio_media_name[0])
            snprintf(src, sizeof(src), "%s", ctx->audio_media_name);
        else if (ctx->audio_app_name[0])
            snprintf(src, sizeof(src), "%s", ctx->audio_app_name);
        else if (ctx->process_name[0])
            snprintf(src, sizeof(src), "%s", ctx->process_name);
        else
            snprintf(src, sizeof(src), "PID %d", (int)ctx->last_pid);

        /* measure text width to right-align */
        cairo_text_extents_t ext;
        cairo_text_extents(cr, src, &ext);
        float tx = ww - ext.width - 8;
        float ty = hh - 44;

        /* shadow */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
        cairo_move_to(cr, tx + 1, ty + 1);
        cairo_show_text(cr, src);
        /* text */
        cairo_set_source_rgba(cr, 0.7, 0.8, 1.0, 0.5);
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, src);
    }

    return FALSE; /* let GL render through */
}

static GtkWidget *md_create_widget(void *opaque)
{
    md_ctx_t *ctx = opaque;

    if (!ctx->lib_loaded) {
        /* GLib RNG self-seeds; no srand() needed */
        md_scan_dir(&ctx->lib, PRESET_DIR, 0);
        if (ctx->lib.count) md_lib_shuffle(&ctx->lib);
        ctx->lib_loaded = 1;
    }

    /* GtkGLArea for hardware-accelerated rendering */
    ctx->gl_area = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(ctx->gl_area), 3, 3);
    gtk_gl_area_set_has_alpha(GTK_GL_AREA(ctx->gl_area), FALSE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(ctx->gl_area), FALSE);
    gtk_widget_set_size_request(ctx->gl_area, 320, 240);
    gtk_widget_set_hexpand(ctx->gl_area, TRUE);
    gtk_widget_set_vexpand(ctx->gl_area, TRUE);
    gtk_widget_set_can_focus(ctx->gl_area, TRUE);
    gtk_widget_add_events(ctx->gl_area,
        GDK_BUTTON_PRESS_MASK|GDK_KEY_PRESS_MASK|GDK_SCROLL_MASK);

    g_signal_connect(ctx->gl_area, "realize", G_CALLBACK(on_realize), ctx);
    g_signal_connect(ctx->gl_area, "render", G_CALLBACK(on_render), ctx);
    g_signal_connect(ctx->gl_area, "unrealize", G_CALLBACK(on_unrealize), ctx);
    g_signal_connect(ctx->gl_area, "button-press-event",
                     G_CALLBACK(on_button_press), ctx);
    g_signal_connect(ctx->gl_area, "key-press-event",
                     G_CALLBACK(on_key_press), ctx);
    g_signal_connect(ctx->gl_area, "scroll-event",
                     G_CALLBACK(on_scroll), ctx);

    /* Overlay drawing area for text (on top of GL) */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), ctx->gl_area);

    GtkWidget *info_area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(info_area, FALSE);
    /* Make the overlay transparent so GL shows through */
    gtk_widget_set_app_paintable(info_area, TRUE);
    GdkScreen *screen = gtk_widget_get_screen(info_area);
    GdkVisual *rgba_visual = gdk_screen_get_rgba_visual(screen);
    if (rgba_visual)
        gtk_widget_set_visual(info_area, rgba_visual);
    g_signal_connect(info_area, "draw", G_CALLBACK(on_overlay_draw), ctx);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), info_area);

    /* stash overlay refs so fullscreen can reparent them */
    ctx->fs_overlay = overlay;
    ctx->fs_info_area = info_area;

    ctx->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(ctx->main_box), overlay, TRUE, TRUE, 0);
    ctx->embed_parent = ctx->main_box;

    gtk_widget_show_all(ctx->main_box);
    return ctx->main_box;
}

static void md_activate(void *opaque, const evemon_host_services_t *svc)
{ ((md_ctx_t*)opaque)->host = svc; }

static void md_update(void *opaque, const evemon_proc_data_t *data)
{
    md_ctx_t *ctx = opaque;
    /* stash process name for overlay display */
    if (data->name && data->name[0])
        snprintf(ctx->process_name, sizeof(ctx->process_name), "%s", data->name);
    if (!data->pw_nodes || !data->pw_node_count) {
        if (ctx->capture_started) md_stop_capture(ctx);
        ctx->audio_node_count=0;
        /* Hide the notebook tab when there are no audio streams */
        if (ctx->main_box) {
            GtkWidget *nb = gtk_widget_get_parent(ctx->main_box);
            if (nb && GTK_IS_NOTEBOOK(nb)) {
                gtk_widget_hide(ctx->main_box);
                GtkWidget *tab = gtk_notebook_get_tab_label(
                    GTK_NOTEBOOK(nb), ctx->main_box);
                if (tab) gtk_widget_hide(tab);
            }
        }
        return;
    }
    size_t old=ctx->audio_node_count; ctx->audio_node_count=0;
    for (size_t i=0; i<data->pw_node_count; i++) {
        const evemon_pw_node_t *nd=&data->pw_nodes[i];
        if (nd->pid!=data->pid) continue;
        if (strstr(nd->media_class,"Stream") && strstr(nd->media_class,"Output")
            && strstr(nd->media_class,"Audio")) {
            if (ctx->audio_node_count<64)
                ctx->audio_node_ids[ctx->audio_node_count++]=nd->id;
            /* capture audio source info from the first matching node */
            if (ctx->audio_node_count == 1) {
                snprintf(ctx->audio_app_name, sizeof(ctx->audio_app_name),
                         "%s", nd->app_name);
                snprintf(ctx->audio_media_name, sizeof(ctx->audio_media_name),
                         "%s", nd->media_name);
            }
        }
    }
    if (ctx->audio_node_count) {
        /* Show the notebook tab now that we have audio streams */
        if (ctx->main_box) {
            GtkWidget *nb = gtk_widget_get_parent(ctx->main_box);
            if (nb && GTK_IS_NOTEBOOK(nb)) {
                gtk_widget_show(ctx->main_box);
                GtkWidget *tab = gtk_notebook_get_tab_label(
                    GTK_NOTEBOOK(nb), ctx->main_box);
                if (tab) gtk_widget_show(tab);
            }
        }
        /* Detect whether the set of audio node IDs actually changed,
         * not just the count.  When a user switches audio output
         * (e.g. headphones → speakers) the PW node ID changes but
         * the count stays the same.  We must restart capture in that
         * case so pw_meter connects to the new stream. */
        int ids_changed = (ctx->audio_node_count != old);
        if (!ids_changed && old > 0) {
            /* count matches — compare the actual IDs */
            for (size_t i = 0; i < ctx->audio_node_count; i++) {
                int found = 0;
                for (size_t j = 0; j < old; j++) {
                    if (ctx->audio_node_ids[i] == ctx->old_audio_node_ids[j])
                        { found = 1; break; }
                }
                if (!found) { ids_changed = 1; break; }
            }
        }
        if (!ctx->capture_started || ids_changed
            || data->pid!=ctx->last_pid) {
            md_stop_capture(ctx); md_start_capture(ctx);
            if (!ctx->audio_timer) ctx->audio_timer=g_timeout_add(20,audio_tick,ctx);
        }
    } else if (ctx->capture_started) md_stop_capture(ctx);
    /* save current IDs for next-cycle comparison */
    memcpy(ctx->old_audio_node_ids, ctx->audio_node_ids,
           ctx->audio_node_count * sizeof(uint32_t));
    ctx->old_audio_node_count = ctx->audio_node_count;
    ctx->last_pid = data->pid;
}

static void md_clear(void *opaque)
{
    md_ctx_t *ctx=opaque;
    md_stop_capture(ctx);
    if (ctx->audio_timer) { g_source_remove(ctx->audio_timer); ctx->audio_timer=0; }
    ctx->audio_node_count=0; ctx->last_pid=0;
    if (ctx->gl_area) gtk_widget_queue_draw(ctx->gl_area);
    /* Hide the notebook tab */
    if (ctx->main_box) {
        GtkWidget *nb = gtk_widget_get_parent(ctx->main_box);
        if (nb && GTK_IS_NOTEBOOK(nb)) {
            gtk_widget_hide(ctx->main_box);
            GtkWidget *tab = gtk_notebook_get_tab_label(
                GTK_NOTEBOOK(nb), ctx->main_box);
            if (tab) gtk_widget_hide(tab);
        }
    }
}

static void md_destroy(void *opaque)
{
    md_ctx_t *ctx=opaque;
    if (ctx->fullscreen && ctx->fs_window) {
        /* reparent back before destroying window */
        g_object_ref(ctx->fs_overlay);
        gtk_container_remove(GTK_CONTAINER(ctx->fs_window), ctx->fs_overlay);
        gtk_box_pack_start(GTK_BOX(ctx->embed_parent), ctx->fs_overlay,
                           TRUE, TRUE, 0);
        g_object_unref(ctx->fs_overlay);
        gtk_widget_destroy(ctx->fs_window);
        ctx->fs_window = NULL;
        ctx->fullscreen = 0;
    }
    md_stop_capture(ctx);
    if (ctx->audio_timer) { g_source_remove(ctx->audio_timer); ctx->audio_timer=0; }
    md_lib_free(&ctx->lib);
    free(ctx);
}

/* ── plugin descriptor ───────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    md_ctx_t *ctx = calloc(1, sizeof(md_ctx_t));
    if (!ctx) return NULL;
    ctx->effects_enabled = 1;
    ctx->blend_time_user = 2.5f;
    ctx->preset_duration = 30.0f;  /* auto-advance every 30s by default */
    ctx->prev_preset_idx = -1;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "MilkDrop",
        .id            = "org.evemon.milkdrop",
        .version       = "3.0",
        .data_needs    = evemon_NEED_PIPEWIRE,
        .plugin_ctx    = ctx,
        .create_widget = md_create_widget,
        .update        = md_update,
        .clear         = md_clear,
        .destroy       = md_destroy,
        .activate      = md_activate,
    };
    return p;
}
