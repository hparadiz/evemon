/* GdkPixbuf-to-Cairo conversion for the original overlay, without GDK. */
static void gdk_cairo_set_source_pixbuf(cairo_t *cr, const GdkPixbuf *pixbuf, double x, double y)
{
    int width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    unsigned char *out = cairo_image_surface_get_data(surface);
    int out_stride = cairo_image_surface_get_stride(surface);
    for (int row = 0; row < height; ++row) {
        uint32_t *dst = (uint32_t *)(out + row * out_stride);
        for (int col = 0; col < width; ++col) {
            const guchar *p = pixels + row * stride + col * channels;
            unsigned a = channels == 4 ? p[3] : 255;
            dst[col] = (a << 24) | ((p[0] * a / 255) << 16) | ((p[1] * a / 255) << 8) | (p[2] * a / 255);
        }
    }
    cairo_surface_mark_dirty(surface);
    cairo_set_source_surface(cr, surface, x, y);
    cairo_surface_destroy(surface);
}
