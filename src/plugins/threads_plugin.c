/*
 * threads_plugin.c – Threads plugin for evemon.
 *
 * Displays the threads (tasks) of a process.  Shows per-thread
 * TID, name, state, CPU time, priority, nice, last CPU core, and
 * context switch counts.
 *
 * Build:
 *   gcc -shared -fPIC -o evemon_threads.so threads_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── thread state definitions ────────────────────────────────── */

enum {
    THR_CAT_RUNNING,
    THR_CAT_SLEEPING,
    THR_CAT_DISK_SLEEP,
    THR_CAT_STOPPED,
    THR_CAT_ZOMBIE,
    THR_CAT_OTHER,
    THR_CAT_COUNT
};

static const char *cat_labels[THR_CAT_COUNT] = {
    [THR_CAT_RUNNING]    = "Running",
    [THR_CAT_SLEEPING]   = "Sleeping",
    [THR_CAT_DISK_SLEEP] = "Disk Sleep",
    [THR_CAT_STOPPED]    = "Stopped / Traced",
    [THR_CAT_ZOMBIE]     = "Zombie",
    [THR_CAT_OTHER]      = "Other",
};

static const char *cat_colors[THR_CAT_COUNT] = {
    [THR_CAT_RUNNING]    = "#66cc66",   /* green  */
    [THR_CAT_SLEEPING]   = "#888888",   /* grey   */
    [THR_CAT_DISK_SLEEP] = "#cc8844",   /* orange */
    [THR_CAT_STOPPED]    = "#cc6666",   /* red    */
    [THR_CAT_ZOMBIE]     = "#cc4444",   /* dark red */
    [THR_CAT_OTHER]      = "#888888",   /* grey   */
};

enum {
    COL_TEXT,
    COL_MARKUP,
    COL_CAT,
    COL_SORT_KEY,    /* for sorting threads within a category */
    NUM_COLS
};

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    GtkWidget      *scroll;
    GtkTreeStore   *store;
    GtkTreeView    *view;
    unsigned        collapsed;    /* bitmask: 1 << cat */
    pid_t           last_pid;
} thr_ctx_t;

/* ── classification ──────────────────────────────────────────── */

static int classify_thread(char state)
{
    switch (state) {
    case 'R': return THR_CAT_RUNNING;
    case 'S': return THR_CAT_SLEEPING;
    case 'D': return THR_CAT_DISK_SLEEP;
    case 'T': case 't': return THR_CAT_STOPPED;
    case 'Z': case 'X': return THR_CAT_ZOMBIE;
    case 'I': return THR_CAT_SLEEPING;  /* idle kernel thread → sleeping */
    default:  return THR_CAT_OTHER;
    }
}

/* ── state description helper ────────────────────────────────── */

static const char *state_desc(char state)
{
    switch (state) {
    case 'R': return "Running";
    case 'S': return "Sleeping";
    case 'D': return "Disk Sleep";
    case 'T': return "Stopped";
    case 't': return "Traced";
    case 'Z': return "Zombie";
    case 'X': return "Dead";
    case 'I': return "Idle";
    default:  return "?";
    }
}

/* ── format CPU time ─────────────────────────────────────────── */

static void format_cpu_time(unsigned long long ticks, char *buf, size_t bufsz)
{
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck < 1) clk_tck = 100;

    unsigned long long total_secs = ticks / (unsigned long long)clk_tck;
    unsigned long long frac = (ticks % (unsigned long long)clk_tck) * 100
                              / (unsigned long long)clk_tck;

    if (total_secs >= 3600) {
        unsigned long long h = total_secs / 3600;
        unsigned long long m = (total_secs % 3600) / 60;
        unsigned long long s = total_secs % 60;
        snprintf(buf, bufsz, "%lluh %llum %llus", h, m, s);
    } else if (total_secs >= 60) {
        unsigned long long m = total_secs / 60;
        unsigned long long s = total_secs % 60;
        snprintf(buf, bufsz, "%llum %llus", m, s);
    } else {
        snprintf(buf, bufsz, "%llu.%02llus", total_secs, frac);
    }
}

/* ── format context switches ─────────────────────────────────── */

static void format_ctxsw(unsigned long long v, char *buf, size_t bufsz)
{
    if (v >= 1000000000ULL)
        snprintf(buf, bufsz, "%.1fG", (double)v / 1e9);
    else if (v >= 1000000ULL)
        snprintf(buf, bufsz, "%.1fM", (double)v / 1e6);
    else if (v >= 1000ULL)
        snprintf(buf, bufsz, "%.1fK", (double)v / 1e3);
    else
        snprintf(buf, bufsz, "%llu", v);
}

/* ── markup helper ───────────────────────────────────────────── */

