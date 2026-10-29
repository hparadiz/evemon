#include "milkdrop_internal.h"

#ifndef EVEMON_QT_MILKDROP
/* Forward declaration — defined after md_toggle_fullscreen */
static void md_resize_fbos(md_ctx_t *ctx, int w, int h);

static gboolean redraw_tick(gpointer data)
{
    md_ctx_t *ctx = data;
    if (ctx->gl_area && GTK_IS_WIDGET(ctx->gl_area) &&
        gtk_widget_get_realized(ctx->gl_area))
        gtk_widget_queue_draw(ctx->gl_area);
    return G_SOURCE_CONTINUE;
}

void on_realize(GtkGLArea *area, gpointer data)
{
    (void)area;
    md_ctx_t *ctx = data;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) {
        if (evemon_debug)
            fprintf(stderr,"milkdrop: GtkGLArea error on realize\n");
        return;
    }
    gl_init_resources(ctx);
    /* 60 Hz redraw timer so rendering keeps going */
    if (!ctx->redraw_timer)
        ctx->redraw_timer = g_timeout_add(16, redraw_tick, ctx);
}

gboolean on_render(GtkGLArea *area, GdkGLContext *glctx, gpointer data)
{
    (void)glctx;
    md_ctx_t *ctx = data;
    if (!ctx->gl.gl_ready) return FALSE;

    /* Apply a deferred FBO resize (triggered by fullscreen toggle).  We do
     * this here because on_render is invoked with a valid, current GL context
     * already bound by GtkGLArea — safe to issue GL calls. */
    if (ctx->pending_fbo_w > 0 && ctx->pending_fbo_h > 0) {
        md_resize_fbos(ctx, ctx->pending_fbo_w, ctx->pending_fbo_h);
        ctx->pending_fbo_w = 0;
        ctx->pending_fbo_h = 0;
    }

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
    glUniform1i(ctx->gl.u_blit_tex, 0);
    gl_draw_fullscreen_quad(ctx);

    if (evemon_debug && ctx->dbg_frame_count < 5) {
        ctx->dbg_frame_count++;
        GLenum err = glGetError();
        fprintf(stderr, "milkdrop: frame %d  gtk_fbo=%u display_fbo=%d tex=%u err=0x%x\n",
                ctx->dbg_frame_count, gtk_fbo, display_fbo,
                ctx->gl.fbo_tex[display_fbo], err);
    }
    if (!ctx->first_render_done) {
        if (evemon_debug)
            fprintf(stderr, "milkdrop: first render frame, preset='%s' cur_fbo=%d\n",
                    ctx->preset.name, ctx->cur_fbo);
        ctx->first_render_done = 1;
    }

    /* ── Restore GL state so GTK/Cairo/other widgets aren't poisoned ── */
    /* Unbind our texture so the driver doesn't keep a reference alive  */
    glBindTexture(GL_TEXTURE_2D, 0);
    /* Unbind the program — GTK may use its own shaders or fixed-function */
    glUseProgram(0);
    /* Unbind our VAO/VBO so GTK's vertex state is clean */
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    /* Reset blend state to default */
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    /* Reset line width to default */
    glLineWidth(1.0f);
    /* Restore the FBO binding to what GtkGLArea expects (its own FBO) */
    glBindFramebuffer(GL_FRAMEBUFFER, gtk_fbo);

    /* request next frame */
    if (GTK_IS_WIDGET(area))
        gtk_widget_queue_draw(GTK_WIDGET(area));
    return TRUE;
}

void on_unrealize(GtkGLArea *area, gpointer data)
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
    /* free persistent scratch buffers */
    free(g->mesh_verts);    g->mesh_verts    = NULL;
    free(g->shape_scratch); g->shape_scratch = NULL;
    free(g->wave_verts);    g->wave_verts    = NULL;
    /* Release GDK's thread-local "current context" reference.
     *
     * gtk_gl_area_make_current() (above) causes GDK to store the
     * GdkGLContext in a thread-local slot and increment its refcount.
     * GtkGLArea's own unrealize handler (which runs after this signal
     * handler) drops GtkGLArea's reference via g_clear_object().  But
     * if GDK's thread-local ref is still held, the GdkGLContext refcount
     * never reaches zero — Mesa does NOT destroy its driver context, and
     * the ~100-180 MB of anonymous mmaps used by the LLVM JIT, gallium
     * state tracker, and shader compiler are never munmap'd.
     *
     * Clearing the current context here makes g_clear_object() in
     * GtkGLArea::unrealize the last reference drop, which triggers
     * GdkGLContext finalization → Mesa context teardown → munmap. */
    gdk_gl_context_clear_current();
    g->gl_ready = 0;
}

