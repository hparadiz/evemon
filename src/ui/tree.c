/*
 * tree.c – GtkTreeStore management: incremental diff-update,
 *          group RSS/CPU computation, initial populate.
 */

#include "ui_internal.h"
#include <time.h>

/* ── hash table for PID lookups ──────────────────────────────── */

#define HT_SIZE 8192

typedef struct { pid_t pid; size_t idx; int used; } ht_entry_t;

static void ht_insert(ht_entry_t *ht, pid_t pid, size_t idx)
{
    unsigned h = (unsigned)pid % HT_SIZE;
    while (ht[h].used)
        h = (h + 1) % HT_SIZE;
    ht[h].pid  = pid;
    ht[h].idx  = idx;
    ht[h].used = 1;
}

static size_t ht_find(const ht_entry_t *ht, pid_t pid)
{
    unsigned h = (unsigned)pid % HT_SIZE;
    for (int k = 0; k < HT_SIZE; k++) {
        if (!ht[h].used) return (size_t)-1;
        if (ht[h].pid == pid) return ht[h].idx;
        h = (h + 1) % HT_SIZE;
    }
    return (size_t)-1;
}

/* ── iter map: PID → GtkTreeIter for existing rows ───────────── */

typedef struct { pid_t pid; GtkTreeIter iter; } iter_map_entry_t;

typedef struct {
    iter_map_entry_t *entries;
    size_t            count;
    size_t            capacity;
} iter_map_t;

static void iter_map_add(iter_map_t *m, pid_t pid, GtkTreeIter *iter)
{
    if (m->count >= m->capacity) {
        size_t newcap = m->capacity ? m->capacity * 2 : 256;
        iter_map_entry_t *tmp = realloc(m->entries, newcap * sizeof(iter_map_entry_t));
        if (!tmp) return;   /* OOM – silently drop */
        m->entries  = tmp;
        m->capacity = newcap;
    }
    m->entries[m->count].pid  = pid;
    m->entries[m->count].iter = *iter;
    m->count++;
}

static GtkTreeIter *iter_map_find(iter_map_t *m, pid_t pid)
{
    for (size_t i = 0; i < m->count; i++)
        if (m->entries[i].pid == pid) return &m->entries[i].iter;
    return NULL;
}

/* ── collect existing tree rows into iter_map (recursive) ────── */

static void collect_iters(GtkTreeModel *model, GtkTreeIter *parent,
                          iter_map_t *map)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gint pid;
        gtk_tree_model_get(model, &iter, COL_PID, &pid, -1);
        iter_map_add(map, (pid_t)pid, &iter);
        collect_iters(model, &iter, map);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

/* ── remove dead rows (recursive, bottom-up) ─────────────────── */

/* Helper: current monotonic time in microseconds */
static gint64 mono_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gint64)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/*
 * Mark dead rows (process no longer in snapshot) with a death
 * timestamp instead of removing them immediately.  Rows whose
 * death timestamp is older than HIGHLIGHT_FADE_US are removed.
 * This gives the UI time to show a red fade-out highlight.
 */
static void remove_dead_rows(GtkTreeStore *store, GtkTreeIter *parent,
                             const ht_entry_t *new_ht)
{
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    gint64 now = mono_now_us();

    while (valid) {
        /* Recurse into children first (bottom-up removal) */
        remove_dead_rows(store, &iter, new_ht);

        gint pid;
        gint64 died;
        gtk_tree_model_get(model, &iter,
                           COL_PID, &pid,
                           COL_HIGHLIGHT_DIED, &died, -1);

        if (ht_find(new_ht, (pid_t)pid) == (size_t)-1) {
            /* Process gone */
            if (died == 0) {
                /* First time we see it missing – stamp with death time */
                gtk_tree_store_set(store, &iter,
                                   COL_HIGHLIGHT_DIED, now, -1);
                valid = gtk_tree_model_iter_next(model, &iter);
            } else if (now - died > HIGHLIGHT_FADE_US) {
                /* Fade period expired – remove the row */
                valid = gtk_tree_store_remove(store, &iter);
            } else {
                /* Still fading – keep it */
                valid = gtk_tree_model_iter_next(model, &iter);
            }
        } else {
            /* Process is alive – clear any lingering death stamp
             * (can happen if a PID is rapidly recycled). */
            if (died != 0)
                gtk_tree_store_set(store, &iter,
                                   COL_HIGHLIGHT_DIED, (gint64)0, -1);
            valid = gtk_tree_model_iter_next(model, &iter);
        }
    }
}

