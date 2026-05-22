/*
 * qbuf — tiny growable byte queue used by the Hotline protocol stack.
 *
 * Extracted from gtkhx.c so both the production binary and the Tier 3
 * integration harness can link the same implementation. There is no
 * GTK dependency here — just <glib.h> for g_realloc and the guint*
 * typedefs — so cipher.c / rcv.c / commands.c can use qbuf_set /
 * qbuf_add inside the harness without dragging the GUI in.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"

void
qbuf_set (struct qbuf *q, guint32 pos, guint32 len)
{
    int need_more = q->pos + q->len < pos + len;

    q->pos = pos;
    q->len = len;
    if (need_more) {
        q->buf = g_realloc (q->buf, q->pos + q->len);
    }
}

void
qbuf_add (struct qbuf *q, void *buf, guint32 len)
{
    size_t pos = q->pos + q->len;

    qbuf_set (q, q->pos, q->len + len);
    memcpy (&q->buf[pos], buf, len);
}
