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

#endif /* GTKHX_HXCONN_H */
