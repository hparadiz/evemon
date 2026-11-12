#include "ui_gtk_private.h"
#include "../ipc/query.h"
#include <errno.h>

/* ── right-click: end process / end process tree ─────────────── */

/* Capture each identity once while walking the displayed subtree.  Signals
 * still validate start_ticks at delivery, so PID reuse cannot change targets. */
static void capture_signal_targets(GtkTreeModel *model, GtkTreeIter *iter,
                                    GArray *targets, gboolean descendants)
{
    if (descendants) {
        GtkTreeIter child;
        gboolean valid = gtk_tree_model_iter_children(model, &child, iter);
        while (valid) {
            capture_signal_targets(model, &child, targets, TRUE);
            valid = gtk_tree_model_iter_next(model, &child);
        }
    }
    proc_entry_t entry = {0};
    gtk_tree_model_get(model, iter, COL_PID, &entry.pid,
                       COL_START_TICKS, &entry.start_ticks,
                       COL_EVEMON_INSTANCE, &entry.evemon_instance, -1);
    if (entry.pid > 1) g_array_append_val(targets, entry);
}

typedef struct { GArray *targets; int signo; char path[108]; } send_signal_data_t;
static send_signal_data_t *signal_data(GArray *targets, int signo)
{
    send_signal_data_t *data = g_new0(send_signal_data_t, 1);
    data->targets = g_array_ref(targets);
    data->signo = signo;
    const char *path = query_remote_path();
    if (path) g_strlcpy(data->path, path, sizeof(data->path));
    return data;
}
static void signal_data_free(gpointer opaque, GClosure *closure)
{
    (void)closure; send_signal_data_t *data = opaque;
    g_array_unref(data->targets); g_free(data);
}
static void signal_task_free(gpointer data) { signal_data_free(data, NULL); }
static void signal_worker(GTask *task, gpointer source, gpointer opaque, GCancellable *cancel)
{
    (void)source; (void)cancel; send_signal_data_t *data = opaque;
    GString *errors = g_string_new(NULL);
    for (guint i = 0; i < data->targets->len; ++i) {
        if (evemon_shutdown_requested) break;
        proc_entry_t *target = &g_array_index(data->targets, proc_entry_t, i);
        if (query_signal_path(data->path, target, data->signo))
            g_string_append_printf(errors, "%d: %s\n", target->pid, strerror(errno));
    }
    g_task_return_pointer(task, g_string_free(errors, FALSE), g_free);
}
static void signal_complete(GObject *source, GAsyncResult *result, gpointer opaque)
{
    (void)source; (void)opaque;
    char *errors = g_task_propagate_pointer(G_TASK(result), NULL);
    if (errors && *errors && !evemon_shutdown_requested) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Signal delivery failed");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", errors);
        gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
    }
    g_free(errors);
}
static void on_send_signal(GtkMenuItem *item, gpointer opaque)
{
    (void)item;
    send_signal_data_t *data = g_new(send_signal_data_t, 1);
    *data = *(send_signal_data_t *)opaque; g_array_ref(data->targets);
    if (!data->targets->len) { signal_task_free(data); return; }
    if (data->signo != SIGCONT) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Send %s to %u process%s?",
            strsignal(data->signo), data->targets->len,
            data->targets->len == 1 ? "" : "es");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "Unsaved work in the affected processes may be lost.");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL,
                                "Send Signal", GTK_RESPONSE_ACCEPT, NULL);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_ACCEPT) { signal_task_free(data); return; }
    }
    GTask *task = g_task_new(NULL, NULL, signal_complete, NULL);
    g_task_set_task_data(task, data, signal_task_free);
    g_task_run_in_thread(task, signal_worker); g_object_unref(task);
}

static void on_copy_command(GtkMenuItem *item, gpointer data)
{
    (void)item;
    const char *cmdline = data;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, cmdline, -1);
}

/* pin/unpin helpers and pinned_panel_create/destroy → see ui_pinned.c */

/* ── Alternate process-list filters ────────────────────────────── */

