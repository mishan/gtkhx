#ifndef HX_BANNER_H
#define HX_BANNER_H

#include <gtk/gtk.h>

/*
 * Server banner support — Phase 5.
 *
 * Hotline servers can advertise a per-server banner image via the
 * HTLS_HDR_BANNER message. Two on-the-wire modes:
 *
 *   "URL " — server provides an http(s) URL; client fetches the
 *            image bytes itself. Implemented via libsoup-3.
 *   "JPEG" / "GIFf" / "PICT" / etc. — server holds the bytes; client
 *            sends HTLC_HDR_BANNER_GET, server replies via the HTXF
 *            subchannel with a flat-file payload.
 *
 * The display surface is a banner widget docked at the bottom of
 * the toolbar window. It collapses (gtk_widget_set_visible(FALSE))
 * while no banner is active or the connection is down. While the
 * fetch is in-flight a small spinner shows in place of the image.
 *
 * This module owns:
 *   - The banner widget (image surface, click handling, visibility).
 *   - The fetch state machine for URL mode (libsoup async fetch ->
 *     GdkPixbuf decode -> GtkPicture display).
 *   - The fetch state machine for HTXF mode (BANNER_GET task ->
 *     HTXF subchannel -> file-mode bytes -> display).
 *
 * Lifecycle:
 *   - banner_widget_new() — called from toolbar.c during window
 *     construction; returns the widget to embed.
 *   - banner_handle_message() — called from rcv.c::hx_rcv_banner
 *     after parsing. Routes to URL or HTXF path based on TYPE.
 *   - banner_clear() — called on disconnect; cancels any in-flight
 *     fetch and hides the widget.
 */

/* Build the banner widget. Returned widget is the root of the
 * banner row — caller embeds it in the toolbar window. There is at
 * most one of these per process; banner_handle_message routes to
 * the most-recently-built widget. */
extern GtkWidget *banner_widget_new (void);

/* Dispatch an HTLS_HDR_BANNER receive event. `type` is the 4-byte
 * banner type code (NUL-terminated, e.g. "URL "). `url` is the
 * URL string when has_url is TRUE — otherwise we'll send
 * HTLC_HDR_BANNER_GET to fetch via HTXF. Safe to call before
 * banner_widget_new() (the message is dropped). */
struct htlc_conn;
extern void banner_handle_message (struct htlc_conn *htlc,
                                   const char *type,
                                   gboolean has_url,
                                   const char *url);

/* Hide the banner + cancel any in-flight fetch. Called on
 * hx_htlc_close so a stale banner from one server doesn't linger
 * after we connect to another. */
extern void banner_clear (void);

/* Continuation of the file-mode banner flow: the server'"'"'s reply
 * to our HTLC_HDR_DOWNLOAD_BANNER carries a transfer refnum +
 * size. rcv_task_banner_get extracts them and calls this so
 * banner.c can spin up an HTXF worker to fetch the bytes.
 * Called on the main thread. */
extern void banner_handle_htxf_reply (struct htlc_conn *htlc,
                                      guint32 ref, guint32 size);

#endif /* HX_BANNER_H */
