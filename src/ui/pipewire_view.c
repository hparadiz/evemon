#ifdef HAVE_PIPEWIRE
#include "ui_internal.h"
#include "pipewire_graph.h"
/* ── per-PID result building ─────────────────────────────────── */

/*
 * Categories for the PipeWire detail panel tree.
 */
typedef enum {
    PW_CAT_OUTPUT,    /* streams sending audio out (playback)   */
    PW_CAT_INPUT,     /* streams capturing audio in (record)    */
    PW_CAT_VIDEO,     /* video streams                          */
    PW_CAT_MIDI,      /* MIDI streams                           */
    PW_CAT_OTHER,     /* anything else                          */
    PW_CAT_COUNT
} pw_scan_category_t;

static const char *pw_cat_label[PW_CAT_COUNT] = {
    [PW_CAT_OUTPUT] = "Audio Output",
    [PW_CAT_INPUT]  = "Audio Input",
    [PW_CAT_VIDEO]  = "Video",
    [PW_CAT_MIDI]   = "MIDI",
    [PW_CAT_OTHER]  = "Other",
};

typedef struct {
    char text[512];
    pw_scan_category_t cat;
    uint32_t node_id;    /* PipeWire node ID (for spectrogram selection) */
} pw_result_entry_t;

typedef struct {
    pw_result_entry_t *entries;
    size_t count;
    size_t capacity;
} pw_result_list_t;

static void pw_result_init(pw_result_list_t *l) { memset(l, 0, sizeof(*l)); }

static void pw_result_free(pw_result_list_t *l)
{
    free(l->entries);
    memset(l, 0, sizeof(*l));
}

static void pw_result_push(pw_result_list_t *l, pw_scan_category_t cat,
                           const char *text, uint32_t node_id)
{
    if (l->count >= l->capacity) {
        size_t nc = l->capacity ? l->capacity * 2 : 32;
        pw_result_entry_t *tmp = realloc(l->entries, nc * sizeof(*tmp));
        if (!tmp) return;
        l->entries  = tmp;
        l->capacity = nc;
    }
    pw_result_entry_t *e = &l->entries[l->count++];
    e->cat = cat;
    e->node_id = node_id;
    snprintf(e->text, sizeof(e->text), "%s", text);
}

static pw_scan_category_t classify_media_class(const char *mc)
{
    if (!mc || !mc[0]) return PW_CAT_OTHER;
    if (strstr(mc, "Audio") && strstr(mc, "Output"))  return PW_CAT_OUTPUT;
    if (strstr(mc, "Audio") && strstr(mc, "Input"))   return PW_CAT_INPUT;
    if (strstr(mc, "Audio") && strstr(mc, "Sink"))    return PW_CAT_OUTPUT;
    if (strstr(mc, "Audio") && strstr(mc, "Source"))  return PW_CAT_INPUT;
    if (strstr(mc, "Video"))                          return PW_CAT_VIDEO;
    if (strstr(mc, "Midi"))                           return PW_CAT_MIDI;
    return PW_CAT_OTHER;
}

/*
 * Get the best human-readable label for a node.
 * For streams: prefer media_name (tab title, song name),
 * then node_desc, then app_name, then node_name.
 */
static const char *node_label(const pw_snap_node_t *n)
{
    if (n->media_name[0])  return n->media_name;
    if (n->node_desc[0])   return n->node_desc;
    if (n->app_name[0])    return n->app_name;
    if (n->node_name[0])   return n->node_name;
    return "(unknown)";
}

/*
 * Build the per-PID result list from a graph snapshot.
 *
 * For each node owned by `pid`:
 *   1. Classify it (output/input/video/midi/other)
 *   2. Follow links to find what it's connected to
 *   3. Format a display string:
 *        "App Name → Sink Description (media class)"
 *      or for inputs:
 *        "Source Description → App Name (media class)"
 */
