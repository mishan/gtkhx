#ifndef HX_GTKURL_H
#define HX_GTKURL_H

#include <gtk/gtk.h>

/*
 * gtkurl — shared URL detection / activation across GtkHx.
 *
 * The chat / private-message / private-chat windows render through
 * the xtext widget, which already understands URL hover (changes
 * cursor to a hand) and emits a "word_click" signal on right- and
 * middle-click. The news / news15 windows render through GtkTextView
 * and need their own detection + popup wiring. This module collects
 * the bits both flavours share — URL recognition, default-browser
 * launch, alternate-browser discovery, and the Adwaita right-click
 * popover — so the call sites stay tiny.
 */

/* TRUE if `word' looks like a URL we should treat as clickable.
 * Includes bare email tokens (foo@bar.com) — xtext uses this to
 * decide whether to draw the word as a link and pop the right-click
 * menu. */
extern gboolean gtkurl_is_url (const char *word);

/* Subset of gtkurl_is_url: TRUE iff `word' starts with one of the
 * scheme prefixes we recognise as a URL (http://, https://, ftp://,
 * ftps://, irc://, mailto:, magnet:, git://, ssh://, sftp://,
 * hotline://) OR one of the bare prefixes (www., ftp., irc.). The
 * email-shape check that gtkurl_is_url does is intentionally NOT
 * included — callers like chat.c's word_check need to distinguish
 * "this is a URL" (WORD_URL) from "this is an email" (WORD_EMAIL).
 * This is the canonical scheme list; both the GtkTextView and xtext
 * paths key off it, so adding a new scheme only requires editing
 * url_schemes[] in gtkurl.c. */
extern gboolean gtkurl_word_has_url_scheme (const char *word);

/* Returns a malloc'd "openable" form of `word' — prepends "https://"
 * to bare "www.foo" / "ftp.foo" tokens so gtk_show_uri / xdg-open
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

/* WORD_CLICK signal handler for xtext consumers. Connect with
 *
 *   g_signal_connect (xtext, "word_click",
 *                     G_CALLBACK (gtkurl_xtext_word_click), NULL);
 *
 * Filters out left-click / non-URL words, calls gtkurl_show_popup
 * for everything else. */
extern void gtkurl_xtext_word_click (GtkWidget *xtext, char *word,
                                     GdkEvent *event, gpointer data);

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
