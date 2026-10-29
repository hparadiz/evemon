#include "../ipc/query.h"
#include "ui_gtk_private.h"

/* Host helper: request eBPF monitor for a (pid,fd) pair */
int gtk_ui_host_monitor_fd_subscribe(void *host_ctx, pid_t pid, int fd)
{
    if (!host_ctx) return -1;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return -1;
    /* Ensure write probe is attached */
    if (fdmon_write_enable(ctx->mon->fdmon) != 0)
        return -1;
    return fdmon_add_pid_fd(ctx->mon->fdmon, pid, fd);
}

void gtk_ui_host_monitor_fd_unsubscribe(void *host_ctx, pid_t pid, int fd)
{
    if (!host_ctx) return;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return;
    fdmon_remove_pid_fd(ctx->mon->fdmon, pid, fd);
}

int gtk_ui_host_monitor_watch_children(void *host_ctx, pid_t pid, int fd_mask)
{
    if (!host_ctx) return -1;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return -1;
    /* Ensure the write probes and exec tracepoint are attached */
    if (fdmon_write_enable(ctx->mon->fdmon) != 0) return -1;
    return fdmon_watch_parent_fds(ctx->mon->fdmon, pid, fd_mask);
}

void gtk_ui_host_monitor_unwatch_children(void *host_ctx, pid_t pid)
{
    if (!host_ctx) return;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return;
    fdmon_unwatch_parent_fds(ctx->mon->fdmon, pid);
}

int gtk_ui_host_orphan_capture_enable(void *host_ctx)
{
    if (!host_ctx) return -1;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return -1;
    return fdmon_orphan_stdout_enable(ctx->mon->fdmon);
}

void gtk_ui_host_orphan_capture_disable(void *host_ctx)
{
    if (!host_ctx) return;
    ui_ctx_t *ctx = host_ctx;
    if (!ctx->mon || !ctx->mon->fdmon) return;
    fdmon_orphan_stdout_disable(ctx->mon->fdmon);
}


/* ── Broker completion: GTK main-thread bridge ───────────────── */
/*
 * Called from the broker worker thread when a gather cycle finishes.
 * Posts the cycle to the GTK main loop via g_idle_add so that
 * broker_dispatch_cycle() runs on the main thread — matching the
 * guarantee the old GTask completion callback provided.
 */
static gboolean on_broker_idle_dispatch(gpointer p)
{
    broker_cycle_t *cycle = p;
    if (!evemon_shutdown_requested && !evemon_restart_requested)
        broker_dispatch_cycle(cycle);
    broker_cycle_free(cycle);
    return G_SOURCE_REMOVE;
}

void gtk_ui_on_broker_complete_gtk(broker_cycle_t *cycle, void *user_data)
{
    (void)user_data;
    g_idle_add(on_broker_idle_dispatch, cycle);
}

/* Forward declaration — defined after gtk_ui_on_broker_complete_gtk. */
void gtk_ui_on_broker_audio_pids(const pid_t *pids, size_t count, void *data);

/* ── Async plugin loading ─────────────────────────────────────── */

/*
 * Tab ordering tables shared between gtk_ui_on_plugin_loaded_gtk() and the
 * old synchronous path (kept for reference).
 */
const char *g_tab_order[] = {
    "org.evemon.pipewire",
    "org.evemon.net",
    NULL
};
const char *g_tab_order_last[] = {
    "org.evemon.milkdrop",
    NULL
};
/* Heap-allocated context passed as user_data to the async scan. */


/*
 * post_fn adapter: wraps g_idle_add so plugin_loader_scan_async() can
 * schedule idle callbacks without depending on GTK headers itself.
 */
void gtk_ui_gtk_post_fn(int (*func)(void *), void *data)
{
    g_idle_add((GSourceFunc)func, data);
}

/*
 * Determine the correct notebook insertion position for a newly loaded
 * plugin, honoring the tab_order / tab_order_last priority scheme.
 *
 * Returns the 0-based page index to insert at (-1 = append at end).
 */
