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
#include <glib.h>
#include "hotline_proto.h" /* gtkhx_proto_parse_file_list_entry */
#include "filelist_walker.h"

void
hl_filelist_walk (const void *buf, gsize buflen, hl_filelist_entry_cb cb,
                  void *user_data)
{
    /* per-entry packed-binary decode moved to the Rust
	 * hotline-proto crate's parse_file_list_entry. The C callback
	 * surface stays the same — files_remote_provider.c keeps its
	 * existing cb signature without knowing the FFI exists. */
    const guint8 *data = (const guint8 *)buf;
    struct gtkhx_proto_file_list_entry entry;
    size_t off = 0;

    while (gtkhx_proto_parse_file_list_entry (data, buflen, off, &entry)) {
        if (cb) {
            cb (entry.ftype, entry.fsize, data + entry.name_off,
                entry.name_len, user_data);
        }
        off = entry.next_off;
    }
}
