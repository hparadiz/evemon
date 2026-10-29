#include "audio_services_native.h"
#include "../core/settings.h"
#include "../plugins/event_bus.h"
static GList *contexts;
static void meter_start(void *ctx, const uint32_t *nodes, size_t count) { pw_meter_start(ctx, nodes, count); }
static void meter_stop(void *ctx) { pw_meter_stop(ctx); }
static void meter_read(void *ctx, uint32_t node, int *l, int *r) { pw_meter_read(ctx, node, l, r); }
static void meter_remove(void *ctx, const uint32_t *nodes, size_t count) { pw_meter_remove_nodes(ctx, nodes, count); }
static void spectro_start(void *ctx, GtkDrawingArea *area, uint32_t node) { spectrogram_start_for_node(ctx, area, node); }
static void spectro_stop(void *ctx, GtkDrawingArea *area) { spectrogram_stop(ctx, area); }
static uint32_t spectro_target(void *ctx, GtkDrawingArea *area) { return spectrogram_get_target_node(ctx, area); }
static void spectro_theme(void *ctx, GtkDrawingArea *area, unsigned theme) { spectrogram_set_theme(ctx, area, theme); }
void evemon_qt_audio_theme(unsigned theme)
{
    if (theme >= SPECTRO_NUM_THEMES) return;
    settings_get()->spectro_theme = theme;
    for (GList *node = contexts; node; node = node->next) {
        ui_ctx_t *ctx = node->data;
        for (size_t i = 0; i < ctx->spectro_count; ++i)
            spectrogram_set_theme(ctx, ctx->spectro_instances[i].draw_area, theme);
        if (ctx->theme_notify) ctx->theme_notify(ctx->theme_notify_ctx, theme);
    }
}
static void set_theme(void *ctx, unsigned theme) { (void)ctx; evemon_qt_audio_theme(theme); }
static void notify_theme(void *opaque, void (*callback)(void *, unsigned), void *data)
{ ui_ctx_t *ctx = opaque; ctx->theme_notify = callback; ctx->theme_notify_ctx = data; }
static void open_window(void *opaque, const char *id, pid_t pid, const char *name)
{ ui_ctx_t *ctx = opaque; if (ctx->open_window) ctx->open_window(ctx->owner, id, pid, name); }
void *evemon_qt_audio_services(evemon_host_services_t *services, void *owner, void (*open)(void *, const char *, pid_t, const char *))
{
    ui_ctx_t *ctx = g_new0(ui_ctx_t, 1); ctx->owner = owner; ctx->open_window = open;
    contexts = g_list_prepend(contexts, ctx);
    *services = (evemon_host_services_t){
        .host_ctx = ctx, .pw_meter_start = meter_start, .pw_meter_stop = meter_stop,
        .pw_meter_read = meter_read, .pw_meter_remove_nodes = meter_remove,
        .spectro_start = spectro_start, .spectro_stop = spectro_stop,
        .spectro_get_target = spectro_target, .spectro_set_theme = spectro_theme,
        .set_charting_theme = set_theme, .charting_theme_notify = notify_theme,
        .subscribe = host_event_subscribe, .publish = host_event_publish, .unsubscribe = host_event_unsubscribe,
        .open_plugin_window = open_window
    };
    return ctx;
}
void evemon_qt_audio_destroy(void *opaque)
{
    ui_ctx_t *ctx = opaque; if (!ctx) return;
    while (ctx->spectro_count) spectrogram_stop(ctx, ctx->spectro_instances[0].draw_area);
    pw_meter_stop(ctx); contexts = g_list_remove(contexts, ctx); g_free(ctx);
}