static int plugin_notebook_insert_pos(GtkNotebook     *nb,
                                      plugin_registry_t *preg,
                                      plugin_instance_t *new_inst)
{
    const char *new_id = new_inst->plugin ? new_inst->plugin->id : NULL;

    /* Determine the priority tier of the new plugin:
     *   0 = tab_order (front)
     *   1 = normal (middle)
     *   2 = tab_order_last (end, hidden)
     */
    int new_tier = 1;
    if (new_id) {
        for (int o = 0; g_tab_order[o]; o++)
            if (strcmp(new_id, g_tab_order[o]) == 0) { new_tier = 0; break; }
        if (new_tier == 1)
            for (int o = 0; g_tab_order_last[o]; o++)
                if (strcmp(new_id, g_tab_order_last[o]) == 0) { new_tier = 2; break; }
    }

    int npages = gtk_notebook_get_n_pages(nb);

    if (new_tier == 0) {
        /* Front-priority: insert before the first non-front-priority page,
         * but after any existing front-priority pages with lower tab_order
         * index. */
        int new_order_idx = INT_MAX;
        for (int o = 0; g_tab_order[o]; o++)
            if (new_id && strcmp(new_id, g_tab_order[o]) == 0)
                { new_order_idx = o; break; }

        for (int p = 0; p < npages; p++) {
            GtkWidget *child = gtk_notebook_get_nth_page(nb, p);
            /* Find which instance owns this page */
            for (size_t i = 0; i < preg->count; i++) {
                if (preg->instances[i].widget == child) {
                    const char *pid = preg->instances[i].plugin
                                    ? preg->instances[i].plugin->id : NULL;
                    /* Check if existing page is also front-priority */
                    int existing_order = INT_MAX;
                    for (int o = 0; g_tab_order[o]; o++)
                        if (pid && strcmp(pid, g_tab_order[o]) == 0)
                            { existing_order = o; break; }
                    if (existing_order == INT_MAX || existing_order > new_order_idx)
                        return p;  /* insert here */
                    break;
                }
            }
        }
        return -1;  /* append */
    }

    if (new_tier == 2) {
        /* Last-priority: always append at end */
        return -1;
    }

    /* Normal tier: insert before the first last-priority page */
    for (int p = 0; p < npages; p++) {
        GtkWidget *child = gtk_notebook_get_nth_page(nb, p);
        for (size_t i = 0; i < preg->count; i++) {
            if (preg->instances[i].widget == child) {
                const char *pid = preg->instances[i].plugin
                                ? preg->instances[i].plugin->id : NULL;
                for (int o = 0; g_tab_order_last[o]; o++)
                    if (pid && strcmp(pid, g_tab_order_last[o]) == 0)
                        return p;  /* insert before this last-tier page */
                break;
            }
        }
    }
    return -1;  /* append */
}

/*
 * Called on the GTK main thread for each plugin loaded by the async
 * scan.  Finishes the GTK-thread work: create_widget(), activate(),
 * append to notebook.
 */
