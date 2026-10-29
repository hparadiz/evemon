#include "milkdrop_internal.h"

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
    /* Always drain a tiny amount toward black so that feedback loops with
     * decay near 1.0 still converge to black instead of grey-washing.
     * 0.004 ≈ 1/250: at 60 fps the buffer drains fully in ~4 s even
     * if decay=1.0, which is imperceptible during normal visual activity.
     * The larger constant is needed to overcome echo passes that inject
     * energy back into the buffer each frame. */
    "  fragColor = vec4(clamp(c * uDecay - 0.004, 0.0, 1.0), 1.0);\n"
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

/* ── vivid random color ─────────────────────────────────────── */
/*
 * Pick a random color guaranteed to be vivid and distinguishable.
 * Strategy: random hue, saturation clamped to [sat_min, 1.0],
 * value clamped to [val_min, val_max].  Converts HSV → RGB.
 * This avoids near-gray, near-black, and near-white outcomes that
 * make the wave invisible against both dark and bright backgrounds.
 */
void md_random_vivid_color(float sat_min, float val_min, float val_max,
                                   float *out_r, float *out_g, float *out_b)
{
    /* random hue [0, 360) */
    float h = (float)g_random_int_range(0, 3600) / 10.0f;
    /* saturation: avoid washed-out grays */
    float s = sat_min + (1.0f - sat_min) *
              ((float)g_random_int_range(0, 1000) / 999.0f);
    /* value: avoid too dark or too white */
    float v = val_min + (val_max - val_min) *
              ((float)g_random_int_range(0, 1000) / 999.0f);

    /* HSV → RGB (standard conversion) */
    float hh = h / 60.0f;
    int   i  = (int)hh % 6;
    float f  = hh - (int)hh;
    float p  = v * (1.0f - s);
    float q  = v * (1.0f - s * f);
    float t  = v * (1.0f - s * (1.0f - f));
    switch (i) {
    case 0: *out_r=v; *out_g=t; *out_b=p; break;
    case 1: *out_r=q; *out_g=v; *out_b=p; break;
    case 2: *out_r=p; *out_g=v; *out_b=t; break;
    case 3: *out_r=p; *out_g=q; *out_b=v; break;
    case 4: *out_r=t; *out_g=p; *out_b=v; break;
    default: *out_r=v; *out_g=p; *out_b=q; break;
    }
}

/* ── beat detection ──────────────────────────────────────────── */

static int md_detect_beat(md_ctx_t *ctx)
{
    if (--ctx->beat_holdoff > 0) return 0;
    if (ctx->sound.imm_rel[0] > 2.2f) { ctx->beat_holdoff = 30; return 1; }
    return 0;
}

void show_info(md_ctx_t *ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    vsnprintf(ctx->info_text, sizeof(ctx->info_text), fmt, ap);
    va_end(ap); ctx->info_frames = 180;
}

void md_load_preset_idx(md_ctx_t *ctx, int idx, int blend)
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
        ctx->preset_serial++; /* invalidate pre-lexed cache */
    }
}

/* ══════════════════════════════════════════════════════════════
 *  GL RENDERING
 * ══════════════════════════════════════════════════════════════ */

