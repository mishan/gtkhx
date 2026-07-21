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
 * xfers_recv.c — the single-file HTXF receive state machine (data +
 * resource forks, the FILP frame, the folder-stream drain). Split out
 * of xfers.c so it depends only on htxf_io (the hxnet channel shim),
 * the hxfiles-xfer FFO codec, the HFS sidecar, and a caller-supplied
 * progress callback — no GTK worker shell. That keeps it directly
 * linkable + testable (tests/integration/test_file_get.c).
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <netinet/in.h> /* htons/ntohs for the folder FILE_NEXT framing */
#include <glib.h>
#include "hx.h"
#include "hfs.h"
#include "htxf_io.h"
#include "preview.h"
#include "xfers_recv.h"

/* Write bulk bytes from the HTXF subchannel to a local file fd. The
 * socket-side read goes through htxf_io_read (AEAD-aware: HOPE+ChaCha20
 * unwraps the length-prefixed frames and serves plaintext). */
static int
rd_wr_recv (int dst_fd, guint64 data_len, struct htxf_conn *htxf,
            xfer_progress_fn progress)
{
    int r, pos, len;
    g_autofree guint8 *buf = NULL;
    size_t bufsiz;

    bufsiz = 0xf000;
    buf = g_malloc (bufsiz);
    while (data_len) {
        size_t want = (bufsiz < data_len) ? bufsiz : (size_t)data_len;
        if ((len = htxf_io_read (htxf, buf, want)) < 1) {
            return len ? errno : EIO;
        }
        pos = 0;
        while (len) {
            if ((r = write (dst_fd, &(buf[pos]), len)) < 1) {
                return errno;
            }
            pos += r;
            len -= r;
            htxf->total_pos += r;

            progress (htxf);
        }
        data_len -= pos;
    }
    return 0;
}

static int
preview_get (guint32 data_len, struct htxf_conn *htxf, hx_preview *p,
             xfer_progress_fn progress)
{
    int len;
    g_autofree guint8 *buf = NULL;
    size_t bufsiz;

    bufsiz = 0xf000;
    buf = g_malloc (bufsiz);
    while (data_len) {
        if ((len = htxf_io_read (htxf, buf,
                                 (bufsiz < data_len) ? bufsiz : data_len))
            < 1) {
            return len ? errno : EIO;
        }
        /* hx_preview_chunk copies the buffer and marshals to the
		 * main thread internally — safe to call from the HTXF
		 * worker, returns immediately. */
        hx_preview_chunk (p, (char *)buf, len);
        htxf->total_pos += len;
        progress (htxf);
        data_len -= len;
    }
    hx_preview_done (p);
    return 0;
}

/* Receive a single file from an HTXF subchannel into htxf->path.
 *
 * Used by:
 *   - get_thread (solo file): file_budget = htxf->total_size
 *   - folder_get_thread (one file inside a folder stream):
 *     file_budget = the u32 size header just read off the wire
 *     for this file
 *
 * The wire framing is identical in both cases:
 *
 *   1. 40-byte FILP fixed header.
 *   2. Variable info+comment block (length encoded by FILP
 *      bytes 38/39, plus 16 bytes of DATA-fork marker at the tail).
 *   3. Data-fork payload.
 *   4. Optional MACR rsrc-fork marker (16 bytes) + rsrc payload,
 *      gated on whether tot_len has caught up to file_budget.
 *
 * Updates htxf->total_pos as bytes arrive (firing `progress`) so the
 * tasks-window progress bar advances. Does NOT play the completion
 * sound, post a final update, or close the socket — those are caller
 * responsibilities (the meaning differs for solo file vs. final file
 * in a folder).
 *
 * Returns 0 on success, errno-like positive code on failure. */
