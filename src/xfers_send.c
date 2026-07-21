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
#include "protocol.h" /* HN32 for the folder-resume RFLT parse */
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

/* One entry in the folder-upload plan built by hx_collect_put_entries. */
struct hx_put_entry {
    int type;              /* 1 = folder marker, 0 = file leaf */
    char *full_local_path; /* on-disk path; used only for files */
    GPtrArray *components;  /* (char *) path components from root */
    guint64 data_size;      /* for files */
};

static void
hx_put_entry_free (struct hx_put_entry *e)
{
    if (e->components) {
        g_ptr_array_unref (e->components);
    }
    g_free (e->full_local_path);
    g_free (e);
}

/* DFS pre-order walk of the local tree at dir_path, appending an
 * hx_put_entry per folder and file (folders before their contents).
 * prefix_components carries the path components from the upload root. */
static void
hx_collect_put_entries (GPtrArray *entries, const char *dir_path,
                        GPtrArray *prefix_components)
{
    GDir *d;
    const char *name;
    GError *err = NULL;
    GList *names = NULL;

    d = g_dir_open (dir_path, 0, &err);
    if (!d) {
        if (err) {
            g_error_free (err);
        }
        return;
    }
    while ((name = g_dir_read_name (d))) {
        names = g_list_prepend (names, g_strdup (name));
    }
    g_dir_close (d);
    /* Sort for deterministic order (helps test reproduction). */
    names = g_list_sort (names, (GCompareFunc)g_strcmp0);

    for (GList *l = names; l; l = l->next) {
        const char *n = l->data;
        char *full;
        struct stat sb;
        struct hx_put_entry *e;

        full = g_build_filename (dir_path, n, NULL);
        if (lstat (full, &sb) < 0) {
            g_free (full);
            continue;
        }

        e = g_new0 (struct hx_put_entry, 1);
        e->components = g_ptr_array_new_with_free_func (g_free);
        for (guint i = 0; i < prefix_components->len; i++) {
            g_ptr_array_add (
                e->components,
                g_strdup (g_ptr_array_index (prefix_components, i)));
        }
        g_ptr_array_add (e->components, g_strdup (n));

        if (S_ISDIR (sb.st_mode)) {
            e->type = 1;
            e->full_local_path = g_strdup (full);
            g_ptr_array_add (entries, e);
            /* DFS pre-order — recurse with this dir prepended to the
			 * prefix. */
            g_ptr_array_add (prefix_components, g_strdup (n));
            hx_collect_put_entries (entries, full, prefix_components);
            g_ptr_array_remove_index (prefix_components,
                                      prefix_components->len - 1);
        } else if (S_ISREG (sb.st_mode)) {
            e->type = 0;
            e->full_local_path = g_strdup (full);
            e->data_size = (guint64)sb.st_size;
            g_ptr_array_add (entries, e);
        } else {
            /* Skip symlinks and special files. */
            hx_put_entry_free (e);
        }

        g_free (full);
    }
    g_list_free_full (names, g_free);
}

/* Send a folder tree — the upload side of the FILE_NEXT/FILE_SEND state
 * machine. We walk the tree, and for each server FILE_NEXT reply with one
 * entry: the nfi header + path components, then (for files) the per-file
 * size + the FILP body via file_send_one. The server closes when done;
 * our next FILE_NEXT read short-reads and we stop. */