#endif /* GTK GL surface */

/* ── mashup helpers ───────────────────────────────────────────── */

/* Mini-mashup: randomize wave colors and motion from another preset */
static void md_mini_mashup(md_ctx_t *ctx, int save)
{
    if (ctx->lib.count < 2) return;
    if (save && !ctx->has_saved_preset) {
        if (!ctx->saved_preset)
            ctx->saved_preset = malloc(sizeof(md_preset_t));
        if (ctx->saved_preset) {
            *ctx->saved_preset = ctx->preset;
            ctx->has_saved_preset = 1;
        }
    }
    int donor = g_random_int_range(0, ctx->lib.count);
    md_preset_t tmp; memset(&tmp, 0, sizeof(tmp));
    if (md_load_preset(&tmp, ctx->lib.paths[donor])) return;
    /* mix in wave colors, motion, and a few interesting params.
     * If the donor's wave color is too dark/gray/white, replace it with
     * a vivid HSV-derived color so the mashup is always visually distinct. */
    {
        float dr = tmp.wave_r, dg = tmp.wave_g, db = tmp.wave_b;
        /* luminance and saturation check (BT.601) */
        float lum  = 0.299f*dr + 0.587f*dg + 0.114f*db;
        float cmax = fmaxf(dr, fmaxf(dg, db));
        float cmin = fminf(dr, fminf(dg, db));
        float sat  = (cmax > 0.001f) ? (cmax - cmin) / cmax : 0.0f;
        if (lum < 0.15f || lum > 0.88f || sat < 0.20f) {
            /* donor color is too dark, too bright, or too gray — generate vivid */
            md_random_vivid_color(0.55f, 0.45f, 0.95f, &dr, &dg, &db);
        }
        ctx->preset.wave_r = dr;
        ctx->preset.wave_g = dg;
        ctx->preset.wave_b = db;
    }
    ctx->preset.dx = tmp.dx; ctx->preset.dy = tmp.dy;
    ctx->preset.rot = tmp.rot; ctx->preset.sx = tmp.sx; ctx->preset.sy = tmp.sy;
    show_info(ctx, "Mini-mashup from: %s", tmp.name);
}

/* Deep mashup: replace warp, shapes, and post-process from donor */
static void md_deep_mashup(md_ctx_t *ctx, int save)
{
    if (ctx->lib.count < 2) return;
    if (save && !ctx->has_saved_preset) {
        if (!ctx->saved_preset)
            ctx->saved_preset = malloc(sizeof(md_preset_t));
        if (ctx->saved_preset) {
            *ctx->saved_preset = ctx->preset;
            ctx->has_saved_preset = 1;
        }
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
    if (!ctx->has_saved_preset || !ctx->saved_preset)
        { show_info(ctx, "No mashup to undo"); return; }
    ctx->preset = *ctx->saved_preset;
    free(ctx->saved_preset); ctx->saved_preset = NULL;
    ctx->has_saved_preset = 0;
    ctx->preset_serial++;
    show_info(ctx, "Mashup undone");
}

#ifndef EVEMON_QT_MILKDROP
/* ── signal handlers ─────────────────────────────────────────── */

gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    md_ctx_t *ctx = data;
    if (ev->type==GDK_2BUTTON_PRESS && ev->button==1) {
        int idx=g_random_int_range(0,ctx->lib.count?ctx->lib.count:1);
        md_load_preset_idx(ctx, idx, 1);
        show_info(ctx,"[%d/%d] %s",ctx->preset_idx+1,ctx->lib.count,
                  ctx->lib.names[ctx->preset_idx]);
    }
    /* Always force keyboard focus onto the GL area and stop propagation
     * so that the notebook / parent containers don't steal it back. */
    gtk_widget_grab_focus(w);
    return TRUE;
}

/* ── fullscreen support ──────────────────────────────────────── */

