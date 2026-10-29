#include "../ipc/query.h"
#include "ui_gtk_private.h"

#include "ui_build.h"

void gtk_build_bind(ui_build_t *b)
{
    /* ── refresh timer ───────────────────────────────────────── */

    b->ctx.mon          = b->mon;
    b->ctx.store        = b->store;
    b->ctx.view         = GTK_TREE_VIEW(b->tree);
    proc_store_init(&b->ctx.pstore);
    b->ctx.scroll       = GTK_SCROLLED_WINDOW(b->scroll);
    b->ctx.status_label = GTK_LABEL(b->status);
    b->ctx.status_right = GTK_LABEL(b->status_right);
    b->ctx.menubar        = b->menubar;
    b->ctx.file_menu_item = b->file_item;
    g_signal_connect(b->vbox, "button-press-event",
                     G_CALLBACK(gtk_ui_on_main_client_button_press), &b->ctx);
    g_signal_connect(b->window, "event-after",
                     G_CALLBACK(gtk_ui_on_main_client_event_after), &b->ctx);
    b->ctx.tree           = b->tree;
    b->ctx.alt_pressed    = FALSE;
    b->ctx.css          = b->css;
    b->ctx.sidebar_css  = b->sidebar_css;
    b->ctx.font_size    = settings_get()->font_size;
    b->ctx.ptree_nodes     = (ptree_node_set_t){ NULL, NULL, NULL, 0, 0 };
    b->ctx.collapsed_pids  = (collapsed_pid_set_t){ NULL, 0, 0 };
    b->ctx.follow_selection = FALSE;

    /* Process icon cache — one-time desktop index scan */
    b->ctx.icon_ctx       = proc_icon_ctx_new(b->ctx.font_size + 4);
    b->ctx.icon_ctx_large = proc_icon_ctx_new(128);

    /* Steam display label side-table */
    b->ctx.steam_map = steam_map_create();

    /* Name filter */
    b->ctx.filter_store = NULL;
    b->ctx.sort_model   = GTK_TREE_MODEL_SORT(b->sort_model);
    b->ctx.filter_entry = b->name_filter_entry;
    b->ctx.filter_css   = b->filt_css;
    b->ctx.name_col     = b->name_col;
    b->ctx.filter_text[0] = '\0';
    b->ctx.show_audio_only = settings_get()->show_audio_only;
    b->ctx.show_cpu_intensive = settings_get()->show_cpu_intensive;
    b->ctx.show_gpu_only = FALSE;  /* restored when the GPU plugin is available */
    for (int i = 0; i < 3; i++) {
        b->ctx.gpu_columns[i] = b->gpu_process_cols[i];
        b->ctx.gpu_column_menu_items[i] = b->gpu_column_menu_items[i];
    }

    /* Go-to-PID */
    b->ctx.pid_entry = b->pid_entry;
    b->ctx.pid_css   = b->pid_entry_css;
    b->ctx.pid_col   = b->pid_col;

    /* Process detail panel */
    b->ctx.sidebar            = b->sidebar_frame;
    b->ctx.sidebar_grid       = b->sidebar_vbox;

    /* Collapsible section state */
    b->ctx.sb_info_collapsed     = FALSE;
    b->ctx.sb_info_content       = b->info_content_box;
    b->ctx.sb_info_header_arrow  = b->info_arrow;

    b->ctx.sb_pid        = b->sb_pid;
    b->ctx.sb_ppid       = b->sb_ppid;
    b->ctx.sb_user       = b->sb_user;
    b->ctx.sb_name       = b->sb_name;
    b->ctx.sb_cpu        = b->sb_cpu;
    b->ctx.sb_rss        = b->sb_rss;
    b->ctx.sb_group_rss  = b->sb_group_rss;
    b->ctx.sb_group_cpu  = b->sb_group_cpu;
    b->ctx.sb_io_read    = b->sb_io_read;
    b->ctx.sb_io_write   = b->sb_io_write;
    b->ctx.sb_net_send   = b->sb_net_send;
    b->ctx.sb_net_recv   = b->sb_net_recv;
    b->ctx.sb_start_time = b->sb_start_time;
    b->ctx.sb_container  = b->sb_container;
    b->ctx.sb_service    = b->sb_service;
    b->ctx.sb_cwd        = b->sb_cwd;
    b->ctx.sb_cmdline    = b->sb_cmdline;

    /* Steam/Proton detail */
    b->ctx.sb_steam_game    = b->sb_steam_game;
    b->ctx.sb_steam_appid   = b->sb_steam_appid;
    b->ctx.sb_steam_proton  = b->sb_steam_proton;
    b->ctx.sb_steam_runtime = b->sb_steam_runtime;
    b->ctx.sb_steam_compat  = b->sb_steam_compat;
    b->ctx.sb_steam_gamedir = b->sb_steam_gamedir;
    b->ctx.sb_steam_frame   = b->steam_box;     /* show/hide entire Steam section */

    /* cgroup resource limits */
    b->ctx.sb_cgroup_path         = b->sb_cgroup_path;
    b->ctx.sb_cgroup_mem          = b->sb_cgroup_mem;
    b->ctx.sb_cgroup_mem_high     = b->sb_cgroup_mem_high;
    b->ctx.sb_cgroup_mem_high_key = b->sb_cgroup_mem_high_key;
    b->ctx.sb_cgroup_swap         = b->sb_cgroup_swap;
    b->ctx.sb_cgroup_swap_key     = b->sb_cgroup_swap_key;
    b->ctx.sb_cgroup_cpu          = b->sb_cgroup_cpu;
    b->ctx.sb_cgroup_pids         = b->sb_cgroup_pids;
    b->ctx.sb_cgroup_io           = b->sb_cgroup_io;
    b->ctx.sb_cgroup_io_key       = b->sb_cgroup_io_key;
    b->ctx.sb_cgroup_frame        = b->cgroup_box;

    /* Process metadata (proc_meta service plugin) */
    b->ctx.sb_meta_organization = b->sb_meta_organization;
    b->ctx.sb_meta_homepage     = b->sb_meta_homepage;
    b->ctx.sb_meta_source_url   = b->sb_meta_source_url;
    b->ctx.sb_meta_funding_url  = b->sb_meta_funding_url;
    b->ctx.sb_meta_license      = b->sb_meta_license;
    b->ctx.sb_meta_summary      = b->sb_meta_summary;
    b->ctx.sb_meta_homepage_button    = b->sb_meta_homepage_button;
    b->ctx.sb_meta_source_url_button  = b->sb_meta_source_url_button;
    b->ctx.sb_meta_funding_url_button = b->sb_meta_funding_url_button;
    b->ctx.sb_meta_frame        = b->meta_box;

#ifdef HAVE_PIPEWIRE
    /* Meter and spectrogram state are still used by host services */
    b->ctx.pw_meter       = NULL;
    b->ctx.pw_meter_timer = 0;
    b->ctx.spectro_count  = 0;
    memset(b->ctx.spectro_instances, 0, sizeof(b->ctx.spectro_instances));
#endif

    /* Pinned processes */
    b->ctx.pinned_pids     = NULL;
    b->ctx.pinned_count    = 0;
    b->ctx.pinned_capacity = 0;

    /* Pinned detail panels */
    b->ctx.pinned_panels      = NULL;
    b->ctx.pinned_panel_count = 0;
    b->ctx.pinned_panel_cap   = 0;
    b->ctx.pinned_box         = b->pinned_box;

    /* ── Plugin system ───────────────────────────────────────── */
    {
        /* Allocate the registry on the heap so we have a stable pointer */
        plugin_registry_t *preg = calloc(1, sizeof(plugin_registry_t));
        if (preg) {
            plugin_registry_init(preg);

            /* Subscribe to proc_meta events to populate the Software section */
            evemon_event_bus_subscribe(EVEMON_EVENT_PROC_META,
                                       gtk_ui_on_proc_meta, &b->ctx);

            /* Build host services table on the heap so it outlives this block */
            evemon_host_services_t *hsvc = calloc(1, sizeof(evemon_host_services_t));
            if (hsvc) {
                hsvc->host_ctx          = &b->ctx;
#ifdef HAVE_PIPEWIRE
                hsvc->pw_meter_start    = gtk_ui_host_pw_meter_start;
                hsvc->pw_meter_stop     = gtk_ui_host_pw_meter_stop;
                hsvc->pw_meter_read     = gtk_ui_host_pw_meter_read;
                hsvc->pw_meter_remove_nodes = gtk_ui_host_pw_meter_remove_nodes;
                hsvc->spectro_start     = gtk_ui_host_spectro_start;
                hsvc->spectro_stop      = gtk_ui_host_spectro_stop;
                hsvc->spectro_get_target = gtk_ui_host_spectro_get_target;
                hsvc->spectro_set_theme  = gtk_ui_host_spectro_set_theme;
#endif
                hsvc->set_charting_theme     = gtk_ui_host_set_charting_theme;
                hsvc->charting_theme_notify  = gtk_ui_host_charting_theme_notify;
                /* Event bus wiring */
                hsvc->subscribe         = host_event_subscribe;
                hsvc->publish           = host_event_publish;
                hsvc->unsubscribe       = host_event_unsubscribe;
                /* eBPF fd monitoring helpers */
                hsvc->monitor_fd_subscribe      = gtk_ui_host_monitor_fd_subscribe;
                hsvc->monitor_fd_unsubscribe    = gtk_ui_host_monitor_fd_unsubscribe;
                hsvc->monitor_watch_children    = gtk_ui_host_monitor_watch_children;
                hsvc->monitor_unwatch_children  = gtk_ui_host_monitor_unwatch_children;
                /* Orphan-capture (cron/daemon) mode */
                hsvc->orphan_capture_enable  = gtk_ui_host_orphan_capture_enable;
                hsvc->orphan_capture_disable = gtk_ui_host_orphan_capture_disable;
                hsvc->open_plugin_window     = gtk_ui_host_open_plugin_window;
                plugin_registry_set_host_services(preg, hsvc);
            }

            /* Resolve plugins directory.  Search order:
             *  1. <exe_dir>/plugins/    — for development (build/plugins/)
             *  2. EVEMON_LIBDIR/plugins — installed system path
             *  3. build/plugins          — last-ditch fallback              */
            char exe_path[4096], plugin_dir[4096];
            plugin_dir[0] = '\0';
            ssize_t exe_len = readlink("/proc/self/exe", exe_path,
                                        sizeof(exe_path) - 1);
            if (exe_len > 0) {
                exe_path[exe_len] = '\0';
                char *slash = strrchr(exe_path, '/');
                if (slash) *slash = '\0';
                snprintf(plugin_dir, sizeof(plugin_dir),
                         "%s/plugins", exe_path);
            }

            /* If the exe-relative dir doesn't exist (installed layout),
             * fall back to the compiled-in LIBDIR.                        */
            if (plugin_dir[0] == '\0' ||
                access(plugin_dir, F_OK) != 0) {
#ifdef EVEMON_LIBDIR
                snprintf(plugin_dir, sizeof(plugin_dir),
                         "%s/plugins", EVEMON_LIBDIR);
#else
                snprintf(plugin_dir, sizeof(plugin_dir), "build/plugins");
#endif
            }

            /* Remember the plugin directory so the Plugins menu can
             * scan for new .so files at open time. */
            snprintf(b->ctx.plugin_dir, sizeof(b->ctx.plugin_dir),
                     "%s", plugin_dir);

            /* Create the notebook immediately so the detail panel is
             * shown at once — plugins stream in as they load. */
            GtkWidget *notebook = gtk_notebook_new();
            gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
            gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);

            /* The plugin notebook lives in the detail panel alongside
             * the process info.  Pack it now so it appears immediately. */
            b->ctx.plugin_registry = preg;
            b->ctx.plugin_notebook = notebook;
            b->ctx.has_audio_plugin = FALSE;

            /* Wire up the Plugins menu now that ctx is ready. */
            g_signal_connect(b->plugins_menu, "show",
                             G_CALLBACK(on_plugins_menu_map), &b->ctx);
            b->ctx.plugins_menu_item = b->plugins_item;

            /* Live CSS provider for plugin notebook font size */
            gtk_style_context_add_class(
                gtk_widget_get_style_context(notebook), "evemon-plugins");
            b->ctx.plugin_css = gtk_css_provider_new();
            {
                char pbuf[256];
                snprintf(pbuf, sizeof(pbuf),
                         ".evemon-plugins tab,"
                         ".evemon-plugins tab label,"
                         ".evemon-plugins treeview,"
                         ".evemon-plugins label,"
                         ".evemon-plugins checkbutton { font-size: %dpt; }",
                         settings_get()->font_size);
                gtk_css_provider_load_from_data(b->ctx.plugin_css, pbuf, -1, NULL);
            }
            gtk_style_context_add_provider_for_screen(
                gdk_screen_get_default(),
                GTK_STYLE_PROVIDER(b->ctx.plugin_css),
                GTK_STYLE_PROVIDER_PRIORITY_USER);

            /* Start the async plugin scan.  Each plugin's create_widget()
             * and notebook insertion happen on the GTK main thread as the
             * background thread loads .so files, so the detail panel is
             * never blocked.  gtk_ui_on_plugin_scan_done_gtk kicks the first
             * broker cycle once all plugins are registered.
             *
             * --safe-mode skips this entire block so no plugins are loaded. */
            if (evemon_safe_mode) {
                evemon_log(LOG_INFO, "evemon: safe mode — plugin loading skipped");
            } else
            {
            plugin_async_ctx_t *ac = malloc(sizeof(*ac));
            if (ac) {
                ac->ctx = &b->ctx;
                if (plugin_loader_scan_async(preg, plugin_dir,
                                             gtk_ui_gtk_post_fn,
                                             gtk_ui_on_plugin_loaded_gtk,
                                             gtk_ui_on_plugin_scan_done_gtk,
                                             ac) != 0) {
                    /* Async scan failed — fall back to synchronous load.
                     * plugin_loader_scan() calls create_widget() and
                     * activate() itself, so all we need to do here is
                     * add the widgets to the notebook. */
                    free(ac);
                    int nloaded = plugin_loader_scan(preg, plugin_dir);
                    evemon_log(LOG_INFO, "evemon: %d plugin(s) loaded (sync fallback) from %s", nloaded, plugin_dir);

                    /* Detect audio plugins */
                    for (size_t i = 0; i < preg->count; i++) {
                        if (preg->instances[i].plugin &&
                            (preg->instances[i].plugin->data_needs &
                             evemon_NEED_PIPEWIRE))
                            b->ctx.has_audio_plugin = TRUE;
                    }
                    if (b->ctx.has_audio_plugin)
                        broker_set_audio_callback(gtk_ui_on_broker_audio_pids, &b->ctx);

                    /* Add UI plugin widgets to the notebook in tab order */
                    for (int o = 0; g_tab_order[o]; o++) {
                        for (size_t i = 0; i < preg->count; i++) {
                            plugin_instance_t *inst = &preg->instances[i];
                            if (!inst->widget || !inst->plugin ||
                                !inst->plugin->id) continue;
                            if (inst->plugin->role == EVEMON_ROLE_SERVICE)
                                continue;
                            if (!PLUGIN_IS_AVAILABLE(inst)) continue;
                            if (strcmp(inst->plugin->id, g_tab_order[o]) != 0)
                                continue;
                            const char *lbl = inst->plugin->name
                                            ? inst->plugin->name : "Plugin";
                            gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                                inst->widget, gtk_label_new(lbl));
                            plugin_instance_set_active(inst, TRUE);
                        }
                    }
                    for (size_t i = 0; i < preg->count; i++) {
                        plugin_instance_t *inst = &preg->instances[i];
                        if (!inst->widget || !inst->plugin) continue;
                        if (inst->plugin->role == EVEMON_ROLE_SERVICE)
                            continue;
                        if (!PLUGIN_IS_AVAILABLE(inst)) continue;
                        if (inst->is_active) continue;
                        if (inst_is_last_order(inst->plugin, g_tab_order_last))
                            continue;
                        const char *lbl = inst->plugin->name
                                        ? inst->plugin->name : "Plugin";
                        gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                            inst->widget, gtk_label_new(lbl));
                        plugin_instance_set_active(inst, TRUE);
                    }
                    for (int o = 0; g_tab_order_last[o]; o++) {
                        for (size_t i = 0; i < preg->count; i++) {
                            plugin_instance_t *inst = &preg->instances[i];
                            if (!inst->widget || !inst->plugin ||
                                !inst->plugin->id) continue;
                            if (inst->plugin->role == EVEMON_ROLE_SERVICE)
                                continue;
                            if (!PLUGIN_IS_AVAILABLE(inst)) continue;
                            if (inst->is_active) continue;
                            if (strcmp(inst->plugin->id,
                                       g_tab_order_last[o]) != 0) continue;
                            const char *lbl = inst->plugin->name
                                            ? inst->plugin->name : "Plugin";
                            gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                                inst->widget, gtk_label_new(lbl));
                            plugin_instance_set_active(inst, TRUE);
                            gtk_widget_set_no_show_all(inst->widget, TRUE);
                            gtk_widget_hide(inst->widget);
                            GtkWidget *tlbl = gtk_notebook_get_tab_label(
                                GTK_NOTEBOOK(notebook), inst->widget);
                            if (tlbl) {
                                gtk_widget_set_no_show_all(tlbl, TRUE);
                                gtk_widget_hide(tlbl);
                            }
                        }
                    }
                }
            }
            } /* end else(!evemon_safe_mode) */
        } else {
            b->ctx.plugin_registry = NULL;
            b->ctx.plugin_notebook = NULL;
        }
        b->ctx.plugin_broker = NULL;  /* broker is stateless (module-level) */

        /* Wire the broker completion hook: the pthread worker posts the
         * finished cycle via g_idle_add so dispatch runs on the GTK
         * main thread — same guarantee the old GTask gave us. */
        broker_set_complete_callback(gtk_ui_on_broker_complete_gtk, NULL);

        /* Audio callback is wired per-plugin in gtk_ui_on_plugin_loaded_gtk /
         * gtk_ui_on_plugin_scan_done_gtk, so no registration needed here. */
    }

