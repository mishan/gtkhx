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
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <gtk/gtk.h>
#include <time.h>
#include "hx.h"
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

int nxfers = 0;
struct htxf_conn **xfers = 0;
static void xfer_remove_from_list (struct htxf_conn *htxf);

/* FFO fork-header / info-block byte math — ported to the hxfiles-xfer
 * Rust crate in Phase F2 (see docs/files-rust-migration-scope.md). The
 * receive worker below keeps its loop + local I/O and calls in here for
 * the fiddly, error-prone parsing (the large-file high32/low32 fork-
 * length split and the variable info-block length). Pure logic, unit-
 * tested headless in the crate. */
extern size_t gtkhx_ffo_info_block_len (guint8 b38, guint8 b39);
extern guint64 gtkhx_ffo_fork_len (const guint8 *marker, size_t marker_len,
                                   int large);

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
            gboolean utf8 = (htxf->htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
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
        gboolean large = (htxf->htlc->caps & HTLC_CAP_LARGE_FILES) != 0;

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
        gboolean utf8 = (htxf->htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
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

    htxf->htlc = &hx_active_session ()->htlc;
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
/* Receive bulk bytes from the HTXF subchannel and write them to a
 * local file fd. The socket side goes through htxf_io_read which
 * is AEAD-aware (HOPE+ChaCha20 wraps the bytes in length-prefixed
 * frames; the wrapper unwraps and serves plaintext) — that's the
 * correctness payoff of the Phase C split. The previous rd_wr
 * took two raw fds and used read(2) directly, which under AEAD
 * would have returned ciphertext bytes; the tests didn't catch
 * this because the file-transfer body never exercised AEAD until
 * this phase.
 *
 * XXX: restore gtk_threads */
static int
rd_wr_recv (int dst_fd, guint64 data_len, struct htxf_conn *htxf)
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

            post_file_update (htxf);
        }
        data_len -= pos;
    }
    return 0;
}

/* Send bulk bytes from a local file fd to the HTXF subchannel.
 * Socket-side write goes through htxf_io_write (AEAD-aware). */
static int
rd_wr_send (int src_fd, guint64 data_len, struct htxf_conn *htxf)
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
        post_file_update (htxf);
        data_len -= (guint64)len;
    }
    return 0;
}

static int
preview_get (guint32 data_len, struct htxf_conn *htxf, hx_preview *p)
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
        post_file_update (htxf);
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
 *      bytes 38/39: `(buf[38] ? 0x100 : 0) + buf[39]`, plus
 *      16 bytes of DATA-fork marker at the tail).
 *   3. Data-fork payload (length = u32 BE at offset pos-4 of
 *      the info block).
 *   4. Optional MACR rsrc-fork marker (16 bytes) + rsrc payload,
 *      gated on whether tot_len has caught up to file_budget.
 *
 * Updates htxf->total_pos as bytes arrive so the tasks-window
 * progress bar advances. Does NOT play the completion sound,
 * post a final file_update, or close the socket — those are
 * caller responsibilities (the meaning differs for solo file
 * vs. final file in a folder).
 *
 * Returns 0 on success, errno-like positive code on failure. */
static int
file_recv_one (struct htxf_conn *htxf, guint64 file_budget, guint8 *buf)
{
    guint32 pos, len;
    guint64 fork_len = 0;
    guint64 tot_len;
    int f, r, retval = 0;
    guint8 typecrea[8];
    struct hfsinfo fi;
    hx_preview *p = NULL;

    len = 40;
    pos = 0;
    while (len) {
        if ((r = htxf_io_read (htxf, &(buf[pos]), len)) < 1) {
            return errno ? errno : EIO;
        }
        pos += r;
        len -= r;
        htxf->total_pos += r;
        post_file_update (htxf);
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
        post_file_update (htxf);
    }
    memcpy (typecrea, &buf[4], 8);
    memset (&fi, 0, sizeof (fi));
    fi.comlen = buf[73 + buf[71]];
    memcpy (fi.type, "HTftHTLC", 8);
    memcpy (fi.comment, &buf[74 + buf[71]], fi.comlen);
    *((guint32 *)(&buf[56])) = hfs_m_to_htime (*((guint32 *)(&buf[56])));
    *((guint32 *)(&buf[64])) = hfs_m_to_htime (*((guint32 *)(&buf[64])));
    memcpy (&fi.create_time, &buf[56], 4);
    memcpy (&fi.modify_time, &buf[64], 4);
    if (!htxf->opt.preview) {
        hfsinfo_write (htxf->path, &fi);
    }

    /* DATA fork length. The 16-byte fork header lives at buf[pos-16
	 * .. pos]: "DATA" + Compression(4) + Reserved(4) + DataSize(4).
	 *
	 * Legacy mode: DataSize at pos-4 is the 32-bit fork length;
	 * Compression must be zero.
	 *
	 * Large-file mode (htxf->opt.large): the same 16-byte header is
	 * reinterpreted — Compression (at pos-12) holds the HIGH 32
	 * bits, DataSize (at pos-4) holds the LOW 32 bits. Combine into
	 * the full 64-bit length. Source:
	 * fogWraith/Hotline Docs/Protocol/Capabilities-Large-File.md
	 * section "Flattened File Object Fork Headers". */
    fork_len = gtkhx_ffo_fork_len (&buf[pos - 16], 16, htxf->opt.large);
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
        retval = rd_wr_recv (f, fork_len, htxf);
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
                              htxf, p);
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
                    post_file_update (htxf);
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
        post_file_update (htxf);
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
    retval = rd_wr_recv (f, fork_len, htxf);
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

