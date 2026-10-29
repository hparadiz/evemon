#include "ui_gtk_private.h"

/* ── periodic refresh callback ───────────────────────────────── */

/* ── highlight animation timer ────────────────────────────────── */

/*
 * Check whether any row in the store has an active highlight
 * (born or died timestamp that hasn't expired yet).
 */
static gboolean store_has_active_highlights(GtkTreeModel *model,
                                            GtkTreeIter  *parent,
                                            gint64        now)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint64 born = 0, died = 0;
        gtk_tree_model_get(model, &iter,
                           COL_HIGHLIGHT_BORN, &born,
                           COL_HIGHLIGHT_DIED, &died, -1);
        if ((born > 0 && now - born < HIGHLIGHT_FADE_US) ||
            (died > 0 && now - died < HIGHLIGHT_FADE_US))
            return TRUE;
        if (store_has_active_highlights(model, &iter, now))
            return TRUE;
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}

/* Timer callback: redraw the tree while highlights are active */
static gboolean on_highlight_tick(gpointer data)
{
    ui_ctx_t *ctx = data;

    if (ctx->shutting_down) {
        ctx->highlight_timer = 0;
        return G_SOURCE_REMOVE;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    gint64 now = (gint64)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;

    GtkTreeModel *model = gtk_tree_view_get_model(ctx->view);
    GtkTreeModel *underlying = model;
    if (GTK_IS_TREE_MODEL_SORT(model))
        underlying = gtk_tree_model_sort_get_model(GTK_TREE_MODEL_SORT(model));

    if (store_has_active_highlights(underlying, NULL, now)) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->view));
        return G_SOURCE_CONTINUE;
    }

    /* No more active highlights — stop the timer */
    ctx->highlight_timer = 0;
    return G_SOURCE_REMOVE;
}

/* Start the highlight timer if not already running */
static void ensure_highlight_timer(ui_ctx_t *ctx)
{
    if (ctx->highlight_timer == 0)
        ctx->highlight_timer = g_timeout_add(33, on_highlight_tick, ctx);
                                              /* ~30 fps for smooth fade */
}


