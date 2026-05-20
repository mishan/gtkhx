/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <gtk/gtk.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include "hx.h"
#ifdef CONFIG_CIPHER
#include "cipher_aead.h"
#include "hope.h"
#endif
#include "gtkhx_session.h"
#include "network.h"
#include "xfers.h"
#include "chat.h"
#include "tasks.h"
#include "files.h"
#include "files_remote_provider.h"
#include "preview.h"
#include "hl_date.h"
#include "gtkutil.h"
#include "msg.h"
#include "news.h"
#include "sound.h"
#include "plugin.h"
#include "users.h"
#include "usermod.h"
#include "rcv.h"
#include "news15.h"
#include "hfs.h"
#include "proto_trace.h"
#include "debug.h"
#include "connect.h"
#include "banner.h"
#include "chat_history.h"
#include "hl_access.h"

static size_t news_len = 0;
static guint8 *news_buf = 0;
static char *hx_timeformat = "%c";
extern int xfer_go_timer (void *__arg);

void rcv_task_user_list (struct htlc_conn *htlc, struct chat *chat, int text);
/* Phase 5: forward decl so hx_post_login_fetches (defined just below)
 * can hand it to task_new before the implementation site. */
void rcv_task_news_users (struct htlc_conn *htlc, struct chat *chat, int text);

/* Phase 5: post-login state-machine. The 1.5 flow per
 * Capabilities/connect-spec is:
 *
 *   C → S  TranLogin
 *   S → C  TranLogin reply (version, server name, banner_id, uid)
 *   S → C  TranUserAccess (HTLS_HDR_USER_SELFINFO, access bits)
 *   S → C  TranShowAgreement (or "no agreement" indicator)
 *   C → S  TranAgreed (NAME + ICON + OPTIONS) — identity arrives HERE
 *   S → C  TranNotifyChangeUser broadcast (others are told we joined)
 *   S → C  TranServerBanner
 *   S → C  TranAgreed reply
 *
 * Only AFTER our TranAgreed has gone out is the server willing to
 * treat us as a fully-joined user — that's when USER_GETLIST /
 * news fetches make sense. fogWraith reported (2026-05) that we
 * were firing those on SELFINFO receipt, which is too early in
 * the 1.5 flow: server logs showed "Get user list" / "Get messages"
 * arriving before "Accept agreement". The fix is to gate the post-
 * login fetches on the AGREEMENTAGREE-send path (concurrence on
 * Agree click, or the auto-send in hx_rcv_agreement_file for
 * HX_AGREEMENT_NONE / NOT_FOUND).
 *
 * 1.2 servers don't send AGREEMENTAGREE either way — name + icon
 * are in the LOGIN packet, agreement (if any) is informational
 * with no response opcode. For those we rely on the 2-second
 * fallback timer armed in rcv_task_login.
 *
 * `post_login_fetched` is the single-fire guard: whichever path
 * runs first sets it, the other path becomes a no-op. Reset on
 * each login attempt at the top of rcv_task_login's else branch. */
static gboolean post_login_fetched = FALSE;
static guint post_login_timer_id = 0;

/* Public entry — network.c::hx_send_agreement_agree calls this
 * right after the hlwrite so post-login fetches fire on the spec-
 * correct boundary (after AGREEMENTAGREE, not after SELFINFO). */
void
hx_post_login_fetches (struct htlc_conn *htlc)
{
    if (post_login_fetched) {
        return;
    }
    post_login_fetched = TRUE;

    if (post_login_timer_id) {
        g_source_remove (post_login_timer_id);
        post_login_timer_id = 0;
    }

    /* Fetch users + (gated) news. rcv_task_news_users handles
	 * both — it calls rcv_task_user_list on the USER_GETLIST
	 * reply and then reload_news, the latter of which is itself
	 * gated on HL_ACCESS_READ_NEWS. */
    task_new (htlc, RCV_TASK_FN (rcv_task_news_users),
              chat_with_cid (&the_session, 0), 0, "who");
    hlwrite (htlc, HTLC_HDR_USER_GETLIST, 0, 0);

    /* Chat-history extension: if the server echoed our cap bit in
	 * the LOGIN reply, request a batch for public chat (channel 0).
	 * hx_get_chat_history is a no-op when the cap wasn't negotiated,
	 * so this is safe to gate on caps here too — task_new only
	 * fires when we'll actually send.
	 *
	 * Two modes:
	 *   * Initial connect (htlc->chat_history_last_msgid == 0):
	 *     limit-based fetch sized by gtkhx_prefs.chat_history_initial
	 *     (default 50). 0 disables the initial pull entirely.
	 *   * Reconnect (last_msgid > 0): AFTER=last_msgid catch-up
	 *     fetch — the server returns everything stored since our
	 *     last view of the chat. No client-side limit; the server
	 *     applies its own (history_max_msgs).
	 *
	 * The cursor is reset in network.c when the user dials a
	 * different host:port, so initial-mode is what happens when
	 * the user switches servers, and reconnect-mode is what
	 * happens when the user clicks Reconnect to the same one.
	 *
	 * Clamp negative limit values defensively (cfgvars INT
	 * parser doesn't enforce a floor). */
    if (htlc->caps & HTLC_CAP_CHAT_HISTORY) {
        if (htlc->chat_history_last_msgid > 0) {
            /* Reconnect catch-up — AFTER=last_msgid, no limit. */
            debug_log ("chat-history",
                       "reconnect catch-up: AFTER=%" G_GUINT64_FORMAT,
                       htlc->chat_history_last_msgid);
            task_new (htlc, RCV_TASK_FN (rcv_task_chat_history),
                      GUINT_TO_POINTER (HX_HISTORY_CHANNEL_PUBLIC), 0,
                      "chat-history-catchup");
            hx_get_chat_history (htlc, HX_HISTORY_CHANNEL_PUBLIC,
                                 /*before=*/0,
                                 /*after=*/htlc->chat_history_last_msgid,
                                 /*limit=*/0);
        } else {
            /* Initial connect — limit-based fetch. */
            int limit = gtkhx_prefs.chat_history_initial;
            if (limit < 0) {
                limit = 0;
            }
            if (limit > 0xffff) {
                limit = 0xffff;
            }
            if (limit > 0) {
                task_new (htlc, RCV_TASK_FN (rcv_task_chat_history),
                          GUINT_TO_POINTER (HX_HISTORY_CHANNEL_PUBLIC), 0,
                          "chat-history");
                hx_get_chat_history (htlc, HX_HISTORY_CHANNEL_PUBLIC,
                                     /*before=*/0, /*after=*/0,
                                     (guint16) limit);
            }
        }
    }
}

static gboolean
post_login_fallback (gpointer data)
{
    struct htlc_conn *htlc = data;

    post_login_timer_id = 0;
    if (htlc && htlc->fd && !post_login_fetched) {
        debug_log (
            "login",
            "AGREEMENTAGREE didn't fire after 2s, firing fetches anyway");
        hx_post_login_fetches (htlc);
    }
    return G_SOURCE_REMOVE;
}

void
rcv_login_reset (void)
{
    if (post_login_timer_id) {
        g_source_remove (post_login_timer_id);
        post_login_timer_id = 0;
    }
    post_login_fetched = FALSE;
}

/*
void print_binary(char *buf, int len)
{
	int i;

	for(i = 0; i < len; i++) {
		int j;

		for(j = 0; j < 8; j++) {
			printf("%d", *buf&j?1:0);
		}
	}
	printf("\n");
}
*/

int
task_inerror (struct htlc_conn *htlc)
{
    struct hl_hdr *h = (struct hl_hdr *)htlc->in.buf;

    /*		g_print("task_inerror: h->flag == %d\n", h->flag);
		print_binary(&h->flag, 4); */

    if ((ntohl (h->flag) & 1)) {
        return 1;
    }

    return 0;
}

void
hx_rcv_chat (struct htlc_conn *htlc)
{
    struct hx_chat_msg msg;
    session *sess = &the_session;
    struct chat *hx_chat = chat_with_cid (sess, 0);

    /* Chunk parse + CR2LF/strip_ansi + leading-LF strip lives in
	 * proto_helpers.c so the Tier 2 unit tests can drive it. */
    if (!hx_chat_extract (htlc, &msg)) {
        return;
    }

    if (msg.uid) { /* do ignoring stuff */
        struct hx_user *user = hx_user_with_uid (hx_chat, msg.uid);
        if (user && user->ignore) {
            return;
        }
    }

#ifdef USE_PLUGIN
    if (EMIT_SIGNAL (XP_RCV_CHAT, sess, msg.text, &msg.cid, &msg.uid, 0, 0)
        == 1) {
        return;
    }
#endif

    /* Phase 3+: hx_output.chat → "chat" signal on the session
	 * emitter. Phase 5+: payload is a boxed HxChatEvent that
	 * bundles the UTF-8-validated line, sender/body slices, and
	 * info/self flags — every subscriber (chat.c renderer,
	 * notify.c) reads the same parsed view. */
    {
        HxChatEvent *ev = hx_chat_event_new (
            msg.text, msg.text_len, msg.cid,
            the_session.htlc.name[0] ? the_session.htlc.name : NULL);
        gtkhx_session_emit_chat (gtkhx_session_get_default (), htlc, ev);
        hx_chat_event_free (ev);
    }
    play_sound (CHAT_POST);
}

void
hx_rcv_msg (struct htlc_conn *htlc)
{
    struct hx_msg_msg pm;
    session *sess = &the_session;
    struct chat *chat = chat_with_cid (sess, 0);
    struct hx_user *user = 0;

    /* Chunk parse + name/body sanitisation lives in proto_helpers.c
	 * so the Tier 2 unit tests can drive it. */
    if (!hx_msg_extract (htlc, &pm)) {
        return;
    }

    user = hx_user_with_uid (chat, pm.uid);
    if (user && user->ignore) {
        return;
    }

#ifdef USE_PLUGIN
    if (EMIT_SIGNAL (XP_RCV_MSG, sess, pm.msg, pm.name, &pm.uid, 0, 0) == 1) {
        return;
    }
#endif

    if (pm.uid > 0) {
        /* Phase 5+: msg signal payload is a boxed HxMsgEvent
		 * (parsed once; every subscriber sees the same
		 * UTF-8-sanitised, self-classified view). */
        HxMsgEvent *ev = hx_msg_event_new (
            pm.uid, pm.name, pm.name_len, pm.msg, pm.msg_len,
            the_session.htlc.name[0] ? the_session.htlc.name : NULL);
        gtkhx_session_emit_msg (gtkhx_session_get_default (), ev);
        hx_msg_event_free (ev);
    } else {
        broadcastmsg (pm.msg);
    }
    play_sound (MSG);

    if (!*last_msg_nick) {
        strncpy (last_msg_nick, pm.name, 31);
        last_msg_nick[31] = 0;
    }
}

