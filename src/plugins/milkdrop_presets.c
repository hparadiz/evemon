#include "milkdrop_internal.h"

#include <limits.h>

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

int md_load_preset(md_preset_t *p, const char *filepath)
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


static void md_lib_add(md_preset_lib_t *lib, const char *path, const char *name)
{
    if (lib->count>=lib->capacity) {
        if (lib->capacity > INT_MAX / 2)
            return;
        int new_capacity = lib->capacity ? lib->capacity * 2 : 1024;
        if ((size_t)new_capacity > SIZE_MAX / sizeof(char *))
            return;
        char **new_paths = malloc((size_t)new_capacity * sizeof(char *));
        char **new_names = malloc((size_t)new_capacity * sizeof(char *));
        if (!new_paths || !new_names) {
            free(new_paths);
            free(new_names);
            return;
        }
        if (lib->count > 0) {
            memcpy(new_paths, lib->paths, lib->count * sizeof(char *));
            memcpy(new_names, lib->names, lib->count * sizeof(char *));
        }
        free(lib->paths);
        free(lib->names);
        lib->paths = new_paths;
        lib->names = new_names;
        lib->capacity = new_capacity;
    }
    char *stored_path = strdup(path);
    char *dn = strdup(name);
    if (!stored_path || !dn) {
        free(stored_path);
        free(dn);
        return;
    }
    lib->paths[lib->count] = stored_path;
    char *dt=strrchr(dn,'.');
    if (dt && !strcmp(dt,".milk")) *dt=0;
    lib->names[lib->count]=dn; lib->count++;
}

void md_scan_dir(md_preset_lib_t *lib, const char *dir, int depth)
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

void md_lib_free(md_preset_lib_t *lib)
{
    for (int i=0;i<lib->count;i++) { free(lib->paths[i]); free(lib->names[i]); }
    free(lib->paths); free(lib->names); memset(lib,0,sizeof(*lib));
}

void md_lib_shuffle(md_preset_lib_t *lib)
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
        if (evemon_debug)
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
        if (evemon_debug)
            fprintf(stderr,"milkdrop: program link error: %s\n",log);
        glDeleteProgram(p); return 0;
    }
    return p;
}

GLuint gl_create_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = gl_compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = gl_compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }
    GLuint p = gl_link_program(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

/* ── pre-lexed cache management ─────────────────────────────── */

void md_lex_cache_free(md_ctx_t *ctx)
{
    free(ctx->lex_per_frame_init); ctx->lex_per_frame_init = NULL;
    free(ctx->lex_per_frame);      ctx->lex_per_frame      = NULL;
    free(ctx->lex_per_pixel);      ctx->lex_per_pixel      = NULL;
    for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
        free(ctx->lex_wave_pf[i]); ctx->lex_wave_pf[i] = NULL;
        free(ctx->lex_wave_pp[i]); ctx->lex_wave_pp[i] = NULL;
    }
    for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
        free(ctx->lex_shape_pf[i]); ctx->lex_shape_pf[i] = NULL;
    }
    ctx->lex_serial = -1;
    /* Return freed pages to the OS immediately so RSS doesn't drift upward
     * as we cycle through presets.  malloc_trim(0) releases all releasable
     * memory in the top of the heap back to the kernel. */
    malloc_trim(0);
}

/* Allocate and lex one program; returns NULL if src is empty. */
static md_lexer_t *md_lex_alloc(const char *src)
{
    if (!src || !src[0]) return NULL;
    md_lexer_t *l = malloc(sizeof(md_lexer_t));
    if (!l) return NULL;
    md_lex(l, src);
    return l;
}

/*
 * Re-build all pre-lexed programs from the active preset.
 * Called once each time preset_serial != lex_serial.
 */