/* Update both halves of the status bar from current system state. */
void gtk_ui_update_status_bar(ui_ctx_t *ctx)
{
    size_t count = ctx->pstore.count;
    pthread_mutex_lock(&ctx->mon->lock);
    monitor_system_t system = ctx->mon->system;
    int disconnected = ctx->mon->remote && !ctx->mon->connected;
    pthread_mutex_unlock(&ctx->mon->lock);
    char system_text[256];
    snprintf(system_text, sizeof(system_text),
             "up %llud %lluh %llum  |  %u user%s  |  load: %.2f %.2f %.2f ",
             (unsigned long long)system.uptime_seconds / 86400,
             (unsigned long long)system.uptime_seconds / 3600 % 24,
             (unsigned long long)system.uptime_seconds / 60 % 60,
             system.users, system.users == 1 ? "" : "s",
             system.load[0], system.load[1], system.load[2]);
    gtk_label_set_text(ctx->status_right, disconnected ? "Evemon disconnected — reconnecting" : system_text);
    long mem_total_kb = system.memory_total / 1024, mem_avail_kb = system.memory_available / 1024;
    long mem_used_kb = mem_total_kb - mem_avail_kb;
    if (mem_used_kb < 0) mem_used_kb = 0;

    char mem_used_buf[32], mem_total_buf[32];
    format_memory(mem_used_kb, mem_used_buf, sizeof(mem_used_buf));
    format_memory(mem_total_kb, mem_total_buf, sizeof(mem_total_buf));

    long ncpus = system.cpus;
    if (ncpus < 1) ncpus = 1;

    double ui_last = 0, ui_avg = 0, ui_max = 0;
    profile_get("ui_render", &ui_last, &ui_avg, &ui_max);

    char gpu_status[256] = "";
    if (ctx->has_gpu_plugin && ctx->gpu_snapshot.device_count > 0) {
        uint64_t gpu_total = 0;
        uint32_t gpu_valid_count = 0;
        uint64_t vram_used = 0;
        uint64_t vram_total = 0;
        uint32_t vram_valid_count = 0;
        uint64_t enc_total = 0;
        uint32_t enc_valid_count = 0;

        for (uint32_t i = 0; i < ctx->gpu_snapshot.device_count; i++) {
            const evemon_gpu_device_t *gpu = &ctx->gpu_snapshot.devices[i];
            if (gpu->valid & EVEMON_GPU_VALID_UTILIZATION) {
                gpu_total += gpu->utilization_percent;
                gpu_valid_count++;
            }
            if (gpu->valid & EVEMON_GPU_VALID_MEMORY) {
                vram_used = UINT64_MAX - vram_used < gpu->memory_used_bytes
                          ? UINT64_MAX
                          : vram_used + gpu->memory_used_bytes;
                vram_total = UINT64_MAX - vram_total < gpu->memory_total_bytes
                           ? UINT64_MAX
                           : vram_total + gpu->memory_total_bytes;
                vram_valid_count++;
            }
            if (gpu->valid & EVEMON_GPU_VALID_ENCODER) {
                enc_total += gpu->encoder_percent;
                enc_valid_count++;
            }
        }
        if (enc_valid_count == 0) {
            gboolean process_encoder_valid = FALSE;
            for (uint32_t i = 0; i < ctx->gpu_snapshot.process_count; i++) {
                const evemon_gpu_process_t *process =
                    &ctx->gpu_snapshot.processes[i];
                if (process->valid & EVEMON_GPU_PROCESS_VALID_ENCODE) {
                    enc_total += process->encode_percent;
                    process_encoder_valid = TRUE;
                }
            }
            if (process_encoder_valid)
                enc_valid_count = ctx->gpu_snapshot.device_count;
        }

        char gpu_text[32] = "—";
        char vram_text[80] = "—";
        char enc_text[32] = "—";
        if (gpu_valid_count > 0)
            snprintf(gpu_text, sizeof(gpu_text), "%" PRIu64 "%%",
                     gpu_total / gpu_valid_count);
        if (vram_valid_count > 0) {
            char used_text[32], total_text[32];
            gtk_ui_format_gpu_bytes(vram_used, used_text, sizeof(used_text));
            gtk_ui_format_gpu_bytes(vram_total, total_text, sizeof(total_text));
            snprintf(vram_text, sizeof(vram_text), "%s / %s",
                     used_text, total_text);
        }
        if (enc_valid_count > 0) {
            uint64_t enc_average = enc_total / enc_valid_count;
            if (enc_average > 100)
                enc_average = 100;
            snprintf(enc_text, sizeof(enc_text), "%" PRIu64 "%%",
                     enc_average);
        }

        snprintf(gpu_status, sizeof(gpu_status),
                 "  |  GPU: %s  |  VRAM: %s  |  ENC: %s",
                 gpu_text, vram_text, enc_text);
    }

    char status[768];
    snprintf(status, sizeof(status),
             " %zu processes  |  %ld CPUs  |  memory: %s / %s%s  |  "
             "render: %.1f ms (avg %.1f, max %.1f)",
             count, ncpus,
             mem_used_buf, mem_total_buf, gpu_status,
             ui_last, ui_avg, ui_max);
    gtk_label_set_text(ctx->status_label, status);
}