void
hx_rcv_agreement_file (struct htlc_conn *htlc)
{
    /* Phase 5: chunk-walking + sanitisation lives in
	 * hx_agreement_extract. The 16 KiB cap is generous; mhxd
	 * agreements hover around 1-2 KiB on the public servers and
	 * the protocol's chunk length is uint16 (max 65535) anyway. */
    char buf[16384];
    gsize body_len = 0;
    hx_agreement_result r
        = hx_agreement_extract (htlc, buf, sizeof (buf), &body_len);

    /* Phase 5+: no-agreement auto-path — the user has nothing to
	 * click Agree on, so we send AGREEMENTAGREE ourselves to:
	 *   - complete login on mhxd-style servers (where finish_login
	 *     runs inside rcv_agreementagree)
	 *   - deliver NAME + ICON to the server in both flavours
	 *   - trigger HTLS_HDR_BANNER emission on banner-configured
	 *     servers (the banner write is unconditional on banner.type
	 *     inside rcv_agreementagree, ungated on in_login)
	 *
	 * HX_AGREEMENT_NONE: server config has agreement disabled.
	 * HX_AGREEMENT_NOT_FOUND: malformed payload.
	 * For HX_AGREEMENT_OK, fall through to popping the agreement
	 * window — concurrence() handles the wire op on Agree click,
	 * using the same AGREEMENTAGREE message.
	 *
	 * Earlier code gated this on !flags.logged_in (to avoid a
	 * suspected MacSecret disconnect on AGREEMENTAGREE-for-logged-
	 * in). That gate was almost certainly chasing a misdiagnosed
	 * symptom — see gtkhx.c::concurrence for the long comment —
	 * and was suppressing banner delivery on every 1.9 server. */
    if (r == HX_AGREEMENT_NONE || r == HX_AGREEMENT_NOT_FOUND) {
        hx_send_agreement_agree (htlc);
        return;
    }
    if (r != HX_AGREEMENT_OK) {
        return;
    }

#ifdef USE_PLUGIN
    guint16 plugin_len = (guint16)body_len;
    if (EMIT_SIGNAL (XP_RCV_AGREE, &the_session, buf, &plugin_len, 0, 0, 0)
        == 1) {
        return;
    }
#endif
    gtkhx_session_emit_agreement (gtkhx_session_get_default (), &the_session,
                                  buf, (guint16)body_len);
}

/* Phase 5: rewritten to use hx_news_post_walk in proto_helpers.c.
 * The previous version maintained an unbounded-growth news_buf
 * accumulator that the emit code never actually consumed —
 * hx_output.news_post was called with just the new chunk's `_len`
 * bytes regardless of how much had been accumulated. See the
 * walker comment in proto_helpers.h for the full breakdown. */
static void
news_post_emit (void *user, const char *bytes, gsize len)
{
    struct htlc_conn *htlc = user;
    gtkhx_session_emit_news_post (gtkhx_session_get_default (), htlc,
                                  (char *)bytes, (guint16)len);
    play_sound (NEWS_POST);
}

void
hx_rcv_news_post (struct htlc_conn *htlc)
{
    hx_news_post_walk (htlc, news_post_emit, htlc);
}

void
hx_rcv_task (struct htlc_conn *htlc)
{
    struct hl_hdr *h = (struct hl_hdr *)htlc->in.buf;
    guint32 trans;
    struct task *tsk;
    char error = 0;

    HN32 (&trans, &h->trans);
    tsk = task_with_trans (&the_session, trans);

    if (task_inerror (htlc)) {
        task_error (htlc);
        error = 1;
    }
    if (tsk) {
        /* XXX tsk->rcv might call task_delete */
        int fd = htlc->fd;
        /* HTXF transfer tasks (the ones xfer_go fires for FILE_GET /
		 * FILE_PUT, identified by the "xfer_go" label) own an
		 * htxf_conn that needs to be reclaimed when the request
		 * errors — otherwise the orphaned transfer hangs in the
		 * tasks UI forever with no progress and no way to dismiss
		 * it. Their rcv functions (rcv_task_file_get /
		 * rcv_task_file_put) already check task_inerror internally
		 * and free the htxf on that path, so we run them on error
		 * too. Non-transfer handlers (login, user-info, news, …)
		 * don't have per-task state to free; the error toast above
		 * is enough and we skip them as before. */
        gboolean is_xfer = tsk->str && !strcmp (tsk->str, "xfer_go");
        if (tsk->rcv && (!error || is_xfer)) {
            tsk->rcv (htlc, tsk->ptr, tsk->data);
        }
        if (hxd_files[fd].conn.htlc) {
            task_delete (&the_session, tsk);
        }
    } else {
        /*	hx_printf_prefix(0, INFOPREFIX, "got task 0x%08x\n", trans); */
    }
}

void
hx_rcv_user_change (struct htlc_conn *htlc)
{
    struct hx_user_change_msg uc;
    struct chat *chat;
    struct hx_user *user;
    session *sess = &the_session;

    if (task_inerror (htlc)) {
        return;
    }

    if (!hx_user_change_extract (htlc, &uc)) {
        return;
    }

    /* Local aliases — keep the rest of the handler readable. */
    guint16 uid = uc.uid;
    guint16 icon = uc.icon;
    guint16 color = uc.color;
    gboolean got_color = uc.got_color;
    guint32 cid = uc.cid;
    char *name = uc.name;
    guint16 nlen = uc.name_len;

    /* Phase 5: self-detection by name. Some 1.9-style servers
	 * (e.g. The Mobius Strip) omit USER_LIST from SELFINFO, so
	 * htlc->uid stays 0 after login. The first USER_CHANGE
	 * broadcast we receive is the server echoing back the
	 * USER_CHANGE we just sent (post-SELFINFO) — its name
	 * matches our local htlc->name and carries our newly-assigned
	 * UID. Adopt that UID as ours. Without this, the rest of the
	 * handler treats the broadcast as a stranger joining: it
	 * adds a row before the USER_LIST reply arrives (so we end
	 * up at the top of the user list) and announces "join: <us>"
	 * in chat. */
    if (htlc->uid == 0 && uid != 0 && nlen > 0
        && strlen ((const char *)htlc->name) == (size_t)nlen
        && memcmp (htlc->name, name, nlen) == 0) {
        htlc->uid = uid;
        debug_log ("login",
                   "adopted self uid=%u from USER_CHANGE "
                   "broadcast (SELFINFO didn't carry it)",
                   (unsigned)uid);
    }
    gboolean is_self = (uid != 0 && uid == htlc->uid);

    chat = chat_with_cid (sess, cid);
    if (!chat) {
        chat = chat_new (sess, cid);
    }
    user = hx_user_with_uid (chat, uid);
    if (!user) {
        if (is_self) {
            /* Don't add our own row here. The USER_LIST reply
			 * (or any subsequent broadcast that mentions us)
			 * will create it in the proper position. Adding it
			 * now would put us at the top of the user list and
			 * spam a "join: <us>" line in chat. */
            return;
        }
        user = hx_user_new (chat, uid);
        chat->nusers++;
        gtkhx_session_emit_user_create (gtkhx_session_get_default (), htlc,
                                        chat, user, name, icon, color);
        play_sound (USER_JOIN);
        if (gtkhx_prefs.showjoin) {
            hx_printf_prefix (htlc, cid, INFOPREFIX, _ ("join: %s\n"), name);
        }
    }

    else {
        if (!got_color) {
            color = user->color;
        }
        gtkhx_session_emit_user_change (gtkhx_session_get_default (), htlc,
                                        chat, user, name, icon, color);
        /* Phase 5: print "X is now known as Y" only when the name
		 * actually changed AND it isn't us. Suppressing the self
		 * case keeps the post-SELFINFO USER_CHANGE we push (to set
		 * our nick on the server) from spamming a redundant
		 * "misha is now known as misha" notice. Also bail on
		 * ignored users early so we neither toast nor log them. */
        if (user->ignore) {
            return;
        }
        if (uid != htlc->uid && nlen > 0 && strcmp (name, user->name) != 0) {
            hx_printf_prefix (htlc, cid, INFOPREFIX,
                              _ ("%s is now known as %s\n"), user->name, name);
        }
    }
    if (nlen) {
        memcpy (user->name, name, nlen);
        user->name[nlen] = 0;
    }
    if (icon) {
        user->icon = icon;
    }
    if (got_color) {
        user->color = color;
    }
    if ((uid) && (uid == htlc->uid)) {
        htlc->icon = user->icon;
        htlc->color = user->color;
        /* Phase 5: deliberately do NOT copy user->name into
		 * htlc->name. Servers can legitimately override a user's
		 * display name — guests get pinned to things like "Read
		 * the agreement" before they have HL_ACCESS_USERNAME_CHANGE
		 * — and that override should appear in the user list (which
		 * user->name already feeds) but must not bleed into our
		 * htlc->name buffer, which doubles as the persisted NICK=
		 * prefs value. Letting the server's override land in
		 * htlc->name and then prefs_write persists 'Read the
		 * agreement' as the user's nick forever.
		 *
		 * Display paths read user_list entries (chat output, user
		 * window, etc.); htlc->name is reserved for the wire-side
		 * USER_CHANGE we *send* and the gtkhxrc persistence. The
		 * two diverging is exactly the model the protocol expects. */
        if (uid && uid == htlc->uid) {
            gsize unlen = strlen (user->name);
            debug_log ("name",
                       "USER_CHANGE for our uid=%u: server says "
                       "'%.*s' (%zu bytes); keeping local "
                       "htlc->name = '%s'",
                       (unsigned)uid, (int)unlen, user->name, (size_t)unlen,
                       htlc->name);
        }
    }
}

