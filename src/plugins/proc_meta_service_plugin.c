/*
 * proc_meta_service_plugin.c – Process Metadata service plugin for evemon.
 *
 * Resolves running processes to software directory metadata (organisation,
 * homepage, source URL, funding link, package name, licence) and publishes
 * EVEMON_EVENT_PROC_META once per unique (pid, name) selection.
 *
 * Cache: 5-slot time-based LRU.  The oldest slot is evicted on a miss.
 * clear() preserves the cache so re-selecting a recent process costs nothing.
 *
 * DB search order (first readable file wins):
 *   $EVEMON_SWDIR_DB
 *   $XDG_DATA_HOME/evemon/software-directory.sqlite
 *   ~/.local/share/evemon/software-directory.sqlite
 *   /usr/local/share/evemon/software-directory.sqlite
 *   /usr/share/evemon/software-directory.sqlite
 */

#include "../evemon_plugin.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

EVEMON_PLUGIN_MANIFEST(
    "org.evemon.proc_meta",
    "Process Metadata",
    "1.0",
    EVEMON_ROLE_SERVICE,
    NULL
);

/* ── 5-slot time-based LRU cache ─────────────────────────────── */

#define META_CACHE_SIZE 5

typedef struct {
    pid_t              pid;
    char               name[64];
    time_t             last_accessed;  /* 0 = empty slot */
    evemon_proc_meta_t meta;
} meta_slot_t;

/* ── Per-instance context ────────────────────────────────────── */

typedef struct {
    const evemon_host_services_t *host;
    sqlite3                      *db;
    sqlite3_stmt                 *stmt_sig;    /* signatures → families */
    sqlite3_stmt                 *stmt_alias;  /* aliases → cards       */
    sqlite3_stmt                 *stmt_card;   /* direct card lookup    */
    meta_slot_t                   cache[META_CACHE_SIZE];
    char                          init_system[32]; /* "systemd", "openrc", or "unknown" */
    meta_slot_t                  *last_published;  /* suppress duplicate publishes */
} meta_ctx_t;

/* ── DB path discovery ───────────────────────────────────────── */

