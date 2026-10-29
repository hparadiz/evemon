#include "net_internal.h"

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.net",
    "Network",
    "1.0",
    EVEMON_ROLE_PROCESS,
    NULL
);

/* ── classification from description string ──────────────────── */

static int classify_socket(const char *desc)
{
    if (!desc || !desc[0]) return NET_CAT_OTHER;

    int is_listening = (strstr(desc, "(listening)") != NULL);

    if (strncmp(desc, "TCP6 ", 5) == 0 || strncmp(desc, "tcp6 ", 5) == 0)
        return is_listening ? NET_CAT_TCP6_LISTEN : NET_CAT_TCP6_CONN;
    if (strncmp(desc, "UDP6 ", 5) == 0 || strncmp(desc, "udp6 ", 5) == 0)
        return NET_CAT_UDP6;
    if (strncmp(desc, "TCP ", 4) == 0 || strncmp(desc, "tcp ", 4) == 0)
        return is_listening ? NET_CAT_TCP_LISTEN : NET_CAT_TCP_CONN;
    if (strncmp(desc, "UDP ", 4) == 0 || strncmp(desc, "udp ", 4) == 0)
        return NET_CAT_UDP;

    return NET_CAT_OTHER;
}

/* ── format helpers ──────────────────────────────────────────── */

void format_rate(uint64_t bytes, char *buf, size_t bufsz)
{
    double v = (double)bytes;
    if (v < 1.0)
        snprintf(buf, bufsz, "0 B/s");
    else if (v < 1024.0)
        snprintf(buf, bufsz, "%.0f B/s", v);
    else if (v < 1048576.0)
        snprintf(buf, bufsz, "%.1f KiB/s", v / 1024.0);
    else if (v < 1073741824.0)
        snprintf(buf, bufsz, "%.1f MiB/s", v / 1048576.0);
    else
        snprintf(buf, bufsz, "%.2f GiB/s", v / 1073741824.0);
}

/* ── markup builder ──────────────────────────────────────────── */

/*
 * Build rich Pango markup for a socket entry.
 *
 * Active connections with throughput get colored rate indicators.
 * The protocol prefix is dimmed, listening sockets are styled
 * distinctively, and addresses are the primary visual element.
 */
static char *sock_to_markup(const evemon_socket_t *s)
{
    char *desc_esc = g_markup_escape_text(s->desc, -1);

    /* Split the description to colour the protocol prefix */
    const char *space = strchr(s->desc, ' ');
    char *proto_esc = NULL;
    char *addr_esc  = NULL;

    if (space) {
        char proto[16];
        size_t plen = (size_t)(space - s->desc);
        if (plen >= sizeof(proto)) plen = sizeof(proto) - 1;
        memcpy(proto, s->desc, plen);
        proto[plen] = '\0';
        proto_esc = g_markup_escape_text(proto, -1);
        addr_esc  = g_markup_escape_text(space + 1, -1);
    }

    char *markup = NULL;
    int is_listening = strstr(s->desc, "(listening)") != NULL;

    if (s->total > 0) {
        /* Active connection with throughput data */
        char send_buf[32], recv_buf[32];
        /* Reverse upload/download display: show recv where send was, and
         * send where recv was. Keep arrow positions the same. */
        format_rate(s->recv_delta, send_buf, sizeof(send_buf));
        format_rate(s->send_delta, recv_buf, sizeof(recv_buf));

        if (proto_esc && addr_esc) {
            markup = g_strdup_printf(
                "<span foreground=\"#888888\">%s</span> %s  "
                "<span foreground=\"#6699cc\">\xe2\x86\x93 %s</span>  "
                "<span foreground=\"#88cc88\">\xe2\x86\x91 %s</span>",
                proto_esc, addr_esc, send_buf, recv_buf);
        } else {
            markup = g_strdup_printf(
                "%s  "
                "<span foreground=\"#6699cc\">\xe2\x86\x93 %s</span>  "
                "<span foreground=\"#88cc88\">\xe2\x86\x91 %s</span>",
                desc_esc, send_buf, recv_buf);
        }
    } else if (is_listening && proto_esc && addr_esc) {
        markup = g_strdup_printf(
            "<span foreground=\"#888888\">%s</span> "
            "<span foreground=\"#ccaa44\">%s</span>",
            proto_esc, addr_esc);
    } else if (proto_esc && addr_esc) {
        markup = g_strdup_printf(
            "<span foreground=\"#888888\">%s</span> %s",
            proto_esc, addr_esc);
    } else {
        markup = g_strdup(desc_esc);
    }

    g_free(desc_esc);
    g_free(proto_esc);
    g_free(addr_esc);
    return markup;
}

