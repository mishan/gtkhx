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

#include <stdio.h>
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/select.h>
#include <time.h>
#include <netinet/in.h>

#ifndef WIN32
#include <signal.h>
#else
#include <winbase.h>
#endif

#include "hx.h"
#include "rcv.h"
#include "gtkthreads.h"
#include "gtkutil.h"
#include "chat.h"
#include "tasks.h"
#include "users.h"
#include "inet.h"
#include "log.h"

#include "sha.h"
#include "md5.h"

char *server_addr;
#ifdef USE_IPV6
guint16 server_port;
#endif

#if 0 /* XXX */
struct log *server_log = NULL;
#endif

#ifndef WIN32
pthread_t conn_tid;
#else
int conn_tid;
#endif

int connected;

int
fd_closeonexec (int fd, int on)
{
	int x;

	if ((x = fcntl(fd, F_GETFD, 0)) == -1)
		return -1;
	if (on)
		x &= ~FD_CLOEXEC;
	else
		x |= FD_CLOEXEC;

	return fcntl(fd, F_SETFD, x);
}

int
fd_lock_write (int fd)
{
	struct flock lk;

	lk.l_type = F_WRLCK;
	lk.l_start = 0;
	lk.l_whence = SEEK_SET;
	lk.l_len = 0;
	lk.l_pid = getpid();

	return fcntl(fd, F_SETLK, &lk);
}

