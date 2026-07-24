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
 * docs/rust/files-migration-scope.md.
 */

#ifndef GTKHX_XFERS_RECV_H
#define GTKHX_XFERS_RECV_H

#include "config.h"
#include <glib.h>
#include <stddef.h>

struct htxf_conn;

/* Progress hook fired as bytes arrive (htxf->total_pos is already
 * bumped). Must be non-NULL. Used by the C send path (file_send_one /
 * folder_send_all). */
typedef void (*xfer_progress_fn) (struct htxf_conn *htxf);

/* The receive-side progress hook — the HxnetXferParams shape (user_data +
 * byte delta), so the C driver owns the total_pos bump + the tasks-window
 * post. folder_recv_all forwards it straight into the per-file params. */
typedef void (*hxnet_xfer_progress_fn) (void *user_data, guint64 delta);

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

/* Receive a single file from an already-open HTXF subchannel — the body moved
 * to Rust (hxnet::xfer, W1 of the xfer-worker migration): it reads the
 * subchannel via hxnet::htxf in-crate and does the FILP/FFO codec + HFS fork I/O
 * natively (hxfiles-xfer / hxhfs). hxnet is a leaf crate that must not reference
 * C symbols, so the C driver hands everything in by value through this struct
 * (the scalars it reads off htxf directly) plus callback function pointers the
 * worker calls through — no upward link reference. Cancellation rides the
 * HtxfConn's HtxfAbort, so there's no canceled flag here.
 *
 * MUST match hxnet::xfer::HxnetXferParams (a #[repr(C)] struct) field-for-field. */
struct HxnetXferParams {
    void *hx;               /* htxf->hx (the Rust HtxfConn *) */
    const char *path;       /* htxf->path */
    guint64 file_budget;    /* solo: htxf->total_size; folder: FILE_SEND size */
    guint64 data_pos;       /* htxf->data_pos (resume offset) */
    guint64 rsrc_pos;       /* htxf->rsrc_pos */
    int opt_preview;
    int opt_folder;
    int opt_large;
    void *preview;          /* htxf->preview (hx_preview *) or NULL */
    void *user_data;        /* passed back to progress (the C side uses it as htxf) */
    void (*progress) (void *user_data, guint64 delta);
    void (*preview_chunk) (void *preview, const char *buf, gsize len);
    void (*preview_set_info) (void *preview, const char *type, const char *creator);
    void (*preview_done) (void *preview);
};

/* Returns 0 on success, an errno-like positive code on failure. Does NOT play
 * the completion sound, post a final update, or close the channel — the C
 * driver (get_thread / folder_recv_all) owns those. */
extern int hxnet_xfer_file_recv_one (const struct HxnetXferParams *params);

/* Receive a folder tree from an already-open HTXF subchannel into the
 * local directory `base_path` (which it creates). Drives the Hotline 1.5
 * FILE_NEXT/FILE_SEND state machine, building per-file HxnetXferParams and
 * calling hxnet_xfer_file_recv_one. `buf` is caller scratch (>= 1024 bytes).
 * `progress` is the HxnetXferParams shape, forwarded per file. Rewrites
 * htxf->path per file; the caller restores it. Returns 0 on success (including
 * the clean end-of-stream when the server closes), an errno-like code on
 * failure. */
extern int folder_recv_all (struct htxf_conn *htxf, const char *base_path,
                            guint8 *buf, hxnet_xfer_progress_fn progress);

#endif /* GTKHX_XFERS_RECV_H */
