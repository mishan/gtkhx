/*
 * banner.c — server banner UI surface and fetch state machines.
 *
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This file owns:
 *   - The banner widget docked at the bottom of the toolbar
 *     window (a GtkPicture wrapped in a clickable area, with a
 *     dim "Server banner" caption that doubles as a fallback when
 *     the image can't be fetched / decoded).
 *   - The URL-mode fetch state machine: libsoup-3 async GET, then
 *     decode via gdk_pixbuf_new_from_stream, then set on the
 *     picture. Cancellable on banner_clear.
 *   - The HTXF-mode fetch will land in a follow-up commit; for
 *     now the file-mode banner shows the type-only caption and
 *     does NOT issue HTLC_HDR_BANNER_GET yet.
 *
 * Released under GPL-2.0-or-later. See COPYING.
 */

#include "config.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <gtk/gtk.h>
#ifdef HAVE_LIBSOUP
#include <libsoup/soup.h>
#endif
#include "compat.h"
#include "debug.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h" /* hl_htxf_hdr_pack */
#include "network.h"
#ifdef CONFIG_CIPHER
#include "cipher.h"
#include "cipher_aead.h"
#include "htxf_io.h"
#endif
#include "banner.h"

/* Inline forward decl so we don't have to pull in hx.h (which
 * brings session + a transitive zoo of UI deps). task_new's real
 * declaration lives in tasks.h. rcv_task_banner_get is declared
 * over in banner.h next to its file-mode partner so rcv.c also
 * sees it via -Wmissing-prototypes. */
struct task;
extern struct task *task_new (struct htlc_conn *htlc, rcv_task_fn rcv,
                              void *ptr, void *data, const char *str);

/* ------------------------------------------------------------------- *
 * Module state — single banner per process. The toolbar is the only
 * window that calls banner_widget_new(), so a handful of statics
 * suffice. */

/* Maximum displayed image dimensions. Most servers ship 468x60
 * Tracker-era banners; cap at 600x60 so unusually-large or
 * unusually-tall images don't blow out the toolbar layout. */
#define BANNER_MAX_W 600
#define BANNER_MAX_H 60

static GtkWidget *banner_root = NULL;    /* outer GtkBox */
static GtkWidget *banner_picture = NULL; /* GtkPicture */
static GtkWidget *banner_caption = NULL; /* GtkLabel */
#ifdef HAVE_LIBSOUP
static SoupSession *soup_session = NULL;
static GCancellable *fetch_cancel = NULL;
#endif
static char *current_url = NULL; /* for click-to-open */

/* HTXF fetch state — single in-flight banner fetch tracked
 * through a fetch-generation counter. The worker captures the
 * generation it was spawned with; the main-thread completion
 * idle compares to the current generation and drops its result
 * if the user already moved on (reconnect, different banner,
 * banner_clear). Avoids needing to thread a GCancellable through
 * blocking BSD socket reads. */
struct htxf_fetch {
    guint32 ref;
    guint32 size;
    guint32 generation;
    guint8 *bytes; /* malloc'd, filled by worker */
    gsize bytes_len;
    gboolean ok;
    /* Server endpoint snapshot — captured at spawn time so the
	 * worker doesn't race with a reconnect (which would mutate
	 * htlc->serverhost / serverport). serverport here is already
	 * the subchannel port (main + 1). */
    char serverhost[HOSTLEN];
    guint16 serverport;
#ifdef CONFIG_CIPHER
    /* HOPE+ChaCha20 AEAD state, snapshot at spawn time. When the
	 * control channel negotiated CIPHER_MODE_AEAD, the HTXF
	 * subchannel for the banner fetch needs the same per-transfer
	 * AEAD framing the regular file path (network.c::htxf_connect)
	 * sets up — the server seals every byte after the initial
	 * connect, including the 16-byte HTXF preamble. Production used
	 * to drive the worker with raw read()/write(), which worked
	 * only against servers that didn't use the AEAD subchannel
	 * (mhxd today); against Janus the JPEG body came through
	 * AEAD-framed and the magic-byte check downstream saw the
	 * 4-byte length prefix instead of "ffd8...".
	 *
	 * aead_active gates whether the worker builds a struct
	 * htxf_conn and uses htxf_io_read / htxf_io_write or falls
	 * back to the legacy raw path. */
    gboolean aead_active;
    chacha_aead_state ctrl_encode;
    chacha_aead_state ctrl_decode;
    uint8_t sessionkey[64];
    guint16 sklen;
#endif
};

