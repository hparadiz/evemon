#pragma once
#include <stdint.h>
#include <strings.h>
#include <stddef.h>
typedef struct { uint32_t codepoint; const char *color; } evemon_distro_logo_t;

static inline evemon_distro_logo_t evemon_distro_logo(const char *distro_id)
{
    /* Default: Tux */
    evemon_distro_logo_t fallback = { 0xf31a, "#333333" };
    if (!distro_id) return fallback;

    /* ── Distros (ID from /etc/os-release → codepoint, brand color) ── */
    static const struct { const char *id; uint32_t cp; const char *color; } map[] = {
        { "almalinux",             0xf31d, "#0f4266" },
        { "alpine",                0xf300, "#0d597f" },
        { "arch",                  0xf303, "#1793d1" },
        { "archlinux",             0xf303, "#1793d1" },
        { "archcraft",             0xf345, "#89b4fa" },
        { "archlabs",              0xf31e, "#1793d1" },
        { "arcolinux",             0xf346, "#6790eb" },
        { "artix",                 0xf31f, "#10a0cc" },
        { "biglinux",              0xf347, "#189cc4" },
        { "centos",                0xf304, "#932279" },
        { "coreos",                0xf305, "#3e6db5" },
        { "crystal",               0xf348, "#a900ff" },
        { "debian",                0xf306, "#d70a53" },
        { "deepin",                0xf321, "#2ca7f8" },
        { "devuan",                0xf307, "#333333" },
        { "elementary",            0xf309, "#4a90d9" },
        { "endeavouros",           0xf322, "#7b3db5" },
        { "fedora",                0xf30a, "#51a2da" },
        { "freebsd",               0xf30c, "#ab2b28" },
        { "garuda",                0xf337, "#e84d3d" },
        { "gentoo",                0xf30d, "#54487a" },
        { "hyperbola",             0xf33a, "#333333" },
        { "kali",                  0xf327, "#367bf0" },
        { "linuxmint",             0xf30e, "#87cf3e" },
        { "mint",                  0xf30e, "#87cf3e" },
        { "locos",                 0xf349, "#ed7f10" },
        { "lxle",                  0xf33e, "#474747" },
        { "mageia",                0xf310, "#2397d4" },
        { "mandriva",              0xf311, "#2b5f8c" },
        { "manjaro",               0xf312, "#35bf5c" },
        { "mxlinux",               0xf33f, "#333333" },
        { "nixos",                 0xf313, "#5277c3" },
        { "nobara",                0xf380, "#4c566a" },
        { "openbsd",               0xf328, "#f2ca30" },
        { "opensuse",              0xf314, "#73ba25" },
        { "opensuse-leap",         0xf37e, "#73ba25" },
        { "opensuse-tumbleweed",   0xf37d, "#73ba25" },
        { "suse",                  0xf314, "#73ba25" },
        { "parabola",              0xf340, "#333333" },
        { "parrot",                0xf329, "#16c3f3" },
        { "pop",                   0xf32a, "#48b9c7" },
        { "puppy",                 0xf341, "#9b59b6" },
        { "qubesos",               0xf342, "#3874d8" },
        { "raspberrypi",           0xf315, "#c51a4a" },
        { "redhat",                0xf316, "#ee0000" },
        { "rhel",                  0xf316, "#ee0000" },
        { "rocky",                 0xf32b, "#10b981" },
        { "rockylinux",            0xf32b, "#10b981" },
        { "sabayon",               0xf317, "#c6c6c6" },
        { "slackware",             0xf318, "#525252" },
        { "solus",                 0xf32d, "#5294e2" },
        { "steamos",               0xf382, "#66c0f4" },
        { "tails",                 0xf343, "#56347c" },
        { "trisquel",              0xf344, "#0060bf" },
        { "ubuntu",                0xf31b, "#e95420" },
        { "vanilla",               0xf366, "#333333" },
        { "void",                  0xf32e, "#478061" },
        { "voidlinux",             0xf32e, "#478061" },
        { "xerolinux",             0xf34a, "#2271b3" },
        { "zorin",                 0xf32f, "#15a6f0" },
        { NULL, 0, NULL }
    };

    for (int i = 0; map[i].id; i++) {
        if (strcasecmp(distro_id, map[i].id) == 0)
            return (evemon_distro_logo_t){ map[i].cp, map[i].color };
    }
    return fallback;
}

