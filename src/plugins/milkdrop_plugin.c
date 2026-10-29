#include "milkdrop_internal.h"
#include "../app/runtime.h"

static const char *_md_deps[] = { "org.evemon.audio_service", NULL };

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.milkdrop",
    "MilkDrop",
    "1.0",
    EVEMON_ROLE_PROCESS,
    "org.evemon.audio_service", NULL
);

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
#ifndef EVEMON_QT_MILKDROP
    if (ctx->gl_area && GTK_IS_WIDGET(ctx->gl_area))
        gtk_widget_queue_draw(ctx->gl_area);
#endif
}

static void md_stop_capture(md_ctx_t *ctx)
{
    ctx->running=0; ctx->capture_started=0;
    /* Remove only our own meter streams — the meter infrastructure is
     * shared across all plugin instances. */
    if (ctx->host && ctx->host->pw_meter_remove_nodes &&
        ctx->audio_node_count > 0)
        ctx->host->pw_meter_remove_nodes(ctx->host->host_ctx,
            ctx->audio_node_ids, ctx->audio_node_count);
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

#ifndef EVEMON_QT_MILKDROP
static GtkWidget *md_create_widget(void *opaque)
{
    md_ctx_t *ctx = opaque;

    if (!ctx->lib_loaded) {
        /* GLib RNG self-seeds; no srand() needed */
        char preset_path[4096];
        if (evemon_data_path("milkdrop/presets", preset_path, sizeof(preset_path)) == 0)
            md_scan_dir(&ctx->lib, preset_path, 0);
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
    /* Let mouse/keyboard events pass through the overlay to the GL area */
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), info_area, TRUE);

    /* stash overlay refs so fullscreen can reparent them */
    ctx->fs_overlay = overlay;
    ctx->fs_info_area = info_area;

    ctx->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(ctx->main_box), overlay, TRUE, TRUE, 0);
    ctx->embed_parent = ctx->main_box;
    /* A host switch destroys the widget hierarchy before the plugin. Keep
     * teardown pointers weak so GL cleanup never inspects finalized widgets. */
    g_object_add_weak_pointer(G_OBJECT(ctx->gl_area), (gpointer *)&ctx->gl_area);
    g_object_add_weak_pointer(G_OBJECT(ctx->fs_info_area), (gpointer *)&ctx->fs_info_area);
    g_object_add_weak_pointer(G_OBJECT(ctx->fs_overlay), (gpointer *)&ctx->fs_overlay);
    g_object_add_weak_pointer(G_OBJECT(ctx->main_box), (gpointer *)&ctx->main_box);
    g_object_add_weak_pointer(G_OBJECT(ctx->embed_parent), (gpointer *)&ctx->embed_parent);

    gtk_widget_show_all(ctx->main_box);
    return ctx->main_box;
}

#endif

/* Forward declaration for event bus callback (defined below md_activate) */
static void md_on_album_art_event(const evemon_event_t *event,
                                  void *user_data);

static void md_activate(void *opaque, const evemon_host_services_t *svc)
{
    md_ctx_t *ctx = (md_ctx_t*)opaque;
    ctx->host = svc;
    /* Subscribe to album art events from the audio service plugin */
    if (svc->subscribe)
        ctx->event_sub_id = svc->subscribe(svc->host_ctx,
                       EVEMON_EVENT_ALBUM_ART_UPDATED,
                       md_on_album_art_event, ctx);
}

/* ── event bus callback for album art ────────────────────────────── */

/*
 * Called on the GTK main thread when the audio service publishes
 * an album art update.  We stash the metadata and pixbuf for the
 * GL overlay renderer.
 */
