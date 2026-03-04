/*
 * log.c – centralised logging implementation.
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>

int evemon_debug       = 0;
int evemon_debug_audio = 0;

void evemon_log(evemon_log_type_t type, const char *fmt, ...)
{
    switch (type) {
    case LOG_DEBUG:
        if (!evemon_debug)       return;
        break;
    case LOG_AUDIO:
        if (!evemon_debug_audio) return;
        break;
    case LOG_INFO:
    case LOG_ERROR:
        break;
    }

    FILE *out = (type == LOG_ERROR) ? stderr : stdout;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fputc('\n', out);
    fflush(out);
}
