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
#include <glib/gstdio.h> /* g_mkdir (portable) */
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <gtk/gtk.h>
#include <sys/time.h>
#include <time.h>
#include "hx.h"
#include "gtkhx_session.h"
#include "network.h"
#include "xfers.h"
#include "chat.h"
#include "chat_members.h" /* hx_member_model_get_ignore */
#include "tasks.h"
#include "files.h"
#include "files_remote_provider.h"
#include "preview.h"
#include "gtkutil.h"
#include "msg.h"
#include "news.h"
#include "users.h"
#include "usermod.h"
#include "hxnet_bridge.h"
#include "hxnet_htxf.h" /* hxnet_hope_aead_free (HOPE AEAD handle) */
#include "rcv.h"
#include "hxconn.h"
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
            hx_get_chat_history (
                htlc, HX_HISTORY_CHANNEL_PUBLIC,
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
                                     /*before=*/0, /*after=*/0, (guint16)limit);
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
     * computation as the old g_ntohl(h->flag) & 1, with bounds
     * checking on a short buffer. */
    return gtkhx_proto_header_in_error (frame, frame_len) ? 1 : 0;
}

/* hx_rcv_chat (HTLS_HDR_CHAT) is a #[no_mangle] fn in the hxhandlers::recv::chat module
 * it parses the body via native hotline_proto::parse::parse_chat,
 * pulls the inline-media companion via native inline_media::extract_chat_media_meta
 * (dropping the line on an orphan), builds + attaches the boxed HxChatEvent via
 * the C producers, delegates the ignore-gate + emit to hx_chat_recv, and frees
 * the event. The dispatch switch below calls it by name (declared in rcv.h). */

/* Private-message ignore-gate + msg emit — Rust hxhandlers::recv::msg module. hx_msg_recv
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
    gboolean have_sender = hx_member_model_get_info (
        hx_chat_member_model (chat), pm.uid, &sender);

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
     * boxed event. The Rust hxhandlers::recv::msg module owns the shared ignore-gate + the
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

/* Agreement show-vs-auto-agree decision + emit — Rust hxhandlers::recv::agreement module.
 * Returns HX_AGREEMENT_ACT_AUTO_AGREE (C sends AGREEMENTAGREE) or
 * HX_AGREEMENT_ACT_SHOWN (the agreement signal fired; the view pops the
 * Agree window). */
#define HX_AGREEMENT_ACT_AUTO_AGREE 0
#define HX_AGREEMENT_ACT_SHOWN 1
extern int hx_agreement_recv (void *sess, int has_agreement, const char *buf,
                              guint16 len);

void
hx_rcv_agreement_file (struct htlc_conn *htlc, const guint8 *frame,
                       gsize frame_len)
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
                           (guint16)body_len)
        == HX_AGREEMENT_ACT_AUTO_AGREE) {
        hx_send_agreement_agree (htlc);
    }
}

/* hx_rcv_news_post (HTLS_HDR_NEWS_POST, the flat 1.0/1.2 news push) is a
 * #[no_mangle] fn in the hxhandlers::recv::news module (rust/crates/hxhandlers/src/recv/news.rs): it walks
 * the HTLS_DATA_NEWS chunks natively (hotline_proto::parse::news_post_chunks)
 * and emits one news-post line per chunk via hx_news_post_recv. The dispatch
 * switch below calls it by name (declared in rcv.h); no C body remains here. */

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
            const char *text = (task_error_extract (frame, frame_len, err_text,
                                                    sizeof (err_text), &err_len)
                                && err_len > 0)
                                   ? err_text
                                   : NULL;
            gtkhx_voice_runtime_task_error (sess->voice_runtime, opcode, text);
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

/* User-roster apply routing lives in the Rust hxhandlers::recv::user module
 * (rust/crates/hxhandlers/src/recv/user.rs). hx_user_apply_recv is shared by the Rust live
 * USER_CHANGE broadcast handler and the bulk USER_LIST load below —
 * `incremental` tells the two apart. */
extern int hx_user_apply_recv (struct htlc_conn *htlc, void *chat,
                               void *member_model, guint16 uid,
                               guint32 nick_color, const char *name,
                               guint16 icon, guint16 color, int is_new,
                               int skip_self_create, int incremental);
/* USER_INFO reply emit — Rust hxhandlers::recv::user module. (The SELFINFO self-updated
 * emit is now internal to hxhandlers::recv::user's hx_rcv_user_selfinfo.) */
