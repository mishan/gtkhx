#ifndef HX_RCV_H
#define HX_RCV_H

extern void hx_rcv_chat (struct htlc_conn *htlc, const guint8 *frame,
                         gsize frame_len);
extern void hx_rcv_msg (struct htlc_conn *htlc, const guint8 *frame,
                        gsize frame_len);
extern void hx_rcv_agreement_file (struct htlc_conn *htlc, const guint8 *frame,
                                   gsize frame_len);
extern void hx_rcv_news_post (struct htlc_conn *htlc, const guint8 *frame,
                              gsize frame_len);
extern void hx_rcv_task (struct htlc_conn *htlc, const guint8 *frame,
                         gsize frame_len);
extern void hx_rcv_user_change (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len);
extern void hx_rcv_user_part (struct htlc_conn *htlc, const guint8 *frame,
                              gsize frame_len);
extern void hx_rcv_chat_subject (struct htlc_conn *htlc, const guint8 *frame,
                                 gsize frame_len);
extern void hx_rcv_chat_invite (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len);
extern void hx_rcv_user_selfinfo (struct htlc_conn *htlc, const guint8 *frame,
                                  gsize frame_len);
extern void hx_rcv_dump (struct htlc_conn *htlc, const guint8 *frame,
                         gsize frame_len);