static void md_on_album_art_event(const evemon_event_t *event,
                                  void *user_data)
{
    md_ctx_t *ctx = user_data;
    const evemon_album_art_payload_t *art = event->payload;
    if (!art) return;

    /* Ignore events for a different PID (prevents cross-pollination
     * between the main panel and pinned detail panels). */
    if (art->source_pid > 0 && ctx->last_pid > 0 &&
        art->source_pid != ctx->last_pid)
        return;

    /* Update MPRIS metadata fields */
    snprintf(ctx->mpris_title, sizeof(ctx->mpris_title),
             "%s", art->track_title);
    snprintf(ctx->mpris_artist, sizeof(ctx->mpris_artist),
             "%s", art->track_artist);
    snprintf(ctx->mpris_album, sizeof(ctx->mpris_album),
             "%s", art->track_album);
    snprintf(ctx->mpris_status, sizeof(ctx->mpris_status),
             "%s", art->playback_status);
    ctx->mpris_position_us = art->position_us;
    ctx->mpris_length_us   = art->length_us;
    ctx->mpris_available   = (art->track_title[0] || art->playback_status[0]) ? 1 : 0;

    /* Update album art pixbuf */
    if (ctx->mpris_art_pixbuf)
        g_object_unref(ctx->mpris_art_pixbuf);
    ctx->mpris_art_pixbuf = art->pixbuf ? g_object_ref(art->pixbuf) : NULL;
    snprintf(ctx->mpris_art_url, sizeof(ctx->mpris_art_url),
             "%s", art->art_url);

    /* Also update the overlay audio source from MPRIS
     * (richer than PipeWire's media_name) */
    if (art->track_title[0]) {
        if (art->track_artist[0])
            snprintf(ctx->audio_media_name,
                     sizeof(ctx->audio_media_name),
                     "%s \xe2\x80\x94 %s", art->track_artist,
                     art->track_title);
        else
            snprintf(ctx->audio_media_name,
                     sizeof(ctx->audio_media_name),
                     "%s", art->track_title);
    }

    /* Trigger a redraw so the new art appears */
#ifndef EVEMON_QT_MILKDROP
    if (ctx->gl_area && GTK_IS_WIDGET(ctx->gl_area))
        gtk_widget_queue_draw(ctx->gl_area);
#endif
}

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
#ifndef EVEMON_QT_MILKDROP
        if (ctx->main_box && ctx->is_active) {
            GtkWidget *nb = gtk_widget_get_parent(ctx->main_box);
            if (nb && GTK_IS_NOTEBOOK(nb)) {
                gtk_widget_hide(ctx->main_box);
                GtkWidget *tab = gtk_notebook_get_tab_label(
                    GTK_NOTEBOOK(nb), ctx->main_box);
                if (tab) gtk_widget_hide(tab);
            }
        }
#endif
        return;
    }
    size_t old=ctx->audio_node_count; ctx->audio_node_count=0;
    for (size_t i=0; i<data->pw_node_count; i++) {
        const evemon_pw_node_t *nd=&data->pw_nodes[i];
        if (nd->pid!=data->pid) continue;
        /* Skip evemon's own capture/meter streams — they appear in
         * the PW graph as "evemon-meter-*" or "evemon-spectrogram"
         * and confuse stream selection for the real audio source. */
        if (strncmp(nd->node_name, "evemon-", 7) == 0) continue;
        if (strncmp(nd->app_name, "evemon", 6) == 0) continue;
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
#ifndef EVEMON_QT_MILKDROP
        if (ctx->main_box && ctx->is_active) {
            GtkWidget *nb = gtk_widget_get_parent(ctx->main_box);
            if (nb && GTK_IS_NOTEBOOK(nb)) {
                gtk_widget_show(ctx->main_box);
                GtkWidget *tab = gtk_notebook_get_tab_label(
                    GTK_NOTEBOOK(nb), ctx->main_box);
                if (tab) gtk_widget_show(tab);
            }
        }
#endif
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

    /* ── MPRIS metadata now arrives via the event bus ────────── */
    /* Just clear the flag when no MPRIS data is present;
     * md_on_album_art_event() handles all metadata updates. */
    if (!data->mpris_players || data->mpris_player_count == 0)
        ctx->mpris_available = 0;
}

static void md_clear(void *opaque)
{
    md_ctx_t *ctx=opaque;
    md_stop_capture(ctx);
    if (ctx->audio_timer) { g_source_remove(ctx->audio_timer); ctx->audio_timer=0; }
    ctx->audio_node_count=0; ctx->last_pid=0;
#ifndef EVEMON_QT_MILKDROP
    if (ctx->gl_area && GTK_IS_WIDGET(ctx->gl_area))
        gtk_widget_queue_draw(ctx->gl_area);
#endif
    /* Hide the notebook tab */
#ifndef EVEMON_QT_MILKDROP
    if (ctx->main_box && ctx->is_active) {
        GtkWidget *nb = gtk_widget_get_parent(ctx->main_box);
        if (nb && GTK_IS_NOTEBOOK(nb)) {
            gtk_widget_hide(ctx->main_box);
            GtkWidget *tab = gtk_notebook_get_tab_label(
                GTK_NOTEBOOK(nb), ctx->main_box);
            if (tab) gtk_widget_hide(tab);
        }
    }
#endif
}