/* Forward declarations */
static void md_toggle_fullscreen(md_ctx_t *ctx);
gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data);

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
    if (!ctx->gl_area || !GTK_IS_WIDGET(ctx->gl_area)) return;
    if (!gtk_widget_get_realized(ctx->gl_area)) return;
    if (gtk_gl_area_get_error(GTK_GL_AREA(ctx->gl_area))) return;

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
    if (evemon_debug)
        fprintf(stderr, "milkdrop: FBO resized to %dx%d\n", w, h);
}

static void md_toggle_fullscreen(md_ctx_t *ctx)
{
    if (!ctx->fs_overlay) return;
    /* Prevent re-entrant toggle (e.g. rapid F-key presses before the WM
     * has processed the previous fullscreen request). */
    if (ctx->fs_toggle_pending) return;
    ctx->fs_toggle_pending = 1;

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
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_window_set_wmclass(GTK_WINDOW(ctx->fs_window), "evemon", "evemon");
        G_GNUC_END_IGNORE_DEPRECATIONS
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

        /* Defer the FBO resize to the next on_render call so we don't
         * race with the unrealize/realize cycle caused by reparenting. */
        ctx->pending_fbo_w = ctx->fs_width;
        ctx->pending_fbo_h = ctx->fs_height;

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

        /* Defer the FBO resize to the next on_render call so we don't
         * call gl ops while the widget may still be mid-realize. */
        ctx->pending_fbo_w = MD_TEX_W;
        ctx->pending_fbo_h = MD_TEX_H;

        show_info(ctx, "Windowed mode");
    }

    ctx->fs_toggle_pending = 0;
}

#else
static void md_toggle_fullscreen(md_ctx_t *ctx) { ctx->fullscreen = !ctx->fullscreen; }
#endif

#ifdef EVEMON_QT_MILKDROP
gboolean md_handle_key(md_ctx_t *ctx, unsigned keyval, unsigned state)
{
#else
gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w; md_ctx_t *ctx=data;

    /* If a text-input widget (GtkEntry, GtkEditable) currently has
     * keyboard focus, let it handle the keypress instead of consuming
     * it for milkdrop hotkeys.  This prevents typing in the filter /
     * PID-go-to entries from triggering visualiser commands. */
    GtkWidget *toplevel = gtk_widget_get_toplevel(w);
    if (GTK_IS_WINDOW(toplevel)) {
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(toplevel));
        if (focus && focus != w && GTK_IS_EDITABLE(focus))
            return FALSE;
    }

    unsigned keyval = ev->keyval, state = ev->state;
