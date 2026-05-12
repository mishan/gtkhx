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

extern void hx_printf_prefix (struct htlc_conn *htlc, guint32 cid,
                              const char *prefix, const char *fmt, ...)
	G_GNUC_PRINTF (4, 5);
extern void hx_printf (struct htlc_conn *htlc, guint32 cid,
                       const char *fmt, ...)
	G_GNUC_PRINTF (3, 4);

#endif /* HX_GTKHX_LOG_H */