void gtk_ui_on_plugin_loaded_gtk(plugin_registry_t *reg,
                                 int inst_id,
                                 void *user_data)
{
    plugin_async_ctx_t *ac = user_data;
    ui_ctx_t *ctx = ac->ctx;

    if (ctx->shutting_down)
        return;

    int idx = plugin_registry_find_by_id(reg, inst_id);
    if (idx < 0)
        return;

    plugin_instance_t *inst = &reg->instances[idx];
    if (!inst->plugin)
        return;

    /* Headless plugins: call activate() now that we're on the GTK main
     * thread (deferred from the async worker which must not touch GTK). */
    if (inst->plugin->role == EVEMON_ROLE_SERVICE) {
        if (inst->plugin->activate && reg->host_services) {
            inst->plugin->activate(inst->plugin->plugin_ctx,
                                   reg->host_services);
        }

        /* Flag audio plugin if it needs PipeWire data */
        if (inst->plugin->data_needs & evemon_NEED_PIPEWIRE) {
            if (!ctx->has_audio_plugin) {
                ctx->has_audio_plugin = TRUE;
                if (ctx->audio_only_menu_item)
                    gtk_widget_set_sensitive(
                        GTK_WIDGET(ctx->audio_only_menu_item), TRUE);
                broker_set_audio_callback(gtk_ui_on_broker_audio_pids, ctx);
                evemon_log(LOG_INFO, "evemon: audio plugin detected (%s)",
                           inst->plugin->id ? inst->plugin->id : "?");
            }
        }
        return;
    }

    /* UI plugin: create the widget on the GTK main thread.
     * The async worker only does dlopen/init; widget creation must
     * happen on the GTK thread. */
    if (!inst->plugin->create_widget) return;
    inst->widget = inst->plugin->create_widget(inst->plugin->plugin_ctx);
    if (!inst->widget) {
        evemon_log(LOG_ERROR, "evemon: plugin %s: create_widget returned NULL", inst->so_path);
        return;
    }
    /* Inject host services now that we're on the GTK main thread */
    if (inst->plugin->activate && reg->host_services)
        inst->plugin->activate(inst->plugin->plugin_ctx, reg->host_services);

    /* Determine the display label */
    const char *lbl = inst->plugin->name ? inst->plugin->name : "Plugin";

    /* SYSTEM role: add to the system plugin panel notebook */
    if (inst->plugin->role == EVEMON_ROLE_SYSTEM) {
        const char *system_tab_lbl = lbl;
        if (inst->plugin->id &&
            strcmp(inst->plugin->id, "org.evemon.system_libs") == 0)
            system_tab_lbl = "Library";

        /* SYSTEM plugins always track PID 1 — they are process-independent */
        plugin_instance_set_pid(inst, 1, FALSE);
        if (ctx->system_panel_notebook) {
            system_panel_add_plugin(ctx, inst->widget, system_tab_lbl, inst->instance_id);
            plugin_instance_set_active(inst, TRUE);
            gtk_widget_show_all(inst->widget);

            if (inst->plugin->id &&
                strcmp(inst->plugin->id, "org.evemon.gpu") == 0)
                gpu_integration_set_available(ctx, TRUE);

            /* Auto-show if setting says open */
            if (settings_get()->system_panel_open &&
                ctx->system_panel && !gtk_widget_get_visible(ctx->system_panel)) {
                gtk_check_menu_item_set_active(ctx->system_panel_menu_item, TRUE);
            }
        }
        evemon_log(LOG_INFO,
                   "evemon: loaded System plugin \"%s\" (%s) v%s — system panel tab added",
                   inst->plugin->name    ? inst->plugin->name    : "?",
                   inst->plugin->id      ? inst->plugin->id      : "?",
                   inst->plugin->version ? inst->plugin->version : "?");
        return;
    }

    /* Is this a last-order (hidden) plugin? */
    gboolean is_last = FALSE;
    if (inst->plugin->id)
        for (int o = 0; g_tab_order_last[o]; o++)
            if (strcmp(inst->plugin->id, g_tab_order_last[o]) == 0)
                { is_last = TRUE; break; }

    /* Add to notebook at the correct position */
    GtkWidget *notebook = ctx->plugin_notebook;
    GtkWidget *tab_label = gtk_label_new(lbl);

    int pos = plugin_notebook_insert_pos(GTK_NOTEBOOK(notebook), reg, inst);
    if (pos < 0)
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), inst->widget, tab_label);
    else
        gtk_notebook_insert_page(GTK_NOTEBOOK(notebook), inst->widget, tab_label, pos);

    plugin_instance_set_active(inst, TRUE);

    if (is_last) {
        /* Hidden until the plugin activates it */
        gtk_widget_set_no_show_all(inst->widget, TRUE);
        gtk_widget_hide(inst->widget);
        GtkWidget *tlbl = gtk_notebook_get_tab_label(
            GTK_NOTEBOOK(notebook), inst->widget);
        if (tlbl) {
            gtk_widget_set_no_show_all(tlbl, TRUE);
            gtk_widget_hide(tlbl);
        }
    } else {
        gtk_widget_show_all(inst->widget);
    }

    evemon_log(LOG_INFO, "evemon: loaded UI plugin \"%s\" (%s) v%s — tab added",
               inst->plugin->name    ? inst->plugin->name    : "?",
               inst->plugin->id      ? inst->plugin->id      : "?",
               inst->plugin->version ? inst->plugin->version : "?");
}

