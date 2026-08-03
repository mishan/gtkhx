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
 *   - banner_clear() — called on disconnect and on closing a connection;
 *     forgets that connection's banner and cancels any fetch still running
 *     for it.
 *   - banner_show_active() — called from hx_chrome_refresh on a connection-tab
 *     switch; repaints the row for whichever connection is now focused.
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

/* Forget `htlc`'s banner and cancel any fetch still running for it. Called on
 * hx_htlc_close so a stale banner from one server doesn't linger after we
 * connect to another.
 *
 * Per connection: there is one banner row, but disconnecting one server must
 * not blank the banner of another that is still up — and the connection
 * closing need not be the one on screen. */
extern void banner_clear (struct htlc_conn *htlc);

/* Repaint the banner row for the connection the user is now looking at.
 * The banner follows the focus, like the status bar and the window title;
 * hx_chrome_refresh calls this on a connection-tab switch. */
extern void banner_show_active (void);

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
