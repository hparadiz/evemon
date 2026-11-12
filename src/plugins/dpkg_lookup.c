/*
 * dpkg_lookup.c – which Debian package or snap owns a process executable.
 *
 * Shared by the dpkg plugin (local host) and headless Evemon, which
 * answers DPKG requests from remote GUIs (see docs/protocol.md).
 *
 * Ownership is found the way `dpkg -S` does it, by searching the
 * per-package file lists, so no reverse index is built or kept; results
 * are cached per executable until /var/lib/dpkg/status changes.
 */

#include "dpkg_lookup.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <time.h>

/* Filesystem root; tests point this at a fixture tree. */
static const char *dpkg_root = "";

static void rooted(char *out, size_t outsz, const char *path)
{
    snprintf(out, outsz, "%s%s", dpkg_root, path);
}

static gboolean cancelled(dpkg_cancel_fn cancel, void *ud)
{
    return cancel && cancel(ud);
}

static void format_size(long long bytes, char *buf, size_t bufsz)
{
    if      (bytes <= 0)          snprintf(buf, bufsz, "—");
    else if (bytes < 1024)        snprintf(buf, bufsz, "%lld B",   bytes);
    else if (bytes < 1048576)     snprintf(buf, bufsz, "%.1f KiB", (double)bytes / 1024.0);
    else if (bytes < 1073741824)  snprintf(buf, bufsz, "%.1f MiB", (double)bytes / 1048576.0);
    else                          snprintf(buf, bufsz, "%.2f GiB", (double)bytes / 1073741824.0);
}

static void format_mtime(const char *path, char *buf, size_t bufsz)
{
    struct stat st;
    struct tm tm_buf;
    char full[PATH_MAX];
    rooted(full, sizeof(full), path);
    if (stat(full, &st) != 0 || !localtime_r(&st.st_mtime, &tm_buf)) {
        snprintf(buf, bufsz, "—");
        return;
    }
    strftime(buf, bufsz, "%Y-%m-%d %H:%M", &tm_buf);
}

/* ── Ownership: which package's file list names this executable ──── */

/*
 * usrmerge: a package may record /bin/bash while the process runs as
 * /usr/bin/bash, or the other way round.  Try both spellings.
 */
static int path_variants(const char *exe, char out[2][PATH_MAX])
{
    static const char *const pairs[][2] = {
        {"/usr/bin/", "/bin/"}, {"/usr/sbin/", "/sbin/"},
        {"/usr/lib64/", "/lib64/"}, {"/usr/lib/", "/lib/"},
    };
    int n = 0;
    snprintf(out[n++], PATH_MAX, "%s", exe);
    for (size_t i = 0; i < G_N_ELEMENTS(pairs); i++) {
        for (int dir = 0; dir < 2; dir++) {
            const char *from = pairs[i][dir], *to = pairs[i][!dir];
            size_t flen = strlen(from);
            if (strncmp(exe, from, flen) == 0) {
                snprintf(out[n++], PATH_MAX, "%s%s", to, exe + flen);
                return n;
            }
        }
    }
    return n;
}

/* True when `path` is a whole line of a dpkg .list file. */
static gboolean list_has_line(const char *data, gsize len, const char *path)
{
    size_t plen = strlen(path);
    if (len >= plen + 1 && memcmp(data, path, plen) == 0 && data[plen] == '\n')
        return TRUE;
    char needle[PATH_MAX + 2];
    int n = snprintf(needle, sizeof(needle), "\n%s\n", path);
    return n > 0 && (size_t)n < sizeof(needle) &&
           memmem(data, len, needle, (size_t)n) != NULL;
}

static gint list_order(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    const char *base = user_data;
    gboolean hx = strstr(x, base) != NULL, hy = strstr(y, base) != NULL;
    return hx == hy ? 0 : (hx ? -1 : 1);
}

/*
 * Finds the owning package as "name" or "name:arch" (the .list file stem).
 * Lists whose name contains the executable's basename are searched first,
 * which usually finds the owner within the first few files.
 * Returns FALSE only when cancelled.
 */
