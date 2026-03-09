/*
 * ui_filter.c – Name-filter and audio-only filter shadow stores.
 *
 * The filter is implemented as a shadow GtkTreeStore that mirrors
 * the subset of rows from the real store that match the current
 * filter text (or audio-PID set).  The real store is never modified;
 * switching between real and filtered view is a matter of swapping
 * the sort model.
 *
 * Public surface (declared in ui_internal.h):
 *   rebuild_filter_store()
 *   sync_filter_store()
 *   switch_to_real_store()
 *   rebuild_audio_filter_store()
 *   sync_audio_filter_store()
 *   copy_subtree()
 */

#include "ui_internal.h"

/* ── row predicate ────────────────────────────────────────────── */

/* Returns TRUE if the row at `iter` should be a filter root. */
typedef gboolean (*row_matches_fn)(GtkTreeModel *model, GtkTreeIter *iter,
                                   void *userdata);

/*
 * Deep-copy a subtree from `src` into `dst` under `dst_parent`.
 */
void copy_subtree(GtkTreeStore *dst, GtkTreeIter *dst_parent,
                  GtkTreeModel *src, GtkTreeIter *src_iter)
{
    GtkTreeIter dst_iter;
    gtk_tree_store_append(dst, &dst_iter, dst_parent);

    gint pid, ppid, cpu, rss, grp_rss, grp_cpu, pinned_root;
    gint io_read_rate, io_write_rate;
    gint64 start_time, hl_born, hl_died;
    gchar *user = NULL, *name = NULL, *cpu_text = NULL, *rss_text = NULL;
    gchar *grp_rss_text = NULL, *grp_cpu_text = NULL;
    gchar *io_read_text = NULL, *io_write_text = NULL;
    gchar *start_text = NULL, *container = NULL, *service = NULL,
          *cwd = NULL, *cmdline = NULL;
    gchar *spark_data = NULL;
    gint spark_peak;
    GdkPixbuf *icon = NULL;

    gtk_tree_model_get(src, src_iter,
        COL_PID, &pid, COL_PPID, &ppid, COL_USER, &user, COL_NAME, &name,
        COL_CPU, &cpu, COL_CPU_TEXT, &cpu_text,
        COL_RSS, &rss, COL_RSS_TEXT, &rss_text,
        COL_GROUP_RSS, &grp_rss, COL_GROUP_RSS_TEXT, &grp_rss_text,
        COL_GROUP_CPU, &grp_cpu, COL_GROUP_CPU_TEXT, &grp_cpu_text,
        COL_IO_READ_RATE, &io_read_rate, COL_IO_READ_RATE_TEXT, &io_read_text,
        COL_IO_WRITE_RATE, &io_write_rate, COL_IO_WRITE_RATE_TEXT, &io_write_text,
        COL_START_TIME, &start_time, COL_START_TIME_TEXT, &start_text,
        COL_CONTAINER, &container, COL_SERVICE, &service,
        COL_CWD, &cwd, COL_CMDLINE, &cmdline,
        COL_IO_SPARKLINE, &spark_data,
        COL_IO_SPARKLINE_PEAK, &spark_peak,
        COL_HIGHLIGHT_BORN, &hl_born, COL_HIGHLIGHT_DIED, &hl_died,
        COL_PINNED_ROOT, &pinned_root,
        COL_ICON, &icon,
        -1);

    gtk_tree_store_set(dst, &dst_iter,
        COL_PID, pid, COL_PPID, ppid, COL_USER, user, COL_NAME, name,
        COL_CPU, cpu, COL_CPU_TEXT, cpu_text,
        COL_RSS, rss, COL_RSS_TEXT, rss_text,
        COL_GROUP_RSS, grp_rss, COL_GROUP_RSS_TEXT, grp_rss_text,
        COL_GROUP_CPU, grp_cpu, COL_GROUP_CPU_TEXT, grp_cpu_text,
        COL_IO_READ_RATE, io_read_rate, COL_IO_READ_RATE_TEXT, io_read_text,
        COL_IO_WRITE_RATE, io_write_rate, COL_IO_WRITE_RATE_TEXT, io_write_text,
        COL_START_TIME, start_time, COL_START_TIME_TEXT, start_text,
        COL_CONTAINER, container, COL_SERVICE, service,
        COL_CWD, cwd, COL_CMDLINE, cmdline,
        COL_IO_SPARKLINE, spark_data,
        COL_IO_SPARKLINE_PEAK, spark_peak,
        COL_HIGHLIGHT_BORN, hl_born, COL_HIGHLIGHT_DIED, hl_died,
        COL_PINNED_ROOT, pinned_root,
        COL_ICON, icon,
        -1);

    if (icon) g_object_unref(icon);   /* gtk_tree_model_get reffed it */
    g_free(user); g_free(name); g_free(cpu_text); g_free(rss_text);
    g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(io_read_text); g_free(io_write_text); g_free(start_text);
    g_free(container); g_free(service); g_free(cwd); g_free(cmdline);
    g_free(spark_data);

    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(src, &child, src_iter);
    while (valid) {
        copy_subtree(dst, &dst_iter, src, &child);
        valid = gtk_tree_model_iter_next(src, &child);
    }
}

