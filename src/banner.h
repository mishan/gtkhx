#ifndef HX_BANNER_H
#define HX_BANNER_H

#include <gtk/gtk.h>

/*
 * Server banner support — C ABI declarations.
 *
 * The implementation is Rust now (rust/crates/gtkhx-ui/src/banner.rs, folded in
 * with the old banner_dispatch.c classifier). This header only carries the C
 * ABI the C call sites link against; the module itself owns the banner widget,
 * the URL-mode fetch (hxnet ureq), the HTXF file-mode fetch, and the glycin
 * decode.
 *
 * Hotline servers advertise a per-server banner via HTLS_HDR_BANNER in two
 * modes: "URL " (server hands a URL, client fetches) and "JPEG"/"GIFf"/"PICT"/…
 * (server holds the bytes, client fetches over the HTXF subchannel).
 *
 * Lifecycle:
 *   - banner_widget_new() — called from toolbar.c during window construction;
 *     returns the widget to embed.
 *   - banner_handle_message() — called from rcv.c::hx_rcv_banner after parsing;
 *     routes to URL or HTXF path based on TYPE.
 *   - banner_clear() — called on disconnect; cancels any in-flight fetch and
 *     hides the widget.
 *   - banner_handle_htxf_reply() — called from rcv_task_banner_get (hxhandlers)
 *     with the file-mode reply's ref + size.
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
extern void banner_handle_message (struct htlc_conn *htlc, const char *type,
                                   gboolean has_url, const char *url);

/* Hide the banner + cancel any in-flight fetch. Called on
 * hx_htlc_close so a stale banner from one server doesn't linger
 * after we connect to another. */
extern void banner_clear (void);

/* Continuation of the file-mode banner flow: the server's reply
 * to our HTLC_HDR_DOWNLOAD_BANNER carries a transfer refnum +
 * size. rcv_task_banner_get extracts them and calls this so
 * banner.c can spin up an HTXF worker to fetch the bytes.
 * Called on the main thread. */
extern void banner_handle_htxf_reply (struct htlc_conn *htlc, guint32 ref,
                                      guint32 size);

/* Task callback for HTLC_HDR_DOWNLOAD_BANNER replies. Defined in
 * rcv.c (where the other rcv_task_* handlers live) but declared
 * here so banner.c can pass it to RCV_TASK_FN() when registering
 * the task, and so rcv.c itself sees a prior prototype for the
 * function body (avoids -Wmissing-prototypes). */
extern void rcv_task_banner_get (struct htlc_conn *htlc, const guint8 *frame,
                                 gsize frame_len, void *ptr, void *data);

#endif /* HX_BANNER_H */