void gl_draw_fullscreen_quad(md_ctx_t *ctx)
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
    /* Clamp decay to prevent energy accumulation.
     * Values above 1.0 from per-frame code would amplify the
     * feedback loop.  Cap at 0.992 so the buffer always drains
     * toward black even for presets that set decay = 1.0 exactly,
     * preventing the grey-wash that otherwise builds up over time. */
    if (decay > 0.992f) decay = 0.992f;
    if (decay < 0.0f) decay = 0.0f;
    int   wrap  = (int)md_var_read(vc,"wrap");

    float wt = ctx->warp_time;
    float ws_inv = 1.0f / (p->fWarpScale + 0.01f);
    float f[4];
    f[0]=11.68f+4*cosf(wt*1.413f+10); f[1]=8.77f+3*cosf(wt*1.113f+7);
    f[2]=10.54f+3*cosf(wt*0.786f+3);  f[3]=11.49f+4*cosf(wt*0.893f+5);
    float ax = 1, ay = (float)ctx->gl.tex_w/ctx->gl.tex_h;

    md_lexer_t *pp_lex = ctx->lex_per_pixel; /* pre-lexed, NULL if no per-pixel code */

    /* Compute warped UV for each mesh vertex – use pre-allocated buffer */
    float *verts = ctx->gl.mesh_verts; /* MESH_FLOAT_COUNT floats, never NULL after gl_init */

    /*
     * Per-pixel variable context: we keep ONE md_var_ctx_t on the stack and
     * reset only the 4 per-vertex inputs (x, y, rad, ang) each iteration.
     * This avoids a 13 KB memcpy for every one of the 1,813 mesh vertices.
     * We pre-seed it from the per-frame var context, then find the indices
     * of x/y/rad/ang once so we can update them directly by pointer.
     */
    /*
     * Per-pixel variable context: ONE md_var_ctx_t copied once per frame.
     * Input pointers (x, y, rad, ang) and output pointers (zoom … sy) are
     * pinned by address so we never call the O(n) md_var_read/md_var_get
     * inside the tight per-vertex loop — just direct float reads/writes.
     */
    md_var_ctx_t pp_vc;
    /* inputs */
    float *pp_x_ptr = NULL, *pp_y_ptr = NULL;
    float *pp_rad_ptr = NULL, *pp_ang_ptr = NULL;
    /* outputs */
    float *pp_zoom = NULL, *pp_zexp = NULL, *pp_rot  = NULL, *pp_warp = NULL;
    float *pp_cx   = NULL, *pp_cy   = NULL, *pp_dx   = NULL, *pp_dy   = NULL;
    float *pp_sx   = NULL, *pp_sy   = NULL;
    if (pp_lex) {
        memcpy(&pp_vc, vc, sizeof(md_var_ctx_t));
        /* ensure all variables exist in the context before pinning */
        pp_x_ptr   = md_var_get(&pp_vc, "x");
        pp_y_ptr   = md_var_get(&pp_vc, "y");
        pp_rad_ptr = md_var_get(&pp_vc, "rad");
        pp_ang_ptr = md_var_get(&pp_vc, "ang");
        pp_zoom = md_var_get(&pp_vc, "zoom");    pp_zexp = md_var_get(&pp_vc, "zoomexp");
        pp_rot  = md_var_get(&pp_vc, "rot");     pp_warp = md_var_get(&pp_vc, "warp");
        pp_cx   = md_var_get(&pp_vc, "cx");      pp_cy   = md_var_get(&pp_vc, "cy");
        pp_dx   = md_var_get(&pp_vc, "dx");      pp_dy   = md_var_get(&pp_vc, "dy");
        pp_sx   = md_var_get(&pp_vc, "sx");      pp_sy   = md_var_get(&pp_vc, "sy");
    }

    for (int my = 0; my <= MESH_H; my++) {
        for (int mx = 0; mx <= MESH_W; mx++) {
            float nx = (float)mx / MESH_W;
            float ny = (float)my / MESH_H;

            /* per-pixel vars: start from per-frame defaults */
            float pz=zoom, pe=zexp, pr=rot, pw=wrp;
            float pcx=cx, pcy=cy, pdx=d_x, pdy=d_y, psx=s_x, psy=s_y;

            if (pp_lex) {
                float x2=nx*2-1, y2=ny*2-1;
                float rad = sqrtf(x2*x2*ax*ax + y2*y2*ay*ay);
                float ang = atan2f(y2*ay, x2*ax);
                /* write the 4 per-vertex inputs via pinned pointers */
                if (pp_x_ptr)   *pp_x_ptr   = nx;
                if (pp_y_ptr)   *pp_y_ptr   = ny;
                if (pp_rad_ptr) *pp_rad_ptr = rad;
                if (pp_ang_ptr) *pp_ang_ptr = ang;
                md_eval_precompiled(pp_lex, &pp_vc);
                /* read 10 outputs via pinned pointers — zero string compares */
                if (pp_zoom) pz  = *pp_zoom;
                if (pp_zexp) pe  = *pp_zexp;
                if (pp_rot)  pr  = *pp_rot;
                if (pp_warp) pw  = *pp_warp;
                if (pp_cx)   pcx = *pp_cx;
                if (pp_cy)   pcy = *pp_cy;
                if (pp_dx)   pdx = *pp_dx;
                if (pp_dy)   pdy = *pp_dy;
                if (pp_sx)   psx = *pp_sx;
                if (pp_sy)   psy = *pp_sy;
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
    glUniform1i(ctx->gl.u_warp_tex,   0);
    glUniform1f(ctx->gl.u_warp_decay, fminf(fmaxf(decay, 0.0f), 1.0f));

    glBindVertexArray(ctx->gl.mesh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->gl.mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, MESH_FLOAT_COUNT*sizeof(float), verts, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                          (void*)(2*sizeof(float)));
    /* re-bind EBO so glDrawElements can find the indices */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->gl.mesh_ebo);

    glDrawElements(GL_TRIANGLES, ctx->gl.mesh_index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    /* verts is ctx->gl.mesh_verts — persistent, do not free here */
}

/* Draw darken-center effect */
static void gl_darken_center(md_ctx_t *ctx)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ctx->gl.prog_darken);
    glUniform1f(ctx->gl.u_darken_aspect, (float)ctx->gl.tex_w/ctx->gl.tex_h);
    gl_draw_fullscreen_quad(ctx);
    glDisable(GL_BLEND);
}

