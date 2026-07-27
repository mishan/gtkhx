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


/* The single-file receive / send state machines (file_recv_one,
 * file_send_one) + the FFO codec FFI (gtkhx_ffo_*) live in
 * xfers_recv.{c,h} / xfers_send.{c,h}. xfers.c keeps the worker shell:
 * the xfers[] list, refcount lifecycle, and the worker/completion
 * dispatch. */

/* Phase R3 X2: worker→main marshalling goes through hxbridge
 * (rust/crates/hxbridge/src/blocking.rs::gtkhx_bridge_post_to_main),
 * with the same g_main_context_invoke(NULL, ...) semantics the old
 * gtkthreads.c::gtkhx_post_to_main had. Moving xfers.c — its last
 * caller — onto the bridge orphaned gtkthreads.c, which has since been
 * deleted. Declared inline like banner.c's
 * gtkhx_bridge_spawn_blocking_with_idle. */
extern void gtkhx_bridge_post_to_main (GSourceFunc func, gpointer user_data);

/* Phase R3 X3: the transfer worker runs on hxbridge's tokio blocking
 * pool. Same shim banner.c uses — the worker callback runs on the
 * blocking pool, the completion callback on the GLib main loop once it
 * returns. Declared inline like banner.c. */
extern void gtkhx_bridge_spawn_blocking_with_idle (void (*worker) (void *),
                                                   void (*completion) (void *),
                                                   void *user_data);

/*
 * Reference counting and the worker → main marshal helpers.
 *
 * See the lifecycle comment over the refcount field in
 * struct htxf_conn (protocol.h) for the ownership model. In short:
 *
 *   - xfers[] holds 1 ref per htxf; dropped by xfer_remove_from_list
 *     when xfer_delete (server-cancel from rcv.c) or
 *     xfer_completion_entry (worker normal exit) unlinks the htxf.
 *   - The worker holds 1 ref taken in xfer_ready_write before the
 *     transfer is handed to the blocking pool; dropped by
 *     xfer_completion_entry, which the bridge runs on the main thread
 *     once the worker returns.
 *   - Each pending post_file_update idle holds 1 ref while it's
 *     queued; dropped by its dispatcher.
 *
 * htxf_conn is freed only when all owners have unref'd. Cancel —
 * either server-initiated or app-shutdown — sets htxf->canceled so
 * dispatchers skip their work, but the htxf stays alive until every
 * outstanding ref drops. No use-after-free even if the worker is
 * mid-stream when the server cancels.
 */
static struct htxf_conn *
htxf_ref (struct htxf_conn *htxf)
{
    if (htxf) {
        hx_htxf_ref (htxf);
    }
    return htxf;
}

/* Close the hxnet HTXF channel and clear the slot. Idempotent — the worker
 * closes on completion, then the destructor closes again on the last unref; the
 * NULL after the first close makes the second a no-op (hxnet_htxf_close is
 * NULL-safe), which is what prevents a double-free of the channel handle. */
static void
xfer_close_channel (struct htxf_conn *htxf)
{
    hxnet_htxf_close ((HtxfConn *) htxf->hx);
    htxf->hx = NULL;
}

/* The C teardown hx_htxf_unref runs on a handle's last ref, registered via
 * hx_htxf_set_destructor (S0.3). It does the GTK/preview + channel teardown that
 * must stay in C; the cancellation token + the struct itself are freed by
 * hx_htxf_free (Rust), which hx_htxf_unref calls right after this returns. */
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

/* Progress callback the Rust hxnet::xfer worker calls per chunk: bump the byte
 * counter and post a tasks-window update. user_data is the htxf. Passed by value
 * into HxnetXferParams so the leaf hxnet crate never references a C symbol. */
static void
xfer_progress_bump (void *user_data, guint64 delta)
{
    struct htxf_conn *htxf = user_data;
    hx_htxf_add_total_pos (htxf, delta);
    post_file_update (htxf);
}

/* Fill an HxnetXferParams for a receive of `file_budget` bytes off htxf. The
 * preview hooks are the hx_preview_* view functions (cast to the void*-first
 * callback shape — they take a pointer we only ever hand back the htxf->preview
 * we read here). Solo-download only (get_thread); the folder loop
 * (hxnet_xfer_folder_recv_all) builds its own preview-less per-file params. */
static void
xfer_recv_params (struct htxf_conn *htxf, guint64 file_budget,
                  struct HxnetXferParams *p)
{
    memset (p, 0, sizeof *p);
    p->hx = htxf->hx;
    p->path = htxf->path;
    p->file_budget = file_budget;
    p->data_pos = htxf->data_pos;
    p->rsrc_pos = htxf->rsrc_pos;
    p->opt_preview = htxf->opt.preview;
    p->opt_folder = htxf->opt.folder;
    p->opt_large = htxf->opt.large;
    p->preview = htxf->preview;
    p->user_data = htxf;
    p->progress = xfer_progress_bump;
    p->preview_chunk = (void (*) (void *, const char *, gsize)) hx_preview_chunk;
    p->preview_set_info
        = (void (*) (void *, const char *, const char *)) hx_preview_set_info;
    p->preview_done = (void (*) (void *)) hx_preview_done;
}

