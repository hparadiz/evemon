#ifndef EVEMON_UI_GTK_PRIVATE_H
#define EVEMON_UI_GTK_PRIVATE_H
#include "ui_internal.h"
#include "../app/runtime.h"
#include "store.h"

#include "../plugins/plugin_loader.h"
#include "../plugins/plugin_broker.h"
#include "../plugins/event_bus.h"
#include "../platform/fdmon_internal.h"
#include "../core/settings.h"

#ifdef HAVE_PIPEWIRE
#include "pipewire_graph.h"
#endif

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <utmpx.h>
#include <pwd.h>
#include <grp.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>

extern char **evemon_restart_argv;


#define SECTION_ARROW_EXPANDED "▼"
#define SECTION_TRANSITION_MS 200
typedef struct {
    ui_ctx_t       *ctx;
    spectro_theme_t theme;
} charting_theme_data_t;
typedef struct {
    ui_ctx_t *ctx;
} plugin_async_ctx_t;
extern const char *g_tab_order[];
extern const char *g_tab_order_last[];
extern charting_theme_data_t charting_theme_data[SPECTRO_THEME_COUNT];

void gtk_ui_ptree_nodes_gc(ptree_node_set_t *s, const proc_store_t *pstore);
gboolean gtk_ui_find_iter_by_pid_and_pinned_root(GtkTreeModel *model,
                                                  GtkTreeIter  *parent,
                                                  pid_t target,
                                                  pid_t pinned_root,
                                                  GtkTreeIter  *result);
void gtk_ui_on_toggle_audio_only(GtkCheckMenuItem *item, gpointer data);
void gtk_ui_on_toggle_cpu_intensive(GtkCheckMenuItem *item, gpointer data);
void gtk_ui_on_toggle_gpu_only(GtkCheckMenuItem *item, gpointer data);
GtkWidget *gtk_ui_append_audio_only_menu_item(ui_ctx_t *ctx, GtkWidget *menu);
GtkWidget *gtk_ui_append_cpu_intensive_menu_item(ui_ctx_t *ctx,
                                                  GtkWidget *menu);
GtkWidget *gtk_ui_append_gpu_only_menu_item(ui_ctx_t *ctx, GtkWidget *menu);
void gtk_ui_on_selection_changed(GtkTreeSelection *sel, gpointer data);
gboolean gtk_ui_on_detail_icon_draw(GtkWidget *widget, cairo_t *cr,
                                     gpointer data);
void gtk_ui_on_toggle_detail_panel(GtkCheckMenuItem *item, gpointer data);
void gtk_ui_on_proc_info_tray_toggle(GtkButton *btn, gpointer data);
void gtk_ui_on_panel_position_changed(GtkCheckMenuItem *item, gpointer data);
void gtk_ui_detail_panel_relayout(ui_ctx_t *ctx);
void gtk_ui_on_panel_map(GtkWidget *panel, gpointer data);
void gtk_ui_on_row_activated(GtkTreeView       *view,
                             GtkTreePath       *path,
                             GtkTreeViewColumn *col,
                             gpointer           data);
gboolean gtk_ui_on_tree_scroll_event(GtkWidget *widget, GdkEventScroll *ev,
                                     gpointer data);
void gtk_ui_desktop_open_context_init_once(void);
void gtk_ui_make_metadata_label_clickable(GtkWidget *button,
                                          GtkLabel  *label);
gboolean gtk_ui_metadata_link_from_main_click(ui_ctx_t *ctx,
                                              GdkEventButton *ev);
void gtk_ui_on_main_client_event_after(GtkWidget *widget,
                                       GdkEvent  *event,
                                       gpointer   data);
void gtk_ui_on_proc_meta(const evemon_event_t *ev, void *ud);
void gtk_ui_on_menu_exit(GtkMenuItem *item, gpointer data);
void gtk_ui_on_menu_restart(GtkMenuItem *item, gpointer data);
void gtk_ui_on_menu_restart_as_admin(GtkMenuItem *item, gpointer data);
void gtk_ui_format_gpu_bytes(uint64_t bytes, char *buf, size_t bufsz);
void gtk_ui_apply_gpu_snapshot_to_store(ui_ctx_t *ctx);
void gtk_ui_update_status_bar(ui_ctx_t *ctx);
gboolean gtk_ui_on_refresh(gpointer data);
void gtk_ui_apply_charting_theme(ui_ctx_t *ctx, spectro_theme_t theme);
void gtk_ui_on_charting_theme_selected(GtkCheckMenuItem *item, gpointer data);
GtkWidget *gtk_ui_build_charting_theme_submenu(ui_ctx_t *ctx,
                                               GSList **out_group);
