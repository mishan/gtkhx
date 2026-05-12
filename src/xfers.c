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
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <gtk/gtk.h>
#include <time.h>
#include <netinet/in.h>
#include "hx.h"
#include "gtkhx_session.h"
#include "hfs.h"
#include "network.h"
#include "rcv.h"
#include "chat.h"
#include "tasks.h"
#include "sound.h"
#include "files.h"
#include "preview.h"
#include "gtkthreads.h"
#include "xfers.h"

int nxfers = 0;
struct htxf_conn **xfers = 0;
void xfer_delete (struct htxf_conn *htxf);
static void xfer_remove_from_list (struct htxf_conn *htxf);

/*
 * Reference counting and the worker → main marshal helpers.
 *
 * See the lifecycle comment over the refcount field in
 * struct htxf_conn (protocol.h) for the ownership model. In short:
 *
 *   - xfers[] holds 1 ref per htxf; dropped by xfer_remove_from_list
 *     when xfer_delete (server-cancel from rcv.c, err_fd from
 *     xfer_ready_write) or cleanup_dispatch (worker normal exit)
 *     unlinks the htxf.
 *   - The worker thread holds 1 ref taken in xfer_ready_write
 *     before pthread_create; dropped by cleanup_dispatch on its
 *     behalf when the worker queues post_xfer_cleanup at exit.
 *   - Each pending post_file_update / post_xfer_cleanup idle holds
 *     1 ref while it's queued; dropped by its dispatcher.
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
	if (htxf)
		g_atomic_int_inc (&htxf->refcount);
	return htxf;
}

static void
htxf_unref (struct htxf_conn *htxf)
{
	if (!htxf)
		return;
	if (!g_atomic_int_dec_and_test (&htxf->refcount))
		return;
#ifdef USE_IPV6
	if (htxf->listen_addr)
		freeaddrinfo (htxf->listen_addr);
#endif
	g_free (htxf);
}

struct fu_job {
	struct htxf_conn *htxf;
};
static gboolean
fu_dispatch (gpointer data)
{
	struct fu_job *j = data;
	if (!j->htxf->canceled)
		gtkhx_session_emit_file_update (gtkhx_session_get_default (), &the_session, j->htxf);
	htxf_unref (j->htxf);
	g_free (j);
	return G_SOURCE_REMOVE;
}
static void
post_file_update (struct htxf_conn *htxf)
{
	struct fu_job *j = g_new0 (struct fu_job, 1);
	j->htxf = htxf_ref (htxf);
	gtkhx_post_to_main (fu_dispatch, j);
}

struct cleanup_job {
	struct htxf_conn *htxf;
};
static gboolean
cleanup_dispatch (gpointer data)
{
	struct cleanup_job *j = data;
	j->htxf->tid = 0;
	/* Unlink from xfers[] if the server didn't already cancel us
	 * out of it; xfer_remove_from_list is a no-op on a not-found
	 * pointer. */
	xfer_remove_from_list (j->htxf);
	/* Drop the worker thread's ref. */
	htxf_unref (j->htxf);
	g_free (j);
	return G_SOURCE_REMOVE;
}
static void
post_xfer_cleanup (struct htxf_conn *htxf)
{
	struct cleanup_job *j = g_new0 (struct cleanup_job, 1);
	/* The worker thread's ref is handed off to the cleanup job
	 * directly — no additional ref taken here. cleanup_dispatch
	 * unrefs on the worker's behalf. */
	j->htxf = htxf;
	gtkhx_post_to_main (cleanup_dispatch, j);
}

static void ignore_signals (sigset_t *oldset)
{
	sigset_t set;

	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, oldset);
}

static void unignore_signals (sigset_t *oldset)
{
	sigprocmask(SIG_SETMASK, oldset, 0);
}

/* Does either fork (data or resource) of the local path exist? */
static int
local_path_exists (const char *path)
{
	struct stat sb;
	if (stat (path, &sb) == 0)
		return 1;
	if (resource_len (path) > 0)
		return 1;
	return 0;
}

