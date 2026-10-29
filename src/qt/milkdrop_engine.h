#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "../plugins/evemon_plugin.h"
typedef struct _cairo cairo_t;
void *evemon_md_create(const char *preset_directory, const evemon_host_services_t *services);
void evemon_md_destroy(void *engine);
void evemon_md_initialize(void *engine);
void evemon_md_release_gl(void *engine);
void evemon_md_render(void *engine, unsigned framebuffer, int width, int height, float dt);
void evemon_md_overlay(void *engine, cairo_t *cr, int width, int height);
void evemon_md_update(void *engine, const evemon_proc_data_t *data);
void evemon_md_clear(void *engine);
int evemon_md_key(void *engine, unsigned key, unsigned modifiers);
int evemon_md_fullscreen(void *engine);
void evemon_md_next(void *engine, int direction);
void evemon_md_random(void *engine);
#ifdef __cplusplus
}
#endif