#endif
    int shift = (state & GDK_SHIFT_MASK) != 0;
    int ctrl  = (state & GDK_CONTROL_MASK) != 0;
    (void)ctrl;

    switch (keyval) {

    /* ── SPACE: next preset (or next favorite in fav_cycle mode) */
    case GDK_KEY_space:
        if (ctx->fav_cycle) {
            md_fav_cycle_next(ctx, shift ? -1 : 1);
        } else if (shift) {
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
        } else if (ctx->fav_cycle) {
            md_fav_cycle_next(ctx, 1);
        } else {
            ctx->prev_preset_idx = ctx->preset_idx;
            md_load_preset_idx(ctx, ctx->preset_idx + 1, 1);
            show_info(ctx, "[%d/%d] %s", ctx->preset_idx+1,
                      ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        }
        return TRUE;

    /* ── P: previous preset ──────────────────────────────────── */
    case GDK_KEY_p: case GDK_KEY_P:
        if (ctx->fav_cycle) {
            md_fav_cycle_next(ctx, -1);
        } else {
            ctx->prev_preset_idx = ctx->preset_idx;
            md_load_preset_idx(ctx, ctx->preset_idx - 1, 1);
            show_info(ctx, "[%d/%d] %s", ctx->preset_idx+1,
                      ctx->lib.count, ctx->lib.names[ctx->preset_idx]);
        }
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
            if (!ctx->saved_preset)
                ctx->saved_preset = malloc(sizeof(md_preset_t));
            if (ctx->saved_preset) {
                *ctx->saved_preset = ctx->preset;
                ctx->has_saved_preset = 1;
            }
        }
        /* Use HSV-based generation: saturation ≥ 0.55, value in [0.45, 0.95]
         * so we always get a vivid, legible color instead of lucky gray/white/black. */
        md_random_vivid_color(0.55f, 0.45f, 0.95f,
                              &ctx->preset.wave_r,
                              &ctx->preset.wave_g,
                              &ctx->preset.wave_b);
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

    /* ── S: Ctrl+S = save favorite; plain S = shuffle library ── */
    case GDK_KEY_s: case GDK_KEY_S:
        if (ctrl) {
            md_fav_save_current(ctx);
        } else {
            md_lib_shuffle(&ctx->lib);
            show_info(ctx, "Library shuffled (%d presets)", ctx->lib.count);
        }
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

    /* ── M: Ctrl+M = fav menu; plain/Shift = now-playing ─────── */
    case GDK_KEY_m:
        if (ctrl) {
            ctx->show_fav_menu = !ctx->show_fav_menu;
            if (ctx->show_fav_menu) {
                ctx->fav_menu_sel = 0;
                /* pre-select the currently-playing favorite if any */
                for (int i = 0; i < MD_FAV_SLOTS; i++) {
                    if (ctx->favorites[i].used &&
                        strcmp(ctx->favorites[i].snap.filepath,
                               ctx->preset.filepath) == 0)
                        { ctx->fav_menu_sel = i; break; }
                }
            }
        } else {
            ctx->show_nowplaying = (ctx->show_nowplaying + 1) % 4;
            static const char *modes[] = {
                "OFF", "Title only", "Title + Artist + Album", "Full"
            };
            show_info(ctx, "Now Playing: %s", modes[ctx->show_nowplaying]);
        }
        return TRUE;
    case GDK_KEY_M:
        if (ctrl) {
            ctx->show_fav_menu = !ctx->show_fav_menu;
        } else {
            /* Shift+M: jump straight to full */
            ctx->show_nowplaying = 3;
            show_info(ctx, "Now Playing: Full");
        }
        return TRUE;

    /* ── F: Ctrl+F = fav-cycle mode; plain F = fullscreen ────── */
    case GDK_KEY_f: case GDK_KEY_F:
        if (ctrl) {
            ctx->fav_cycle = !ctx->fav_cycle;
            show_info(ctx, "Favorites-only cycle: %s",
                      ctx->fav_cycle ? "ON (Space/N/P cycle favs)" : "OFF");
        } else {
            md_toggle_fullscreen(ctx);
        }
        return TRUE;

    /* ── Favorites menu navigation (active when menu is open) ── */
    case GDK_KEY_Up:
        if (ctx->show_fav_menu) {
            /* skip unused slots going upward */
            int s = ctx->fav_menu_sel - 1;
            while (s >= 0 && !ctx->favorites[s].used) s--;
            if (s >= 0) ctx->fav_menu_sel = s;
            return TRUE;
        }
        return FALSE;
    case GDK_KEY_Down:
        if (ctx->show_fav_menu) {
            int s = ctx->fav_menu_sel + 1;
            while (s < MD_FAV_SLOTS && !ctx->favorites[s].used) s++;
            if (s < MD_FAV_SLOTS) ctx->fav_menu_sel = s;
            return TRUE;
        }
        return FALSE;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
        if (ctx->show_fav_menu) {
            md_fav_recall(ctx, ctx->fav_menu_sel);
            ctx->show_fav_menu = 0;
            return TRUE;
        }
        return FALSE;
    case GDK_KEY_Delete:
        if (ctx->show_fav_menu) {
            int prev = ctx->fav_menu_sel;
            md_fav_delete(ctx, ctx->fav_menu_sel);
            /* keep cursor on nearest remaining entry */
            while (prev > 0 && !ctx->favorites[prev].used) prev--;
            ctx->fav_menu_sel = prev;
            return TRUE;
        }
        return FALSE;

    /* ── Escape: close fav menu, then exit fullscreen ─────────── */
    case GDK_KEY_Escape:
        if (ctx->show_fav_menu) { ctx->show_fav_menu = 0; return TRUE; }
        if (ctx->fullscreen)    { md_toggle_fullscreen(ctx); return TRUE; }
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

    default: break;
    }
    return FALSE;
}

#ifndef EVEMON_QT_MILKDROP
gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)w; md_ctx_t *ctx=data;
    if (ev->direction==GDK_SCROLL_UP) md_load_preset_idx(ctx,ctx->preset_idx+1,1);
    else if (ev->direction==GDK_SCROLL_DOWN) md_load_preset_idx(ctx,ctx->preset_idx-1,1);
    else return FALSE;
    show_info(ctx,"[%d/%d] %s",ctx->preset_idx+1,ctx->lib.count,
              ctx->lib.names[ctx->preset_idx]);
    return TRUE;
}

#endif

