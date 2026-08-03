/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * notify.c — see notify.h for the design overview.
 *
 * For each event class:
 *
 *   1. The pref guard runs first (a one-byte check against
 *      gtkhx_prefs.notify_*). Cheap; no-op if the user disabled
 *      this event class.
 *   2. For mention-class events, hx_highlight_match runs against
 *      the body using the same word list (own nick +
 *      CFG_HIGHLIGHT_WORDS) that the xtext chat-line highlight
 *      uses. Reusing the matcher means visual highlight and
 *      notification trigger always agree.
 *   3. If notify_omit_focused is on, we look up the relevant
 *      window (chat for cid, pchat for cid > 0, msg window for
 *      uid, etc.) and bail if it has keyboard focus.
 *   4. Survivors build a GNotification with a per-source ID so a
 *      flood from one chat replaces rather than stacks.
 *
 * No threading: every entry point runs on main (callers are the
 * GtkhxSession signal handlers, which the signal infrastructure
 * already marshals to main).
 */

#include "config.h"
#include <string.h>
#include <gtk/gtk.h>
#include "hx.h"
#include "hxconn.h"
#include "session.h"
#include "chat.h"
#include "msg.h"
#include "proto_helpers.h"
#include "toolbar.h"
#include "notify.h"

/* ---- Module state -------------------------------------------------- */

static GtkApplication *notify_app;

/* ---- Helpers ------------------------------------------------------- */

/* Pull the user's current nick + the highlight-words list and run
 * hx_highlight_match against `body`. Reuses the same logic chat.c
 * uses for the inline ATTR_BOLD+ATTR_COLOR highlight so visual
 * highlight and notification trigger always agree. */
static gboolean
body_mentions_us (struct htlc_conn *htlc, const char *body)
{
    /* Our nick on *this* server. We can appear under different names on
     * different servers, so matching against the focused connection's would
     * both miss mentions and invent them. */
    const char *self_nick = htlc ? hx_conn_name (htlc) : NULL;
    gboolean matched = FALSE;
    GPtrArray *words;
    gchar **extras = NULL;

    if (!body || !*body) {
        return FALSE;
    }

    words = g_ptr_array_new ();
    if (self_nick && *self_nick) {
        g_ptr_array_add (words, (gpointer)self_nick);
    }
    if (gtkhx_prefs.highlight_words && *gtkhx_prefs.highlight_words) {
        extras = g_strsplit (gtkhx_prefs.highlight_words, ",", -1);
        for (gsize i = 0; extras && extras[i]; i++) {
            gchar *w = g_strstrip (extras[i]);
            if (*w) {
                g_ptr_array_add (words, w);
            }
        }
    }
    g_ptr_array_add (words, NULL);

    matched = hx_highlight_match (body, strlen (body),
                                  (const char *const *)words->pdata);

    g_ptr_array_unref (words);
    if (extras) {
        g_strfreev (extras);
    }
    return matched;
}

/* gtk_window_is_active() returns TRUE when the window is the OS-level
 * active window (focused). NULL-safe. */
static gboolean
window_is_active (GtkWidget *w)
{
    return w && GTK_IS_WINDOW (w) && gtk_window_is_active (GTK_WINDOW (w));
}

/* Look up the gtkhx_chat (UI) wrapper for a given cid. cid=0 is the
 * public chat. Returns NULL if no UI window has been created yet —
 * which counts as "not focused" for the purposes of omit-focused. */
static GtkWidget *
chat_window_for_cid (session *sess, guint32 cid)
{
    struct gtkhx_chat *gc = sess ? gchat_with_cid (sess, cid) : NULL;
    return hx_gchat_window (gc);
}

static GtkWidget *
msg_window_for_uid (session *sess, guint16 uid)
{
    struct msgwin *m = msgwin_with_uid (sess, uid);
    return m ? m->window : NULL;
}

