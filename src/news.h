#ifndef HX_NEWS_H
#define HX_NEWS_H

/* The flat-news content moved to the gtkhx-ui `news` Rust module (news.rs):
 * the create_news_window shell plus reload_news / open_news / output_news_*
 * keep their C ABI (rcv.c, toolbar.c, and the gtkhx.c signal adapters link
 * against them unchanged). The content build + post-embed hooks are now
 * Rust-internal (no C caller). create_post_window and hx_get_news are likewise
 * Rust-only callers now, so they're no longer declared here. */
extern void reload_news (GtkWidget *widget, gpointer data);
extern void create_news_window (GtkWidget *toolbar_window, session *sess);
extern void open_news (GtkWidget *widget, gpointer data);

extern void hx_post_news (struct htlc_conn *htlc, const char *news,
                          guint16 len);

extern void output_news_post (struct htlc_conn *htlc, char *news, guint16 len);
extern void output_news_file (struct htlc_conn *htlc, char *news, guint16 len);

#endif
