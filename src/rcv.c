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
#include "rcv.h"
#include "hxconn.h"
#include "hfs.h"
#include "hotline_proto.h"
#include "debug.h"
#include "connect.h"
#include "banner.h"
#include "chat_history.h"
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

    if (task_inerror (htlc, frame, frame_len)) {
        task_error (htlc, frame, frame_len);
        error = 1;
    }
    if (tsk) {
        /* XXX tsk->rcv might call task_delete */
        if (tsk->rcv && !error) {
            tsk->rcv (htlc, frame, frame_len, tsk->ptr, tsk->data);
        }
        /* Liveness gate: skip task_delete if the rcv handler tore
         * down the connection. hx_htlc_close
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
    case HX_RECV_AGREEMENT:
        handler = hx_rcv_agreement_file;
        break;
    case HX_RECV_BANNER:
        handler = hx_rcv_banner;
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
