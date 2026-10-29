#include "../ipc/query.h"
#include "pipewire_internal.h"

static const char *_pw_deps[] = { "org.evemon.audio_service", NULL };

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.pipewire",
    "PipeWire",
    "1.0",
    EVEMON_ROLE_PROCESS,
    "org.evemon.audio_service", NULL
);

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *pw_create_widget(void *opaque)
{
    pw_ctx_t *ctx = opaque;

    ctx->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* ── Connection tree ──────────────────────────────────────── */
    ctx->store = gtk_list_store_new(NUM_COLS,
                                    G_TYPE_STRING, G_TYPE_STRING,
                                    G_TYPE_INT, G_TYPE_UINT,
                                    G_TYPE_INT, G_TYPE_INT);
    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    /* NOTE: we keep our own ref to ctx->store (don't unref here)
     * so that the meter timer can safely access it.  We release
     * the ref in pw_destroy(). */

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Audio", cell, "markup", COL_MARKUP, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(ctx->view, col);

    GtkCellRenderer *meter_r = pw_meter_renderer_new();
    GtkTreeViewColumn *meter_col = gtk_tree_view_column_new_with_attributes(
        "Level", meter_r,
        "level-l", COL_LEVEL_L,
        "level-r", COL_LEVEL_R,
        NULL);
    gtk_tree_view_column_set_fixed_width(meter_col, 66);
    gtk_tree_view_column_set_sizing(meter_col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_append_column(ctx->view, meter_col);

    ctx->css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(ctx->css,
        "treeview { font-family: Monospace; font-size: 8pt; }", -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(GTK_WIDGET(ctx->view)),
        GTK_STYLE_PROVIDER(ctx->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);

    g_signal_connect(ctx->view, "row-activated",
                     G_CALLBACK(on_row_activated), ctx);
    g_signal_connect(ctx->view, "button-press-event",
                     G_CALLBACK(on_pw_button_press), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);

    gtk_box_pack_start(GTK_BOX(ctx->main_box), ctx->scroll, TRUE, TRUE, 0);

    /* ── Now Playing section (MPRIS metadata) ─────────────────── */
    {
        GtkWidget *media_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *media_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(media_label),
            "<small><b>Now Playing</b></small>");
        gtk_label_set_xalign(GTK_LABEL(media_label), 0.0f);
        gtk_box_pack_start(GTK_BOX(media_box), media_label,
                           FALSE, FALSE, 2);

        GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        /* Album art — fixed 64×64, no expand */
        ctx->media_art_image = gtk_image_new();
        gtk_widget_set_size_request(ctx->media_art_image, 64, 64);
        gtk_widget_set_halign(ctx->media_art_image, GTK_ALIGN_START);
        gtk_widget_set_valign(ctx->media_art_image, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(info_box), ctx->media_art_image,
                           FALSE, FALSE, 4);

        /* Text info */
        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);

        ctx->media_title_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_title_label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(ctx->media_title_label),
                                PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(text_box), ctx->media_title_label,
                           FALSE, FALSE, 0);

        ctx->media_artist_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_artist_label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(ctx->media_artist_label),
                                PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(text_box), ctx->media_artist_label,
                           FALSE, FALSE, 0);

        ctx->media_album_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_album_label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(ctx->media_album_label),
                                PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(text_box), ctx->media_album_label,
                           FALSE, FALSE, 0);

        GtkWidget *status_pos_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        ctx->media_status_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_status_label), 0.0f);
        gtk_box_pack_start(GTK_BOX(status_pos_box), ctx->media_status_label,
                           FALSE, FALSE, 0);
        ctx->media_position_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->media_position_label), 0.0f);
        gtk_box_pack_start(GTK_BOX(status_pos_box), ctx->media_position_label,
                           FALSE, FALSE, 0);

        /* BPM indicators: trailing (~) on left, track-wide on right */
        ctx->bpm_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->bpm_label), 0.0f);
        gtk_label_set_use_markup(GTK_LABEL(ctx->bpm_label), TRUE);

        ctx->bpm_track_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->bpm_track_label), 0.0f);
        gtk_label_set_use_markup(GTK_LABEL(ctx->bpm_track_label), TRUE);

        /* Pack: [status] [position]  ...spacer...  [~trail] [track BPM] */
        gtk_box_pack_end(GTK_BOX(status_pos_box), ctx->bpm_track_label,
                         FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(status_pos_box), ctx->bpm_label,
                         FALSE, FALSE, 2);

        gtk_box_pack_start(GTK_BOX(text_box), status_pos_box,
                           FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(info_box), text_box, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(media_box), info_box, FALSE, FALSE, 2);

        /* Apply monospace style to match the tree */
        GtkCssProvider *media_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(media_css,
            "label { font-family: Sans; font-size: 8pt; }", -1, NULL);
        GtkWidget *labels[] = { ctx->media_title_label,
                                ctx->media_artist_label,
                                ctx->media_album_label,
                                ctx->media_status_label,
                                ctx->media_position_label,
                                ctx->bpm_label,
                                ctx->bpm_track_label };
        for (int i = 0; i < 7; i++)
            gtk_style_context_add_provider(
                gtk_widget_get_style_context(labels[i]),
                GTK_STYLE_PROVIDER(media_css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(media_css);

        ctx->media_section = media_box;
        gtk_widget_set_no_show_all(ctx->media_section, TRUE);
        /* Pack at the end so it stays fixed at the bottom */
        gtk_box_pack_end(GTK_BOX(ctx->main_box), ctx->media_section,
                         FALSE, FALSE, 4);
    }

    if (query_is_foreign_host()) {
        GtkWidget *notice = gtk_label_new("Remote PipeWire graph — audio capture is local only");
        gtk_box_pack_end(GTK_BOX(ctx->main_box), notice, FALSE, FALSE, 4);
    }

    /* ── Spectrogram section ──────────────────────────────────── */
    ctx->spectro_draw = gtk_drawing_area_new();
#ifndef EVEMON_QT_WIDGETS
    g_object_add_weak_pointer(G_OBJECT(ctx->spectro_draw), (gpointer *)&ctx->spectro_draw);
#endif
    gtk_widget_set_size_request(ctx->spectro_draw, -1, 180);
    gtk_widget_set_hexpand(ctx->spectro_draw, TRUE);

    GtkWidget *spectro_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(spectro_frame), ctx->spectro_draw);

    GtkWidget *spectro_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* ── header row: label + theme dropdown ─────────────────── */
    GtkWidget *spectro_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    GtkWidget *spectro_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(spectro_label),
        "<small><b>Audio Spectrogram</b></small>");
    gtk_label_set_xalign(GTK_LABEL(spectro_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(spectro_header), spectro_label,
                       TRUE, TRUE, 2);

    /* Hidden combo: not added to the widget tree; used only as the
     * backing store for the current theme index so right-click popup
     * and menubar items stay in sync. */
    ctx->spectro_theme_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(ctx->spectro_theme_combo), "Classic");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(ctx->spectro_theme_combo), "Heat");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(ctx->spectro_theme_combo), "Cool");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(ctx->spectro_theme_combo), "Greyscale");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(ctx->spectro_theme_combo), "Neon");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(ctx->spectro_theme_combo), "Vaporwave");
    {
        evemon_settings_t *s = settings_get();
        int saved = (s && s->spectro_theme >= 0 && s->spectro_theme < 6)
                    ? s->spectro_theme : 0;
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->spectro_theme_combo), saved);
    }
    g_signal_connect(ctx->spectro_theme_combo, "changed",
                     G_CALLBACK(on_spectro_theme_changed), ctx);
    g_object_ref_sink(ctx->spectro_theme_combo); /* own the floating widget */

    /* Right-click on the spectrogram to pick a charting theme */
    gtk_widget_add_events(ctx->spectro_draw, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(ctx->spectro_draw, "button-press-event",
                     G_CALLBACK(on_spectro_button_press), ctx);

    gtk_box_pack_start(GTK_BOX(spectro_box), spectro_header,
                       FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(spectro_box), spectro_frame,
                       FALSE, FALSE, 0);

    ctx->spectro_section = spectro_box;

    g_signal_connect(ctx->spectro_draw, "draw",
                     G_CALLBACK(on_spectro_draw), ctx);

    gtk_widget_set_no_show_all(ctx->spectro_section, TRUE);
    /* Pack at end (above Now Playing) so the tree list gets grow space */
    gtk_box_pack_end(GTK_BOX(ctx->main_box), ctx->spectro_section,
                     FALSE, FALSE, 4);

    gtk_widget_show_all(ctx->main_box);

    return ctx->main_box;
}