static gboolean find_owner(const char *exe, char *owner, size_t ownersz,
                           dpkg_cancel_fn cancel, void *ud)
{
    owner[0] = '\0';
    char info_dir[PATH_MAX];
    rooted(info_dir, sizeof(info_dir), "/var/lib/dpkg/info");
    DIR *dir = opendir(info_dir);
    if (!dir)
        return TRUE;

    GPtrArray *lists = g_ptr_array_new_with_free_func(g_free);
    struct dirent *d;
    while ((d = readdir(dir))) {
        size_t len = strlen(d->d_name);
        if (len > 5 && strcmp(d->d_name + len - 5, ".list") == 0)
            g_ptr_array_add(lists, g_strdup(d->d_name));
    }
    closedir(dir);

    const char *slash = strrchr(exe, '/');
    char base[NAME_MAX + 1];
    snprintf(base, sizeof(base), "%s", slash ? slash + 1 : exe);
    g_ptr_array_sort_with_data(lists, list_order, base);

    char variants[2][PATH_MAX];
    int nvariants = path_variants(exe, variants);
    gboolean completed = TRUE;

    for (guint i = 0; i < lists->len; i++) {
        if ((i & 31u) == 0 && cancelled(cancel, ud)) {
            completed = FALSE;
            break;
        }
        char path[PATH_MAX];
        g_snprintf(path, sizeof(path), "%s/%s", info_dir, (const char *)lists->pdata[i]);
        gchar *data = NULL;
        gsize len = 0;
        if (!g_file_get_contents(path, &data, &len, NULL))
            continue;
        gboolean hit = FALSE;
        for (int v = 0; v < nvariants && !hit; v++)
            hit = list_has_line(data, len, variants[v]);
        g_free(data);
        if (hit) {
            const char *stem = lists->pdata[i];
            snprintf(owner, ownersz, "%.*s", (int)(strlen(stem) - 5), stem);
            break;
        }
    }
    g_ptr_array_free(lists, TRUE);
    return completed;
}

/* ── Debian control stanzas ──────────────────────────────────────── */

/* Value of `key` in one stanza, with continuation lines joined by '\n'. */
static gboolean stanza_field(const char *stanza, size_t len, const char *key,
                             char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = stanza, *end = stanza + len;
    out[0] = '\0';
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        if ((size_t)(eol - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t used = (size_t)snprintf(out, outsz, "%.*s", (int)(eol - v), v);
            if (used >= outsz) used = outsz - 1;
            /* Continuation lines start with whitespace. */
            const char *q = eol < end ? eol + 1 : end;
            while (q < end && (*q == ' ' || *q == '\t')) {
                const char *qe = memchr(q, '\n', (size_t)(end - q));
                if (!qe) qe = end;
                used += (size_t)snprintf(out + used, outsz - used, "\n%.*s", (int)(qe - q), q);
                if (used >= outsz) { used = outsz - 1; break; }
                q = qe < end ? qe + 1 : end;
            }
            return TRUE;
        }
        p = eol < end ? eol + 1 : end;
    }
    return FALSE;
}

/*
 * Locates the stanza for `name` (optionally a given architecture) in a
 * control-format buffer.  Stanzas begin with "Package: <name>".
 */
static const char *find_stanza(const char *data, size_t len, const char *name,
                               const char *arch, size_t *stanza_len)
{
    char needle[256];
    int n = snprintf(needle, sizeof(needle), "Package: %s\n", name);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return NULL;
    const char *end = data + len, *p = data;
    while (p < end) {
        const char *hit = memmem(p, (size_t)(end - p), needle, (size_t)n);
        if (!hit) return NULL;
        if (hit == data || hit[-1] == '\n') {
            const char *stop = memmem(hit, (size_t)(end - hit), "\n\n", 2);
            size_t slen = stop ? (size_t)(stop - hit) + 1 : (size_t)(end - hit);
            char value[64];
            if (!arch || !arch[0] ||
                !stanza_field(hit, slen, "Architecture", value, sizeof(value)) ||
                strcmp(value, arch) == 0 || strcmp(value, "all") == 0) {
                *stanza_len = slen;
                return hit;
            }
        }
        p = hit + n;
    }
    return NULL;
}

