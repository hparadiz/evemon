/*
 * net_plugin.c – Network Sockets plugin for evemon.
 *
 * Displays network sockets open by a process with eBPF per-socket
 * throughput data.  Sockets are categorised by protocol and state
 * (TCP Connected, TCP Listening, UDP, IPv6) with aggregate throughput
 * in the category headers.  Active connections with traffic are
 * highlighted with colored send/recv rate indicators.
 *
 * Build:
 *   gcc -shared -fPIC -o evemon_net.so net_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>

/* ── categories ──────────────────────────────────────────────── */

enum {
    NET_CAT_TCP_CONN,      /* TCP connected (ESTABLISHED etc)  */
    NET_CAT_TCP_LISTEN,    /* TCP listening                     */
    NET_CAT_UDP,           /* UDP (v4)                          */
    NET_CAT_TCP6_CONN,     /* TCP6 connected                    */
    NET_CAT_TCP6_LISTEN,   /* TCP6 listening                    */
    NET_CAT_UDP6,          /* UDP6                              */
    NET_CAT_OTHER,         /* anything else                     */
    NET_CAT_COUNT
};

static const char *cat_labels[NET_CAT_COUNT] = {
    [NET_CAT_TCP_CONN]    = "TCP Connected",
    [NET_CAT_TCP_LISTEN]  = "TCP Listening",
    [NET_CAT_UDP]         = "UDP",
    [NET_CAT_TCP6_CONN]   = "TCP6 Connected",
    [NET_CAT_TCP6_LISTEN] = "TCP6 Listening",
    [NET_CAT_UDP6]        = "UDP6",
    [NET_CAT_OTHER]       = "Other",
};

static const char *cat_icons[NET_CAT_COUNT] = {
    [NET_CAT_TCP_CONN]    = "\xe2\x87\x84",  /* ⇄ */
    [NET_CAT_TCP_LISTEN]  = "\xe2\x97\x89",  /* ◉ */
    [NET_CAT_UDP]         = "\xe2\x97\x87",  /* ◇ */
    [NET_CAT_TCP6_CONN]   = "\xe2\x87\x84",  /* ⇄ */
    [NET_CAT_TCP6_LISTEN] = "\xe2\x97\x89",  /* ◉ */
    [NET_CAT_UDP6]        = "\xe2\x97\x87",  /* ◇ */
    [NET_CAT_OTHER]       = "?",
};

enum {
    COL_TEXT,       /* plain text                               */
    COL_MARKUP,     /* Pango markup for display                 */
    COL_CAT,        /* category (-1 for leaf rows)              */
    COL_SORT_KEY,   /* sort key: total bytes (gint64)           */
    NUM_COLS
};

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    GtkWidget      *main_box;
    GtkWidget      *header_label;  /* aggregate throughput summary */
    GtkWidget      *chk_desc;     /* "Include Descendants" checkbox */
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    GtkCssProvider *css;
    unsigned        collapsed;     /* bitmask: 1 << cat */
    pid_t           last_pid;
    gboolean        include_desc; /* current toggle state           */
} net_ctx_t;

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

static void format_rate(uint64_t bytes, char *buf, size_t bufsz)
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
        format_rate(s->send_delta, send_buf, sizeof(send_buf));
        format_rate(s->recv_delta, recv_buf, sizeof(recv_buf));

        if (proto_esc && addr_esc) {
            markup = g_strdup_printf(
                "<span foreground=\"#888888\">%s</span> %s  "
                "<span foreground=\"#88cc88\">\xe2\x86\x91 %s</span>  "
                "<span foreground=\"#6699cc\">\xe2\x86\x93 %s</span>",
                proto_esc, addr_esc, send_buf, recv_buf);
        } else {
            markup = g_strdup_printf(
                "%s  "
                "<span foreground=\"#88cc88\">\xe2\x86\x91 %s</span>  "
                "<span foreground=\"#6699cc\">\xe2\x86\x93 %s</span>",
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
        format_rate(cat_send, sbuf, sizeof(sbuf));
        format_rate(cat_recv, rbuf, sizeof(rbuf));
        return g_strdup_printf(
            "<b>%s %s</b> <small>(%zu)</small>  "
            "<span foreground=\"#88cc88\"><small>\xe2\x86\x91 %s</small></span>  "
            "<span foreground=\"#6699cc\"><small>\xe2\x86\x93 %s</small></span>",
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

    gtk_box_pack_start(GTK_BOX(ctx->main_box), hbox, FALSE, FALSE, 0);

    /* ── Connection tree ──────────────────────────────────────── */
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

    gtk_box_pack_start(GTK_BOX(ctx->main_box), ctx->scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(ctx->main_box);

    return ctx->main_box;
}

static void net_update(void *opaque, const evemon_proc_data_t *data)
{
    net_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    /* Read checkbox state */
    ctx->include_desc = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->chk_desc));

    size_t total = data->socket_count;

    if (total == 0) {
        gtk_tree_store_clear(ctx->store);
        gtk_label_set_markup(GTK_LABEL(ctx->header_label),
            "<small>No network sockets</small>");
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
        if (data->sockets[i].total > 0) active_count++;
        visible++;
    }

    if (visible == 0) {
        gtk_tree_store_clear(ctx->store);
        gtk_label_set_markup(GTK_LABEL(ctx->header_label),
            "<small>No network sockets</small>");
        g_free(ents);
        return;
    }

    /* ── Update aggregate header ──────────────────────────────── */
    {
        char *hdr;
        if (agg_send > 0 || agg_recv > 0) {
            char sbuf[32], rbuf[32];
            format_rate(agg_send, sbuf, sizeof(sbuf));
            format_rate(agg_recv, rbuf, sizeof(rbuf));
            hdr = g_strdup_printf(
                "<small><b>%zu</b> connection%s"
                "  \xc2\xb7  <b>%zu</b> active"
                "  \xc2\xb7  <span foreground=\"#88cc88\">\xe2\x86\x91 %s</span>"
                "  <span foreground=\"#6699cc\">\xe2\x86\x93 %s</span></small>",
                visible, visible == 1 ? "" : "s",
                active_count, sbuf, rbuf);
        } else {
            hdr = g_strdup_printf(
                "<small><b>%zu</b> connection%s</small>",
                visible, visible == 1 ? "" : "s");
        }
        gtk_label_set_markup(GTK_LABEL(ctx->header_label), hdr);
        g_free(hdr);
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
    ctx->last_pid = 0;
}

static void net_destroy(void *opaque)
{
    net_ctx_t *ctx = opaque;
    if (ctx->css) g_object_unref(ctx->css);
    free(ctx);
}

/* ── descriptor ──────────────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    net_ctx_t *ctx = calloc(1, sizeof(net_ctx_t));
    if (!ctx) return NULL;
    ctx->include_desc = TRUE;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Network Sockets",
        .id            = "org.evemon.net",
        .version       = "2.0",
        .data_needs    = evemon_NEED_SOCKETS | evemon_NEED_DESCENDANTS,
        .plugin_ctx    = ctx,
        .create_widget = net_create_widget,
        .update        = net_update,
        .clear         = net_clear,
        .destroy       = net_destroy,
    };

    return p;
}
