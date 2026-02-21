# Per-Core CPU Tracking (Shelved)

This document captures the per-core CPU feature that was prototyped and then
removed from the codebase to focus on refactoring.  Re-apply when ready.

## Overview

The goal was to track **which CPU core each process last ran on** and to
compute **per-core utilization percentages**, then surface both in the UI.

## Data Model Changes (`proc.h`)

```c
#define PROC_MAX_CORES 1024

/* In proc_entry_t: */
int last_cpu;  /* last CPU core this process ran on (-1 = unknown) */

/* In proc_snapshot_t: */
int    num_cores;                    /* online CPU cores       */
double core_percent[PROC_MAX_CORES]; /* per-core CPU% (0-100) */
```

## Monitor Changes (`monitor.c`)

### `read_cpu_ticks()` – extract last-run core

The function was extended with an `int *out_cpu` output parameter.  The
`sscanf` format string was expanded to parse all the way to field 39
("processor") of `/proc/<pid>/stat`:

```c
static unsigned long long read_cpu_ticks(pid_t pid, int *out_cpu)
```

Fields parsed after the comm field (field 2):

| Field | Name          | Notes                          |
|-------|---------------|--------------------------------|
| 14    | utime         | user-mode CPU ticks            |
| 15    | stime         | kernel-mode CPU ticks          |
| 16–19 | cutime…nice   | skipped                        |
| 20–38 | various       | skipped                        |
| 39    | processor     | last CPU core the process ran on |

### `build_snapshot()` – wire up new fields

```c
proc_snapshot_t snap = { .entries = NULL, .count = 0, .num_cores = 0 };
memset(snap.core_percent, 0, sizeof(snap.core_percent));
// ...
e->cpu_ticks = read_cpu_ticks(pid, &e->last_cpu);
```

### Per-core CPU% block in `monitor_thread()`

A new block was added after the per-process CPU% computation.  It reads
`/proc/stat` for each `cpuN` line, keeps static arrays of previous totals
and idle ticks, and computes delta-based utilization:

```c
/* ── per-core CPU% from /proc/stat ─────────────────────── */
{
    static unsigned long long prev_core_total[PROC_MAX_CORES];
    static unsigned long long prev_core_idle[PROC_MAX_CORES];
    static int prev_core_valid = 0;

    // Parse /proc/stat for lines matching "cpu<N> ..."
    // Compute: core_percent[c] = (dt - di) / dt * 100.0
    // where dt = delta total ticks, di = delta idle ticks.
}
```

The results were stored in `snap.num_cores` and `snap.core_percent[]`.

## UI Changes (`ui.c`)

### Tree-view column

A new `COL_CORE` column (type `G_TYPE_INT`) was added to the tree store
and displayed as a right-aligned "Core" column with sort support via
`sort_int_inverted`.

### Sidebar "Core" row

A `GtkLabel *sb_core` was added to the sidebar detail panel.  When a
process was selected, the label showed the core number plus its current
utilization if available:

```c
if (core >= 0) {
    if (core < ctx->num_cores)
        snprintf(core_buf, sizeof(core_buf), "CPU %d  (%.1f%%)",
                 core, ctx->core_percent[core]);
    else
        snprintf(core_buf, sizeof(core_buf), "CPU %d", core);
    gtk_label_set_text(ctx->sb_core, core_buf);
}
```

### Snapshot propagation in `on_refresh()`

The per-core arrays were copied from the snapshot into the `ui_ctx_t`
under the lock, then used by the sidebar:

```c
ctx->num_cores = snap_num_cores;
memcpy(ctx->core_percent, snap_core_pct, snap_num_cores * sizeof(double));
```

### UI context additions

```c
/* In ui_ctx_t: */
GtkLabel *sb_core;
int       num_cores;
double    core_percent[PROC_MAX_CORES];
```

## Why It Was Removed

The feature worked but added complexity to the data path (extra fields in
every `proc_entry_t`, a 1024-element `double` array copied every refresh,
an extra `/proc/stat` parse per poll cycle).  It was shelved to focus on
refactoring and optimizing the existing code before adding more surface
area.

## Re-implementation Notes

- Consider making `PROC_MAX_CORES` dynamic (allocate to actual core count).
- The per-core `/proc/stat` parse could be merged with the boot-time parse
  to reduce file opens.
- The sidebar display could show a mini bar-chart or sparkline per core
  instead of just a percentage number.
