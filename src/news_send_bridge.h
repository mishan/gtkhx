#ifndef HX_NEWS_SEND_BRIDGE_H
#define HX_NEWS_SEND_BRIDGE_H

#include <glib.h>

/*
 * Field accessors for the hxnews-send Rust crate (see news_send_bridge.c).
 * The get_post / cat_list / fldr_list senders take these C structs; the Rust
 * side reads the fields it needs (and sets the `listing` flag) through here
 * instead of mirroring the GTK-laden struct layouts.
 */

G_BEGIN_DECLS

struct gnews_catalog;
struct gnews_folder;

extern const char *gnews_catalog_path (struct gnews_catalog *g);
extern void gnews_catalog_mark_listing (struct gnews_catalog *g);

extern const char *gnews_folder_path (struct gnews_folder *g);
extern void gnews_folder_mark_listing (struct gnews_folder *g);

G_END_DECLS

#endif /* HX_NEWS_SEND_BRIDGE_H */
