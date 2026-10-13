/*
 * news_browser.h — unified 1.5 threaded-news browser.
 *
 * One window per connection that walks the whole news hierarchy in a
 * tree view:
 *
 *   root folder
 *     ├── sub-folder
 *     │     └── ... (recursive)
 *     └── category
 *           ├── post
 *           │     └── reply (threaded via parentid)
 *           └── post ...
 *
 * Folders are enumerated via HTLC_HDR_NEWSDIRLIST (the same opcode
 * the old gnews_folder window used) and contain folders + categories.
 * Categories are enumerated via HTLC_HDR_NEWSCATLIST and contain
 * posts; posts inside a category are threaded by parentid.
 *
 * This replaced the old gnews_folder / gnews_catalog dual-window UI
 * (open_news15 in news15.c, retired in Phase 6). news15.c now
 * contains only the wire-format RPC senders that this browser
 * drives.
 */

#ifndef HX_NEWS_BROWSER_H
#define HX_NEWS_BROWSER_H 1

#include <gtk/gtk.h>

struct _session;
struct _gnews_browser;
typedef struct _gnews_browser gnews_browser;

/* Open (or raise) the news browser window for the given session's
 * connection — the gtkhx-ui `news_browser` Rust module. Hooked as the
 * toolbar callback for the News (1.5+) button. */
extern void open_news_browser (GtkWidget *widget, struct _session *sess);

/* Reply-routing hooks called from gtkhx.c::on_news_*_signal. The
 * browser registers in-flight fetches (via stub gnews_folder /
 * gnews_catalog / news_item pointers fed to the existing
 * hx_news15_fldr_list / _cat_list / _get_post helpers) and matches
 * them here; a post's body goes to the browser of the connection
 * it came in on. The browser frees the carrier; the caller does
 * not. */
struct htlc_conn;
extern gboolean gnews_browser_handle_dirlist (gpointer gfnews);
extern gboolean gnews_browser_handle_catlist (gpointer gcnews);
extern gboolean gnews_browser_handle_thread (struct htlc_conn *htlc,
                                             gpointer post);

#endif /* HX_NEWS_BROWSER_H */
