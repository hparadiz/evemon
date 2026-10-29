#include "ui_gtk_private.h"

/* ── host-side GPU process integration ───────────────────────── */

void gtk_ui_update_status_bar(ui_ctx_t *ctx);

typedef struct {
    gboolean active;
    gboolean gpu_valid;
    gboolean vram_valid;
    gboolean enc_valid;
    uint64_t gpu_percent;
    uint64_t vram_bytes;
    uint64_t enc_percent;
} gpu_process_aggregate_t;

void gtk_ui_format_gpu_bytes(uint64_t bytes, char *buf, size_t bufsz)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < G_N_ELEMENTS(units)) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(buf, bufsz, "%" PRIu64 " %s", bytes, units[unit]);
    else
        snprintf(buf, bufsz, "%.1f %s", value, units[unit]);
}

static gpu_process_aggregate_t gpu_aggregate_for_pid(
    const evemon_gpu_snapshot_t *snapshot, pid_t pid)
{
    gpu_process_aggregate_t aggregate = {0};
    for (uint32_t i = 0; i < snapshot->process_count; i++) {
        const evemon_gpu_process_t *process = &snapshot->processes[i];
        if (process->pid != pid)
            continue;

        aggregate.active = TRUE;
        if (process->valid & EVEMON_GPU_PROCESS_VALID_UTILIZATION) {
            aggregate.gpu_valid = TRUE;
            aggregate.gpu_percent += process->utilization_percent;
        }
        if (process->valid & EVEMON_GPU_PROCESS_VALID_MEMORY) {
            aggregate.vram_valid = TRUE;
            if (UINT64_MAX - aggregate.vram_bytes < process->memory_bytes)
                aggregate.vram_bytes = UINT64_MAX;
            else
                aggregate.vram_bytes += process->memory_bytes;
        }
        if (process->valid & EVEMON_GPU_PROCESS_VALID_ENCODE) {
            aggregate.enc_valid = TRUE;
            aggregate.enc_percent += process->encode_percent;
        }
    }
    return aggregate;
}

static void apply_gpu_snapshot_rows(ui_ctx_t *ctx, GtkTreeIter *parent)
{
    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint pid_value = 0;
        gtk_tree_model_get(model, &iter, COL_PID, &pid_value, -1);
        gpu_process_aggregate_t aggregate = gpu_aggregate_for_pid(
            &ctx->gpu_snapshot, (pid_t)pid_value);

        char gpu_text[32] = "—";
        char vram_text[32] = "—";
        char enc_text[32] = "—";
        gint gpu_value = 0;
        gint enc_value = 0;
        gint64 vram_value = 0;

        if (ctx->has_gpu_plugin && aggregate.gpu_valid) {
            gpu_value = aggregate.gpu_percent > G_MAXINT
                      ? G_MAXINT : (gint)aggregate.gpu_percent;
            snprintf(gpu_text, sizeof(gpu_text), "%" G_GUINT64_FORMAT "%%",
                     aggregate.gpu_percent);
        }
        if (ctx->has_gpu_plugin && aggregate.vram_valid) {
            vram_value = aggregate.vram_bytes > (uint64_t)G_MAXINT64
                       ? G_MAXINT64 : (gint64)aggregate.vram_bytes;
            gtk_ui_format_gpu_bytes(aggregate.vram_bytes, vram_text,
                             sizeof(vram_text));
        }
        if (ctx->has_gpu_plugin && aggregate.enc_valid) {
            enc_value = aggregate.enc_percent > G_MAXINT
                      ? G_MAXINT : (gint)aggregate.enc_percent;
            snprintf(enc_text, sizeof(enc_text), "%" G_GUINT64_FORMAT "%%",
                     aggregate.enc_percent);
        }

        gtk_tree_store_set(ctx->store, &iter,
                           COL_GPU, gpu_value,
                           COL_GPU_TEXT, gpu_text,
                           COL_GPU_VRAM, vram_value,
                           COL_GPU_VRAM_TEXT, vram_text,
                           COL_GPU_ENC, enc_value,
                           COL_GPU_ENC_TEXT, enc_text,
                           COL_GPU_ACTIVE,
                               ctx->has_gpu_plugin && aggregate.active,
                           -1);

        apply_gpu_snapshot_rows(ctx, &iter);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

void gtk_ui_apply_gpu_snapshot_to_store(ui_ctx_t *ctx)
{
    if (!ctx || !ctx->store)
        return;
    apply_gpu_snapshot_rows(ctx, NULL);
}

static gboolean gpu_process_store_has_rows(ui_ctx_t *ctx)
{
    GtkTreeIter first;
    return ctx && ctx->store && gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(ctx->store), &first);
}

static void sync_current_filter_after_gpu_change(ui_ctx_t *ctx)
{
    if (ctx->show_gpu_only)
        sync_gpu_filter_store(ctx);
    else if (ctx->show_cpu_intensive)
        sync_cpu_intensive_filter_store(ctx);
    else if (ctx->show_audio_only)
        sync_audio_filter_store(ctx);
    else if (ctx->filter_text[0] != '\0')
        sync_filter_store(ctx);
}

