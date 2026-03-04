/*
 * plugin_broker.h – Data broker that gathers /proc data once per unique
 *                   PID and dispatches it to all interested plugin instances.
 *
 * A single pthread worker gathers exactly the data needed (keyed on the
 * combined data_needs mask) and posts the completed cycle to the active
 * frontend via broker_complete_cb.  The frontend calls
 * broker_dispatch_cycle() from its own main-loop context, then
 * broker_cycle_free() to release the cycle's memory.
 *
 * This design is toolkit-neutral: GTK wires g_idle_add, Qt wires
 * QMetaObject::invokeMethod, headless/terminal wire an eventfd.
 */

#pragma once

#include "plugin_loader.h"
#include "fdmon.h"
#include <sys/types.h>
#include <stddef.h>

/* Opaque handle to a completed broker cycle */
typedef struct broker_cycle broker_cycle_t;

/* ── Frontend completion hook ────────────────────────────────── */

/*
 * broker_complete_cb – called from the worker thread when a cycle
 * finishes (or is cancelled with result_count == 0).
 *
 * The callback must marshal broker_dispatch_cycle(cycle) onto the
 * UI/main-loop thread, then call broker_cycle_free(cycle).
 *
 * Example for GTK (g_idle_add):
 *
 *   static gboolean _on_idle(gpointer p) {
 *       broker_cycle_t *c = p;
 *       broker_dispatch_cycle(c);
 *       broker_cycle_free(c);
 *       return G_SOURCE_REMOVE;
 *   }
 *   static void _on_complete(broker_cycle_t *c, void *data) {
 *       g_idle_add(_on_idle, c);
 *   }
 *   broker_set_complete_callback(_on_complete, NULL);
 */
typedef void (*broker_complete_cb)(broker_cycle_t *cycle, void *user_data);

/*
 * Register the frontend completion hook.  Must be called before the
 * first broker_start().  Thread-safe (no lock needed — set once at init).
 */
void broker_set_complete_callback(broker_complete_cb cb, void *user_data);

/*
 * Dispatch a completed cycle to all plugin instances.
 * Call from the main/UI thread inside the broker_complete_cb hook.
 * Does NOT free the cycle — call broker_cycle_free() afterwards.
 */
void broker_dispatch_cycle(broker_cycle_t *cycle);

/*
 * Free a completed (or cancelled) cycle.
 * Safe to call on any thread once broker_dispatch_cycle() has returned.
 */
void broker_cycle_free(broker_cycle_t *cycle);

/* ── Lifecycle ───────────────────────────────────────────────── */

/*
 * Start a broker gather-and-dispatch cycle.
 *
 *   reg    – plugin registry (read for PID list / data_needs)
 *   fdmon  – the eBPF fd-monitor context (may be NULL)
 *
 * Spawns a pthread worker.  If a previous cycle is still running it is
 * cancelled automatically before the new one starts.
 * Thread-safe — may be called from any thread.
 */
void broker_start(plugin_registry_t *reg, void *fdmon);

/*
 * Cancel any in-flight broker cycle.  The worker thread will detect the
 * cancel flag, free the cycle itself, and not invoke the completion hook.
 * Thread-safe.
 */
void broker_cancel(void);

/*
 * Clean up broker state at shutdown.
 * Cancels any running cycle and clears all registered callbacks.
 */
void broker_destroy(void);

/* ── Audio PID delivery ──────────────────────────────────────── */

/*
 * Callback type for delivering audio PIDs extracted from the PipeWire
 * graph snapshot.  Called from broker_dispatch_cycle() (i.e. on the
 * main/UI thread).
 *
 *   pids  – array of PIDs with active PipeWire Audio/Stream nodes
 *   count – number of elements in pids
 *   data  – user_data passed to broker_set_audio_callback()
 *
 * The pids array is owned by the cycle and freed when broker_cycle_free()
 * is called.  Copy it if you need it beyond broker_dispatch_cycle().
 */
typedef void (*broker_audio_pids_cb)(const pid_t *pids, size_t count,
                                     void *data);

/*
 * Register a callback to receive audio PIDs after each broker cycle.
 * Thread-safe — typically called once at frontend init.
 */
void broker_set_audio_callback(broker_audio_pids_cb cb, void *user_data);
