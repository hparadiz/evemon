#include "ui_gtk_private.h"

/* ── pid_set helpers ─────────────────────────────────────────── */

/*
 * set_process_tree_node – record the expand/collapse state for a PID.
 *
 * pinned_pid = the PID of the pinned root that owns this subtree,
 *              or PTREE_UNPINNED (-1) for the main (unpinned) tree.
 * pid        = the actual process PID whose state we are recording.
 * state      = PTREE_COLLAPSED (1) or PTREE_EXPANDED (0).
 *
 * The (pinned_pid, pid) pair forms the composite key, so the same
 * PID can have independent expand/collapse state in multiple pinned
 * subtrees without collision.
 *
 * If the entry already exists its state is updated in place;
 * otherwise a new entry is appended.
 */
void set_process_tree_node(ptree_node_set_t *s, pid_t pinned_pid,
                           pid_t pid, int state)
{
    /* Update in place if the (pinned_pid, pid) pair already exists */
    for (size_t i = 0; i < s->count; i++) {
        if (s->pinned_pids[i] == pinned_pid && s->pids[i] == pid) {
            s->states[i] = state;
            return;
        }
    }

    /* Append new entry */
    if (s->count >= s->capacity) {
        size_t newcap = s->capacity ? s->capacity * 2 : 64;
        pid_t *tp = realloc(s->pinned_pids, newcap * sizeof(pid_t));
        if (!tp) return;   /* OOM – silently drop */
        s->pinned_pids = tp;
        pid_t *tk = realloc(s->pids, newcap * sizeof(pid_t));
        if (!tk) return;
        s->pids = tk;
        int *ts = realloc(s->states, newcap * sizeof(int));
        if (!ts) return;
        s->states   = ts;
        s->capacity = newcap;
    }
    s->pinned_pids[s->count] = pinned_pid;
    s->pids       [s->count] = pid;
    s->states     [s->count] = state;
    s->count++;
}

/*
 * get_process_tree_node – query the expand/collapse state for a PID.
 *
 * pinned_pid = the PID of the pinned root that owns this subtree,
 *              or PTREE_UNPINNED (-1) for the main (unpinned) tree.
 * pid        = the actual process PID to query.
 *
 * Returns the stored state (PTREE_COLLAPSED or PTREE_EXPANDED).
 * If the (pinned_pid, pid) pair has never been recorded, returns
 * PTREE_EXPANDED (the default: rows start expanded).
 */
int get_process_tree_node(const ptree_node_set_t *s, pid_t pinned_pid,
                          pid_t pid)
{
    for (size_t i = 0; i < s->count; i++)
        if (s->pinned_pids[i] == pinned_pid && s->pids[i] == pid)
            return s->states[i];
    return PTREE_EXPANDED;   /* default */
}

/*
 * gtk_ui_ptree_nodes_gc – evict entries for PIDs that are no longer alive in
 * the store.  Called once per refresh tick so the set never accumulates
 * unbounded state for processes that have come and gone.
 *
 * We keep entries whose PID still exists in pstore (any status other
 * than PROC_KILLED) OR whose pinned_pid is still in pstore (so that
 * pinned panels keep their expand state while visible).  Everything
 * else is compacted out.
 */

/* Local inline PID lookup in a store_ht_entry_t table.
 * Mirrors the static ht_find() in store.c / tree.c. */
static size_t ptree_ht_find(const store_ht_entry_t *ht, pid_t pid)
{
    unsigned h = (unsigned)pid % STORE_HT_SIZE;
    for (int k = 0; k < STORE_HT_SIZE; k++) {
        if (!ht[h].used) return (size_t)-1;
        if (ht[h].pid == pid) return ht[h].idx;
        h = (h + 1) % STORE_HT_SIZE;
    }
    return (size_t)-1;
}

void gtk_ui_ptree_nodes_gc(ptree_node_set_t *s, const proc_store_t *pstore)
{
    if (s->count == 0) return;

    size_t w = 0;
    for (size_t i = 0; i < s->count; i++) {
        pid_t pid        = s->pids[i];
        pid_t pinned_pid = s->pinned_pids[i];

        /* Keep if the actual process PID is still in the store */
        size_t pidx = ptree_ht_find(pstore->ht, pid);
        if (pidx != (size_t)-1 && pstore->records[pidx].status != PROC_KILLED) {
            if (w != i) {
                s->pinned_pids[w] = pinned_pid;
                s->pids       [w] = pid;
                s->states     [w] = s->states[i];
            }
            w++;
            continue;
        }

        if (pinned_pid == PTREE_UNPINNED) {
            /* Entry is for the main tree; PID is gone — drop it */
            continue;
        }

        /* Keep if the pinned_pid itself is still alive (the entry
         * records the expand state of some child in a pinned panel) */
        size_t ppidx = ptree_ht_find(pstore->ht, pinned_pid);
        if (ppidx != (size_t)-1 && pstore->records[ppidx].status != PROC_KILLED) {
            if (w != i) {
                s->pinned_pids[w] = pinned_pid;
                s->pids       [w] = pid;
                s->states     [w] = s->states[i];
            }
            w++;
        }
        /* otherwise: drop this entry */
    }
    s->count = w;
}

/* ── collapsed PID set ───────────────────────────────────────── */

