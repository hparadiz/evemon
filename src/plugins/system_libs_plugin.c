/*
 * system_libs_plugin.c – Shared Libraries plugin for the System Panel.
 *
 * Shows all shared libraries loaded across PID 1 and its descendants,
 * in a flat two-column list (Type / Name).  Selecting a row shows every
 * process that has the library mapped in the right-hand detail pane.
 */

#include "../evemon_plugin.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.system_libs",
    "System Libraries",
    "1.0",
    EVEMON_ROLE_SYSTEM,
    NULL
);

/* ── category definitions ────────────────────────────────────── */

enum {
    LIB_CAT_RUNTIME,
    LIB_CAT_SYSTEM,
    LIB_CAT_APPLICATION,
    LIB_CAT_WINE_BUILTIN,
    LIB_CAT_WINDOWS_DLL,
    LIB_CAT_OTHER,
    LIB_CAT_COUNT
};

static const char *cat_labels[LIB_CAT_COUNT] = {
    [LIB_CAT_RUNTIME]      = "Runtime",
    [LIB_CAT_SYSTEM]       = "System",
    [LIB_CAT_APPLICATION]  = "Application",
    [LIB_CAT_WINE_BUILTIN] = "Wine/Proton",
    [LIB_CAT_WINDOWS_DLL]  = "Windows DLL",
    [LIB_CAT_OTHER]        = "Other",
};

/* ── column enums ────────────────────────────────────────────── */

enum {
    COL_TYPE_MARKUP,
    COL_NAME_MARKUP,
    COL_VERSION_MARKUP,
    COL_SIZE_MARKUP,
    COL_PROCS_MARKUP,
    COL_PATH,           /* raw path for detail lookup */
    NUM_COLS
};

enum {
    DCOL_PROCESS,    /* process name markup        */
    DCOL_PID,        /* pid markup                 */
    DCOL_ADDR,       /* address range markup       */
    DCOL_PERMS,      /* perms markup               */
    DCOL_SIZE,       /* segment size markup        */
    DCOL_NUM_COLS
};

/* ── per-lib snapshot entry (with owning pid) ────────────────── */

typedef struct {
    char   path[512];
    char   name[256];
    char   version[64];
    char   origin[128];
    int    cat;
    size_t size_kb;
    pid_t  source_pid;
} slib_entry_t;

/* ── pre-built display row (produced off-thread) ─────────────── */

typedef struct {
    char  type_markup[64];
    char  name_markup[512];
    char  version_markup[80];
    char  size_markup[48];
    char  procs_markup[32];
    char  path[512];
} slib_row_t;

/* ── per-instance state ──────────────────────────────────────── */

typedef struct {
    GtkWidget      *vbox;
    GtkWidget      *scroll;
    GtkListStore   *store;
    GtkTreeView    *view;
    GtkWidget      *chk_desc;
    GtkWidget      *chk_dedup;
    GtkWidget      *search_entry;
    char            filter_text[256];
    pid_t           last_pid;
    /* detail panel */
    GtkWidget      *detail_scroll;
    GtkListStore   *detail_store;
    GtkTreeView    *detail_view;
    char            selected_path[512];
    /* snapshot for detail lookup */
    slib_entry_t   *snap;
    size_t          snap_count;
    /* background worker coordination */
    guint           generation;
} slib_ctx_t;

/* ── worker job / result ─────────────────────────────────────── */

typedef struct {
    pid_t       *pids;
    size_t       pid_count;
    char         filter[256];
    slib_ctx_t  *ctx;
    guint        generation;
} slib_job_t;

typedef struct {
    slib_row_t   *rows;
    size_t        row_count;
    slib_entry_t *snap;
    size_t        snap_count;
    slib_ctx_t   *ctx;
    guint         generation;
} slib_result_t;

/* ── UTF-8 helpers ───────────────────────────────────────────── */

static char *utf8_sanitize(const char *raw)
{
    if (!raw || !raw[0]) return g_strdup("");
    if (g_utf8_validate(raw, -1, NULL)) return g_strdup(raw);
    return g_utf8_make_valid(raw, -1);
}

/* ── lib helpers (mirrors plugin_broker.c) ───────────────────── */

static void sl_extract_version(const char *fn, char *ver, size_t vsz)
{
    ver[0] = '\0';
    const char *p = strstr(fn, ".so.");
    if (p) snprintf(ver, vsz, "%s", p + 4);
}

