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
#include <glib/gstdio.h> /* g_lstat / GStatBuf (portable) */
#include <glib.h>
#include "hx.h"
#include "protocol.h" /* HN32 for the folder-resume RFLT parse */
#include "hfs.h"
#include "htxf_io.h"
#include "xfers_send.h"

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
        GStatBuf sb;
        struct hx_put_entry *e;

        full = g_build_filename (dir_path, n, NULL);
        /* g_lstat: portable lstat (no symlink follow). */
        if (g_lstat (full, &sb) < 0) {
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
                 hxnet_xfer_progress_fn progress)
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
        if (g_ntohs(cmd_n) != 3 /* FILE_NEXT */) {
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
            t = g_htons(wire_len);
            memcpy (&buf[0], &t, 2);
            t = g_htons((guint16)e->type);
            memcpy (&buf[2], &t, 2);
            t = g_htons((guint16)e->components->len);
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
        cmd_n = g_ntohs(cmd_n);
        htxf->data_pos = 0;
        htxf->rsrc_pos = 0;
        if (cmd_n == 2 /* FILE_RESUME */) {
            guint16 rlen;
            guint8 rflt[128];
            if (htxf_io_read (htxf, &rlen, 2) != 2) {
                retval = errno ? errno : EIO;
                goto cleanup;
            }
            rlen = g_ntohs(rlen);
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

        /* Per-file payload size — MUST equal exactly what file_send_one
		 * writes below, or the trailing bytes desync the server's parse
		 * of the NEXT file's nfi and the whole folder stream fails.
		 * file_send_one always writes the FILP header (133 + comment_len)
		 * + the data fork + a 16-byte MACR marker + the rsrc fork:
		 *
		 *   133 + comment_len + (data_size - data_pos)
		 *       + 16 + (rsrc_size - rsrc_pos)
		 *
		 * The 16-byte MACR marker is unconditional (file_send_one emits
		 * it even with no resource fork), so it must always be counted.
		 * The old accounting only added the 16 when a resource fork was
		 * present, under-declaring by 16 for the common no-rsrc file and
		 * desyncing every multi-file folder upload — a bug that stayed
		 * hidden because the solo-file path has no following file for the
		 * leaked bytes to corrupt. */
        {
            guint32 file_size;
            guint32 size_n;
            guint32 com = (guint32)comment_len (e->full_local_path);
            file_size = 133 + com + (htxf->data_size - htxf->data_pos) + 16
                        + (htxf->rsrc_size - htxf->rsrc_pos);
            size_n = g_htonl(file_size);
            if (htxf_io_write (htxf, &size_n, 4) != 4) {
                retval = errno ? errno : EIO;
                goto cleanup;
            }
        }

        {
            /* Built inline (not via the xfers.c helper) so xfers_send.c has no
             * cross-file reference — the Tier-3 folder test links this TU
             * without xfers.c. htxf->data_size/rsrc_size + the resume offsets
             * were set above. */
            struct HxnetXferParams params;
            memset (&params, 0, sizeof params);
            params.hx = htxf->hx;
            params.path = htxf->path;
            params.data_pos = htxf->data_pos;
            params.rsrc_pos = htxf->rsrc_pos;
            params.data_size = htxf->data_size;
            params.rsrc_size = htxf->rsrc_size;
            params.opt_folder = htxf->opt.folder;
            params.opt_large = htxf->opt.large;
            params.user_data = htxf;
            params.progress = progress;
            retval = hxnet_xfer_file_send_one (&params);
        }
        if (retval) {
            goto cleanup;
        }
    }

cleanup:
    g_ptr_array_unref (entries);
    return retval;
}