/* Upload wave/shape vertex data and draw */

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
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glLineWidth(1.0f);
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

    /* fill: triangle fan with center + rim — use pre-allocated scratch buffer */
    int nv = sides + 2;   /* sides ≤ 100, so nv ≤ 102 ≤ SHAPE_SCRATCH_VERTS */
    color_vert_t *fv = ctx->gl.shape_scratch;
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
    /* fv is shape_scratch — persistent, do not free */

    /* border — reuse the same scratch buffer (fill is already uploaded) */
    if (ba > 0.001f) {
        int nb = sides + 1;  /* nb ≤ 101 ≤ SHAPE_SCRATCH_VERTS */
        color_vert_t *bv = ctx->gl.shape_scratch;
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
        /* bv is shape_scratch — persistent, do not free */
        glLineWidth(1.0f);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
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
        md_lexer_t *pp_lex = ctx->lex_wave_pp[wi];
        if (!pp_lex) continue;
        int samples=cw->samples;
        if (samples<2) samples=2;
        if (samples>MD_WAVEFORM_N) samples=MD_WAVEFORM_N;
        md_lexer_t *pf_lex = ctx->lex_wave_pf[wi];

        color_vert_t *verts = ctx->gl.wave_verts; /* persistent — no stack alloc */
        int nv=0;
        /*
         * Single var context for both per-frame and per-point evaluation.
         * One memcpy from pf_vars, seed all variables, run pf_lex in-place,
         * then pin pointers for the per-point loop — zero extra copies.
         */
        md_var_ctx_t ppv;
        memcpy(&ppv, &ctx->pf_vars, sizeof(md_var_ctx_t));
        md_var_set(&ppv,"r",cw->r); md_var_set(&ppv,"g",cw->g);
        md_var_set(&ppv,"b",cw->b); md_var_set(&ppv,"a",cw->a);
        md_var_set(&ppv,"samples",(float)samples);
        /* seed per-point variables so hash slots are allocated before pinning */
        md_var_set(&ppv,"sample",0); md_var_set(&ppv,"value1",0);
        md_var_set(&ppv,"value2",0);
        md_var_set(&ppv,"x",0.5f); md_var_set(&ppv,"y",0.5f);
        if (pf_lex) md_eval_precompiled(pf_lex, &ppv);
        /* re-read wave-level outputs after per-frame eval */
        float br = md_var_read(&ppv,"r"),  bg = md_var_read(&ppv,"g");
        float bb = md_var_read(&ppv,"b"),  ba = md_var_read(&ppv,"a");
        samples = (int)md_var_read(&ppv,"samples");
        if (samples<2) samples=2;
        if (samples>MD_WAVEFORM_N) samples=MD_WAVEFORM_N;
        /* pin all per-point pointers */
        float *wp_sample = md_var_get(&ppv,"sample");
        float *wp_v1     = md_var_get(&ppv,"value1");
        float *wp_v2     = md_var_get(&ppv,"value2");
        float *wp_x      = md_var_get(&ppv,"x");
        float *wp_y      = md_var_get(&ppv,"y");
        float *wp_r      = md_var_get(&ppv,"r");
        float *wp_g      = md_var_get(&ppv,"g");
        float *wp_b_     = md_var_get(&ppv,"b");
        float *wp_a      = md_var_get(&ppv,"a");
        for (int i=0; i<samples && nv<MAX_WAVE_VERTS; i++) {
            float sf=(float)i/(samples-1);
            float v1 = cw->bSpectrum ? snd->spec[i%MD_FFT_HALF]*cw->scaling
                                     : snd->wave[0][i%MD_WAVEFORM_N]*cw->scaling;
            float v2 = cw->bSpectrum ? snd->spec[i%MD_FFT_HALF]*cw->scaling
                                     : snd->wave[1][i%MD_WAVEFORM_N]*cw->scaling;
            if (wp_sample) *wp_sample = sf;
            if (wp_v1)     *wp_v1     = v1;
            if (wp_v2)     *wp_v2     = v2;
            if (wp_x)      *wp_x      = 0.5f+v1*0.5f;
            if (wp_y)      *wp_y      = 0.5f+v2*0.5f;
            if (wp_r)      *wp_r      = br;
            if (wp_g)      *wp_g      = bg;
            if (wp_b_)     *wp_b_     = bb;
            if (wp_a)      *wp_a      = ba;
            md_eval_precompiled(pp_lex, &ppv);
            float px = wp_x ? *wp_x : 0.5f;
            float py = 1.0f - (wp_y ? *wp_y : 0.5f);
            verts[nv++]=(color_vert_t){
                px, py,
                wp_r  ? *wp_r  : br,
                wp_g  ? *wp_g  : bg,
                wp_b_ ? *wp_b_ : bb,
                fminf(wp_a ? *wp_a : ba, 1)
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
        md_lexer_t *pf_lex = ctx->lex_shape_pf[si];
        int inst=cs->num_inst;
        if (inst<1) inst=1;
        if (inst>1024) inst=1024;
        /*
         * Pin all shape variable pointers once outside the instance loop.
         * md_var_get is now O(1) with the hash table, but doing 20 lookups
         * × up to 1024 instances is still wasteful.  One memcpy + pin
         * outside, then direct pointer reads inside.
         */
        md_var_ctx_t svc;
        memcpy(&svc, &ctx->pf_vars, sizeof(md_var_ctx_t));
        /* seed all shape variables so the hash slots are allocated */
        md_var_set(&svc,"x",cs->x);           md_var_set(&svc,"y",cs->y);
        md_var_set(&svc,"rad",cs->rad);        md_var_set(&svc,"ang",cs->ang);
        md_var_set(&svc,"sides",(float)cs->sides);
        md_var_set(&svc,"r",cs->r);    md_var_set(&svc,"g",cs->g);
        md_var_set(&svc,"b",cs->b);    md_var_set(&svc,"a",cs->a);
        md_var_set(&svc,"r2",cs->r2);  md_var_set(&svc,"g2",cs->g2);
        md_var_set(&svc,"b2",cs->b2);  md_var_set(&svc,"a2",cs->a2);
        md_var_set(&svc,"border_r",cs->border_r); md_var_set(&svc,"border_g",cs->border_g);
        md_var_set(&svc,"border_b",cs->border_b); md_var_set(&svc,"border_a",cs->border_a);
        md_var_set(&svc,"additive",(float)cs->additive);
        md_var_set(&svc,"thick",(float)cs->thickOutline);
        md_var_set(&svc,"instance",0); md_var_set(&svc,"instances",(float)inst);
        /* pin output pointers — valid for the lifetime of svc on this stack frame */
        float *sp_x        = md_var_get(&svc,"x");        float *sp_y   = md_var_get(&svc,"y");
        float *sp_rad      = md_var_get(&svc,"rad");       float *sp_ang = md_var_get(&svc,"ang");
        float *sp_sides    = md_var_get(&svc,"sides");
        float *sp_r        = md_var_get(&svc,"r");         float *sp_g   = md_var_get(&svc,"g");
        float *sp_b        = md_var_get(&svc,"b");         float *sp_a   = md_var_get(&svc,"a");
        float *sp_r2       = md_var_get(&svc,"r2");        float *sp_g2  = md_var_get(&svc,"g2");
        float *sp_b2       = md_var_get(&svc,"b2");        float *sp_a2  = md_var_get(&svc,"a2");
        float *sp_br       = md_var_get(&svc,"border_r");  float *sp_bg  = md_var_get(&svc,"border_g");
        float *sp_bb       = md_var_get(&svc,"border_b");  float *sp_ba  = md_var_get(&svc,"border_a");
        float *sp_add      = md_var_get(&svc,"additive");  float *sp_thk = md_var_get(&svc,"thick");
        float *sp_inst     = md_var_get(&svc,"instance");
        for (int ii=0; ii<inst; ii++) {
            /* update only the two per-instance inputs */
            if (sp_inst) *sp_inst = (float)ii;
            /* reset shape vars to preset defaults each instance
             * (the per-frame code may modify them) */
            if (sp_x)   { *sp_x   = cs->x;   }       if (sp_y)   { *sp_y   = cs->y;   }
            if (sp_rad) { *sp_rad = cs->rad;  }       if (sp_ang) { *sp_ang = cs->ang; }
            if (sp_r)   { *sp_r   = cs->r;   }       if (sp_g)   { *sp_g   = cs->g;   }
            if (sp_b)   { *sp_b   = cs->b;   }       if (sp_a)   { *sp_a   = cs->a;   }
            if (sp_r2)  { *sp_r2  = cs->r2;  }       if (sp_g2)  { *sp_g2  = cs->g2;  }
            if (sp_b2)  { *sp_b2  = cs->b2;  }       if (sp_a2)  { *sp_a2  = cs->a2;  }
            if (sp_br)  { *sp_br  = cs->border_r; }  if (sp_bg)  { *sp_bg  = cs->border_g; }
            if (sp_bb)  { *sp_bb  = cs->border_b; }  if (sp_ba)  { *sp_ba  = cs->border_a; }
            if (sp_add) { *sp_add = (float)cs->additive; }
            if (sp_thk) { *sp_thk = (float)cs->thickOutline; }
            if (pf_lex) md_eval_precompiled(pf_lex, &svc);
            float shx = (sp_x   ? *sp_x   : cs->x)   * (float)ctx->gl.tex_w;
            float shy = (1.0f - (sp_y   ? *sp_y   : cs->y)) * (float)ctx->gl.tex_h;
            float shr = (sp_rad ? *sp_rad : cs->rad) * fminf(ctx->gl.tex_w,ctx->gl.tex_h)*0.5f;
            float sha = sp_ang   ? *sp_ang   : cs->ang;
            int   shs = sp_sides ? (int)*sp_sides : cs->sides;
            gl_draw_shape(ctx, shx, shy, shr, sha, shs,
                sp_r  ? *sp_r  : cs->r,   sp_g  ? *sp_g  : cs->g,
                sp_b  ? *sp_b  : cs->b,   sp_a  ? *sp_a  : cs->a,
                sp_r2 ? *sp_r2 : cs->r2,  sp_g2 ? *sp_g2 : cs->g2,
                sp_b2 ? *sp_b2 : cs->b2,  sp_a2 ? *sp_a2 : cs->a2,
                sp_br ? *sp_br : cs->border_r, sp_bg ? *sp_bg : cs->border_g,
                sp_bb ? *sp_bb : cs->border_b, sp_ba ? *sp_ba : cs->border_a,
                sp_add ? (int)*sp_add : cs->additive,
                sp_thk ? (int)*sp_thk : cs->thickOutline);
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
    /* Cap echo alpha: values near 1.0 inject energy faster than the warp
     * drain can remove it, causing white-out.  0.85 keeps the echo effect
     * visually prominent while leaving headroom for the drain to dominate. */
    if (ea > 0.85f) ea = 0.85f;

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
    glUniform1i(ctx->gl.u_echo_tex,    0);
    glUniform1f(ctx->gl.u_echo_alpha,  ea);
    glUniform1f(ctx->gl.u_echo_zoom,   ez);
    glUniform1i(ctx->gl.u_echo_orient, eo);
    gl_draw_fullscreen_quad(ctx);
    glDisable(GL_BLEND);
}

/* ── post-process pass ───────────────────────────────────────── */

static void gl_post_process(md_ctx_t *ctx)
{
    md_var_ctx_t *vc = &ctx->pf_vars;
    float gam = md_var_read(vc,"gamma");
    /* Cap gamma: unbounded values from per-frame code drive mid-tones to
     * white in the feedback loop.  2.5 is already very bright (equivalent
     * to a strong monitor gamma boost) and sufficient for any preset. */
    if (gam > 2.5f) gam = 2.5f;
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
    glUniform1i(ctx->gl.u_post_tex,      0);
    glUniform1f(ctx->gl.u_post_gamma,    gam);
    glUniform1i(ctx->gl.u_post_brighten, br);
    glUniform1i(ctx->gl.u_post_darken,   dk);
    glUniform1i(ctx->gl.u_post_solarize, sol);
    glUniform1i(ctx->gl.u_post_invert,   inv);
    gl_draw_fullscreen_quad(ctx);
}

/* ── main render frame ───────────────────────────────────────── */

void md_render_frame(md_ctx_t *ctx)
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
            ctx->preset_serial++; /* invalidate pre-lexed cache */
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

    /* Rebuild pre-lexed token streams whenever the preset changes */
    if (ctx->lex_serial != ctx->preset_serial)
        md_lex_cache_rebuild(ctx);

    if (!ctx->pf_init_done && ctx->lex_per_frame_init) {
        md_eval_precompiled(ctx->lex_per_frame_init, &ctx->pf_vars);
        md_readback_pf_vars(ctx); ctx->pf_init_done=1;
    }
    if (ctx->lex_per_frame) {
        md_eval_precompiled(ctx->lex_per_frame, &ctx->pf_vars);
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

void gl_init_resources(md_ctx_t *ctx)
{
    md_gl_t *g = &ctx->gl;

    /* compile shaders */
    g->prog_warp   = gl_create_program(WARP_VS, WARP_FS);
    g->prog_post   = gl_create_program(QUAD_VS, POST_FS);
    g->prog_echo   = gl_create_program(QUAD_VS, ECHO_FS);
    g->prog_darken = gl_create_program(QUAD_VS, DARKEN_FS);
    g->prog_color  = gl_create_program(COLOR_VS, COLOR_FS);
    g->prog_blit   = gl_create_program(QUAD_VS, BLIT_FS);

    /* cache uniform locations — avoids a hash lookup every frame */
    g->u_warp_tex      = glGetUniformLocation(g->prog_warp,   "uTex");
    g->u_warp_decay    = glGetUniformLocation(g->prog_warp,   "uDecay");
    g->u_post_tex      = glGetUniformLocation(g->prog_post,   "uTex");
    g->u_post_gamma    = glGetUniformLocation(g->prog_post,   "uGamma");
    g->u_post_brighten = glGetUniformLocation(g->prog_post,   "uBrighten");
    g->u_post_darken   = glGetUniformLocation(g->prog_post,   "uDarken");
    g->u_post_solarize = glGetUniformLocation(g->prog_post,   "uSolarize");
    g->u_post_invert   = glGetUniformLocation(g->prog_post,   "uInvert");
    g->u_echo_tex      = glGetUniformLocation(g->prog_echo,   "uTex");
    g->u_echo_alpha    = glGetUniformLocation(g->prog_echo,   "uAlpha");
    g->u_echo_zoom     = glGetUniformLocation(g->prog_echo,   "uZoom");
    g->u_echo_orient   = glGetUniformLocation(g->prog_echo,   "uOrient");
    g->u_darken_aspect = glGetUniformLocation(g->prog_darken, "uAspect");
    g->u_blit_tex      = glGetUniformLocation(g->prog_blit,   "uTex");

    /* persistent scratch buffers — allocated once, reused every frame */
    g->mesh_verts   = malloc(MESH_FLOAT_COUNT * sizeof(float));
    g->shape_scratch = malloc(SHAPE_SCRATCH_VERTS * sizeof(color_vert_t));
    g->wave_verts    = malloc(MAX_WAVE_VERTS * sizeof(color_vert_t));

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
    if (evemon_debug)
        fprintf(stderr,"milkdrop: GtkGLArea FBO = %u\n", g->gtk_fbo);

    g->gl_ready = 1;
    if (evemon_debug) {
        fprintf(stderr,"milkdrop: GL init OK  warp=%u post=%u echo=%u darken=%u color=%u blit=%u\n",
                g->prog_warp, g->prog_post, g->prog_echo, g->prog_darken,
                g->prog_color, g->prog_blit);
        GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        fprintf(stderr,"milkdrop: FBO status=0x%x (want 0x%x)\n", fbs, GL_FRAMEBUFFER_COMPLETE);
    }
}