/* ── predicate implementations ────────────────────────────────── */

static gboolean match_by_name(GtkTreeModel *model, GtkTreeIter *iter,
                               void *userdata)
{
    const char *filter_lower = (const char *)userdata;
    gchar *name = NULL;
    gtk_tree_model_get(model, iter, COL_NAME, &name, -1);
    if (!name) return FALSE;
    gchar *down = g_utf8_strdown(name, -1);
    gboolean match = (strstr(down, filter_lower) != NULL);
    g_free(down);
    g_free(name);
    return match;
}

static gboolean match_audio(GtkTreeModel *model, GtkTreeIter *iter,
                             void *userdata)
{
    const ui_ctx_t *ctx = (const ui_ctx_t *)userdata;
    gint pid = 0, pr = 0;
    gtk_tree_model_get(model, iter, COL_PID, &pid, COL_PINNED_ROOT, &pr, -1);
    if (pr != (gint)PTREE_UNPINNED) return FALSE;
    return audio_pid_is_active(ctx, (pid_t)pid) || pid_is_pinned(ctx, (pid_t)pid);
}

/* ── generic tree-walking helpers ─────────────────────────────── */

static void find_and_copy_matches(GtkTreeStore *dst, GtkTreeModel *src,
                                   GtkTreeIter *parent,
                                   row_matches_fn matches, void *userdata,
                                   gboolean ancestor_matched)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(src, &iter, parent);
    while (valid) {
        if (!ancestor_matched && matches(src, &iter, userdata))
            copy_subtree(dst, NULL, src, &iter);
        else if (!ancestor_matched)
            find_and_copy_matches(dst, src, &iter, matches, userdata, FALSE);
        valid = gtk_tree_model_iter_next(src, &iter);
    }
}

static int count_filter_matches(GtkTreeModel *model, GtkTreeIter *parent,
                                 row_matches_fn matches, void *userdata,
                                 gboolean ancestor_matched)
{
    int count = 0;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gboolean self_matches = ancestor_matched || matches(model, &iter, userdata);
        count += self_matches ? 1 : 0;
        count += count_filter_matches(model, &iter, matches, userdata, self_matches);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return count;
}