#ifndef EVEMON_QT_MILKDROP
static void md_destroy(void *opaque)
{
    md_ctx_t *ctx=opaque;

    /* Unsubscribe from the event bus BEFORE freeing ctx,
     * otherwise a deferred album art event will invoke the
     * callback with a dangling user_data pointer → segfault. */
    if (ctx->event_sub_id > 0 && ctx->host && ctx->host->unsubscribe) {
        ctx->host->unsubscribe(ctx->host->host_ctx, ctx->event_sub_id);
        ctx->event_sub_id = 0;
    }

    if (ctx->fullscreen && ctx->fs_window) {
        /* reparent back before destroying window */
        if (ctx->embed_parent && ctx->fs_overlay) {
            g_object_ref(ctx->fs_overlay);
            gtk_container_remove(GTK_CONTAINER(ctx->fs_window), ctx->fs_overlay);
            gtk_box_pack_start(GTK_BOX(ctx->embed_parent), ctx->fs_overlay,
                               TRUE, TRUE, 0);
            g_object_unref(ctx->fs_overlay);
        }
        gtk_widget_destroy(ctx->fs_window);
        ctx->fs_window = NULL;
        ctx->fullscreen = 0;
    }
    md_stop_capture(ctx);
    if (ctx->redraw_timer) { g_source_remove(ctx->redraw_timer); ctx->redraw_timer=0; }
    if (ctx->audio_timer) { g_source_remove(ctx->audio_timer); ctx->audio_timer=0; }
    if (ctx->mpris_art_pixbuf) {
        g_object_unref(ctx->mpris_art_pixbuf);
        ctx->mpris_art_pixbuf = NULL;
    }

    /* Force GL cleanup while ctx is still valid and the GL context is
     * accessible.  plugin_registry_destroy() calls md_destroy() before the
     * GTK window hierarchy is torn down, so on_unrealize() has not fired
     * yet but ctx is about to be freed.  Without this:
     *   • glDeleteTextures() is never called → the two 1024×768 FBO
     *     textures (~6 MB of Mesa anonymous mmaps) are never munmap'd.
     *   • glDeleteFramebuffers/Programs/Buffers/VertexArrays are skipped,
     *     leaking GPU objects.
     *   • mesh_verts / shape_scratch / wave_verts (CPU scratch) leak.
     * If on_unrealize() already ran (gl_ready==0) this block is skipped
     * and the subsequent g_signal_handlers_disconnect_by_data() is still
     * needed to prevent the pending GTK unrealize from reading freed ctx. */
    if (ctx->gl_area && GTK_IS_WIDGET(ctx->gl_area) && ctx->gl.gl_ready)
        on_unrealize(GTK_GL_AREA(ctx->gl_area), ctx); /* sets gl_ready = 0 */

    /* Disconnect every widget signal that carries ctx as user_data.
     * After free(ctx) below, any GTK signal (unrealize, render, draw,
     * key-press …) would call its handler with a dangling pointer.
     * g_signal_handlers_disconnect_by_data() is O(#signals) and safe to
     * call even if the widget has already been finalized. */
    if (ctx->gl_area && GTK_IS_WIDGET(ctx->gl_area))
        g_signal_handlers_disconnect_by_data(ctx->gl_area, ctx);
    if (ctx->fs_info_area && GTK_IS_WIDGET(ctx->fs_info_area))
        g_signal_handlers_disconnect_by_data(ctx->fs_info_area, ctx);

#ifndef EVEMON_QT_MILKDROP
    if (ctx->gl_area) g_object_remove_weak_pointer(G_OBJECT(ctx->gl_area), (gpointer *)&ctx->gl_area);
    if (ctx->fs_info_area) g_object_remove_weak_pointer(G_OBJECT(ctx->fs_info_area), (gpointer *)&ctx->fs_info_area);
    if (ctx->fs_overlay) g_object_remove_weak_pointer(G_OBJECT(ctx->fs_overlay), (gpointer *)&ctx->fs_overlay);
    if (ctx->main_box) g_object_remove_weak_pointer(G_OBJECT(ctx->main_box), (gpointer *)&ctx->main_box);
    if (ctx->embed_parent) g_object_remove_weak_pointer(G_OBJECT(ctx->embed_parent), (gpointer *)&ctx->embed_parent);
#endif

    free(ctx->saved_preset);
    md_lex_cache_free(ctx);
    md_lib_free(&ctx->lib);
    free(ctx);
}

/* ── plugin descriptor ───────────────────────────────────────── */

static void md_set_active(void *opaque, int active)
{
    md_ctx_t *ctx = opaque;
    if (!ctx) return;
    ctx->is_active = active;
}

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    md_ctx_t *ctx = calloc(1, sizeof(md_ctx_t));
    if (!ctx) return NULL;
    ctx->effects_enabled = 1;
    ctx->blend_time_user = 2.5f;
    ctx->preset_duration = 30.0f;  /* auto-advance every 30s by default */
    ctx->prev_preset_idx = -1;
    ctx->show_nowplaying = 3;       /* full now-playing by default */
    ctx->lex_serial      = -1;     /* force lex cache build on first frame */
    md_favorites_load(ctx);         /* restore favorites from disk */

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "MilkDrop",
        .id            = "org.evemon.milkdrop",
        .version       = "1.0",
        .data_needs    = evemon_NEED_PIPEWIRE | evemon_NEED_MPRIS,
        .plugin_ctx    = ctx,
        .create_widget = md_create_widget,
        .update        = md_update,
        .clear         = md_clear,
        .destroy       = md_destroy,
        .activate      = md_activate,
        .set_active    = md_set_active,
        .role          = EVEMON_ROLE_PROCESS,
        .dependencies  = _md_deps,
    };
    return p;
}

#else
#include "../qt/milkdrop_engine.inc"
#endif