static void sl_extract_origin(const char *fp, char *out, size_t osz)
{
    out[0] = '\0';
    const char *p;
    if ((p = strstr(fp, "/dist/lib")) || (p = strstr(fp, "/files/lib"))) {
        const char *c = strstr(fp, "/common/");
        if (c) {
            c += 8;
            const char *s = strchr(c, '/');
            if (s) {
                size_t n = (size_t)(s - c);
                if (n >= osz) n = osz - 1;
                memcpy(out, c, n); out[n] = '\0'; return;
            }
        }
        snprintf(out, osz, "Proton"); return;
    }
    if (strstr(fp, "/wine/") || strstr(fp, "/Wine/")) {
        snprintf(out, osz, "Wine"); return;
    }
    if (strstr(fp, "steam-runtime") || strstr(fp, "SteamLinuxRuntime") ||
        strstr(fp, "pressure-vessel")) {
        if      (strstr(fp, "sniper"))  snprintf(out, osz, "Runtime (sniper)");
        else if (strstr(fp, "soldier")) snprintf(out, osz, "Runtime (soldier)");
        else if (strstr(fp, "scout"))   snprintf(out, osz, "Runtime (scout)");
        else                            snprintf(out, osz, "Steam Runtime");
        return;
    }
    if (strstr(fp, "/steamapps/common/")) {
        const char *c = strstr(fp, "/common/");
        if (c) {
            c += 8;
            const char *s = strchr(c, '/');
            if (s) {
                size_t n = (size_t)(s - c);
                if (n >= osz) n = osz - 1;
                memcpy(out, c, n); out[n] = '\0'; return;
            }
        }
    }
    if (strstr(fp, "/compatdata/")) { snprintf(out, osz, "Wine Prefix"); return; }
}

static int sl_is_runtime(const char *name)
{
    static const char *rt[] = {
        "ld-linux", "ld-musl", "libc.so", "libc-",
        "libm.so", "libm-", "libdl.so", "libdl-",
        "libpthread.so", "libpthread-", "librt.so", "librt-",
        "libresolv.so", "libnss_", "libgcc_s.so", "libstdc++.so",
        NULL
    };
    for (int i = 0; rt[i]; i++)
        if (strncmp(name, rt[i], strlen(rt[i])) == 0) return 1;
    return 0;
}

static int sl_classify(const char *path, const char *name)
{
    if (strstr(path, "x86_64-windows") || strstr(path, "i386-windows") ||
        strstr(path, "i686-windows"))
        return LIB_CAT_WINDOWS_DLL;

    size_t nlen = strlen(name);
    if (nlen >= 4) {
        const char *ext = name + nlen - 4;
        if (strcasecmp(ext, ".dll") == 0 || strcasecmp(ext, ".drv") == 0 ||
            strcasecmp(ext, ".exe") == 0) {
            if (strstr(path, "/wine/") || strstr(path, "/dist/") ||
                strstr(path, "/files/"))
                return LIB_CAT_WINE_BUILTIN;
            return LIB_CAT_WINDOWS_DLL;
        }
    }
    if (strstr(path, "x86_64-unix") || strstr(path, "i386-unix") ||
        strstr(path, "i686-unix"))
        return LIB_CAT_WINE_BUILTIN;
    if (strstr(path, "/wine/") && strstr(name, ".so"))
        return LIB_CAT_WINE_BUILTIN;
    if (sl_is_runtime(name))
        return LIB_CAT_RUNTIME;
    if (strncmp(path, "/usr/lib", 8) == 0 ||
        strncmp(path, "/usr/local/lib", 14) == 0 ||
        strncmp(path, "/lib/", 5) == 0 ||
        strncmp(path, "/lib64/", 7) == 0 ||
        strncmp(path, "/nix/store", 10) == 0 ||
        strstr(path, "steam-runtime") ||
        strstr(path, "SteamLinuxRuntime") ||
        strstr(path, "pressure-vessel"))
        return LIB_CAT_SYSTEM;
    return LIB_CAT_APPLICATION;
}

