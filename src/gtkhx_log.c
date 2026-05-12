/*
 * gtkhx_log.c — see gtkhx_log.h. printf-style logging helpers that
 * format into a heap-or-stack buffer and emit the "chat-log-line"
 * signal on GtkhxSession. View-side handler in chat.c does the
 * actual widget write.
 */

#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "gtkhx_log.h"
#include "gtkhx_session.h"

/* Common back-end: format `fmt` + va_list into a buffer (using
 * autobuf when it fits, growing via g_malloc/realloc when it doesn't)
 * and emit. `prefix` is concatenated in front of the formatted body
 * if non-NULL — the legacy hx_printf_prefix shape. */
static void
log_line_emit (struct htlc_conn *htlc, guint32 cid,
               const char *prefix, const char *fmt, va_list ap)
{
	va_list save;
	char autobuf[256], *buf;
	size_t mal_len;
	size_t plen;

	__va_copy (save, ap);
	mal_len = 256;
	buf = autobuf;
	plen = prefix ? strlen (prefix) : 0;
	for (;;) {
		va_list ap2;
		__va_copy (ap2, save);
		vsnprintf (buf + plen, mal_len - plen, fmt, ap2);
		va_end (ap2);
		if (strlen (buf + plen) != mal_len - plen - 1)
			break;
		mal_len <<= 1;
		if (buf == autobuf)
			buf = g_malloc (mal_len);
		else
			buf = g_realloc (buf, mal_len);
	}
	va_end (save);
	if (plen)
		memcpy (buf, prefix, plen);

	gtkhx_session_emit_chat_log_line (gtkhx_session_get_default (),
	                                  htlc, cid, buf);

	if (buf != autobuf)
		g_free (buf);
}

void
hx_printf_prefix (struct htlc_conn *htlc, guint32 cid,
                  const char *prefix, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	log_line_emit (htlc, cid, prefix, fmt, ap);
	va_end (ap);
}

void
hx_printf (struct htlc_conn *htlc, guint32 cid, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	log_line_emit (htlc, cid, NULL, fmt, ap);
	va_end (ap);
}
