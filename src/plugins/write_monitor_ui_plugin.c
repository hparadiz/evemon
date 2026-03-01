#include "../evemon_plugin.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const evemon_host_services_t *hsvc;
    GtkWidget *root;
    GtkWidget *combo;
    GtkWidget *check_descendants; /* GtkCheckButton */
    GtkWidget *view; /* GtkTextView */
    GtkTextBuffer *buf;
    pid_t current_pid;
    int subscribed_fd_stdout;
    int subscribed_fd_stderr;
    int include_descendants;     /* bool: also monitor child processes */
    /* Tracked descendant PIDs we have subscribed */
    pid_t *desc_pids;            /* heap-allocated array */
    size_t desc_count;
    size_t desc_cap;
    int sub_pid_select_id;
    int sub_fd_write_id;
} wm_ui_ctx_t;

static void on_fd_write_event(const evemon_event_t *ev, void *user_data)
{
    if (!ev || ev->type != EVEMON_EVENT_FD_WRITE) return;
    wm_ui_ctx_t *c = user_data;
    if (!c || !c->buf) return;
    evemon_fd_write_payload_t *p = (evemon_fd_write_payload_t *)ev->payload;
    if (!p || p->len == 0) return;

    /* Only show writes on fds we actually subscribed, from the selected
     * process or a tracked descendant. */
    int fd_subscribed = (p->fd == 1 && c->subscribed_fd_stdout) ||
                        (p->fd == 2 && c->subscribed_fd_stderr);
    if (!fd_subscribed) return;
    if (c->current_pid == 0) return;

    int pid_match = (p->tgid == c->current_pid);
    if (!pid_match && c->include_descendants) {
        /* BPF already filters to the process tree we're watching —
         * any write that arrives here from a non-root pid IS a descendant.
         * Accept it without requiring explicit desc_pids registration,
         * which loses the race against short-lived workers. */
        pid_match = 1;
    }
    if (!pid_match) {
        for (size_t i = 0; i < c->desc_count; ++i) {
            if (c->desc_pids[i] == p->tgid) { pid_match = 1; break; }
        }
    }
    if (!pid_match) return;

    /* Build a sanitised copy: keep printable ASCII and newlines/tabs,
     * drop everything else (NUL, control chars, high bytes). */
    size_t src_len = p->len;
    if (src_len > sizeof(p->data)) src_len = sizeof(p->data);

    char *out = malloc(src_len + 1);
    if (!out) return;

    size_t out_len = 0;
    for (size_t i = 0; i < src_len; ++i) {
        unsigned char ch = (unsigned char)p->data[i];
        if (ch == '\n' || ch == '\t' || (ch >= 32 && ch < 127))
            out[out_len++] = (char)ch;
    }
    out[out_len] = '\0';

    if (out_len > 0) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(c->buf, &end);
        gtk_text_buffer_insert(c->buf, &end, out, (gint)out_len);

        /* Auto-scroll to bottom */
        GtkTextMark *mark = gtk_text_buffer_get_insert(c->buf);
        GtkWidget *tv = gtk_bin_get_child(GTK_BIN(c->view));
        if (tv)
            gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(tv), mark);
    }

    free(out);
}

static int subscribe_to_fd(wm_ui_ctx_t *c, pid_t pid, int fd)
{
    if (!c || !c->hsvc) return -1;
    if (!c->hsvc->monitor_fd_subscribe) return -1;
    return c->hsvc->monitor_fd_subscribe(c->hsvc->host_ctx, pid, fd);
}

static void unsubscribe_from_fd(wm_ui_ctx_t *c, pid_t pid, int fd)
{
    if (!c || !c->hsvc || !c->hsvc->monitor_fd_unsubscribe) return;
    c->hsvc->monitor_fd_unsubscribe(c->hsvc->host_ctx, pid, fd);
}

/* Subscribe a descendant pid to whichever fds are currently active.
 * Adds it to the tracked list so we can unsubscribe it later. */
static void subscribe_descendant(wm_ui_ctx_t *c, pid_t dpid)
{
    /* Already tracked? */
    for (size_t i = 0; i < c->desc_count; ++i)
        if (c->desc_pids[i] == dpid) return;

    if (c->subscribed_fd_stdout) subscribe_to_fd(c, dpid, 1);
    if (c->subscribed_fd_stderr) subscribe_to_fd(c, dpid, 2);

    /* Grow array if needed */
    if (c->desc_count >= c->desc_cap) {
        size_t new_cap = c->desc_cap ? c->desc_cap * 2 : 8;
        pid_t *tmp = realloc(c->desc_pids, new_cap * sizeof(pid_t));
        if (!tmp) return;
        c->desc_pids = tmp;
        c->desc_cap = new_cap;
    }
    c->desc_pids[c->desc_count++] = dpid;
}