/* Debian long descriptions: one leading space per line, " ." for blank lines. */
static void unfold_description(const char *raw, char *summary, size_t sumsz,
                               char *body, size_t bodysz)
{
    const char *nl = strchr(raw, '\n');
    snprintf(summary, sumsz, "%.*s", nl ? (int)(nl - raw) : (int)strlen(raw), raw);
    body[0] = '\0';
    if (!nl) return;
    size_t used = 0;
    for (const char *p = nl + 1; *p && used < bodysz - 1; ) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        const char *line = p;
        if (l && (*line == ' ' || *line == '\t')) { line++; l--; }
        if (l == 1 && *line == '.') l = 0;
        used += (size_t)snprintf(body + used, bodysz - used, "%s%.*s",
                                 used ? "\n" : "", (int)l, line);
        if (!e) break;
        p = e + 1;
    }
}

/* One relationship field as an indented list, one alternative group per line. */
static void append_relations(char *out, size_t outsz, const char *title, const char *value)
{
    if (!value[0]) return;
    size_t used = strlen(out);
    used += (size_t)snprintf(out + used, outsz - used, "%s%s:\n", used ? "\n" : "", title);
    gchar **items = g_strsplit(value, ",", -1);
    for (gchar **it = items; *it && used < outsz - 1; it++) {
        gchar *item = g_strstrip(g_strdelimit(*it, "\n", ' '));
        if (*item)
            used += (size_t)snprintf(out + used, outsz - used, "  %s\n", item);
    }
    g_strfreev(items);
}

static gboolean read_status(const char *name, const char *arch, dpkg_result_t *res)
{
    char path[PATH_MAX];
    rooted(path, sizeof(path), "/var/lib/dpkg/status");
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, NULL))
        return FALSE;
    size_t slen = 0;
    const char *st = find_stanza(data, len, name, arch, &slen);
    if (!st) { g_free(data); return FALSE; }

    char value[DPKG_DEP_SIZE];
    snprintf(res->name, sizeof(res->name), "%s", name);
    stanza_field(st, slen, "Version", res->version, sizeof(res->version));
    stanza_field(st, slen, "Architecture", res->arch, sizeof(res->arch));
    stanza_field(st, slen, "Section", res->section, sizeof(res->section));
    stanza_field(st, slen, "Priority", res->priority, sizeof(res->priority));
    stanza_field(st, slen, "Maintainer", res->maintainer, sizeof(res->maintainer));
    stanza_field(st, slen, "Original-Maintainer", res->original_maintainer, sizeof(res->original_maintainer));
    stanza_field(st, slen, "Homepage", res->homepage, sizeof(res->homepage));
    stanza_field(st, slen, "Multi-Arch", res->multi_arch, sizeof(res->multi_arch));
    if (stanza_field(st, slen, "Essential", value, sizeof(value)))
        res->essential = strcmp(value, "yes") == 0;
    if (stanza_field(st, slen, "Protected", value, sizeof(value)))
        res->protected_pkg = strcmp(value, "yes") == 0;

    /* "Source: name (version)" when the source version differs. */
    if (stanza_field(st, slen, "Source", value, sizeof(value)))
        g_strlcpy(res->source, value, sizeof(res->source));
    else
        snprintf(res->source, sizeof(res->source), "%s", name);

    if (stanza_field(st, slen, "Installed-Size", value, sizeof(value)))
        format_size(atoll(value) * 1024, res->size_str, sizeof(res->size_str));

    if (stanza_field(st, slen, "Description", value, sizeof(value)))
        unfold_description(value, res->summary, sizeof(res->summary),
                           res->description, sizeof(res->description));

    static const char *const relations[][2] = {
        {"Pre-Depends", "Pre-Depends"}, {"Depends", "Depends"},
        {"Recommends", "Recommends"}, {"Suggests", "Suggests"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(relations); i++)
        if (stanza_field(st, slen, relations[i][0], value, sizeof(value)))
            append_relations(res->depends, sizeof(res->depends), relations[i][1], value);

    g_free(data);
    return TRUE;
}

/* ── Repository origin, as APT itself reports it ─────────────────── */

/*
 * "http://archive.ubuntu.com/ubuntu" + "noble-updates/main"
 *   → "noble-updates/main (archive.ubuntu.com)"
 * "https://ppa.launchpadcontent.net/owner/name/ubuntu" + "noble/main"
 *   → "ppa:owner/name (noble)"
 */
