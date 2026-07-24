/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * xfers_send.{c,h} — the single-file HTXF send state machine, the
 * mirror of xfers_recv.c. Split out of xfers.c so it depends only on
 * htxf_io (the hxnet channel shim), the hxfiles-xfer FFO codec, the HFS
 * sidecar reads, and a caller-supplied progress callback — no GTK worker
 * shell. That keeps it directly linkable + testable
 * (tests/integration/test_file_put.c). See
 * docs/rust/files-migration-scope.md.
 */

#ifndef GTKHX_XFERS_SEND_H
#define GTKHX_XFERS_SEND_H

#include "config.h"
#include <glib.h>
/* xfer_progress_fn + the gtkhx_ffo_* codec FFI (pack_fork_header) live
 * in xfers_recv.h, shared by both directions. */
#include "xfers_recv.h"

struct htxf_conn;

/* The single-file send body moved to Rust (hxnet::xfer, W2 of the xfer-worker
 * migration): hxnet_xfer_file_send_one (declared in xfers_recv.h alongside the
 * shared HxnetXferParams). The C drivers (put_thread, folder_send_all) build the
 * params and call it. */

/* Send a folder tree rooted at `base_path` out over an already-open HTXF
 * subchannel. Walks the local tree (DFS pre-order) and answers each
 * server FILE_NEXT with one entry: a next_file_info header + path
 * components, then (for files) the per-file size + FILP body, built per file
 * into HxnetXferParams and sent via hxnet_xfer_file_send_one. `buf` is caller
 * scratch (>= 2048 bytes). `progress` is the HxnetXferParams shape. Rewrites
 * htxf->path per file; the caller restores it. Returns 0 on success, an
 * errno-like code on failure. */
extern int folder_send_all (struct htxf_conn *htxf, const char *base_path,
                            guint8 *buf, hxnet_xfer_progress_fn progress);

#endif /* GTKHX_XFERS_SEND_H */
