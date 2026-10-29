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
#include <stdint.h>
#include "../platform/desktop_session.h"

/* from main.c */
#include "../core/log.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define ART_MAX_BYTES      (16U * 1024U * 1024U)
#define ART_READ_CHUNK     (64U * 1024U)
#define ART_MAX_DIMENSION  4096

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
    GByteArray     *bytes;    /* bounded encoded image buffer */
    SoupMessage    *message;  /* HTTP status/headers during send */
} art_load_ctx_t;

static void art_ctx_free(art_load_ctx_t *ac)
{
    if (ac->stream)  g_object_unref(ac->stream);
    if (ac->cancel)  g_object_unref(ac->cancel);
    if (ac->bytes)   g_byte_array_unref(ac->bytes);
    if (ac->message) g_object_unref(ac->message);
    free(ac);
}

static void art_load_failed(art_load_ctx_t *ac, GError *err)
{
    if (evemon_debug && err)
        fprintf(stderr, "[ART LOADER] load FAILED: %s\n", err->message);
    g_clear_error(&err);
    ac->cb(NULL, ac->user_data);
    art_ctx_free(ac);
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

/* Read every source through a bounded encoded-byte buffer before handing it
 * to an image decoder.  MPRIS metadata is controlled by another process; a
 * direct stream would otherwise allow an unbounded HTTP or pseudo-file body. */
static void art_read_next(art_load_ctx_t *ac);

static void art_begin_decode(art_load_ctx_t *ac)
{
    GBytes *bytes = g_byte_array_free_to_bytes(ac->bytes);
    ac->bytes = NULL;

    g_clear_object(&ac->stream);
    ac->stream = g_memory_input_stream_new_from_bytes(bytes);
    g_bytes_unref(bytes);

    gdk_pixbuf_new_from_stream_at_scale_async(
        ac->stream, ART_MAX_DIMENSION, ART_MAX_DIMENSION, TRUE,
        ac->cancel, on_pixbuf_ready, ac);
}

static void on_stream_read_ready(GObject *source, GAsyncResult *res,
                                 gpointer data)
{
    art_load_ctx_t *ac = data;
    GError *err = NULL;
    GBytes *chunk = g_input_stream_read_bytes_finish(
        G_INPUT_STREAM(source), res, &err);
    if (!chunk) {
        art_load_failed(ac, err);
        return;
    }

    gsize length = 0;
    const guint8 *contents = g_bytes_get_data(chunk, &length);
    if (length == 0) {
        g_bytes_unref(chunk);
        if (ac->bytes->len == 0) {
            art_load_failed(ac, NULL);
            return;
        }
        art_begin_decode(ac);
        return;
    }
    if (length > ART_MAX_BYTES - ac->bytes->len) {
        g_bytes_unref(chunk);
        art_load_failed(ac, NULL);
        return;
    }
    g_byte_array_append(ac->bytes, contents, (guint)length);
    g_bytes_unref(chunk);
    art_read_next(ac);
}

static void art_read_next(art_load_ctx_t *ac)
{
    g_input_stream_read_bytes_async(ac->stream, ART_READ_CHUNK,
                                    G_PRIORITY_LOW, ac->cancel,
                                    on_stream_read_ready, ac);
}

static int art_start_bounded_read(art_load_ctx_t *ac)
{
    ac->bytes = g_byte_array_sized_new(ART_READ_CHUNK);
    if (!ac->bytes)
        return 0;
    art_read_next(ac);
    return 1;
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

    guint status = soup_message_get_status(ac->message);
    if (status < 200 || status >= 300) {
        g_object_unref(stream);
        art_load_failed(ac, NULL);
        return;
    }

    ac->stream = stream;
    if (!art_start_bounded_read(ac))
        art_load_failed(ac, NULL);
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

    GFileInfo *info = g_file_input_stream_query_info(
        fis,
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_UNIX_UID ","
        G_FILE_ATTRIBUTE_UNIX_MODE,
        ac->cancel, &err);
    guint32 owner = info
        ? g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID)
        : (guint32)-1;
    guint32 mode = info
        ? g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE)
        : 0;
    goffset size = info ? g_file_info_get_size(info) : -1;
    GFileType type = info ? g_file_info_get_file_type(info)
                          : G_FILE_TYPE_UNKNOWN;
    uid_t session_uid = desktop_session_uid();
    int trusted = info && type == G_FILE_TYPE_REGULAR && size > 0 &&
        (uint64_t)size <= ART_MAX_BYTES &&
        (owner == session_uid ||
         (owner == 0 && !(mode & (S_IWGRP | S_IWOTH))));
    if (info) g_object_unref(info);
    if (!trusted) {
        g_object_unref(fis);
        g_object_unref(file);
        art_load_failed(ac, err);
        return;
    }
    g_clear_error(&err);

    ac->stream = G_INPUT_STREAM(fis);
    g_object_unref(file);
    if (!art_start_bounded_read(ac))
        art_load_failed(ac, NULL);
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
    if (enc_len > (size_t)ART_MAX_BYTES * 4 / 3 + 4)
        return NULL;

    gsize out_len = 0;
    guchar *decoded = g_base64_decode(encoded, &out_len);
    if (!decoded || out_len == 0 || out_len > ART_MAX_BYTES) {
        g_free(decoded);
        return NULL;
    }

    GInputStream *stream = g_memory_input_stream_new_from_data(
        decoded, (gssize)out_len, g_free);

    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_stream_at_scale(
        stream, ART_MAX_DIMENSION, ART_MAX_DIMENSION, TRUE, NULL, &err);
    if (!pb) g_clear_error(&err);

    g_object_unref(stream);
    (void)enc_len;
    return pb;
}

/* ── public API ──────────────────────────────────────────────── */

void art_load_async(const char *url, art_loaded_cb cb, void *user_data,
                    GCancellable **cancel_out)
{
    if (cancel_out) *cancel_out = NULL;
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
        ac->message = g_object_ref(msg);
        soup_session_send_async(session, msg, G_PRIORITY_LOW, ac->cancel,
                                on_soup_send_ready, ac);
        g_object_unref(msg);
        return;
    }

    /* ── file:// → validated local regular file ───────────── */
    if (strncmp(url, "file://", 7) != 0) {
        ac->cb(NULL, ac->user_data);
        art_ctx_free(ac);
        return;
    }
    char *filename = g_filename_from_uri(url, NULL, NULL);
    struct stat st;
    uid_t session_uid = desktop_session_uid();
    int trusted_file = filename && lstat(filename, &st) == 0 &&
        S_ISREG(st.st_mode) && st.st_size > 0 &&
        (uint64_t)st.st_size <= ART_MAX_BYTES &&
        (st.st_uid == session_uid ||
         (st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH))));
    g_free(filename);
    if (!trusted_file) {
        ac->cb(NULL, ac->user_data);
        art_ctx_free(ac);
        return;
    }
    GFile *file = g_file_new_for_uri(url);
    g_file_read_async(file, G_PRIORITY_LOW, ac->cancel,
                      on_file_read_ready, ac);
    /* file ownership: on_file_read_ready unrefs it */
}