GtkWidget *gtk_ui_build_theme_submenu(void);
void gtk_ui_on_font_increase(GtkMenuItem *item, gpointer data);
void gtk_ui_on_font_decrease(GtkMenuItem *item, gpointer data);
void gtk_ui_on_font_reset(GtkMenuItem *item, gpointer data);
void gtk_ui_on_menu_filter(GtkMenuItem *item, gpointer data);
void gtk_ui_on_menu_goto_pid(GtkMenuItem *item, gpointer data);
void gtk_ui_on_column_toggled(GtkCheckMenuItem *item, gpointer data);
GtkWidget *gtk_ui_build_columns_submenu(
    GtkTreeView *view, GtkCheckMenuItem *gpu_items[3]);
gboolean gtk_ui_on_status_button_press(GtkWidget *widget, GdkEventButton *ev,
                                       gpointer data);
void gtk_ui_on_destroy(GtkWidget *w, gpointer data);
void gtk_ui_highlight_cell_data_func(GtkTreeViewColumn *col,
                                     GtkCellRenderer   *cell,
                                     GtkTreeModel      *model,
                                     GtkTreeIter       *iter,
                                     gpointer           data);
int gtk_ui_host_monitor_fd_subscribe(void *host_ctx, pid_t pid, int fd);
void gtk_ui_host_monitor_fd_unsubscribe(void *host_ctx, pid_t pid, int fd);
int gtk_ui_host_monitor_watch_children(void *host_ctx, pid_t pid, int fd_mask);
void gtk_ui_host_monitor_unwatch_children(void *host_ctx, pid_t pid);
int gtk_ui_host_orphan_capture_enable(void *host_ctx);
void gtk_ui_host_orphan_capture_disable(void *host_ctx);
void gtk_ui_on_broker_complete_gtk(broker_cycle_t *cycle, void *user_data);
void gtk_ui_gtk_post_fn(int (*func)(void *), void *data);
void gtk_ui_on_plugin_loaded_gtk(plugin_registry_t *reg,
                                 int inst_id,
                                 void *user_data);
void gtk_ui_on_plugin_scan_done_gtk(int n_loaded, void *user_data);
void gtk_ui_on_broker_audio_pids(const pid_t *pids, size_t count,
                                 void *data);
void gtk_ui_name_cell_data_func(GtkTreeViewColumn *col,
                                GtkCellRenderer   *cell,
                                GtkTreeModel      *model,
                                GtkTreeIter       *iter,
                                gpointer           data);
void gtk_ui_host_pw_meter_start(void *host_ctx,
                                const uint32_t *node_ids, size_t count);
void gtk_ui_host_pw_meter_stop(void *host_ctx);
void gtk_ui_host_pw_meter_read(void *host_ctx, uint32_t node_id,
                               int *level_l, int *level_r);
void gtk_ui_host_pw_meter_remove_nodes(void *host_ctx,
                                       const uint32_t *node_ids,
                                       size_t count);
void gtk_ui_host_spectro_start(void *host_ctx, GtkDrawingArea *draw_area,
                               uint32_t node_id);
void gtk_ui_host_spectro_stop(void *host_ctx, GtkDrawingArea *draw_area);
uint32_t gtk_ui_host_spectro_get_target(void *host_ctx,
                                        GtkDrawingArea *draw_area);
void gtk_ui_host_spectro_set_theme(void *host_ctx,
                                   GtkDrawingArea *draw_area,
                                   unsigned theme_index);
void gtk_ui_host_set_charting_theme(void *host_ctx, unsigned theme_index);
void gtk_ui_host_charting_theme_notify(void *host_ctx,
                                       void (*cb)(void *plugin_ctx,
                                                  unsigned theme_index),
                                       void *plugin_ctx);
void gtk_ui_host_open_plugin_window(void *host_ctx,
                                    const char *plugin_id,
                                    pid_t pid,
                                    const char *proc_name);
gboolean gtk_ui_on_main_client_button_press(GtkWidget *widget,
                                            GdkEventButton *event,
                                            gpointer data);
void gtk_ui_on_menu_about(GtkMenuItem *item, gpointer data);
void gtk_ui_on_menu_compatibility(GtkMenuItem *item, gpointer data);
#endif
