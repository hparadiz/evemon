/*
 * cgroup_scan.c – cgroup resource limits display for the detail panel.
 *
 * Reads /proc/<pid>/cgroup to find the cgroup path, then reads
 * resource controller files from /sys/fs/cgroup/<path>/ to show
 * what limits the process (or its container) is running under.
 *
 * Displayed limits:
 *   memory.max / memory.current   → "3.2 GiB / 4.0 GiB (80%)"
 *   memory.swap.max               → swap limit if set
 *   cpu.max                       → "200000 100000" → "200% (2 cores)"
 *   pids.max / pids.current       → "142 / 1000"
 *   io.max                        → I/O bandwidth limits if set
 *
 * Only cgroup v2 (unified hierarchy) is supported.  If no limits
 * are set (everything is "max"), the section is hidden.
 */

#include "ui_internal.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>

/* ── cgroup path resolution ─────────────────────────────────── */

/*
 * Read /proc/<pid>/cgroup and extract the cgroup v2 path.
 * Returns 0 on success, -1 on failure.
 *
 * cgroup v2 unified hierarchy line format:  0::<path>
 */
static int read_cgroup_path(pid_t pid, char *buf, size_t bufsz)
{
    buf[0] = '\0';

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/cgroup", pid);

    FILE *f = fopen(proc_path, "r");
    if (!f)
        return -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* cgroup v2 unified: starts with "0::" */
        if (strncmp(line, "0::", 3) == 0) {
            /* Strip trailing newline */
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n')
                line[len - 1] = '\0';

            const char *path = line + 3;
            snprintf(buf, bufsz, "%s", path);
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

/* ── read a single cgroup file ──────────────────────────────── */

static int read_cgroup_file(const char *cgroup_dir, const char *filename,
                            char *buf, size_t bufsz)
{
    buf[0] = '\0';
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", cgroup_dir, filename);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    if (!fgets(buf, (int)bufsz, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Strip trailing newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';

    return 0;
}

static int64_t read_cgroup_int(const char *cgroup_dir, const char *filename)
{
    char buf[64];
    if (read_cgroup_file(cgroup_dir, filename, buf, sizeof(buf)) != 0)
        return -1;

    /* "max" means unlimited */
    if (strcmp(buf, "max") == 0)
        return -1;

    char *end = NULL;
    int64_t val = strtoll(buf, &end, 10);
    if (end == buf)
        return -1;

    return val;
}

/* ── formatting helpers ─────────────────────────────────────── */

static void format_bytes(int64_t bytes, char *buf, size_t bufsz)
{
    if (bytes < 0) {
        snprintf(buf, bufsz, "unlimited");
        return;
    }
    double val = (double)bytes;
    if (val < 1024.0)
        snprintf(buf, bufsz, "%" PRId64 " B", bytes);
    else if (val < 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f KiB", val / 1024.0);
    else if (val < 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f MiB", val / (1024.0 * 1024.0));
    else
        snprintf(buf, bufsz, "%.1f GiB", val / (1024.0 * 1024.0 * 1024.0));
}

/* ── cgroup limits data structure ───────────────────────────── */

typedef struct {
    char cgroup_path[1024];    /* v2 cgroup path (e.g. /system.slice/docker-xxx.scope) */
    char cgroup_dir[1280];     /* /sys/fs/cgroup/<path> */

    /* memory controller */
    int64_t mem_current;       /* bytes, -1 = unavailable */
    int64_t mem_max;           /* bytes, -1 = unlimited   */
    int64_t mem_swap_current;
    int64_t mem_swap_max;
    int64_t mem_high;          /* soft limit */

    /* cpu controller */
    int64_t cpu_quota;         /* µs per period, -1 = unlimited */
    int64_t cpu_period;        /* µs (typically 100000)         */

    /* pids controller */
    int64_t pids_current;
    int64_t pids_max;          /* -1 = unlimited */

    /* io controller — we just store the raw line(s) */
    char io_max[256];          /* raw io.max content (multi-line → first line) */

    int has_any_limit;         /* 1 if at least one controller has a non-max limit */
} cgroup_limits_t;

/* ── read all limits ────────────────────────────────────────── */

static void read_cgroup_limits(pid_t pid, cgroup_limits_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mem_current     = -1;
    out->mem_max         = -1;
    out->mem_swap_current = -1;
    out->mem_swap_max    = -1;
    out->mem_high        = -1;
    out->cpu_quota       = -1;
    out->cpu_period      = -1;
    out->pids_current    = -1;
    out->pids_max        = -1;

    /* Find the cgroup v2 path */
    if (read_cgroup_path(pid, out->cgroup_path, sizeof(out->cgroup_path)) != 0)
        return;

    /* Construct /sys/fs/cgroup/<path> */
    snprintf(out->cgroup_dir, sizeof(out->cgroup_dir),
             "/sys/fs/cgroup%s", out->cgroup_path);

    /* Check it exists */
    struct stat st;
    if (stat(out->cgroup_dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return;

    /* ── memory controller ───────────────────────────────────── */
    out->mem_current = read_cgroup_int(out->cgroup_dir, "memory.current");
    out->mem_max     = read_cgroup_int(out->cgroup_dir, "memory.max");
    out->mem_high    = read_cgroup_int(out->cgroup_dir, "memory.high");
    out->mem_swap_current = read_cgroup_int(out->cgroup_dir, "memory.swap.current");
    out->mem_swap_max     = read_cgroup_int(out->cgroup_dir, "memory.swap.max");

    /* ── cpu controller ──────────────────────────────────────── */
    {
        char buf[64];
        if (read_cgroup_file(out->cgroup_dir, "cpu.max", buf, sizeof(buf)) == 0) {
            /* Format: "<quota> <period>" or "max <period>" */
            char quota_str[32], period_str[32];
            if (sscanf(buf, "%31s %31s", quota_str, period_str) == 2) {
                if (strcmp(quota_str, "max") != 0)
                    out->cpu_quota = strtoll(quota_str, NULL, 10);
                out->cpu_period = strtoll(period_str, NULL, 10);
            }
        }
    }

    /* ── pids controller ─────────────────────────────────────── */
    out->pids_current = read_cgroup_int(out->cgroup_dir, "pids.current");
    out->pids_max     = read_cgroup_int(out->cgroup_dir, "pids.max");

    /* ── io controller ───────────────────────────────────────── */
    read_cgroup_file(out->cgroup_dir, "io.max", out->io_max, sizeof(out->io_max));

    /* Determine if any meaningful limits are set */
    if (out->mem_max > 0)           out->has_any_limit = 1;
    if (out->mem_high > 0)          out->has_any_limit = 1;
    if (out->mem_swap_max > 0)      out->has_any_limit = 1;
    if (out->cpu_quota > 0)         out->has_any_limit = 1;
    if (out->pids_max > 0)          out->has_any_limit = 1;
    if (out->io_max[0] != '\0')     out->has_any_limit = 1;
}

/* ── GTask async scan ───────────────────────────────────────── */

typedef struct {
    pid_t            pid;
    guint            generation;
    cgroup_limits_t  limits;
} cgroup_scan_task_t;

static void cgroup_scan_task_free(gpointer p)
{
    g_free(p);
}

static void cgroup_scan_thread_func(GTask        *task,
                                    gpointer      source_object,
                                    gpointer      task_data,
                                    GCancellable *cancellable)
{
    (void)source_object;
    cgroup_scan_task_t *t = task_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    read_cgroup_limits(t->pid, &t->limits);

    g_task_return_boolean(task, TRUE);
}

static void cgroup_scan_complete(GObject      *source_object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
    (void)source_object;
    ui_ctx_t *ctx = user_data;

    GTask *task = G_TASK(result);
    cgroup_scan_task_t *t = g_task_get_task_data(task);

    if (!t || t->generation != ctx->cgroup_generation)
        return;

    if (g_task_had_error(task))
        return;

    const cgroup_limits_t *lim = &t->limits;

    /* Hide the section for regular non-container processes that have
     * no actual resource limits set (every process has a cgroup path,
     * but only containers / slices with explicit limits are interesting). */
    if (lim->cgroup_path[0] == '\0' || !lim->has_any_limit) {
        gtk_widget_hide(ctx->sb_cgroup_frame);
        return;
    }

    /* Cgroup path label */
    gtk_label_set_text(ctx->sb_cgroup_path, lim->cgroup_path);
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->sb_cgroup_path),
                                lim->cgroup_dir);

    /* ── Memory ──────────────────────────────────────────────── */
    if (lim->mem_max > 0 && lim->mem_current >= 0) {
        char cur[32], max[32];
        format_bytes(lim->mem_current, cur, sizeof(cur));
        format_bytes(lim->mem_max, max, sizeof(max));
        double pct = (double)lim->mem_current / (double)lim->mem_max * 100.0;
        char combined[128];
        snprintf(combined, sizeof(combined), "%s / %s (%.0f%%)", cur, max, pct);
        gtk_label_set_text(ctx->sb_cgroup_mem, combined);
    } else if (lim->mem_current >= 0) {
        char cur[32];
        format_bytes(lim->mem_current, cur, sizeof(cur));
        char combined[128];
        snprintf(combined, sizeof(combined), "%s / unlimited", cur);
        gtk_label_set_text(ctx->sb_cgroup_mem, combined);
    } else {
        gtk_label_set_text(ctx->sb_cgroup_mem, "–");
    }

    /* Memory soft limit (memory.high) */
    if (lim->mem_high > 0) {
        char high[32];
        format_bytes(lim->mem_high, high, sizeof(high));
        gtk_label_set_text(ctx->sb_cgroup_mem_high, high);
        gtk_widget_show(GTK_WIDGET(ctx->sb_cgroup_mem_high));
        gtk_widget_show(ctx->sb_cgroup_mem_high_key);
    } else {
        gtk_widget_hide(GTK_WIDGET(ctx->sb_cgroup_mem_high));
        gtk_widget_hide(ctx->sb_cgroup_mem_high_key);
    }

    /* Swap */
    if (lim->mem_swap_max > 0 && lim->mem_swap_current >= 0) {
        char cur[32], max[32];
        format_bytes(lim->mem_swap_current, cur, sizeof(cur));
        format_bytes(lim->mem_swap_max, max, sizeof(max));
        double pct = (double)lim->mem_swap_current / (double)lim->mem_swap_max * 100.0;
        char combined[128];
        snprintf(combined, sizeof(combined), "%s / %s (%.0f%%)", cur, max, pct);
        gtk_label_set_text(ctx->sb_cgroup_swap, combined);
        gtk_widget_show(GTK_WIDGET(ctx->sb_cgroup_swap));
        gtk_widget_show(ctx->sb_cgroup_swap_key);
    } else {
        gtk_widget_hide(GTK_WIDGET(ctx->sb_cgroup_swap));
        gtk_widget_hide(ctx->sb_cgroup_swap_key);
    }

    /* ── CPU ─────────────────────────────────────────────────── */
    if (lim->cpu_quota > 0 && lim->cpu_period > 0) {
        double pct = (double)lim->cpu_quota / (double)lim->cpu_period * 100.0;
        double cores = (double)lim->cpu_quota / (double)lim->cpu_period;
        char combined[128];
        if (cores >= 1.0)
            snprintf(combined, sizeof(combined), "%.0f%% (%.1f cores)",
                     pct, cores);
        else
            snprintf(combined, sizeof(combined), "%.0f%%", pct);
        gtk_label_set_text(ctx->sb_cgroup_cpu, combined);
    } else {
        gtk_label_set_text(ctx->sb_cgroup_cpu, "unlimited");
    }

    /* ── PIDs ────────────────────────────────────────────────── */
    if (lim->pids_max > 0 && lim->pids_current >= 0) {
        char combined[64];
        snprintf(combined, sizeof(combined), "%" PRId64 " / %" PRId64,
                 lim->pids_current, lim->pids_max);
        gtk_label_set_text(ctx->sb_cgroup_pids, combined);
    } else if (lim->pids_current >= 0) {
        char combined[64];
        snprintf(combined, sizeof(combined), "%" PRId64 " / unlimited",
                 lim->pids_current);
        gtk_label_set_text(ctx->sb_cgroup_pids, combined);
    } else {
        gtk_label_set_text(ctx->sb_cgroup_pids, "–");
    }

    /* ── I/O limits ──────────────────────────────────────────── */
    if (lim->io_max[0] != '\0') {
        gtk_label_set_text(ctx->sb_cgroup_io, lim->io_max);
        gtk_widget_show(GTK_WIDGET(ctx->sb_cgroup_io));
        gtk_widget_show(ctx->sb_cgroup_io_key);
    } else {
        gtk_widget_hide(GTK_WIDGET(ctx->sb_cgroup_io));
        gtk_widget_hide(ctx->sb_cgroup_io_key);
    }

    /* Only run show_all when the frame is not yet visible; subsequent
     * refreshes update labels and optional rows in place to avoid the
     * show_all → re-hide flash. */
    if (!gtk_widget_get_visible(ctx->sb_cgroup_frame)) {
        gtk_widget_set_no_show_all(ctx->sb_cgroup_frame, FALSE);
        gtk_widget_show_all(ctx->sb_cgroup_frame);
        gtk_widget_set_no_show_all(ctx->sb_cgroup_frame, TRUE);

        /* After show_all, hide optional rows that are not applicable */
        if (lim->mem_high <= 0) {
            gtk_widget_hide(GTK_WIDGET(ctx->sb_cgroup_mem_high));
            gtk_widget_hide(ctx->sb_cgroup_mem_high_key);
        }
        if (!(lim->mem_swap_max > 0 && lim->mem_swap_current >= 0)) {
            gtk_widget_hide(GTK_WIDGET(ctx->sb_cgroup_swap));
            gtk_widget_hide(ctx->sb_cgroup_swap_key);
        }
        if (lim->io_max[0] == '\0') {
            gtk_widget_hide(GTK_WIDGET(ctx->sb_cgroup_io));
            gtk_widget_hide(ctx->sb_cgroup_io_key);
        }
    }
}

/* ── public API ──────────────────────────────────────────────── */

void cgroup_scan_start(ui_ctx_t *ctx, pid_t pid)
{
    ctx->cgroup_generation++;

    /* Cancel any previous in-flight scan */
    if (ctx->cgroup_cancel) {
        g_cancellable_cancel(ctx->cgroup_cancel);
        g_object_unref(ctx->cgroup_cancel);
    }
    ctx->cgroup_cancel = g_cancellable_new();

    cgroup_scan_task_t *t = g_new0(cgroup_scan_task_t, 1);
    t->pid        = pid;
    t->generation = ctx->cgroup_generation;

    GTask *task = g_task_new(NULL, ctx->cgroup_cancel,
                             cgroup_scan_complete, ctx);
    g_task_set_task_data(task, t, cgroup_scan_task_free);
    g_task_run_in_thread(task, cgroup_scan_thread_func);
    g_object_unref(task);
}

/*
 * Synchronous cgroup update for pinned panels.
 * Reads limits on the calling thread and writes results directly
 * to the provided label set.  This is safe because it's called
 * from the GTK main thread (on_refresh → pinned_panels_update).
 * cgroup reads are fast (<1 ms) so no async is needed.
 */
void cgroup_update_labels(pid_t pid, const cgroup_label_set_t *ls)
{
    cgroup_limits_t lim;
    read_cgroup_limits(pid, &lim);

    if (lim.cgroup_path[0] == '\0' || !lim.has_any_limit) {
        gtk_widget_hide(ls->frame);
        return;
    }

    /* Path */
    gtk_label_set_text(ls->path, lim.cgroup_path);
    gtk_widget_set_tooltip_text(GTK_WIDGET(ls->path), lim.cgroup_dir);

    /* Memory */
    if (lim.mem_max > 0 && lim.mem_current >= 0) {
        char cur[32], max[32];
        format_bytes(lim.mem_current, cur, sizeof(cur));
        format_bytes(lim.mem_max, max, sizeof(max));
        double pct = (double)lim.mem_current / (double)lim.mem_max * 100.0;
        char combined[128];
        snprintf(combined, sizeof(combined), "%s / %s (%.0f%%)", cur, max, pct);
        gtk_label_set_text(ls->mem, combined);
    } else if (lim.mem_current >= 0) {
        char cur[32];
        format_bytes(lim.mem_current, cur, sizeof(cur));
        char combined[128];
        snprintf(combined, sizeof(combined), "%s / unlimited", cur);
        gtk_label_set_text(ls->mem, combined);
    } else {
        gtk_label_set_text(ls->mem, "–");
    }

    /* Memory high (soft limit) */
    if (lim.mem_high > 0) {
        char high[32];
        format_bytes(lim.mem_high, high, sizeof(high));
        gtk_label_set_text(ls->mem_high, high);
        gtk_widget_show(GTK_WIDGET(ls->mem_high));
        gtk_widget_show(ls->mem_high_key);
    } else {
        gtk_widget_hide(GTK_WIDGET(ls->mem_high));
        gtk_widget_hide(ls->mem_high_key);
    }

    /* Swap */
    if (lim.mem_swap_max > 0 && lim.mem_swap_current >= 0) {
        char cur[32], max[32];
        format_bytes(lim.mem_swap_current, cur, sizeof(cur));
        format_bytes(lim.mem_swap_max, max, sizeof(max));
        double pct = (double)lim.mem_swap_current / (double)lim.mem_swap_max * 100.0;
        char combined[128];
        snprintf(combined, sizeof(combined), "%s / %s (%.0f%%)", cur, max, pct);
        gtk_label_set_text(ls->swap, combined);
        gtk_widget_show(GTK_WIDGET(ls->swap));
        gtk_widget_show(ls->swap_key);
    } else {
        gtk_widget_hide(GTK_WIDGET(ls->swap));
        gtk_widget_hide(ls->swap_key);
    }

    /* CPU */
    if (lim.cpu_quota > 0 && lim.cpu_period > 0) {
        double pct = (double)lim.cpu_quota / (double)lim.cpu_period * 100.0;
        double cores = (double)lim.cpu_quota / (double)lim.cpu_period;
        char combined[128];
        if (cores >= 1.0)
            snprintf(combined, sizeof(combined), "%.0f%% (%.1f cores)", pct, cores);
        else
            snprintf(combined, sizeof(combined), "%.0f%%", pct);
        gtk_label_set_text(ls->cpu, combined);
    } else {
        gtk_label_set_text(ls->cpu, "unlimited");
    }

    /* PIDs */
    if (lim.pids_max > 0 && lim.pids_current >= 0) {
        char combined[64];
        snprintf(combined, sizeof(combined), "%" PRId64 " / %" PRId64,
                 lim.pids_current, lim.pids_max);
        gtk_label_set_text(ls->pids, combined);
    } else if (lim.pids_current >= 0) {
        char combined[64];
        snprintf(combined, sizeof(combined), "%" PRId64 " / unlimited",
                 lim.pids_current);
        gtk_label_set_text(ls->pids, combined);
    } else {
        gtk_label_set_text(ls->pids, "–");
    }

    /* I/O */
    if (lim.io_max[0] != '\0') {
        gtk_label_set_text(ls->io, lim.io_max);
        gtk_widget_show(GTK_WIDGET(ls->io));
        gtk_widget_show(ls->io_key);
    } else {
        gtk_widget_hide(GTK_WIDGET(ls->io));
        gtk_widget_hide(ls->io_key);
    }

    /* Only run show_all when the frame is not yet visible */
    if (!gtk_widget_get_visible(ls->frame)) {
        gtk_widget_set_no_show_all(ls->frame, FALSE);
        gtk_widget_show_all(ls->frame);
        gtk_widget_set_no_show_all(ls->frame, TRUE);

        /* After show_all, hide optional rows that are not applicable */
        if (lim.mem_high <= 0) {
            gtk_widget_hide(GTK_WIDGET(ls->mem_high));
            gtk_widget_hide(ls->mem_high_key);
        }
        if (!(lim.mem_swap_max > 0 && lim.mem_swap_current >= 0)) {
            gtk_widget_hide(GTK_WIDGET(ls->swap));
            gtk_widget_hide(ls->swap_key);
        }
        if (lim.io_max[0] == '\0') {
            gtk_widget_hide(GTK_WIDGET(ls->io));
            gtk_widget_hide(ls->io_key);
        }
    }
}