static guint htxf_generation = 0;

/* ------------------------------------------------------------------- *
 * Forward declarations
 * ------------------------------------------------------------------- */

static void banner_show_caption (const char *text);
static void banner_show_pixbuf (GdkPixbuf *pb);
#ifdef HAVE_LIBSOUP
static void banner_start_url_fetch (const char *url);
static void on_soup_send_done (GObject *source, GAsyncResult *result,
                               gpointer user_data);
#endif
static void on_banner_clicked (GtkGestureClick *gesture, int n_press, double x,
                               double y, gpointer user_data);

/* HTXF (file-mode) fetch */
static void banner_send_download_request (struct htlc_conn *htlc);
static void *banner_htxf_worker_thread (void *arg);
static gboolean banner_htxf_completion_idle (gpointer data);

/* ------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------- */

GtkWidget *
banner_widget_new (void)
{
    if (banner_root) {
        return banner_root;
    }

    banner_root = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (banner_root, 6);
    gtk_widget_set_margin_end (banner_root, 6);
    gtk_widget_set_margin_top (banner_root, 4);
    gtk_widget_set_margin_bottom (banner_root, 4);
    gtk_widget_set_visible (banner_root, FALSE);

    /* GtkPicture, not GtkImage: GtkImage caps its natural size at
	 * the icon-size CSS variable (~16px), which would silently
	 * shrink any banner pixbuf. GtkPicture renders at the
	 * paintable's natural size and we control bounds via
	 * set_size_request.
	 *
	 * gtk_picture_set_keep_aspect_ratio is available on the 4.6
	 * floor; it was deprecated in 4.8 in favour of set_content_fit
	 * but the replacement isn't available pre-4.8 so we keep the
	 * older call and suppress the deprecation. */
    banner_picture = gtk_picture_new ();
    gtk_widget_set_size_request (banner_picture, -1, BANNER_MAX_H);
    gtk_picture_set_can_shrink (GTK_PICTURE (banner_picture), TRUE);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_picture_set_keep_aspect_ratio (GTK_PICTURE (banner_picture), TRUE);
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_widget_set_visible (banner_picture, FALSE);
    gtk_box_append (GTK_BOX (banner_root), banner_picture);

    banner_caption = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (banner_caption), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (banner_caption), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (banner_caption, TRUE);
    gtk_widget_add_css_class (banner_caption, "dim-label");
    gtk_box_append (GTK_BOX (banner_root), banner_caption);

    /* Click anywhere on the banner row → open the URL in the
	 * user's default browser. Tooltip mirrors the URL so a hover
	 * preview is enough to verify the destination. */
    {
        GtkGesture *click = gtk_gesture_click_new ();
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
                                       GDK_BUTTON_PRIMARY);
        g_signal_connect (click, "pressed", G_CALLBACK (on_banner_clicked),
                          NULL);
        gtk_widget_add_controller (banner_root, GTK_EVENT_CONTROLLER (click));
    }

    return banner_root;
}