/* Unsubscribe and forget all currently tracked descendants. */
static void unsubscribe_all_descendants(wm_ui_ctx_t *c)
{
    for (size_t i = 0; i < c->desc_count; ++i) {
        if (c->subscribed_fd_stdout) unsubscribe_from_fd(c, c->desc_pids[i], 1);
        if (c->subscribed_fd_stderr) unsubscribe_from_fd(c, c->desc_pids[i], 2);
    }
    c->desc_count = 0;
}

/*
 * Apply fd subscription state for the current pid according to the combo index.
 * Assumes c->current_pid is already set to the target pid and the old pid's
 * subscriptions have already been torn down.  Only adjusts what needs changing.
 */
static void apply_selection(wm_ui_ctx_t *c, int selection_index)
{
    if (!c || c->current_pid == 0) return;

    pid_t pid = c->current_pid;
    int want_stdout = (selection_index == 1 || selection_index == 3);
    int want_stderr = (selection_index == 2 || selection_index == 3);

    /* STDOUT */
    if (want_stdout && !c->subscribed_fd_stdout) {
        if (subscribe_to_fd(c, pid, 1) == 0)
            c->subscribed_fd_stdout = 1;
    } else if (!want_stdout && c->subscribed_fd_stdout) {
        unsubscribe_from_fd(c, pid, 1);
        c->subscribed_fd_stdout = 0;
    }

    /* STDERR */
    if (want_stderr && !c->subscribed_fd_stderr) {
        if (subscribe_to_fd(c, pid, 2) == 0)
            c->subscribed_fd_stderr = 1;
    } else if (!want_stderr && c->subscribed_fd_stderr) {
        unsubscribe_from_fd(c, pid, 2);
        c->subscribed_fd_stderr = 0;
    }
}

/*
 * Tear down all subscriptions and BPF watching for c->current_pid, then
 * reset tracking state.  After this, current_pid == 0.
 */
static void clear_current_pid(wm_ui_ctx_t *c)
{
    if (!c || c->current_pid == 0) return;

    if (c->subscribed_fd_stdout) {
        unsubscribe_from_fd(c, c->current_pid, 1);
        c->subscribed_fd_stdout = 0;
    }
    if (c->subscribed_fd_stderr) {
        unsubscribe_from_fd(c, c->current_pid, 2);
        c->subscribed_fd_stderr = 0;
    }
    unsubscribe_all_descendants(c);

    if (c->hsvc && c->hsvc->monitor_unwatch_children)
        c->hsvc->monitor_unwatch_children(c->hsvc->host_ctx, c->current_pid);

    c->current_pid = 0;
}

/*
 * Set up subscriptions and BPF watching for a new pid, according to the
 * current combo selection.  Assumes current_pid is already 0 (call
 * clear_current_pid first).
 */
static void activate_pid(wm_ui_ctx_t *c, pid_t pid)
{
    if (!c || pid == 0) return;

    c->current_pid = pid;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(c->combo));
    apply_selection(c, idx);

    /* Register as watched parent in BPF so all descendants are captured
     * immediately on fork, with no exec-event race. */
    if (c->hsvc && c->hsvc->monitor_watch_children) {
        int mask = (c->subscribed_fd_stdout ? 1 : 0) |
                   (c->subscribed_fd_stderr ? 2 : 0);
        if (mask)
            c->hsvc->monitor_watch_children(c->hsvc->host_ctx, pid, mask);
    }
}

static void on_combo_changed(GtkComboBox *combo, gpointer user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c || c->current_pid == 0) return;
    int idx = gtk_combo_box_get_active(combo);
    apply_selection(c, idx);
    /* Update the BPF watch mask to match the new fd selection. */
    if (c->hsvc && c->hsvc->monitor_watch_children) {
        int mask = (c->subscribed_fd_stdout ? 1 : 0) |
                   (c->subscribed_fd_stderr ? 2 : 0);
        if (mask)
            c->hsvc->monitor_watch_children(c->hsvc->host_ctx, c->current_pid, mask);
        else
            c->hsvc->monitor_unwatch_children(c->hsvc->host_ctx, c->current_pid);
    }
}

static void on_check_descendants_toggled(GtkToggleButton *btn, gpointer user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c) return;
    c->include_descendants = gtk_toggle_button_get_active(btn);
    if (!c->include_descendants)
        unsubscribe_all_descendants(c);
    /* When turned on, descendants will be picked up on the next plugin_update. */
}

static void on_process_selected(const evemon_event_t *ev, void *user_data)
{
    wm_ui_ctx_t *c = user_data;
    if (!c) return;
    pid_t sel = ev->payload ? *(pid_t *)ev->payload : 0;
    if (c->current_pid == sel) return;  /* no change */

    clear_current_pid(c);
    activate_pid(c, sel);
}