static void *
get_thread (void *arg)
{
    struct htxf_conn *htxf = (struct htxf_conn *)arg;
    int retval;
    guint8 buf[1024];

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    retval = file_recv_one (htxf, htxf->total_size, buf);
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

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    /* Snapshot the destination root. file_recv_one rewrites
	 * htxf->path per-file; we restore the root on exit. */
    g_strlcpy (base_path, htxf->path, sizeof (base_path));

    if (g_mkdir_with_parents (base_path, 0755) < 0 && errno != EEXIST) {
        retval = errno;
        goto ret;
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
            retval = errno ? errno : EIO;
            goto ret;
        }

        n = htxf_io_read (htxf, &nfi, sizeof (nfi));
        if (n != (ssize_t)sizeof (nfi)) {
            /* Clean end-of-stream when n == 0 — server has run
			 * out of files and closed the socket. */
            if (n == 0) {
                retval = 0;
                break;
            }
            retval = errno ? errno : EIO;
            goto ret;
        }
        nfi.len = ntohs (nfi.len);
        nfi.type = ntohs (nfi.type);
        nfi.pathcount = ntohs (nfi.pathcount);

        /* Read pathcount name components and join with '/' into
		 * the per-entry relative path. */
        for (i = 0; i < nfi.pathcount; i++) {
            guint8 ph[3];
            guint8 nlen;
            char name[256];
            if (htxf_io_read (htxf, ph, 3) != 3) {
                retval = errno ? errno : EIO;
                goto ret;
            }
            nlen = ph[2];
            /* nlen is guint8 (max 255); name is 256 bytes — the
			 * read can never overflow. The original explicit guard
			 * triggered a `comparison always false` warning. */
            if (nlen && htxf_io_read (htxf, name, nlen) != nlen) {
                retval = errno ? errno : EIO;
                goto ret;
            }
            name[nlen] = 0;
            /* Defence in depth — refuse `..` and embedded `/`
			 * which would escape base_path. */
            if (!strcmp (name, "..") || memchr (name, '/', nlen)) {
                retval = EINVAL;
                goto ret;
            }
            if (rel_len + (rel_len ? 1 : 0) + nlen + 1 >= sizeof (rel_path)) {
                retval = ENAMETOOLONG;
                goto ret;
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
            retval = EINVAL;
            goto ret;
        }
        if (snprintf (htxf->path, sizeof (htxf->path), "%s/%s", base_path,
                      rel_path)
            >= (int)sizeof (htxf->path)) {
            retval = ENAMETOOLONG;
            goto ret;
        }

        if (nfi.type == 1) {
            /* Folder marker — mkdir, no payload. */
            if (g_mkdir_with_parents (htxf->path, 0755) < 0
                && errno != EEXIST) {
                retval = errno;
                goto ret;
            }
            continue;
        }

        /* File entry — request fresh. Resume support is a
		 * follow-up; FILE_SEND with data_pos/rsrc_pos zeroed
		 * tells the server to send the whole file. */
        cmd_n = htons (1); /* FILE_SEND */
        if (htxf_io_write (htxf, &cmd_n, 2) != 2) {
            retval = errno ? errno : EIO;
            goto ret;
        }

        if (htxf_io_read (htxf, &file_size, 4) != 4) {
            retval = errno ? errno : EIO;
            goto ret;
        }
        file_size = ntohl (file_size);

        htxf->data_pos = 0;
        htxf->rsrc_pos = 0;

        retval = file_recv_one (htxf, file_size, buf);
        if (retval) {
            goto ret;
        }
    }

    /* Restore the root path BEFORE the completion post_file_update
	 * so the final task-window label and the xfer-done notification
	 * both read as the folder, not as whatever per-file path
	 * file_recv_one left in htxf->path on its way out of the
	 * last iteration. The ret: label below also restores it (for
	 * the error paths that jump straight there). */
    g_strlcpy (htxf->path, base_path, sizeof (htxf->path));
    play_sound (FILE_DONE);
    htxf->total_pos = htxf->total_size;
    post_file_update (htxf);

ret:
    (void)retval;
    htxf_io_release (htxf);

    /* Restore the root path on error paths that goto'd here mid-
	 * loop. The success path above already restored before
	 * post_file_update; this is the catch-all. */
    g_strlcpy (htxf->path, base_path, sizeof (htxf->path));

    return NULL;
}