static void on_gpu_snapshot_host(const evemon_event_t *event, void *opaque)
{
    ui_ctx_t *ctx = opaque;
    if (!ctx || ctx->shutting_down || !event ||
        event->type != EVEMON_EVENT_GPU_SNAPSHOT || !event->payload)
        return;

    const evemon_gpu_snapshot_t *src = event->payload;
    evemon_gpu_device_t *devices = NULL;
    evemon_gpu_process_t *processes = NULL;
    if (src->device_count > 0) {
        devices = g_try_malloc_n(src->device_count, sizeof(*devices));
        if (!devices) return;
        memcpy(devices, src->devices,
               (size_t)src->device_count * sizeof(*devices));
    }
    if (src->process_count > 0) {
        processes = g_try_malloc_n(src->process_count, sizeof(*processes));
        if (!processes) {
            g_free(devices);
            return;
        }
        memcpy(processes, src->processes,
               (size_t)src->process_count * sizeof(*processes));
    }
    g_free(ctx->gpu_snapshot.devices);
    g_free(ctx->gpu_snapshot.processes);
    ctx->gpu_snapshot = *src;
    ctx->gpu_snapshot.devices = devices;
    ctx->gpu_snapshot.processes = processes;
    gtk_ui_apply_gpu_snapshot_to_store(ctx);
    if (gpu_process_store_has_rows(ctx))
        sync_current_filter_after_gpu_change(ctx);
    gtk_ui_update_status_bar(ctx);
}

void gpu_integration_set_available(ui_ctx_t *ctx, gboolean available)
{
    if (!ctx)
        return;

    gboolean was_available = ctx->has_gpu_plugin;
    gboolean was_gpu_view = ctx->show_gpu_only;

    if (available && ctx->gpu_subscription_id == 0) {
        ctx->gpu_subscription_id = evemon_event_bus_subscribe(
            EVEMON_EVENT_GPU_SNAPSHOT, on_gpu_snapshot_host, ctx);
    } else if (!available && ctx->gpu_subscription_id != 0) {
        evemon_event_bus_unsubscribe(ctx->gpu_subscription_id);
        ctx->gpu_subscription_id = 0;
    }

    ctx->has_gpu_plugin = available;

    for (int i = 0; i < 3; i++) {
        GtkTreeViewColumn *column = ctx->gpu_columns[i];
        GtkCheckMenuItem *menu_item = ctx->gpu_column_menu_items[i];
        if (!column)
            continue;

        if (!available && was_available) {
            g_object_set_data(
                G_OBJECT(column), "evemon-gpu-wanted",
                GINT_TO_POINTER(
                    gtk_tree_view_column_get_visible(column) ? 1 : 0));
        }

        gboolean wanted = GPOINTER_TO_INT(g_object_get_data(
            G_OBJECT(column), "evemon-gpu-wanted"));
        g_object_set_data(G_OBJECT(column), "evemon-gpu-available",
                          GINT_TO_POINTER(available ? 1 : 0));
        gtk_tree_view_column_set_visible(column, available && wanted);

        if (menu_item) {
            g_signal_handlers_block_by_func(menu_item, gtk_ui_on_column_toggled,
                                            column);
            gtk_check_menu_item_set_active(menu_item, wanted);
            g_signal_handlers_unblock_by_func(menu_item, gtk_ui_on_column_toggled,
                                              column);
            gtk_widget_set_sensitive(GTK_WIDGET(menu_item), available);
            gtk_widget_set_no_show_all(GTK_WIDGET(menu_item), !available);
            if (available)
                gtk_widget_show(GTK_WIDGET(menu_item));
            else
                gtk_widget_hide(GTK_WIDGET(menu_item));
        }
    }

    if (available && !was_available)
        ctx->show_gpu_only = settings_get()->show_gpu_only;
    else if (!available)
        ctx->show_gpu_only = FALSE;

    if (ctx->gpu_only_menu_item) {
        g_signal_handlers_block_by_func(ctx->gpu_only_menu_item,
                                        gtk_ui_on_toggle_gpu_only, ctx);
        gtk_check_menu_item_set_active(ctx->gpu_only_menu_item,
                                       ctx->show_gpu_only);
        g_signal_handlers_unblock_by_func(ctx->gpu_only_menu_item,
                                          gtk_ui_on_toggle_gpu_only, ctx);
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->gpu_only_menu_item),
                                 available);
        gtk_widget_set_no_show_all(GTK_WIDGET(ctx->gpu_only_menu_item),
                                   !available);
        if (available)
            gtk_widget_show(GTK_WIDGET(ctx->gpu_only_menu_item));
        else
            gtk_widget_hide(GTK_WIDGET(ctx->gpu_only_menu_item));
    }

    if (!available) {
        g_free(ctx->gpu_snapshot.devices);
        g_free(ctx->gpu_snapshot.processes);
        memset(&ctx->gpu_snapshot, 0, sizeof(ctx->gpu_snapshot));
        if (was_gpu_view) {
            settings_get()->show_gpu_only = false;
            settings_save();
            if (ctx->filter_text[0] != '\0')
                rebuild_filter_store(ctx);
            else if (ctx->show_cpu_intensive)
                rebuild_cpu_intensive_filter_store(ctx);
            else if (ctx->show_audio_only)
                rebuild_audio_filter_store(ctx);
            else
                switch_to_real_store(ctx);
        }
    }

    gtk_ui_apply_gpu_snapshot_to_store(ctx);
    if (available && ctx->show_gpu_only && gpu_process_store_has_rows(ctx))
        rebuild_gpu_filter_store(ctx);
    else if (!was_gpu_view && gpu_process_store_has_rows(ctx))
        sync_current_filter_after_gpu_change(ctx);
    gtk_ui_update_status_bar(ctx);
}