static void sync_row_from_real(GtkTreeStore *fs, GtkTreeIter *fs_iter,
                               GtkTreeModel *real, GtkTreeIter *real_iter)
{
    gint pid, ppid, cpu, rss, grp_rss, grp_cpu;
    gint io_read_rate, io_write_rate;
    gint64 start_time, hl_born, hl_died;
    gchar *user = NULL, *name = NULL, *cpu_text = NULL, *rss_text = NULL;
    gchar *grp_rss_text = NULL, *grp_cpu_text = NULL;
    gchar *io_read_text = NULL, *io_write_text = NULL;
    gchar *start_text = NULL, *container = NULL, *service = NULL,
          *cwd = NULL, *cmdline = NULL;
    gchar *spark_data = NULL;
    gint spark_peak;

    gtk_tree_model_get(real, real_iter,
        COL_PID, &pid, COL_PPID, &ppid, COL_USER, &user, COL_NAME, &name,
        COL_CPU, &cpu, COL_CPU_TEXT, &cpu_text,
        COL_RSS, &rss, COL_RSS_TEXT, &rss_text,
        COL_GROUP_RSS, &grp_rss, COL_GROUP_RSS_TEXT, &grp_rss_text,
        COL_GROUP_CPU, &grp_cpu, COL_GROUP_CPU_TEXT, &grp_cpu_text,
        COL_IO_READ_RATE, &io_read_rate, COL_IO_READ_RATE_TEXT, &io_read_text,
        COL_IO_WRITE_RATE, &io_write_rate, COL_IO_WRITE_RATE_TEXT, &io_write_text,
        COL_START_TIME, &start_time, COL_START_TIME_TEXT, &start_text,
        COL_CONTAINER, &container, COL_SERVICE, &service,
        COL_CWD, &cwd, COL_CMDLINE, &cmdline,
        COL_IO_SPARKLINE, &spark_data,
        COL_IO_SPARKLINE_PEAK, &spark_peak,
        COL_HIGHLIGHT_BORN, &hl_born, COL_HIGHLIGHT_DIED, &hl_died,
        -1);

    gint pinned_root;
    gtk_tree_model_get(GTK_TREE_MODEL(fs), fs_iter,
                       COL_PINNED_ROOT, &pinned_root, -1);

    gtk_tree_store_set(fs, fs_iter,
        COL_PID, pid, COL_PPID, ppid, COL_USER, user, COL_NAME, name,
        COL_CPU, cpu, COL_CPU_TEXT, cpu_text,
        COL_RSS, rss, COL_RSS_TEXT, rss_text,
        COL_GROUP_RSS, grp_rss, COL_GROUP_RSS_TEXT, grp_rss_text,
        COL_GROUP_CPU, grp_cpu, COL_GROUP_CPU_TEXT, grp_cpu_text,
        COL_IO_READ_RATE, io_read_rate, COL_IO_READ_RATE_TEXT, io_read_text,
        COL_IO_WRITE_RATE, io_write_rate, COL_IO_WRITE_RATE_TEXT, io_write_text,
        COL_START_TIME, start_time, COL_START_TIME_TEXT, start_text,
        COL_CONTAINER, container, COL_SERVICE, service,
        COL_CWD, cwd, COL_CMDLINE, cmdline,
        COL_IO_SPARKLINE, spark_data,
        COL_IO_SPARKLINE_PEAK, spark_peak,
        COL_HIGHLIGHT_BORN, hl_born, COL_HIGHLIGHT_DIED, hl_died,
        COL_PINNED_ROOT, pinned_root,
        -1);

    g_free(user); g_free(name); g_free(cpu_text); g_free(rss_text);
    g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(io_read_text); g_free(io_write_text); g_free(start_text);
    g_free(container); g_free(service); g_free(cwd); g_free(cmdline);
    g_free(spark_data);
}

static gboolean sync_filter_rows(GtkTreeStore *fs, GtkTreeIter *fs_parent,
                                  GtkTreeModel *real)
{
    GtkTreeIter fs_iter;
    gboolean valid = fs_parent
        ? gtk_tree_model_iter_children(GTK_TREE_MODEL(fs), &fs_iter, fs_parent)
        : gtk_tree_model_get_iter_first(GTK_TREE_MODEL(fs), &fs_iter);

    while (valid) {
        gint pid;
        gtk_tree_model_get(GTK_TREE_MODEL(fs), &fs_iter, COL_PID, &pid, -1);

        GtkTreeIter real_iter;
        if (!find_iter_by_pid(real, NULL, (pid_t)pid, &real_iter))
            return FALSE;

        sync_row_from_real(fs, &fs_iter, real, &real_iter);

        if (!sync_filter_rows(fs, &fs_iter, real))
            return FALSE;

        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(fs), &fs_iter);
    }
    return TRUE;
}

static int count_store_rows(GtkTreeModel *model, GtkTreeIter *parent)
{
    int count = 0;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        count++;
        count += count_store_rows(model, &iter);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return count;
}

/* Helper: allocate a fresh store matching the main store column layout */
static GtkTreeStore *alloc_filter_store(void)
{
    return gtk_tree_store_new(NUM_COLS,
        G_TYPE_INT,    /* COL_PID               */
        G_TYPE_INT,    /* COL_PPID              */
        G_TYPE_STRING, /* COL_USER              */
        G_TYPE_STRING, /* COL_NAME              */
        G_TYPE_INT,    /* COL_CPU               */
        G_TYPE_STRING, /* COL_CPU_TEXT          */
        G_TYPE_INT,    /* COL_RSS               */
        G_TYPE_STRING, /* COL_RSS_TEXT          */
        G_TYPE_INT,    /* COL_GROUP_RSS         */
        G_TYPE_STRING, /* COL_GROUP_RSS_TEXT    */
        G_TYPE_INT,    /* COL_GROUP_CPU         */
        G_TYPE_STRING, /* COL_GROUP_CPU_TEXT    */
        G_TYPE_INT,    /* COL_IO_READ_RATE      */
        G_TYPE_STRING, /* COL_IO_READ_RATE_TEXT */
        G_TYPE_INT,    /* COL_IO_WRITE_RATE     */
        G_TYPE_STRING, /* COL_IO_WRITE_RATE_TEXT*/
        G_TYPE_INT64,  /* COL_START_TIME        */
        G_TYPE_STRING, /* COL_START_TIME_TEXT   */
        G_TYPE_STRING, /* COL_CONTAINER         */
        G_TYPE_STRING, /* COL_SERVICE           */
        G_TYPE_STRING, /* COL_CWD               */
        G_TYPE_STRING, /* COL_CMDLINE           */
        G_TYPE_STRING, /* COL_IO_SPARKLINE      */
        G_TYPE_INT,    /* COL_IO_SPARKLINE_PEAK */
        G_TYPE_INT64,  /* COL_HIGHLIGHT_BORN    */
        G_TYPE_INT64,  /* COL_HIGHLIGHT_DIED    */
        G_TYPE_INT,    /* COL_PINNED_ROOT       */
        GDK_TYPE_PIXBUF/* COL_ICON              */
    );
}

