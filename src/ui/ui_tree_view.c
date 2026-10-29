#include "ui_gtk_private.h"

/* ── status bar right-click context menu ──────────────────────── */

static void on_toggle_menubar(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (gtk_widget_get_visible(ctx->menubar))
        gtk_widget_hide(ctx->menubar);
    else
        gtk_widget_show_all(ctx->menubar);
}

static gboolean get_selected_process_for_menu(ui_ctx_t *ctx, pid_t *pid,
                                               gchar **name)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(ctx->view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (!selection ||
        !gtk_tree_selection_get_selected(selection, &model, &iter))
        return FALSE;

    gint value = 0;
    gtk_tree_model_get(model, &iter,
                       COL_PID, &value,
                       COL_NAME, name,
                       -1);
    if (value <= 0) {
        g_clear_pointer(name, g_free);
        return FALSE;
    }

    *pid = (pid_t)value;
    return TRUE;
}

void show_status_context_menu(ui_ctx_t *ctx, GdkEventButton *ev)
{
    GtkWidget *menu = gtk_menu_new();
    gboolean have_shared_item = FALSE;

    if (gtk_ui_append_audio_only_menu_item(ctx, menu))
        have_shared_item = TRUE;
    if (gtk_ui_append_gpu_only_menu_item(ctx, menu))
        have_shared_item = TRUE;
    if (gtk_ui_append_cpu_intensive_menu_item(ctx, menu))
        have_shared_item = TRUE;

    pid_t pid = 0;
    gchar *name = NULL;
    gboolean have_process = get_selected_process_for_menu(ctx, &pid, &name);
    GtkWidget *plugin_item = show_open_plugin_as_window_menu(
        ctx, menu, pid, have_process ? name : "No process selected");
    if (plugin_item) {
        gtk_widget_set_sensitive(plugin_item, have_process);
        have_shared_item = TRUE;
    }
    g_free(name);

    if (have_shared_item)
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());

    gboolean visible = gtk_widget_get_visible(ctx->menubar);
    const char *label = visible ? "Hide Menubar" : "Show Menubar";

    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    g_signal_connect(mi, "activate", G_CALLBACK(on_toggle_menubar), ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

gboolean gtk_ui_on_status_button_press(GtkWidget *widget, GdkEventButton *ev,
                                       gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    if (ev->button == 3) {   /* right-click */
        show_status_context_menu(ctx, ev);
        return TRUE;
    }
    return FALSE;
}

/* ── window close handler ────────────────────────────────────── */

void gtk_ui_on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    ui_ctx_t *ctx = data;
    monitor_state_t *mon = ctx->mon;

    ctx->shutting_down = TRUE;

    pthread_mutex_lock(&mon->lock);
    mon->running = 0;
    pthread_cond_broadcast(&mon->updated);
    pthread_mutex_unlock(&mon->lock);

    gtk_main_quit();
}

/* ── inverted sort comparators ────────────────────────────────── */

/*
 * GTK's default sort-indicator arrows can feel backwards: the "up"
 * arrow (▲) means ascending (smallest first) and "down" (▼) means
 * descending.  Most users expect ▲ = highest first.  We fix this by
 * negating the comparison so GTK's "ascending" actually sorts
 * descending-by-value, making the arrow intuitive.
 *
 * Pinned top-level rows (COL_PINNED_ROOT != PTREE_UNPINNED) always
 * sort above normal top-level rows.  Among pinned rows the normal
 * column sort order applies.
 */

/* Helper: compare two top-level iters by pinned status.
 *
 * Pinned rows must always appear visually above non-pinned rows,
 * regardless of the current sort column or direction.
 *
 * Because our sort functions invert the comparison (so GTK's
 * "ascending" displays largest first) AND GTK itself negates
 * the result when the direction is descending, the sign we
 * need to return flips depending on the current sort order.
 *
 * GTK "ascending" + our inversion  → return  1 means a BEFORE b
 * GTK "descending" + our inversion → return -1 means a BEFORE b
 *
 * Returns 0 when both rows have the same pinned status or when
 * the rows are not both top-level (children sort normally).
 */