static void build_pid_results(const pw_graph_t *g, pid_t pid,
                              pw_result_list_t *out)
{
    for (size_t ni = 0; ni < g->node_count; ni++) {
        const pw_snap_node_t *node = &g->nodes[ni];
        if (node->pid != pid)
            continue;

        pw_scan_category_t cat = classify_media_class(node->media_class);
        const char *self_label = node_label(node);

        /* Collect unique peer node IDs this node links to.
         * For an output stream (Stream/Output/Audio), the stream
         * is the output side of the link → peer is the input side.
         * For an input stream (Stream/Input/Audio), the stream
         * is the input side → peer is the output side. */
        uint32_t peers[64];
        size_t npeer = 0;

        for (size_t li = 0; li < g->link_count; li++) {
            const pw_snap_link_t *lk = &g->links[li];
            uint32_t peer_id = 0;

            if (lk->output_node_id == node->id)
                peer_id = lk->input_node_id;
            else if (lk->input_node_id == node->id)
                peer_id = lk->output_node_id;
            else
                continue;

            if (peer_id == 0 || peer_id == node->id)
                continue;

            /* Dedup */
            int found = 0;
            for (size_t k = 0; k < npeer; k++)
                if (peers[k] == peer_id) { found = 1; break; }
            if (!found && npeer < 64)
                peers[npeer++] = peer_id;
        }

        if (npeer == 0) {
            /* Node exists but has no active links */
            char buf[512];
            snprintf(buf, sizeof(buf), "%s  (not connected)", self_label);
            pw_result_push(out, cat, buf, node->id);
        } else {
            for (size_t pi = 0; pi < npeer; pi++) {
                const pw_snap_node_t *peer = pw_graph_find_node(g, peers[pi]);
                const char *peer_label = peer ? node_label(peer) : "(unknown)";

                char buf[512];
                if (cat == PW_CAT_OUTPUT) {
                    /* Our stream → Sink */
                    snprintf(buf, sizeof(buf), "%s → %s",
                             self_label, peer_label);
                } else if (cat == PW_CAT_INPUT) {
                    /* Source → Our stream */
                    snprintf(buf, sizeof(buf), "%s → %s",
                             peer_label, self_label);
                } else {
                    snprintf(buf, sizeof(buf), "%s ↔ %s",
                             self_label, peer_label);
                }
                pw_result_push(out, cat, buf, node->id);
            }
        }
    }
}

/* ── Pango markup ────────────────────────────────────────────── */

static char *pw_entry_to_markup(const char *text)
{
    /* Highlight the arrow and the part after it in a colour */
    const char *arrow = strstr(text, " → ");
    if (!arrow)
        arrow = strstr(text, " ↔ ");

    if (arrow) {
        char *left  = g_markup_escape_text(text, (gssize)(arrow - text));
        char *right = g_markup_escape_text(arrow + strlen(" → "), -1);
        /* Pick a nice colour for the target device */
        char *markup = g_strdup_printf(
            "<b>%s</b> → <span foreground=\"#6699cc\">%s</span>",
            left, right);
        g_free(left);
        g_free(right);
        return markup;
    }

    return g_markup_escape_text(text, -1);
}

/* ── GTask async scan ────────────────────────────────────────── */

typedef struct {
    pid_t              pid;
    guint              generation;
    pw_result_list_t   buckets[PW_CAT_COUNT];
    uint32_t           audio_node_id;   /* first Stream/Output/Audio node for pid, or 0 */
    uint32_t           audio_node_ids[64]; /* all audio output node IDs for this pid */
    size_t             audio_node_count;
    ui_ctx_t          *ctx;
} pw_scan_task_t;

static void pw_scan_task_free(pw_scan_task_t *t)
{
    if (!t) return;
    for (int c = 0; c < PW_CAT_COUNT; c++)
        pw_result_free(&t->buckets[c]);
    free(t);
}

