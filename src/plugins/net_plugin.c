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
#include "../settings.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.net",
    "Network",
    "1.0",
    EVEMON_ROLE_PROCESS,
    NULL
);

/* ── throughput history chart ────────────────────────────────── */

#define NET_HISTORY_LEN   120   /* samples kept (≈2 min at 1 s tick)  */
#define NET_CHART_WIDTH   160   /* fixed pixel width of side panel     */

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


/*
 * Sample a colour from the active spectrogram theme (mirrors spectrogram.c).
 * `mag` in [0,1].  Writes r,g,b to *r_out, *g_out, *b_out.
 */
static void spectro_sample(float mag, int theme, double *r_out, double *g_out, double *b_out)
{
    float r = 0, g = 0, b = 0;
    if (mag < 0) mag = 0;
    if (mag > 1) mag = 1;
    switch (theme) {
    default:
    case 0: /* Classic: dark-blue → cyan → green → yellow → white */
        if (mag < 0.25f) { float t=mag/0.25f; r=0; g=0; b=0.2f+0.6f*t; }
        else if (mag < 0.50f) { float t=(mag-0.25f)/0.25f; r=0; g=t; b=0.8f*(1-t)+0.2f*t; }
        else if (mag < 0.75f) { float t=(mag-0.50f)/0.25f; r=t; g=1; b=0.2f*(1-t); }
        else { float t=(mag-0.75f)/0.25f; r=1; g=1; b=t; }
        break;
    case 1: /* Heat: black → dark-red → red → orange → yellow → white */
        if (mag < 0.25f) { float t=mag/0.25f; r=0.5f*t; g=0; b=0; }
        else if (mag < 0.50f) { float t=(mag-0.25f)/0.25f; r=0.5f+0.5f*t; g=0; b=0; }
        else if (mag < 0.75f) { float t=(mag-0.50f)/0.25f; r=1; g=0.5f*t; b=0; }
        else { float t=(mag-0.75f)/0.25f; r=1; g=0.5f+0.5f*t; b=t; }
        break;
    case 2: /* Cool: black → indigo → violet → cyan → white */
        if (mag < 0.33f) { float t=mag/0.33f; r=0.15f*t; g=0; b=0.5f*t; }
        else if (mag < 0.66f) { float t=(mag-0.33f)/0.33f; r=0.15f+0.15f*t; g=0.5f*t; b=0.5f+0.5f*t; }
        else { float t=(mag-0.66f)/0.34f; r=0.30f+0.70f*t; g=0.5f+0.5f*t; b=1; }
        break;
    case 3: /* Greyscale */
        r = g = b = mag;
        break;
    case 4: /* Neon: black → magenta → hot-pink → cyan → white */
        if (mag < 0.30f) { float t=mag/0.30f; r=0.6f*t; g=0; b=0.6f*t; }
        else if (mag < 0.55f) { float t=(mag-0.30f)/0.25f; r=0.6f+0.4f*t; g=0; b=0.6f-0.3f*t; }
        else if (mag < 0.80f) { float t=(mag-0.55f)/0.25f; r=1-0.8f*t; g=0.7f*t; b=0.3f+0.7f*t; }
        else { float t=(mag-0.80f)/0.20f; r=0.2f+0.8f*t; g=0.7f+0.3f*t; b=1; }
        break;
    case 5: /* Viridis/Vaporwave: navy → purple → pink → cyan → white */
        if (mag < 0.25f) { float t=mag/0.25f; r=0.04f+0.10f*t; g=0.02f+0.03f*t; b=0.18f+0.32f*t; }
        else if (mag < 0.50f) { float t=(mag-0.25f)/0.25f; r=0.14f+0.56f*t; g=0.05f; b=0.50f+0.10f*t; }
        else if (mag < 0.75f) { float t=(mag-0.50f)/0.25f; r=0.70f+0.10f*t; g=0.05f+0.65f*t; b=0.60f+0.30f*t; }
        else { float t=(mag-0.75f)/0.25f; r=0.80f+0.20f*t; g=0.70f+0.30f*t; b=0.90f+0.10f*t; }
        break;
    }

    /* Guarantee legibility on the dark (0.06,0.06,0.09) background.
     * Boost any component that would make the colour too dark/muddy:
     * if perceived luminance < 0.35, lift all channels proportionally. */
    float lum = 0.299f*r + 0.587f*g + 0.114f*b;
    if (lum < 0.35f && lum > 0.0f) {
        float scale = 0.35f / lum;
        r *= scale; g *= scale; b *= scale;
        if (r > 1) r = 1;
        if (g > 1) g = 1;
        if (b > 1) b = 1;
    } else if (lum <= 0.0f) {
        /* pure black — replace with a mid-grey so it's at least visible */
        r = g = b = 0.55f;
    }

    *r_out = r; *g_out = g; *b_out = b;
}

/*
 * Compute colours for all NET_CAT_COUNT categories from the active theme.
 * Samples 7 points spread across the *bright* portion of each theme
 * (avoiding the near-black low end) so every colour is legible on the
 * dark chart background.  recv/send are also theme-derived but always
 * look distinct from each other and from the category lines.
 */