/* Notification ids are app-global: g_application_send_notification replaces
 * any earlier notification with the same id. A cid or a uid is only unique
 * within a connection, so "msg-5" from two servers is one notification and
 * the second silently replaces the first. The connection's serial supplies
 * the missing dimension. */
#define NOTIFY_ID_FMT(kind) kind "-%u-%u"
#define NOTIFY_CONN_ID(htlc) ((unsigned)hx_conn_serial (htlc))

/* Truncate a body to a sensible notification length. GNOME / KDE
 * tend to wrap long notifications anyway, but a 200-char cap keeps
 * the popup readable and avoids dragging the user's eye through a
 * paragraph in their peripheral vision. */
#define NOTIFY_BODY_MAX 200

static char *
truncated (const char *body)
{
    gsize len;
    const char *cut;

    if (!body) {
        return g_strdup ("");
    }
    len = strlen (body);
    if (len <= NOTIFY_BODY_MAX) {
        return g_strdup (body);
    }

    /* g_utf8_find_prev_char-safe truncation: walk back from the
     * cap to the previous UTF-8 boundary so we don't slice through
     * a multi-byte sequence. */
    cut = g_utf8_find_prev_char (body, body + NOTIFY_BODY_MAX);
    if (!cut) {
        cut = body + NOTIFY_BODY_MAX;
    }
    return g_strdup_printf ("%.*s…", (int)(cut - body), body);
}

static void
send_notify (const char *id, const char *title, const char *body)
{
    GNotification *n;

    if (!notify_app || !title) {
        return;
    }

    n = g_notification_new (title);
    if (body && *body) {
        char *trim = truncated (body);
        g_notification_set_body (n, trim);
        g_free (trim);
    }
    g_notification_set_priority (n, G_NOTIFICATION_PRIORITY_NORMAL);

    /* The icon is sourced from the app's installed icon (matching
     * the GApplication app-id) so we don't have to bundle a
     * separate notification glyph. */

    g_application_send_notification (G_APPLICATION (notify_app), id, n);
    g_object_unref (n);
}

/* ---- Public API ---------------------------------------------------- */

void
gtkhx_notify_init (GtkApplication *app)
{
    notify_app = app;
}

/* Pull a g_strdup'd sender + body out of an HxChatEvent so the
 * notification gets a real sender in its title. Both fall back to
 * empty strings if the parser didn't find a "Nick: body" split
 * (an emote or raw server prose). Caller g_frees both. */
static void
event_slices (HxChatEvent *e, char **sender_out, char **body_out)
{
    *sender_out = (e && e->sender_len > 0)
                      ? g_strndup (e->line + e->sender_off, e->sender_len)
                      : g_strdup ("");
    *body_out = (e && e->body_len > 0)
                    ? g_strndup (e->line + e->body_off, e->body_len)
                    : (e ? g_strndup (e->line, e->line_len) : g_strdup (""));
}

void
gtkhx_notify_chat (struct htlc_conn *htlc, HxChatEvent *event)
{
    gboolean is_mention;
    gboolean want;
    char *sender = NULL, *body = NULL;
    char *title;
    char id[64];

    if (!event) {
        return;
    }

    /* cid > 0 is a private chat; the dedicated pchat entry point
     * handles those. This entry point is for the public chat
     * (cid == 0). */
    if (event->cid != 0) {
        gtkhx_notify_pchat (htlc, event);
        return;
    }

    event_slices (event, &sender, &body);
    is_mention = body_mentions_us (htlc, body);

    want = is_mention ? gtkhx_prefs.notify_chat_highlight
                      : gtkhx_prefs.notify_chat;
    if (!want) {
        goto out;
    }

    if (event->is_self) {
        goto out; /* never notify on our own line */
    }

    if (gtkhx_prefs.notify_omit_focused
        && window_is_active (
            chat_window_for_cid (sess_from_htlc (htlc), event->cid))) {
        goto out;
    }

    g_snprintf (id, sizeof (id), NOTIFY_ID_FMT ("chat"), NOTIFY_CONN_ID (htlc),
                event->cid);

    if (*sender) {
        title
            = g_strdup_printf (is_mention ? "%s mentioned you" : "%s", sender);
    } else {
        title
            = g_strdup (is_mention ? "Mention in public chat" : "Public chat");
    }
    send_notify (id, title, body);
    g_free (title);

out:
    g_free (sender);
    g_free (body);
}