void md_lex_cache_rebuild(md_ctx_t *ctx)
{
    md_preset_t *p = &ctx->preset;
    md_lex_cache_free(ctx);

    if (p->per_frame_init_count > 0) {
        md_concat_lines(p->per_frame_init, p->per_frame_init_count,
                        ctx->code_buf, sizeof(ctx->code_buf));
        ctx->lex_per_frame_init = md_lex_alloc(ctx->code_buf);
    }
    if (p->per_frame_count > 0) {
        md_concat_lines(p->per_frame, p->per_frame_count,
                        ctx->code_buf, sizeof(ctx->code_buf));
        ctx->lex_per_frame = md_lex_alloc(ctx->code_buf);
    }
    if (p->per_pixel_count > 0) {
        md_concat_lines(p->per_pixel, p->per_pixel_count,
                        ctx->code_buf, sizeof(ctx->code_buf));
        ctx->lex_per_pixel = md_lex_alloc(ctx->code_buf);
    }
    for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
        md_custom_wave_t *cw = &p->waves[i];
        if (!cw->enabled) continue;
        if (cw->per_frame_count > 0) {
            md_concat_lines(cw->per_frame, cw->per_frame_count,
                            ctx->code_buf, sizeof(ctx->code_buf));
            ctx->lex_wave_pf[i] = md_lex_alloc(ctx->code_buf);
        }
        if (cw->per_point_count > 0) {
            md_concat_lines(cw->per_point, cw->per_point_count,
                            ctx->code_buf, sizeof(ctx->code_buf));
            ctx->lex_wave_pp[i] = md_lex_alloc(ctx->code_buf);
        }
    }
    for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
        md_custom_shape_t *cs = &p->shapes[i];
        if (!cs->enabled) continue;
        if (cs->per_frame_count > 0) {
            md_concat_lines(cs->per_frame, cs->per_frame_count,
                            ctx->code_buf, sizeof(ctx->code_buf));
            ctx->lex_shape_pf[i] = md_lex_alloc(ctx->code_buf);
        }
    }
    ctx->lex_serial = ctx->preset_serial;
}

/* ── per-frame variable setup ────────────────────────────────── */

void md_setup_pf_vars(md_ctx_t *ctx, md_preset_t *p)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    vc->var_count = 0;
    memset(vc->ht, 0, sizeof(vc->ht));  /* clear hash table alongside var_count */
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

void md_readback_pf_vars(md_ctx_t *ctx)
{
    for (int i=0; i<NUM_Q_VAR; i++) {
        char nm[8]; snprintf(nm,sizeof(nm),"q%d",i+1);
        ctx->q_vars[i] = md_var_read(&ctx->pf_vars, nm);
    }
}

/* ── favorites: save / load / recall / cycle ────────────────── */

/* forward declaration – defined below */
void show_info(md_ctx_t *ctx, const char *fmt, ...);

static char *md_favorites_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "evemon",
                            MD_FAV_BASENAME, NULL);
}