static void net_theme_colours(
    double cat_r[NET_CAT_COUNT], double cat_g[NET_CAT_COUNT], double cat_b[NET_CAT_COUNT],
    double *recv_r, double *recv_g, double *recv_b,
    double *send_r, double *send_g, double *send_b)
{
    evemon_settings_t *s = evemon_settings_get();
    int theme = s ? s->spectro_theme : 0;

    /*
     * Per-theme sample points chosen to land in the colourful bright
     * band of each gradient, skipping the near-black low end:
     *
     *  Classic  (0→1: black-blue → cyan → green → yellow → white)
     *    → use 0.30 … 0.95  (cyan through white)
     *  Heat     (0→1: black → dark-red → orange → yellow → white)
     *    → use 0.35 … 0.95  (red through white)
     *  Cool     (0→1: black → indigo → violet → cyan → white)
     *    → use 0.30 … 0.95
     *  Greyscale → spread 0.40 … 0.95 (mid-grey through white)
     *  Neon     (0→1: black → magenta → hot-pink → cyan → white)
     *    → use 0.28 … 0.95
     *  Viridis  (0→1: navy → purple → pink → cyan → white)
     *    → use 0.35 … 0.95
     */
    static const float lo[6] = { 0.30f, 0.35f, 0.30f, 0.40f, 0.28f, 0.35f };
    static const float hi[6] = { 0.95f, 0.95f, 0.95f, 0.95f, 0.95f, 0.95f };
    int t = (theme >= 0 && theme <= 5) ? theme : 0;
    float span = hi[t] - lo[t];

    for (int c = 0; c < NET_CAT_COUNT; c++) {
        float mag = lo[t] + span * (float)c / (float)(NET_CAT_COUNT - 1);
        spectro_sample(mag, theme, &cat_r[c], &cat_g[c], &cat_b[c]);
    }

    /* recv: sample near the low end of the bright band (cooler hue)
     * send: sample near the high end (warmer / brighter hue)
     * Both go through spectro_sample's luminance floor so they're legible. */
    spectro_sample(lo[t] + span * 0.10f, theme, recv_r, recv_g, recv_b);
    spectro_sample(lo[t] + span * 0.85f, theme, send_r, send_g, send_b);
}

enum {
    COL_TEXT,       /* plain text                               */
    COL_MARKUP,     /* Pango markup for display                 */
    COL_CAT,        /* category (-1 for leaf rows)              */
    COL_SORT_KEY,   /* sort key: total bytes (gint64)           */
    NUM_COLS
};

/* ── resolution options ─────────────────────────────────────── */
/* Each entry: label shown in dropdown, ticks-per-sample (1 tick ≈ 1 s),
 * and total window duration string shown on chart.              */
typedef struct {
    const char *label;   /* dropdown text          */
    int         ticks;   /* update() calls per sample */
    const char *window;  /* e.g. "2 min" for x-axis  */
} net_res_t;

static const net_res_t net_resolutions[] = {
    { "1 sec",   1,   "2 min" },
    { "5 sec",   5,   "10 min" },
    { "30 sec",  30,  "60 min" },
    { "5 min",   300, "10 hr"  },
};
#define NET_RES_COUNT ((int)(sizeof(net_resolutions)/sizeof(net_resolutions[0])))
#define NET_RES_DEFAULT 0   /* 5 sec */

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    GtkWidget      *main_box;
    GtkWidget      *header_label;  /* aggregate throughput summary */
    GtkWidget      *chk_desc;     /* "Include Descendants" checkbox */
    GtkWidget      *combo_res;    /* resolution dropdown           */
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    GtkCssProvider *css;
    unsigned        collapsed;     /* bitmask: 1 << cat */
    pid_t           last_pid;
    gboolean        include_desc; /* current toggle state           */

    /* ── throughput history chart (right-side panel) ──────────── */
    GtkWidget      *chart;        /* GtkDrawingArea, fixed-width    */
    uint64_t        send_hist[NET_HISTORY_LEN];
    uint64_t        recv_hist[NET_HISTORY_LEN];
    /* per-category socket-count history (secondary Y axis) */
    uint32_t        cat_count_hist[NET_CAT_COUNT][NET_HISTORY_LEN];
    int             hist_head;    /* next write index               */
    int             hist_count;   /* how many samples filled so far */
    gboolean        has_ebpf;     /* TRUE once any non-zero rate seen */
    /* active connection count history (right axis) */
    uint32_t        active_hist[NET_HISTORY_LEN];
    uint32_t        acc_active;   /* max active conns seen this bucket */

    /* ── mouse tracking for chart tooltip ───────────────────── */
    int             tooltip_x;    /* last mouse x in widget coords, -1 = none */
    int             tooltip_y;    /* last mouse y in widget coords             */

    /* ── cached chart surface (invalidated on new data) ───────── */
    cairo_surface_t *chart_cache; /* NULL = needs redraw                      */
    int              cache_w;     /* width the cache was rendered at           */
    int              cache_h;     /* height the cache was rendered at          */

    /* ── downsampling accumulator ─────────────────────────────── */
    int             res_idx;      /* index into net_resolutions[]   */
    int             tick_accum;   /* ticks accumulated this sample  */
    uint64_t        acc_send;     /* accumulated send bytes         */
    uint64_t        acc_recv;     /* accumulated recv bytes         */
    uint32_t        acc_cat[NET_CAT_COUNT]; /* max cat count seen    */
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

/* ── chart drawing ───────────────────────────────────────────── */

/*
 * Draw a single line for one data series onto a shared chart area.
 *
 * Values are normalized against `peak` and plotted bottom-to-top
 * within [chart_y .. chart_y+chart_h].  The line is drawn with
 * anti-aliasing and a 1.5 px stroke in the caller-supplied colour.
 */
static void draw_line(cairo_t *cr, int chart_x, int chart_y,
                      int chart_w, int chart_h,
                      const uint64_t *hist,
                      int hist_head, int n,
                      uint64_t peak,
                      double r, double g, double b)
{
    if (chart_h <= 0 || n == 0 || peak == 0) return;

    double step  = (double)chart_w / (double)NET_HISTORY_LEN;
    double x_off = chart_x + (double)(NET_HISTORY_LEN - n) * step;
    double bot   = (double)(chart_y + chart_h);

    cairo_save(cr);
    cairo_rectangle(cr, chart_x, chart_y, chart_w, chart_h);
    cairo_clip(cr);

    cairo_new_path(cr);
    for (int i = 0; i < n; i++) {
        int    idx  = (hist_head - n + i + NET_HISTORY_LEN) % NET_HISTORY_LEN;
        double norm = (double)hist[idx] / (double)peak;
        if (norm > 1.0) norm = 1.0;
        double x = x_off + i * step + step * 0.5;
        double y = bot - norm * (double)chart_h;
        if (i == 0)
            cairo_move_to(cr, x, y);
        else
            cairo_line_to(cr, x, y);
    }
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgba(cr, r, g, b, 0.95);
    cairo_stroke(cr);

    cairo_restore(cr);
}

