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
#include "users.h"
#include "usermod.h"
#include "hxnet_bridge.h"
#include "hxnet_htxf.h" /* hxnet_hope_aead_free (HOPE AEAD handle) */
#include "rcv.h"
#include "hxconn.h"
#include "hfs.h"
#include "hotline_proto.h"
#include "debug.h"
#include "connect.h"
#include "banner.h"
#include "chat_history.h"
#include "inline_media.h"
#include "sound.h"
#include "text_util.h"
#include "gif_icons.h"
#include "hl_access.h"
#ifdef HAVE_VOICE
#include "voice_runtime.h"
#include "voice_model.h"
#endif

/* What follows the login, on the session's LOGIN_READY: a 1.5+ server
 * takes requests before AGREEMENTAGREE as from a user not yet joined. The
 * files browser's remote provider waits on hx_conn_post_login_fetched. */
void
hx_post_login_fetches (struct htlc_conn *htlc)
{
    if (hx_conn_post_login_fetched (htlc)) {
        return;
    }
    hx_conn_set_post_login_fetched (htlc, 1);

    /* AGREEMENTAGREE carries no DATA_COLOR, so our color goes in a
     * USER_CHANGE, which is also the opt-in to others' colors. A 1.2 server
     * knows nothing of colors and has our name already. */
    if (hx_conn_nick_color (htlc) != HX_NICK_COLOR_NONE
        && hx_conn_version (htlc) != 0) {
        hx_change_name_icon (htlc);
    }

    /* The news follows the user list's reply. */
    hx_user_list_get (htlc);

    /* GIF-icons extension: probe for support (no capability bit). Sends
     * ICON_GETLIST and arms a watchdog; a reply marks the session
     * supported and delivers any avatars already set, a timeout marks
     * it unsupported. Safe against legacy servers. */
    hx_icon_probe (htlc);

    hx_chat_history_fetch_initial (htlc);

    /* Announce the spec-correct "fully joined" boundary to the UI.
     * Consumers (e.g. the files browser's remote provider) use this
     * to defer post-login RPCs like FILE_LIST until after the server
     * has accepted our AGREEMENTAGREE — sending one before that
     * trips "action attributed to not-yet-joined session" errors on
     * 1.5+ servers and outright disconnects on the stricter ones. */
    gtkhx_session_emit_connection_state (gtkhx_session_get_default (), htlc,
                                         GTKHX_CONNECTION_LOGIN_READY);
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
     * hxproto crate (gtkhx_proto_header_in_error). Same
     * computation as the old g_ntohl(h->flag) & 1, with bounds
     * checking on a short buffer. */
    return gtkhx_proto_header_in_error (frame, frame_len) ? 1 : 0;
}

/* An agreement with text is shown; the session has answered any other. */
void
hx_rcv_agreement_file (struct htlc_conn *htlc, const guint8 *frame,
                       gsize frame_len)
{
    /* The protocol's chunk length is 16 bits; mhxd agreements on public
     * servers run 1-2 KiB. */
    char buf[16384];
    gsize body_len = 0;
    hx_agreement_result r
        = hx_agreement_extract (frame, frame_len, buf, sizeof (buf), &body_len);

    if (r == HX_AGREEMENT_OK && body_len > 0) {
        gtkhx_session_emit_agreement (gtkhx_session_get_default (),
                                      sess_from_htlc (htlc), buf,
                                      (guint16)body_len);
    }
}

#ifdef HAVE_VOICE
/* Whether `label` names a voice or video request whose refusal the voice
 * runtime reports itself. Its state machine turns every one into an Error
 * signal carrying the server's text, which the voice panel shows, so the
 * generic toast would say it a second time. */
