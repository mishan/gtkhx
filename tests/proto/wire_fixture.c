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
#include <netinet/in.h>
#include <glib.h>
#include "wire_fixture.h"
#include "hotline.h"

/* Memoryview of the hl_hdr currently sitting at htlc->in.buf[0]. */
static struct hl_hdr *
hdr (struct htlc_conn *htlc)
{
    return (struct hl_hdr *)htlc->in.buf;
}

void
wire_fixture_init (struct htlc_conn *htlc, guint32 type, guint32 trans,
                   guint32 flag)
{
    memset (htlc, 0, sizeof (*htlc));

    htlc->in.buf = g_malloc0 (SIZEOF_HL_HDR);
    htlc->in.len = SIZEOF_HL_HDR;
    htlc->in.pos = SIZEOF_HL_HDR;

    struct hl_hdr *h = hdr (htlc);
    h->type = htonl (type);
    h->trans = htonl (trans);
    h->flag = htonl (flag);
    h->len = htonl (0); /* updated as chunks are added */
    h->len2 = htonl (0);
    h->hc = htons (0);
}

void
wire_fixture_add_chunk (struct htlc_conn *htlc, guint16 type, guint16 len,
                        const void *data)
{
    const guint32 needed = SIZEOF_HL_DATA_HDR + len;
    const guint32 old_pos = htlc->in.pos;

    htlc->in.buf = g_realloc (htlc->in.buf, old_pos + needed);
    htlc->in.len = old_pos + needed;
    htlc->in.pos = old_pos + needed;

    struct hl_data_hdr *dh = (struct hl_data_hdr *)(htlc->in.buf + old_pos);
    dh->type = htons (type);
    dh->len = htons (len);
    if (len && data) {
        memcpy (htlc->in.buf + old_pos + SIZEOF_HL_DATA_HDR, data, len);
    }

    struct hl_hdr *h = hdr (htlc);
    guint16 hc = ntohs (h->hc) + 1;
    h->hc = htons (hc);

    /* len / len2: hlwrite encodes the byte count from the start of
	 * the data section to the end (i.e. total - SIZEOF_HL_HDR + 2,
	 * since the hc field is part of the data section in the wire
	 * format — see hlwrite). The dh_start macro doesn't actually
	 * read these fields though, it walks until htlc->in.pos. We
	 * fill them in for correctness in case a handler ever looks. */
    guint32 data_len = htlc->in.pos - SIZEOF_HL_HDR + sizeof (h->hc);
    h->len = htonl (data_len);
    h->len2 = htonl (data_len);
}

void
wire_fixture_free (struct htlc_conn *htlc)
{
    if (htlc->in.buf) {
        g_free (htlc->in.buf);
        htlc->in.buf = NULL;
    }
    htlc->in.len = 0;
    htlc->in.pos = 0;
}
