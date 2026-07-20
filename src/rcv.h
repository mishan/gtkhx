#ifndef HX_RCV_H
#define HX_RCV_H

extern void hx_rcv_chat (struct htlc_conn *htlc);
extern void hx_rcv_msg (struct htlc_conn *htlc);
extern void hx_rcv_agreement_file (struct htlc_conn *htlc);
extern void hx_rcv_news_post (struct htlc_conn *htlc);
extern void hx_rcv_task (struct htlc_conn *htlc);
extern void hx_rcv_user_change (struct htlc_conn *htlc);
extern void hx_rcv_user_part (struct htlc_conn *htlc);
extern void hx_rcv_chat_subject (struct htlc_conn *htlc);
extern void hx_rcv_chat_invite (struct htlc_conn *htlc);
extern void hx_rcv_user_selfinfo (struct htlc_conn *htlc);
extern void hx_rcv_dump (struct htlc_conn *htlc);
extern void hx_rcv_xfer_queue (struct htlc_conn *htlc);
extern void hx_rcv_banner (struct htlc_conn *htlc);
extern void hx_rcv_magic (struct htlc_conn *htlc);
extern void hx_rcv_hdr (struct htlc_conn *htlc);

/* Voice-chat extension (fogWraith Capabilities-Voice.md), Phase 8.A.
 * Server-initiated notifications dispatched from the hx_rcv_hdr switch.
 *   _sdp_offer   — 602 VOICE_SDP_OFFER, initial offer or renegotiation.
 *   _ice         — 604 VOICE_ICE, trickle-ICE candidate (server side).
 *   _room_status — 605 VOICE_ROOM_STATUS, updated participant list.
 * Phase 8.A logs the parsed payload via debug_log("voice", ...) and
 * proto_trace; the runtime state machine + GtkhxSession signals land
 * in Phase 8.C with hxvoice-runtime. */
extern void hx_rcv_voice_sdp_offer (struct htlc_conn *htlc);
extern void hx_rcv_voice_ice (struct htlc_conn *htlc);
extern void hx_rcv_voice_room_status (struct htlc_conn *htlc);

/* TASK-reply handlers for the client-initiated 600/601/603/606
 * transactions. The voice send wrappers in src/voice.c register
 * one of these via task_new() before each hlwrite_chunks call;
 * hx_rcv_task dispatches here when the matching trans id comes
 * back.
 *   _join         — 600 VOICE_JOIN reply parser. JOIN reply
 *                   carries the server's initial SDP offer +
 *                   active codec + current participants per spec.
 *                   `channel_ptr` is GUINT_TO_POINTER(cid) the
 *                   send wrapper handed task_new for the data.
 *   _simple_ack   — Empty-success reply for 601/603/606. Logs
 *                   that the trans completed. `opcode_ptr` is
 *                   the originating opcode (label only),
 *                   `cid_ptr` is the originating cid (diagnostic
 *                   only). */
extern void rcv_task_voice_join (struct htlc_conn *htlc, void *channel_ptr);
extern void rcv_task_voice_simple_ack (struct htlc_conn *htlc,
                                       void *opcode_ptr, void *cid_ptr);

extern void rcv_task_user_open (struct htlc_conn *htlc, struct uesp_fn *uespfn);
extern void rcv_task_msg (struct htlc_conn *htlc, char *msg_buf);
/* rcv_task_newscat_list moved to the hxnews-recv Rust crate as a 3-arg
 * rcv_task_fn (htlc, ptr, data). No C caller references it by name — the
 * hxnews-send cat_list sender registers it via task_new — so the old 2-arg
 * prototype is gone rather than left to drift from the real ABI. */
extern void rcv_task_newsfolder_list (struct htlc_conn *htlc,
                                      struct gnews_folder *gfnews);
extern void rcv_task_news_post (struct htlc_conn *htlc, struct news_item *item);
extern void rcv_task_login (struct htlc_conn *htlc, char *pass);
extern void rcv_task_news_users (struct htlc_conn *htlc, struct chat *chat,
                                 int text);
extern void rcv_task_news_file (struct htlc_conn *htlc);

/* TRAN_GET_CHAT_HISTORY (700) reply walker. Parses 0..N
 * HTLS_DATA_HISTORY_ENTRY packed-binary chunks and the
 * HTLS_DATA_HISTORY_HAS_MORE u8 flag out of htlc->in, then emits
 * GtkhxSession::chat-history-batch. The channel id is carried via
 * the task ptr (GUINT_TO_POINTER) since the reply itself doesn't
 * repeat it. */
extern void rcv_task_chat_history (struct htlc_conn *htlc,
                                   void             *channel_ptr);
/* GIF-icons extension (fogWraith GIF-Icons.md) reply handlers. The
 * send wrappers in gif_icons.c register these via task_new before the
 * matching hlwrite_chunks. Both delegate parsing to the Rust
 * hotline-proto crate and emit GtkhxSession::gif-icon-data.
 *   _get      — ICON_GET (1863) reply: UID + ICON_GIF. uid_ptr is
 *               GUINT_TO_POINTER(uid) from the send wrapper.
 *   _getlist  — ICON_GETLIST (1861) reply: 0..N packed ICON_LIST
 *               entries; also resolves the probe to SUPPORTED. */
extern void rcv_task_icon_get (struct htlc_conn *htlc, void *uid_ptr);
extern void rcv_task_icon_getlist (struct htlc_conn *htlc);
/* ICON_CHANGE (1864) broadcast: UID only. Emits gif-icon-changed so a
 * view can re-fetch the avatar via hx_icon_get. */
extern void hx_rcv_icon_change (struct htlc_conn *htlc);

extern void rcv_task_user_list (struct htlc_conn *htlc, struct chat *chat,
                                int text);
extern void rcv_task_user_list_switch (struct htlc_conn *htlc,
                                       struct chat *chat);
extern void rcv_task_kick (struct htlc_conn *htlc);
extern void rcv_task_user_info (struct htlc_conn *htlc, guint16 *_uid,
                                int text);
extern void rcv_task_file_list (struct htlc_conn *htlc,
                                struct cached_filelist *cfl, void *data);
extern void rcv_task_file_getinfo (struct htlc_conn *htlc, char *path);
extern void rcv_task_file_get (struct htlc_conn *htlc, struct htxf_conn *htxf);
extern void rcv_task_file_put (struct htlc_conn *htlc, struct htxf_conn *htxf);
/* Folder transfer task replies. Mirror rcv_task_file_get /
 * rcv_task_file_put but also parse HTLS_DATA_FILE_NFILES so the
 * tasks-window can show the leaf count. The actual stream is
 * handled by folder_get_thread / folder_put_thread in xfers.c. */
extern void rcv_task_folder_get (struct htlc_conn *htlc,
                                 struct htxf_conn *htxf);
extern void rcv_task_folder_put (struct htlc_conn *htlc,
                                 struct htxf_conn *htxf);

/* drop any pending post-login fallback timer. Called from
 * hx_htlc_close so we don't fire fetches into a closed connection if
 * the user disconnects within the 2-second AGREEMENTAGREE window. */
extern void rcv_login_reset (void);

/* Fire deferred USER_GETLIST + news fetch. Idempotent (single-fire
 * guard). Called from hx_send_agreement_agree (network.c) after
 * AGREEMENTAGREE goes out — that's the spec-correct join boundary
 * per the 1.5 flow. Also called from the 2-second fallback timer
 * for 1.2 servers that never trigger AGREEMENTAGREE. */
extern void hx_post_login_fetches (struct htlc_conn *htlc);

#endif /* HX_RCV_H */
