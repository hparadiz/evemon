/*
 * Distro glyph info: a Unicode codepoint from font-logos + brand color.
 */
typedef struct {
    gunichar codepoint;     /* Unicode codepoint in font-logos     */
    const char *color;      /* hex color string e.g. "#1793d1"     */
} distro_glyph_t;

/* ── Animated glyph rendering ────────────────────────────────── */

/*
 * We rasterize the font-logos glyph once into an alpha mask, then on
 * every animation frame we sweep an animated gradient across it using
 * the distro's brand color.  The gradient goes:
 *   brand color → bright highlight → brand color
 * sweeping diagonally to create a shimmering effect.
 */

#define GLYPH_ANIM_FPS     30
#define GLYPH_ANIM_MS      (1000 / GLYPH_ANIM_FPS)
#define GLYPH_ANIM_PERIOD  4.0   /* seconds for one full cycle */

typedef struct {
    uint8_t *alpha;          /* alpha mask (pw × ph, row-major)     */
    int      pw, ph;         /* pixel dimensions of the glyph       */
    uint8_t  r0, g0, b0;    /* brand color RGB                     */
    double   phase;          /* animation phase 0..1                */
    guint    timer_id;       /* g_timeout source id                 */
    GtkWidget *da;           /* the GtkDrawingArea we draw into     */
} glyph_anim_t;

static void glyph_anim_free(glyph_anim_t *ga)
{
    if (!ga) return;
    if (ga->timer_id) {
        g_source_remove(ga->timer_id);
        ga->timer_id = 0;
    }
    g_free(ga->alpha);
    g_free(ga);
}

static gboolean glyph_anim_tick(gpointer data)
{
    glyph_anim_t *ga = data;
    ga->phase += (double)GLYPH_ANIM_MS / (GLYPH_ANIM_PERIOD * 1000.0);
    if (ga->phase >= 1.0) ga->phase -= 1.0;
    gtk_widget_queue_draw(ga->da);
    return G_SOURCE_CONTINUE;
}

static gboolean glyph_anim_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)widget;
    glyph_anim_t *ga = data;
    int pw = ga->pw, ph = ga->ph;

    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, pw, ph);
    unsigned char *pixels = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    double phase = ga->phase;
    double r0 = ga->r0, g0 = ga->g0, b0 = ga->b0;

    /* Brighter highlight color: blend brand → white at 70% */
    double rH = r0 + 0.70 * (255.0 - r0);
    double gH = g0 + 0.70 * (255.0 - g0);
    double bH = b0 + 0.70 * (255.0 - b0);

    /* Diagonal dimension for normalization */
    double diag = (double)(pw + ph);

    for (int y = 0; y < ph; y++) {
        uint32_t *row = (uint32_t *)(pixels + y * stride);
        const uint8_t *arow = ga->alpha + y * pw;
        for (int x = 0; x < pw; x++) {
            uint8_t a = arow[x];
            if (a == 0) { row[x] = 0; continue; }

            /* Diagonal position normalized to 0..1 */
            double pos = (double)(x + y) / diag;

            /* Shimmer: a Gaussian-ish bright band sweeping diagonally.
             * 'center' is where the bright peak is right now.  */
            double center = phase;
            /* Distance on the ring (wraps around) */
            double d = pos - center;
            if (d > 0.5)  d -= 1.0;
            if (d < -0.5) d += 1.0;
            /* Gaussian-like falloff, width ~0.15 of the diagonal */
            double t = exp(-d * d / (2.0 * 0.02));

            /* Interpolate brand ↔ highlight */
            uint8_t r = (uint8_t)(r0 + t * (rH - r0));
            uint8_t g = (uint8_t)(g0 + t * (gH - g0));
            uint8_t b = (uint8_t)(b0 + t * (bH - b0));

            /* Pre-multiplied alpha for Cairo ARGB32 */
            r = (uint8_t)(r * a / 255);
            g = (uint8_t)(g * a / 255);
            b = (uint8_t)(b * a / 255);
            row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8)  | (uint32_t)b;
        }
    }
    cairo_surface_mark_dirty(surf);

    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(surf);

    return FALSE;
}

