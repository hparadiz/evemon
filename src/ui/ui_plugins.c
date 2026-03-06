/*
 * ui_plugins.c – Floating plugin windows, Plugins menu, and host-service
 *                bridges for PipeWire and eBPF fd-monitoring.
 *
 * Public surface (declared in ui_internal.h):
 *   open_plugin_window()
 *   on_plugins_menu_map()   — connected as "show" signal on the menu
 */

#include "ui_internal.h"
#include "../plugin_loader.h"
#include "../plugin_broker.h"
#include "../settings.h"
#include "../fdmon_internal.h"

#include <dlfcn.h>
#include <dirent.h>

/* ── floating plugin window ─────────────────────────────────────── */

typedef struct {
    ui_ctx_t   *ctx;
    pid_t       pid;
    char        proc_name[256];
    char        plugin_id[SETTINGS_PLUGIN_ID_MAX];
} open_plugin_window_data_t;

/*
 * Called when the user closes a floating plugin window (delete-event,
 * fires BEFORE widget destruction).
 */
gboolean on_plugin_window_delete(GtkWidget *window, GdkEvent *ev,
                                 gpointer data)
{
    (void)ev;
    ui_ctx_t *ctx = data;
    plugin_registry_t *preg = ctx->plugin_registry;
    if (!preg) {
        return FALSE;
    }

    for (size_t i = 0; i < ctx->plugin_window_count; i++) {
        if (ctx->plugin_windows[i].window != window) continue;

        int inst_id = ctx->plugin_windows[i].instance_id;

        int idx = plugin_registry_find_by_id(preg, inst_id);
        if (idx >= 0 && preg->instances[idx].widget) {
            GtkWidget *pw = preg->instances[idx].widget;
            GtkWidget *parent = gtk_widget_get_parent(pw);
            if (parent)
                gtk_container_remove(GTK_CONTAINER(parent), pw);
        }

        plugin_instance_destroy(preg, inst_id);

        for (size_t j = i; j + 1 < ctx->plugin_window_count; j++)
            ctx->plugin_windows[j] = ctx->plugin_windows[j + 1];
        ctx->plugin_window_count--;

        gtk_widget_destroy(window);

        return TRUE;
    }

    return FALSE;
}

/*
 * Open a fresh instance of a plugin in its own GtkWindow, tracking `pid`.
 */
void open_plugin_window(ui_ctx_t *ctx, pid_t pid,
                        const char *proc_name,
                        const char *plugin_id)
{
    plugin_registry_t *preg = ctx->plugin_registry;
    if (!preg || !plugin_id) return;

    int inst_id = plugin_instance_create(preg, plugin_id);
    if (inst_id < 0) {
        fprintf(stderr, "evemon: open_plugin_window: failed to create instance "
                        "for %s\n", plugin_id);
        return;
    }

    int idx = plugin_registry_find_by_id(preg, inst_id);
    if (idx < 0) return;
    plugin_instance_t *inst = &preg->instances[idx];
    plugin_instance_set_pid(inst, pid, FALSE);

    if (!inst->widget) {
        plugin_instance_destroy(preg, inst_id);
        return;
    }

    plugin_instance_set_active(inst, FALSE);

    char title[512];
    const char *pname = inst->plugin && inst->plugin->name
                      ? inst->plugin->name : plugin_id;
    snprintf(title, sizeof(title), "%s — %s (pid %d)",
             pname, proc_name, (int)pid);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 480);

    if (ctx->plugin_css)
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(win),
            GTK_STYLE_PROVIDER(ctx->plugin_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(frame), inst->widget);
    gtk_container_add(GTK_CONTAINER(win), frame);

    if (ctx->plugin_window_count >= ctx->plugin_window_cap) {
        size_t newcap = ctx->plugin_window_cap ? ctx->plugin_window_cap * 2 : 8;
        plugin_window_t *tmp = realloc(ctx->plugin_windows,
                                       newcap * sizeof(plugin_window_t));
        if (!tmp) {
            plugin_instance_destroy(preg, inst_id);
            gtk_widget_destroy(win);
            return;
        }
        ctx->plugin_windows    = tmp;
        ctx->plugin_window_cap = newcap;
    }
    ctx->plugin_windows[ctx->plugin_window_count++] = (plugin_window_t){
        .instance_id = inst_id,
        .pid         = pid,
        .window      = win,
    };

    g_signal_connect(win, "delete-event",
                     G_CALLBACK(on_plugin_window_delete), ctx);

    gtk_widget_show_all(win);

    broker_start(preg, ctx->mon ? ctx->mon->fdmon : NULL);
}

static void on_open_plugin_window(GtkMenuItem *item, gpointer data)
{
    (void)item;
    open_plugin_window_data_t *d = data;
    open_plugin_window(d->ctx, d->pid, d->proc_name, d->plugin_id);
    free(d);
}

/* ── Plugins menu helpers ──────────────────────────────────────── */

/*
 * Data bag passed to each per-plugin checkbox and Reload item.
 * Allocated with g_new0, freed via GClosureNotify(g_free).
 */
