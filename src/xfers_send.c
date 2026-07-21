/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
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
 * xfers_send.c — the single-file HTXF send state machine (FILP header,
 * data + resource forks). The mirror of xfers_recv.c; split out of
 * xfers.c so it depends only on htxf_io, the hxfiles-xfer FFO codec, the
 * HFS sidecar reads, and a caller-supplied progress callback — no GTK
 * worker shell. That keeps it directly linkable + testable
 * (tests/integration/test_file_put.c).
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <netinet/in.h> /* hfs_h_to_mtime uses htonl/ntohl */
#include <glib.h>
#include "hx.h"
#include "hfs.h"
#include "htxf_io.h"
#include "xfers_send.h"

/* Send bulk bytes from a local file fd to the HTXF subchannel.
 * Socket-side write goes through htxf_io_write (AEAD-aware). The
 * receive counterpart (rd_wr_recv) lives in xfers_recv.c. */
static int
rd_wr_send (int src_fd, guint64 data_len, struct htxf_conn *htxf,
            xfer_progress_fn progress)
{
    int len;
    g_autofree guint8 *buf = NULL;
    size_t bufsiz;

    bufsiz = 0xf000;
    buf = g_malloc (bufsiz);
    while (data_len) {
        size_t want = (bufsiz < data_len) ? bufsiz : (size_t)data_len;
        if ((len = read (src_fd, buf, want)) < 1) {
            return len ? errno : EIO;
        }
        /* htxf_io_write returns the full logical len on success — the
		 * hxnet channel writes the whole buffer (one AEAD frame when
		 * armed, or a write_all under the hood) — so the partial-write
		 * loop the fd-shaped rd_wr needed is gone. */
        if (htxf_io_write (htxf, buf, (size_t)len) != len) {
            return errno ? errno : EIO;
        }
        htxf->total_pos += len;
        progress (htxf);
        data_len -= (guint64)len;
    }
    return 0;
}

/* Send a single file out over an HTXF subchannel from htxf->path.
 * Mirror of file_recv_one — same wire framing, just the sending end.
 * The header layout is:
 *
 *   FILP fixed header (40 bytes)
 *   INFO/MAC block + TYPECREA + create/modify times + comment
 *   DATA marker + u32 BE data-fork-remaining length
 *   data fork bytes
 *   MACR marker + u32 BE rsrc-fork length
 *   rsrc fork bytes
 *
 * Used by:
 *   - put_thread (solo file): just call directly.
 *   - folder_put_thread: writes the per-file u32 size header first
 *     (over the HTXF socket), then calls this.
 *
 * htxf->data_size / data_pos / rsrc_size / rsrc_pos must be set to the
 * actual local file values before calling. Caller closes the socket and
 * plays the completion sound. Returns 0 on success, errno-like positive
 * code on failure. */
