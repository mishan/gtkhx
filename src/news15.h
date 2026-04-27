#ifndef HX_NEWS15_H
#define HX_NEWS15_H

extern void output_news_file (struct htlc_conn *htlc, char *news, guint16 len);
extern void output_news_post (struct htlc_conn *htlc, char *news, guint16 len);
extern void output_news_folder(struct gnews_folder *gfnews);
extern void output_news_catalog(struct gnews_catalog *gcnews);
extern void output_news_thread(struct news_post *post);

extern void hx_news15_get_post(struct htlc_conn *htlc, struct news_item *item);
extern void hx_news15_cat_list(struct htlc_conn *htlc, char *path);
extern void hx_news15_fldr_list(struct htlc_conn *htlc, char *path);


extern void open_news15(GtkWidget *widget, session *sess);
struct gnews_folder *gfnews_list;
#endif