typedef struct {
    ui_ctx_t   *ctx;
    char        plugin_id[SETTINGS_PLUGIN_ID_MAX];
    char        so_path[4096];
} plugin_menu_item_data_t;

static void plugin_menu_data_free(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

void on_plugin_toggled(GtkCheckMenuItem *item, gpointer data)
{
    plugin_menu_item_data_t *d = data;
    ui_ctx_t *ctx = d->ctx;
    gboolean active = gtk_check_menu_item_get_active(item);

    if (!ctx || !ctx->plugin_registry || d->so_path[0] == '\0')
        return;

    plugin_registry_t *preg = ctx->plugin_registry;

    if (active) {
        if (d->plugin_id[0] == '\0') {
            void *h = dlopen(d->so_path, RTLD_NOW | RTLD_LOCAL);
            if (h) {
                evemon_plugin_init_fn init_fn =
                    (evemon_plugin_init_fn)dlsym(h, "evemon_plugin_init");
                if (init_fn) {
                    evemon_plugin_t *p = init_fn();
                    if (p && p->id)
                        snprintf(d->plugin_id, sizeof(d->plugin_id),
                                 "%s", p->id);
                    if (p && p->destroy) p->destroy(p->plugin_ctx);
                    else free(p);
                }
                dlclose(h);
            }
        }

        settings_plugin_set_enabled(d->plugin_id[0] ? d->plugin_id : NULL,
                                    true);

        plugin_reload(preg, d->so_path);

        GtkWidget *notebook = ctx->plugin_notebook;
        if (notebook) {
            for (size_t i = 0; i < preg->count; i++) {
                plugin_instance_t *inst = &preg->instances[i];
                if (!inst->widget || !inst->plugin || !inst->plugin->id)
                    continue;
                if (strcmp(inst->so_path, d->so_path) != 0) continue;
                if (!PLUGIN_IS_AVAILABLE(inst)) continue;
                gboolean found = FALSE;
                int npages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
                for (int p = 0; p < npages; p++) {
                    if (gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), p)
                            == inst->widget) {
                        found = TRUE; break;
                    }
                }
                if (!found) {
                    const char *lbl = inst->plugin->name
                                    ? inst->plugin->name
                                    : inst->plugin->id;
                    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                        inst->widget, gtk_label_new(lbl));
                    plugin_instance_set_active(inst, TRUE);
                }
            }
            gtk_widget_show_all(notebook);
        }

    } else {
        int ids[256];
        size_t n_ids = 0;
        void *handle = NULL;
        for (size_t i = 0; i < preg->count; i++) {
            if (strcmp(preg->instances[i].so_path, d->so_path) != 0) continue;
            if (d->plugin_id[0] == '\0' && preg->instances[i].plugin &&
                preg->instances[i].plugin->id)
                snprintf(d->plugin_id, sizeof(d->plugin_id),
                         "%s", preg->instances[i].plugin->id);
            if (n_ids < 256)
                ids[n_ids++] = preg->instances[i].instance_id;
            if (!handle)
                handle = preg->instances[i].handle;
        }

        GtkWidget *notebook = ctx->plugin_notebook;
        if (notebook) {
            for (size_t i = 0; i < preg->count; i++) {
                if (strcmp(preg->instances[i].so_path, d->so_path) != 0)
                    continue;
                if (!preg->instances[i].widget) continue;
                int page = gtk_notebook_page_num(GTK_NOTEBOOK(notebook),
                                                 preg->instances[i].widget);
                if (page >= 0)
                    gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page);
            }
        }

        for (size_t i = 0; i < n_ids; i++)
            plugin_instance_destroy(preg, ids[i]);

        if (handle)
            dlclose(handle);

        settings_plugin_set_enabled(d->plugin_id[0] ? d->plugin_id : NULL,
                                    false);
    }
}

typedef struct { char path[4096]; char name[256]; } so_entry_t;

static so_entry_t *scan_so_files(const char *dir, size_t *out_count)
{
    *out_count = 0;
    DIR *dp = opendir(dir);
    if (!dp) return NULL;

    size_t cap = 16, count = 0;
    so_entry_t *entries = calloc(cap, sizeof(so_entry_t));
    if (!entries) { closedir(dp); return NULL; }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len < 4 || strcmp(de->d_name + len - 3, ".so") != 0) continue;
        if (count >= cap) {
            cap *= 2;
            so_entry_t *tmp = realloc(entries, cap * sizeof(so_entry_t));
            if (!tmp) break;
            entries = tmp;
        }
        snprintf(entries[count].path, sizeof(entries[count].path),
                 "%s/%s", dir, de->d_name);
        const char *n = de->d_name;
        if (strncmp(n, "lib", 3) == 0) n += 3;
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", n);
        char *dot = strrchr(entries[count].name, '.');
        if (dot) *dot = '\0';
        count++;
    }
    closedir(dp);
    *out_count = count;
    return entries;
}

/*
 * Rebuilds the Plugins submenu from scratch every time it's opened,
 * so new .so files in the plugin directory appear immediately.
 */