void
banner_handle_message (struct htlc_conn *htlc, const char *type,
                       gboolean has_url, const char *url)
{
    (void)htlc;

    if (!banner_root) {
        return;
    }

    debug_log ("banner", "received: type='%s' has_url=%d url='%s'",
               type ? type : "(null)", has_url ? 1 : 0, url ? url : "");

    /* Drop any in-flight fetch from a previous banner. */
    banner_clear ();

    gtk_widget_set_visible (banner_root, TRUE);

    /* Dispatch on TYPE per the 1.9 spec, NOT on URL-chunk
	 * presence. mhxd in file mode still includes the URL chunk
	 * from its config (its banner-send logic doesn't gate the
	 * url field on type) — letting "any URL means URL mode" win
	 * would route us into URL mode even when the server is
	 * actually streaming bytes over HTXF. TYPE is authoritative:
	 *   "URL " (or just "URL") — URL mode
	 *   "GIFf" / "JPEG" / "PICT" / etc. — file mode (HTXF)
	 *
	 * Strip any trailing space when comparing — mhxd pads the
	 * type to 4 bytes with a space ("URL ", "URL\0", etc.). */
    gboolean is_url_mode = FALSE;
    if (type) {
        gchar trimmed[8];
        gsize n = 0;
        while (n < sizeof (trimmed) - 1 && type[n] && type[n] != ' ') {
            trimmed[n] = type[n], n++;
        }
        trimmed[n] = '\0';
        is_url_mode = (g_ascii_strcasecmp (trimmed, "URL") == 0);
    }

    if (is_url_mode) {
        if (has_url && url && *url) {
            /* URL-mode: cache the URL for click-to-open. With
			 * libsoup present we kick off an inline fetch and
			 * swap in the decoded image when it lands; without
			 * libsoup we leave the URL caption in place and
			 * rely on the click handler to open the URL. */
            g_free (current_url);
            current_url = g_strdup (url);
            gtk_widget_set_tooltip_text (banner_root, url);
            banner_show_caption (url);
#ifdef HAVE_LIBSOUP
            banner_start_url_fetch (url);
#endif
        } else {
            /* URL mode advertised but no URL chunk — server is
			 * misconfigured. Show a friendly caption and bail. */
            banner_show_caption (_ ("Server banner: URL mode without URL"));
        }
        return;
    }

    /* File-mode: TYPE is a binary image-format tag (GIFf, JPEG,
	 * PICT, ...). The server holds the banner bytes and expects
	 * HTLC_HDR_DOWNLOAD_BANNER (212 in the 1.9 spec), replies
	 * with HTLS_DATA_HTXF_REF + HTLS_DATA_HTXF_SIZE, and then we
	 * open the HTXF subchannel at server_port+1, send the
	 * 16-byte header, and read `size` bytes of image data.
	 * banner_handle_htxf_reply (called from rcv_task_banner_get)
	 * picks up after the reply parse.
	 *
	 * Show a "Loading..." caption in the meantime so the user
	 * sees feedback while the fetch is in flight. */
    {
        gchar *caption = g_strdup_printf (_ ("Server banner [%s] — loading..."),
                                          type ? type : "?");
        banner_show_caption (caption);
        g_free (caption);
    }
    banner_send_download_request (htlc);
}

void
banner_clear (void)
{
#ifdef HAVE_LIBSOUP
    if (fetch_cancel) {
        g_cancellable_cancel (fetch_cancel);
        g_clear_object (&fetch_cancel);
    }
#endif
    /* Bump the HTXF fetch generation so any in-flight file-mode
	 * worker'"'"'s completion idle (when it eventually fires) sees a
	 * mismatch and silently drops its result. */
    htxf_generation++;

    g_free (current_url);
    current_url = NULL;

    if (banner_picture) {
        gtk_picture_set_paintable (GTK_PICTURE (banner_picture), NULL);
        gtk_widget_set_visible (banner_picture, FALSE);
    }
    if (banner_caption) {
        gtk_label_set_text (GTK_LABEL (banner_caption), "");
    }
    if (banner_root) {
        gtk_widget_set_tooltip_text (banner_root, NULL);
        gtk_widget_set_visible (banner_root, FALSE);
    }
}

