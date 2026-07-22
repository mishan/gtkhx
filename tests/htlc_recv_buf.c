/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "htlc_recv_buf.h"

/* htlc pointer -> struct qbuf *. Value destroy g_free's the qbuf
 * struct; the caller frees q->buf first via hx_test_in_free. */
static GHashTable *table;

struct qbuf *
hx_test_in (const struct htlc_conn *htlc)
{
    if (!table) {
        table = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                       g_free);
    }
    struct qbuf *q = g_hash_table_lookup (table, htlc);
    if (!q) {
        q = g_new0 (struct qbuf, 1);
        g_hash_table_insert (table, (gpointer) htlc, q);
    }
    return q;
}

void
hx_test_in_free (const struct htlc_conn *htlc)
{
    if (!table) {
        return;
    }
    struct qbuf *q = g_hash_table_lookup (table, htlc);
    if (q) {
        g_free (q->buf);
        g_hash_table_remove (table, htlc); /* frees q via value destroy */
    }
}