static void md_favorites_save(md_ctx_t *ctx)
{
    char *path = md_favorites_path();
    if (!path) return;

    char *dir = g_path_get_dirname(path);
    if (dir) {
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
    }

    FILE *f = fopen(path, "w");
    if (!f) { g_free(path); return; }
    fprintf(f, "; MilkDrop favorites – managed by evemon\n\n");

    for (int i = 0; i < MD_FAV_SLOTS; i++) {
        md_fav_slot_t *s = &ctx->favorites[i];
        if (!s->used) continue;
        fprintf(f, "[slot%d]\n", i);
        fprintf(f, "name=%s\n",     s->name);
        fprintf(f, "filepath=%s\n", s->snap.filepath);
        /* scalar fields */
        fprintf(f, "fGammaAdj=%.6g\n",        s->snap.fGammaAdj);
        fprintf(f, "fDecay=%.6g\n",            s->snap.fDecay);
        fprintf(f, "fVideoEchoZoom=%.6g\n",    s->snap.fVideoEchoZoom);
        fprintf(f, "fVideoEchoAlpha=%.6g\n",   s->snap.fVideoEchoAlpha);
        fprintf(f, "nVideoEchoOrientation=%d\n",s->snap.nVideoEchoOrientation);
        fprintf(f, "nWaveMode=%d\n",           s->snap.nWaveMode);
        fprintf(f, "bAdditiveWaves=%d\n",      s->snap.bAdditiveWaves);
        fprintf(f, "bWaveDots=%d\n",           s->snap.bWaveDots);
        fprintf(f, "bWaveThick=%d\n",          s->snap.bWaveThick);
        fprintf(f, "bModWaveAlphaByVolume=%d\n",s->snap.bModWaveAlphaByVolume);
        fprintf(f, "bMaximizeWaveColor=%d\n",  s->snap.bMaximizeWaveColor);
        fprintf(f, "bTexWrap=%d\n",            s->snap.bTexWrap);
        fprintf(f, "bDarkenCenter=%d\n",       s->snap.bDarkenCenter);
        fprintf(f, "bBrighten=%d\n",           s->snap.bBrighten);
        fprintf(f, "bDarken=%d\n",             s->snap.bDarken);
        fprintf(f, "bSolarize=%d\n",           s->snap.bSolarize);
        fprintf(f, "bInvert=%d\n",             s->snap.bInvert);
        fprintf(f, "fWaveAlpha=%.6g\n",        s->snap.fWaveAlpha);
        fprintf(f, "fWaveScale=%.6g\n",        s->snap.fWaveScale);
        fprintf(f, "fWaveSmoothing=%.6g\n",    s->snap.fWaveSmoothing);
        fprintf(f, "fWaveParam=%.6g\n",        s->snap.fWaveParam);
        fprintf(f, "fModWaveAlphaStart=%.6g\n",s->snap.fModWaveAlphaStart);
        fprintf(f, "fModWaveAlphaEnd=%.6g\n",  s->snap.fModWaveAlphaEnd);
        fprintf(f, "fWarpAnimSpeed=%.6g\n",    s->snap.fWarpAnimSpeed);
        fprintf(f, "fWarpScale=%.6g\n",        s->snap.fWarpScale);
        fprintf(f, "fZoomExponent=%.6g\n",     s->snap.fZoomExponent);
        fprintf(f, "fShader=%.6g\n",           s->snap.fShader);
        fprintf(f, "zoom=%.6g\n",              s->snap.zoom);
        fprintf(f, "rot=%.6g\n",               s->snap.rot);
        fprintf(f, "cx=%.6g\n",                s->snap.cx);
        fprintf(f, "cy=%.6g\n",                s->snap.cy);
        fprintf(f, "dx=%.6g\n",                s->snap.dx);
        fprintf(f, "dy=%.6g\n",                s->snap.dy);
        fprintf(f, "warp=%.6g\n",              s->snap.warp);
        fprintf(f, "sx=%.6g\n",                s->snap.sx);
        fprintf(f, "sy=%.6g\n",                s->snap.sy);
        fprintf(f, "wave_r=%.6g\n",            s->snap.wave_r);
        fprintf(f, "wave_g=%.6g\n",            s->snap.wave_g);
        fprintf(f, "wave_b=%.6g\n",            s->snap.wave_b);
        fprintf(f, "wave_x=%.6g\n",            s->snap.wave_x);
        fprintf(f, "wave_y=%.6g\n",            s->snap.wave_y);
        fprintf(f, "ob_size=%.6g\n",           s->snap.ob_size);
        fprintf(f, "ob_r=%.6g\n",              s->snap.ob_r);
        fprintf(f, "ob_g=%.6g\n",              s->snap.ob_g);
        fprintf(f, "ob_b=%.6g\n",              s->snap.ob_b);
        fprintf(f, "ob_a=%.6g\n",              s->snap.ob_a);
        fprintf(f, "ib_size=%.6g\n",           s->snap.ib_size);
        fprintf(f, "ib_r=%.6g\n",              s->snap.ib_r);
        fprintf(f, "ib_g=%.6g\n",              s->snap.ib_g);
        fprintf(f, "ib_b=%.6g\n",              s->snap.ib_b);
        fprintf(f, "ib_a=%.6g\n",              s->snap.ib_a);
        fprintf(f, "nMotionVectorsX=%.6g\n",   s->snap.nMotionVectorsX);
        fprintf(f, "nMotionVectorsY=%.6g\n",   s->snap.nMotionVectorsY);
        fprintf(f, "mv_dx=%.6g\n",             s->snap.mv_dx);
        fprintf(f, "mv_dy=%.6g\n",             s->snap.mv_dy);
        fprintf(f, "mv_l=%.6g\n",              s->snap.mv_l);
        fprintf(f, "mv_r=%.6g\n",              s->snap.mv_r);
        fprintf(f, "mv_g=%.6g\n",              s->snap.mv_g);
        fprintf(f, "mv_b=%.6g\n",              s->snap.mv_b);
        fprintf(f, "mv_a=%.6g\n",              s->snap.mv_a);
        for (int q = 0; q < NUM_Q_VAR; q++)
            fprintf(f, "q%d=%.8g\n", q+1, s->q_vars[q]);
        fprintf(f, "\n");
    }
    fclose(f);
    g_free(path);
}