void
gtkhx_notify_msg (struct htlc_conn *htlc, HxMsgEvent *event)
{
    char *title;
    char id[64];

    if (!event) {
        return;
    }

    if (!gtkhx_prefs.notify_msg) {
        return;
    }

    /* Don't notify on our own outbound PMs (echoed back by the
     * server, or local self-injects). */
    if (event->is_self) {
        return;
    }

    if (gtkhx_prefs.notify_omit_focused
        && window_is_active (
            msg_window_for_uid (sess_from_htlc (htlc), event->uid))) {
        return;
    }

    g_snprintf (id, sizeof (id), NOTIFY_ID_FMT ("msg"), NOTIFY_CONN_ID (htlc),
                event->uid);
    title = g_strdup_printf ("%s (private message)",
                             (event->name && *event->name) ? event->name : "?");
    send_notify (id, title, event->body);
    g_free (title);
}

void
gtkhx_notify_pchat (struct htlc_conn *htlc, HxChatEvent *event)
{
    gboolean is_mention;
    gboolean want;
    char *sender = NULL, *body = NULL;
    char *title;
    char id[64];

    if (!event) {
        return;
    }

    event_slices (event, &sender, &body);
    is_mention = body_mentions_us (htlc, body);

    want = is_mention ? gtkhx_prefs.notify_pchat_highlight
                      : gtkhx_prefs.notify_pchat;
    if (!want) {
        goto out;
    }

    if (event->is_self) {
        goto out;
    }

    if (gtkhx_prefs.notify_omit_focused
        && window_is_active (
            chat_window_for_cid (sess_from_htlc (htlc), event->cid))) {
        goto out;
    }

    g_snprintf (id, sizeof (id), NOTIFY_ID_FMT ("pchat"), NOTIFY_CONN_ID (htlc),
                event->cid);

    if (*sender) {
        title = g_strdup_printf (is_mention ? "%s mentioned you (private chat)"
                                            : "%s (private chat)",
                                 sender);
    } else {
        title = g_strdup (is_mention ? "Mention in private chat"
                                     : "Private chat");
    }
    send_notify (id, title, body);
    g_free (title);

out:
    g_free (sender);
    g_free (body);
}

void
gtkhx_notify_pchat_invite (struct htlc_conn *htlc, guint32 cid,
                           const char *inviter)
{
    char *title;
    char id[64];

    if (!gtkhx_prefs.notify_pchat_invite) {
        return;
    }

    g_snprintf (id, sizeof (id), NOTIFY_ID_FMT ("pchat-invite"),
                NOTIFY_CONN_ID (htlc), cid);
    title
        = g_strdup_printf ("Chat invitation from %s", inviter ? inviter : "?");
    send_notify (id, title, NULL);
    g_free (title);
}

void
gtkhx_notify_news (const char *headline)
{
    if (!gtkhx_prefs.notify_news) {
        return;
    }

    /* News notifications are coarse — one ID for all news, so a
     * burst of posts only fires the most recent. No focus check
     * (news window isn't tracked by uid/cid, and a news post
     * arriving while the news window is open is still
     * notification-worthy). */
    send_notify ("news", "New news post", headline ? headline : NULL);
}

void
gtkhx_notify_xfer_done (const char *filename)
{
    if (!gtkhx_prefs.notify_xfer) {
        return;
    }

    send_notify ("xfer", "File transfer complete", filename ? filename : NULL);
}

void
gtkhx_notify_broadcast (const char *text)
{
    if (!gtkhx_prefs.notify_broadcast) {
        return;
    }

    send_notify ("broadcast", "Server broadcast", text ? text : NULL);
}
