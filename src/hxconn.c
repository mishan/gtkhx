/*
 * hxconn.c — struct htlc_conn field accessors (C bodies).
 *
 * Thin passthroughs over the still-C struct while the E1 accessor migration is
 * in flight (docs/rust/network-endgame.md). At the E1c flip this file is
 * deleted and the same C ABI is provided by the Rust `hxconn` crate, which will
 * own the struct's storage. Until then every accessor is a one-liner so the
 * seam adds a call boundary but no behaviour.
 */

#include "config.h"

#include "hxconn.h"
#include "protocol.h" /* the full struct htlc_conn definition (still C for now) */

guint32
hx_conn_history_max_msgs (const struct htlc_conn *h)
{
    return h->history_max_msgs;
}

void
hx_conn_set_history_max_msgs (struct htlc_conn *h, guint32 v)
{
    h->history_max_msgs = v;
}

guint32
hx_conn_history_max_days (const struct htlc_conn *h)
{
    return h->history_max_days;
}

void
hx_conn_set_history_max_days (struct htlc_conn *h, guint32 v)
{
    h->history_max_days = v;
}

guint64
hx_conn_chat_history_last_msgid (const struct htlc_conn *h)
{
    return h->chat_history_last_msgid;
}

void
hx_conn_set_chat_history_last_msgid (struct htlc_conn *h, guint64 v)
{
    h->chat_history_last_msgid = v;
}
