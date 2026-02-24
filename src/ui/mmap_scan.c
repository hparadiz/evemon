/*
 * mmap_scan.c – on-demand /proc/<pid>/maps reading and display.
 *
 * Reads the memory map for the selected process, classifies regions
 * into logical categories, and populates a GtkTreeStore in the
 * sidebar.  The read runs in a GTask worker thread so it never
 * blocks the UI, matching the fd_scan / env_scan pattern.
 */

#include "ui_internal.h"

#include <unistd.h>

/* ── category labels ─────────────────────────────────────────── */

const char *mmap_cat_label[MMAP_CAT_COUNT] = {
    [MMAP_CAT_CODE]    = "Code (r-x)",
    [MMAP_CAT_DATA]    = "Data (rw-)",
    [MMAP_CAT_RODATA]  = "Read-only",
    [MMAP_CAT_HEAP]    = "Heap",
    [MMAP_CAT_STACK]   = "Stack",
    [MMAP_CAT_VDSO]    = "vDSO / Kernel",
    [MMAP_CAT_ANON]    = "Anonymous",
    [MMAP_CAT_OTHER]   = "Other",
};

/* ── classification ──────────────────────────────────────────── */

static mmap_category_t classify_region(const char *perms,
                                       const char *pathname)
{
    /* Named kernel regions */
    if (pathname[0] == '[') {
        if (strcmp(pathname, "[heap]") == 0)
            return MMAP_CAT_HEAP;
        if (strncmp(pathname, "[stack", 6) == 0)   /* [stack] or [stack:tid] */
            return MMAP_CAT_STACK;
        if (strcmp(pathname, "[vdso]") == 0 ||
            strcmp(pathname, "[vvar]") == 0 ||
            strcmp(pathname, "[vsyscall]") == 0)
            return MMAP_CAT_VDSO;
        return MMAP_CAT_OTHER;
    }

    /* Executable mapping (code) */
    if (perms[2] == 'x')
        return MMAP_CAT_CODE;

    /* Anonymous: no pathname and not a kernel-special region */
    if (pathname[0] == '\0')
        return MMAP_CAT_ANON;

    /* File-backed data (read-write) */
    if (perms[1] == 'w')
        return MMAP_CAT_DATA;

    /* File-backed read-only (e.g. .rodata sections, fonts, locale) */
    if (perms[0] == 'r')
        return MMAP_CAT_RODATA;

    return MMAP_CAT_OTHER;
}

/* ── map entry ───────────────────────────────────────────────── */

typedef struct {
    char  text[768];          /* "addr-addr perms  size  pathname" */
    mmap_category_t cat;
    size_t size_kb;           /* region size in KiB */
} mmap_entry_t;

typedef struct {
    mmap_entry_t *entries;
    size_t        count;
    size_t        capacity;
} mmap_list_t;

