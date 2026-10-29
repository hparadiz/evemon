#pragma once
/* Native Qt implementation of the small widget vocabulary used by Evemon's
 * original process inspectors. Their presentation and update code stays C. */
#ifdef __cplusplus
#pragma push_macro("signals")
#undef signals
#endif
#include <glib.h>
#include <glib-object.h>
#include <pango/pangocairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#ifdef __cplusplus
#pragma pop_macro("signals")
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef void GtkWidget;
typedef void GtkDrawingArea;
typedef void GtkTreeStore;
typedef void GtkTreeModel;
typedef void GtkTreeView;
typedef void GtkTreeViewColumn;
typedef void GtkCellRenderer;
typedef void GtkToggleButton;
typedef void GtkAdjustment;
typedef void GtkGrid;
typedef void GtkLabel;
typedef void GtkTooltip;
typedef void GtkListStore;
typedef void GtkEntry;
typedef void GtkTreeSelection;
#define GTK_ENTRY(p) ((GtkEntry *)(p))
#define GTK_PANED(p) (p)
#define GTK_SELECTION_SINGLE 1
GtkListStore *gtk_list_store_new(int count, ...);
#define gtk_list_store_set gtk_tree_store_set
#define gtk_list_store_clear gtk_tree_store_clear
#define gtk_list_store_remove gtk_tree_store_remove
#define gtk_list_store_append(p, i) gtk_tree_store_append(p, i, NULL)
GtkWidget *gtk_search_entry_new(void);
void gtk_entry_set_placeholder_text(GtkEntry *, const char *);
const char *gtk_entry_get_text(GtkEntry *);
GtkWidget *gtk_paned_new(int);
void gtk_paned_pack1(void *, GtkWidget *, gboolean, gboolean);
void gtk_paned_pack2(void *, GtkWidget *, gboolean, gboolean);
void gtk_paned_set_position(void *, int);
void gtk_widget_set_no_show_all(GtkWidget *, gboolean);
typedef struct { void *item; } GtkTreeIter;
typedef struct { void *item; } GtkTreePath;
#define GTK_WIDGET(p) ((GtkWidget *)(p))
#define GTK_TREE_MODEL(p) ((GtkTreeModel *)(p))
#define GTK_TREE_VIEW(p) ((GtkTreeView *)(p))
#define GTK_SCROLLED_WINDOW(p) (p)
#define GTK_CONTAINER(p) (p)
#define GTK_BOX(p) (p)
#define GTK_GRID(p) ((GtkGrid *)(p))
#define GTK_STACK(p) (p)
#define GTK_LABEL(p) ((GtkLabel *)(p))
#define GTK_TOGGLE_BUTTON(p) ((GtkToggleButton *)(p))
#define GTK_IS_WIDGET(p) ((p) != NULL)
#define GTK_ORIENTATION_HORIZONTAL 0
#define GTK_ORIENTATION_VERTICAL 1
#define GTK_POLICY_AUTOMATIC 0
#define GTK_ALIGN_START 0