void md_favorites_load(md_ctx_t *ctx)
{
    memset(ctx->favorites, 0, sizeof(ctx->favorites));
    char *path = md_favorites_path();
    if (!path) return;

    FILE *f = fopen(path, "r");
    g_free(path);
    if (!f) return;

    char line[1024];
    int  slot = -1;
    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        /* skip blank / comment */
        if (!line[0] || line[0] == ';') continue;
        /* section header */
        if (line[0] == '[') {
            int s = -1;
            if (sscanf(line, "[slot%d]", &s) == 1 && s >= 0 && s < MD_FAV_SLOTS)
                slot = s;
            else
                slot = -1;
            continue;
        }
        if (slot < 0) continue;
        md_fav_slot_t *sv = &ctx->favorites[slot];
        sv->used = 1;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line, *val = eq+1;

        if (!strcmp(key,"name"))     { strncpy(sv->name,           val, 255); sv->name[255]=0; }
        if (!strcmp(key,"filepath")) { strncpy(sv->snap.filepath, val,1023); sv->snap.filepath[1023]=0; }
#define SF(field) if (!strcmp(key,#field)) sv->snap.field = (float)atof(val)
#define SI(field) if (!strcmp(key,#field)) sv->snap.field = atoi(val)
        SF(fGammaAdj); SF(fDecay); SF(fVideoEchoZoom); SF(fVideoEchoAlpha);
        SI(nVideoEchoOrientation); SI(nWaveMode);
        SI(bAdditiveWaves); SI(bWaveDots); SI(bWaveThick);
        SI(bModWaveAlphaByVolume); SI(bMaximizeWaveColor);
        SI(bTexWrap); SI(bDarkenCenter);
        SI(bBrighten); SI(bDarken); SI(bSolarize); SI(bInvert);
        SF(fWaveAlpha); SF(fWaveScale); SF(fWaveSmoothing); SF(fWaveParam);
        SF(fModWaveAlphaStart); SF(fModWaveAlphaEnd);
        SF(fWarpAnimSpeed); SF(fWarpScale); SF(fZoomExponent); SF(fShader);
        SF(zoom); SF(rot); SF(cx); SF(cy); SF(dx); SF(dy);
        SF(warp); SF(sx); SF(sy);
        SF(wave_r); SF(wave_g); SF(wave_b); SF(wave_x); SF(wave_y);
        SF(ob_size); SF(ob_r); SF(ob_g); SF(ob_b); SF(ob_a);
        SF(ib_size); SF(ib_r); SF(ib_g); SF(ib_b); SF(ib_a);
        SF(nMotionVectorsX); SF(nMotionVectorsY);
        SF(mv_dx); SF(mv_dy); SF(mv_l); SF(mv_r); SF(mv_g); SF(mv_b); SF(mv_a);
        /* q-vars: q1 .. q32 */
        if (key[0] == 'q' && key[1] >= '1' && key[1] <= '9') {
            int qi = atoi(key+1) - 1;
            if (qi >= 0 && qi < NUM_Q_VAR) sv->q_vars[qi] = (float)atof(val);
        }
#undef SF
#undef SI
    }
    fclose(f);
}

/* Find the name to show for a preset: use the base filename without extension */
static void md_fav_preset_label(md_ctx_t *ctx, char *out, int outsz)
{
    const char *fp = ctx->preset.filepath;
    const char *base = strrchr(fp, '/');
    base = base ? base+1 : fp;
    strncpy(out, base, outsz-1); out[outsz-1] = 0;
    /* strip .milk extension */
    char *dot = strrchr(out, '.');
    if (dot && !strcasecmp(dot, ".milk")) *dot = 0;
    /* if preset has a human name, prefer it */
    if (ctx->preset.name[0])
        strncpy(out, ctx->preset.name, outsz-1);
}

