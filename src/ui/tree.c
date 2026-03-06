/*
 * tree.c – GtkTreeStore management: incremental diff-update,
 *          group RSS/CPU computation, initial populate.
 */

#include "ui_internal.h"
#include "proc_icon.h"
#include "store.h"
#include <time.h>

/* ── local hash helpers (operate on store_ht_entry_t arrays) ─── */
/*
 * tree.c allocates temporary store_ht_entry_t arrays for local
 * PID→index lookups (old_ht, new_ht inside update_store).
 * These mirror the static helpers in store.c but operate on
 * caller-supplied arrays, so they must live here too.
 */
static void ht_insert(store_ht_entry_t *ht, pid_t pid, size_t idx)
{
    unsigned h = (unsigned)pid % STORE_HT_SIZE;
    for (int k = 0; k < STORE_HT_SIZE; k++) {
        if (!ht[h].used) {
            ht[h].pid  = pid;
            ht[h].idx  = idx;
            ht[h].used = 1;
            return;
        }
        h = (h + 1) % STORE_HT_SIZE;
    }
}

static size_t ht_find(const store_ht_entry_t *ht, pid_t pid)
{
    unsigned h = (unsigned)pid % STORE_HT_SIZE;
    for (int k = 0; k < STORE_HT_SIZE; k++) {
        if (!ht[h].used) return (size_t)-1;
        if (ht[h].pid == pid) return ht[h].idx;
        h = (h + 1) % STORE_HT_SIZE;
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

    /* Build sparkline data string: semicolon-separated float values */
    char spark_buf[IO_HISTORY_LEN * 16 + 1];
    spark_buf[0] = '\0';
    if (e->io_history_len > 0) {
        char *p = spark_buf;
        size_t remaining = sizeof(spark_buf);
        for (int i = 0; i < e->io_history_len && remaining > 16; i++) {
            int n = snprintf(p, remaining, "%.0f;", (double)e->io_history[i]);
            if (n < 0 || (size_t)n >= remaining) break;
            p += n;
            remaining -= (size_t)n;
        }
    }

    /* Current combined I/O rate as peak for glow animation */
    double combined_rate = e->io_read_rate + e->io_write_rate;
    /* Normalise: 10 MiB/s → peak of 1000 */
    int spark_peak = (int)(combined_rate / (10.0 * 1024.0 * 1024.0) * 1000.0);
    if (spark_peak > 1000) spark_peak = 1000;

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
                       COL_CMDLINE,  PROC_CMDLINE(e),
                       COL_STEAM_LABEL, (e->steam && e->steam->is_steam) ? e->steam->display_label : "",
                       COL_IO_SPARKLINE, spark_buf,
                       COL_IO_SPARKLINE_PEAK, spark_peak,
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

/* ── remove / update dead rows (recursive, bottom-up) ────────── */
/*
 * Walk the GTK tree bottom-up.  For each row:
 *   - PROC_KILLED (or absent from store): gtk_tree_store_remove.
 *   - PROC_DYING:  update data + stamp COL_HIGHLIGHT_DIED.
 *   - PROC_ALIVE:  update data in-place, preserve highlight timestamps.
 *
 * Bottom-up ordering ensures that when a parent is removed its children
 * are already gone, so we never hand GTK a stale child iter.
 */
static void update_or_remove_rows(GtkTreeStore *store, GtkTreeIter *parent,
                                  const proc_store_t *pstore)
{
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);

    while (valid) {
        /* Recurse first so children are handled before the parent */
        update_or_remove_rows(store, &iter, pstore);

        gint pid_val;
        gtk_tree_model_get(model, &iter, COL_PID, &pid_val, -1);
        pid_t pid = (pid_t)pid_val;

        size_t ridx = ht_find(pstore->ht, pid);

        if (ridx == (size_t)-1) {
            /* Store already compacted this PID — remove immediately */
            valid = gtk_tree_store_remove(store, &iter);
            continue;
        }

        const proc_record_t *rec = &pstore->records[ridx];

        if (rec->status == PROC_KILLED) {
            valid = gtk_tree_store_remove(store, &iter);
            continue;
        }

        if (rec->status == PROC_DYING) {
            gint64 born = 0;
            gtk_tree_model_get(model, &iter, COL_HIGHLIGHT_BORN, &born, -1);
            set_row_data(store, &iter, &rec->entry);

            /* Only start the red animation after the process has been
             * absent for at least HIGHLIGHT_START_DELAY_US.  Before
             * that threshold we keep COL_HIGHLIGHT_DIED = 0 so nothing
             * is rendered, avoiding flicker on transient absences. */
            struct timespec _ts;
            clock_gettime(CLOCK_MONOTONIC, &_ts);
            int64_t now_us = (int64_t)_ts.tv_sec * 1000000LL
                           + _ts.tv_nsec / 1000;
            gint64 died_stamp = (now_us - rec->died_at_us >= HIGHLIGHT_START_DELAY_US)
                                ? (gint64)rec->died_at_us
                                : (gint64)0;
            gtk_tree_store_set(store, &iter,
                               COL_HIGHLIGHT_BORN, born,
                               COL_HIGHLIGHT_DIED, died_stamp,
                               -1);
        } else {
            /* PROC_ALIVE — update in-place, preserve timestamps */
            gint64 born = 0, died = 0;
            gtk_tree_model_get(model, &iter,
                               COL_HIGHLIGHT_BORN, &born,
                               COL_HIGHLIGHT_DIED, &died, -1);
            set_row_data(store, &iter, &rec->entry);
            gtk_tree_store_set(store, &iter,
                               COL_HIGHLIGHT_BORN, born,
                               COL_HIGHLIGHT_DIED, died, -1);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

/* ── icon-resolved callback ───────────────────────────────────── */

/*
 * Called (possibly async) when proc_icon_lookup_async() resolves an icon.
 * Walks the entire store and stamps every row whose comm matches `key`
 * with the resolved pixbuf.  O(N) but N is small and this fires at most
 * once per unique process name (cache hit → synchronous, no store walk).
 */
typedef struct {
    GtkTreeStore *store;
    char          key[PROC_NAME_MAX];  /* same key format used by proc_icon_lookup_async */
} icon_cb_data_t;

static void walk_set_icon(GtkTreeModel *model, GtkTreeIter *parent,
                          const char *key, GdkPixbuf *pb)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    while (valid) {
        gchar *name = NULL;
        gchar *steam_label = NULL;
        gtk_tree_model_get(model, &iter,
                           COL_NAME, &name,
                           COL_STEAM_LABEL, &steam_label,
                           -1);

        /* Match by comm name or steam:<appid> key */
        gboolean match = FALSE;
        if (name) {
            /* direct comm match */
            if (strcmp(name, key) == 0)
                match = TRUE;
            /* steam key: "steam:<appid>" — match any steam row */
            if (!match && strncmp(key, "steam:", 6) == 0 &&
                steam_label && steam_label[0])
                match = TRUE;
        }

        if (match)
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                               COL_ICON, pb, -1);

        g_free(name);
        g_free(steam_label);

        walk_set_icon(model, &iter, key, pb);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

static void on_icon_resolved(const char *key, GdkPixbuf *pb, void *userdata)
{
    icon_cb_data_t *d = userdata;
    if (pb)   /* NULL = miss, no need to walk */
        walk_set_icon(GTK_TREE_MODEL(d->store), NULL, key, pb);
    g_free(d);
}

/*
 * Fire a non-blocking icon lookup for a newly inserted row.
 * If already cached the callback fires synchronously and sets COL_ICON
 * before returning; otherwise sets it asynchronously.
 */
static void request_icon(ui_ctx_t *ctx, GtkTreeStore *store,
                         GtkTreeIter *iter, const proc_entry_t *e)
{
    if (!ctx || !ctx->icon_ctx) return;

    /* Check synchronous cache first — avoids allocating a callback struct
     * for the common case (same process seen again after first scan). */
    GdkPixbuf *cached = proc_icon_get_cached(ctx->icon_ctx, e->name);
    if (cached) {
        gtk_tree_store_set(store, iter, COL_ICON, cached, -1);
        return;
    }

    /* Async path — allocate callback data */
    icon_cb_data_t *d = g_new(icon_cb_data_t, 1);
    d->store = store;
    snprintf(d->key, sizeof(d->key), "%s", e->name);

    proc_icon_lookup_async(ctx->icon_ctx, e->name,
                           e->steam,
                           on_icon_resolved, d);
}

/*
 * Incremental update: diff the store's snapshot against the existing tree.
 *   1. Remove PROC_KILLED rows and update PROC_ALIVE/PROC_DYING rows
 *      in-place (bottom-up, via update_or_remove_rows).
 *   2. Insert new PROC_ALIVE processes (not yet in the tree) under the
 *      correct parent.
 *
 * Lifecycle tracking (DYING / KILLED) is owned by proc_store_t; tree.c
 * just translates record status into GTK tree operations.
 */
void update_store(GtkTreeStore       *store,
                  GtkTreeView        *view,
                  const proc_store_t *pstore,
                  ui_ctx_t           *ctx)
{
    static int first_update = 1;

    /* ── Phase 1 & 2: remove dead rows and update surviving rows ── */
    update_or_remove_rows(store, NULL, pstore);

    /* ── Phase 3: collect existing rows after removals ───────── */
    iter_map_t existing = { NULL, 0, 0 };
    collect_iters(GTK_TREE_MODEL(store), NULL, &existing);

    /* Build a quick "already has a GTK row?" lookup */
    store_ht_entry_t *old_ht = calloc(STORE_HT_SIZE, sizeof(store_ht_entry_t));
    if (!old_ht) { free(existing.entries); return; }
    for (size_t i = 0; i < existing.count; i++)
        ht_insert(old_ht, existing.entries[i].pid, i);

    /* ── Phase 4: insert new ALIVE processes ─────────────────── */
    /*
     * Gather the ALIVE records that have no GTK row yet into a
     * temporary flat list, then use the ancestor-stack approach to
     * insert parents before children.
     */
    size_t new_count = 0;
    for (size_t i = 0; i < pstore->count; i++) {
        const proc_record_t *rec = &pstore->records[i];
        if (rec->status == PROC_ALIVE &&
            ht_find(old_ht, rec->entry.pid) == (size_t)-1)
            new_count++;
    }

    if (new_count == 0) {
        free(old_ht);
        free(existing.entries);
        first_update = 0;
        return;
    }

    /* Build an index array of new records */
    size_t *new_idx = calloc(new_count, sizeof(size_t));
    if (!new_idx) { free(old_ht); free(existing.entries); return; }
    {
        size_t j = 0;
        for (size_t i = 0; i < pstore->count; i++) {
            const proc_record_t *rec = &pstore->records[i];
            if (rec->status == PROC_ALIVE &&
                ht_find(old_ht, rec->entry.pid) == (size_t)-1)
                new_idx[j++] = i;
        }
    }

    int *inserted = calloc(new_count, sizeof(int));
    if (!inserted) { free(old_ht); free(existing.entries); free(new_idx); return; }

    /* Local PID→new_idx[] lookup for ancestor-stack resolution */
    store_ht_entry_t *new_ht = calloc(STORE_HT_SIZE, sizeof(store_ht_entry_t));
    if (!new_ht) { free(old_ht); free(existing.entries); free(new_idx); free(inserted); return; }
    for (size_t j = 0; j < new_count; j++)
        ht_insert(new_ht, pstore->records[new_idx[j]].entry.pid, j);

    pid_t stack[64];
    int sp;

    for (size_t j = 0; j < new_count; j++) {
        if (inserted[j]) continue;

        /* Build ancestor stack */
        sp = 0;
        size_t cur = j;
        while (!inserted[cur]) {
            if (sp >= 64) break;
            stack[sp++] = pstore->records[new_idx[cur]].entry.pid;

            pid_t pp = pstore->records[new_idx[cur]].entry.ppid;
            /* Check if parent is also a new entry */
            size_t pidx = ht_find(new_ht, pp);
            if (pidx == (size_t)-1 || pidx == cur) break;
            if (inserted[pidx]) break;
            cur = pidx;
        }

        /* Pop stack: insert outermost ancestor first */
        while (sp > 0) {
            pid_t p = stack[--sp];
            size_t jj = ht_find(new_ht, p);
            if (jj == (size_t)-1 || inserted[jj]) continue;

            const proc_entry_t *e = &pstore->records[new_idx[jj]].entry;

            GtkTreeIter *parent_iter = find_parent_iter(&existing,
                                                         e->ppid, e->pid);

            GtkTreeIter new_iter;
            gtk_tree_store_append(store, &new_iter, parent_iter);
            set_row_data(store, &new_iter, e);
            request_icon(ctx, store, &new_iter, e);

            /* Stamp newly inserted process with a birth highlight */
            if (!first_update)
                gtk_tree_store_set(store, &new_iter,
                                   COL_HIGHLIGHT_BORN, (gint64)
                                   pstore->records[new_idx[jj]].died_at_us
                                   /* died_at_us == 0 for ALIVE; use
                                    * a live timestamp for born */,
                                   -1);
            /* Overwrite with a real timestamp for birth highlight */
            if (!first_update) {
                struct timespec _ts;
                clock_gettime(CLOCK_MONOTONIC, &_ts);
                gint64 now_us = (gint64)_ts.tv_sec * 1000000LL
                              + _ts.tv_nsec / 1000;
                gtk_tree_store_set(store, &new_iter,
                                   COL_HIGHLIGHT_BORN, now_us, -1);
            }

            /* Expand PID 1 and PID 2 the moment their first child arrives */
            if ((e->ppid == 1 || e->ppid == 2) && parent_iter) {
                GtkTreeModel *sort = GTK_TREE_MODEL(
                    gtk_tree_view_get_model(view));
                GtkTreePath *cp = gtk_tree_model_get_path(
                    GTK_TREE_MODEL(store), parent_iter);
                if (cp && GTK_IS_TREE_MODEL_SORT(sort)) {
                    GtkTreePath *sp2 =
                        gtk_tree_model_sort_convert_child_path_to_path(
                            GTK_TREE_MODEL_SORT(sort), cp);
                    if (sp2) {
                        gtk_tree_view_expand_row(view, sp2, TRUE);
                        gtk_tree_path_free(sp2);
                    }
                    gtk_tree_path_free(cp);
                }
            }

            /* Add to existing map so children can find us */
            iter_map_add(&existing, e->pid, &new_iter);
            inserted[jj] = 1;
        }
    }

    free(new_ht);
    free(old_ht);
    free(existing.entries);
    free(new_idx);
    free(inserted);
    first_update = 0;
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
 * Uses the same ancestor-stack insertion as before, driven from the
 * proc_store_t instead of a raw proc_entry_t array.
 */
void populate_store_initial(GtkTreeStore       *store,
                            GtkTreeView        *view,
                            const proc_store_t *pstore,
                            pid_t               preselect_pid,
                            ui_ctx_t           *ctx)
{
    if (pstore->count == 0) return;

    /* Build a PID → records[] index hash for parent lookups */
    store_ht_entry_t *ht = calloc(STORE_HT_SIZE, sizeof(store_ht_entry_t));
    if (!ht) return;
    for (size_t i = 0; i < pstore->count; i++) {
        if (pstore->records[i].status != PROC_KILLED)
            ht_insert(ht, pstore->records[i].entry.pid, i);
    }

    size_t count = pstore->count;

    int          *inserted = calloc(count, sizeof(int));
    GtkTreeIter  *iters    = calloc(count, sizeof(GtkTreeIter));
    if (!inserted || !iters) {
        free(ht); free(inserted); free(iters);
        return;
    }

    pid_t stack[64];
    int sp;
    gboolean preselected = FALSE;

    for (size_t i = 0; i < count; i++) {
        if (inserted[i]) continue;
        if (pstore->records[i].status == PROC_KILLED) { inserted[i] = 1; continue; }

        sp = 0;
        size_t cur = i;
        while (!inserted[cur]) {
            if (pstore->records[cur].status == PROC_KILLED) break;
            if (sp >= 64) break;
            stack[sp++] = pstore->records[cur].entry.pid;
            pid_t pp = pstore->records[cur].entry.ppid;
            size_t pidx = ht_find(ht, pp);
            if (pidx == (size_t)-1 || pidx == cur) break;
            if (inserted[pidx]) break;
            cur = pidx;
        }

        while (sp > 0) {
            pid_t p = stack[--sp];
            size_t sidx = ht_find(ht, p);
            if (sidx == (size_t)-1 || inserted[sidx]) continue;

            const proc_entry_t *e = &pstore->records[sidx].entry;
            GtkTreeIter *parent_iter = NULL;
            size_t pidx = ht_find(ht, e->ppid);
            if (pidx != (size_t)-1 && inserted[pidx] && pidx != sidx)
                parent_iter = &iters[pidx];

            gtk_tree_store_append(store, &iters[sidx], parent_iter);
            set_row_data(store, &iters[sidx], e);
            request_icon(ctx, store, &iters[sidx], e);
            inserted[sidx] = 1;

            /* As soon as a direct child of PID 1 or 2 is inserted,
             * expand that parent so it is open immediately. */
            if ((e->ppid == 1 || e->ppid == 2) && parent_iter) {
                GtkTreeModel *sort = gtk_tree_view_get_model(view);
                GtkTreePath *cp = gtk_tree_model_get_path(
                    GTK_TREE_MODEL(store), parent_iter);
                if (cp && GTK_IS_TREE_MODEL_SORT(sort)) {
                    GtkTreePath *sp2 =
                        gtk_tree_model_sort_convert_child_path_to_path(
                            GTK_TREE_MODEL_SORT(sort), cp);
                    if (sp2) {
                        gtk_tree_view_expand_row(view, sp2, TRUE);
                        gtk_tree_path_free(sp2);
                    }
                    gtk_tree_path_free(cp);
                }
            }

            /* Select and open the detail panel the instant the
             * preselected row lands in the store. */
            if (!preselected && preselect_pid > 0 &&
                e->pid == preselect_pid && ctx) {
                preselected = TRUE;
                GtkTreeModel *sort = gtk_tree_view_get_model(view);
                GtkTreePath *child_path = gtk_tree_model_get_path(
                    GTK_TREE_MODEL(store), &iters[sidx]);
                if (child_path && GTK_IS_TREE_MODEL_SORT(sort)) {
                    GtkTreePath *sort_path =
                        gtk_tree_model_sort_convert_child_path_to_path(
                            GTK_TREE_MODEL_SORT(sort), child_path);
                    if (sort_path) {
                        GtkTreeSelection *sel =
                            gtk_tree_view_get_selection(view);
                        gtk_tree_selection_select_path(sel, sort_path);
                        gtk_tree_path_free(sort_path);
                    }
                    gtk_tree_path_free(child_path);
                }
                if (ctx->detail_vbox && gtk_widget_get_realized(ctx->detail_vbox)) {
                    if (gtk_widget_get_visible(ctx->detail_vbox))
                        gtk_widget_show_all(ctx->detail_panel);
                    proc_detail_update(ctx);
                }
            }
        }
    }

    free(ht);
    free(inserted);
    free(iters);
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
    gchar *spark_data = NULL;
    gint spark_peak;
    GdkPixbuf *icon = NULL;

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
        COL_IO_SPARKLINE, &spark_data,
        COL_IO_SPARKLINE_PEAK, &spark_peak,
        COL_HIGHLIGHT_BORN, &hl_born, COL_HIGHLIGHT_DIED, &hl_died,
        COL_ICON, &icon,
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
        COL_IO_SPARKLINE, spark_data,
        COL_IO_SPARKLINE_PEAK, spark_peak,
        COL_HIGHLIGHT_BORN, hl_born, COL_HIGHLIGHT_DIED, hl_died,
        COL_PINNED_ROOT, pinned_root,
        COL_ICON, icon,
        -1);

    if (icon) g_object_unref(icon);   /* gtk_tree_model_get reffed it */
    g_free(user); g_free(name); g_free(cpu_text); g_free(rss_text);
    g_free(grp_rss_text); g_free(grp_cpu_text);
    g_free(io_read_text); g_free(io_write_text); g_free(start_text);
    g_free(container); g_free(service); g_free(cwd); g_free(cmdline);
    g_free(steam_label); g_free(spark_data);

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
