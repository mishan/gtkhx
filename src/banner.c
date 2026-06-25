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
 *     async decode via the glycin loader (the same sandboxed
 *     subprocess pipeline the inline-media path uses), then
 *     `gtk_picture_set_paintable` with the resulting
 *     `GdkTexture`. Cancellable on `banner_clear` (both the
 *     libsoup fetch and the in-flight glycin decode).
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
#include <gtk/gtk.h>
#ifdef HAVE_LIBSOUP
#include <libsoup/soup.h>
#endif
#include "compat.h"
#include "debug.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "network.h"
#include "cipher.h"
#include "htxf_io.h"
#include "htxf_subchannel.h"
#include "banner.h"
#include "banner_dispatch.h"
/* Glycin migration (June 2026): banner decode used to call
 * gdk_pixbuf_new_from_stream directly, then promote to a
 * GdkTexture via the deprecated gdk_texture_new_for_pixbuf.
 * Both decode call sites now route through the same async
 * glycin loader the inline-media path uses, which gives us
 * sandboxed image decode + drops every deprecated pixbuf
 * call from the banner hot path. */
#include "inline_media_decode.h"

/* Inline forward decl so we don't have to pull in hx.h (which
 * brings session + a transitive zoo of UI deps). task_new's real
 * declaration lives in tasks.h. rcv_task_banner_get is declared
 * over in banner.h next to its file-mode partner so rcv.c also
 * sees it via -Wmissing-prototypes. */
struct task;
extern struct task *task_new (struct htlc_conn *htlc, rcv_task_fn rcv,
                              void *ptr, void *data, const char *str);

/* Phase R3.2: tokio-blocking-pool replacement for the pthread + idle
 * pair this worker used to run on. Implementation lives in the
 * `hxbridge::blocking` module (rust/crates/hxbridge/src/blocking.rs);
 * the worker callback runs on tokio's dedicated blocking pool, the
 * completion runs on whatever GMainContext was thread-default at
 * the call site — which in production is the GLib main context the
 * UI iterates. The shim's lifetime contract is documented inline at
 * the call site below. */
extern void gtkhx_bridge_spawn_blocking_with_idle (
    void (*worker) (void *),
    void (*completion) (void *),
    void *user_data);

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
    /* TLS mode snapshot from htlc->tls at spawn time. When set,
     * the worker passes tls=1 to hx_sync_connect_to_host, which
     * wraps the HTXF subchannel in a GTlsClientConnection just
     * like the control channel was. Separate-port model: this
     * is the only path needed — no STARTTLS, no protocol
     * negotiation. */
    char tls;
    /* Opaque HOPE control-channel AEAD material handle (Rust
     * HxnetHopeAead*), an OWNED clone of htlc->hope_aead taken at spawn
     * (decoupled from htlc's lifetime — see the clone site). NULL unless
     * the control channel negotiated ChaCha20-Poly1305; hxnet_htxf_open
     * derives the per-transfer keys from it, so the worker never touches
     * the session key or cipher state. Freed with hxnet_hope_aead_free
     * when the fetch struct is freed. */
    void *hope_aead;
};

static guint htxf_generation = 0;

/* Glycin async-decode token. Tracks the in-flight banner
 * image decode regardless of which fetch mode (URL or HTXF)
 * fed it. A single static suffices because banner_clear /
 * banner_handle_message always tear down a previous banner
 * before kicking off a new one, so the two modes never run
 * decodes concurrently. banner_clear cancels via
 * inline_media_decode_cancel — same shape the inline-media
 * dialog uses. */
static gpointer decode_token = NULL;

/* ------------------------------------------------------------------- *
 * Forward declarations
 * ------------------------------------------------------------------- */

static void banner_show_caption (const char *text);
static void banner_show_texture (GdkTexture *tex);
static void banner_start_image_decode (const guint8 *bytes, gsize len);
static void on_banner_decode_done (HxInlineMediaDecoded *decoded,
                                   gpointer user_data);