/* ------------------------------------------------------------------- *
 * Internals
 * ------------------------------------------------------------------- */

static void
banner_show_caption (const char *text)
{
    if (banner_caption) {
        gtk_label_set_text (GTK_LABEL (banner_caption), text ? text : "");
    }
}

static void
banner_show_pixbuf (GdkPixbuf *pb)
{
    GdkTexture *tex;
    int w, h;

    if (!banner_picture || !pb) {
        return;
    }

    w = gdk_pixbuf_get_width (pb);
    h = gdk_pixbuf_get_height (pb);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    tex = gdk_texture_new_for_pixbuf (pb);
    G_GNUC_END_IGNORE_DEPRECATIONS

    gtk_picture_set_paintable (GTK_PICTURE (banner_picture),
                               GDK_PAINTABLE (tex));
    g_object_unref (tex);

    /* Pin the picture's allocation so the layout doesn't reflow
	 * once the natural-size hint kicks in. Cap to BANNER_MAX_W /
	 * BANNER_MAX_H; aspect ratio preserved by CONTENT_FIT_CONTAIN. */
    {
        int cap_w = (w > BANNER_MAX_W) ? BANNER_MAX_W : w;
        int cap_h = (h > BANNER_MAX_H) ? BANNER_MAX_H : h;
        gtk_widget_set_size_request (banner_picture, cap_w, cap_h);
    }

    gtk_widget_set_visible (banner_picture, TRUE);
    /* Once the image is up the URL-as-caption is redundant; the
	 * tooltip and click-to-open carry that affordance. */
    banner_show_caption ("");
}

/* libsoup async fetch ----------------------------------------------- */

#ifdef HAVE_LIBSOUP
static SoupSession *
ensure_session (void)
{
    if (!soup_session) {
        soup_session = soup_session_new ();
        /* Sensible defaults; we don't need persistent cookies or
		 * other per-app session state for banner fetches. */
        soup_session_set_user_agent (soup_session, "GtkHx/Phase5 ");
        soup_session_set_timeout (soup_session, 10);
        soup_session_set_idle_timeout (soup_session, 20);
    }
    return soup_session;
}

static void
banner_start_url_fetch (const char *url)
{
    SoupMessage *msg;
    GUri *uri;
    GError *err = NULL;

    uri = g_uri_parse (url, G_URI_FLAGS_NONE, &err);
    if (!uri) {
        debug_log ("banner", "URL parse failed: %s", err ? err->message : "?");
        g_clear_error (&err);
        banner_show_caption (_ ("Server banner: URL is not parseable"));
        return;
    }
    msg = soup_message_new_from_uri ("GET", uri);
    g_uri_unref (uri);

    fetch_cancel = g_cancellable_new ();

    soup_session_send_and_read_async (ensure_session (), msg,
                                      G_PRIORITY_DEFAULT, fetch_cancel,
                                      on_soup_send_done, msg);
}

static void
on_soup_send_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    SoupSession *session = SOUP_SESSION (source);
    SoupMessage *msg = SOUP_MESSAGE (user_data);
    GError *err = NULL;
    GBytes *bytes;
    GdkPixbuf *pb;
    GInputStream *stream;
    guint status;

    bytes = soup_session_send_and_read_finish (session, result, &err);

    /* The cancellable was bound to this fetch; the next fetch
	 * (or banner_clear) will create a new one. Clear our handle
	 * so banner_clear's cancel-then-clear path doesn't try to
	 * cancel a finished operation. */
    g_clear_object (&fetch_cancel);

    if (!bytes) {
        debug_log ("banner", "soup fetch failed: %s", err ? err->message : "?");
        if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            banner_show_caption (_ ("Server banner: fetch failed"));
        }
        g_clear_error (&err);
        g_object_unref (msg);
        return;
    }

    status = soup_message_get_status (msg);
    if (status < 200 || status >= 300) {
        debug_log ("banner", "soup fetch http %u", status);
        banner_show_caption (_ ("Server banner: HTTP error"));
        g_bytes_unref (bytes);
        g_object_unref (msg);
        return;
    }

    stream = g_memory_input_stream_new_from_bytes (bytes);
    pb = gdk_pixbuf_new_from_stream (stream, NULL, &err);
    g_object_unref (stream);
    g_bytes_unref (bytes);

    if (!pb) {
        debug_log ("banner", "pixbuf decode failed: %s",
                   err ? err->message : "?");
        g_clear_error (&err);
        banner_show_caption (_ ("Server banner: image not decodable"));
        g_object_unref (msg);
        return;
    }

    banner_show_pixbuf (pb);
    g_object_unref (pb);
    g_object_unref (msg);
}
#endif /* HAVE_LIBSOUP — closes the #ifdef around banner_start_url_fetch */

