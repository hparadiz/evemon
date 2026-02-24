/*
 * profile.c – global profiler instance.
 *
 * The single g_profile is defined here so that all translation units
 * share the same profiler state (timers, mutex, counters).
 */

#include "profile.h"

profile_state_t g_profile;