static void mmap_list_init(mmap_list_t *l)
{
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void mmap_list_free(mmap_list_t *l)
{
    free(l->entries);
    l->entries  = NULL;
    l->count    = 0;
    l->capacity = 0;
}

static void mmap_list_push(mmap_list_t *l, const char *text,
                           mmap_category_t cat, size_t size_kb)
{
    if (l->count >= l->capacity) {
        size_t newcap = l->capacity ? l->capacity * 2 : 128;
        mmap_entry_t *tmp = realloc(l->entries, newcap * sizeof(mmap_entry_t));
        if (!tmp) return;
        l->entries  = tmp;
        l->capacity = newcap;
    }
    snprintf(l->entries[l->count].text,
             sizeof(l->entries[0].text), "%s", text);
    l->entries[l->count].cat     = cat;
    l->entries[l->count].size_kb = size_kb;
    l->count++;
}

/* Sort by category first, then by address string within category. */
static int mmap_entry_cmp(const void *a, const void *b)
{
    const mmap_entry_t *ea = a;
    const mmap_entry_t *eb = b;
    if (ea->cat != eb->cat)
        return (int)ea->cat - (int)eb->cat;
    return strcmp(ea->text, eb->text);
}

/* ── /proc/<pid>/maps reader ─────────────────────────────────── */

static void format_size(size_t kb, char *buf, size_t bufsz)
{
    if (kb >= 1048576)
        snprintf(buf, bufsz, "%.1f GiB", (double)kb / 1048576.0);
    else if (kb >= 1024)
        snprintf(buf, bufsz, "%.1f MiB", (double)kb / 1024.0);
    else
        snprintf(buf, bufsz, "%zu KiB", kb);
}

static void read_maps(pid_t pid, mmap_list_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        /* Parse: addr_start-addr_end perms offset dev inode [pathname] */
        unsigned long addr_start = 0, addr_end = 0;
        char perms[8] = "----";
        unsigned long offset = 0;
        char dev[16] = "";
        unsigned long inode = 0;
        char pathname[512] = "";

        /* sscanf will handle the fixed fields; pathname may contain
         * spaces (rare, but possible for some FUSE mounts). */
        int n = sscanf(line, "%lx-%lx %4s %lx %15s %lu",
                       &addr_start, &addr_end, perms, &offset, dev, &inode);
        if (n < 5) continue;

        /* Extract pathname: skip the first 5 whitespace-delimited
         * tokens (addr-range, perms, offset, dev, inode) and then the
         * inode value itself, leaving p at the pathname. */
        {
            const char *p = line;
            int fields = 0;
            /* Skip tokens 1..5: addr-range, perms, offset, dev */
            while (fields < 4 && *p) {
                while (*p && *p != ' ' && *p != '\t') p++;
                while (*p == ' ' || *p == '\t') p++;
                fields++;
            }
            /* p now points at the inode field */
            while (*p && *p != ' ' && *p != '\t') p++;  /* skip inode */
            while (*p == ' ' || *p == '\t') p++;
            if (*p)
                snprintf(pathname, sizeof(pathname), "%s", p);
        }

        size_t size_kb = (addr_end - addr_start) / 1024;
        mmap_category_t cat = classify_region(perms, pathname);

        /* Build display line: "addr range  perms  size  pathname" */
        char sz_buf[32];
        format_size(size_kb, sz_buf, sizeof(sz_buf));

        char display[768];
        if (pathname[0])
            snprintf(display, sizeof(display), "%lx-%lx  %s  %s  %s",
                     addr_start, addr_end, perms, sz_buf, pathname);
        else
            snprintf(display, sizeof(display), "%lx-%lx  %s  %s",
                     addr_start, addr_end, perms, sz_buf);

        mmap_list_push(out, display, cat, size_kb);
    }
    fclose(f);
}

/* ── Pango markup for mmap entries ───────────────────────────── */

/*
 * Render the display line with the permissions and size highlighted.
 * Format: "addr-addr  <b>perms</b>  <color>size</color>  pathname"
 *
 * We parse the display text to find the perms and size fields.
 */
static char *mmap_to_markup(const char *text)
{
    /* The display format is: "addr-addr  perms  size  [pathname]"
     * We separate at double-space boundaries. */
    const char *p = text;

    /* addr range: skip to first double-space */
    const char *addr_end = strstr(p, "  ");
    if (!addr_end) {
        return g_markup_escape_text(text, -1);
    }

    char *addr_esc = g_markup_escape_text(p, (gssize)(addr_end - p));

    /* skip spaces to perms */
    const char *perms_start = addr_end;
    while (*perms_start == ' ') perms_start++;
    const char *perms_end = strstr(perms_start, "  ");
    if (!perms_end) {
        char *result = g_markup_escape_text(text, -1);
        g_free(addr_esc);
        return result;
    }

    char *perms_esc = g_markup_escape_text(perms_start,
                                           (gssize)(perms_end - perms_start));

    /* skip spaces to size */
    const char *size_start = perms_end;
    while (*size_start == ' ') size_start++;
    const char *size_end = strstr(size_start, "  ");

    char *size_esc, *path_esc;
    if (size_end) {
        size_esc = g_markup_escape_text(size_start,
                                        (gssize)(size_end - size_start));
        const char *path_start = size_end;
        while (*path_start == ' ') path_start++;
        path_esc = g_markup_escape_text(path_start, -1);
    } else {
        /* No pathname — the rest is the size */
        size_esc = g_markup_escape_text(size_start, -1);
        path_esc = g_strdup("");
    }

    char *markup;
    if (path_esc[0])
        markup = g_strdup_printf(
            "<span foreground=\"#888888\">%s</span>  "
            "<b>%s</b>  "
            "<span foreground=\"#6699cc\">%s</span>  "
            "%s",
            addr_esc, perms_esc, size_esc, path_esc);
    else
        markup = g_strdup_printf(
            "<span foreground=\"#888888\">%s</span>  "
            "<b>%s</b>  "
            "<span foreground=\"#6699cc\">%s</span>",
            addr_esc, perms_esc, size_esc);

    g_free(addr_esc);
    g_free(perms_esc);
    g_free(size_esc);
    g_free(path_esc);
    return markup;
}

