/*
 * Copyright (C) 2000-2002 Misha Nasledov <misha@nasledov.com>
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
#include "hfs.h"
#include "network.h"
#include "rcv.h"
#include "chat.h"
#include "tasks.h"
#include "sound.h"
#include "files.h"
#include "preview.h"
#include "gtkthreads.h"

int nxfers = 0;
struct htxf_conn **xfers = 0;
void xfer_delete (struct htxf_conn *htxf);

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

void xfer_go (struct htxf_conn *htxf)
{
	char *rfile;
	guint16 hldirlen;
	guint8 *hldir;
	guint32 data_size = 0, rsrc_size = 0;
	guint8 rflt[74];
	struct stat sb;
	
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
		if (!stat(htxf->path, &sb))
			data_size = sb.st_size;
		rsrc_size = resource_len(htxf->path);
		
		rfile = dirchar_basename(htxf->remotepath);
		if (data_size || rsrc_size) {
			memcpy(rflt, "\
                          RFLT\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
                          \0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\2\
                          DATA\0\0\0\0\0\0\0\0\0\0\0\0\
                          MACR\0\0\0\0\0\0\0\0\0\0\0\0", 74);
			/*   XXX: Fix this, aaron */
			/*	 HN32(&rflt[46], data_size);
				 HN32(&rflt[62], rsrc_size); */
			S32HTON(data_size, &rflt[46]);
			S32HTON(rsrc_size, &rflt[62]);
			htxf->data_pos = data_size;
			htxf->rsrc_pos = rsrc_size;
		}
		task_new(&sessions[0].htlc, rcv_task_file_get, htxf, 0, "xfer_go");
		if (rfile != htxf->remotepath) {
			hldir = path_to_hldir(htxf->remotepath, &hldirlen, 1);
			hlwrite(&sessions[0].htlc, HTLC_HDR_FILE_GET, 0, 
					(data_size || rsrc_size) ? 3 : 2,
					HTLC_DATA_FILE_NAME, strlen(rfile), rfile,
					HTLC_DATA_DIR, hldirlen, hldir,
					HTLC_DATA_RFLT, 74, rflt);
			g_free(hldir);
		} else {
			hlwrite(&sessions[0].htlc, HTLC_HDR_FILE_GET, 0, 
					(data_size || rsrc_size) ? 2 : 1,
					HTLC_DATA_FILE_NAME, strlen(rfile), rfile,
					HTLC_DATA_RFLT, 74, rflt);
		}
	} 
	else {
		guint32 size = htonl(htxf->total_size);

		rfile = basename(htxf->path);
		hldir = path_to_hldir(htxf->remotepath, &hldirlen, 1);
		task_new(&sessions[0].htlc, rcv_task_file_put, htxf, 0, "xfer_go");
		if (exists_remote(htxf->remotepath)) {
			hlwrite(&sessions[0].htlc, HTLC_HDR_FILE_PUT, 0, 4,
					HTLC_DATA_FILE_NAME, strlen(rfile), rfile,
					HTLC_DATA_DIR, hldirlen, hldir,
					HTLC_DATA_FILE_PREVIEW, 2, "\0\1",
					HTLC_DATA_HTXF_SIZE, 4, &size);
		} else {
			hlwrite(&sessions[0].htlc, HTLC_HDR_FILE_PUT, 0, 3,
					HTLC_DATA_FILE_NAME, strlen(rfile), rfile,
					HTLC_DATA_DIR, hldirlen, hldir,
					HTLC_DATA_HTXF_SIZE, 4, &size);
		}
		g_free(hldir);
	}
}

int xfer_go_timer (void *__arg)
{
	
	LOCK_HTXF((&(sessions[0].htlc)));
	xfer_go((struct htxf_conn *)__arg);
	UNLOCK_HTXF((&(sessions[0].htlc)));


	return 0;
}