static void sl_format_size(size_t kb, char *buf, size_t bufsz)
{
    if (kb >= 1048576)
        snprintf(buf, bufsz, "%.1f GiB", (double)kb / 1048576.0);
    else if (kb >= 1024)
        snprintf(buf, bufsz, "%.1f MiB", (double)kb / 1024.0);
    else if (kb > 0)
        snprintf(buf, bufsz, "%zu KiB", kb);
    else
        buf[0] = '\0';
}

/* ── gather libs for one PID into a growable array ───────────── */

static void sl_gather_pid(pid_t pid,
                          slib_entry_t **buf, size_t *count, size_t *cap)
{
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);
    FILE *f = fopen(maps_path, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        unsigned long addr_s = 0, addr_e = 0;
        char perms[8] = "----";
        unsigned long dummy_off; char devstr[16]; unsigned long inode;
        if (sscanf(line, "%lx-%lx %4s %lx %15s %lu",
                   &addr_s, &addr_e, perms, &dummy_off, devstr, &inode) < 5)
            continue;
        if (perms[2] != 'x') continue;

        /* Find pathname */
        const char *p = line;
        for (int fld = 0; fld < 5 && *p; fld++) {
            while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' ' || *p == '\t') p++;
        }
        if (!*p || *p == '[') continue;

        const char *bname = strrchr(p, '/');
        bname = bname ? bname + 1 : p;

        int is_so  = strstr(bname, ".so") != NULL;
        int is_dll = 0;
        size_t blen = strlen(bname);
        if (blen >= 4) {
            const char *ext = bname + blen - 4;
            is_dll = strcasecmp(ext, ".dll") == 0 ||
                     strcasecmp(ext, ".drv") == 0 ||
                     strcasecmp(ext, ".exe") == 0 ||
                     strcasecmp(ext, ".sys") == 0 ||
                     strcasecmp(ext, ".ocx") == 0;
        }
        if (!is_so && !is_dll) continue;

        size_t seg_kb = (addr_e - addr_s) / 1024;

        /* Check for existing entry for this pid+path */
        int found = 0;
        for (size_t i = 0; i < *count; i++) {
            if ((*buf)[i].source_pid == pid &&
                strcmp((*buf)[i].path, p) == 0) {
                (*buf)[i].size_kb += seg_kb;
                found = 1;
                break;
            }
        }
        if (found) continue;

        if (*count >= *cap) {
            *cap *= 2;
            slib_entry_t *nb = realloc(*buf, *cap * sizeof(slib_entry_t));
            if (!nb) break;
            *buf = nb;
        }

        slib_entry_t *e = &(*buf)[*count];
        memset(e, 0, sizeof(*e));
        snprintf(e->path,    sizeof(e->path),    "%s", p);
        snprintf(e->name,    sizeof(e->name),    "%s", bname);
        e->size_kb   = seg_kb;
        e->source_pid = pid;
        e->cat        = sl_classify(p, bname);
        if (is_so) sl_extract_version(bname, e->version, sizeof(e->version));
        sl_extract_origin(p, e->origin, sizeof(e->origin));
        (*count)++;
    }
    fclose(f);
}

/* ── detail panel ────────────────────────────────────────────── */

/* Read all mapped segments for `lib_path` in `spid` and append rows */
static void append_detail_rows(slib_ctx_t *ctx, pid_t spid,
                               const char *lib_path)
{
    /* Process name */
    char comm[64] = "";
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "/proc/%d/comm", (int)spid);
    FILE *f = fopen(tmp, "r");
    if (f) {
        if (fgets(comm, sizeof(comm), f)) {
            size_t l = strlen(comm);
            if (l > 0 && comm[l-1] == '\n') comm[l-1] = '\0';
        }
        fclose(f);
    }
    if (!comm[0]) snprintf(comm, sizeof(comm), "%d", (int)spid);

    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)spid);
    f = fopen(maps_path, "r");
    if (!f) return;

    char *comm_esc = g_markup_escape_text(comm, -1);
    char *proc_markup = g_strdup_printf("<b>%s</b>", comm_esc);
    char *pid_markup  = g_strdup_printf(
        "<span foreground=\"#888888\">%d</span>", (int)spid);
    g_free(comm_esc);

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        /* Find pathname field (field 6) */
        const char *p = line;
        for (int fld = 0; fld < 5 && *p; fld++) {
            while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' '  || *p == '\t') p++;
        }
        if (!*p || strcmp(p, lib_path) != 0) continue;

        unsigned long addr_s = 0, addr_e = 0;
        char perms[8] = "----";
        sscanf(line, "%lx-%lx %4s", &addr_s, &addr_e, perms);

        size_t seg_kb = (addr_e - addr_s) / 1024;
        char sz_buf[32];
        sl_format_size(seg_kb, sz_buf, sizeof(sz_buf));

        char *addr_markup = g_strdup_printf(
            "<span foreground=\"#6699cc\"><tt>%08lx-%08lx</tt></span>",
            addr_s, addr_e);
        char *perms_esc   = g_markup_escape_text(perms, -1);
        char *size_markup = g_strdup_printf(
            "<span foreground=\"#888888\">%s</span>", sz_buf);

        GtkTreeIter it;
        gtk_list_store_append(ctx->detail_store, &it);
        gtk_list_store_set(ctx->detail_store, &it,
                           DCOL_PROCESS, proc_markup,
                           DCOL_PID,     pid_markup,
                           DCOL_ADDR,    addr_markup,
                           DCOL_PERMS,   perms_esc,
                           DCOL_SIZE,    size_markup, -1);
        g_free(addr_markup);
        g_free(perms_esc);
        g_free(size_markup);
    }
    fclose(f);
    g_free(proc_markup);
    g_free(pid_markup);
}

