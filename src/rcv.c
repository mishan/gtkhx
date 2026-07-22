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
#include "hx.h"
#include "login_packet.h"
#include "cipher.h"
#include "gtkhx_session.h"
#include "network.h"
#include "xfers.h"
#include "chat.h"
#include "chat_members.h" /* hx_member_model_get_ignore */
#include "tasks.h"
#include "files.h"
#include "files_remote_provider.h"
#include "preview.h"
#include "hl_date.h"
#include "gtkutil.h"
#include "msg.h"
#include "news.h"
#include "users.h"
#include "usermod.h"
#include "hxnet_bridge.h"
#include "htxf_io.h"             /* hxnet_hope_aead_free (HOPE AEAD handle) */
#include "rcv.h"
#include "hxconn.h"
#include "news15.h"
#include "hfs.h"
#include "proto_trace.h"
#include "hotline_proto.h"
#include "debug.h"
#include "connect.h"
#include "banner.h"
#include "chat_history.h"
#include "inline_media.h"
#include "gif_icons.h"
#include "hl_access.h"
#ifdef HAVE_VOICE
#include "voice_runtime.h"
#include "voice_model.h"
#endif

static size_t news_len = 0;
static guint8 *news_buf = 0;
static char *hx_timeformat = "%c";

/* xfer_go_timer (xfers.h), rcv_task_user_list and rcv_task_news_users
 * (rcv.h) are forward-referenced by hx_post_login_fetches below
 * before their definitions land later in the file. Prototypes come
 * from the headers we already #include. */

/* post-login state-machine. The 1.5 flow per
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
 * `hx_conn_post_login_fetched (htlc)` is the single-fire guard:
 * whichever path runs first sets it, the other path becomes a
 * no-op. Reset in hx_htlc_close so the next connect starts
 * fresh. Stored on the htlc rather than as a file-local static
 * so the files-browser's remote provider (and other consumers
 * that need the "fully joined" gate) can read it directly. */
static guint post_login_timer_id = 0;

/* Public entry — network.c::hx_send_agreement_agree calls this
 * right after the hlwrite so post-login fetches fire on the spec-
 * correct boundary (after AGREEMENTAGREE, not after SELFINFO). */
