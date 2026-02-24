/*
 * plugin_broker.h – Data broker that gathers /proc data once per unique
 *                   PID and dispatches it to all interested plugin instances.
 *
 * The broker replaces the old pattern of 7 separate GTask threads each
 * re-reading /proc.  Instead, a single GTask worker gathers exactly the
 * data needed (keyed on the combined data_needs mask), and a completion
 * callback dispatches to plugins on the GTK main thread.
 */

#pragma once

#include "plugin_loader.h"
#include "fdmon.h"

/* Opaque handle to a running broker cycle */
typedef struct broker_cycle broker_cycle_t;

/*
 * Start a broker gather-and-dispatch cycle.
 *
 *   reg    – plugin registry (read on main thread for PID list / needs)
 *   fdmon  – the eBPF fd-monitor context (may be NULL)
 *
 * This creates a GTask that runs the gather in a worker thread and
 * dispatches results to plugins on the GTK main thread.  If a previous
 * cycle is still running it is cancelled automatically.
 *
 * Must be called from the GTK main thread.
 */
void broker_start(plugin_registry_t *reg, void *fdmon);

/*
 * Cancel any in-flight broker cycle.
 * Safe to call from the GTK main thread even if nothing is running.
 */
void broker_cancel(void);

/*
 * Clean up broker state at shutdown.
 * Cancels running cycles and frees internal bookkeeping.
 */
void broker_destroy(void);