/* ------------------------------------------------------------------- *
 * HTXF (file-mode) fetch
 *
 * 1. Client sends HTLC_HDR_DOWNLOAD_BANNER (no parameters); task
 *    callback is rcv_task_banner_get in rcv.c.
 * 2. Server replies with HTLS_DATA_HTXF_REF + HTLS_DATA_HTXF_SIZE.
 *    rcv_task_banner_get calls banner_handle_htxf_reply.
 * 3. banner_handle_htxf_reply spawns a detached pthread that
 *    opens base_port+1, sends the 16-byte HTXF header, reads
 *    `size` bytes into a buffer.
 * 4. Worker hands the buffer to the main thread via g_idle_add;
 *    main-thread idle decodes via gdk_pixbuf_new_from_stream
 *    and calls banner_show_pixbuf.
 *
 * Cancellation: banner_clear bumps htxf_generation. The worker
 * captured its own generation at spawn; the completion idle
 * compares it to current and silently drops the result if it
 * was bumped (reconnect, banner_clear, new banner). The worker
 * itself can'"'"'t be interrupted mid-read (POSIX blocking sockets,
 * no GIO involvement), but it always finishes within seconds —
 * banner data is small (a few KB to ~50 KB in practice). */

static void
banner_send_download_request (struct htlc_conn *htlc)
{
    if (!htlc) {
        return;
    }
    debug_log ("banner", "sending HTLC_HDR_DOWNLOAD_BANNER (file-mode fetch)");
    task_new (htlc, RCV_TASK_FN (rcv_task_banner_get), NULL, NULL,
              "banner_get");
    hlwrite (htlc, HTLC_HDR_DOWNLOAD_BANNER, 0, 0);
}