/* ── set row data from a proc_entry ──────────────────────────── */

static void set_row_data(GtkTreeStore *store, GtkTreeIter *iter,
                         const proc_entry_t *e)
{
    char rss_text[64];
    format_memory(e->mem_rss_kb, rss_text, sizeof(rss_text));

    char cpu_text[32];
    if (e->cpu_percent < 0.05)
        snprintf(cpu_text, sizeof(cpu_text), "0.0%%");
    else
        snprintf(cpu_text, sizeof(cpu_text), "%.1f%%", e->cpu_percent);

    /* Format I/O rates as human-readable byte/s strings */
    char io_read_text[64], io_write_text[64];
    {
        double r = e->io_read_rate;
        if (r < 0.5)
            snprintf(io_read_text, sizeof(io_read_text), "0 B/s");
        else if (r < 1024.0)
            snprintf(io_read_text, sizeof(io_read_text), "%.0f B/s", r);
        else if (r < 1024.0 * 1024.0)
            snprintf(io_read_text, sizeof(io_read_text), "%.1f KiB/s", r / 1024.0);
        else if (r < 1024.0 * 1024.0 * 1024.0)
            snprintf(io_read_text, sizeof(io_read_text), "%.1f MiB/s", r / (1024.0 * 1024.0));
        else
            snprintf(io_read_text, sizeof(io_read_text), "%.2f GiB/s", r / (1024.0 * 1024.0 * 1024.0));
    }
    {
        double w = e->io_write_rate;
        if (w < 0.5)
            snprintf(io_write_text, sizeof(io_write_text), "0 B/s");
        else if (w < 1024.0)
            snprintf(io_write_text, sizeof(io_write_text), "%.0f B/s", w);
        else if (w < 1024.0 * 1024.0)
            snprintf(io_write_text, sizeof(io_write_text), "%.1f KiB/s", w / 1024.0);
        else if (w < 1024.0 * 1024.0 * 1024.0)
            snprintf(io_write_text, sizeof(io_write_text), "%.1f MiB/s", w / (1024.0 * 1024.0));
        else
            snprintf(io_write_text, sizeof(io_write_text), "%.2f GiB/s", w / (1024.0 * 1024.0 * 1024.0));
    }

    char start_text[64] = "–";
    if (e->start_time > 0) {
        time_t t = (time_t)e->start_time;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(start_text, sizeof(start_text), "%Y-%m-%d %H:%M:%S", &tm);
    }

    gtk_tree_store_set(store, iter,
                       COL_PID,      (gint)e->pid,
                       COL_PPID,     (gint)e->ppid,
                       COL_USER,     e->user,
                       COL_NAME,     e->name,
                       COL_CPU,      (gint)(e->cpu_percent * 10000),
                       COL_CPU_TEXT, cpu_text,
                       COL_RSS,      (gint)(e->mem_rss_kb),
                       COL_RSS_TEXT, rss_text,
                       COL_GROUP_RSS,      (gint)0,
                       COL_GROUP_RSS_TEXT, "–",
                       COL_GROUP_CPU,      (gint)0,
                       COL_GROUP_CPU_TEXT, "0.0%",
                       COL_IO_READ_RATE,      (gint)(e->io_read_rate),
                       COL_IO_READ_RATE_TEXT,  io_read_text,
                       COL_IO_WRITE_RATE,      (gint)(e->io_write_rate),
                       COL_IO_WRITE_RATE_TEXT,  io_write_text,
                       COL_START_TIME,      (gint64)e->start_time,
                       COL_START_TIME_TEXT, start_text,
                       COL_CONTAINER,  e->container[0] ? e->container : "",
                       COL_SERVICE,    e->service[0]   ? e->service   : "",
                       COL_CWD,      e->cwd,
                       COL_CMDLINE,  e->cmdline,
                       COL_STEAM_LABEL, (e->steam && e->steam->is_steam) ? e->steam->display_label : "",
                       COL_HIGHLIGHT_BORN, (gint64)0,
                       COL_HIGHLIGHT_DIED, (gint64)0,
                       COL_PINNED_ROOT, (gint)PTREE_UNPINNED,
                       -1);
}