void
hx_rcv_user_part (struct htlc_conn *htlc)
{
    struct hx_user_part_msg pm;
    struct chat *chat;
    struct hx_user *user;
    session *sess = &the_session;

    if (!hx_user_part_extract (htlc, &pm)) {
        return;
    }

    chat = chat_with_cid (sess, pm.cid);
    if (!chat) {
        return;
    }

    user = hx_user_with_uid (chat, pm.uid);
    if (user) {
        gtkhx_session_emit_user_delete (gtkhx_session_get_default (), htlc,
                                        chat, user);

        if (gtkhx_prefs.showjoin) {
            hx_printf_prefix (htlc, pm.cid, INFOPREFIX, _ ("parts: %s \n"),
                              user->name);
        }

        hx_user_delete (chat, user);
        chat->nusers--;
        play_sound (USER_PART);
    }
}

void
hx_rcv_chat_subject (struct htlc_conn *htlc)
{
    struct hx_chat_subject_msg sm;
    struct chat *chat;
    session *sess = &the_session;

    if (!hx_chat_subject_extract (htlc, &sm)) {
        return;
    }

    if (sm.subject_len) {
        chat = chat_with_cid (sess, sm.cid);
        if (!chat) {
            return;
        }
        if (strcmp (sm.subject, chat->subject) == 0) {
            return;
        }
        memcpy (chat->subject, sm.subject, sm.subject_len);
        chat->subject[sm.subject_len] = 0;

#ifdef USE_PLUGIN
        if (EMIT_SIGNAL (XP_RCV_SUBJ, sess, chat->subject, &sm.cid, 0, 0, 0)
            == 1) {
            return;
        }
#endif
        /* Update the subject widget (pure view), then log the
		 * change as a chat line. Splitting the two means the
		 * initial-subject-discovery path (rcv_task_user_list's
		 * HTLS_DATA_CHAT_SUBJECT chunk) can call only the widget
		 * update without spamming a "Subject Changed to X" line
		 * for a subject that, from the user's point of view, was
		 * already there before they joined. */
        gtkhx_session_emit_chat_subject (gtkhx_session_get_default (), htlc,
                                         sm.cid, chat->subject);
        hx_printf_prefix (htlc, sm.cid, INFOPREFIX, "%s: %s",
                          _ ("Subject Changed to"), chat->subject);
    }
}

void
hx_rcv_banner (struct htlc_conn *htlc)
{
    struct hx_banner_msg bm;

    /* Phase 5: HTLS_HDR_BANNER arrives unsolicited from the server
	 * after the AGREEMENTAGREE round-trip. Parse the type +
	 * optional URL and hand off to banner.c, which owns the
	 * toolbar widget and the URL/HTXF fetch state machines. */
    if (!hx_banner_extract (htlc, &bm)) {
        return;
    }

    banner_handle_message (htlc, bm.type, bm.has_url,
                           bm.has_url ? bm.url : NULL);
}

void
hx_rcv_chat_invite (struct htlc_conn *htlc)
{
    struct hx_chat_invite_msg im;
    session *sess = &the_session;
    struct chat *chat = chat_with_cid (sess, 0);
    struct hx_user *user = 0;

    if (!hx_chat_invite_extract (htlc, &im)) {
        return;
    }

    user = hx_user_with_uid (chat, im.uid);
    if (user && user->ignore) {
        return;
    }
#ifdef USE_PLUGIN
    if (EMIT_SIGNAL (XP_RCV_INVITE, sess, im.name, &im.uid, &im.cid, 0, 0)
        == 1) {
        return;
    }
#endif
    gtkhx_session_emit_chat_invitation (gtkhx_session_get_default (), htlc,
                                        im.cid, im.name);
    play_sound (CHAT_INVITE);
}

void
hx_rcv_user_selfinfo (struct htlc_conn *htlc)
{
    /* The chunk walker (parses HTLS_DATA_ACCESS + HTLS_DATA_USER_LIST
	 * into htlc->access / uid / icon) is in proto_helpers.c so the
	 * Tier 2 unit tests can drive it without GTK. NB: the parser
	 * deliberately ignores the server-supplied name bytes (see the
	 * comment there) — we treat our local prefs nick as authoritative
	 * and push it back to the server immediately below. */
    hx_selfinfo_parse (htlc);

    /* Phase 5: SELFINFO is the canonical 'login complete' signal.
	 * Track it on htlc->flags so the agreement Agree button can
	 * tell whether to send AGREEMENTAGREE. See the comment on the
	 * flag in protocol.h for the legacy-vs-1.9 reasoning. */
    htlc->flags.logged_in = 1;

    setbtns (&the_session, 1);

    /* Note: SELFINFO is NOT where we fire post-login fetches. In
	 * the 1.5 flow SELFINFO (TranUserAccess) arrives BEFORE the
	 * server sends the agreement — firing USER_GETLIST / news here
	 * would land them at the server before our AGREEMENTAGREE, so
	 * the server logs the action against the not-yet-joined session.
	 * fogWraith caught this on Mobius (Classic Macs / MacSecret /
	 * vespernet) where the server-side log shows "Get user list"
	 * arriving before "Accept agreement". The fetches now fire from
	 * hx_send_agreement_agree, after AGREEMENTAGREE is on the wire.
	 *
	 * Phase 5+: the SELFINFO USE_ANY_NAME auto-push that used to
	 * live here is gone. It existed to deliver NAME + ICON to the
	 * server on flows where AGREEMENTAGREE couldn't be relied on:
	 *
	 *   - 1.9-style servers where SELFINFO arrived first and the
	 *     concurrence() click path was sending USER_CHANGE instead
	 *     of AGREEMENTAGREE (to dodge a misdiagnosed Mobius
	 *     disconnect bug). See gtkhx_mobius_options_field memory.
	 *   - no-agreement servers where the user has nothing to
	 *     click Agree on.
	 *
	 * Both cases are now handled by AGREEMENTAGREE itself:
	 *   - concurrence() always sends AGREEMENTAGREE on Agree click
	 *     (with NAME + ICON + OPTIONS chunks).
	 *   - hx_rcv_agreement_file's HX_AGREEMENT_NONE / NOT_FOUND
	 *     branch auto-sends AGREEMENTAGREE when there's no
	 *     agreement to display.
	 *
	 * Keeping the auto-push here would have it fire alongside the
	 * Agree click on 1.9 servers, producing two redundant NAME +
	 * ICON deliveries plus a USER_CHANGE broadcast race that nudged
	 * us to the top of the local user_list. Cleaner without it. */
}

void
hx_rcv_dump (struct htlc_conn *htlc)
{
    int fd;
    ssize_t n;

    fd = open ("hx.dump", O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        return;
    }
    /* Best-effort diagnostic dump — if the write fails or comes up
	 * short there's no recovery path, but we shouldn't silently
	 * pretend it succeeded either. */
    n = write (fd, htlc->in.buf, htlc->in.pos);
    if (n != (ssize_t)htlc->in.pos) {
        g_warning ("hx_rcv_dump: short write to hx.dump");
    }
    fsync (fd);
    close (fd);
}

void
hx_rcv_xfer_queue (struct htlc_conn *htlc)
{
    struct hx_xfer_queue_msg xq;
    struct htxf_conn *htxf;

    if (!hx_xfer_queue_extract (htlc, &xq)) {
        return;
    }

    htxf = htxf_with_ref (xq.ref);

    if (!htxf) {
        g_warning (_ ("Received queue id (%d) for xfer ref %d\n"
                      "No such xfer.\n"),
                   xq.queueid, xq.ref);
        return;
    }
    htxf->queue = xq.queueid;
    gtkhx_session_emit_xfer_queue (gtkhx_session_get_default (), &the_session,
                                   htxf);

    if (!htxf->queue) {
        xfer_ready_write (htxf);
    }
}

void
hx_rcv_hdr (struct htlc_conn *htlc)
{
    /* Shared header decoder in proto_helpers — same math the
	 * integration harness's integration_recv_message uses. Returns
	 * the raw wire_len (for the proto trace) plus the clamped body
	 * payload size (what the receive state machine needs to read
	 * next). */
    guint32 wire_len, len, type, trace_trans, trace_flag;
    hl_hdr_decode (htlc->in.buf, &type, &trace_trans, &trace_flag, NULL,
                   &wire_len, &len);
    proto_trace_recv_hdr (type, trace_trans, trace_flag, wire_len);

    /* htlc->trans = ntohl(h->trans); */
    htlc->rcv = 0;
    switch (type) {
    case HTLS_HDR_CHAT:
        htlc->rcv = hx_rcv_chat;
        break;
    case HTLS_HDR_MSG:
        htlc->rcv = hx_rcv_msg;
        break;
    case HTLS_HDR_USER_CHANGE:
    case HTLS_HDR_CHAT_USER_CHANGE:
        htlc->rcv = hx_rcv_user_change;
        break;
    case HTLS_HDR_USER_PART:
    case HTLS_HDR_CHAT_USER_PART:
        htlc->rcv = hx_rcv_user_part;
        break;
    case HTLS_HDR_NEWS_POST:
        htlc->rcv = hx_rcv_news_post;
        break;
    case HTLS_HDR_TASK:
        htlc->rcv = hx_rcv_task;
        break;
    case HTLS_HDR_CHAT_SUBJECT:
        htlc->rcv = hx_rcv_chat_subject;
        break;
    case HTLS_HDR_CHAT_INVITE:
        htlc->rcv = hx_rcv_chat_invite;
        break;
    case HTLS_HDR_MSG_BROADCAST:
        htlc->rcv = hx_rcv_msg;
        break;
    case HTLS_HDR_USER_SELFINFO:
        htlc->rcv = hx_rcv_user_selfinfo;
        break;
    case HTLS_HDR_AGREEMENT:
        htlc->rcv = hx_rcv_agreement_file;
        break;
    case HTLS_HDR_BANNER:
        htlc->rcv = hx_rcv_banner;
        break;
    case HTLS_HDR_POLITEQUIT:
        hx_printf_prefix (htlc, 0, INFOPREFIX, _ ("polite quit\n"));
        htlc->rcv = hx_rcv_msg;
        break;
    case HTLS_HDR_QUEUE:
        htlc->rcv = hx_rcv_xfer_queue;
        break;
    default:
        g_print ("0x%08x\n", type);
        hx_printf_prefix (htlc, 0, INFOPREFIX,
                          _ ("unknown header type 0x%08x\n"), type);
        htlc->rcv = hx_rcv_dump;
        break;
    }

    if (len) {
        qbuf_set (&htlc->in, htlc->in.pos, len);
    } else {
        if (htlc->rcv) {
            htlc->rcv (htlc);
        }
        htlc->rcv = hx_rcv_hdr;
        qbuf_set (&htlc->in, 0, SIZEOF_HL_HDR);
    }
}

