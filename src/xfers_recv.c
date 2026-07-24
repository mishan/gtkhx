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
#include <glib.h>
#include "hx.h"
#include "hfs.h"
#include "htxf_io.h"
#include "preview.h"
#include "xfers_recv.h"


/* Receive a folder tree — the Hotline 1.5 FILE_NEXT/FILE_SEND state
 * machine. We drive the loop by writing FILE_NEXT; the server answers
 * with a next_file_info (nfi) header + path components, then either a
 * folder marker (mkdir, no payload) or a file (FILE_SEND + u32 size +
 * FILP body via file_recv_one). The server closes the socket when the
 * tree is exhausted; our next FILE_NEXT short-reads and we exit clean. */
int
folder_recv_all (struct htxf_conn *htxf, const char *base_path, guint8 *buf,
                 hxnet_xfer_progress_fn progress)
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

        cmd_n = g_htons(3); /* FILE_NEXT */
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
        nfi.len = g_ntohs(nfi.len);
        nfi.type = g_ntohs(nfi.type);
        nfi.pathcount = g_ntohs(nfi.pathcount);

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
        cmd_n = g_htons(1); /* FILE_SEND */
        if (htxf_io_write (htxf, &cmd_n, 2) != 2) {
            return errno ? errno : EIO;
        }

        if (htxf_io_read (htxf, &file_size, 4) != 4) {
            return errno ? errno : EIO;
        }
        file_size = g_ntohl(file_size);

        htxf->data_pos = 0;
        htxf->rsrc_pos = 0;

        {
            /* Folder-stream files never preview, so the preview hooks stay
             * NULL. Built inline (not via the xfers.c helper) so xfers_recv.c
             * has no cross-file reference — the Tier-3 folder test links this
             * TU without xfers.c. */
            struct HxnetXferParams params;
            memset (&params, 0, sizeof params);
            params.hx = htxf->hx;
            params.path = htxf->path;
            params.file_budget = file_size;
            params.data_pos = htxf->data_pos;
            params.rsrc_pos = htxf->rsrc_pos;
            params.opt_preview = htxf->opt.preview;
            params.opt_folder = htxf->opt.folder;
            params.opt_large = htxf->opt.large;
            params.user_data = htxf;
            params.progress = progress;
            retval = hxnet_xfer_file_recv_one (&params);
        }
        if (retval) {
            return retval;
        }
    }
}
