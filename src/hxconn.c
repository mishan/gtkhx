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
#include "hl_access.h" /* hl_access_has / hl_access_permits over the bitmap */

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

const char *
hx_conn_name (const struct htlc_conn *h)
{
    return h->name;
}

void
hx_conn_set_name (struct htlc_conn *h, const char *v)
{
    g_strlcpy (h->name, v, sizeof (h->name));
}

char *
hx_conn_name_buf (struct htlc_conn *h)
{
    return h->name;
}

void
hx_conn_set_login (struct htlc_conn *h, const char *v)
{
    g_strlcpy (h->login, v, sizeof (h->login));
}

gboolean
hx_conn_logged_in (const struct htlc_conn *h)
{
    return h->flags.logged_in;
}

void
hx_conn_set_logged_in (struct htlc_conn *h, gboolean v)
{
    h->flags.logged_in = v ? 1 : 0;
}

gboolean
hx_conn_post_login_fetched (const struct htlc_conn *h)
{
    return h->flags.post_login_fetched;
}

void
hx_conn_set_post_login_fetched (struct htlc_conn *h, gboolean v)
{
    h->flags.post_login_fetched = v ? 1 : 0;
}

int
hx_conn_gif_icons_state (const struct htlc_conn *h)
{
    return h->gif_icons_state;
}

void
hx_conn_set_gif_icons_state (struct htlc_conn *h, int v)
{
    h->gif_icons_state = v;
}

guint
hx_conn_gif_icons_probe_timer (const struct htlc_conn *h)
{
    return h->gif_icons_probe_timer;
}

void
hx_conn_set_gif_icons_probe_timer (struct htlc_conn *h, guint v)
{
    h->gif_icons_probe_timer = v;
}

guint32
hx_conn_gif_icons_probe_trans (const struct htlc_conn *h)
{
    return h->gif_icons_probe_trans;
}

void
hx_conn_set_gif_icons_probe_trans (struct htlc_conn *h, guint32 v)
{
    h->gif_icons_probe_trans = v;
}

gboolean
hx_conn_access_has (const struct htlc_conn *h, int bit)
{
    return hl_access_has ((const guint8 *) &h->access, bit);
}

gboolean
hx_conn_access_permits (const struct htlc_conn *h, int bit)
{
    return hl_access_permits ((const guint8 *) &h->access, bit);
}

int
hx_conn_fd (const struct htlc_conn *h)
{
    return h->fd;
}

void
hx_conn_set_fd (struct htlc_conn *h, int v)
{
    h->fd = v;
}