void
rcv_task_user_open (struct htlc_conn *htlc, struct uesp_fn *uespfn)
{
    char name[32], login[32], pass[32];
    guint16 nlen = 0, llen = 0, plen = 0;
    hl_access_bits access;
    char accessbool = 0;

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_NAME:
            if (_len >= sizeof (name)) {
                nlen = sizeof (name) - 1;
            } else {
                nlen = _len;
            }
            memcpy (name, dh->data, nlen);
            break;
        case HTLS_DATA_LOGIN:
            if (_len >= sizeof (login)) {
                llen = sizeof (login) - 1;
            } else {
                llen = _len;
            }
            hl_decode (login, dh->data, llen);
            break;
        case HTLS_DATA_PASSWORD:
            if (_len >= sizeof (pass)) {
                plen = sizeof (pass) - 1;
            } else {
                plen = _len;
            }
            if (plen > 1 && dh->data[0]) {
                hl_decode (pass, dh->data, plen);
            } else {
                pass[0] = 0;
            }
            break;
        case HTLS_DATA_ACCESS:
            if (_len >= sizeof (access)) {
                accessbool = 1;
                memcpy (&access, dh->data, sizeof (access));
            }
            break;
        }
    }
    dh_end ();

    name[nlen] = 0;
    login[llen] = 0;
    pass[plen] = 0;
    if (accessbool) {
        uespfn->fn (uespfn->uesp, name, login, pass, access);
    }
    g_free (uespfn);
}

void
rcv_task_msg (struct htlc_conn *htlc, char *msg_buf)
{
    if (msg_buf) {
        hx_printf (htlc, 0, "%s\n", msg_buf);
        g_free (msg_buf);
    }
}

/* Translate one parsed hx_newscat_post into the GUI's news_item. The
 * field shapes match 1:1 except for the parts array (which we copy
 * shallowly and steal the mime_type pointers from) and the GTK-only
 * `iter` field (zeroed; news15.c populates it later). The `group`
 * back-pointer is set by the caller. */
static void
news_item_take_from_wire (struct news_item *ni, struct hx_newscat_post *p)
{
    guint16 j;

    ni->postid = p->postid;
    ni->parentid = p->parentid;
    ni->date.base_year = p->date_base_year;
    ni->date.pad = p->date_pad;
    ni->date.seconds = p->date_seconds;
    ni->partcount = p->partcount;
    ni->size = p->size_total;

    /* Steal ownership of subject + sender strings — the wire struct
	 * will be cleared next so it won't double-free. */
    ni->subject = p->subject;
    ni->sender = p->sender;
    p->subject = p->sender = NULL;

    if (p->partcount) {
        ni->parts = g_new0 (struct news_parts, p->partcount);
        for (j = 0; j < p->partcount; j++) {
            ni->parts[j].size = p->parts[j].size;
            ni->parts[j].mime_type = p->parts[j].mime_type;
            p->parts[j].mime_type = NULL; /* stolen */
        }
    } else {
        ni->parts = NULL;
    }

    memset (&ni->iter, 0, sizeof (ni->iter));
}

void
rcv_task_newscat_list (struct htlc_conn *htlc, struct gnews_catalog *gcnews)
{
    struct news_group *group = g_malloc0 (sizeof (struct news_group));
    struct hx_newscat parsed;
    guint32 i;

    if (!hx_newscat_parse (htlc, &parsed)) {
        /* No CATLIST chunk or malformed payload. Surface an empty
		 * group rather than a NULL — preserves the original
		 * behaviour (the parser bailed out of the loop and still
		 * emitted an empty signal payload). */
        group->post_count = 0;
        group->posts = NULL;
        gcnews->group = group;
        gtkhx_session_emit_news_catalog (gtkhx_session_get_default (), gcnews);
        return;
    }

    group->post_count = parsed.post_count;
    if (parsed.post_count) {
        group->posts = g_new0 (struct news_item, parsed.post_count);
        for (i = 0; i < parsed.post_count; i++) {
            news_item_take_from_wire (&group->posts[i], &parsed.posts[i]);
            group->posts[i].group = group;
        }
    } else {
        group->posts = NULL;
    }
    hx_newscat_clear (&parsed);

    gcnews->group = group;
    gtkhx_session_emit_news_catalog (gtkhx_session_get_default (), gcnews);
}

void
rcv_task_newsfolder_list (struct htlc_conn *htlc, struct gnews_folder *gfnews)
{
    struct news_folder *folder = g_malloc (sizeof (struct news_folder));
    struct folder_item *item;
    int num = 0;

    folder->entry = g_malloc (sizeof (struct folder_item *));
    folder->path = gfnews->path;

    dh_start (htlc)
    {
        struct hx_news_dirlist_entry entry;
        gboolean got = FALSE;

        /* Either chunk type can carry either a folder-entry or a
		 * category-entry; both parsers normalise to entry.kind. */
        switch (_type) {
        case HTLC_DATA_NEWSFOLDERITEM:
            got = hx_news_dirlist_parse_folderitem (dh->data, _len, &entry);
            break;
        case HTLC_DATA_CATEGORYITEM:
            /* Same listing reply but with per-category sync
			 * metadata (GUID + add/delete SNs). Some servers
			 * emit this instead of NEWSFOLDERITEM. */
            got = hx_news_dirlist_parse_categoryitem (dh->data, _len, &entry);
            break;
        }
        if (!got) {
            continue;
        }

        num++;
        folder->entry
            = g_realloc (folder->entry, sizeof (struct folder_item *) * num);
        item = g_malloc (sizeof (struct folder_item));
        item->type = entry.kind;
        item->name = g_strndup (entry.name, entry.name_len);
        folder->entry[num - 1] = item;
    }
    dh_end ();

    folder->num_entries = num;

    gfnews->news = folder;
    gtkhx_session_emit_news_folder (gtkhx_session_get_default (), gfnews);
}

void
rcv_task_news_post (struct htlc_conn *htlc, struct news_item *item)
{
    struct news_post *post = 0;
    guint32 postid;

    dh_start (htlc)
    {
        switch (_type) {
        case HTLC_DATA_NEWSDATA:
            post = g_malloc (sizeof (struct news_post));
            post->buf = g_malloc (_len + 1);
            memcpy (post->buf, dh->data, _len);
            CR2LF (post->buf, _len);
            strip_ansi (post->buf, _len);
            post->buf[_len] = 0;
            break;
        case HTLC_DATA_THREADID:
            dh_getint (postid);
            break;
        case HTLS_DATA_TASKERROR:
            return;
        }
    }
    dh_end ();

    post->item = item;
    gtkhx_session_emit_news_thread (gtkhx_session_get_default (), post);
}

void
rcv_task_news_users (struct htlc_conn *htlc, struct chat *chat, int text)
{
    /* output user list and then grab news */
    /* this is only used for login events  */
    rcv_task_user_list (htlc, chat, text);

    reload_news (0, &the_session);
}

void
rcv_task_login (struct htlc_conn *htlc, char *pass)
{
    char buf[HOSTLEN];
    guint16 uid;
    guint16 version;
    guint16 len;
    char servername[8192 + 1];
    session *sess = &the_session;

    g_strlcpy (buf, htlc->ip_addr[0] ? htlc->ip_addr : "?", sizeof (buf));

    if (!pass) {
        hx_printf_prefix (htlc, 0, INFOPREFIX, "%s:%u: %s %s\n", buf,
                          htlc->serverport, _ ("login"),

                          task_inerror (htlc) ? _ ("failed?")
                                              : _ ("successful"));
    }

    if (pass) {
        /* HOPE Step 1 reply handling.
		 *
		 * The data layer (chunk walking, algorithm-name extraction,
		 * MAC-chain crypto, session-key validation, login-field
		 * encoding, reply-alg-list packing) lives in src/hope.c —
		 * we hand it the htlc + a few client-side knobs and it
		 * returns parsed data. Side effects (UI logging, error
		 * tear-down, hlwrite, cipher/compress init) stay here in
		 * the orchestrator. */
        struct hope_step1_reply sel;
        enum hope_step1_err herr
            = hope_parse_step1_reply (htlc, htlc->macalg, &sel);

        if (herr == HOPE_ERR_NO_MAC_ALG || herr == HOPE_ERR_BAD_MAC_ALG) {
            hx_printf_prefix (htlc, 0, INFOPREFIX, "No macalg from server\n");
            hx_htlc_close (htlc, 0);
            return;
        }
        if (herr == HOPE_ERR_SHORT_SESSIONKEY) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "sessionkey length (%u) not big enough\n",
                              htlc->sklen);
            hx_htlc_close (htlc, 0);
            return;
        }
        if (herr != HOPE_OK) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "HOPE Step 1 parse error %d\n", (int) herr);
            hx_htlc_close (htlc, 0);
            return;
        }
        /* hope_parse_step1_reply outputs the server-picked macalg
		 * in sel.macalg; copy back into htlc->macalg so the rest
		 * of the code (and downstream messages) sees the agreed
		 * value rather than our pre-negotiation preference. */
        g_strlcpy (htlc->macalg, sel.macalg, sizeof (htlc->macalg));

        /* Session-key IP:port advisory check. NAT or MITM produces a
		 * mismatch; warn but continue (shxd-family clients disconnect
		 * — friendlier behind home-NAT). */
        char ip_warn[256];
        if (hope_validate_sessionkey_ip (htlc->sessionkey, htlc->sklen,
                                         htlc->ip_addr, htlc->serverport,
                                         ip_warn, sizeof (ip_warn))) {
            hx_printf_prefix (htlc, 0, INFOPREFIX, "%s", ip_warn);
        }

        if (task_inerror (htlc)) {
            g_free (pass);
            hx_htlc_close (htlc, 0);
            return;
        }
        task_new (htlc, RCV_TASK_FN (rcv_task_login), 0, 0, "login");
        guint16 icon16 = htons (htlc->icon);

        /* HTLC_DATA_LOGIN field encoding (HMAC variant for
		 * secure_login, hl_code XOR otherwise). */
        guint8 login[32];
        size_t llen = hope_build_login_field (
            htlc->login, sel.secure_login,
            htlc->sessionkey, htlc->sklen, htlc->macalg,
            login, sizeof (login));
        if (!llen) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "bad HMAC algorithm %s\n", htlc->macalg);
            hx_htlc_close (htlc, 0);
            return;
        }

        /* HMAC chain: password_mac + spec encode_key + spec decode_key.
		 * Spec-aligned outputs; we map into htlc->cipher_{en,de}code_key
		 * with the legacy-storage swap below (encode_key → decode slot,
		 * decode_key → encode slot — see hope.h's docstring). */
        uint8_t password_mac[HOPE_MAC_MAX];
        uint8_t spec_encode_key[HOPE_MAC_MAX];
        uint8_t spec_decode_key[HOPE_MAC_MAX];
        size_t pmaclen = hope_compute_chain (
            pass, htlc->sessionkey, htlc->sklen, htlc->macalg,
            password_mac, spec_encode_key, spec_decode_key);
        if (!pmaclen) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "bad HMAC algorithm %s\n", htlc->macalg);
            hx_htlc_close (htlc, 0);
            return;
        }

        guint16 hc = 4;

