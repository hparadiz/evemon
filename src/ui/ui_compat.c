#include "../ipc/query.h"
#include "ui_gtk_private.h"
#include "../platform/compatibility.h"

/* ── GTK dialog ───────────────────────────────────────────────── */

void gtk_ui_on_menu_compatibility(GtkMenuItem *item, gpointer data)
{
    (void)item;
    GtkWidget *parent_window = data;

    compat_result_t results[32];
    int nresults = evemon_collect_compat_results(results,
                                          (int)(sizeof(results) /
                                                sizeof(results[0])));

    /* ── Window ─────────────────────────────────────────────────── */
    GtkWidget *dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dlg), query_is_foreign_host() ? "evemon – Local GUI Compatibility" : "evemon – System Compatibility");
    gtk_window_set_transient_for(GTK_WINDOW(dlg),
                                 GTK_WINDOW(parent_window));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_window_set_wmclass(GTK_WINDOW(dlg), "evemon", "evemon");
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 720, 500);
    gtk_window_set_resizable(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);

    /* Close on Escape */
    g_signal_connect_swapped(dlg, "key-press-event",
                             G_CALLBACK(gtk_widget_destroy), dlg);

    /* ── Outer vbox ─────────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dlg), vbox);

    /* ── Header bar ─────────────────────────────────────────────── */
    {
        GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(hdr, 16);
        gtk_widget_set_margin_end(hdr, 16);
        gtk_widget_set_margin_top(hdr, 12);
        gtk_widget_set_margin_bottom(hdr, 8);

        /* CSS for header */
        {
            GtkCssProvider *hdr_css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(hdr_css,
                ".compat-header { font-size: 13pt; font-weight: bold; }",
                -1, NULL);
            gtk_style_context_add_provider_for_screen(
                gdk_screen_get_default(),
                GTK_STYLE_PROVIDER(hdr_css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(hdr_css);
        }

        GtkWidget *title_lbl = gtk_label_new("System Compatibility");
        GtkStyleContext *sc = gtk_widget_get_style_context(title_lbl);
        gtk_style_context_add_class(sc, "compat-header");
        gtk_widget_set_hexpand(title_lbl, TRUE);
        gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(hdr), title_lbl, TRUE, TRUE, 0);

        /* Close button */
        GtkWidget *close_btn = gtk_button_new_with_label("Close");
        g_signal_connect_swapped(close_btn, "clicked",
                                 G_CALLBACK(gtk_widget_destroy), dlg);
        gtk_box_pack_end(GTK_BOX(hdr), close_btn, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), hdr, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                           FALSE, FALSE, 0);
    }

    /* ── Scrolled content ───────────────────────────────────────── */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 16);
    gtk_widget_set_margin_end(grid, 16);
    gtk_widget_set_margin_top(grid, 10);
    gtk_widget_set_margin_bottom(grid, 16);
    gtk_container_add(GTK_CONTAINER(scroll), grid);

    /* Column header row */
    {
        static const char *hdrs[] = { "Feature", "Status", "Details" };
        for (int c = 0; c < 3; c++) {
            GtkWidget *lbl = gtk_label_new(hdrs[c]);
            GtkStyleContext *sc = gtk_widget_get_style_context(lbl);
            gtk_style_context_add_class(sc, "compat-header");
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_set_margin_bottom(lbl, 4);
            gtk_grid_attach(GTK_GRID(grid), lbl, c, 0, 1, 1);
        }
        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_grid_attach(GTK_GRID(grid), sep, 0, 1, 3, 1);
    }

    /* CSS for severity badges */
    {
        GtkCssProvider *badge_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(badge_css,
            ".badge-ok   { color: #2ecc71; font-weight: bold; }"
            ".badge-warn { color: #e67e22; font-weight: bold; }"
            ".badge-fail { color: #e74c3c; font-weight: bold; }"
            ".badge-info { color: #95a5a6; font-weight: bold; }"
            ".compat-detail { font-size: 9pt; }",
            -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(badge_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(badge_css);
    }

    for (int i = 0; i < nresults; i++) {
        int row = i + 2;   /* +2 for header + separator rows */
        const compat_result_t *r = &results[i];

        /* Alternating row background */
        if (i % 2 == 0) {
            GtkWidget *row_bg = gtk_event_box_new();
            gtk_widget_set_hexpand(row_bg, TRUE);
            GtkCssProvider *row_css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(row_css,
                "eventbox { background-color: alpha(currentColor, 0.04); }",
                -1, NULL);
            gtk_style_context_add_provider(
                gtk_widget_get_style_context(row_bg),
                GTK_STYLE_PROVIDER(row_css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(row_css);
            gtk_grid_attach(GTK_GRID(grid), row_bg, 0, row, 3, 1);
        }

        /* Icon + feature name */
        {
            const char *icon = "●";
            GtkWidget *feat_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            GtkWidget *icon_lbl = gtk_label_new(icon);
            GtkStyleContext *sc = gtk_widget_get_style_context(icon_lbl);
            const char *badge_class;
            switch (r->severity) {
            case COMPAT_OK:   badge_class = "badge-ok";   break;
            case COMPAT_WARN: badge_class = "badge-warn"; break;
            case COMPAT_FAIL: badge_class = "badge-fail"; break;
            default:          badge_class = "badge-info"; break;
            }
            gtk_style_context_add_class(sc, badge_class);
            gtk_widget_set_valign(icon_lbl, GTK_ALIGN_START);

            GtkWidget *feat_lbl = gtk_label_new(r->feature);
            gtk_widget_set_halign(feat_lbl, GTK_ALIGN_START);
            gtk_widget_set_valign(feat_lbl, GTK_ALIGN_START);

            gtk_box_pack_start(GTK_BOX(feat_box), icon_lbl, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(feat_box), feat_lbl, FALSE, FALSE, 0);

            gtk_widget_set_margin_top(feat_box, 4);
            gtk_widget_set_margin_bottom(feat_box, 4);
            gtk_grid_attach(GTK_GRID(grid), feat_box, 0, row, 1, 1);
        }

        /* Status label */
        {
            GtkWidget *status_lbl = gtk_label_new(r->status);
            GtkStyleContext *sc = gtk_widget_get_style_context(status_lbl);
            const char *badge_class;
            switch (r->severity) {
            case COMPAT_OK:   badge_class = "badge-ok";   break;
            case COMPAT_WARN: badge_class = "badge-warn"; break;
            case COMPAT_FAIL: badge_class = "badge-fail"; break;
            default:          badge_class = "badge-info"; break;
            }
            gtk_style_context_add_class(sc, badge_class);
            gtk_widget_set_halign(status_lbl, GTK_ALIGN_START);
            gtk_widget_set_valign(status_lbl, GTK_ALIGN_START);
            gtk_widget_set_margin_top(status_lbl, 4);
            gtk_widget_set_margin_bottom(status_lbl, 4);
            gtk_grid_attach(GTK_GRID(grid), status_lbl, 1, row, 1, 1);
        }

        /* Detail text */
        {
            GtkWidget *detail_lbl = gtk_label_new(r->detail);
            gtk_label_set_line_wrap(GTK_LABEL(detail_lbl), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(detail_lbl),
                                         PANGO_WRAP_WORD_CHAR);
            gtk_label_set_xalign(GTK_LABEL(detail_lbl), 0.0f);
            gtk_widget_set_halign(detail_lbl, GTK_ALIGN_FILL);
            gtk_widget_set_valign(detail_lbl, GTK_ALIGN_START);
            gtk_widget_set_hexpand(detail_lbl, TRUE);
            gtk_widget_set_margin_top(detail_lbl, 4);
            gtk_widget_set_margin_bottom(detail_lbl, 4);
            GtkStyleContext *sc = gtk_widget_get_style_context(detail_lbl);
            gtk_style_context_add_class(sc, "compat-detail");
            gtk_grid_attach(GTK_GRID(grid), detail_lbl, 2, row, 1, 1);
        }
    }

    /* ── Footer note ─────────────────────────────────────────────── */
    {
        gtk_box_pack_start(GTK_BOX(vbox),
                           gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                           FALSE, FALSE, 0);
        GtkWidget *footer = gtk_label_new(query_is_foreign_host() ?
            "These checks describe this GUI machine. Monitoring permissions belong to the remote evemon." :
            "Use File → Restart as Administrator to gain full access.  "
            "Alternatively, grant individual capabilities with setcap(8).");
        gtk_label_set_line_wrap(GTK_LABEL(footer), TRUE);
        gtk_label_set_xalign(GTK_LABEL(footer), 0.0f);
        gtk_widget_set_margin_start(footer, 16);
        gtk_widget_set_margin_end(footer, 16);
        gtk_widget_set_margin_top(footer, 8);
        gtk_widget_set_margin_bottom(footer, 10);
        GtkStyleContext *sc = gtk_widget_get_style_context(footer);
        gtk_style_context_add_class(sc, "compat-detail");
        gtk_box_pack_start(GTK_BOX(vbox), footer, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(dlg);
}