/* Fill an HxnetXferParams for an upload (send) of htxf. No preview; the send
 * worker uses data_size/rsrc_size + the resume offsets. Solo-upload only
 * (put_thread); the folder loop (hxnet_xfer_folder_send_all) builds its own
 * per-file params. */
static void
xfer_send_params (struct htxf_conn *htxf, struct HxnetXferParams *p)
{
    memset (p, 0, sizeof *p);
    p->hx = htxf->hx;
    p->path = htxf->path;
    p->data_pos = htxf->data_pos;
    p->rsrc_pos = htxf->rsrc_pos;
    p->data_size = htxf->data_size;
    p->rsrc_size = htxf->rsrc_size;
    p->opt_folder = htxf->opt.folder;
    p->opt_large = htxf->opt.large;
    p->user_data = htxf;
    p->progress = xfer_progress_bump;
}

/* Fill an HxnetFolderParams for a folder receive or send. The Rust folder loop
 * builds each per-file path from base_path itself, so htxf->path (the tree root)
 * is passed straight through and never mutated. */
static void
xfer_folder_params (struct htxf_conn *htxf, struct HxnetFolderParams *p)
{
    memset (p, 0, sizeof *p);
    p->hx = htxf->hx;
    p->base_path = htxf->path;
    p->opt_preview = htxf->opt.preview;
    p->opt_folder = htxf->opt.folder;
    p->opt_large = htxf->opt.large;
    p->user_data = htxf;
    p->progress = xfer_progress_bump;
}

static void *
get_thread (void *arg)
{
    struct htxf_conn *htxf = (struct htxf_conn *)arg;
    int retval;
    struct HxnetXferParams params;

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    xfer_recv_params (htxf, htxf->total_size, &params);
    retval = hxnet_xfer_file_recv_one (&params);
    if (retval) {
        goto ret;
    }

    play_sound (FILE_DONE);
    hx_htxf_set_total_pos (htxf, htxf->total_size);
    post_file_update (htxf);

ret:
    (void)retval;
    /* Close the hxnet HTXF channel, dropping the socket fd (and any rustls
     * session) it owns. */
    xfer_close_channel (htxf);

    /* Cleanup runs in xfer_completion_entry on the main thread once
	 * this worker returns — after every file_update idle queued above
	 * (same GMainContext, FIFO), so htxf stays alive for every pending
	 * dispatcher. */
    return NULL;
}

/* Receive a folder tree from an HTXF subchannel into the local
 * directory htxf->path. Implements the Hotline 1.5 folder
 * transfer protocol — the same FILE_NEXT/FILE_SEND state machine
 * that mhxd's folder_recv runs server-side. From the client's
 * (our) perspective we are the receiver; we drive the loop by
 * writing FILE_NEXT.
 *
 * Wire format per iteration:
 *
 *   us → server : FILE_NEXT (u16 BE = 3)
 *   server → us : next_file_info struct (6 bytes):
 *                   len:       u16 BE  (7 + nlen across all
 *                                       path components)
 *                   type:      u16 BE  (1 = folder, 0 = file)
 *                   pathcount: u16 BE  (number of name
 *                                       components that follow;
 *                                       mhxd always sends 1)
 *   for each pathcount:
 *       2 bytes pad, 1 byte nlen, nlen bytes name (joined with
 *       '/' onto the running relative path)
 *
 *   if type == 1 (folder):
 *       mkdir, loop back
 *   if type == 0 (file):
 *       us → server : FILE_SEND (u16 BE = 1)  — fresh download
 *                                              (resume support
 *                                              is a follow-up;
 *                                              the wire shape is
 *                                              FILE_RESUME = 2
 *                                              followed by a
 *                                              74-byte RFLT)
 *       server → us : u32 BE size, then standard file framing
 *                     (FILP / INFO / DATA / optional MACR)
 *       us : file_recv_one(s, htxf, size, buf) to drain
 *
 * The server doesn't send a terminator — it just closes the
 * socket when nfiles is exhausted. Our next FILE_NEXT short-reads
 * the nfi and we exit cleanly.
 *
 * htxf->path holds the local destination root on entry; we
 * snapshot it and rewrite the path field per-file for the
 * file_recv_one call. Restored to the root before cleanup so the
 * tasks-window display has a sensible label. */
static void *
folder_get_thread (void *arg)
{
    struct htxf_conn *htxf = (struct htxf_conn *)arg;
    int retval = 0;
    struct HxnetFolderParams params;

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    /* The Rust folder loop builds per-file paths from base_path (htxf->path)
	 * internally and never mutates it, so no snapshot/restore is needed — the
	 * task-window label stays on the folder root throughout. */
    xfer_folder_params (htxf, &params);
    retval = hxnet_xfer_folder_recv_all (&params);
    if (retval) {
        goto ret;
    }

    play_sound (FILE_DONE);
    hx_htxf_set_total_pos (htxf, htxf->total_size);
    post_file_update (htxf);

ret:
    (void)retval;
    xfer_close_channel (htxf);
    return NULL;
}