static void pw_activate(void *opaque,
                        const evemon_host_services_t *services)
{
    pw_ctx_t *ctx = opaque;
    ctx->host = services;
    /* Subscribe to album art events from the audio service plugin */
    if (services->subscribe)
        ctx->event_sub_id = services->subscribe(services->host_ctx,
                            EVEMON_EVENT_ALBUM_ART_UPDATED,
                            pw_on_album_art_event, ctx);
    /* Register for external charting-theme changes (e.g. menu bar) */
    if (services->charting_theme_notify)
        services->charting_theme_notify(services->host_ctx,
                                        on_charting_theme_notify, ctx);
}

static void pw_update(void *opaque, const evemon_proc_data_t *data)
{
    pw_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->last_pid = data->pid;
        snprintf(ctx->last_proc_name, sizeof(ctx->last_proc_name),
                 "%s", data->name ? data->name : "");
        ctx->spectro_shown = FALSE;
        ctx->spectro_target_node = 0;
        ctx->spectro_miss_count = 0;
        gtk_list_store_clear(ctx->store);
        ctx->audio_node_count = 0;
    }

    if (!data->pw_nodes || data->pw_node_count == 0) {
        /* Keep the last known good display — don't clear the store.
         * PipeWire snapshots are inherently racy; an empty result
         * on one cycle doesn't mean the streams are gone.  The store
         * is cleared explicitly on PID change (above) and in pw_clear(). */
        return;
    }

    /* Build display entries.
     * We store left/right labels separately so that markup generation
     * never has to search for arrow characters inside metadata strings
     * (which can legitimately contain → or ↔ in song titles, etc.).
     * Using heap-allocated sanitised UTF-8 strings avoids truncation
     * of multi-byte sequences that plagued the old 512-byte buffer.
     */
    typedef struct {
        char *left;          /* sanitised UTF-8 (g_free) */
        char *right;         /* sanitised UTF-8 or NULL  */
        const char *arrow;   /* static UTF-8 arrow or NULL */
        int cat;
        uint32_t node_id;
    } ent_t;
    size_t ent_cap = 64, ent_count = 0;
    ent_t *ents = calloc(ent_cap, sizeof(ent_t));
    if (!ents) return;

    ctx->audio_node_count = 0;

    /* When PID 1 (init/systemd) is selected, show all output streams
     * from every process — this process is typically selected on
     * evemon startup so we should show the user active audio streams
     * system wide. */
    int is_init = (data->pid == 1);

    for (size_t ni = 0; ni < data->pw_node_count; ni++) {
        const evemon_pw_node_t *node = &data->pw_nodes[ni];
        if (!is_init && node->pid != data->pid) continue;

        /* Skip evemon's own capture/meter/spectrogram streams. */
        if (is_evemon_node(node)) continue;

        /* When showing all streams (init), only display audio output
         * streams — keep the list focused and avoid flooding the UI
         * with every microphone and MIDI device on the system. */
        if (is_init) {
            int cat = classify_node(node->media_class);
            if (cat != PW_CAT_OUTPUT) continue;
        }

        int cat = classify_node(node->media_class);
        const char *self = node_label(node);

        /* Track audio output nodes for meters/spectrogram */
        if (strstr(node->media_class, "Stream") &&
            strstr(node->media_class, "Output") &&
            strstr(node->media_class, "Audio")) {
            if (ctx->audio_node_count < 64) {
                ctx->audio_node_pids[ctx->audio_node_count] = node->pid;
                ctx->audio_node_ids[ctx->audio_node_count++] = node->id;
            }
        }

        /* Find linked peers */
        uint32_t peers[32]; size_t np = 0;
        for (size_t li = 0; li < data->pw_link_count; li++) {
            const evemon_pw_link_t *lk = &data->pw_links[li];
            uint32_t peer_id = 0;
            if (lk->output_node == node->id)
                peer_id = lk->input_node;
            else if (lk->input_node == node->id)
                peer_id = lk->output_node;
            else continue;
            if (peer_id == 0 || peer_id == node->id) continue;
            int dup = 0;
            for (size_t k = 0; k < np; k++)
                if (peers[k] == peer_id) { dup = 1; break; }
            if (!dup && np < 32) peers[np++] = peer_id;
        }

        /* Filter out evemon's own nodes from the peer list so our
         * meter/spectrogram capture streams never show as connections. */
        size_t real_np = 0;
        for (size_t pi = 0; pi < np; pi++) {
            const evemon_pw_node_t *peer = find_node(data, peers[pi]);
            if (is_evemon_node(peer)) continue;
            peers[real_np++] = peers[pi];
        }
        np = real_np;

        if (np == 0) {
            if (ent_count >= ent_cap) {
                if (ent_cap > SIZE_MAX / 2 / sizeof(ent_t)) break;
                size_t new_cap = ent_cap * 2;
                ent_t *tmp = realloc(ents, new_cap * sizeof(ent_t));
                if (!tmp) break;
                ents = tmp;
                ent_cap = new_cap;
            }
            ents[ent_count].left  = g_strdup_printf("%s  (not connected)", self);
            ents[ent_count].right = NULL;
            ents[ent_count].arrow = NULL;
            ents[ent_count].cat = cat;
            ents[ent_count].node_id = node->id;
            ent_count++;
        } else {
            for (size_t pi = 0; pi < np; pi++) {
                if (ent_count >= ent_cap) {
                    if (ent_cap > SIZE_MAX / 2 / sizeof(ent_t)) break;
                    size_t new_cap = ent_cap * 2;
                    ent_t *tmp = realloc(ents,
                                         new_cap * sizeof(ent_t));
                    if (!tmp) break;
                    ents = tmp;
                    ent_cap = new_cap;
                }
                const evemon_pw_node_t *peer =
                    find_node(data, peers[pi]);
                const char *peer_lbl =
                    peer ? node_label(peer) : "(unknown)";

                if (cat == PW_CAT_OUTPUT) {
                    ents[ent_count].left  = utf8_sanitize(self);
                    ents[ent_count].right = utf8_sanitize(peer_lbl);
                    ents[ent_count].arrow = "\xe2\x86\x92";  /* → */
                } else if (cat == PW_CAT_INPUT) {
                    ents[ent_count].left  = utf8_sanitize(peer_lbl);
                    ents[ent_count].right = utf8_sanitize(self);
                    ents[ent_count].arrow = "\xe2\x86\x92";  /* → */
                } else {
                    ents[ent_count].left  = utf8_sanitize(self);
                    ents[ent_count].right = utf8_sanitize(peer_lbl);
                    ents[ent_count].arrow = "\xe2\x86\x94";  /* ↔ */
                }
                ents[ent_count].cat = cat;
                ents[ent_count].node_id = node->id;
                ent_count++;
            }
        }
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    /* Remember which node_id is currently selected so we can
     * re-select it after the store is rebuilt. */
    uint32_t sel_node_id = 0;
    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
        GtkTreeIter sel_iter;
        if (gtk_tree_selection_get_selected(sel, NULL, &sel_iter)) {
            guint nid = 0;
            gtk_tree_model_get(model, &sel_iter, COL_NODE_ID, &nid, -1);
            sel_node_id = nid;
        }
    }

    /* Update the flat list in place: reuse existing rows, append
     * new ones, remove excess. */
    GtkTreeIter iter;
    gboolean iter_valid = gtk_tree_model_iter_children(model, &iter, NULL);
    size_t ei = 0;

    while (ei < ent_count) {
        char *markup = pw_build_markup(ents[ei].left,
                                       ents[ei].right,
                                       ents[ei].arrow);
        char *plain;
        if (ents[ei].right && ents[ei].arrow)
            plain = g_strdup_printf("%s %s %s",
                ents[ei].left, ents[ei].arrow, ents[ei].right);
        else
            plain = g_strdup(ents[ei].left);

        if (iter_valid) {
            gtk_list_store_set(ctx->store, &iter,
                COL_TEXT, plain,
                COL_MARKUP, markup,
                COL_CAT, (gint)ents[ei].cat,
                COL_NODE_ID, (guint)ents[ei].node_id, -1);
            iter_valid = gtk_tree_model_iter_next(model, &iter);
        } else {
            GtkTreeIter new_iter;
            gtk_list_store_append(ctx->store, &new_iter);
            gtk_list_store_set(ctx->store, &new_iter,
                COL_TEXT, plain,
                COL_MARKUP, markup,
                COL_CAT, (gint)ents[ei].cat,
                COL_NODE_ID, (guint)ents[ei].node_id,
                COL_LEVEL_L, (gint)0,
                COL_LEVEL_R, (gint)0, -1);
        }
        g_free(markup);
        g_free(plain);
        ei++;
    }

    /* Remove excess rows */
    while (iter_valid)
        iter_valid = gtk_list_store_remove(ctx->store, &iter);

    gtk_adjustment_set_value(vadj, scroll_pos);

    /* Re-select the previously selected node by node_id. */
    if (sel_node_id != 0) {
        GtkTreeIter si;
        gboolean sv = gtk_tree_model_iter_children(model, &si, NULL);
        while (sv) {
            guint nid = 0;
            gtk_tree_model_get(model, &si, COL_NODE_ID, &nid, -1);
            if (nid == sel_node_id) {
                GtkTreeSelection *sel =
                    gtk_tree_view_get_selection(ctx->view);
                gtk_tree_selection_select_iter(sel, &si);
                break;
            }
            sv = gtk_tree_model_iter_next(model, &si);
        }
    }

    for (size_t i = 0; i < ent_count; i++) {
        g_free(ents[i].left);
        g_free(ents[i].right);
    }
    free(ents);

    /* ── Start/update peak meters and spectrogram ─────────────── */
    if (ctx->host) {
        if (ctx->audio_node_count > 0) {
            if (ctx->host->pw_meter_start)
                ctx->host->pw_meter_start(ctx->host->host_ctx,
                    ctx->audio_node_ids, ctx->audio_node_count);

            /* Start meter timer lazily on first data */
            if (!ctx->meter_timer)
                ctx->meter_timer = g_timeout_add(50, meter_tick, ctx);

            if (ctx->spectro_shown && ctx->spectro_target_node != 0
                && ctx->host->spectro_start) {
                /* Verify the user-selected node is still in the PipeWire
                 * graph.  We check the full pw_nodes list — not just the
                 * audio_node_ids subset — so any node class can be tracked.
                 * A 3-update debounce absorbs transient PipeWire snapshot
                 * gaps and prevents spurious restarts (visible pauses). */
                int still_there = 0;
                for (size_t i = 0; i < data->pw_node_count; i++) {
                    if (data->pw_nodes[i].id == ctx->spectro_target_node) {
                        still_there = 1; break;
                    }
                }
                if (still_there) {
                    ctx->spectro_miss_count = 0;
                } else if (++ctx->spectro_miss_count >= 3) {
                    /* Node truly gone — fall back to first audio output stream */
                    ctx->spectro_target_node = ctx->audio_node_ids[0];
                    ctx->spectro_miss_count = 0;
                    ctx->host->spectro_start(ctx->host->host_ctx,
                        GTK_DRAWING_AREA(ctx->spectro_draw),
                        ctx->spectro_target_node);
                    /* Re-apply the selected theme after auto-switching nodes */
                    if (ctx->host->spectro_set_theme && ctx->spectro_theme_combo) {
                        int active = gtk_combo_box_get_active(
                            GTK_COMBO_BOX(ctx->spectro_theme_combo));
                        if (active >= 0)
                            ctx->host->spectro_set_theme(ctx->host->host_ctx,
                                GTK_DRAWING_AREA(ctx->spectro_draw),
                                (unsigned)active);
                    }
                }
            }
        } else {
            /* No audio output nodes for this process — stop our own
             * spectrogram (per-draw-area, safe for other instances).
             * Don't call pw_meter_stop or pw_meter_remove_nodes since
             * the meter is shared; stale streams read silence and are
             * cleaned up in pw_destroy(). */
            if (ctx->spectro_shown && ctx->spectro_draw &&
                ctx->host->spectro_stop)
                ctx->host->spectro_stop(ctx->host->host_ctx,
                    GTK_DRAWING_AREA(ctx->spectro_draw));
        }
    }

    /* ── Update MPRIS metadata display ────────────────────────── */
    pw_update_mpris(ctx, data);
}

