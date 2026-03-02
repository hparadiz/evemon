/*
 * ui_input.c – Keyboard, mouse, and row-collapse/expand event handlers.
 *
 * Public surface (declared in ui_internal.h):
 *   on_button_press()
 *   on_button_release()
 *   on_motion_notify()
 *   on_focus_out()
 *   on_row_collapsed()
 *   on_row_expanded()
 *   on_key_press()
 *   on_key_release()
 *   on_filter_entry_key_release()
 *   on_pid_entry_insert_text()
 *   on_pid_entry_key_release()
 *   on_overlay_get_child_position()
 */

#include "ui_internal.h"
#include <math.h>

/* Defined in ui_gtk.c */
extern void show_process_context_menu(ui_ctx_t *ctx, GdkEventButton *ev,
                                      pid_t pid, const char *name,
                                      const char *cmdline);
extern void stop_autoscroll(ui_ctx_t *ctx);
extern gboolean autoscroll_tick(gpointer data);
extern void reload_font_css(ui_ctx_t *ctx);
extern void register_sort_funcs(GtkTreeModelSort *sm);

/* Defined in ui_filter.c */
extern void rebuild_filter_store(ui_ctx_t *ctx);
extern void switch_to_real_store(ui_ctx_t *ctx);
extern void rebuild_audio_filter_store(ui_ctx_t *ctx);

/* ── overlay positioning ─────────────────────────────────────── */

/*
 * Position the filter / PID entry widget inside the column header row,
 * right after the "Name" / "PID" label text.
 */
gboolean on_overlay_get_child_position(GtkOverlay   *overlay,
                                       GtkWidget    *child,
                                       GdkRectangle *alloc,
                                       gpointer      data)
{
    (void)overlay;
    ui_ctx_t *ctx = data;

    GtkTreeViewColumn *target_col = NULL;
    const char        *label_text = NULL;

    if (child == ctx->filter_entry) {
        target_col = ctx->name_col;
        label_text = "Name";
    } else if (child == ctx->pid_entry) {
        target_col = ctx->pid_col;
        label_text = "PID";
    } else {
        return FALSE;
    }

    GtkWidget *btn = gtk_tree_view_column_get_button(target_col);
    if (!btn || !gtk_widget_get_realized(btn))
        return FALSE;

    GtkAllocation btn_alloc;
    gtk_widget_get_allocation(btn, &btn_alloc);

    int ox = 0, oy = 0;
    GtkWidget *overlay_widget = GTK_WIDGET(overlay);
    gtk_widget_translate_coordinates(btn, overlay_widget, 0, 0, &ox, &oy);

    PangoLayout *layout = gtk_widget_create_pango_layout(btn, label_text);
    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    g_object_unref(layout);

    GtkRequisition entry_nat;
    gtk_widget_get_preferred_size(child, NULL, &entry_nat);

    int margin  = 4;
    int min_gap = 6;
    int entry_h = entry_nat.height;
    if (entry_h > btn_alloc.height - 2)
        entry_h = btn_alloc.height - 2;

    int col_right = ox + btn_alloc.width - margin;
    int entry_w   = entry_nat.width;
    int max_w     = col_right - (ox + text_w + min_gap);
    if (max_w < 30) max_w = 30;
    if (entry_w > max_w) entry_w = max_w;

    alloc->x      = col_right - entry_w;
    alloc->y      = oy + (btn_alloc.height - entry_h) / 2;
    alloc->width  = entry_w;
    alloc->height = entry_h;

    return TRUE;
}

/* ── auto-hide timer for empty, visible filter entry ─────────── */

static gboolean on_filter_hide_timeout(gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->filter_hide_timer = 0;

    if (ctx->filter_text[0] == '\0' && ctx->filter_entry &&
        gtk_widget_get_visible(ctx->filter_entry)) {
        gtk_widget_hide(ctx->filter_entry);
    }
    return G_SOURCE_REMOVE;
}

void filter_cancel_hide_timer(ui_ctx_t *ctx)
{
    if (ctx->filter_hide_timer) {
        g_source_remove(ctx->filter_hide_timer);
        ctx->filter_hide_timer = 0;
    }
}

static void filter_schedule_hide(ui_ctx_t *ctx)
{
    filter_cancel_hide_timer(ctx);
    ctx->filter_hide_timer = g_timeout_add(5000, on_filter_hide_timeout, ctx);
}