/* If the local path collides with an existing file, mutate it in
 * place to a non-colliding variant by inserting " (N)" before the
 * last extension:
 *
 *   /dl/foo.txt        with foo.txt present  →  /dl/foo (1).txt
 *   /dl/archive.tar.gz with that present     →  /dl/archive.tar (1).gz
 *   /dl/README         with that present     →  /dl/README (1)
 *
 * N counts up from 1. A leading dot in the basename (".bashrc") is
 * treated as part of the name, not an extension. After ~10000 tries
 * we give up and leave path at its last attempt — the subsequent
 * open() will overwrite at that name, which is the same behavior
 * as before this helper existed; the user has bigger problems if
 * they have ten thousand "foo (N).txt" copies. */
static void
uniquify_local_path (char *path, size_t cap)
{
	const char *base, *dot;
	char prefix[MAXPATHLEN];
	char suffix[MAXPATHLEN];
	size_t pre_len;
	int n;

	if (!local_path_exists (path))
		return;

	base = strrchr (path, '/');
	base = base ? base + 1 : path;
	dot = strrchr (base, '.');
	if (dot == base)            /* leading-dot basename, no extension */
		dot = NULL;

	if (dot) {
		pre_len = dot - path;
		if (pre_len >= sizeof prefix)
			pre_len = sizeof prefix - 1;
		memcpy (prefix, path, pre_len);
		prefix[pre_len] = '\0';
		g_strlcpy (suffix, dot, sizeof suffix);
	} else {
		g_strlcpy (prefix, path, sizeof prefix);
		suffix[0] = '\0';
	}

	for (n = 1; n < 10000; n++) {
		/* The precision specifiers cap each component at slightly under
		 * half of MAXPATHLEN so GCC can prove the format fits in path's
		 * cap bytes. snprintf would truncate safely either way; the
		 * specifiers exist only to satisfy the static analysis. */
		snprintf (path, cap, "%.*s (%d)%.*s",
		          MAXPATHLEN / 2 - 16, prefix, n,
		          MAXPATHLEN / 2 - 16, suffix);
		if (!local_path_exists (path))
			return;
	}
}

void xfer_go (struct htxf_conn *htxf)
{
	char *rfile;
	guint16 hldirlen;
	guint8 *hldir;
	guint8 rflt[74];
	int resuming = 0;

	if (htxf->gone)
		return;

	htxf->gone = 1;

	if(htxf->type == XFER_GET) {
/*		hx_htlc.nr_gets++; */
	}
	else if(htxf->type == XFER_PUT) {
/*		hx_htlc.nr_puts++; */
	}
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
				guint32 local_data = (guint32) sb.st_size;
				if (htxf->srv_data_size > 0
				    && local_data < htxf->srv_data_size) {
					htxf->data_pos = local_data;
					htxf->rsrc_pos = resource_len (htxf->path);
					resuming = 1;
				} else {
					uniquify_local_path (htxf->path,
					                     sizeof htxf->path);
				}
			}
		}

		if (resuming) {
			memcpy (rflt, "\
                          RFLT\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
                          \0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\2\
                          DATA\0\0\0\0\0\0\0\0\0\0\0\0\
                          MACR\0\0\0\0\0\0\0\0\0\0\0\0", 74);
			S32HTON (htxf->data_pos, &rflt[46]);
			S32HTON (htxf->rsrc_pos, &rflt[62]);
		}

		rfile = dirchar_basename(htxf->remotepath);
		task_new(&the_session.htlc, RCV_TASK_FN(rcv_task_file_get), htxf, 0, "xfer_go");
		if (rfile != htxf->remotepath) {
			hldir = path_to_hldir(htxf->remotepath, &hldirlen, 1);
			hlwrite(&the_session.htlc, HTLC_HDR_FILE_GET, 0,
					resuming ? 3 : 2,
					HTLC_DATA_FILE_NAME, strlen(rfile), rfile,
					HTLC_DATA_DIR, hldirlen, hldir,
					HTLC_DATA_RFLT, 74, rflt);
			g_free(hldir);
		} else {
			hlwrite(&the_session.htlc, HTLC_HDR_FILE_GET, 0,
					resuming ? 2 : 1,
					HTLC_DATA_FILE_NAME, strlen(rfile), rfile,
					HTLC_DATA_RFLT, 74, rflt);
		}
	}
	else {
		guint32 size = htonl(htxf->total_size);

		rfile = basename(htxf->path);
		hldir = path_to_hldir(htxf->remotepath, &hldirlen, 1);
		task_new(&the_session.htlc, RCV_TASK_FN(rcv_task_file_put), htxf, 0, "xfer_go");
		if (exists_remote(htxf->remotepath)) {
			hlwrite(&the_session.htlc, HTLC_HDR_FILE_PUT, 0, 4,
					HTLC_DATA_FILE_NAME, strlen(rfile), rfile,
					HTLC_DATA_DIR, hldirlen, hldir,
					HTLC_DATA_FILE_PREVIEW, 2, "\0\1",
					HTLC_DATA_HTXF_SIZE, 4, &size);
		} else {
			hlwrite(&the_session.htlc, HTLC_HDR_FILE_PUT, 0, 3,
					HTLC_DATA_FILE_NAME, strlen(rfile), rfile,
					HTLC_DATA_DIR, hldirlen, hldir,
					HTLC_DATA_HTXF_SIZE, 4, &size);
		}
		g_free(hldir);
	}
}

