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
 * This is the replacement for the old gnews_folder / gnews_catalog
 * dual-window UI (open_news15 in news15.c). Both UIs coexist in the
 * binary during the migration; the toolbar now points at the unified
 * browser. Once the new UI is fully functional the old code paths
 * will be removed.
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

/* Intercept hook called from gtkhx.c::on_news_folder_signal /
 * on_news_catalog_signal. The browser registers in-flight fetches
 * (via stub gnews_folder / gnews_catalog pointers fed to the
 * existing hx_news15_fldr_list / hx_news15_cat_list helpers) and
 * matches them here. Returns TRUE if the browser consumed the
 * reply (the gnews_folder/gnews_catalog pointer was one of ours);
 * FALSE if it's an old-style window's fetch that should fall
 * through to output_news_folder / output_news_catalog.
 *
 * Both functions free the stub when they return TRUE — the stub
 * isn't a window, it's a one-shot RPC carrier. */
extern gboolean gnews_browser_handle_dirlist (gpointer gfnews);
extern gboolean gnews_browser_handle_catlist (gpointer gcnews);

#endif /* HX_NEWS_BROWSER_H */