gboolean on_filter_entry_key_release(GtkWidget *widget, GdkEventKey *ev,
                                     gpointer data)
{
    ui_ctx_t *ctx = data;

    if (ev->keyval == GDK_KEY_Escape) {
        filter_cancel_hide_timer(ctx);
        gtk_entry_set_text(GTK_ENTRY(ctx->filter_entry), "");
        ctx->filter_text[0] = '\0';
        if (ctx->show_audio_only)
            rebuild_audio_filter_store(ctx);
        else
            switch_to_real_store(ctx);
        gtk_widget_hide(ctx->filter_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
        return TRUE;
    }

    guint state = ev->state & gtk_accelerator_get_default_mod_mask();
    if (state & (GDK_CONTROL_MASK | GDK_META_MASK))
        return FALSE;

    const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
    snprintf(ctx->filter_text, sizeof(ctx->filter_text), "%s", text ? text : "");

    if (ctx->filter_text[0] != '\0') {
        filter_cancel_hide_timer(ctx);
        rebuild_filter_store(ctx);
    } else {
        if (ctx->show_audio_only)
            rebuild_audio_filter_store(ctx);
        else
            switch_to_real_store(ctx);
        filter_schedule_hide(ctx);
    }

    return FALSE;
}

/* ── Go-to-PID entry helpers ──────────────────────────────────── */

void on_pid_entry_insert_text(GtkEditable *editable,
                              const gchar *text,
                              gint         length,
                              gint        *position,
                              gpointer     data)
{
    (void)editable; (void)position; (void)data;
    for (gint i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') {
            g_signal_stop_emission_by_name(editable, "insert-text");
            return;
        }
    }
}

void goto_pid(ui_ctx_t *ctx, pid_t target)
{
    GtkTreeModel *child = gtk_tree_model_sort_get_model(ctx->sort_model);
    GtkTreeIter child_iter;

    if (!find_iter_by_pid(child, NULL, target, &child_iter))
        return;

    GtkTreePath *child_path = gtk_tree_model_get_path(child, &child_iter);
    if (!child_path)
        return;

    GtkTreePath *sort_path =
        gtk_tree_model_sort_convert_child_path_to_path(
            ctx->sort_model, child_path);
    gtk_tree_path_free(child_path);
    if (!sort_path)
        return;

    GtkTreePath *parent_path = gtk_tree_path_copy(sort_path);
    while (gtk_tree_path_up(parent_path) &&
           gtk_tree_path_get_depth(parent_path) > 0) {
        gtk_tree_view_expand_row(ctx->view, parent_path, FALSE);
    }
    gtk_tree_path_free(parent_path);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    gtk_tree_selection_select_path(sel, sort_path);
    gtk_tree_view_scroll_to_cell(ctx->view, sort_path,
                                 NULL, TRUE, 0.5f, 0.0f);
    gtk_tree_path_free(sort_path);
}

gboolean on_pid_entry_key_release(GtkWidget *widget, GdkEventKey *ev,
                                  gpointer data)
{
    ui_ctx_t *ctx = data;

    if (ev->keyval == GDK_KEY_Escape) {
        gtk_entry_set_text(GTK_ENTRY(ctx->pid_entry), "");
        gtk_widget_hide(ctx->pid_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
        return TRUE;
    }

    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        gtk_entry_set_text(GTK_ENTRY(ctx->pid_entry), "");
        gtk_widget_hide(ctx->pid_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
        return TRUE;
    }

    guint state = ev->state & gtk_accelerator_get_default_mod_mask();
    if (state & (GDK_CONTROL_MASK | GDK_META_MASK))
        return FALSE;

    const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
    if (!text || !text[0])
        return FALSE;

    char *end = NULL;
    long val = strtol(text, &end, 10);
    if (end == text || *end != '\0' || val <= 0)
        return FALSE;

    goto_pid(ctx, (pid_t)val);
    return FALSE;
}

/* ── mouse / autoscroll handlers ─────────────────────────────── */

gboolean on_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    ui_ctx_t *ctx = data;

    if (ev->button == 3) {   /* right-click */
        GtkTreePath *path = NULL;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                         (gint)ev->x, (gint)ev->y,
                                         &path, NULL, NULL, NULL)) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(
                GTK_TREE_VIEW(widget));
            gtk_tree_selection_select_path(sel, path);

            GtkTreeIter sort_iter;
            if (gtk_tree_model_get_iter(
                    GTK_TREE_MODEL(ctx->sort_model),
                    &sort_iter, path)) {
                GtkTreeIter child_iter;
                gtk_tree_model_sort_convert_iter_to_child_iter(
                    ctx->sort_model, &child_iter, &sort_iter);
                GtkTreeModel *child_model = gtk_tree_model_sort_get_model(
                    ctx->sort_model);
                gint pid = 0;
                gchar *name = NULL;
                gchar *cmdline = NULL;
                gtk_tree_model_get(child_model, &child_iter,
                                   COL_PID, &pid, COL_NAME, &name,
                                   COL_CMDLINE, &cmdline, -1);
                show_process_context_menu(ctx, ev, (pid_t)pid,
                                         name ? name : "?",
                                         cmdline ? cmdline : "");
                g_free(name);
                g_free(cmdline);
            }
            gtk_tree_path_free(path);
        }
        return TRUE;
    }

    if (ev->button == 2) {   /* middle button – autoscroll */
        ctx->autoscroll = TRUE;
        ctx->follow_selection = FALSE;
        ctx->anchor_x   = ev->x_root;
        ctx->anchor_y   = ev->y_root;
        ctx->velocity_x = 0;
        ctx->velocity_y = 0;

        GdkDisplay *display = gdk_display_get_default();
        GdkSeat    *seat    = gdk_display_get_default_seat(display);
        GdkWindow  *win     = gtk_widget_get_window(widget);
        GdkCursor  *cursor  = gdk_cursor_new_from_name(display, "all-scroll");

        gdk_seat_grab(seat, win, GDK_SEAT_CAPABILITY_POINTER,
                      TRUE, cursor, (GdkEvent *)ev, NULL, NULL);
        if (cursor) g_object_unref(cursor);

        ctx->scroll_timer = g_timeout_add(AUTOSCROLL_INTERVAL,
                                          autoscroll_tick, ctx);
        return TRUE;
    }
    return FALSE;
}

