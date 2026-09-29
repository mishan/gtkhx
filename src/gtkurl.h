#ifndef HX_GTKURL_H
#define HX_GTKURL_H

#include <gtk/gtk.h>

/*
 * gtkurl — shared URL detection / activation across GtkHx.
 *
 * The chat views detect and underline links themselves and hand a
 * right-click to gtkurl_show_popup (gtkhx-ui's chat_view.rs). The news
 * windows render through GtkTextView and use the detection + popup
 * wiring here. Detection is implemented in Rust, on the scheme list the
 * chat view uses, so the two cannot disagree about what a link is.
 */

/* TRUE if `word' looks like a URL we should treat as clickable.
 * Includes bare email tokens (foo@bar.com). */
extern gboolean gtkurl_is_url (const char *word);

/* Subset of gtkurl_is_url: TRUE iff `word' starts with one of the
 * scheme prefixes we recognise as a URL (rotulus-layout's default set
 * plus hotline://) OR one of the bare prefixes (www., ftp., irc.). The
 * email-shape check that gtkurl_is_url does is intentionally NOT
 * included. */
extern gboolean gtkurl_word_has_url_scheme (const char *word);

/* Returns a malloc'd "openable" form of `word' — prepends "https://"
 * to bare "www.foo" / "ftp.foo" tokens so GtkUriLauncher / xdg-open
 * actually launch a browser instead of bouncing off scheme parsing.
 * Free with g_free. */
extern char *gtkurl_normalize (const char *word);

/* Scan `text' (UTF-8) and call cb (text, start_byte, end_byte, user)
 * once per detected URL substring. Used by news.c / news15.c to
 * apply the "url" GtkTextTag over the matching ranges after a
 * gtk_text_buffer_insert / set_text. */
typedef void (*gtkurl_match_cb) (const char *text, int start_byte, int end_byte,
                                 gpointer user);
extern void gtkurl_scan (const char *text, gssize length, gtkurl_match_cb cb,
                         gpointer user);

/* Pop the right-click context menu for `url' anchored at `widget' /
 * (x, y). Builds: a header showing the URL truncated to fit, "Open
 * Link in Browser" (default GAppInfo for http), "Copy Selected
 * Link", plus one row per alternate browser GAppInfo registered for
 * http on this system. Free-floats: the popover destroys itself on
 * close. */
extern void gtkurl_show_popup (GtkWidget *anchor, const char *url, double x,
                               double y);

/* Wire up a GtkTextView to render URLs as clickable. Creates the
 * "url" tag on its buffer, installs a motion controller (cursor →
 * pointer + hover-underline) and a click gesture (right-click →
 * popup). Idempotent: safe to call multiple times on the same
 * widget. Use gtkurl_textview_apply_tags() after any text mutation
 * to (re-)apply the URL tag over detected matches. */
extern void gtkurl_textview_install (GtkTextView *tv);

/* Re-scan the text view's buffer for URLs and apply the "url" tag
 * over matched ranges. Call after gtk_text_buffer_insert /
 * set_text. Removes any prior url-tag markup first so we don't
 * end up with stale tags after a buffer reset. */
extern void gtkurl_textview_apply_tags (GtkTextView *tv);

#endif /* HX_GTKURL_H */