/* Copy all parametric scalars from a preset into a snapshot. */
static void md_fav_snapshot_from_preset(md_fav_snapshot_t *dst,
                                        const md_preset_t  *src)
{
#define CP(f) dst->f = src->f
    {   /* avoid gcc -Wrestrict false positive from inlining into on_key_press */
        size_t n = strnlen(src->filepath, sizeof(dst->filepath) - 1);
        memcpy(dst->filepath, src->filepath, n);
        dst->filepath[n] = '\0';
    }
    CP(fGammaAdj); CP(fDecay); CP(fVideoEchoZoom); CP(fVideoEchoAlpha);
    CP(nVideoEchoOrientation); CP(nWaveMode);
    CP(bAdditiveWaves); CP(bWaveDots); CP(bWaveThick);
    CP(bModWaveAlphaByVolume); CP(bMaximizeWaveColor);
    CP(bTexWrap); CP(bDarkenCenter);
    CP(bBrighten); CP(bDarken); CP(bSolarize); CP(bInvert);
    CP(fWaveAlpha); CP(fWaveScale); CP(fWaveSmoothing); CP(fWaveParam);
    CP(fModWaveAlphaStart); CP(fModWaveAlphaEnd);
    CP(fWarpAnimSpeed); CP(fWarpScale); CP(fZoomExponent); CP(fShader);
    CP(zoom); CP(rot); CP(cx); CP(cy); CP(dx); CP(dy); CP(warp); CP(sx); CP(sy);
    CP(wave_r); CP(wave_g); CP(wave_b); CP(wave_x); CP(wave_y);
    CP(ob_size); CP(ob_r); CP(ob_g); CP(ob_b); CP(ob_a);
    CP(ib_size); CP(ib_r); CP(ib_g); CP(ib_b); CP(ib_a);
    CP(nMotionVectorsX); CP(nMotionVectorsY);
    CP(mv_dx); CP(mv_dy); CP(mv_l); CP(mv_r); CP(mv_g); CP(mv_b); CP(mv_a);
#undef CP
}

/* Overlay all parametric scalars from a snapshot onto an already-loaded preset
 * (which has its expression arrays populated from the .milk file). */
static void md_fav_overlay_scalars(md_preset_t *dst, const md_fav_snapshot_t *src)
{
#define OV(f) dst->f = src->f
    OV(fGammaAdj); OV(fDecay); OV(fVideoEchoZoom); OV(fVideoEchoAlpha);
    OV(nVideoEchoOrientation); OV(nWaveMode);
    OV(bAdditiveWaves); OV(bWaveDots); OV(bWaveThick);
    OV(bModWaveAlphaByVolume); OV(bMaximizeWaveColor);
    OV(bTexWrap); OV(bDarkenCenter);
    OV(bBrighten); OV(bDarken); OV(bSolarize); OV(bInvert);
    OV(fWaveAlpha); OV(fWaveScale); OV(fWaveSmoothing); OV(fWaveParam);
    OV(fModWaveAlphaStart); OV(fModWaveAlphaEnd);
    OV(fWarpAnimSpeed); OV(fWarpScale); OV(fZoomExponent); OV(fShader);
    OV(zoom); OV(rot); OV(cx); OV(cy); OV(dx); OV(dy); OV(warp); OV(sx); OV(sy);
    OV(wave_r); OV(wave_g); OV(wave_b); OV(wave_x); OV(wave_y);
    OV(ob_size); OV(ob_r); OV(ob_g); OV(ob_b); OV(ob_a);
    OV(ib_size); OV(ib_r); OV(ib_g); OV(ib_b); OV(ib_a);
    OV(nMotionVectorsX); OV(nMotionVectorsY);
    OV(mv_dx); OV(mv_dy); OV(mv_l); OV(mv_r); OV(mv_g); OV(mv_b); OV(mv_a);
#undef OV
}