static gboolean
voice_reports_error (const char *label)
{
    static const char *const labels[] = {
        "voice-join",      "voice-leave",        "voice-sdp-answer",
        "voice-mute",      "video-start-camera", "video-start-screen",
        "video-stop",      "video-state-camera", "video-state-screen",
        "video-subscribe",
    };
    if (!label) {
        return FALSE;
    }
    for (gsize i = 0; i < G_N_ELEMENTS (labels); i++) {
        if (!strcmp (label, labels[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

/* The server's text for a refused voice or video request, as UTF-8, or NULL
 * when it gave none. The runtime takes C strings as UTF-8, and a Mac server's
 * text is MacRoman, so convert here as toolbar_show_toast would have: once
 * the runtime has read it, the bytes it couldn't decode are already gone. */
static char *
voice_error_text (const guint8 *frame, gsize frame_len)
{
    char buf[8192 + 1];
    gsize len = 0;
    if (!task_error_extract (frame, frame_len, buf, sizeof (buf), &len)
        || len == 0) {
        return NULL;
    }
    if (g_utf8_validate (buf, -1, NULL)) {
        return g_strdup (buf);
    }
    return gtkhx_text_to_utf8 (buf, strlen (buf), NULL);
}
#endif /* HAVE_VOICE */

void
hx_rcv_task (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    guint32 trans = 0;
    struct task *tsk;
    char error = 0;

    /* transaction-id extraction moved to the Rust
     * hxproto crate (replaces HN32(&trans, &h->trans)). A
     * short buffer leaves trans at 0, which task_with_trans treats
     * as "no such task" — the same safe fallthrough as before. */
    gtkhx_proto_header_trans (frame, frame_len, &trans);
    tsk = task_with_trans (sess_from_htlc (htlc), trans);

    /* Login-time requests whose rejection is expected and non-actionable:
     * the GIF-icons probe (a task error is just "unsupported") and the
     * saved avatar's automatic re-send (a guest, a rate limit). Their own
     * rcv handler takes the error (dispatched below), so suppress the
     * generic toast + ERROR sound — otherwise every login nags the user
     * about a request they never made. */
    gboolean silent_probe = tsk && tsk->str
                            && (!strcmp (tsk->str, "icon-list")
                                || !strcmp (tsk->str, "icon-set-auto"));
#ifdef HAVE_VOICE
    {
        session *vsess = sess_from_htlc (htlc);
        if (tsk && vsess && vsess->voice_runtime
            && voice_reports_error (tsk->str)) {
            /* Not a probe, but the same treatment: the voice panel
             * shows this one. */
            silent_probe = TRUE;
        }
    }
#endif /* HAVE_VOICE */

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
        } else if (!strcmp (tsk->str, "video-stop")) {
            opcode = HTLC_HDR_VIDEO_STOP;
        } else if (!strcmp (tsk->str, "video-subscribe")) {
            opcode = HTLC_HDR_VIDEO_SUBSCRIBE;
        }
        /* A refused Video Start or Video State names its kind in the task
         * label: the state machine has to undo that request and no other.
         * send_video keeps the room in the task's data and the machine's
         * generation for the request in its ptr. */
        guint16 start_kind = 0, state_kind = 0;
        if (!strcmp (tsk->str, "video-start-camera")) {
            start_kind = HX_VIDEO_KIND_CAMERA;
        } else if (!strcmp (tsk->str, "video-start-screen")) {
            start_kind = HX_VIDEO_KIND_SCREEN;
        } else if (!strcmp (tsk->str, "video-state-camera")) {
            state_kind = HX_VIDEO_KIND_CAMERA;
        } else if (!strcmp (tsk->str, "video-state-screen")) {
            state_kind = HX_VIDEO_KIND_SCREEN;
        }
        if ((start_kind || state_kind || opcode) && sess
            && sess->voice_runtime) {
            g_autofree char *text = voice_error_text (frame, frame_len);
            guint32 cid = GPOINTER_TO_UINT (tsk->data);
            guint32 gen = GPOINTER_TO_UINT (tsk->ptr);
            if (start_kind) {
                gtkhx_voice_runtime_video_start_failed (
                    sess->voice_runtime, cid, start_kind, gen, text);
            } else if (state_kind) {
                gtkhx_voice_runtime_video_state_failed (
                    sess->voice_runtime, cid, state_kind, gen, text);
            } else {
                gtkhx_voice_runtime_task_error (sess->voice_runtime, opcode,
                                                text);
            }
            /* The generic path was skipped for these; its toast is the
             * voice panel's to show, but the alert is still ours. */
            play_sound (ERROR);
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
         * fired from hxhandlers::send::files). Their rcv functions
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

/* hx_rcv_user_selfinfo (HTLS_HDR_USER_SELFINFO) is a #[no_mangle] fn in the
 * hxhandlers::recv::user module (rust/crates/hxhandlers/src/recv/user.rs): it calls hx_selfinfo_parse
 * (proto_helpers.c chunk walker → htlc access/uid/icon), flips the logged-in
 * flag (SELFINFO is the canonical login-complete signal the agreement Agree
 * button reads), and emits self-updated via hx_selfinfo_recv so the view
 * refreshes toolbar sensitivity. Post-login fetches are deliberately NOT fired
 * here — in the 1.5 flow SELFINFO precedes the agreement, so USER_GETLIST / news
 * wait for the session's LOGIN_READY, after AGREEMENTAGREE. The dispatch switch
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
 * The handlers below parse the body via the Rust hxproto::voice
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

    /* The runtime re-parses the blob for its mid -> user map. The voice
     * model behind the speaker indicators takes only the room this client
     * is in, as the 611 path does: a 605 for a room just left would land
     * after the model was cleared and become the next room's baseline. */
    session *sess = sess_from_htlc (htlc);
    if (sess && sess->voice_runtime) {
        gtkhx_voice_runtime_room_status (sess->voice_runtime, r.cid, blob,
                                         blob_len);
    }
    uint32_t active_cid = 0;
    if (sess && sess->voice_model && sess->voice_runtime
        && gtkhx_voice_runtime_active_cid (sess->voice_runtime, &active_cid)
        && active_cid == r.cid) {
        hx_voice_model_ingest_participants (
            sess->voice_model, blob, blob_len,
            hx_conn_has_cap (htlc, HTLC_CAP_VIDEO));
    }
}

/* Video Status (611): the room's complete publication list, replacing
 * whatever was known. It goes to the runtime, which owns what the video
 * panel shows and what this client subscribes to, and to the voice
 * model, which marks publishers in the user list whether or not anyone
 * is watching them. */
void
hx_rcv_video_status (struct htlc_conn *htlc, const guint8 *frame,
                     gsize frame_len)
{
    struct gtkhx_proto_video_reply r;
    gtkhx_proto_parse_video_reply (frame, frame_len, &r);
    debug_log ("voice", "← VIDEO_STATUS cid=%u publishers=%zu", r.cid,
               r.publishers_len / 8);

    session *sess = sess_from_htlc (htlc);
    if (!sess) {
        return;
    }
    if (sess->voice_runtime) {
        gtkhx_voice_runtime_video_status (sess->voice_runtime, r.cid,
                                          r.publishers_ptr, r.publishers_len);
    }
    /* The user list shows the room this client is in. A 611 that races
     * a leave or a room switch would put stale flags back after the
     * model was cleared, and no later 611 would take them down. */
    uint32_t active_cid = 0;
    if (sess->voice_model && sess->voice_runtime
        && gtkhx_voice_runtime_active_cid (sess->voice_runtime, &active_cid)
        && active_cid == r.cid) {
        hx_voice_model_ingest_video_publishers (
            sess->voice_model, r.publishers_ptr, r.publishers_len);
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

    /* The reply's SDP offer starts the answer; its participants fill the
     * runtime's mid -> user map and, for the room this client is in, are
     * the voice model's first list. A reply for a room already switched
     * away from would otherwise become the next room's baseline. */
    session *sess = sess_from_htlc (htlc);
    if (sess && sess->voice_runtime) {
        gtkhx_voice_runtime_room_status (sess->voice_runtime, r.cid, blob,
                                         blob_len);
        if (sdp_ptr && sdp_len > 0) {
            char *sdp_str = g_malloc (sdp_len + 1);
            memcpy (sdp_str, sdp_ptr, sdp_len);
            sdp_str[sdp_len] = '\0';
            gtkhx_voice_runtime_sdp_offer (sess->voice_runtime, r.cid, sdp_str);
            g_free (sdp_str);
        }
    }
    uint32_t active_cid = 0;
    if (sess && sess->voice_model && sess->voice_runtime
        && gtkhx_voice_runtime_active_cid (sess->voice_runtime, &active_cid)
        && active_cid == r.cid) {
        hx_voice_model_ingest_participants (
            sess->voice_model, blob, blob_len,
            hx_conn_has_cap (htlc, HTLC_CAP_VIDEO));
    }
}

void
rcv_task_voice_simple_ack (struct htlc_conn *htlc, const guint8 *frame,
                           gsize frame_len, void *tag_ptr, void *cid_ptr)
{
    /* The ptr slot holds the opcode, or for a video start or state the
     * state machine's generation, which only the error path above uses;
     * cid is in the data slot. Both are diagnostic only here — the
     * empty-success-reply path doesn't carry any state worth
     * extracting. task_inerror is handled before this is called by
     * hx_rcv_task; we only see the success path. */
    guint32 tag = GPOINTER_TO_UINT (tag_ptr);
    guint32 cid = GPOINTER_TO_UINT (cid_ptr);
    (void)htlc;
    debug_log ("voice", "← VOICE ack (tag=%u cid=%u)", tag, cid);
}
#endif /* HAVE_VOICE */

/* Dispatch a received frame. The Rust hxnet actor already parsed the header
 * and the bridge hands us the whole frame (22-byte header + body) as a
 * (frame, frame_len) slice, so this no longer re-decodes the header or runs
 * the old two-phase receive state machine — it routes the opcode to a body
 * handler (via the Rust dispatch::route table behind hx_recv_route), and calls
 * it. The session's tap has traced it already (hx_recv_session_event). */
void
hx_dispatch_frame (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len,
                   guint32 type, guint32 trans G_GNUC_UNUSED,
                   guint32 flag G_GNUC_UNUSED, guint32 body_len G_GNUC_UNUSED)
{
    void (*handler) (struct htlc_conn *, const guint8 *, gsize) = NULL;
    switch (hx_recv_route (type)) {
    case HX_RECV_TASK:
        handler = hx_rcv_task;
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
    case HX_RECV_VIDEO_STATUS:
        handler = hx_rcv_video_status;
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
     * PASSWORD moved to the Rust hxproto crate's
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
        /* On the connection, not a global. `connected` named whichever
         * connection had most recently logged in — the same thing at one and a
         * race at two, with the toolbar's Disconnect button reading it. */
        hx_conn_set_logged_in (htlc, 1);

        /* Seed the opaque HOPE AEAD material handle (if the orchestrated
         * control channel negotiated ChaCha20-Poly1305). The handshake
         * is complete by now, so the retained material is populated;
         * HTXF subchannels (banner.c / xfers.c) read htlc->hope_aead to
         * derive their per-transfer keys in-process. NULL for plaintext
         * / Blowfish / no-cipher. Freed on connection teardown. */
        if (hx_conn_hope_aead (htlc)) {
            hxnet_hope_aead_free (hx_conn_hope_aead (htlc));
        }
        hx_conn_set_hope_aead (htlc, hx_bridge_orchestrated_hope_aead (htlc));

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
        hx_conn_reset_video_limits (htlc);

        /* The LOGIN reply chunk-walk moved to the Rust hxproto crate
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
            /* On the session this reply arrived for, not a global — the name
             * belongs to one server, and a second connection logging in used
             * to overwrite the first's.
             *
             * Server names from old Hotline servers are 8-bit Mac Roman text,
             * not UTF-8 — and gtk_window_set_title et al. assert UTF-8.
             * gtkhx_text_to_utf8 handles the already-UTF-8 / Mac-Roman /
             * fall-back-to-substitute cascade. The window title picks it up
             * when the "logged-in" signal is emitted after this walk. */
            {
                session *ss = sess_from_htlc (htlc);
                if (ss) {
                    g_free (ss->server_name);
                    ss->server_name = gtkhx_text_to_utf8 (
                        servername, strlen (servername), NULL);
                }
            }
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
        /* Video ceilings, one field per kind the server supports. Kept
         * on the connection and handed to the voice runtime when it is
         * built (or now, if it already exists), so the encoder starts
         * inside them rather than learning them by rejection. */
        {
            static const struct {
                unsigned seen;
                guint16 kind;
            } video_kinds[] = {
                { HX_LOGIN_SEEN_VIDEO_CAMERA_LIMITS, HX_VIDEO_KIND_CAMERA },
                { HX_LOGIN_SEEN_VIDEO_SCREEN_LIMITS, HX_VIDEO_KIND_SCREEN },
            };
            for (gsize i = 0; i < G_N_ELEMENTS (video_kinds); i++) {
                if (!(login_seen & video_kinds[i].seen)) {
                    continue;
                }
                const struct gtkhx_proto_login_video_limits *vl
                    = &li.video_limits[video_kinds[i].kind - 1];
                hx_conn_set_video_limits (htlc, video_kinds[i].kind,
                                          vl->max_width, vl->max_height,
                                          vl->max_fps, vl->max_bitrate);
#ifdef HAVE_VOICE
                session *vs = sess_from_htlc (htlc);
                if (vs && vs->voice_runtime) {
                    gtkhx_voice_runtime_set_video_limits (
                        vs->voice_runtime, video_kinds[i].kind, vl->max_width,
                        vl->max_height, vl->max_fps, vl->max_bitrate);
                }
#endif
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
    }
}

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

void
rcv_task_kick (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    if (task_inerror (htlc, frame, frame_len)) {
        return;
    }

    hx_printf_prefix (htlc, 0, INFOPREFIX, "%s\n", _ ("kick successful"));
}