int xfer_go_timer (void *__arg)
{
	xfer_go((struct htxf_conn *)__arg);
	return 0;
}

struct htxf_conn *xfer_new (const char *path, const char *remotepath,
							guint16 type, int preview,
							guint32 srv_data_size)
{
	struct htxf_conn *htxf;

	htxf = g_malloc0(sizeof(struct htxf_conn));
	strcpy(htxf->remotepath, remotepath);
	strcpy(htxf->path, path);
	htxf->type = type;
	htxf->queue = -1;
	/* refcount = 1 represents the xfers[] array's ownership. The
	 * worker thread will take its own ref before pthread_create
	 * (in xfer_ready_write). */
	htxf->refcount = 1;
	htxf->canceled = FALSE;
	/* opt.preview and srv_data_size MUST be set before xfer_go
	 * runs below — xfer_go gates its resume / rename decision on
	 * both. Setting these via the returned htxf pointer after this
	 * function returns is too late: when nxfers == 1 (or queueing
	 * is off) we call xfer_go inline, and the wire request goes
	 * out before the caller could flip them. */
	htxf->opt.preview = preview ? 1 : 0;
	htxf->srv_data_size = srv_data_size;

	xfers = g_realloc(xfers, (nxfers + 1) * sizeof(struct htxf_conn *));
	xfers[nxfers] = htxf;
	nxfers++;

	htxf->htlc = &the_session.htlc;
	htxf->total_pos = 0;
	htxf->total_size = 1;
	gtkhx_session_emit_file_update (gtkhx_session_get_default (), &the_session, htxf);

	if(nxfers == 1 || !gtkhx_prefs.queuedl) {
		xfer_go(htxf);
	}

	return htxf;
}

void xfer_up(int num)
{
	struct htxf_conn *tmp;

	tmp = xfers[num-1];
	xfers[num-1] = xfers[num];
	xfers[num] = tmp;
}

int xfer_down(int num)
{
	struct htxf_conn *tmp;


	if(nxfers-1 == num) {
		return 1;
	}


	tmp = xfers[num+1];
	xfers[num+1] = xfers[num];
	xfers[num] = tmp;

	return 0;
}

int xfer_num (struct htxf_conn *htxf)
{
	int i;


	for(i = 0; i < nxfers; i++) {
		if(xfers[i] == htxf) {
			return i;
		}
	}

	return -1;
}
/* XXX: restore gtk_threads */
static int rd_wr (int rd_fd, int wr_fd, guint32 data_len, 
				  struct htxf_conn *htxf)
{
	int r, pos, len;
	guint8 *buf;
	size_t bufsiz;


	bufsiz = 0xf000;
	buf = g_malloc(bufsiz);
	if (!buf)
		return 111;
	while (data_len) {
		if ((len = read(rd_fd, buf, (bufsiz < data_len) ? bufsiz : data_len)) < 1)
			return len ? errno : EIO;
		pos = 0;
		while (len) {
			if ((r = write(wr_fd, &(buf[pos]), len)) < 1)
				return errno;
			pos += r;
			len -= r;
			htxf->total_pos += r;

			post_file_update(htxf);
		}
		data_len -= pos;
	}
	g_free(buf);

	return 0;
}

static int preview_get (int rd_fd, guint32 data_len, struct htxf_conn *htxf,
						struct hx_preview *p)
{
	int len;
	guint8 *buf;
	size_t bufsiz;


