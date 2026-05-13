#ifndef HX_NEWS15_H
#define HX_NEWS15_H

/*
 * Hotline 1.5 threaded-news RPC senders.
 *
 * The two-window legacy UI (gnews_folder / gnews_catalog) that used
 * to live in news15.c is gone — news_browser.c is the only consumer
 * now. What remains in news15.{c,h} is purely wire-format: each
 * helper takes the request shape from the caller, registers a task,
 * and emits the HTLC frame.
 */

extern void hx_news15_get_post   (struct htlc_conn *htlc,
                                  struct news_item *item);
extern void hx_news15_cat_list   (struct htlc_conn *htlc,
                                  struct gnews_catalog *gcnews);
extern void hx_news15_fldr_list  (struct htlc_conn *htlc,
                                  struct gnews_folder *gfnews);

/* Post a news article. `threadid` is the post being replied to
 * (HTLC_DATA_THREADID on the wire — mhxd writes this value into
 * the new post's "References:" header). Pass 0 for a brand-new
 * top-level post. */
extern void hx_news15_post_thread (struct htlc_conn *htlc, char *path,
                                   const char *subject, guint32 threadid,
                                   char *text);

extern void hx_news15_delete        (struct htlc_conn *htlc, char *path);
extern void hx_news15_mkcat         (struct htlc_conn *htlc, char *path,
                                     const char *name);
extern void hx_news15_mkdir         (struct htlc_conn *htlc, char *path);
extern void hx_news15_delete_thread (struct htlc_conn *htlc, char *path,
                                     guint32 threadid);
#endif
