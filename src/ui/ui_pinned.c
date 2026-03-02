/*
 * ui_pinned.c – Pinned process detail panels.
 *
 * Each pinned PID gets a complete clone of the main detail panel:
 * a collapsible process-info tray with all metadata labels, a full
 * plugin notebook (including headless audio service + milkdrop),
 * and independent data tracking.
 *
 * Public surface (declared in ui_internal.h):
 *   pinned_panel_create()
 *   pinned_panel_destroy()
 *   pinned_panels_update()  — implemented in proc_detail.c
 *
 * Internal helpers used only by ui_gtk.c via the non-static declarations
 * added to ui_internal.h:
 *   pid_is_pinned()
 *   pin_pid()
 *   unpin_pid()
 *   on_toggle_pin()   — signal callback, exported as non-static
 */

#include "ui_internal.h"
#include "../plugin_loader.h"
#include "../plugin_broker.h"
#include "../settings.h"

/* ── pin / unpin helpers ─────────────────────────────────────── */

gboolean pid_is_pinned(const ui_ctx_t *ctx, pid_t pid)
{
    for (size_t i = 0; i < ctx->pinned_count; i++)
        if (ctx->pinned_pids[i] == pid) return TRUE;
    return FALSE;
}

void pin_pid(ui_ctx_t *ctx, pid_t pid)
{
    if (pid_is_pinned(ctx, pid)) return;
    if (ctx->pinned_count >= ctx->pinned_capacity) {
        size_t newcap = ctx->pinned_capacity ? ctx->pinned_capacity * 2 : 16;
        pid_t *tmp = realloc(ctx->pinned_pids, newcap * sizeof(pid_t));
        if (!tmp) return;
        ctx->pinned_pids     = tmp;
        ctx->pinned_capacity = newcap;
    }
    ctx->pinned_pids[ctx->pinned_count++] = pid;
}

void unpin_pid(ui_ctx_t *ctx, pid_t pid)
{
    for (size_t i = 0; i < ctx->pinned_count; i++) {
        if (ctx->pinned_pids[i] == pid) {
            ctx->pinned_pids[i] = ctx->pinned_pids[--ctx->pinned_count];
            return;
        }
    }
}

void on_toggle_pin(GtkMenuItem *item, gpointer data)
{
    (void)item;
    pin_toggle_data_t *d = data;
    if (pid_is_pinned(d->ctx, d->pid)) {
        unpin_pid(d->ctx, d->pid);
        pinned_panel_destroy(d->ctx, d->pid);
    } else {
        pin_pid(d->ctx, d->pid);
        pinned_panel_create(d->ctx, d->pid);
    }
    free(d);
}

/* ── pinned detail panels: create / destroy ──────────────────── */

/*
 * Helper: find a pinned_panel_t by PID.
 * Returns NULL if the panel has been destroyed (e.g. array compacted).
 */
static pinned_panel_t *find_pinned_panel_by_pid(ui_ctx_t *ctx, pid_t pid)
{
    for (size_t i = 0; i < ctx->pinned_panel_count; i++)
        if (ctx->pinned_panels[i].pid == pid)
            return &ctx->pinned_panels[i];
    return NULL;
}

/*
 * Callback data for the tray collapse toggle button.
 * We store ctx + pid instead of a direct pointer into the
 * pinned_panels array, because that array can be compacted
 * when another panel is removed (invalidating raw pointers).
 */
typedef struct {
    ui_ctx_t *ctx;
    pid_t     pid;
} pinned_tray_toggle_data_t;

/*
 * Callback for the tray collapse toggle button inside a pinned panel.
 */
