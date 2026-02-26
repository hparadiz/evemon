/*
 * art_loader.c – Async album art loader for evemon plugins.
 *
 * Handles three URI schemes:
 *   1. file://    – GIO local file read, then pixbuf from stream
 *   2. http(s):// – libsoup async HTTP GET, then pixbuf from stream
 *   3. data:image/...;base64,... – inline base64 decode, pixbuf from bytes
 *
 * All loading is fully async using GIO / libsoup / GdkPixbuf async APIs,
 * so the GTK main thread never blocks.
 *
 * Thread safety: art_load_async() must be called from the GTK main
 * thread.  The callback is always delivered on the GTK main thread.
 */

#include "art_loader.h"

/* from main.c */
extern int evemon_debug;

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <string.h>
#include <stdlib.h>

/* ── shared SoupSession (lazy init, reused across loads) ────── */

static SoupSession *shared_session = NULL;

static SoupSession *get_soup_session(void)
{
    if (!shared_session) {
        shared_session = soup_session_new_with_options(
            "timeout", 10,
            NULL);
    }
    return shared_session;
}

/* ── internal context for an in-flight load ─────────────────── */

typedef struct {
    art_loaded_cb   cb;
    void           *user_data;
    GCancellable   *cancel;
    GInputStream   *stream;   /* held during pixbuf decode */
} art_load_ctx_t;

static void art_ctx_free(art_load_ctx_t *ac)
{
    if (ac->stream)  g_object_unref(ac->stream);
    if (ac->cancel)  g_object_unref(ac->cancel);
    free(ac);
}

/* ── step 2: pixbuf from stream completed ───────────────────── */

static void on_pixbuf_ready(GObject *source, GAsyncResult *res,
                            gpointer data)
{
    (void)source;
    art_load_ctx_t *ac = data;

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_stream_finish(res, &err);
    if (!pb) {
        if (evemon_debug)
            fprintf(stderr, "[ART LOADER] pixbuf decode FAILED: %s\n",
                    err ? err->message : "(unknown)");
        g_clear_error(&err);
    } else {
        if (evemon_debug)
            fprintf(stderr, "[ART LOADER] pixbuf OK  %dx%d\n",
                    gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
    }

    ac->cb(pb, ac->user_data);
    art_ctx_free(ac);
}

/* ── HTTP send completed → start pixbuf decode ──────────────── */

static void on_soup_send_ready(GObject *source, GAsyncResult *res,
                               gpointer data)
{
    art_load_ctx_t *ac = data;
    SoupSession *session = SOUP_SESSION(source);

    GError *err = NULL;
    GInputStream *stream = soup_session_send_finish(session, res, &err);

    if (!stream) {
        if (evemon_debug)
            fprintf(stderr, "[ART LOADER] HTTP fetch FAILED: %s\n",
                err ? err->message : "(unknown)");
        g_clear_error(&err);
        ac->cb(NULL, ac->user_data);
        art_ctx_free(ac);
        return;
    }

    ac->stream = stream;

    gdk_pixbuf_new_from_stream_async(ac->stream, ac->cancel,
                                     on_pixbuf_ready, ac);
}

/* ── GFile read completed → start pixbuf decode ──────────────── */

static void on_file_read_ready(GObject *source, GAsyncResult *res,
                               gpointer data)
{
    art_load_ctx_t *ac = data;
    GFile *file = G_FILE(source);

    GError *err = NULL;
    GFileInputStream *fis = g_file_read_finish(file, res, &err);

    if (!fis) {
        if (evemon_debug)
            fprintf(stderr, "[ART LOADER] file read FAILED: %s\n",
                    err ? err->message : "(unknown)");
        g_clear_error(&err);
        ac->cb(NULL, ac->user_data);
        art_ctx_free(ac);
        g_object_unref(file);
        return;
    }

    ac->stream = G_INPUT_STREAM(fis);
    g_object_unref(file);

    gdk_pixbuf_new_from_stream_async(ac->stream, ac->cancel,
                                     on_pixbuf_ready, ac);
}

/* ── data: URI handler (base64 decode, synchronous but instant) ─ */

static GdkPixbuf *decode_data_uri(const char *url)
{
    /*
     * Format: data:[<mediatype>][;base64],<data>
     * We only handle base64-encoded image data.
     */
    const char *comma = strchr(url, ',');
    if (!comma) return NULL;

    /* Check for ;base64 before the comma */
    const char *header = url + 5;   /* skip "data:" */
    const char *b64_marker = strstr(header, ";base64");
    if (!b64_marker || b64_marker > comma) return NULL;

    const char *encoded = comma + 1;
    size_t enc_len = strlen(encoded);

    gsize out_len = 0;
    guchar *decoded = g_base64_decode(encoded, &out_len);
    if (!decoded || out_len == 0) {
        g_free(decoded);
        return NULL;
    }

    GInputStream *stream = g_memory_input_stream_new_from_data(
        decoded, (gssize)out_len, g_free);

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_stream(stream, NULL, &err);
    if (!pb) g_clear_error(&err);

    g_object_unref(stream);
    (void)enc_len;
    return pb;
}

/* ── public API ──────────────────────────────────────────────── */

void art_load_async(const char *url, art_loaded_cb cb, void *user_data,
                    GCancellable **cancel_out)
{
    if (!url || !url[0] || !cb) {
        if (cb) cb(NULL, user_data);
        return;
    }

    if (evemon_debug)
        fprintf(stderr, "[ART LOADER] url='%.120s'\n", url);

    /* ── data: URI → decode inline (fast, no I/O) ─────────── */
    if (strncmp(url, "data:", 5) == 0) {
        GdkPixbuf *pb = decode_data_uri(url);
        cb(pb, user_data);
        if (cancel_out) *cancel_out = NULL;
        return;
    }

    /* ── set up async load context ────────────────────────── */
    art_load_ctx_t *ac = calloc(1, sizeof(art_load_ctx_t));
    if (!ac) { cb(NULL, user_data); return; }

    ac->cb = cb;
    ac->user_data = user_data;
    ac->cancel = g_cancellable_new();

    if (cancel_out) {
        *cancel_out = ac->cancel;
        g_object_ref(ac->cancel);   /* caller gets their own ref */
    }

    /* ── http:// or https:// → libsoup async GET ──────────── */
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        SoupSession *session = get_soup_session();
        SoupMessage *msg = soup_message_new("GET", url);
        if (!msg) {
            if (evemon_debug)
                fprintf(stderr, "[ART LOADER] bad HTTP URL\n");
            ac->cb(NULL, ac->user_data);
            art_ctx_free(ac);
            return;
        }
        soup_session_send_async(session, msg, G_PRIORITY_LOW, ac->cancel,
                                on_soup_send_ready, ac);
        g_object_unref(msg);
        return;
    }

    /* ── file:// → GIO async file read ────────────────────── */
    GFile *file = g_file_new_for_uri(url);
    g_file_read_async(file, G_PRIORITY_LOW, ac->cancel,
                      on_file_read_ready, ac);
    /* file ownership: on_file_read_ready unrefs it */
}