gboolean on_button_release(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    if (ev->button == 2 && ctx->autoscroll) {
        stop_autoscroll(ctx);
        return TRUE;
    }
    return FALSE;
}

gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *ev, gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;
    if (!ctx->autoscroll) return FALSE;

    double raw_dx = ev->x_root - ctx->anchor_x;
    double raw_dy = ev->y_root - ctx->anchor_y;
    double dist   = sqrt(raw_dx * raw_dx + raw_dy * raw_dy);

    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    int win_w = gtk_widget_get_allocated_width(toplevel);
    int win_h = gtk_widget_get_allocated_height(toplevel);
    double deadzone = AUTOSCROLL_DEADZONE_FRAC * (win_w < win_h ? win_w : win_h);
    if (deadzone < 8.0) deadzone = 8.0;

    if (dist < deadzone) {
        ctx->velocity_x = 0;
        ctx->velocity_y = 0;
    } else {
        double beyond = dist - deadzone;
        double speed  = AUTOSCROLL_SCALE * log(1.0 + beyond);
        ctx->velocity_x = speed * (raw_dx / dist);
        ctx->velocity_y = speed * (raw_dy / dist);
    }
    return TRUE;
}

gboolean on_focus_out(GtkWidget *widget, GdkEventFocus *ev, gpointer data)
{
    (void)widget; (void)ev;
    ui_ctx_t *ctx = data;

    if (ctx->autoscroll)
        stop_autoscroll(ctx);
    return FALSE;
}

/* ── row collapse / expand persistence ──────────────────────── */

void on_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                      GtkTreePath *path, gpointer data)
{
    ui_ctx_t *ctx = data;

    if (gtk_tree_path_get_depth(path) > 1) {
        GtkTreePath *parent_path = gtk_tree_path_copy(path);
        gtk_tree_path_up(parent_path);
        gboolean parent_expanded = gtk_tree_view_row_expanded(view, parent_path);
        gtk_tree_path_free(parent_path);
        if (!parent_expanded)
            return;
    }

    gint pid;
    GtkTreeIter child_iter;
    gtk_tree_model_sort_convert_iter_to_child_iter(
        ctx->sort_model, &child_iter, iter);
    GtkTreeModel *child_model = gtk_tree_model_sort_get_model(ctx->sort_model);
    gtk_tree_model_get(child_model, &child_iter, COL_PID, &pid, -1);

    pid_t pinned_root = get_row_pinned_root(child_model, &child_iter);
    set_process_tree_node(&ctx->ptree_nodes, pinned_root, (pid_t)pid,
                          PTREE_COLLAPSED);
}