static void on_pinned_tray_toggle(GtkButton *btn, gpointer data)
{
    (void)btn;
    pinned_tray_toggle_data_t *td = data;
    pinned_panel_t *pp = find_pinned_panel_by_pid(td->ctx, td->pid);
    if (!pp) return;  /* panel was already destroyed */

    pp->proc_info_collapsed = !pp->proc_info_collapsed;

    gtk_revealer_set_reveal_child(
        GTK_REVEALER(pp->proc_info_revealer), !pp->proc_info_collapsed);
    gtk_button_set_label(GTK_BUTTON(pp->proc_info_toggle),
                         pp->proc_info_collapsed ? "▶" : "◀");
    gtk_widget_set_tooltip_text(pp->proc_info_toggle,
                                pp->proc_info_collapsed
                                    ? "Show process info"
                                    : "Hide process info");
    if (pp->proc_info_collapsed)
        gtk_widget_show(GTK_WIDGET(pp->proc_info_summary));
    else
        gtk_widget_hide(GTK_WIDGET(pp->proc_info_summary));
}

/*
 * Callback data for the "Unpin" button inside a pinned panel header.
 */
typedef struct {
    ui_ctx_t *ctx;
    pid_t     pid;
} pinned_unpin_data_t;

static void on_pinned_panel_unpin(GtkButton *btn, gpointer data);

/*
 * Helper macros for building the process info grid inside a pinned
 * panel.  These are local to pinned_panel_create and write to a
 * named GtkGrid variable.
 */
#define PP_ROW(grid, row, key_str, label_var) do { \
    GtkWidget *_k = gtk_label_new(key_str); \
    gtk_label_set_xalign(GTK_LABEL(_k), 0.0f); \
    gtk_widget_set_halign(_k, GTK_ALIGN_START); \
    PangoAttrList *_a = pango_attr_list_new(); \
    pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
    gtk_label_set_attributes(GTK_LABEL(_k), _a); \
    pango_attr_list_unref(_a); \
    GtkWidget *_v = gtk_label_new("–"); \
    gtk_label_set_xalign(GTK_LABEL(_v), 0.0f); \
    gtk_label_set_selectable(GTK_LABEL(_v), TRUE); \
    gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_END); \
    gtk_widget_set_halign(_v, GTK_ALIGN_START); \
    gtk_widget_set_hexpand(_v, TRUE); \
    gtk_grid_attach(GTK_GRID(grid), _k, 0, row, 1, 1); \
    gtk_grid_attach(GTK_GRID(grid), _v, 1, row, 1, 1); \
    label_var = GTK_LABEL(_v); \
} while (0)

#define PP_ROW_K(grid, row, key_str, label_var, key_var) do { \
    GtkWidget *_k = gtk_label_new(key_str); \
    gtk_label_set_xalign(GTK_LABEL(_k), 0.0f); \
    gtk_widget_set_halign(_k, GTK_ALIGN_START); \
    PangoAttrList *_a = pango_attr_list_new(); \
    pango_attr_list_insert(_a, pango_attr_weight_new(PANGO_WEIGHT_BOLD)); \
    gtk_label_set_attributes(GTK_LABEL(_k), _a); \
    pango_attr_list_unref(_a); \
    GtkWidget *_v = gtk_label_new("–"); \
    gtk_label_set_xalign(GTK_LABEL(_v), 0.0f); \
    gtk_label_set_selectable(GTK_LABEL(_v), TRUE); \
    gtk_label_set_ellipsize(GTK_LABEL(_v), PANGO_ELLIPSIZE_MIDDLE); \
    gtk_widget_set_halign(_v, GTK_ALIGN_START); \
    gtk_widget_set_hexpand(_v, TRUE); \
    gtk_grid_attach(GTK_GRID(grid), _k, 0, row, 1, 1); \
    gtk_grid_attach(GTK_GRID(grid), _v, 1, row, 1, 1); \
    label_var = GTK_LABEL(_v); \
    key_var = _k; \
} while (0)

/*
 * Create a new pinned detail panel for the given PID.
 *
 * This builds a complete clone of the main detail panel:
 *   • Collapsible process info tray (all sb_* labels, Steam, cgroup)
 *   • Full plugin notebook (ALL plugins including headless + milkdrop)
 *   • Unpin button in the tray header
 *
 * The panel is appended to ctx->pinned_box and shown immediately.
 */
