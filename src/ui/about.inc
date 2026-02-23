/*
 * Distro glyph info: a Unicode codepoint from font-logos + brand color.
 */
typedef struct {
    gunichar codepoint;     /* Unicode codepoint in font-logos     */
    const char *color;      /* hex color string e.g. "#1793d1"     */
} distro_glyph_t;

static distro_glyph_t get_distro_glyph(const char *distro_id)
{
    /* Default: Tux */
    distro_glyph_t fallback = { 0xf31a, "#333333" };
    if (!distro_id) return fallback;

    /* ── Distros (ID from /etc/os-release → codepoint, brand color) ── */
    static const struct { const char *id; gunichar cp; const char *color; } map[] = {
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
            return (distro_glyph_t){ map[i].cp, map[i].color };
    }
    return fallback;
}

/*
 * Load the embedded font-logos.ttf from GResource and register it
 * with fontconfig so Pango/GTK can render the glyphs.  Called once.
 *
 * Strategy: extract the GResource bytes to a temp file, then use
 * FcConfigAppFontAddFile() to make "font-logos" available.
 * The temp file is deleted on exit via atexit().
 */
static char g_font_logos_tmp[PATH_MAX];

static void cleanup_font_logos_tmp(void)
{
    if (g_font_logos_tmp[0])
        unlink(g_font_logos_tmp);
}