void on_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                     GtkTreePath *path, gpointer data)
{
    (void)path;
    ui_ctx_t *ctx = data;

    gint pid;
    GtkTreeIter child_iter;
    gtk_tree_model_sort_convert_iter_to_child_iter(
        ctx->sort_model, &child_iter, iter);
    GtkTreeModel *child_model = gtk_tree_model_sort_get_model(ctx->sort_model);
    gtk_tree_model_get(child_model, &child_iter, COL_PID, &pid, -1);

    pid_t pinned_root = get_row_pinned_root(child_model, &child_iter);
    set_process_tree_node(&ctx->ptree_nodes, pinned_root, (pid_t)pid,
                          PTREE_EXPANDED);

    GtkTreeModel *sort_model = gtk_tree_view_get_model(view);
    GtkTreeIter sort_child;
    gboolean valid = gtk_tree_model_iter_children(sort_model, &sort_child, iter);
    while (valid) {
        if (gtk_tree_model_iter_has_child(sort_model, &sort_child)) {
            GtkTreeIter store_child;
            gtk_tree_model_sort_convert_iter_to_child_iter(
                ctx->sort_model, &store_child, &sort_child);
            gint child_pid;
            gtk_tree_model_get(child_model, &store_child,
                               COL_PID, &child_pid, -1);

            if (get_process_tree_node(&ctx->ptree_nodes, pinned_root,
                                      (pid_t)child_pid) == PTREE_EXPANDED) {
                GtkTreePath *child_path = gtk_tree_model_get_path(
                    sort_model, &sort_child);
                if (child_path) {
                    gtk_tree_view_expand_row(view, child_path, FALSE);
                    gtk_tree_path_free(child_path);
                }
            }
        }
        valid = gtk_tree_model_iter_next(sort_model, &sort_child);
    }
}

/* ── keyboard shortcut detection ────────────────────────────── */

static GdkModifierType detect_shortcut_modifier(void)
{
    const char *config_home = g_getenv("XDG_CONFIG_HOME");
    char path[PATH_MAX];
    if (config_home && config_home[0])
        snprintf(path, sizeof(path), "%s/kdeglobals", config_home);
    else
        snprintf(path, sizeof(path), "%s/.config/kdeglobals",
                 g_get_home_dir());

    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[512];
        gboolean in_shortcuts = FALSE;
        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'
                           || line[len - 1] == ' '))
                line[--len] = '\0';

            if (line[0] == '[') {
                in_shortcuts = (g_ascii_strcasecmp(line, "[Shortcuts]") == 0);
                continue;
            }
            if (!in_shortcuts)
                continue;

            if (g_str_has_prefix(line, "Copy=") ||
                g_str_has_prefix(line, "Paste=") ||
                g_str_has_prefix(line, "Cut=") ||
                g_str_has_prefix(line, "SelectAll=")) {
                const char *val = strchr(line, '=');
                if (val && g_ascii_strncasecmp(val + 1, "Meta+", 5) == 0) {
                    fclose(fp);
                    return GDK_META_MASK;
                }
                if (val && val[1] != '\0') {
                    fclose(fp);
                    return GDK_CONTROL_MASK;
                }
            }
        }
        fclose(fp);
    }

    if (config_home && config_home[0])
        snprintf(path, sizeof(path), "%s/gtk-3.0/settings.ini", config_home);
    else
        snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini",
                 g_get_home_dir());

    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *theme = g_key_file_get_string(kf, "Settings",
                                            "gtk-key-theme-name", NULL);
        if (theme) {
            gboolean is_mac = (g_ascii_strcasecmp(theme, "Mac") == 0);
            g_free(theme);
            g_key_file_free(kf);
            return is_mac ? GDK_META_MASK : GDK_CONTROL_MASK;
        }
    }
    g_key_file_free(kf);

    snprintf(path, sizeof(path), "%s/.gtkrc-2.0", g_get_home_dir());
    fp = fopen(path, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "gtk-key-theme-name")) {
                gboolean is_mac = (strcasestr(line, "\"Mac\"") != NULL);
                fclose(fp);
                return is_mac ? GDK_META_MASK : GDK_CONTROL_MASK;
            }
        }
        fclose(fp);
    }

    return GDK_CONTROL_MASK;
}

/* ── keyboard event handlers ─────────────────────────────────── */