static void pw_scan_thread_func(GTask        *task,
                                gpointer      source_object,
                                gpointer      task_data,
                                GCancellable *cancellable)
{
    (void)source_object;
    pw_scan_task_t *t = task_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    pw_graph_t graph;
    if (pw_snapshot(&graph) != 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "PipeWire connection failed");
        return;
    }

    if (g_cancellable_is_cancelled(cancellable)) {
        pw_graph_free(&graph);
        return;
    }

    /* Build per-PID results */
    pw_result_list_t all;
    pw_result_init(&all);
    build_pid_results(&graph, t->pid, &all);

    /* Find all audio output nodes for the spectrogram */
    t->audio_node_id = 0;
    t->audio_node_count = 0;
    for (size_t i = 0; i < graph.node_count; i++) {
        if (graph.nodes[i].pid == t->pid &&
            strstr(graph.nodes[i].media_class, "Stream") &&
            strstr(graph.nodes[i].media_class, "Output") &&
            strstr(graph.nodes[i].media_class, "Audio")) {
            if (t->audio_node_count < 64)
                t->audio_node_ids[t->audio_node_count++] = graph.nodes[i].id;
            if (t->audio_node_id == 0)
                t->audio_node_id = graph.nodes[i].id;
        }
    }

    pw_graph_free(&graph);

    /* Split into per-category buckets */
    for (int c = 0; c < PW_CAT_COUNT; c++)
        pw_result_init(&t->buckets[c]);

    for (size_t i = 0; i < all.count; i++) {
        pw_scan_category_t cat = all.entries[i].cat;
        pw_result_push(&t->buckets[cat], cat, all.entries[i].text,
                       all.entries[i].node_id);
    }
    pw_result_free(&all);

    g_task_return_boolean(task, TRUE);
}

static void pw_scan_complete(GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
    (void)source_object;
    ui_ctx_t *ctx = user_data;

    GTask *task = G_TASK(result);
    pw_scan_task_t *t = g_task_get_task_data(task);

    if (!t || t->generation != ctx->pw_generation)
        return;
    if (g_task_had_error(task))
        return;

    /* Save scroll position */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(gtk_widget_get_parent(
            GTK_WIDGET(ctx->pw_view))));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->pw_store);

    /* Index existing category rows */
    GtkTreeIter cat_iters[PW_CAT_COUNT];
    gboolean    cat_exists[PW_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(model, &top, PW_COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < PW_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    /* Remove empty categories */
    for (int c = 0; c < PW_CAT_COUNT; c++) {
        if (cat_exists[c] && t->buckets[c].count == 0) {
            gtk_tree_store_remove(ctx->pw_store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
    for (int c = 0; c < PW_CAT_COUNT; c++) {
        if (t->buckets[c].count == 0) continue;

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s (%zu)",
                 pw_cat_label[c], t->buckets[c].count);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_append(ctx->pw_store, &parent, NULL);
            gtk_tree_store_set(ctx->pw_store, &parent,
                               PW_COL_TEXT, hdr,
                               PW_COL_MARKUP, hdr_escaped,
                               PW_COL_CAT, (gint)c, -1);
            g_free(hdr_escaped);
            cat_exists[c] = TRUE;
            cat_iters[c]  = parent;
        } else {
            parent = cat_iters[c];
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_set(ctx->pw_store, &parent,
                               PW_COL_TEXT, hdr,
                               PW_COL_MARKUP, hdr_escaped, -1);
            g_free(hdr_escaped);
        }

        /* Update child rows in place, adding/removing as needed */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);
        size_t bi = 0;

        while (bi < t->buckets[c].count && child_valid) {
            char *markup = pw_entry_to_markup(t->buckets[c].entries[bi].text);
            gtk_tree_store_set(ctx->pw_store, &child,
                               PW_COL_TEXT, t->buckets[c].entries[bi].text,
                               PW_COL_MARKUP, markup,
                               PW_COL_CAT, (gint)-1,
                               PW_COL_NODE_ID, (guint)t->buckets[c].entries[bi].node_id, -1);
            g_free(markup);
            bi++;
            child_valid = gtk_tree_model_iter_next(model, &child);
        }

        /* Append new rows */
        while (bi < t->buckets[c].count) {
            GtkTreeIter new_child;
            char *markup = pw_entry_to_markup(t->buckets[c].entries[bi].text);
            gtk_tree_store_append(ctx->pw_store, &new_child, &parent);
            gtk_tree_store_set(ctx->pw_store, &new_child,
                               PW_COL_TEXT, t->buckets[c].entries[bi].text,
                               PW_COL_MARKUP, markup,
                               PW_COL_CAT, (gint)-1,
                               PW_COL_NODE_ID, (guint)t->buckets[c].entries[bi].node_id, -1);
            g_free(markup);
            bi++;
        }

        /* Remove excess rows */
        while (child_valid) {
            child_valid = gtk_tree_store_remove(ctx->pw_store, &child);
        }

        /* Restore expand/collapse state */
        GtkTreePath *cat_path = gtk_tree_model_get_path(
            model, &cat_iters[c]);
        if (ctx->pw_collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->pw_view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->pw_view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);

    /* Start (or stop) the spectrogram based on audio node availability.
     *
     * If we're already capturing one of this PID's audio nodes,
     * keep it (the user may have explicitly selected it).
     * Otherwise, auto-start on the first available audio output node.
     */
    if (t->audio_node_count > 0) {
        /* Only auto-start spectrogram if user has explicitly shown it */
        if (ctx->sb_spectro_user_shown) {
            uint32_t current_node = spectrogram_get_target_node(ctx, NULL);
            int found_current = 0;
            for (size_t i = 0; i < t->audio_node_count; i++) {
                if (t->audio_node_ids[i] == current_node) {
                    found_current = 1;
                    break;
                }
            }
            if (!found_current)
                spectrogram_start_for_node(ctx, NULL,
                                           t->audio_node_ids[0]);
        }

        /* Start peak meters for all audio output nodes */
        pw_meter_start(ctx, t->audio_node_ids, t->audio_node_count);
    } else {
        spectrogram_stop(ctx, NULL);
        pw_meter_stop(ctx);
    }
}

/* ── public API ──────────────────────────────────────────────── */

void pipewire_scan_start(ui_ctx_t *ctx, pid_t pid)
{
    /* Disabled: PipeWire audio is now handled entirely by the
     * PipeWire plugin (src/plugins/pipewire_plugin.c).  The old
     * detail panel scanner is kept compiled but not called to avoid
     * conflicting PipeWire sink creation. */
    (void)ctx;
    (void)pid;
}

/* ── signal callbacks ────────────────────────────────────────── */

void on_pw_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                         GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->pw_store), iter,
                       PW_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < PW_CAT_COUNT)
        ctx->pw_collapsed |= (1u << cat_id);
}