extern void hx_user_info_recv (guint16 uid, const char *name, const char *info,
                               guint16 len);

/* hx_rcv_user_change (HTLS_HDR_USER_CHANGE) is a #[no_mangle] fn in the
 * hxhandlers::recv::user module (rust/crates/hxhandlers/src/recv/user.rs): it parses the frame natively,
 * resolves the chat, runs the native user_change::resolve plan (self-detection,
 * new-vs-change, colour/nick-colour preserve, rename-notice), routes the apply
 * through the shared hx_user_apply_recv, and does the join / rename logging
 * (showjoin-gated in the C shims) plus the self icon / nick-colour bookkeeping.
 * The dispatch switch below calls it by name (declared in rcv.h); no C body
 * remains here. */

/* hx_rcv_user_part (HTLS_HDR_USER_PART) is a #[no_mangle] fn in the hxhandlers::recv::user
 * crate: it parses the frame natively, resolves the chat, snapshots the leaving
 * member's name, and delegates the membership-gated user-delete emit to
 * hx_user_part_recv, then logs the showjoin-gated "parts" line. The dispatch
 * switch below calls it by name (declared in rcv.h); no C body remains here. */

/* hx_rcv_chat_subject (HTLS_HDR_CHAT_SUBJECT) is a #[no_mangle] fn in the
 * hxhandlers::recv::chat module module: it parses the frame,
 * resolves the chat, delegates the change-gate + emit to hx_chat_subject_recv,
 * and on a real change sets the model subject + emits the "chat-subject-notice"
 * signal for the "Subject Changed to" line (view-side handler in chat.c). The
 * dispatch switch below calls it by name (declared in rcv.h); no C body remains
 * here. */

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

/* hx_rcv_chat_invite (HTLS_HDR_CHAT_INVITE) is the first receive handler whose
 * whole body lives in Rust: it's now a #[no_mangle] fn in the hxhandlers::recv::chat module
 * (rust/crates/hxhandlers/src/recv/chat.rs) that parses the frame, resolves the public chat's
 * member model via chat_with_cid/hx_chat_member_model, and delegates the
 * ignore-gate + emit to hx_chat_invite_recv. The dispatch switch below calls it
 * by name (declared in rcv.h); no C body remains here.
 * See docs/rust/network-endgame.md. */

/* hx_rcv_user_selfinfo (HTLS_HDR_USER_SELFINFO) is a #[no_mangle] fn in the
 * hxhandlers::recv::user module (rust/crates/hxhandlers/src/recv/user.rs): it calls hx_selfinfo_parse
 * (proto_helpers.c chunk walker → htlc access/uid/icon), flips the logged-in
 * flag (SELFINFO is the canonical login-complete signal the agreement Agree
 * button reads), and emits self-updated via hx_selfinfo_recv so the view
 * refreshes toolbar sensitivity. Post-login fetches are deliberately NOT fired
 * here — in the 1.5 flow SELFINFO precedes the agreement, so USER_GETLIST / news
 * go out from hx_send_agreement_agree after AGREEMENTAGREE. The dispatch switch
 * below calls it by name (declared in rcv.h); no C body remains here. */

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
    hx_fsync (fd);
    close (fd);
}

/* Shared file-transfer reply tail — Rust hxhandlers::recv::xfer module. Emits the transfer's
 * queue position to the tasks view, then (when queue == 0) starts the byte
 * stream via xfer_ready_write. Called by all five xfer reply handlers once
 * they've stamped ref/size/queue onto htxf. */