	bufsiz = 0xf000;
	buf = g_malloc(bufsiz);
	if (!buf)
		return 111;
	while (data_len) {
		if ((len = read(rd_fd, buf, (bufsiz < data_len) ? bufsiz : data_len)) < 1)
			return len ? errno : EIO;
		/* XXX: we need some kind of plugin schematic where a plugin registers
		   itself for a given creator/type and the preview function looks for
		   a plugin to match the file it is about to download and loads a 
		   session with a plugin, if such a plugin exists, passes it on to here
		   so that here we can pass the data into that session, otherwise
		   we tell the user that no plugin exists for such data. we must also
		   take into consideration that the person may be viewing some large
		   file. do we want to keep this in memory or save to /tmp? */

		/* XXX: Here is where we should output to some preview widget */
		/*			g_print("%.*s", len, &(buf[pos])); */

		/* p->output is hx_preview_text_output, which already does
		 * its own g_idle_add to marshal the gtk_text_buffer_insert
		 * to the main thread (see preview.c). It is safe to call
		 * directly from the worker — no GTK lock needed. */
		p->output(p, (char *) buf, len);
		htxf->total_pos += len;
		post_file_update(htxf);
		data_len -= len;
	}
	g_free(buf);

	return 0;
}

static void *get_thread (void *__arg)
{
	struct htxf_conn *htxf = (struct htxf_conn *)__arg;
	guint32 pos, len, tot_len;
	int s, f, r, retval = 0;
	guint8 typecrea[8], buf[1024];
	struct hfsinfo fi;
	struct hx_preview *p = NULL;

	s = htxf_connect(htxf);
	if (s < 0) {
		retval = s;
		goto ret;
	}

	len = 40;
	pos = 0;
	while (len) {
		if ((r = read(s, &(buf[pos]), len)) < 1) {
			retval = errno;
			goto ret;
		}
		pos += r;
		len -= r;
		htxf->total_pos += r;
		post_file_update(htxf);
	}
	pos = 0;
	len = (buf[38] ? 0x100 : 0) + buf[39];
	len += 16;
	tot_len = 40 + len;
	while (len) {
		if ((r = read(s, &(buf[pos]), len)) < 1) {
			retval = errno;
			goto ret;
		}
		pos += r;
		len -= r;
		htxf->total_pos += r;

		post_file_update(htxf);
	}
	memcpy(typecrea, &buf[4], 8);
	memset(&fi, 0, sizeof(fi));
	fi.comlen = buf[73 + buf[71]];
	memcpy(fi.type, "HTftHTLC", 8);
	memcpy(fi.comment, &buf[74 + buf[71]], fi.comlen);
	*((guint32 *)(&buf[56])) = hfs_m_to_htime(*((guint32 *)(&buf[56])));
	*((guint32 *)(&buf[64])) = hfs_m_to_htime(*((guint32 *)(&buf[64])));
	memcpy(&fi.create_time, &buf[56], 4);
	memcpy(&fi.modify_time, &buf[64], 4);
	if(!htxf->opt.preview)
		hfsinfo_write(htxf->path, &fi);

	HN32(&len, &buf[pos - 4]);
	tot_len += len;
	if (!len)
		goto get_rsrc;
	if(!htxf->opt.preview) {
		if ((f = open(htxf->path, O_CREAT|O_WRONLY, S_IRUSR|S_IWUSR)) < 0) {
			retval = errno;
			goto ret;
		}

		if (htxf->data_pos)
			lseek(f, htxf->data_pos, SEEK_SET);
		retval = rd_wr(s, f, len, htxf);
		fsync(f);
		close(f);
	}
	else {
		/* The preview window is constructed on the main thread by
		 * rcv_task_file_get and stashed here as a struct hx_preview *;
		 * the worker just streams bytes through it. Constructing
		 * GtkWindow + AdwHeaderBar and calling gtk_window_present
		 * from a worker thread caused intermittent lockups — Wayland
		 * compositor round-trips during window mapping don't play
		 * nicely from non-main threads. */
		p = (struct hx_preview *) htxf->preview;
		if (!p) {
			goto ret;
		}
		retval = preview_get(s, len, htxf, p);
	}
	if(retval)
		goto ret;
get_rsrc:
	if(htxf->opt.preview) {
		goto done;
	}
	if (tot_len >= htxf->total_size)
		goto done;
	pos = 0;
	len = 16;
	while (len) {
		if ((r = read(s, &(buf[pos]), len)) < 1) {
			retval = errno;
			goto ret;
		}
		pos += r;
		len -= r;
		htxf->total_pos += r;

		post_file_update(htxf);
	}
	HN32(&len, &buf[12]);
	if (!len)
		goto done;
	if ((f = resource_open(htxf->path, O_CREAT|O_WRONLY, S_IRUSR|S_IWUSR)) < 0) {
		retval = errno;
		goto ret;
	}
	if (htxf->rsrc_pos)
		lseek(f, htxf->rsrc_pos, SEEK_SET);
	retval = rd_wr(s, f, len, htxf);
	if (retval)
		goto ret;
	close(f);

done:
	memcpy(fi.type, typecrea, 8);
	if(!htxf->opt.preview)
		hfsinfo_write(htxf->path, &fi);
	play_sound(FILE_DONE);
	htxf->total_pos = htxf->total_size;
	post_file_update(htxf);

ret:
	close(s);

	/* Cleanup is marshaled to the main thread so it runs AFTER
	 * every file_update idle posted above — GMainContext FIFO
	 * ordering keeps htxf alive for every pending dispatcher. */
	post_xfer_cleanup(htxf);
	return NULL;
}