#ifdef CONFIG_COMPRESS
        guint8 compressalglist[64];
        size_t compressalglistlen = 0;
        if (!htlc->compressalg[0] || !strcmp (htlc->compressalg, "NONE")) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "WARNING: this connection is not compressed\n");
        } else if (!sel.s_compressalg[0] || !sel.c_compressalg[0]) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "No compress algorithm from server\n");
            hx_htlc_close (htlc, 0);
            return;
        } else if (!valid_compress (sel.c_compressalg)) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "Bad client compress algorithm %s\n",
                              sel.c_compressalg);
            hx_htlc_close (htlc, 0);
            return;
        } else if (!valid_compress (sel.s_compressalg)) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "Bad server compress algorithm %s\n",
                              sel.s_compressalg);
            hx_htlc_close (htlc, 0);
            return;
        } else {
            compressalglistlen = hope_build_alg_reply (
                sel.s_compressalg, compressalglist, sizeof (compressalglist));
        }
        hc++;
#endif

#ifdef CONFIG_CIPHER
        guint8 cipheralglist[64];
        size_t cipheralglistlen = 0;
        if (!htlc->cipheralg[0] || !strcmp (htlc->cipheralg, "NONE")) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "WARNING: this connection is not encrypted\n");
        } else if (!sel.s_cipheralg[0] || !sel.c_cipheralg[0]) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "No cipher algorithm from server\n");
            hx_htlc_close (htlc, 0);
            return;
        } else if (!valid_cipher (sel.c_cipheralg)) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "Bad client cipher algorithm %s\n",
                              sel.c_cipheralg);
            hx_htlc_close (htlc, 0);
            return;
        } else if (!valid_cipher (sel.s_cipheralg)) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "Bad server cipher algorithm %s\n",
                              sel.s_cipheralg);
            hx_htlc_close (htlc, 0);
            return;
        } else {
            cipheralglistlen = hope_build_alg_reply (
                sel.s_cipheralg, cipheralglist, sizeof (cipheralglist));
            /* Map spec-aligned chain outputs into GtkHx's legacy
			 * storage convention. The labels are intentionally
			 * swapped — see hope.h's hope_compute_chain docstring
			 * for why this isn't symmetric with the function's
			 * output naming. */
            memcpy (htlc->cipher_decode_key, spec_encode_key, pmaclen);
            htlc->cipher_decode_keylen = pmaclen;
            memcpy (htlc->cipher_encode_key, spec_decode_key, pmaclen);
            htlc->cipher_encode_keylen = pmaclen;
        }
        hc++;
#endif

        /* DATA_CAPABILITIES on the HOPE Step 2 authenticated LOGIN
		 * — mirror the legacy LOGIN's set (network.c around line
		 * 1159): large-files, text-encoding, chat-history. */
        guint16 caps16 = htons (HTLC_CAP_LARGE_FILES
                              | HTLC_CAP_TEXT_ENCODING
                              | HTLC_CAP_CHAT_HISTORY);
        hc++;
        hlwrite (htlc, HTLC_HDR_LOGIN, 0, hc, HTLC_DATA_LOGIN, (int) llen,
                 login, HTLC_DATA_PASSWORD, (int) pmaclen, password_mac,
#ifdef CONFIG_CIPHER
                 HTLS_DATA_CIPHER_ALG, (int) cipheralglistlen, cipheralglist,
#endif
#ifdef CONFIG_COMPRESS
                 HTLS_DATA_COMPRESS_ALG, (int) compressalglistlen,
                 compressalglist,
#endif
                 HTLC_DATA_NAME, strlen (htlc->name), htlc->name,
                 HTLC_DATA_ICON, 2, &icon16, HTLC_DATA_CAPABILITIES, 2,
                 &caps16);
        g_free (pass);

#ifdef CONFIG_COMPRESS
        if (compressalglistlen) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "compress: server %s client %s\n",
                              sel.c_compressalg, sel.s_compressalg);
            htlc->compress_encode_type
                = compress_id_from_name (sel.c_compressalg);
            compress_encode_init (htlc);
            htlc->compress_decode_type
                = compress_id_from_name (sel.s_compressalg);
            compress_decode_init (htlc);
        }
#endif

#ifdef CONFIG_CIPHER
        if (cipheralglistlen) {
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "cipher: server %s client %s\n",
                              sel.c_cipheralg, sel.s_cipheralg);
            htlc->cipher_decode_type
                = hope_cipher_id_from_name (sel.s_cipheralg);
            htlc->cipher_encode_type
                = hope_cipher_id_from_name (sel.c_cipheralg);

            /* HOPE-ChaCha20-Poly1305: when both directions resolve
			 * to CHACHA20, flip cipher_mode to AEAD and stretch
			 * the MAC-derived keys to 256 bits via HKDF-SHA256.
			 * The spec's "encode_key" goes into our DECODE slot
			 * and vice versa — server's outbound key is what we
			 * use to read; see cipher_aead_derive_session_keys'
			 * docstring for the full rationale. We pass our
			 * (already mapped) storage slots in the spec-aligned
			 * argument order. */
            if (hope_cipher_is_aead (sel.s_cipheralg)
                && hope_cipher_is_aead (sel.c_cipheralg)) {
                htlc->cipher_mode = sel.server_says_aead
                                      ? CIPHER_MODE_AEAD
                                      : CIPHER_MODE_AEAD;
                cipher_aead_derive_session_keys (
                    &htlc->cipher_encode_state.chacha,
                    &htlc->cipher_decode_state.chacha,
                    htlc->sessionkey, htlc->sklen,
                    htlc->cipher_decode_key, htlc->cipher_decode_keylen,
                    htlc->cipher_encode_key, htlc->cipher_encode_keylen);
            } else {
                htlc->cipher_mode = CIPHER_MODE_STREAM;
            }
            cipher_encode_init (htlc);
            cipher_decode_init (htlc);
        }