void
hx_htlc_close (struct htlc_conn *htlc, int expected)
{
	int fd = htlc->fd;
	struct chat *chat, *cnext;
	struct hx_user *user, *unext;
	struct task *tsk, *tsknext;
	char buf[HOSTLEN];

	session *sess = sess_from_htlc(htlc);

	if(conn_tid) {
#ifndef WIN32
		pthread_cancel(conn_tid);
#else
		TerminateThread((HANDLE)conn_tid, 0);
		CloseHandle((HANDLE)conn_tid);
#endif
		conn_tid = 0;
	}
#ifdef USE_IPV6
	getnameinfo(htlc->addr->ai_addr, htlc->addr->ai_addrlen, buf, 
				sizeof(buf), NULL, 0, NI_NUMERICHOST);
#else
#ifndef WIN32
	inet_ntop(AF_INET, &htlc->addr.sin_addr, buf, sizeof(buf));
#else
	snprintf(buf, sizeof(buf), "%s", inet_ntoa(htlc->addr.sin_addr));
#endif
#endif
	hx_printf_prefix(htlc, 0, INFOPREFIX, "%s: %s\n", buf,

					 _("connection closed"));

	if(!expected)
		error_dialog("Error", "You have been disconnected.");
	
	connected = 0;
	setbtns(sess, 0);
	set_disconnect_btn(sess, 0);
	set_status_bar(0);
	close_connected_windows (sess);
	changetitlesdisconnected(sess);
	hxd_fd_clr(fd, FDR|FDW);

	close(fd);

	if (htlc->in.buf) {
		g_free(htlc->in.buf);
		htlc->in.buf = NULL;
	}
	if (htlc->out.buf) {
		g_free(htlc->out.buf);
		htlc->out.buf = NULL;
	}
	memset(&hxd_files[fd], 0, sizeof(struct hxd_file));
	htlc->fd = 0;
	htlc->uid = 0;
	htlc->color = 0;
	htlc->gdk_input = 0;
	htlc->version = 0;
	memset(htlc->login, 0, sizeof(htlc->login));


	for (chat = sess->chat_list; chat; chat = cnext) {
		cnext = chat->next;
		hx_output.users_clear(htlc, chat);
		for (user = chat->user_list->next; user; user = unext) {
			unext = user->next;
			hx_user_delete(&chat->user_tail, user);
		}
		if (chat != sess->chat_list)
			chat_delete(sess, chat);
	}
	memset(sess->chat_list, 0, sizeof(struct chat));
	sess->chat_list->user_list = sess->chat_list->user_tail =
		&sess->chat_list->__user_list;

	for (tsk = sess->task_list->next; tsk; tsk = tsknext) {
		tsknext = tsk->next;
		task_delete(sess, tsk);
	}


#ifdef USE_IPV6
	freeaddrinfo(htlc->addr);
	htlc->addr = NULL;
#endif

#ifdef CONFIG_CIPHER
	memset(htlc->cipher_encode_key, 0, sizeof(htlc->cipher_encode_key));
	memset(htlc->cipher_decode_key, 0, sizeof(htlc->cipher_decode_key));
	memset(&htlc->cipher_encode_state, 0, sizeof(htlc->cipher_encode_state));
	memset(&htlc->cipher_decode_state, 0, sizeof(htlc->cipher_decode_state));
	htlc->cipher_encode_type = 0;
	htlc->cipher_decode_type = 0;
	htlc->cipher_encode_keylen = 0;
	htlc->cipher_decode_keylen = 0;
#endif
#ifdef CONFIG_COMPRESS
	if (htlc->compress_encode_type != COMPRESS_NONE) {
		hx_printf_prefix(htlc, 0, INFOPREFIX, "GZIP deflate: in: %u  out: %u\n",
				 htlc->gzip_deflate_total_in, htlc->gzip_deflate_total_out);
		compress_encode_end(htlc);
	}
	if (htlc->compress_decode_type != COMPRESS_NONE) {
		hx_printf_prefix(htlc, 0, INFOPREFIX, "GZIP inflate: in: %u  out: %u\n",
				 htlc->gzip_inflate_total_in, htlc->gzip_inflate_total_out);
		compress_decode_end(htlc);
	}
	memset(&htlc->compress_encode_state, 0, sizeof(htlc->compress_encode_state));
	memset(&htlc->compress_decode_state, 0, sizeof(htlc->compress_decode_state));
	htlc->compress_encode_type = 0;
	htlc->compress_decode_type = 0;
	htlc->gzip_deflate_total_in = 0;
	htlc->gzip_deflate_total_out = 0;
	htlc->gzip_inflate_total_in = 0;
	htlc->gzip_inflate_total_out = 0;
#endif
	memset(htlc->sessionkey, 0, sizeof(htlc->sessionkey));
	htlc->sklen = 0;

#if 0 /* XXX */
	close_log(server_log);
	server_log = NULL;
#endif

	g_free(server_addr);
	server_addr = NULL;
}
unsigned int
decode (struct htlc_conn *htlc)
{
	struct qbuf *in = &htlc->read_in;
	struct qbuf *out = in;
	u_int32_t len, max, inused, r = in->len;
#ifdef CONFIG_CIPHER
	union cipher_state cipher_state;
	struct qbuf cipher_out;
#endif
#ifdef CONFIG_COMPRESS
	struct qbuf compress_out;
#endif

#ifdef CONFIG_CIPHER
	memset(&cipher_out, 0, sizeof(struct qbuf));
#endif
#ifdef CONFIG_COMPRESS
	memset(&compress_out, 0, sizeof(struct qbuf));
#endif

	if (!r)
		return 0;
	inused = 0;
	len = r;
	in->pos = 0;

#ifdef CONFIG_CIPHER
#ifdef CONFIG_COMPRESS
	if (htlc->compressalg && htlc->compress_decode_type != COMPRESS_NONE)
		max = 0xffffffff;
	else
#endif
		max = htlc->in.len;
	if (htlc->cipheralg && htlc->cipher_decode_type != CIPHER_NONE) {
		memcpy(&cipher_state, &htlc->cipher_decode_state, sizeof(cipher_state));
		out = &cipher_out;
		len = cipher_decode(htlc, out, in, max, &inused);
	} else
#endif
#ifdef CONFIG_COMPRESS
	if (htlc->compress_decode_type == COMPRESS_NONE)
#endif
	{
		max = htlc->in.len;
		out = in;
		if (r > max) {
			inused = max;
			len = max;
		} else {
			inused = r;
			len = r;
		}
	}
#ifdef CONFIG_COMPRESS
	if (htlc->compress_decode_type != COMPRESS_NONE) {
		max = htlc->in.len;
		out = &compress_out;
		len = compress_decode(htlc, out,
#ifdef CONFIG_CIPHER
				      htlc->cipher_decode_type == CIPHER_NONE ? in : &cipher_out,
#else
				      in,
#endif
				      max, &inused);
	}
#endif
	memcpy(&htlc->in.buf[htlc->in.pos], &out->buf[out->pos], len);
	if (r != inused) {
#ifdef CONFIG_CIPHER
		if (htlc->cipher_decode_type != CIPHER_NONE) {
			memcpy(&htlc->cipher_decode_state, &cipher_state, sizeof(cipher_state));
			cipher_decode(htlc, &cipher_out, in, inused, &inused);
		}
#endif
		memmove(&in->buf[0], &in->buf[inused], r - inused);
	}
	in->pos = r - inused;
	in->len -= inused;
	htlc->in.pos += len;
	htlc->in.len -= len;

#if defined(CONFIG_COMPRESS)
	if (compress_out.buf)
		g_free(compress_out.buf);
#endif
#if defined(CONFIG_CIPHER)
	if (cipher_out.buf)
		g_free(cipher_out.buf);
#endif

	return (htlc->in.len == 0);
}

