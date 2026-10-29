/* Qt adapter for the existing MilkDrop evaluator and OpenGL renderer.
 * Included by milkdrop_plugin.c only for the Qt renderer module. */
#include "milkdrop_engine.h"

void *evemon_md_create(const char *directory, const evemon_host_services_t *services)
{
    md_ctx_t *ctx = calloc(1, sizeof(md_ctx_t));
    if (!ctx) return NULL;
    ctx->effects_enabled = 1;
    ctx->blend_time_user = 2.5f;
    ctx->preset_duration = 30;
    ctx->prev_preset_idx = -1;
    ctx->lex_serial = -1;
    ctx->show_nowplaying = 3;
    md_favorites_load(ctx);
    md_activate(ctx, services);
    md_scan_dir(&ctx->lib, directory, 0);
    if (ctx->lib.count) md_lib_shuffle(&ctx->lib);
    ctx->lib_loaded = 1;
    return ctx;
}
void evemon_md_release_gl(void *opaque)
{
    md_ctx_t *ctx = opaque;
    md_gl_t *g = &ctx->gl;
    if (!g->gl_ready) return;
    glDeleteProgram(g->prog_warp); glDeleteProgram(g->prog_post);
    glDeleteProgram(g->prog_echo); glDeleteProgram(g->prog_darken);
    glDeleteProgram(g->prog_color); glDeleteProgram(g->prog_blit);
    glDeleteVertexArrays(1, &g->quad_vao); glDeleteBuffers(1, &g->quad_vbo);
    glDeleteVertexArrays(1, &g->mesh_vao); glDeleteBuffers(1, &g->mesh_vbo);
    glDeleteBuffers(1, &g->mesh_ebo);
    glDeleteVertexArrays(1, &g->wave_vao); glDeleteBuffers(1, &g->wave_vbo);
    glDeleteFramebuffers(2, g->fbo); glDeleteTextures(2, g->fbo_tex);
    free(g->mesh_verts); free(g->shape_scratch); free(g->wave_verts);
    memset(g, 0, sizeof(*g));
}
void evemon_md_destroy(void *opaque)
{
    md_ctx_t *ctx = opaque;
    if (!ctx) return;
    md_clear(ctx);
    if (ctx->event_sub_id > 0 && ctx->host->unsubscribe) ctx->host->unsubscribe(ctx->host->host_ctx, ctx->event_sub_id);
    if (ctx->mpris_art_pixbuf) g_object_unref(ctx->mpris_art_pixbuf);
    md_lex_cache_free(ctx); md_lib_free(&ctx->lib); free(ctx->saved_preset); free(ctx);
}
void evemon_md_initialize(void *opaque) { gl_init_resources(opaque); }
void evemon_md_update(void *opaque, const evemon_proc_data_t *data) { md_update(opaque, data); }
void evemon_md_clear(void *opaque) { md_clear(opaque); }
void evemon_md_overlay(void *opaque, cairo_t *cr, int width, int height) { md_draw_overlay(opaque, cr, width, height); }
int evemon_md_key(void *opaque, unsigned key, unsigned modifiers)
{
    md_ctx_t *ctx = opaque;
    if (!ctx->lib.count && key != GDK_KEY_F1 && key != GDK_KEY_f && key != GDK_KEY_F && key != GDK_KEY_Escape) return 0;
    return md_handle_key(ctx, key, modifiers);
}
int evemon_md_fullscreen(void *opaque) { return ((md_ctx_t *)opaque)->fullscreen; }
void evemon_md_next(void *opaque, int delta)
{
    md_ctx_t *ctx = opaque;
    if (!ctx->lib.count) return;
    md_load_preset_idx(ctx, ctx->preset_idx + delta, 1);
    show_info(ctx, "[%d/%d] %s", ctx->preset_idx + 1, ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
}
void evemon_md_random(void *opaque)
{
    md_ctx_t *ctx = opaque;
    if (!ctx->lib.count) return;
    md_load_preset_idx(ctx, g_random_int_range(0, ctx->lib.count), 1);
    show_info(ctx, "[%d/%d] %s", ctx->preset_idx + 1, ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
}
void evemon_md_render(void *opaque, unsigned fbo, int width, int height, float dt)
{
    md_ctx_t *ctx = opaque;
    if (!ctx->gl.gl_ready) return;
    if (!ctx->running || !ctx->audio_node_count) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); return;
    }
    ctx->frame_time = fmaxf(0.001f, fminf(dt, 0.1f));
    md_analyze_sound(ctx); md_render_frame(ctx);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(ctx->gl.prog_blit); glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->gl.fbo_tex[1 - ctx->cur_fbo]);
    glUniform1i(ctx->gl.u_blit_tex, 0); gl_draw_fullscreen_quad(ctx);
    glBindTexture(GL_TEXTURE_2D, 0); glUseProgram(0); glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND); glBlendFunc(GL_ONE, GL_ZERO); glLineWidth(1);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}
