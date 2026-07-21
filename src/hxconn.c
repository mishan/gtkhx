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

const char *
hx_conn_serverhost (const struct htlc_conn *h)
{
    return h->serverhost;
}

void
hx_conn_set_serverhost (struct htlc_conn *h, const char *v)
{
    g_strlcpy (h->serverhost, v, sizeof (h->serverhost));
}

guint16
hx_conn_serverport (const struct htlc_conn *h)
{
    return h->serverport;
}

void
hx_conn_set_serverport (struct htlc_conn *h, guint16 v)
{
    h->serverport = v;
}

const char *
hx_conn_ip_addr (const struct htlc_conn *h)
{
    return h->ip_addr;
}

void
hx_conn_set_ip_addr (struct htlc_conn *h, const char *v)
{
    g_strlcpy (h->ip_addr, v, sizeof (h->ip_addr));
}

char
hx_conn_tls (const struct htlc_conn *h)
{
    return h->tls;
}

void
hx_conn_set_tls (struct htlc_conn *h, char v)
{
    h->tls = v;
}

guint16
hx_conn_version (const struct htlc_conn *h)
{
    return h->version;
}

void
hx_conn_set_version (struct htlc_conn *h, guint16 v)
{
    h->version = v;
}

guint64
hx_conn_caps (const struct htlc_conn *h)
{
    return h->caps;
}

void
hx_conn_set_caps (struct htlc_conn *h, guint64 v)
{
    h->caps = v;
}

gboolean
hx_conn_has_cap (const struct htlc_conn *h, guint64 cap)
{
    return (h->caps & cap) != 0;
}

guint16
hx_conn_uid (const struct htlc_conn *h)
{
    return h->uid;
}

void
hx_conn_set_uid (struct htlc_conn *h, guint16 v)
{
    h->uid = v;
}

guint16
hx_conn_icon (const struct htlc_conn *h)
{
    return h->icon;
}

void
hx_conn_set_icon (struct htlc_conn *h, guint16 v)
{
    h->icon = v;
}

guint16 *
hx_conn_icon_ptr (struct htlc_conn *h)
{
    return &h->icon;
}

guint16
hx_conn_color (const struct htlc_conn *h)
{
    return h->color;
}

void
hx_conn_set_color (struct htlc_conn *h, guint16 v)
{
    h->color = v;
}

guint32
hx_conn_nick_color (const struct htlc_conn *h)
{
    return h->nick_color;
}

void
hx_conn_set_nick_color (struct htlc_conn *h, guint32 v)
{
    h->nick_color = v;
}
