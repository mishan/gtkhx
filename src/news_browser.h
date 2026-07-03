/*
 * news_browser.h — unified 1.5 threaded-news browser.
 *
 * One window per session that walks the whole news hierarchy in a
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

/* Open the news browser window for the given session. Hooked as the
 * toolbar callback for the News (1.5+) button. */
extern void open_news_browser (GtkWidget *widget, struct _session *sess);

/* create_news_browser_window is the gtkhx-ui `news_browser` Rust shell (dock
 * registration via dock_bridge, CENTER area); these are its C content-build +
 * post-embed hooks. build_content builds the whole gnews_browser + content
 * tree (stashing it in the_browser) and returns the content box; after_embed
 * wires the one panel-level hook (PanelWidget::presented) once embedded. */
extern void create_news_browser_window (GtkWidget *widget,
                                        struct _session *sess);
extern GtkWidget *gtkhx_news_browser_build_content (void);
extern void gtkhx_news_browser_after_embed (void);

/* Reply-routing hooks called from gtkhx.c::on_news_*_signal. The
 * browser registers in-flight fetches (via stub gnews_folder /
 * gnews_catalog / news_item pointers fed to the existing
 * hx_news15_fldr_list / _cat_list / _get_post helpers) and matches
 * them here. Returns TRUE on a match — the browser owned the stub
 * and the stub is now freed. FALSE means no match (caller can
 * safely drop the carrier, but in practice the browser is the
 * only producer of these stubs since Phase 6, so a FALSE return
 * just means the reply landed after the browser closed).
 *
 * Stub freeing is handled inside the browser on match; the caller
 * does not free. */
extern gboolean gnews_browser_handle_dirlist (gpointer gfnews);
extern gboolean gnews_browser_handle_catlist (gpointer gcnews);
extern gboolean gnews_browser_handle_thread (gpointer post);

#endif /* HX_NEWS_BROWSER_H */
