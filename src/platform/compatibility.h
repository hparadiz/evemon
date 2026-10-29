#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMPAT_OK,       /* feature fully available                  */
    COMPAT_WARN,     /* feature available but degraded           */
    COMPAT_FAIL,     /* feature unavailable                      */
    COMPAT_INFO,     /* neutral information row                  */
} compat_severity_t;

/* ── single check result ─────────────────────────────────────── */

typedef struct {
    compat_severity_t severity;
    const char       *feature;     /* short feature name           */
    const char       *status;      /* one-word status label        */
    char              detail[512]; /* explanation string           */
} compat_result_t;

int evemon_collect_compat_results(compat_result_t *out, int capacity);
#ifdef __cplusplus
}
#endif