static void describe_apt_source(const char *uri, const char *dist, char *out, size_t outsz)
{
    const char *host = strstr(uri, "://");
    host = host ? host + 3 : uri;
    const char *path = strchr(host, '/');
    char hostname[128], owner[64], name[64];
    snprintf(hostname, sizeof(hostname), "%.*s", path ? (int)(path - host) : (int)strlen(host), host);
    if (strstr(hostname, "launchpad") && path &&
        sscanf(path, "/%63[^/]/%63[^/]", owner, name) == 2)
        g_snprintf(out, outsz, "ppa:%s/%s (%.*s)", owner, name, (int)strcspn(dist, "/"), dist);
    else
        g_snprintf(out, outsz, "%s (%s)", dist, hostname);
}

/*
 * Sources of the installed ("***") version in `apt-cache policy` output:
 *
 *    *** 1.7.1-3ubuntu0.24.04.2 500
 *           500 http://archive.ubuntu.com/ubuntu noble-updates/main amd64 Packages
 *           100 /var/lib/dpkg/status
 */
static void parse_policy(const char *text, char *out, size_t outsz)
{
    out[0] = '\0';
    size_t used = 0;
    gboolean in_installed = FALSE, saw_installed = FALSE;
    gchar **lines = g_strsplit(text, "\n", -1);
    for (gchar **lp = lines; *lp; lp++) {
        size_t indent = strspn(*lp, " ");
        const char *t = *lp + indent;
        if (!*t) continue;
        if (g_str_has_prefix(t, "*** ")) {
            in_installed = saw_installed = TRUE;
            continue;
        }
        if (indent < 8) {               /* the next version, or a header line */
            in_installed = FALSE;
            continue;
        }
        char uri[256], dist[128];
        int priority;
        if (!in_installed || sscanf(t, "%d %255s %127s", &priority, uri, dist) != 3)
            continue;                   /* "100 /var/lib/dpkg/status" has no dist */
        char label[256];
        describe_apt_source(uri, dist, label, sizeof(label));
        used += (size_t)g_snprintf(out + used, outsz - used, "%s%s", used ? ", " : "", label);
        if (used >= outsz) used = outsz - 1;
    }
    g_strfreev(lines);
    if (saw_installed && !out[0])
        snprintf(out, outsz, "Not in any configured repository (local or obsolete)");
}

/* Package version → origin label; apt-cache can take a while on a cold cache. */
G_LOCK_DEFINE_STATIC(origin_cache);
static GHashTable *origin_cache;

/*
 * apt-cache knows which repositories carry the installed version whatever
 * the list format (plain, gz, lz4) and honours pinning.
 */
static gboolean read_origin(dpkg_result_t *res, dpkg_cancel_fn cancel, void *ud)
{
    if (dpkg_root[0] || access("/usr/bin/apt-cache", X_OK) != 0)
        return TRUE;

    char spec[192];
    if (res->arch[0] && strcmp(res->arch, "all") != 0)
        snprintf(spec, sizeof(spec), "%s:%s", res->name, res->arch);
    else
        snprintf(spec, sizeof(spec), "%s", res->name);
    gchar *key = g_strdup_printf("%s=%s", spec, res->version);

    G_LOCK(origin_cache);
    if (!origin_cache)
        origin_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const char *cached = g_hash_table_lookup(origin_cache, key);
    if (cached)
        g_strlcpy(res->origin, cached, sizeof(res->origin));
    G_UNLOCK(origin_cache);
    if (cached || cancelled(cancel, ud)) {
        g_free(key);
        return cached != NULL;
    }

    gchar *argv[] = { "/usr/bin/apt-cache", "policy", spec, NULL };
    gchar *envp[] = { "LC_ALL=C", "PATH=/usr/bin:/bin", NULL };
    gchar *out = NULL;
    gint status = 0;
    if (g_spawn_sync(NULL, argv, envp, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                     &out, NULL, &status, NULL) &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 && out) {
        parse_policy(out, res->origin, sizeof(res->origin));
        G_LOCK(origin_cache);
        g_hash_table_replace(origin_cache, key, g_strdup(res->origin));
        G_UNLOCK(origin_cache);
        key = NULL;
    }
    g_free(key);
    g_free(out);
    return !cancelled(cancel, ud);
}

/* ── Install reason, license, install time ───────────────────────── */