static void populate_detail(slib_ctx_t *ctx, const char *path)
{
    gtk_list_store_clear(ctx->detail_store);
    if (!path || !path[0]) return;

    for (size_t i = 0; i < ctx->snap_count; i++) {
        if (strcmp(ctx->snap[i].path, path) != 0) continue;
        append_detail_rows(ctx, ctx->snap[i].source_pid, path);
    }
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    slib_ctx_t *ctx = data;
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) {
        gtk_list_store_clear(ctx->detail_store);
        ctx->selected_path[0] = '\0';
        return;
    }

    gchar *path = NULL;
    gtk_tree_model_get(model, &iter, COL_PATH, &path, -1);
    if (path) {
        g_strlcpy(ctx->selected_path, path, sizeof(ctx->selected_path));
        g_free(path);
    }
    populate_detail(ctx, ctx->selected_path);
}

/* ── widget creation ─────────────────────────────────────────── */

static GtkWidget *slib_create_widget(void *opaque)
{
    slib_ctx_t *ctx = opaque;

    ctx->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* Hidden checkbox row (kept for toggle_button_get_active compatibility) */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 4);
    gtk_widget_set_margin_top(hbox, 2);
    gtk_widget_set_margin_bottom(hbox, 2);

    ctx->chk_desc = gtk_check_button_new_with_label("Include Descendants");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->chk_desc), TRUE);
    gtk_widget_set_no_show_all(ctx->chk_desc, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), ctx->chk_desc, FALSE, FALSE, 0);

    ctx->chk_dedup = gtk_check_button_new_with_label("Merge Duplicates");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->chk_dedup), TRUE);
    gtk_widget_set_no_show_all(ctx->chk_dedup, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), ctx->chk_dedup, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(ctx->vbox), hbox, FALSE, FALSE, 0);

    /* Search bar */
    ctx->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->search_entry), "Filter…");
    gtk_widget_set_margin_start(ctx->search_entry, 4);
    gtk_widget_set_margin_end(ctx->search_entry, 4);
    gtk_widget_set_margin_bottom(ctx->search_entry, 2);
    gtk_box_pack_start(GTK_BOX(ctx->vbox), ctx->search_entry, FALSE, FALSE, 0);

    /* Main list store */
    ctx->store = gtk_list_store_new(NUM_COLS,
                                    G_TYPE_STRING,   /* COL_TYPE_MARKUP    */
                                    G_TYPE_STRING,   /* COL_NAME_MARKUP    */
                                    G_TYPE_STRING,   /* COL_VERSION_MARKUP */
                                    G_TYPE_STRING,   /* COL_SIZE_MARKUP    */
                                    G_TYPE_STRING,   /* COL_PROCS_MARKUP   */
                                    G_TYPE_STRING);  /* COL_PATH           */

    ctx->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ctx->store)));
    g_object_unref(ctx->store);

    gtk_tree_view_set_headers_visible(ctx->view, TRUE);
    gtk_tree_view_set_enable_search(ctx->view, FALSE);

    /* Type column */
    GtkCellRenderer *type_cell = gtk_cell_renderer_text_new();
    g_object_set(type_cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *type_col = gtk_tree_view_column_new_with_attributes(
        "Type", type_cell, "markup", COL_TYPE_MARKUP, NULL);
    gtk_tree_view_column_set_resizable(type_col, TRUE);
    gtk_tree_view_column_set_min_width(type_col, 90);
    gtk_tree_view_append_column(ctx->view, type_col);

    /* Name column */
    GtkCellRenderer *name_cell = gtk_cell_renderer_text_new();
    g_object_set(name_cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes(
        "Name", name_cell, "markup", COL_NAME_MARKUP, NULL);
    gtk_tree_view_column_set_resizable(name_col, TRUE);
    gtk_tree_view_column_set_expand(name_col, TRUE);
    gtk_tree_view_append_column(ctx->view, name_col);

    /* Version column */
    GtkCellRenderer *ver_cell = gtk_cell_renderer_text_new();
    g_object_set(ver_cell, "ellipsize", PANGO_ELLIPSIZE_END,
                           "foreground", "#88aa88", NULL);
    GtkTreeViewColumn *ver_col = gtk_tree_view_column_new_with_attributes(
        "Version", ver_cell, "markup", COL_VERSION_MARKUP, NULL);
    gtk_tree_view_column_set_resizable(ver_col, TRUE);
    gtk_tree_view_column_set_min_width(ver_col, 70);
    gtk_tree_view_append_column(ctx->view, ver_col);

    /* Size column */
    GtkCellRenderer *size_cell = gtk_cell_renderer_text_new();
    g_object_set(size_cell, "xalign", 1.0f,
                            "foreground", "#6699cc", NULL);
    GtkTreeViewColumn *size_col = gtk_tree_view_column_new_with_attributes(
        "Size", size_cell, "markup", COL_SIZE_MARKUP, NULL);
    gtk_tree_view_column_set_resizable(size_col, TRUE);
    gtk_tree_view_column_set_min_width(size_col, 70);
    gtk_tree_view_append_column(ctx->view, size_col);

    /* Procs column */
    GtkCellRenderer *procs_cell = gtk_cell_renderer_text_new();
    g_object_set(procs_cell, "xalign", 1.0f,
                             "foreground", "#aaaacc", NULL);
    GtkTreeViewColumn *procs_col = gtk_tree_view_column_new_with_attributes(
        "Procs", procs_cell, "markup", COL_PROCS_MARKUP, NULL);
    gtk_tree_view_column_set_resizable(procs_col, TRUE);
    gtk_tree_view_column_set_min_width(procs_col, 50);
    gtk_tree_view_append_column(ctx->view, procs_col);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(ctx->view);
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), ctx);

    ctx->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->scroll), GTK_WIDGET(ctx->view));
    gtk_widget_set_vexpand(ctx->scroll, TRUE);

    /* Detail list (right pane) */
    ctx->detail_store = gtk_list_store_new(DCOL_NUM_COLS,
                                           G_TYPE_STRING,   /* DCOL_PROCESS */
                                           G_TYPE_STRING,   /* DCOL_PID     */
                                           G_TYPE_STRING,   /* DCOL_ADDR    */
                                           G_TYPE_STRING,   /* DCOL_PERMS   */
                                           G_TYPE_STRING);  /* DCOL_SIZE    */
    ctx->detail_view  = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->detail_store)));
    g_object_unref(ctx->detail_store);
    gtk_tree_view_set_headers_visible(ctx->detail_view, TRUE);
    gtk_tree_view_set_enable_search(ctx->detail_view, FALSE);

    /* Process column */
    GtkCellRenderer *dp_cell = gtk_cell_renderer_text_new();
    g_object_set(dp_cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *dp_col = gtk_tree_view_column_new_with_attributes(
        "Process", dp_cell, "markup", DCOL_PROCESS, NULL);
    gtk_tree_view_column_set_resizable(dp_col, TRUE);
    gtk_tree_view_column_set_expand(dp_col, TRUE);
    gtk_tree_view_append_column(ctx->detail_view, dp_col);

    /* PID column */
    GtkCellRenderer *dpid_cell = gtk_cell_renderer_text_new();
    g_object_set(dpid_cell, "xalign", 1.0f, NULL);
    GtkTreeViewColumn *dpid_col = gtk_tree_view_column_new_with_attributes(
        "PID", dpid_cell, "markup", DCOL_PID, NULL);
    gtk_tree_view_column_set_resizable(dpid_col, TRUE);
    gtk_tree_view_column_set_min_width(dpid_col, 52);
    gtk_tree_view_append_column(ctx->detail_view, dpid_col);

    /* Address column */
    GtkCellRenderer *da_cell = gtk_cell_renderer_text_new();
    g_object_set(da_cell, "family", "Monospace", NULL);
    GtkTreeViewColumn *da_col = gtk_tree_view_column_new_with_attributes(
        "Address Range", da_cell, "markup", DCOL_ADDR, NULL);
    gtk_tree_view_column_set_resizable(da_col, TRUE);
    gtk_tree_view_column_set_min_width(da_col, 160);
    gtk_tree_view_append_column(ctx->detail_view, da_col);

    /* Perms column */
    GtkCellRenderer *dperms_cell = gtk_cell_renderer_text_new();
    g_object_set(dperms_cell, "family", "Monospace", NULL);
    GtkTreeViewColumn *dperms_col = gtk_tree_view_column_new_with_attributes(
        "Perms", dperms_cell, "markup", DCOL_PERMS, NULL);
    gtk_tree_view_column_set_resizable(dperms_col, TRUE);
    gtk_tree_view_column_set_min_width(dperms_col, 48);
    gtk_tree_view_append_column(ctx->detail_view, dperms_col);

    /* Size column */
    GtkCellRenderer *dsz_cell = gtk_cell_renderer_text_new();
    g_object_set(dsz_cell, "xalign", 1.0f, NULL);
    GtkTreeViewColumn *dsz_col = gtk_tree_view_column_new_with_attributes(
        "Size", dsz_cell, "markup", DCOL_SIZE, NULL);
    gtk_tree_view_column_set_resizable(dsz_col, TRUE);
    gtk_tree_view_column_set_min_width(dsz_col, 70);
    gtk_tree_view_append_column(ctx->detail_view, dsz_col);

    ctx->detail_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->detail_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->detail_scroll),
                      GTK_WIDGET(ctx->detail_view));
    gtk_widget_set_vexpand(ctx->detail_scroll, TRUE);

    /* HPaned */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hpaned), ctx->scroll, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(hpaned), ctx->detail_scroll, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(hpaned), 340);

    gtk_box_pack_start(GTK_BOX(ctx->vbox), hpaned, TRUE, TRUE, 0);
    gtk_widget_show_all(ctx->vbox);

    return ctx->vbox;
}