typedef void GtkComboBox;
typedef void GtkCssProvider;
typedef void GdkScreen;
typedef struct { double x, y; } GdkEventMotion;
typedef struct { int unused; } GdkEventCrossing;
typedef struct { unsigned keyval; } GdkEventKey;
typedef struct { int x, y, width, height; } GtkAllocation;
typedef GtkAllocation GdkRectangle;
#define GDK_KEY_Return 0xff0d
#define GDK_KEY_KP_Enter 0xff8d
#define GDK_POINTER_MOTION_MASK 1
#define GDK_LEAVE_NOTIFY_MASK 2
#define GTK_COMBO_BOX(p) ((GtkComboBox *)(p))
#define GTK_COMBO_BOX_TEXT(p) (p)
#define GTK_PROGRESS_BAR(p) (p)
#define GTK_STYLE_PROVIDER(p) (p)
#define GTK_STYLE_PROVIDER_PRIORITY_APPLICATION 0
#undef g_signal_handlers_disconnect_by_func
#define g_signal_handlers_disconnect_by_func(p, cb, data) ((void)0)
GtkWidget *gtk_drawing_area_new(void);
void gtk_widget_queue_draw(GtkWidget *);
int gtk_widget_get_allocated_width(GtkWidget *);
int gtk_widget_get_allocated_height(GtkWidget *);
void gtk_widget_add_events(GtkWidget *, int);
void gtk_widget_set_hexpand(GtkWidget *, gboolean);
void gtk_widget_set_size_request(GtkWidget *, int, int);
void gtk_widget_set_sensitive(GtkWidget *, gboolean);
PangoContext *gtk_widget_get_pango_context(GtkWidget *);
GdkScreen *gtk_widget_get_screen(GtkWidget *);
double gdk_screen_get_resolution(GdkScreen *);
GtkWidget *gtk_combo_box_text_new(void);
void gtk_combo_box_text_append_text(void *, const char *);
void gtk_combo_box_text_append(void *, const char *, const char *);
void gtk_combo_box_text_remove_all(void *);
void gtk_combo_box_set_active(GtkComboBox *, int);
int gtk_combo_box_get_active(GtkComboBox *);
GtkWidget *gtk_event_box_new(void);
GtkWidget *gtk_frame_new(const char *);
void gtk_container_set_border_width(void *, int);
GtkWidget *gtk_separator_new(int);
GtkWidget *gtk_progress_bar_new(void);
void gtk_progress_bar_set_fraction(void *, double);
void gtk_progress_bar_set_show_text(void *, gboolean);
void gtk_label_set_markup(GtkLabel *, const char *);
void gtk_label_set_ellipsize(GtkLabel *, int);
void gtk_box_pack_end(void *, GtkWidget *, gboolean, gboolean, guint);
gboolean gtk_tree_view_row_expanded(GtkTreeView *, GtkTreePath *);
void gtk_tree_view_set_enable_tree_lines(GtkTreeView *, gboolean);
GtkCssProvider *gtk_css_provider_new(void);
void gtk_css_provider_load_from_data(GtkCssProvider *, const char *, gssize, void *);
void *gtk_widget_get_style_context(GtkWidget *);
void gtk_style_context_add_provider(void *, void *, int);
void gtk_style_context_add_class(void *, const char *);
typedef void GtkTextBuffer;
#define GTK_FRAME(p) (p)
#define GTK_TEXT_VIEW(p) (p)
#define GTK_FLOW_BOX(p) (p)
#define GTK_POLICY_NEVER 1
#define GTK_SHADOW_NONE 0
#define GTK_SELECTION_NONE 0
#define GTK_WRAP_WORD_CHAR 1
void gtk_frame_set_shadow_type(void *, int);
void gtk_label_set_line_wrap(GtkLabel *, gboolean);
void gtk_label_set_max_width_chars(GtkLabel *, int);
GtkWidget *gtk_flow_box_new(void);
void gtk_flow_box_set_selection_mode(void *, int);
void gtk_flow_box_set_min_children_per_line(void *, int);
void gtk_flow_box_set_max_children_per_line(void *, int);
void gtk_flow_box_set_row_spacing(void *, int);
void gtk_flow_box_set_column_spacing(void *, int);
GList *gtk_container_get_children(void *);
void gtk_widget_destroy(GtkWidget *);
void gtk_widget_set_tooltip_text(GtkWidget *, const char *);
void gtk_scrolled_window_set_min_content_height(void *, int);
GtkWidget *gtk_text_view_new(void);
void gtk_text_view_set_editable(void *, gboolean);
void gtk_text_view_set_cursor_visible(void *, gboolean);
void gtk_text_view_set_wrap_mode(void *, int);
GtkTextBuffer *gtk_text_view_get_buffer(void *);
void gtk_text_buffer_set_text(GtkTextBuffer *, const char *, int);
typedef void GtkMenuItem;
typedef void GtkCheckMenuItem;
typedef struct { int type; unsigned button; double x, y; } GdkEventButton;
typedef void GdkEvent;
#define GDK_BUTTON_PRESS 1
#define GDK_BUTTON_PRESS_MASK 4
#define GTK_ALIGN_CENTER 1
#define GTK_TREE_VIEW_COLUMN_FIXED 1
#define GTK_DRAWING_AREA(p) ((GtkDrawingArea *)(p))
#define GTK_CHECK_MENU_ITEM(p) ((GtkCheckMenuItem *)(p))
#define GTK_RADIO_MENU_ITEM(p) (p)
#define GTK_IMAGE(p) (p)
#define GTK_MENU(p) (p)
#define GTK_MENU_SHELL(p) (p)
#define GTK_IS_IMAGE(p) qt_inspector_is_label(p)
#define GTK_IS_LABEL(p) qt_inspector_is_label(p)
#define GTK_IS_TREE_MODEL(p) ((p) != NULL)
#undef g_object_ref_sink
#define g_object_ref_sink(p) qt_inspector_ref_sink(p)
#define g_signal_handler_disconnect(p, id) qt_inspector_disconnect(p, id)
#undef g_signal_handlers_block_by_func
#undef g_signal_handlers_unblock_by_func
#define g_signal_handlers_block_by_func(p, cb, data) qt_inspector_block(p, TRUE)
#define g_signal_handlers_unblock_by_func(p, cb, data) qt_inspector_block(p, FALSE)
gboolean qt_inspector_is_label(void *);
void *qt_inspector_ref_sink(void *);
void qt_inspector_disconnect(void *, gulong);
void qt_inspector_block(void *, gboolean);
GtkWidget *gtk_image_new(void);
void gtk_image_clear(void *);
void gtk_image_set_from_pixbuf(void *, GdkPixbuf *);
void gtk_label_set_use_markup(GtkLabel *, gboolean);
void gtk_tree_view_column_set_fixed_width(GtkTreeViewColumn *, int);
void gtk_tree_view_column_set_sizing(GtkTreeViewColumn *, int);
GtkTreeModel *gtk_tree_view_get_model(GtkTreeView *);
gboolean gtk_tree_view_get_path_at_pos(GtkTreeView *, int, int, GtkTreePath **, void *, int *, int *);
gboolean gtk_tree_model_get_iter(GtkTreeModel *, GtkTreeIter *, GtkTreePath *);
void gtk_tree_selection_select_iter(GtkTreeSelection *, GtkTreeIter *);
GtkWidget *gtk_menu_new(void);
GtkWidget *gtk_menu_item_new_with_label(const char *);
GtkWidget *gtk_separator_menu_item_new(void);
GtkWidget *gtk_radio_menu_item_new_with_label(GSList *, const char *);
GSList *gtk_radio_menu_item_get_group(void *);
void gtk_check_menu_item_set_active(GtkCheckMenuItem *, gboolean);
gboolean gtk_check_menu_item_get_active(GtkCheckMenuItem *);
void gtk_menu_shell_append(void *, GtkWidget *);
void gtk_menu_popup_at_pointer(void *, GdkEvent *);
void qt_inspector_object_set(void *, const char *, ...);
void qt_inspector_unref(void *);
gulong qt_inspector_connect(void *, const char *, GCallback, void *);
#undef g_signal_connect
#define g_signal_connect qt_inspector_connect
#define g_object_set qt_inspector_object_set
#define g_object_unref qt_inspector_unref
GtkWidget *gtk_box_new(int orientation, int spacing);
void gtk_box_pack_start(void *, GtkWidget *, gboolean expand, gboolean fill, guint padding);
GtkWidget *gtk_check_button_new_with_label(const char *);
void gtk_toggle_button_set_active(GtkToggleButton *, gboolean);
gboolean gtk_toggle_button_get_active(GtkToggleButton *);
GtkWidget *gtk_scrolled_window_new(void *, void *);
void gtk_scrolled_window_set_policy(void *, int, int);
void gtk_container_add(void *, GtkWidget *);
GtkAdjustment *gtk_scrolled_window_get_vadjustment(void *);
double gtk_adjustment_get_value(GtkAdjustment *);
void gtk_adjustment_set_value(GtkAdjustment *, double);
GtkWidget *gtk_grid_new(void);
void gtk_grid_set_column_spacing(GtkGrid *, int);
void gtk_grid_set_row_spacing(GtkGrid *, int);
void gtk_grid_attach(GtkGrid *, GtkWidget *, int, int, int, int);
GtkWidget *gtk_grid_get_child_at(GtkGrid *, int, int);
GtkWidget *gtk_label_new(const char *);
void gtk_label_set_text(GtkLabel *, const char *);
void gtk_label_set_xalign(GtkLabel *, float);
void gtk_label_set_selectable(GtkLabel *, gboolean);
GtkWidget *gtk_stack_new(void);
void gtk_stack_add_named(void *, GtkWidget *, const char *);
void gtk_stack_set_visible_child_name(void *, const char *);
void gtk_widget_set_margin_start(GtkWidget *, int);
void gtk_widget_set_margin_end(GtkWidget *, int);
void gtk_widget_set_margin_top(GtkWidget *, int);
void gtk_widget_set_margin_bottom(GtkWidget *, int);
void gtk_widget_set_halign(GtkWidget *, int);
void gtk_widget_set_valign(GtkWidget *, int);
void gtk_widget_set_vexpand(GtkWidget *, gboolean);
void gtk_widget_set_has_tooltip(GtkWidget *, gboolean);
void gtk_widget_show(GtkWidget *);
void gtk_widget_hide(GtkWidget *);
void gtk_widget_show_all(GtkWidget *);
gboolean gtk_widget_get_mapped(GtkWidget *);
gboolean gtk_widget_get_child_visible(GtkWidget *);
GtkTreeStore *gtk_tree_store_new(int count, ...);
void gtk_tree_store_append(GtkTreeStore *, GtkTreeIter *, GtkTreeIter *);
void gtk_tree_store_set(GtkTreeStore *, GtkTreeIter *, ...);
void gtk_tree_store_clear(GtkTreeStore *);
gboolean gtk_tree_store_remove(GtkTreeStore *, GtkTreeIter *);
gboolean gtk_tree_model_iter_children(GtkTreeModel *, GtkTreeIter *, GtkTreeIter *);
gboolean gtk_tree_model_iter_next(GtkTreeModel *, GtkTreeIter *);
void gtk_tree_model_get(GtkTreeModel *, GtkTreeIter *, ...);
GtkTreePath *gtk_tree_model_get_path(GtkTreeModel *, GtkTreeIter *);
void gtk_tree_path_free(GtkTreePath *);
GtkWidget *gtk_tree_view_new_with_model(GtkTreeModel *);
void gtk_tree_view_set_headers_visible(GtkTreeView *, gboolean);
void gtk_tree_view_set_enable_search(GtkTreeView *, gboolean);
GtkCellRenderer *gtk_cell_renderer_text_new(void);
GtkTreeViewColumn *gtk_tree_view_column_new_with_attributes(const char *, GtkCellRenderer *, ...);
void gtk_tree_view_append_column(GtkTreeView *, GtkTreeViewColumn *);
void gtk_tree_view_column_set_resizable(GtkTreeViewColumn *, gboolean);
void gtk_tree_view_column_set_expand(GtkTreeViewColumn *, gboolean);
void gtk_tree_view_column_set_min_width(GtkTreeViewColumn *, int);
GtkTreeSelection *gtk_tree_view_get_selection(GtkTreeView *);
void gtk_tree_selection_set_mode(GtkTreeSelection *, int);
gboolean gtk_tree_selection_get_selected(GtkTreeSelection *, GtkTreeModel **, GtkTreeIter *);
gboolean gtk_tree_model_get_iter_first(GtkTreeModel *, GtkTreeIter *);
void gtk_tree_view_expand_row(GtkTreeView *, GtkTreePath *, gboolean);
void gtk_tree_view_collapse_row(GtkTreeView *, GtkTreePath *);
gboolean gtk_tree_view_get_tooltip_context(GtkTreeView *, int *, int *, gboolean, GtkTreeModel **, GtkTreePath **, GtkTreeIter *);
void gtk_tree_view_set_tooltip_row(GtkTreeView *, GtkTooltip *, GtkTreePath *);
void gtk_tooltip_set_text(GtkTooltip *, const char *);
#ifdef __cplusplus
}
#endif
