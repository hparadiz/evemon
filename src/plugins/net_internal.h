#ifndef EVEMON_NET_INTERNAL_H
#define EVEMON_NET_INTERNAL_H
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

#include "evemon_plugin.h"
#include "../core/settings.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

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

gboolean on_chart_motion(GtkWidget *widget, GdkEventMotion *ev,
                                gpointer data);
gboolean on_chart_leave(GtkWidget *widget, GdkEventCrossing *ev,
                               gpointer data);
gboolean on_chart_draw(GtkWidget *widget, cairo_t *cr, gpointer data);
void format_rate(uint64_t bytes, char *buf, size_t bufsz);

#endif