static void read_install_reason(dpkg_result_t *res)
{
    char path[PATH_MAX];
    rooted(path, sizeof(path), "/var/lib/apt/extended_states");
    gchar *data = NULL;
    gsize len = 0;
    res->automatic = -1;
    if (!g_file_get_contents(path, &data, &len, NULL))
        return;
    size_t slen = 0;
    /* APT records Architecture: all packages under the native architecture. */
    const char *arch = strcmp(res->arch, "all") != 0 ? res->arch : NULL;
    const char *st = find_stanza(data, len, res->name, arch, &slen);
    char value[16];
    /* Packages APT never marked automatic are absent: they were installed by hand. */
    res->automatic = st && stanza_field(st, slen, "Auto-Installed", value, sizeof(value))
                   ? atoi(value) == 1 : 0;
    g_free(data);
}

/*
 * Licenses from a DEP-5 (machine-readable) copyright file: the License
 * short names of its Files paragraphs, main paragraph first.
 */
static void read_license(dpkg_result_t *res)
{
    char path[PATH_MAX], rel[PATH_MAX];
    snprintf(rel, sizeof(rel), "/usr/share/doc/%s/copyright", res->name);
    rooted(path, sizeof(path), rel);
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, NULL)) {
        snprintf(res->license, sizeof(res->license), "—");
        return;
    }
    if (!strstr(data, "copyright-format")) {
        g_snprintf(res->license, sizeof(res->license), "See %s", rel);
        g_free(data);
        return;
    }

    size_t used = 0;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gchar **paragraphs = g_strsplit(data, "\n\n", -1);
    for (gchar **p = paragraphs; *p && g_hash_table_size(seen) < 6; p++) {
        if (!g_str_has_prefix(*p, "Files:") && !strstr(*p, "\nFiles:"))
            continue;
        char value[256];
        if (!stanza_field(*p, strlen(*p), "License", value, sizeof(value)))
            continue;
        char *nl = strchr(value, '\n');
        if (nl) *nl = '\0';
        g_strstrip(value);
        if (!value[0] || g_hash_table_contains(seen, value))
            continue;
        g_hash_table_add(seen, g_strdup(value));
        used += (size_t)snprintf(res->license + used, sizeof(res->license) - used,
                                 "%s%s", used ? " · " : "", value);
        if (used >= sizeof(res->license)) used = sizeof(res->license) - 1;
    }
    g_strfreev(paragraphs);
    g_hash_table_destroy(seen);
    g_free(data);
    if (!res->license[0])
        g_snprintf(res->license, sizeof(res->license), "See %s", rel);
}

/* ── Snaps ────────────────────────────────────────────────────────── */

/* Top-level scalar and block-scalar keys of meta/snap.yaml. */
static void snap_yaml_field(const char *yaml, const char *key, char *out, size_t outsz)
{
    out[0] = '\0';
    size_t klen = strlen(key);
    for (const char *p = yaml; p && *p; ) {
        const char *e = strchr(p, '\n');
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (*v == ' ') v++;
            size_t vl = e ? (size_t)(e - v) : strlen(v);
            if (vl && (*v == '|' || *v == '>')) {
                /* Block scalar: the following indented lines. */
                size_t used = 0;
                for (const char *q = e ? e + 1 : NULL; q && (*q == ' ' || *q == '\n'); ) {
                    const char *qe = strchr(q, '\n');
                    const char *line = q;
                    while (*line == ' ') line++;
                    size_t ll = qe ? (size_t)(qe - line) : strlen(line);
                    used += (size_t)snprintf(out + used, outsz - used, "%s%.*s",
                                             used ? "\n" : "", (int)ll, line);
                    if (used >= outsz) break;
                    q = qe ? qe + 1 : NULL;
                }
                g_strchomp(out);
            } else {
                if (vl >= 2 && (*v == '\'' || *v == '"') && v[vl - 1] == *v) { v++; vl -= 2; }
                snprintf(out, outsz, "%.*s", (int)vl, v);
            }
            return;
        }
        p = e ? e + 1 : NULL;
    }
}

