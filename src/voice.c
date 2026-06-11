/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Voice-chat extension wire-out path (Phase 8.A).
 *
 * Thin C wrappers around the Rust hotline-proto::voice builders. The
 * 600/601/603/604/606 transactions are emitted from here; the receive
 * dispatch (602/604/605, plus the JOIN reply and TASK replies for the
 * client-initiated opcodes) lives in rcv.c.
 *
 * Phase 8.A doesn't run the WebRTC stack: there's no state machine
 * yet, no audio capture, no UI affordance. The sends here are the
 * scaffolding the proto-trace exit criterion exercises (fire a Join
 * from a debug toggle, watch the 602 offer come back parsed in the
 * trace). The hxvoice / hxvoice-runtime crates land in Phase 8.B
 * onward.
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "compat.h"        /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h" /* struct hx_chunk */
#include "hotline_proto.h" /* gtkhx_proto_build_voice_* */
#include "hx.h"
#include "network.h"       /* hlwrite_chunks */
#include "rcv.h"           /* rcv_task_voice_join / _simple_ack */
#include "tasks.h"         /* task_new + RCV_TASK_FN */
#include "voice.h"
#include "debug.h"

/* Defensive: every send is gated on the server having echoed
 * HTLC_CAP_VOICE. Sending a 600 to a server that didn't negotiate the
 * cap earns a task-error per the spec; logging the skip is more
 * useful than spamming the user with toasts. Same convention as
 * chat_history.c::hx_get_chat_history. */
static gboolean
voice_cap_ok (struct htlc_conn *htlc)
{
    if (!htlc) {
        return FALSE;
    }
    if (!(htlc->caps & HTLC_CAP_VOICE)) {
        debug_log ("voice",
                   "skip voice send: server didn't echo CAP_VOICE "
                   "(caps=0x%" G_GINT64_MODIFIER "x)",
                   (guint64) htlc->caps);
        return FALSE;
    }
    return TRUE;
}

gboolean
hx_send_voice_join (struct htlc_conn *htlc, guint32 cid)
{
    if (!voice_cap_ok (htlc)) {
        return FALSE;
    }
    struct hx_chunk chunks[1];
    guint8 scratch[4];
    int32_t hc = gtkhx_proto_build_voice_join_chunks (
        cid, chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc <= 0) {
        debug_log ("voice", "VOICE_JOIN builder failed");
        return FALSE;
    }
    debug_log ("voice", "→ VOICE_JOIN cid=%u", cid);
    /* Register the JOIN reply task BEFORE the wire send so
     * hx_rcv_task finds the entry when the TASK reply (carrying
     * the server's initial SDP offer + codec + participants) lands.
     * Without this the reply gets silently dropped. */
    task_new (htlc, RCV_TASK_FN (rcv_task_voice_join),
              GUINT_TO_POINTER (cid), 0, "voice-join");
    hlwrite_chunks (htlc, HTLC_HDR_VOICE_JOIN, 0, chunks, hc);
    return TRUE;
}

gboolean
hx_send_voice_leave (struct htlc_conn *htlc, guint32 cid)
{
    if (!voice_cap_ok (htlc)) {
        return FALSE;
    }
    struct hx_chunk chunks[1];
    guint8 scratch[4];
    int32_t hc = gtkhx_proto_build_voice_leave_chunks (
        cid, chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc <= 0) {
        debug_log ("voice", "VOICE_LEAVE builder failed");
        return FALSE;
    }
    debug_log ("voice", "→ VOICE_LEAVE cid=%u", cid);
    /* 601 reply is an empty-body success; the simple_ack handler
     * logs that the trans completed. Spec doesn't require us to
     * wait for it before tearing the pipeline down. */
    task_new (htlc, RCV_TASK_FN (rcv_task_voice_simple_ack),
              GUINT_TO_POINTER (HTLC_HDR_VOICE_LEAVE),
              GUINT_TO_POINTER (cid), "voice-leave");
    hlwrite_chunks (htlc, HTLC_HDR_VOICE_LEAVE, 0, chunks, hc);
    return TRUE;
}