void gtk_ui_on_toggle_cpu_intensive(GtkCheckMenuItem *item, gpointer data);
void gtk_ui_on_toggle_gpu_only(GtkCheckMenuItem *item, gpointer data);

void gtk_ui_on_toggle_audio_only(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->show_audio_only = gtk_check_menu_item_get_active(item);
    settings_get()->show_audio_only = ctx->show_audio_only;

    if (ctx->show_audio_only && ctx->show_cpu_intensive) {
        ctx->show_cpu_intensive = FALSE;
        settings_get()->show_cpu_intensive = false;
        if (ctx->cpu_intensive_menu_item) {
            g_signal_handlers_block_by_func(ctx->cpu_intensive_menu_item,
                                            gtk_ui_on_toggle_cpu_intensive, ctx);
            gtk_check_menu_item_set_active(ctx->cpu_intensive_menu_item,
                                           FALSE);
            g_signal_handlers_unblock_by_func(ctx->cpu_intensive_menu_item,
                                              gtk_ui_on_toggle_cpu_intensive, ctx);
        }
    }

    if (ctx->show_audio_only && ctx->show_gpu_only) {
        ctx->show_gpu_only = FALSE;
        settings_get()->show_gpu_only = false;
        if (ctx->gpu_only_menu_item) {
            g_signal_handlers_block_by_func(ctx->gpu_only_menu_item,
                                            gtk_ui_on_toggle_gpu_only, ctx);
            gtk_check_menu_item_set_active(ctx->gpu_only_menu_item, FALSE);
            g_signal_handlers_unblock_by_func(ctx->gpu_only_menu_item,
                                              gtk_ui_on_toggle_gpu_only, ctx);
        }
    }

    if (ctx->audio_only_menu_item &&
        item != ctx->audio_only_menu_item &&
        gtk_check_menu_item_get_active(ctx->audio_only_menu_item) !=
            ctx->show_audio_only) {
        g_signal_handlers_block_by_func(ctx->audio_only_menu_item,
                                        gtk_ui_on_toggle_audio_only, ctx);
        gtk_check_menu_item_set_active(ctx->audio_only_menu_item,
                                       ctx->show_audio_only);
        g_signal_handlers_unblock_by_func(ctx->audio_only_menu_item,
                                          gtk_ui_on_toggle_audio_only, ctx);
    }

    settings_save();

    if (ctx->show_audio_only) {
        /* Build a filtered store showing only audio processes */
        rebuild_audio_filter_store(ctx);
    } else {
        /* If a name filter is active, rebuild that; otherwise switch
         * back to the unfiltered real store. */
        if (ctx->filter_text[0] != '\0')
            rebuild_filter_store(ctx);
        else if (ctx->show_gpu_only)
            rebuild_gpu_filter_store(ctx);
        else if (ctx->show_cpu_intensive)
            rebuild_cpu_intensive_filter_store(ctx);
        else
            switch_to_real_store(ctx);
    }
}