static inline int pinned_cmp(GtkTreeModel *model,
                             GtkTreeIter  *a,
                             GtkTreeIter  *b)
{
    /* Check depth: only top-level rows participate in pinned ordering.
     * A quick way: see if the iter has a parent. */
    GtkTreeIter parent_a, parent_b;
    gboolean a_top = !gtk_tree_model_iter_parent(model, &parent_a, a);
    gboolean b_top = !gtk_tree_model_iter_parent(model, &parent_b, b);

    if (!a_top || !b_top)
        return 0;  /* children within a subtree: normal sort */

    gint pa = 0, pb = 0;
    gtk_tree_model_get(model, a, COL_PINNED_ROOT, &pa, -1);
    gtk_tree_model_get(model, b, COL_PINNED_ROOT, &pb, -1);

    gboolean a_pinned = (pa != (gint)PTREE_UNPINNED);
    gboolean b_pinned = (pb != (gint)PTREE_UNPINNED);

    if (a_pinned == b_pinned)
        return 0;  /* both pinned or both unpinned → normal sort */

    /* Determine effective sign: we need "pinned row first" in both
     * ascending and descending modes.
     *
     * Our sort funcs already invert (ascending → largest first).
     * GTK applies an extra negation when direction == descending.
     *
     *   ascending:  return  1 → GTK displays a before b  (inverted)
     *   descending: return -1 → GTK negates to 1 → a before b
     *
     * So: ascending needs +1 for "a first", descending needs -1.      */
    GtkSortType order = GTK_SORT_ASCENDING;
    gint sort_col = GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID;
    gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(model),
                                         &sort_col, &order);

    int sign = (order == GTK_SORT_ASCENDING) ? 1 : -1;

    return a_pinned ? sign : -sign;
}

static gint sort_int_inverted(GtkTreeModel *model,
                              GtkTreeIter  *a,
                              GtkTreeIter  *b,
                              gpointer      col_id_ptr)
{
    int pc = pinned_cmp(model, a, b);
    if (pc) return pc;

    gint col = GPOINTER_TO_INT(col_id_ptr);
    gint va = 0, vb = 0;
    gtk_tree_model_get(model, a, col, &va, -1);
    gtk_tree_model_get(model, b, col, &vb, -1);
    /* Negate: GTK "ascending" will now show largest first. */
    return (va < vb) ? 1 : (va > vb) ? -1 : 0;
}

static gint sort_string_inverted(GtkTreeModel *model,
                                 GtkTreeIter  *a,
                                 GtkTreeIter  *b,
                                 gpointer      col_id_ptr)
{
    int pc = pinned_cmp(model, a, b);
    if (pc) return pc;

    gint col = GPOINTER_TO_INT(col_id_ptr);
    gchar *sa = NULL, *sb = NULL;
    gtk_tree_model_get(model, a, col, &sa, -1);
    gtk_tree_model_get(model, b, col, &sb, -1);
    int cmp = 0;
    if (sa && sb)      cmp = g_utf8_collate(sa, sb);
    else if (sa)       cmp = 1;
    else if (sb)       cmp = -1;
    g_free(sa);
    g_free(sb);
    /* Negate for inverted arrow direction. */
    return -cmp;
}

static gint sort_int64_inverted(GtkTreeModel *model,
                               GtkTreeIter  *a,
                               GtkTreeIter  *b,
                               gpointer      col_id_ptr)
{
    int pc = pinned_cmp(model, a, b);
    if (pc) return pc;

    gint col = GPOINTER_TO_INT(col_id_ptr);
    gint64 va = 0, vb = 0;
    gtk_tree_model_get(model, a, col, &va, -1);
    gtk_tree_model_get(model, b, col, &vb, -1);
    return (va < vb) ? 1 : (va > vb) ? -1 : 0;
}

/*
 * Register all inverted sort functions on a GtkTreeModelSort.
 * Called every time we create a new sort model (when switching
 * between the real store and the filter shadow store).
 */
void register_sort_funcs(GtkTreeModelSort *sm)
{
    GtkTreeSortable *sortable = GTK_TREE_SORTABLE(sm);

    gtk_tree_sortable_set_sort_func(sortable, COL_PID,
        sort_int_inverted, GINT_TO_POINTER(COL_PID), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_PPID,
        sort_int_inverted, GINT_TO_POINTER(COL_PPID), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_USER,
        sort_string_inverted, GINT_TO_POINTER(COL_USER), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_NAME,
        sort_string_inverted, GINT_TO_POINTER(COL_NAME), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_CPU,
        sort_int_inverted, GINT_TO_POINTER(COL_CPU), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_GPU,
        sort_int_inverted, GINT_TO_POINTER(COL_GPU), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_GPU_VRAM,
        sort_int64_inverted, GINT_TO_POINTER(COL_GPU_VRAM), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_GPU_ENC,
        sort_int_inverted, GINT_TO_POINTER(COL_GPU_ENC), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_RSS,
        sort_int_inverted, GINT_TO_POINTER(COL_RSS), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_GROUP_RSS,
        sort_int_inverted, GINT_TO_POINTER(COL_GROUP_RSS), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_GROUP_CPU,
        sort_int_inverted, GINT_TO_POINTER(COL_GROUP_CPU), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_IO_READ_RATE,
        sort_int_inverted, GINT_TO_POINTER(COL_IO_READ_RATE), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_IO_WRITE_RATE,
        sort_int_inverted, GINT_TO_POINTER(COL_IO_WRITE_RATE), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_START_TIME,
        sort_int64_inverted, GINT_TO_POINTER(COL_START_TIME), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_CONTAINER,
        sort_string_inverted, GINT_TO_POINTER(COL_CONTAINER), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_SERVICE,
        sort_string_inverted, GINT_TO_POINTER(COL_SERVICE), NULL);
    gtk_tree_sortable_set_sort_func(sortable, COL_CWD,
        sort_string_inverted, GINT_TO_POINTER(COL_CWD), NULL);
}

