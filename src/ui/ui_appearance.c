#include "ui_gtk_private.h"

#include <limits.h>

/* ── GTK theme discovery & selection ──────────────────────────── */

static int theme_name_cmp(const void *a, const void *b)
{
    return g_ascii_strcasecmp(*(const char **)a, *(const char **)b);
}

/*
 * Collect available GTK3 theme names from the standard search paths.
 * A directory is a valid theme if it contains gtk-3.0/gtk.css.
 * Returns a NULL-terminated array of strdup'd names (caller frees).
 */
static char **collect_gtk_themes(int *out_count)
{
    /* Candidate directories: ~/.themes, ~/.local/share/themes,
     * $XDG_DATA_DIRS/themes (default /usr/share/themes) */
    const char *home = g_get_home_dir();
    char buf[PATH_MAX];

    /* Build a list of base directories to scan */
    const char *bases[16];
    int nbases = 0;

    static char home_themes[PATH_MAX];
    static char home_local[PATH_MAX];
    snprintf(home_themes, sizeof(home_themes), "%s/.themes", home);
    snprintf(home_local,  sizeof(home_local),  "%s/.local/share/themes", home);
    bases[nbases++] = home_themes;
    bases[nbases++] = home_local;

    /* Parse XDG_DATA_DIRS (colon-separated) */
    const char *xdg = g_getenv("XDG_DATA_DIRS");
    if (!xdg || !xdg[0])
        xdg = "/usr/local/share:/usr/share";

    char *xdg_copy = strdup(xdg);
    char *saveptr = NULL;
    for (char *tok = strtok_r(xdg_copy, ":", &saveptr);
         tok && nbases < 14;
         tok = strtok_r(NULL, ":", &saveptr)) {
        /* We'll construct the full path later with /themes appended */
        bases[nbases++] = tok;  /* points into xdg_copy, alive until free */
    }

    /* Collect theme names into a dynamic array */
    int cap = 64, count = 0;
    char **names = malloc(cap * sizeof(char *));

    for (int i = 0; i < nbases; i++) {
        /* For XDG_DATA_DIRS entries, append /themes */
        const char *dir;
        char full[PATH_MAX];
        if (i < 2) {
            dir = bases[i];  /* already has /themes */
        } else {
            snprintf(full, sizeof(full), "%s/themes", bases[i]);
            dir = full;
        }

        DIR *d = opendir(dir);
        if (!d) continue;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            /* Check for gtk-3.0/gtk.css inside the theme dir */
            int n = snprintf(buf, sizeof(buf), "%s/%s/gtk-3.0/gtk.css",
                             dir, ent->d_name);
            if (n < 0 || (size_t)n >= sizeof(buf))
                continue;  /* path too long – skip */
            struct stat st;
            if (stat(buf, &st) != 0 || !S_ISREG(st.st_mode))
                continue;

            /* Skip duplicates */
            gboolean dup = FALSE;
            for (int j = 0; j < count; j++) {
                if (strcmp(names[j], ent->d_name) == 0) {
                    dup = TRUE;
                    break;
                }
            }
            if (dup) continue;

            if (count + 1 >= cap) {
                if (cap > INT_MAX / 2 ||
                    (size_t)cap * 2 > SIZE_MAX / sizeof(char *))
                    break;
                int new_cap = cap * 2;
                char **tmp = realloc(names,
                                     (size_t)new_cap * sizeof(char *));
                if (!tmp) break;   /* OOM – stop collecting */
                names = tmp;
                cap = new_cap;
            }
            names[count++] = strdup(ent->d_name);
        }
        closedir(d);
    }
    free(xdg_copy);

    /* Sort alphabetically */
    if (count > 1)
        qsort(names, count, sizeof(char *), theme_name_cmp);

    names[count] = NULL;
    if (out_count) *out_count = count;
    return names;
}

static void on_theme_selected(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    if (!gtk_check_menu_item_get_active(item))
        return;  /* ignore deactivation of the old radio item */

    const char *name = gtk_menu_item_get_label(GTK_MENU_ITEM(item));
    GtkSettings *gs = gtk_settings_get_default();
    g_object_set(gs, "gtk-theme-name", name, NULL);

    /* Persist theme choice */
    evemon_settings_t *s = settings_get();
    snprintf(s->theme, sizeof(s->theme), "%s", name);
    settings_save();
}