/*
 * Save the currently selected PID across a store rebuild.
 * Works regardless of whether old_model is a sort wrapper or bare store.
 */
static pid_t save_selected_pid(ui_ctx_t *ctx)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    GtkTreeModel     *old_model = NULL;
    GtkTreeIter       sel_iter;

    if (!sel || !gtk_tree_selection_get_selected(sel, &old_model, &sel_iter))
        return 0;

    if (GTK_IS_TREE_MODEL_SORT(old_model)) {
        GtkTreeIter child_iter;
        gtk_tree_model_sort_convert_iter_to_child_iter(
            GTK_TREE_MODEL_SORT(old_model), &child_iter, &sel_iter);
        GtkTreeModel *child_model =
            gtk_tree_model_sort_get_model(GTK_TREE_MODEL_SORT(old_model));
        gint pid_val;
        gtk_tree_model_get(child_model, &child_iter, COL_PID, &pid_val, -1);
        return (pid_t)pid_val;
    }

    /* bare store (shouldn't normally happen, but be safe) */
    gint pid_val;
    gtk_tree_model_get(old_model, &sel_iter, COL_PID, &pid_val, -1);
    return (pid_t)pid_val;
}

/*
 * Rebuild scaffold shared by all filter variants.
 * Populates a fresh store using `matches`/`userdata`, swaps in a new sort
 * model, calls `expand_fn` (with signals blocked), reconnects
 * sort-column-changed, then restores the selection.  If the previously
 * selected PID is not in the new store and `fallback_first` is TRUE,
 * selects the first row instead.
 */
static void install_filter_store(ui_ctx_t *ctx,
                                  row_matches_fn matches, void *userdata,
                                  void (*expand_fn)(ui_ctx_t *),
                                  gboolean fallback_first)
{
    pid_t sel_pid = save_selected_pid(ctx);

    GtkTreeStore *fs = alloc_filter_store();
    find_and_copy_matches(fs, GTK_TREE_MODEL(ctx->store), NULL,
                          matches, userdata, FALSE);

    if (ctx->filter_store)
        g_object_unref(ctx->filter_store);
    ctx->filter_store = fs;

    gint sort_col = GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID;
    GtkSortType sort_order = GTK_SORT_ASCENDING;
    gtk_tree_sortable_get_sort_column_id(
        GTK_TREE_SORTABLE(ctx->sort_model), &sort_col, &sort_order);

    GtkTreeModel *new_sort = gtk_tree_model_sort_new_with_model(
        GTK_TREE_MODEL(fs));
    ctx->sort_model = GTK_TREE_MODEL_SORT(new_sort);
    register_sort_funcs(ctx->sort_model);

    if (sort_col != GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID)
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(ctx->sort_model), sort_col, sort_order);

    gtk_tree_view_set_model(ctx->view, new_sort);

    g_signal_handlers_block_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_block_by_func(ctx->view, on_row_expanded,  ctx);
    expand_fn(ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);

    g_signal_connect(new_sort, "sort-column-changed",
                     G_CALLBACK(on_sort_column_changed), ctx);

    gboolean selection_restored = FALSE;
    if (sel_pid > 0) {
        GtkTreeIter found;
        if (find_iter_by_pid(GTK_TREE_MODEL(fs), NULL, sel_pid, &found)) {
            GtkTreePath *child_path =
                gtk_tree_model_get_path(GTK_TREE_MODEL(fs), &found);
            if (child_path) {
                GtkTreePath *sort_path =
                    gtk_tree_model_sort_convert_child_path_to_path(
                        ctx->sort_model, child_path);
                if (sort_path) {
                    gtk_tree_selection_select_path(
                        gtk_tree_view_get_selection(ctx->view), sort_path);
                    gtk_tree_path_free(sort_path);
                    selection_restored = TRUE;
                }
                gtk_tree_path_free(child_path);
            }
        }
    }

    if (!selection_restored && fallback_first) {
        GtkTreeIter first;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ctx->sort_model), &first)) {
            GtkTreePath *p = gtk_tree_model_get_path(
                GTK_TREE_MODEL(ctx->sort_model), &first);
            if (p) {
                gtk_tree_selection_select_path(
                    gtk_tree_view_get_selection(ctx->view), p);
                gtk_tree_view_scroll_to_cell(ctx->view, p, NULL, FALSE, 0, 0);
                gtk_tree_path_free(p);
            }
        }
    }
}

