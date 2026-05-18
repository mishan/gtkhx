#ifndef HX_TEXT_UTIL_H
#define HX_TEXT_UTIL_H 1

/*
 * Standalone encoding-conversion helpers. Lifted out of gtkutil.h so
 * the function body in text_util.c can be tested in isolation —
 * gtkutil.h pulls in <gtk/gtk.h>, hx.h, and friends, which would
 * force every test program to link the entire app.
 *
 * gtkutil.h re-includes this header so existing callers continue to
 * see the same prototype path they always have.
 */

#include <glib.h>

/*
 * Convert a buffer of 8-bit text in unknown encoding to valid UTF-8.
 *
 * Hotline servers send strings (server names, news bodies, post
 * subjects, etc.) in whatever encoding they happened to use —
 * historically Mac Roman (the original Mac-OS-classic stack),
 * occasionally Latin-1 from later Unix servers, sometimes already
 * UTF-8 on modern stacks like mhxd. GTK widgets that take text
 * (GtkTextBuffer, gtk_window_set_title, …) assert UTF-8 on input, so
 * anything we get from the wire has to be sanitized before it lands
 * in a widget.
 *
 * Strategy:
 *   1. If the input is already valid UTF-8, return a copy verbatim.
 *   2. Try Mac Roman → UTF-8 (iconv knows it as MACINTOSH).
 *   3. Fall back to g_utf8_make_valid, which substitutes U+FFFD for
 *      anything that isn't self-consistent UTF-8.
 *
 * Edge cases:
 *   - bytes == NULL returns an empty (non-NULL) g_strdup-ed string;
 *     callers can g_free unconditionally.
 *   - len == 0 returns a copy of the empty input verbatim (still
 *     valid UTF-8 by definition).
 *
 * Caller g_frees the result. If out_len is non-NULL it receives the
 * UTF-8 byte length (excluding any trailing NUL).
 */
extern char *gtkhx_text_to_utf8 (const char *bytes, gsize len, gsize *out_len);

/*
 * Outbound counterpart of gtkhx_text_to_utf8: take a UTF-8 string
 * destined for the wire and return the bytes to actually send.
 *
 * utf8_mode = TRUE  → pass-through. The negotiated server speaks
 *                     UTF-8 (CAP_TEXT_ENCODING confirmed), so the
 *                     input is already in the wire encoding. Caller
 *                     gets a g_strndup of the input.
 * utf8_mode = FALSE → encode to MACINTOSH (Mac Roman) via
 *                     g_convert_with_fallback, substituting '?'
 *                     (0x3F) for any UTF-8 codepoint not in the
 *                     target encoding's repertoire. Matches the
 *                     fogWraith Capabilities-Text-Encoding spec:
 *                     "characters that cannot be represented in
 *                     the target encoding are replaced with `?`
 *                     (0x3F)."
 *
 * is_body  = TRUE   → after encoding, replace LF (0x0A) with CR
 *                     (0x0D) for line-ending normalisation to
 *                     classic Mac convention. Only meaningful in
 *                     legacy mode; UTF-8 mode passes LF through
 *                     unchanged regardless. Apply only to body
 *                     fields (chat / msg / news article data); name
 *                     and subject fields stay one-line.
 *
 * Caller g_frees the result. *out_len receives the wire byte length
 * (excluding any trailing NUL). NULL input returns g_strdup("") with
 * *out_len = 0 to keep call sites simple — they can pass directly
 * into hlwrite without a separate NULL check.
 */
extern char *gtkhx_text_for_wire (const char *utf8, gsize utf8_len,
                                  gboolean utf8_mode, gboolean is_body,
                                  gsize *out_len);

#endif /* HX_TEXT_UTIL_H */
