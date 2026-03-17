/*
 * cgroup_plugin.c – cgroup Resource Limits plugin for evemon.
 *
 * Displays cgroup v2 resource limits for a process:
 *   memory.max/current, cpu.max, pids.max/current, io.max
 *
 * Build:
 *   gcc -shared -fPIC -o evemon_cgroup.so cgroup_plugin.c \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.cgroup",
    "cgroup Limits",
    "1.0",
    EVEMON_ROLE_PROCESS,
    NULL
);

/* ── per-instance state ────────────────────────────────────────── */

typedef struct {
    GtkWidget  *box;       /* top-level VBox */
    GtkWidget  *grid;      /* key/value grid */
    GtkLabel   *path_val;
    GtkLabel   *mem_val;
    GtkLabel   *mem_high_key;
    GtkLabel   *mem_high_val;
    GtkLabel   *swap_key;
    GtkLabel   *swap_val;
    GtkLabel   *cpu_val;
    GtkLabel   *pids_val;
    GtkLabel   *io_key;
    GtkLabel   *io_val;
    GtkWidget  *no_limits;  /* "No resource limits set" label */
    pid_t       last_pid;
} cgroup_ctx_t;

/* ── formatting helpers ──────────────────────────────────────── */

static void format_bytes(int64_t bytes, char *buf, size_t bufsz)
{
    if (bytes < 0) { snprintf(buf, bufsz, "unlimited"); return; }
    double v = (double)bytes;
    if (v < 1024.0)            snprintf(buf, bufsz, "%" PRId64 " B", bytes);
    else if (v < 1048576.0)    snprintf(buf, bufsz, "%.1f KiB", v / 1024.0);
    else if (v < 1073741824.0) snprintf(buf, bufsz, "%.1f MiB", v / 1048576.0);
    else                       snprintf(buf, bufsz, "%.1f GiB", v / 1073741824.0);
}

/* ── add a key/value row to the grid ─────────────────────────── */

static GtkLabel *add_row(GtkGrid *grid, int row, const char *key)
{
    GtkWidget *k = gtk_label_new(key);
    gtk_widget_set_halign(k, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(k), 0.0);
    gtk_grid_attach(grid, k, 0, row, 1, 1);

    GtkWidget *v = gtk_label_new("—");
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(v), 0.0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_grid_attach(grid, v, 1, row, 1, 1);

    return GTK_LABEL(v);
}

/* ── plugin callbacks ────────────────────────────────────────── */

static GtkWidget *cgroup_create_widget(void *opaque)
{
    cgroup_ctx_t *ctx = opaque;

    ctx->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(ctx->box, 8);
    gtk_widget_set_margin_end(ctx->box, 8);
    gtk_widget_set_margin_top(ctx->box, 4);
    gtk_widget_set_margin_bottom(ctx->box, 4);

    ctx->grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(ctx->grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(ctx->grid), 2);
    gtk_box_pack_start(GTK_BOX(ctx->box), ctx->grid, FALSE, FALSE, 0);

    int row = 0;
    ctx->path_val     = add_row(GTK_GRID(ctx->grid), row++, "cgroup path");
    ctx->mem_val      = add_row(GTK_GRID(ctx->grid), row++, "Memory");
    ctx->mem_high_val = add_row(GTK_GRID(ctx->grid), row++, "Memory high");
    ctx->swap_val     = add_row(GTK_GRID(ctx->grid), row++, "Swap");
    ctx->cpu_val      = add_row(GTK_GRID(ctx->grid), row++, "CPU");
    ctx->pids_val     = add_row(GTK_GRID(ctx->grid), row++, "PIDs");
    ctx->io_val       = add_row(GTK_GRID(ctx->grid), row++, "I/O max");

    /* Stash key labels for show/hide */
    ctx->mem_high_key = GTK_LABEL(gtk_grid_get_child_at(GTK_GRID(ctx->grid), 0, 2));
    ctx->swap_key     = GTK_LABEL(gtk_grid_get_child_at(GTK_GRID(ctx->grid), 0, 3));
    ctx->io_key       = GTK_LABEL(gtk_grid_get_child_at(GTK_GRID(ctx->grid), 0, 6));

    ctx->no_limits = gtk_label_new("No resource limits set");
    gtk_widget_set_halign(ctx->no_limits, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(ctx->box), ctx->no_limits, FALSE, FALSE, 0);

    gtk_widget_show_all(ctx->box);
    gtk_widget_hide(ctx->no_limits);

    return ctx->box;
}