void
banner_handle_htxf_reply (struct htlc_conn *htlc, guint32 ref, guint32 size)
{
    struct htxf_fetch *f;
    pthread_t tid;
    pthread_attr_t attr;
    int err;

    if (!banner_root || !htlc) {
        debug_log ("banner", "htxf reply ignored: no widget / no htlc");
        return;
    }
    if (size == 0 || ref == 0) {
        debug_log ("banner", "htxf reply rejected: ref=%u size=%u", ref, size);
        banner_show_caption (_ ("Server banner: empty transfer"));
        return;
    }
    /* Sanity cap. Largest banner we'"'"'ve seen in the wild is ~30
	 * KB; legitimate banners stay below 100 KB. Anything above
	 * 1 MB is almost certainly a hostile or misconfigured server
	 * and we don'"'"'t want to allocate that much for a toolbar
	 * decoration. */
    if (size > 1024 * 1024) {
        debug_log ("banner", "htxf reply rejected: size %u > 1 MB", size);
        banner_show_caption (_ ("Server banner: image too large"));
        return;
    }

    f = g_new0 (struct htxf_fetch, 1);
    f->ref = ref;
    f->size = size;
    f->generation = ++htxf_generation;
    f->bytes = g_malloc (size);

    /* Snapshot the subchannel endpoint — same host as the main
	 * connection, port + 1. Stored as plain strings; the worker
	 * hands them straight to GSocketClient. */
    g_strlcpy (f->serverhost, htlc->serverhost, sizeof (f->serverhost));
    f->serverport = htlc->serverport + 1;

#ifdef CONFIG_CIPHER
    /* Snapshot the HOPE AEAD context so the worker can derive its
	 * per-transfer keys (cipher_aead_derive_transfer_keys mixes the
	 * ref in) and run htxf_io_read/_write the same way regular file
	 * transfers do. Only valid when the control channel actually
	 * negotiated CHACHA20-POLY1305 — leaves aead_active=FALSE
	 * otherwise and the worker keeps the legacy raw read/write
	 * path. */
    if (htlc->cipher_mode == CIPHER_MODE_AEAD) {
        f->aead_active = TRUE;
        f->ctrl_encode = htlc->cipher_encode_state.chacha;
        f->ctrl_decode = htlc->cipher_decode_state.chacha;
        memcpy (f->sessionkey, htlc->sessionkey, sizeof (htlc->sessionkey));
        f->sklen = htlc->sklen;
    }
#endif

    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
    err = pthread_create (&tid, &attr, banner_htxf_worker_thread, f);
    pthread_attr_destroy (&attr);

    if (err) {
        debug_log ("banner", "pthread_create failed: %s", g_strerror (err));
        g_free (f->bytes);
        g_free (f);
        banner_show_caption (_ ("Server banner: fetch worker failed to start"));
        return;
    }
    debug_log ("banner", "htxf worker spawned: ref=%u size=%u gen=%u", ref,
               size, f->generation);
}

static gboolean
read_n (int fd, void *buf, gsize len)
{
    guint8 *p = buf;
    while (len) {
        ssize_t r = read (fd, p, len);
        if (r <= 0) {
            return FALSE;
        }
        p += r;
        len -= r;
    }
    return TRUE;
}

static gboolean
write_n (int fd, const void *buf, gsize len)
{
    const guint8 *p = buf;
    while (len) {
        ssize_t w = write (fd, p, len);
        if (w <= 0) {
            return FALSE;
        }
        p += w;
        len -= w;
    }
    return TRUE;
}