void gtk_ui_on_toggle_cpu_intensive(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    ctx->show_cpu_intensive = gtk_check_menu_item_get_active(item);
    settings_get()->show_cpu_intensive = ctx->show_cpu_intensive;

    if (ctx->show_cpu_intensive && ctx->show_audio_only) {
        ctx->show_audio_only = FALSE;
        settings_get()->show_audio_only = false;
        if (ctx->audio_only_menu_item) {
            g_signal_handlers_block_by_func(ctx->audio_only_menu_item,
                                            gtk_ui_on_toggle_audio_only, ctx);
            gtk_check_menu_item_set_active(ctx->audio_only_menu_item, FALSE);
            g_signal_handlers_unblock_by_func(ctx->audio_only_menu_item,
                                              gtk_ui_on_toggle_audio_only, ctx);
        }
    }

    if (ctx->show_cpu_intensive && ctx->show_gpu_only) {
        ctx->show_gpu_only = FALSE;
        settings_get()->show_gpu_only = false;
        if (ctx->gpu_only_menu_item) {
            g_signal_handlers_block_by_func(ctx->gpu_only_menu_item,
                                            gtk_ui_on_toggle_gpu_only, ctx);
            gtk_check_menu_item_set_active(ctx->gpu_only_menu_item, FALSE);
            g_signal_handlers_unblock_by_func(ctx->gpu_only_menu_item,
                                              gtk_ui_on_toggle_gpu_only, ctx);
        }
    }

    if (ctx->cpu_intensive_menu_item &&
        item != ctx->cpu_intensive_menu_item &&
        gtk_check_menu_item_get_active(ctx->cpu_intensive_menu_item) !=
            ctx->show_cpu_intensive) {
        g_signal_handlers_block_by_func(ctx->cpu_intensive_menu_item,
                                        gtk_ui_on_toggle_cpu_intensive, ctx);
        gtk_check_menu_item_set_active(ctx->cpu_intensive_menu_item,
                                       ctx->show_cpu_intensive);
        g_signal_handlers_unblock_by_func(ctx->cpu_intensive_menu_item,
                                          gtk_ui_on_toggle_cpu_intensive, ctx);
    }

    settings_save();

    if (ctx->show_cpu_intensive) {
        rebuild_cpu_intensive_filter_store(ctx);
    } else if (ctx->filter_text[0] != '\0') {
        rebuild_filter_store(ctx);
    } else if (ctx->show_audio_only) {
        rebuild_audio_filter_store(ctx);
    } else if (ctx->show_gpu_only) {
        rebuild_gpu_filter_store(ctx);
    } else {
        switch_to_real_store(ctx);
    }
}

void gtk_ui_on_toggle_gpu_only(GtkCheckMenuItem *item, gpointer data)
{
    ui_ctx_t *ctx = data;
    gboolean requested = gtk_check_menu_item_get_active(item);
    ctx->show_gpu_only = requested && ctx->has_gpu_plugin;
    settings_get()->show_gpu_only = ctx->show_gpu_only;

    if (requested != ctx->show_gpu_only) {
        g_signal_handlers_block_by_func(item, gtk_ui_on_toggle_gpu_only, ctx);
        gtk_check_menu_item_set_active(item, ctx->show_gpu_only);
        g_signal_handlers_unblock_by_func(item, gtk_ui_on_toggle_gpu_only, ctx);
    }

    if (ctx->show_gpu_only && ctx->show_audio_only) {
        ctx->show_audio_only = FALSE;
        settings_get()->show_audio_only = false;
        if (ctx->audio_only_menu_item) {
            g_signal_handlers_block_by_func(ctx->audio_only_menu_item,
                                            gtk_ui_on_toggle_audio_only, ctx);
            gtk_check_menu_item_set_active(ctx->audio_only_menu_item, FALSE);
            g_signal_handlers_unblock_by_func(ctx->audio_only_menu_item,
                                              gtk_ui_on_toggle_audio_only, ctx);
        }
    }

    if (ctx->show_gpu_only && ctx->show_cpu_intensive) {
        ctx->show_cpu_intensive = FALSE;
        settings_get()->show_cpu_intensive = false;
        if (ctx->cpu_intensive_menu_item) {
            g_signal_handlers_block_by_func(ctx->cpu_intensive_menu_item,
                                            gtk_ui_on_toggle_cpu_intensive, ctx);
            gtk_check_menu_item_set_active(ctx->cpu_intensive_menu_item, FALSE);
            g_signal_handlers_unblock_by_func(ctx->cpu_intensive_menu_item,
                                              gtk_ui_on_toggle_cpu_intensive, ctx);
        }
    }

    if (ctx->gpu_only_menu_item && item != ctx->gpu_only_menu_item &&
        gtk_check_menu_item_get_active(ctx->gpu_only_menu_item) !=
            ctx->show_gpu_only) {
        g_signal_handlers_block_by_func(ctx->gpu_only_menu_item,
                                        gtk_ui_on_toggle_gpu_only, ctx);
        gtk_check_menu_item_set_active(ctx->gpu_only_menu_item,
                                       ctx->show_gpu_only);
        g_signal_handlers_unblock_by_func(ctx->gpu_only_menu_item,
                                          gtk_ui_on_toggle_gpu_only, ctx);
    }

    settings_save();

    if (ctx->show_gpu_only)
        rebuild_gpu_filter_store(ctx);
    else if (ctx->filter_text[0] != '\0')
        rebuild_filter_store(ctx);
    else if (ctx->show_cpu_intensive)
        rebuild_cpu_intensive_filter_store(ctx);
    else if (ctx->show_audio_only)
        rebuild_audio_filter_store(ctx);
    else
        switch_to_real_store(ctx);
}