gboolean on_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    static GdkModifierType mod = 0;
    if (mod == 0) {
        hotkey_mode_t hmode = settings_get()->hotkey_mode;
        if (hmode == HOTKEY_MODE_MACOS)
            mod = GDK_META_MASK;
        else
            mod = detect_shortcut_modifier();
    }

    guint state = ev->state & gtk_accelerator_get_default_mod_mask();

    if (ev->keyval == GDK_KEY_Alt_L || ev->keyval == GDK_KEY_Alt_R) {
        ctx->alt_pressed = TRUE;
        return FALSE;
    }
    if (state & GDK_MOD1_MASK)
        ctx->alt_pressed = FALSE;

    /* Escape: return focus to tree view or deselect */
    if (ev->keyval == GDK_KEY_Escape && state == 0) {
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(widget));
        if (focus && gtk_widget_get_visible(ctx->sidebar) &&
            gtk_widget_is_ancestor(focus, ctx->sidebar)) {
            gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
            return TRUE;
        }
        if (focus == GTK_WIDGET(ctx->view)) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
            gtk_tree_selection_unselect_all(sel);
            return TRUE;
        }
    }

    /* Arrow keys: collapse / expand selected row */
    if (state == 0 &&
        (ev->keyval == GDK_KEY_Left || ev->keyval == GDK_KEY_Right)) {
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(widget));
        if (focus == GTK_WIDGET(ctx->view)) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
            GtkTreeModel *model = NULL;
            GtkTreeIter sort_iter;
            if (gtk_tree_selection_get_selected(sel, &model, &sort_iter)) {
                GtkTreePath *path = gtk_tree_model_get_path(model, &sort_iter);
                if (path) {
                    if (ev->keyval == GDK_KEY_Right) {
                        gtk_tree_view_expand_row(ctx->view, path, FALSE);
                    } else {
                        if (!gtk_tree_view_row_expanded(ctx->view, path) ||
                            !gtk_tree_model_iter_has_child(model, &sort_iter)) {
                            if (gtk_tree_path_up(path) &&
                                gtk_tree_path_get_depth(path) > 0) {
                                gtk_tree_view_set_cursor(ctx->view, path,
                                                         NULL, FALSE);
                            }
                        } else {
                            gtk_tree_view_collapse_row(ctx->view, path);
                        }
                    }
                    gtk_tree_path_free(path);
                }
            }
            return TRUE;
        }
    }

    /* Ctrl+F / Meta+F → toggle name filter */
    if (ev->keyval == GDK_KEY_f &&
        (state == GDK_CONTROL_MASK || state == GDK_META_MASK)) {
        if (ctx->filter_entry) {
            if (gtk_widget_get_visible(ctx->filter_entry)) {
                filter_cancel_hide_timer(ctx);
                gtk_entry_set_text(GTK_ENTRY(ctx->filter_entry), "");
                ctx->filter_text[0] = '\0';
                switch_to_real_store(ctx);
                gtk_widget_hide(ctx->filter_entry);
                gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
            } else {
                filter_cancel_hide_timer(ctx);
                gtk_widget_show(ctx->filter_entry);
                gtk_widget_grab_focus(ctx->filter_entry);
            }
        }
        return TRUE;
    }

    /* Ctrl+G / Meta+G → toggle Go-to-PID entry */
    if (ev->keyval == GDK_KEY_g &&
        (state == GDK_CONTROL_MASK || state == GDK_META_MASK)) {
        if (ctx->pid_entry) {
            if (gtk_widget_get_visible(ctx->pid_entry)) {
                gtk_entry_set_text(GTK_ENTRY(ctx->pid_entry), "");
                gtk_widget_hide(ctx->pid_entry);
                gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
            } else {
                gtk_widget_show(ctx->pid_entry);
                gtk_widget_grab_focus(ctx->pid_entry);
            }
        }
        return TRUE;
    }

    if (state != (guint)mod)
        return FALSE;

    switch (ev->keyval) {
    case GDK_KEY_plus:
    case GDK_KEY_equal:
    case GDK_KEY_KP_Add:
        if (ctx->font_size < FONT_SIZE_MAX) {
            ctx->font_size++;
            reload_font_css(ctx);
        }
        return TRUE;

    case GDK_KEY_minus:
    case GDK_KEY_KP_Subtract:
        if (ctx->font_size > FONT_SIZE_MIN) {
            ctx->font_size--;
            reload_font_css(ctx);
        }
        return TRUE;

    case GDK_KEY_0:
    case GDK_KEY_KP_0:
        ctx->font_size = FONT_SIZE_DEFAULT;
        reload_font_css(ctx);
        return TRUE;

    case GDK_KEY_q:
        gtk_main_quit();
        return TRUE;

    default:
        break;
    }

    return FALSE;
}

gboolean on_key_release(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)widget;
    ui_ctx_t *ctx = data;

    if ((ev->keyval == GDK_KEY_Alt_L || ev->keyval == GDK_KEY_Alt_R) &&
        ctx->alt_pressed) {
        ctx->alt_pressed = FALSE;

        if (!gtk_widget_get_visible(ctx->menubar))
            gtk_widget_show_all(ctx->menubar);

        gtk_menu_shell_select_item(GTK_MENU_SHELL(ctx->menubar),
                                   ctx->file_menu_item);
        return TRUE;
    }
    ctx->alt_pressed = FALSE;
    return FALSE;
}