static gboolean on_chart_motion(GtkWidget *widget, GdkEventMotion *ev,
                                gpointer data)
{
    net_ctx_t *ctx = data;
    ctx->tooltip_x = (int)ev->x;
    ctx->tooltip_y = (int)ev->y;
    gtk_widget_queue_draw(widget);
    return FALSE;
}

static gboolean on_chart_leave(GtkWidget *widget, GdkEventCrossing *ev,
                               gpointer data)
{
    (void)ev;
    net_ctx_t *ctx = data;
    ctx->tooltip_x = -1;
    gtk_widget_queue_draw(widget);
    return FALSE;
}

static gboolean on_chart_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    net_ctx_t *ctx = data;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    if (w <= 0 || h <= 0) return FALSE;

    /* ── Cache management ────────────────────────────────────────
     * All static chart content is rendered into chart_cache once
     * per data tick.  Mouse-move redraws only blit the cache and
     * draw the tooltip overlay, keeping motion perfectly smooth.  */
    if (!ctx->chart_cache || ctx->cache_w != w || ctx->cache_h != h) {
        if (ctx->chart_cache) cairo_surface_destroy(ctx->chart_cache);
        ctx->chart_cache = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        ctx->cache_w = w;
        ctx->cache_h = h;
        /* force full render into cache below */
    }

    /* If the surface is freshly created (or was just invalidated by new
     * data) its content is transparent/stale – render into it now.      */

    /* ── Resolve font size from the widget's Pango context ──────
     * Computed outside the cache block so it's available to the
     * draw_border: section (and any other outer code).           */
    double fsize;
    {
        PangoContext  *pc = gtk_widget_get_pango_context(widget);
        const PangoFontDescription *fd =
            pango_context_get_font_description(pc);   /* borrowed */
        if (pango_font_description_get_size_is_absolute(fd)) {
            fsize = (double)pango_font_description_get_size(fd)
                    / (double)PANGO_SCALE;
        } else {
            double dpi = 96.0;
            GdkScreen *scr = gtk_widget_get_screen(widget);
            if (scr) { double d = gdk_screen_get_resolution(scr);
                       if (d > 0) dpi = d; }
            fsize = (double)pango_font_description_get_size(fd)
                    / (double)PANGO_SCALE * dpi / 72.0;
        }
        if (fsize < 7.0)  fsize = 7.0;
        if (fsize > 24.0) fsize = 24.0;
    }

    /* ── Margins (also needed outside the cache block) ───────── */
    int lbl_w_outer, lbl_h_outer;
    {
        /* Quick measurement using a temporary Pango layout on cr */
        PangoLayout *_lo = pango_cairo_create_layout(cr);
        PangoFontDescription *_fd = pango_font_description_new();
        pango_font_description_set_family(_fd, "Monospace");
        pango_font_description_set_absolute_size(_fd, fsize * PANGO_SCALE);
        pango_layout_set_font_description(_lo, _fd);
        pango_font_description_free(_fd);
        pango_layout_set_text(_lo, "100 MiB/s", -1);
        pango_layout_get_pixel_size(_lo, &lbl_w_outer, &lbl_h_outer);
        g_object_unref(_lo);
    }
    int cnt_w_outer;
    {
        PangoLayout *_lo = pango_cairo_create_layout(cr);
        PangoFontDescription *_fd = pango_font_description_new();
        pango_font_description_set_family(_fd, "Monospace");
        pango_font_description_set_absolute_size(_fd, fsize * PANGO_SCALE);
        pango_layout_set_font_description(_lo, _fd);
        pango_font_description_free(_fd);
        pango_layout_set_text(_lo, "999", -1);
        int _h;
        pango_layout_get_pixel_size(_lo, &cnt_w_outer, &_h);
        g_object_unref(_lo);
    }
    int pad_outer = (int)(fsize * 0.55 + 0.5);
    int ML  = lbl_w_outer + pad_outer * 3;
    int MR  = cnt_w_outer + pad_outer * 3;
    int MT  = lbl_h_outer + pad_outer;
    int MB  = lbl_h_outer * 2 + pad_outer * 4;
    int cw  = w - ML - MR;
    int ch  = h - MT - MB;
    if (cw < 8 || ch < 8) return FALSE;

    /* ── Theme colours (needed both inside cache block and tooltip) */
    double cat_r[NET_CAT_COUNT], cat_g[NET_CAT_COUNT], cat_b[NET_CAT_COUNT];
    double recv_r, recv_g, recv_b;
    double send_r, send_g, send_b;
    net_theme_colours(cat_r, cat_g, cat_b,
                      &recv_r, &recv_g, &recv_b,
                      &send_r, &send_g, &send_b);

    {
        cairo_t *ccr = cairo_create(ctx->chart_cache);
        /* ── delegate all static rendering to ccr ── */
#define cr ccr   /* shadow the outer cr so all existing code writes to cache */

    /* ── Aliases for margin vars (computed outside the cache block) */
    int lbl_w = lbl_w_outer;
    int lbl_h = lbl_h_outer;
    int cnt_w = cnt_w_outer;
    int pad   = pad_outer;

    /* ── Helpers using PangoCairo (accurate metrics, DPI-aware) ─ */

    /* Create a PangoLayout sized to fsize for the given family.
     * Caller must g_object_unref() the returned layout.          */
#define MAKE_LAYOUT(family_) ({                                      \
        PangoLayout *_lo = pango_cairo_create_layout(cr);            \
        PangoFontDescription *_fd = pango_font_description_new();    \
        pango_font_description_set_family(_fd, (family_));           \
        pango_font_description_set_absolute_size(                    \
            _fd, fsize * PANGO_SCALE);                               \
        pango_layout_set_font_description(_lo, _fd);                 \
        pango_font_description_free(_fd);                            \
        _lo; })

    /* Measure a string: fills *pw, *ph with pixel dimensions.    */
