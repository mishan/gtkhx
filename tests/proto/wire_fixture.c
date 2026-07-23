/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "wire_fixture.h"
#include "htlc_recv_buf.h"
#include "hotline.h"

/* Memoryview of the hl_hdr currently sitting at the fixture buffer[0]. */
static struct hl_hdr *
hdr (struct htlc_conn *htlc)
{
    return (struct hl_hdr *)hx_test_in (htlc)->buf;
}

void
wire_fixture_init (struct htlc_conn *htlc, guint32 type, guint32 trans,
                   guint32 flag)
{
    memset (htlc, 0, sizeof (*htlc));
    /* Drop any stale receive buffer left keyed to this htlc address by
     * a previous test before allocating a fresh one. */
    hx_test_in_free (htlc);

    struct qbuf *q = hx_test_in (htlc);
    q->buf = g_malloc0 (SIZEOF_HL_HDR);
    q->len = SIZEOF_HL_HDR;
    q->pos = SIZEOF_HL_HDR;

    struct hl_hdr *h = hdr (htlc);
    h->type = g_htonl(type);
    h->trans = g_htonl(trans);
    h->flag = g_htonl(flag);
    h->len = g_htonl(0); /* updated as chunks are added */
    h->len2 = g_htonl(0);
    h->hc = g_htons(0);
}

void
wire_fixture_add_chunk (struct htlc_conn *htlc, guint16 type, guint16 len,
                        const void *data)
{
    struct qbuf *q = hx_test_in (htlc);
    const guint32 needed = SIZEOF_HL_DATA_HDR + len;
    const guint32 old_pos = q->pos;

    q->buf = g_realloc (q->buf, old_pos + needed);
    q->len = old_pos + needed;
    q->pos = old_pos + needed;

    struct hl_data_hdr *dh = (struct hl_data_hdr *)(q->buf + old_pos);
    dh->type = g_htons(type);
    dh->len = g_htons(len);
    if (len && data) {
        memcpy (q->buf + old_pos + SIZEOF_HL_DATA_HDR, data, len);
    }

    struct hl_hdr *h = hdr (htlc);
    guint16 hc = g_ntohs(h->hc) + 1;
    h->hc = g_htons(hc);

    /* len / len2: hlwrite encodes the byte count from the start of
	 * the data section to the end (i.e. total - SIZEOF_HL_HDR + 2,
	 * since the hc field is part of the data section in the wire
	 * format — see hlwrite). The dh_start macro doesn't actually
	 * read these fields though, it walks until the buffer length. We
	 * fill them in for correctness in case a handler ever looks. */
    guint32 data_len = q->pos - SIZEOF_HL_HDR + sizeof (h->hc);
    h->len = g_htonl(data_len);
    h->len2 = g_htonl(data_len);
}

void
wire_fixture_free (struct htlc_conn *htlc)
{
    hx_test_in_free (htlc);
}
