#ifndef EVEMON_RUNTIME_H
#define EVEMON_RUNTIME_H
#ifdef __cplusplus
extern "C" {
#endif
#include "../core/proc.h"
#include <signal.h>
extern struct timespec evemon_start_time;
extern int evemon_safe_mode;
extern char **evemon_restart_argv;
extern volatile sig_atomic_t evemon_shutdown_requested;
/* Frontends return to the launcher before restart so all workers are joined. */
extern int evemon_restart_requested;
const char *evemon_default_ui(void);
int evemon_data_path(const char *relative, char *path, size_t capacity);
int evemon_ui_available(const char *name);
int evemon_set_preferred_ui(const char *name);
void evemon_request_restart(void);
typedef int (*evemon_ui_run_fn)(monitor_state_t *, int, char **);
#ifdef __cplusplus
}
#endif
#endif
