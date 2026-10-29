#pragma once
#include "../plugins/evemon_plugin.h"
#include "../core/log.h"
#include <stdint.h>
#define SPECTRO_MAX_INSTANCES 16
typedef enum {
    SPECTRO_THEME_CLASSIC, SPECTRO_THEME_HEAT, SPECTRO_THEME_COOL,
    SPECTRO_THEME_GREYSCALE, SPECTRO_THEME_NEON, SPECTRO_THEME_VIRIDIS,
    SPECTRO_NUM_THEMES
} spectro_theme_t;
typedef struct {
    void *pw_meter;
    guint pw_meter_timer;
    struct { GtkWidget *draw_area; void *state; } spectro_instances[SPECTRO_MAX_INSTANCES];
    size_t spectro_count;
    void (*theme_notify)(void *, unsigned);
    void *theme_notify_ctx;
    void (*open_window)(void *, const char *, pid_t, const char *);
    void *owner;
} ui_ctx_t;
void pw_meter_start(ui_ctx_t *, const uint32_t *, size_t);
void pw_meter_stop(ui_ctx_t *);
void pw_meter_read(ui_ctx_t *, uint32_t, int *, int *);
void pw_meter_remove_nodes(ui_ctx_t *, const uint32_t *, size_t);
void spectrogram_start_for_node(ui_ctx_t *, GtkDrawingArea *, uint32_t);
void spectrogram_stop(ui_ctx_t *, GtkDrawingArea *);
uint32_t spectrogram_get_target_node(ui_ctx_t *, GtkDrawingArea *);
void spectrogram_set_theme(ui_ctx_t *, GtkDrawingArea *, spectro_theme_t);
void *evemon_qt_audio_services(evemon_host_services_t *, void *, void (*)(void *, const char *, pid_t, const char *));
void evemon_qt_audio_destroy(void *);
void evemon_qt_audio_theme(unsigned);