static void cgroup_update(void *opaque, const evemon_proc_data_t *data)
{
    cgroup_ctx_t *ctx = opaque;
    const evemon_cgroup_t *cg = data->cgroup;

    if (!cg || cg->path[0] == '\0') {
        /* No cgroup data at all (not on cgroup v2?) */
        gtk_widget_hide(ctx->grid);
        gtk_widget_show(ctx->no_limits);
        return;
    }

    /* Always show cgroup info when a path exists — current usage
     * values (memory, PIDs) are useful even without explicit limits. */
    gtk_widget_show(ctx->grid);
    gtk_widget_hide(ctx->no_limits);

    /* Path */
    gtk_label_set_text(ctx->path_val, cg->path);

    /* Memory */
    if (cg->mem_max > 0 && cg->mem_current >= 0) {
        char cur[32], max[32];
        format_bytes(cg->mem_current, cur, sizeof(cur));
        format_bytes(cg->mem_max, max, sizeof(max));
        double pct = (double)cg->mem_current / (double)cg->mem_max * 100.0;
        char combined[128];
        snprintf(combined, sizeof(combined), "%s / %s (%.0f%%)", cur, max, pct);
        gtk_label_set_text(ctx->mem_val, combined);
    } else if (cg->mem_current >= 0) {
        char cur[32];
        format_bytes(cg->mem_current, cur, sizeof(cur));
        char combined[128];
        snprintf(combined, sizeof(combined), "%s / unlimited", cur);
        gtk_label_set_text(ctx->mem_val, combined);
    } else {
        gtk_label_set_text(ctx->mem_val, "unlimited");
    }

    /* Memory high */
    if (cg->mem_high > 0) {
        char high[32]; format_bytes(cg->mem_high, high, sizeof(high));
        gtk_label_set_text(ctx->mem_high_val, high);
        gtk_widget_show(GTK_WIDGET(ctx->mem_high_val));
        gtk_widget_show(GTK_WIDGET(ctx->mem_high_key));
    } else {
        gtk_widget_hide(GTK_WIDGET(ctx->mem_high_val));
        gtk_widget_hide(GTK_WIDGET(ctx->mem_high_key));
    }

    /* Swap */
    if (cg->swap_max > 0) {
        char max[32]; format_bytes(cg->swap_max, max, sizeof(max));
        gtk_label_set_text(ctx->swap_val, max);
        gtk_widget_show(GTK_WIDGET(ctx->swap_val));
        gtk_widget_show(GTK_WIDGET(ctx->swap_key));
    } else {
        gtk_widget_hide(GTK_WIDGET(ctx->swap_val));
        gtk_widget_hide(GTK_WIDGET(ctx->swap_key));
    }

    /* CPU */
    if (cg->cpu_quota > 0 && cg->cpu_period > 0) {
        double pct = (double)cg->cpu_quota / (double)cg->cpu_period * 100.0;
        double cores = (double)cg->cpu_quota / (double)cg->cpu_period;
        char combined[128];
        if (cores >= 1.0)
            snprintf(combined, sizeof(combined), "%.0f%% (%.1f cores)", pct, cores);
        else
            snprintf(combined, sizeof(combined), "%.0f%%", pct);
        gtk_label_set_text(ctx->cpu_val, combined);
    } else {
        gtk_label_set_text(ctx->cpu_val, "unlimited");
    }

    /* PIDs */
    if (cg->pids_max > 0 && cg->pids_current >= 0) {
        char combined[64];
        snprintf(combined, sizeof(combined), "%" PRId64 " / %" PRId64,
                 cg->pids_current, cg->pids_max);
        gtk_label_set_text(ctx->pids_val, combined);
    } else if (cg->pids_current >= 0) {
        char combined[64];
        snprintf(combined, sizeof(combined), "%" PRId64 " / unlimited",
                 cg->pids_current);
        gtk_label_set_text(ctx->pids_val, combined);
    } else {
        gtk_label_set_text(ctx->pids_val, "unlimited");
    }

    /* I/O */
    if (cg->io_max[0] != '\0') {
        gtk_label_set_text(ctx->io_val, cg->io_max);
        gtk_widget_show(GTK_WIDGET(ctx->io_val));
        gtk_widget_show(GTK_WIDGET(ctx->io_key));
    } else {
        gtk_widget_hide(GTK_WIDGET(ctx->io_val));
        gtk_widget_hide(GTK_WIDGET(ctx->io_key));
    }
}

static void cgroup_clear(void *opaque)
{
    cgroup_ctx_t *ctx = opaque;
    gtk_label_set_text(ctx->path_val, "—");
    gtk_label_set_text(ctx->mem_val, "—");
    gtk_label_set_text(ctx->cpu_val, "—");
    gtk_label_set_text(ctx->pids_val, "—");
    gtk_widget_hide(ctx->grid);
    gtk_widget_show(ctx->no_limits);
}

static void cgroup_destroy(void *opaque) { free(opaque); }

/* ── descriptor ──────────────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    cgroup_ctx_t *ctx = calloc(1, sizeof(cgroup_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "cgroup Limits",
        .id            = "org.evemon.cgroup",
        .version       = "1.0",
        .data_needs    = evemon_NEED_CGROUP,
        .plugin_ctx    = ctx,
        .create_widget = cgroup_create_widget,
        .update        = cgroup_update,
        .clear         = cgroup_clear,
        .destroy       = cgroup_destroy,
        .role          = EVEMON_ROLE_PROCESS,
        .dependencies  = NULL,
    };

    return p;
}
