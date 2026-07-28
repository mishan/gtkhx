/*
 * gtkhx_log.h — printf-style helpers for logging a line into a chat
 * window.
 *
 * Phase 3 follow-up: hx_printf / hx_printf_prefix used to live in
 * chat.c (the view side) and call the file-local xoutput_chat
 * directly. Model-side files (rcv.c, network.c, commands.c, etc.)
 * called these by name, the last remaining model→view edge that
 * didn't go through GtkhxSession. The functions now live here,
 * vsnprintf into a buffer, and emit the "chat-log-line" signal
 * with (htlc, cid, body); the view-side handler in chat.c does
 * the actual widget write. Same call-site contract — model files
 * keep calling hx_printf_prefix(htlc, cid, prefix, "fmt", ...).
 *
 * Decls were previously in chat.h; they stay there for source-
 * compatibility, but the implementations are GTK-free now.
 */

#ifndef HX_GTKHX_LOG_H
#define HX_GTKHX_LOG_H 1

#include <glib.h>
#include "protocol.h"

/* Palette index the "[hx]" tag renders in. Mirrors chat_view.h's
 * HX_CHAT_INFO_COLOR; duplicated rather than included so the non-widget
 * callers that pull in this header don't drag GTK along. */
#define HX_CHAT_LOG_INFO_COLOR 3

extern void hx_printf_prefix (struct htlc_conn *htlc, guint32 cid,
                              const char *prefix, const char *fmt, ...)
    G_GNUC_PRINTF (4, 5);

/* As hx_printf_prefix, but with an explicit gutter tag and colour —
 * broadcastmsg's per-sender "[name]" lines, which used to encode the
 * colour as escape bytes inside a pre-formatted prefix string. */
extern void hx_printf_named (struct htlc_conn *htlc, guint32 cid,
                             const char *name, gint32 color, const char *fmt,
                             ...) G_GNUC_PRINTF (5, 6);
extern void hx_printf (struct htlc_conn *htlc, guint32 cid, const char *fmt,
                       ...) G_GNUC_PRINTF (3, 4);

/* INFOPREFIX is the gutter tag hx_printf_prefix marks user-visible
 * status lines with — the "[hx]" seen in chat windows. It is now just
 * the string "hx"; the brackets and colours are applied by the view.
 * It used to be " \00310[\00303hx\00310]\003 ", pre-formatted with
 * mIRC escapes the view had to parse back out.
 *
 * Defined in gtkhx.c; declared here rather than in session.h so
 * non-widget callers (the test stubs that satisfy the linker) don't
 * have to pull in the GTK surface session.h drags along. */
extern const char *INFOPREFIX;

#endif /* HX_GTKHX_LOG_H */