/* ── async GTask ─────────────────────────────────────────────── */

typedef struct {
    pid_t          pid;
    guint          generation;
    mmap_list_t    buckets[MMAP_CAT_COUNT];
    size_t         cat_total_kb[MMAP_CAT_COUNT];
    ui_ctx_t      *ctx;
} mmap_scan_task_t;

static void mmap_scan_task_free(mmap_scan_task_t *t)
{
    if (!t) return;
    for (int c = 0; c < MMAP_CAT_COUNT; c++)
        mmap_list_free(&t->buckets[c]);
    free(t);
}

static void mmap_scan_thread_func(GTask        *task,
                                  gpointer      source_object,
                                  gpointer      task_data,
                                  GCancellable *cancellable)
{
    (void)source_object;
    mmap_scan_task_t *t = task_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    mmap_list_t all;
    mmap_list_init(&all);
    read_maps(t->pid, &all);

    if (g_cancellable_is_cancelled(cancellable)) {
        mmap_list_free(&all);
        return;
    }

    /* Sort and split into per-category buckets */
    if (all.count > 1)
        qsort(all.entries, all.count, sizeof(mmap_entry_t), mmap_entry_cmp);

    for (int c = 0; c < MMAP_CAT_COUNT; c++) {
        mmap_list_init(&t->buckets[c]);
        t->cat_total_kb[c] = 0;
    }

    for (size_t i = 0; i < all.count; i++) {
        mmap_category_t cat = all.entries[i].cat;
        mmap_list_push(&t->buckets[cat], all.entries[i].text,
                       cat, all.entries[i].size_kb);
        t->cat_total_kb[cat] += all.entries[i].size_kb;
    }

    mmap_list_free(&all);

    g_task_return_boolean(task, TRUE);
}

static void mmap_scan_complete(GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
    (void)source_object;
    ui_ctx_t *ctx = user_data;

    GTask *task = G_TASK(result);
    mmap_scan_task_t *t = g_task_get_task_data(task);

    if (!t || t->generation != ctx->mmap_generation)
        return;

    if (g_task_had_error(task))
        return;

    /* Save scroll position */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(gtk_widget_get_parent(
            GTK_WIDGET(ctx->mmap_view))));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->mmap_store);

    /* Index existing category rows */
    GtkTreeIter cat_iters[MMAP_CAT_COUNT];
    gboolean    cat_exists[MMAP_CAT_COUNT];
    memset(cat_exists, 0, sizeof(cat_exists));

    {
        GtkTreeIter top;
        gboolean valid = gtk_tree_model_iter_children(model, &top, NULL);
        while (valid) {
            gint cat_id = -1;
            gtk_tree_model_get(model, &top, MMAP_COL_CAT, &cat_id, -1);
            if (cat_id >= 0 && cat_id < MMAP_CAT_COUNT) {
                cat_iters[cat_id]  = top;
                cat_exists[cat_id] = TRUE;
            }
            valid = gtk_tree_model_iter_next(model, &top);
        }
    }

    /* Remove empty categories */
    for (int c = 0; c < MMAP_CAT_COUNT; c++) {
        if (cat_exists[c] && t->buckets[c].count == 0) {
            gtk_tree_store_remove(ctx->mmap_store, &cat_iters[c]);
            cat_exists[c] = FALSE;
        }
    }

    /* Populate / update each category */
    for (int c = 0; c < MMAP_CAT_COUNT; c++) {
        if (t->buckets[c].count == 0) continue;

        char sz_buf[32];
        format_size(t->cat_total_kb[c], sz_buf, sizeof(sz_buf));

        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s — %zu regions, %s",
                 mmap_cat_label[c], t->buckets[c].count, sz_buf);

        GtkTreeIter parent;
        if (!cat_exists[c]) {
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_append(ctx->mmap_store, &parent, NULL);
            gtk_tree_store_set(ctx->mmap_store, &parent,
                               MMAP_COL_TEXT, hdr,
                               MMAP_COL_MARKUP, hdr_escaped,
                               MMAP_COL_CAT, (gint)c, -1);
            g_free(hdr_escaped);
            cat_exists[c] = TRUE;
            cat_iters[c]  = parent;
        } else {
            parent = cat_iters[c];
            char *hdr_escaped = g_markup_escape_text(hdr, -1);
            gtk_tree_store_set(ctx->mmap_store, &parent,
                               MMAP_COL_TEXT, hdr,
                               MMAP_COL_MARKUP, hdr_escaped, -1);
            g_free(hdr_escaped);
        }

        /* Update child rows in place, adding/removing as needed */
        GtkTreeIter child;
        gboolean child_valid = gtk_tree_model_iter_children(
            model, &child, &parent);
        size_t bi = 0;

        while (bi < t->buckets[c].count && child_valid) {
            char *markup = mmap_to_markup(t->buckets[c].entries[bi].text);
            gtk_tree_store_set(ctx->mmap_store, &child,
                               MMAP_COL_TEXT, t->buckets[c].entries[bi].text,
                               MMAP_COL_MARKUP, markup,
                               MMAP_COL_CAT, (gint)-1, -1);
            g_free(markup);
            bi++;
            child_valid = gtk_tree_model_iter_next(model, &child);
        }

        /* Append new rows */
        while (bi < t->buckets[c].count) {
            GtkTreeIter new_child;
            char *markup = mmap_to_markup(t->buckets[c].entries[bi].text);
            gtk_tree_store_append(ctx->mmap_store, &new_child, &parent);
            gtk_tree_store_set(ctx->mmap_store, &new_child,
                               MMAP_COL_TEXT, t->buckets[c].entries[bi].text,
                               MMAP_COL_MARKUP, markup,
                               MMAP_COL_CAT, (gint)-1, -1);
            g_free(markup);
            bi++;
        }

        /* Remove excess rows */
        while (child_valid) {
            child_valid = gtk_tree_store_remove(ctx->mmap_store, &child);
        }

        /* Restore expand/collapse state */
        GtkTreePath *cat_path = gtk_tree_model_get_path(
            model, &cat_iters[c]);
        if (ctx->mmap_collapsed & (1u << c))
            gtk_tree_view_collapse_row(ctx->mmap_view, cat_path);
        else
            gtk_tree_view_expand_row(ctx->mmap_view, cat_path, FALSE);
        gtk_tree_path_free(cat_path);
    }

    gtk_adjustment_set_value(vadj, scroll_pos);
}

