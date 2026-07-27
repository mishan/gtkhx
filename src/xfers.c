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
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <time.h>
#include "hx.h"
#include "hxconn.h"
#include "hotline_proto.h"
#include "gtkhx_session.h"
#include "hfs.h"
#include "text_util.h"
#include "network.h"
#include "rcv.h"
#include "chat.h"
#include "tasks.h"
#include "uniquify_path.h"
#include "sound.h"
#include "files.h"
#include "hxnet_htxf.h"
#include "htxf_accessors.h"
#include "preview.h"
#include "xfers.h"
#include "xfers_recv.h"


/* xfers.c is the shrinking C remnant of the file-transfer worker shell. The
 * transfer byte-loops (hxnet::xfer) and the struct htxf_conn storage +
 * refcount/cancel lifecycle (hxnet::xfer_handle) were the first to move; the
 * xfers[] registry (Y1), construction + progress/completion marshaling (Y2),
 * and the worker dispatch + params (Y3) followed into the Rust transfer module
 * (hxhandlers::xfer). What's left here is xfer_go's wire-request build + the
 * local-path helpers (Y4) and the last-ref destructor (Y5). */

/* The last-ref teardown hx_htxf_unref runs on a handle's last ref, registered
 * via hx_htxf_set_destructor (S0.3). It does the GTK/preview + channel teardown
 * that must stay in C; the cancellation token + the struct itself are freed by
 * hx_htxf_free (Rust), which hx_htxf_unref calls right after this returns.
 * xfer_close_channel is the Rust transfer module's channel-close (hxhandlers). */
void
htxf_destructor (struct htxf_conn *htxf)
{
    /* Drop the per-htxf ref on the preview window if this transfer
	 * was a preview. The window holds its own ref independently;
	 * if the user has already closed the preview, this is the ref
	 * that keeps the struct alive long enough for the worker to
	 * stop touching it — see hx_preview_new's docstring. The
	 * field is NULL on a download/upload (non-preview) htxf,
	 * which hx_preview_unref handles with an early return. */
    hx_preview_unref ((hx_preview *)htxf->preview);
    htxf->preview = NULL;
    /* Close the hxnet channel (drops its ref to the cancellation token, so the
	 * token frees when hx_htxf_free drops the C-side creation ref). Also
	 * releases any AEAD read-side accumulator buffers. No-op on a transfer
	 * that ran in plaintext mode / was already closed by the worker. */
    xfer_close_channel (htxf);
}

/* Does either fork (data or resource) of the local path exist? */
static int
local_path_exists (const char *path)
{
    struct stat sb;
    if (stat (path, &sb) == 0) {
        return 1;
    }
    if (resource_len (path) > 0) {
        return 1;
    }
    return 0;
}

/* uniquify_local_path's core lives in src/uniquify_path.c now —
 * collision-resolving "foo.txt" → "foo (1).txt" logic, parameterised
 * over an exists predicate so the Tier 1 test can drive it without
 * touching the filesystem. This wrapper plugs in the real
 * local_path_exists check.
 *
 *   /dl/foo.txt        with foo.txt present  →  /dl/foo (1).txt
 *   /dl/archive.tar.gz with that present     →  /dl/archive.tar (1).gz
 *   /dl/README         with that present     →  /dl/README (1)
 *
 * N counts up from 1. After ~10000 tries we give up and leave path
 * at its last attempt — the subsequent open() will overwrite at
 * that name, the same behaviour as before this helper existed. */
static int
local_path_exists_adapter (const char *path, void *user_data)
{
    (void)user_data;
    return local_path_exists (path);
}

static void
uniquify_local_path (char *path, size_t cap)
{
    uniquify_path (path, cap, local_path_exists_adapter, NULL);
}