/* ── category header markup ──────────────────────────────────── */

static char *cat_header_markup(int cat, size_t cnt,
                               uint64_t cat_send, uint64_t cat_recv)
{
    if (cat_send > 0 || cat_recv > 0) {
        char sbuf[32], rbuf[32];
        /* Swap values so the upload/download display is reversed */
        format_rate(cat_recv, sbuf, sizeof(sbuf));
        format_rate(cat_send, rbuf, sizeof(rbuf));
        return g_strdup_printf(
            "<b>%s %s</b> <small>(%zu)</small>  "
            "<span foreground=\"#6699cc\"><small>\xe2\x86\x93 %s</small></span>  "
            "<span foreground=\"#88cc88\"><small>\xe2\x86\x91 %s</small></span>",
            cat_icons[cat], cat_labels[cat], cnt, sbuf, rbuf);
    }
    return g_strdup_printf(
        "<b>%s %s</b> <small>(%zu)</small>",
        cat_icons[cat], cat_labels[cat], cnt);
}

/* ── signal callbacks ────────────────────────────────────────── */

static void on_row_collapsed(GtkTreeView *v, GtkTreeIter *it,
                             GtkTreePath *p, gpointer data)
{
    (void)v; (void)p;
    net_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), it, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < NET_CAT_COUNT) ctx->collapsed |= (1u << cat);
}

static void on_row_expanded(GtkTreeView *v, GtkTreeIter *it,
                            GtkTreePath *p, gpointer data)
{
    (void)v; (void)p;
    net_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), it, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < NET_CAT_COUNT) ctx->collapsed &= ~(1u << cat);
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *ev,
                             gpointer data)
{
    (void)data;
    if (ev->keyval != GDK_KEY_Return && ev->keyval != GDK_KEY_KP_Enter)
        return FALSE;

    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return FALSE;

    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, COL_CAT, &cat_id, -1);
    if (cat_id < 0) return FALSE;

    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    if (!path) return FALSE;
    if (gtk_tree_view_row_expanded(view, path))
        gtk_tree_view_collapse_row(view, path);
    else
        gtk_tree_view_expand_row(view, path, FALSE);
    gtk_tree_path_free(path);
    return TRUE;
}

/* ── tooltip callback ────────────────────────────────────────── */

static gboolean on_query_tooltip(GtkWidget *widget, gint x, gint y,
                                 gboolean keyboard_mode,
                                 GtkTooltip *tooltip, gpointer data)
{
    (void)data;
    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeModel *model;
    GtkTreePath *path;
    GtkTreeIter iter;

    if (!gtk_tree_view_get_tooltip_context(view, &x, &y, keyboard_mode,
                                           &model, &path, &iter)) {
        return FALSE;
    }

    gint cat = -1;
    gtk_tree_model_get(model, &iter, COL_CAT, &cat, -1);

    if (cat < 0) {
        /* Leaf row: show full socket description as tooltip */
        gchar *text = NULL;
        gtk_tree_model_get(model, &iter, COL_TEXT, &text, -1);
        if (text && text[0]) {
            gtk_tooltip_set_text(tooltip, text);
            gtk_tree_view_set_tooltip_row(view, tooltip, path);
            g_free(text);
            gtk_tree_path_free(path);
            return TRUE;
        }
        g_free(text);
    }

    gtk_tree_path_free(path);
    return FALSE;
}