static sqlite3 *open_database(void)
{
    sqlite3 *db = NULL;

    /* Only system-wide paths are trusted.  User-writable locations
     * (HOME, XDG_DATA_HOME) are intentionally excluded to prevent a
     * local user from substituting a malicious database. */
    const char *candidates[2];
    candidates[0] = "/usr/local/share/evemon/software-directory.sqlite";
    candidates[1] = "/usr/share/evemon/software-directory.sqlite";

    for (int i = 0; i < 2; i++) {
        if (!candidates[i] || !candidates[i][0]) continue;
        if (access(candidates[i], R_OK) != 0)    continue;
        if (sqlite3_open_v2(candidates[i], &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
            return db;
        sqlite3_close(db);
        db = NULL;
    }

    fprintf(stderr,
        "evemon proc_meta: software-directory database not found.\n"
        "  Expected: /usr/local/share/evemon/software-directory.sqlite\n"
        "         or /usr/share/evemon/software-directory.sqlite\n");
    return NULL;
}

/* ── Prepared statements ─────────────────────────────────────── */

/*
 * Primary: process_signatures → project_families, enriched with a
 * LEFT JOIN to program_cards for source_host, summary, package_name.
 */
static const char SQL_SIG[] =
    "SELECT pf.family, pf.display_name, pf.organization_name,"
    "  pf.homepage, pf.source_url, pf.funding_url, pf.funding_provider,"
    "  pf.primary_license, ps.confidence,"
    "  pc.package_name, pc.source_host, pc.summary"
    " FROM process_signatures ps"
    " JOIN project_families pf ON ps.project_family = pf.family"
    " LEFT JOIN program_cards pc ON pc.project_family = pf.family"
    " WHERE ps.match_kind IN ('process_aliases','exe_basenames')"
    "   AND LOWER(ps.match_value) = LOWER(?1)"
    " ORDER BY ps.confidence DESC, (ps.match_kind = 'process_aliases') DESC"
    " LIMIT 1";

/* Fallback 1: alias → card */
static const char SQL_ALIAS[] =
    "SELECT pc.package_name, pc.display_name, pc.organization_name,"
    "  pc.homepage, pc.source_url, pc.source_host,"
    "  pc.funding_url, pc.funding_provider, pc.summary, pc.primary_license"
    " FROM program_card_aliases pca"
    " JOIN program_cards pc ON pca.package_name = pc.package_name"
    " WHERE LOWER(pca.alias) = LOWER(?1)"
    " LIMIT 1";

/* Fallback 2: direct card */
static const char SQL_CARD[] =
    "SELECT package_name, display_name, organization_name,"
    "  homepage, source_url, source_host,"
    "  funding_url, funding_provider, summary, primary_license"
    " FROM program_cards"
    " WHERE LOWER(package_name) = LOWER(?1)"
    " LIMIT 1";

static int prepare_stmts(meta_ctx_t *ctx)
{
    return sqlite3_prepare_v2(ctx->db, SQL_SIG,   -1, &ctx->stmt_sig,   NULL) == SQLITE_OK
        && sqlite3_prepare_v2(ctx->db, SQL_ALIAS, -1, &ctx->stmt_alias, NULL) == SQLITE_OK
        && sqlite3_prepare_v2(ctx->db, SQL_CARD,  -1, &ctx->stmt_card,  NULL) == SQLITE_OK;
}

static void finalize_stmts(meta_ctx_t *ctx)
{
    sqlite3_finalize(ctx->stmt_sig);   ctx->stmt_sig   = NULL;
    sqlite3_finalize(ctx->stmt_alias); ctx->stmt_alias = NULL;
    sqlite3_finalize(ctx->stmt_card);  ctx->stmt_card  = NULL;
}

/* ── DB lookup ───────────────────────────────────────────────── */

static void col_str(sqlite3_stmt *s, int col, char *dst, size_t dsz)
{
    const unsigned char *v = sqlite3_column_text(s, col);
    if (v && v[0]) snprintf(dst, dsz, "%s", (const char *)v);
}

static void db_lookup(meta_ctx_t *ctx, const char *name,
                      pid_t pid, evemon_proc_meta_t *out)
{
    memset(out, 0, sizeof(*out));
    out->pid = pid;

    /* 1. signatures → families (+ enrichment join) */
    sqlite3_stmt *s = ctx->stmt_sig;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        col_str(s, 0,  out->project_family,   sizeof(out->project_family));
        col_str(s, 1,  out->display_name,     sizeof(out->display_name));
        col_str(s, 2,  out->organization,     sizeof(out->organization));
        col_str(s, 3,  out->homepage,         sizeof(out->homepage));
        col_str(s, 4,  out->source_url,       sizeof(out->source_url));
        col_str(s, 5,  out->funding_url,      sizeof(out->funding_url));
        col_str(s, 6,  out->funding_provider, sizeof(out->funding_provider));
        col_str(s, 7,  out->primary_license,  sizeof(out->primary_license));
        out->confidence = sqlite3_column_int(s, 8);
        col_str(s, 9,  out->package_name,     sizeof(out->package_name));
        col_str(s, 10, out->source_host,      sizeof(out->source_host));
        col_str(s, 11, out->summary,          sizeof(out->summary));
        if (!out->display_name[0])
            snprintf(out->display_name, sizeof(out->display_name),
                     "%s", out->project_family);
        out->matched = 1;
        return;
    }

    /* 2. aliases → cards */
    s = ctx->stmt_alias;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        col_str(s, 0, out->package_name,     sizeof(out->package_name));
        col_str(s, 1, out->display_name,     sizeof(out->display_name));
        col_str(s, 2, out->organization,     sizeof(out->organization));
        col_str(s, 3, out->homepage,         sizeof(out->homepage));
        col_str(s, 4, out->source_url,       sizeof(out->source_url));
        col_str(s, 5, out->source_host,      sizeof(out->source_host));
        col_str(s, 6, out->funding_url,      sizeof(out->funding_url));
        col_str(s, 7, out->funding_provider, sizeof(out->funding_provider));
        col_str(s, 8, out->summary,          sizeof(out->summary));
        col_str(s, 9, out->primary_license,  sizeof(out->primary_license));
        out->confidence = 70;
        out->matched    = 1;
        return;
    }

    /* 3. direct card */
    s = ctx->stmt_card;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        col_str(s, 0, out->package_name,     sizeof(out->package_name));
        col_str(s, 1, out->display_name,     sizeof(out->display_name));
        col_str(s, 2, out->organization,     sizeof(out->organization));
        col_str(s, 3, out->homepage,         sizeof(out->homepage));
        col_str(s, 4, out->source_url,       sizeof(out->source_url));
        col_str(s, 5, out->source_host,      sizeof(out->source_host));
        col_str(s, 6, out->funding_url,      sizeof(out->funding_url));
        col_str(s, 7, out->funding_provider, sizeof(out->funding_provider));
        col_str(s, 8, out->summary,          sizeof(out->summary));
        col_str(s, 9, out->primary_license,  sizeof(out->primary_license));
        out->confidence = 50;
        out->matched    = 1;
    }
}

/* ── Cache ───────────────────────────────────────────────────── */

static meta_slot_t *cache_find(meta_ctx_t *ctx, pid_t pid, const char *name)
{
    for (int i = 0; i < META_CACHE_SIZE; i++) {
        meta_slot_t *s = &ctx->cache[i];
        if (s->last_accessed && s->pid == pid && strcmp(s->name, name) == 0) {
            s->last_accessed = time(NULL);
            return s;
        }
    }
    return NULL;
}