/* ── background worker ───────────────────────────────────────── */

static gboolean slib_apply_result(gpointer data)
{
    slib_result_t *res = data;
    slib_ctx_t    *ctx = res->ctx;

    /* Drop stale result from a previous tick */
    if (res->generation != ctx->generation) {
        free(res->rows);
        free(res->snap);
        free(res);
        return G_SOURCE_REMOVE;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(ctx->store);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->scroll));
    double scroll_pos = gtk_adjustment_get_value(vadj);

    GtkTreeIter row;
    gboolean row_valid = gtk_tree_model_get_iter_first(model, &row);

    for (size_t i = 0; i < res->row_count; i++) {
        if (row_valid) {
            gtk_list_store_set(ctx->store, &row,
                               COL_TYPE_MARKUP,    res->rows[i].type_markup,
                               COL_NAME_MARKUP,    res->rows[i].name_markup,
                               COL_VERSION_MARKUP, res->rows[i].version_markup,
                               COL_SIZE_MARKUP,    res->rows[i].size_markup,
                               COL_PROCS_MARKUP,   res->rows[i].procs_markup,
                               COL_PATH,           res->rows[i].path, -1);
            row_valid = gtk_tree_model_iter_next(model, &row);
        } else {
            GtkTreeIter new_row;
            gtk_list_store_append(ctx->store, &new_row);
            gtk_list_store_set(ctx->store, &new_row,
                               COL_TYPE_MARKUP,    res->rows[i].type_markup,
                               COL_NAME_MARKUP,    res->rows[i].name_markup,
                               COL_VERSION_MARKUP, res->rows[i].version_markup,
                               COL_SIZE_MARKUP,    res->rows[i].size_markup,
                               COL_PROCS_MARKUP,   res->rows[i].procs_markup,
                               COL_PATH,           res->rows[i].path, -1);
        }
    }
    while (row_valid)
        row_valid = gtk_list_store_remove(ctx->store, &row);

    gtk_adjustment_set_value(vadj, scroll_pos);

    free(ctx->snap);
    ctx->snap       = res->snap;
    ctx->snap_count = res->snap_count;

    if (ctx->selected_path[0])
        populate_detail(ctx, ctx->selected_path);

    free(res->rows);
    free(res);
    return G_SOURCE_REMOVE;
}