/*
 * set_node_pid_state – add or remove a PID from the collapsed set.
 *
 * collapsed = 1: insert the PID (push) if not already present.
 * collapsed = 0: remove the PID  (pop)  if present, compacting the
 *                array in place.
 */
void set_node_pid_state(collapsed_pid_set_t *s, pid_t pid, int collapsed)
{
    if (collapsed) {
        /* Check for duplicate before inserting */
        for (size_t i = 0; i < s->count; i++)
            if (s->pids[i] == pid) return;   /* already tracked */

        if (s->count >= s->capacity) {
            size_t newcap = s->capacity ? s->capacity * 2 : 64;
            pid_t *tmp = realloc(s->pids, newcap * sizeof(pid_t));
            if (!tmp) return;   /* OOM – silently drop */
            s->pids     = tmp;
            s->capacity = newcap;
        }
        s->pids[s->count++] = pid;
    } else {
        /* Remove by swapping with the last element */
        for (size_t i = 0; i < s->count; i++) {
            if (s->pids[i] == pid) {
                s->pids[i] = s->pids[--s->count];
                return;
            }
        }
    }
}

/*
 * node_pid_is_collapsed – returns 1 if pid is in the collapsed set,
 * 0 otherwise.
 */
int node_pid_is_collapsed(const collapsed_pid_set_t *s, pid_t pid)
{
    for (size_t i = 0; i < s->count; i++)
        if (s->pids[i] == pid) return 1;
    return 0;
}

/* ── find a row by PID (recursive) ────────────────────────────── */

/*
 * Walk the tree model to find the row whose COL_PID equals `target`.
 * Returns TRUE and fills `result` if found, FALSE otherwise.
 */
gboolean find_iter_by_pid(GtkTreeModel *model, GtkTreeIter *parent,
                          pid_t target, GtkTreeIter *result)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint pid;
        gtk_tree_model_get(model, &iter, COL_PID, &pid, -1);
        if ((pid_t)pid == target) {
            *result = iter;
            return TRUE;
        }
        if (find_iter_by_pid(model, &iter, target, result))
            return TRUE;
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}

/*
 * Walk the tree model to find the row whose COL_PID equals `target`
 * AND whose COL_PINNED_ROOT equals `pinned_root`.  This disambiguates
 * between the pinned copy and the original tree row for the same PID.
 * Returns TRUE and fills `result` if found, FALSE otherwise.
 */
gboolean gtk_ui_find_iter_by_pid_and_pinned_root(GtkTreeModel *model,
                                                  GtkTreeIter  *parent,
                                                  pid_t target,
                                                  pid_t pinned_root,
                                                  GtkTreeIter  *result)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint pid, pr;
        gtk_tree_model_get(model, &iter,
                           COL_PID, &pid,
                           COL_PINNED_ROOT, &pr, -1);
        if ((pid_t)pid == target && (pid_t)pr == pinned_root) {
            *result = iter;
            return TRUE;
        }
        if (gtk_ui_find_iter_by_pid_and_pinned_root(model, &iter, target,
                                              pinned_root, result))
            return TRUE;
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}

/* ── middle-click autoscroll (browser-style) ─────────────────── */

/*
 * get_row_pinned_root – given a store iter, return its COL_PINNED_ROOT
 * value.  For rows in the normal tree this is PTREE_UNPINNED (-1);
 * for rows inside a pinned subtree it is the PID of the pinned root.
 *
 * This works by reading COL_PINNED_ROOT directly from the row –
 * every row in a pinned subtree is stamped with the same pinned root.
 */
pid_t get_row_pinned_root(GtkTreeModel *model, GtkTreeIter *iter)
{
    gint pr = (gint)PTREE_UNPINNED;
    gtk_tree_model_get(model, iter, COL_PINNED_ROOT, &pr, -1);
    return (pid_t)pr;
}

void stop_autoscroll(ui_ctx_t *ctx)
{
    ctx->autoscroll = FALSE;
    ctx->velocity_x = ctx->velocity_y = 0;

    if (ctx->scroll_timer) {
        g_source_remove(ctx->scroll_timer);
        ctx->scroll_timer = 0;
    }

    GdkDisplay *display = gdk_display_get_default();
    GdkSeat    *seat    = gdk_display_get_default_seat(display);
    gdk_seat_ungrab(seat);
}

/* Timer callback – apply velocity each tick. */
gboolean autoscroll_tick(gpointer data)
{
    ui_ctx_t *ctx = data;
    if (!ctx->autoscroll || ctx->shutting_down) return G_SOURCE_REMOVE;

    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(ctx->scroll);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(ctx->scroll);

    double hval = gtk_adjustment_get_value(hadj) + ctx->velocity_x;
    double vval = gtk_adjustment_get_value(vadj) + ctx->velocity_y;

    double hmax = gtk_adjustment_get_upper(hadj) - gtk_adjustment_get_page_size(hadj);
    double vmax = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj);

    if (hval < 0) hval = 0;
    if (hval > hmax) hval = hmax;
    if (vval < 0) vval = 0;
    if (vval > vmax) vval = vmax;

    gtk_adjustment_set_value(hadj, hval);
    gtk_adjustment_set_value(vadj, vval);

    return G_SOURCE_CONTINUE;
}