struct htxf_conn *xfer_new (const char *path, const char *remotepath, 
							guint16 type)
{
	struct htxf_conn *htxf;

	htxf = g_malloc0(sizeof(struct htxf_conn));
	strcpy(htxf->remotepath, remotepath);
	strcpy(htxf->path, path);
	htxf->type = type;
	htxf->queue = -1;

	LOCK_HTXF((&(sessions[0].htlc)));
	xfers = g_realloc(xfers, (nxfers + 1) * sizeof(struct htxf_conn *));
	xfers[nxfers] = htxf;
	nxfers++;
	UNLOCK_HTXF((&(sessions[0].htlc)));

	htxf->htlc = &sessions[0].htlc;
	htxf->total_pos = 0;
	htxf->total_size = 1;
	hx_output.file_update(&sessions[0], htxf);

	LOCK_HTXF((&(sessions[0].htlc)));
	if(nxfers == 1 || !gtkhx_prefs.queuedl) {
		xfer_go(htxf);
	}
	UNLOCK_HTXF((&(sessions[0].htlc)));

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

			gtk_threads_enter();
			hx_output.file_update(&sessions[0], htxf);
			gtk_threads_leave();
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
		
		gtk_threads_enter();
		p->output(p, buf, len);
		htxf->total_pos += len;
		hx_output.file_update(&sessions[0], htxf);
		gtk_threads_leave();
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
		gtk_threads_enter();
		hx_output.file_update(&sessions[0], htxf);
		gtk_threads_leave();
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
		
		gtk_threads_enter();
		hx_output.file_update(&sessions[0], htxf);
		gtk_threads_leave();
	}
	memcpy(typecrea, &buf[4], 8);
	memset(&fi, 0, sizeof(struct hfs_cap_info));
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
		char *name = dirchar_basename(htxf->path);

		gtk_threads_enter();
		p = hx_preview_new(&typecrea[3], &typecrea[0], name);
		gtk_threads_leave();
		if(!p) {
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

		gtk_threads_enter();
		hx_output.file_update(&sessions[0], htxf);
		gtk_threads_leave();
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
	gtk_threads_enter();
	htxf->total_pos = htxf->total_size;
	hx_output.file_update(&sessions[0], htxf);
	gtk_threads_leave();

ret:
	close(s);

	LOCK_HTXF((&(sessions[0].htlc)));
	htxf->tid = 0;
	xfer_delete(htxf);
	UNLOCK_HTXF((&(sessions[0].htlc)));
	
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
	gtk_threads_enter();
	hx_output.file_update(&sessions[0], htxf);
	gtk_threads_leave();

ret:
	close(s);

	LOCK_HTXF((&(sessions[0].htlc)));
	htxf->tid = 0;
	xfer_delete(htxf);
	UNLOCK_HTXF((&(sessions[0].htlc)));

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

	err = pthread_create(&tid, 0, ((htxf->type == XFER_GET) ? 
								   get_thread : put_thread), htxf);

	sigaction(SIGTSTP, &tstpact, 0);
	sigaction(SIGCONT, &contact, 0);
	unignore_signals(&oldset);

	if (err) {
		hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, "xfer: pthread_create: %s\n", strerror(err));
		goto err_fd;
	}
	htxf->tid = tid;
//	pthread_detach(tid);

	return;

err_fd:
	LOCK_HTXF((&(sessions[0].htlc)));
	xfer_delete(htxf);
	UNLOCK_HTXF((&(sessions[0].htlc)));
}

void xfer_tasks_update (struct htlc_conn *htlc)
{
	int i;

	for (i = 0; i < nxfers; i++) {
		if (xfers[i]->htlc == htlc)
			hx_output.file_update(&sessions[0], xfers[i]);
	}
}

void xfers_delete_all (void)
{
	struct htxf_conn *htxf;
	int i;

	for (i = 0; i < nxfers; i++) {
		htxf = xfers[i];
		if (htxf->tid) {
//			void *thread_retval;

			pthread_cancel(htxf->tid);
//			pthread_join(htxf->tid, &thread_retval);
			g_free(htxf);
		}
	}
	nxfers = 0;
}

void xfer_delete (struct htxf_conn *htxf)
{
	int i;

	for (i = 0; i < nxfers; i++) {
		if (xfers[i] == htxf) {
			if(htxf->tid) {
//				void *thread_retval;
				
				pthread_cancel(htxf->tid);
//				pthread_join(htxf->tid, &thread_retval);
			}
#ifdef USE_IPV6
			if(htxf->listen_addr) {
				freeaddrinfo(htxf->listen_addr);
			}
#endif
			g_free(htxf);
			if (nxfers > (i+1)) {
				memcpy(&xfers[i], &xfers[i+1], (nxfers-(i+1)) * 
					   sizeof(struct htxf_conn *));
			}
			nxfers--;
			if (nxfers) {
				xfer_go(xfers[0]);
			}
			break;
		}
	}
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
