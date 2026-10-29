#include <stdio.h>
#include <time.h>
/* ── formatting helpers ──────────────────────────────────────── */

/* Format a KiB value into a human-readable string. */
void format_memory(long kb, char *buf, size_t bufsz)
{
    if (kb <= 0)
        snprintf(buf, bufsz, "–");
    else if (kb < 1024)
        snprintf(buf, bufsz, "%ld KiB", kb);
    else if (kb < 1024 * 1024)
        snprintf(buf, bufsz, "%.1f MiB", (double)kb / 1024.0);
    else
        snprintf(buf, bufsz, "%.2f GiB", (double)kb / (1024.0 * 1024.0));
}

/* Format an elapsed-seconds value into a human-friendly "ago" string. */
void format_fuzzy_time(time_t epoch, char *buf, size_t bufsz)
{
    if (epoch <= 0) { snprintf(buf, bufsz, "–"); return; }

    time_t now = time(NULL);
    long diff = (long)(now - epoch);
    if (diff < 0) { snprintf(buf, bufsz, "just now"); return; }

    if (diff < 60)
        snprintf(buf, bufsz, "%lds ago", diff);
    else if (diff < 3600)
        snprintf(buf, bufsz, "%ldm %lds ago", diff / 60, diff % 60);
    else if (diff < 86400) {
        long h = diff / 3600;
        long m = (diff % 3600) / 60;
        snprintf(buf, bufsz, "%ldh %ldm ago", h, m);
    } else if (diff < 86400 * 30L) {
        long d = diff / 86400;
        long h = (diff % 86400) / 3600;
        snprintf(buf, bufsz, "%ldd %ldh ago", d, h);
    } else if (diff < 86400 * 365L) {
        long d = diff / 86400;
        snprintf(buf, bufsz, "%ldd ago", d);
    } else {
        long y = diff / (86400 * 365L);
        long d = (diff % (86400 * 365L)) / 86400;
        snprintf(buf, bufsz, "%ldy %ldd ago", y, d);
    }
}