#endif
    } else {
        if (!task_inerror (htlc)) {
            play_sound (LOGIN);
            changetitlesconnected (sess);
            setbtns (sess, 1);
            set_status_bar (2);
            connected = 1;

            /* Reset post-login fetch state before scheduling so
			 * a reconnection during this process state starts clean.
			 *
			 * The check on already_fetched used to cover a race where
			 * SELFINFO arrived before the login TASK reply and fired
			 * the fetches already; that race no longer matters because
			 * SELFINFO is not a fetch trigger anymore (fetches fire
			 * from hx_send_agreement_agree). The check is harmless
			 * to keep — it's a no-op when post_login_fetched is FALSE,
			 * which is the new common case. */
            gboolean already_fetched = post_login_fetched;
            if (!already_fetched) {
                post_login_fetched = FALSE;
                if (post_login_timer_id) {
                    g_source_remove (post_login_timer_id);
                    post_login_timer_id = 0;
                }
            }

            dh_start (htlc)
            {
                switch (_type) {
                case HTLS_DATA_UID:
                    dh_getint (uid);
                    htlc->uid = uid;
                    break;
                case HTLS_DATA_VERSION: /* Hotline 1.5+ servers only */
                    dh_getint (version);
                    htlc->version = version;
                    break;
                case HTLS_DATA_SERVERNAME: /* Hotline 1.5+ servers only */
                    len = (_len > sizeof (servername) - 1)
                              ? sizeof (servername) - 1
                              : _len;
                    memcpy (servername, dh->data, len);
                    CR2LF (servername, len);
                    strip_ansi (servername, len);
                    servername[len] = 0;
                    if (server_addr) {
                        g_free (server_addr);
                    }
                    /* Phase 5: server names from old Hotline servers are
					 * 8-bit Mac Roman text, not UTF-8 — and gtk_window_set_title
					 * et al. assert UTF-8. gtkhx_text_to_utf8 handles the
					 * already-UTF-8 / Mac-Roman / fall-back-to-substitute
					 * cascade. */
                    server_addr = gtkhx_text_to_utf8 (
                        servername, strlen (servername), NULL);
                    changetitlesconnected (sess);
                    break;
                case HTLS_DATA_CAPABILITIES:
                    /* DATA_CAPABILITIES echo from the server — the bits
					 * the server agreed to enable for this session.
					 * Per spec the field is a variable-width big-endian
					 * unsigned integer (typically 2 bytes, extensible
					 * to 8). Decode whatever width arrived into our 64-
					 * bit field. Bits we don't recognise are silently
					 * preserved per the spec's "ignore unknown bits"
					 * requirement — they don't affect behaviour but
					 * leave the door open if a server advertises a cap
					 * we'll start using later. */
                    {
                        guint64 caps = 0;
                        for (guint16 i = 0; i < _len && i < 8; i++) {
                            caps = (caps << 8) | dh->data[i];
                        }
                        htlc->caps = caps;
                        if (caps & HTLC_CAP_LARGE_FILES) {
                            hx_printf_prefix (htlc, 0, INFOPREFIX,
                                              "server confirmed large-file "
                                              "(64-bit) mode for this "
                                              "session\n");
                        }
                        if (caps & HTLC_CAP_TEXT_ENCODING) {
                            hx_printf_prefix (htlc, 0, INFOPREFIX,
                                              "server confirmed UTF-8 text "
                                              "encoding for this session\n");
                        }
                        if (caps & HTLC_CAP_CHAT_HISTORY) {
                            hx_printf_prefix (htlc, 0, INFOPREFIX,
                                              "server confirmed chat-history "
                                              "extension for this session\n");
                        }
                    }
                    break;
                case HTLS_DATA_HISTORY_MAX_MSGS:
                    /* Chat-history retention hint — server's max
					 * message count. uint32 big-endian; 0 means
					 * unlimited. Spec note: hints only, the
					 * authoritative end-of-history signal is
					 * DATA_HISTORY_HAS_MORE = 0 in TRAN 700 replies. */
                    if (_len >= 4) {
                        guint32 v;
                        HN32 (&v, dh->data);
                        htlc->history_max_msgs = v;
                    }
                    break;
                case HTLS_DATA_HISTORY_MAX_DAYS:
                    if (_len >= 4) {
                        guint32 v;
                        HN32 (&v, dh->data);
                        htlc->history_max_days = v;
                    }
                    break;
                }
            }
            dh_end ();

            /* Phase 5: PING keepalive only on confirmed 1.5+ servers.
			 * htlc->version is populated by the HTLS_DATA_VERSION
			 * chunk just parsed above; servers that don't advertise
			 * a version (1.0/1.2 originals like hlserver.com) leave
			 * it at 0, and sending HTLC_HDR_PING to them earns a
			 * task-error toast every minute ("Uh, no.") plus the
			 * ERROR sound. >= 150 is the bar — that covers every
			 * server we've seen (Badmoon at 190, mhxd at 150+) that
			 * implements PING, and excludes the ones that don't. */
            if (htlc->version >= 150) {
                ping_start (htlc);
            }

            /* 1.0/1.2 detection: the server did not include an
			 * HTLS_DATA_VERSION chunk in this LOGIN reply, so it
			 * doesn't speak the 1.5 agreement / AGREEMENTAGREE
			 * flow. The LOGIN packet we sent followed the 1.5
			 * spec (no HTLC_DATA_NAME), so the server has no name
			 * for us yet — USER_GETLIST replies would return our
			 * record with an uninitialised name field (fogWraith's
			 * hlserver.com trace showed exactly this: "00 07 00
			 * 86 00 00 00 05 f0 d0 73 28 2d"). Deliver NAME + ICON
			 * via USER_CHANGE now, and fire the post-login fetches
			 * immediately — no agreement is coming, so there is no
			 * "after AGREEMENTAGREE" boundary to wait for.
			 *
			 * 1.5+ servers (version >= 150 here) take the AGREEMENT-
			 * AGREE path: gtkhx.c::concurrence on the Agree click,
			 * or hx_rcv_agreement_file's HX_AGREEMENT_NONE auto-
			 * send when the account has AccessNoAgreement. Both
			 * call hx_post_login_fetches after the wire send. The
			 * 2s fallback timer below arms as a last resort if the
			 * agreement opcode doesn't arrive at all. */
            if (htlc->version == 0 && !already_fetched) {
                hx_change_name_icon (htlc);
                hx_post_login_fetches (htlc);
            } else if (!already_fetched) {
                /* Phase 5: do NOT fire HTLC_HDR_USER_GETLIST yet —
				 * wait for AGREEMENTAGREE to go out (or its no-
				 * agreement auto-send). The fallback timer covers
				 * 1.5+ servers that misbehave and don't send any
				 * agreement opcode. */
                post_login_timer_id
                    = g_timeout_add_seconds (2, post_login_fallback, htlc);
            }
        }
    }
}

void
rcv_task_news_file (struct htlc_conn *htlc)
{
    /* Phase 5: parse + sanitise in hx_news_file_extract. We still
	 * use the file-scope news_buf scratch as the destination so
	 * downstream-allocated callers reading news_len/news_buf get
	 * the same shape as before (the lifetime is "until the next
	 * NEWS_FILE arrives"). */
    gsize copied = 0;
    news_buf = g_realloc (news_buf, 65536);
    if (hx_news_file_extract (htlc, (char *)news_buf, 65536, &copied)) {
        news_len = copied;
    } else {
        news_len = 0;
        news_buf[0] = 0;
    }
    gtkhx_session_emit_news_file (gtkhx_session_get_default (), htlc,
                                  (char *)news_buf, news_len);
}

/* TRAN_GET_CHAT_HISTORY (700) reply walker. The reply carries:
 *   - 0..N HTLS_DATA_HISTORY_ENTRY (0x0F05) packed-binary chunks
 *   - 1 HTLS_DATA_HISTORY_HAS_MORE (0x0F06) u8 flag
 *
 * Channel id isn't repeated in the reply per the spec — we passed
 * it to task_new via GUINT_TO_POINTER and recover it here.
 *
 * Entries are parsed via hx_history_entry_parse, accumulated into
 * a GPtrArray with hx_history_entry_free as the free_func, and the
 * array (plus has_more) is handed to the chat-history-batch signal
 * subscribers. After every subscriber returns, the array is
 * unref'd and entries free along with it. */
void
rcv_task_chat_history (struct htlc_conn *htlc, void *channel_ptr)
{
    guint32 cid = GPOINTER_TO_UINT (channel_ptr);
    GPtrArray *entries
        = g_ptr_array_new_with_free_func ((GDestroyNotify) hx_history_entry_free);
    gboolean has_more = FALSE;

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_HISTORY_ENTRY: {
            HxHistoryEntry *e = hx_history_entry_parse (dh->data, _len);
            if (e) {
                g_ptr_array_add (entries, e);
            } else {
                debug_log ("chat-history",
                           "skipping malformed entry, len=%u", _len);
            }
            break;
        }
        case HTLS_DATA_HISTORY_HAS_MORE:
            if (_len >= 1) {
                has_more = (dh->data[0] != 0);
            }
            break;
        case HTLS_DATA_TASKERROR:
            /* Server refused the request — log and stop. The
			 * subscriber gets zero entries + has_more=FALSE, which
			 * is the same shape as "no history to return," and
			 * shouldn't make the UI do anything dramatic. */
            debug_log ("chat-history",
                       "server returned task error for GET_CHAT_HISTORY "
                       "(cid=%u, len=%u)", cid, _len);
            break;
        }
    }
    dh_end ();

    /* Phase 4: advance the session-wide newest-msgid cursor used
	 * for AFTER= reconnect catch-up. This is independent of the
	 * per-chat oldest-msgid (gtkhx_chat::history_oldest_msgid) the
	 * Load-older flow uses — the cursor we maintain here grows
	 * monotonically over the htlc's lifetime, while the per-chat
	 * oldest shrinks as new older batches arrive. */
    for (guint i = 0; i < entries->len; i++) {
        HxHistoryEntry *e = g_ptr_array_index (entries, i);
        if (e && e->message_id > htlc->chat_history_last_msgid) {
            htlc->chat_history_last_msgid = e->message_id;
        }
    }

    debug_log ("chat-history",
               "received batch: cid=%u entries=%u has_more=%d last_msgid=%"
               G_GUINT64_FORMAT,
               cid, entries->len, (int) has_more,
               htlc->chat_history_last_msgid);

    gtkhx_session_emit_chat_history_batch (gtkhx_session_get_default (), htlc,
                                           cid, entries, has_more);

    /* Free the array (and via free_func, every entry inside) now
	 * that subscribers have had their pass. */
    g_ptr_array_unref (entries);
}

void
rcv_task_user_list (struct htlc_conn *htlc, struct chat *chat, int text)
{
    struct hl_userlist_hdr *uh;
    struct hx_user *user;
    guint16 nlen, uid;
    int new;

    dh_start (htlc)
    {
        if (_type == HTLS_DATA_USER_LIST) {
            uh = (struct hl_userlist_hdr *)dh;
            HN16 (&uid, &uh->uid);
            user = hx_user_with_uid (chat, uid);
            /* Phase 5: reset `new` per chunk. Previously declared
			 * once at the top of the function and set to 1 inside
			 * the "user not found" branch, then never reset — so
			 * after the first new user in the response, every
			 * subsequent EXISTING user (found via hx_user_with_uid)
			 * inherited new=1 from the previous iteration and got
			 * a spurious hx_output.user_create call, doubling the
			 * UI row.
			 *
			 * Latent since the original handler; surfaced when
			 * hx_rcv_user_selfinfo started pushing USER_CHANGE
			 * before USER_GETLIST — that broadcast adds our own
			 * entry first, then USER_GETLIST returns [others, us]
			 * and the existing-us match was using the stale new=1
			 * from the first new other-user. */
            new = 0;
            if (!user) {
                new = 1;
                user = hx_user_new (chat, uid);
                chat->nusers++;
            }
            HN16 (&user->uid, &uh->uid);
            HN16 (&user->icon, &uh->icon);
            HN16 (&user->color, &uh->color);
            HN16 (&nlen, &uh->nlen);
            nlen = (nlen > 31) ? 31 : nlen;
            memcpy (user->name, uh->name, nlen);
            strip_ansi (user->name, nlen);
            user->name[nlen] = 0;
            if (!htlc->uid && !strcmp (user->name, htlc->name) &&

                user->icon == htlc->icon) {
                htlc->uid = user->uid;
                htlc->color = user->color;
            }
            if (new) {
                gtkhx_session_emit_user_create (gtkhx_session_get_default (),
                                                htlc, chat, user, user->name,
                                                user->icon, user->color);
            }
        }

        else if (_type == HTLS_DATA_CHAT_SUBJECT) {
            guint16 slen = (_len > 255) ? 255 : _len;
            memcpy (chat->subject, dh->data, slen);
            chat->subject[slen] = 0;
            /* Phase 5+ (MVC boundary): route through the view
			 * vtable rather than poking the subject widget
			 * directly. Initial-subject-discovery path — no
			 * 'Subject Changed to X' log line. */
            gtkhx_session_emit_chat_subject (gtkhx_session_get_default (), htlc,
                                             chat->cid, chat->subject);
        }
    }
    dh_end ();
}

void
rcv_task_user_list_switch (struct htlc_conn *htlc, struct chat *chat)
{
    session *sess = &the_session;

    if (task_inerror (htlc)) {
        chat_delete (sess, chat);
        return;
    }

    rcv_task_user_list (htlc, chat, 0);
}