static char *thread_to_markup(const evemon_thread_t *t)
{
    GString *s = g_string_new(NULL);

    /* TID — blue */
    g_string_append_printf(s,
        "<span foreground=\"#6699cc\">%d</span>  ", t->tid);

    /* Name — bold */
    char *name_esc = g_markup_escape_text(
        t->name[0] ? t->name : "(unnamed)", -1);
    g_string_append_printf(s, "<b>%s</b>  ", name_esc);
    g_free(name_esc);

    /* State — coloured */
    int cat = classify_thread(t->state);
    g_string_append_printf(s,
        "<span foreground=\"%s\">%s</span>  ",
        cat_colors[cat], state_desc(t->state));

    /* CPU time (user+system) */
    char cpu_buf[64];
    format_cpu_time(t->utime + t->stime, cpu_buf, sizeof(cpu_buf));
    g_string_append_printf(s,
        "<span foreground=\"#aaaaaa\">CPU: %s</span>  ", cpu_buf);

    /* Priority / nice */
    g_string_append_printf(s,
        "<span foreground=\"#888888\">pri %d  nice %d  cpu#%d</span>",
        t->priority, t->nice, t->processor);

    /* Context switches (if nonzero) */
    if (t->voluntary_ctxt_switches > 0 || t->nonvoluntary_ctxt_switches > 0) {
        char vcsw[32], nvcsw[32];
        format_ctxsw(t->voluntary_ctxt_switches, vcsw, sizeof(vcsw));
        format_ctxsw(t->nonvoluntary_ctxt_switches, nvcsw, sizeof(nvcsw));
        g_string_append_printf(s,
            "  <span foreground=\"#777777\">csw: %s/%s</span>",
            vcsw, nvcsw);
    }

    return g_string_free(s, FALSE);
}

/* ── signal callbacks ────────────────────────────────────────── */

static void on_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                             GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    thr_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < THR_CAT_COUNT)
        ctx->collapsed |= (1u << cat);
}

static void on_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                            GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    thr_ctx_t *ctx = data;
    gint cat = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), iter, COL_CAT, &cat, -1);
    if (cat >= 0 && cat < THR_CAT_COUNT)
        ctx->collapsed &= ~(1u << cat);
}

/* ── tooltip: show full detail on hover ──────────────────────── */