int
file_recv_one (struct htxf_conn *htxf, guint64 file_budget, guint8 *buf,
               xfer_progress_fn progress)
{
    guint32 pos, len;
    guint64 fork_len = 0;
    guint64 tot_len;
    int f, r, retval = 0;
    guint8 typecrea[8];
    struct hfsinfo fi;
    hx_preview *p = NULL;

    /* progress is called unconditionally below; a NULL hook is a caller
	 * bug, not a runtime condition — fail cleanly rather than segfault. */
    if (!progress) {
        return EINVAL;
    }

    len = 40;
    pos = 0;
    while (len) {
        if ((r = htxf_io_read (htxf, &(buf[pos]), len)) < 1) {
            return errno ? errno : EIO;
        }
        pos += r;
        len -= r;
        htxf->total_pos += r;
        progress (htxf);
    }
    pos = 0;
    len = (guint32)gtkhx_ffo_info_block_len (buf[38], buf[39]);
    tot_len = 40 + len;
    while (len) {
        if ((r = htxf_io_read (htxf, &(buf[pos]), len)) < 1) {
            return errno ? errno : EIO;
        }
        pos += r;
        len -= r;
        htxf->total_pos += r;
        progress (htxf);
    }
    /* Interpret the FILP info block in one shot: type/creator, the
	 * comment (via the buf[73+buf[71]] offset), the mac→header
	 * timestamp munge, and the trailing 16-byte DATA fork marker
	 * (with the large-file high32/low32 split). Ported to hxfiles-xfer;
	 * on a truncated block the parser reports !ok and we fail the
	 * transfer rather than reading past the info block the way the old
	 * blind indexing did.
	 *
	 * pos holds the info-block length (the read loop above accumulated
	 * it while draining len to 0); pass pos, not the now-zero len. */
    struct gtkhx_filp_info pi;
    gtkhx_ffo_parse_filp_info (buf, pos, htxf->opt.large, &pi);
    if (!pi.ok) {
        return EIO;
    }
    memcpy (typecrea, pi.type_creator, 8);
    memset (&fi, 0, sizeof (fi));
    /* Clamp the comment to the sidecar buffer — real comments are
	 * <200 bytes, so this only bites a hostile length. */
    fi.comlen = MIN (pi.comment_len, (guint32)sizeof (fi.comment));
    memcpy (fi.type, "HTftHTLC", 8);
    memcpy (fi.comment, pi.comment, fi.comlen);
    memcpy (&fi.create_time, pi.create_time, 4);
    memcpy (&fi.modify_time, pi.modify_time, 4);
    if (!htxf->opt.preview) {
        hfsinfo_write (htxf->path, &fi);
    }

    fork_len = pi.data_fork_len;
    tot_len += fork_len;
    if (!fork_len) {
        goto get_rsrc;
    }
    if (!htxf->opt.preview) {
        if ((f = open (htxf->path, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR))
            < 0) {
            return errno;
        }

        if (htxf->data_pos) {
            lseek (f, htxf->data_pos, SEEK_SET);
        }
        retval = rd_wr_recv (f, fork_len, htxf, progress);
        fsync (f);
        close (f);
    } else {
        /* The preview window is constructed on the main thread by
		 * rcv_task_file_get and stashed here as an hx_preview *;
		 * the worker just streams bytes through it. Constructing
		 * GtkWindow + AdwHeaderBar and calling gtk_window_present
		 * from a worker thread caused intermittent lockups — Wayland
		 * compositor round-trips during window mapping don't play
		 * nicely from non-main threads. */
        p = (hx_preview *)htxf->preview;
        if (!p) {
            return 0; /* nothing to write into; quietly stop */
        }
        /* Hand the FILP type/creator over to the preview module
		 * BEFORE the first chunk lands, so the viewer dispatch
		 * (text vs. image vs. ...) has the metadata it needs and
		 * the placeholder body gets replaced before any chunk
		 * tries to render. typecrea is laid out as type[0..3] +
		 * creator[4..7]. */
        {
            char type_s[5] = { 0 };
            char creator_s[5] = { 0 };
            memcpy (type_s, &typecrea[0], 4);
            memcpy (creator_s, &typecrea[4], 4);
            hx_preview_set_info (p, type_s, creator_s);
        }
        /* preview_get still takes 32-bit length; previews are small
		 * files (the preview window's whole point), so a >4 GiB
		 * preview is implausible. Clamp on the way through. */
        retval = preview_get ((guint32)MIN (fork_len, (guint64)0xFFFFFFFFu),
                              htxf, p, progress);
    }
    if (retval) {
        return retval;
    }
get_rsrc:
    /* Previews never carry resource forks — the server slices the
	 * payload at the data fork. */
    if (htxf->opt.preview) {
        goto done;
    }
    /* Folder transfers: skip the rsrc fork. mhxd's
	 * folder_getpaths populates pf->total_size with a phantom
	 * rsrc-fork allowance (sizeof(pathbuf) = MAXPATHLEN); file_send
	 * then writes the 16-byte MACR marker but, on regular text
	 * files with no AppleDouble sidecar, resource_open
	 * fails-into-stdin and file_send hangs (or returns -1) without
	 * actually streaming the claimed rsrc bytes. Following the
	 * MACR marker into a blocking rd_wr hangs the worker forever
	 * (issue surfaced on the 'Folder download task hangs' bug
	 * trace). Folder-stream consumers never persist resource forks
	 * to disk anyway — they're just plain-file copies of the
	 * tree — so skipping rsrc here is functionally equivalent.
	 *
	 * BUT we still have to consume whatever the server actually
	 * wrote since FILE_SEND announced file_budget bytes. Anything
	 * left in the socket buffer (the MACR marker mhxd buffered
	 * before its resource_open errored) would otherwise be read
	 * as the next FILE_NEXT response and corrupt the loop. Drain
	 * up to (file_budget - tot_len) bytes with a short per-read
	 * timeout — if data is in flight we slurp it; if the server
	 * gave up after the marker we time out and move on. */
    if (htxf->opt.folder) {
        if (tot_len < file_budget) {
            /* Arm a 200 ms per-read timeout on the hxnet channel so a
             * stalled server (mhxd's hung resource_open after the MACR
             * marker) does not block the worker forever. A timed-out
             * read surfaces as -1 / EIO, which the got<=0 break treats
             * as "give up and move on" -- the same semantics the old
             * g_socket_condition_timed_wait gate had. Long enough for
             * buffered marker bytes still in flight to land, short
             * enough not to stall the whole folder tree.
             *
             * If the timeout can't be armed, skip the drain entirely:
             * blocking reads against a server that may never send the
             * claimed bytes is the exact hang this is meant to avoid.
             * Same "give up and move on" outcome as the old NULL-socket
             * fallback. */
            if (htxf_io_set_read_timeout (htxf, 200) == 0) {
                guint64 remaining = file_budget - tot_len;
                while (remaining > 0) {
                    guint8 sink[2048];
                    size_t want = remaining < sizeof (sink) ? remaining
                                                            : sizeof (sink);
                    ssize_t got = htxf_io_read (htxf, sink, want);
                    if (got <= 0) {
                        break; /* timeout (EIO) or EOF -- give up */
                    }
                    remaining -= (guint64)got;
                    htxf->total_pos += (guint32)got;
                    progress (htxf);
                }
                /* Restore blocking reads for the next file in the
                 * stream. If clearing the timeout fails, a later read
                 * could time out mid-file and desync the folder stream
                 * (a partial AEAD frame leaves the decode state stuck),
                 * so fail the transfer rather than risk corruption. */
                if (htxf_io_set_read_timeout (htxf, 0) != 0) {
                    return errno ? errno : EIO;
                }
            }
        }
        goto done;
    }
    /* The file_budget gate is what makes this helper reusable for
	 * folder streams: solo mode passes htxf->total_size; folder
	 * mode passes this one file's size off the FILE_SEND header.
	 * Either way, "consumed all our budget" means no rsrc fork. */
    if (tot_len >= file_budget) {
        goto done;
    }
    pos = 0;
    len = 16;
    while (len) {
        if ((r = htxf_io_read (htxf, &(buf[pos]), len)) < 1) {
            return errno ? errno : EIO;
        }
        pos += r;
        len -= r;
        htxf->total_pos += r;
        progress (htxf);
    }
    /* MACR fork header — same split encoding as DATA: in large-
	 * file mode the Compression field at offset 4-7 holds the
	 * high 32 bits, DataSize at 12-15 holds the low 32 bits. */
    fork_len = gtkhx_ffo_fork_len (&buf[0], 16, htxf->opt.large);
    if (!fork_len) {
        goto done;
    }
    if ((f = resource_open (htxf->path, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR))
        < 0) {
        return errno;
    }
    if (htxf->rsrc_pos) {
        lseek (f, htxf->rsrc_pos, SEEK_SET);
    }
    retval = rd_wr_recv (f, fork_len, htxf, progress);
    if (retval) {
        return retval;
    }
    close (f);

done:
    memcpy (fi.type, typecrea, 8);
    if (!htxf->opt.preview) {
        hfsinfo_write (htxf->path, &fi);
    }
    return 0;
}