/*
 * Called on the GTK main thread once the async scan completes.
 */
void gtk_ui_on_plugin_scan_done_gtk(int n_loaded, void *user_data)
{
    plugin_async_ctx_t *ac = user_data;
    ui_ctx_t *ctx = ac->ctx;
    free(ac);

    if (ctx->shutting_down)
        return;

    evemon_log(LOG_INFO, "evemon: %d plugin(s) loaded (async)", n_loaded);

    /* Select the first visible tab now that sorting is complete */
    if (ctx->plugin_notebook) {
        GtkNotebook *nb = GTK_NOTEBOOK(ctx->plugin_notebook);
        int npages = gtk_notebook_get_n_pages(nb);
        for (int p = 0; p < npages; p++) {
            GtkWidget *child = gtk_notebook_get_nth_page(nb, p);
            if (child && gtk_widget_get_visible(child)) {
                gtk_notebook_set_current_page(nb, p);
                break;
            }
        }
    }

    /* Wire audio callback if any audio plugin was already detected above */
    if (ctx->has_audio_plugin)
        broker_set_audio_callback(gtk_ui_on_broker_audio_pids, ctx);

    /* Kick an initial broker cycle now that all plugins are ready */
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

/*
 * Audio PIDs callback — called on the GTK main thread (inside
 * broker_dispatch_cycle → on_broker_idle_dispatch → g_idle_add).
 * main thread.  This replaces the old approach of calling
 * pw_snapshot() directly on the main thread, which was unsafe
 * (raced with the broker's own pw_snapshot on the worker thread
 * via non-thread-safe pw_init / setenv calls → SIGSEGV).
 */
void gtk_ui_on_broker_audio_pids(const pid_t *pids, size_t count,
                                 void *data)
{
    ui_ctx_t *ctx = data;
    if (ctx->shutting_down)
        return;

    /* When the audio-only filter is active, collapse any PID that is
     * newly appearing in the audio set (i.e. not already tracked).
     * This gives new audio processes a collapsed-by-default initial
     * state so the filtered tree stays tidy. */
    if (ctx->show_audio_only) {
        for (size_t i = 0; i < count; i++) {
            gboolean already_known = FALSE;
            for (size_t j = 0; j < ctx->audio_pid_count; j++) {
                if (ctx->audio_pids[j] == pids[i]) {
                    already_known = TRUE;
                    break;
                }
            }
            if (!already_known) {
                /* Only collapse if there is no explicit state recorded yet */
                if (get_process_tree_node(&ctx->ptree_nodes,
                                          PTREE_UNPINNED, pids[i])
                        == PTREE_EXPANDED) {
                    set_process_tree_node(&ctx->ptree_nodes,
                                          PTREE_UNPINNED, pids[i],
                                          PTREE_COLLAPSED);
                }
            }
        }
    }

    /* Grow the audio_pids array if needed */
    if (count > ctx->audio_pid_cap) {
        size_t new_cap = count < 64 ? 64 : count;
        pid_t *tmp = realloc(ctx->audio_pids, new_cap * sizeof(pid_t));
        if (!tmp) { ctx->audio_pid_count = 0; return; }
        ctx->audio_pids   = tmp;
        ctx->audio_pid_cap = new_cap;
    }
    if (count) memcpy(ctx->audio_pids, pids, count * sizeof(pid_t));
    ctx->audio_pid_count = count;
}

/*
 * Resolve the inherited Steam game label for a row that has no own steam
 * label.  Walks up the tree model's parent chain and returns a pointer
 * to the first ancestor label found in `ctx->steam_map`, or NULL.
 * The returned pointer is owned by steam_map and valid until the next
 * steam_map mutation; the caller must NOT g_free() it.
 */
static const char *
find_ancestor_steam_label(GtkTreeModel *model, GtkTreeIter *iter,
                          ui_ctx_t *ctx)
{
    if (!ctx->steam_map) return NULL;

    GtkTreeIter parent;
    GtkTreeIter child = *iter;

    while (gtk_tree_model_iter_parent(model, &parent, &child)) {
        gint ppid = 0;
        gtk_tree_model_get(model, &parent, COL_PID, &ppid, -1);
        const char *lbl = steam_map_get(ctx->steam_map, (pid_t)ppid);
        if (lbl && lbl[0])
            return lbl;
        child = parent;
    }
    return NULL;
}

/*
 * Extract just the game name from a steam display_label.
 * Labels have the form:  "comm (Steam) · GameName [ProtonVer]"
 * or just               "comm (Steam) · GameName"
 * Returns a pointer into `label` past the "· ", or NULL if not found.
 * The game name ends at " [" or end-of-string; caller is responsible for
 * limiting the copy.
 */
static const char *steam_label_game_name(const char *label)
{
    if (!label) return NULL;
    /* Find " · " separator */
    const char *sep = strstr(label, " \xC2\xB7 ");  /* UTF-8 middle dot U+00B7 */
    if (!sep) return NULL;
    return sep + 4;  /* skip " · " (3 bytes for the middle dot + surrounding spaces) */
}

void gtk_ui_name_cell_data_func(GtkTreeViewColumn *col,
                                GtkCellRenderer   *cell,
                                GtkTreeModel      *model,
                                GtkTreeIter       *iter,
                                gpointer           data)
{
    ui_ctx_t *ctx = data;

    /* Apply highlight background (same logic as gtk_ui_highlight_cell_data_func) */
    gtk_ui_highlight_cell_data_func(col, cell, model, iter, NULL);

    gchar *name = NULL;
    gint pid = 0;
    gtk_tree_model_get(model, iter,
                       COL_NAME, &name,
                       COL_PID, &pid,
                       -1);

    /* Prefer Steam display label from the side-table */
    const char *steam_label = ctx->steam_map
        ? steam_map_get(ctx->steam_map, (pid_t)pid)
        : NULL;

    /*
     * If this row has no own steam label, check whether it lives inside
     * a Steam game subtree by walking up to the nearest labelled ancestor.
     * In that case we show:  "procname  [GameName]"
     */
    char inherited_buf[512];
    const char *display;
    if (steam_label && steam_label[0]) {
        display = steam_label;
    } else {
        const char *anc = find_ancestor_steam_label(model, iter, ctx);
        if (anc) {
            const char *game = steam_label_game_name(anc);
            if (game) {
                /* Trim at " [" (proton version suffix) if present */
                const char *end = strstr(game, " [");
                if (end) {
                    int glen = (int)(end - game);
                    snprintf(inherited_buf, sizeof(inherited_buf),
                             "%s  [%.*s]", name ? name : "", glen, game);
                } else {
                    snprintf(inherited_buf, sizeof(inherited_buf),
                             "%s  [%s]", name ? name : "", game);
                }
            } else {
                /* Label exists but has no "· GameName" part – show full label */
                snprintf(inherited_buf, sizeof(inherited_buf),
                         "%s  [%s]", name ? name : "", anc);
            }
            display = inherited_buf;
        } else {
            display = name;
        }
    }

    gboolean is_pinned = display && pid_is_pinned(ctx, (pid_t)pid);
    gboolean is_audio  = audio_pid_is_active(ctx, (pid_t)pid);

    if (is_pinned || is_audio) {
        char buf[512];
        const char *prefix = is_audio && is_pinned ? "\xF0\x9F\x94\x8A ➡ "
                           : is_audio              ? "\xF0\x9F\x94\x8A "
                           :                         "➡ ";
        snprintf(buf, sizeof(buf), "%s%s", prefix, display ? display : "");
        g_object_set(cell, "text", buf, NULL);
    } else {
        g_object_set(cell, "text", display ? display : "", NULL);
    }
    g_free(name);
}

/* ── Host service forwarding functions for PipeWire plugins ──── */
#ifdef HAVE_PIPEWIRE
/*
 * These bridge from the plugin API to the detail panel's pw_meter.c
 * and spectrogram.c implementations.  The host_ctx is a ui_ctx_t*.
 */
void gtk_ui_host_pw_meter_start(void *host_ctx,
                                const uint32_t *node_ids, size_t count)
{
    ui_ctx_t *c = host_ctx;
    pw_meter_start(c, node_ids, count);
}

void gtk_ui_host_pw_meter_stop(void *host_ctx)
{
    ui_ctx_t *c = host_ctx;
    pw_meter_stop(c);
}

void gtk_ui_host_pw_meter_read(void *host_ctx, uint32_t node_id,
                               int *level_l, int *level_r)
{
    ui_ctx_t *c = host_ctx;
    pw_meter_read(c, node_id, level_l, level_r);
}

void gtk_ui_host_pw_meter_remove_nodes(void *host_ctx,
                                       const uint32_t *node_ids,
                                       size_t count)
{
    ui_ctx_t *c = host_ctx;
    pw_meter_remove_nodes(c, node_ids, count);
}

void gtk_ui_host_spectro_start(void *host_ctx, GtkDrawingArea *draw_area,
                               uint32_t node_id)
{
    ui_ctx_t *c = host_ctx;
    spectrogram_start_for_node(c, draw_area, node_id);
}

void gtk_ui_host_spectro_stop(void *host_ctx, GtkDrawingArea *draw_area)
{
    ui_ctx_t *c = host_ctx;
    spectrogram_stop(c, draw_area);
}

uint32_t gtk_ui_host_spectro_get_target(void *host_ctx,
                                        GtkDrawingArea *draw_area)
{
    ui_ctx_t *c = host_ctx;
    return spectrogram_get_target_node(c, draw_area);
}

void gtk_ui_host_spectro_set_theme(void *host_ctx,
                                   GtkDrawingArea *draw_area,
                                   unsigned theme_index)
{
    ui_ctx_t *c = host_ctx;
    spectrogram_set_theme(c, draw_area, (spectro_theme_t)theme_index);
}

#endif /* HAVE_PIPEWIRE */

void gtk_ui_host_set_charting_theme(void *host_ctx, unsigned theme_index)
{
    ui_ctx_t *c = host_ctx;
    if (theme_index >= SPECTRO_THEME_COUNT) return;
    gtk_ui_apply_charting_theme(c, (spectro_theme_t)theme_index);
}

void gtk_ui_host_charting_theme_notify(void *host_ctx,
                                       void (*cb)(void *plugin_ctx,
                                                  unsigned theme_index),
                                       void *plugin_ctx)
{
    ui_ctx_t *c = host_ctx;
    /* Unregister: remove any existing entry for this plugin_ctx */
    for (int i = 0; i < c->charting_notify_count; i++) {
        if (c->charting_notify[i].plugin_ctx == plugin_ctx) {
            /* Shift remaining entries down */
            for (int j = i; j < c->charting_notify_count - 1; j++)
                c->charting_notify[j] = c->charting_notify[j + 1];
            c->charting_notify_count--;
            break;
        }
    }
    if (!cb) return;
    /* Register */
    if (c->charting_notify_count < CHARTING_NOTIFY_MAX) {
        c->charting_notify[c->charting_notify_count].cb         = cb;
        c->charting_notify[c->charting_notify_count].plugin_ctx = plugin_ctx;
        c->charting_notify_count++;
    }
}

/* Trampoline: open a plugin in a floating window (callable by plugins) */
void gtk_ui_host_open_plugin_window(void *host_ctx,
                                    const char *plugin_id,
                                    pid_t pid,
                                    const char *proc_name)
{
    ui_ctx_t *c = host_ctx;
    open_plugin_window(c, pid,
                       proc_name ? proc_name : "",
                       plugin_id);
}