void
rcv_task_kick (struct htlc_conn *htlc)
{
    if (task_inerror (htlc)) {
        return;
    }

    hx_printf_prefix (htlc, 0, INFOPREFIX, "%s\n", _ ("kick successful"));
}

void
rcv_task_user_info (struct htlc_conn *htlc, guint16 *_uid, int text)
{
    guint16 ilen = 0, nlen = 0;
    char info[4096 + 1], name[32];
    guint16 uid = *_uid;
    g_free (_uid);

    name[0] = 0;
    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_USER_INFO:
            ilen = (_len > 4096) ? 4096 : _len;
            memcpy (info, dh->data, ilen);
            info[ilen] = 0;
            break;
        case HTLS_DATA_NAME:
            nlen = (_len > 31) ? 31 : _len;
            memcpy (name, dh->data, nlen);
            name[nlen] = 0;
            strip_ansi (name, nlen);
            break;
        }
    }
    dh_end ();

    if (nlen && ilen) {
        CR2LF (info, ilen);
        strip_ansi (info, ilen);
        gtkhx_session_emit_user_info (gtkhx_session_get_default (), uid, name,
                                      info, ilen);
    }
}

void
rcv_task_file_list (struct htlc_conn *htlc, struct cached_filelist *cfl,
                    void *data)
{
    struct hl_filelist_hdr *fh = 0;
    guint32 fh_len;
    guint16 fhlen;

    if (task_inerror (htlc)) {
        /* Phase 5+: give the new-browser remote provider a chance
		 * to react before we drop the cfl. The helper marks the
		 * provider's listing_error flag and emits "navigated" so
		 * the panel can show an empty-state hint ("Folder is
		 * upload-only — drop files here to upload" if the access
		 * bits also indicate a drop-box). Returns FALSE silently
		 * when data isn't a HxRemoteFilesProvider (e.g. the
		 * recursive-download path uses data=NULL); we just free
		 * cfl regardless. */
        (void)hx_remote_files_provider_handle_file_list_error (cfl, data);
        g_free (cfl->path);
        g_free (cfl);
        return;
    }
    dh_start (htlc)
    {
        if (_type != HTLS_DATA_FILE_LIST) {
            continue;
        }
        fh = (struct hl_filelist_hdr *)dh;
        if (cfl->completing > 1) {
            char *pathbuf;
            int fnlen, len;
            guint32 ftype;

            HN32 (&fnlen, &fh->fnlen);
            len = strlen (cfl->path) + 1 + fnlen + 1;
            pathbuf = g_malloc (len + 1);
            snprintf (pathbuf, len, "%s%c%.*s", cfl->path[1] ? cfl->path : "",
                      dir_char, (int)fnlen, fh->fname);
            HN32 (&ftype, &fh->ftype);
            if (ftype == 0x666c6472) {
                struct cached_filelist *ncfl;
                guint16 hldirlen;
                guint8 *hldir;

                ncfl = cfl_lookup (pathbuf);
                ncfl->completing = cfl->completing;
                ncfl->filter_argv = cfl->filter_argv;
                if (!ncfl->path) {
                    ncfl->path = g_strdup (pathbuf);
                }
                hldir = path_to_hldir (pathbuf, &hldirlen, 0);
                task_new (&the_session.htlc, RCV_TASK_FN (rcv_task_file_list),
                          ncfl, 0, "ls_complete");
                hlwrite (&the_session.htlc, HTLC_HDR_FILE_LIST, 0, 1,
                         HTLC_DATA_DIR, hldirlen, hldir);
                g_free (hldir);
            } else if (cfl->completing == COMPLETE_GET_R) {
                struct htxf_conn *htxf;
                char *lpath, *p;

                lpath = g_malloc (len + 1);
                dirmask (lpath, pathbuf, "/");
                p = lpath + 1;
                while ((p = strchr (p, dir_char))) {
                    *p = 0;
                    if (mkdir (lpath + 1, S_IRUSR | S_IWUSR | S_IXUSR)) {
                        if (errno != EEXIST) {
                            hx_printf_prefix (htlc, 0, INFOPREFIX,
                                              "mkdir(%s): %s\n", lpath + 1,
                                              strerror (errno));
                        }
                    }
                    *p++ = '/';
                    while (*p == dir_char) {
                        *p++ = '/';
                    }
                }
                p = basename (lpath + 1);
                if (p) {
                    dirchar_fix (p);
                }
                {
                    guint32 fsize;
                    /* pathbuf is the joined parent + name in `cfl->path`
					 * space. Pass the structured (dir, name) tuple to
					 * xfer_new so the filename's bytes (including any
					 * `/` in the name) survive the wire trip.
					 *
					 * Phase E (follow-up): store remotename as UTF-8
					 * in memory so the display side (gtkhx.c folder-
					 * xfer label) is consistent and xfer_go can
					 * re-encode to the negotiated wire format. The
					 * file_list walker converts file entry names to
					 * UTF-8 in populate_from_chunks_cb; do the same
					 * here for the recursive-get path so both routes
					 * agree on the in-memory contract. */
                    char *nm_utf8;
                    gsize nm_utf8_len = 0;
                    HN32 (&fsize, &fh->fsize);
                    nm_utf8 = gtkhx_text_to_utf8 ((const char *)fh->fname,
                                                  (gsize)fnlen, &nm_utf8_len);
                    htxf = xfer_new (lpath + 1, cfl->path,
                                     nm_utf8 ? nm_utf8 : (const char *)fh->fname,
                                     nm_utf8 ? nm_utf8_len : (gsize)fnlen,
                                     XFER_GET, 0, fsize);
                    g_free (nm_utf8);
                }
                htxf->filter_argv = cfl->filter_argv;
                g_free (lpath);
            }
            g_free (pathbuf);
        }
        HN16 (&fhlen, &fh->len);
        fh_len = SIZEOF_HL_DATA_HDR + fhlen;
        fh_len += 4 - (fh_len % 4);
        cfl->fh = g_realloc (cfl->fh, cfl->fhlen + fh_len);
        memcpy ((char *)cfl->fh + cfl->fhlen, fh, SIZEOF_HL_DATA_HDR + fhlen);
        *((guint16 *)((char *)cfl->fh + cfl->fhlen + 2))
            = htons (fh_len - SIZEOF_HL_DATA_HDR);
        cfl->fhlen += fh_len;
    }
    dh_end ();

    cfl_print (cfl, data);
    cfl->completing = COMPLETE_NONE;
}

/* Format a Hotline 8-byte timestamp into a locale-formatted string.
 *
 * Wire layout / per-format decoding lives in src/hl_date.c so the
 * Tier 1 test can drive it without GTK. Two wire formats exist:
 *
 *   Mac 1904 epoch     legacy; vintage Mac servers, mhxd, Mobius
 *                      default. year=1904, secs since 1904-01-01 UTC.
 *   Modern             Capabilities.md spec adds this; servers that
 *                      see DATA_CAPABILITIES from us switch to it
 *                      to avoid the 2040 u32 overflow. year=actual,
 *                      secs since Jan 1 of that year in local time.
 *
 * hl_date_decode auto-detects via the year field, so neither side
 * needs to know what mode the server is in.
 *
 * Servers commonly send ts=0 to mean "no timestamp set" rather than
 * literally 1904-01-01. hl_date_decode rejects that and returns
 * FALSE so we leave `out` empty — the dialog renders empty values
 * as an em-dash so we don't show the user a date in the year 1838. */
static void
hx_format_hotline_date (const guint8 *bytes, char *out, size_t cap)
{
    time_t t;
    struct tm tm;

    if (cap == 0) {
        return;
    }
    out[0] = '\0';

    if (!hl_date_decode (bytes, &t)) {
        return;
    }
    if (!localtime_r (&t, &tm)) {
        return;
    }
    strftime (out, cap, hx_timeformat, &tm);
}

void
rcv_task_file_getinfo (struct htlc_conn *htlc, char *path)
{
    guint8 icon[4], date_create[8], date_modify[8];
    char type[32], crea[32];
    char name[256], comment[256];
    guint16 nlen, clen, tlen;
    guint32 size = 0;
    /* Large-file (CAP_LARGE_FILES) companion: when present, prefer
	 * the 64-bit size over the clamped 32-bit one. */
    guint64 size64 = 0;
    gboolean size64_seen = FALSE;
    char created[32], modified[32];

    if (task_inerror (htlc)) {
        return;
    }
    name[0] = comment[0] = type[0] = crea[0] = 0;
    memset (date_create, 0, sizeof date_create);
    memset (date_modify, 0, sizeof date_modify);
    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_FILE_ICON:
            if (_len >= 4) {
                memcpy (icon, dh->data, 4);
            }
            break;
        case HTLS_DATA_FILE_TYPE:
            tlen = (_len > 31) ? 31 : _len;
            memcpy (type, dh->data, tlen);
            type[tlen] = 0;
            break;
        case HTLS_DATA_FILE_CREATOR:
            clen = (_len > 31) ? 31 : _len;
            memcpy (crea, dh->data, clen);
            crea[clen] = 0;
            break;
        case HTLS_DATA_FILE_SIZE:
            /* Some servers (mhxd) emit the size in the smallest
			 * big-endian width that fits, so we may see len 1, 2,
			 * 3, or 4. Read whatever bytes arrived as a big-endian
			 * unsigned and zero-extend to guint32. */
            size = 0;
            for (guint16 i = 0; i < _len && i < 4; i++) {
                size = (size << 8) | dh->data[i];
            }
            break;
        case HTLS_DATA_FILESIZE64:
            /* Companion to FILE_SIZE; sent by large-file-mode
			 * servers. 8-byte big-endian unsigned. */
            if (_len >= 8) {
                size64 = 0;
                for (guint16 _i = 0; _i < 8; _i++) {
                    size64 = (size64 << 8) | dh->data[_i];
                }
                size64_seen = TRUE;
            }
            break;
        case HTLS_DATA_FILE_NAME:
            nlen = (_len > 255) ? 255 : _len;
            memcpy (name, dh->data, nlen);
            name[nlen] = 0;
            strip_ansi (name, nlen);
            break;
        case HTLS_DATA_FILE_DATE_CREATE:
            if (_len >= 8) {
                memcpy (date_create, dh->data, 8);
            }
            break;
        case HTLS_DATA_FILE_DATE_MODIFY:
            if (_len >= 8) {
                memcpy (date_modify, dh->data, 8);
            }
            break;
        case HTLS_DATA_FILE_COMMENT:
            clen = (_len > 255) ? 255 : _len;
            memcpy (comment, dh->data, clen);
            comment[clen] = 0;
            CR2LF (comment, clen);
            /* historical bug: pre-cleanup version called
			 * strip_ansi(name, clen) here, stripping ANSI from the
			 * filename buffer using the comment length. We hit it
			 * during the pointer-sign retype pass — surfaced because
			 * GCC complained about (now-mismatched) sign once both
			 * buffers became char[]. */
            strip_ansi (comment, clen);
            break;
        }
    }
    dh_end ();

    hx_format_hotline_date (date_create, created, sizeof created);
    hx_format_hotline_date (date_modify, modified, sizeof modified);

    gtkhx_session_emit_file_info (gtkhx_session_get_default (), path, name,
                                  crea, type, comment, modified, created,
                                  size64_seen ? size64 : (guint64)size);
}