gboolean
hx_send_voice_sdp_answer (struct htlc_conn *htlc, guint32 cid,
                          const guint8 *sdp, gsize sdp_len)
{
    if (!voice_cap_ok (htlc)) {
        return FALSE;
    }
    if (!sdp || sdp_len == 0) {
        debug_log ("voice", "VOICE_SDP_ANSWER: empty SDP rejected");
        return FALSE;
    }
    struct hx_chunk chunks[2];
    guint8 scratch[4];
    int32_t hc = gtkhx_proto_build_voice_answer_chunks (
        cid, sdp, sdp_len, chunks, G_N_ELEMENTS (chunks), scratch,
        sizeof (scratch));
    if (hc <= 0) {
        debug_log ("voice", "VOICE_SDP_ANSWER builder failed (sdp_len=%zu)",
                   sdp_len);
        return FALSE;
    }
    debug_log ("voice", "→ VOICE_SDP_ANSWER cid=%u sdp_len=%zu", cid,
               sdp_len);
    /* 603 reply: empty-body success; an error here is fatal to the
     * session so the simple_ack-on-success / task_error-on-failure
     * shape is the right one. */
    task_new (htlc, RCV_TASK_FN (rcv_task_voice_simple_ack),
              GUINT_TO_POINTER (HTLC_HDR_VOICE_SDP_ANSWER),
              GUINT_TO_POINTER (cid), "voice-sdp-answer");
    hlwrite_chunks (htlc, HTLC_HDR_VOICE_SDP_ANSWER, 0, chunks, hc);
    return TRUE;
}

gboolean
hx_send_voice_ice (struct htlc_conn *htlc, guint32 cid, const guint8 *ice,
                   gsize ice_len)
{
    if (!voice_cap_ok (htlc)) {
        return FALSE;
    }
    struct hx_chunk chunks[2];
    guint8 scratch[4];
    /* Empty ICE is the legitimate end-of-candidates marker per spec.
     * NULL with zero len reaches the builder as an empty slice. */
    int32_t hc = gtkhx_proto_build_voice_ice_chunks (
        cid, ice, ice_len, chunks, G_N_ELEMENTS (chunks), scratch,
        sizeof (scratch));
    if (hc <= 0) {
        debug_log ("voice", "VOICE_ICE builder failed (ice_len=%zu)",
                   ice_len);
        return FALSE;
    }
    debug_log ("voice", "→ VOICE_ICE cid=%u ice_len=%zu%s", cid, ice_len,
               ice_len == 0 ? " (end-of-candidates)" : "");
    /* 604 VOICE_ICE is bidirectional per spec; both client and
     * server send it as a notification — no reply expected. We
     * deliberately don't register a task here. */
    hlwrite_chunks (htlc, HTLC_HDR_VOICE_ICE, 0, chunks, hc);
    return TRUE;
}

gboolean
hx_send_voice_mute (struct htlc_conn *htlc, guint32 cid, gboolean muted)
{
    if (!voice_cap_ok (htlc)) {
        return FALSE;
    }
    /* Spec: 0 = unmuted, 1 = muted. Cast through ?: to normalise the
     * GLib gboolean (which is just gint) to exactly 0/1; servers that
     * read the field as a strict u16-in-{0,1} would otherwise reject
     * a TRUE-as-some-other-nonzero value. */
    guint16 wire_muted = muted ? 1 : 0;
    struct hx_chunk chunks[2];
    guint8 scratch[6];
    int32_t hc = gtkhx_proto_build_voice_mute_chunks (
        cid, wire_muted, chunks, G_N_ELEMENTS (chunks), scratch,
        sizeof (scratch));
    if (hc <= 0) {
        debug_log ("voice", "VOICE_MUTE builder failed");
        return FALSE;
    }
    debug_log ("voice", "→ VOICE_MUTE cid=%u muted=%u", cid, wire_muted);
    /* 606 reply: empty-body success. simple_ack on success;
     * task_error path logs the failure via the standard
     * task_inerror toast. */
    task_new (htlc, RCV_TASK_FN (rcv_task_voice_simple_ack),
              GUINT_TO_POINTER (HTLC_HDR_VOICE_MUTE),
              GUINT_TO_POINTER (cid), "voice-mute");
    hlwrite_chunks (htlc, HTLC_HDR_VOICE_MUTE, 0, chunks, hc);
    return TRUE;
}
