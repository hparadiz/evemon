#ifndef EVEMON_DPKG_LOOKUP_H
#define EVEMON_DPKG_LOOKUP_H
/*
 * Toolkit-free dpkg/snap package lookup.  Lives in the core so that a
 * headless Evemon can answer DPKG requests for remote GUIs; the dpkg
 * plugin calls the same code directly when it runs on the monitored host.
 */
#include <glib.h>
#include <sys/types.h>

#define DPKG_DEP_SIZE   16384
#define DPKG_DESC_SIZE  8192

typedef struct {
    int  has_exe;                /* /proc/<pid>/exe could be read         */
    int  found;
    int  snap;
    int  essential;
    int  protected_pkg;
    int  automatic;              /* 1 = auto-installed dependency, 0 = manual, -1 = unknown */
    char name[128];
    char version[128];
    char arch[32];
    char source[192];
    char origin[512];
    char section[96];
    char priority[32];
    char maintainer[256];
    char original_maintainer[256];
    char homepage[512];
    char license[512];
    char multi_arch[32];
    char size_str[64];
    char installed_str[64];
    char summary[512];
    char description[DPKG_DESC_SIZE];
    char depends[DPKG_DEP_SIZE];
    /* snaps */
    char revision[32];
    char base[64];
    char grade[32];
    char confinement[32];
} dpkg_result_t;

typedef gboolean (*dpkg_cancel_fn)(void *user_data);

/* This machine has a populated dpkg database (Debian, Ubuntu, derivatives). */
int dpkg_available(void);

/* "Ubuntu", "Debian", "Linux Mint", ... from this machine's os-release. */
const char *dpkg_distro_label(void);

/*
 * Resolves the package (or snap) owning /proc/<pid>/exe on this machine.
 * Thread-safe.  Executable -> package ownership is cached until the dpkg
 * status database changes.  Returns FALSE only when cancelled.
 */
gboolean dpkg_lookup_pid(pid_t pid, dpkg_result_t *res,
                         dpkg_cancel_fn cancel, void *user_data);

#endif
