/*
 * log.h – centralised logging for evemon.
 *
 * Usage:
 *   evemon_log(LOG_INFO,  "loaded %d plugins", n);
 *   evemon_log(LOG_DEBUG, "detail: x=%d", x);
 *   evemon_log(LOG_AUDIO, "pw node id=%u pid=%d", id, pid);
 *   evemon_log(LOG_ERROR, "failed to open %s", path);
 *
 * Filtering (set by CLI flags in main.c):
 *   LOG_INFO  — always printed
 *   LOG_ERROR — always printed (to stderr)
 *   LOG_DEBUG — only when evemon_debug  != 0  (--debug)
 *   LOG_AUDIO — only when evemon_debug_audio != 0  (--debug-audio)
 */

#ifndef EVEMON_LOG_H
#define EVEMON_LOG_H

#include <stdio.h>

typedef enum {
    LOG_INFO,   /* always visible — normal operational messages   */
    LOG_ERROR,  /* always visible — printed to stderr             */
    LOG_DEBUG,  /* gated behind --debug                           */
    LOG_AUDIO,  /* gated behind --debug-audio                     */
} evemon_log_type_t;

extern int evemon_debug;
extern int evemon_debug_audio;

/*
 * evemon_log(type, fmt, ...)
 *
 * Checks the filter, then prints to stdout (INFO/DEBUG/AUDIO) or
 * stderr (ERROR).  A newline is always appended; callers must not
 * include one in fmt.
 */
void evemon_log(evemon_log_type_t type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* EVEMON_LOG_H */