gboolean gtk_ui_on_refresh(gpointer data)
{
    ui_ctx_t *ctx = data;
    if (ctx->shutting_down) return G_SOURCE_REMOVE;

    pthread_mutex_lock(&ctx->mon->lock);
    int running = ctx->mon->running;
    size_t count = ctx->mon->snapshot.len;

    /* One-shot: log when we first see a non-empty snapshot */
    {
        static gboolean _logged = FALSE;
        if (!_logged && count > 0) {
            _logged = TRUE;
            extern struct timespec evemon_start_time;
            struct timespec _now;
            clock_gettime(CLOCK_MONOTONIC, &_now);
            double _e = (double)(_now.tv_sec  - evemon_start_time.tv_sec)
                      + (double)(_now.tv_nsec - evemon_start_time.tv_nsec) / 1e9;
            evemon_log(LOG_DEBUG, "[evemon] gtk_ui_on_refresh: first non-empty snapshot seen %.3f s after startup", _e);
        }
    }

    /* Feed the raw snapshot into the GTK-free process store.
     * proc_store_update() deep-copies all proc_entry_t data
     * (including steam_info_t) so we can release the monitor lock
     * immediately after. */
    {
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        int64_t now_us = (int64_t)_ts.tv_sec * 1000000LL
                       + _ts.tv_nsec / 1000;
        proc_store_update(&ctx->pstore,
                          &ctx->mon->snapshot,
                          now_us);
    }
    pthread_mutex_unlock(&ctx->mon->lock);

    /* GC stale expand/collapse state for PIDs that are no longer alive.
     * Prevents ptree_nodes from growing without bound over long sessions. */
    gtk_ui_ptree_nodes_gc(&ctx->ptree_nodes, &ctx->pstore);

    if (!running || ctx->shutting_down) {
        if (!running)
            gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    if (count == 0 && ctx->pstore.count == 0)
        return G_SOURCE_CONTINUE;

    /* Block collapse/expand and selection-changed signals during
     * programmatic changes.  Without blocking selection-changed,
     * remove_pinned_rows would destroy the selected pinned row and
     * GTK would auto-select a nearby row (e.g. init/kthreadd). */
    g_signal_handlers_block_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_block_by_func(ctx->view, on_row_expanded,  ctx);
    GtkTreeSelection *tree_sel = gtk_tree_view_get_selection(ctx->view);
    g_signal_handlers_block_by_func(tree_sel, gtk_ui_on_selection_changed, ctx);

    /*
     * Always remember the selected PID and its pinned-root so we can
     * re-select the correct copy (pinned vs original) after the store
     * update.  Pinned rows are destroyed and recreated every tick, so
     * we must restore selection even when follow_selection is off.
     */
    pid_t    sel_pid         = 0;
    pid_t    sel_pinned_root = PTREE_UNPINNED;
    gboolean have_sel        = FALSE;

    {
        GList *rows = gtk_tree_selection_get_selected_rows(tree_sel, NULL);
        if (rows) {
            GtkTreePath *sel_path = rows->data;
            /* sel_path is a sort-model path; sort → underlying store */
            GtkTreePath *store_path = gtk_tree_model_sort_convert_path_to_child_path(
                ctx->sort_model, sel_path);
            GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                ctx->sort_model);
            GtkTreeIter sel_iter;
            if (store_path &&
                gtk_tree_model_get_iter(child_model,
                                       &sel_iter, store_path)) {
                gint pid, pr;
                gtk_tree_model_get(child_model, &sel_iter,
                                   COL_PID, &pid,
                                   COL_PINNED_ROOT, &pr, -1);
                sel_pid         = (pid_t)pid;
                sel_pinned_root = (pid_t)pr;
                have_sel = TRUE;
            }
            if (store_path)
                gtk_tree_path_free(store_path);
            g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
        }
    }

    PROFILE_BEGIN(ui_render);

    if (ctx->initial_refresh) {
        /* First time: full populate + expand all */
        populate_store_initial(ctx->store, ctx->view, &ctx->pstore,
                               settings_get()->preselected_pid, ctx);
        gtk_ui_apply_gpu_snapshot_to_store(ctx);

        /* Switch from fast startup poll to the normal 1-second interval */
        if (ctx->initial_refresh) {
            ctx->initial_refresh = FALSE;
            g_timeout_add(1000, gtk_ui_on_refresh, ctx);

            /* Selection and detail panel are now handled inside
             * populate_store_initial as soon as the preselected row
             * is inserted.  Fall back to goto_pid only if the PID
             * wasn't found during the initial populate (e.g. it
             * appeared after the snapshot was taken). */
            {
                pid_t sel_pid = settings_get()->preselected_pid;
                GtkTreeSelection *psel = gtk_tree_view_get_selection(ctx->view);
                GList *prows = psel ? gtk_tree_selection_get_selected_rows(psel, NULL) : NULL;
                if (!prows && sel_pid > 0)
                    goto_pid(ctx, sel_pid);
                else if (prows)
                    g_list_free_full(prows, (GDestroyNotify)gtk_tree_path_free);
            }

            /* Honour the saved alternate view on first load.  The CPU view
             * is installed below, after group totals have been computed. */
            if (ctx->show_gpu_only && ctx->has_gpu_plugin)
                rebuild_gpu_filter_store(ctx);
            else if (ctx->show_audio_only && !ctx->show_cpu_intensive)
                rebuild_audio_filter_store(ctx);

            /* Kick the plugin broker immediately on first display so the
             * detail panel is populated without waiting for the first
             * 1-second tick.  We seed the selected PID ourselves here
             * because gtk_ui_on_selection_changed fires *after* goto_pid returns
             * but the broker gather is asynchronous — starting it now
             * lets it run in parallel while GTK renders the tree. */
            {
                plugin_registry_t *preg = ctx->plugin_registry;
                if (preg && preg->count > 0) {
                    pid_t sel_pid = settings_get()->preselected_pid;
                    for (size_t i = 0; i < preg->count; i++) {
                        plugin_instance_t *inst = &preg->instances[i];
                        /* SYSTEM plugins always track PID 1; never follow selection */
                        if (inst->plugin && inst->plugin->role == EVEMON_ROLE_SYSTEM) continue;
                        if (!inst->pinned && inst->is_active)
                            plugin_instance_set_pid(inst, sel_pid, 0);
                    }
                    broker_start(preg, ctx->mon ? ctx->mon->fdmon : NULL);
                }
            }

            /* Update status bar with first data before we drop this source */
            goto finish;
        }
    } else {
        /* Remove pinned copies before the incremental update so that
         * duplicate PIDs don't confuse the diff algorithm. */
        remove_pinned_rows(ctx->store);
        /* Incremental: update in-place, no clear, no flash */
        update_store(ctx->store, ctx->view, &ctx->pstore, ctx);
        gtk_ui_apply_gpu_snapshot_to_store(ctx);
    }

    /* Rebuild pinned subtrees (copies of pinned PIDs at the top) */
    rebuild_pinned_rows(ctx->store,
                        ctx->pinned_pids, ctx->pinned_count);

    /* Expand pinned subtrees according to stored collapse/expand state.
     * Pinned rows are rebuilt from scratch every tick, so they default
     * to collapsed in the view.  Walk top-level pinned rows in the
     * sort model and apply the per-pinned-root expand state. */
    if (ctx->pinned_count > 0) {
        GtkTreeModel *sort = GTK_TREE_MODEL(ctx->sort_model);
        GtkTreeIter sort_iter;
        gboolean valid = gtk_tree_model_get_iter_first(sort, &sort_iter);
        while (valid) {
            GtkTreeIter store_iter;
            gtk_tree_model_sort_convert_iter_to_child_iter(
                ctx->sort_model, &store_iter, &sort_iter);
            GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                ctx->sort_model);
            gint pr = 0;
            gtk_tree_model_get(child_model, &store_iter,
                               COL_PINNED_ROOT, &pr, -1);
            if (pr != (gint)PTREE_UNPINNED) {
                /* This is a pinned top-level row – expand it and
                 * its children according to stored state. */
                GtkTreePath *sort_path = gtk_tree_model_get_path(
                    sort, &sort_iter);
                if (sort_path) {
                    gint pid;
                    gtk_tree_model_get(child_model, &store_iter,
                                       COL_PID, &pid, -1);
                    pid_t pinned_root = (pid_t)pr;
                    if (get_process_tree_node(&ctx->ptree_nodes,
                                              pinned_root, (pid_t)pid)
                        != PTREE_COLLAPSED) {
                        gtk_tree_view_expand_row(ctx->view, sort_path,
                                                 FALSE);
                        /* Recurse: expand children respecting state */
                        expand_respecting_collapsed_recurse(
                            ctx, sort, &sort_iter);
                    }
                    gtk_tree_path_free(sort_path);
                }
            }
            valid = gtk_tree_model_iter_next(sort, &sort_iter);
        }
    }

    /* Recompute group totals (self + all descendants) */
    compute_group_rss(ctx->store, NULL);
    compute_group_cpu(ctx->store, NULL);

    /* Sync the filtered shadow store if a filter is active.
     * This updates data in-place, preserving view state (expand,
     * selection, scroll).  A full rebuild only happens if the set
     * of matching processes has structurally changed. */
    if (ctx->show_gpu_only)
        sync_gpu_filter_store(ctx);
    else if (ctx->show_cpu_intensive)
        sync_cpu_intensive_filter_store(ctx);
    else if (ctx->show_audio_only)
        sync_audio_filter_store(ctx);
    else if (ctx->filter_text[0] != '\0')
        sync_filter_store(ctx);

    /* Kick the highlight fade timer if any rows are newly born/dying */
    ensure_highlight_timer(ctx);


    PROFILE_END(ui_render);

    /*
     * If there was a selection, find the row by PID (stable across
     * re-sorts) and use GTK's scroll_to_cell to place it at the same
     * viewport fraction as before.  This avoids manual bin-window
     * coordinate arithmetic which is unreliable across re-sorts.
     */
    if (have_sel && sel_pid > 0) {
        /* Find the PID+pinned_root in whichever store the sort model
         * is wrapping.  This ensures that when a pinned copy was
         * selected we re-select the pinned copy, not the original
         * tree row (which has the same PID but a different
         * COL_PINNED_ROOT).  Fall back to a plain PID match if the
         * exact copy is gone (e.g. process exited). */
        GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
            ctx->sort_model);
        GtkTreeIter found_iter;
        if (gtk_ui_find_iter_by_pid_and_pinned_root(child_model, NULL,
                             sel_pid, sel_pinned_root, &found_iter) ||
            find_iter_by_pid(child_model, NULL,
                             sel_pid, &found_iter)) {
            GtkTreePath *child_path = gtk_tree_model_get_path(
                child_model, &found_iter);
            if (child_path) {
                GtkTreePath *sort_path =
                    gtk_tree_model_sort_convert_child_path_to_path(
                        ctx->sort_model, child_path);
                if (sort_path) {
                    /* Re-select (sort may have moved the iter) */
                    GtkTreeSelection *sel =
                        gtk_tree_view_get_selection(ctx->view);
                    gtk_tree_selection_select_path(sel, sort_path);
                    gtk_tree_path_free(sort_path);
                }
                gtk_tree_path_free(child_path);
            }
        }
    }

    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);
    g_signal_handlers_unblock_by_func(tree_sel, gtk_ui_on_selection_changed, ctx);

    /* Update the process detail panel for the selected process */
    proc_detail_update(ctx);
    evemon_log(LOG_DEBUG, "[DBG] proc_detail_update returned");

    /* Update pinned detail panels' header labels */
    pinned_panels_update(ctx);
    evemon_log(LOG_DEBUG, "[DBG] pinned_panels_update returned");

    /* ── Plugin broker dispatch ──────────────────────────────── */
    /* After the detail panel updates, kick off the plugin data broker.
     * Non-pinned instances follow the tree selection. */
    {
        plugin_registry_t *preg = ctx->plugin_registry;
        if (preg && preg->count > 0) {
            /* Determine the currently selected PID */
            pid_t sel_pid = 0;
            GtkTreeSelection *psel =
                gtk_tree_view_get_selection(ctx->view);
            if (psel) {
                GList *prows =
                    gtk_tree_selection_get_selected_rows(psel, NULL);
                if (prows) {
                    GtkTreePath *pp = prows->data;
                    GtkTreePath *cpath =
                        gtk_tree_model_sort_convert_path_to_child_path(
                            ctx->sort_model, pp);
                    GtkTreeModel *cmodel =
                        gtk_tree_model_sort_get_model(ctx->sort_model);
                    GtkTreeIter citer;
                    if (cpath && gtk_tree_model_get_iter(cmodel, &citer,
                                                         cpath)) {
                        gint v = 0;
                        gtk_tree_model_get(cmodel, &citer,
                                           COL_PID, &v, -1);
                        sel_pid = (pid_t)v;
                    }
                    if (cpath) gtk_tree_path_free(cpath);
                    g_list_free_full(prows,
                                     (GDestroyNotify)gtk_tree_path_free);
                }
            }

            /* Update follow-selection instances to the selected PID.
             * Skip pinned and floating-window instances — they track
             * a fixed PID and must not follow the tree selection.
             * Skip SYSTEM role plugins — they always track PID 1. */
            for (size_t i = 0; i < preg->count; i++) {
                plugin_instance_t *inst = &preg->instances[i];
                if (inst->plugin && inst->plugin->role == EVEMON_ROLE_SYSTEM) continue;
                if (!inst->pinned && inst->is_active)
                    plugin_instance_set_pid(inst, sel_pid, FALSE);
            }

            /* If nothing is selected, clear all plugins and
             * hide the main (unpinned) detail panel. */
            if (sel_pid <= 0) {
                plugin_dispatch_clear_all(preg);
                if (gtk_widget_get_visible(ctx->detail_panel))
                    gtk_widget_hide(ctx->detail_panel);
            }

            /* Start the broker gather.  When sel_pid <= 0 the broker
             * still runs with zero tracked PIDs so it can take a
             * PipeWire graph snapshot and deliver system-wide audio
             * PIDs for the tree-view audio icons. */
            evemon_log(LOG_DEBUG, "[DBG] broker_start: sel_pid=%d", (int)sel_pid);
            broker_start(preg, ctx->mon ? ctx->mon->fdmon : NULL);
            evemon_log(LOG_DEBUG, "[DBG] broker_start returned");
        }
    }

    evemon_log(LOG_DEBUG, "[DBG] gtk_ui_update_status_bar");
    gtk_ui_update_status_bar(ctx);
    evemon_log(LOG_DEBUG, "[DBG] gtk_ui_on_refresh done");

    return G_SOURCE_CONTINUE;

finish:
    /* Unblock signals that were blocked above */
    g_signal_handlers_unblock_by_func(ctx->view, on_row_collapsed, ctx);
    g_signal_handlers_unblock_by_func(ctx->view, on_row_expanded,  ctx);
    g_signal_handlers_unblock_by_func(tree_sel, gtk_ui_on_selection_changed, ctx);

    /* First refresh done – finish status update, then remove the fast timer */
    gtk_ui_update_status_bar(ctx);
    return G_SOURCE_REMOVE;
}