extern void hx_xfer_announce (struct htlc_conn *htlc, struct htxf_conn *htxf,
                              guint32 queue);

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
hx_rcv_voice_sdp_offer (struct htlc_conn *htlc, const guint8 *frame,
                        gsize frame_len)
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
        debug_log ("voice", "← VOICE_SDP_OFFER cid=%u: missing VOICE_SDP chunk",
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
                                        (const uint8_t **)&sdp_ptr, &sdp_len)) {
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
    debug_log ("voice",
               "← VOICE_SDP_OFFER cid=%u sdp_len=%u mids=%u unknown_mids=%u "
               "bundle=%u has_pcmu=%d disabled_slot=%d",
               r.cid, r.sdp_len, sum.mid_count, sum.unknown_mid_count,
               sum.bundle_count, (int)sum.has_pcmu, (int)sum.has_disabled_slot);

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
        (void)htlc;
        if (sess && sess->voice_runtime && sdp_ptr && sdp_len > 0) {
            char *sdp_str = g_malloc (sdp_len + 1);
            memcpy (sdp_str, sdp_ptr, sdp_len);
            sdp_str[sdp_len] = '\0';
            gtkhx_voice_runtime_sdp_offer (sess->voice_runtime, r.cid, sdp_str);
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
        debug_log ("voice", "← VOICE_ICE cid=%u (end-of-candidates)", r.cid);
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
            gtkhx_voice_runtime_ice_candidate (sess->voice_runtime, r.cid,
                                               NULL);
        }
        return;
    }

    const guint8 *ice_ptr = NULL;
    gsize ice_len = 0;
    if (!gtkhx_proto_voice_reply_field (frame, frame_len, 1,
                                        (const uint8_t **)&ice_ptr, &ice_len)) {
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
        debug_log ("voice", "← VOICE_ICE cid=%u ice_len=%zu: JSON parse failed",
                   r.cid, ice_len);
        return;
    }
    debug_log ("voice",
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
        (void)htlc;
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
hx_rcv_voice_room_status (struct htlc_conn *htlc, const guint8 *frame,
                          gsize frame_len)
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
                                        (const uint8_t **)&blob, &blob_len)) {
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
    enum { MAX_LOG_ENTRIES = 64 };
    struct gtkhx_proto_voice_participant ents[MAX_LOG_ENTRIES];
    size_t n = gtkhx_proto_parse_voice_participants (blob, blob_len, ents,
                                                     MAX_LOG_ENTRIES);
    debug_log ("voice",
               "← VOICE_ROOM_STATUS cid=%u participants=%zu (blob=%zu)", r.cid,
               n, blob_len);
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
        (void)htlc;
        if (sess && sess->voice_runtime) {
            gtkhx_voice_runtime_room_status (sess->voice_runtime, r.cid, blob,
                                             blob_len);
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
rcv_task_voice_join (struct htlc_conn *htlc, const guint8 *frame,
                     gsize frame_len, void *channel_ptr)
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
        debug_log ("voice",
                   "← VOICE_JOIN reply cid=%u (expected %u) — server echoed "
                   "different room",
                   r.cid, expected_cid);
    }

    if (!r.sdp_present || !r.codec_present || !r.participants_present) {
        debug_log ("voice",
                   "← VOICE_JOIN reply cid=%u: malformed (sdp=%d codec=%d "
                   "participants=%d)",
                   r.cid, (int)r.sdp_present, (int)r.codec_present,
                   (int)r.participants_present);
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
                                        (const uint8_t **)&sdp_ptr, &sdp_len)) {
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
    if (!gtkhx_proto_voice_reply_field (
            frame, frame_len, 2, (const uint8_t **)&codec_ptr, &codec_len)) {
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
                                        (const uint8_t **)&blob, &blob_len)) {
        debug_log ("voice",
                   "← VOICE_JOIN reply cid=%u: field walker rejected "
                   "VOICE_PARTICIPANTS after presence check",
                   r.cid);
        return;
    }
    enum { MAX_LOG_ENTRIES = 64 };
    struct gtkhx_proto_voice_participant ents[MAX_LOG_ENTRIES];
    size_t n = gtkhx_proto_parse_voice_participants (blob, blob_len, ents,
                                                     MAX_LOG_ENTRIES);

    debug_log ("voice",
               "← VOICE_JOIN reply cid=%u codec=%s sdp_len=%u "
               "mids=%u has_pcmu=%d participants=%zu",
               r.cid, codec, r.sdp_len, sum.mid_count, (int)sum.has_pcmu, n);
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
        (void)htlc;
        if (sess && sess->voice_runtime) {
            gtkhx_voice_runtime_room_status (sess->voice_runtime, r.cid, blob,
                                             blob_len);
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
rcv_task_voice_simple_ack (struct htlc_conn *htlc, const guint8 *frame,
                           gsize frame_len, void *opcode_ptr, void *cid_ptr)
{
    /* opcode is stashed in ptr via GUINT_TO_POINTER for the trace
     * label; cid via the data slot. Both are diagnostic only — the
     * empty-success-reply path doesn't carry any state worth
     * extracting. task_inerror is handled before this is called by
     * hx_rcv_task; we only see the success path. */
    guint32 opcode = GPOINTER_TO_UINT (opcode_ptr);
    guint32 cid = GPOINTER_TO_UINT (cid_ptr);
    (void)htlc;
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
                   guint32 type, guint32 trans, guint32 flag, guint32 body_len)
{
    /* Wire len field encodes body_len + the 2-byte hc; the proto trace wants
     * that raw value. */
    proto_trace_recv_hdr (type, trans, flag,
                          body_len + (guint32)sizeof (guint16));

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
rcv_task_user_open (struct htlc_conn *htlc, const guint8 *frame,
                    gsize frame_len, struct uesp_fn *uespfn)
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
        frame, frame_len, (uint8_t *)name, sizeof (name), (uint8_t *)login,
        sizeof (login), (uint8_t *)pass, sizeof (pass), &ar);
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
rcv_task_msg (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len,
              char *msg_buf)
{
    if (msg_buf) {
        hx_printf (htlc, 0, "%s\n", msg_buf);
        g_free (msg_buf);
    }
}

/* rcv_task_newscat_list moved to the hxhandlers::recv::news Rust crate — it parses the
 * CATLIST chunk to an owned handle, stashes it on the gnews_catalog carrier, and
 * emits news-catalog, with no intermediate news_item / news_group. hxhandlers::send::news's
 * task_new still registers it; the symbol now resolves against hxhandlers::recv::news. */

/* rcv_task_newsfolder_list moved to the hxhandlers::recv::news Rust crate — it parses the
 * NEWSDIRLIST chunks (the dh_start walk + per-chunk parsers) into an owned
 * DirList handle via gtkhx_proto_parse_dirlist, stashes it on the gnews_folder
 * carrier, and emits news-folder, with no intermediate folder_item / news_folder.
 * hxhandlers::send::news's task_new still registers it; the symbol now resolves against
 * hxhandlers::recv::news. */

/* rcv_task_news_post moved to the hxhandlers::recv::news Rust crate — it parses the
 * GETTHREAD NEWSDATA body (gtkhx_proto_parse_news_thread_reply), bails on a
 * TASK_ERROR / body-less reply, then builds the news_post carrier via
 * news_post_new (news_recv_bridge.c) and emits news-thread. hxhandlers::send::news's
 * get_post sender still registers it; the symbol now resolves against
 * hxhandlers::recv::news. With this, no news code remains in rcv.c. */

/* rcv_task_news_users / rcv_task_user_list / rcv_task_user_list_switch /
 * rcv_task_user_info moved to the hxhandlers Rust crate (recv/user.rs): they
 * walk the reply chunks natively (hotline_proto::parse::parse_user_list_record /
 * parse_user_info) and fold into the roster through the shared, already-Rust
 * hx_user_apply_recv — no C chunk-walk or C↔Rust bounce. The C senders still
 * register them via RCV_TASK_FN(); the symbols resolve against the Rust crate at
 * link. rcv_task_kick stays here (it logs via the variadic hx_printf_prefix). */

/* Post-login fetch sequencing decision — Rust hotline-proto (login module).
 * Returns HX_POST_LOGIN_FETCH_NOW (1.0/1.2: fire fetches now),
 * HX_POST_LOGIN_ARM_FALLBACK (1.5+: wait for AGREEMENTAGREE, arm the 2s timer),
 * or HX_POST_LOGIN_NOTHING (already fetched). */
#define HX_POST_LOGIN_NOTHING 0
#define HX_POST_LOGIN_FETCH_NOW 1
#define HX_POST_LOGIN_ARM_FALLBACK 2
extern int hx_post_login_route (guint16 version, int already_fetched);

void
rcv_task_login (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len,
                char *pass)
{
    char buf[HOSTLEN];
    char servername[8192 + 1];

    g_strlcpy (buf, hx_conn_ip_addr (htlc)[0] ? hx_conn_ip_addr (htlc) : "?",
               sizeof (buf));

    if (!pass) {
        hx_printf_prefix (htlc, 0, INFOPREFIX, "%s:%u: %s %s\n", buf,
                          hx_conn_serverport (htlc), _ ("login"),

                          task_inerror (htlc, frame, frame_len)
                              ? _ ("failed?")
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
            frame, frame_len, (uint8_t *)servername, sizeof (servername), &li);

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
            server_addr
                = gtkhx_text_to_utf8 (servername, strlen (servername), NULL);
        }
        if (login_seen & HX_LOGIN_SEEN_CAPS) {
            /* DATA_CAPABILITIES echo — the bits the server agreed to
             * enable for this session. Bits we don't recognise are
             * preserved per the spec's "ignore unknown bits" rule. */
            hx_conn_set_caps (htlc, li.caps);
            if (li.caps & HTLC_CAP_LARGE_FILES) {
                hx_printf_prefix (htlc, 0, INFOPREFIX,
                                  _ ("server confirmed large-file (64-bit) "
                                     "mode for this session\n"));
            }
            if (li.caps & HTLC_CAP_TEXT_ENCODING) {
                hx_printf_prefix (htlc, 0, INFOPREFIX,
                                  _ ("server confirmed UTF-8 text encoding "
                                     "for this session\n"));
            }
            if (li.caps & HTLC_CAP_CHAT_HISTORY) {
                hx_printf_prefix (htlc, 0, INFOPREFIX,
                                  _ ("server confirmed chat-history extension "
                                     "for this session\n"));
            }
            if (li.caps & HTLC_CAP_INLINE_MEDIA) {
                hx_printf_prefix (htlc, 0, INFOPREFIX,
                                  _ ("server confirmed inline-media extension "
                                     "for this session\n"));
            }
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_BYTES) {
            hx_conn_set_media_max_bytes (htlc, li.media_max_bytes);
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_DIMENSION) {
            hx_conn_set_media_max_dimension (htlc, li.media_max_dimension);
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_PIXELS) {
            hx_conn_set_media_max_pixels (htlc, li.media_max_pixels);
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_CHUNK_SIZE) {
            hx_conn_set_media_chunk_size (htlc, li.media_chunk_size);
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_FRAMES) {
            hx_conn_set_media_max_frames (htlc, li.media_max_frames);
        }
        if (login_seen & HX_LOGIN_SEEN_MEDIA_MAX_DURATION_MS) {
            hx_conn_set_media_max_duration_ms (htlc, li.media_max_duration_ms);
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

/* rcv_task_news_file (the flat NEWS_FILE task reply — the whole 1.0/1.2 news
 * document) is a #[no_mangle] fn in the hxhandlers::recv::news module: it parses the first
 * HTLS_DATA_NEWS chunk natively (hotline_proto::parse::parse_news_file) and
 * publishes it via hx_news_file_recv, emitting an empty document on a chunk-less
 * reply. hxhandlers::send::news registers it as the reply callback (declared in rcv.h); no
 * C body — and no news_buf/news_len scratch — remains here. */

/* GIF-icons extension (fogWraith GIF-Icons.md). The ICON_GET / ICON_GETLIST
 * task-reply handlers (rcv_task_icon_get / rcv_task_icon_getlist) moved to the
 * hxhandlers Rust crate (rust/crates/hxhandlers/src/recv/icon.rs): each walks
 * the reply natively (crate::gif_icons), flips the probe negotiation state via
 * the hx_conn_gif_icons_* accessors, and publishes avatars through
 * hx_icon_data_recv (also Rust). The C senders (gif_icons.c) still register them
 * via RCV_TASK_FN(); the symbols resolve against the Rust crate at link. */

/* ICON_CHANGE (1864) server broadcast: UID only. Parse + gif-icon-changed emit
 * live in the Rust hxhandlers::recv::icon module (rust/crates/hxhandlers/src/recv/icon.rs). */
extern void hx_icon_change_recv (struct htlc_conn *htlc, const guint8 *buf,
                                 gsize len);

void
hx_rcv_icon_change (struct htlc_conn *htlc, const guint8 *frame,
                    gsize frame_len)
{
    hx_icon_change_recv (htlc, frame, frame_len);
}

/* rcv_task_chat_history (the TRAN_GET_CHAT_HISTORY 700 reply walker) moved to the
 * hxhandlers Rust crate (recv/chat.rs): it walks the reply chunks natively, builds
 * the GPtrArray<HxHistoryEntry*> via glib + the native hx_history_entry_parse,
 * advances the newest-msgid cursor, and emits chat-history-batch. The reply task
 * is registered via RCV_TASK_FN(task_new) at each send call site — chat.c's
 * Load-older flow and hx_post_login_fetches below — right before calling
 * hx_get_chat_history (chat_history.c itself stays free of tasks.h/rcv.h). The
 * symbol resolves against the Rust crate at link. */

/* rcv_task_user_list / rcv_task_user_list_switch / rcv_task_user_info moved to
 * the hxhandlers Rust crate (recv/user.rs) — see the note above rcv_task_login. */

void
rcv_task_kick (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    if (task_inerror (htlc, frame, frame_len)) {
        return;
    }

    hx_printf_prefix (htlc, 0, INFOPREFIX, "%s\n", _ ("kick successful"));
}