static void *
banner_htxf_worker_thread (void *arg)
{
    struct htxf_fetch *f = arg;
    int s;
    guint8 hdr_buf[SIZEOF_HTXF_HDR];
    char errbuf[256] = { 0 };

    s = hx_sync_connect_to_host (f->serverhost, f->serverport, errbuf,
                                 sizeof (errbuf));
    if (s < 0) {
        debug_log ("banner", "htxf connect failed: %s", errbuf);
        goto out;
    }

    /* type=HTXF_TYPE_BANNER so Mac-native servers route this
	 * subchannel through their banner-send path rather than the
	 * generic single-file path. Shared packer in proto_helpers —
	 * see the htxf_connect comment in network.c for the full story
	 * on the type field. */
    hl_htxf_hdr_pack (hdr_buf, f->ref, f->size, HTXF_TYPE_BANNER, 0);

#ifdef CONFIG_CIPHER
    /* AEAD path: under HOPE+ChaCha20 every byte on the subchannel
	 * (including the 16-byte HTXF preamble) goes through Seal/
	 * Open. Build a transient struct htxf_conn that mirrors what
	 * network.c::htxf_connect would have set up for a regular
	 * transfer — populate the per-transfer keys via
	 * cipher_aead_derive_transfer_keys (mixing in the ref) and
	 * flip aead_active. htxf_io_write / htxf_io_read then do the
	 * right thing.
	 *
	 * Stack-allocate the htxf_conn since the worker owns it for
	 * its entire lifetime and we don't need it on a list. */
    if (f->aead_active) {
        struct htxf_conn xfer;
        memset (&xfer, 0, sizeof (xfer));
        xfer.ref = f->ref;
        htxf_io_init (&xfer);
        cipher_aead_derive_transfer_keys (
            &xfer.xfer_encode, &xfer.xfer_decode,
            f->sessionkey, f->sklen,
            &f->ctrl_encode, &f->ctrl_decode,
            f->ref);
        xfer.aead_active = TRUE;

        if (htxf_io_write (&xfer, s, hdr_buf, SIZEOF_HTXF_HDR)
            != (ssize_t) SIZEOF_HTXF_HDR) {
            debug_log ("banner", "htxf header write failed (AEAD): %s",
                       g_strerror (errno));
            htxf_io_release (&xfer);
            close (s);
            goto out;
        }

        guint8 *p = f->bytes;
        gsize remain = f->size;
        while (remain) {
            ssize_t r = htxf_io_read (&xfer, s, p, remain);
            if (r <= 0) {
                debug_log ("banner",
                           "htxf body read failed (AEAD) at < %zu bytes: %s",
                           (size_t) remain, g_strerror (errno));
                htxf_io_release (&xfer);
                close (s);
                goto out;
            }
            p += r;
            remain -= (gsize) r;
        }

        htxf_io_release (&xfer);
    } else
#endif
    {
        if (!write_n (s, hdr_buf, SIZEOF_HTXF_HDR)) {
            debug_log ("banner", "htxf header write failed: %s",
                       g_strerror (errno));
            close (s);
            goto out;
        }

        if (!read_n (s, f->bytes, f->size)) {
            debug_log ("banner",
                       "htxf body read failed at < %u bytes: %s",
                       f->size, g_strerror (errno));
            close (s);
            goto out;
        }
    }

    close (s);
    f->bytes_len = f->size;
    f->ok = TRUE;
    debug_log ("banner", "htxf worker fetched %u bytes (gen=%u)", f->size,
               f->generation);

out:
    /* Hand back to the main thread regardless of success — the
	 * idle decides whether to display or surface an error. */
    g_idle_add (banner_htxf_completion_idle, f);
    return NULL;
}

static gboolean
banner_htxf_completion_idle (gpointer data)
{
    struct htxf_fetch *f = data;
    GdkPixbuf *pb = NULL;
    GError *err = NULL;
    GInputStream *stream;

    /* Drop stale completions (the user moved on while we were
	 * reading the bytes). */
    if (f->generation != htxf_generation) {
        debug_log ("banner", "htxf completion dropped (gen %u != current %u)",
                   f->generation, htxf_generation);
        goto cleanup;
    }

    if (!f->ok || !f->bytes_len) {
        banner_show_caption (_ ("Server banner: HTXF fetch failed"));
        goto cleanup;
    }

    stream = g_memory_input_stream_new_from_data (f->bytes, f->bytes_len, NULL);
    pb = gdk_pixbuf_new_from_stream (stream, NULL, &err);
    g_object_unref (stream);
    if (!pb) {
        debug_log ("banner", "htxf pixbuf decode failed: %s",
                   err ? err->message : "?");
        g_clear_error (&err);
        banner_show_caption (_ ("Server banner: image not decodable"));
        goto cleanup;
    }

    banner_show_pixbuf (pb);
    g_object_unref (pb);

cleanup:
    g_free (f->bytes);
    g_free (f);
    return G_SOURCE_REMOVE;
}

/* Click handler ------------------------------------------------------ */

static void
on_banner_clicked (GtkGestureClick *gesture, int n_press, double x, double y,
                   gpointer user_data)
{
    GtkWidget *anchor;
    GtkUriLauncher *launcher;
    GtkWindow *parent;
    (void)n_press;
    (void)x;
    (void)y;
    (void)user_data;

    if (!current_url || !*current_url) {
        return;
    }

    anchor = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    parent = GTK_WINDOW (gtk_widget_get_root (anchor));

    launcher = gtk_uri_launcher_new (current_url);
    gtk_uri_launcher_launch (launcher, parent, NULL, NULL, NULL);
    g_object_unref (launcher);
}