static gboolean on_query_tooltip(GtkWidget *widget, gint x, gint y,
                                 gboolean kb, GtkTooltip *tip, gpointer data)
{
    (void)data;
    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeModel *model;
    GtkTreePath *path;
    GtkTreeIter iter;

    if (!gtk_tree_view_get_tooltip_context(view, &x, &y, kb,
                                            &model, &path, &iter))
        return FALSE;

    gint cat = -1;
    gchar *text = NULL;
    gtk_tree_model_get(model, &iter, COL_CAT, &cat, COL_TEXT, &text, -1);

    if (cat >= 0 || !text || !text[0]) {
        g_free(text);
        gtk_tree_path_free(path);
        return FALSE;
    }

    gtk_tooltip_set_text(tip, text);
    gtk_tree_view_set_tooltip_row(view, tip, path);
    g_free(text);
    gtk_tree_path_free(path);
    return TRUE;
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *thr_create_widget(void *opaque)
{
    thr_ctx_t *ctx = opaque;

    ctx->store = gtk_tree_store_new(NUM_COLS,
                                    G_TYPE_STRING,   /* text (tooltip) */
                                    G_TYPE_STRING,   /* markup         */
                                    G_TYPE_INT,      /* category       */
                                    G_TYPE_INT64);   /* sort key       */

    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    g_object_unref(ctx->store);

    gtk_tree_view_set_headers_visible(ctx->view, FALSE);
    gtk_tree_view_set_enable_search(ctx->view, FALSE);
    gtk_widget_set_has_tooltip(GTK_WIDGET(ctx->view), TRUE);

    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Thread", cell, "markup", COL_MARKUP, NULL);
    gtk_tree_view_append_column(ctx->view, col);

    g_signal_connect(ctx->view, "row-collapsed",
                     G_CALLBACK(on_row_collapsed), ctx);
    g_signal_connect(ctx->view, "row-expanded",
                     G_CALLBACK(on_row_expanded), ctx);
    g_signal_connect(ctx->view, "query-tooltip",
                     G_CALLBACK(on_query_tooltip), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);
    gtk_widget_show_all(ctx->scroll);

    return ctx->scroll;
}

static void thr_update(void *opaque, const evemon_proc_data_t *data)
{
    thr_ctx_t *ctx = opaque;

    if (data->pid != ctx->last_pid) {
        ctx->collapsed = 0;
        ctx->last_pid = data->pid;
    }

    /* Bucket threads by state category */
    size_t cat_count[THR_CAT_COUNT] = {0};
    size_t total = data->thread_count;

    typedef struct {
        const evemon_thread_t *t;
        int cat;
    } thr_ent_t;

    thr_ent_t *ents = g_new0(thr_ent_t, total > 0 ? total : 1);

    for (size_t i = 0; i < total; i++) {
        ents[i].t   = &data->threads[i];
        ents[i].cat = classify_thread(data->threads[i].state);
        cat_count[ents[i].cat]++;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    /* Save scroll position */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    /* Index existing category rows */
    GtkTreeIter cat_iters[THR_CAT_COUNT];
    gboolean    cat_exists[THR_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(model, &top, COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < THR_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    /* Remove empty categories */
    for (int c = 0; c < THR_CAT_COUNT; c++) {
        if (cat_exists[c] && cat_count[c] == 0) {
            gtk_tree_store_remove(ctx->store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
    for (int c = 0; c < THR_CAT_COUNT; c++) {
        if (cat_count[c] == 0) continue;

        /* Category header: "Running (3 threads)" */
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s (%zu thread%s)",
                 cat_labels[c], cat_count[c],
                 cat_count[c] == 1 ? "" : "s");
        char *hdr_esc = g_markup_escape_text(hdr, -1);

        /* Colorise the category header */
        char *hdr_markup = g_strdup_printf(
            "<span foreground=\"%s\"><b>%s</b></span>",
            cat_colors[c], hdr_esc);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            gtk_tree_store_append(ctx->store, &parent, NULL);
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr,
                               COL_MARKUP, hdr_markup,
                               COL_CAT, (gint)c,
                               COL_SORT_KEY, (gint64)0, -1);
            cat_exists[c] = TRUE;
            cat_iters[c] = parent;
        } else {
            parent = cat_iters[c];
            gtk_tree_store_set(ctx->store, &parent,
                               COL_TEXT, hdr,
                               COL_MARKUP, hdr_markup, -1);
        }
        g_free(hdr_esc);
        g_free(hdr_markup);

        /* Update children in place */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);

        for (size_t i = 0; i < total; i++) {
            if (ents[i].cat != c) continue;

            const evemon_thread_t *t = ents[i].t;
            char *markup = thread_to_markup(t);

            /* Build a text tooltip with full detail */
            char tooltip[512];
            char cpu_buf[64];
            format_cpu_time(t->utime + t->stime, cpu_buf, sizeof(cpu_buf));
            snprintf(tooltip, sizeof(tooltip),
                     "TID %d: %s\n"
                     "State: %s  Priority: %d  Nice: %d\n"
                     "CPU Time: %s  (user: %llu  sys: %llu ticks)\n"
                     "Last CPU: #%d\n"
                     "Context Switches: %llu voluntary, %llu involuntary",
                     t->tid, t->name[0] ? t->name : "(unnamed)",
                     state_desc(t->state), t->priority, t->nice,
                     cpu_buf, t->utime, t->stime,
                     t->processor,
                     t->voluntary_ctxt_switches,
                     t->nonvoluntary_ctxt_switches);

            /* Sort key: running threads first (by CPU time desc) */
            gint64 sort_key = (gint64)(t->utime + t->stime);

            if (child_valid) {
                gtk_tree_store_set(ctx->store, &child,
                                   COL_TEXT, tooltip,
                                   COL_MARKUP, markup,
                                   COL_CAT, (gint)-1,
                                   COL_SORT_KEY, sort_key, -1);
                child_valid = gtk_tree_model_iter_next(model, &child);
            } else {
                GtkTreeIter new_child;
                gtk_tree_store_append(ctx->store, &new_child, &parent);
                gtk_tree_store_set(ctx->store, &new_child,
                                   COL_TEXT, tooltip,
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

    gtk_adjustment_set_value(vadj, scroll_pos);
    g_free(ents);
}

static void thr_clear(void *opaque)
{
    thr_ctx_t *ctx = opaque;
    gtk_tree_store_clear(ctx->store);
    ctx->last_pid = 0;
}

static void thr_destroy(void *opaque)
{
    thr_ctx_t *ctx = opaque;
    free(ctx);
}

/* ── plugin descriptor ───────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    thr_ctx_t *ctx = calloc(1, sizeof(thr_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "Threads",
        .id            = "org.evemon.threads",
        .version       = "1.0",
        .data_needs    = evemon_NEED_THREADS,
        .plugin_ctx    = ctx,
        .create_widget = thr_create_widget,
        .update        = thr_update,
        .clear         = thr_clear,
        .destroy       = thr_destroy,
    };

    return p;
}