/* ── find the parent iter for a given ppid ───────────────────── */

static GtkTreeIter *find_parent_iter(iter_map_t *map, pid_t ppid, pid_t self_pid)
{
    if (ppid <= 0 || ppid == self_pid)
        return NULL;
    return iter_map_find(map, ppid);
}

/*
 * Incremental update: diff the new snapshot against the existing tree.
 *   1. Remove rows for processes that no longer exist.
 *   2. Update existing rows in-place.
 *   3. Insert new processes under the correct parent.
 *
 * This avoids clearing the store, so there's no visual flash, and
 * scroll position / expand state / selection are all preserved.
 */
void update_store(GtkTreeStore       *store,
                  GtkTreeView        *view,
                  const proc_entry_t *entries,
                  size_t              count)
{
    /* Build hash of new snapshot: PID → index */
    ht_entry_t *new_ht = calloc(HT_SIZE, sizeof(ht_entry_t));
    if (!new_ht) return;
    for (size_t i = 0; i < count; i++)
        ht_insert(new_ht, entries[i].pid, i);

    /* Phase 1: Remove dead rows */
    remove_dead_rows(store, NULL, new_ht);

    /* Phase 2: Collect remaining existing rows */
    iter_map_t existing = { NULL, 0, 0 };
    collect_iters(GTK_TREE_MODEL(store), NULL, &existing);

    /* Build a hash of existing PIDs for quick "already exists?" check */
    ht_entry_t *old_ht = calloc(HT_SIZE, sizeof(ht_entry_t));
    if (!old_ht) { free(new_ht); free(existing.entries); return; }
    for (size_t i = 0; i < existing.count; i++)
        ht_insert(old_ht, existing.entries[i].pid, i);

    /* Phase 3: Update existing rows in-place */
    for (size_t i = 0; i < existing.count; i++) {
        pid_t pid = existing.entries[i].pid;
        size_t sidx = ht_find(new_ht, pid);
        if (sidx != (size_t)-1) {
            /* Preserve highlight timestamps across data refresh */
            gint64 born = 0, died = 0;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &existing.entries[i].iter,
                               COL_HIGHLIGHT_BORN, &born,
                               COL_HIGHLIGHT_DIED, &died, -1);
            set_row_data(store, &existing.entries[i].iter, &entries[sidx]);
            gtk_tree_store_set(store, &existing.entries[i].iter,
                               COL_HIGHLIGHT_BORN, born,
                               COL_HIGHLIGHT_DIED, died, -1);
        }
    }

    /* Phase 4: Insert new processes.
     * We need to insert parents before children, so we use the same
     * ancestor-stack approach as before. */
    int *inserted = calloc(count, sizeof(int));
    if (!inserted) { free(new_ht); free(old_ht); free(existing.entries); return; }

    /* Mark already-existing entries as inserted */
    for (size_t i = 0; i < count; i++) {
        if (ht_find(old_ht, entries[i].pid) != (size_t)-1)
            inserted[i] = 1;
    }

    pid_t stack[64];
    int sp;

    for (size_t i = 0; i < count; i++) {
        if (inserted[i]) continue;

        /* Build ancestor stack */
        sp = 0;
        size_t cur = i;
        while (!inserted[cur]) {
            if (sp >= 64) break;
            stack[sp++] = entries[cur].pid;

            pid_t pp = entries[cur].ppid;
            size_t pidx = ht_find(new_ht, pp);
            if (pidx == (size_t)-1 || pidx == cur) break;
            if (inserted[pidx]) break;
            cur = pidx;
        }

        /* Pop stack: insert outermost ancestor first */
        while (sp > 0) {
            pid_t p = stack[--sp];
            size_t sidx = ht_find(new_ht, p);
            if (sidx == (size_t)-1 || inserted[sidx]) continue;

            const proc_entry_t *e = &entries[sidx];

            /* Find parent iter – check both existing map and freshly
             * inserted entries (which we add to existing as we go). */
            GtkTreeIter *parent_iter = find_parent_iter(&existing,
                                                         e->ppid, e->pid);

            GtkTreeIter new_iter;
            gtk_tree_store_append(store, &new_iter, parent_iter);
            set_row_data(store, &new_iter, e);

            /* Stamp newly inserted process with a birth highlight */
            gtk_tree_store_set(store, &new_iter,
                               COL_HIGHLIGHT_BORN, mono_now_us(), -1);

            /* Add to existing map so children can find us */
            iter_map_add(&existing, e->pid, &new_iter);
            inserted[sidx] = 1;
        }
    }

    free(new_ht);
    free(old_ht);
    free(existing.entries);
    free(inserted);
}