void on_plugins_menu_map(GtkWidget *menu, gpointer data)
{
    static gboolean rebuilding = FALSE;
    if (rebuilding) return;
    rebuilding = TRUE;

    ui_ctx_t *ctx = data;

    if (!ctx || !ctx->plugin_registry) {
        rebuilding = FALSE;
        return;
    }

    plugin_registry_t *preg = ctx->plugin_registry;

    GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    if (ctx->plugin_dir[0] == '\0') {
        GtkWidget *mi = gtk_menu_item_new_with_label("(no plugin directory)");
        gtk_widget_set_sensitive(mi, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        gtk_widget_show_all(menu);
        rebuilding = FALSE;
        return;
    }

    size_t n_on_disk = 0;
    so_entry_t *on_disk = scan_so_files(ctx->plugin_dir, &n_on_disk);

    if (n_on_disk == 0) {
        GtkWidget *mi = gtk_menu_item_new_with_label("(no plugins found)");
        gtk_widget_set_sensitive(mi, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        free(on_disk);
        gtk_widget_show_all(menu);
        rebuilding = FALSE;
        return;
    }

    for (size_t s = 0; s < n_on_disk; s++) {
        const char *so_path = on_disk[s].path;
        const char *display = on_disk[s].name;

        const char *plugin_id   = NULL;
        const char *plugin_name = NULL;
        gboolean    is_new      = TRUE;

        for (size_t i = 0; i < preg->count; i++) {
            if (strcmp(preg->instances[i].so_path, so_path) == 0) {
                is_new = FALSE;
                if (preg->instances[i].plugin) {
                    plugin_id   = preg->instances[i].plugin->id;
                    plugin_name = preg->instances[i].plugin->name;
                }
                break;
            }
        }

        const char *label = plugin_name ? plugin_name
                          : plugin_id   ? plugin_id
                          :               display;

        gboolean enabled = is_new ? FALSE
                         : plugin_id ? (gboolean)settings_plugin_enabled(plugin_id)
                         : TRUE;

        GtkWidget *chk = gtk_check_menu_item_new_with_label(label);

        plugin_menu_item_data_t *chk_data = g_new0(plugin_menu_item_data_t, 1);
        chk_data->ctx = ctx;
        if (plugin_id)
            snprintf(chk_data->plugin_id, sizeof(chk_data->plugin_id),
                     "%s", plugin_id);
        snprintf(chk_data->so_path, sizeof(chk_data->so_path), "%s", so_path);
        g_signal_connect_data(chk, "toggled",
            G_CALLBACK(on_plugin_toggled),
            chk_data, plugin_menu_data_free, 0);

        g_signal_handlers_block_by_func(chk,
            G_CALLBACK(on_plugin_toggled), chk_data);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(chk), enabled);
        g_signal_handlers_unblock_by_func(chk,
            G_CALLBACK(on_plugin_toggled), chk_data);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), chk);
    }
    free(on_disk);
    gtk_widget_show_all(menu);
    rebuilding = FALSE;
}

/* ── plugin ordering helper ──────────────────────────────────── */

/* Helper: check if a plugin is in a load-last list */
int inst_is_last_order(const evemon_plugin_t *p, const char *list[])
{
    if (!p || !p->id) return 0;
    for (int i = 0; list[i]; i++)
        if (strcmp(p->id, list[i]) == 0) return 1;
    return 0;
}

/* ── show-process context menu: Open Plugin as Window entry ─── */

void show_open_plugin_as_window_menu(ui_ctx_t *ctx, GtkWidget *menu,
                                     pid_t pid, const char *name)
{
    if (!ctx->plugin_registry) return;

    plugin_registry_t *preg = ctx->plugin_registry;
    GtkWidget *pw_menu = gtk_menu_new();
    gboolean   any     = FALSE;

    for (size_t i = 0; i < preg->count; i++) {
        plugin_instance_t *inst = &preg->instances[i];
        if (!inst->plugin || !inst->plugin->id) continue;
        if (inst->plugin->kind == EVEMON_PLUGIN_HEADLESS) continue;
        if (!inst->widget) continue;
        if (!PLUGIN_IS_AVAILABLE(inst)) continue;

        const char *lbl = inst->plugin->name ? inst->plugin->name
                                             : inst->plugin->id;
        GtkWidget *mi_pw = gtk_menu_item_new_with_label(lbl);

        open_plugin_window_data_t *wd =
            malloc(sizeof(open_plugin_window_data_t));
        if (wd) {
            wd->ctx = ctx;
            wd->pid = pid;
            snprintf(wd->proc_name, sizeof(wd->proc_name), "%s", name);
            snprintf(wd->plugin_id, sizeof(wd->plugin_id),
                     "%s", inst->plugin->id);
            g_signal_connect_data(mi_pw, "activate",
                G_CALLBACK(on_open_plugin_window), wd,
                (GClosureNotify)(void(*)(void))free, 0);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(pw_menu), mi_pw);
        any = TRUE;
    }

    if (any) {
        GtkWidget *pw_item =
            gtk_menu_item_new_with_label("Open Plugin as Window");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(pw_item), pw_menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), pw_item);
    } else {
        gtk_widget_destroy(pw_menu);
    }
}