static void *
put_thread (void *arg)
{
    struct htxf_conn *htxf = (struct htxf_conn *)arg;
    int retval;
    struct HxnetXferParams params;

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    xfer_send_params (htxf, &params);
    retval = hxnet_xfer_file_send_one (&params);
    if (retval) {
        goto ret;
    }

    play_sound (FILE_DONE);
    post_file_update (htxf);

ret:
    (void)retval;
    xfer_close_channel (htxf);

    /* Cleanup runs in xfer_completion_entry — see get_thread. */
    return NULL;
}

/* Mirror of folder_get_thread for the upload direction. The
 * server drives the loop by writing FILE_NEXT to us; we walk the
 * local tree in DFS pre-order (parent dirs before their
 * contents) and respond to each FILE_NEXT with one entry:
 *
 *   us → server : nfi (6 bytes: len, type, pathcount) + per
 *                 component (2 byte pad + 1 byte nlen + nlen
 *                 bytes name). type=1 marks a folder (no
 *                 payload), type=0 marks a file leaf.
 *   server → us : for a file leaf, a u16 BE cmd:
 *                   FILE_SEND   (1) — server wants a fresh send
 *                   FILE_RESUME (2) — server sends a u16 BE len
 *                                     and then `len` bytes of
 *                                     RFLT carrying data_pos /
 *                                     rsrc_pos. We honour the
 *                                     resume offsets in
 *                                     file_send_one.
 *   us → server : u32 BE size (remaining file payload bytes),
 *                 then the standard FILP/INFO/DATA/MACR framing
 *                 via file_send_one.
 *
 * When the local walk runs out of entries we just close the
 * socket; the server's next FILE_NEXT write short-reads and its
 * loop exits cleanly. Same convention mhxd uses on its own
 * folder_recv exit path.
 *
 * htxf->path holds the local source root; the Rust folder loop
 * (hxnet_xfer_folder_send_all) walks it and builds each per-file
 * path internally, so htxf->path is never mutated.
 *
 * Components are sent with pathcount equal to the depth of the
 * entry under the root, so files nested at root/sub/sub2/leaf
 * become pathcount=3 with components ["sub","sub2","leaf"].
 * mhxd's folder_recv joins them with '/' on a fresh fpath built
 * from dirpath, so deep trees land correctly even though
 * folder_recv itself never advances dirpath. */
static void *
folder_put_thread (void *arg)
{
    struct htxf_conn *htxf = (struct htxf_conn *)arg;
    int retval = 0;
    struct HxnetFolderParams params;

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    /* base_path (htxf->path) is the local source root; the Rust folder loop
	 * walks it and never mutates it. */
    xfer_folder_params (htxf, &params);
    retval = hxnet_xfer_folder_send_all (&params);
    if (retval) {
        goto ret;
    }

    play_sound (FILE_DONE);
    hx_htxf_set_total_pos (htxf, htxf->total_size);
    post_file_update (htxf);

ret:
    (void)retval;
    xfer_close_channel (htxf);
    return NULL;
}

/* Worker entry on hxbridge's tokio blocking pool. Dispatches to the
 * right transfer body and returns; cleanup happens separately in
 * xfer_completion_entry once this returns. Runs OFF the main thread, so
 * it never touches GTK directly — only marshals via post_file_update.
 * opt.folder picks the FILE_NEXT
 * folder-stream worker; plain XFER_GET / XFER_PUT use the single-file
 * workers. */
static void
xfer_worker_entry (void *arg)
{
    struct htxf_conn *htxf = arg;
    if (htxf->opt.folder) {
        if (htxf->type == XFER_GET) {
            folder_get_thread (htxf);
        } else {
            folder_put_thread (htxf);
        }
    } else {
        if (htxf->type == XFER_GET) {
            get_thread (htxf);
        } else {
            put_thread (htxf);
        }
    }
}

void
xfer_ready_write (struct htxf_conn *htxf)
{
    /* Take the worker's reference BEFORE handing the transfer to the
	 * blocking pool, so the htxf can't be freed mid-spawn if some other
	 * path drops the xfers[] ref. xfer_completion_entry drops this ref
	 * on the worker's behalf once the worker returns.
	 *
	 * The worker runs on hxbridge's tokio blocking pool and the
	 * completion is marshalled back to the main thread for us. The shim
	 * can't fail softly — a runtime-startup failure aborts the process
	 * (same fatal posture as the banner.c HTXF worker) — so there's no
	 * spawn error path to handle here. */
    htxf_ref (htxf);
    gtkhx_bridge_spawn_blocking_with_idle (xfer_worker_entry,
                                           xfer_completion_entry, htxf);
}
