/* This service uses the plugin ABI but creates no GTK widgets. */
#ifndef EVEMON_NO_GTK
#define EVEMON_NO_GTK
#endif
#include "evemon_plugin.h"
#include <stdlib.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.write_monitor",
    "Write Monitor",
    "1.0",
    EVEMON_ROLE_SERVICE,
    NULL
);

/* The host owns capture and Write Log views own subscriptions. This service
 * remains the plugin dependency marker; selecting a process alone must not
 * enable probes whose events have no consumer. */

evemon_plugin_t *evemon_plugin_init(void)
{
    evemon_plugin_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->abi_version = evemon_PLUGIN_ABI_VERSION;
    p->name = "Write Monitor";
    p->id = "org.evemon.write_monitor";
    p->version = "1.0";
    p->data_needs = 0;
    p->plugin_ctx = NULL;
    p->create_widget = NULL;
    p->update = NULL;
    p->clear = NULL;
    p->destroy = NULL;
    p->activate = NULL;
    p->role = EVEMON_ROLE_SERVICE;
    p->dependencies = NULL;

    return p;
}