/*
 * Recursively compute the group RSS for every row in the tree.
 * Group RSS = own RSS + sum of all descendants' RSS.
 * Returns the group total for the subtree rooted at `parent`.
 */
long compute_group_rss(GtkTreeStore *store, GtkTreeIter *parent)
{
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(model, &child, parent);

    while (valid) {
        compute_group_rss(store, &child);
        valid = gtk_tree_model_iter_next(model, &child);
    }

    /* Now every child has its group RSS computed.  Sum them. */
    if (!parent) return 0;   /* top-level call, nothing to store */

    gint own_rss = 0;
    gtk_tree_model_get(model, parent, COL_RSS, &own_rss, -1);

    long total = (long)own_rss;
    valid = gtk_tree_model_iter_children(model, &child, parent);
    while (valid) {
        gint child_group;
        gtk_tree_model_get(model, &child, COL_GROUP_RSS, &child_group, -1);
        total += (long)child_group;
        valid = gtk_tree_model_iter_next(model, &child);
    }

    char grp_text[64];
    format_memory(total, grp_text, sizeof(grp_text));
    gtk_tree_store_set(store, parent,
                       COL_GROUP_RSS,      (gint)total,
                       COL_GROUP_RSS_TEXT, grp_text,
                       -1);
    return total;
}

/*
 * Recursively compute the group CPU% for every row in the tree.
 * Group CPU% = own CPU% + sum of all descendants' CPU%.
 * Works with the raw ×10000 int values for precision; formats text.
 * Returns the group total (×10000) for the subtree rooted at `parent`.
 */
long compute_group_cpu(GtkTreeStore *store, GtkTreeIter *parent)
{
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(model, &child, parent);

    while (valid) {
        compute_group_cpu(store, &child);
        valid = gtk_tree_model_iter_next(model, &child);
    }

    /* Now every child has its group CPU computed.  Sum them. */
    if (!parent) return 0;   /* top-level call, nothing to store */

    gint own_cpu = 0;
    gtk_tree_model_get(model, parent, COL_CPU, &own_cpu, -1);

    long total = (long)own_cpu;
    valid = gtk_tree_model_iter_children(model, &child, parent);
    while (valid) {
        gint child_group;
        gtk_tree_model_get(model, &child, COL_GROUP_CPU, &child_group, -1);
        total += (long)child_group;
        valid = gtk_tree_model_iter_next(model, &child);
    }

    char grp_text[32];
    double pct = total / 10000.0;
    if (pct < 0.05)
        snprintf(grp_text, sizeof(grp_text), "0.0%%");
    else
        snprintf(grp_text, sizeof(grp_text), "%.1f%%", pct);
    gtk_tree_store_set(store, parent,
                       COL_GROUP_CPU,      (gint)total,
                       COL_GROUP_CPU_TEXT, grp_text,
                       -1);
    return total;
}