int
file_send_one (struct htxf_conn *htxf, guint8 *buf, xfer_progress_fn progress)
{
    int f, retval;
    struct hfsinfo fi;

    /* progress is called unconditionally below; a NULL hook is a caller
	 * bug, not a runtime condition — fail cleanly rather than segfault. */
    if (!progress) {
        return EINVAL;
    }

    /* Large-file solo upload: spec says "uploads send raw file data
	 * only — no FFO wrapper." The server reconstructs metadata from
	 * the filesystem. Per-file framing inside folder uploads still
	 * uses FFO (folder spec requires it), so this raw-data shortcut
	 * is gated on !opt.folder. The handshake's HTXF_FLAG_LARGE_FILE
	 * tells the server which shape to expect.
	 *
	 * Note: large-file uploads do NOT support resume — the FFO
	 * framing's INFO fork carries the resume offset, and we are
	 * omitting it. The spec says "If a partial upload exists,
	 * implementations SHOULD overwrite it." */
    if (htxf->opt.large && !htxf->opt.folder) {
        if ((f = open (htxf->path, O_RDONLY)) < 0) {
            return errno;
        }
        retval = rd_wr_send (f, htxf->data_size, htxf, progress);
        close (f);
        return retval;
    }

    memcpy (buf, "\
FILP\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
\2INFO\0\0\0\0\0\0\0\0\0\0\0^AMAC\
TYPECREA\
\0\0\0\0\0\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\
\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
\7\160\0\0\0\0\0\0\7\160\0\0\0\0\0\0\0\0\0\3hxd",
            115);
    hfsinfo_read (htxf->path, &fi);
    if (htxf->rsrc_size - htxf->rsrc_pos) {
        buf[23] = 3;
    }
    if (65 + fi.comlen + 12 > 0xff) {
        buf[38] = 1;
    }
    buf[39] = 65 + fi.comlen + 12;
    type_creator (&buf[44], htxf->path);
    *((guint32 *)(&buf[96])) = hfs_h_to_mtime (*((guint32 *)(&fi.create_time)));
    *((guint32 *)(&buf[104]))
        = hfs_h_to_mtime (*((guint32 *)(&fi.modify_time)));
    /* The 115-byte FILP-header template above ends at offset 114
     * (filled by the memcpy). The comment block starts at offset
     * 116 (comlen) + 117 (body). Offset 115 is structurally the
     * high byte of a u16 comment-length prefix (or padding,
     * depending on which FFO variant you read) and the original
     * code never wrote it — leaving the stack-allocated buf with
     * one uninitialised byte that went straight onto the wire when
     * htxf_io_write was called with length 133+comlen.
     *
     * Valgrind caught this as "Syscall param write(buf) points to
     * uninitialised byte(s)". Zero it. */
    buf[115] = 0;
    buf[116] = fi.comlen;
    memcpy (&buf[117], fi.comment, fi.comlen);
    /* DATA fork header — 16 bytes at buf[117+comlen], packed by the
	 * hxfiles-xfer encoder (the twin of the receive-side fork_len
	 * decode): "DATA" + the legacy 32-bit length, or the large-file
	 * high32/low32 split when opt.large. */
    gtkhx_ffo_pack_fork_header ((const guint8 *)"DATA", 4,
                                htxf->data_size - htxf->data_pos,
                                htxf->opt.large, &buf[117 + fi.comlen], 16);
    if (htxf_io_write (htxf, buf, 133 + fi.comlen) != 133 + (ssize_t)fi.comlen) {
        return errno ? errno : EIO;
    }
    htxf->total_pos += 133 + fi.comlen;
    if (!(htxf->data_size - htxf->data_pos)) {
        goto put_rsrc;
    }
    if ((f = open (htxf->path, O_RDONLY)) < 0) {
        return errno;
    }
    if (htxf->data_pos) {
        lseek (f, htxf->data_pos, SEEK_SET);
    }
    retval = rd_wr_send (f, htxf->data_size, htxf, progress);
    if (retval) {
        close (f);
        return retval;
    }
    close (f);

put_rsrc:
    /* MACR fork header — 16 bytes, same encoder as the DATA fork. */
    gtkhx_ffo_pack_fork_header ((const guint8 *)"MACR", 4, htxf->rsrc_size,
                                htxf->opt.large, buf, 16);
    if (htxf_io_write (htxf, buf, 16) != 16) {
        /* Same behaviour as the inlined version: a short write at
		 * the MACR-marker boundary is treated as a clean stop (the
		 * server may not want the rsrc fork). Don't surface as an
		 * error. */
        return 0;
    }
    htxf->total_pos += 16;
    if (!(htxf->rsrc_size - htxf->rsrc_pos)) {
        return 0;
    }

    if ((f = resource_open (htxf->path, O_RDONLY, 0)) < 0) {
        return errno;
    }
    if (htxf->rsrc_pos) {
        lseek (f, htxf->rsrc_pos, SEEK_SET);
    }
    retval = rd_wr_send (f, htxf->rsrc_size, htxf, progress);
    if (retval) {
        close (f);
        return retval;
    }
    close (f);
    return 0;
}