void
xfer_go (struct htxf_conn *htxf)
{
    guint16 hldirlen;
    guint8 *hldir;
    guint8 rflt[74];
    int resuming = 0;

    if (htxf->gone) {
        return;
    }

    htxf->gone = 1;

    if (htxf->type == XFER_GET) {
        /* Resume vs rename decision for downloads (skipped for
		 * previews, which don't write to disk):
		 *
		 *   - local file doesn't exist     →  fresh download
		 *   - local exists & local < srv   →  resume from local size
		 *   - local exists & local == srv  →  rename (file's already
		 *                                     fully downloaded —
		 *                                     don't blow it away,
		 *                                     don't ask the server
		 *                                     to resume past EOF)
		 *   - local exists & local > srv   →  rename (probably a
		 *                                     different file with
		 *                                     the same name)
		 *   - local exists & srv unknown   →  rename (no listing
		 *                                     captured at xfer_new
		 *                                     time — safer to keep
		 *                                     the existing copy)
		 *
		 * srv_data_size comes from the file listing's fsize and
		 * was captured at xfer_new. Treats only the data fork —
		 * the listing doesn't expose resource fork sizes, so for
		 * resumes we trust the worker's tot_len >= total_size
		 * check in get_thread to terminate the resource fork loop
		 * cleanly when the local rsrc fork is already complete. */
        if (!htxf->opt.preview) {
            struct stat sb;
            if (stat (htxf->path, &sb) == 0) {
                guint32 local_data = (guint32)sb.st_size;
                if (htxf->srv_data_size > 0
                    && local_data < htxf->srv_data_size) {
                    htxf->data_pos = local_data;
                    htxf->rsrc_pos = resource_len (htxf->path);
                    resuming = 1;
                } else {
                    uniquify_local_path (htxf->path, sizeof htxf->path);
                }
            }
        }

        if (resuming) {
            memcpy (rflt, "\
                          RFLT\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
                          \0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\2\
                          DATA\0\0\0\0\0\0\0\0\0\0\0\0\
                          MACR\0\0\0\0\0\0\0\0\0\0\0\0",
                    74);
            S32HTON (htxf->data_pos, &rflt[46]);
            S32HTON (htxf->rsrc_pos, &rflt[62]);
        }

        /* Use the structured fields: remotename for FILE_NAME
		 * (byte-for-byte from the listing, so `/` chars in names
		 * pass through), remotedir for the DIR chunk components.
		 * The has-parent-dir test is whether remotedir is anything
		 * other than empty or just `/`. */

        /* Phase E (follow-up): encode remotename to the negotiated
		 * wire encoding. remotename is stored UTF-8 in memory
		 * (file_list walker converts on receive; UI sources also
		 * pass UTF-8); convert back to Mac Roman in legacy mode.
		 * is_body = FALSE — filenames are single-line. */
        {
            gboolean utf8 = (hx_conn_has_cap (htxf->htlc, HTLC_CAP_TEXT_ENCODING)) != 0;
            gsize nm_wire_len = 0;
            char *nm_wire = gtkhx_text_for_wire (
                htxf->remotename, htxf->remotename_len, utf8, FALSE,
                &nm_wire_len);

            bool has_dir
                = htxf->remotedir[0]
                  && !(htxf->remotedir[0] == '/' && htxf->remotedir[1] == 0);
            hldir = NULL;
            hldirlen = 0;
            if (has_dir) {
                hldir = path_to_hldir (htxf->remotedir, &hldirlen, 0);
            }

            /* chunk layout moved to
			 * gtkhx_proto_build_file_get_chunks. Build BEFORE
			 * task_new — see hx_send_msg for the rationale. */
            struct hx_chunk chunks[3];
            int hc = (int)gtkhx_proto_build_file_get_chunks (
                (const uint8_t *)nm_wire, nm_wire_len, has_dir ? 1 : 0, hldir,
                hldirlen, resuming ? 1 : 0, resuming ? rflt : NULL, chunks,
                G_N_ELEMENTS (chunks));
            if (hc > 0) {
                task_new (htxf->htlc, RCV_TASK_FN (rcv_task_file_get),
                          htxf, 0, "xfer_go");
                hlwrite_chunks (htxf->htlc, HTLC_HDR_FILE_GET, 0, chunks,
                                hc);
            }
            g_free (hldir);
            g_free (nm_wire);
        }
    } else {
        /* Legacy 32-bit HTXF_SIZE — clamped to 0xFFFFFFFF when the
		 * true size exceeds 32 bits per the Large-File spec.
		 * Receivers in large-file mode prefer the 64-bit XFERSIZE64
		 * companion (sent only when CAP_LARGE_FILES was negotiated). */
        guint32 size_host = (guint32)MIN (htxf->total_size,
                                          (guint64)0xFFFFFFFFUL);
        gboolean large = (hx_conn_has_cap (htxf->htlc, HTLC_CAP_LARGE_FILES)) != 0;

        /* Initialise before the ternary — when the no-dir branch
		 * takes the NULL path, path_to_hldir never runs and
		 * hldirlen would otherwise stay indeterminate (function-
		 * scope declaration at the top of xfer_go). The builder
		 * still reads hldirlen on its way through, so an
		 * indeterminate read here is undefined behaviour even
		 * though the chunk is ultimately skipped. */
        hldirlen = 0;
        hldir = (htxf->remotedir[0]
                 && !(htxf->remotedir[0] == '/' && htxf->remotedir[1] == 0))
                    ? path_to_hldir (htxf->remotedir, &hldirlen, 0)
                    : NULL;
        bool has_dir = (hldir != NULL);
        bool has_preview = exists_remote (htxf->remotepath);

        /* Phase E (follow-up): encode remotename. */
        gboolean utf8 = (hx_conn_has_cap (htxf->htlc, HTLC_CAP_TEXT_ENCODING)) != 0;
        gsize nm_wire_len = 0;
        char *nm_wire
            = gtkhx_text_for_wire (htxf->remotename, htxf->remotename_len,
                                   utf8, FALSE, &nm_wire_len);

        /* chunk layout moved to gtkhx_proto_build_file_put
		 * _chunks. Build BEFORE task_new — see hx_send_msg for the
		 * rationale. Eight variants collapse to one builder call:
		 * presence flags for DIR / FILE_PREVIEW / XFERSIZE64 select
		 * the chunk count (2..=5). The builder takes host-order
		 * u32/u64 and BE-encodes into scratch. */
        struct hx_chunk chunks[5];
        uint8_t scratch[12];
        int hc = (int)gtkhx_proto_build_file_put_chunks (
            (const uint8_t *)nm_wire, nm_wire_len, has_dir ? 1 : 0, hldir,
            hldirlen, has_preview ? 1 : 0, size_host, large ? 1 : 0,
            htxf->total_size, chunks, G_N_ELEMENTS (chunks), scratch,
            sizeof (scratch));
        if (hc > 0) {
            task_new (htxf->htlc, RCV_TASK_FN (rcv_task_file_put), htxf,
                      0, "xfer_go");
            hlwrite_chunks (htxf->htlc, HTLC_HDR_FILE_PUT, 0, chunks,
                            hc);
        }
        g_free (nm_wire);
        if (hldir) {
            g_free (hldir);
        }
    }
}

int
xfer_go_timer (void *arg)
{
    xfer_go ((struct htxf_conn *)arg);
    return 0;
}