/*
 * Rebuild the shadow filter_store from scratch.
 */
void rebuild_filter_store(ui_ctx_t *ctx)
{
    gchar *filter_lower = g_utf8_strdown(ctx->filter_text, -1);
    install_filter_store(ctx, match_by_name, filter_lower,
                         expand_respecting_collapsed, FALSE);
    g_free(filter_lower);
}


/*
 * Incremental sync of the filter store.
 */
void sync_filter_store(ui_ctx_t *ctx)
{
    if (!ctx->filter_store || ctx->filter_text[0] == '\0')
        return;

    GtkTreeModel *real = GTK_TREE_MODEL(ctx->store);

    gchar *filter_lower = g_utf8_strdown(ctx->filter_text, -1);
    int expected = count_filter_matches(real, NULL, match_by_name, filter_lower, FALSE);
    int current  = count_store_rows(GTK_TREE_MODEL(ctx->filter_store), NULL);
    g_free(filter_lower);

    if (expected != current) {
        rebuild_filter_store(ctx);
        return;
    }

    if (!sync_filter_rows(ctx->filter_store, NULL, real)) {
        rebuild_filter_store(ctx);
    }
}

/*
 * Rebuild the audio-only filter store from scratch.
 * Name/PID filter takes priority: if filter_text is active, rebuild
 * by name instead so the user's search is never hidden by audio mode.
 */
void rebuild_audio_filter_store(ui_ctx_t *ctx)
{
    if (ctx->filter_text[0] != '\0') {
        rebuild_filter_store(ctx);
        return;
    }
    install_filter_store(ctx, match_audio, (void *)ctx,
                         expand_respecting_collapsed, TRUE);
}


/*
 * Incremental sync of the audio filter store.
 * Name/PID filter takes priority: if filter_text is active, sync
 * by name instead.
 */
void sync_audio_filter_store(ui_ctx_t *ctx)
{
    if (!ctx->filter_store)
        return;

    if (ctx->filter_text[0] != '\0') {
        sync_filter_store(ctx);
        return;
    }

    GtkTreeModel *real = GTK_TREE_MODEL(ctx->store);
    int expected = count_filter_matches(real, NULL, match_audio, (void *)ctx, FALSE);
    int current  = count_store_rows(GTK_TREE_MODEL(ctx->filter_store), NULL);

    if (expected != current || !sync_filter_rows(ctx->filter_store, NULL, real))
        rebuild_audio_filter_store(ctx);
}

/*
 * Switch the view back to the real (unfiltered) store.
 */
void switch_to_real_store(ui_ctx_t *ctx)
{
    if (ctx->filter_store) {
        g_object_unref(ctx->filter_store);
        ctx->filter_store = NULL;
    }

    gint sort_col = GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID;
    GtkSortType sort_order = GTK_SORT_ASCENDING;
    gtk_tree_sortable_get_sort_column_id(
        GTK_TREE_SORTABLE(ctx->sort_model), &sort_col, &sort_order);

    GtkTreeModel *new_sort = gtk_tree_model_sort_new_with_model(
        GTK_TREE_MODEL(ctx->store));
    ctx->sort_model = GTK_TREE_MODEL_SORT(new_sort);
    register_sort_funcs(ctx->sort_model);

    if (sort_col != GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID)
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(ctx->sort_model), sort_col, sort_order);

    gtk_tree_view_set_model(ctx->view, new_sort);

    g_signal_handlers_block_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_block_by_func(ctx->view, on_row_expanded,  ctx);
    expand_respecting_collapsed(ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);

    g_signal_connect(new_sort, "sort-column-changed",
                     G_CALLBACK(on_sort_column_changed), ctx);
}