#ifdef HAVE_PIPEWIRE
    broker_set_pipewire_gatherer(pw_broker_gather);
    /* Start the persistent PipeWire watcher so pw_snapshot() is a fast
     * mutex-copy instead of a full connect-enumerate-disconnect cycle.
     * Start it after plugins are loaded so the watcher is ready by the
     * time the first broker cycle runs. */
    if (!query_is_foreign_host()) pw_watcher_start();
#endif

    /* ── Finish detail panel setup: put notebook into the hpaned ── */
    b->ctx.detail_panel          = b->detail_panel;
    b->ctx.detail_vbox           = b->detail_vbox;
    b->ctx.detail_panel_pos      = (panel_position_t)settings_get()->detail_panel_position;
    b->ctx.detail_panel_menu_item = GTK_CHECK_MENU_ITEM(b->detail_panel_toggle);
    b->ctx.audio_only_menu_item   = GTK_CHECK_MENU_ITEM(b->audio_only_item);
    g_signal_connect(b->audio_only_item, "toggled",
                     G_CALLBACK(gtk_ui_on_toggle_audio_only), &b->ctx);
    gtk_widget_set_sensitive(b->audio_only_item, b->ctx.has_audio_plugin);
    b->ctx.cpu_intensive_menu_item = GTK_CHECK_MENU_ITEM(b->cpu_intensive_item);
    g_signal_connect(b->cpu_intensive_item, "toggled",
                     G_CALLBACK(gtk_ui_on_toggle_cpu_intensive), &b->ctx);
    b->ctx.gpu_only_menu_item = GTK_CHECK_MENU_ITEM(b->gpu_only_item);
    g_signal_connect(b->gpu_only_item, "toggled",
                     G_CALLBACK(gtk_ui_on_toggle_gpu_only), &b->ctx);
    b->ctx.detail_paned          = b->outer_paned;
    b->ctx.content_box           = b->vbox;
    b->ctx.hpaned                = b->hpaned;
    b->ctx.panel_pos_group       = b->pos_group;

    /* ── System plugin panel setup ───────────────────────────── */
    b->ctx.system_panel_pos         = (panel_position_t)settings_get()->system_panel_position;
    b->ctx.system_panel_menu_item   = GTK_CHECK_MENU_ITEM(b->system_panel_toggle);
    b->ctx.system_panel_pos_group         = b->system_panel_pos_group;
    b->ctx.system_panel_has_plugins = FALSE;
    b->ctx.system_panel_paned             = NULL;  /* created lazily in system_panel_relayout */

    /* Build the system panel widget (frame + notebook, no content yet) */
    system_panel_build(&b->ctx);

    /* Set the correct radio button for sys panel position from settings */
    {
        GtkWidget *system_panel_pos_items[4] = {
            b->system_panel_pos_bottom, b->system_panel_pos_top, b->system_panel_pos_left, b->system_panel_pos_right
        };
        int saved_system_panel_pos = settings_get()->system_panel_position;
        if (saved_system_panel_pos >= 0 && saved_system_panel_pos <= 3) {
            /* Block signals to avoid triggering relayout before ctx is ready */
            for (int _i = 0; _i < 4; _i++)
                g_signal_handlers_block_by_func(system_panel_pos_items[_i],
                    G_CALLBACK(system_panel_relayout), &b->ctx);
            gtk_check_menu_item_set_active(
                GTK_CHECK_MENU_ITEM(system_panel_pos_items[saved_system_panel_pos]), TRUE);
            for (int _i = 0; _i < 4; _i++)
                g_signal_handlers_unblock_by_func(system_panel_pos_items[_i],
                    G_CALLBACK(system_panel_relayout), &b->ctx);
        }
    }

    /* Collapsible process info tray */
    b->ctx.proc_info_revealer  = b->proc_info_revealer;
    b->ctx.proc_info_toggle    = b->proc_info_toggle;
    b->ctx.proc_info_summary   = GTK_LABEL(b->proc_info_summary_w);
    b->ctx.proc_info_collapsed = !settings_get()->proc_info_open;

    /* Icon watermark overlay */
    b->ctx.detail_icon_da = b->detail_icon_da;
    b->ctx.detail_icon_pb = NULL;
    g_signal_connect(b->detail_icon_da, "draw",
                     G_CALLBACK(gtk_ui_on_detail_icon_draw), &b->ctx);

    g_signal_connect(b->proc_info_toggle, "clicked",
                     G_CALLBACK(gtk_ui_on_proc_info_tray_toggle), &b->ctx);

    /* Pack the plugin notebook into the detail panel's horizontal paned.
     * The notebook is created immediately (possibly empty) so the panel
     * is shown at once; tabs appear as plugins load in the background. */
    if (b->ctx.plugin_notebook) {
        gtk_paned_pack2(GTK_PANED(b->detail_hpaned), b->ctx.plugin_notebook,
                        TRUE, FALSE);
    }

    /* Set up the Name column cell data function so pinned processes
     * get the arrow prefix.  We use name_text_r directly — the pixbuf
     * renderer was packed first so renderers->data would be wrong. */
    gtk_tree_view_column_set_cell_data_func(
        b->name_col, b->name_text_r, gtk_ui_name_cell_data_func, &b->ctx, NULL);

    /* Font menu callbacks (need ctx address, so connect after ctx init) */
    g_signal_connect(b->font_inc,   "activate", G_CALLBACK(gtk_ui_on_font_increase),    &b->ctx);
    g_signal_connect(b->font_dec,   "activate", G_CALLBACK(gtk_ui_on_font_decrease),    &b->ctx);
    g_signal_connect(b->font_reset, "activate", G_CALLBACK(gtk_ui_on_font_reset),       &b->ctx);

    /* Charting Theme submenu — built here so ctx pointer is valid */
    {
        GSList *ct_group = NULL;
        GtkWidget *ct_menu = gtk_ui_build_charting_theme_submenu(&b->ctx, &ct_group);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(b->charting_theme_item), ct_menu);
        for (int i = 0; i < SPECTRO_THEME_COUNT; i++) {
            charting_theme_data[i] = (charting_theme_data_t){ &b->ctx, (spectro_theme_t)i };
            if (b->ctx.charting_theme_items[i])
                g_signal_connect(b->ctx.charting_theme_items[i], "toggled",
                                 G_CALLBACK(gtk_ui_on_charting_theme_selected),
                                 &charting_theme_data[i]);
        }
        gtk_widget_show_all(ct_menu);
    }
    g_signal_connect(b->filter_item,   "activate", G_CALLBACK(gtk_ui_on_menu_filter),   &b->ctx);
    g_signal_connect(b->goto_pid_item, "activate", G_CALLBACK(gtk_ui_on_menu_goto_pid), &b->ctx);
    g_signal_connect(b->detail_panel_toggle, "toggled",
                     G_CALLBACK(gtk_ui_on_toggle_detail_panel), &b->ctx);

    /* Detail panel position radio items */
    b->pos_data_bottom = (panel_pos_data_t){ &b->ctx, PANEL_POS_BOTTOM, FALSE };
    b->pos_data_top    = (panel_pos_data_t){ &b->ctx, PANEL_POS_TOP,    FALSE };
    b->pos_data_left   = (panel_pos_data_t){ &b->ctx, PANEL_POS_LEFT,   FALSE };
    b->pos_data_right  = (panel_pos_data_t){ &b->ctx, PANEL_POS_RIGHT,  FALSE };
    g_signal_connect(b->pos_bottom, "toggled",
                     G_CALLBACK(gtk_ui_on_panel_position_changed), &b->pos_data_bottom);
    g_signal_connect(b->pos_top, "toggled",
                     G_CALLBACK(gtk_ui_on_panel_position_changed), &b->pos_data_top);
    g_signal_connect(b->pos_left, "toggled",
                     G_CALLBACK(gtk_ui_on_panel_position_changed), &b->pos_data_left);
    g_signal_connect(b->pos_right, "toggled",
                     G_CALLBACK(gtk_ui_on_panel_position_changed), &b->pos_data_right);

    /* System panel toggle and position radio items */
    g_signal_connect(b->system_panel_toggle, "toggled",
                     G_CALLBACK(on_toggle_system_panel), &b->ctx);

    b->system_panel_pos_data_bottom = (panel_pos_data_t){ &b->ctx, PANEL_POS_BOTTOM, TRUE };
    b->system_panel_pos_data_top    = (panel_pos_data_t){ &b->ctx, PANEL_POS_TOP,    TRUE };
    b->system_panel_pos_data_left   = (panel_pos_data_t){ &b->ctx, PANEL_POS_LEFT,   TRUE };
    b->system_panel_pos_data_right  = (panel_pos_data_t){ &b->ctx, PANEL_POS_RIGHT,  TRUE };
    g_signal_connect(b->system_panel_pos_bottom, "toggled",
                     G_CALLBACK(gtk_ui_on_panel_position_changed), &b->system_panel_pos_data_bottom);
    g_signal_connect(b->system_panel_pos_top, "toggled",
                     G_CALLBACK(gtk_ui_on_panel_position_changed), &b->system_panel_pos_data_top);
    g_signal_connect(b->system_panel_pos_left, "toggled",
                     G_CALLBACK(gtk_ui_on_panel_position_changed), &b->system_panel_pos_data_left);
    g_signal_connect(b->system_panel_pos_right, "toggled",
                     G_CALLBACK(gtk_ui_on_panel_position_changed), &b->system_panel_pos_data_right);

    g_signal_connect(b->name_filter_entry, "key-release-event",
                     G_CALLBACK(on_filter_entry_key_release), &b->ctx);
    g_signal_connect(b->pid_entry, "key-release-event",
                     G_CALLBACK(on_pid_entry_key_release), &b->ctx);
    g_signal_connect(b->pid_entry, "insert-text",
                     G_CALLBACK(on_pid_entry_insert_text), NULL);
    g_signal_connect(b->tree_overlay, "get-child-position",
                     G_CALLBACK(on_overlay_get_child_position), &b->ctx);

    /* Right-click on status bar to toggle menu bar */
    g_signal_connect(b->status_ebox, "button-press-event",
                     G_CALLBACK(gtk_ui_on_status_button_press), &b->ctx);

    /* Middle-click drag-to-scroll */
    gtk_widget_add_events(b->tree, GDK_BUTTON_PRESS_MASK
                              | GDK_BUTTON_RELEASE_MASK
                              | GDK_POINTER_MOTION_MASK);
    g_signal_connect(b->tree,   "button-press-event",   G_CALLBACK(on_button_press),   &b->ctx);
    g_signal_connect(b->tree,   "button-release-event", G_CALLBACK(on_button_release), &b->ctx);
    g_signal_connect(b->tree,   "motion-notify-event",  G_CALLBACK(on_motion_notify),  &b->ctx);
    g_signal_connect(b->window, "focus-out-event",      G_CALLBACK(on_focus_out),      &b->ctx);

    /* Global keyboard shortcuts (Ctrl+Plus / Ctrl+Minus / Ctrl+0 / Ctrl+Q) */
    g_signal_connect(b->window, "key-press-event",   G_CALLBACK(on_key_press),   &b->ctx);
    g_signal_connect(b->window, "key-release-event", G_CALLBACK(on_key_release), &b->ctx);

    /* Double-click a row to open the detail panel */
    g_signal_connect(b->tree, "row-activated", G_CALLBACK(gtk_ui_on_row_activated), &b->ctx);

    /* Track user collapse / expand actions */
    g_signal_connect(b->tree, "row-collapsed", G_CALLBACK(on_row_collapsed), &b->ctx);
    g_signal_connect(b->tree, "row-expanded",  G_CALLBACK(on_row_expanded),  &b->ctx);

    /* Update sidebar immediately when selection changes (arrow keys, click) */
    b->sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(b->tree));
    g_signal_connect(b->sel, "changed", G_CALLBACK(gtk_ui_on_selection_changed), &b->ctx);

    /* Enable follow-selection when user clicks a sort column */
    g_signal_connect(b->sort_model, "sort-column-changed",
                     G_CALLBACK(on_sort_column_changed), &b->ctx);

    /* Disable follow-selection when user scrolls manually */
    gtk_widget_add_events(b->tree, GDK_SCROLL_MASK);
    g_signal_connect(b->tree, "scroll-event",
                     G_CALLBACK(gtk_ui_on_tree_scroll_event), &b->ctx);

    b->ctx.initial_refresh = TRUE;
    b->ctx.highlight_timer = 0;

    g_signal_connect(b->window, "destroy", G_CALLBACK(gtk_ui_on_destroy), &b->ctx);
    g_timeout_add(20, gtk_ui_on_refresh, &b->ctx);

    /* ── Apply theme from settings ────────────────────────────── */
    {
        const evemon_settings_t *s = settings_get();
        if (s->theme[0]) {
            GtkSettings *gtk_s = gtk_settings_get_default();
            g_object_set(gtk_s, "gtk-theme-name", s->theme, NULL);
        }
    }

    /* ── Apply detail panel position layout before showing ────── */
    if (b->ctx.detail_panel_pos != PANEL_POS_BOTTOM)
        gtk_ui_detail_panel_relayout(&b->ctx);

    /* ── Apply system panel position layout before showing ────── */
    /* Always call relayout: system_panel_build() only creates the widget,
     * it does not insert it into the layout hierarchy.  system_panel_relayout()
     * wraps detail_paned in a new paned and packs the system panel in, which
     * is required even for the default PANEL_POS_BOTTOM position. */
    if (b->ctx.system_panel)
        system_panel_relayout(&b->ctx);

    g_signal_connect(b->ctx.detail_vbox, "map", G_CALLBACK(gtk_ui_on_panel_map), &b->ctx);
    if (b->ctx.system_panel)
        g_signal_connect(b->ctx.system_panel, "map", G_CALLBACK(gtk_ui_on_panel_map), &b->ctx);

}