static meta_slot_t *cache_evict_slot(meta_ctx_t *ctx)
{
    meta_slot_t *oldest = &ctx->cache[0];
    for (int i = 1; i < META_CACHE_SIZE; i++)
        if (ctx->cache[i].last_accessed < oldest->last_accessed)
            oldest = &ctx->cache[i];
    return oldest;
}

/* ── Plugin callbacks ────────────────────────────────────────── */

static GtkWidget *meta_create_widget(void *ctx) { (void)ctx; return NULL; }

static void meta_update(void *vctx, const evemon_proc_data_t *data)
{
    meta_ctx_t *ctx = vctx;
    if (!ctx->host || !ctx->db) return;

    /* data->name is not populated by the broker (no evemon_NEED_NAME flag
     * exists).  Read the process name directly from /proc/<pid>/comm. */
    char comm_buf[64] = "";
    {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", (int)data->pid);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(comm_buf, sizeof(comm_buf), f)) {
                size_t l = strlen(comm_buf);
                if (l > 0 && comm_buf[l-1] == '\n') comm_buf[l-1] = '\0';
            }
            fclose(f);
        }
    }
    const char *name = comm_buf[0] ? comm_buf : (data->name ? data->name : "");

    /* For PID 1, the comm may be a generic "init" that won't match anything.
     * Use the detected init system name (set at activate time) as the lookup
     * key so that selecting PID 1 shows the systemd/openrc software card. */
    char pid1_name_buf[64];
    if (data->pid == 1 && ctx->init_system[0] &&
        (strcmp(name, "init") == 0 || strcmp(name, "1") == 0 || name[0] == '\0')) {
        snprintf(pid1_name_buf, sizeof(pid1_name_buf), "%s", ctx->init_system);
        name = pid1_name_buf;
    }

    meta_slot_t *slot = cache_find(ctx, data->pid, name);
    if (!slot) {
        slot = cache_evict_slot(ctx);
        slot->pid  = data->pid;
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        slot->last_accessed = time(NULL);
        db_lookup(ctx, name, data->pid, &slot->meta);
    }

    /* Skip publish if this is the same resolved slot as last time.
     * Without this, every poll cycle re-fires show_all in the UI
     * even when nothing changed, causing visible blinking. */
    if (slot == ctx->last_published) return;
    ctx->last_published = slot;

    evemon_event_t ev = { .type = EVEMON_EVENT_PROC_META, .payload = &slot->meta };
    ctx->host->publish(ctx->host->host_ctx, &ev);
}

static void meta_clear(void *vctx)
{
    meta_ctx_t *ctx = vctx;
    if (!ctx->host) return;
    /* Publish empty to signal "nothing selected"; cache is intentionally kept.
     * Reset last_published so the next selection always fires a fresh publish. */
    ctx->last_published = NULL;
    evemon_proc_meta_t empty = { 0 };
    evemon_event_t ev = { .type = EVEMON_EVENT_PROC_META, .payload = &empty };
    ctx->host->publish(ctx->host->host_ctx, &ev);
}

static void meta_activate(void *vctx, const evemon_host_services_t *services)
{
    meta_ctx_t *ctx = vctx;
    ctx->host = services;
    ctx->db   = open_database();
    if (ctx->db && !prepare_stmts(ctx)) {
        fprintf(stderr, "evemon proc_meta: failed to prepare statements: %s\n",
                sqlite3_errmsg(ctx->db));
        sqlite3_close(ctx->db);
        ctx->db = NULL;
    }

    /* Detect init system once at activation time. */
    {
        struct stat st;
        if (stat("/run/systemd/system", &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(ctx->init_system, sizeof(ctx->init_system), "systemd");
        else if (stat("/run/openrc", &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(ctx->init_system, sizeof(ctx->init_system), "openrc");
        else
            snprintf(ctx->init_system, sizeof(ctx->init_system), "unknown");
    }
}

static void meta_destroy(void *vctx)
{
    meta_ctx_t *ctx = vctx;
    finalize_stmts(ctx);
    if (ctx->db) sqlite3_close(ctx->db);
    free(ctx);
}

/* ── Entry point ─────────────────────────────────────────────── */

evemon_plugin_t *evemon_plugin_init(void)
{
    meta_ctx_t *ctx = calloc(1, sizeof(meta_ctx_t));
    if (!ctx) return NULL;

    evemon_plugin_t *p = calloc(1, sizeof(evemon_plugin_t));
    if (!p) { free(ctx); return NULL; }

    p->abi_version   = evemon_PLUGIN_ABI_VERSION;
    p->name          = "Process Metadata";
    p->id            = "org.evemon.proc_meta";
    p->version       = "1.0";
    p->data_needs    = 0;
    p->role          = EVEMON_ROLE_SERVICE;
    p->plugin_ctx    = ctx;
    p->create_widget = meta_create_widget;
    p->update        = meta_update;
    p->clear         = meta_clear;
    p->activate      = meta_activate;
    p->destroy       = meta_destroy;

    return p;
}