void
hx_post_login_fetches (struct htlc_conn *htlc)
{
    if (hx_conn_post_login_fetched (htlc)) {
        return;
    }
    hx_conn_set_post_login_fetched (htlc, 1);

    if (post_login_timer_id) {
        g_source_remove (post_login_timer_id);
        post_login_timer_id = 0;
    }

    /* Fetch users + (gated) news. rcv_task_news_users handles
	 * both — it calls rcv_task_user_list on the USER_GETLIST
	 * reply and then reload_news, the latter of which is itself
	 * gated on HL_ACCESS_READ_NEWS. */
    task_new (htlc, RCV_TASK_FN (rcv_task_news_users),
              chat_with_cid (sess_from_htlc (htlc), 0), 0, "who");
    /* USER_GETLIST is a zero-chunk opcode. */
    hlwrite_chunks (htlc, HTLC_HDR_USER_GETLIST, 0, NULL, 0);

    /* GIF-icons extension: probe for support (no capability bit). Sends
	 * ICON_GETLIST and arms a watchdog; a reply marks the session
	 * supported and delivers any avatars already set, a timeout marks
	 * it unsupported. Safe against legacy servers. */
    hx_icon_probe (htlc);

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
    if (hx_conn_has_cap (htlc, HTLC_CAP_CHAT_HISTORY)) {
        if (hx_conn_chat_history_last_msgid (htlc) > 0) {
            /* Reconnect catch-up — AFTER=last_msgid, no limit. */
            debug_log ("chat-history",
                       "reconnect catch-up: AFTER=%" G_GUINT64_FORMAT,
                       hx_conn_chat_history_last_msgid (htlc));
            task_new (htlc, RCV_TASK_FN (rcv_task_chat_history),
                      GUINT_TO_POINTER (HX_HISTORY_CHANNEL_PUBLIC), 0,
                      "chat-history-catchup");
            hx_get_chat_history (htlc, HX_HISTORY_CHANNEL_PUBLIC,
                                 /*before=*/0,
                                 /*after=*/hx_conn_chat_history_last_msgid (htlc),
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

    /* Announce the spec-correct "fully joined" boundary to the UI.
     * Consumers (e.g. the files browser's remote provider) use this
     * to defer post-login RPCs like FILE_LIST until after the server
     * has accepted our AGREEMENTAGREE — sending one before that
     * trips "action attributed to not-yet-joined session" errors on
     * 1.5+ servers and outright disconnects on the stricter ones. */
    gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
                                         GTKHX_CONNECTION_LOGIN_READY);
}

static gboolean
post_login_fallback (gpointer data)
{
    struct htlc_conn *htlc = data;

    post_login_timer_id = 0;
    if (htlc && hx_conn_fd (htlc) && !hx_conn_post_login_fetched (htlc)) {
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
    /* The post_login_fetched bit on htlc->flags is reset alongside
     * the other flags in hx_htlc_close — same reset point as
     * flags.logged_in. */
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
task_inerror (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    /* the header error-bit test moved to the Rust
	 * hotline-proto crate (gtkhx_proto_header_in_error). Same
	 * computation as the old ntohl(h->flag) & 1, with bounds
	 * checking on a short buffer. */
    return gtkhx_proto_header_in_error (frame, frame_len) ? 1 : 0;
}

/* Public-chat line ignore-gate + emit — Rust hxchat-recv crate. */
extern int hx_chat_recv (struct htlc_conn *htlc, void *member_model,
                         guint16 uid, void *event);

void
hx_rcv_chat (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    struct hx_chat_msg msg;
    session *sess = sess_from_htlc (htlc);
    struct chat *hx_chat = chat_with_cid (sess, 0);

    /* Chunk parse + CR2LF/strip_ansi + leading-LF strip lives in
	 * proto_helpers.c so the Tier 2 unit tests can drive it. */
    if (!hx_chat_extract (frame, frame_len, &msg)) {
        return;
    }

    /* Phase 9.D — inline-media companion fields. The relayed
	 * chat may carry CHAT_MEDIA_ID + CHAT_MEDIA_TYPE plus the
	 * server-supplied advisory width/height/bytes. The Phase A
	 * Rust extractor enforces the spec's "both or neither" rule
	 * and reports orphan pairs separately so the receiver can
	 * drop them per spec rather than render a half-blank
	 * placeholder. Only fires when the server confirmed the
	 * inline-media cap — receiving media chunks despite the cap
	 * not being negotiated implies a server bug, safer to
	 * ignore. */
    struct gtkhx_proto_chat_media_meta media_meta;
    int media_status = GTKHX_PROTO_MEDIA_META_NONE;
    if (hx_conn_has_cap (htlc, HTLC_CAP_INLINE_MEDIA)) {
        memset (&media_meta, 0, sizeof (media_meta));
        media_status = gtkhx_proto_extract_chat_media_meta (
            frame, frame_len, &media_meta);
        if (media_status == GTKHX_PROTO_MEDIA_META_ORPHAN) {
            /* Spec: receivers MUST reject a transaction with
			 * exactly one companion field present. Drop the chat
			 * entirely — orphans imply server bug / wire damage. */
            debug_log ("media",
                       "drop chat with orphaned media companion (cid=%u, "
                       "uid=%u)",
                       (unsigned) msg.cid, (unsigned) msg.uid);
            return;
        }
    }

    /* hx_output.chat → "chat" signal on the session
	 * emitter. Phase 5+: payload is a boxed HxChatEvent that
	 * bundles the UTF-8-validated line, sender/body slices, and
	 * info/self flags — every subscriber (chat.c renderer,
	 * notify.c) reads the same parsed view. */
    {
        HxChatEvent *ev = hx_chat_event_new (
            msg.text, msg.text_len, msg.cid,
            hx_conn_name (htlc)[0] ? hx_conn_name (htlc) : NULL);
        if (media_status == GTKHX_PROTO_MEDIA_META_PRESENT) {
            hx_chat_event_attach_media (
                ev, media_meta.id_ptr, media_meta.id_len,
                (const char *) media_meta.type_ptr, media_meta.type_len,
                media_meta.width, media_meta.width_present,
                media_meta.height, media_meta.height_present,
                media_meta.bytes, media_meta.bytes_present);
            debug_log ("media",
                       "chat with media: cid=%u uid=%u mime=%.*s "
                       "dims=%ux%u bytes=%u",
                       (unsigned) msg.cid, (unsigned) msg.uid,
                       (int) media_meta.type_len,
                       (const char *) media_meta.type_ptr,
                       (unsigned) media_meta.width,
                       (unsigned) media_meta.height,
                       (unsigned) media_meta.bytes);
        }
        /* Ignore-gate + emit live in the Rust hxchat-recv crate: it drops the
         * line when the sender is ignored (uid 0 = server line, never ignored)
         * and otherwise fires the "chat" signal. We keep ownership of ev and
         * free it either way. */
        hx_chat_recv (htlc, hx_chat_member_model (hx_chat), msg.uid, ev);
        hx_chat_event_free (ev);
    }
    /* CHAT_POST chime is played by the sound_events subscriber off the
     * "chat" signal — no inline play_sound here. */
}

/* Private-message ignore-gate + msg emit — Rust hxmsg-recv crate. hx_msg_recv
 * returns HX_MSG_DROPPED (ignored), HX_MSG_EMITTED (private message, boxed msg
 * signal fired), or HX_MSG_BROADCAST (C renders it via broadcastmsg). */
#define HX_MSG_DROPPED 0
#define HX_MSG_EMITTED 1
#define HX_MSG_BROADCAST 2
extern int hx_msg_recv (void *member_model, guint16 uid, int is_pm,
                        void *event);

void
hx_rcv_msg (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    struct hx_msg_msg pm;
    session *sess = sess_from_htlc (htlc);
    struct chat *chat = chat_with_cid (sess, 0);
    guint32 hdr_type = 0;
    gboolean is_broadcast;

    /* Chunk parse + name/body sanitisation lives in proto_helpers.c
	 * so the Tier 2 unit tests can drive it. */
    if (!hx_msg_extract (frame, frame_len, &pm)) {
        return;
    }

    /* Sender snapshot from the authoritative member model — used to fill
     * a missing display name (self-PM path) and the broadcast colour. */
    struct hx_member_info sender;
    gboolean have_sender
        = hx_member_model_get_info (hx_chat_member_model (chat), pm.uid, &sender);

    /* Dispatch on the wire opcode, not on pm.uid. mhxd echoes
	 * broadcasts back with the sender's UID populated (so the
	 * client can render "broadcast from <name>" if it wants),
	 * which means a UID-only check would mis-route every broadcast
	 * on mhxd-family servers into the private-message handler.
	 * The header type is the authoritative signal. */
    hl_hdr_decode (frame, &hdr_type, NULL, NULL, NULL, NULL, NULL);
    is_broadcast = (hdr_type == HTLS_HDR_MSG_BROADCAST);
    gboolean is_pm = !is_broadcast && pm.uid > 0;

    /* For a private message, build the boxed HxMsgEvent here — it needs the
     * self-PM display-name resolution + hx_conn_name (htlc). The broadcast branch has no
     * boxed event. The Rust hxmsg-recv crate owns the shared ignore-gate + the
     * PM emit; it returns which branch so we run broadcastmsg + preserve the
     * ignored-message early-out (no last_msg_nick update). */
    HxMsgEvent *ev = NULL;
    if (is_pm) {
        /* Some servers (mhxd on a self-directed PM) deliver the message
		 * with the sender UID but an empty NAME chunk, which rendered as
		 * "<> body" and — because HxMsgEvent's is_self test is name-based —
		 * mis-coloured the self-echo as incoming. Resolve a display name
		 * from the uid when the wire omits it: our own nick for a self-PM,
		 * else the sender's user-list entry. With the name filled in, the
		 * is_self classification inside hx_msg_event_new also works. */
        const char *disp_name = pm.name;
        gsize disp_name_len = pm.name_len;
        if (disp_name_len == 0) {
            if (pm.uid == hx_conn_uid (htlc) && hx_conn_name (htlc)[0]) {
                disp_name = hx_conn_name (htlc);
                disp_name_len = strlen (hx_conn_name (htlc));
            } else if (have_sender && sender.name[0]) {
                disp_name = sender.name;
                disp_name_len = strlen (sender.name);
            }
        }
        /* msg signal payload is a boxed HxMsgEvent
		 * (parsed once; every subscriber sees the same
		 * UTF-8-sanitised, self-classified view). */
        ev = hx_msg_event_new (
            pm.uid, disp_name, disp_name_len, pm.msg, pm.msg_len,
            hx_conn_name (htlc)[0] ? hx_conn_name (htlc) : NULL);
    }

    int r = hx_msg_recv (hx_chat_member_model (chat), pm.uid, is_pm, ev);
    if (ev) {
        hx_msg_event_free (ev);
    }
    if (r == HX_MSG_DROPPED) {
        return;
    }
    if (r == HX_MSG_BROADCAST) {
        /* Broadcasts on mhxd-family servers carry the sender's UID
		 * + NAME so the client can render "[name] body" with the
		 * sender's color. Pull the color from the cached user
		 * struct keyed on UID; pm.name is the wire-supplied name
		 * (UTF-8-sanitised by hx_msg_extract). When neither is
		 * present (older servers, server-generated rate-limit
		 * notes), broadcastmsg falls back to the legacy
		 * "[hx] broadcast: …" chat line. */
        const char *sender_name = pm.name_len > 0 ? pm.name : NULL;
        guint16 sender_color = have_sender ? sender.status : 0;
        broadcastmsg (sender_name, sender_color, pm.msg);
    }
    /* MSG chime: the sound_events subscriber plays it off the "msg"
     * signal (private-message branch). The broadcast branch has no "msg"
     * signal, so broadcastmsg() plays MSG itself to preserve the chime
     * that used to fire here for both branches. */

    if (!*last_msg_nick) {
        strncpy (last_msg_nick, pm.name, 31);
        last_msg_nick[31] = 0;
    }
}

/* Agreement show-vs-auto-agree decision + emit — Rust hxagreement-recv crate.
 * Returns HX_AGREEMENT_ACT_AUTO_AGREE (C sends AGREEMENTAGREE) or
 * HX_AGREEMENT_ACT_SHOWN (the agreement signal fired; the view pops the
 * Agree window). */
#define HX_AGREEMENT_ACT_AUTO_AGREE 0
#define HX_AGREEMENT_ACT_SHOWN 1
extern int hx_agreement_recv (void *sess, int has_agreement, const char *buf,
                              guint16 len);

void
hx_rcv_agreement_file (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    /* chunk-walking + sanitisation lives in
	 * hx_agreement_extract. The 16 KiB cap is generous; mhxd
	 * agreements hover around 1-2 KiB on the public servers and
	 * the protocol's chunk length is uint16 (max 65535) anyway. */
    char buf[16384];
    gsize body_len = 0;
    hx_agreement_result r
        = hx_agreement_extract (frame, frame_len, buf, sizeof (buf), &body_len);

    /* no-agreement auto-path — the user has nothing to
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
    /* HX_AGREEMENT_OK → show it (crate emits the agreement signal). Otherwise
     * (HX_AGREEMENT_NONE = agreement disabled, HX_AGREEMENT_NOT_FOUND =
     * malformed) there's nothing to click, so the crate returns AUTO_AGREE and
     * we send AGREEMENTAGREE ourselves — that completes login on no-agreement
     * servers and triggers the banner on banner-configured ones. */
    if (hx_agreement_recv (sess_from_htlc (htlc), r == HX_AGREEMENT_OK, buf,
                           (guint16) body_len)
        == HX_AGREEMENT_ACT_AUTO_AGREE) {
        hx_send_agreement_agree (htlc);
    }
}

/* rewritten to use hx_news_post_walk in proto_helpers.c.
 * The previous version maintained an unbounded-growth news_buf
 * accumulator that the emit code never actually consumed —
 * hx_output.news_post was called with just the new chunk's `_len`
 * bytes regardless of how much had been accumulated. See the
 * walker comment in proto_helpers.h for the full breakdown. */
/* flat-news chunk emit — Rust hxnews-recv crate. The chunk walk stays C
 * (gtkhx_proto_walk_news_post); hx_news_post_recv publishes the news-post
 * signal (the NEWS_POST chime plays off it via the sound_events subscriber). */
extern void hx_news_post_recv (struct htlc_conn *htlc, const char *bytes,
                               gsize len);

static void
news_post_emit (void *user, const char *bytes, gsize len)
{
    hx_news_post_recv ((struct htlc_conn *) user, bytes, len);
}

void
hx_rcv_news_post (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    hx_news_post_walk (frame, frame_len, news_post_emit, htlc);
}

void
hx_rcv_task (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    guint32 trans = 0;
    struct task *tsk;
    char error = 0;

    /* transaction-id extraction moved to the Rust
	 * hotline-proto crate (replaces HN32(&trans, &h->trans)). A
	 * short buffer leaves trans at 0, which task_with_trans treats
	 * as "no such task" — the same safe fallthrough as before. */
    gtkhx_proto_header_trans (frame, frame_len, &trans);
    tsk = task_with_trans (sess_from_htlc (htlc), trans);

    /* Speculative bootstrap probes whose rejection is expected and
	 * non-actionable: the GIF-icons capability probe (no cap/access bit
	 * and no version tie, so a task error is just "unsupported"). Their
	 * own rcv handler records the verdict on the error path (dispatched
	 * below), so suppress the generic error toast + ERROR sound for them
	 * — otherwise every login to a server without the extension nags the
	 * user about a request they never made. */
    gboolean silent_probe = tsk && tsk->str && !strcmp (tsk->str, "icon-list");

    if (task_inerror (htlc, frame, frame_len)) {
        if (!silent_probe) {
            task_error (htlc, frame, frame_len);
        }
        error = 1;
    }
#ifdef HAVE_VOICE
    /* Phase 8.D runtime wiring: a TASK error reply for one of the
     * voice opcodes (600 JOIN, 601 LEAVE, 603 SDP_ANSWER, 606
     * MUTE — 604 ICE doesn't register a task) needs to reach the
     * state machine via gtkhx_voice_runtime_task_error so it can
     * decide whether to tear the session down (JOIN/SDP failures
     * are fatal) or just surface a toast (MUTE/LEAVE failures are
     * benign). hx_rcv_task otherwise skips the task's rcv handler
     * for non-xfer error paths, so this is the only place voice
     * error replies get inspected. */
    if (error && tsk && tsk->str) {
        session *sess = sess_from_htlc (htlc);
        uint32_t opcode = 0;
        if (!strcmp (tsk->str, "voice-join")) {
            opcode = HTLC_HDR_VOICE_JOIN;
        } else if (!strcmp (tsk->str, "voice-leave")) {
            opcode = HTLC_HDR_VOICE_LEAVE;
        } else if (!strcmp (tsk->str, "voice-sdp-answer")) {
            opcode = HTLC_HDR_VOICE_SDP_ANSWER;
        } else if (!strcmp (tsk->str, "voice-mute")) {
            opcode = HTLC_HDR_VOICE_MUTE;
        }
        if (opcode && sess->voice_runtime) {
            char err_text[256];
            gsize err_len = 0;
            const char *text =
                (task_error_extract (frame, frame_len, err_text, sizeof (err_text),
                                     &err_len) && err_len > 0)
                    ? err_text
                    : NULL;
            gtkhx_voice_runtime_task_error (sess->voice_runtime, opcode,
                                            text);
        }
    }
#endif /* HAVE_VOICE */
    if (tsk) {
        /* XXX tsk->rcv might call task_delete */
        /* HTXF transfer tasks own an htxf_conn that needs to be
		 * reclaimed when the request errors — otherwise the
		 * orphaned transfer hangs in the Tasks UI forever with
		 * no progress and no way to dismiss it. The two labels
		 * are 'xfer_go' (single-file FILE_GET / FILE_PUT, fired
		 * from xfers.c) and 'xfer_go_folder' (folder transfers,
		 * fired from files.c). Their rcv functions
		 * (rcv_task_file_get / rcv_task_file_put) already check
		 * task_inerror internally and free the htxf on that
		 * path, so we run them on error too.
		 *
		 * Phase 9.C inline-media upload tasks ('upload-media')
		 * follow the same shape: rcv_task_upload_media owns the
		 * per-upload context (callback + user_data + heap state),
		 * checks task_inerror at its entry and routes to the
		 * failure-delivery path which invokes the caller's on_done
		 * with the spec MediaErrorCode + DATA_ERROR text. Without
		 * the dispatch, the ctx leaks and the caller's UI sits
		 * forever waiting for a callback that never fires.
		 *
		 * Non-transfer handlers (login, user-info, news, …) don't
		 * have per-task state to free; the error toast above is
		 * enough and we skip them as before. */
        gboolean dispatch_on_error
            = silent_probe
              || (tsk->str
                  && (!strcmp (tsk->str, "xfer_go")
                      || !strcmp (tsk->str, "xfer_go_folder")
                      || !strcmp (tsk->str, "upload-media")
                      || !strcmp (tsk->str, "download-media")));
        if (tsk->rcv && (!error || dispatch_on_error)) {
            tsk->rcv (htlc, frame, frame_len, tsk->ptr, tsk->data);
        }
        /* Liveness gate: skip task_delete if the rcv handler tore
		 * down the connection (rcv_task_login does this on a
		 * malformed HOPE Step 1 reply, for example). hx_htlc_close
		 * clears htlc->fd to 0, so a non-zero fd here means the
		 * connection is still live and task_delete (hash remove +
		 * gtask UI row removal) is safe to run.
		 *
		 * The pre-GIOStream code used `hxd_files[fd].conn.htlc`
		 * here — that array stopped tracking the control fd after
		 * the GIOStream rewrite (see comment in network.c
		 * connect_finish_handshake) and the check became always-
		 * false, so task_delete was always skipped and Tasks-window
		 * rows accumulated forever. The bug was latent against
		 * servers like mhxd that mostly skip TASK replies for
		 * login-time setup; it surfaced against Heidrun's Inn
		 * (which echoed the request opcode in the TASK reply type —
		 * a since-fixed server bug — and reaches hx_rcv_task once the
		 * dispatch mask folds it). */
        if (hx_conn_fd (htlc)) {
            task_delete (sess_from_htlc (htlc), tsk);
        }
    } else {
        /*	hx_printf_prefix(0, INFOPREFIX, "got task 0x%08x\n", trans); */
    }
}

/* User-roster apply routing lives in the Rust hxuser-recv crate
 * (rust/crates/hxuser-recv). hx_user_apply_recv is shared by the live
 * USER_CHANGE broadcast and the bulk USER_LIST load — `incremental` tells the
 * two apart. */
#define HX_USER_CHANGE_SKIPPED 0
#define HX_USER_CHANGE_CREATED 1
#define HX_USER_CHANGE_CHANGED 2
#define HX_USER_CHANGE_UPDATED 3
extern int hx_user_apply_recv (struct htlc_conn *htlc, void *chat,
                               void *member_model, guint16 uid,
                               guint32 nick_color, const char *name,
                               guint16 icon, guint16 color, int is_new,
                               int skip_self_create, int incremental);
extern int hx_user_part_recv (struct htlc_conn *htlc, void *chat,
                              void *member_model, guint16 uid);
/* USER_INFO reply + SELFINFO self-updated emits — Rust hxuser-recv crate. */
extern void hx_user_info_recv (guint16 uid, const char *name, const char *info,
                               guint16 len);
extern void hx_selfinfo_recv (struct htlc_conn *htlc);

void
hx_rcv_user_change (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    struct hx_user_change_msg uc;
    struct chat *chat;
    session *sess = sess_from_htlc (htlc);

    if (task_inerror (htlc, frame, frame_len)) {
        return;
    }

    if (!hx_user_change_extract (frame, frame_len, &uc)) {
        return;
    }

    /* Local aliases — keep the rest of the handler readable. */
    guint16 uid = uc.uid;
    guint16 icon = uc.icon;
    guint32 nick_color = uc.nick_color;
    gboolean got_nick_color = uc.got_nick_color;
    guint32 cid = uc.cid;
    char *name = uc.name;
    guint16 nlen = uc.name_len;

    /* Resolve every change decision in one pure, Tier-2-tested helper
     * (hx_user_change_plan_resolve; tests/proto/test_user_change.c): self
     * detection (incl. the SELFINFO-less uid adoption some 1.9 servers
     * force by omitting USER_LIST from SELFINFO, leaving hx_conn_uid (htlc) 0),
     * new-vs-change, the colour / nick-colour preserve rules, and whether
     * to print a rename notice. */
    chat = chat_with_cid (sess, cid);
    if (!chat) {
        chat = chat_new (sess, cid);
    }

    /* Old state is read from the authoritative member model (the per-chat
     * user hashtable is gone). get_info fills a *value* snapshot; it's
     * taken before the emit below updates the model, so `old` stays the
     * pre-change state even after the fan-out upserts. */
    struct hx_member_info old;
    gboolean old_exists = hx_member_model_get_info (hx_chat_member_model (chat), uid, &old);

    struct hx_user_change_plan plan;
    hx_user_change_plan_resolve (&uc, old_exists,
                                 old_exists ? old.status : 0,
                                 old_exists ? old.nick_color : HX_NICK_COLOR_NONE,
                                 old_exists ? old.name : NULL, hx_conn_uid (htlc),
                                 (const char *)hx_conn_name (htlc), &plan);

    if (plan.adopt_self_uid) {
        hx_conn_set_uid (htlc, uid);
        debug_log ("login",
                   "adopted self uid=%u from USER_CHANGE "
                   "broadcast (SELFINFO didn't carry it)",
                   (unsigned)uid);
    }

    /* Route through the shared roster-apply in the Rust hxuser-recv crate
     * (incremental=TRUE — this is a live broadcast, not the bulk load); it
     * returns what it emitted so we do the matching join / rename logging. */
    int emitted = hx_user_apply_recv (htlc, chat, hx_chat_member_model (chat),
                                      uid, plan.eff_nick_color, name, icon,
                                      plan.eff_color, plan.is_new,
                                      plan.skip_self_create, TRUE);
    if (emitted == HX_USER_CHANGE_SKIPPED) {
        /* Our own row — the USER_LIST reply creates it in the right spot. */
        return;
    }
    if (emitted == HX_USER_CHANGE_CREATED) {
        if (gtkhx_prefs.showjoin) {
            hx_printf_prefix (htlc, cid, INFOPREFIX, _ ("join: %s\n"), name);
        }
    } else { /* HX_USER_CHANGE_CHANGED */
        /* Bail on ignored users before we toast or log them. */
        if (hx_member_model_get_ignore (hx_chat_member_model (chat), uid)) {
            return;
        }
        if (plan.do_rename_notice) {
            /* old.name is the pre-change snapshot taken above. */
            hx_printf_prefix (htlc, cid, INFOPREFIX,
                              _ ("%1$s is now known as %2$s\n"), old.name,
                              name);
        }
    }

    /* Self bookkeeping — mirror the just-applied wire/plan values into
     * htlc. (skip_self_create returned early for a new-self, so here a
     * self change is always an existing member: old_exists is true.)
     *
     * deliberately do NOT copy the server's name into hx_conn_name (htlc).
     * Servers can legitimately override a display name — guests get
     * pinned to things like "Read the agreement" before they have
     * HL_ACCESS_USERNAME_CHANGE — and that override should show in the
     * user list but must not bleed into hx_conn_name (htlc), which doubles as the
     * persisted NICK= prefs value (prefs_write would then persist the
     * override forever). */
    if ((uid) && (uid == hx_conn_uid (htlc))) {
        hx_conn_set_icon (htlc,
                          icon ? icon
                               : (old_exists ? old.icon : hx_conn_icon (htlc)));
        if (got_nick_color) {
            hx_conn_set_nick_color (htlc, nick_color);
        }
        debug_log ("name",
                   "USER_CHANGE for our uid=%u: server says "
                   "'%.*s' (%u bytes); keeping local htlc->name = '%s'",
                   (unsigned)uid, (int)nlen, name, (unsigned)nlen, hx_conn_name (htlc));
    }
}

void
hx_rcv_user_part (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    struct hx_user_part_msg pm;
    struct chat *chat;
    session *sess = sess_from_htlc (htlc);

    if (!hx_user_part_extract (frame, frame_len, &pm)) {
        return;
    }

    chat = chat_with_cid (sess, pm.cid);
    if (!chat) {
        return;
    }

    /* Capture the member's name before the emit — the user_delete fan-out
     * removes the model entry. hx_user_part_recv (Rust hxuser-recv) re-checks
     * membership and emits user_delete only if present (incremental=TRUE: a
     * genuine part broadcast the sound subscriber chimes off), returning
     * whether it did so we log the "parts" line to match. */
    struct hx_member_info mi;
    gboolean have
        = hx_member_model_get_info (hx_chat_member_model (chat), pm.uid, &mi);
    if (hx_user_part_recv (htlc, chat, hx_chat_member_model (chat), pm.uid)) {
        if (have && gtkhx_prefs.showjoin) {
            hx_printf_prefix (htlc, pm.cid, INFOPREFIX, _ ("parts: %s \n"),
                              mi.name);
        }
    }
}

/* The changed-gate + chat-subject emit live in the Rust hxchat-recv crate
 * (rust/crates/hxchat-recv). C keeps the wire parse, the chat lookup, the model
 * set, and the "Subject Changed to" announce. */
extern int hx_chat_subject_recv (struct htlc_conn *htlc, guint32 cid,
                                 const char *subject, gsize subject_len,
                                 const char *current_subject);

void
hx_rcv_chat_subject (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    struct hx_chat_subject_msg sm;
    struct chat *chat;
    session *sess = sess_from_htlc (htlc);

    if (!hx_chat_subject_extract (frame, frame_len, &sm)) {
        return;
    }
    if (!sm.subject_len) {
        return;
    }
    chat = chat_with_cid (sess, sm.cid);
    if (!chat) {
        return;
    }

    /* On a real change the crate emits chat-subject and returns non-zero; the
     * initial-subject-discovery path (rcv_task_user_list) still updates the
     * widget directly without this announce. Set the model + log only when a
     * change actually fired. */
    if (hx_chat_subject_recv (htlc, sm.cid, sm.subject, sm.subject_len,
                              hx_chat_subject (chat))) {
        hx_chat_set_subject (chat, (const char *) (sm.subject), sm.subject_len);
        hx_printf_prefix (htlc, sm.cid, INFOPREFIX, "%s: %s",
                          _ ("Subject Changed to"), hx_chat_subject (chat));
    }
}

void
hx_rcv_banner (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    struct hx_banner_msg bm;

    /* HTLS_HDR_BANNER arrives unsolicited from the server
	 * after the AGREEMENTAGREE round-trip. Parse the type +
	 * optional URL and hand off to banner.c, which owns the
	 * toolbar widget and the URL/HTXF fetch state machines. */
    if (!hx_banner_extract (frame, frame_len, &bm)) {
        return;
    }

    banner_handle_message (htlc, bm.type, bm.has_url,
                           bm.has_url ? bm.url : NULL);
}

/* The ignore-gate + chat-invitation emit live in the Rust hxchat-recv crate
 * (rust/crates/hxchat-recv). C keeps the wire parse + the member-model lookup. */
extern void hx_chat_invite_recv (struct htlc_conn *htlc, void *member_model,
                                 guint32 cid, guint16 uid, const char *name);

void
hx_rcv_chat_invite (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    struct hx_chat_invite_msg im;
    session *sess = sess_from_htlc (htlc);
    struct chat *chat = chat_with_cid (sess, 0);

    if (!hx_chat_invite_extract (frame, frame_len, &im)) {
        return;
    }

    /* Drops the invite if the inviter is ignored, else emits chat-invitation
     * (the sound subscriber chimes off it). */
    hx_chat_invite_recv (htlc, hx_chat_member_model (chat), im.cid, im.uid,
                         im.name);
}

void
hx_rcv_user_selfinfo (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    /* The chunk walker (parses HTLS_DATA_ACCESS + HTLS_DATA_USER_LIST
	 * into htlc->access / uid / icon) is in proto_helpers.c so the
	 * Tier 2 unit tests can drive it without GTK. NB: the parser
	 * deliberately ignores the server-supplied name bytes (see the
	 * comment there) — we treat our local prefs nick as authoritative
	 * and push it back to the server immediately below. */
    hx_selfinfo_parse (htlc, frame, frame_len);

    /* SELFINFO is the canonical 'login complete' signal.
	 * Track it on htlc->flags so the agreement Agree button can
	 * tell whether to send AGREEMENTAGREE. See the comment on the
	 * flag in protocol.h for the legacy-vs-1.9 reasoning. */
    hx_conn_set_logged_in (htlc, 1);

    /* Access bits just landed; the view refreshes toolbar-button
     * sensitivity (kick/ban etc. gate on the access bitmap) off the
     * "self-updated" signal. The emit lives in the Rust hxuser-recv crate. */
    hx_selfinfo_recv (htlc);

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
	 * the SELFINFO USE_ANY_NAME auto-push that used to
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
hx_rcv_dump (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
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
    n = write (fd, frame, frame_len);
    if (n != (ssize_t)frame_len) {
        g_warning ("hx_rcv_dump: short write to hx.dump");
    }
    fsync (fd);
    close (fd);
}

/* Shared file-transfer reply tail — Rust hxxfer-recv crate. Emits the transfer's
 * queue position to the tasks view, then (when queue == 0) starts the byte
 * stream via xfer_ready_write. Called by all five xfer reply handlers once
 * they've stamped ref/size/queue onto htxf. */
extern void hx_xfer_announce (struct htlc_conn *htlc, struct htxf_conn *htxf,
                              guint32 queue);
/* file-info reply emit — Rust hxxfer-recv crate. */
extern void hx_file_info_recv (const char *path, const char *name,
                               const char *creator, const char *type,
                               const char *comments, const char *modified,
                               const char *created, guint64 size);

void
hx_rcv_xfer_queue (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    struct hx_xfer_queue_msg xq;
    struct htxf_conn *htxf;

    if (!hx_xfer_queue_extract (frame, frame_len, &xq)) {
        return;
    }

    htxf = htxf_with_ref (xq.ref);

    if (!htxf) {
        g_warning (_ ("Received queue id (%1$d) for xfer ref %2$d\n"
                      "No such xfer.\n"),
                   xq.queueid, xq.ref);
        return;
    }
    htxf->queue = xq.queueid;
    hx_xfer_announce (htlc, htxf, htxf->queue);
}

#ifdef HAVE_VOICE
/* ---- Voice-chat extension (Phase 8.A) ---------------------------- */
/*
 * The handlers below parse the body via the Rust hotline-proto::voice
 * shims and log a structured line through debug_log("voice", ...).
 * Phase 8.A intentionally does not emit GtkhxSession signals — the
 * model→view bridge for voice lands in Phase 8.C with the
 * hxvoice-runtime crate, which turns these wire frames into typed
 * state-machine events. For now, the logging path is sufficient to
 * satisfy the exit criterion ("see 602 SDP offer come back in the
 * trace, see participant list updates from 605 parsed by Rust into
 * structured events").
 *
 * The proto-trace category "voice" surfaces these lines independently
 * of the protocol category — `GTKHX_DEBUG=voice` for just the voice
 * events, or `GTKHX_DEBUG=proto,voice` for the full wire trace plus
 * the typed summaries here.
 */

void
hx_rcv_voice_sdp_offer (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    /* gtkhx_proto_parse_voice_reply only fails on NULL out; with a
     * stack-allocated `r` it always succeeds. The presence flags
     * below are the real malformed-frame signal. */
    struct gtkhx_proto_voice_reply r;
    gtkhx_proto_parse_voice_reply (frame, frame_len, &r);

    /* SDP is the mandatory payload on 602; absence is a malformed
     * frame from the server. Surface the case but don't crash — Phase
     * 8.C will decide whether to tear down the session. */
    if (!r.sdp_present) {
        debug_log ("voice",
                   "← VOICE_SDP_OFFER cid=%u: missing VOICE_SDP chunk",
                   r.cid);
        return;
    }

    const guint8 *sdp_ptr = NULL;
    gsize sdp_len = 0;
    /* Defensive: r.sdp_present was true, so the field walker should
     * find it, but bail out cleanly if it doesn't (logic bug
     * upstream, or an exotic frame the wire layer accepted in one
     * pass and refused in another). Leaving sum uninitialised below
     * would log garbage; surfacing the inconsistency is better. */
    if (!gtkhx_proto_voice_reply_field (frame, frame_len, 0,
                                        (const uint8_t **) &sdp_ptr,
                                        &sdp_len)) {
        debug_log ("voice",
                   "← VOICE_SDP_OFFER cid=%u: field walker rejected "
                   "VOICE_SDP after presence check",
                   r.cid);
        return;
    }

    struct gtkhx_proto_voice_sdp_summary sum;
    if (!gtkhx_proto_parse_voice_sdp_summary (sdp_ptr, sdp_len, &sum)) {
        debug_log ("voice",
                   "← VOICE_SDP_OFFER cid=%u sdp_len=%u: SDP summary "
                   "rejected",
                   r.cid, r.sdp_len);
        return;
    }
    debug_log (
        "voice",
        "← VOICE_SDP_OFFER cid=%u sdp_len=%u mids=%u unknown_mids=%u "
        "bundle=%u has_pcmu=%d disabled_slot=%d",
        r.cid, r.sdp_len, sum.mid_count, sum.unknown_mid_count,
        sum.bundle_count, (int) sum.has_pcmu, (int) sum.has_disabled_slot);

    /* Phase 8.D runtime wiring: feed the typed event into the
     * state machine + GStreamer dispatch. The runtime then walks
     * SetRemoteDescription + CreateAnswer; the answer flows back
     * out via the existing hx_send_voice_sdp_answer path once we
     * wire the SendWireFrame Backend (today the runtime uses a
     * NoopBackend so the C side keeps owning wire-out). The SDP
     * bytes from the wire aren't NUL-terminated — copy + NUL the
     * scratch buffer before handing off. */
    {
        session *sess = sess_from_htlc (htlc);
        (void) htlc;
        if (sess && sess->voice_runtime && sdp_ptr && sdp_len > 0) {
            char *sdp_str = g_malloc (sdp_len + 1);
            memcpy (sdp_str, sdp_ptr, sdp_len);
            sdp_str[sdp_len] = '\0';
            gtkhx_voice_runtime_sdp_offer (sess->voice_runtime, r.cid,
                                           sdp_str);
            g_free (sdp_str);
        }
    }
}

void
hx_rcv_voice_ice (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    /* See hx_rcv_voice_sdp_offer for the parse_voice_reply contract:
     * it only fails on NULL out, which a stack-allocated r can't
     * trigger. The presence flags below are the malformed-frame
     * signal. */
    struct gtkhx_proto_voice_reply r;
    gtkhx_proto_parse_voice_reply (frame, frame_len, &r);

    /* Distinguish the spec's end-of-candidates shorthand from a
     * malformed 604:
     *
     *   - VOICE_ICE chunk MISSING entirely: the server sent a 604
     *     with no payload chunk at all. That's a protocol violation
     *     — the spec mandates the chunk on every 604 (with either a
     *     JSON candidate or the empty-string EOC marker inside).
     *     Conflating it with EOC would hide server bugs. Log and
     *     return.
     *   - VOICE_ICE chunk PRESENT but zero-length: the EOC shorthand.
     *     The spec lets the chunk's body be empty as an alternative
     *     to a {"candidate":""} JSON payload. Honour it as EOC. */
    if (!r.ice_present) {
        debug_log ("voice",
                   "← VOICE_ICE cid=%u: missing VOICE_ICE chunk (malformed)",
                   r.cid);
        return;
    }
    if (r.ice_len == 0) {
        debug_log ("voice", "← VOICE_ICE cid=%u (end-of-candidates)",
                   r.cid);
        /* Spec EOC shorthand: zero-length chunk body. The hxvoice
         * state machine intercepts both the empty-string JSON
         * variant and the empty-chunk variant inside
         * `gtkhx_voice_runtime_ice_candidate` (which accepts NULL
         * candidate_json), so route the empty case through too
         * rather than
         * dropping it. Otherwise the state machine never sees
         * the server finishing its ICE gathering. */
        session *sess = sess_from_htlc (htlc);
        if (sess && sess->voice_runtime) {
            gtkhx_voice_runtime_ice_candidate (sess->voice_runtime,
                                               r.cid, NULL);
        }
        return;
    }

    const guint8 *ice_ptr = NULL;
    gsize ice_len = 0;
    if (!gtkhx_proto_voice_reply_field (frame, frame_len, 1,
                                        (const uint8_t **) &ice_ptr,
                                        &ice_len)) {
        debug_log ("voice",
                   "← VOICE_ICE cid=%u: field walker rejected VOICE_ICE "
                   "after presence check",
                   r.cid);
        return;
    }

    struct gtkhx_proto_voice_ice_candidate cand;
    struct gtkhx_proto_voice_ice_handle *h
        = gtkhx_proto_parse_voice_ice_json (ice_ptr, ice_len, &cand);
    if (!h) {
        debug_log ("voice",
                   "← VOICE_ICE cid=%u ice_len=%zu: JSON parse failed",
                   r.cid, ice_len);
        return;
    }
    debug_log (
        "voice",
        "← VOICE_ICE cid=%u ice_len=%zu candidate_len=%zu mid_len=%zu "
        "mline=%u%s%s",
        r.cid, ice_len, cand.candidate_len, cand.sdp_mid_len,
        cand.sdp_mline_index,
        cand.sdp_mline_index_present ? "" : " (absent)",
        cand.is_end_of_candidates ? " EOC" : "");
    gtkhx_proto_voice_ice_free (h);

    /* Phase 8.D runtime wiring: hand the raw JSON to the runtime.
     * The hxvoice state machine re-parses it (same parser, same
     * required-key validation) and feeds webrtcbin's
     * add-ice-candidate signal via Action::AddRemoteIce. */
    {
        session *sess = sess_from_htlc (htlc);
        (void) htlc;
        if (sess && sess->voice_runtime && ice_ptr && ice_len > 0) {
            char *json_str = g_malloc (ice_len + 1);
            memcpy (json_str, ice_ptr, ice_len);
            json_str[ice_len] = '\0';
            gtkhx_voice_runtime_ice_candidate (sess->voice_runtime, r.cid,
                                               json_str);
            g_free (json_str);
        }
    }
}

void
hx_rcv_voice_room_status (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    /* parse_voice_reply only fails on NULL out — see
     * hx_rcv_voice_sdp_offer's comment. */
    struct gtkhx_proto_voice_reply r;
    gtkhx_proto_parse_voice_reply (frame, frame_len, &r);

    if (!r.participants_present) {
        debug_log (
            "voice",
            "← VOICE_ROOM_STATUS cid=%u: missing VOICE_PARTICIPANTS chunk",
            r.cid);
        return;
    }

    const guint8 *blob = NULL;
    gsize blob_len = 0;
    /* Defensive return-value check, same shape as the SDP / ICE
     * handlers above: the presence flag was true, so the field
     * walker should hand back a non-NULL slice. Surface any
     * inconsistency instead of walking an uninitialised blob_len. */
    if (!gtkhx_proto_voice_reply_field (frame, frame_len, 3,
                                        (const uint8_t **) &blob,
                                        &blob_len)) {
        debug_log ("voice",
                   "← VOICE_ROOM_STATUS cid=%u: field walker rejected "
                   "VOICE_PARTICIPANTS after presence check",
                   r.cid);
        return;
    }

    /* Bounded stack buffer for the typed walk. The spec's room cap
     * default is 16 participants; allow plenty of headroom for
     * operator-overridden caps. blob_len / 6 is the upper bound on
     * count; we additionally cap at 64 for stack hygiene. */
    enum
    {
        MAX_LOG_ENTRIES = 64
    };
    struct gtkhx_proto_voice_participant ents[MAX_LOG_ENTRIES];
    size_t n = gtkhx_proto_parse_voice_participants (blob, blob_len, ents,
                                                     MAX_LOG_ENTRIES);
    debug_log ("voice",
               "← VOICE_ROOM_STATUS cid=%u participants=%zu (blob=%zu)",
               r.cid, n, blob_len);
    for (size_t i = 0; i < n; i++) {
        debug_log ("voice", "    uid=%u flags=0x%04x codec=%u%s",
                   ents[i].user_id, ents[i].flags, ents[i].codec_id,
                   (ents[i].flags & 0x0001) ? " MUTED" : "");
    }

    /* Phase 8.D runtime wiring: forward the raw blob. The Rust
     * side re-parses via hotline_proto::voice::parse_voice_participants
     * (same parser the typed walk above used) and feeds the state
     * machine's mid_to_user / participants caches. */
    {
        session *sess = sess_from_htlc (htlc);
        (void) htlc;
        if (sess && sess->voice_runtime) {
            gtkhx_voice_runtime_room_status (sess->voice_runtime, r.cid,
                                             blob, blob_len);
        }
        /* Speaker indicator: refresh the canonical per-uid voice
         * model from the new participants blob. The model emits
         * "indicator-changed" per uid whose state flipped; the
         * user list view subscribes and repaints the affected
         * rows. Independent of the runtime — the C side computes
         * indicator state from raw wire data, no extra round-trip
         * required. */
        if (sess && sess->voice_model) {
            hx_voice_model_ingest_participants (sess->voice_model, blob,
                                                blob_len);
        }
    }
}

/* ---- Voice TASK reply handlers (client-initiated 600/601/603/606) -- */
/*
 * The voice send wrappers in src/voice.c register one of these via
 * task_new() before each hlwrite_chunks call. hx_rcv_task looks up
 * the task entry by trans id on the TASK reply and dispatches here.
 *
 * Per the fogWraith spec, the JOIN (600) reply carries the server's
 * initial SDP offer, codec name, and current participant list — the
 * bulk of the session bootstrap payload. The other three opcodes
 * (601 LEAVE / 603 SDP_ANSWER / 606 MUTE) get empty-body success
 * replies; the simple_ack handler logs that the trans completed.
 * (604 VOICE_ICE is a notification both directions per spec — no
 * reply expected, so no task is registered for outgoing 604s.)
 *
 * The Phase 8.C state machine in hxvoice consumes the SDP / codec /
 * participants extracted here via SessionMachine events; for now the
 * handler just logs structured info through the "voice" debug
 * category so the proto-trace shows the bootstrap succeeded.
 */

void
rcv_task_voice_join (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, void *channel_ptr)
{
    guint32 expected_cid = GPOINTER_TO_UINT (channel_ptr);

    /* JOIN reply shape: CHAT_ID (echo) + VOICE_SDP (server offer) +
     * VOICE_CODEC (active codec name) + VOICE_PARTICIPANTS (current
     * list). All four fields per spec; a missing one is malformed.
     * gtkhx_proto_parse_voice_reply only returns false on NULL out,
     * so we don't need the dead-code conditional here either. */
    struct gtkhx_proto_voice_reply r;
    gtkhx_proto_parse_voice_reply (frame, frame_len, &r);

    if (r.cid != expected_cid) {
        debug_log (
            "voice",
            "← VOICE_JOIN reply cid=%u (expected %u) — server echoed "
            "different room",
            r.cid, expected_cid);
    }

    if (!r.sdp_present || !r.codec_present || !r.participants_present) {
        debug_log (
            "voice",
            "← VOICE_JOIN reply cid=%u: malformed (sdp=%d codec=%d "
            "participants=%d)",
            r.cid, (int) r.sdp_present, (int) r.codec_present,
            (int) r.participants_present);
        return;
    }

    /* Defensive: same shape as the other voice handlers — the
     * presence flag and the field walker agree on a well-formed
     * frame, so a walker rejection after the presence flag passed
     * is an internal inconsistency. Surface it instead of logging
     * a misleading zero-mids/empty-blob summary.
     *
     * SDP summary for the trace; the full SDP body lands in the
     * received frame at the offset the per-field accessor returns. */
    const guint8 *sdp_ptr = NULL;
    gsize sdp_len = 0;
    if (!gtkhx_proto_voice_reply_field (frame, frame_len, 0,
                                        (const uint8_t **) &sdp_ptr,
                                        &sdp_len)) {
        debug_log ("voice",
                   "← VOICE_JOIN reply cid=%u: field walker rejected "
                   "VOICE_SDP after presence check",
                   r.cid);
        return;
    }
    struct gtkhx_proto_voice_sdp_summary sum;
    gtkhx_proto_parse_voice_sdp_summary (sdp_ptr, sdp_len, &sum);

    /* Codec name (short ASCII, typically "PCMU"). */
    const guint8 *codec_ptr = NULL;
    gsize codec_len = 0;
    if (!gtkhx_proto_voice_reply_field (frame, frame_len, 2,
                                        (const uint8_t **) &codec_ptr,
                                        &codec_len)) {
        debug_log ("voice",
                   "← VOICE_JOIN reply cid=%u: field walker rejected "
                   "VOICE_CODEC after presence check",
                   r.cid);
        return;
    }
    char codec[32] = "?";
    if (codec_ptr && codec_len > 0 && codec_len < sizeof (codec)) {
        memcpy (codec, codec_ptr, codec_len);
        codec[codec_len] = '\0';
    }

    /* Participants — same walk as hx_rcv_voice_room_status. */
    const guint8 *blob = NULL;
    gsize blob_len = 0;
    if (!gtkhx_proto_voice_reply_field (frame, frame_len, 3,
                                        (const uint8_t **) &blob,
                                        &blob_len)) {
        debug_log ("voice",
                   "← VOICE_JOIN reply cid=%u: field walker rejected "
                   "VOICE_PARTICIPANTS after presence check",
                   r.cid);
        return;
    }
    enum
    {
        MAX_LOG_ENTRIES = 64
    };
    struct gtkhx_proto_voice_participant ents[MAX_LOG_ENTRIES];
    size_t n = gtkhx_proto_parse_voice_participants (blob, blob_len, ents,
                                                     MAX_LOG_ENTRIES);

    debug_log ("voice",
               "← VOICE_JOIN reply cid=%u codec=%s sdp_len=%u "
               "mids=%u has_pcmu=%d participants=%zu",
               r.cid, codec, r.sdp_len, sum.mid_count,
               (int) sum.has_pcmu, n);
    for (size_t i = 0; i < n; i++) {
        debug_log ("voice", "    uid=%u flags=0x%04x codec=%u%s",
                   ents[i].user_id, ents[i].flags, ents[i].codec_id,
                   (ents[i].flags & 0x0001) ? " MUTED" : "");
    }

    /* Phase 8.D runtime wiring: the JOIN reply carries the
     * server's SDP offer + initial participants. Drive both into
     * the state machine — SdpOfferReceived starts the answer-
     * generation walk, ParticipantsUpdated populates the
     * mid_to_user cache the pad-added path needs. */
    {
        session *sess = sess_from_htlc (htlc);
        (void) htlc;
        if (sess && sess->voice_runtime) {
            gtkhx_voice_runtime_room_status (sess->voice_runtime, r.cid,
                                             blob, blob_len);
            if (sdp_ptr && sdp_len > 0) {
                char *sdp_str = g_malloc (sdp_len + 1);
                memcpy (sdp_str, sdp_ptr, sdp_len);
                sdp_str[sdp_len] = '\0';
                gtkhx_voice_runtime_sdp_offer (sess->voice_runtime, r.cid,
                                               sdp_str);
                g_free (sdp_str);
            }
        }
        /* Speaker indicator: feed the canonical voice model too.
         * The JOIN reply's participants blob is the first
         * authoritative list we'll see for this room, so the
         * indicator column starts painting the moment our own
         * JOIN lands rather than waiting for the first 605. */
        if (sess && sess->voice_model) {
            hx_voice_model_ingest_participants (sess->voice_model, blob,
                                                blob_len);
        }
    }
}

void
rcv_task_voice_simple_ack (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, void *opcode_ptr,
                           void *cid_ptr)
{
    /* opcode is stashed in ptr via GUINT_TO_POINTER for the trace
     * label; cid via the data slot. Both are diagnostic only — the
     * empty-success-reply path doesn't carry any state worth
     * extracting. task_inerror is handled before this is called by
     * hx_rcv_task; we only see the success path. */
    guint32 opcode = GPOINTER_TO_UINT (opcode_ptr);
    guint32 cid = GPOINTER_TO_UINT (cid_ptr);
    (void) htlc;
    debug_log ("voice", "← VOICE %u ack (cid=%u)", opcode, cid);
}
#endif /* HAVE_VOICE */

/* Dispatch a received frame. The Rust hxnet actor already parsed the header
 * and the bridge hands us the whole frame (22-byte header + body) as a
 * (frame, frame_len) slice, so this no longer re-decodes the header or runs
 * the old two-phase receive state machine — it traces, routes the opcode to a
 * body handler (via the Rust dispatch::route table behind hx_recv_route), and
 * calls it. */
void
hx_dispatch_frame (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len,
                   guint32 type, guint32 trans,
                   guint32 flag, guint32 body_len)
{
    /* Wire len field encodes body_len + the 2-byte hc; the proto trace wants
     * that raw value. */
    proto_trace_recv_hdr (type, trans, flag,
                          body_len + (guint32) sizeof (guint16));

    void (*handler) (struct htlc_conn *, const guint8 *, gsize) = NULL;
    switch (hx_recv_route (type)) {
    case HX_RECV_CHAT:
        handler = hx_rcv_chat;
        break;
    case HX_RECV_MSG:
        handler = hx_rcv_msg;
        break;
    case HX_RECV_USER_CHANGE:
        handler = hx_rcv_user_change;
        break;
    case HX_RECV_USER_PART:
        handler = hx_rcv_user_part;
        break;
    case HX_RECV_NEWS_POST:
        handler = hx_rcv_news_post;
        break;
    case HX_RECV_TASK:
        handler = hx_rcv_task;
        break;
    case HX_RECV_CHAT_SUBJECT:
        handler = hx_rcv_chat_subject;
        break;
    case HX_RECV_CHAT_INVITE:
        handler = hx_rcv_chat_invite;
        break;
    case HX_RECV_USER_SELFINFO:
        handler = hx_rcv_user_selfinfo;
        break;
    case HX_RECV_AGREEMENT:
        handler = hx_rcv_agreement_file;
        break;
    case HX_RECV_BANNER:
        handler = hx_rcv_banner;
        break;
    case HX_RECV_POLITEQUIT:
        hx_printf_prefix (htlc, 0, INFOPREFIX, _ ("polite quit\n"));
        handler = hx_rcv_msg;
        break;
    case HX_RECV_XFER_QUEUE:
        handler = hx_rcv_xfer_queue;
        break;
#ifdef HAVE_VOICE
    case HX_RECV_VOICE_SDP_OFFER:
        handler = hx_rcv_voice_sdp_offer;
        break;
    case HX_RECV_VOICE_ICE:
        handler = hx_rcv_voice_ice;
        break;
    case HX_RECV_VOICE_ROOM_STATUS:
        handler = hx_rcv_voice_room_status;
        break;
#endif /* HAVE_VOICE */
    case HX_RECV_ICON_CHANGE:
        handler = hx_rcv_icon_change;
        break;
    default:
        /* HX_RECV_UNKNOWN, plus the voice kinds in a -Dvoice=disabled build. */
        debug_log ("proto", "unknown header type 0x%08x", type);
        hx_printf_prefix (htlc, 0, INFOPREFIX,
                          _ ("unknown header type 0x%08x\n"), type);
        handler = hx_rcv_dump;
        break;
    }

    if (handler && hx_conn_fd (htlc) != 0) {
        handler (htlc, frame, frame_len);
    }
}

void
rcv_task_user_open (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct uesp_fn *uespfn)
{
    char name[32], login[32], pass[32];
    hl_access_bits access;

    /* chunk-walk + hl_decode (XOR-0xff) of LOGIN /
	 * PASSWORD moved to the Rust hotline-proto crate's
	 * parse_account_read. The PASSWORD no-password sentinel
	 * (single 0x00 byte, or empty) is preserved by the Rust
	 * parser — pass_len = 0 in that case, and the C buffer stays
	 * NUL-terminated at offset 0. */
    struct gtkhx_proto_account_read ar;
    bool ok = gtkhx_proto_parse_account_read (
        frame, frame_len, (uint8_t *)name, sizeof (name),
        (uint8_t *)login, sizeof (login), (uint8_t *)pass, sizeof (pass), &ar);
    if (ok && ar.got_access) {
        /* ACCESS lands in ar.access as raw 8 wire bytes; copy into
		 * the typed hl_access_bits exactly as the C extractor did
		 * (memcpy preserves byte order on this struct, which the
		 * server's access bitmap is). */
        memcpy (&access, ar.access, sizeof (access));
        uespfn->fn (uespfn->uesp, name, login, pass, access);
    }
    g_free (uespfn);
}

void
rcv_task_msg (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, char *msg_buf)
{
    if (msg_buf) {
        hx_printf (htlc, 0, "%s\n", msg_buf);
        g_free (msg_buf);
    }
}

/* rcv_task_newscat_list moved to the hxnews-recv Rust crate — it parses the
 * CATLIST chunk to an owned handle, stashes it on the gnews_catalog carrier, and
 * emits news-catalog, with no intermediate news_item / news_group. hxnews-send's
 * task_new still registers it; the symbol now resolves against hxnews-recv. */

/* rcv_task_newsfolder_list moved to the hxnews-recv Rust crate — it parses the
 * NEWSDIRLIST chunks (the dh_start walk + per-chunk parsers) into an owned
 * DirList handle via gtkhx_proto_parse_dirlist, stashes it on the gnews_folder
 * carrier, and emits news-folder, with no intermediate folder_item / news_folder.
 * hxnews-send's task_new still registers it; the symbol now resolves against
 * hxnews-recv. */

/* rcv_task_news_post moved to the hxnews-recv Rust crate — it parses the
 * GETTHREAD NEWSDATA body (gtkhx_proto_parse_news_thread_reply), bails on a
 * TASK_ERROR / body-less reply, then builds the news_post carrier via
 * news_post_new (news_recv_bridge.c) and emits news-thread. hxnews-send's
 * get_post sender still registers it; the symbol now resolves against
 * hxnews-recv. With this, no news code remains in rcv.c. */

void
rcv_task_news_users (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct chat *chat, int text)
{
    /* output user list and then grab news */
    /* this is only used for login events  */
    rcv_task_user_list (htlc, frame, frame_len, chat, text);

    reload_news (0, sess_from_htlc (htlc));
}

/* Post-login fetch sequencing decision — Rust hotline-proto (login module).
 * Returns HX_POST_LOGIN_FETCH_NOW (1.0/1.2: fire fetches now),
 * HX_POST_LOGIN_ARM_FALLBACK (1.5+: wait for AGREEMENTAGREE, arm the 2s timer),
 * or HX_POST_LOGIN_NOTHING (already fetched). */
#define HX_POST_LOGIN_NOTHING 0
#define HX_POST_LOGIN_FETCH_NOW 1
#define HX_POST_LOGIN_ARM_FALLBACK 2
extern int hx_post_login_route (guint16 version, int already_fetched);

void
rcv_task_login (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, char *pass)
{
    char buf[HOSTLEN];
    char servername[8192 + 1];

    g_strlcpy (buf,
               hx_conn_ip_addr (htlc)[0] ? hx_conn_ip_addr (htlc) : "?",
               sizeof (buf));

    if (!pass) {
        hx_printf_prefix (htlc, 0, INFOPREFIX, "%s:%u: %s %s\n", buf,
                          hx_conn_serverport (htlc), _ ("login"),

                          task_inerror (htlc, frame, frame_len) ? _ ("failed?")
                                              : _ ("successful"));
    }

    /* The HOPE step-1→step-2 handshake the legacy connect path drove
     * here is gone — the orchestrator (hxnet) owns the whole HOPE
     * handshake in Rust and replays only the final reply, so this task
     * always runs the post-login completion below (pass is always
     * NULL). */
    if (!task_inerror (htlc, frame, frame_len)) {
        /* Login task reply came back successful. The connected-state UI
         * (window titles / toolbar buttons / status bar) and the LOGIN
         * chime are driven off the "logged-in" signal, emitted below once
         * the LOGIN reply has been fully walked — see the emit site after
         * dh_end(). */
        connected = 1;

        /* Seed the opaque HOPE AEAD material handle (if the orchestrated
         * control channel negotiated ChaCha20-Poly1305). The handshake
         * is complete by now, so the retained material is populated;
         * HTXF subchannels (banner.c / xfers.c) read htlc->hope_aead to
         * derive their per-transfer keys in-process. NULL for plaintext
         * / Blowfish / no-cipher. Freed on connection teardown. */
        if (hx_conn_hope_aead (htlc)) {
            hxnet_hope_aead_free (hx_conn_hope_aead (htlc));
        }
        hx_conn_set_hope_aead (htlc, hx_bridge_orchestrated_hope_aead ());

        /* Reset post-login fetch state before scheduling so
		 * a reconnection during this process state starts clean.
		 *
		 * The check on already_fetched used to cover a race where
		 * SELFINFO arrived before the login TASK reply and fired
		 * the fetches already; that race no longer matters because
		 * SELFINFO is not a fetch trigger anymore (fetches fire
		 * from hx_send_agreement_agree). The check is harmless
		 * to keep — it's a no-op when the flag is FALSE, which is
		 * the new common case. The fetched-bit itself lives on
		 * hx_conn_post_login_fetched (htlc) now (so the files browser
		 * can read it); the running timer id is still our local
		 * static. */
        gboolean already_fetched = hx_conn_post_login_fetched (htlc);
        if (!already_fetched) {
            hx_conn_set_post_login_fetched (htlc, 0);
            if (post_login_timer_id) {
                g_source_remove (post_login_timer_id);
                post_login_timer_id = 0;
            }
        }

        /* Phase 9.A: clear inline-media advisory limits BEFORE
		 * walking the LOGIN reply. Each MAX_* field is
		 * independently optional on the wire (spec: "Clients
		 * MUST tolerate any individual field being absent"),
		 * so the chunk walker below only writes the ones the
		 * server advertised — any field omitted from this
		 * particular LOGIN would otherwise inherit a stale
		 * value from a prior session on the same htlc_conn
		 * struct (network.c::hx_htlc_close also zeroes them
		 * on disconnect, but a server reconfiguration mid-
		 * lifetime that re-LOGINs without going through
		 * close would otherwise still carry stale fields).
		 * htlc->caps is overwritten outright by the chunk
		 * walker; these can't piggyback on that. */
        inline_media_reset_advisory_limits (htlc);

        /* The LOGIN reply chunk-walk moved to the Rust hotline-proto crate
		 * (gtkhx_proto_parse_login). It enforces the same per-field width
		 * gates the C code did (UID/VERSION as u16; each media / history
		 * limit requires the spec's 4 bytes or it's skipped), sanitises
		 * the server name (CR2LF + strip_ansi), and reports which fields
		 * were present via the returned HX_LOGIN_SEEN_* bitmask. Every
		 * field is independently optional on the wire — a 1.0/1.2 server
		 * sends almost none of them — so each htlc assignment below is
		 * gated on its seen bit. The advisory media limits were already
		 * reset above; caps is overwritten only when the server echoed a
		 * DATA_CAPABILITIES chunk. */
        struct gtkhx_proto_login li;
        unsigned login_seen = gtkhx_proto_parse_login (
            frame, frame_len, (uint8_t *) servername,
            sizeof (servername), &li);

        if (login_seen & HX_LOGIN_SEEN_UID) {
            hx_conn_set_uid (htlc, li.uid);
        }
        if (login_seen & HX_LOGIN_SEEN_VERSION) { /* Hotline 1.5+ only */
            hx_conn_set_version (htlc, li.version);
        }
        if (login_seen & HX_LOGIN_SEEN_SERVERNAME) { /* Hotline 1.5+ only */
            if (server_addr) {
                g_free (server_addr);
            }
            /* server names from old Hotline servers are 8-bit Mac Roman
			 * text, not UTF-8 — and gtk_window_set_title et al. assert
			 * UTF-8. gtkhx_text_to_utf8 handles the already-UTF-8 /
			 * Mac-Roman / fall-back-to-substitute cascade. The window
			 * titles pick server_addr up when the "logged-in" signal is
			 * emitted after this walk completes. */
            server_addr = gtkhx_text_to_utf8 (
                servername, strlen (servername), NULL);
        }
        if (login_seen & HX_LOGIN_SEEN_CAPS) {
            /* DATA_CAPABILITIES echo — the bits the server agreed to
			 * enable for this session. Bits we don't recognise are
			 * preserved per the spec's "ignore unknown bits" rule. */
            hx_conn_set_caps (htlc, li.caps);
            if (li.caps & HTLC_CAP_LARGE_FILES) {
                hx_printf_prefix (
                    htlc, 0, INFOPREFIX,
                    _ ("server confirmed large-file (64-bit) "
                       "mode for this session\n"));
            }
            if (li.caps & HTLC_CAP_TEXT_ENCODING) {
                hx_printf_prefix (
                    htlc, 0, INFOPREFIX,
                    _ ("server confirmed UTF-8 text encoding "
                       "for this session\n"));
            }
            if (li.caps & HTLC_CAP_CHAT_HISTORY) {
                hx_printf_prefix (
                    htlc, 0, INFOPREFIX,
                    _ ("server confirmed chat-history extension "
                       "for this session\n"));
            }
            if (li.caps & HTLC_CAP_INLINE_MEDIA) {
                hx_printf_prefix (
                    htlc, 0, INFOPREFIX,
                    _ ("server confirmed inline-media extension "
                       "for this session\n"));
            }
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_BYTES) {
            htlc->media_max_bytes = li.media_max_bytes;
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_DIMENSION) {
            htlc->media_max_dimension = li.media_max_dimension;
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_PIXELS) {
            htlc->media_max_pixels = li.media_max_pixels;
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_CHUNK_SIZE) {
            htlc->media_chunk_size = li.media_chunk_size;
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_FRAMES) {
            htlc->media_max_frames = li.media_max_frames;
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_DURATION_MS) {
            htlc->media_max_duration_ms = li.media_max_duration_ms;
        }
        /* Chat-history retention hints — max message count / age. 0 means
		 * unlimited; these are hints only, the authoritative end-of-history
		 * signal is DATA_HISTORY_HAS_MORE = 0 in TRAN 700 replies. */
        if (login_seen & HX_LOGIN_SEEN_HISTORY_MAX_MSGS) {
            hx_conn_set_history_max_msgs (htlc, li.history_max_msgs);
        }
        if (login_seen & HX_LOGIN_SEEN_HISTORY_MAX_DAYS) {
            hx_conn_set_history_max_days (htlc, li.history_max_days);
        }

        /* Phase 9.A: log the server's advertised inline-media
		 * limits at debug-category "media". Routed through a
		 * stable helper so future logging adjustments don't
		 * spider out across rcv.c. */
        if (hx_conn_has_cap (htlc, HTLC_CAP_INLINE_MEDIA)) {
            inline_media_log_advertised_limits (htlc);
        }

        /* Login processing is complete: uid, version, server name, and
		 * caps have all been parsed out of this LOGIN reply. Emit the
		 * "logged-in" signal now so the view-side handler in gtkhx.c
		 * settles the connected UI in one shot — window titles (needs
		 * the parsed SERVERNAME → server_addr) and toolbar buttons (the
		 * news15 button gate is version >= 150, so it needs the parsed
		 * HTLS_DATA_VERSION) and the status bar — and sound_events plays
		 * the LOGIN chime. Emitting after the walk rather than before it
		 * is what lets this be a single settle instead of the old
		 * set-then-re-run dance. */
        gtkhx_session_emit_logged_in (gtkhx_session_get_default (), htlc);

        /* PING keepalive only on confirmed 1.5+ servers.
		 * hx_conn_version (htlc) is populated by the HTLS_DATA_VERSION
		 * chunk just parsed above; servers that don't advertise
		 * a version (1.0/1.2 originals like hlserver.com) leave
		 * it at 0, and sending HTLC_HDR_PING to them earns a
		 * task-error toast every minute ("Uh, no.") plus the
		 * ERROR sound. >= 150 is the bar — that covers every
		 * server we've seen (Badmoon at 190, mhxd at 150+) that
		 * implements PING, and excludes the ones that don't. */
        if (hx_conn_version (htlc) >= 150) {
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
        switch (hx_post_login_route (hx_conn_version (htlc), already_fetched)) {
        case HX_POST_LOGIN_FETCH_NOW:
            /* 1.0/1.2 server: no agreement flow — deliver NAME + ICON and fire
             * the fetches now (no AGREEMENTAGREE boundary is coming). */
            hx_change_name_icon (htlc);
            hx_post_login_fetches (htlc);
            break;
        case HX_POST_LOGIN_ARM_FALLBACK:
            /* 1.5+ server: hx_send_agreement_agree / the Agree click fire the
             * fetches after the AGREEMENTAGREE round-trip. Do NOT fire
             * HTLC_HDR_USER_GETLIST yet; arm a 2s fallback in case a misbehaving
             * server sends no agreement opcode at all. */
            post_login_timer_id
                = g_timeout_add_seconds (2, post_login_fallback, htlc);
            break;
        default: /* HX_POST_LOGIN_NOTHING — fetches already fired */
            break;
        }
    }
}

/* news-file emit — Rust hxnews-recv crate. */
extern void hx_news_file_recv (struct htlc_conn *htlc, const char *bytes,
                               gsize len);

void
rcv_task_news_file (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    /* parse + sanitise in hx_news_file_extract. We still
	 * use the file-scope news_buf scratch as the destination so
	 * downstream-allocated callers reading news_len/news_buf get
	 * the same shape as before (the lifetime is "until the next
	 * NEWS_FILE arrives"). */
    gsize copied = 0;
    news_buf = g_realloc (news_buf, 65536);
    if (hx_news_file_extract (frame, frame_len, (char *)news_buf, 65536, &copied)) {
        news_len = copied;
    } else {
        news_len = 0;
        news_buf[0] = 0;
    }
    /* news-file emit — Rust hxnews-recv crate. */
    hx_news_file_recv (htlc, (char *)news_buf, news_len);
}

/* GIF-icons extension (fogWraith GIF-Icons.md). Parsing lives in the
 * Rust hotline-proto crate (crate::gif_icons via the gtkhx_proto_*
 * shims); these handlers pass the received frame slice straight in and only
 * emit GtkhxSession signals — no chunk walking on the C side. */

/* gif-icon-data validation + emit lives in the Rust hxicon-recv crate: it
 * upholds the signal's "raw GIF bytes or empty" contract — a zero-length or
 * non-GIF-signed payload is coerced to a cleared (NULL, 0) so no subscriber
 * decodes network garbage or a dangling pointer. */
extern void hx_icon_data_recv (struct htlc_conn *htlc, guint16 uid,
                               const guint8 *gif, guint32 len);

/* ICON_GET (1863) task reply: UID + ICON_GIF. */
void
rcv_task_icon_get (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, void *uid_ptr)
{
    (void) uid_ptr; /* uid is echoed in the reply; we read it from there */
    struct gtkhx_proto_icon_entry e;
    if (!gtkhx_proto_parse_icon_get_reply (frame, frame_len, &e)) {
        debug_log ("icon", "ICON_GET reply missing UID");
        return;
    }
    /* A get reply implies the server speaks the extension. gif_len == 0
	 * is a valid "avatar cleared" result — we still emit it so the view
	 * drops any stale cached avatar. */
    hx_conn_set_gif_icons_state (htlc, GIF_ICONS_SUPPORTED);
    debug_log ("icon", "ICON_GET reply: uid=%u gif_len=%zu", (unsigned) e.uid,
               e.gif_len);
    hx_icon_data_recv (htlc, e.uid, e.gif_ptr, (guint32) e.gif_len);
}

/* ICON_GETLIST (1861) task reply: 0..N packed ICON_LIST entries. Also
 * the resolution point for the post-login probe. */
void
rcv_task_icon_getlist (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    /* The reply arriving at all means the server supports the
	 * extension — flip the probe state and disarm the watchdog. */
    /* GIF-icons has no capability/access bit and no version tie (a 1.5+
     * server may or may not implement it), so support is detected purely
     * by this probe. An ERROR reply is the "not supported" answer, exactly
     * like the watchdog's no-reply timeout — mark unsupported, disarm the
     * watchdog, and return WITHOUT a user toast (hx_rcv_task suppresses
     * task_error for the "icon-list" task and dispatches us on the error
     * path so we record the verdict). A speculative probe's rejection is
     * expected and non-actionable. */
    if (task_inerror (htlc, frame, frame_len)) {
        hx_conn_set_gif_icons_state (htlc, GIF_ICONS_UNSUPPORTED);
        if (hx_conn_gif_icons_probe_timer (htlc)) {
            g_source_remove (hx_conn_gif_icons_probe_timer (htlc));
            hx_conn_set_gif_icons_probe_timer (htlc, 0);
        }
        debug_log ("icon",
                   "ICON_GETLIST rejected (task error) — server lacks the "
                   "GIF-icons extension; probe verdict UNSUPPORTED");
        return;
    }

    hx_conn_set_gif_icons_state (htlc, GIF_ICONS_SUPPORTED);
    if (hx_conn_gif_icons_probe_timer (htlc)) {
        g_source_remove (hx_conn_gif_icons_probe_timer (htlc));
        hx_conn_set_gif_icons_probe_timer (htlc, 0);
    }

    /* The server is confirmed capable — push our saved avatar (if the
	 * user picked one while offline / on a non-supporting server). No-op
	 * when there's nothing saved. */
    hx_icon_send_saved (htlc);

    /* Count first (out=NULL), then allocate exactly and fill — the
	 * Rust walker returns the total even when out is NULL. The count is
	 * server-controlled, so clamp it: a uid is a u16, so a well-formed
	 * list has at most 65536 entries; a hostile/buggy reply with massive
	 * duplication shouldn't drive a huge allocation + emit storm. */
    size_t n = gtkhx_proto_parse_icon_list (frame, frame_len, NULL, 0);
    /* A uid is u16, so the space is 65536 distinct values (0..65535) —
	 * clamp to that, not G_MAXUINT16, so a full list isn't off-by-one. */
    const size_t max_entries = (size_t) G_MAXUINT16 + 1;
    if (n > max_entries) {
        debug_log ("icon", "ICON_GETLIST reply: clamping %zu entries to %zu", n,
                   max_entries);
        n = max_entries;
    }
    debug_log ("icon", "ICON_GETLIST reply: %zu entr%s", n,
               n == 1 ? "y" : "ies");
    if (n == 0) {
        return;
    }
    struct gtkhx_proto_icon_entry *entries
        = g_new0 (struct gtkhx_proto_icon_entry, n);
    size_t got
        = gtkhx_proto_parse_icon_list (frame, frame_len, entries, n);
    if (got > n) {
        got = n; /* defensive: never iterate past the allocation */
    }
    for (size_t i = 0; i < got; i++) {
        hx_icon_data_recv (htlc, entries[i].uid, entries[i].gif_ptr,
                           (guint32) entries[i].gif_len);
    }
    g_free (entries);
}

/* ICON_CHANGE (1864) server broadcast: UID only. Parse + gif-icon-changed emit
 * live in the Rust hxicon-recv crate (rust/crates/hxicon-recv). */
extern void hx_icon_change_recv (struct htlc_conn *htlc, const guint8 *buf,
                                 gsize len);

void
hx_rcv_icon_change (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    hx_icon_change_recv (htlc, frame, frame_len);
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

/* chat-history-batch + initial-subject-discovery emits — Rust hxchat-recv
 * crate (rust/crates/hxchat-recv). */
extern void hx_chat_history_recv (struct htlc_conn *htlc, guint32 cid,
                                  GPtrArray *entries, int has_more);
extern void hx_chat_subject_emit (struct htlc_conn *htlc, guint32 cid,
                                  const char *subject);

void
rcv_task_chat_history (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, void *channel_ptr)
{
    guint32 cid = GPOINTER_TO_UINT (channel_ptr);
    GPtrArray *entries
        = g_ptr_array_new_with_free_func ((GDestroyNotify) hx_history_entry_free);
    gboolean has_more = FALSE;

    dh_start (frame, frame_len)
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

    /* advance the session-wide newest-msgid cursor used
	 * for AFTER= reconnect catch-up. This is independent of the
	 * per-chat oldest-msgid (gtkhx_chat::render.oldest_msgid) the
	 * Load-older flow uses — the cursor we maintain here grows
	 * monotonically over the htlc's lifetime, while the per-chat
	 * oldest shrinks as new older batches arrive. */
    for (guint i = 0; i < entries->len; i++) {
        HxHistoryEntry *e = g_ptr_array_index (entries, i);
        if (e && e->message_id > hx_conn_chat_history_last_msgid (htlc)) {
            hx_conn_set_chat_history_last_msgid (htlc, e->message_id);
        }
    }

    debug_log ("chat-history",
               "received batch: cid=%u entries=%u has_more=%d last_msgid=%"
               G_GUINT64_FORMAT,
               cid, entries->len, (int) has_more,
               hx_conn_chat_history_last_msgid (htlc));

    /* chat-history-batch emit — Rust hxchat-recv crate. */
    hx_chat_history_recv (htlc, cid, entries, has_more);

    /* Free the array (and via free_func, every entry inside) now
	 * that subscribers have had their pass. */
    g_ptr_array_unref (entries);
}

void
rcv_task_user_list (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct chat *chat, int text)
{
    guint16 uid;
    int new;

    dh_start (frame, frame_len)
    {
        if (_type == HTLS_DATA_USER_LIST) {
            /* chunk-record parsing moved to Rust
			 * gtkhx_proto_parse_user_list_record. The parser handles
			 * the 8-byte fixed header, two-stage nlen clamp
			 * (avail-first, then cap-31), strip_ansi, and the
			 * Colored-Nicknames trailer at `8 + clamped_nlen`. The C
			 * side keeps the "is this us?" adoption gate that sets
			 * hx_conn_uid (htlc) / ->color, and the GtkhxSession signal emit —
			 * all of which need session/chat objects the Rust layer
			 * doesn't see. */
            struct gtkhx_proto_user_list_record rec;
            char name_buf[32];
            if (!gtkhx_proto_parse_user_list_record (
                    dh->data, _len, (uint8_t *)name_buf, sizeof (name_buf),
                    &rec)) {
                continue;
            }
            uid = rec.uid;
            name_buf[rec.name_len] = 0;
            /* `new` means "not already in this chat's membership". Computed
             * per record: a stale new=1 from a previous new user would
             * otherwise spawn a spurious user_create for every subsequent
             * EXISTING user, doubling the UI row. Existence is a model query —
             * the same store every reader uses. */
            new = !hx_member_model_contains (hx_chat_member_model (chat), uid);

            /* Colored-Nicknames: mirror the trailer colour onto htlc when
             * this record is us (absent trailer => HX_NICK_COLOR_NONE). */
            if (rec.got_nick_color && uid == hx_conn_uid (htlc)) {
                hx_conn_set_nick_color (htlc, rec.nick_color);
            }
            /* "is this us?" adoption for servers that omit USER_LIST from
             * SELFINFO: the first record matching our nick+icon claims our
             * uid. (The server's status colour used to be mirrored onto
             * htlc->color here, but that field was write-only and is gone.) */
            if (!hx_conn_uid (htlc) && !strcmp (name_buf, hx_conn_name (htlc))
                && rec.icon == hx_conn_icon (htlc)) {
                hx_conn_set_uid (htlc, uid);
            }

            /* Same shared roster-apply as the live USER_CHANGE path, but
             * incremental=FALSE: a new record emits user-create with the join
             * chime suppressed (we're loading users already in the room at
             * login), and an existing record folds into the model silently. */
            hx_user_apply_recv (htlc, chat, hx_chat_member_model (chat), uid,
                                rec.nick_color, name_buf, rec.icon, rec.color,
                                new, /*skip_self_create=*/FALSE,
                                /*incremental=*/FALSE);
        }

        else if (_type == HTLS_DATA_CHAT_SUBJECT) {
            guint16 slen = (_len > 255) ? 255 : _len;
            hx_chat_set_subject (chat, (const char *) (dh->data), slen);
            /* Initial-subject-discovery path — the Rust hxchat-recv crate
			 * publishes chat-subject unconditionally (no 'Subject Changed
			 * to X' log line). */
            hx_chat_subject_emit (htlc, hx_chat_cid (chat),
                                  hx_chat_subject (chat));
        }
    }
    dh_end ();
}

void
rcv_task_user_list_switch (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct chat *chat)
{
    session *sess = sess_from_htlc (htlc);

    if (task_inerror (htlc, frame, frame_len)) {
        chat_delete (sess, chat);
        return;
    }

    rcv_task_user_list (htlc, frame, frame_len, chat, 0);
}

void
rcv_task_kick (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    if (task_inerror (htlc, frame, frame_len)) {
        return;
    }

    hx_printf_prefix (htlc, 0, INFOPREFIX, "%s\n", _ ("kick successful"));
}

void
rcv_task_user_info (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, guint16 *_uid, int text)
{
    char info[4096 + 1], name[32];
    guint16 uid = *_uid;
    g_free (_uid);

    /* chunk-walk + CR2LF + strip_ansi moved to the Rust
	 * hotline-proto crate's parse_user_info. The C side keeps the
	 * uid carry-through (it's a task parameter, not a chunk) and
	 * the `nlen && ilen` dispatch gate that filters out unanswered
	 * server frames. */
    struct gtkhx_proto_user_info ui;
    bool ok = gtkhx_proto_parse_user_info (frame, frame_len, (uint8_t *)name,
                                           sizeof (name), (uint8_t *)info,
                                           sizeof (info), &ui);
    if (ok && ui.name_len && ui.info_len) {
        /* user-info emit — Rust hxuser-recv crate. */
        hx_user_info_recv (uid, name, info, ui.info_len);
    }
}

void
rcv_task_file_list (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct cached_filelist *cfl,
                    void *data)
{
    struct hl_filelist_hdr *fh = 0;
    guint32 fh_len;
    guint16 fhlen;

    if (task_inerror (htlc, frame, frame_len)) {
        /* give the new-browser remote provider a chance
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
    dh_start (frame, frame_len)
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

                /* chunk layout moved to
				 * gtkhx_proto_build_file_list_chunks. Build BEFORE
				 * task_new — see hx_send_msg for the rationale. */
                struct hx_chunk chunks[1];
                int hc = (int)gtkhx_proto_build_file_list_chunks (
                    hldir, hldirlen, chunks, G_N_ELEMENTS (chunks));
                if (hc > 0) {
                    task_new (htlc,
                              RCV_TASK_FN (rcv_task_file_list), ncfl, 0,
                              "ls_complete");
                    hlwrite_chunks (htlc, HTLC_HDR_FILE_LIST, 0,
                                    chunks, hc);
                }
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
                    while ((guint8)*p == dir_char) {
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
					 * file_list populate path converts file entry
					 * names to UTF-8 (gtkhx_files_populate_from_reply
					 * in the hxfiles-entry crate); do the same here
					 * for the recursive-get path so both routes agree
					 * on the in-memory contract. */
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

    /* Reset completing BEFORE cfl_print: cfl_print emits the
     * file_list signal, and the remote-files-provider handler that
     * subscribes to it takes ownership of cfl and frees it
     * (files_remote_provider.c:290 — g_free (cfl)). Touching cfl
     * after the emit is a use-after-free; valgrind caught it on
     * the Open Files / navigate path. The recursive folder-download
     * path goes through cfl_print with data==NULL and isn't freed
     * by the handler — for that path this reorder is a harmless
     * no-op since completing is the next field we'd read anyway. */
    cfl->completing = COMPLETE_NONE;
    cfl_print (cfl, data);
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
rcv_task_file_getinfo (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, char *path)
{
    char type[32], crea[32];
    char name[256], comment[256];
    char created[32], modified[32];

    if (task_inerror (htlc, frame, frame_len)) {
        return;
    }

    /* chunk-walk + CR2LF + strip_ansi moved to Rust
	 * parse_file_getinfo. The C side keeps the formatted-date
	 * conversion (calls into hl_date which speaks the Hotline
	 * date stamp format) and the per-call session emit. */
    struct gtkhx_proto_file_getinfo f;
    bool ok = gtkhx_proto_parse_file_getinfo (
        frame, frame_len, (uint8_t *)name, sizeof (name),
        (uint8_t *)type, sizeof (type), (uint8_t *)crea, sizeof (crea),
        (uint8_t *)comment, sizeof (comment), &f);
    if (!ok) {
        return;
    }

    hx_format_hotline_date (f.date_create, created, sizeof created);
    hx_format_hotline_date (f.date_modify, modified, sizeof modified);

    /* file-info emit — Rust hxxfer-recv crate. */
    hx_file_info_recv (path, name, crea, type, comment, modified, created,
                       f.size64_seen ? f.size64 : (guint64)f.size);
}

/* Adapter matching hx_preview_cancel_fn (void (*)(void *)).
 * Registered with hx_preview_set_cancel_cb on the preview's HTXF
 * pointer so closing the preview window mid-transfer fires
 * xfer_delete on the right struct. A free-standing adapter is
 * required because xfer_delete takes a struct htxf_conn *: calling
 * it through a void* function-pointer type would be UB even though
 * the args are pointer-sized in practice. */
static void
preview_cancel_xfer_cb (void *user_data)
{
    xfer_delete ((struct htxf_conn *) user_data);
}

void
rcv_task_file_get (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct htxf_conn *htxf)
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
    if (task_inerror (htlc, frame, frame_len)) {
        if (htxf->opt.retry) {
            htxf->gone = 0;
            timer_add_secs (1, xfer_go_timer, htxf);
        } else {
            gtask_delete_htxf (sess_from_htlc (htlc), htxf);
            xfer_delete (htxf);
        }
        return;
    }

    /* chunk-walk moved to Rust parse_file_get_reply. The
	 * `(!size && !size64_seen) || !ref` dispatch gate stays here —
	 * malformed replies short-circuit the rest of the xfer kickoff. */
    struct gtkhx_proto_file_get_reply r;
    gtkhx_proto_parse_file_get_reply (frame, frame_len, &r);
    ref = r.ref_;
    size = r.size;
    size64 = r.size64;
    size64_seen = r.size64_seen != 0;
    queue = r.queue;

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
    g_strlcpy (htxf->serverhost, hx_conn_serverhost (htlc),
               sizeof (htxf->serverhost));
    htxf->serverport = hx_conn_serverport (htlc) + 1;

    /* For previews, build the GtkWindow + GtkTextView on the main
	 * thread (we are on the main thread here — this is the
	 * GIOChannel callback path). The download worker subsequently
	 * feeds bytes via htxf->preview without ever touching GTK,
	 * sidestepping a class of lockups we hit when constructing
	 * widgets and calling gtk_window_present from a worker. Built
	 * before hx_xfer_announce below because that call starts the
	 * download when unqueued, and the worker streams into
	 * htxf->preview. */
    if (htxf->opt.preview && !htxf->preview) {
        char *name = dirchar_basename (htxf->path);
        htxf->preview = hx_preview_new (name ? name : htxf->path);
        /* Wire the close-window-cancels-transfer hook. Without
		 * this, closing the preview mid-download silently leaves
		 * the worker streaming bytes into the (still-ref-held but
		 * invisible) byte buffer — wasted bandwidth with nowhere
		 * to render or save the result. preview_cancel_xfer_cb
		 * is the typed adapter that matches hx_preview_cancel_fn;
		 * casting xfer_delete directly would be calling a
		 * function through a mismatched pointer type (UB in C,
		 * even though both args are pointer-sized). */
        hx_preview_set_cancel_cb (htxf->preview, preview_cancel_xfer_cb,
                                  htxf);
    }

    hx_xfer_announce (htlc, htxf, htxf->queue);
}

/* HTLS reply to HTLC_HDR_FILE_GETFOLDER. Mirror of
 * rcv_task_file_get with the addition of HTLS_DATA_FILE_NFILES
 * (the server tells us how many file leaves the tree has so we
 * can display a non-trivial percentage even though the per-file
 * sizes come in as the stream unfolds). */
void
rcv_task_folder_get (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct htxf_conn *htxf)
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
    if (task_inerror (htlc, frame, frame_len)) {
        if (htxf->opt.retry) {
            htxf->gone = 0;
            timer_add_secs (1, xfer_go_timer, htxf);
        } else {
            gtask_delete_htxf (sess_from_htlc (htlc), htxf);
            xfer_delete (htxf);
        }
        return;
    }

    /* chunk-walk moved to Rust parse_folder_get_reply.
	 * Same scalar shape as file_get plus FILE_NFILES (folder file
	 * count). The C-side gate here is just `!ref` — folders are
	 * legal at total_size 0 (the `total_size = ... : 1` clamp
	 * below normalises that for the progress UI). */
    struct gtkhx_proto_folder_get_reply r;
    gtkhx_proto_parse_folder_get_reply (frame, frame_len, &r);
    ref = r.ref_;
    size = r.size;
    size64 = r.size64;
    size64_seen = r.size64_seen != 0;
    queue = r.queue;
    nfiles = r.nfiles;

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

    g_strlcpy (htxf->serverhost, hx_conn_serverhost (htlc),
               sizeof (htxf->serverhost));
    htxf->serverport = hx_conn_serverport (htlc) + 1;

    hx_xfer_announce (htlc, htxf, htxf->queue);
}

void
rcv_task_file_put (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct htxf_conn *htxf)
{
    guint32 ref = 0, data_pos = 0, rsrc_pos = 0, queue = 0;
    struct stat sb;

    if (task_inerror (htlc, frame, frame_len)) {
        gtask_delete_htxf (sess_from_htlc (htlc), htxf);
        xfer_delete (htxf);
        return;
    }

    /* chunk-walk moved to Rust parse_file_put_reply. RFLT
	 * resume offsets (data_pos at +46, rsrc_pos at +62) gate on
	 * len >= 66 in the Rust parser, matching the C extractor. */
    struct gtkhx_proto_file_put_reply r;
    gtkhx_proto_parse_file_put_reply (frame, frame_len, &r);
    ref = r.ref_;
    queue = r.queue;
    data_pos = r.data_pos;
    rsrc_pos = r.rsrc_pos;

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
    g_strlcpy (htxf->serverhost, hx_conn_serverhost (htlc),
               sizeof (htxf->serverhost));
    htxf->serverport = hx_conn_serverport (htlc) + 1;

    hx_xfer_announce (htlc, htxf, htxf->queue);
}

/* HTLS reply to HTLC_HDR_FILE_PUTFOLDER. The server has created
 * the destination folder root and is waiting for us to open the
 * HTXF subchannel and walk the local tree. Mirror of
 * rcv_task_file_put — same chunks, no resume RFLT (per-file
 * resume happens inside folder_put_thread, not at the task
 * boundary). */
void
rcv_task_folder_put (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, struct htxf_conn *htxf)
{
    guint32 ref = 0, queue = 0;

    if (task_inerror (htlc, frame, frame_len)) {
        gtask_delete_htxf (sess_from_htlc (htlc), htxf);
        xfer_delete (htxf);
        return;
    }

    /* chunk-walk moved to Rust parse_folder_put_reply.
	 * Strict subset of file_put — no RFLT (per-file resume happens
	 * inside folder_put_thread, not at the task boundary). */
    struct gtkhx_proto_folder_put_reply r;
    gtkhx_proto_parse_folder_put_reply (frame, frame_len, &r);
    ref = r.ref_;
    queue = r.queue;

    if (!ref) {
        return;
    }

    htxf->ref = ref;
    htxf->queue = queue;
    gettimeofday (&htxf->start, 0);

    g_strlcpy (htxf->serverhost, hx_conn_serverhost (htlc),
               sizeof (htxf->serverhost));
    htxf->serverport = hx_conn_serverport (htlc) + 1;

    hx_xfer_announce (htlc, htxf, htxf->queue);
}

/* Reply to our HTLC_HDR_DOWNLOAD_BANNER. The server gives us a
 * transfer reference and total byte count; banner.c spins up an
 * HTXF worker thread to actually fetch the bytes off
 * server_port + 1. */
void
rcv_task_banner_get (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len, void *ptr, void *data)
{
    guint32 ref = 0, size = 0;
    (void)ptr;
    (void)data;

    if (task_inerror (htlc, frame, frame_len)) {
        debug_log ("banner", "DOWNLOAD_BANNER task error from server");
        return;
    }

    /* chunk-walk moved to Rust parse_banner_get_reply.
	 * Just REF + SIZE — banner.c spins up an HTXF subchannel
	 * worker on the back of these two scalars. */
    struct gtkhx_proto_banner_get_reply r;
    gtkhx_proto_parse_banner_get_reply (frame, frame_len, &r);
    ref = r.ref_;
    size = r.size;

    debug_log ("banner", "DOWNLOAD_BANNER reply: ref=%u size=%u", ref, size);

    banner_handle_htxf_reply (htlc, ref, size);
}