#define MEASURE(family_, str_, pw_, ph_) do {                        \
        PangoLayout *_lo = MAKE_LAYOUT(family_);                     \
        pango_layout_set_text(_lo, (str_), -1);                      \
        pango_layout_get_pixel_size(_lo, (pw_), (ph_));              \
        g_object_unref(_lo); } while (0)

    /* Draw text; (x_,y_) is the top-left of the bounding box.    */
#define DRAW_TEXT(family_, str_, x_, y_) do {                        \
        PangoLayout *_lo = MAKE_LAYOUT(family_);                     \
        pango_layout_set_text(_lo, (str_), -1);                      \
        cairo_move_to(cr, (x_), (y_));                               \
        pango_cairo_show_layout(cr, _lo);                            \
        g_object_unref(_lo); } while (0)

    /* ── background ──────────────────────────────────────────── */
    cairo_set_source_rgb(cr, 0.06, 0.06, 0.09);
    cairo_paint(cr);

    /* ── "No data" / "Need root" overlay ────────────────────── */
    if (ctx->hist_count == 0 || !ctx->has_ebpf) {
        const char *line1 = ctx->hist_count == 0
            ? "No data yet" : "Restart as root";
        const char *line2 = ctx->hist_count == 0
            ? NULL          : "for eBPF speeds";
        int tw1, th1; MEASURE("Sans", line1, &tw1, &th1);
        double cy = (double)h * 0.5 - (line2 ? (double)th1 * 0.5 : 0.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.30);
        DRAW_TEXT("Sans", line1, (double)ML + ((double)cw - tw1) * 0.5, cy);
        if (line2) {
            int tw2, th2; MEASURE("Sans", line2, &tw2, &th2);
            (void)th2;
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
            DRAW_TEXT("Sans", line2,
                      (double)ML + ((double)cw - tw2) * 0.5,
                      cy + (double)th1 * 1.4);
        }
        goto do_blit;
    }

    {
        int n = ctx->hist_count;

        /* ── Throughput peak (left axis) ─────────────────────── */
        uint64_t raw = 1;
        for (int i = 0; i < n; i++) {
            int idx = (ctx->hist_head - n + i + NET_HISTORY_LEN)
                      % NET_HISTORY_LEN;
            if (ctx->send_hist[idx] > raw) raw = ctx->send_hist[idx];
            if (ctx->recv_hist[idx] > raw) raw = ctx->recv_hist[idx];
        }
        static const uint64_t bw_steps[] = {
            1024, 2048, 5120,
            10240, 20480, 51200,
            102400, 204800, 512000,
            1048576, 2097152, 5242880,
            10485760, 20971520, 52428800,
            104857600, 209715200, 524288000,
            1073741824ULL, 2147483648ULL, 5368709120ULL, 0
        };
        uint64_t peak = bw_steps[0];
        for (int si = 0; bw_steps[si]; si++) {
            peak = bw_steps[si];
            if (peak >= raw) break;
        }

        /* ── Connection-count peak (right axis) ──────────────── */
        uint32_t count_raw = 1;
        for (int c = 0; c < NET_CAT_COUNT; c++)
            for (int i = 0; i < n; i++) {
                int idx = (ctx->hist_head - n + i + NET_HISTORY_LEN)
                          % NET_HISTORY_LEN;
                if (ctx->cat_count_hist[c][idx] > count_raw)
                    count_raw = ctx->cat_count_hist[c][idx];
            }
        for (int i = 0; i < n; i++) {
            int idx = (ctx->hist_head - n + i + NET_HISTORY_LEN) % NET_HISTORY_LEN;
            if (ctx->active_hist[idx] > count_raw) count_raw = ctx->active_hist[idx];
            uint32_t total_sample = 0;
            for (int c = 0; c < NET_CAT_COUNT; c++) total_sample += ctx->cat_count_hist[c][idx];
            if (total_sample > count_raw) count_raw = total_sample;
        }
        static const uint32_t cnt_steps[] = {
            1,2,5,10,20,50,100,200,500,1000,2000,5000,0 };
        uint32_t cpeak = 1;
        for (int si = 0; cnt_steps[si]; si++) {
            cpeak = cnt_steps[si];
            if (cpeak >= count_raw) break;
        }

        /* ── Dashed grid lines at 25 / 50 / 75 % ────────────── */
        {
            double dashes[] = { 2.0, 3.0 };
            cairo_save(cr);
            cairo_set_dash(cr, dashes, 2, 0.0);
            cairo_set_line_width(cr, 0.5);
            cairo_set_source_rgba(cr, 0.32, 0.32, 0.40, 0.38);
            for (int qi = 1; qi < 4; qi++) {
                double y = MT + (1.0 - qi / 4.0) * ch;
                cairo_move_to(cr, (double)ML,        y);
                cairo_line_to(cr, (double)(ML + cw), y);
            }
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0.0);
            cairo_restore(cr);
        }

        /* ── Throughput lines ────────────────────────────────── */
        draw_line(cr, ML, MT, cw, ch,
                  ctx->recv_hist, ctx->hist_head, n, peak,
                  recv_r, recv_g, recv_b);   /* recv */
        draw_line(cr, ML, MT, cw, ch,
                  ctx->send_hist, ctx->hist_head, n, peak,
                  send_r, send_g, send_b);   /* send */

        /* ── Left axis labels ────────────────────────────────── */
        {
            /* Format helper – inline lambda via nested function   */
            auto void fmt_bw(uint64_t v, char *buf, size_t sz);
            void fmt_bw(uint64_t v, char *buf, size_t sz) {
                if      (v >= 1073741824ULL)
                    snprintf(buf, sz, "%.0f GiB/s", (double)v/1073741824.0);
                else if (v >= 1048576ULL)
                    snprintf(buf, sz, "%.0f MiB/s", (double)v/1048576.0);
                else if (v >= 1024ULL)
                    snprintf(buf, sz, "%.0f KiB/s", (double)v/1024.0);
                else
                    snprintf(buf, sz, "%u B/s", (unsigned)v);
            }
            char s_peak[16], s_mid[16];
            fmt_bw(peak,      s_peak, sizeof(s_peak));
            fmt_bw(peak / 2,  s_mid,  sizeof(s_mid));

            int tw, th;

            /* Peak label – top of chart area */
            MEASURE("Monospace", s_peak, &tw, &th);
            cairo_set_source_rgba(cr, 0.70, 0.70, 0.80, 0.75);
            DRAW_TEXT("Monospace", s_peak,
                      (double)(ML - tw - pad), (double)MT);

            /* Mid label – 50 % gridline, vertically centred */
            MEASURE("Monospace", s_mid, &tw, &th);
            cairo_set_source_rgba(cr, 0.70, 0.70, 0.80, 0.52);
            DRAW_TEXT("Monospace", s_mid,
                      (double)(ML - tw - pad),
                      (double)MT + (double)ch * 0.5 - (double)th * 0.5);

            /* Zero label – bottom of chart area */
            MEASURE("Monospace", "0", &tw, &th);
            cairo_set_source_rgba(cr, 0.70, 0.70, 0.80, 0.38);
            DRAW_TEXT("Monospace", "0",
                      (double)(ML - tw - pad),
                      (double)(MT + ch - th));

            /* 25 % label */
            {
                char s_q1[20]; fmt_bw(peak / 4, s_q1, sizeof(s_q1));
                MEASURE("Monospace", s_q1, &tw, &th);
                cairo_set_source_rgba(cr, 0.70, 0.70, 0.80, 0.42);
                DRAW_TEXT("Monospace", s_q1,
                          (double)(ML - tw - pad),
                          (double)MT + (double)ch * 0.75 - (double)th * 0.5);
            }

            /* 75 % label */
            {
                char s_q3[20]; fmt_bw(peak * 3 / 4, s_q3, sizeof(s_q3));
                MEASURE("Monospace", s_q3, &tw, &th);
                cairo_set_source_rgba(cr, 0.70, 0.70, 0.80, 0.42);
                DRAW_TEXT("Monospace", s_q3,
                          (double)(ML - tw - pad),
                          (double)MT + (double)ch * 0.25 - (double)th * 0.5);
            }

            /* Rotated axis title "B/s" – centred on left margin */
            {
                int aw, ah; MEASURE("Sans", "B/s", &aw, &ah);
                cairo_save(cr);
                cairo_translate(cr,
                    (double)(ML - lbl_w - pad * 2 - ah),
                    (double)MT + ((double)ch + (double)aw) * 0.5);
                cairo_rotate(cr, -M_PI_2);
                cairo_set_source_rgba(cr, 0.60, 0.60, 0.72, 0.55);
                DRAW_TEXT("Sans", "B/s", 0.0, 0.0);
                cairo_restore(cr);
            }
        }

        /* ── Category count lines (secondary / right axis) ─────  */
        {
            double cstep  = (double)cw / (double)NET_HISTORY_LEN;
            double cx_off = ML + (double)(NET_HISTORY_LEN - n) * cstep;
            double cbot   = (double)(MT + ch);

            cairo_save(cr);
            cairo_rectangle(cr, ML, MT, cw, ch);
            cairo_clip(cr);

            for (int c = 0; c < NET_CAT_COUNT; c++) {
                gboolean any = FALSE;
                for (int i = 0; i < n && !any; i++) {
                    int idx = (ctx->hist_head - n + i + NET_HISTORY_LEN)
                              % NET_HISTORY_LEN;
                    if (ctx->cat_count_hist[c][idx] > 0) any = TRUE;
                }
                if (!any) continue;

                cairo_new_path(cr);
                for (int i = 0; i < n; i++) {
                    int idx = (ctx->hist_head - n + i + NET_HISTORY_LEN)
                              % NET_HISTORY_LEN;
                    double norm = (double)ctx->cat_count_hist[c][idx]
                                  / (double)cpeak;
                    if (norm > 1.0) norm = 1.0;
                    double x = cx_off + i * cstep + cstep * 0.5;
                    double y = cbot - norm * (double)ch;
                    if (i == 0) cairo_move_to(cr, x, y);
                    else        cairo_line_to(cr, x, y);
                }
                double dash[] = { 4.0, 3.0 };
                cairo_set_line_width(cr, 1.0);
                cairo_set_dash(cr, dash, 2, (double)c * 1.5);
                cairo_set_source_rgba(cr,
                    cat_r[c], cat_g[c], cat_b[c], 0.80);
                cairo_stroke(cr);
                cairo_set_dash(cr, NULL, 0, 0.0);
            }
            cairo_restore(cr);
        }

        /* ── Total + Active connections lines (solid, right axis) */
        {
            double cstep  = (double)cw / (double)NET_HISTORY_LEN;
            double cx_off = ML + (double)(NET_HISTORY_LEN - n) * cstep;
            double cbot   = (double)(MT + ch);

            cairo_save(cr);
            cairo_rectangle(cr, ML, MT, cw, ch);
            cairo_clip(cr);

            /* total = sum of all category counts */
            cairo_new_path(cr);
            for (int i = 0; i < n; i++) {
                int idx = (ctx->hist_head - n + i + NET_HISTORY_LEN)
                          % NET_HISTORY_LEN;
                uint32_t tot = 0;
                for (int c = 0; c < NET_CAT_COUNT; c++) tot += ctx->cat_count_hist[c][idx];
                double norm = (double)tot / (double)cpeak;
                if (norm > 1.0) norm = 1.0;
                double x = cx_off + i * cstep + cstep * 0.5;
                double y = cbot - norm * (double)ch;
                if (i == 0) cairo_move_to(cr, x, y);
                else        cairo_line_to(cr, x, y);
            }
            cairo_set_line_width(cr, 2.0);
            cairo_set_source_rgba(cr, 0.80, 0.80, 0.95, 0.85);
            cairo_stroke(cr);

            /* active = sockets with traffic this sample */
            cairo_new_path(cr);
            for (int i = 0; i < n; i++) {
                int idx = (ctx->hist_head - n + i + NET_HISTORY_LEN)
                          % NET_HISTORY_LEN;
                double norm = (double)ctx->active_hist[idx] / (double)cpeak;
                if (norm > 1.0) norm = 1.0;
                double x = cx_off + i * cstep + cstep * 0.5;
                double y = cbot - norm * (double)ch;
                if (i == 0) cairo_move_to(cr, x, y);
                else        cairo_line_to(cr, x, y);
            }
            cairo_set_line_width(cr, 2.0);
            cairo_set_source_rgba(cr, 0.40, 1.00, 0.55, 0.90);
            cairo_stroke(cr);

            cairo_restore(cr);
        }

        /* ── Right axis labels ───────────────────────────────── */
        {
            int rx   = ML + cw + pad;
            char s_cpeak[12], s_cmid[12];
            snprintf(s_cpeak, sizeof(s_cpeak), "%u", cpeak);
            snprintf(s_cmid,  sizeof(s_cmid),  "%u", cpeak / 2);

            int tw, th;

            /* Peak – top */
            MEASURE("Monospace", s_cpeak, &tw, &th);
            cairo_set_source_rgba(cr, 0.78, 0.78, 0.62, 0.75);
            DRAW_TEXT("Monospace", s_cpeak, (double)rx, (double)MT);

            /* Mid – 50 % */
            MEASURE("Monospace", s_cmid, &tw, &th);
            cairo_set_source_rgba(cr, 0.78, 0.78, 0.62, 0.52);
            DRAW_TEXT("Monospace", s_cmid,
                      (double)rx,
                      (double)MT + (double)ch * 0.5 - (double)th * 0.5);

            /* Zero – bottom */
            MEASURE("Monospace", "0", &tw, &th);
            cairo_set_source_rgba(cr, 0.78, 0.78, 0.62, 0.38);
            DRAW_TEXT("Monospace", "0",
                      (double)rx, (double)(MT + ch - th));

            /* 25 % count label */
            {
                char s_cq1[12]; snprintf(s_cq1, sizeof(s_cq1), "%u", cpeak / 4);
                MEASURE("Monospace", s_cq1, &tw, &th);
                cairo_set_source_rgba(cr, 0.78, 0.78, 0.62, 0.42);
                DRAW_TEXT("Monospace", s_cq1,
                          (double)rx,
                          (double)MT + (double)ch * 0.75 - (double)th * 0.5);
            }

            /* 75 % count label */
            {
                char s_cq3[12]; snprintf(s_cq3, sizeof(s_cq3), "%u", cpeak * 3 / 4);
                MEASURE("Monospace", s_cq3, &tw, &th);
                cairo_set_source_rgba(cr, 0.78, 0.78, 0.62, 0.42);
                DRAW_TEXT("Monospace", s_cq3,
                          (double)rx,
                          (double)MT + (double)ch * 0.25 - (double)th * 0.5);
            }

            /* Rotated axis title "conns" – centred on right margin */
            {
                int aw, ah; MEASURE("Sans", "conns", &aw, &ah);
                cairo_save(cr);
                cairo_translate(cr,
                    (double)(ML + cw + pad + cnt_w + pad * 2 + ah),
                    (double)MT + ((double)ch - (double)aw) * 0.5);
                cairo_rotate(cr, M_PI_2);
                cairo_set_source_rgba(cr, 0.65, 0.65, 0.50, 0.55);
                DRAW_TEXT("Sans", "conns", 0.0, 0.0);
                cairo_restore(cr);
            }

            /* Tick marks at top / mid / bottom of right border */
            cairo_set_line_width(cr, 0.5);
            cairo_set_source_rgba(cr, 0.40, 0.40, 0.32, 0.55);
            int tx = ML + cw;
            for (int qi = 0; qi <= 2; qi++) {
                double ty = (double)MT + (double)ch * (qi / 2.0);
                cairo_move_to(cr, (double)tx,       ty);
                cairo_line_to(cr, (double)(tx + 3),  ty);
            }
            cairo_stroke(cr);
        }
    }

    /* ── Legend strip (bottom) ──────────────────────────────────
     * Row 0 (always): recv dot+label (left), send dot+label (left)
     * Row 1: active category swatches + full names, right-justified.
     * If all categories fit on the same row as recv/send we use one
     * row; otherwise we use two rows and draw cats on row 1.       */
    {
        double dot_r  = fsize * 0.28;
        double swatch = fsize * 1.2;
        double sep    = (double)pad * 0.5;
        int tw, th;

        /* Measure recv/send label widths */
        int recv_tw, send_tw, dummy_th;
        MEASURE("Sans", "recv", &recv_tw, &dummy_th);
        MEASURE("Sans", "send", &send_tw, &dummy_th);
        double recv_send_w = dot_r*2 + sep + (double)recv_tw + (double)pad*1.5
                           + dot_r*2 + sep + (double)send_tw + (double)pad*1.5;

        /* Collect active categories and measure their total width */
        int    active[NET_CAT_COUNT];
        int    nactive  = 0;
        double cat_total_w = 0.0;
        if (ctx->hist_count > 0) {
            int n = ctx->hist_count;
            for (int c = 0; c < NET_CAT_COUNT; c++) {
                gboolean any = FALSE;
                for (int i = 0; i < n && !any; i++) {
                    int idx = (ctx->hist_head - n + i + NET_HISTORY_LEN)
                              % NET_HISTORY_LEN;
                    if (ctx->cat_count_hist[c][idx] > 0) any = TRUE;
                }
                if (!any) continue;
                MEASURE("Sans", cat_labels[c], &tw, &th);
                cat_total_w += swatch + sep + (double)tw + (double)pad * 1.5;
                active[nactive++] = c;
            }
        }

        /* Does everything fit on one row?  Gap between recv/send and cats
         * must be at least 2*pad wide.  The chart width is cw pixels.    */
        int two_rows = (recv_send_w + cat_total_w + (double)pad*2 > (double)cw);

        /* Recompute MB now that we know row count, and re-derive ch/cw.  */
        int new_MB = two_rows ? (lbl_h * 2 + pad * 4) : (lbl_h + pad * 2);
        /* Adjust ch to reflect the actual MB (we already painted the chart
         * content above using the old ch — just need correct y for labels). */
        int leg_ch  = h - MT - new_MB;   /* chart height with new MB */
        (void)leg_ch;                     /* chart was drawn with old ch; labels below */

        /* Row 0 baseline: centred in first legend row */
        double row0_y = (double)(MT + ch) + (double)pad;
        double row1_y = row0_y + (double)lbl_h + (double)pad;
        /* When single-row, cats share row0_y */
        double cat_y  = two_rows ? row1_y : row0_y;

        /* ── recv dot + label (left) ── */
        cairo_set_source_rgba(cr, recv_r, recv_g, recv_b, 0.90);
        cairo_arc(cr, (double)ML + dot_r,
                  row0_y + (double)lbl_h * 0.5,
                  dot_r, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, recv_r, recv_g, recv_b, 0.90);
        DRAW_TEXT("Sans", "recv", (double)ML + dot_r*2 + sep, row0_y);

        double after_recv = (double)ML + dot_r*2 + sep + (double)recv_tw + (double)pad*1.5;

        /* ── send dot + label ── */
        cairo_set_source_rgba(cr, send_r, send_g, send_b, 0.90);
        cairo_arc(cr, after_recv + dot_r,
                  row0_y + (double)lbl_h * 0.5,
                  dot_r, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, send_r, send_g, send_b, 0.90);
        DRAW_TEXT("Sans", "send", after_recv + dot_r*2 + sep, row0_y);

        double after_send = after_recv + dot_r*2 + sep + (double)send_tw + (double)pad*1.5;

        /* ── total solid line swatch + label ── */
        {
            int total_tw, total_th;
            MEASURE("Sans", "total", &total_tw, &total_th); (void)total_th;
            cairo_save(cr);
            cairo_set_line_width(cr, 2.0);
            cairo_set_source_rgba(cr, 0.80, 0.80, 0.95, 0.85);
            cairo_move_to(cr, after_send,          row0_y + (double)lbl_h * 0.5);
            cairo_line_to(cr, after_send + swatch, row0_y + (double)lbl_h * 0.5);
            cairo_stroke(cr);
            cairo_restore(cr);
            cairo_set_source_rgba(cr, 0.80, 0.80, 0.95, 0.85);
            DRAW_TEXT("Sans", "total", after_send + swatch + sep, row0_y);
            after_send += swatch + sep + (double)total_tw + (double)pad*1.5;
        }

        /* ── active solid line swatch + label ── */
        {
            cairo_save(cr);
            cairo_set_line_width(cr, 2.0);
            cairo_set_source_rgba(cr, 0.40, 1.00, 0.55, 0.90);
            cairo_move_to(cr, after_send,          row0_y + (double)lbl_h * 0.5);
            cairo_line_to(cr, after_send + swatch, row0_y + (double)lbl_h * 0.5);
            cairo_stroke(cr);
            cairo_restore(cr);
            int active_tw, active_th;
            MEASURE("Sans", "active", &active_tw, &active_th); (void)active_th;
            cairo_set_source_rgba(cr, 0.40, 1.00, 0.55, 0.90);
            DRAW_TEXT("Sans", "active", after_send + swatch + sep, row0_y);
        }
        if (nactive > 0) {
            /* Start x so block ends flush with right chart edge */
            double rx = (double)(ML + cw) - cat_total_w;
            double sy = cat_y + (double)lbl_h * 0.5;

            for (int ai = 0; ai < nactive; ai++) {
                int c = active[ai];
                MEASURE("Sans", cat_labels[c], &tw, &th);

                /* dashed colour swatch */
                double dash[] = { 3.0, 2.0 };
                cairo_save(cr);
                cairo_set_dash(cr, dash, 2, 0.0);
                cairo_set_line_width(cr, 1.2);
                cairo_set_source_rgba(cr, cat_r[c], cat_g[c], cat_b[c], 0.85);
                cairo_move_to(cr, rx,          sy);
                cairo_line_to(cr, rx + swatch, sy);
                cairo_stroke(cr);
                cairo_restore(cr);
                rx += swatch + sep;

                /* full name */
                cairo_set_source_rgba(cr, cat_r[c], cat_g[c], cat_b[c], 0.85);
                DRAW_TEXT("Sans", cat_labels[c], rx, cat_y);
                rx += (double)tw + (double)pad * 1.5;
            }
        }
    }