#ifdef HAVE_LIBSOUP
static void banner_start_url_fetch (const char *url);
static void on_soup_send_done (GObject *source, GAsyncResult *result,
                               gpointer user_data);
#endif
static void on_banner_clicked (GtkGestureClick *gesture, int n_press, double x,
                               double y, gpointer user_data);

/* HTXF (file-mode) fetch */
static void banner_send_download_request (struct htlc_conn *htlc);
static void banner_htxf_worker_run (void *arg);
static void banner_htxf_completion_run (void *data);

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
	 * set_size_request. */
    banner_picture = gtk_picture_new ();
    gtk_widget_set_size_request (banner_picture, -1, BANNER_MAX_H);
    gtk_picture_set_can_shrink (GTK_PICTURE (banner_picture), TRUE);
    gtk_picture_set_content_fit (GTK_PICTURE (banner_picture), GTK_CONTENT_FIT_CONTAIN);
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
	 * presence. See hx_banner_type_is_url's docstring in
	 * banner_dispatch.h for the full rationale and the wire-shape
	 * normalisation. */
    if (hx_banner_type_is_url (type)) {
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

    /* Cancel any in-flight glycin decode — covers both URL and
	 * HTXF modes (a fetch already finished but the subprocess
	 * loader was still resolving). inline_media_decode_cancel
	 * suppresses the callback, so banner_show_texture won't
	 * race the impending banner replacement. */
    if (decode_token) {
        inline_media_decode_cancel (decode_token);
        decode_token = NULL;
    }

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
banner_show_texture (GdkTexture *tex)
{
    int w, h;

    if (!banner_picture || !tex) {
        return;
    }

    w = gdk_texture_get_width (tex);
    h = gdk_texture_get_height (tex);

    gtk_picture_set_paintable (GTK_PICTURE (banner_picture),
                               GDK_PAINTABLE (tex));

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

/* Common entry point shared by both fetch modes: spawn a
 * glycin decode against the freshly-fetched bytes. Per-banner
 * caps are loose-ish — max_dimension is BANNER_MAX_W (matches
 * the displayed cap, with room for over-sized server banners
 * we'd just scale down for display anyway), max_bytes is 256
 * KiB to match the inline-media spec floor (Hotline banners
 * are typically a few KB to ~50 KB in practice; the cap
 * defends against a hostile server stuffing megabytes through
 * the HTXF subchannel). The animation caps stay at 1 / 0:
 * banner display is a single GtkPicture frame, so we don't
 * collect (or run a tick over) per-frame data. A GIF with
 * multiple frames decodes; the result's first frame is what
 * we render and the rest gets dropped on decoded_free. */
static void
banner_start_image_decode (const guint8 *bytes, gsize len)
{
    HxInlineMediaCaps caps = {0};
    gpointer token;

    /* Cancel any prior in-flight decode (e.g. a previous
	 * fetch raced ahead of banner_clear). Drops its callback. */
    if (decode_token) {
        inline_media_decode_cancel (decode_token);
        decode_token = NULL;
    }

    caps.max_bytes = 256u * 1024u;
    caps.max_dimension = BANNER_MAX_W; /* width AND height axis */
    caps.max_pixels = (guint32)(BANNER_MAX_W) * (guint32)(BANNER_MAX_W);
    caps.max_frames = 1;
    caps.max_duration_ms = 1;

    /* Per inline_media_decode_async's contract: NULL return =
	 * synchronous reject + callback already fired. Store the
	 * token ONLY when it's non-NULL so the sync-reject path
	 * doesn't write into already-cleared state. */
    token = inline_media_decode_async (bytes, len, &caps,
                                       on_banner_decode_done, NULL);
    if (token) {
        decode_token = token;
    }
}

static void
on_banner_decode_done (HxInlineMediaDecoded *decoded, gpointer user_data)
{
    (void) user_data;

    /* The token has now been consumed by the callback firing;
	 * canonical-free it (cancel-after-completion is a no-op
	 * cancel but still drops the reference). */
    if (decode_token) {
        inline_media_decode_cancel (decode_token);
        decode_token = NULL;
    }

    if (!decoded->texture) {
        debug_log ("banner", "glycin decode rejected: code=%u msg=%s",
                   decoded->error_code,
                   decoded->error_message ? decoded->error_message
                                          : "(none)");
        banner_show_caption (_ ("Server banner: image not decodable"));
        inline_media_decoded_free (decoded);
        return;
    }

    banner_show_texture (decoded->texture);
    inline_media_decoded_free (decoded);
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
    guint status;
    gsize data_len = 0;
    const guint8 *data;

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

    /* Hand the bytes off to glycin via the shared async decode
	 * helper. `bytes` content is copied into the decoder's
	 * internal glib::Bytes wrapper synchronously, so we can
	 * unref the GBytes the moment the call returns. */
    data = g_bytes_get_data (bytes, &data_len);
    banner_start_image_decode (data, data_len);
    g_bytes_unref (bytes);
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
 * 3. banner_handle_htxf_reply schedules a tokio blocking-pool task
 *    via gtkhx_bridge_spawn_blocking_with_idle. The worker opens
 *    base_port+1, sends the 16-byte HTXF header, reads `size` bytes
 *    into a buffer.
 * 4. When the worker returns, the shim posts the completion onto
 *    the captured GMainContext (the GLib main loop). The completion
 *    hands the bytes to the shared async glycin decode helper
 *    (banner_start_image_decode); the decode callback calls
 *    banner_show_texture on success.
 *
 * Cancellation: banner_clear bumps htxf_generation. The worker
 * captured its own generation at spawn; the completion compares
 * it to current and silently drops the result if it was bumped
 * (reconnect, banner_clear, new banner). The worker itself can't
 * be interrupted mid-read (GIO blocking calls inside a tokio
 * spawn_blocking task — the same constraint POSIX blocking
 * sockets put on the legacy pthread path), but it always
 * finishes within seconds — banner data is small (a few KB to
 * ~50 KB in practice). The R3.3 hxnet rewrite that swaps GIO
 * for tokio::net::TcpStream is what eventually gives us a real
 * mid-flight cancel. */

static void
banner_send_download_request (struct htlc_conn *htlc)
{
    if (!htlc) {
        return;
    }
    debug_log ("banner", "sending HTLC_HDR_DOWNLOAD_BANNER (file-mode fetch)");
    task_new (htlc, RCV_TASK_FN (rcv_task_banner_get), NULL, NULL,
              "banner_get");
    /* DOWNLOAD_BANNER is a zero-chunk opcode. */
    hlwrite_chunks (htlc, HTLC_HDR_DOWNLOAD_BANNER, 0, NULL, 0);
}

void
banner_handle_htxf_reply (struct htlc_conn *htlc, guint32 ref, guint32 size)
{
    struct htxf_fetch *f;

    if (!banner_root || !htlc) {
        debug_log ("banner", "htxf reply ignored: no widget / no htlc");
        return;
    }
    /* Validation lives in banner_dispatch.c so the Tier 2 test
	 * suite can pin the contract without dragging in the rest of
	 * banner.c. The reject branches share a generic caption today
	 * but stay distinct in the enum so a future logging layer can
	 * attribute drops to the right cause. */
    switch (hx_banner_validate_htxf_reply (ref, size)) {
    case HX_BANNER_HTXF_OK:
        break;
    case HX_BANNER_HTXF_ZERO_REF:
    case HX_BANNER_HTXF_ZERO_SIZE:
        debug_log ("banner", "htxf reply rejected: ref=%u size=%u", ref, size);
        banner_show_caption (_ ("Server banner: empty transfer"));
        return;
    case HX_BANNER_HTXF_TOO_LARGE:
        debug_log ("banner", "htxf reply rejected: size %u > %u", size,
                   HX_BANNER_MAX_HTXF_SIZE);
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
    /* mirror the control-channel TLS mode so the HTXF
     * subchannel wraps in TLS too when the control did. Janus
     * binds TLS-HTXF on TLSPort+1 (5601), which falls out of the
     * existing port+1 arithmetic — the htlc->serverport already
     * stores the TLS HTLS port (5600) in TLS mode. */
    f->tls = htlc->tls;

    /* Clone the control connection's opaque HOPE AEAD material handle
     * (seeded on htlc at login when ChaCha20-Poly1305 was negotiated).
     * The worker hands it to hxnet_htxf_open, which derives the
     * per-transfer keys in-process — no session key or cipher state
     * crosses into this fetch. NULL (plaintext / Blowfish / no-cipher)
     * leaves the subchannel plaintext.
     *
     * We clone rather than borrow htlc->hope_aead: the blocking HTXF
     * worker can still be in flight when the connection drops, and
     * hx_htlc_close frees htlc->hope_aead (banner_clear only bumps the
     * fetch generation, it doesn't join the worker). An owned copy
     * decouples the worker's handle from htlc's lifetime — freed when
     * the fetch struct is freed in banner_htxf_completion_run. The
     * clone happens here on the main thread, where htlc->hope_aead is
     * still alive (spawn and hx_htlc_close are both main-thread). */
    f->hope_aead = hxnet_hope_aead_clone (
        (const HxnetHopeAead *) htlc->hope_aead);

    /* Hand the fetch off to tokio's blocking pool via hxbridge.
     * The shim runs `banner_htxf_worker_run` on a dedicated
     * blocking-pool thread, then posts `banner_htxf_completion_run`
     * back onto the captured GMainContext (the GLib main loop) when
     * the worker returns. The shim's only failure mode is a NULL
     * callback (this call has both non-NULL), so we don't need to
     * check a return value. Lazy runtime startup happens on the
     * first call; if it fails, Runtime::global() panics, and
     * hxbridge::blocking::spawn_blocking_with_idle catches the
     * panic in its catch_unwind block and calls std::process::abort
     * rather than let the panic unwind across the C ABI. The test
     * suite already exercises that path. */
    gtkhx_bridge_spawn_blocking_with_idle (
        banner_htxf_worker_run, banner_htxf_completion_run, f);
    debug_log ("banner", "htxf worker spawned: ref=%u size=%u gen=%u", ref,
               size, f->generation);
}

/* TLS TOFU trampoline for the banner HTXF subchannel. hxnet calls
 * this only when WebPKI validation failed; key the known-hosts
 * decision on the subchannel's own host:port (the endpoint the
 * pre-rewire GTlsConnection accept-cert handler used). user_data is
 * the struct htxf_fetch, which snapshotted serverhost/serverport at
 * spawn time. */
static int
banner_verify_cert_cb (const guint8 *fp, gsize fp_len, void *user_data)
{
    struct htxf_fetch *f = user_data;
    if (!f || !fp) {
        return 0;
    }
    g_autofree char *fp_str = g_strndup ((const char *) fp, fp_len);
    return hx_tls_verify_subchannel_cert (f->serverhost, f->serverport, fp_str)
               ? 1
               : 0;
}

static void
banner_htxf_worker_run (void *arg)
{
    struct htxf_fetch *f = arg;
    guint8 hdr_buf[HX_HTXF_PREAMBLE_MAX_BYTES];
    char errbuf[256] = { 0 };
    GSocketConnection *conn = NULL;
    int dupfd = -1;

    /* Transient htxf_conn the worker owns for its whole lifetime —
     * carries the hxnet channel handle (`hx`) plus the per-transfer
     * AEAD keys. No list membership needed. */
    struct htxf_conn xfer;
    memset (&xfer, 0, sizeof (xfer));
    xfer.ref = f->ref;
    htxf_io_init (&xfer);

    /* Plaintext TCP connect (SOCKS / IPv4-IPv6 fallback via
     * GSocketClient); the optional TLS wrap is done by hxnet below,
     * not here. Extract the connected fd and hand ownership over. */
    conn = hx_sync_connect_to_host (f->serverhost, f->serverport, errbuf,
                                    sizeof (errbuf), /*tls=*/0);
    if (!conn) {
        debug_log ("banner", "htxf connect failed: %s", errbuf);
        goto out;
    }
    {
        GSocket *sock = g_socket_connection_get_socket (conn);
        int sfd = sock ? g_socket_get_fd (sock) : -1;
        if (sfd >= 0) {
            dupfd = dup (sfd);
        }
    }
    g_object_unref (conn);
    if (dupfd < 0) {
        debug_log ("banner", "htxf could not dup connected fd");
        goto out;
    }

    /* type=HTXF_TYPE_BANNER so Mac-native servers route this subchannel
     * through their banner-send path. The preamble ALWAYS travels
     * plaintext (the server matches the subchannel to the queued
     * transfer by ref before any cipher state exists) — hxnet_htxf_open
     * writes it raw before arming AEAD. Banner transfers never need the
     * 24-byte SIZE64 variant. */
    size_t hdr_len = hx_htxf_subchannel_pack_preamble (
        hdr_buf, sizeof (hdr_buf),
        f->ref, f->size, HTXF_TYPE_BANNER, /*flags=*/0,
        /*size64=*/FALSE);
    if (hdr_len == 0) {
        debug_log ("banner",
                   "htxf header build failed (preamble builder returned 0)");
        close (dupfd);
        goto out;
    }

    /* Under HOPE+ChaCha20 the body bytes after the preamble are framed
     * AEAD packets. hxnet_htxf_open derives the per-transfer key pair
     * (mixing in ref so each subchannel gets its own key) from the
     * borrowed HOPE material handle and owns the framing thereafter;
     * a NULL handle leaves the body plaintext. */
    xfer.hx = hxnet_htxf_open (
        dupfd, f->tls,
        (const guint8 *) f->serverhost, strlen (f->serverhost),
        hdr_buf, hdr_len,
        (const HxnetHopeAead *) f->hope_aead, f->ref,
        banner_verify_cert_cb, f);
    if (!xfer.hx) {
        debug_log ("banner", "htxf hxnet_htxf_open failed");
        goto out;
    }

    /* Drain the body through the hxnet channel — passthrough for
     * plaintext, AEAD-deframing when armed. Same loop either way. */
    {
        guint8 *p = f->bytes;
        gsize remain = f->size;
        while (remain) {
            ssize_t r = htxf_io_read (&xfer, p, remain);
            if (r <= 0) {
                debug_log ("banner",
                           "htxf body read failed at < %zu bytes: %s",
                           (size_t) remain, g_strerror (errno));
                goto out;
            }
            p += r;
            remain -= (gsize) r;
        }
    }

    f->bytes_len = f->size;
    f->ok = TRUE;
    debug_log ("banner", "htxf worker fetched %u bytes (gen=%u)", f->size,
               f->generation);

out:
    /* htxf_io_release closes the hxnet channel (drops the fd / TLS
     * session). NULL-safe — no-op if open never succeeded. */
    htxf_io_release (&xfer);

    /* The hxbridge shim posts banner_htxf_completion_run onto the
     * captured main context automatically as soon as this worker
     * returns. The user_data pointer (this `f`) flows through
     * unchanged. */
}

static void
banner_htxf_completion_run (void *data)
{
    struct htxf_fetch *f = data;

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

    /* Same async glycin decode the URL path uses. The bytes
	 * are still owned by this fetch struct — banner_start_
	 * image_decode hands them off to the decoder which copies
	 * synchronously, so the cleanup below is safe to run
	 * before the decode callback fires. */
    banner_start_image_decode (f->bytes, f->bytes_len);

cleanup:
    hxnet_hope_aead_free (f->hope_aead);
    g_free (f->bytes);
    g_free (f);
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