static gboolean ensure_font_logos_loaded(void)
{
    static gboolean loaded = FALSE;
    static gboolean failed = FALSE;
    if (loaded) return TRUE;
    if (failed) return FALSE;

    GBytes *bytes = g_resources_lookup_data(
        "/org/allmon/font-logos.ttf", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (!bytes) {
        fprintf(stderr, "allmon: font-logos.ttf not found in GResource\n");
        failed = TRUE;
        return FALSE;
    }

    /* Write to a temp file (fontconfig needs a filesystem path).
     * Use .ttf suffix so fontconfig recognises the format. */
    snprintf(g_font_logos_tmp, sizeof(g_font_logos_tmp),
             "/tmp/allmon-font-logos-XXXXXX.ttf");
    int fd = mkstemps(g_font_logos_tmp, 4);
    if (fd < 0) {
        fprintf(stderr, "allmon: mkstemp failed for font-logos\n");
        g_bytes_unref(bytes);
        failed = TRUE;
        return FALSE;
    }
    atexit(cleanup_font_logos_tmp);

    gsize sz = 0;
    const void *fdata = g_bytes_get_data(bytes, &sz);
    ssize_t written = write(fd, fdata, sz);
    close(fd);
    g_bytes_unref(bytes);

    if (written != (ssize_t)sz) {
        fprintf(stderr, "allmon: failed to write font-logos temp file\n");
        failed = TRUE;
        return FALSE;
    }

    /* Ensure fontconfig is initialised before adding app fonts */
    FcConfig *fc_cfg = FcInitLoadConfigAndFonts();
    if (!fc_cfg)
        fc_cfg = FcConfigGetCurrent();

    /* Register with fontconfig */
    FcBool ok = FcConfigAppFontAddFile(
        fc_cfg, (const FcChar8 *)g_font_logos_tmp);
    if (!ok) {
        fprintf(stderr, "allmon: FcConfigAppFontAddFile failed\n");
        failed = TRUE;
        return FALSE;
    }

    /* Make sure this config is the active one and force Pango to
     * pick up the newly added font by resetting its font map. */
    FcConfigSetCurrent(fc_cfg);
    pango_cairo_font_map_set_default(NULL);

    loaded = TRUE;
    return TRUE;
}

static void on_menu_about(GtkMenuItem *item, gpointer data)
{
    (void)item;
    GtkWidget *window = data;

    /* ── Load the font-logos font ─────────────────────────────── */
    ensure_font_logos_loaded();

    /* ── Gather system information ──────────────────────────────── */
    char *distro_id   = read_os_release_field("ID");
    char *distro_name = read_os_release_field("PRETTY_NAME");
    if (!distro_name)
        distro_name = read_os_release_field("NAME");
    if (!distro_name)
        distro_name = strdup("Linux");

    distro_glyph_t glyph = get_distro_glyph(distro_id);

    /* Hostname */
    char hostname[256] = "unknown";
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';

    /* Username */
    const char *user = g_get_user_name();
    if (!user) user = "unknown";

    /* Kernel */
    char kernel[128] = "unknown";
    {
        FILE *f = fopen("/proc/version", "r");
        if (f) {
            if (fgets(kernel, sizeof(kernel), f)) {
                char *p = strchr(kernel, ' ');
                if (p) {
                    p++;
                    char *q = strchr(p, ' ');
                    if (q) {
                        q++;
                        char *r = strchr(q, ' ');
                        if (r) *r = '\0';
                        memmove(kernel, q, strlen(q) + 1);
                    }
                }
            }
            fclose(f);
        }
    }

    /* Uptime */
    char uptime_str[128] = "unknown";
    {
        double up = 0;
        FILE *f = fopen("/proc/uptime", "r");
        if (f) { int r_ = fscanf(f, "%lf", &up); (void)r_; fclose(f); }
        int days  = (int)(up / 86400);
        int hours = (int)((long)up % 86400) / 3600;
        int mins  = (int)((long)up % 3600) / 60;
        if (days > 0)
            snprintf(uptime_str, sizeof(uptime_str), "%dd %dh %dm",
                     days, hours, mins);
        else if (hours > 0)
            snprintf(uptime_str, sizeof(uptime_str), "%dh %dm", hours, mins);
        else
            snprintf(uptime_str, sizeof(uptime_str), "%dm", mins);
    }

    /* CPU model */
    char cpu_model[256] = "unknown";
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "model name", 10) == 0) {
                    char *colon = strchr(line, ':');
                    if (colon) {
                        colon++;
                        while (*colon == ' ') colon++;
                        size_t len = strlen(colon);
                        while (len > 0 && (colon[len - 1] == '\n' ||
                                           colon[len - 1] == '\r'))
                            colon[--len] = '\0';
                        snprintf(cpu_model, sizeof(cpu_model), "%s", colon);
                    }
                    break;
                }
            }
            fclose(f);
        }
    }

    /* CPU count */
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 1;

    /* Memory */
    long mem_total_kb = 0, mem_avail_kb = 0;
    {
        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                sscanf(line, "MemTotal: %ld kB", &mem_total_kb);
                sscanf(line, "MemAvailable: %ld kB", &mem_avail_kb);
            }
            fclose(f);
        }
    }
    long mem_used_kb = mem_total_kb - mem_avail_kb;
    if (mem_used_kb < 0) mem_used_kb = 0;
    char mem_used_buf[64], mem_total_buf[64];
    format_memory(mem_used_kb, mem_used_buf, sizeof(mem_used_buf));
    format_memory(mem_total_kb, mem_total_buf, sizeof(mem_total_buf));

    /* Shell */
    const char *shell = g_getenv("SHELL");
    if (!shell) shell = "/bin/sh";
    const char *shell_base = strrchr(shell, '/');
    shell_base = shell_base ? shell_base + 1 : shell;

    /* Desktop environment */
    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    if (!desktop || !desktop[0])
        desktop = g_getenv("DESKTOP_SESSION");
    if (!desktop || !desktop[0])
        desktop = "unknown";

    /* Display resolution, refresh rate, and HDR */
    char display_str[512] = "unknown";
    {
        GdkDisplay *gdkdisp = gdk_display_get_default();
        if (gdkdisp) {
            int n_monitors = gdk_display_get_n_monitors(gdkdisp);
            if (n_monitors > 0) {
                /* Collect info for each monitor */
                typedef struct { int w, h, rate_mhz; } mon_info_t;
                mon_info_t *infos = g_new0(mon_info_t, n_monitors);
                for (int i = 0; i < n_monitors; i++) {
                    GdkMonitor *m = gdk_display_get_monitor(gdkdisp, i);
                    if (!m) continue;
                    GdkRectangle geom;
                    gdk_monitor_get_geometry(m, &geom);
                    int scale = gdk_monitor_get_scale_factor(m);
                    infos[i].w = geom.width * scale;
                    infos[i].h = geom.height * scale;
                    infos[i].rate_mhz = gdk_monitor_get_refresh_rate(m);
                }

                /* Check if all monitors are identical */
                gboolean all_same = TRUE;
                for (int i = 1; i < n_monitors; i++) {
                    if (infos[i].w != infos[0].w ||
                        infos[i].h != infos[0].h ||
                        infos[i].rate_mhz != infos[0].rate_mhz) {
                        all_same = FALSE;
                        break;
                    }
                }

                if (n_monitors == 1 || all_same) {
                    /* Single monitor or all identical: show once */
                    double hz = infos[0].rate_mhz / 1000.0;
                    int off = 0;
                    off += snprintf(display_str + off,
                                    sizeof(display_str) - (size_t)off,
                                    "%dx%d", infos[0].w, infos[0].h);
                    if (infos[0].rate_mhz > 0)
                        off += snprintf(display_str + off,
                                        sizeof(display_str) - (size_t)off,
                                        " @ %.0f Hz", hz);
                    if (n_monitors > 1)
                        snprintf(display_str + off,
                                 sizeof(display_str) - (size_t)off,
                                 " (%d displays)", n_monitors);
                } else {
                    /* Different monitors: list each one */
                    int off = 0;
                    for (int i = 0; i < n_monitors && off < (int)sizeof(display_str) - 1; i++) {
                        if (i > 0)
                            off += snprintf(display_str + off,
                                            sizeof(display_str) - (size_t)off,
                                            ", ");
                        double hz = infos[i].rate_mhz / 1000.0;
                        off += snprintf(display_str + off,
                                        sizeof(display_str) - (size_t)off,
                                        "%dx%d", infos[i].w, infos[i].h);
                        if (infos[i].rate_mhz > 0)
                            off += snprintf(display_str + off,
                                            sizeof(display_str) - (size_t)off,
                                            " @ %.0f Hz", hz);
                    }
                }
                g_free(infos);
            }
        }
    }

    /* HDR support: check DRM connector properties */
    const char *hdr_status = "No";
    {
        GDir *drm_dir = g_dir_open("/sys/class/drm", 0, NULL);
        if (drm_dir) {
            const gchar *entry;
            while ((entry = g_dir_read_name(drm_dir)) != NULL) {
                /* Look for card*-* connector dirs (e.g. card1-DP-1) */
                if (!g_str_has_prefix(entry, "card") || !strchr(entry, '-'))
                    continue;
                char hdr_path[512];
                snprintf(hdr_path, sizeof(hdr_path),
                         "/sys/class/drm/%s/hdr_output_metadata", entry);
                if (g_file_test(hdr_path, G_FILE_TEST_EXISTS)) {
                    hdr_status = "Supported";
                    break;
                }
                /* Also check the "HDR sink metadata" via edid or status */
                snprintf(hdr_path, sizeof(hdr_path),
                         "/sys/class/drm/%s/status", entry);
                char status_buf[32] = "";
                FILE *sf = fopen(hdr_path, "r");
                if (sf) {
                    int r_ = fscanf(sf, "%31s", status_buf); (void)r_;
                    fclose(sf);
                }
                if (strcmp(status_buf, "connected") != 0)
                    continue;
                /* Try to read hdr_output_metadata presence */
                snprintf(hdr_path, sizeof(hdr_path),
                         "/sys/class/drm/%s/hdr_output_metadata", entry);
                if (g_file_test(hdr_path, G_FILE_TEST_EXISTS)) {
                    hdr_status = "Supported";
                    break;
                }
            }
            g_dir_close(drm_dir);
        }
    }

    /* GPU model(s) */
    char gpu_str[512] = "unknown";
    {
        GString *gpus = g_string_new(NULL);
        GDir *drm_dir = g_dir_open("/sys/class/drm", 0, NULL);
        if (drm_dir) {
            const gchar *entry;
            while ((entry = g_dir_read_name(drm_dir)) != NULL) {
                /* Match card0, card1, etc. (no dash → not a connector) */
                if (!g_str_has_prefix(entry, "card"))
                    continue;
                if (strchr(entry, '-'))
                    continue; /* skip card0-DP-1 etc. */

                /* Read PCI slot name */
                char uevent_path[512];
                snprintf(uevent_path, sizeof(uevent_path),
                         "/sys/class/drm/%s/device/uevent", entry);
                char pci_slot[64] = "";
                FILE *uf = fopen(uevent_path, "r");
                if (uf) {
                    char line[256];
                    while (fgets(line, sizeof(line), uf)) {
                        if (strncmp(line, "PCI_SLOT_NAME=", 14) == 0) {
                            char *val = line + 14;
                            size_t vlen = strlen(val);
                            while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r'))
                                val[--vlen] = '\0';
                            snprintf(pci_slot, sizeof(pci_slot), "%s", val);
                            break;
                        }
                    }
                    fclose(uf);
                }

                /* Use lspci to get human-readable name */
                char gpu_name[256] = "";
                if (pci_slot[0]) {
                    char cmd[128];
                    snprintf(cmd, sizeof(cmd), "lspci -s %s", pci_slot);
                    FILE *pp = popen(cmd, "r");
                    if (pp) {
                        char buf[512];
                        if (fgets(buf, sizeof(buf), pp)) {
                            /* Format: "0e:00.0 VGA compatible controller: AMD ..." */
                            char *colon = strchr(buf, ':');
                            if (colon) {
                                colon++; /* skip type colon */
                                colon = strchr(colon, ':');
                                if (colon) {
                                    colon++;
                                    while (*colon == ' ') colon++;
                                    size_t len = strlen(colon);
                                    while (len > 0 && (colon[len-1] == '\n' || colon[len-1] == '\r'))
                                        colon[--len] = '\0';
                                    /* Strip "(rev XX)" suffix */
                                    char *rev = strstr(colon, " (rev ");
                                    if (rev) *rev = '\0';
                                    /* Strip verbose vendor prefix before [...] shorthand */
                                    /* e.g. "Advanced Micro Devices, Inc. [AMD/ATI] Navi 21 ..."
                                     *    → "AMD Navi 21 ..." */
                                    char *bracket = strchr(colon, '[');
                                    if (bracket && bracket > colon) {
                                        char *close = strchr(bracket, ']');
                                        if (close) {
                                            /* Extract short vendor from [AMD/ATI] → "AMD" */
                                            char short_vendor[64] = "";
                                            size_t blen = (size_t)(close - bracket - 1);
                                            if (blen > 0 && blen < sizeof(short_vendor)) {
                                                memcpy(short_vendor, bracket + 1, blen);
                                                short_vendor[blen] = '\0';
                                                /* Use text before '/' if present (AMD/ATI → AMD) */
                                                char *slash = strchr(short_vendor, '/');
                                                if (slash) *slash = '\0';
                                            }
                                            /* Rest after "] " */
                                            char *rest = close + 1;
                                            while (*rest == ' ') rest++;
                                            if (short_vendor[0] && rest[0])
                                                snprintf(gpu_name, sizeof(gpu_name),
                                                         "%s %s", short_vendor, rest);
                                            else if (rest[0])
                                                snprintf(gpu_name, sizeof(gpu_name), "%s", rest);
                                            else
                                                snprintf(gpu_name, sizeof(gpu_name), "%s", colon);
                                        } else {
                                            snprintf(gpu_name, sizeof(gpu_name), "%s", colon);
                                        }
                                    } else {
                                        snprintf(gpu_name, sizeof(gpu_name), "%s", colon);
                                    }
                                }
                            }
                        }
                        pclose(pp);
                    }
                }

                /* Read VRAM if available */
                long long vram_bytes = 0;
                {
                    /* AMD: mem_info_vram_total (bytes) */
                    char vram_path[512];
                    snprintf(vram_path, sizeof(vram_path),
                             "/sys/class/drm/%s/device/mem_info_vram_total", entry);
                    FILE *vf = fopen(vram_path, "r");
                    if (vf) {
                        int r_ = fscanf(vf, "%lld", &vram_bytes); (void)r_;
                        fclose(vf);
                    }

                    /* NVIDIA: try /proc/driver/nvidia/gpus/<pci_slot>/information */
                    if (vram_bytes == 0 && pci_slot[0]) {
                        char nv_path[512];
                        snprintf(nv_path, sizeof(nv_path),
                                 "/proc/driver/nvidia/gpus/%s/information", pci_slot);
                        FILE *nf = fopen(nv_path, "r");
                        if (nf) {
                            char line[256];
                            while (fgets(line, sizeof(line), nf)) {
                                /* "Video BIOS:   xx.xx.xx.xx.xx" — skip */
                                /* "Bus Type:     PCIe" — skip */
                                /* "DMA Size:     47 bits" — skip */
                                /* "GPU UUID:     GPU-..." — skip */
                                /* Look for nothing here, but we confirm NVIDIA */
                            }
                            fclose(nf);
                        }
                        /* nvidia-smi for VRAM in MiB */
                        if (vram_bytes == 0) {
                            char cmd[256];
                            snprintf(cmd, sizeof(cmd),
                                     "nvidia-smi --id=%s"
                                     " --query-gpu=memory.total"
                                     " --format=csv,noheader,nounits 2>/dev/null",
                                     pci_slot);
                            FILE *nsp = popen(cmd, "r");
                            if (nsp) {
                                long long mib = 0;
                                if (fscanf(nsp, "%lld", &mib) == 1 && mib > 0)
                                    vram_bytes = mib * 1024LL * 1024LL;
                                pclose(nsp);
                            }
                        }
                    }

                    /* Intel: try device/lmem_total_bytes (DG1/DG2/Arc) */
                    if (vram_bytes == 0) {
                        char intel_path[512];
                        snprintf(intel_path, sizeof(intel_path),
                                 "/sys/class/drm/%s/device/lmem_total_bytes",
                                 entry);
                        FILE *inf = fopen(intel_path, "r");
                        if (inf) {
                            int r_ = fscanf(inf, "%lld", &vram_bytes); (void)r_;
                            fclose(inf);
                        }
                    }
                }

                /* Append to result */
                if (gpu_name[0]) {
                    if (gpus->len > 0)
                        g_string_append(gpus, ", ");
                    g_string_append(gpus, gpu_name);
                    if (vram_bytes > 0) {
                        double vram_gib = vram_bytes / (1024.0 * 1024.0 * 1024.0);
                        g_string_append_printf(gpus, " (%.0f GiB)", vram_gib);
                    }
                }
            }
            g_dir_close(drm_dir);
        }
        if (gpus->len > 0)
            snprintf(gpu_str, sizeof(gpu_str), "%s", gpus->str);
        g_string_free(gpus, TRUE);
    }

    /* ── Create the dialog ──────────────────────────────────────── */
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "About allmon",
        GTK_WINDOW(window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 680, 380);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));

    /* ── Horizontal layout: icon | info ─────────────────────────── */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_margin_start(hbox, 32);
    gtk_widget_set_margin_end(hbox, 32);
    gtk_widget_set_margin_top(hbox, 24);
    gtk_widget_set_margin_bottom(hbox, 16);

    /* ── Distro icon (big font-logos glyph) ─────────────────────── */
    {
        /* Encode the codepoint as UTF-8 */
        char utf8_glyph[8];
        int len = g_unichar_to_utf8(glyph.codepoint, utf8_glyph);
        utf8_glyph[len] = '\0';

        /* Pango markup: large, colored, font-logos family */
        char markup[512];
        snprintf(markup, sizeof(markup),
                 "<span font_family='font-logos' size='200000'"
                 " foreground='%s'>%s</span>",
                 glyph.color, utf8_glyph);

        GtkWidget *icon_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(icon_label), markup);
        gtk_widget_set_valign(icon_label, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(icon_label, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(hbox), icon_label, FALSE, FALSE, 0);
    }

    /* ── Info text ──────────────────────────────────────────────── */
    {
        GString *info = g_string_new(NULL);

        /* user@host header (bold + colored) */
        char *user_esc = g_markup_escape_text(user, -1);
        char *host_esc = g_markup_escape_text(hostname, -1);
        g_string_append_printf(info,
            "<span font_family='Monospace' size='12000'"
            " foreground='%s' weight='bold'>%s</span>"
            "<span font_family='Monospace' size='12000'>@</span>"
            "<span font_family='Monospace' size='12000'"
            " foreground='%s' weight='bold'>%s</span>\n",
            glyph.color, user_esc, glyph.color, host_esc);

        /* separator */
        int sep_len = (int)(strlen(user) + 1 + strlen(hostname));
        g_string_append(info,
            "<span font_family='Monospace' size='10000'>");
        for (int i = 0; i < sep_len; i++)
            g_string_append_c(info, '-');
        g_string_append(info, "</span>\n");

        /* Key-value info lines — escape values for Pango markup */
        char *dn_esc   = g_markup_escape_text(distro_name, -1);
        char *kern_esc = g_markup_escape_text(kernel, -1);
        char *up_esc   = g_markup_escape_text(uptime_str, -1);
        char *sh_esc   = g_markup_escape_text(shell_base, -1);
        char *de_esc   = g_markup_escape_text(desktop, -1);
        char *cpu_esc  = g_markup_escape_text(cpu_model, -1);
        char *disp_esc = g_markup_escape_text(display_str, -1);
        char *hdr_esc  = g_markup_escape_text(hdr_status, -1);
        char *gpu_esc  = g_markup_escape_text(gpu_str, -1);

        #define INFO_LINE(key, valfmt, ...) \
            g_string_append_printf(info, \
                "<span font_family='Monospace' size='10000'" \
                " foreground='%s' weight='bold'>" key "</span>" \
                "<span font_family='Monospace' size='10000'> " valfmt "</span>\n", \
                glyph.color, __VA_ARGS__)

        INFO_LINE("OS:",         "%s", dn_esc);
        INFO_LINE("Kernel:",     "%s", kern_esc);
        INFO_LINE("Uptime:",     "%s", up_esc);
        INFO_LINE("Shell:",      "%s", sh_esc);
        INFO_LINE("DE:",         "%s", de_esc);
        INFO_LINE("Resolution:", "%s", disp_esc);
        INFO_LINE("HDR:",        "%s", hdr_esc);
        INFO_LINE("GPU:",        "%s", gpu_esc);
        INFO_LINE("CPU:",        "%s (%ld)", cpu_esc, ncpus);
        INFO_LINE("Memory:",     "%s / %s", mem_used_buf, mem_total_buf);
        #undef INFO_LINE

        g_free(user_esc); g_free(host_esc);
        g_free(dn_esc); g_free(kern_esc); g_free(up_esc);
        g_free(sh_esc); g_free(de_esc); g_free(cpu_esc);
        g_free(disp_esc); g_free(hdr_esc); g_free(gpu_esc);

        /* Blank line + allmon branding */
        g_string_append(info,
            "\n<span font_family='Monospace' size='10000'"
            " weight='bold'>allmon</span>"
            "<span font_family='Monospace' size='10000'>"
            " 0.1.0</span>\n"
            "<span font_family='Monospace' size='9000'"
            " style='italic'>A lightweight Linux process monitor"
            "</span>\n");

        GtkWidget *info_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(info_label), info->str);
        gtk_label_set_selectable(GTK_LABEL(info_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(info_label), 0.0f);
        gtk_widget_set_valign(info_label, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(hbox), info_label, TRUE, TRUE, 0);

        g_string_free(info, TRUE);
    }

    gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);

    /* Non-blocking: let Ctrl+C and other signals through */
    g_signal_connect(dlg, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect(dlg, "destroy",  G_CALLBACK(gtk_widget_destroyed), &dlg);
    gtk_widget_show_all(dlg);

    free(distro_id);
    free(distro_name);
}