void pinned_panel_create(ui_ctx_t *ctx, pid_t pid)
{
    plugin_registry_t *preg = ctx->plugin_registry;
    if (!preg) return;

    /* Grow the pinned_panels array if needed */
    if (ctx->pinned_panel_count >= ctx->pinned_panel_cap) {
        size_t newcap = ctx->pinned_panel_cap ? ctx->pinned_panel_cap * 2 : 8;
        pinned_panel_t *tmp = realloc(ctx->pinned_panels,
                                      newcap * sizeof(pinned_panel_t));
        if (!tmp) return;
        ctx->pinned_panels   = tmp;
        ctx->pinned_panel_cap = newcap;
    }

    pinned_panel_t *pp = &ctx->pinned_panels[ctx->pinned_panel_count];
    memset(pp, 0, sizeof(*pp));
    pp->pid = pid;

    /* ── Build process info grid (clone of main sidebar) ─────── */
    GtkWidget *info_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(info_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(info_grid), 8);

    PP_ROW(info_grid, 0,  "PID",          pp->sb_pid);
    PP_ROW(info_grid, 1,  "PPID",         pp->sb_ppid);
    PP_ROW(info_grid, 2,  "User",         pp->sb_user);
    PP_ROW(info_grid, 3,  "Name",         pp->sb_name);
    PP_ROW(info_grid, 4,  "CPU%",         pp->sb_cpu);
    PP_ROW(info_grid, 5,  "Memory (RSS)", pp->sb_rss);
    PP_ROW(info_grid, 6,  "Group Memory", pp->sb_group_rss);
    PP_ROW(info_grid, 7,  "Group CPU%",   pp->sb_group_cpu);
    PP_ROW(info_grid, 8,  "Disk Read",    pp->sb_io_read);
    PP_ROW(info_grid, 9,  "Disk Write",   pp->sb_io_write);
    PP_ROW(info_grid, 10, "Net Send",     pp->sb_net_send);
    PP_ROW(info_grid, 11, "Net Recv",     pp->sb_net_recv);
    PP_ROW(info_grid, 12, "Start Time",   pp->sb_start_time);
    PP_ROW(info_grid, 13, "Container",    pp->sb_container);
    PP_ROW(info_grid, 14, "Service",      pp->sb_service);
    PP_ROW(info_grid, 15, "CWD",          pp->sb_cwd);
    PP_ROW(info_grid, 16, "Command",      pp->sb_cmdline);

    /* ── Steam / Proton section ──────────────────────────────── */
    GtkWidget *steam_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(steam_box),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    {
        GtkWidget *sh = gtk_label_new("Steam / Proton");
        gtk_label_set_xalign(GTK_LABEL(sh), 0.0f);
        PangoAttrList *a = pango_attr_list_new();
        pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(sh), a);
        pango_attr_list_unref(a);
        gtk_box_pack_start(GTK_BOX(steam_box), sh, FALSE, FALSE, 0);
    }
    GtkWidget *steam_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(steam_grid), 8);
    gtk_box_pack_start(GTK_BOX(steam_box), steam_grid, FALSE, FALSE, 0);

    PP_ROW(steam_grid, 0, "Game",           pp->sb_steam_game);
    PP_ROW(steam_grid, 1, "App ID",         pp->sb_steam_appid);
    PP_ROW(steam_grid, 2, "Proton",         pp->sb_steam_proton);
    PP_ROW(steam_grid, 3, "Runtime",        pp->sb_steam_runtime);
    PP_ROW(steam_grid, 4, "Compat Data",    pp->sb_steam_compat);
    PP_ROW(steam_grid, 5, "Game Directory", pp->sb_steam_gamedir);
    gtk_widget_set_no_show_all(steam_box, TRUE);
    pp->sb_steam_frame = steam_box;

    /* ── cgroup section ──────────────────────────────────────── */
    GtkWidget *cgroup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(cgroup_box),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    {
        GtkWidget *ch = gtk_label_new("cgroup Limits");
        gtk_label_set_xalign(GTK_LABEL(ch), 0.0f);
        PangoAttrList *a = pango_attr_list_new();
        pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(ch), a);
        pango_attr_list_unref(a);
        gtk_box_pack_start(GTK_BOX(cgroup_box), ch, FALSE, FALSE, 0);
    }
    GtkWidget *cgroup_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(cgroup_grid), 8);
    gtk_box_pack_start(GTK_BOX(cgroup_box), cgroup_grid, FALSE, FALSE, 0);

    PP_ROW(cgroup_grid, 0, "Path",   pp->sb_cgroup_path);
    PP_ROW(cgroup_grid, 1, "Memory", pp->sb_cgroup_mem);
    PP_ROW_K(cgroup_grid, 2, "Mem High", pp->sb_cgroup_mem_high,
             pp->sb_cgroup_mem_high_key);
    PP_ROW_K(cgroup_grid, 3, "Swap", pp->sb_cgroup_swap,
             pp->sb_cgroup_swap_key);
    PP_ROW(cgroup_grid, 4, "CPU",    pp->sb_cgroup_cpu);
    PP_ROW(cgroup_grid, 5, "PIDs",   pp->sb_cgroup_pids);
    PP_ROW_K(cgroup_grid, 6, "I/O",  pp->sb_cgroup_io, pp->sb_cgroup_io_key);
    gtk_widget_set_no_show_all(cgroup_box, TRUE);
    pp->sb_cgroup_frame = cgroup_box;

    /* ── Assemble sidebar content ────────────────────────────── */
    GtkWidget *info_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(info_content), info_grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_content), steam_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_content), cgroup_box, FALSE, FALSE, 0);

    GtkWidget *sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(sidebar_vbox, 8);
    gtk_widget_set_margin_end(sidebar_vbox, 8);
    gtk_widget_set_margin_top(sidebar_vbox, 8);
    gtk_widget_set_margin_bottom(sidebar_vbox, 8);
    gtk_box_pack_start(GTK_BOX(sidebar_vbox), info_content, TRUE, TRUE, 0);

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sidebar_scroll, 240, -1);
    gtk_container_add(GTK_CONTAINER(sidebar_scroll), sidebar_vbox);

    GtkWidget *sidebar_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(sidebar_frame), sidebar_scroll);
    pp->sidebar_frame = sidebar_frame;

    /* Apply main panel's sidebar CSS for font sizing */
    if (ctx->sidebar_css)
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(sidebar_frame),
            GTK_STYLE_PROVIDER(ctx->sidebar_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* ── Collapsible tray (clone of main panel tray) ─────────── */
    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer),
                                         200 /* SECTION_TRANSITION_MS */);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
    gtk_widget_set_size_request(sidebar_frame, 280, -1);
    gtk_container_add(GTK_CONTAINER(revealer), sidebar_frame);
    pp->proc_info_revealer = revealer;

    /* Toggle button */
    GtkWidget *toggle_btn = gtk_button_new_with_label("◀");
    gtk_widget_set_size_request(toggle_btn, 16, -1);
    gtk_widget_set_tooltip_text(toggle_btn, "Hide process info");
    pp->proc_info_toggle = toggle_btn;
    pp->proc_info_collapsed = FALSE;

    /* Compact inline summary (shown when collapsed) */
    char init_summary[128];
    snprintf(init_summary, sizeof(init_summary), "📌 PID %d", (int)pid);
    GtkWidget *summary_w = gtk_label_new(init_summary);
    gtk_label_set_angle(GTK_LABEL(summary_w), 90);
    gtk_label_set_ellipsize(GTK_LABEL(summary_w), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(summary_w, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(summary_w, TRUE);
    gtk_widget_hide(summary_w);
    pp->proc_info_summary = GTK_LABEL(summary_w);

    /* Unpin button (at far left of tray) */
    GtkWidget *unpin_btn = gtk_button_new_with_label("✕");
    gtk_widget_set_tooltip_text(unpin_btn, "Unpin process");
    gtk_widget_set_size_request(unpin_btn, 16, -1);

    /* CSS for tray buttons + summary */
    {
        GtkCssProvider *tray_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(tray_css,
            "button { padding: 0; min-width: 14px; }"
            ".proc-info-summary { padding: 2px 0; font-size: 9pt; }",
            -1, NULL);
        GtkWidget *widgets[] = { toggle_btn, summary_w, unpin_btn };
        for (int i = 0; i < 3; i++)
            gtk_style_context_add_provider(
                gtk_widget_get_style_context(widgets[i]),
                GTK_STYLE_PROVIDER(tray_css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(summary_w), "proc-info-summary");
        g_object_unref(tray_css);
    }

    /* Connect unpin button */
    pinned_unpin_data_t *ud = g_new(pinned_unpin_data_t, 1);
    ud->ctx = ctx;
    ud->pid = pid;
    g_signal_connect(unpin_btn, "clicked",
                     G_CALLBACK(on_pinned_panel_unpin), ud);
    g_object_set_data_full(G_OBJECT(unpin_btn), "ud", ud, g_free);

    /* Tray container: [unpin] [toggle] [summary] [revealer(sidebar)] */
    GtkWidget *tray_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(tray_box), unpin_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tray_box), toggle_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tray_box), summary_w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tray_box), revealer, FALSE, FALSE, 0);

    /* ── Plugin notebook (ALL plugins, including headless + milkdrop) ── */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);

    /* Preferred tab order (same as main panel) */
    static const char *tab_order[] = {
        "org.evemon.pipewire",
        "org.evemon.net",
        NULL
    };
    static const char *tab_order_last[] = {
        "org.evemon.milkdrop",
        NULL
    };
    static const struct { const char *id; const char *label; }
    pp_tab_overrides[] = {
        { "org.evemon.net", "Network" },
        { NULL, NULL }
    };

    size_t max_ids = preg->count + 16;
    int *inst_ids = calloc(max_ids, sizeof(int));
    size_t n_inst = 0;

    /* Collect unique plugin IDs from the existing registry */
    const char **seen_ids = calloc(max_ids, sizeof(const char *));
    size_t n_seen = 0;

    /* Helper: check if a plugin ID was already cloned */
    #define PP_SEEN(id) ({ \
        gboolean _f = FALSE; \
        for (size_t _s = 0; _s < n_seen; _s++) \
            if (strcmp(seen_ids[_s], (id)) == 0) { _f = TRUE; break; } \
        _f; })

    /* Helper: find override label */
    #define PP_LABEL(inst) ({ \
        const char *_l = ((inst)->plugin && (inst)->plugin->name) \
                       ? (inst)->plugin->name : "Plugin"; \
        if ((inst)->plugin && (inst)->plugin->id) { \
            for (int _k = 0; pp_tab_overrides[_k].id; _k++) \
                if (strcmp((inst)->plugin->id, pp_tab_overrides[_k].id) == 0) \
                    { _l = pp_tab_overrides[_k].label; break; } \
        } _l; })

    /* Helper: check if plugin is in last-order list */
    #define PP_IS_LAST(plugin) ({ \
        gboolean _r = FALSE; \
        if ((plugin) && (plugin)->id) { \
            for (int _o = 0; tab_order_last[_o]; _o++) \
                if (strcmp((plugin)->id, tab_order_last[_o]) == 0) \
                    { _r = TRUE; break; } \
        } _r; })

    const size_t orig_count = preg->count;
    gboolean *pass_added = g_new0(gboolean, orig_count);

    /* Pass 1: preferred order */
    for (int o = 0; tab_order[o]; o++) {
        for (size_t i = 0; i < orig_count; i++) {
            plugin_instance_t *orig = &preg->instances[i];
            if (!orig->plugin || !orig->plugin->id) continue;
            if (orig->plugin->kind == EVEMON_PLUGIN_HEADLESS) continue;
            if (!orig->widget) continue;
            if (!PLUGIN_IS_AVAILABLE(orig)) continue;
            if (strcmp(orig->plugin->id, tab_order[o]) != 0) continue;
            if (PP_SEEN(orig->plugin->id)) continue;
            seen_ids[n_seen++] = orig->plugin->id;

            int new_id = plugin_instance_create(preg, orig->plugin->id);
            if (new_id < 0) continue;
            int idx = plugin_registry_find_by_id(preg, new_id);
            if (idx < 0) continue;
            plugin_instance_t *ni = &preg->instances[idx];
            plugin_instance_set_pid(ni, pid, TRUE);
            if (ni->widget) {
                gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                    ni->widget, gtk_label_new(PP_LABEL(ni)));
            }
            inst_ids[n_inst++] = new_id;
            pass_added[i] = TRUE;
        }
    }

    /* Pass 2: remaining (skip last-order) */
    for (size_t i = 0; i < orig_count; i++) {
        if (pass_added[i]) continue;
        plugin_instance_t *orig = &preg->instances[i];
        if (!orig->plugin || !orig->plugin->id) continue;
        if (orig->plugin->kind == EVEMON_PLUGIN_HEADLESS) {
            /* Clone headless plugins too (e.g. audio_service) */
            if (PP_SEEN(orig->plugin->id)) continue;
            seen_ids[n_seen++] = orig->plugin->id;
            int new_id = plugin_instance_create(preg, orig->plugin->id);
            if (new_id < 0) continue;
            int idx = plugin_registry_find_by_id(preg, new_id);
            if (idx < 0) continue;
            plugin_instance_set_pid(&preg->instances[idx], pid, TRUE);
            inst_ids[n_inst++] = new_id;
            pass_added[i] = TRUE;
            continue;
        }
        if (!orig->widget) continue;
        if (!PLUGIN_IS_AVAILABLE(orig)) continue;
        if (PP_IS_LAST(orig->plugin)) continue;
        if (PP_SEEN(orig->plugin->id)) continue;
        seen_ids[n_seen++] = orig->plugin->id;

        int new_id = plugin_instance_create(preg, orig->plugin->id);
        if (new_id < 0) continue;
        int idx = plugin_registry_find_by_id(preg, new_id);
        if (idx < 0) continue;
        plugin_instance_t *ni = &preg->instances[idx];
        plugin_instance_set_pid(ni, pid, TRUE);
        if (ni->widget) {
            gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                ni->widget, gtk_label_new(PP_LABEL(ni)));
        }
        inst_ids[n_inst++] = new_id;
        pass_added[i] = TRUE;
    }

    /* Pass 3: last-order plugins (milkdrop etc.) — hidden by default */
    for (int o = 0; tab_order_last[o]; o++) {
        for (size_t i = 0; i < orig_count; i++) {
            if (pass_added[i]) continue;
            plugin_instance_t *orig = &preg->instances[i];
            if (!orig->plugin || !orig->plugin->id) continue;
            if (orig->plugin->kind == EVEMON_PLUGIN_HEADLESS) continue;
            if (!PLUGIN_IS_AVAILABLE(orig)) continue;
            if (strcmp(orig->plugin->id, tab_order_last[o]) != 0) continue;
            if (PP_SEEN(orig->plugin->id)) continue;
            seen_ids[n_seen++] = orig->plugin->id;

            int new_id = plugin_instance_create(preg, orig->plugin->id);
            if (new_id < 0) continue;
            int idx = plugin_registry_find_by_id(preg, new_id);
            if (idx < 0) continue;
            plugin_instance_t *ni = &preg->instances[idx];
            plugin_instance_set_pid(ni, pid, TRUE);
            if (ni->widget) {
                gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                    ni->widget, gtk_label_new(PP_LABEL(ni)));
                /* Hidden by default — plugin auto-shows when active */
                gtk_widget_set_no_show_all(ni->widget, TRUE);
                gtk_widget_hide(ni->widget);
                GtkWidget *tab_lbl = gtk_notebook_get_tab_label(
                    GTK_NOTEBOOK(notebook), ni->widget);
                if (tab_lbl) {
                    gtk_widget_set_no_show_all(tab_lbl, TRUE);
                    gtk_widget_hide(tab_lbl);
                }
            }
            inst_ids[n_inst++] = new_id;
            pass_added[i] = TRUE;
        }
    }

    g_free(pass_added);
    free(seen_ids);
    #undef PP_SEEN
    #undef PP_LABEL
    #undef PP_IS_LAST

    pp->instance_ids = inst_ids;
    pp->n_instances  = n_inst;
    pp->notebook     = notebook;

    /* Apply plugin CSS class for font sizing */
    gtk_style_context_add_class(
        gtk_widget_get_style_context(notebook), "evemon-plugins");

    /* ── Assemble: detail_hpaned (tray | notebook) inside a frame ── */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hpaned), tray_box, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(hpaned), notebook, TRUE, FALSE);
    pp->detail_hpaned = hpaned;

    GtkWidget *outer = gtk_frame_new(NULL);
    gtk_widget_set_size_request(outer, -1, 200);
    gtk_container_add(GTK_CONTAINER(outer), hpaned);
    pp->outer_frame = outer;

    /* Pack into pinned_box and show */
    gtk_box_pack_start(GTK_BOX(ctx->pinned_box), outer, TRUE, TRUE, 0);

    ctx->pinned_panel_count++;

    /* Connect tray toggle using ctx + pid (looked up at callback time)
     * instead of a direct pointer into the pinned_panels array,
     * which can be invalidated by array compaction when other
     * panels are removed. */
    {
        pinned_tray_toggle_data_t *td = g_new(pinned_tray_toggle_data_t, 1);
        td->ctx = ctx;
        td->pid = pid;
        g_signal_connect(toggle_btn, "clicked",
                         G_CALLBACK(on_pinned_tray_toggle), td);
        g_object_set_data_full(G_OBJECT(toggle_btn), "td", td, g_free);
    }

    gtk_widget_show_all(outer);
    /* Restore summary hidden state after show_all */
    gtk_widget_hide(summary_w);

    /* Kick the broker so the new instances get data immediately */
    broker_start(preg, ctx->mon ? ctx->mon->fdmon : NULL);
}