static gboolean read_snap(const char *exe, dpkg_result_t *res)
{
    char name[128], revision[32];
    if (sscanf(exe, "/snap/%127[^/]/%31[^/]/", name, revision) != 2)
        return FALSE;

    char rel[PATH_MAX], path[PATH_MAX];
    snprintf(rel, sizeof(rel), "/snap/%s/%s/meta/snap.yaml", name, revision);
    rooted(path, sizeof(path), rel);
    gchar *yaml = NULL;
    if (!g_file_get_contents(path, &yaml, NULL, NULL))
        return FALSE;

    res->snap = 1;
    res->found = 1;
    snprintf(res->name, sizeof(res->name), "%s", name);
    snprintf(res->revision, sizeof(res->revision), "%s", revision);
    snap_yaml_field(yaml, "version", res->version, sizeof(res->version));
    snap_yaml_field(yaml, "summary", res->summary, sizeof(res->summary));
    snap_yaml_field(yaml, "description", res->description, sizeof(res->description));
    snap_yaml_field(yaml, "license", res->license, sizeof(res->license));
    snap_yaml_field(yaml, "base", res->base, sizeof(res->base));
    snap_yaml_field(yaml, "grade", res->grade, sizeof(res->grade));
    snap_yaml_field(yaml, "confinement", res->confinement, sizeof(res->confinement));
    if (!res->license[0]) snprintf(res->license, sizeof(res->license), "—");
    if (!res->confinement[0]) snprintf(res->confinement, sizeof(res->confinement), "strict");
    g_free(yaml);

    snprintf(rel, sizeof(rel), "/var/lib/snapd/snaps/%s_%s.snap", name, revision);
    rooted(path, sizeof(path), rel);
    struct stat st;
    if (stat(path, &st) == 0)
        format_size((long long)st.st_size, res->size_str, sizeof(res->size_str));
    format_mtime(rel, res->installed_str, sizeof(res->installed_str));
    snprintf(res->origin, sizeof(res->origin), "Snap Store");
    return TRUE;
}

/* ── Full lookup ──────────────────────────────────────────────────── */

/*
 * Resolves a package (or snap) for an executable.  `owner` caches the
 * .list stem between calls; pass an empty string to search.
 * Returns FALSE only when cancelled before finishing.
 */
static gboolean dpkg_lookup(const char *exe, char *owner, size_t ownersz,
                            dpkg_result_t *res, dpkg_cancel_fn cancel, void *ud)
{
    memset(res, 0, sizeof(*res));
    if (g_str_has_prefix(exe, "/snap/") && read_snap(exe, res))
        return TRUE;

    if (!owner[0] && !find_owner(exe, owner, ownersz, cancel, ud))
        return FALSE;
    if (!owner[0] || strcmp(owner, "-") == 0) {
        snprintf(owner, ownersz, "-");   /* cached miss */
        return TRUE;
    }

    char name[128], arch[32] = "";
    const char *colon = strchr(owner, ':');
    snprintf(name, sizeof(name), "%.*s", colon ? (int)(colon - owner) : (int)strlen(owner), owner);
    if (colon) snprintf(arch, sizeof(arch), "%s", colon + 1);

    if (!read_status(name, arch, res))
        return TRUE;
    char list[PATH_MAX];
    snprintf(list, sizeof(list), "/var/lib/dpkg/info/%s.list", owner);
    format_mtime(list, res->installed_str, sizeof(res->installed_str));
    read_install_reason(res);
    read_license(res);
    if (!read_origin(res, cancel, ud))
        return FALSE;
    res->found = 1;
    return TRUE;
}

/* ── Per-process entry point with shared caches ──────────────────── */

int dpkg_available(void)
{
    struct stat st;
    return stat("/var/lib/dpkg/status", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

const char *dpkg_distro_label(void)
{
    static char label[64];
    static gsize once;
    if (!g_once_init_enter(&once))
        return label;

    char id[64] = "", name[64] = "";
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "ID=\"%63[^\"\n]", id) == 1 || sscanf(line, "ID=%63[^\n]", id) == 1) continue;
            if (sscanf(line, "NAME=\"%63[^\"\n]", name) == 1 || sscanf(line, "NAME=%63[^\n]", name) == 1) continue;
        }
        fclose(f);
    }
    snprintf(label, sizeof(label), "%s", !strcmp(id, "ubuntu") ? "Ubuntu"
                                        : !strcmp(id, "debian") ? "Debian"
                                        : name[0] ? name : "Debian");
    g_once_init_leave(&once, 1);
    return label;
}

