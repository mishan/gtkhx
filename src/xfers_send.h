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
 * docs/files-rust-migration-scope.md.
 */

#ifndef GTKHX_XFERS_SEND_H
#define GTKHX_XFERS_SEND_H

#include "config.h"
#include <glib.h>
/* xfer_progress_fn + the gtkhx_ffo_* codec FFI (pack_fork_header) live
 * in xfers_recv.h, shared by both directions. */
#include "xfers_recv.h"

struct htxf_conn;

/* Send a single file out over an already-open HTXF subchannel from
 * htxf->path. htxf->data_size / data_pos / rsrc_size / rsrc_pos must be
 * set to the local file's values before calling. `buf` is caller scratch
 * (>= 512 bytes, or larger for folder framing). `progress` fires as
 * bytes go out and must be non-NULL (a NULL hook returns EINVAL). Does
 * NOT play the completion sound, post a final update, or close the
 * channel. Returns 0 on success, an errno-like positive code on
 * failure. */
extern int file_send_one (struct htxf_conn *htxf, guint8 *buf,
                          xfer_progress_fn progress);

#endif /* GTKHX_XFERS_SEND_H */
