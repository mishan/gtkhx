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

extern void rcv_task_user_open (struct htlc_conn *htlc, struct uesp_fn *uespfn);
extern void rcv_task_msg (struct htlc_conn *htlc, char *msg_buf);
extern void rcv_task_newscat_list (struct htlc_conn *htlc,
                                   struct gnews_catalog *gcnews);
extern void rcv_task_newsfolder_list (struct htlc_conn *htlc,
                                      struct gnews_folder *gfnews);
extern void rcv_task_news_post (struct htlc_conn *htlc, struct news_item *item);
extern void rcv_task_login (struct htlc_conn *htlc, char *pass);
extern void rcv_task_news_users (struct htlc_conn *htlc, struct chat *chat,
                                 int text);
extern void rcv_task_news_file (struct htlc_conn *htlc);
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

/* Phase 5: drop any pending post-login fallback timer. Called from
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
