#include "milkdrop_internal.h"

/* ── FFT ─────────────────────────────────────────────────────── */

void fft_dit(float *re, float *im, int n)
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
void ensure_hann(void)
{
    if (g_hann_ready) return;
    for (int i = 0; i < MD_FFT_SIZE; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f*(float)M_PI*i/(MD_FFT_SIZE-1)));
    g_hann_ready = 1;
}

static float g_equalize[MD_FFT_HALF];
static int   g_eq_ready;
void ensure_equalize(void)
{
    if (g_eq_ready) return;
    for (int i = 0; i < MD_FFT_HALF; i++) {
        float x = (float)(MD_FFT_HALF - i) / (float)MD_FFT_HALF;
        g_equalize[i] = -0.02f * logf(x + 1e-10f);
    }
    g_eq_ready = 1;
}


/* ══════════════════════════════════════════════════════════════
 *  EXPRESSION EVALUATOR  (unchanged from Cairo version)
 * ══════════════════════════════════════════════════════════════ */


/*
 * Open-addressing hash table over a flat vars[] array.
 * EXPR_MAX_VARS=192, table size must be a power-of-two > 192.
 * 256 gives a max load factor of 0.75 — good collision rate.
 * hash[] stores the index into vars[] + 1 (0 = empty bucket).
 */


/* djb2-style hash truncated to table size */
static inline unsigned md_var_hash(const char *s)
{
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h & VAR_HT_MASK;
}

float *md_var_get(md_var_ctx_t *vc, const char *name)
{
    unsigned h = md_var_hash(name);
    for (unsigned probe = 0; probe < VAR_HT_SIZE; probe++) {
        unsigned slot = (h + probe) & VAR_HT_MASK;
        uint8_t  idx  = vc->ht[slot];
        if (idx == 0) {
            /* empty bucket — insert new variable here */
            if (vc->var_count >= EXPR_MAX_VARS) return NULL;
            int i = vc->var_count++;
            strncpy(vc->vars[i].name, name, 63);
            vc->vars[i].name[63] = 0;
            vc->vars[i].value    = 0.0f;
            vc->ht[slot] = (uint8_t)(i + 1);  /* store 1-based index */
            return &vc->vars[i].value;
        }
        if (strcmp(vc->vars[idx - 1].name, name) == 0)
            return &vc->vars[idx - 1].value;
    }
    return NULL;  /* table full (should not happen with load < 0.75) */
}

void md_var_set(md_var_ctx_t *vc, const char *name, float val)
{ float *p = md_var_get(vc, name); if (p) *p = val; }

float md_var_read(md_var_ctx_t *vc, const char *name)
{ float *p = md_var_get(vc, name); return p ? *p : 0.0f; }


static const char *g_func_names[] = {
    "sin","cos","tan","asin","acos","atan","atan2",
    "pow","log","log10","sqrt","abs","min","max",
    "sign","sqr","if","above","below","equal",
    "bnot","band","bor","rand","floor","ceil",
    "int","sigmoid","avg","exp","fmod",NULL
};

void md_lex(md_lexer_t *lex, const char *src)
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
            char buf[64] = {0}; int bi=0;
            while (pos<len && (isdigit((unsigned char)src[pos])||src[pos]=='.')) {
                if (bi < (int)sizeof(buf)-1) buf[bi++]=src[pos];
                pos++;
            }
            if (pos<len && (src[pos]=='e'||src[pos]=='E')) {
                if (bi < (int)sizeof(buf)-1) buf[bi++]=src[pos];
                pos++;
                if (pos<len && (src[pos]=='+'||src[pos]=='-')) {
                    if (bi < (int)sizeof(buf)-1) buf[bi++]=src[pos];
                    pos++;
                }
                while (pos<len && isdigit((unsigned char)src[pos])) {
                    if (bi < (int)sizeof(buf)-1) buf[bi++]=src[pos];
                    pos++;
                }
            }
            buf[bi]=0; t->type=TOK_NUM; t->num_val=(float)atof(buf);
            lex->count++; continue;
        }
        if (isalpha((unsigned char)c)||c=='_') {
            char buf[64] = {0}; int bi=0;
            while (pos<len && (isalnum((unsigned char)src[pos])||src[pos]=='_')) {
                if (bi < (int)sizeof(buf)-1) buf[bi++]=src[pos];
                pos++;
            }
            buf[bi]=0;
            int sv=pos;
            while (sv<len && (src[sv]==' '||src[sv]=='\t')) sv++;
            int is_func=0;
            if (sv<len && src[sv]=='(') {
                for (int i=0; g_func_names[i]; i++)
                    if (!strcmp(buf,g_func_names[i])) { is_func=1; break; }
            }
            t->type = is_func ? TOK_FUNC : TOK_VAR;
            memcpy(t->str_val, buf, sizeof(t->str_val));
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

/*
 * Replay a pre-lexed token stream with a fresh variable context.
 * Resets cur and depth to 0 so the same lexer can be re-evaluated
 * each frame (or each mesh vertex) without re-lexing.
 * The caller must not modify l->tokens.
 */
float md_eval_precompiled(md_lexer_t *l, md_var_ctx_t *vc)
{
    if (!l || l->count == 0) return 0.0f;
    l->cur   = 0;
    l->depth = 0;
    return md_eval_program(l, vc);
}

void md_concat_lines(const char lines[][MAX_LINE_LEN], int n,
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


/* ── favorites slot ──────────────────────────────────────────── */

/*
 * Compact scalar-only snapshot for a saved favorite.  md_preset_t carries
 * ~1.44 MB of expression-code arrays (per_frame[64][1024], per_pixel,
 * custom wave/shape code…) that the favorites system never serialises to
 * disk.  Embedding a full md_preset_t in every slot was therefore wasting
 * 16 × 1.44 MB ≈ 23 MB inside md_ctx_t's calloc'd block.
 *
 * Only the parametric scalar fields are stored here.  Expression code is
 * always (re)loaded from the .milk file on recall, which is also what
 * fixes cross-session restore: after a restart the expressions were
 * previously empty because they were never written to the INI file.
 */


/* ── sound analysis ──────────────────────────────────────────── */

void md_analyze_sound(md_ctx_t *ctx)
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

