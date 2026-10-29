#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#ifdef HAVE_GTK_WAYLAND_BLUR
#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#include "evemon-background-effect-client.h"
#include <stdint.h>
#include <string.h>

static struct ext_background_effect_manager_v1 *effects;
static struct wl_compositor *compositor;
static struct wl_event_queue *queue;
static gboolean initialized;

static void capabilities(void *data, struct ext_background_effect_manager_v1 *manager, uint32_t flags)
{ (void)data; (void)manager; (void)flags; }
static const struct ext_background_effect_manager_v1_listener effects_listener = { capabilities };
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(interface, "ext_background_effect_manager_v1")) {
        effects = wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1);
        ext_background_effect_manager_v1_add_listener(effects, &effects_listener, NULL);
    } else if (!strcmp(interface, "wl_compositor")) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    }
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{ (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = { global, global_remove };
static void release_blur(gpointer effect) { ext_background_effect_surface_v1_destroy(effect); }

static void wayland_blur(GdkWindow *window)
{
    // The installed Navi GTK module can already own this surface's effect.
    // The protocol permits only one effect object per wl_surface.
    if (g_object_get_data(G_OBJECT(window), "navi-blur") ||
        g_object_get_data(G_OBJECT(window), "evemon-blur")) return;
    struct wl_display *display = gdk_wayland_display_get_wl_display(gdk_window_get_display(window));
    if (!initialized) {
        initialized = TRUE;
        queue = wl_display_create_queue(display);
        struct wl_registry *registry = wl_display_get_registry(display);
        wl_proxy_set_queue((struct wl_proxy *)registry, queue);
        wl_registry_add_listener(registry, &registry_listener, NULL);
        wl_display_roundtrip_queue(display, queue);
        wl_registry_destroy(registry);
    }
    struct wl_surface *surface = gdk_wayland_window_get_wl_surface(window);
    if (!surface || !effects || !compositor) return;
    struct ext_background_effect_surface_v1 *effect =
        ext_background_effect_manager_v1_get_background_effect(effects, surface);
    struct wl_region *region = wl_compositor_create_region(compositor);
    wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
    ext_background_effect_surface_v1_set_blur_region(effect, region);
    wl_region_destroy(region);
    g_object_set_data_full(G_OBJECT(window), "evemon-blur", effect, release_blur);
    // Also advertise ownership to a settings-loaded Navi module that may run
    // after this callback. This alias is borrowed; evemon-blur owns the object.
    g_object_set_data(G_OBJECT(window), "navi-blur", effect);
    // This is double-buffered surface state; GTK commits it on the next paint.
    gdk_window_invalidate_rect(window, NULL, FALSE);
    wl_display_flush(display);
}
#endif

static gboolean apply_blur(gpointer data)
{
    GtkWidget *widget = data;
    g_object_set_data(G_OBJECT(widget), "evemon-blur-pending", NULL);
    if (!gtk_widget_get_mapped(widget)) return G_SOURCE_REMOVE;
    GdkWindow *window = gtk_widget_get_window(widget);
    if (!window || !gdk_window_has_native(window) ||
        gdk_window_get_type_hint(window) == GDK_WINDOW_TYPE_HINT_DND) return G_SOURCE_REMOVE;
    GdkDisplay *display = gdk_window_get_display(window);
    if (GDK_IS_X11_DISPLAY(display)) {
        Display *x = gdk_x11_display_get_xdisplay(display);
        Atom atom = XInternAtom(x, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
        // An empty CARDINAL region means the whole window, including resizes.
        XChangeProperty(x, gdk_x11_window_get_xid(window), atom, XA_CARDINAL,
                        32, PropModeReplace, NULL, 0);
        XFlush(x);
    }
#ifdef HAVE_GTK_WAYLAND_BLUR
    else if (GDK_IS_WAYLAND_DISPLAY(display)) wayland_blur(window);
#endif
    return G_SOURCE_REMOVE;
}

static gboolean mapped(GSignalInvocationHint *hint, guint count, const GValue *values, gpointer data)
{
    (void)hint; (void)count; (void)data;
    GtkWidget *widget = g_value_get_object(values);
    if (GTK_IS_WINDOW(widget) && !g_object_get_data(G_OBJECT(widget), "evemon-blur-pending")) {
        g_object_set_data(G_OBJECT(widget), "evemon-blur-pending", GINT_TO_POINTER(1));
        // Run after decoration/theme modules have finished mapping the window.
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, apply_blur, g_object_ref(widget), g_object_unref);
    }
    return TRUE;
}

static gboolean unmapped(GSignalInvocationHint *hint, guint count, const GValue *values, gpointer data)
{
    (void)hint; (void)count; (void)data;
    GtkWidget *widget = g_value_get_object(values);
    if (GTK_IS_WINDOW(widget)) {
        GdkWindow *window = gtk_widget_get_window(widget);
        if (window && g_object_get_data(G_OBJECT(window), "evemon-blur")) {
            g_object_set_data(G_OBJECT(window), "navi-blur", NULL);
            g_object_set_data(G_OBJECT(window), "evemon-blur", NULL);
        }
    }
    return TRUE;
}

void gtk_ui_blur_init(void)
{
    gpointer widget_class = g_type_class_ref(GTK_TYPE_WIDGET);
    g_signal_add_emission_hook(g_signal_lookup("map", GTK_TYPE_WIDGET), 0, mapped, NULL, NULL);
    g_signal_add_emission_hook(g_signal_lookup("map-event", GTK_TYPE_WIDGET), 0, mapped, NULL, NULL);
    g_signal_add_emission_hook(g_signal_lookup("unmap", GTK_TYPE_WIDGET), 0, unmapped, NULL, NULL);
    g_type_class_unref(widget_class);
}