static void pw_clear(void *opaque)
{
    pw_ctx_t *ctx = opaque;
    gtk_list_store_clear(ctx->store);
    ctx->last_pid = 0;
    ctx->audio_node_count = 0;
    /* Hide the media section so stale metadata isn't shown when
     * no process is selected. */
    if (ctx->media_section && GTK_IS_WIDGET(ctx->media_section))
        gtk_widget_hide(ctx->media_section);
    /* Clear album art */
    if (ctx->media_art_image && GTK_IS_IMAGE(ctx->media_art_image))
        gtk_image_clear(GTK_IMAGE(ctx->media_art_image));
    if (ctx->art_pixbuf) {
        g_object_unref(ctx->art_pixbuf);
        ctx->art_pixbuf = NULL;
    }
    ctx->art_url_cached[0] = '\0';
    /* Reset BPM detector */
    bpm_reset(&ctx->bpm);
    ctx->bpm_track_key[0] = '\0';
    ctx->bpm_last_pos_us = -1;
    if (ctx->bpm_label && GTK_IS_LABEL(ctx->bpm_label))
        gtk_label_set_text(GTK_LABEL(ctx->bpm_label), "");
    if (ctx->bpm_track_label && GTK_IS_LABEL(ctx->bpm_track_label))
        gtk_label_set_text(GTK_LABEL(ctx->bpm_track_label), "");
    /* Reset spectrogram tracking */
    ctx->spectro_target_node = 0;
    ctx->spectro_miss_count = 0;
    /* Don't call pw_meter_stop here — the meter is shared. */
}

