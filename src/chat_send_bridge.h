#ifndef HX_CHAT_SEND_BRIDGE_H
#define HX_CHAT_SEND_BRIDGE_H 1

#include <glib.h>

G_BEGIN_DECLS

/*
 * chat_send_bridge.c — the narrow C seam the Rust chat wire-senders
 * (hxhandlers::send::chat module, the Phase R5 port of chat.c's send path) reach
 * through for the two things they can't do from Rust: read the
 * per-connection HTLC_CAP_TEXT_ENCODING bit off the opaque htlc_conn,
 * and look up the per-session `struct chat` a PART needs.
 * The chat model (the GHashTable + struct chat) stays C in chat.c; these
 * shims keep the Rust side from touching its layout, exactly like
 * voice_bridge.c does for the voice UI.
 */

struct htlc_conn;

/* TRUE if this connection negotiated HTLC_CAP_TEXT_ENCODING (UTF-8 on
 * the wire); FALSE for legacy Mac Roman. NULL-safe. */
extern gboolean hx_htlc_text_encoding_cap (struct htlc_conn *htlc);

/* Look up the `struct chat` for `cid` on this connection's session, or
 * NULL if it isn't registered. Returned as an opaque pointer — the Rust
 * caller only checks it for NULL. NULL-safe. */
extern void *hx_chat_lookup (struct htlc_conn *htlc, guint32 cid);

G_END_DECLS

#endif