#define READ_BUFSIZE   0x4000

static void
update_task (struct htlc_conn *htlc)
{
	if (htlc->in.pos >= SIZEOF_HL_HDR) {
		struct hl_hdr *h = 0;
		u_int32_t off = 0;

		/* find the last packet */
		while (off+20 <= htlc->in.pos) {
			h = (struct hl_hdr *)(&htlc->in.buf[off]);
			off += 20+ntohl(h->len);
		}
		if (h && (ntohl(h->type)&0xffffff) == HTLS_HDR_TASK) {
			struct task *tsk = task_with_trans(sess_from_htlc(htlc),
											   ntohl(h->trans));
			if (tsk) {
				tsk->pos = htlc->in.pos;
				tsk->len = htlc->in.len;
				hx_output.task_update(sess_from_htlc(htlc), tsk);
			}
		}
	}
}


static void htlc_read (int fd)
{
	ssize_t r;
	struct htlc_conn *htlc = hxd_files[fd].conn.htlc;
	struct qbuf *in = &htlc->read_in;

	if (!in->len) {
	 	qbuf_set(in, in->pos, READ_BUFSIZE);
		in->len = 0;
	} 
	r = read(fd, &in->buf[in->pos], READ_BUFSIZE-in->len);
	if (r == 0 || (r < 0 && errno != EWOULDBLOCK && errno != EINTR)) {
		hx_printf_prefix(htlc, 0, INFOPREFIX, "htlc_read: %d %s\n", r, strerror(errno));
		hx_htlc_close(htlc, 0);
	}
	else {
		in->len += r;
		while (decode(htlc)) {
			update_task(htlc);
			if (htlc->rcv) {
				if (htlc->rcv == hx_rcv_hdr) {
					hx_rcv_hdr(htlc);
					if (!hxd_files[fd].conn.htlc)
						return;
				} else {
					htlc->rcv(htlc);
					if (!hxd_files[fd].conn.htlc)
						return;
					goto reset;
				}
			} else {
			  reset:
				htlc->rcv = hx_rcv_hdr;
				qbuf_set(&htlc->in, 0, SIZEOF_HL_HDR);
			}
		}
		update_task(htlc);
	}	
}

static void htlc_write (int fd)
{
	ssize_t r;
	struct htlc_conn *htlc = hxd_files[fd].conn.htlc;

	r = write(fd, &htlc->out.buf[htlc->out.pos], htlc->out.len);
	if (r == 0 || (r < 0 && errno != EWOULDBLOCK && errno != EINTR)) {
		hx_printf_prefix(htlc, 0, INFOPREFIX, "htlc_write: %d %s\n", r, strerror(errno));
		hx_htlc_close(htlc, 0);
	}
	else {
		htlc->out.pos += r;
		htlc->out.len -= r;
		if (!htlc->out.len) {
			htlc->out.pos = 0;
			htlc->out.len = 0;
			hxd_fd_clr(fd, FDW);
		}
	}
}

struct connect_data {
	struct htlc_conn *htlc;
	char *serverstr, *login, *pass;
	guint16 port;
	int s;
	int secure;
};