/*
 * Full populate for the initial load (tree is empty).
 * Uses the same ancestor-stack insertion as before.
 */
void populate_store_initial(GtkTreeStore       *store,
                            GtkTreeView        *view,
                            const proc_entry_t *entries,
                            size_t              count)
{
    if (count == 0) return;

    ht_entry_t *ht = calloc(HT_SIZE, sizeof(ht_entry_t));
    if (!ht) return;
    for (size_t i = 0; i < count; i++)
        ht_insert(ht, entries[i].pid, i);

    int          *inserted = calloc(count, sizeof(int));
    GtkTreeIter  *iters    = calloc(count, sizeof(GtkTreeIter));
    if (!inserted || !iters) {
        free(ht); free(inserted); free(iters);
        return;
    }

    pid_t stack[64];
    int sp;

    for (size_t i = 0; i < count; i++) {
        if (inserted[i]) continue;

        sp = 0;
        size_t cur = i;
        while (!inserted[cur]) {
            if (sp >= 64) break;
            stack[sp++] = entries[cur].pid;
            pid_t pp = entries[cur].ppid;
            size_t pidx = ht_find(ht, pp);
            if (pidx == (size_t)-1 || pidx == cur) break;
            if (inserted[pidx]) break;
            cur = pidx;
        }

        while (sp > 0) {
            pid_t p = stack[--sp];
            size_t sidx = ht_find(ht, p);
            if (sidx == (size_t)-1 || inserted[sidx]) continue;

            const proc_entry_t *e = &entries[sidx];
            GtkTreeIter *parent_iter = NULL;
            size_t pidx = ht_find(ht, e->ppid);
            if (pidx != (size_t)-1 && inserted[pidx] && pidx != sidx)
                parent_iter = &iters[pidx];

            gtk_tree_store_append(store, &iters[sidx], parent_iter);
            set_row_data(store, &iters[sidx], e);
            inserted[sidx] = 1;
        }
    }

    free(ht);
    free(inserted);
    free(iters);

    /* Expand everything on first load */
    gtk_tree_view_expand_all(view);
}

/* ── pinned-process subtree management ───────────────────────── */

/*
 * Remove all top-level rows whose COL_PINNED_ROOT != PTREE_UNPINNED.
 * Called before rebuilding the pinned section so stale copies are purged.
 */
void remove_pinned_rows(GtkTreeStore *store)
{
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

    while (valid) {
        gint pr = 0;
        gtk_tree_model_get(model, &iter, COL_PINNED_ROOT, &pr, -1);
        if (pr != (gint)PTREE_UNPINNED) {
            valid = gtk_tree_store_remove(store, &iter);
        } else {
            valid = gtk_tree_model_iter_next(model, &iter);
        }
    }
}

/*
 * Deep-copy a subtree from `src_iter` (and all children) into `dst`
 * under `dst_parent`, stamping every row with the given pinned_root.
 */