/* ── spectrogram / charting colour theme ────────────────────── */

static const char *charting_theme_labels[SPECTRO_THEME_COUNT] = {
    "Classic", "Heat", "Cool", "Greyscale", "Neon", "Vaporwave"
};



charting_theme_data_t charting_theme_data[SPECTRO_THEME_COUNT];

/* Forward declaration — defined after gtk_ui_apply_charting_theme */
void gtk_ui_on_charting_theme_selected(GtkCheckMenuItem *item, gpointer data);

void gtk_ui_apply_charting_theme(ui_ctx_t *ctx, spectro_theme_t theme)
{
#ifdef HAVE_PIPEWIRE
    /* Apply to every active spectrogram instance */
    for (size_t i = 0; i < ctx->spectro_count; i++) {
        GtkDrawingArea *da = GTK_DRAWING_AREA(ctx->spectro_instances[i].draw_area);
        spectrogram_set_theme(ctx, da, theme);
    }
#endif
    /* Persist */
    evemon_settings_t *s = settings_get();
    if (s) {
        s->spectro_theme = (int)theme;
        settings_save();
    }
    /* Sync menubar radio buttons — block signals to avoid re-entrant call */
    if (ctx->charting_theme_items[theme]) {
        for (int _i = 0; _i < SPECTRO_THEME_COUNT; _i++)
            if (ctx->charting_theme_items[_i])
                g_signal_handlers_block_by_func(
                    ctx->charting_theme_items[_i],
                    G_CALLBACK(gtk_ui_on_charting_theme_selected),
                    &charting_theme_data[_i]);
        gtk_check_menu_item_set_active(ctx->charting_theme_items[theme], TRUE);
        for (int _i = 0; _i < SPECTRO_THEME_COUNT; _i++)
            if (ctx->charting_theme_items[_i])
                g_signal_handlers_unblock_by_func(
                    ctx->charting_theme_items[_i],
                    G_CALLBACK(gtk_ui_on_charting_theme_selected),
                    &charting_theme_data[_i]);
    }
    /* Notify plugins that registered a charting-theme callback */
    for (int _i = 0; _i < ctx->charting_notify_count; _i++)
        if (ctx->charting_notify[_i].cb)
            ctx->charting_notify[_i].cb(ctx->charting_notify[_i].plugin_ctx,
                                        (unsigned)theme);
}

void gtk_ui_on_charting_theme_selected(GtkCheckMenuItem *item, gpointer data)
{
    if (!gtk_check_menu_item_get_active(item))
        return;
    charting_theme_data_t *d = data;
    gtk_ui_apply_charting_theme(d->ctx, d->theme);
}

/*
 * Build a "Charting Theme" submenu with radio items for each spectro theme.
 */