#undef MAKE_LAYOUT
#undef MEASURE
#undef DRAW_TEXT
do_blit: ;
#undef cr  /* restore outer cr */
        cairo_destroy(ccr);
    } /* end cache render block */

    /* ── Blit cache onto widget surface ─────────────────────── */
    cairo_set_source_surface(cr, ctx->chart_cache, 0, 0);
    cairo_paint(cr);

    /* ── Mouse-tracking tooltip (drawn directly on cr) ─────────── */
    if (ctx->tooltip_x >= ML && ctx->tooltip_x < ML + cw &&
        ctx->hist_count > 0 && ctx->has_ebpf) {

        int n = ctx->hist_count;
        double step = (double)cw / (double)NET_HISTORY_LEN;
        int slot = (int)((ctx->tooltip_x - ML) / step);
        int first_slot = NET_HISTORY_LEN - n;
        if (slot < first_slot) slot = first_slot;
        if (slot >= NET_HISTORY_LEN) slot = NET_HISTORY_LEN - 1;
        int age = slot - first_slot;
        int idx = (ctx->hist_head - n + age + NET_HISTORY_LEN) % NET_HISTORY_LEN;

        /* vertical hairline */
        double hx = (double)ML + (slot + 0.5) * step;
        cairo_save(cr);
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25);
        cairo_move_to(cr, hx, (double)MT);
        cairo_line_to(cr, hx, (double)(MT + ch));
        cairo_stroke(cr);
        cairo_restore(cr);

        /* format tooltip text */
        char tt[8][64];
        int nlines = 0;
        int secs_ago = (n - 1 - age) * net_resolutions[ctx->res_idx].ticks;
        if (secs_ago == 0)
            snprintf(tt[nlines++], 64, "now");
        else
            snprintf(tt[nlines++], 64, "-%d s", secs_ago);
        char sbuf[32], rbuf[32];
        format_rate(ctx->recv_hist[idx], sbuf, sizeof(sbuf));
        format_rate(ctx->send_hist[idx], rbuf, sizeof(rbuf));
        snprintf(tt[nlines++], 64, "recv  %s", sbuf);
        snprintf(tt[nlines++], 64, "send  %s", rbuf);
        uint32_t tot = 0;
        for (int c = 0; c < NET_CAT_COUNT; c++) tot += ctx->cat_count_hist[c][idx];
        snprintf(tt[nlines++], 64, "total  %u", tot);
        snprintf(tt[nlines++], 64, "active %u", ctx->active_hist[idx]);

        /* measure box */
        cairo_save(cr);
        cairo_select_font_face(cr, "Monospace",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fsize * 0.90);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        double line_h = fe.height;
        double box_w  = 0;
        for (int li = 0; li < nlines; li++) {
            cairo_text_extents_t te;
            cairo_text_extents(cr, tt[li], &te);
            if (te.width > box_w) box_w = te.width;
        }
        double bpad  = fsize * 0.5;
        double box_h = nlines * line_h;
        double box_x = hx + 6;
        double box_y = (double)MT + (double)ch * 0.08;
        if (box_x + box_w + bpad * 2 > (double)(ML + cw))
            box_x = hx - 6 - box_w - bpad * 2;

        /* background + border */
        cairo_set_source_rgba(cr, 0.08, 0.08, 0.13, 0.88);
        cairo_rectangle(cr, box_x, box_y, box_w + bpad*2, box_h + bpad*2);
        cairo_fill(cr);
        cairo_set_line_width(cr, 0.7);
        cairo_set_source_rgba(cr, 0.45, 0.45, 0.60, 0.70);
        cairo_rectangle(cr, box_x, box_y, box_w + bpad*2, box_h + bpad*2);
        cairo_stroke(cr);

        /* text lines, colour-coded to match the chart lines */
        for (int li = 0; li < nlines; li++) {
            if      (li == 0) cairo_set_source_rgba(cr, 0.75, 0.75, 0.85, 0.80); /* time */
            else if (li == 1) cairo_set_source_rgba(cr, recv_r, recv_g, recv_b, 0.95); /* recv */
            else if (li == 2) cairo_set_source_rgba(cr, send_r, send_g, send_b, 0.95); /* send */
            else if (li == 3) cairo_set_source_rgba(cr, 0.80, 0.80, 0.95, 0.85); /* total */
            else              cairo_set_source_rgba(cr, 0.40, 1.00, 0.55, 0.90); /* active */
            cairo_move_to(cr,
                box_x + bpad,
                box_y + bpad + fe.ascent + li * line_h);
            cairo_show_text(cr, tt[li]);
        }
        cairo_restore(cr);
    }

    /* ── Chart border + time label drawn on real cr (over tooltip) */
    cairo_set_line_width(cr, 0.5);
    cairo_set_source_rgba(cr, 0.28, 0.28, 0.36, 0.60);
    cairo_rectangle(cr, (double)ML, (double)MT, (double)cw, (double)ch);
    cairo_stroke(cr);
    {
        const char *win_str = net_resolutions[ctx->res_idx].window;
        cairo_select_font_face(cr, "Sans",
                               CAIRO_FONT_SLANT_ITALIC,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fsize * 0.85);
        cairo_text_extents_t te;
        cairo_text_extents(cr, win_str, &te);
        cairo_set_source_rgba(cr, 0.55, 0.55, 0.65, 0.45);
        cairo_move_to(cr, (double)ML + 3, (double)(MT + ch) - 3);
        cairo_show_text(cr, win_str);
    }

    return FALSE;
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
        .role          = EVEMON_ROLE_PROCESS,
        .dependencies  = NULL,
    };

    return p;
}