static void *put_thread (void *__arg)
{
	struct htxf_conn *htxf = (struct htxf_conn *)__arg;
	int s, f, retval = 0;
	guint8 buf[512];
	struct hfsinfo fi;

	s = htxf_connect(htxf);
	if (s < 0) {
		retval = s;
		goto ret;
	}

	memcpy(buf, "\
FILP\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
\2INFO\0\0\0\0\0\0\0\0\0\0\0^AMAC\
TYPECREA\
\0\0\0\0\0\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\
\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
\7\160\0\0\0\0\0\0\7\160\0\0\0\0\0\0\0\0\0\3hxd", 115);
	hfsinfo_read(htxf->path, &fi);
	if (htxf->rsrc_size - htxf->rsrc_pos)
		buf[23] = 3;
	if (65 + fi.comlen + 12 > 0xff)
		buf[38] = 1;
	buf[39] = 65 + fi.comlen + 12;
	type_creator(&buf[44], htxf->path);
	*((guint32 *)(&buf[96])) = hfs_h_to_mtime(*((guint32 *)(&fi.create_time)));
	*((guint32 *)(&buf[104])) = hfs_h_to_mtime(*((guint32 *)(&fi.modify_time)));
	buf[116] = fi.comlen;
	memcpy(&buf[117], fi.comment, fi.comlen);
	memcpy(&buf[117 + fi.comlen], "DATA\0\0\0\0\0\0\0\0", 12);
	{
		guint32 tmp=htxf->data_size-htxf->data_pos;
		HN32(&buf[129+fi.comlen], &tmp);
	}
	if (write(s, buf, 133 + fi.comlen) != (ssize_t)(133 + fi.comlen)) {
		retval = errno;
		goto ret;
	}
	htxf->total_pos += 133 + fi.comlen;
	if (!(htxf->data_size - htxf->data_pos))
		goto put_rsrc;
	if ((f = open(htxf->path, O_RDONLY)) < 0) {
		retval = errno;
		goto ret;
	}
	if (htxf->data_pos)
		lseek(f, htxf->data_pos, SEEK_SET);
	retval = rd_wr(f, s, htxf->data_size, htxf);
	if (retval) {
		goto ret;
	}
	close(f);

put_rsrc:
	memcpy(buf, "MACR\0\0\0\0\0\0\0\0", 12);
	HN32(&buf[12], &htxf->rsrc_size);
	if (write(s, buf, 16) != 16) {
		retval = 0;
		goto ret;
	}
	htxf->total_pos += 16;
	if (!(htxf->rsrc_size - htxf->rsrc_pos))
		goto done; 

	if ((f = resource_open(htxf->path, O_RDONLY, 0)) < 0) {
		retval = errno;
		goto ret;
	}
	if (htxf->rsrc_pos)
		lseek(f, htxf->rsrc_pos, SEEK_SET);
	retval = rd_wr(f, s, htxf->rsrc_size, htxf);
	if (retval)
		goto ret;
	close(f);

done:
	play_sound(FILE_DONE);
	post_file_update(htxf);

ret:
	close(s);

	/* See get_thread for the cleanup-via-marshal rationale. */
	post_xfer_cleanup(htxf);

	return NULL;
}


void xfer_ready_write (struct htxf_conn *htxf)
{
	sigset_t oldset;
	struct sigaction act, tstpact, contact;
	pthread_t tid;
	int err;

	ignore_signals(&oldset);
	act.sa_flags = 0;
	act.sa_handler = SIG_DFL;
	sigfillset(&act.sa_mask);
 	sigaction(SIGTSTP, &act, &tstpact);
	sigaction(SIGCONT, &act, &contact);

	/* Take the worker thread's reference BEFORE pthread_create so
	 * the htxf can't be freed mid-spawn if some other path drops
	 * the xfers[] ref between here and the worker's first
	 * htxf_ref call. cleanup_dispatch drops this ref on the
	 * worker's behalf at exit. */
	htxf_ref (htxf);

	err = pthread_create(&tid, 0, ((htxf->type == XFER_GET) ?
								   get_thread : put_thread), htxf);

	sigaction(SIGTSTP, &tstpact, 0);
	sigaction(SIGCONT, &contact, 0);
	unignore_signals(&oldset);

	if (err) {
		/* pthread_create failed — we'll never get a
		 * cleanup_dispatch to drop the ref, so drop it here. */
		htxf_unref (htxf);
		hx_printf_prefix(&the_session.htlc, 0, INFOPREFIX, "xfer: pthread_create: %s\n", strerror(err));
		goto err_fd;
	}
	htxf->tid = tid;
//	pthread_detach(tid);

	return;

err_fd:
	xfer_delete(htxf);
}

void xfer_tasks_update (struct htlc_conn *htlc)
{
	int i;

	for (i = 0; i < nxfers; i++) {
		if (xfers[i]->htlc == htlc)
			gtkhx_session_emit_file_update (gtkhx_session_get_default (), &the_session, xfers[i]);
	}
}

/* Best-effort cancellation of all in-flight transfers at app shutdown.
 * Each htxf has its xfers[] ref dropped here; the worker's ref (and
 * any pending dispatcher refs) keep the htxf alive until the workers
 * actually exit. The process is going down anyway, so leaks of the
 * worker-still-running case don't matter. */
void xfers_delete_all (void)
{
	int i;

	for (i = 0; i < nxfers; i++) {
		struct htxf_conn *htxf = xfers[i];
		htxf->canceled = TRUE;
		if (htxf->tid)
			pthread_cancel (htxf->tid);
		htxf_unref (htxf);   /* drop xfers[] ref */
	}
	nxfers = 0;
}

/* Internal: remove htxf from the xfers[] array and drop the
 * array's reference. Idempotent — if the htxf isn't in the array,
 * does nothing. The actual free happens via the unref only when the
 * last owner (worker, queued dispatchers) drops their refs. */
static void
xfer_remove_from_list (struct htxf_conn *htxf)
{
	int i;

	for (i = 0; i < nxfers; i++) {
		if (xfers[i] != htxf)
			continue;

		if (nxfers > (i + 1)) {
			memcpy (&xfers[i], &xfers[i + 1],
			        (nxfers - (i + 1)) * sizeof (struct htxf_conn *));
		}
		nxfers--;
		htxf_unref (htxf);   /* drop the xfers[] ref */
		if (nxfers)
			xfer_go (xfers[0]);
		return;
	}
}

/* Public: cancel an in-flight transfer.
 *
 * Called from rcv.c when the server sends a cancel / error and from
 * xfer_ready_write's err_fd path when pthread_create fails. Sets
 * htxf->canceled so any pending or future dispatchers skip their
 * work, kicks the worker thread (best-effort — pthread_cancel is
 * async), and unlinks from xfers[] (which drops the array's ref). */
void
xfer_delete (struct htxf_conn *htxf)
{
	if (!htxf)
		return;

	htxf->canceled = TRUE;
	if (htxf->tid)
		pthread_cancel (htxf->tid);
	xfer_remove_from_list (htxf);
}


struct htxf_conn *htxf_with_ref(guint32 ref)
{
	int i;

	for(i = 0; i < nxfers; i++) {
		if(xfers[i]->ref == ref) {
			return xfers[i];
		}
	}

	return 0;
}

void
hlclient_reap_pid (pid_t pid, int status)
{

}
