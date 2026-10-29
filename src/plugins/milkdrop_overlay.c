#include "milkdrop_internal.h"

/* ── PangoCairo text helpers ─────────────────────────────────── */
/*
 * Cairo's "toy" font API (cairo_select_font_face / cairo_show_text)
 * picks a single FreeType face and has NO Unicode fallback — CJK,
 * Hangul, emoji, and even the Unicode play symbols (▶ ⏸ ⏹) all
 * render as blank boxes when the primary face lacks those glyphs.
 *
 * PangoCairo goes through fontconfig's full fallback chain and
 * handles every script automatically.  Pango is already linked
 * via GTK3 so this adds zero new dependencies.
 */

/*
 * Measure a UTF-8 string using Pango (handles all scripts).
 * Returns width and height in pixels via *out_w, *out_h.
 */
static PangoLayout *md_pango_layout(cairo_t *cr,
                                     const char *family,
                                     double font_px,
                                     PangoWeight weight,
                                     PangoStyle style,
                                     const char *text,
                                     int *out_w, int *out_h)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1);

    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, family);
    /* Pango sizes are in Pango units (1/1024 of a point).
     * We want pixel sizes, so use set_absolute_size (takes Pango units
     * where 1px = PANGO_SCALE). */
    pango_font_description_set_absolute_size(fd, font_px * PANGO_SCALE);
    pango_font_description_set_weight(fd, weight);
    pango_font_description_set_style(fd, style);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    pango_layout_get_pixel_size(layout, out_w, out_h);
    return layout;
}

/*
 * Draw text with a drop shadow using PangoCairo.
 * (x, y) is the top-left corner of the text bounding box.
 */
static void md_pango_show(cairo_t *cr, PangoLayout *layout,
                           float x, float y,
                           float sr, float sg, float sb, float sa,
                           float fr, float fg, float fb, float fa)
{
    /* shadow */
    cairo_set_source_rgba(cr, sr, sg, sb, sa);
    cairo_move_to(cr, x + 1, y + 1);
    pango_cairo_show_layout(cr, layout);
    /* foreground */
    cairo_set_source_rgba(cr, fr, fg, fb, fa);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
}

/*
 * Convenience: create layout, draw right-aligned with shadow, free layout.
 * Returns the layout height so callers can advance the cursor.
 */
static float md_pango_right(cairo_t *cr, float right_x, float baseline_y,
                             const char *family, double font_px,
                             PangoWeight weight, PangoStyle style,
                             const char *text,
                             float sr, float sg, float sb, float sa,
                             float fr, float fg, float fb, float fa)
{
    int tw, th;
    PangoLayout *layout = md_pango_layout(cr, family, font_px,
                                           weight, style, text,
                                           &tw, &th);
    float tx = right_x - tw;
    /* baseline_y is the baseline position; Pango draws from top-left,
     * so approximate: top = baseline - ascent ≈ baseline - 0.8*font_px */
    float top_y = baseline_y - font_px * 0.82f;
    md_pango_show(cr, layout, tx, top_y, sr, sg, sb, sa, fr, fg, fb, fa);
    g_object_unref(layout);
    return (float)th;
}

/*
 * Simple left-aligned Pango text (for preset names, status text, etc.)
 */