/* ── public API ──────────────────────────────────────────────── */

void mmap_scan_start(ui_ctx_t *ctx, pid_t pid)
{
    /* Cancel any in-flight scan */
    if (ctx->mmap_cancel) {
        g_cancellable_cancel(ctx->mmap_cancel);
        g_object_unref(ctx->mmap_cancel);
    }
    ctx->mmap_cancel = g_cancellable_new();
    ctx->mmap_generation++;

    /* Reset collapse state when switching to a different process */
    if (pid != ctx->mmap_last_pid) {
        ctx->mmap_collapsed = 0;
        ctx->mmap_last_pid  = pid;
    }

    mmap_scan_task_t *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->pid        = pid;
    t->generation = ctx->mmap_generation;
    t->ctx        = ctx;

    GTask *task = g_task_new(NULL, ctx->mmap_cancel, mmap_scan_complete, ctx);
    g_task_set_task_data(task, t, (GDestroyNotify)mmap_scan_task_free);
    g_task_run_in_thread(task, mmap_scan_thread_func);
    g_object_unref(task);
}

/* ── signal callbacks ────────────────────────────────────────── */

void on_mmap_row_collapsed(GtkTreeView *view, GtkTreeIter *iter,
                           GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->mmap_store), iter,
                       MMAP_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < MMAP_CAT_COUNT)
        ctx->mmap_collapsed |= (1u << cat_id);
}

void on_mmap_row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, gpointer data)
{
    (void)view; (void)path;
    ui_ctx_t *ctx = data;
    gint cat_id = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->mmap_store), iter,
                       MMAP_COL_CAT, &cat_id, -1);
    if (cat_id >= 0 && cat_id < MMAP_CAT_COUNT)
        ctx->mmap_collapsed &= ~(1u << cat_id);
}

gboolean on_mmap_key_press(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)data;

    if (ev->keyval != GDK_KEY_Return && ev->keyval != GDK_KEY_KP_Enter)
        return FALSE;

    GtkTreeView *view = GTK_TREE_VIEW(widget);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return FALSE;

    gint cat_id = -1;
    gtk_tree_model_get(model, &iter, MMAP_COL_CAT, &cat_id, -1);
    if (cat_id < 0)
        return FALSE;

    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    if (!path)
        return FALSE;

    if (gtk_tree_view_row_expanded(view, path))
        gtk_tree_view_collapse_row(view, path);
    else
        gtk_tree_view_expand_row(view, path, FALSE);

    gtk_tree_path_free(path);
    return TRUE;
}
