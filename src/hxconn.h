/*
 * hxconn.h — field accessors for struct htlc_conn.
 *
 * The connection struct is on the path to becoming an opaque, Rust-owned
 * allocation (docs/rust/network-endgame.md, phase E1). The migration can't be
 * atomic — the struct's fields are read and written from ~30 C files — so this
 * header introduces a stable getter/setter seam that call sites move onto
 * group-by-group. Today the bodies (hxconn.c) are thin C over the still-C
 * struct; at the E1c flip the struct definition and these bodies move into the
 * Rust `hxconn` crate with the same C ABI, and the call sites — already using
 * the accessors — don't change.
 *
 * `struct htlc_conn` is deliberately only forward-declared here: a consumer
 * that includes hxconn.h (and not protocol.h) sees the accessors but NOT the
 * fields, which is the end-state contract. This file grows one field group per
 * E1 increment.
 */

#ifndef GTKHX_HXCONN_H
#define GTKHX_HXCONN_H

#include <glib.h>

struct htlc_conn;

/* ---- Chat-history extension session state ---------------------------------
 *
 * history_max_msgs / history_max_days: server retention hints from the LOGIN
 * reply (0 = unlimited / undisclosed). chat_history_last_msgid: the newest
 * message_id rendered this run, used as the AFTER= reconnect cursor. All three
 * are wiped on disconnect. */
extern guint32 hx_conn_history_max_msgs (const struct htlc_conn *h);
extern void    hx_conn_set_history_max_msgs (struct htlc_conn *h, guint32 v);
extern guint32 hx_conn_history_max_days (const struct htlc_conn *h);
extern void    hx_conn_set_history_max_days (struct htlc_conn *h, guint32 v);
extern guint64 hx_conn_chat_history_last_msgid (const struct htlc_conn *h);
extern void    hx_conn_set_chat_history_last_msgid (struct htlc_conn *h,
                                                    guint64 v);

/* ---- Server endpoint identity --------------------------------------------
 *
 * Populated at hx_connect time and read by the HTXF-subchannel setup, the TLS
 * TOFU verify, and the connection-event log lines. serverhost / ip_addr are
 * NUL-terminated strings; the getters return the internal buffer (valid for
 * the connection's lifetime) and the setters copy into it (truncating to the
 * field's capacity). `tls` is the per-connection separate-port TLS flag (0/1).
 * ip_addr is the resolved printable peer address; hx_conn_set_ip_addr with ""
 * clears it. */
extern const char *hx_conn_serverhost (const struct htlc_conn *h);
extern void        hx_conn_set_serverhost (struct htlc_conn *h, const char *v);
extern guint16     hx_conn_serverport (const struct htlc_conn *h);
extern void        hx_conn_set_serverport (struct htlc_conn *h, guint16 v);
extern const char *hx_conn_ip_addr (const struct htlc_conn *h);
extern void        hx_conn_set_ip_addr (struct htlc_conn *h, const char *v);
extern char        hx_conn_tls (const struct htlc_conn *h);
extern void        hx_conn_set_tls (struct htlc_conn *h, char v);

/* ---- Negotiated protocol identity ----------------------------------------
 *
 * version: the server's HTLS_DATA_VERSION from the LOGIN reply (0 on 1.0/1.2
 * servers that don't advertise one — the gate for PING, the 1.5 news button,
 * etc.). caps: the DATA_CAPABILITIES bitmask the server confirmed for this
 * session (0 on legacy servers). Both are wiped on disconnect. hx_conn_has_cap
 * is the predicate every `htlc->caps & HTLC_CAP_*` bit-test now goes through;
 * hx_conn_caps exposes the raw bitmask for the rare whole-value consumer. */
extern guint16  hx_conn_version (const struct htlc_conn *h);
extern void     hx_conn_set_version (struct htlc_conn *h, guint16 v);
extern guint64  hx_conn_caps (const struct htlc_conn *h);
extern void     hx_conn_set_caps (struct htlc_conn *h, guint64 v);
extern gboolean hx_conn_has_cap (const struct htlc_conn *h, guint64 cap);

/* ---- Our own user identity -----------------------------------------------
 *
 * uid: our session user-id (assigned by the server). icon: our chat-list icon
 * id. nick_color: the Colored-Nicknames RGB (0x00RRGGBB, or
 * HX_NICK_COLOR_NONE). Seeded from prefs at connect and updated from SELFINFO /
 * USER_CHANGE echoes. (The old status-colour field `color` was write-only and
 * has been removed.)
 *
 * hx_conn_icon_ptr returns the raw address of the icon field: the ICON cfgvar
 * (options.c) binds the prefs read/write path to a stable guint16* and can't go
 * through a value accessor. It's the deliberate escape hatch for that one
 * pointer-based consumer; at the E1c flip the Rust hxconn owner returns a raw
 * pointer into its struct here. Everything else uses the value get/set. */
extern guint16 hx_conn_uid (const struct htlc_conn *h);
extern void    hx_conn_set_uid (struct htlc_conn *h, guint16 v);
extern guint16 hx_conn_icon (const struct htlc_conn *h);
extern void    hx_conn_set_icon (struct htlc_conn *h, guint16 v);
extern guint16 *hx_conn_icon_ptr (struct htlc_conn *h);
extern guint32 hx_conn_nick_color (const struct htlc_conn *h);
extern void    hx_conn_set_nick_color (struct htlc_conn *h, guint32 v);

#endif /* GTKHX_HXCONN_H */