extern void hx_rcv_xfer_queue (struct htlc_conn *htlc, const guint8 *frame,
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
extern void rcv_task_voice_join (struct htlc_conn *htlc, const guint8 *frame,
                                 gsize frame_len, void *channel_ptr);
extern void rcv_task_voice_simple_ack (struct htlc_conn *htlc,
                                       const guint8 *frame, gsize frame_len,
                                       void *opcode_ptr, void *cid_ptr);

extern void rcv_task_user_open (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len, struct uesp_fn *uespfn);
extern void rcv_task_msg (struct htlc_conn *htlc, const guint8 *frame,
                          gsize frame_len, char *msg_buf);
/* rcv_task_newscat_list moved to the hxnews-recv Rust crate as a 3-arg
 * rcv_task_fn (htlc, ptr, data). No C caller references them by name — the
 * hxnews-send cat_list / fldr_list / get_post senders register them via
 * task_new — so the old 2-arg prototypes are gone rather than left to drift
 * from the real ABI. */
extern void rcv_task_login (struct htlc_conn *htlc, const guint8 *frame,
                            gsize frame_len, char *pass);
/* rcv_task_news_users (post-login USER_GETLIST reply) moved to the hxhandlers
 * Rust crate (recv/user.rs); the prototype stays for the RCV_TASK_FN(task_new)
 * registration in the post-login fetch path. */
extern void rcv_task_news_users (struct htlc_conn *htlc, const guint8 *frame,
                                 gsize frame_len, struct chat *chat, int text);
extern void rcv_task_news_file (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len);

/* TRAN_GET_CHAT_HISTORY (700) reply walker. Moved to the hxhandlers Rust crate
 * (recv/chat.rs): it walks the reply chunks natively, builds the
 * GPtrArray<HxHistoryEntry*> via glib + the native hx_history_entry_parse,
 * advances the newest-msgid cursor, and emits GtkhxSession::chat-history-batch.
 * The channel id rides the task ptr (GUINT_TO_POINTER) since the reply doesn't
 * repeat it. The prototype stays for the RCV_TASK_FN(task_new) registrations at
 * the send call sites (chat.c's Load-older flow and rcv.c's
 * hx_post_login_fetches, which register the reply task before calling
 * hx_get_chat_history); the symbol resolves against the Rust crate at link. The
 * Rust body takes the canonical rcv_task_fn shape, so the prototype matches it
 * (ptr = channel id, data unused) rather than the historical short form. */
extern void rcv_task_chat_history (struct htlc_conn *htlc, const guint8 *frame,
                                   gsize frame_len, void *channel_ptr,
                                   void *data);
/* GIF-icons extension (fogWraith GIF-Icons.md) reply handlers. The bodies moved
 * to the hxhandlers Rust crate (recv/icon.rs): each walks the reply natively
 * (crate::gif_icons), flips the probe negotiation state via the
 * hx_conn_gif_icons_* accessors, and publishes avatars through hx_icon_data_recv.
 * The prototypes stay because the send wrappers in gif_icons.c register them via
 * RCV_TASK_FN(task_new); the symbols resolve against the Rust crate at link.
 *   _get      — ICON_GET (1863) reply: UID + ICON_GIF. uid_ptr is
 *               GUINT_TO_POINTER(uid) from the send wrapper (uid is echoed in
 *               the reply, so the Rust body ignores it).
 *   _getlist  — ICON_GETLIST (1861) reply: 0..N packed ICON_LIST
 *               entries; also resolves the probe to SUPPORTED. */
extern void rcv_task_icon_get (struct htlc_conn *htlc, const guint8 *frame,
                               gsize frame_len, void *uid_ptr);
extern void rcv_task_icon_getlist (struct htlc_conn *htlc, const guint8 *frame,
                                   gsize frame_len);
/* ICON_CHANGE (1864) broadcast: UID only. Emits gif-icon-changed so a
 * view can re-fetch the avatar via hx_icon_get. */
extern void hx_rcv_icon_change (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len);

/* rcv_task_user_list / _user_list_switch / _user_info moved to the hxhandlers
 * Rust crate (recv/user.rs): they walk the reply chunks natively
 * (parse_user_list_record / parse_user_info) and fold into the roster through the
 * shared, already-Rust hx_user_apply_recv. The prototypes stay for the
 * RCV_TASK_FN(task_new) registrations in users.c / chat.c. rcv_task_kick stays C
 * (it logs via the variadic hx_printf_prefix). */
extern void rcv_task_user_list (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len, struct chat *chat, int text);
extern void rcv_task_user_list_switch (struct htlc_conn *htlc,
                                       const guint8 *frame, gsize frame_len,
                                       struct chat *chat);
extern void rcv_task_kick (struct htlc_conn *htlc, const guint8 *frame,
                           gsize frame_len);
extern void rcv_task_user_info (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len, guint16 *_uid, int text);
/* rcv_task_file_list moved to the hxfiles-recv Rust crate, which also owns struct
 * cached_filelist (the hx_cfl_* accessor facade in files.h). It walks the
 * FILE_LIST chunks natively and accumulates them into the Rust-owned cfl. */
extern void rcv_task_file_list (struct htlc_conn *htlc, const guint8 *frame,
                                gsize frame_len, struct cached_filelist *cfl,
                                void *data);
/* rcv_task_file_getinfo / _file_get / _file_put / _folder_get / _folder_put (and
 * rcv_task_banner_get in banner.h) moved to the hxxfer-recv Rust crate: each
 * parses its reply natively (hotline_proto::parse::*) and reaches the C-owned
 * htxf state through the hx_htxf_* accessor seam (htxf_accessors.c). The
 * prototypes stay because the C senders (xfers.c, files.c, banner.c) register the
 * handlers via RCV_TASK_FN(); the symbols now resolve against the Rust crate at
 * link. */
extern void rcv_task_file_getinfo (struct htlc_conn *htlc, const guint8 *frame,
                                   gsize frame_len, char *path);
extern void rcv_task_file_get (struct htlc_conn *htlc, const guint8 *frame,
                               gsize frame_len, struct htxf_conn *htxf);
extern void rcv_task_file_put (struct htlc_conn *htlc, const guint8 *frame,
                               gsize frame_len, struct htxf_conn *htxf);
/* Folder transfer task replies. Mirror rcv_task_file_get /
 * rcv_task_file_put but also parse HTLS_DATA_FILE_NFILES so the
 * tasks-window can show the leaf count. The actual stream is
 * handled by folder_get_thread / folder_put_thread in xfers.c. */
extern void rcv_task_folder_get (struct htlc_conn *htlc, const guint8 *frame,
                                 gsize frame_len, struct htxf_conn *htxf);
extern void rcv_task_folder_put (struct htlc_conn *htlc, const guint8 *frame,
                                 gsize frame_len, struct htxf_conn *htxf);

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
