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
#include "htxf_io.h"
#include "preview.h"
#include "xfers.h"
#include "xfers_recv.h"
#include "xfers_send.h"

int nxfers = 0;
struct htxf_conn **xfers = 0;
static void xfer_remove_from_list (struct htxf_conn *htxf);

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
        g_atomic_int_inc (&htxf->refcount);
    }
    return htxf;
}

static void
htxf_unref (struct htxf_conn *htxf)
{
    if (!htxf) {
        return;
    }
    if (!g_atomic_int_dec_and_test (&htxf->refcount)) {
        return;
    }
    /* Drop the per-htxf ref on the preview window if this transfer
	 * was a preview. The window holds its own ref independently;
	 * if the user has already closed the preview, this is the ref
	 * that keeps the struct alive long enough for the worker to
	 * stop touching it — see hx_preview_new's docstring. The
	 * field is NULL on a download/upload (non-preview) htxf,
	 * which hx_preview_unref handles with an early return. */
    hx_preview_unref ((hx_preview *)htxf->preview);
    htxf->preview = NULL;
    /* Release any AEAD read-side accumulator buffers the HTXF
	 * subchannel Phase E wrappers might have allocated. No-op
	 * on a transfer that ran in plaintext mode. */
    htxf_io_release (htxf);
    /* Drop the C side's ref to the cancellation token. The hxnet channel
	 * (closed by htxf_io_release just above) already dropped its ref, so
	 * this frees the token. NULL-safe on a transfer that never opened. */
    htxf_io_abort_free (htxf);
    g_free (htxf);
}

struct fu_job {
    struct htxf_conn *htxf;
};
static gboolean
fu_dispatch (gpointer data)
{
    struct fu_job *j = data;
    /* canceled is a cross-thread flag (worker reads it in htxf_io_read/
	 * _write); access it atomically everywhere for a single coherent
	 * memory model, even though this dispatcher runs on the main thread. */
    if (!g_atomic_int_get (&j->htxf->canceled)) {
        gtkhx_session_emit_file_update (gtkhx_session_get_default (),
                                        sess_from_htlc (j->htxf->htlc), j->htxf);
    }
    htxf_unref (j->htxf);
    g_free (j);
    return G_SOURCE_REMOVE;
}
static void
post_file_update (struct htxf_conn *htxf)
{
    struct fu_job *j = g_new0 (struct fu_job, 1);
    j->htxf = htxf_ref (htxf);
    gtkhx_bridge_post_to_main (fu_dispatch, j);
}

/* The actual transfer teardown, deferred onto the GLOBAL default main
 * context as an idle source. Unlink from xfers[] (a no-op if the server
 * already cancelled us out) and drop the worker's ref. Returns
 * G_SOURCE_REMOVE — runs once. */
static gboolean
xfer_cleanup_dispatch (gpointer data)
{
    struct htxf_conn *htxf = data;
    xfer_remove_from_list (htxf);
    htxf_unref (htxf);
    return G_SOURCE_REMOVE;
}

/* Worker → main completion, invoked by gtkhx_bridge_spawn_blocking_with_
 * idle once the transfer worker returns. The teardown must run AFTER
 * every progress idle the worker already queued via post_file_update
 * (g_main_context_invoke(NULL, ...) at G_PRIORITY_DEFAULT on the global
 * default context) — running it early would unlink+unref the htxf out
 * from under a still-pending file_update dispatcher (emitting
 * xfer-destroyed / starting the next transfer too soon, and letting a
 * late file_update fire after destruction).
 *
 * We can't re-post via gtkhx_bridge_post_to_main here: it wraps
 * g_main_context_invoke, which runs the callback SYNCHRONOUSLY when
 * called from the thread that owns the target context — and this
 * completion already runs on the main thread, the owner of the global
 * default context. That would tear down immediately, ahead of the
 * queued file_updates, defeating the ordering.
 *
 * Instead attach a real idle source with g_idle_add. It is always
 * asynchronous (dispatched in a later iteration, never inline), and its
 * G_PRIORITY_DEFAULT_IDLE priority sits strictly below the file_updates'
 * G_PRIORITY_DEFAULT, so the teardown is guaranteed to run after every
 * pending file_update on the same (global default) context. The worker's
 * ref keeps htxf alive until the idle fires. */
