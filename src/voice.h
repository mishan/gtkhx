/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * Voice-chat extension wire-out path (Phase 8.A).
 *
 * Pure send-side wrappers around the Rust `hotline-proto::voice`
 * builders. Each function is `hx_send_voice_*` so the call sites in
 * future UI / debug paths read naturally; under the hood they call the
 * `gtkhx_proto_build_voice_*_chunks` FFI shims declared in
 * hotline_proto.h.
 *
 * The receive path lives in src/rcv.c. It has two halves:
 *   - **Server-initiated notifications** (602 VOICE_SDP_OFFER,
 *     604 VOICE_ICE, 605 VOICE_ROOM_STATUS) dispatched by name out of
 *     hx_rcv_hdr's switch, handled by hx_rcv_voice_sdp_offer /
 *     hx_rcv_voice_ice / hx_rcv_voice_room_status.
 *   - **TASK replies** to client-initiated 600/601/603/606 requests
 *     handled by rcv_task_voice_join (parses the JOIN reply's SDP +
 *     codec + participants payload) and rcv_task_voice_simple_ack
 *     (the empty-body 601/603/606 acks). The send wrappers below
 *     register these via task_new() before calling hlwrite_chunks
 *     so the trans-id lookup in hx_rcv_task finds them.
 *
 * The state machine, GStreamer runtime, audio capture/playback, and
 * UI all land in later sub-phases (8.B+) — this header is
 * deliberately minimal.
 *
 * Capability + access gating:
 *
 *   - Every send is no-op'd (returns FALSE) if the session didn't
 *     negotiate HTLC_CAP_VOICE — same convention as
 *     chat_history.c::hx_get_chat_history. Sending a 600-606 to a
 *     non-voice-capable server earns a task-error every time.
 *   - The optional access-bit check (HL_ACCESS_VOICE_CHAT) is enforced
 *     by the server, not here — Phase 8.D's UI will grey out the
 *     toolbar at the user-facing layer.
 */

#ifndef _VOICE_H
#define _VOICE_H

#include <glib.h>

struct htlc_conn;

/* Send HTLC_HDR_VOICE_JOIN (600) requesting voice in chat room `cid`.
 * cid 0 is the public chat. Returns TRUE if the request was put on the
 * wire, FALSE if the session didn't negotiate CAP_VOICE or the builder
 * rejected its inputs. */
extern gboolean hx_send_voice_join (struct htlc_conn *htlc, guint32 cid);

/* Send HTLC_HDR_VOICE_LEAVE (601) for chat room `cid`. */
extern gboolean hx_send_voice_leave (struct htlc_conn *htlc, guint32 cid);

/* Send HTLC_HDR_VOICE_SDP_ANSWER (603). `sdp` must be a non-empty UTF-8
 * SDP blob; an empty answer is rejected at the builder. `sdp_len` is in
 * bytes and must not exceed 65535 (Hotline data-chunk wire limit). */
extern gboolean hx_send_voice_sdp_answer (struct htlc_conn *htlc,
                                          guint32 cid, const guint8 *sdp,
                                          gsize sdp_len);

/* Send HTLC_HDR_VOICE_ICE (604). `ice` is the JSON-encoded
 * RTCIceCandidateInit (build via gtkhx_proto_build_voice_ice_json from
 * hotline_proto.h). Pass NULL / 0 for the end-of-candidates marker. */
extern gboolean hx_send_voice_ice (struct htlc_conn *htlc, guint32 cid,
                                   const guint8 *ice, gsize ice_len);

/* Send HTLC_HDR_VOICE_MUTE (606) toggling the local mute state.
 * `muted` is 0 (unmute) or 1 (mute); any other value is normalised to
 * 1 (any non-zero is treated as muted) to match the spec's bit-0
 * semantics for the future participant-blob reflection. */
extern gboolean hx_send_voice_mute (struct htlc_conn *htlc, guint32 cid,
                                    gboolean muted);

#endif /* _VOICE_H */