static void on_glyph_dialog_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    glyph_anim_free((glyph_anim_t *)data);
}

/*
 * Rasterize the font-logos glyph into an alpha mask and return a
 * GtkDrawingArea that animates a gradient shimmer across it.
 * The caller must connect the dialog's "destroy" signal to
 * on_glyph_dialog_destroy() with the returned glyph_anim_t.
 */
static GtkWidget *create_animated_glyph(gunichar codepoint,
                                        const char *hex_color,
                                        glyph_anim_t **out_ga)
{
    /* Encode the codepoint as UTF-8 */
    char utf8[8];
    int ulen = g_unichar_to_utf8(codepoint, utf8);
    utf8[ulen] = '\0';

    /* Measure the glyph with a throwaway Cairo context */
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *tmp_cr = cairo_create(tmp);
    PangoLayout *layout = pango_cairo_create_layout(tmp_cr);
    PangoFontDescription *fd = pango_font_description_from_string("font-logos 150");
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, utf8, -1);

    int pw, ph;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    cairo_destroy(tmp_cr);
    cairo_surface_destroy(tmp);

    /* Render white-on-transparent to extract alpha */
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    pango_cairo_show_layout(cr, layout);
    cairo_destroy(cr);

    pango_font_description_free(fd);
    g_object_unref(layout);

    /* Extract the alpha channel into a separate buffer */
    cairo_surface_flush(surf);
    unsigned char *sdata = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    uint8_t *alpha = g_malloc(pw * ph);
    for (int y = 0; y < ph; y++) {
        const uint32_t *row = (const uint32_t *)(sdata + y * stride);
        for (int x = 0; x < pw; x++)
            alpha[y * pw + x] = (row[x] >> 24) & 0xFF;
    }
    cairo_surface_destroy(surf);

    /* Parse brand color */
    unsigned int rc, gc, bc;
    if (sscanf(hex_color, "#%02x%02x%02x", &rc, &gc, &bc) != 3)
        rc = gc = bc = 0x33;

    /* Build animation state */
    glyph_anim_t *ga = g_new0(glyph_anim_t, 1);
    ga->alpha = alpha;
    ga->pw    = pw;
    ga->ph    = ph;
    ga->r0    = (uint8_t)rc;
    ga->g0    = (uint8_t)gc;
    ga->b0    = (uint8_t)bc;
    ga->phase = 0.0;

    /* Create a drawing area sized to the glyph */
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, pw, ph);
    g_signal_connect(da, "draw", G_CALLBACK(glyph_anim_draw), ga);
    ga->da = da;

    /* Start the animation timer */
    ga->timer_id = g_timeout_add(GLYPH_ANIM_MS, glyph_anim_tick, ga);

    *out_ga = ga;
    return da;
}

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
        "/org/evemon/font-logos.ttf", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (!bytes) {
        fprintf(stderr, "evemon: font-logos.ttf not found in GResource\n");
        failed = TRUE;
        return FALSE;
    }

    /* Write to a temp file (fontconfig needs a filesystem path).
     * Use .ttf suffix so fontconfig recognises the format. */
    snprintf(g_font_logos_tmp, sizeof(g_font_logos_tmp),
             "/tmp/evemon-font-logos-XXXXXX.ttf");
    int fd = mkstemps(g_font_logos_tmp, 4);
    if (fd < 0) {
        fprintf(stderr, "evemon: mkstemp failed for font-logos\n");
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
        fprintf(stderr, "evemon: failed to write font-logos temp file\n");
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
        fprintf(stderr, "evemon: FcConfigAppFontAddFile failed\n");
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

/*
 * After the splash window is realized, set _MOTIF_WM_HINTS on the
 * underlying X11 window to forcibly disable server-side decorations.
 * KDE/KWin respects this even when it ignores gtk_window_set_decorated().
 */
static void splash_force_no_decorations(GtkWidget *widget,
                                        gpointer   user_data)
{
    (void)user_data;
#ifdef GDK_WINDOWING_X11
    GdkWindow *gdkwin = gtk_widget_get_window(widget);
    if (!gdkwin) return;
    GdkDisplay *display = gdk_window_get_display(gdkwin);
    if (!GDK_IS_X11_DISPLAY(display)) return;

    Display *xdpy = GDK_DISPLAY_XDISPLAY(display);
    Window   xwin = GDK_WINDOW_XID(gdkwin);

    /* Motif WM hints: flags=2 (MWM_HINTS_DECORATIONS), decorations=0 */
    typedef struct { unsigned long flags, functions, decorations, input_mode;
                     unsigned long status; } MwmHints;
    MwmHints hints = { 2, 0, 0, 0, 0 };   /* flags=MWM_HINTS_DECORATIONS */

    Atom motif_atom = XInternAtom(xdpy, "_MOTIF_WM_HINTS", False);
    XChangeProperty(xdpy, xwin, motif_atom, motif_atom, 32,
                    PropModeReplace, (unsigned char *)&hints, 5);
#else
    (void)widget;
#endif
}

/*
 * Expand a zsh PS1 prompt string into plain text.
 * Strips all %{...%} literal escape wrappers and expands common
 * prompt escapes (%n, %m, %M, %~, %d, %/, %%, %#, %D, %T, %t, %w,
 * %W, %l, %?, %j, %i).  Color/attribute sequences (%F, %f, %K, %k,
 * %B, %b, %U, %u, %S, %s) are silently consumed.
 * Returns a g_malloc'd string (caller frees with g_free).
 */
static char *expand_zsh_ps1(const char *ps1,
                            const char *user,
                            const char *hostname)
{
    if (!ps1 || !ps1[0])
        return NULL;

    /* Short hostname (up to first dot) */
    char short_host[256];
    snprintf(short_host, sizeof(short_host), "%s", hostname);
    { char *dot = strchr(short_host, '.'); if (dot) *dot = '\0'; }

    GString *out = g_string_new(NULL);
    const char *p = ps1;

    while (*p) {
        if (*p == '%' && *(p + 1) == '{') {
            /* %{...%} — literal escape wrapper, skip everything inside */
            p += 2;
            while (*p) {
                if (*p == '%' && *(p + 1) == '}') { p += 2; break; }
                p++;
            }
            continue;
        }

        if (*p != '%') {
            g_string_append_c(out, *p);
            p++;
            continue;
        }

        /* We have a % escape */
        p++;  /* skip the '%' */
        if (!*p) break;

        switch (*p) {
        /* ── Text sequences that produce visible output ─────── */
        case 'n':   /* username */
            g_string_append(out, user);
            p++;
            break;
        case 'm':   /* short hostname */
            g_string_append(out, short_host);
            p++;
            break;
        case 'M':   /* full hostname */
            g_string_append(out, hostname);
            p++;
            break;
        case '~':   /* cwd with ~ abbreviation */
        case 'd':   /* cwd */
        case '/':   /* cwd */
            {
                const char *home = g_get_home_dir();
                char cwd[PATH_MAX] = "";
                if (getcwd(cwd, sizeof(cwd)) == NULL)
                    snprintf(cwd, sizeof(cwd), "~");
                if (*p == '~' && home && strncmp(cwd, home, strlen(home)) == 0) {
                    g_string_append_c(out, '~');
                    g_string_append(out, cwd + strlen(home));
                } else {
                    g_string_append(out, cwd);
                }
            }
            p++;
            break;
        case '%':   /* literal % */
            g_string_append_c(out, '%');
            p++;
            break;
        case '#':   /* # if root, %% if not */
            g_string_append_c(out, (getuid() == 0) ? '#' : '%');
            p++;
            break;
        case 'l':   /* tty basename */
            {
                const char *tty = ttyname(STDIN_FILENO);
                if (tty) {
                    const char *base = strrchr(tty, '/');
                    g_string_append(out, base ? base + 1 : tty);
                }
            }
            p++;
            break;

        /* ── Color / attribute sequences — consume silently ──── */
        case 'F':   /* foreground: %F{color} or %F alone */
        case 'K':   /* background: %K{color} or %K alone */
            p++;
            if (*p == '{') {
                p++;
                while (*p && *p != '}') p++;
                if (*p == '}') p++;
            }
            break;
        case 'f':   /* reset foreground */
        case 'k':   /* reset background */
        case 'B':   /* bold on */
        case 'b':   /* bold off */
        case 'U':   /* underline on */
        case 'u':   /* underline off */
        case 'S':   /* standout on */
        case 's':   /* standout off */
            p++;
            break;

        /* ── Conditional expressions %( — skip to matching %) ── */
        case '(':
            {
                int depth = 1;
                p++;
                while (*p && depth > 0) {
                    if (*p == '%' && *(p + 1) == '(') { depth++; p += 2; }
                    else if (*p == '%' && *(p + 1) == ')') { depth--; p += 2; }
                    else p++;
                }
            }
            break;

        /* ── Numeric argument prefix (e.g. %2~) — skip digits ─ */
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            /* Skip digits then re-process the following escape char */
            while (*p >= '0' && *p <= '9') p++;
            /* The next char is the actual escape (e.g. ~ in %2~);
             * don't advance p, let the loop re-process it as %<char>.
             * We need to prepend a fake '%' — simplest: just handle
             * the common case of %N~ directly. */
            if (*p == '~' || *p == 'd' || *p == '/') {
                const char *home = g_get_home_dir();
                char cwd[PATH_MAX] = "";
                if (getcwd(cwd, sizeof(cwd)) == NULL)
                    snprintf(cwd, sizeof(cwd), "~");
                if (*p == '~' && home && strncmp(cwd, home, strlen(home)) == 0) {
                    g_string_append_c(out, '~');
                    g_string_append(out, cwd + strlen(home));
                } else {
                    g_string_append(out, cwd);
                }
                p++;
            } else {
                /* Unknown numeric-prefixed escape, skip the char */
                if (*p) p++;
            }
            break;

        default:
            /* Unknown escape — skip it silently */
            p++;
            break;
        }
    }

    /* Trim trailing whitespace / newlines */
    while (out->len > 0 && (out->str[out->len - 1] == ' ' ||
                            out->str[out->len - 1] == '\n' ||
                            out->str[out->len - 1] == '\r'))
        g_string_truncate(out, out->len - 1);

    /* If the expansion is empty, return NULL to fall back to default */
    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }

    return g_string_free(out, FALSE);
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

    /* Username – prefer the original (pre-sudo) user */
    const char *user = g_getenv("SUDO_USER");
    if (!user || !user[0])
        user = g_get_user_name();
    if (!user) user = "unknown";

    /* Resolve the original user's home directory.
     * Under sudo, g_get_home_dir() returns /root, but we want the
     * invoking user's home so we can read their shell config.      */
    char real_home[PATH_MAX] = "";
    {
        const char *sudo_user = g_getenv("SUDO_USER");
        if (sudo_user && sudo_user[0]) {
            /* Look up the user's home from /etc/passwd */
            struct passwd *pw = getpwnam(sudo_user);
            if (pw && pw->pw_dir)
                snprintf(real_home, sizeof(real_home), "%s", pw->pw_dir);
        }
        if (!real_home[0]) {
            const char *h = g_get_home_dir();
            if (h) snprintf(real_home, sizeof(real_home), "%s", h);
        }
    }

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

    /* Shell – resolve the original user's login shell, not root's */
    const char *shell = NULL;
    char shell_buf[PATH_MAX] = "";
    {
        const char *sudo_user = g_getenv("SUDO_USER");
        if (sudo_user && sudo_user[0]) {
            struct passwd *pw = getpwnam(sudo_user);
            if (pw && pw->pw_shell && pw->pw_shell[0]) {
                snprintf(shell_buf, sizeof(shell_buf), "%s", pw->pw_shell);
                shell = shell_buf;
            }
        }
        if (!shell) {
            shell = g_getenv("SHELL");
            if (!shell) shell = "/bin/sh";
        }
    }
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

    /* ── Create splash-style window (no titlebar) ─────────────── */
    GtkWidget *dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(window));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_window_set_wmclass(GTK_WINDOW(dlg), "evemon", "evemon");
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(dlg), GDK_WINDOW_TYPE_HINT_SPLASHSCREEN);

    /* Force CSD mode with an invisible header bar.  On KDE Wayland,
     * set_decorated(FALSE) alone doesn't prevent KWin from adding
     * its own server-side title bar.  Setting an empty GtkHeaderBar
     * as the titlebar tells GTK to use client-side decorations,
     * which in turn tells KWin "this window draws its own chrome."
     * We hide the header bar so it takes zero vertical space.       */
    {
        GtkWidget *fake_hdr = gtk_header_bar_new();
        gtk_widget_set_no_show_all(fake_hdr, TRUE);
        gtk_widget_hide(fake_hdr);
        gtk_window_set_titlebar(GTK_WINDOW(dlg), fake_hdr);
    }

    /* Override WM_CLASS so compositors that apply title-bar rules
     * based on class name treat this as a true splash, not a
     * regular "evemon" window that should get decorations.         */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_window_set_wmclass(GTK_WINDOW(dlg), "evemon_splash", "evemon_splash");
    G_GNUC_END_IGNORE_DEPRECATIONS

    /* Force-strip SSD on X11: set _MOTIF_WM_HINTS with decorations=0.
     * KDE/KWin on X11 sometimes ignores gtk_window_set_decorated(FALSE)
     * and the SPLASHSCREEN type hint, but respects _MOTIF_WM_HINTS.  */
    g_signal_connect(dlg, "realize", G_CALLBACK(splash_force_no_decorations), NULL);

    /* Rounded-corner frame + border via CSS */
    {
        char splash_css_str[256];
        snprintf(splash_css_str, sizeof(splash_css_str),
                 "window { border-radius: 12px;"
                 " border: 1px solid %s;"
                 " padding: 4px; }",
                 glyph.color);
        GtkCssProvider *splash_css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(splash_css,
            splash_css_str, -1, NULL);
        GtkStyleContext *sc = gtk_widget_get_style_context(dlg);
        gtk_style_context_add_provider(sc,
            GTK_STYLE_PROVIDER(splash_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(splash_css);
    }

    /* Wrap everything in an event box so clicks anywhere dismiss it */
    GtkWidget *evbox = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(dlg), evbox);

    /* ── Horizontal layout: icon | info ─────────────────────────── */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_margin_start(hbox, 32);
    gtk_widget_set_margin_end(hbox, 32);
    gtk_widget_set_margin_top(hbox, 24);
    gtk_widget_set_margin_bottom(hbox, 16);

    /* ── Distro icon (animated gradient glyph) ─────────────────── */
    glyph_anim_t *glyph_anim = NULL;
    {
        GtkWidget *icon_da = create_animated_glyph(
            glyph.codepoint, glyph.color, &glyph_anim);
        gtk_widget_set_valign(icon_da, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(icon_da, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(hbox), icon_da, FALSE, FALSE, 0);
    }

    /* ── Info text ──────────────────────────────────────────────── */
    {
        GString *info = g_string_new(NULL);

        /*
         * If the user's shell is zsh, try to expand their PS1 prompt
         * (stripping color/formatting codes) for the header line.
         * Otherwise fall back to the plain user@hostname.
         */
        char *prompt_header = NULL;
        if (strcmp(shell_base, "zsh") == 0) {
            /*
             * The user's prompt may be set by a framework like
             * oh-my-zsh (theme functions, powerline segments, etc.),
             * so a simple file parse won't work.  Instead, spawn
             * zsh as the original user in interactive mode, let it
             * source all configs/themes, then print the expanded
             * prompt.  Strip ANSI escape sequences and Powerline
             * separator glyphs from the output.
             *
             * Under sudo we use "sudo -u <user>" to run as the
             * original invoker; without sudo we run zsh directly.
             */
            char cmd[512];
            const char *sudo_user = g_getenv("SUDO_USER");
            if (sudo_user && sudo_user[0])
                snprintf(cmd, sizeof(cmd),
                         "sudo -u %s zsh -ic 'print -Pn \"$PROMPT\"'"
                         " 2>/dev/null", sudo_user);
            else
                snprintf(cmd, sizeof(cmd),
                         "zsh -ic 'print -Pn \"$PROMPT\"'"
                         " 2>/dev/null");

            FILE *pp = popen(cmd, "r");
            if (pp) {
                char raw[2048] = "";
                size_t total = 0;
                size_t n;
                while ((n = fread(raw + total, 1,
                                  sizeof(raw) - 1 - total, pp)) > 0)
                    total += n;
                raw[total] = '\0';
                pclose(pp);

                if (total > 0) {
                    /* Strip ANSI escape sequences (\e[...m etc.) and
                     * Powerline separator glyphs (U+E0B0–U+E0B3).
                     * Keep only printable UTF-8 text.                */
                    GString *clean = g_string_new(NULL);
                    const char *r = raw;
                    while (*r) {
                        /* Skip ANSI CSI sequences: ESC [ ... final_byte */
                        if (r[0] == '\033' && r[1] == '[') {
                            r += 2;
                            while (*r && !(*r >= 0x40 && *r <= 0x7E)) r++;
                            if (*r) r++;
                            continue;
                        }
                        /* Skip other ESC sequences (OSC, etc.) */
                        if (r[0] == '\033') {
                            r++;
                            if (*r) r++;
                            continue;
                        }
                        /* Decode a UTF-8 character and check for
                         * Powerline glyphs (U+E0A0–U+E0D4) */
                        gunichar uc = g_utf8_get_char_validated(r, -1);
                        if (uc == (gunichar)-1 || uc == (gunichar)-2) {
                            r++;
                            continue;
                        }
                        const char *next = g_utf8_next_char(r);
                        /* Skip Powerline Private Use Area glyphs */
                        if (uc >= 0xE0A0 && uc <= 0xE0D4) {
                            r = next;
                            continue;
                        }
                        /* Skip control characters */
                        if (g_unichar_iscntrl(uc) && uc != ' ') {
                            r = next;
                            continue;
                        }
                        /* Keep this character */
                        g_string_append_len(clean, r, (gssize)(next - r));
                        r = next;
                    }

                    /* Collapse runs of spaces, trim edges */
                    {
                        GString *trimmed = g_string_new(NULL);
                        const char *s = clean->str;
                        gboolean prev_space = TRUE;
                        while (*s) {
                            if (*s == ' ') {
                                if (!prev_space)
                                    g_string_append_c(trimmed, ' ');
                                prev_space = TRUE;
                            } else {
                                g_string_append_c(trimmed, *s);
                                prev_space = FALSE;
                            }
                            s++;
                        }
                        /* Trim trailing space */
                        while (trimmed->len > 0 &&
                               trimmed->str[trimmed->len - 1] == ' ')
                            g_string_truncate(trimmed, trimmed->len - 1);
                        /* Trim leading space */
                        if (trimmed->len > 0 && trimmed->str[0] == ' ')
                            g_string_erase(trimmed, 0, 1);
                        g_string_free(clean, TRUE);
                        clean = trimmed;
                    }

                    if (clean->len > 0) {
                        /* The cleaned prompt may contain multiple
                         * segments (user@host, cwd, git branch, etc.).
                         * Extract only the part containing '@' — that
                         * is the user@host context segment.           */
                        char *at = strstr(clean->str, "@");
                        if (at) {
                            /* Walk backwards from '@' to find the start
                             * of this segment (after a space or at the
                             * beginning of the string). */
                            const char *seg_start = clean->str;
                            for (const char *p = at - 1; p >= clean->str; ) {
                                gunichar uc = g_utf8_get_char(p);
                                if (uc == ' ') {
                                    seg_start = g_utf8_next_char(p);
                                    break;
                                }
                                if (p == clean->str) break;
                                p = g_utf8_prev_char(p);
                            }
                            /* Walk forwards from '@' to find the end
                             * (next space or end of string). */
                            const char *seg_end = at + 1;
                            while (*seg_end && *seg_end != ' ')
                                seg_end = g_utf8_next_char(seg_end);
                            prompt_header = g_strndup(seg_start,
                                (gsize)(seg_end - seg_start));
                            g_string_free(clean, TRUE);
                        } else {
                            prompt_header = g_string_free(clean, FALSE);
                        }
                    } else {
                        g_string_free(clean, TRUE);
                    }
                }
            }
        }

        if (prompt_header) {
            /* Use the expanded PS1 as the header (all in brand color) */
            char *hdr_esc = g_markup_escape_text(prompt_header, -1);
            g_string_append_printf(info,
                "<span font_family='Monospace' size='12000'"
                " foreground='%s' weight='bold'>%s</span>\n",
                glyph.color, hdr_esc);

            /* separator */
            int sep_len = (int)g_utf8_strlen(prompt_header, -1);
            g_string_append(info,
                "<span font_family='Monospace' size='10000'>");
            for (int i = 0; i < sep_len; i++)
                g_string_append_c(info, '-');
            g_string_append(info, "</span>\n");

            g_free(hdr_esc);
            g_free(prompt_header);
        } else {
            /* Fallback: user@host header (bold + colored) */
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

            g_free(user_esc); g_free(host_esc);
        }

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

        g_free(dn_esc); g_free(kern_esc); g_free(up_esc);
        g_free(sh_esc); g_free(de_esc); g_free(cpu_esc);
        g_free(disp_esc); g_free(hdr_esc); g_free(gpu_esc);

        /* Blank line + evemon branding */
        g_string_append(info,
            "\n<span font_family='Monospace' size='10000'"
            " weight='bold'>evemon</span>"
            "<span font_family='Monospace' size='10000'>"
            " 0.1.0</span>\n"
            "<span font_family='Monospace' size='9000'"
            " style='italic'>A lightweight Linux everything monitor"
            "</span>\n");

        GtkWidget *info_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(info_label), info->str);
        gtk_label_set_xalign(GTK_LABEL(info_label), 0.0f);
        gtk_widget_set_valign(info_label, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(hbox), info_label, TRUE, TRUE, 0);

        g_string_free(info, TRUE);
    }

    gtk_container_add(GTK_CONTAINER(evbox), hbox);

    /* Dismiss on click anywhere */
    g_signal_connect_swapped(evbox, "button-press-event",
                             G_CALLBACK(gtk_widget_destroy), dlg);

    /* Dismiss on any key press */
    g_signal_connect_swapped(dlg, "key-press-event",
                             G_CALLBACK(gtk_widget_destroy), dlg);

    /* Dismiss on focus loss */
    g_signal_connect_swapped(dlg, "focus-out-event",
                             G_CALLBACK(gtk_widget_destroy), dlg);

    /* Clean up animation on destroy */
    g_signal_connect(dlg, "destroy",
                     G_CALLBACK(on_glyph_dialog_destroy), glyph_anim);

    gtk_widget_show_all(dlg);

    free(distro_id);
    free(distro_name);
}
