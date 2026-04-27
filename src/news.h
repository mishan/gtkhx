#ifndef HX_NEWS_H
#define HX_NEWS_H

extern GtkWidget *reloadButton;
extern GtkWidget *postButton;

extern void reload_news (GtkWidget *widget, gpointer data);
extern void create_post_window (void);
extern void create_news_window (session *sess);
extern void open_news (GtkWidget *widget, gpointer data);

extern void hx_post_news (struct htlc_conn *htlc, const char *news, guint16 len);
extern void hx_get_news (struct htlc_conn *htlc);

#endif