/* ── paned initial split helper ─────────────────────────────── */

static void net_paned_size_allocate(GtkWidget *paned,
                                    GdkRectangle *alloc,
                                    gpointer data)
{
    (void)data;
    /* Fire once: set divider to 50%, then disconnect. */
    gint half = alloc->width / 2;
    gtk_paned_set_position(GTK_PANED(paned), half);
    g_signal_handlers_disconnect_by_func(paned,
        G_CALLBACK(net_paned_size_allocate), data);
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *net_create_widget(void *opaque)
{
    net_ctx_t *ctx = opaque;

    ctx->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* ── Aggregate throughput header ──────────────────────────── */
    ctx->header_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(ctx->header_label),
        "<small>No network sockets</small>");
    gtk_label_set_xalign(GTK_LABEL(ctx->header_label), 0.0f);
    gtk_widget_set_margin_start(ctx->header_label, 6);
    gtk_widget_set_margin_end(ctx->header_label, 6);
    gtk_widget_set_margin_top(ctx->header_label, 4);
    gtk_widget_set_margin_bottom(ctx->header_label, 4);

    GtkWidget *header_box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(header_box), ctx->header_label);

    gtk_box_pack_start(GTK_BOX(ctx->main_box), header_box,
                       FALSE, FALSE, 0);

    /* ── Separator ────────────────────────────────────────────── */
    gtk_box_pack_start(GTK_BOX(ctx->main_box),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* ── Checkbox bar ─────────────────────────────────────────── */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 6);
    gtk_widget_set_margin_end(hbox, 6);
    gtk_widget_set_margin_top(hbox, 2);
    gtk_widget_set_margin_bottom(hbox, 2);

    ctx->chk_desc = gtk_check_button_new_with_label("Include Descendants");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->chk_desc),
                                  ctx->include_desc);
    gtk_box_pack_start(GTK_BOX(hbox), ctx->chk_desc, FALSE, FALSE, 0);

    /* Resolution dropdown */
    GtkWidget *res_lbl = gtk_label_new("Resolution:");
    gtk_box_pack_end(GTK_BOX(hbox), res_lbl, FALSE, FALSE, 0);

    ctx->combo_res = gtk_combo_box_text_new();
    for (int i = 0; i < NET_RES_COUNT; i++)
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(ctx->combo_res), net_resolutions[i].label);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->combo_res), ctx->res_idx);
    gtk_box_pack_end(GTK_BOX(hbox), ctx->combo_res, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(ctx->main_box), hbox, FALSE, FALSE, 0);

    /* ── Horizontal split: tree (left) | chart (right) ───────── */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    /* ── Connection tree (left pane) ─────────────────────────── */
    ctx->store = gtk_tree_store_new(NUM_COLS,
                                    G_TYPE_STRING,   /* text      */
                                    G_TYPE_STRING,   /* markup    */
                                    G_TYPE_INT,      /* cat       */
                                    G_TYPE_INT64);   /* sort key  */

    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    g_object_unref(ctx->store);

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);
    gtk_tree_view_set_enable_search(ctx->view, FALSE);
    gtk_tree_view_set_enable_tree_lines(ctx->view, TRUE);
    gtk_widget_set_has_tooltip(GTK_WIDGET(ctx->view), TRUE);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Net", cell, "markup", COL_MARKUP, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(ctx->view, col);

    /* Monospace CSS for alignment */
    ctx->css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(ctx->css,
        "treeview { font-family: Monospace; font-size: 8pt; }", -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(GTK_WIDGET(ctx->view)),
        GTK_STYLE_PROVIDER(ctx->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);

    g_signal_connect(ctx->view, "row-collapsed",
                     G_CALLBACK(on_row_collapsed), ctx);
    g_signal_connect(ctx->view, "row-expanded",
                     G_CALLBACK(on_row_expanded), ctx);
    g_signal_connect(ctx->view, "key-press-event",
                     G_CALLBACK(on_key_press), ctx);
    g_signal_connect(ctx->view, "query-tooltip",
                     G_CALLBACK(on_query_tooltip), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);
    gtk_widget_set_hexpand(ctx->scroll, TRUE);

    /* ── Throughput chart (right pane) ───────────────────────── */
    ctx->chart = gtk_drawing_area_new();
    gtk_widget_set_size_request(ctx->chart, 80, -1);  /* min width */
    gtk_widget_set_vexpand(ctx->chart, TRUE);
    gtk_widget_set_hexpand(ctx->chart, TRUE);
    g_signal_connect(ctx->chart, "draw", G_CALLBACK(on_chart_draw), ctx);
    gtk_widget_add_events(ctx->chart,
        GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(ctx->chart, "motion-notify-event",
                     G_CALLBACK(on_chart_motion), ctx);
    g_signal_connect(ctx->chart, "leave-notify-event",
                     G_CALLBACK(on_chart_leave), ctx);

    gtk_paned_pack1(GTK_PANED(hpaned), ctx->scroll, TRUE, TRUE);
    gtk_paned_pack2(GTK_PANED(hpaned), ctx->chart,  TRUE, TRUE);

    /* Position divider at 50% on first allocation, then let user drag freely */
    g_signal_connect(hpaned, "size-allocate",
                     G_CALLBACK(net_paned_size_allocate), NULL);

    gtk_box_pack_start(GTK_BOX(ctx->main_box), hpaned, TRUE, TRUE, 0);

    gtk_widget_show_all(ctx->main_box);

    return ctx->main_box;
}

static void net_update(void *opaque, const evemon_proc_data_t *data)
{
    net_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed   = 0;
        ctx->last_pid    = data->pid;
        ctx->hist_head   = 0;
        ctx->hist_count  = 0;
        ctx->has_ebpf    = FALSE;
        ctx->tick_accum  = 0;
        ctx->acc_send    = 0;
        ctx->acc_recv    = 0;
        ctx->acc_active  = 0;
        memset(ctx->acc_cat,        0, sizeof(ctx->acc_cat));
        memset(ctx->send_hist,      0, sizeof(ctx->send_hist));
        memset(ctx->recv_hist,      0, sizeof(ctx->recv_hist));
        memset(ctx->cat_count_hist, 0, sizeof(ctx->cat_count_hist));
        memset(ctx->active_hist,    0, sizeof(ctx->active_hist));
    }

    /* Read checkbox state */
    ctx->include_desc = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->chk_desc));

    /* Read resolution dropdown; reset history + accumulator on change */
    if (ctx->combo_res) {
        int new_res = gtk_combo_box_get_active(GTK_COMBO_BOX(ctx->combo_res));
        if (new_res < 0) new_res = NET_RES_DEFAULT;
        if (new_res != ctx->res_idx) {
            ctx->res_idx    = new_res;
            ctx->tick_accum = 0;
            ctx->acc_send   = 0;
            ctx->acc_recv   = 0;
            ctx->acc_active = 0;
            memset(ctx->acc_cat,      0, sizeof(ctx->acc_cat));
            ctx->hist_head  = 0;
            ctx->hist_count = 0;
            memset(ctx->send_hist,      0, sizeof(ctx->send_hist));
            memset(ctx->recv_hist,      0, sizeof(ctx->recv_hist));
            memset(ctx->cat_count_hist, 0, sizeof(ctx->cat_count_hist));
            memset(ctx->active_hist,    0, sizeof(ctx->active_hist));
        }
    }

    size_t total = data->socket_count;

    if (total == 0) {
        gtk_tree_store_clear(ctx->store);
        gtk_label_set_markup(GTK_LABEL(ctx->header_label),
            "<small>No network sockets</small>");
        /* Count this as a zero tick against the accumulator */
        ctx->tick_accum++;
        if (ctx->tick_accum >= net_resolutions[ctx->res_idx].ticks) {
            ctx->send_hist[ctx->hist_head] = 0;
            ctx->recv_hist[ctx->hist_head] = 0;
            for (int c = 0; c < NET_CAT_COUNT; c++)
                ctx->cat_count_hist[c][ctx->hist_head] = 0;
            ctx->hist_head = (ctx->hist_head + 1) % NET_HISTORY_LEN;
            if (ctx->hist_count < NET_HISTORY_LEN) ctx->hist_count++;
            ctx->tick_accum = 0;
            ctx->acc_send = 0;
            ctx->acc_recv = 0;
            memset(ctx->acc_cat, 0, sizeof(ctx->acc_cat));
            gtk_widget_queue_draw(ctx->chart);
        }
        return;
    }

    /* ── Bucket sockets into categories ───────────────────────── */
    typedef struct {
        const evemon_socket_t *sock;
        int cat;
    } sock_ent_t;

    sock_ent_t *ents = g_new0(sock_ent_t, total);
    size_t cat_count[NET_CAT_COUNT] = {0};
    uint64_t cat_send[NET_CAT_COUNT] = {0};
    uint64_t cat_recv[NET_CAT_COUNT] = {0};
    uint64_t agg_send = 0, agg_recv = 0;
    size_t active_count = 0;
    size_t visible = 0;

    for (size_t i = 0; i < total; i++) {
        /* Filter out descendant sockets when checkbox is unchecked */
        if (!ctx->include_desc &&
            data->sockets[i].source_pid != data->pid)
            continue;

        ents[visible].sock = &data->sockets[i];
        ents[visible].cat  = classify_socket(data->sockets[i].desc);
        cat_count[ents[visible].cat]++;
        cat_send[ents[visible].cat] += data->sockets[i].send_delta;
        cat_recv[ents[visible].cat] += data->sockets[i].recv_delta;
        agg_send += data->sockets[i].send_delta;
        agg_recv += data->sockets[i].recv_delta;
        if (data->sockets[i].send_delta > 0 || data->sockets[i].recv_delta > 0) active_count++;
        visible++;
    }

    if (visible == 0) {
        gtk_tree_store_clear(ctx->store);
        gtk_label_set_markup(GTK_LABEL(ctx->header_label),
            "<small>No network sockets</small>");
        ctx->tick_accum++;
        if (ctx->tick_accum >= net_resolutions[ctx->res_idx].ticks) {
            ctx->send_hist[ctx->hist_head] = 0;
            ctx->recv_hist[ctx->hist_head] = 0;
            ctx->active_hist[ctx->hist_head] = 0;
            for (int c = 0; c < NET_CAT_COUNT; c++)
                ctx->cat_count_hist[c][ctx->hist_head] = 0;
            ctx->hist_head = (ctx->hist_head + 1) % NET_HISTORY_LEN;
            if (ctx->hist_count < NET_HISTORY_LEN) ctx->hist_count++;
            ctx->tick_accum = 0;
            ctx->acc_send = 0;
            ctx->acc_recv = 0;
            ctx->acc_active = 0;
            memset(ctx->acc_cat, 0, sizeof(ctx->acc_cat));
            gtk_widget_queue_draw(ctx->chart);
        }
        g_free(ents);
        return;
    }

    /* ── Update aggregate header ──────────────────────────────── */
    {
        char *hdr;
        if (agg_send > 0 || agg_recv > 0) {
            char sbuf[32], rbuf[32];
            /* Reverse upload/download display in aggregate header */
            format_rate(agg_recv, sbuf, sizeof(sbuf));
            format_rate(agg_send, rbuf, sizeof(rbuf));
            hdr = g_strdup_printf(
                "<small><b>%zu</b> connection%s"
                "  \xc2\xb7  <b>%zu</b> active"
                "  \xc2\xb7  <span foreground=\"#6699cc\">\xe2\x86\x93 %s</span>"
                "  <span foreground=\"#88cc88\">\xe2\x86\x91 %s</span></small>",
                visible, visible == 1 ? "" : "s",
                active_count, sbuf, rbuf);
        } else {
            hdr = g_strdup_printf(
                "<small><b>%zu</b> connection%s"
                "  \xc2\xb7  <b>%zu</b> active</small>",
                visible, visible == 1 ? "" : "s",
                active_count);
        }
        gtk_label_set_markup(GTK_LABEL(ctx->header_label), hdr);
        g_free(hdr);
    }

    /* ── Accumulate and push to ring at the chosen interval ─────── */
    {
        if (agg_send > 0 || agg_recv > 0)
            ctx->has_ebpf = TRUE;

        int ticks_needed = net_resolutions[ctx->res_idx].ticks;

        /* Accumulate: sum bytes, keep max cat count per bucket */
        ctx->acc_send += agg_send;
        ctx->acc_recv += agg_recv;
        for (int c = 0; c < NET_CAT_COUNT; c++)
            if ((uint32_t)cat_count[c] > ctx->acc_cat[c])
                ctx->acc_cat[c] = (uint32_t)cat_count[c];
        if ((uint32_t)active_count > ctx->acc_active)
            ctx->acc_active = (uint32_t)active_count;
        ctx->tick_accum++;

        if (ctx->tick_accum >= ticks_needed) {
            /* Commit averaged bytes-per-second and max counts to ring */
            ctx->send_hist[ctx->hist_head] = ctx->acc_send / (uint64_t)ticks_needed;
            ctx->recv_hist[ctx->hist_head] = ctx->acc_recv / (uint64_t)ticks_needed;
            for (int c = 0; c < NET_CAT_COUNT; c++)
                ctx->cat_count_hist[c][ctx->hist_head] = ctx->acc_cat[c];
            ctx->active_hist[ctx->hist_head] = ctx->acc_active;
            ctx->hist_head = (ctx->hist_head + 1) % NET_HISTORY_LEN;
            if (ctx->hist_count < NET_HISTORY_LEN)
                ctx->hist_count++;

            /* Reset accumulator */
            ctx->tick_accum = 0;
            ctx->acc_send   = 0;
            ctx->acc_recv   = 0;
            ctx->acc_active = 0;
            memset(ctx->acc_cat, 0, sizeof(ctx->acc_cat));

            gtk_widget_queue_draw(ctx->chart);
            if (ctx->chart_cache) {
                cairo_surface_destroy(ctx->chart_cache);
                ctx->chart_cache = NULL;
            }
        }
    }

    /* ── Update the tree store in place ───────────────────────── */
    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    /* Save scroll position */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    /* Index existing category rows */
    GtkTreeIter cat_iters[NET_CAT_COUNT];
    gboolean    cat_exists[NET_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cid = -1;
            gtk_tree_model_get(model, &top, COL_CAT, &cid, -1);
            if (cid >= 0 && cid < NET_CAT_COUNT) {
                cat_iters[cid]  = top;
                cat_exists[cid] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    /* Remove empty categories */
    for (int c = 0; c < NET_CAT_COUNT; c++) {
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
    for (int c = 0; c < NET_CAT_COUNT; c++) {
        if (cat_count[c] == 0) continue;

        char *hdr_markup = cat_header_markup(c, cat_count[c],
                                             cat_send[c], cat_recv[c]);
        char hdr_plain[128];
        snprintf(hdr_plain, sizeof(hdr_plain), "%s %s (%zu)",
                 cat_icons[c], cat_labels[c], cat_count[c]);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            gtk_tree_store_append(ctx->store, &parent, NULL);
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr_plain,
                               COL_MARKUP, hdr_markup,
                               COL_CAT, (gint)c,
                               COL_SORT_KEY, (gint64)0, -1);
            cat_exists[c] = TRUE;
            cat_iters[c] = parent;
        } else {
            parent = cat_iters[c];
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr_plain,
                               COL_MARKUP, hdr_markup, -1);
        }
        g_free(hdr_markup);

        /* Update children in place */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);

        for (size_t i = 0; i < visible; i++) {
            if (ents[i].cat != c) continue;

            const evemon_socket_t *s = ents[i].sock;
            char *markup = sock_to_markup(s);
            gint64 sort_key = (gint64)s->total;

            if (child_valid) {
                gtk_tree_store_set(ctx->store, &child,
                                   COL_TEXT, s->desc,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1,
                                   COL_SORT_KEY, sort_key, -1);
                child_valid = gtk_tree_model_iter_next(model, &child);
            } else {
                GtkTreeIter new_child;
                gtk_tree_store_append(ctx->store, &new_child, &parent);
                gtk_tree_store_set(ctx->store, &new_child,
                                   COL_TEXT, s->desc,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1,
                                   COL_SORT_KEY, sort_key, -1);
            }
            g_free(markup);
        }

        /* Remove excess children */
        while (child_valid)
            child_valid = gtk_tree_store_remove(ctx->store, &child);

        /* Restore collapse state */
        GtkTreePath *cat_path = gtk_tree_model_get_path(model, &cat_iters[c]);
        if (ctx->collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    /* Restore scroll */
    gtk_adjustment_set_value(vadj, scroll_pos);
    g_free(ents);
}

