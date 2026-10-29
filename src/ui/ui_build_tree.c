#include "../ipc/query.h"
#include "ui_gtk_private.h"

#include "ui_build.h"

void gtk_build_tree(ui_build_t *b)
{


    gtk_ui_desktop_open_context_init_once();

    /* ── window ──────────────────────────────────────────────── */
    b->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[320];
    if (query_is_foreign_host())
        snprintf(title, sizeof(title), "ＥＶＥＭＯＮ | %s", query_host_name());
    else
        snprintf(title, sizeof(title), "ＥＶＥＭＯＮ");
    gtk_window_set_title(GTK_WINDOW(b->window), title);
    gtk_window_set_role(GTK_WINDOW(b->window), "main");
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_window_set_wmclass(GTK_WINDOW(b->window), "evemon", "evemon");
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_window_set_default_size(GTK_WINDOW(b->window), 1100, 700);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(b->window), "evemon-main-window");

    /* Accel group for displaying hotkey hints in menus.
     * The actual key handling is in on_key_press. */
    b->accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(b->window), b->accel_group);

    /* Set the window icon from the embedded GResource PNG.
     *
     * The source image may be large and non-square, so we produce
     * square, RGBA-with-alpha versions at the standard icon sizes
     * that window managers request (16, 32, 48, 64, 128).
     * This avoids blurry stretching and preserves transparency.      */
    {
        GdkPixbuf *raw = gdk_pixbuf_new_from_resource(
            "/org/evemon/icon.png", NULL);
        if (raw) {
            static const int sizes[] = { 16, 32, 48, 64, 128 };
            GList *icon_list = NULL;

            for (int i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
                int sz = sizes[i];

                /* Scale preserving aspect ratio into a sz×sz box */
                int src_w = gdk_pixbuf_get_width(raw);
                int src_h = gdk_pixbuf_get_height(raw);
                double scale = (double)sz / (src_w > src_h ? src_w : src_h);
                int dst_w = (int)(src_w * scale + 0.5);
                int dst_h = (int)(src_h * scale + 0.5);
                if (dst_w < 1) dst_w = 1;
                if (dst_h < 1) dst_h = 1;

                GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
                    raw, dst_w, dst_h, GDK_INTERP_BILINEAR);
                if (!scaled) continue;

                /* Centre the scaled image on a transparent sz×sz canvas */
                GdkPixbuf *canvas = gdk_pixbuf_new(
                    GDK_COLORSPACE_RGB, TRUE, 8, sz, sz);
                gdk_pixbuf_fill(canvas, 0x00000000);   /* fully transparent */

                int off_x = (sz - dst_w) / 2;
                int off_y = (sz - dst_h) / 2;
                gdk_pixbuf_composite(scaled, canvas,
                                     off_x, off_y, dst_w, dst_h,
                                     off_x, off_y, 1.0, 1.0,
                                     GDK_INTERP_BILINEAR, 255);
                g_object_unref(scaled);

                icon_list = g_list_append(icon_list, canvas);
            }

            if (icon_list)
                gtk_window_set_icon_list(GTK_WINDOW(b->window), icon_list);

            g_list_free_full(icon_list, g_object_unref);
            g_object_unref(raw);
        }
    }

    /* NOTE: the "destroy" signal is connected later, after ctx is
     * initialised, so the callback can set ctx.shutting_down. */

    /* Match the old binary's main-window backdrop override. */
    static gboolean backdrop_installed;
    if (!backdrop_installed) {
        GtkCssProvider *bd_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(bd_css,
            ".evemon-main-window:backdrop { opacity: 1.0; }", -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(bd_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(bd_css);
        backdrop_installed = TRUE;
    }

    /* ── tree store & view ───────────────────────────────────── */
    b->store = gtk_tree_store_new(NUM_COLS,
                                             G_TYPE_INT,      /* PID          */
                                             G_TYPE_INT,      /* PPID         */
                                             G_TYPE_STRING,   /* USER         */
                                             G_TYPE_STRING,   /* NAME         */
                                             G_TYPE_INT,      /* CPU% × 10000*/
                                             G_TYPE_STRING,   /* CPU% text    */
                                             G_TYPE_INT,      /* RSS (KiB)    */
                                             G_TYPE_STRING,   /* RSS text     */
                                             G_TYPE_INT,      /* group RSS    */
                                             G_TYPE_STRING,   /* group RSS txt*/
                                             G_TYPE_INT,      /* group CPU%   */
                                             G_TYPE_STRING,   /* group CPU txt*/
                                             G_TYPE_INT,      /* IO read rate */
                                             G_TYPE_STRING,   /* IO read text */
                                             G_TYPE_INT,      /* IO write rate*/
                                             G_TYPE_STRING,   /* IO write text*/
                                             G_TYPE_INT64,    /* start time   */
                                             G_TYPE_STRING,   /* start time txt*/
                                             G_TYPE_STRING,   /* container    */
                                             G_TYPE_STRING,   /* service      */
                                             G_TYPE_STRING,   /* CWD          */
                                             G_TYPE_STRING,   /* CMDLINE      */
                                             G_TYPE_STRING,   /* IO_SPARKLINE */
                                             G_TYPE_INT,      /* IO_SPARKLINE_PEAK */
                                             G_TYPE_INT64,    /* HIGHLIGHT_BORN */
                                             G_TYPE_INT64,    /* HIGHLIGHT_DIED */
                                             G_TYPE_INT,      /* PINNED_ROOT  */
                                             GDK_TYPE_PIXBUF, /* ICON          */
                                             G_TYPE_INT,      /* GPU%          */
                                             G_TYPE_STRING,   /* GPU% text     */
                                             G_TYPE_INT64,    /* GPU VRAM bytes*/
                                             G_TYPE_STRING,   /* GPU VRAM text */
                                             G_TYPE_INT,      /* GPU ENC%      */
                                             G_TYPE_STRING,   /* GPU ENC text  */
                                             G_TYPE_BOOLEAN, /* GPU client    */
                                             G_TYPE_UINT64, G_TYPE_UINT64);

    /* Sort model wraps the store directly (no filter model – we use
     * a shadow store approach for filtering instead). */
    b->sort_model = gtk_tree_model_sort_new_with_model(
        GTK_TREE_MODEL(b->store));

    b->tree = gtk_tree_view_new_with_model(b->sort_model);
    /* Don't unref sort_model – kept in ctx */

    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(b->tree), TRUE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(b->tree), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(b->tree), FALSE);

    /* Columns */


    b->r = gtk_cell_renderer_text_new();
    b->col = gtk_tree_view_column_new_with_attributes("PID", b->r,
                                                   "text", COL_PID, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_PID);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);
    b->pid_col = b->col;   /* save for Go-to-PID positioning */

    b->r = gtk_cell_renderer_text_new();
    b->col = gtk_tree_view_column_new_with_attributes("PPID", b->r,
                                                   "text", COL_PPID, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_PPID);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    b->col = gtk_tree_view_column_new_with_attributes("User", b->r,
                                                   "text", COL_USER, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_USER);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    b->name_text_r = b->r;   /* saved for cell-data-func wiring below */
    b->col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(b->col, "Name");
    {
        /* Icon renderer — packed before the text, bound to COL_ICON */
        GtkCellRenderer *icon_r = gtk_cell_renderer_pixbuf_new();
        g_object_set(icon_r, "yalign", 0.5f, NULL);
        gtk_tree_view_column_pack_start(b->col, icon_r, FALSE);
        gtk_tree_view_column_add_attribute(b->col, icon_r, "pixbuf", COL_ICON);
    }
    gtk_tree_view_column_pack_start(b->col, b->r, TRUE);
    gtk_tree_view_column_add_attribute(b->col, b->r, "text", COL_NAME);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_NAME);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 150);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);
    b->name_col = b->col;  /* save for filter positioning */

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);
    b->col = gtk_tree_view_column_new_with_attributes("CPU%", b->r,
                                                   "text", COL_CPU_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_CPU);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);
    b->col = gtk_tree_view_column_new_with_attributes("GPU", b->r,
                                                   "text", COL_GPU_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_GPU);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 60);
    g_object_set_data(G_OBJECT(b->col), "evemon-gpu-column",
                      GINT_TO_POINTER(1));
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);
    b->gpu_process_cols[0] = b->col;

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);
    b->col = gtk_tree_view_column_new_with_attributes("VRAM", b->r,
                                                   "text", COL_GPU_VRAM_TEXT,
                                                   NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_GPU_VRAM);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 80);
    g_object_set_data(G_OBJECT(b->col), "evemon-gpu-column",
                      GINT_TO_POINTER(2));
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);
    b->gpu_process_cols[1] = b->col;

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);
    b->col = gtk_tree_view_column_new_with_attributes("ENC", b->r,
                                                   "text", COL_GPU_ENC_TEXT,
                                                   NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_GPU_ENC);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 55);
    g_object_set_data(G_OBJECT(b->col), "evemon-gpu-column",
                      GINT_TO_POINTER(3));
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);
    b->gpu_process_cols[2] = b->col;

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);   /* right-align numbers */
    b->col = gtk_tree_view_column_new_with_attributes("Memory (RSS)", b->r,
                                                   "text", COL_RSS_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_RSS);  /* sort by raw value */
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);
    b->col = gtk_tree_view_column_new_with_attributes("Group Memory (RSS)", b->r,
                                                   "text", COL_GROUP_RSS_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_GROUP_RSS);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 120);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);
    b->col = gtk_tree_view_column_new_with_attributes("Group CPU%", b->r,
                                                   "text", COL_GROUP_CPU_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_GROUP_CPU);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);
    b->col = gtk_tree_view_column_new_with_attributes("Disk Read", b->r,
                                                   "text", COL_IO_READ_RATE_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_IO_READ_RATE);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    g_object_set(b->r, "xalign", 1.0f, NULL);
    b->col = gtk_tree_view_column_new_with_attributes("Disk Write", b->r,
                                                   "text", COL_IO_WRITE_RATE_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_IO_WRITE_RATE);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    /* I/O Sparkline — custom cell renderer with animated bar chart */
    {
        GtkCellRenderer *spark_r = sparkline_cell_renderer_new();
        b->col = gtk_tree_view_column_new_with_attributes("I/O History", spark_r,
                                                       "spark-data", COL_IO_SPARKLINE,
                                                       "spark-peak", COL_IO_SPARKLINE_PEAK,
                                                       NULL);
        gtk_tree_view_column_set_resizable(b->col, TRUE);
        gtk_tree_view_column_set_min_width(b->col, 86);
        gtk_tree_view_column_set_fixed_width(b->col, 86);
        gtk_tree_view_column_set_sizing(b->col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);
    }

    b->r = gtk_cell_renderer_text_new();
    b->col = gtk_tree_view_column_new_with_attributes("Start Time", b->r,
                                                   "text", COL_START_TIME_TEXT, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_START_TIME);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 140);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    b->col = gtk_tree_view_column_new_with_attributes("Container", b->r,
                                                   "text", COL_CONTAINER, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_CONTAINER);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    b->col = gtk_tree_view_column_new_with_attributes("Service", b->r,
                                                   "text", COL_SERVICE, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_SERVICE);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 120);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    b->col = gtk_tree_view_column_new_with_attributes("CWD", b->r,
                                                   "text", COL_CWD, NULL);
    gtk_tree_view_column_set_sort_column_id(b->col, COL_CWD);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 150);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    b->r = gtk_cell_renderer_text_new();
    b->col = gtk_tree_view_column_new_with_attributes("Command", b->r,
                                                   "text", COL_CMDLINE, NULL);
    gtk_tree_view_column_set_resizable(b->col, TRUE);
    gtk_tree_view_column_set_min_width(b->col, 300);
    gtk_tree_view_append_column(GTK_TREE_VIEW(b->tree), b->col);

    /* Wire the highlight cell-data function to every column so that
     * newly spawned (green) and dying (red) rows get a full-row
     * background colour that fades over HIGHLIGHT_FADE_US.          */
    {
        GList *cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(b->tree));
        for (GList *c = cols; c; c = c->next) {
            GtkTreeViewColumn *tv_col = c->data;
            GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(tv_col));
            for (GList *r2 = renderers; r2; r2 = r2->next) {
                gtk_tree_view_column_set_cell_data_func(
                    tv_col, r2->data, gtk_ui_highlight_cell_data_func, NULL, NULL);
            }
            g_list_free(renderers);
        }
        g_list_free(cols);
    }

    /* Apply column visibility from settings.  GPU columns retain their
     * requested state but remain hidden until the GPU System plugin passes
     * its availability probe and is enabled. */
    {
        const evemon_settings_t *s = settings_get();
        GList *all = gtk_tree_view_get_columns(GTK_TREE_VIEW(b->tree));
        for (GList *c = all; c; c = c->next) {
            GtkTreeViewColumn *tv_col = c->data;
            const char *title = gtk_tree_view_column_get_title(tv_col);
            if (!title) continue;
            gboolean requested = s->column_count == 0;
            if (!requested) {
                for (int i = 0; i < s->column_count; i++) {
                    if (strcmp(s->columns[i], title) == 0) {
                        requested = TRUE;
                        break;
                    }
                }
            }
            if (g_object_get_data(G_OBJECT(tv_col), "evemon-gpu-column")) {
                g_object_set_data(G_OBJECT(tv_col), "evemon-gpu-wanted",
                                  GINT_TO_POINTER(requested ? 1 : 0));
                g_object_set_data(G_OBJECT(tv_col), "evemon-gpu-available",
                                  GINT_TO_POINTER(0));
                gtk_tree_view_column_set_visible(tv_col, FALSE);
            } else {
                gtk_tree_view_column_set_visible(tv_col, requested);
            }
        }
        g_list_free(all);
    }

    /* Register inverted sort functions so ▲ = largest/highest first. */
    register_sort_funcs(GTK_TREE_MODEL_SORT(b->sort_model));

    /* Use a monospace font for the tree via CSS */
    b->css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(b->css,
        "treeview { font-family: Monospace; font-size: 9pt; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(b->tree),
        GTK_STYLE_PROVIDER(b->css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    /* NOTE: don't unref css – kept alive for dynamic font changes */

    /* ── scrolled window ─────────────────────────────────────── */
    b->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(b->scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(b->scroll), b->tree);

    /* ── name-filter entry (overlaid on the column header) ────── */
    b->name_filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->name_filter_entry), "filter…");
    gtk_entry_set_width_chars(GTK_ENTRY(b->name_filter_entry), 16);
    gtk_widget_set_no_show_all(b->name_filter_entry, TRUE);
    gtk_widget_set_valign(b->name_filter_entry, GTK_ALIGN_START);
    gtk_widget_set_halign(b->name_filter_entry, GTK_ALIGN_START);

    /* Compact CSS: small font, minimal padding so it fits the header row.
     * NOTE: don't unref – kept alive for dynamic font changes. */
    b->filt_css = gtk_css_provider_new();
    {
        char fbuf[128];
        snprintf(fbuf, sizeof(fbuf),
                 "entry {"
                 "  font-size: %dpt;"
                 "  min-height: 0;"
                 "  padding: 1px 4px;"
                 "  border-radius: 3px;"
                 "}", FONT_SIZE_DEFAULT);
        gtk_css_provider_load_from_data(b->filt_css, fbuf, -1, NULL);
    }
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(b->name_filter_entry),
        GTK_STYLE_PROVIDER(b->filt_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    b->tree_overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(b->tree_overlay), b->scroll);
    gtk_overlay_add_overlay(GTK_OVERLAY(b->tree_overlay), b->name_filter_entry);

    /* ── Go-to-PID entry (overlaid on the PID column header) ──── */
    b->pid_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->pid_entry), "PID…");
    gtk_entry_set_width_chars(GTK_ENTRY(b->pid_entry), 8);
    gtk_entry_set_input_purpose(GTK_ENTRY(b->pid_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_no_show_all(b->pid_entry, TRUE);
    gtk_widget_set_valign(b->pid_entry, GTK_ALIGN_START);
    gtk_widget_set_halign(b->pid_entry, GTK_ALIGN_START);

    /* NOTE: don't unref – kept alive for dynamic font changes. */
    b->pid_entry_css = gtk_css_provider_new();
    {
        char pbuf[128];
        snprintf(pbuf, sizeof(pbuf),
                 "entry {"
                 "  font-size: %dpt;"
                 "  min-height: 0;"
                 "  padding: 1px 4px;"
                 "  border-radius: 3px;"
                 "}", FONT_SIZE_DEFAULT);
        gtk_css_provider_load_from_data(b->pid_entry_css, pbuf, -1, NULL);
    }
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(b->pid_entry),
        GTK_STYLE_PROVIDER(b->pid_entry_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    gtk_overlay_add_overlay(GTK_OVERLAY(b->tree_overlay), b->pid_entry);

}