GtkWidget *gtk_ui_build_charting_theme_submenu(ui_ctx_t *ctx,
                                               GSList **out_group)
{
    GtkWidget *menu = gtk_menu_new();
    evemon_settings_t *s = settings_get();
    int saved = (s && s->spectro_theme >= 0 &&
                 s->spectro_theme < SPECTRO_THEME_COUNT)
                ? s->spectro_theme : 0;

    GSList *group = NULL;
    for (int i = 0; i < SPECTRO_THEME_COUNT; i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_label(
            group, charting_theme_labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        if (i == saved)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        ctx->charting_theme_items[i] = GTK_CHECK_MENU_ITEM(item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    if (out_group) *out_group = group;
    return menu;
}

/*
 * Build a "Theme" submenu with radio items for each discovered GTK3 theme.
 * The currently active theme gets the radio bullet.
 */
GtkWidget *gtk_ui_build_theme_submenu(void)
{
    GtkWidget *menu = gtk_menu_new();

    /* Get current theme name */
    GtkSettings *settings = gtk_settings_get_default();
    char *current = NULL;
    g_object_get(settings, "gtk-theme-name", &current, NULL);

    int count = 0;
    char **themes = collect_gtk_themes(&count);

    GSList *group = NULL;
    for (int i = 0; i < count; i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_label(group, themes[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));

        if (current && strcmp(themes[i], current) == 0)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);

        g_signal_connect(item, "toggled", G_CALLBACK(on_theme_selected), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        free(themes[i]);
    }
    free(themes);
    g_free(current);

    return menu;
}

/* ── font helpers ─────────────────────────────────────────────── */

void reload_font_css(ui_ctx_t *ctx)
{
    char buf[256];

    /* Main process tree */
    snprintf(buf, sizeof(buf),
             "treeview { font-family: Monospace; font-size: %dpt; }",
             ctx->font_size);
    gtk_css_provider_load_from_data(ctx->css, buf, -1, NULL);

    /* Detail panel labels and frame title */
    if (ctx->sidebar_css) {
        snprintf(buf, sizeof(buf),
                 "frame, label, checkbutton { font-size: %dpt; }",
                 ctx->font_size);
        gtk_css_provider_load_from_data(ctx->sidebar_css, buf, -1, NULL);
    }

    /* Plugin notebook: tab labels + all plugin widget contents */
    if (ctx->plugin_css) {
        snprintf(buf, sizeof(buf),
                 ".evemon-plugins tab,"
                 ".evemon-plugins tab label,"
                 ".evemon-plugins treeview,"
                 ".evemon-plugins label,"
                 ".evemon-plugins checkbutton { font-size: %dpt; }",
                 ctx->font_size);
        gtk_css_provider_load_from_data(ctx->plugin_css, buf, -1, NULL);
    }

    /* Name-filter and PID search entries */
    if (ctx->filter_css) {
        snprintf(buf, sizeof(buf),
                 "entry {"
                 "  font-size: %dpt;"
                 "  min-height: 0;"
                 "  padding: 1px 4px;"
                 "  border-radius: 3px;"
                 "}", ctx->font_size);
        gtk_css_provider_load_from_data(ctx->filter_css, buf, -1, NULL);
    }
    if (ctx->pid_css) {
        snprintf(buf, sizeof(buf),
                 "entry {"
                 "  font-size: %dpt;"
                 "  min-height: 0;"
                 "  padding: 1px 4px;"
                 "  border-radius: 3px;"
                 "}", ctx->font_size);
        gtk_css_provider_load_from_data(ctx->pid_css, buf, -1, NULL);
    }
}

/* ── desktop-environment–aware modifier detection ─────────────── */

/*
 * Detect whether the user's desktop shortcuts use Meta (Super) instead
 * of Ctrl as the primary application-shortcut modifier.
 *
 * Strategy (checked in order):
 *
 *  1. KDE  – parse ~/.config/kdeglobals [Shortcuts] section.
 *            If Copy=Meta+C (or similar), the user has macOS-style
 *            bindings → use GDK_META_MASK.
 *
 *  2. GTK 3 – parse ~/.config/gtk-3.0/settings.ini for
 *             gtk-key-theme-name.  "Mac" or "Emacs" themes remap
 *             shortcuts, but in practice GTK apps always use Ctrl
 *             unless the KDE portal overrides it (handled by #1).
 *
 *  3. GTK 2 – ~/.gtkrc-2.0 gtk-key-theme-name, same idea.
 *
 *  4. Fallback – Ctrl (the universal default).
 */

void gtk_ui_on_font_increase(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (ctx->font_size < FONT_SIZE_MAX) {
        ctx->font_size++;
        reload_font_css(ctx);
        settings_get()->font_size = ctx->font_size;
        settings_save();
    }
}

void gtk_ui_on_font_decrease(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (ctx->font_size > FONT_SIZE_MIN) {
        ctx->font_size--;
        reload_font_css(ctx);
        settings_get()->font_size = ctx->font_size;
        settings_save();
    }
}

void gtk_ui_on_font_reset(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    ctx->font_size = FONT_SIZE_DEFAULT;
    reload_font_css(ctx);
    settings_get()->font_size = ctx->font_size;
    settings_save();
}

void gtk_ui_on_menu_filter(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (!ctx->filter_entry) return;
    if (gtk_widget_get_visible(ctx->filter_entry)) {
        filter_cancel_hide_timer(ctx);
        gtk_entry_set_text(GTK_ENTRY(ctx->filter_entry), "");
        ctx->filter_text[0] = '\0';
        if (ctx->show_gpu_only)
            rebuild_gpu_filter_store(ctx);
        else if (ctx->show_cpu_intensive)
            rebuild_cpu_intensive_filter_store(ctx);
        else if (ctx->show_audio_only)
            rebuild_audio_filter_store(ctx);
        else
            switch_to_real_store(ctx);
        gtk_widget_hide(ctx->filter_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
    } else {
        filter_cancel_hide_timer(ctx);
        gtk_widget_show(ctx->filter_entry);
        gtk_widget_grab_focus(ctx->filter_entry);
    }
}

void gtk_ui_on_menu_goto_pid(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_ctx_t *ctx = data;
    if (!ctx->pid_entry) return;
    if (gtk_widget_get_visible(ctx->pid_entry)) {
        gtk_entry_set_text(GTK_ENTRY(ctx->pid_entry), "");
        gtk_widget_hide(ctx->pid_entry);
        gtk_widget_grab_focus(GTK_WIDGET(ctx->view));
    } else {
        gtk_widget_show(ctx->pid_entry);
        gtk_widget_grab_focus(ctx->pid_entry);
    }
}

/* ── column visibility toggle ─────────────────────────────────── */

/*
 * Callback for View → Columns → <column name> check menu items.
 * Toggles the column's visibility and persists the set of visible
 * columns to settings.json.
 */
void gtk_ui_on_column_toggled(GtkCheckMenuItem *item, gpointer data)
{
    GtkTreeViewColumn *col = data;
    gboolean active = gtk_check_menu_item_get_active(item);
    if (g_object_get_data(G_OBJECT(col), "evemon-gpu-column"))
        g_object_set_data(G_OBJECT(col), "evemon-gpu-wanted",
                          GINT_TO_POINTER(active ? 1 : 0));
    gtk_tree_view_column_set_visible(col, active);

    /* Rebuild the settings columns[] list from the tree view */
    GtkTreeView *view = GTK_TREE_VIEW(
        gtk_tree_view_column_get_tree_view(col));
    GList *all_cols = gtk_tree_view_get_columns(view);

    evemon_settings_t *s = settings_get();
    s->column_count = 0;
    for (GList *c = all_cols; c; c = c->next) {
        GtkTreeViewColumn *tv_col = c->data;
        gboolean include = gtk_tree_view_column_get_visible(tv_col);
        if (g_object_get_data(G_OBJECT(tv_col), "evemon-gpu-column") &&
            !GPOINTER_TO_INT(g_object_get_data(
                G_OBJECT(tv_col), "evemon-gpu-available"))) {
            include = GPOINTER_TO_INT(g_object_get_data(
                G_OBJECT(tv_col), "evemon-gpu-wanted"));
        }
        if (include) {
            const char *title = gtk_tree_view_column_get_title(tv_col);
            if (title && s->column_count < SETTINGS_MAX_COLUMNS) {
                snprintf(s->columns[s->column_count],
                         SETTINGS_COL_NAME_MAX, "%s", title);
                s->column_count++;
            }
        }
    }
    g_list_free(all_cols);
    settings_save();
}

/*
 * Build the View → Columns submenu with a check item for each
 * tree view column.  Visibility is initialised from the current
 * column state (which was set from settings at startup).
 */
GtkWidget *gtk_ui_build_columns_submenu(
    GtkTreeView *view, GtkCheckMenuItem *gpu_items[3])
{
    GtkWidget *menu = gtk_menu_new();
    GList *cols = gtk_tree_view_get_columns(view);

    for (GList *c = cols; c; c = c->next) {
        GtkTreeViewColumn *tv_col = c->data;
        const char *title = gtk_tree_view_column_get_title(tv_col);
        if (!title || title[0] == '\0') continue;

        GtkWidget *mi = gtk_check_menu_item_new_with_label(title);
        int gpu_slot = GPOINTER_TO_INT(g_object_get_data(
            G_OBJECT(tv_col), "evemon-gpu-column")) - 1;
        gboolean active = gtk_tree_view_column_get_visible(tv_col);
        if (gpu_slot >= 0 && gpu_slot < 3) {
            active = GPOINTER_TO_INT(g_object_get_data(
                G_OBJECT(tv_col), "evemon-gpu-wanted"));
            gtk_widget_set_sensitive(mi, FALSE);
            gtk_widget_set_no_show_all(mi, TRUE);
            gtk_widget_hide(mi);
            if (gpu_items)
                gpu_items[gpu_slot] = GTK_CHECK_MENU_ITEM(mi);
        }
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), active);
        g_signal_connect(mi, "toggled",
                         G_CALLBACK(gtk_ui_on_column_toggled), tv_col);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    g_list_free(cols);
    return menu;
}

