#ifndef HX_RCV_H
#define HX_RCV_H

extern void hx_rcv_agreement_file (struct htlc_conn *htlc, const guint8 *frame,
                                   gsize frame_len);
extern void hx_rcv_dump (struct htlc_conn *htlc, const guint8 *frame,
                         gsize frame_len);
extern void hx_rcv_banner (struct htlc_conn *htlc, const guint8 *frame,
                           gsize frame_len);
extern void hx_rcv_magic (struct htlc_conn *htlc, const guint8 *frame,
                          gsize frame_len);
/* Dispatch a received frame: route the (already-parsed) opcode to a body
 * handler and call it. The hxnet bridge assembles the 22-byte header + body
 * into a transient buffer and passes it here as an explicit (frame, frame_len)
 * slice alongside the parsed header fields. Replaces the old hx_rcv_hdr
 * two-phase state machine. */
extern void hx_dispatch_frame (struct htlc_conn *htlc, const guint8 *frame,
                               gsize frame_len, guint32 type, guint32 trans,
                               guint32 flag, guint32 body_len);

/* Voice-chat extension (fogWraith Capabilities-Voice.md), Phase 8.A.
 * Server-initiated notifications dispatched from the hx_dispatch_frame switch.
 *   _sdp_offer   — 602 VOICE_SDP_OFFER, initial offer or renegotiation.
 *   _ice         — 604 VOICE_ICE, trickle-ICE candidate (server side).
 *   _room_status — 605 VOICE_ROOM_STATUS, updated participant list.
 * Phase 8.A logs the parsed payload via debug_log("voice", ...) and
 * proto_trace; the runtime state machine + GtkhxSession signals land
 * in Phase 8.C with hxvoice-runtime. */
extern void hx_rcv_voice_sdp_offer (struct htlc_conn *htlc, const guint8 *frame,
                                    gsize frame_len);
extern void hx_rcv_voice_ice (struct htlc_conn *htlc, const guint8 *frame,
                              gsize frame_len);
extern void hx_rcv_voice_room_status (struct htlc_conn *htlc,
                                      const guint8 *frame, gsize frame_len);
/* 611 VIDEO_STATUS: the room's complete publication list. */
extern void hx_rcv_video_status (struct htlc_conn *htlc, const guint8 *frame,
                                 gsize frame_len);

/* USER_GETLIST, whose reply the session reads (hxhandlers::send::user). */
extern void hx_user_list_get (struct htlc_conn *htlc);

/* ICON_CHANGE (1864) broadcast: UID only. Emits gif-icon-changed so a
 * view can re-fetch the avatar via hx_icon_get. */
extern void hx_rcv_icon_change (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len);

/* Send what follows the login, on the bridge's LOGIN_READY. Idempotent. */
extern void hx_post_login_fetches (struct htlc_conn *htlc);

#endif /* HX_RCV_H */