/* ── expand / collapse helpers ───────────────────────────────── */

void expand_respecting_collapsed_recurse(ui_ctx_t *ctx,
                                         GtkTreeModel *model,
                                         GtkTreeIter *parent)
{
    GtkTreeIter iter;
    gboolean valid = parent
        ? gtk_tree_model_iter_children(model, &iter, parent)
        : gtk_tree_model_get_iter_first(model, &iter);

    while (valid) {
        if (gtk_tree_model_iter_has_child(model, &iter)) {
            GtkTreeIter child_it;
            gtk_tree_model_sort_convert_iter_to_child_iter(
                ctx->sort_model, &child_it, &iter);
            GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                ctx->sort_model);
            gint pid;
            gtk_tree_model_get(child_model, &child_it, COL_PID, &pid, -1);

            pid_t pinned_root = get_row_pinned_root(child_model, &child_it);
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            if (get_process_tree_node(&ctx->ptree_nodes, pinned_root,
                                      (pid_t)pid) == PTREE_COLLAPSED) {
                gtk_tree_view_collapse_row(ctx->view, path);
            } else {
                gtk_tree_view_expand_row(ctx->view, path, FALSE);
                expand_respecting_collapsed_recurse(ctx, model, &iter);
            }
            gtk_tree_path_free(path);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

void expand_respecting_collapsed(ui_ctx_t *ctx)
{
    GtkTreeModel *model = gtk_tree_view_get_model(ctx->view);
    if (!model) return;
    expand_respecting_collapsed_recurse(ctx, model, NULL);
}

/* ── public entry point ──────────────────────────────────────── */

/*
 * Cell data function for row highlighting: computes a fading
 * background colour for newly spawned (green) or dying (red)
 * processes.  Applied to every column renderer so the entire row
 * is coloured.
 *
 * The sort model exposes the underlying store's data, so we read
 * COL_HIGHLIGHT_BORN / COL_HIGHLIGHT_DIED directly from `model`.
 */
void gtk_ui_highlight_cell_data_func(GtkTreeViewColumn *col,
                                     GtkCellRenderer   *cell,
                                     GtkTreeModel      *model,
                                     GtkTreeIter       *iter,
                                     gpointer           data)
{
    (void)col; (void)data;

    gint64 born = 0, died = 0;
    gtk_tree_model_get(model, iter,
                       COL_HIGHLIGHT_BORN, &born,
                       COL_HIGHLIGHT_DIED, &died, -1);

    /* Get current monotonic time */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    gint64 now = (gint64)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;

    GdkRGBA rgba = { 0, 0, 0, 0 };   /* fully transparent = no background */
    gboolean set_bg = FALSE;

    if (died > 0) {
        /* Dying process: red fade-out */
        gint64 age = now - died;
        if (age < HIGHLIGHT_FADE_US) {
            double alpha = 0.35 * (1.0 - (double)age / HIGHLIGHT_FADE_US);
            rgba = (GdkRGBA){ 0.95, 0.25, 0.20, alpha };
            set_bg = TRUE;
        }
    } else if (born > 0) {
        /* New process: green fade-out */
        gint64 age = now - born;
        if (age < HIGHLIGHT_FADE_US) {
            double alpha = 0.30 * (1.0 - (double)age / HIGHLIGHT_FADE_US);
            rgba = (GdkRGBA){ 0.20, 0.80, 0.30, alpha };
            set_bg = TRUE;
        }
    }

    if (set_bg)
        g_object_set(cell, "cell-background-rgba", &rgba, NULL);
    else
        g_object_set(cell, "cell-background-set", FALSE, NULL);
}

/*
 * Cell data function for the Name column: prepend an arrow when the
 * process is pinned.  The process is considered "pinned" when its
 * own PID appears in the pinned set (regardless of whether the
 * current row is the pinned copy or the original tree entry).
 */
/* ── Audio PID probing ────────────────────────────────────────
 * Take a PipeWire graph snapshot and collect every PID that has
 * an active audio stream.  Called once per refresh tick when the
 * audio plugin is loaded.
 */
/*
 * Broker callback: receives audio PIDs extracted from the PipeWire
 * graph on the broker worker thread, delivered to us on the GTK thread.
 */
