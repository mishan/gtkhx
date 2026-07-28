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

/* Common back-end: format `fmt` + va_list into a buffer (using autobuf
 * when it fits, growing via g_malloc/realloc when it doesn't) and emit.
 *
 * `name`/`color` travel as signal parameters rather than being
 * concatenated into the body. They used to be a `prefix` string the
 * caller pre-formatted with mIRC escapes —
 * " \00310[\00303hx\00310]\003 " — which the view then had to parse
 * back out to find where the name ended. Passing them alongside means
 * nothing has to reconstruct what the caller already knew. */
static void
log_line_emit (struct htlc_conn *htlc, guint32 cid, const char *name,
               gint32 color, const char *fmt, va_list ap)
{
    va_list save;
    char autobuf[256], *buf;
    size_t mal_len;

    __va_copy (save, ap);
    mal_len = 256;
    buf = autobuf;
    for (;;) {
        va_list ap2;
        __va_copy (ap2, save);
        vsnprintf (buf, mal_len, fmt, ap2);
        va_end (ap2);
        if (strlen (buf) != mal_len - 1) {
            break;
        }
        mal_len <<= 1;
        if (buf == autobuf) {
            buf = g_malloc (mal_len);
        } else {
            buf = g_realloc (buf, mal_len);
        }
    }
    va_end (save);

    gtkhx_session_emit_chat_log_line (gtkhx_session_get_default (), htlc, cid,
                                      name, color, buf);

    if (buf != autobuf) {
        g_free (buf);
    }
}

void
hx_printf_prefix (struct htlc_conn *htlc, guint32 cid, const char *prefix,
                  const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    /* `prefix` is now the bare tag to show in brackets — INFOPREFIX is
     * the string "hx", not a wrapper full of escape bytes. The forty-odd
     * callers pass INFOPREFIX and are unchanged. */
    log_line_emit (htlc, cid, prefix, HX_CHAT_LOG_INFO_COLOR, fmt, ap);
    va_end (ap);
}

void
hx_printf_named (struct htlc_conn *htlc, guint32 cid, const char *name,
                 gint32 color, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    log_line_emit (htlc, cid, name, color, fmt, ap);
    va_end (ap);
}

void
hx_printf (struct htlc_conn *htlc, guint32 cid, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    log_line_emit (htlc, cid, NULL, 0, fmt, ap);
    va_end (ap);
}