GtkWidget *gtk_ui_append_audio_only_menu_item(ui_ctx_t *ctx, GtkWidget *menu)
{
    if (!ctx->has_audio_plugin)
        return NULL;

    GtkWidget *item = gtk_check_menu_item_new_with_label(
        "Show Audio Processes Only");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                   ctx->show_audio_only);
    g_signal_connect(item, "toggled", G_CALLBACK(gtk_ui_on_toggle_audio_only), ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget *gtk_ui_append_cpu_intensive_menu_item(ui_ctx_t *ctx,
                                                  GtkWidget *menu)
{
    GtkWidget *item = gtk_check_menu_item_new_with_label(
        "Show CPU-Intensive Processes Only");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                   ctx->show_cpu_intensive);
    gtk_widget_set_tooltip_text(
        item,
        "Show groups using at least half of one logical CPU and the busy subprocesses within them");
    g_signal_connect(item, "toggled", G_CALLBACK(gtk_ui_on_toggle_cpu_intensive), ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget *gtk_ui_append_gpu_only_menu_item(ui_ctx_t *ctx, GtkWidget *menu)
{
    if (!ctx->has_gpu_plugin)
        return NULL;

    GtkWidget *item = gtk_check_menu_item_new_with_label(
        "Show GPU Processes Only");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                   ctx->show_gpu_only);
    g_signal_connect(item, "toggled", G_CALLBACK(gtk_ui_on_toggle_gpu_only), ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}


void show_process_context_menu(ui_ctx_t *ctx, GdkEventButton *ev,
                                     pid_t pid, const char *name,
                                     const char *cmdline)
{
    GtkWidget *menu = gtk_menu_new();
    GArray *single_targets = g_array_new(FALSE, FALSE, sizeof(proc_entry_t));
    GArray *tree_targets = g_array_new(FALSE, FALSE, sizeof(proc_entry_t));
    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);
    GtkTreeIter target;
    if (find_iter_by_pid(model, NULL, pid, &target)) {
        capture_signal_targets(model, &target, single_targets, FALSE);
        capture_signal_targets(model, &target, tree_targets, TRUE);
    }
    g_signal_connect_swapped(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), menu);

    /* ── Pin / Unpin toggle (at the very top) ── */
    {
        gboolean pinned = pid_is_pinned(ctx, pid);
        char pin_label[320];
        snprintf(pin_label, sizeof(pin_label), "%s (%s, pid %d)",
                 pinned ? "Unpin Process" : "Pin Process", name, pid);
        GtkWidget *mi_pin = gtk_menu_item_new_with_label(pin_label);
        pin_toggle_data_t *pd = malloc(sizeof(*pd));
        if (pd) {
            pd->ctx = ctx;
            pd->pid = pid;
            g_signal_connect_data(mi_pin, "activate",
                                  G_CALLBACK(on_toggle_pin), pd,
                                  (GClosureNotify)g_free, 0);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_pin);
    }

    /* ── Show Audio Processes Only toggle ── */
    gtk_ui_append_audio_only_menu_item(ctx, menu);

    /* ── Show GPU Processes Only toggle ── */
    gtk_ui_append_gpu_only_menu_item(ctx, menu);

    /* ── Show CPU-Intensive Processes Only toggle ── */
    gtk_ui_append_cpu_intensive_menu_item(ctx, menu);

    /* ── Open Plugin as Window submenu ── */
    show_open_plugin_as_window_menu(ctx, menu, pid, name);

    /* ── separator ── */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    /* ── Copy Command ── */
    GtkWidget *mi_copy = gtk_menu_item_new_with_label("Copy Command");
    if (cmdline && cmdline[0]) {
        /* Store a copy of the cmdline string as data on the menu item
         * so it survives until the callback fires. */
        gchar *cmd_dup = g_strdup(cmdline);
        g_signal_connect(mi_copy, "activate",
                         G_CALLBACK(on_copy_command), cmd_dup);
        g_object_set_data_full(G_OBJECT(mi_copy), "cmd",
                               cmd_dup, g_free);
    } else {
        gtk_widget_set_sensitive(mi_copy, FALSE);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_copy);

    /* ── separator ── */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    char label1[320];
    snprintf(label1, sizeof(label1), "End Process (%s, pid %d)", name, pid);
    GtkWidget *mi1 = gtk_menu_item_new_with_label(label1);
    g_signal_connect_data(mi1, "activate", G_CALLBACK(on_send_signal),
                          signal_data(single_targets, SIGTERM), signal_data_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi1);

    char label2[320];
    snprintf(label2, sizeof(label2), "End Process Tree (%s, pid %d)", name, pid);
    GtkWidget *mi2 = gtk_menu_item_new_with_label(label2);
    g_signal_connect_data(mi2, "activate", G_CALLBACK(on_send_signal),
                          signal_data(tree_targets, SIGTERM), signal_data_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi2);

    /* ── Send Signal submenu ── */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    static const struct { const char *label; int signo; } signals[] = {
        { "SIGTERM (15) – Terminate",  SIGTERM },
        { "SIGKILL (9)  – Kill",       SIGKILL },
        { "SIGSTOP (19) – Stop",       SIGSTOP },
        { "SIGCONT (18) – Continue",   SIGCONT },
        { NULL, 0 },   /* separator */
        { "SIGINT  (2)  – Interrupt",  SIGINT  },
        { "SIGHUP  (1)  – Hangup",    SIGHUP  },
        { "SIGUSR1 (10)",             SIGUSR1 },
        { "SIGUSR2 (12)",             SIGUSR2 },
        { NULL, 0 },   /* separator */
        { "SIGABRT (6)  – Abort",     SIGABRT },
        { "SIGQUIT (3)  – Quit",      SIGQUIT },
    };

    {
        GtkWidget *sig_menu = gtk_menu_new();
        GtkWidget *sig_item = gtk_menu_item_new_with_label("Send Signal");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(sig_item), sig_menu);

        for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++) {
            if (!signals[i].label) {
                gtk_menu_shell_append(GTK_MENU_SHELL(sig_menu),
                                      gtk_separator_menu_item_new());
                continue;
            }
            GtkWidget *mi = gtk_menu_item_new_with_label(signals[i].label);
            g_signal_connect_data(mi, "activate", G_CALLBACK(on_send_signal),
                                  signal_data(single_targets, signals[i].signo), signal_data_free, 0);
            gtk_menu_shell_append(GTK_MENU_SHELL(sig_menu), mi);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), sig_item);
    }

    /* ── Send Signal to Tree submenu ── */
    {
        GtkWidget *sig_tree_menu = gtk_menu_new();
        GtkWidget *sig_tree_item = gtk_menu_item_new_with_label(
            "Send Signal to Tree");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(sig_tree_item), sig_tree_menu);

        for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++) {
            if (!signals[i].label) {
                gtk_menu_shell_append(GTK_MENU_SHELL(sig_tree_menu),
                                      gtk_separator_menu_item_new());
                continue;
            }
            GtkWidget *mi = gtk_menu_item_new_with_label(signals[i].label);
            g_signal_connect_data(mi, "activate", G_CALLBACK(on_send_signal),
                                  signal_data(tree_targets, signals[i].signo), signal_data_free, 0);
            gtk_menu_shell_append(GTK_MENU_SHELL(sig_tree_menu), mi);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), sig_tree_item);
    }

    g_array_unref(single_targets);
    g_array_unref(tree_targets);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