/* create widget */
static GtkWidget *plugin_create_widget(void *ctx)
{
    wm_ui_ctx_t *c = ctx;
    if (!c) return NULL;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl = gtk_label_new("Monitor writes to:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);

    c->combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->combo), "None");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->combo), "STDOUT (fd 1)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->combo), "STDERR (fd 2)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->combo), "Both");
    gtk_combo_box_set_active(GTK_COMBO_BOX(c->combo), 0);
    gtk_box_pack_start(GTK_BOX(hbox), c->combo, FALSE, FALSE, 0);

    c->check_descendants = gtk_check_button_new_with_label("Include descendants");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->check_descendants), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), c->check_descendants, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(box), hbox, FALSE, FALSE, 0);

    c->view = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(c->view), text);
    gtk_widget_set_vexpand(c->view, TRUE);
    gtk_widget_set_hexpand(c->view, TRUE);
    gtk_box_pack_start(GTK_BOX(box), c->view, TRUE, TRUE, 0);

    c->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));

    g_signal_connect(c->combo, "changed", G_CALLBACK(on_combo_changed), c);
    g_signal_connect(c->check_descendants, "toggled",
                     G_CALLBACK(on_check_descendants_toggled), c);

    c->root = box;
    return box;
}

static void plugin_activate(void *ctx, const evemon_host_services_t *services)
{
    wm_ui_ctx_t *c = ctx;
    if (!c) return;
    c->hsvc = services;
    c->current_pid = 0;
    c->subscribed_fd_stdout = 0;
    c->subscribed_fd_stderr = 0;
    c->include_descendants = 0;
    c->desc_pids = NULL;
    c->desc_count = 0;
    c->desc_cap = 0;
    c->sub_pid_select_id = 0;
    c->sub_fd_write_id = 0;

    if (services && services->subscribe) {
        c->sub_pid_select_id = services->subscribe(services->host_ctx,
                                                   EVEMON_EVENT_PROCESS_SELECTED,
                                                   on_process_selected, c);
        c->sub_fd_write_id = services->subscribe(services->host_ctx,
                                                 EVEMON_EVENT_FD_WRITE,
                                                 on_fd_write_event, c);
    }
}

static void plugin_destroy(void *ctx)
{
    wm_ui_ctx_t *c = ctx;
    if (!c) return;

    if (c->hsvc) {
        clear_current_pid(c);
        if (c->sub_pid_select_id && c->hsvc->unsubscribe) c->hsvc->unsubscribe(c->hsvc->host_ctx, c->sub_pid_select_id);
        if (c->sub_fd_write_id && c->hsvc->unsubscribe) c->hsvc->unsubscribe(c->hsvc->host_ctx, c->sub_fd_write_id);
    }

    free(c->desc_pids);
    /* free widgets are managed by GTK; free context */
    free(c);
}

/* update: called when the host has a fresh evemon_proc_data_t for the tracked PID */
static void plugin_update(void *ctx, const evemon_proc_data_t *data)
{
    wm_ui_ctx_t *c = ctx;
    if (!c || !data) return;
    pid_t sel = data->pid;
    /* React when PID changes */
    if (c->current_pid != sel) {
        clear_current_pid(c);
        activate_pid(c, sel);
    }

    /* If descendants checkbox is on and we have active fd subscriptions,
     * subscribe any new descendant PIDs we haven't seen yet. */
    if (c->include_descendants &&
        (c->subscribed_fd_stdout || c->subscribed_fd_stderr) &&
        data->descendant_pids && data->descendant_count > 0) {
        for (size_t i = 0; i < data->descendant_count; ++i)
            subscribe_descendant(c, data->descendant_pids[i]);
    }
}

static void plugin_clear(void *ctx)
{
    wm_ui_ctx_t *c = ctx;
    if (!c) return;
    clear_current_pid(c);
    if (c->buf)
        gtk_text_buffer_set_text(c->buf, "", 0);
}

/* init */

evemon_plugin_t *evemon_plugin_init(void)
{
    evemon_plugin_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    wm_ui_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) { free(p); return NULL; }

    p->abi_version = evemon_PLUGIN_ABI_VERSION;
    p->name = "Write Monitor UI";
    p->id = "org.evemon.write_monitor_ui";
    p->version = "1.0";
    p->data_needs = evemon_NEED_DESCENDANTS;
    p->plugin_ctx = c;
    p->create_widget = plugin_create_widget;
    p->update = plugin_update;
    p->clear = plugin_clear;
    p->destroy = plugin_destroy;
    p->activate = plugin_activate;
    p->kind = EVEMON_PLUGIN_UI;

    return p;
}