static void *slib_worker(void *arg)
{
    slib_job_t *job = arg;

    /* ── gather ── */
    size_t cap = 256, count = 0;
    slib_entry_t *all = malloc(cap * sizeof(slib_entry_t));
    if (!all) { free(job->pids); free(job); return NULL; }

    for (size_t i = 0; i < job->pid_count; i++)
        sl_gather_pid(job->pids[i], &all, &count, &cap);

    /* ── filter + merge ── */
    gboolean has_filter = job->filter[0] != '\0';

    typedef struct { size_t first_idx; size_t proc_count; size_t total_size_kb; } merged_t;
    merged_t *merged = malloc((count > 0 ? count : 1) * sizeof(merged_t));
    size_t    mcount = 0;

    if (merged) {
        for (size_t i = 0; i < count; i++) {
            if (has_filter &&
                !strcasestr(all[i].path, job->filter) &&
                !strcasestr(all[i].name, job->filter))
                continue;
            int found = 0;
            for (size_t j = 0; j < mcount; j++) {
                if (strcmp(all[merged[j].first_idx].path, all[i].path) == 0) {
                    merged[j].proc_count++;
                    merged[j].total_size_kb += all[i].size_kb;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                merged[mcount].first_idx    = i;
                merged[mcount].proc_count   = 1;
                merged[mcount].total_size_kb = all[i].size_kb;
                mcount++;
            }
        }
    }

    /* ── sort: Application first, then Runtime, System, Wine, Windows, Other ── */
    if (mcount > 1) {
        static const int cat_prio[LIB_CAT_COUNT] = {
            [LIB_CAT_APPLICATION]  = 0,
            [LIB_CAT_RUNTIME]      = 1,
            [LIB_CAT_SYSTEM]       = 2,
            [LIB_CAT_WINE_BUILTIN] = 3,
            [LIB_CAT_WINDOWS_DLL]  = 4,
            [LIB_CAT_OTHER]        = 5,
        };
        int merged_cmp(const void *a, const void *b, void *ctx) {
            const merged_t *ma = a, *mb = b;
            slib_entry_t   *entries = ctx;
            int pa = cat_prio[entries[ma->first_idx].cat];
            int pb = cat_prio[entries[mb->first_idx].cat];
            if (pa != pb) return pa - pb;
            return strcasecmp(entries[ma->first_idx].name,
                              entries[mb->first_idx].name);
        }
        qsort_r(merged, mcount, sizeof(merged_t), merged_cmp, all);
    }

    /* ── pre-render rows ── */
    slib_row_t *rows = malloc((mcount > 0 ? mcount : 1) * sizeof(slib_row_t));
    if (!rows) { free(merged); free(all); free(job->pids); free(job); return NULL; }

    for (size_t i = 0; i < mcount; i++) {
        slib_entry_t *e = &all[merged[i].first_idx];

        snprintf(rows[i].type_markup, sizeof(rows[i].type_markup),
                 "%s", cat_labels[e->cat]);

        /* Name: bold lib name + origin in grey */
        char *safe_name = utf8_sanitize(e->name);
        char *n_esc = g_markup_escape_text(safe_name, -1);
        g_free(safe_name);
        if (e->origin[0]) {
            char *o = g_markup_escape_text(e->origin, -1);
            snprintf(rows[i].name_markup, sizeof(rows[i].name_markup),
                "<b>%s</b>  <span foreground=\"#888888\">(%s)</span>",
                n_esc, o);
            g_free(o);
        } else {
            snprintf(rows[i].name_markup, sizeof(rows[i].name_markup),
                "<b>%s</b>", n_esc);
        }
        g_free(n_esc);

        /* Version */
        if (e->version[0]) {
            char *v = g_markup_escape_text(e->version, -1);
            snprintf(rows[i].version_markup, sizeof(rows[i].version_markup),
                     "%s", v);
            g_free(v);
        } else {
            rows[i].version_markup[0] = '\0';
        }

        /* Size */
        char sz_buf[32];
        sl_format_size(merged[i].total_size_kb, sz_buf, sizeof(sz_buf));
        snprintf(rows[i].size_markup, sizeof(rows[i].size_markup),
                 "%s", sz_buf);

        /* Procs */
        if (merged[i].proc_count > 1)
            snprintf(rows[i].procs_markup, sizeof(rows[i].procs_markup),
                     "%zu", merged[i].proc_count);
        else
            rows[i].procs_markup[0] = '\0';

        snprintf(rows[i].path, sizeof(rows[i].path), "%s", e->path);
    }
    free(merged);

    slib_result_t *res = malloc(sizeof(slib_result_t));
    if (!res) { free(rows); free(all); free(job->pids); free(job); return NULL; }
    res->rows       = rows;
    res->row_count  = mcount;
    res->snap       = all;
    res->snap_count = count;
    res->ctx        = job->ctx;
    res->generation = job->generation;

    free(job->pids);
    free(job);

    g_idle_add(slib_apply_result, res);
    return NULL;
}

/* ── update (UI thread — snapshot PIDs, fire worker) ─────────── */

static void slib_update(void *opaque, const evemon_proc_data_t *data)
{
    slib_ctx_t *ctx = opaque;
    ctx->last_pid = data->pid;
    ctx->generation++;

    const char *filter_raw = gtk_entry_get_text(GTK_ENTRY(ctx->search_entry));
    g_strlcpy(ctx->filter_text, filter_raw ? filter_raw : "",
              sizeof(ctx->filter_text));

    size_t pid_count = 1 + data->descendant_count;
    pid_t *pids = malloc(pid_count * sizeof(pid_t));
    if (!pids) return;
    pids[0] = data->pid;
    for (size_t i = 0; i < data->descendant_count; i++)
        pids[1 + i] = data->descendant_pids[i];

    slib_job_t *job = malloc(sizeof(slib_job_t));
    if (!job) { free(pids); return; }
    job->pids       = pids;
    job->pid_count  = pid_count;
    job->ctx        = ctx;
    job->generation = ctx->generation;
    g_strlcpy(job->filter, ctx->filter_text, sizeof(job->filter));

    GThread *t = g_thread_try_new("slib_worker", slib_worker, job, NULL);
    if (!t)
        slib_worker(job);   /* fallback: run inline */
    else
        g_thread_unref(t);
}

static void slib_clear(void *opaque)
{
    slib_ctx_t *ctx = opaque;
    gtk_list_store_clear(ctx->store);
    gtk_list_store_clear(ctx->detail_store);
    ctx->selected_path[0] = '\0';
    free(ctx->snap);
    ctx->snap       = NULL;
    ctx->snap_count = 0;
    ctx->last_pid   = 0;
}

static void slib_destroy(void *opaque)
{
    slib_ctx_t *ctx = opaque;
    free(ctx->snap);
    free(ctx);
}

static int slib_wants_update(void *opaque)
{
    slib_ctx_t *ctx = opaque;
    return ctx->vbox &&
           GTK_IS_WIDGET(ctx->vbox) &&
           gtk_widget_get_mapped(ctx->vbox) &&
           gtk_widget_get_child_visible(ctx->vbox);
}

/* ── plugin descriptor ───────────────────────────────────────── */

__attribute__((visibility("default")))
evemon_plugin_t *evemon_plugin_init(void)
{
    slib_ctx_t *ctx = calloc(1, sizeof(slib_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    *p = (evemon_plugin_t){
        .abi_version   = evemon_PLUGIN_ABI_VERSION,
        .name          = "System Libraries",
        .id            = "org.evemon.system_libs",
        .version       = "1.0",
        .data_needs    = evemon_NEED_LIBS | evemon_NEED_DESCENDANTS,
        .plugin_ctx    = ctx,
        .create_widget = slib_create_widget,
        .update        = slib_update,
        .clear         = slib_clear,
        .destroy       = slib_destroy,
        .wants_update  = slib_wants_update,
        .role          = EVEMON_ROLE_SYSTEM,
        .dependencies  = NULL,
    };

    return p;
}