#undef PP_ROW
#undef PP_ROW_K

/*
 * Destroy the pinned panel for the given PID.
 * Removes all its plugin instances from the registry,
 * destroys the GTK widgets, and compacts the array.
 */
void pinned_panel_destroy(ui_ctx_t *ctx, pid_t pid)
{
    plugin_registry_t *preg = ctx->plugin_registry;

    for (size_t i = 0; i < ctx->pinned_panel_count; i++) {
        pinned_panel_t *pp = &ctx->pinned_panels[i];
        if (pp->pid != pid) continue;

        /* Destroy plugin instances */
        if (preg) {
            for (size_t j = 0; j < pp->n_instances; j++)
                plugin_instance_destroy(preg, pp->instance_ids[j]);
        }
        free(pp->instance_ids);

        /* Destroy the GTK widget tree */
        if (pp->outer_frame)
            gtk_widget_destroy(pp->outer_frame);

        /* Compact the array */
        for (size_t k = i; k + 1 < ctx->pinned_panel_count; k++)
            ctx->pinned_panels[k] = ctx->pinned_panels[k + 1];
        ctx->pinned_panel_count--;
        return;
    }
}

/* Unpin button callback inside a pinned panel header.
 * We must defer the actual destroy to an idle callback because
 * the unpin button is a *child* of the widget tree that
 * pinned_panel_destroy() will destroy.  Destroying a widget
 * while its "clicked" signal is still being emitted causes
 * GTK to access freed memory when it unwinds the emission
 * chain → segfault.
 */
static gboolean pinned_panel_unpin_idle(gpointer data)
{
    pinned_unpin_data_t *ud = data;
    unpin_pid(ud->ctx, ud->pid);
    pinned_panel_destroy(ud->ctx, ud->pid);
    g_free(ud);
    return G_SOURCE_REMOVE;
}

static void on_pinned_panel_unpin(GtkButton *btn, gpointer data)
{
    (void)btn;
    pinned_unpin_data_t *ud = data;
    /* Schedule the destroy on the next idle iteration so the
     * button signal emission can finish safely. */
    pinned_unpin_data_t *copy = g_new(pinned_unpin_data_t, 1);
    copy->ctx = ud->ctx;
    copy->pid = ud->pid;
    g_idle_add(pinned_panel_unpin_idle, copy);
}