static void md_pango_left(cairo_t *cr, float x, float y,
                           const char *family, double font_px,
                           PangoWeight weight, PangoStyle style,
                           const char *text,
                           float r, float g_, float b, float a)
{
    int tw, th; (void)th;
    PangoLayout *layout = md_pango_layout(cr, family, font_px,
                                           weight, style, text,
                                           &tw, &th);
    cairo_set_source_rgba(cr, r, g_, b, a);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

/* Draw info overlay using Cairo on top of GL */
#ifdef EVEMON_QT_MILKDROP
gboolean md_draw_overlay(md_ctx_t *ctx, cairo_t *cr, int ww, int hh)
{
#else
gboolean on_overlay_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    md_ctx_t *ctx = data;
    int ww = gtk_widget_get_allocated_width(w);
    int hh = gtk_widget_get_allocated_height(w);
#endif

    /* Clear to fully transparent so GL area shows through */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (!ctx->running && !ctx->audio_node_count) {
        md_pango_left(cr, 12, hh/2.0-26, "Sans", 12,
                      PANGO_WEIGHT_NORMAL, PANGO_STYLE_ITALIC,
                      "MilkDrop — waiting for audio stream",
                      1,1,1, 0.3f);
        md_pango_left(cr, 12, hh/2.0-4, "Sans", 9,
                      PANGO_WEIGHT_NORMAL, PANGO_STYLE_NORMAL,
                      "Select a process with PipeWire audio output",
                      1,1,1, 0.3f);
        char buf[80]; snprintf(buf,sizeof(buf),"%d presets loaded from disk",
                               ctx->lib.count);
        md_pango_left(cr, 12, hh/2.0+12, "Sans", 9,
                      PANGO_WEIGHT_NORMAL, PANGO_STYLE_NORMAL,
                      buf, 1,1,1, 0.3f);
    }

    /* ── help overlay (F1) ───────────────────────────────────── */
    if (ctx->show_help) {
        /* semi-transparent dark background */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.82);
        cairo_rectangle(cr, 0, 0, ww, hh);
        cairo_fill(cr);

        md_pango_left(cr, 16, 14, "Monospace", 13,
                      PANGO_WEIGHT_BOLD, PANGO_STYLE_NORMAL,
                      "MilkDrop Hotkeys (F1 to close)",
                      1, 0.9f, 0.4f, 1);

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
            "M / Shift+M  Cycle now-playing (off/title/info/full)",
            "",
            "B / Shift+B  Increase/decrease blend time",
            "+/-          Increase/decrease auto-advance time",
            "",
            "Ctrl+S       Save preset as favorite",
            "Ctrl+M       Open/close favorites menu",
            "Ctrl+F       Toggle favorites-only cycle mode",
            "  In menu:   \xe2\x86\x91\xe2\x86\x93 navigate   Enter=load   Del=remove",
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
            "Escape       Exit fullscreen / Close favorites menu",
            "",
            "Scroll       Next/previous preset",
            "Double-click Random preset",
            NULL
        };

        float y = 36;
        for (int i = 0; help[i]; i++) {
            if (help[i][0] == '\0') { y += 6; continue; }
            md_pango_left(cr, 24, y, "Monospace", 10,
                          PANGO_WEIGHT_NORMAL, PANGO_STYLE_NORMAL,
                          help[i], 0.85f, 0.9f, 1.0f, 0.95f);
            y += 14;
            if (y > hh - 16) break;
        }
    }

    /* ── FPS counter (F5) ────────────────────────────────────── */
    if (ctx->show_fps && ctx->preset_loaded) {
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "%.1f fps", ctx->fps_display);
        int tw, th;
        PangoLayout *fps_l = md_pango_layout(cr, "Monospace", 10,
                                              PANGO_WEIGHT_BOLD,
                                              PANGO_STYLE_NORMAL,
                                              fps_buf, &tw, &th);
        md_pango_show(cr, fps_l,
                       (float)(ww - tw - 6), 4,
                       0,0,0, 0.6f,
                       0.2f, 1.0f, 0.2f, 0.8f);
        g_object_unref(fps_l);
    }

    /* ── status indicators (lock, auto, blend) ───────────────── */
    if (ctx->preset_loaded) {
        float sy = hh - 60;
        if (ctx->locked) {
            md_pango_left(cr, 6, sy, "Sans", 9,
                          PANGO_WEIGHT_BOLD, PANGO_STYLE_NORMAL,
                          "LOCKED", 1, 0.3f, 0.3f, 0.7f);
            sy -= 13;
        }
        if (ctx->auto_preset) {
            md_pango_left(cr, 6, sy, "Sans", 9,
                          PANGO_WEIGHT_BOLD, PANGO_STYLE_NORMAL,
                          "AUTO", 0.3f, 1, 0.3f, 0.7f);
            sy -= 13;
        }
        if (ctx->blending) {
            char bb[32]; snprintf(bb, sizeof(bb), "BLEND %.0f%%",
                                  ctx->blend_progress * 100);
            md_pango_left(cr, 6, sy, "Sans", 9,
                          PANGO_WEIGHT_BOLD, PANGO_STYLE_NORMAL,
                          bb, 0.5f, 0.7f, 1, 0.6f);
        }
    }

    /* ── info toast (bottom bar) ─────────────────────────────── */
    if (ctx->info_frames > 0) {
        ctx->info_frames--;
        float a = ctx->info_frames>60 ? 0.9f : 0.9f*ctx->info_frames/60.0f;
        cairo_set_source_rgba(cr,0,0,0,a*0.7);
        cairo_rectangle(cr,0,hh-36,ww,36); cairo_fill(cr);
        md_pango_left(cr, 8, hh-30, "Sans", 11,
                      PANGO_WEIGHT_BOLD, PANGO_STYLE_NORMAL,
                      ctx->info_text, 1,1,1, a);
    }

    /* ── preset name (top-left, always or faded) ─────────────── */
    if (ctx->preset_loaded) {
        float name_alpha = ctx->show_preset_name ? 0.6f : 0.2f;
        /* check whether this preset is a saved favorite */
        int is_fav = 0;
        for (int i = 0; i < MD_FAV_SLOTS; i++) {
            if (ctx->favorites[i].used &&
                strcmp(ctx->favorites[i].snap.filepath,
                       ctx->preset.filepath) == 0)
                { is_fav = 1; break; }
        }
        char dn[96];
        if (is_fav) {
            snprintf(dn, sizeof(dn), "\xe2\x98\x85 %.90s",
                     ctx->preset.name);   /* UTF-8 ★ */
        } else {
            strncpy(dn, ctx->preset.name, 70); dn[70] = 0;
            if (strlen(ctx->preset.name) > 70) strcat(dn, "\xe2\x80\xa6");
        }
        /* gold when favorited, plain white otherwise */
        float nr = 1.0f, ng = is_fav ? 0.82f : 1.0f, nb = is_fav ? 0.15f : 1.0f;
        md_pango_left(cr, 6, 4, "Sans", 9,
                      PANGO_WEIGHT_NORMAL, PANGO_STYLE_NORMAL,
                      dn, nr, ng, nb, name_alpha);
        if (ctx->fav_cycle) {
            md_pango_left(cr, 6, 17, "Sans", 8,
                          PANGO_WEIGHT_BOLD, PANGO_STYLE_NORMAL,
                          "FAV CYCLE", 1.0f, 0.82f, 0.15f, 0.65f);
        }
    }

    /* ── favorites menu overlay (Ctrl+M) ─────────────────────── */
    if (ctx->show_fav_menu) {
        int used_count = 0;
        for (int i = 0; i < MD_FAV_SLOTS; i++)
            if (ctx->favorites[i].used) used_count++;

        const float lh = 22.0f, pw = 460.0f;
        float ph = 50.0f + (used_count > 0 ? used_count : 1) * lh + 8.0f;
        float px = (ww - pw) * 0.5f, py = (hh - ph) * 0.5f;
        if (py < 8.0f) py = 8.0f;

        /* dark panel */
        cairo_set_source_rgba(cr, 0.04, 0.04, 0.12, 0.93);
        cairo_rectangle(cr, px, py, pw, ph);
        cairo_fill(cr);
        /* gold border */
        cairo_set_source_rgba(cr, 1.0, 0.82, 0.15, 0.55);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, px, py, pw, ph);
        cairo_stroke(cr);

        /* title */
        md_pango_left(cr, px+12, py+8, "Sans", 12,
                      PANGO_WEIGHT_BOLD, PANGO_STYLE_NORMAL,
                      "\xe2\x98\x85  Favorites", 1.0f, 0.82f, 0.15f, 0.95f);
        /* hint */
        md_pango_left(cr, px+pw-220.0f, py+12, "Monospace", 8,
                      PANGO_WEIGHT_NORMAL, PANGO_STYLE_NORMAL,
                      "\xe2\x86\x91\xe2\x86\x93 nav   Enter=load   Del=remove   Esc=close",
                      0.5f, 0.5f, 0.5f, 0.8f);

        float ry = py + 36.0f;
        if (used_count == 0) {
            md_pango_left(cr, px+16, ry+3, "Sans", 10,
                          PANGO_WEIGHT_NORMAL, PANGO_STYLE_ITALIC,
                          "No favorites yet \xe2\x80\x94 press Ctrl+S while a preset is running",
                          0.55f, 0.55f, 0.55f, 0.85f);
        } else {
            for (int i = 0; i < MD_FAV_SLOTS; i++) {
                if (!ctx->favorites[i].used) continue;
                int sel = (i == ctx->fav_menu_sel);
                if (sel) {
                    cairo_set_source_rgba(cr, 1.0, 0.82, 0.15, 0.14);
                    cairo_rectangle(cr, px+4, ry, pw-8, lh-1);
                    cairo_fill(cr);
                }
                char row[320];
                snprintf(row, sizeof(row), "\xe2\x98\x85  %s",
                         ctx->favorites[i].name);
                /* truncate if too long */
                if (strlen(row) > 60) { row[60] = 0; strcat(row, "\xe2\x80\xa6"); }
                md_pango_left(cr, px+16, ry+3, "Sans", 10,
                              sel ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                              PANGO_STYLE_NORMAL, row,
                              sel ? 1.0f  : 0.82f,
                              sel ? 0.88f : 0.82f,
                              sel ? 0.15f : 0.82f,
                              sel ? 1.0f  : 0.72f);
                ry += lh;
            }
        }
    }

    /* ── now-playing / audio source (bottom-right) ───────────── */
    /*
     * show_nowplaying controls detail level:
     *   0 = off (hidden)
     *   1 = title only
     *   2 = title + artist + album
     *   3 = full (title + artist + album + status/timer + album art)
     */
    if (ctx->running && ctx->audio_node_count > 0 && ctx->show_nowplaying > 0) {
        /*
         * Scale all text proportionally to the widget's shorter
         * dimension so it looks right at 320×240 and at 3840×2160.
         * Reference size: 480 px → base multiplier 1.0.
         */
        float ref = (ww < hh ? ww : hh);
        float scale = ref / 480.0f;
        if (scale < 0.6f) scale = 0.6f;
        if (scale > 3.0f) scale = 3.0f;

        int np_level = ctx->show_nowplaying; /* 1, 2, or 3 */

        if (ctx->mpris_available && ctx->mpris_title[0]) {
            /* ── album art thumbnail (level 3 only) ──────────── */
            float art_sz = 0;           /* actual rendered size   */
            float art_margin = 6 * scale;
            if (np_level >= 3 && ctx->mpris_art_pixbuf) {
                int aw = gdk_pixbuf_get_width(ctx->mpris_art_pixbuf);
                int ah = gdk_pixbuf_get_height(ctx->mpris_art_pixbuf);
                art_sz = 48 * scale;
                if (art_sz < 24) art_sz = 24;
                float asx = art_sz / (float)aw;
                float asy = art_sz / (float)ah;
                float as  = asx < asy ? asx : asy;
                float rw = aw * as, rh = ah * as;

                float ax = ww - rw - 8 * scale;
                float ay = hh - 44 * scale - rh;

                /* subtle shadow behind the art */
                cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
                cairo_rectangle(cr, ax + 2, ay + 2, rw, rh);
                cairo_fill(cr);

                cairo_save(cr);
                cairo_translate(cr, ax, ay);
                cairo_scale(cr, as, as);
                gdk_cairo_set_source_pixbuf(cr, ctx->mpris_art_pixbuf,
                                            0, 0);
                cairo_paint_with_alpha(cr, 0.85);
                cairo_restore(cr);

                /* thin border */
                cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
                cairo_set_line_width(cr, 1);
                cairo_rectangle(cr, ax, ay, rw, rh);
                cairo_stroke(cr);
            }

            /* ── text block (right-aligned, above info bar) ─── */
            float text_right = ww - (art_sz > 0 ? art_sz + art_margin : 0)
                               - 8 * scale;
            float font_title  = 14 * scale;
            float font_artist = 12 * scale;
            float font_album  = 10 * scale;
            float font_status = 9  * scale;
            float line_gap    = 4  * scale;
            float ty = hh - 44 * scale;

            /* Status icon + position/length (level 3 only) */
            if (np_level >= 3) {
                const char *icon = "⏹";
                if (strcmp(ctx->mpris_status, "Playing") == 0) icon = "▶";
                else if (strcmp(ctx->mpris_status, "Paused") == 0) icon = "⏸";

                char pos_buf[32] = "", len_buf[32] = "", status[128];
                if (ctx->mpris_position_us > 0) {
                    int64_t ps = ctx->mpris_position_us / 1000000;
                    int pm = (int)(ps / 60), pss = (int)(ps % 60);
                    snprintf(pos_buf, sizeof(pos_buf), "%d:%02d", pm, pss);
                }
                if (ctx->mpris_length_us > 0) {
                    int64_t ls = ctx->mpris_length_us / 1000000;
                    int lm = (int)(ls / 60), lss = (int)(ls % 60);
                    snprintf(len_buf, sizeof(len_buf), "%d:%02d", lm, lss);
                }
                if (pos_buf[0] && len_buf[0])
                    snprintf(status, sizeof(status), "%s %s / %s",
                             icon, pos_buf, len_buf);
                else if (len_buf[0])
                    snprintf(status, sizeof(status), "%s / %s",
                             icon, len_buf);
                else
                    snprintf(status, sizeof(status), "%s %s",
                             icon, ctx->mpris_status);

                int tw, th;
                PangoLayout *sl = md_pango_layout(
                    cr, "Sans", font_status,
                    PANGO_WEIGHT_NORMAL, PANGO_STYLE_NORMAL,
                    status, &tw, &th);
                md_pango_show(cr, sl,
                              text_right - tw, ty - th,
                              0,0,0, 0.4f,
                              0.6f, 0.7f, 0.8f, 0.55f);
                g_object_unref(sl);
                ty -= th + line_gap;
            }

            /* Album (level 2+) */
            if (np_level >= 2 && ctx->mpris_album[0]) {
                int tw, th;
                PangoLayout *al = md_pango_layout(
                    cr, "Sans", font_album,
                    PANGO_WEIGHT_NORMAL, PANGO_STYLE_ITALIC,
                    ctx->mpris_album, &tw, &th);
                md_pango_show(cr, al,
                              text_right - tw, ty - th,
                              0,0,0, 0.4f,
                              0.6f, 0.65f, 0.8f, 0.5f);
                g_object_unref(al);
                ty -= th + line_gap;
            }

            /* Artist (level 2+) */
            if (np_level >= 2 && ctx->mpris_artist[0]) {
                int tw, th;
                PangoLayout *al = md_pango_layout(
                    cr, "Sans", font_artist,
                    PANGO_WEIGHT_NORMAL, PANGO_STYLE_NORMAL,
                    ctx->mpris_artist, &tw, &th);
                md_pango_show(cr, al,
                              text_right - tw, ty - th,
                              0,0,0, 0.4f,
                              0.7f, 0.8f, 1.0f, 0.6f);
                g_object_unref(al);
                ty -= th + line_gap + 1;
            }

            /* Title (largest, boldest) */
            {
                int tw, th;
                PangoLayout *tl = md_pango_layout(
                    cr, "Sans", font_title,
                    PANGO_WEIGHT_BOLD, PANGO_STYLE_NORMAL,
                    ctx->mpris_title, &tw, &th);
                md_pango_show(cr, tl,
                              text_right - tw, ty - th,
                              0,0,0, 0.5f,
                              0.9f, 0.93f, 1.0f, 0.7f);
                g_object_unref(tl);
            }
        } else {
            /* Fallback: single-line PipeWire media name */
            float font_fb = 12 * scale;

            char src[384];
            if (ctx->audio_media_name[0])
                snprintf(src, sizeof(src), "%s", ctx->audio_media_name);
            else if (ctx->audio_app_name[0])
                snprintf(src, sizeof(src), "%s", ctx->audio_app_name);
            else if (ctx->process_name[0])
                snprintf(src, sizeof(src), "%s", ctx->process_name);
            else
                snprintf(src, sizeof(src), "PID %d", (int)ctx->last_pid);

            int tw, th;
            PangoLayout *fl = md_pango_layout(
                cr, "Sans", font_fb,
                PANGO_WEIGHT_NORMAL, PANGO_STYLE_NORMAL,
                src, &tw, &th);
            float tx = ww - tw - 8 * scale;
            float ty = hh - 44 * scale - th;
            md_pango_show(cr, fl, tx, ty,
                           0,0,0, 0.4f,
                           0.7f, 0.8f, 1.0f, 0.5f);
            g_object_unref(fl);
        }
    }

    return FALSE; /* let GL render through */
}