/* Send a single file out over an HTXF subchannel from
 * htxf->path. Mirror of file_recv_one — same wire framing, just
 * the sending end. The header layout is:
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
 *   - folder_put_thread: writes the per-file u32 size header
 *     first (over the HTXF socket), then calls this. Hotline's
 *     folder framing puts the per-file size up front so the
 *     receiver knows the budget.
 *
 * htxf->data_size / data_pos / rsrc_size / rsrc_pos must be set
 * to the actual local file values before calling. Caller closes
 * the socket and plays the completion sound.
 *
 * Returns 0 on success, errno-like positive code on failure. */
static int
file_send_one (struct htxf_conn *htxf, guint8 *buf)
{
    int f, retval;
    struct hfsinfo fi;

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
        retval = rd_wr_send (f, htxf->data_size, htxf);
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
    /* DATA fork header. Legacy mode: 16 bytes of "DATA" + zeros +
	 * 32-bit length. Large-file mode (folder per-file in large-
	 * file mode): split encoding — high 32 bits in the Compression
	 * slot at offset 4-7, low 32 bits in DataSize at offset 12-15. */
    memcpy (&buf[117 + fi.comlen], "DATA\0\0\0\0\0\0\0\0", 12);
    {
        guint64 fork_len = htxf->data_size - htxf->data_pos;
        guint32 lo = (guint32)(fork_len & 0xFFFFFFFFu);
        HN32 (&buf[129 + fi.comlen], &lo);
        if (htxf->opt.large) {
            guint32 hi = (guint32)(fork_len >> 32);
            HN32 (&buf[121 + fi.comlen], &hi);
        }
    }
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
    retval = rd_wr_send (f, htxf->data_size, htxf);
    if (retval) {
        close (f);
        return retval;
    }
    close (f);

put_rsrc:
    /* MACR fork header — same legacy / split-encoding choice as
	 * the DATA fork above. */
    memcpy (buf, "MACR\0\0\0\0\0\0\0\0", 12);
    {
        guint32 lo = (guint32)(htxf->rsrc_size & 0xFFFFFFFFu);
        HN32 (&buf[12], &lo);
        if (htxf->opt.large) {
            guint32 hi = (guint32)(htxf->rsrc_size >> 32);
            HN32 (&buf[4], &hi);
        }
    }
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
    retval = rd_wr_send (f, htxf->rsrc_size, htxf);
    if (retval) {
        close (f);
        return retval;
    }
    close (f);
    return 0;
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

    retval = file_send_one (htxf, buf);
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

struct hx_put_entry {
    int type;                 /* 1 = folder marker, 0 = file leaf */
    char *full_local_path;    /* on-disk path; used only for files */
    GPtrArray *components;    /* (char *) path components from root */
    guint64 data_size;        /* for files */
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
            g_ptr_array_add (e->components,
                             g_strdup (g_ptr_array_index (prefix_components,
                                                          i)));
        }
        g_ptr_array_add (e->components, g_strdup (n));

        if (S_ISDIR (sb.st_mode)) {
            e->type = 1;
            e->full_local_path = g_strdup (full);
            g_ptr_array_add (entries, e);
            /* DFS pre-order — recurse with this dir prepended to
			 * the prefix. */
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

static void *
folder_put_thread (void *arg)
{
    struct htxf_conn *htxf = (struct htxf_conn *)arg;
    int retval = 0;
    guint8 buf[2048];
    char base_path[MAXPATHLEN];
    GPtrArray *entries = NULL;
    GPtrArray *initial_comps = NULL;

    if (!htxf_connect (htxf)) {
        retval = -1;
        goto ret;
    }

    g_strlcpy (base_path, htxf->path, sizeof (base_path));

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

        /* Set up htxf for file_send_one. data_size / rsrc_size
		 * come from the local file. */
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

        retval = file_send_one (htxf, buf);
        if (retval) {
            goto cleanup;
        }
    }

    play_sound (FILE_DONE);
    htxf->total_pos = htxf->total_size;
    post_file_update (htxf);

cleanup:
    if (entries) {
        g_ptr_array_unref (entries);
    }

ret:
    (void)retval;
    htxf_io_release (htxf);

    /* Restore the root path so the tasks-window label stays
	 * sensible post-completion. */
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

void
hlclient_reap_pid (pid_t pid, int status)
{
}