static void hx_thread_connect (void *arg)
{
	int s;
	struct connect_data *cdata = arg;
	char enclogin[64], encpass[64];
	guint16 icon16;
	guint16 llen, plen;
	char *serverstr = cdata->serverstr;
	struct htlc_conn *htlc = cdata->htlc;
	char *login = cdata->login;
	char *pass = cdata->pass;
	int secure = cdata->secure;
	guint16 port = cdata->port;
	session *sess = sess_from_htlc(htlc);
#ifdef USE_IPV6
	char buf[HOSTLEN+1];
#else
	char buf[16];
#endif
#ifdef USE_IPV6
	char portstr[HOSTLEN];
	struct addrinfo *he;
	struct addrinfo hints;
	int error;

	server_port = port;
	sprintf(portstr, "%u", port);

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;	
	hints.ai_protocol = IPPROTO_TCP;
#else
	struct sockaddr_in saddr;

	memset(&saddr, 0, sizeof(struct sockaddr_in));
	saddr.sin_port = htons(port);
	saddr.sin_family = AF_INET;
#endif

#ifdef USING_DARWIN
	conn_tid = getpid();
#endif
	
	htlc->gdk_input = 0;

	debug("entering gtk_threads\n");
	gtk_threads_enter();
	hx_printf_prefix(htlc, 0, INFOPREFIX, _("connecting to %s\n"),
					 server_addr);
	conn_task_update(sess, 0);
	set_status_bar(-1);
	set_disconnect_btn(sess, 1);
	debug("leaving gtk_threads\n");
	gtk_threads_leave();


	debug("about to resolve host/address\n");
#ifndef USE_IPV6
#ifndef WIN32
	if(!(inet_pton(AF_INET, serverstr, &saddr.sin_addr))) {
#else
		if(!(inet_aton(serverstr, &saddr.sin_addr))) {
#endif
		struct hostent *he;

		if((he = gethostbyname(serverstr))) {
			size_t len = (unsigned)he->h_length > sizeof(struct in_addr)
				? sizeof(struct in_addr) : he->h_length;
			memcpy(&saddr.sin_addr, he->h_addr, len);
		}
		else {
#else
		if((error = getaddrinfo(serverstr, portstr, &hints, &he))) {
#endif
			debug("entering gtk_threads\n");
			gtk_threads_enter();

#ifdef USE_IPV6
			hx_printf_prefix(htlc, 0, INFOPREFIX, "%s: %s\n", serverstr,

							 gai_strerror(error));
#else
# ifdef HAVE_HSTRERROR
			hx_printf_prefix(htlc, 0, INFOPREFIX, _("DNS lookup for %s failed: %s\n"),
				serverstr, hstrerror(h_errno));
# else
			hx_printf_prefix(htlc, 0, INFOPREFIX, _("DNS lookup for %s failed\n"),
							 serverstr);
# endif
#endif

			setbtns(sess, 0);
			set_status_bar(0);
			set_disconnect_btn(sess, 0);
			conn_task_update(sess, 2);
			debug("leaving gtk_threads\n");
			gtk_threads_leave();
			return;
		}
#ifndef USE_IPV6
		}
#endif
		debug("resolved host/address\n");

#ifdef USE_IPV6
	while((s = socket(he->ai_family, SOCK_STREAM, IPPROTO_TCP)) < 0) {
#else
	if((s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
#endif
#ifdef USE_IPV6
		if(he->ai_next) {
			he = he->ai_next;
		}
		else {
#endif
			debug("entering gtk_threads\n");
			gtk_threads_enter();
			hx_printf_prefix(htlc, 0, INFOPREFIX, _("socket: %s\n"), strerror(errno));
			setbtns(sess, 0);
			set_status_bar(0);
			set_disconnect_btn(sess, 0);
			conn_task_update(sess, 2);
			debug("leaving gtk_threads\n");
			gtk_threads_leave();
			return;
#ifdef USE_IPV6
		}
#endif

	}
	debug("created socket\n");

	if (s >= hxd_open_max) {
		debug("entering gtk_threads\n");
		gtk_threads_enter();
		hx_printf_prefix(htlc, 0, INFOPREFIX, "%s:%d: Too many open files (%d >= %d)",
						 __FILE__, __LINE__, s, hxd_open_max);
		setbtns(sess, 0);
		set_status_bar(0);
		set_disconnect_btn(sess, 0);
		conn_task_update(sess, 2);
		debug("leaving gtk_threads\n");
		gtk_threads_leave();
		close(s);
		return;
	}

	debug("entering gtk_threads\n");
	gtk_threads_enter();
	conn_task_update(sess, 1);
	debug("leaving gtk_threads\n");
	gtk_threads_leave();
#ifdef USE_IPV6

	if (connect(s, he->ai_addr, he->ai_addrlen)) {
#else
	if(connect(s, (struct sockaddr *)&saddr, sizeof(saddr))) {
#endif
		debug("entering gtk_threads\n");
		gtk_threads_enter();
		hx_printf_prefix(htlc, 0, INFOPREFIX, _("connect: %s\n"),
						 strerror(errno));
		setbtns(sess, 0);
		set_status_bar(0);
		set_disconnect_btn(sess, 0);
		conn_task_update(sess, 2);
		debug("leaving gtk_threads\n");
		gtk_threads_leave();
		close(s);
		return;
	}
	debug("connected\n");

	debug("entering gtk_threads\n");
	gtk_threads_enter();
	set_status_bar(1);
	hx_printf_prefix(htlc, 0, INFOPREFIX, _("connected to %s\n"), server_addr);
	debug("leaving gtk_threads\n");
	gtk_threads_leave();
	
	{
		char magic[HTLS_MAGIC_LEN];
		fd_set rfds;
		struct timeval tv;

		write(s, HTLC_MAGIC, HTLC_MAGIC_LEN);

		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		tv.tv_sec = 30;
		tv.tv_usec = 0;
		select(s+1, &rfds, NULL, NULL, &tv);

		if(FD_ISSET(s, &rfds)) {
			read(s, magic, HTLS_MAGIC_LEN);
			if(strncmp(HTLS_MAGIC, magic, HTLS_MAGIC_LEN)) {
				debug("entering gtk_threads\n");
				gtk_threads_enter();
				hx_printf_prefix(htlc, 0, INFOPREFIX,
								 _("invalid hotline server\n"));
				setbtns(sess, 0);
				set_status_bar(0);
				set_disconnect_btn(sess, 0);
				conn_task_update(sess, 2);
				debug("leaving gtk_threads\n");
				gtk_threads_leave();
				close(s);
				return;
			}
		}
		else {
			debug("entering gtk_threads\n");
			gtk_threads_enter();
			hx_printf_prefix(htlc, 0, INFOPREFIX, 
							 _("no response from server after thirty seconds"));
			setbtns(sess, 0);
			set_status_bar(0);
			set_disconnect_btn(sess, 0);
			conn_task_update(sess, 2);
			debug("leaving gtk_threads\n");
			gtk_threads_leave();
			close(s);
			return;
		}
	}
	debug("received correct HTLS_MAGIC\n");
#ifdef USE_IPV6
	htlc->addr = he;
#else
	htlc->addr = saddr;
#endif

	htlc->fd = s;
	htlc->trans = 1;

	memset(&htlc->in, 0, sizeof(struct qbuf));
	memset(&htlc->out, 0, sizeof(struct qbuf));

	htlc->rcv = hx_rcv_hdr;
	qbuf_set(&htlc->in, 0, SIZEOF_HL_HDR);

	debug("entering gtk_threads\n");
	gtk_threads_enter();
	conn_task_update(sess, 2);
	debug("leaving gtk_threads\n");
	gtk_threads_leave();

	set_nonblocking(s);
	fd_closeonexec(s, 1);

	hxd_files[s].ready_read = htlc_read;
	hxd_files[s].ready_write = htlc_write;
	hxd_files[s].conn.htlc = htlc;
	hxd_files[s].fd = s;

	connected = 1;
	htlc->gdk_input = 1;

	hxd_fd_set(s, FDR);

	if (login) {
		strcpy(htlc->login, login);
	}

	if (secure) {
#ifdef CONFIG_CIPHER
		u_int8_t cipheralglist[64];
		u_int16_t cipheralglistlen;
		u_int8_t cipherlen;
#endif
#ifdef CONFIG_COMPRESS
		u_int8_t compressalglist[64];
		u_int16_t compressalglistlen;
		u_int8_t compresslen;
#endif
		u_int16_t hc;
		u_int8_t macalglist[64];
		u_int16_t macalglistlen;

		buf[0] = 0;
		debug("entering gtk_threads\n");
		gtk_threads_enter();
		task_new(htlc, rcv_task_login, pass ? g_strdup(pass) : g_strdup(buf), 0, "login");
		debug("leaving gtk_threads\n");
		gtk_threads_leave();
		strcpy(htlc->macalg, "HMAC-SHA1");
		{
			guint16 val = 2;
			HN16(macalglist, &val);
		}
		macalglistlen = 2;
		macalglist[macalglistlen] = 9;
		macalglistlen++;
		memcpy(macalglist+macalglistlen, htlc->macalg, 9);
		macalglistlen += 9;
		macalglist[macalglistlen] = 8;
		macalglistlen++;
		memcpy(macalglist+macalglistlen, "HMAC-MD5", 8);
		macalglistlen += 8;

		hc = 4;
#ifdef CONFIG_COMPRESS
		if (htlc->compressalg[0]) {
			compresslen = strlen(htlc->compressalg);
			{
				guint16 val = 1;
				HN16(compressalglist, &val);
			}
			compressalglistlen = 2;
			compressalglist[compressalglistlen] = compresslen;
			compressalglistlen++;
			memcpy(compressalglist+compressalglistlen, htlc->compressalg, compresslen);
			compressalglistlen += compresslen;
			hc++;
		} else
			compressalglistlen = 0;
#endif
#ifdef CONFIG_CIPHER
		if (htlc->cipheralg[0]) {
			cipherlen = strlen(htlc->cipheralg);
			{
				guint16 val = 1;

				HN16(cipheralglist, &val);
			}
			cipheralglistlen = 2;
			cipheralglist[cipheralglistlen] = cipherlen;
			cipheralglistlen++;
			memcpy(cipheralglist+cipheralglistlen, htlc->cipheralg, cipherlen);
			cipheralglistlen += cipherlen;
			hc++;
		} else
			cipheralglistlen = 0;
#endif
		hlwrite(htlc, HTLC_HDR_LOGIN, 0, hc,
			HTLC_DATA_LOGIN, 1, buf,
			HTLC_DATA_PASSWORD, 1, buf,
			HTLC_DATA_MAC_ALG, macalglistlen, macalglist,
#ifdef CONFIG_CIPHER
			HTLC_DATA_CIPHER_ALG, cipheralglistlen, cipheralglist,
#endif
#ifdef CONFIG_COMPRESS
			HTLC_DATA_COMPRESS_ALG, compressalglistlen, compressalglist,
#endif
			HTLC_DATA_SESSIONKEY, 0, 0);
		return;
	}


	debug("entering gtk_threads\n");
	gtk_threads_enter();
	task_new(htlc, rcv_task_login, 0, 0, "login");
	debug("leaving gtk_threads\n");
	gtk_threads_leave();

	icon16 = htons(htlc->icon);
	if (login) {
		llen = strlen(login);
		if (llen > 64)
			llen = 64;
		hl_encode(enclogin, login, llen);
	} else
		llen = 0;

	debug("entering gtk_threads\n");
	gtk_threads_enter();

	if (pass) {
		plen = strlen(pass);
		if (plen > 64)
			plen = 64;
		hl_encode(encpass, pass, plen);
		hlwrite(htlc, HTLC_HDR_LOGIN, 0, 4,
				HTLC_DATA_ICON, 2, &icon16,
				HTLC_DATA_LOGIN, llen, enclogin,
				HTLC_DATA_PASSWORD, plen, encpass,
				HTLC_DATA_NAME, strlen(htlc->name), htlc->name);
	}
	else {
		hlwrite(htlc, HTLC_HDR_LOGIN, 0, 3,
				HTLC_DATA_ICON, 2, &icon16,
				HTLC_DATA_LOGIN, llen, enclogin,
				HTLC_DATA_NAME, strlen(htlc->name), htlc->name);
	}
	debug("leaving gtk_threads\n");
	gtk_threads_leave();

	g_free(cdata->login);
	g_free(cdata->pass);
	g_free(cdata->serverstr);
	g_free(cdata);
	
	conn_tid = 0;
	pthread_exit(0);
}


void hx_connect(struct htlc_conn *htlc, const char *serverstr, guint16 port,
				const char *login, const char *pass, char secure)
{
	pthread_attr_t attr;
	struct connect_data *cdata;

	if(conn_tid) {
		pthread_cancel(conn_tid);
		pthread_join(conn_tid, NULL);
		conn_tid = 0;
	}

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	if (htlc->fd) {
		hx_htlc_close(htlc, 1);
	}
	hx_clear_chat(htlc, 0, 1);

	server_addr = g_strdup_printf("%s:%u", serverstr, port);

#if 0 /* XXX */
	server_log = create_log(server_addr);
#endif

	cdata = g_malloc(sizeof(struct connect_data));
	cdata->htlc = htlc;
	cdata->serverstr = g_strdup(serverstr);
	cdata->port = port;
	cdata->login = g_strdup(login);
	cdata->pass = g_strdup(pass);
	cdata->secure = secure;

#ifdef WIN32
	conn_tid = (int)CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)hx_thread_connect,
							(void *)cdata, 0, (DWORD *)&pid);
#else
	pthread_create(&conn_tid, &attr, (void *)&hx_thread_connect, 
				   (void *)cdata);
#endif
}

int htxf_connect (struct htxf_conn *htxf)
{
	struct htxf_hdr h;
	int s;

#ifdef USE_IPV6
	s = socket(htxf->listen_addr->ai_family, SOCK_STREAM, IPPROTO_TCP);
#else
	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
	if (s < 0) {
		return -1;
	}

#ifdef USE_IPV6
	if (connect(s, htxf->listen_addr->ai_addr, htxf->listen_addr->ai_addrlen)){
#else
	if (connect(s, (struct sockaddr *)&htxf->listen_addr, 
				sizeof(struct sockaddr))) {
#endif
		close(s);
		return -1;
	}

	h.magic = htonl(HTXF_MAGIC_INT);
	h.ref = htonl(htxf->ref);
	h.unknown = 0;
	h.len = htonl(htxf->total_size);
	if (write(s, &h, SIZEOF_HTXF_HDR) != SIZEOF_HTXF_HDR) {
		return -1;
	}

	return s;
}

static int b_read (int fd, void *bufp, size_t len)
{
	register guint8 *buf = (guint8 *)bufp;
	register int r, pos = 0;

	while (len) {
		if ((r = read(fd, &(buf[pos]), len)) <= 0)
			return -1;
		pos += r;
		len -= r;
	}

	return pos;
}

void hx_tracker_list(session *sess, char *serverstr, guint16 port)
{
	int s;
	guint16 nusers, nservers;
	unsigned char buf[HOSTLEN];
	unsigned char name[512], desc[512];
	struct in_addr a;
	int total;
	int i;
#ifdef USE_IPV6
	struct addrinfo *he;
	struct addrinfo hints;
	int error;
	char portstr[HOSTLEN];

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	sprintf(portstr, "%u", port);
#else
	struct sockaddr_in saddr;

	memset(&saddr, 0, sizeof(struct sockaddr_in));
	saddr.sin_port = htons(port);
	saddr.sin_family = AF_INET;
#endif
	
	debug("entering gtk_threads\n");
	gtk_threads_enter();
	trackconn_prog_update(sess, serverstr, 0, 2);
	debug("leaving gtk_threads\n");
	gtk_threads_leave();
	
#ifndef USE_IPV6
#ifndef WIN32
	if(!inet_pton(AF_INET, serverstr, &saddr.sin_addr)) {
#else
		if(!inet_aton(serverstr, &saddr.sin_addr)) {
#endif
		struct hostent *he;


		if((he = gethostbyname(serverstr))) {
			size_t len = (unsigned)he->h_length > sizeof(struct in_addr)
				? sizeof(struct in_addr) : he->h_length;
			memcpy(&saddr.sin_addr, he->h_addr, len);
		}
		else {
#else
			if((error = getaddrinfo(serverstr, portstr, &hints, &he))) {
#endif
				debug("entering gtk_threads\n");
				gtk_threads_enter();
#ifdef USE_IPV6
				hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX,

								 "%s: %s\n", serverstr, gai_strerror(error));
#else
# ifdef HAVE_HSTRERROR
				hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX,
								 _("DNS lookup for %s failed: %s\n"),
								 serverstr, hstrerror(h_errno));
# else
				hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX,
								 _("DNS lookup for %s failed\n"), serverstr);
# endif
#endif
				trackconn_prog_update(sess, serverstr, 2, 2);
				debug("leaving gtk_threads\n");
				gtk_threads_leave();
				return;
			}
#ifndef USE_IPV6
		}
#endif
		
		debug("entering gtk_threads\n");
		gtk_threads_enter();
		trackconn_prog_update(sess, serverstr, 1, 2);
		debug("leaving gtk_threads\n");
		gtk_threads_leave();

#ifdef USE_IPV6
	while((s = socket(he->ai_family, SOCK_STREAM, IPPROTO_TCP)) < 0) {
#else
	if((s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
#endif
#ifdef USE_IPV6
		if(he->ai_next) {
			he = he->ai_next;
		}
		else {
#endif
			debug("entering gtk_threads\n");
			gtk_threads_enter();
			hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, _("tracker: %s\n"), strerror(errno));
			trackconn_prog_update(sess, serverstr, 2, 2);
			debug("leaving gtk_threads\n");
			gtk_threads_leave();
			return;
#ifdef USE_IPV6
		}
#endif
	}

	set_blocking(s);

#ifdef USE_IPV6
	if((connect(s, he->ai_addr, he->ai_addrlen)) < 0) {
#else
	if((connect(s, (struct sockaddr *)&saddr, sizeof(struct sockaddr)))) {
#endif
		debug("entering gtk_threads\n");
		gtk_threads_enter();
		hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, _("tracker: %s: %s"),

						 serverstr, strerror(errno));
		trackconn_prog_update(sess, serverstr, 2, 2);
		debug("leaving gtk_threads\n");
		gtk_threads_leave();
		return;
	}

	debug("entering gtk_threads\n");
	gtk_threads_enter();
	trackconn_prog_update(sess, serverstr, 2, 2);
	debug("leaving gtk_threads\n");
	gtk_threads_leave();

	if (write(s, HTRK_MAGIC, HTRK_MAGIC_LEN) != HTRK_MAGIC_LEN)
		goto funk_dat;

	track_prog_update(sess, serverstr, 0, 0);

	if (b_read(s, buf, 14) != 14) {
		goto funk_dat;
	}


	nservers = ntohs(*((guint16 *)(&(buf[10]))));
	total = nservers;
	track_prog_update(sess, serverstr, 0, total);
	for (i = 1; nservers; nservers--, i++) {
		if (b_read(s, buf, 8) == -1) {
			break;
		}

		if (!buf[0]) {	/* assuming an address does not begin with 0,
						   we can skip this */
			nservers++;
			continue;
		}
		if (b_read(s, buf+8, 3) == -1) {
			break;
		}

		a.s_addr = *((guint32 *)buf);
		port = ntohs(*((guint16 *)(&(buf[4]))));
		nusers = ntohs(*((guint16 *)(&(buf[6]))));


		if (b_read(s, name, (size_t)buf[10]) == -1) {
			break;
		}


		name[(size_t)buf[10]] = 0;
		CR2LF(name, (size_t)buf[10]);
		strip_ansi(name, (size_t)buf[10]);


		if (b_read(s, buf, 1) == -1) {
			break;
		}


		memset(desc, 0, sizeof(desc));


		if (b_read(s, desc, (size_t)buf[0]) == -1) {
			break;
		}


		desc[(size_t)buf[0]] = 0;
		CR2LF(desc, (size_t)buf[0]);
		strip_ansi(desc, (size_t)buf[0]);

		debug("entering gtk_threads\n");
		gtk_threads_enter();
		hx_output.tracker_server_create(a, port, nusers, name, desc, total);
		track_prog_update(sess, serverstr, i, total);
		debug("leaving gtk_threads\n");
		gtk_threads_leave();
	}
  funk_dat:


	close(s);


	return;
}

void kill_threads(void) {
	pthread_cancel(conn_tid);
}

void hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
	va_list ap;
	struct hl_hdr h;
	struct hl_data_hdr dhs;
	struct qbuf *q;
	guint32 this_off, pos;
	guint32 len = 0;

	if (!htlc->fd) {
		return;
	}

	q = &htlc->out;
	this_off = q->pos + q->len;
	pos = this_off + SIZEOF_HL_HDR;
	q->len += SIZEOF_HL_HDR;
	q->buf = g_realloc(q->buf, q->pos + q->len);

	h.type = htonl(type);
	h.trans = htonl(htlc->trans);
	htlc->trans++;

	h.flag = htonl(flag);
	h.hc = htons(hc);

	va_start (ap, hc);
	while (hc) {
		guint16 t, l;
		guint8 *data;

		t = (guint16)va_arg (ap, int);
		l = (guint16)va_arg (ap, int);
		dhs.type = htons (t);
		dhs.len = htons (l);

		q->len += SIZEOF_HL_DATA_HDR + l;
		q->buf = g_realloc (q->buf, q->pos + q->len);
		memcpy (&q->buf[pos], (guint8 *)&dhs, 4);
		pos += 4;
		data = va_arg (ap, guint8 *);
		if (l) {
			memcpy (&q->buf[pos], data, l);
			pos += l;
		}
		hc--;
	}
	va_end(ap);
	
	len = pos - this_off;
	h.len = h.len2= htonl(len - (SIZEOF_HL_HDR - sizeof(h.hc)));
	memcpy(q->buf + this_off, &h, SIZEOF_HL_HDR);
	hxd_fd_set(htlc->fd, FDW);
#ifdef CONFIG_COMPRESS
	if (htlc->compress_encode_type != COMPRESS_NONE)
		len = compress_encode(htlc, this_off, len);
#endif
#ifdef CONFIG_CIPHER
	if (htlc->cipher_encode_type != CIPHER_NONE)
		cipher_encode(htlc, this_off, len);
#endif
}

void hl_code (void *__dst, const void *__src, size_t len)
{
	guint8 *dst = (guint8 *)__dst, *src = (guint8 *)__src;

	for (; len; len--)
		*dst++ = ~*src++;
}
