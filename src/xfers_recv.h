/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * xfers_recv.h — the FFI seam for the HTXF file-transfer workers, which
 * now live in Rust (hxnet::xfer): the per-file HxnetXferParams + the
 * folder HxnetFolderParams the C drivers (xfers.c) fill and hand across,
 * plus the hxfiles-xfer FFO codec decls a couple of C sites still use.
 * The single-file + folder state machines themselves are all Rust; the
 * C side keeps only the refcounted htxf_conn struct + the GTK worker
 * shell. See docs/rust/ROADMAP.md.
 */

#ifndef GTKHX_XFERS_RECV_H
#define GTKHX_XFERS_RECV_H

#include "config.h"
#include <glib.h>
#include <stddef.h>

struct htxf_conn;

/* ---- hxfiles-xfer FFO/FILP codec FFI (rust/crates/hxfiles-xfer) ----
 * Shared by the receive state machine here and the send packing in
 * xfers.c. Pure byte math; unit-tested headless in the crate. */

extern size_t gtkhx_ffo_info_block_len (guint8 b38, guint8 b39);
extern guint64 gtkhx_ffo_fork_len (const guint8 *marker, size_t marker_len,
                                   int large);
extern void gtkhx_ffo_pack_fork_header (const guint8 *tag, size_t tag_len,
                                        guint64 length, int large, guint8 *out,
                                        size_t out_len);

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
    void *hx;            /* htxf->hx (the Rust HtxfConn *) */
    const char *path;    /* htxf->path */
    guint64 file_budget; /* solo: htxf->total_size; folder: FILE_SEND size */
    guint64 data_pos;    /* htxf->data_pos (resume offset) */
    guint64 rsrc_pos;    /* htxf->rsrc_pos */
    int opt_preview;
    int opt_folder;
    int opt_large;
    void *preview;   /* htxf->preview (hx_preview *) or NULL */
    void *user_data; /* passed back to progress (the C side uses it as htxf) */
    void (*progress) (void *user_data, guint64 delta);
    void (*preview_chunk) (void *preview, const char *buf, gsize len);
    void (*preview_set_info) (void *preview, const char *type,
                              const char *creator);
    void (*preview_done) (void *preview);
    guint64 data_size; /* send-only: htxf->data_size (local data fork) */
    guint64 rsrc_size; /* send-only: htxf->rsrc_size (local rsrc fork) */
};

/* Returns 0 on success, an errno-like positive code on failure. Does NOT play
 * the completion sound, post a final update, or close the channel — the caller
 * owns those (the get_thread driver for a solo download, or the Rust
 * hxnet_xfer_folder_recv_all loop for a folder file). */
extern int hxnet_xfer_file_recv_one (const struct HxnetXferParams *params);

/* Send one file out an already-open HTXF subchannel from params->path — the
 * upload twin of hxnet_xfer_file_recv_one (was xfers_send.c::file_send_one),
 * ported to hxnet::xfer. Uses data_size/rsrc_size (+ data_pos/rsrc_pos resume
 * offsets); the preview + file_budget fields are unused. Returns 0 on success,
 * errno-like on failure; the caller (the put_thread driver for a solo upload,
 * or the Rust hxnet_xfer_folder_send_all loop for a folder file) closes the
 * channel + plays the sound. */
extern int hxnet_xfer_file_send_one (const struct HxnetXferParams *params);

/* Everything the folder transfer loops need, supplied by the C driver by value
 * (same no-upward-FFI discipline as HxnetXferParams). The worker builds each
 * per-file path from base_path internally, so it never touches htxf->path.
 *
 * MUST match hxnet::xfer::HxnetFolderParams (a #[repr(C)] struct) field-for-
 * field. */
struct HxnetFolderParams {
    void *hx;              /* htxf->hx (the Rust HtxfConn *) */
    const char *base_path; /* local tree root: created + written (recv) or
                             * walked (send) */
    int opt_preview;
    int opt_folder;
    int opt_large;
    void *user_data; /* passed back to progress (the C side uses it as htxf) */
    void (*progress) (void *user_data, guint64 delta);
};

/* Receive / send a whole folder tree over an already-open HTXF subchannel — the
 * Hotline 1.5 FILE_NEXT/FILE_SEND/FILE_RESUME state machine, ported to Rust
 * (hxnet::xfer, W3). recv creates base_path and mkdir/receives each entry; send
 * walks base_path (DFS pre-order) and answers each FILE_NEXT. Per-file byte
 * copying delegates to hxnet_xfer_file_{recv,send}_one. Returns 0 on success
 * (recv includes the clean end-of-stream when the server closes), an errno-like
 * positive code on failure. The C driver (folder_get_thread / folder_put_thread)
 * closes the channel + plays the completion sound. */
extern int hxnet_xfer_folder_recv_all (const struct HxnetFolderParams *params);
extern int hxnet_xfer_folder_send_all (const struct HxnetFolderParams *params);

#endif /* GTKHX_XFERS_RECV_H */