void md_fav_save_current(md_ctx_t *ctx)
{
    /* Check if already saved (same filepath → update in place) */
    int target = -1;
    for (int i = 0; i < MD_FAV_SLOTS; i++) {
        if (ctx->favorites[i].used &&
            strcmp(ctx->favorites[i].snap.filepath,
                   ctx->preset.filepath) == 0) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        /* Find first free slot */
        for (int i = 0; i < MD_FAV_SLOTS; i++) {
            if (!ctx->favorites[i].used) { target = i; break; }
        }
    }
    if (target < 0) {
        /* All slots full: evict slot 0, shift everything down */
        memmove(&ctx->favorites[0], &ctx->favorites[1],
                sizeof(md_fav_slot_t) * (MD_FAV_SLOTS-1));
        target = MD_FAV_SLOTS - 1;
        memset(&ctx->favorites[target], 0, sizeof(md_fav_slot_t));
    }

    md_fav_slot_t *sv = &ctx->favorites[target];
    sv->used = 1;
    md_fav_snapshot_from_preset(&sv->snap, &ctx->preset);
    memcpy(sv->q_vars, ctx->q_vars, sizeof(ctx->q_vars));
    md_fav_preset_label(ctx, sv->name, sizeof(sv->name));
    md_favorites_save(ctx);
    show_info(ctx, "\xe2\x98\x85 Saved: %s", sv->name);
}

/* Returns 1 on success, 0 if slot is empty */
int md_fav_recall(md_ctx_t *ctx, int slot)
{
    if (slot < 0 || slot >= MD_FAV_SLOTS) return 0;
    md_fav_slot_t *sv = &ctx->favorites[slot];
    if (!sv->used) return 0;

    /* Reload expression code from the .milk file, then layer the saved
     * parametric overrides on top.  This is correct both in-session and
     * after a restart (previously the expression arrays were never
     * serialised to the INI file, so cross-session restore produced a
     * blank/broken visual). */
    if (sv->snap.filepath[0] &&
        md_load_preset(&ctx->preset, sv->snap.filepath) == 0) {
        md_fav_overlay_scalars(&ctx->preset, &sv->snap);
    } else {
        /* .milk file missing — at least apply the saved scalars over defaults */
        md_preset_defaults(&ctx->preset);
        strncpy(ctx->preset.filepath, sv->snap.filepath,
                sizeof(ctx->preset.filepath) - 1);
        md_fav_overlay_scalars(&ctx->preset, &sv->snap);
    }

    memcpy(ctx->q_vars, sv->q_vars, sizeof(ctx->q_vars));
    ctx->preset_loaded = 1;
    ctx->pf_init_done  = 0;   /* let per_frame_init run with restored q_vars */
    ctx->blending      = 0;
    ctx->preset_serial++;
    show_info(ctx, "\xe2\x98\x85 Recalled: %s", sv->name);
    return 1;
}

void md_fav_delete(md_ctx_t *ctx, int slot)
{
    if (slot < 0 || slot >= MD_FAV_SLOTS) return;
    char name[256];
    strncpy(name, ctx->favorites[slot].name, 255); name[255] = 0;
    /* Compact: shift slots after 'slot' down by one */
    for (int i = slot; i < MD_FAV_SLOTS-1; i++)
        ctx->favorites[i] = ctx->favorites[i+1];
    memset(&ctx->favorites[MD_FAV_SLOTS-1], 0, sizeof(md_fav_slot_t));
    md_favorites_save(ctx);
    show_info(ctx, "Removed: %s", name);
}

void md_fav_cycle_next(md_ctx_t *ctx, int delta)
{
    /* Build a list of used slot indices */
    int used[MD_FAV_SLOTS], nc = 0;
    for (int i = 0; i < MD_FAV_SLOTS; i++)
        if (ctx->favorites[i].used) used[nc++] = i;
    if (nc == 0) {
        show_info(ctx, "No favorites saved yet (Ctrl+S to save)");
        return;
    }
    /* Find current position in the used list */
    int pos = 0;
    for (int i = 0; i < nc; i++) {
        if (used[i] == ctx->fav_cycle_idx) { pos = i; break; }
    }
    pos = ((pos + delta) % nc + nc) % nc;
    ctx->fav_cycle_idx = used[pos];
    md_fav_recall(ctx, ctx->fav_cycle_idx);
}

