#include "ui_gtk_private.h"

#include "ui_build.h"

void gtk_build_sidebar(ui_build_t *b)
{
    /* ── process detail panel ─────────────────────────────────── */

    /*
     * Helper: create a collapsible detail panel section.
     *
     * Returns a GtkBox (vertical) containing:
     *   [0] separator
     *   [1] event-box header (double-click to toggle)
     *   [2] content_widget (shown/hidden on collapse)
     *
     * *out_arrow receives the arrow label widget (▼/▶).
     * The event-box has "sb-section-header" as widget name for CSS.
     */
    b->sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(b->sidebar_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(b->sidebar_scroll), TRUE);
    gtk_widget_set_size_request(b->sidebar_scroll, 240, -1);

    b->sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(b->sidebar_vbox, 8);
    gtk_widget_set_margin_end(b->sidebar_vbox, 8);
    gtk_widget_set_margin_top(b->sidebar_vbox, 8);
    gtk_widget_set_margin_bottom(b->sidebar_vbox, 8);

    /* ── Process Info section (non-scrollable grid) ───────────── */
    b->info_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(b->info_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(b->info_grid), 8);

    /* Helper macro to add a label row to the info grid */
    #define SIDEBAR_ROW(row, key_str, label_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_END);     \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(b->info_grid), _k, 0, row, 1, 1);         \
        gtk_grid_attach(GTK_GRID(b->info_grid), _v, 1, row, 1, 1);         \
        label_var = GTK_LABEL(_v);                                       \
    } while (0)


    SIDEBAR_ROW(0,  "PID",             b->sb_pid);
    SIDEBAR_ROW(1,  "PPID",            b->sb_ppid);
    SIDEBAR_ROW(2,  "User",            b->sb_user);
    SIDEBAR_ROW(3,  "Name",            b->sb_name);
    SIDEBAR_ROW(4,  "CPU%",            b->sb_cpu);
    SIDEBAR_ROW(5,  "Memory (RSS)",    b->sb_rss);
    SIDEBAR_ROW(6,  "Group Memory",    b->sb_group_rss);
    SIDEBAR_ROW(7,  "Group CPU%",      b->sb_group_cpu);
    SIDEBAR_ROW(8,  "Disk Read",       b->sb_io_read);
    SIDEBAR_ROW(9,  "Disk Write",      b->sb_io_write);
    SIDEBAR_ROW(10, "Net Send",        b->sb_net_send);
    SIDEBAR_ROW(11, "Net Recv",        b->sb_net_recv);
    SIDEBAR_ROW(12, "Start Time",      b->sb_start_time);
    SIDEBAR_ROW(13, "Container",       b->sb_container);
    SIDEBAR_ROW(14, "Service",         b->sb_service);
    SIDEBAR_ROW(15, "CWD",            b->sb_cwd);
    SIDEBAR_ROW(16, "Command",         b->sb_cmdline);
    #undef SIDEBAR_ROW

    /* ── Steam / Proton metadata section ──────────────────────── */
    b->steam_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    b->steam_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(b->steam_box), b->steam_sep, FALSE, FALSE, 0);

    b->steam_header = gtk_label_new("Steam / Proton");
    gtk_label_set_xalign(GTK_LABEL(b->steam_header), 0.0f);
    {
        PangoAttrList *a = pango_attr_list_new();
        pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(b->steam_header), a);
        pango_attr_list_unref(a);
    }
    gtk_box_pack_start(GTK_BOX(b->steam_box), b->steam_header, FALSE, FALSE, 0);

    b->steam_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(b->steam_grid), 8);
    gtk_box_pack_start(GTK_BOX(b->steam_box), b->steam_grid, FALSE, FALSE, 0);

    #define SIDEBAR_ROW_S(row, key_str, label_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_END);     \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(b->steam_grid), _k, 0, row, 1, 1);        \
        gtk_grid_attach(GTK_GRID(b->steam_grid), _v, 1, row, 1, 1);        \
        label_var = GTK_LABEL(_v);                                       \
    } while (0)


    SIDEBAR_ROW_S(0, "Game",            b->sb_steam_game);
    SIDEBAR_ROW_S(1, "App ID",          b->sb_steam_appid);
    SIDEBAR_ROW_S(2, "Proton",          b->sb_steam_proton);
    SIDEBAR_ROW_S(3, "Runtime",         b->sb_steam_runtime);
    SIDEBAR_ROW_S(4, "Compat Data",     b->sb_steam_compat);
    SIDEBAR_ROW_S(5, "Game Directory",  b->sb_steam_gamedir);
    #undef SIDEBAR_ROW_S

    gtk_widget_set_no_show_all(b->steam_box, TRUE);

    /* ── cgroup resource limits section (conditionally visible) ── */
    b->cgroup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    b->cgroup_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(b->cgroup_box), b->cgroup_sep, FALSE, FALSE, 0);

    b->cgroup_header = gtk_label_new("cgroup Limits");
    gtk_label_set_xalign(GTK_LABEL(b->cgroup_header), 0.0f);
    {
        PangoAttrList *a = pango_attr_list_new();
        pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(b->cgroup_header), a);
        pango_attr_list_unref(a);
    }
    gtk_box_pack_start(GTK_BOX(b->cgroup_box), b->cgroup_header, FALSE, FALSE, 0);

    b->cgroup_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(b->cgroup_grid), 8);
    gtk_box_pack_start(GTK_BOX(b->cgroup_box), b->cgroup_grid, FALSE, FALSE, 0);

    /* Cgroup row helper macro (same pattern as SIDEBAR_ROW_S) */
    #define CGROUP_ROW(row, key_str, label_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_MIDDLE);  \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(b->cgroup_grid), _k, 0, row, 1, 1);       \
        gtk_grid_attach(GTK_GRID(b->cgroup_grid), _v, 1, row, 1, 1);       \
        label_var = GTK_LABEL(_v);                                       \
    } while (0)

    /* Variant that also stores the key widget for hide/show */
    #define CGROUP_ROW_K(row, key_str, label_var, key_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_MIDDLE);  \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(b->cgroup_grid), _k, 0, row, 1, 1);       \
        gtk_grid_attach(GTK_GRID(b->cgroup_grid), _v, 1, row, 1, 1);       \
        label_var = GTK_LABEL(_v);                                       \
        key_var = _k;                                                    \
    } while (0)


    CGROUP_ROW(0, "Path",     b->sb_cgroup_path);
    CGROUP_ROW(1, "Memory",   b->sb_cgroup_mem);
    CGROUP_ROW_K(2, "Mem High", b->sb_cgroup_mem_high, b->sb_cgroup_mem_high_key);
    CGROUP_ROW_K(3, "Swap",    b->sb_cgroup_swap, b->sb_cgroup_swap_key);
    CGROUP_ROW(4, "CPU",      b->sb_cgroup_cpu);
    CGROUP_ROW(5, "PIDs",     b->sb_cgroup_pids);
    CGROUP_ROW_K(6, "I/O",     b->sb_cgroup_io, b->sb_cgroup_io_key);
    #undef CGROUP_ROW
    #undef CGROUP_ROW_K

    gtk_widget_set_no_show_all(b->cgroup_box, TRUE);

    /*
     * ── Collapsible section header helper ───────────────────────
     *
     * Creates a horizontal box with an arrow label (▼/▶) and a bold
     * title, wrapped in a GtkEventBox that responds to double-click.
     * Returns the event box.  *out_arrow receives the arrow label.
     */
    /*
     * MAKE_SECTION: Build a complete collapsible section.
     *
     * section_var  – GtkWidget* to receive the outer section GtkBox
     * header_var   – GtkWidget* to receive the header event box
     * revealer_var – GtkWidget* to receive the GtkRevealer
     * arrow_var    – GtkWidget* to receive the arrow label
     * title        – section title string
     * content_w    – GtkWidget* of the content to place inside the revealer
     * expandable   – TRUE if this section should vexpand when open
     */
    #define MAKE_SECTION(section_var, header_var, revealer_var,         \
                         arrow_var, title, content_w, expandable)       \
    do {                                                                \
        /* Header: arrow + bold label in an event box */                \
        GtkWidget *_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);   \
        gtk_widget_set_margin_top(_hb, 2);                             \
        gtk_widget_set_margin_bottom(_hb, 2);                          \
        GtkWidget *_ar = gtk_label_new(SECTION_ARROW_EXPANDED);        \
        gtk_label_set_xalign(GTK_LABEL(_ar), 0.0f);                   \
        gtk_box_pack_start(GTK_BOX(_hb), _ar, FALSE, FALSE, 0);       \
        GtkWidget *_lbl = gtk_label_new(title);                        \
        gtk_label_set_xalign(GTK_LABEL(_lbl), 0.0f);                  \
        {                                                              \
            PangoAttrList *_a = pango_attr_list_new();                 \
            pango_attr_list_insert(_a,                                 \
                pango_attr_weight_new(PANGO_WEIGHT_BOLD));             \
            gtk_label_set_attributes(GTK_LABEL(_lbl), _a);            \
            pango_attr_list_unref(_a);                                 \
        }                                                              \
        gtk_box_pack_start(GTK_BOX(_hb), _lbl, TRUE, TRUE, 0);        \
        GtkWidget *_eb = gtk_event_box_new();                          \
        gtk_container_add(GTK_CONTAINER(_eb), _hb);                    \
        gtk_widget_add_events(_eb, GDK_BUTTON_PRESS_MASK              \
                                 | GDK_BUTTON_RELEASE_MASK            \
                                 | GDK_POINTER_MOTION_MASK);          \
        (arrow_var)  = _ar;                                            \
        (header_var) = _eb;                                            \
                                                                       \
        /* Revealer: smooth animated show/hide of content */           \
        GtkWidget *_rv = gtk_revealer_new();                           \
        gtk_revealer_set_transition_type(GTK_REVEALER(_rv),            \
            GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);                  \
        gtk_revealer_set_transition_duration(GTK_REVEALER(_rv),        \
            SECTION_TRANSITION_MS);                                    \
        gtk_revealer_set_reveal_child(GTK_REVEALER(_rv), TRUE);       \
        gtk_container_add(GTK_CONTAINER(_rv), (content_w));            \
        (revealer_var) = _rv;                                          \
                                                                       \
        /* Section box: separator + header + revealer */               \
        GtkWidget *_sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);    \
        gtk_box_pack_start(GTK_BOX(_sec),                              \
            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),             \
            FALSE, FALSE, 0);                                          \
        gtk_box_pack_start(GTK_BOX(_sec), _eb, FALSE, FALSE, 0);      \
        gtk_box_pack_start(GTK_BOX(_sec), _rv,                         \
            (expandable), (expandable), 0);                            \
        (section_var) = _sec;                                          \
    } while (0)

    /* ── Process Info collapsible section ─────────────────────── */
    /* Info section is special: always expanded, content = info_grid + steam_box
     * wrapped together in a vertical box.  We still use a revealer for
     * visual consistency when collapsing. */
    /* ── Process Metadata section (shown when proc_meta service plugin fires) ─ */
    b->meta_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    b->meta_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(b->meta_box), b->meta_sep, FALSE, FALSE, 4);

    b->meta_header = gtk_label_new("Software");
    gtk_label_set_xalign(GTK_LABEL(b->meta_header), 0.0f);
    {
        PangoAttrList *a = pango_attr_list_new();
        pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(b->meta_header), a);
        pango_attr_list_unref(a);
    }
    gtk_box_pack_start(GTK_BOX(b->meta_box), b->meta_header, FALSE, FALSE, 0);

    b->meta_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(b->meta_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(b->meta_grid), 8);
    gtk_box_pack_start(GTK_BOX(b->meta_box), b->meta_grid, FALSE, FALSE, 0);

    #define META_ROW(row, key_str, label_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_selectable(GTK_LABEL(_v), TRUE);                   \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_END);     \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        gtk_grid_attach(GTK_GRID(b->meta_grid), _k, 0, row, 1, 1);         \
        gtk_grid_attach(GTK_GRID(b->meta_grid), _v, 1, row, 1, 1);         \
        label_var = GTK_LABEL(_v);                                       \
    } while (0)

    #define META_LINK_ROW(row, key_str, label_var, button_var) do { \
        GtkWidget *_k = gtk_label_new(key_str);                          \
        gtk_label_set_xalign(GTK_LABEL(_k), 0.0f);                      \
        gtk_widget_set_halign(_k, GTK_ALIGN_START);                      \
        PangoAttrList *_a = pango_attr_list_new();                       \
        pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
        gtk_label_set_attributes(GTK_LABEL(_k), _a);                     \
        pango_attr_list_unref(_a);                                       \
        GtkWidget *_v = gtk_label_new("–");                              \
        gtk_label_set_xalign(GTK_LABEL(_v), 0.0f);                      \
        gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_END);     \
        gtk_widget_set_halign(_v, GTK_ALIGN_START);                      \
        gtk_widget_set_hexpand(_v, TRUE);                                \
        GtkWidget *_b = gtk_button_new();                                \
        gtk_container_add(GTK_CONTAINER(_b), _v);                        \
        gtk_widget_set_halign(_b, GTK_ALIGN_START);                     \
        gtk_widget_set_hexpand(_b, TRUE);                                \
        gtk_grid_attach(GTK_GRID(b->meta_grid), _k, 0, row, 1, 1);         \
        gtk_grid_attach(GTK_GRID(b->meta_grid), _b, 1, row, 1, 1);         \
        label_var = GTK_LABEL(_v);                                       \
        button_var = _b;                                                 \
    } while (0)


    META_ROW(0, "Organisation", b->sb_meta_organization);
    META_LINK_ROW(1, "Homepage", b->sb_meta_homepage, b->sb_meta_homepage_button);
    META_LINK_ROW(2, "Source",   b->sb_meta_source_url, b->sb_meta_source_url_button);
    META_LINK_ROW(3, "Funding",  b->sb_meta_funding_url, b->sb_meta_funding_url_button);
    META_ROW(4, "Licence",     b->sb_meta_license);
    META_ROW(5, "Summary",     b->sb_meta_summary);
    #undef META_ROW
    #undef META_LINK_ROW

    gtk_ui_make_metadata_label_clickable(b->sb_meta_homepage_button, b->sb_meta_homepage);
    gtk_ui_make_metadata_label_clickable(b->sb_meta_source_url_button, b->sb_meta_source_url);
    gtk_ui_make_metadata_label_clickable(b->sb_meta_funding_url_button, b->sb_meta_funding_url);

    gtk_widget_set_no_show_all(b->meta_box, TRUE);

    b->info_content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(b->info_content_box), b->info_grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->info_content_box), b->steam_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(b->info_content_box), b->cgroup_box, FALSE, FALSE, 0);
    gtk_box_pack_end  (GTK_BOX(b->info_content_box), b->meta_box,   FALSE, FALSE, 0);


    MAKE_SECTION(b->info_section, b->info_header_eb, b->info_revealer,
                 b->info_arrow, "Process Info", b->info_content_box, FALSE);
    (void)b->info_revealer;
    gtk_widget_set_name(b->info_section, "info-section");
    gtk_widget_set_valign(b->info_section, GTK_ALIGN_START);
    /* Info section is never collapsible – hide the arrow and header */
    gtk_widget_hide(b->info_arrow);
    gtk_widget_set_no_show_all(b->info_arrow, TRUE);
    /* Hide the "Process Info" header label and its separator/divider */
    gtk_widget_hide(b->info_header_eb);
    gtk_widget_set_no_show_all(b->info_header_eb, TRUE);
    {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(b->info_section));
        if (kids) {
            /* First child is the separator added by MAKE_SECTION */
            gtk_widget_hide(GTK_WIDGET(kids->data));
            gtk_widget_set_no_show_all(GTK_WIDGET(kids->data), TRUE);
            g_list_free(kids);
        }
    }

    #undef MAKE_SECTION

    /* ── Pack detail panel content ───────────────────────────────── */
    gtk_box_pack_start(GTK_BOX(b->sidebar_vbox), b->info_section, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(b->sidebar_scroll), b->sidebar_vbox);

    b->sidebar_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(b->sidebar_frame), b->sidebar_scroll);

    /* Overlay for the icon watermark — sits on top of sidebar_frame */
    b->sidebar_overlay = gtk_overlay_new();
    b->detail_icon_da = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(b->sidebar_overlay), b->sidebar_frame);
    /* The drawing area must not receive input events */
    gtk_widget_set_can_focus(b->detail_icon_da, FALSE);
    gtk_widget_set_events(b->detail_icon_da, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(b->sidebar_overlay), b->detail_icon_da);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(b->sidebar_overlay),
                                         b->detail_icon_da, TRUE);

    /* Live CSS provider for detail panel font size (cascades to all children) */
    b->sidebar_css = gtk_css_provider_new();
    {
        char sbuf[128];
        snprintf(sbuf, sizeof(sbuf),
                 "frame, label, checkbutton { font-size: %dpt; }",
                 FONT_SIZE_DEFAULT);
        gtk_css_provider_load_from_data(b->sidebar_css, sbuf, -1, NULL);
    }
    gtk_style_context_add_provider(gtk_widget_get_style_context(b->sidebar_frame),
        GTK_STYLE_PROVIDER(b->sidebar_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* ── layout: tree is the main content ───────────────────── */
    b->hpaned = b->tree_overlay;

}
