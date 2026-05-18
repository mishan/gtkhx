/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * hl_filelist_walk — walk a packed run of hl_filelist_hdr chunks
 * (the body of an accumulated HTLS_DATA_FILE_LIST listing), firing
 * a callback per entry.
 *
 * Extracted from files_remote_provider.c::populate_from_chunks so
 * the Tier 2 unit test can drive the walker with hand-built wire
 * bytes — no GListStore, no provider, no HxFileEntry.
 *
 * Wire shape (one chunk):
 *
 *   u16 type        (always HTLS_DATA_FILE_LIST = 0xc8)
 *   u16 len         (rest-of-chunk length, big-endian)
 *   u32 ftype       (FourCC, e.g. "fldr" / "TEXT" / "JPEG")
 *   u32 fcreator    (FourCC)
 *   u32 fsize       (byte size, or item count for folders)
 *   u32 unknown
 *   u32 fnlen       (filename byte length)
 *   u8  fname[fnlen]
 *
 * The walker takes a borrowed reference to the buffer and does NOT
 * mutate it. The legacy walker in xtra/output_file_list path used
 * to byteswap fh->len in-place; that has been pulled out into a
 * local here.
 */

#include "config.h"
#include <string.h>
#include <arpa/inet.h>          /* ntohs */
#include <glib.h>
#include "compat.h"             /* PACKED */
#include "hotline.h"            /* struct hl_filelist_hdr, SIZEOF_HL_DATA_HDR */
#include "protocol.h"           /* HN32 */
#include "filelist_walker.h"

void
hl_filelist_walk (const void *buf, gsize buflen, hl_filelist_entry_cb cb,
                  void *user_data)
{
    const guint8 *p = (const guint8 *)buf;
    const guint8 *end = p + buflen;

    while (p + sizeof (struct hl_filelist_hdr) - 1 <= end) {
        const struct hl_filelist_hdr *fh = (const struct hl_filelist_hdr *)p;
        guint16 len_be;
        guint16 chunk_len;
        guint32 ftype, fsize, fnlen;

        memcpy (&len_be, &fh->len, sizeof len_be);
        chunk_len = ntohs (len_be);

        /* Compute the per-entry stride and make sure the body fits
		 * inside `end`. The legacy walker trusted the server; here
		 * we bound-check so a malformed chunk doesn't walk us off
		 * the end of the receive buffer. */
        if (p + SIZEOF_HL_DATA_HDR + chunk_len > end) {
            return;
        }

        HN32 (&ftype, &fh->ftype);
        HN32 (&fsize, &fh->fsize);
        HN32 (&fnlen, &fh->fnlen);

        if (cb) {
            cb (ftype, fsize, fh->fname, fnlen, user_data);
        }

        p += SIZEOF_HL_DATA_HDR + chunk_len;
    }
}