/* Receive a folder tree — the Hotline 1.5 FILE_NEXT/FILE_SEND state
 * machine. We drive the loop by writing FILE_NEXT; the server answers
 * with a next_file_info (nfi) header + path components, then either a
 * folder marker (mkdir, no payload) or a file (FILE_SEND + u32 size +
 * FILP body via file_recv_one). The server closes the socket when the
 * tree is exhausted; our next FILE_NEXT short-reads and we exit clean. */
int
folder_recv_all (struct htxf_conn *htxf, const char *base_path, guint8 *buf,
                 xfer_progress_fn progress)
{
    int retval = 0;

    if (!progress) {
        return EINVAL;
    }

    if (g_mkdir_with_parents (base_path, 0755) < 0 && errno != EEXIST) {
        return errno;
    }

    for (;;) {
        guint16 cmd_n;
        struct {
            guint16 len;
            guint16 type;
            guint16 pathcount;
        } __attribute__ ((packed)) nfi;
        guint16 i;
        char rel_path[MAXPATHLEN] = { 0 };
        gsize rel_len = 0;
        guint32 file_size;
        ssize_t n;

        cmd_n = htons (3); /* FILE_NEXT */
        if (htxf_io_write (htxf, &cmd_n, 2) != 2) {
            return errno ? errno : EIO;
        }

        n = htxf_io_read (htxf, &nfi, sizeof (nfi));
        if (n != (ssize_t)sizeof (nfi)) {
            /* Clean end-of-stream when n == 0 — server has run out of
			 * files and closed the socket. */
            if (n == 0) {
                return 0;
            }
            return errno ? errno : EIO;
        }
        nfi.len = ntohs (nfi.len);
        nfi.type = ntohs (nfi.type);
        nfi.pathcount = ntohs (nfi.pathcount);

        /* Read pathcount name components and join with '/' into the
		 * per-entry relative path. */
        for (i = 0; i < nfi.pathcount; i++) {
            guint8 ph[3];
            guint8 nlen;
            char name[256];
            if (htxf_io_read (htxf, ph, 3) != 3) {
                return errno ? errno : EIO;
            }
            nlen = ph[2];
            /* nlen is guint8 (max 255); name is 256 bytes — the read can
			 * never overflow. */
            if (nlen && htxf_io_read (htxf, name, nlen) != nlen) {
                return errno ? errno : EIO;
            }
            name[nlen] = 0;
            /* Defence in depth — refuse `..` and embedded `/` which would
			 * escape base_path. */
            if (!strcmp (name, "..") || memchr (name, '/', nlen)) {
                return EINVAL;
            }
            if (rel_len + (rel_len ? 1 : 0) + nlen + 1 >= sizeof (rel_path)) {
                return ENAMETOOLONG;
            }
            if (rel_len > 0) {
                rel_path[rel_len++] = '/';
            }
            memcpy (&rel_path[rel_len], name, nlen);
            rel_len += nlen;
            rel_path[rel_len] = 0;
        }

        /* Build the per-entry full local path. */
        if (rel_len == 0) {
            return EINVAL;
        }
        if (snprintf (htxf->path, sizeof (htxf->path), "%s/%s", base_path,
                      rel_path)
            >= (int)sizeof (htxf->path)) {
            return ENAMETOOLONG;
        }

        if (nfi.type == 1) {
            /* Folder marker — mkdir, no payload. */
            if (g_mkdir_with_parents (htxf->path, 0755) < 0
                && errno != EEXIST) {
                return errno;
            }
            continue;
        }

        /* Ensure the file's parent directory exists before writing it.
		 * The server normally sends a folder marker for each subdir
		 * first, but don't rely on that — a file at pathcount > 1 whose
		 * parent marker was missing (or a server that doesn't send
		 * markers) would otherwise fail the open. mkdir -p the parent. */
        if (nfi.pathcount > 1) {
            char *slash = strrchr (htxf->path, '/');
            if (slash && slash != htxf->path) {
                *slash = 0;
                int mrv = (g_mkdir_with_parents (htxf->path, 0755) < 0
                           && errno != EEXIST)
                              ? errno
                              : 0;
                *slash = '/';
                if (mrv) {
                    return mrv;
                }
            }
        }

        /* File entry — request fresh. FILE_SEND with data_pos/rsrc_pos
		 * zeroed tells the server to send the whole file. */
        cmd_n = htons (1); /* FILE_SEND */
        if (htxf_io_write (htxf, &cmd_n, 2) != 2) {
            return errno ? errno : EIO;
        }

        if (htxf_io_read (htxf, &file_size, 4) != 4) {
            return errno ? errno : EIO;
        }
        file_size = ntohl (file_size);

        htxf->data_pos = 0;
        htxf->rsrc_pos = 0;

        retval = file_recv_one (htxf, file_size, buf, progress);
        if (retval) {
            return retval;
        }
    }
}
