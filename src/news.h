#ifndef HX_NEWS_H
#define HX_NEWS_H

extern void reload_news (GtkWidget *widget, gpointer data);
extern void create_post_window (GtkWidget *widget, gpointer data);
/* create_news_window is the gtkhx-ui `news` Rust shell (dock registration
 * via dock_bridge); these two are its C content-build + post-embed
 * lifecycle hooks, mirroring users_bridge.c / tasks.c. */
extern void create_news_window (GtkWidget *toolbar_window, session *sess);
extern GtkWidget *gtkhx_news_build_content (session *sess);
extern void gtkhx_news_after_embed (session *sess);
extern void open_news (GtkWidget *widget, gpointer data);

extern void hx_post_news (struct htlc_conn *htlc, const char *news,
                          guint16 len);
extern void hx_get_news (struct htlc_conn *htlc);

extern void output_news_post (struct htlc_conn *htlc, char *news, guint16 len);
extern void output_news_file (struct htlc_conn *htlc, char *news, guint16 len);

#endif
