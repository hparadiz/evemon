/*
 * event_bus.h – Lightweight publish/subscribe event bus for evemon.
 *
 * Provides typed, host-mediated event routing between plugins.
 * Plugins never call each other directly — all communication flows
 * through the event bus.
 *
 * Thread safety: evemon_event_bus_publish() may be called from any
 * thread.  Subscriber callbacks are always dispatched on the GTK
 * main thread.
 *
 * Not part of the public plugin ABI — plugins access the bus through
 * the subscribe/publish function pointers in evemon_host_services_t.
 */

#ifndef EVEMON_EVENT_BUS_H
#define EVEMON_EVENT_BUS_H

#include "evemon_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise the global event bus.
 * Must be called once from the GTK main thread before any plugins load.
 */
void evemon_event_bus_init(void);

/*
 * Destroy the global event bus and free all subscriptions.
 * Must be called from the GTK main thread at shutdown.
 */
void evemon_event_bus_destroy(void);

/*
 * Subscribe to events of a given type.
 * The callback is always invoked on the GTK main thread.
 *
 * Returns a subscription ID (>0) that can be used to unsubscribe,
 * or 0 on failure.
 */
int evemon_event_bus_subscribe(evemon_event_type_t type,
                               evemon_event_cb cb,
                               void *user_data);

/*
 * Remove a subscription by its ID.
 */
void evemon_event_bus_unsubscribe(int subscription_id);

/*
 * Publish an event to all subscribers of the given type.
 *
 * If called from the GTK main thread, subscribers are invoked
 * synchronously.  If called from a worker thread, dispatch is
 * marshalled to the main thread via g_idle_add.
 *
 * The payload is COPIED into a heap-allocated event structure so
 * the caller's stack frame does not need to outlive the call.
 * For GObject payloads (e.g. GdkPixbuf), the caller must ensure
 * the payload remains valid or use the typed payload structs that
 * include ownership semantics.
 */
void evemon_event_bus_publish(const evemon_event_t *event);

/*
 * Host-side wrappers matching the evemon_host_services_t function
 * pointer signatures.  These are wired into host_services so
 * plugins call them indirectly via the services table.
 */
void host_event_subscribe(void *host_ctx,
                          evemon_event_type_t type,
                          evemon_event_cb cb,
                          void *user_data);

void host_event_publish(void *host_ctx,
                        const evemon_event_t *event);

void host_event_unsubscribe(void *host_ctx, int subscription_id);

#ifdef __cplusplus
}
#endif

#endif /* EVEMON_EVENT_BUS_H */