void pw_destroy(void *opaque)
{
    pw_ctx_t *ctx = opaque;

    /* Unsubscribe from the event bus BEFORE freeing ctx,
     * otherwise deferred g_idle_add callbacks can dereference
     * the freed user_data pointer → segfault. */
    if (ctx->event_sub_id > 0 && ctx->host && ctx->host->unsubscribe) {
        ctx->host->unsubscribe(ctx->host->host_ctx, ctx->event_sub_id);
        ctx->event_sub_id = 0;
    }
    /* Unregister charting-theme notification to avoid dangling callback */
    if (ctx->host && ctx->host->charting_theme_notify)
        ctx->host->charting_theme_notify(ctx->host->host_ctx, NULL, ctx);

    if (ctx->meter_timer) {
        g_source_remove(ctx->meter_timer);
        ctx->meter_timer = 0;
    }
    /* Don't call pw_meter_stop — the meter is shared across all plugin
     * instances.  We remove only our own nodes via pw_meter_remove_nodes
     * so other instances' streams remain intact. */
    if (ctx->host) {
        if (ctx->audio_node_count > 0 && ctx->host->pw_meter_remove_nodes)
            ctx->host->pw_meter_remove_nodes(ctx->host->host_ctx,
                ctx->audio_node_ids, ctx->audio_node_count);
        /* Stop our own spectrogram instance (per-draw-area, safe) */
        if (ctx->spectro_draw && ctx->host->spectro_stop)
            ctx->host->spectro_stop(ctx->host->host_ctx,
                GTK_DRAWING_AREA(ctx->spectro_draw));
    }
#ifndef EVEMON_QT_WIDGETS
    if (ctx->spectro_draw)
        g_object_remove_weak_pointer(G_OBJECT(ctx->spectro_draw), (gpointer *)&ctx->spectro_draw);
#endif
    if (ctx->store) {
        g_object_unref(ctx->store);
        ctx->store = NULL;
    }
    if (ctx->css) g_object_unref(ctx->css);
    if (ctx->art_pixbuf) g_object_unref(ctx->art_pixbuf);

    /* Null out widget pointers before free — defensive against
     * any stale references that might still try to touch them. */
    ctx->media_title_label    = NULL;
    ctx->media_artist_label   = NULL;
    ctx->media_album_label    = NULL;
    ctx->media_status_label   = NULL;
    ctx->media_position_label = NULL;
    ctx->media_art_image      = NULL;
    ctx->media_section        = NULL;
    ctx->bpm_label            = NULL;
    ctx->bpm_track_label      = NULL;

#ifdef EVEMON_QT_WIDGETS
    gtk_widget_destroy(ctx->spectro_theme_combo);
#endif
    free(ctx);
}

static int pw_wants_update(void *opaque)
{
    pw_ctx_t *ctx = opaque;
    return ctx->main_box &&
           GTK_IS_WIDGET(ctx->main_box) &&
           gtk_widget_get_mapped(ctx->main_box) &&
           gtk_widget_get_child_visible(ctx->main_box);
}

/* ── descriptor ──────────────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    pw_ctx_t *ctx = calloc(1, sizeof(pw_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "PipeWire",
        .id            = "org.evemon.pipewire",
        .version       = "1.0",
        .data_needs    = evemon_NEED_PIPEWIRE | evemon_NEED_MPRIS,
        .plugin_ctx    = ctx,
        .create_widget = pw_create_widget,
        .update        = pw_update,
        .clear         = pw_clear,
        .destroy       = pw_destroy,
        .activate      = pw_activate,
        .wants_update  = pw_wants_update,
        .role          = EVEMON_ROLE_PROCESS,
        .dependencies  = _pw_deps,
    };

    return p;
}