int
folder_send_all (struct htxf_conn *htxf, const char *base_path, guint8 *buf,
                 xfer_progress_fn progress)
{
    int retval = 0;
    GPtrArray *entries;
    GPtrArray *initial_comps;

    if (!progress) {
        return EINVAL;
    }

    entries
        = g_ptr_array_new_with_free_func ((GDestroyNotify)hx_put_entry_free);
    initial_comps = g_ptr_array_new_with_free_func (g_free);
    hx_collect_put_entries (entries, base_path, initial_comps);
    g_ptr_array_unref (initial_comps);

    for (guint i = 0; i < entries->len; i++) {
        struct hx_put_entry *e = g_ptr_array_index (entries, i);
        guint16 cmd_n;
        ssize_t n;
        guint16 wire_len = 4;

        /* Wait for FILE_NEXT from the server. */
        n = htxf_io_read (htxf, &cmd_n, 2);
        if (n != 2) {
            retval = errno ? errno : EIO;
            goto cleanup;
        }
        if (ntohs (cmd_n) != 3 /* FILE_NEXT */) {
            retval = EPROTO;
            goto cleanup;
        }

        /* nfi header: len = 4 + sum(3+nlen_i), type, pathcount. */
        for (guint j = 0; j < e->components->len; j++) {
            wire_len += 3
                        + (guint16)strlen (
                            (const char *)g_ptr_array_index (e->components, j));
        }
        {
            guint16 t;
            t = htons (wire_len);
            memcpy (&buf[0], &t, 2);
            t = htons ((guint16)e->type);
            memcpy (&buf[2], &t, 2);
            t = htons ((guint16)e->components->len);
            memcpy (&buf[4], &t, 2);
        }
        if (htxf_io_write (htxf, buf, 6) != 6) {
            retval = errno ? errno : EIO;
            goto cleanup;
        }

        for (guint j = 0; j < e->components->len; j++) {
            const char *c = g_ptr_array_index (e->components, j);
            gsize cl = strlen (c);
            guint8 ch[3];
            if (cl > 255) {
                retval = ENAMETOOLONG;
                goto cleanup;
            }
            ch[0] = 0;
            ch[1] = 0;
            ch[2] = (guint8)cl;
            if (htxf_io_write (htxf, ch, 3) != 3) {
                retval = errno ? errno : EIO;
                goto cleanup;
            }
            if (cl && htxf_io_write (htxf, c, cl) != (ssize_t)cl) {
                retval = errno ? errno : EIO;
                goto cleanup;
            }
        }

        if (e->type == 1) {
            /* Folder marker — no payload. */
            continue;
        }

        /* File leaf — server replies with FILE_SEND (fresh) or
		 * FILE_RESUME (resume from data_pos/rsrc_pos). */
        n = htxf_io_read (htxf, &cmd_n, 2);
        if (n != 2) {
            retval = errno ? errno : EIO;
            goto cleanup;
        }
        cmd_n = ntohs (cmd_n);
        htxf->data_pos = 0;
        htxf->rsrc_pos = 0;
        if (cmd_n == 2 /* FILE_RESUME */) {
            guint16 rlen;
            guint8 rflt[128];
            if (htxf_io_read (htxf, &rlen, 2) != 2) {
                retval = errno ? errno : EIO;
                goto cleanup;
            }
            rlen = ntohs (rlen);
            if (rlen > sizeof (rflt)) {
                retval = EPROTO;
                goto cleanup;
            }
            if (rlen && htxf_io_read (htxf, rflt, rlen) != (ssize_t)rlen) {
                retval = errno ? errno : EIO;
                goto cleanup;
            }
            if (rlen >= 50) {
                HN32 (&htxf->data_pos, &rflt[46]);
            }
            if (rlen >= 66) {
                HN32 (&htxf->rsrc_pos, &rflt[62]);
            }
        } else if (cmd_n != 1 /* FILE_SEND */) {
            retval = EPROTO;
            goto cleanup;
        }

        /* Set up htxf for file_send_one. data_size / rsrc_size come from
		 * the local file. */
        {
            struct stat sb;
            if (stat (e->full_local_path, &sb) < 0) {
                retval = errno ? errno : EIO;
                goto cleanup;
            }
            g_strlcpy (htxf->path, e->full_local_path, sizeof (htxf->path));
            htxf->data_size = (guint32)sb.st_size;
            htxf->rsrc_size = (guint32)resource_len (e->full_local_path);
        }

        /* Per-file payload size, matching file_send_one's writes:
		 * 133 + comment_len + ((rsrc_size - rsrc_pos) ? 16 : 0)
		 * + (data_size - data_pos) + (rsrc_size - rsrc_pos). */
        {
            guint32 file_size;
            guint32 size_n;
            guint32 com = (guint32)comment_len (e->full_local_path);
            file_size = 133 + com + (htxf->data_size - htxf->data_pos);
            if (htxf->rsrc_size - htxf->rsrc_pos) {
                file_size += 16 + (htxf->rsrc_size - htxf->rsrc_pos);
            }
            size_n = htonl (file_size);
            if (htxf_io_write (htxf, &size_n, 4) != 4) {
                retval = errno ? errno : EIO;
                goto cleanup;
            }
        }

        retval = file_send_one (htxf, buf, progress);
        if (retval) {
            goto cleanup;
        }
    }

cleanup:
    g_ptr_array_unref (entries);
    return retval;
}