static void copy_subtree_pinned(GtkTreeStore *dst, GtkTreeIter *dst_parent,
                                GtkTreeModel *src, GtkTreeIter *src_iter,
                                gint pinned_root)
{
    GtkTreeIter dst_iter;
    gtk_tree_store_append(dst, &dst_iter, dst_parent);

    gint pid, ppid, cpu, rss, grp_rss, grp_cpu;
    gint io_read_rate, io_write_rate;
    gint64 start_time, hl_born, hl_died;
    gchar *user = NULL, *name = NULL, *cpu_text = NULL, *rss_text = NULL;
    gchar *grp_rss_text = NULL, *grp_cpu_text = NULL;
    gchar *io_read_text = NULL, *io_write_text = NULL;
    gchar *start_text = NULL, *container = NULL, *service = NULL,
          *cwd = NULL, *cmdline = NULL, *steam_label = NULL;

    gtk_tree_model_get(src, src_iter,
        COL_PID, &pid, COL_PPID, &ppid, COL_USER, &user, COL_NAME, &name,
        COL_CPU, &cpu, COL_CPU_TEXT, &cpu_text,
        COL_RSS, &rss, COL_RSS_TEXT, &rss_text,
        COL_GROUP_RSS, &grp_rss, COL_GROUP_RSS_TEXT, &grp_rss_text,
        COL_GROUP_CPU, &grp_cpu, COL_GROUP_CPU_TEXT, &grp_cpu_text,
        COL_IO_READ_RATE, &io_read_rate, COL_IO_READ_RATE_TEXT, &io_read_text,
        COL_IO_WRITE_RATE, &io_write_rate, COL_IO_WRITE_RATE_TEXT, &io_write_text,
        COL_START_TIME, &start_time, COL_START_TIME_TEXT, &start_text,
        COL_CONTAINER, &container, COL_SERVICE, &service,
        COL_CWD, &cwd, COL_CMDLINE, &cmdline,
        COL_STEAM_LABEL, &steam_label,
        COL_HIGHLIGHT_BORN, &hl_born, COL_HIGHLIGHT_DIED, &hl_died,
        -1);

    gtk_tree_store_set(dst, &dst_iter,
        COL_PID, pid, COL_PPID, ppid, COL_USER, user, COL_NAME, name,
        COL_CPU, cpu, COL_CPU_TEXT, cpu_text,
        COL_RSS, rss, COL_RSS_TEXT, rss_text,
        COL_GROUP_RSS, grp_rss, COL_GROUP_RSS_TEXT, grp_rss_text,
        COL_GROUP_CPU, grp_cpu, COL_GROUP_CPU_TEXT, grp_cpu_text,
        COL_IO_READ_RATE, io_read_rate, COL_IO_READ_RATE_TEXT, io_read_text,
        COL_IO_WRITE_RATE, io_write_rate, COL_IO_WRITE_RATE_TEXT, io_write_text,
        COL_START_TIME, start_time, COL_START_TIME_TEXT, start_text,
        COL_CONTAINER, container, COL_SERVICE, service,
        COL_CWD, cwd, COL_CMDLINE, cmdline,
        COL_STEAM_LABEL, steam_label,
        COL_HIGHLIGHT_BORN, hl_born, COL_HIGHLIGHT_DIED, hl_died,
        COL_PINNED_ROOT, pinned_root,
        -1);

    g_free(user); g_free(name); g_free(cpu_text); g_free(rss_text);
    g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(io_read_text); g_free(io_write_text); g_free(start_text);
    g_free(container); g_free(service); g_free(cwd); g_free(cmdline);
    g_free(steam_label);

    /* Recurse into children */
    GtkTreeIter child;
    gboolean valid = gtk_tree_model_iter_children(src, &child, src_iter);
    while (valid) {
        copy_subtree_pinned(dst, &dst_iter, src, &child, pinned_root);
        valid = gtk_tree_model_iter_next(src, &child);
    }
}

/*
 * Rebuild the pinned section of the store.  Removes all existing pinned
 * rows, then for each PID in pinned_pids, finds the row in the normal
 * tree (COL_PINNED_ROOT == PTREE_UNPINNED) and copies it + descendants
 * as a new top-level subtree with COL_PINNED_ROOT = that pid.
 *
 * The sort comparators ensure pinned rows always float to the top.
 */
void rebuild_pinned_rows(GtkTreeStore *store,
                         const pid_t *pinned_pids, size_t pinned_count)
{
    remove_pinned_rows(store);

    if (pinned_count == 0)
        return;

    GtkTreeModel *model = GTK_TREE_MODEL(store);

    for (size_t i = 0; i < pinned_count; i++) {
        GtkTreeIter src_iter;
        if (!find_iter_by_pid(model, NULL, pinned_pids[i], &src_iter))
            continue;  /* process no longer alive – skip */

        /* Verify this is a normal-tree row, not another pinned copy */
        gint pr = 0;
        gtk_tree_model_get(model, &src_iter, COL_PINNED_ROOT, &pr, -1);
        if (pr != (gint)PTREE_UNPINNED)
            continue;

        copy_subtree_pinned(store, NULL, model, &src_iter,
                            (gint)pinned_pids[i]);
    }
}