static void net_clear(void *opaque)
{
    net_ctx_t *ctx = opaque;
    gtk_tree_store_clear(ctx->store);
    gtk_label_set_markup(GTK_LABEL(ctx->header_label),
        "<small>No network sockets</small>");
    ctx->last_pid   = 0;
    ctx->hist_head  = 0;
    ctx->hist_count = 0;
    ctx->has_ebpf   = FALSE;
    ctx->tick_accum = 0;
    ctx->acc_send   = 0;
    ctx->acc_recv   = 0;
    ctx->acc_active = 0;
    memset(ctx->acc_cat,        0, sizeof(ctx->acc_cat));
    memset(ctx->send_hist,      0, sizeof(ctx->send_hist));
    memset(ctx->recv_hist,      0, sizeof(ctx->recv_hist));
    memset(ctx->cat_count_hist, 0, sizeof(ctx->cat_count_hist));
    memset(ctx->active_hist,    0, sizeof(ctx->active_hist));
    if (ctx->chart_cache) {
        cairo_surface_destroy(ctx->chart_cache);
        ctx->chart_cache = NULL;
    }
    if (ctx->chart) gtk_widget_queue_draw(ctx->chart);
}

static void net_destroy(void *opaque)
{
    net_ctx_t *ctx = opaque;
    if (ctx->css) g_object_unref(ctx->css);
    if (ctx->chart_cache) cairo_surface_destroy(ctx->chart_cache);
    free(ctx);
}

static int net_wants_update(void *opaque)
{
    net_ctx_t *ctx = opaque;
    return ctx->main_box &&
           GTK_IS_WIDGET(ctx->main_box) &&
           gtk_widget_get_mapped(ctx->main_box) &&
           gtk_widget_get_child_visible(ctx->main_box);
}

/* ── descriptor ──────────────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    net_ctx_t *ctx = calloc(1, sizeof(net_ctx_t));
    if (!ctx) return NULL;
    ctx->include_desc = TRUE;
    ctx->res_idx      = NET_RES_DEFAULT;
    ctx->tooltip_x    = -1;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Network",
        .id            = "org.evemon.net",
        .version       = "1.0",
        .data_needs    = evemon_NEED_SOCKETS | evemon_NEED_DESCENDANTS,
        .plugin_ctx    = ctx,
        .create_widget = net_create_widget,
        .update        = net_update,
        .clear         = net_clear,
        .destroy       = net_destroy,
        .wants_update  = net_wants_update,
        .role          = EVEMON_ROLE_PROCESS,
        .dependencies  = NULL,
    };

    return p;
}
