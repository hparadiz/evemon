#include "../evemon_plugin.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <glib.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.write_monitor",
    "Write Monitor",
    "1.0",
    EVEMON_ROLE_SERVICE,
    NULL
);

typedef struct {
    const evemon_host_services_t *hsvc;
    int sub_pid_select_id;
    int sub_fd_write_id;
    pid_t monitored_pid;
    int monitor_fd_1; /* bool: subscribed to fd 1 */
    int monitor_fd_2; /* bool: subscribed to fd 2 */
} wm_ctx_t;

static void on_fd_write(const evemon_event_t *ev, void *user_data)
{
    (void)ev;
    (void)user_data;
    /* Write events are handled by the UI plugin; nothing to do here. */
}

static void on_process_selected(const evemon_event_t *ev, void *user_data)
{
    wm_ctx_t *c = user_data;
    if (!c || !c->hsvc) return;

    if (!ev || ev->type != EVEMON_EVENT_PROCESS_SELECTED) return;
    pid_t sel = ev->payload ? *(pid_t *)ev->payload : 0;

    /* Unsubscribe previous monitored fds if any */
    if (c->monitored_pid != 0) {
        if (c->monitor_fd_1) {
            c->hsvc->monitor_fd_unsubscribe(c->hsvc->host_ctx, c->monitored_pid, 1);
            c->monitor_fd_1 = 0;
        }
        if (c->monitor_fd_2) {
            c->hsvc->monitor_fd_unsubscribe(c->hsvc->host_ctx, c->monitored_pid, 2);
            c->monitor_fd_2 = 0;
        }
        c->monitored_pid = 0;
    }

    if (sel == 0) return; /* nothing selected */

    /* For demo: subscribe to stdout (fd=1) and stderr (fd=2) */
    if (c->hsvc->monitor_fd_subscribe) {
        if (c->hsvc->monitor_fd_subscribe(c->hsvc->host_ctx, sel, 1) == 0)
            c->monitor_fd_1 = 1;
        if (c->hsvc->monitor_fd_subscribe(c->hsvc->host_ctx, sel, 2) == 0)
            c->monitor_fd_2 = 1;
        c->monitored_pid = sel;
    }
}

static void plugin_activate(void *ctx, const evemon_host_services_t *services)
{
    wm_ctx_t *c = ctx;
    if (!c) return;
    c->hsvc = services;
    c->sub_pid_select_id = 0;
    c->sub_fd_write_id = 0;
    c->monitored_pid = 0;
    c->monitor_fd_1 = 0;
    c->monitor_fd_2 = 0;

    if (services && services->subscribe) {
        c->sub_pid_select_id = services->subscribe(services->host_ctx,
                                                   EVEMON_EVENT_PROCESS_SELECTED,
                                                   on_process_selected, c);
        c->sub_fd_write_id = services->subscribe(services->host_ctx,
                                                 EVEMON_EVENT_FD_WRITE,
                                                 on_fd_write, c);
    }
}

static void plugin_destroy(void *ctx)
{
    wm_ctx_t *c = ctx;
    if (!c) return;

    if (c->hsvc) {
        if (c->monitored_pid != 0) {
            if (c->monitor_fd_1)
                c->hsvc->monitor_fd_unsubscribe(c->hsvc->host_ctx, c->monitored_pid, 1);
            if (c->monitor_fd_2)
                c->hsvc->monitor_fd_unsubscribe(c->hsvc->host_ctx, c->monitored_pid, 2);
            c->monitored_pid = 0;
        }
        if (c->sub_pid_select_id && c->hsvc->unsubscribe)
            c->hsvc->unsubscribe(c->hsvc->host_ctx, c->sub_pid_select_id);
        if (c->sub_fd_write_id && c->hsvc->unsubscribe)
            c->hsvc->unsubscribe(c->hsvc->host_ctx, c->sub_fd_write_id);
    }

    free(c);
}

evemon_plugin_t *evemon_plugin_init(void)
{
    evemon_plugin_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    wm_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) { free(p); return NULL; }

    p->abi_version = evemon_PLUGIN_ABI_VERSION;
    p->name = "Write Monitor";
    p->id = "org.evemon.write_monitor";
    p->version = "1.0";
    p->data_needs = 0;
    p->plugin_ctx = c;
    p->create_widget = NULL;
    p->update = NULL;
    p->clear = NULL;
    p->destroy = plugin_destroy;
    p->activate = plugin_activate;
    p->role = EVEMON_ROLE_SERVICE;
    p->dependencies = NULL;

    return p;
}