static void
xfer_completion_entry (void *arg)
{
    g_idle_add (xfer_cleanup_dispatch, arg);
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

/* Shared init for xfer_new / xfer_new_folder — sets up the
 * structured fields, the xfers[] enqueue, refcount, and the
 * initial file_update emission. Does NOT call xfer_go; the caller
 * decides whether to drive the wire request inline (xfer_new →
 * xfer_go) or to send a different opcode itself (xfer_new_folder
 * → hx_get_folder / hx_put_folder builds its own task_new +
 * hlwrite). */
static struct htxf_conn *
xfer_init (const char *path, const char *remotedir, const char *remotename,
           gsize remotename_len, guint16 type)
{
    struct htxf_conn *htxf;
    gsize dir_len;
    gsize sep_len;
    gsize stash_len;

    htxf = g_malloc0 (sizeof (struct htxf_conn));

    /* Stash the structured fields verbatim. remotename is the wire
	 * NAME chunk; remotedir is the wire DIR chunk source. Names can
	 * contain any byte (incl. dir_char) so we never split, ever. */
    if (remotename_len >= sizeof htxf->remotename) {
        remotename_len = sizeof htxf->remotename - 1;
    }
    htxf->remotename_len = (guint16)remotename_len;
    memcpy (htxf->remotename, remotename, remotename_len);
    htxf->remotename[remotename_len] = 0;

    if (remotedir) {
        g_strlcpy (htxf->remotedir, remotedir, sizeof htxf->remotedir);
    }

    /* remotepath is kept for display/log purposes only. It's the
	 * dir-with-trailing-slash-then-name joined string. If a
	 * downstream consumer tries to split it on dir_char, they'll
	 * mis-identify slashes-in-name as dir boundaries — that is the
	 * exact bug we're sidestepping by keeping the structured fields
	 * alongside. */
    dir_len = strlen (htxf->remotedir);
    sep_len = (dir_len > 0 && htxf->remotedir[dir_len - 1] != '/') ? 1 : 0;
    stash_len = dir_len + sep_len + remotename_len;
    if (stash_len >= sizeof htxf->remotepath) {
        /* Truncate; remotepath is for display only. */
        stash_len = sizeof htxf->remotepath - 1;
    }
    {
        char *p = htxf->remotepath;
        gsize n = stash_len;
        gsize d = MIN (n, dir_len);
        memcpy (p, htxf->remotedir, d);
        p += d;
        n -= d;
        if (sep_len && n > 0) {
            *p++ = '/';
            n--;
        }
        if (n > 0) {
            memcpy (p, htxf->remotename, n);
            p += n;
        }
        *p = 0;
    }

    strcpy (htxf->path, path);
    htxf->type = type;
    htxf->queue = -1;
    /* refcount = 1 represents the xfers[] array's ownership. The
	 * worker takes its own ref in xfer_ready_write before the
	 * transfer is handed to the blocking pool. */
    htxf->refcount = 1;
    /* canceled is read on the worker thread (htxf_io_read/_write); use
	 * atomics for every access. This initial store is before any worker
	 * exists, but stay consistent with the cross-thread stores below. */
    g_atomic_int_set (&htxf->canceled, FALSE);

    /* Allocate the cancellation token now, on the main thread, so it's
	 * live for the htxf's whole lifetime — armed later by htxf_connect
	 * (worker), triggered by xfer_delete (main), freed in htxf_unref. */
    htxf_io_abort_init (htxf);

    xfers = g_realloc (xfers, (nxfers + 1) * sizeof (struct htxf_conn *));
    xfers[nxfers] = htxf;
    nxfers++;

    htxf->htlc = hx_active_session ()->htlc;
    htxf->total_pos = 0;
    htxf->total_size = 1;
    gtkhx_session_emit_file_update (gtkhx_session_get_default (), sess_from_htlc (htxf->htlc),
                                    htxf);

    return htxf;
}

struct htxf_conn *
xfer_new (const char *path, const char *remotedir, const char *remotename,
          gsize remotename_len, guint16 type, int preview,
          guint32 srv_data_size)
{
    struct htxf_conn *htxf;

    htxf = xfer_init (path, remotedir, remotename, remotename_len, type);

    /* opt.preview and srv_data_size MUST be set before xfer_go
	 * runs below — xfer_go gates its resume / rename decision on
	 * both. Setting these via the returned htxf pointer after this
	 * function returns is too late: when nxfers == 1 (or queueing
	 * is off) we call xfer_go inline, and the wire request goes
	 * out before the caller could flip them. */
    htxf->opt.preview = preview ? 1 : 0;
    htxf->srv_data_size = srv_data_size;

    if (nxfers == 1 || !gtkhx_prefs.queuedl) {
        xfer_go (htxf);
    }

    return htxf;
}

struct htxf_conn *
xfer_new_folder (const char *path, const char *remotedir,
                 const char *remotename, gsize remotename_len, guint16 type)
{
    struct htxf_conn *htxf;

    htxf = xfer_init (path, remotedir, remotename, remotename_len, type);

    /* Flagged BEFORE the caller fires the wire request so the
	 * dispatcher in xfer_ready_write picks the folder thread when
	 * the server's task reply arrives and we hit the
	 * xfer_ready_write call from rcv_task_folder_get. */
    htxf->opt.folder = 1;

    return htxf;
}

void
xfer_up (int num)
{
    struct htxf_conn *tmp;

    tmp = xfers[num - 1];
    xfers[num - 1] = xfers[num];
    xfers[num] = tmp;
}

int
xfer_down (int num)
{
    struct htxf_conn *tmp;

    if (nxfers - 1 == num) {
        return 1;
    }

    tmp = xfers[num + 1];
    xfers[num + 1] = xfers[num];
    xfers[num] = tmp;

    return 0;
}

int
xfer_num (struct htxf_conn *htxf)
{
    int i;

    for (i = 0; i < nxfers; i++) {
        if (xfers[i] == htxf) {
            return i;
        }
    }

    return -1;
}

/* Progress callback the Rust hxnet::xfer worker calls per chunk: bump the byte
 * counter and post a tasks-window update. user_data is the htxf. Passed by value
 * into HxnetXferParams so the leaf hxnet crate never references a C symbol. */
static void
xfer_progress_bump (void *user_data, guint64 delta)
{
    struct htxf_conn *htxf = user_data;
    htxf->total_pos += delta;
    post_file_update (htxf);
}

/* Fill an HxnetXferParams for a receive of `file_budget` bytes off htxf. The
 * preview hooks are the hx_preview_* view functions (cast to the void*-first
 * callback shape — they take a pointer we only ever hand back the htxf->preview
 * we read here). Solo-download only (get_thread); folder_recv_all builds its own
 * preview-less params inline so xfers_recv.c stays free of xfers.c references. */
static void
xfer_recv_params (struct htxf_conn *htxf, guint64 file_budget,
                  struct HxnetXferParams *p)
{
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
    htxf->total_pos = htxf->total_size;
    post_file_update (htxf);

ret:
    (void)retval;
    /* htxf_io_release closes the hxnet HTXF channel, dropping the
     * socket fd (and any rustls session) it owns. */
    htxf_io_release (htxf);

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
    guint8 buf[1024];
    char base_path[MAXPATHLEN];

    /* Snapshot the destination root before connecting: folder_recv_all
	 * rewrites htxf->path per file, and we restore the root on exit. */
    g_strlcpy (base_path, htxf->path, sizeof (base_path));

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    retval = folder_recv_all (htxf, base_path, buf, xfer_progress_bump);
    if (retval) {
        goto ret;
    }

    /* Restore the root path BEFORE the completion post_file_update so the
	 * final task-window label reads as the folder, not the last per-file
	 * path folder_recv_all left in htxf->path. */
    g_strlcpy (htxf->path, base_path, sizeof (htxf->path));
    play_sound (FILE_DONE);
    htxf->total_pos = htxf->total_size;
    post_file_update (htxf);

ret:
    (void)retval;
    htxf_io_release (htxf);
    /* Restore the root path on the error paths too. */
    g_strlcpy (htxf->path, base_path, sizeof (htxf->path));
    return NULL;
}

static void *
put_thread (void *arg)
{
    struct htxf_conn *htxf = (struct htxf_conn *)arg;
    int retval;
    guint8 buf[512];

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    retval = file_send_one (htxf, buf, post_file_update);
    if (retval) {
        goto ret;
    }

    play_sound (FILE_DONE);
    post_file_update (htxf);

ret:
    (void)retval;
    htxf_io_release (htxf);

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
 * htxf->path holds the local source root on entry; we snapshot
 * it and rewrite per-file for file_send_one. Restored to the
 * root before cleanup so the tasks-window display reads sanely.
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
    guint8 buf[2048];
    char base_path[MAXPATHLEN];

    g_strlcpy (base_path, htxf->path, sizeof (base_path));

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    retval = folder_send_all (htxf, base_path, buf, post_file_update);
    if (retval) {
        goto ret;
    }

    play_sound (FILE_DONE);
    htxf->total_pos = htxf->total_size;
    post_file_update (htxf);

ret:
    (void)retval;
    htxf_io_release (htxf);
    /* Restore the root path so the tasks-window label stays sensible
	 * post-completion. */
    g_strlcpy (htxf->path, base_path, sizeof (htxf->path));
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

void
xfer_tasks_update (struct htlc_conn *htlc)
{
    int i;

    for (i = 0; i < nxfers; i++) {
        if (xfers[i]->htlc == htlc) {
            gtkhx_session_emit_file_update (gtkhx_session_get_default (),
                                            sess_from_htlc (htlc), xfers[i]);
        }
    }
}

/* Best-effort cancellation of all in-flight transfers at app shutdown.
 * Each htxf has its xfers[] ref dropped here; the worker's ref (and
 * any pending dispatcher refs) keep the htxf alive until the workers
 * actually exit. The process is going down anyway, so leaks of the
 * worker-still-running case don't matter. */
void
xfers_delete_all (void)
{
    int i;

    for (i = 0; i < nxfers; i++) {
        struct htxf_conn *htxf = xfers[i];
        /* Atomic store — the worker reads canceled in htxf_io_read/_write. */
        g_atomic_int_set (&htxf->canceled, TRUE);
        /* Shut the subchannel socket down to wake a worker parked in a
		 * blocking read/write; the htxf_io_read/_write canceled-check
		 * then unwinds it cleanly. The worker runs on tokio's blocking
		 * pool, which can't be force-cancelled — cooperative abort is
		 * the whole mechanism. */
        htxf_io_abort (htxf);
        htxf_unref (htxf); /* drop xfers[] ref */
    }
    nxfers = 0;
}

/* Internal: remove htxf from the xfers[] array and drop the
 * array's reference. Idempotent — if the htxf isn't in the array,
 * does nothing. The actual free happens via the unref only when the
 * last owner (worker, queued dispatchers) drops their refs.
 *
 * Emits "xfer-destroyed" on the GtkhxSession singleton AFTER
 * removal from xfers[] but BEFORE the unref. Subscribers must
 * NULL any cached pointers to this htxf at that point — the next
 * unref (worker exit, dispatcher cleanup) can be the last and free
 * the slab. The signal is synchronous; handlers run with the htxf
 * still alive (the unref we're about to do drops only the xfers[]
 * ref). */
static void
xfer_remove_from_list (struct htxf_conn *htxf)
{
    int i;

    for (i = 0; i < nxfers; i++) {
        if (xfers[i] != htxf) {
            continue;
        }

        if (nxfers > (i + 1)) {
            memcpy (&xfers[i], &xfers[i + 1],
                    (nxfers - (i + 1)) * sizeof (struct htxf_conn *));
        }
        nxfers--;
        gtkhx_session_emit_xfer_destroyed (gtkhx_session_get_default (),
                                           sess_from_htlc (htxf->htlc), htxf);
        htxf_unref (htxf); /* drop the xfers[] ref */
        if (nxfers) {
            xfer_go (xfers[0]);
        }
        return;
    }
}

/* Public: cancel an in-flight transfer.
 *
 * Called from rcv.c when the server sends a cancel / error. Sets
 * htxf->canceled so any pending or future dispatchers skip their work,
 * shuts the subchannel socket down to wake a parked worker, and unlinks
 * from xfers[] (which drops the array's ref). */
void
xfer_delete (struct htxf_conn *htxf)
{
    if (!htxf) {
        return;
    }

    /* Atomic store — the worker reads canceled in htxf_io_read/_write. */
    g_atomic_int_set (&htxf->canceled, TRUE);
    /* Wake a worker parked in a blocking subchannel read/write by
	 * shutting its socket down; the htxf_io_read/_write canceled-check
	 * then turns the resulting error into a clean exit. The worker runs
	 * on tokio's blocking pool, which can't be force-cancelled —
	 * cooperative abort is the whole mechanism. */
    htxf_io_abort (htxf);
    xfer_remove_from_list (htxf);
}

struct htxf_conn *
htxf_with_ref (guint32 ref)
{
    int i;

    for (i = 0; i < nxfers; i++) {
        if (xfers[i]->ref == ref) {
            return xfers[i];
        }
    }

    return 0;
}