void
rcv_task_file_get (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
    guint32 ref = 0, size = 0, queue = 0;
    /* Large-file (CAP_LARGE_FILES) companion: when present, the
	 * server sends DATA_XFERSIZE64 alongside the legacy 32-bit
	 * DATA_HTXF_SIZE. We prefer the 64-bit value when set; the
	 * legacy field is clamped to 0xFFFFFFFF in that case. */
    guint64 size64 = 0;
    gboolean size64_seen = FALSE;
    int i;

    for (i = 0; i < nxfers; i++) {
        if (xfers[i] == htxf) {
            break;
        }
    }

    if (i == nxfers) {
        return;
    }
    if (task_inerror (htlc)) {
        if (htxf->opt.retry) {
            htxf->gone = 0;
            timer_add_secs (1, xfer_go_timer, htxf);
        } else {
            gtask_delete_htxf (&the_session, htxf);
            xfer_delete (htxf);
        }
        return;
    }

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_SIZE:
            dh_getint (size);
            break;
        case HTLS_DATA_XFERSIZE64:
            /* 8-byte big-endian unsigned. */
            if (_len >= 8) {
                size64 = 0;
                for (guint16 _i = 0; _i < 8; _i++) {
                    size64 = (size64 << 8) | dh->data[_i];
                }
                size64_seen = TRUE;
            }
            break;
        case HTLS_DATA_HTXF_REF:
            dh_getint (ref);
            break;
        case HTLS_DATA_QUEUE:
            dh_getint (queue); /* Only on 1.5+ servers */
            break;
        }
    }
    dh_end ();

    if ((!size && !size64_seen) || !ref) {
        return;
    }

    htxf->ref = ref;
    htxf->total_size = size64_seen ? size64 : (guint64)size;
    htxf->queue = queue;

    gettimeofday (&htxf->start, 0);

    /* Stamp the HTXF subchannel target onto htxf so the worker can
	 * hand it straight to GSocketClient without re-resolving — the
	 * subchannel is always (main server hostname, main port + 1). */
    g_strlcpy (htxf->serverhost, htlc->serverhost, sizeof (htxf->serverhost));
    htxf->serverport = htlc->serverport + 1;

    gtkhx_session_emit_xfer_queue (gtkhx_session_get_default (), &the_session,
                                   htxf); /* we most certainly want
														 to output its position
														 in the queue */

    /* For previews, build the GtkWindow + GtkTextView on the main
	 * thread (we are on the main thread here — this is the
	 * GIOChannel callback path). The download worker subsequently
	 * feeds bytes via htxf->preview without ever touching GTK,
	 * sidestepping a class of lockups we hit when constructing
	 * widgets and calling gtk_window_present from a worker. */
    if (htxf->opt.preview && !htxf->preview) {
        char *name = dirchar_basename (htxf->path);
        htxf->preview = hx_preview_new (name ? name : htxf->path);
    }

    if (!htxf->queue) {
        xfer_ready_write (htxf);
    }
}

/* HTLS reply to HTLC_HDR_FILE_GETFOLDER. Mirror of
 * rcv_task_file_get with the addition of HTLS_DATA_FILE_NFILES
 * (the server tells us how many file leaves the tree has so we
 * can display a non-trivial percentage even though the per-file
 * sizes come in as the stream unfolds). */
void
rcv_task_folder_get (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
    guint32 ref = 0, size = 0, queue = 0, nfiles = 0;
    /* Large-file (CAP_LARGE_FILES) companion field — see
	 * rcv_task_file_get above for the rationale. */
    guint64 size64 = 0;
    gboolean size64_seen = FALSE;
    int i;

    for (i = 0; i < nxfers; i++) {
        if (xfers[i] == htxf) {
            break;
        }
    }

    if (i == nxfers) {
        return;
    }
    if (task_inerror (htlc)) {
        if (htxf->opt.retry) {
            htxf->gone = 0;
            timer_add_secs (1, xfer_go_timer, htxf);
        } else {
            gtask_delete_htxf (&the_session, htxf);
            xfer_delete (htxf);
        }
        return;
    }

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_SIZE:
            dh_getint (size);
            break;
        case HTLS_DATA_XFERSIZE64:
            if (_len >= 8) {
                size64 = 0;
                for (guint16 _i = 0; _i < 8; _i++) {
                    size64 = (size64 << 8) | dh->data[_i];
                }
                size64_seen = TRUE;
            }
            break;
        case HTLS_DATA_HTXF_REF:
            dh_getint (ref);
            break;
        case HTLS_DATA_QUEUE:
            dh_getint (queue);
            break;
        case HTLS_DATA_FILE_NFILES:
            dh_getint (nfiles);
            break;
        }
    }
    dh_end ();

    if (!ref) {
        return;
    }

    htxf->ref = ref;
    /* total_size is the aggregate byte count for the whole tree;
	 * folder_get_thread's per-file file_recv_one calls bump
	 * total_pos so progress reads sensibly across the whole
	 * folder. */
    htxf->total_size = size64_seen ? size64 : (size ? (guint64)size : 1);
    htxf->queue = queue;
    (void)nfiles; /* count is informational for now — wire it into
	                 * the tasks-window label in a follow-up. */

    gettimeofday (&htxf->start, 0);

    g_strlcpy (htxf->serverhost, htlc->serverhost, sizeof (htxf->serverhost));
    htxf->serverport = htlc->serverport + 1;

    gtkhx_session_emit_xfer_queue (gtkhx_session_get_default (), &the_session,
                                   htxf);

    if (!htxf->queue) {
        xfer_ready_write (htxf);
    }
}

void
rcv_task_file_put (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
    guint32 ref = 0, data_pos = 0, rsrc_pos = 0, queue = 0;
    struct stat sb;

    if (task_inerror (htlc)) {
        gtask_delete_htxf (&the_session, htxf);
        xfer_delete (htxf);
        return;
    }

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (ref);
            break;
        case HTLS_DATA_QUEUE:
            dh_getint (queue);
            break;
        case HTLS_DATA_RFLT:
            if (_len >= 66) {
                HN32 (&data_pos, &dh->data[46]);
                HN32 (&rsrc_pos, &dh->data[62]);
            }
            break;
        }
    }
    dh_end ();

    if (!ref) {
        return;
    }

    htxf->data_pos = data_pos;
    htxf->rsrc_pos = rsrc_pos;
    htxf->queue = queue;

    if (!stat (htxf->path, &sb)) {
        htxf->data_size = sb.st_size;
    }
    htxf->rsrc_size = resource_len (htxf->path);
    htxf->total_size = 133 + ((htxf->rsrc_size - htxf->rsrc_pos) ? 16 : 0)
                       + comment_len (htxf->path)
                       + (htxf->data_size - htxf->data_pos)
                       + (htxf->rsrc_size - htxf->rsrc_pos);
    htxf->ref = ref;
    gettimeofday (&htxf->start, 0);

    /* Stamp the HTXF subchannel target onto htxf. See the file_get
	 * sibling above for the same idiom. */
    g_strlcpy (htxf->serverhost, htlc->serverhost, sizeof (htxf->serverhost));
    htxf->serverport = htlc->serverport + 1;

    gtkhx_session_emit_xfer_queue (gtkhx_session_get_default (), &the_session,
                                   htxf);

    if (!htxf->queue) {
        xfer_ready_write (htxf);
    }
}

/* HTLS reply to HTLC_HDR_FILE_PUTFOLDER. The server has created
 * the destination folder root and is waiting for us to open the
 * HTXF subchannel and walk the local tree. Mirror of
 * rcv_task_file_put — same chunks, no resume RFLT (per-file
 * resume happens inside folder_put_thread, not at the task
 * boundary). */
void
rcv_task_folder_put (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
    guint32 ref = 0, queue = 0;

    if (task_inerror (htlc)) {
        gtask_delete_htxf (&the_session, htxf);
        xfer_delete (htxf);
        return;
    }

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (ref);
            break;
        case HTLS_DATA_QUEUE:
            dh_getint (queue);
            break;
        }
    }
    dh_end ();

    if (!ref) {
        return;
    }

    htxf->ref = ref;
    htxf->queue = queue;
    gettimeofday (&htxf->start, 0);

    g_strlcpy (htxf->serverhost, htlc->serverhost, sizeof (htxf->serverhost));
    htxf->serverport = htlc->serverport + 1;

    gtkhx_session_emit_xfer_queue (gtkhx_session_get_default (), &the_session,
                                   htxf);

    if (!htxf->queue) {
        xfer_ready_write (htxf);
    }
}

/* Reply to our HTLC_HDR_DOWNLOAD_BANNER. The server gives us a
 * transfer reference and total byte count; banner.c spins up an
 * HTXF worker thread to actually fetch the bytes off
 * server_port + 1. */
void
rcv_task_banner_get (struct htlc_conn *htlc, void *ptr, void *data)
{
    guint32 ref = 0, size = 0;
    (void)ptr;
    (void)data;

    if (task_inerror (htlc)) {
        debug_log ("banner", "DOWNLOAD_BANNER task error from server");
        return;
    }

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (ref);
            break;
        case HTLS_DATA_HTXF_SIZE:
            dh_getint (size);
            break;
        }
    }
    dh_end ();

    debug_log ("banner", "DOWNLOAD_BANNER reply: ref=%u size=%u", ref, size);

    banner_handle_htxf_reply (htlc, ref, size);
}
