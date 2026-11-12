#ifndef EVEMON_NVTOP_TABLE_HELPERS_H
#define EVEMON_NVTOP_TABLE_HELPERS_H

/* The pinned nvtop backend tables use only ARRAY_SIZE and NULL from this
 * terminal-UI header. Keep those helpers independent of ncurses. */
#include <stddef.h>
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#endif