static void resolve_proc_exe(pid_t pid, char *exe_path, size_t exe_path_sz)
{
    exe_path[0] = '\0';

    char proc_exe[64];
    snprintf(proc_exe, sizeof(proc_exe), "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(proc_exe, exe_path, exe_path_sz - 1);
    if (n <= 0)
        return;

    exe_path[n] = '\0';
    static const char deleted_sfx[] = " (deleted)";
    size_t elen = (size_t)n, dlen = sizeof(deleted_sfx) - 1;
    if (elen > dlen && strcmp(exe_path + elen - dlen, deleted_sfx) == 0)
        exe_path[elen - dlen] = '\0';
}

/*
 * /proc/<pid>/exe reads as a path in the process's own mount namespace.  It
 * names a file of this machine's packages only when the process shares our
 * root and mount namespace, or when that path here is the very file being run (a snap is the
 * same squashfs at /snap/<name>/<revision> inside and outside its sandbox).
 * Without this a container's /usr/bin/python3 is reported as the host's.
 */
static gboolean exe_is_host_file(pid_t pid, const char *exe_path)
{
    char link[64];
    struct stat ours, theirs, our_ns, their_ns;

    snprintf(link, sizeof(link), "/proc/%d/ns/mnt", (int)pid);
    gboolean same_mounts = stat("/proc/self/ns/mnt", &our_ns) == 0 &&
        stat(link, &their_ns) == 0 && our_ns.st_dev == their_ns.st_dev &&
        our_ns.st_ino == their_ns.st_ino;
    snprintf(link, sizeof(link), "/proc/%d/root", (int)pid);
    if (same_mounts && stat("/", &ours) == 0 && stat(link, &theirs) == 0 &&
        ours.st_dev == theirs.st_dev && ours.st_ino == theirs.st_ino)
        return TRUE;

    snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
    return stat(link, &theirs) == 0 && stat(exe_path, &ours) == 0 &&
           ours.st_dev == theirs.st_dev && ours.st_ino == theirs.st_ino;
}

/* exe path -> .list stem, "-" for misses. */
G_LOCK_DEFINE_STATIC(owner_cache);
static GHashTable *owner_cache;

/*
 * A long-lived Evemon outlives package installs, upgrades and removals:
 * drop cached ownership and repository origins when the status file changes.
 */
static void caches_validate(void)
{
    static struct stat seen;
    struct stat st;
    char path[PATH_MAX];
    rooted(path, sizeof(path), "/var/lib/dpkg/status");
    if (stat(path, &st) != 0)
        memset(&st, 0, sizeof(st));

    G_LOCK(owner_cache);
    gboolean changed = st.st_ino != seen.st_ino || st.st_size != seen.st_size ||
                       st.st_mtim.tv_sec != seen.st_mtim.tv_sec ||
                       st.st_mtim.tv_nsec != seen.st_mtim.tv_nsec;
    if (changed) {
        seen = st;
        if (owner_cache)
            g_hash_table_remove_all(owner_cache);
    }
    if (!owner_cache)
        owner_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    G_UNLOCK(owner_cache);

    if (changed) {
        G_LOCK(origin_cache);
        if (origin_cache)
            g_hash_table_remove_all(origin_cache);
        G_UNLOCK(origin_cache);
    }
}

gboolean dpkg_lookup_pid(pid_t pid, dpkg_result_t *res,
                         dpkg_cancel_fn cancel, void *user_data)
{
    char exe[PATH_MAX];
    resolve_proc_exe(pid, exe, sizeof(exe));
    if (!exe[0]) {
        memset(res, 0, sizeof(*res));
        return TRUE;
    }
    if (!exe_is_host_file(pid, exe)) {
        /* Container, chroot or flatpak runtime: not this machine's packages. */
        memset(res, 0, sizeof(*res));
        res->has_exe = 1;
        return TRUE;
    }

    caches_validate();
    char owner[256] = "";
    G_LOCK(owner_cache);
    const char *cached = g_hash_table_lookup(owner_cache, exe);
    gboolean hit = cached != NULL;
    if (hit)
        snprintf(owner, sizeof(owner), "%s", cached);
    G_UNLOCK(owner_cache);

    gboolean completed = dpkg_lookup(exe, owner, sizeof(owner), res, cancel, user_data);
    res->has_exe = 1;
    if (owner[0] && !hit) {
        G_LOCK(owner_cache);
        g_hash_table_replace(owner_cache, g_strdup(exe), g_strdup(owner));
        G_UNLOCK(owner_cache);
    }
    return completed;
}