void on_pw_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                        GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->pw_store), iter,
                       PW_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < PW_CAT_COUNT)
        ctx->pw_collapsed &= ~(1u << cat_id);
}

gboolean on_pw_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
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
    gtk_tree_model_get(model, &iter, PW_COL_CAT, &cat_id, -1);
    if (cat_id < 0)
        return FALSE;

    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    if (!path)
        return FALSE;

    if (gtk_tree_view_row_expanded(view, path))
        gtk_tree_view_collapse_row(view, path);
    else
        gtk_tree_view_expand_row(view, path, FALSE);

    gtk_tree_path_free(path);
    return TRUE;
}

/*
 * Double-click (or Enter on) a leaf row in the PipeWire tree to
 * switch the spectrogram to that specific audio stream.
 */
void on_pw_row_activated(GtkTreeView *view, GtkTreePath *path,
                         GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    ui_ctx_t *ctx = data;

    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;

    /* Only act on leaf rows (cat == -1) */
    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, PW_COL_CAT, &cat_id, -1);
    if (cat_id >= 0)
        return;   /* category header row — handled by expand/collapse */

    guint node_id = 0;
    gtk_tree_model_get(model, &iter, PW_COL_NODE_ID, &node_id, -1);
    if (node_id == 0)
        return;

    /* Mark that the user explicitly wants the spectrogram visible */
    ctx->sb_spectro_user_shown = TRUE;
    spectrogram_start_for_node(ctx, NULL, (uint32_t)node_id);
}


#endif
