/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * xfers_recv.{c,h} — the single-file HTXF receive state machine, split
 * out of xfers.c so it can be linked and driven directly by a test
 * (tests/integration/test_file_get.c) without dragging in the GTK
 * worker shell. The only coupling back to that shell is a progress
 * callback (xfer_progress_fn) the caller supplies; xfers.c passes its
 * post_file_update, a test passes a no-op. See
 * docs/files-rust-migration-scope.md.
 */

#ifndef GTKHX_XFERS_RECV_H
#define GTKHX_XFERS_RECV_H

#include "config.h"
#include <glib.h>
#include <stddef.h>

struct htxf_conn;

/* Progress hook fired as bytes arrive (htxf->total_pos is already
 * bumped). Must be non-NULL. */
typedef void (*xfer_progress_fn) (struct htxf_conn *htxf);

/* ---- hxfiles-xfer FFO/FILP codec FFI (rust/crates/hxfiles-xfer) ----
 * Shared by the receive state machine here and the send packing in
 * xfers.c. Pure byte math; unit-tested headless in the crate. */

extern size_t gtkhx_ffo_info_block_len (guint8 b38, guint8 b39);
extern guint64 gtkhx_ffo_fork_len (const guint8 *marker, size_t marker_len,
                                   int large);
extern void gtkhx_ffo_pack_fork_header (const guint8 *tag, size_t tag_len,
                                        guint64 length, int large,
                                        guint8 *out, size_t out_len);

/* Fields the Rust FILP parser fills. Layout mirrors hxfiles-xfer's
 * #[repr(C)] GtkhxFilpInfo; offsets pinned on both sides so a field
 * reorder can't silently desync the ABI. */
struct gtkhx_filp_info {
    guint8 type_creator[8];
    guint8 create_time[4];
    guint8 modify_time[4];
    guint64 data_fork_len;
    guint8 comment[256];
    guint32 comment_len;
    int ok;
};
_Static_assert (sizeof (struct gtkhx_filp_info) == 288,
                "gtkhx_filp_info wire size");
_Static_assert (offsetof (struct gtkhx_filp_info, data_fork_len) == 16,
                "gtkhx_filp_info.data_fork_len offset");
_Static_assert (offsetof (struct gtkhx_filp_info, comment) == 24,
                "gtkhx_filp_info.comment offset");
_Static_assert (offsetof (struct gtkhx_filp_info, comment_len) == 280,
                "gtkhx_filp_info.comment_len offset");
_Static_assert (offsetof (struct gtkhx_filp_info, ok) == 284,
                "gtkhx_filp_info.ok offset");

extern void gtkhx_ffo_parse_filp_info (const guint8 *info, size_t info_len,
                                       int large, struct gtkhx_filp_info *out);

/* Receive a single file from an already-open HTXF subchannel into
 * htxf->path. file_budget is htxf->total_size for a solo download, or
 * this file's size off the FILE_SEND header inside a folder stream.
 * `buf` is caller-provided scratch (>= 1024 bytes). `progress` fires as
 * bytes arrive and must be non-NULL (a NULL hook returns EINVAL). Does
 * NOT play the completion sound, post a final update, or close the
 * channel — those stay with the caller. Returns 0 on success, an
 * errno-like positive code on failure. */
extern int file_recv_one (struct htxf_conn *htxf, guint64 file_budget,
                          guint8 *buf, xfer_progress_fn progress);

#endif /* GTKHX_XFERS_RECV_H */
