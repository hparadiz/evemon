#include "runtime.h"
#include "../core/settings.h"
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
struct timespec evemon_start_time;
int evemon_safe_mode;
char **evemon_restart_argv;
volatile sig_atomic_t evemon_shutdown_requested;
int evemon_restart_requested;
const char *evemon_default_ui(void) { return EVEMON_DEFAULT_UI; }
int evemon_ui_available(const char *name)
{
    return name && ((!strcmp(name, "gtk") && EVEMON_HAVE_GTK) ||
                    (!strcmp(name, "qt") && EVEMON_HAVE_QT) ||
                    !strcmp(name, "headless"));
}
int evemon_set_preferred_ui(const char *name)
{
    if (!evemon_ui_available(name)) { errno = EINVAL; return -1; }
    evemon_settings_t *s = settings_get();
    char previous[sizeof(s->frontend)];
    memcpy(previous, s->frontend, sizeof(previous));
    snprintf(s->frontend, sizeof(s->frontend), "%s", name);
    if (settings_save() != 0) {
        memcpy(s->frontend, previous, sizeof(previous));
        return -1;
    }
    return 0;
}
void evemon_request_restart(void) { evemon_restart_requested = 1; }

int evemon_data_path(const char *relative, char *path, size_t capacity)
{
    char executable[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size > 0) {
        executable[size] = 0;
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = 0;
            int n = snprintf(path, capacity, "%s/share/evemon/%s", executable, relative);
            if (n >= 0 && (size_t)n < capacity && access(path, R_OK) == 0) return 0;
        }
    }
    int n = snprintf(path, capacity, "%s/%s", EVEMON_DATADIR, relative);
    if (n < 0 || (size_t)n >= capacity) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
