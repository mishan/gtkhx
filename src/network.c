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

#include <signal.h>
#include <stdarg.h>

#include "hx.h"
#include "rcv.h"
#include "gtkthreads.h"
#include "gtkutil.h"
#include "chat.h"
#include "tasks.h"
#include "users.h"
#include "inet.h"
#include "log.h"
#include "proto_trace.h"
#include "network.h"
#include "banner.h"

char *server_addr;
#ifdef USE_IPV6
guint16 server_port;
#endif

#if 0 /* XXX */
struct log *server_log = NULL;
#endif

pthread_t conn_tid;

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

/* Phase 5: PING keepalive. Some servers (hlserver.com is the known
 * case) drop idle connections after a few minutes of silence. mhxd
 * defines HTLC_HDR_PING / HTLS_HDR_PING for client-driven keepalive;
 * we send an empty PING every PING_INTERVAL_SEC seconds while
 * connected, and the server resets its idle timer on receipt. The
 * server replies with HTLS_HDR_TASK flag=0 (no chunks) — we treat
 * it like any other no-op task reply.
 *
 * Old (1.0/1.2) servers will respond with a task error to the
 * unknown opcode, which task_error() now toasts (instead of modal-
 * dialoging) — annoying but not fatal. If that proves to be a
 * compat problem in the wild, we can gate sending on a
 * server-supports-ping flag detected from version info, but for
 * now sending unconditionally matches mhxd's hx client behaviour. */

#define PING_INTERVAL_SEC 60

static guint ping_timer_id = 0;

static gboolean
ping_tick (gpointer data)
{
	struct htlc_conn *htlc = data;

	if (!htlc || !htlc->fd) {
		ping_timer_id = 0;
		return G_SOURCE_REMOVE;
	}
	hlwrite (htlc, HTLC_HDR_PING, 0, 0);
	return G_SOURCE_CONTINUE;
}

void
ping_start (struct htlc_conn *htlc)
{
	if (ping_timer_id || !htlc || !htlc->fd)
		return;
	ping_timer_id = g_timeout_add_seconds (PING_INTERVAL_SEC,
	                                       ping_tick, htlc);
}

void
ping_stop (void)
{
	if (ping_timer_id) {
		g_source_remove (ping_timer_id);
		ping_timer_id = 0;
	}
}

void
hx_htlc_close (struct htlc_conn *htlc, int expected)
{
	int fd = htlc->fd;
	struct chat *chat, *cnext;
	struct hx_user *user, *unext;
	struct task *tsk, *tsknext;
	char buf[HOSTLEN];

	session *sess = &the_session;

	ping_stop ();
	rcv_login_reset ();
	banner_clear ();

	if(conn_tid) {
		pthread_cancel(conn_tid);
		conn_tid = 0;
	}
#ifdef USE_IPV6
	getnameinfo(htlc->addr->ai_addr, htlc->addr->ai_addrlen, buf, 
				sizeof(buf), NULL, 0, NI_NUMERICHOST);
#else
	inet_ntop(AF_INET, &htlc->addr.sin_addr, buf, sizeof(buf));
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
static unsigned int
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
	if (htlc->compressalg[0] && htlc->compress_decode_type != COMPRESS_NONE)
		max = 0xffffffff;
	else
#endif
		max = htlc->in.len;
	if (htlc->cipheralg[0] && htlc->cipher_decode_type != CIPHER_NONE) {
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
			struct task *tsk = task_with_trans(&the_session,
											   ntohl(h->trans));
			if (tsk) {
				tsk->pos = htlc->in.pos;
				tsk->len = htlc->in.len;
				hx_output.task_update(&the_session, tsk);
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
					/* Phase 5: body is fully buffered now;
					 * dump its data chunks before dispatch.
					 * No-op when "proto" debug category is
					 * disabled. */
					proto_trace_recv_chunks (htlc);
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


/*
 * Worker → main thread marshal helpers.
 *
 * Each post_* allocates a job, deep-copies any caller-owned strings
 * out of the worker's stack, and queues a one-shot idle on the
 * default main context. The dispatcher runs the original UI function
 * on the main thread, frees the job, returns G_SOURCE_REMOVE.
 *
 * Used by hx_thread_connect (connect/login flow), hx_tracker_list
 * (tracker fetch flow), and any future worker-thread caller of
 * GTK-touching functions in this file. See gtkthreads.h for the
 * thread-contract rationale.
 */

/* (session *, int) — for conn_task_update, set_disconnect_btn, setbtns */
struct si_job {
	void (*fn)(session *, int);
	session *sess;
	int n;
};
static gboolean
si_dispatch (gpointer data)
{
	struct si_job *j = data;
	j->fn (j->sess, j->n);
	g_free (j);
	return G_SOURCE_REMOVE;
}
static void
post_session_int (void (*fn)(session *, int), session *sess, int n)
{
	struct si_job *j = g_new0 (struct si_job, 1);
	j->fn = fn;
	j->sess = sess;
	j->n = n;
	gtkhx_post_to_main (si_dispatch, j);
}

/* (int) — for set_status_bar */
struct i_job {
	void (*fn)(int);
	int n;
};
static gboolean
i_dispatch (gpointer data)
{
	struct i_job *j = data;
	j->fn (j->n);
	g_free (j);
	return G_SOURCE_REMOVE;
}
static void
post_int (void (*fn)(int), int n)
{
	struct i_job *j = g_new0 (struct i_job, 1);
	j->fn = fn;
	j->n = n;
	gtkhx_post_to_main (i_dispatch, j);
}

/* task_new(htlc, rcv, ptr, data, str). The caller's `ptr` is forwarded
 * to task_new which stores it in tsk->ptr without copying — so we pass
 * it through unchanged and don't free it. `str` is g_strdup'd inside
 * task_new, so we deep-copy here only to keep the worker's stack
 * buffer alive until dispatch (and free our copy after). */
/* `rcv` rides the canonical rcv_task_fn typedef from protocol.h. The
 * heterogeneous rcv_task_* implementations are cast to this type by
 * their caller; we just thread the value through to task_new. */
struct tn_job {
	struct htlc_conn *htlc;
	rcv_task_fn rcv;
	void *ptr;
	void *data;
	char *str;
};
static gboolean
tn_dispatch (gpointer data)
{
	struct tn_job *j = data;
	task_new (j->htlc, j->rcv, j->ptr, j->data, j->str);
	g_free (j->str);
	g_free (j);
	return G_SOURCE_REMOVE;
}
static void
post_task_new (struct htlc_conn *htlc, rcv_task_fn rcv,
               void *ptr, void *data, const char *str)
{
	struct tn_job *j = g_new0 (struct tn_job, 1);
	j->htlc = htlc;
	j->rcv = rcv;
	j->ptr = ptr;
	j->data = data;
	j->str = g_strdup (str ? str : "");
	gtkhx_post_to_main (tn_dispatch, j);
}

/* (session *, char *, int, int) — for trackconn_prog_update,
 * track_prog_update. Used by the tracker worker. */
struct prog_job {
	void (*fn)(session *, char *, int, int);
	session *sess;
	char *str;
	int num;
	int total;
};
static gboolean
prog_dispatch (gpointer data)
{
	struct prog_job *j = data;
	j->fn (j->sess, j->str, j->num, j->total);
	g_free (j->str);
	g_free (j);
	return G_SOURCE_REMOVE;
}
static void
post_prog (void (*fn)(session *, char *, int, int),
           session *sess, const char *str, int num, int total)
{
	struct prog_job *j = g_new0 (struct prog_job, 1);
	j->fn = fn;
	j->sess = sess;
	j->str = g_strdup (str ? str : "");
	j->num = num;
	j->total = total;
	gtkhx_post_to_main (prog_dispatch, j);
}

/* tracker_server_create(addr, port, nusers, name, desc, total). */
struct ts_job {
	struct in_addr addr;
	guint16 port;
	guint16 nusers;
	char *name;
	char *desc;
	int total;
};
static gboolean
ts_dispatch (gpointer data)
{
	struct ts_job *j = data;
	hx_output.tracker_server_create (j->addr, j->port, j->nusers,
	                                 j->name, j->desc, j->total);
	g_free (j->name);
	g_free (j->desc);
	g_free (j);
	return G_SOURCE_REMOVE;
}
static void
post_ts (struct in_addr addr, guint16 port, guint16 nusers,
         const char *name, const char *desc, int total)
{
	struct ts_job *j = g_new0 (struct ts_job, 1);
	j->addr = addr;
	j->port = port;
	j->nusers = nusers;
	j->name = g_strdup (name ? name : "");
	j->desc = g_strdup (desc ? desc : "");
	j->total = total;
	gtkhx_post_to_main (ts_dispatch, j);
}

/* hx_printf_prefix(htlc, cid, prefix, fmt, ...). Format on the worker,
 * pass the already-formatted text through "%s" so any % in the captured
 * text isn't reinterpreted by hx_printf_prefix's own vsnprintf. */
struct log_job {
	struct htlc_conn *htlc;
	guint32 cid;
	char *prefix;
	char *text;
};
static gboolean
log_dispatch (gpointer data)
{
	struct log_job *j = data;
	hx_printf_prefix (j->htlc, j->cid, j->prefix, "%s", j->text);
	g_free (j->prefix);
	g_free (j->text);
	g_free (j);
	return G_SOURCE_REMOVE;
}
static void G_GNUC_PRINTF (4, 5)
post_log (struct htlc_conn *htlc, guint32 cid,
          const char *prefix, const char *fmt, ...)
{
	struct log_job *j = g_new0 (struct log_job, 1);
	va_list ap;

	j->htlc = htlc;
	j->cid = cid;
	j->prefix = g_strdup (prefix ? prefix : "");
	va_start (ap, fmt);
	j->text = g_strdup_vprintf (fmt, ap);
	va_end (ap);
	gtkhx_post_to_main (log_dispatch, j);
}

/*
 * Synchronous worker → main login-send dispatchers.
 *
 * The hlwrite calls in hx_thread_connect mutate htlc->out (the
 * connection send qbuf), htlc->trans (transaction counter),
 * htlc->{cipher,compress}_* (encoding state), and the global
 * fd_set via hxd_fd_set. All of those are now main-thread-only on
 * every other path; these three login-send sites are the last
 * worker-thread writers. Marshal them via gtkhx_invoke_sync so the
 * worker blocks while the main thread runs the actual hlwrite.
 *
 * Sync rather than async because (a) the worker has nothing to do
 * until the packet is queued, (b) the args are stack locals
 * (icon16, llen/plen, encoded buffers) that we'd otherwise have to
 * deep-copy for an async post, and (c) the legacy bracketed code
 * was synchronous, so we preserve the original ordering contract
 * exactly. The gtkhx_invoke_sync footgun (don't call from main)
 * doesn't apply here — hx_thread_connect is always a worker.
 */
struct login_secure_args {
	struct htlc_conn *htlc;
	int hc;
	const guint8 *buf;          /* 1-byte zero, used for both empty
	                             * login and empty password fields */
	const guint8 *macalglist;
	guint16 macalglistlen;
#ifdef CONFIG_CIPHER
	const guint8 *cipheralglist;
	guint16 cipheralglistlen;
#endif
#ifdef CONFIG_COMPRESS
	const guint8 *compressalglist;
	guint16 compressalglistlen;
#endif
};

static gboolean
login_secure_dispatch (gpointer data)
{
	struct login_secure_args *a = data;
	hlwrite (a->htlc, HTLC_HDR_LOGIN, 0, a->hc,
	    HTLC_DATA_LOGIN, 1, a->buf,
	    HTLC_DATA_PASSWORD, 1, a->buf,
	    HTLC_DATA_MAC_ALG, a->macalglistlen, a->macalglist,
#ifdef CONFIG_CIPHER
	    HTLC_DATA_CIPHER_ALG, a->cipheralglistlen, a->cipheralglist,
#endif
#ifdef CONFIG_COMPRESS
	    HTLC_DATA_COMPRESS_ALG, a->compressalglistlen, a->compressalglist,
#endif
	    HTLC_DATA_SESSIONKEY, 0, 0);
	return G_SOURCE_REMOVE;
}

/* Covers both the password-bearing and password-less legacy login
 * paths. encpass == NULL signals the no-password variant; the field
 * is omitted from the packet (hc 2 instead of 3).
 *
 * Phase 5: NAME used to be sent here (the modern, mhxd-extension
 * flow), but mhxd's banner-send code path is gated on
 * htlc->access_extra.can_agree, which is ONLY set when LOGIN
 * arrived without a NAME chunk. The banner support work needs the
 * legacy two-stage flow:
 *
 *   client → LOGIN (no NAME)
 *   server → loginreply TASK + AGREEMENT
 *   client → AGREEMENTAGREE with NAME + ICON  (see hx_send_agreement_agree)
 *   server → SELFINFO + USER_GETLIST + (if configured) HTLS_HDR_BANNER
 *
 * The legacy flow is also more compatible with original Hotline
 * 1.0 / 1.2 servers, which spec'd NAME at AGREEMENTAGREE-time
 * rather than LOGIN-time. */
struct login_args {
	struct htlc_conn *htlc;
	guint16 icon16;
	const guint8 *enclogin;
	guint16 llen;
	const guint8 *encpass;
	guint16 plen;
};

static gboolean
login_dispatch (gpointer data)
{
	struct login_args *a = data;
	if (a->encpass) {
		hlwrite (a->htlc, HTLC_HDR_LOGIN, 0, 3,
		    HTLC_DATA_ICON, 2, &a->icon16,
		    HTLC_DATA_LOGIN, a->llen, a->enclogin,
		    HTLC_DATA_PASSWORD, a->plen, a->encpass);
	} else {
		hlwrite (a->htlc, HTLC_HDR_LOGIN, 0, 2,
		    HTLC_DATA_ICON, 2, &a->icon16,
		    HTLC_DATA_LOGIN, a->llen, a->enclogin);
	}
	return G_SOURCE_REMOVE;
}

/* Phase 5: HTLC_HDR_AGREEMENTAGREE with NAME + ICON. Sent from
 * gtkhx.c::concurrence (Agree button) once the agreement-window
 * is dismissed. Two server-side effects:
 *
 *   1. mhxd's rcv_agreementagree finishes login (calls account_read
 *      against our login, sets access bits, broadcasts join), and
 *      sends SELFINFO + USER_GETLIST.
 *   2. If the server config has banner.type set, mhxd unconditionally
 *      sends HTLS_HDR_BANNER from inside that handler.
 *
 * ICON comes from htlc->icon (preserved across the connect), NAME
 * from htlc->name (set from prefs.nick at connect time). */
void
hx_send_agreement_agree (struct htlc_conn *htlc)
{
	guint16 icon16 = htons (htlc->icon);

	hlwrite (htlc, HTLC_HDR_AGREEMENTAGREE, 0, 2,
	    HTLC_DATA_ICON, 2, &icon16,
	    HTLC_DATA_NAME, strlen ((const char *) htlc->name), htlc->name);
}

/* Initialize the connection's qbuf state (and assign the freshly
 * connected fd) on the main thread. The worker used to memset
 * these directly, but htlc->in/out/trans/rcv are read by
 * htlc_read/htlc_write on the main thread, and htlc->fd is read by
 * htlc_write — initializing all of them on the same thread as the
 * readers eliminates the cross-thread visibility question entirely.
 *
 * Run via gtkhx_invoke_sync from the worker before the fd watch is
 * installed, so the dispatch returns (and all writes are visible)
 * by the time the main loop can pick the fd up. */
struct htlc_init_args {
	struct htlc_conn *htlc;
	int fd;
};

static gboolean
htlc_init_dispatch (gpointer data)
{
	struct htlc_init_args *a = data;
	a->htlc->fd = a->fd;
	a->htlc->trans = 1;
	memset (&a->htlc->in,  0, sizeof (struct qbuf));
	memset (&a->htlc->out, 0, sizeof (struct qbuf));
	a->htlc->rcv = hx_rcv_hdr;
	qbuf_set (&a->htlc->in, 0, SIZEOF_HL_HDR);
	return G_SOURCE_REMOVE;
}

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
	session *sess = &the_session;
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
	g_snprintf(portstr, sizeof(portstr), "%u", port);

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

	post_log(htlc, 0, INFOPREFIX, _("connecting to %s\n"), server_addr);
	post_session_int(conn_task_update, sess, 0);
	post_int(set_status_bar, -1);
	post_session_int(set_disconnect_btn, sess, 1);


	debug("about to resolve host/address\n");
#ifndef USE_IPV6
	if(!(inet_pton(AF_INET, serverstr, &saddr.sin_addr))) {
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
#ifdef USE_IPV6
			post_log(htlc, 0, INFOPREFIX, "%s: %s\n", serverstr,
			         gai_strerror(error));
#else
# ifdef HAVE_HSTRERROR
			post_log(htlc, 0, INFOPREFIX,
			         _("DNS lookup for %s failed: %s\n"),
			         serverstr, hstrerror(h_errno));
# else
			post_log(htlc, 0, INFOPREFIX,
			         _("DNS lookup for %s failed\n"), serverstr);
# endif
#endif
			post_session_int(setbtns, sess, 0);
			post_int(set_status_bar, 0);
			post_session_int(set_disconnect_btn, sess, 0);
			post_session_int(conn_task_update, sess, 2);
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
			post_log(htlc, 0, INFOPREFIX, _("socket: %s\n"), strerror(errno));
			post_session_int(setbtns, sess, 0);
			post_int(set_status_bar, 0);
			post_session_int(set_disconnect_btn, sess, 0);
			post_session_int(conn_task_update, sess, 2);
			return;
#ifdef USE_IPV6
		}
#endif

	}
	debug("created socket\n");

	if (s >= hxd_open_max) {
		post_log(htlc, 0, INFOPREFIX,
		         "%s:%d: Too many open files (%d >= %d)",
		         __FILE__, __LINE__, s, hxd_open_max);
		post_session_int(setbtns, sess, 0);
		post_int(set_status_bar, 0);
		post_session_int(set_disconnect_btn, sess, 0);
		post_session_int(conn_task_update, sess, 2);
		close(s);
		return;
	}

	post_session_int(conn_task_update, sess, 1);
#ifdef USE_IPV6

	if (connect(s, he->ai_addr, he->ai_addrlen)) {
#else
	if(connect(s, (struct sockaddr *)&saddr, sizeof(saddr))) {
#endif
		post_log(htlc, 0, INFOPREFIX, _("connect: %s\n"),
		         strerror(errno));
		post_session_int(setbtns, sess, 0);
		post_int(set_status_bar, 0);
		post_session_int(set_disconnect_btn, sess, 0);
		post_session_int(conn_task_update, sess, 2);
		close(s);
		return;
	}
	debug("connected\n");

	post_int(set_status_bar, 1);
	post_log(htlc, 0, INFOPREFIX, _("connected to %s\n"), server_addr);

	{
		char magic[HTLS_MAGIC_LEN];
		fd_set rfds;
		struct timeval tv;

		/* The magic write/read pair is the Hotline handshake.
		 * If write fails, the subsequent select+read will see a
		 * dead socket and the magic compare downstream will reject
		 * the connection. We do explicitly check both calls so a
		 * partial-write or short-read produces a clean rejection
		 * instead of a stale buffer compare. */
		if (write(s, HTLC_MAGIC, HTLC_MAGIC_LEN) != HTLC_MAGIC_LEN) {
			post_log(htlc, 0, INFOPREFIX,
			         _("error writing client magic\n"));
			post_session_int(setbtns, sess, 0);
			post_int(set_status_bar, 0);
			post_session_int(set_disconnect_btn, sess, 0);
			post_session_int(conn_task_update, sess, 2);
			close(s);
			return;
		}

		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		tv.tv_sec = 30;
		tv.tv_usec = 0;
		select(s+1, &rfds, NULL, NULL, &tv);

		if(FD_ISSET(s, &rfds)) {
			ssize_t magic_n = read(s, magic, HTLS_MAGIC_LEN);
			if (magic_n != HTLS_MAGIC_LEN) {
				post_log(htlc, 0, INFOPREFIX,
				         _("error reading server magic\n"));
				post_session_int(setbtns, sess, 0);
				post_int(set_status_bar, 0);
				post_session_int(set_disconnect_btn, sess, 0);
				post_session_int(conn_task_update, sess, 2);
				close(s);
				return;
			}
			if(strncmp(HTLS_MAGIC, magic, HTLS_MAGIC_LEN)) {
				post_log(htlc, 0, INFOPREFIX,
				         _("invalid hotline server\n"));
				post_session_int(setbtns, sess, 0);
				post_int(set_status_bar, 0);
				post_session_int(set_disconnect_btn, sess, 0);
				post_session_int(conn_task_update, sess, 2);
				close(s);
				return;
			}
		}
		else {
			post_log(htlc, 0, INFOPREFIX,
			         _("no response from server after thirty seconds"));
			post_session_int(setbtns, sess, 0);
			post_int(set_status_bar, 0);
			post_session_int(set_disconnect_btn, sess, 0);
			post_session_int(conn_task_update, sess, 2);
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

	{
		/* Phase 5: htlc state init runs on the main thread via
		 * gtkhx_invoke_sync. The fields touched here (fd, trans,
		 * in, out, rcv) are read on the main thread by htlc_read /
		 * htlc_write / hlwrite / task_new, so the writes belong on
		 * the same thread as the readers. */
		struct htlc_init_args ia = { .htlc = htlc, .fd = s };
		gtkhx_invoke_sync (htlc_init_dispatch, &ia);
	}

	post_session_int(conn_task_update, sess, 2);

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
		/* task_new mutates session->task_list and creates a GTK task
		 * widget — must run on main. The pass/buf pointer is stored
		 * in tsk->ptr; we hand it off to the dispatcher unchanged so
		 * task ownership semantics match the legacy code. */
		post_task_new(htlc, RCV_TASK_FN(rcv_task_login),
		              pass ? g_strdup(pass) : g_strdup(buf),
		              0, "login");
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
		{
			/* Phase 5: hlwrite runs on the main thread. Args are
			 * stack locals so we capture by pointer; gtkhx_invoke_sync
			 * blocks the worker until the dispatch returns, keeping
			 * the args alive. */
			struct login_secure_args la = {
				.htlc = htlc,
				.hc = hc,
				.buf = (const guint8 *)buf,
				.macalglist = macalglist,
				.macalglistlen = macalglistlen,
#ifdef CONFIG_CIPHER
				.cipheralglist = cipheralglist,
				.cipheralglistlen = cipheralglistlen,
#endif
#ifdef CONFIG_COMPRESS
				.compressalglist = compressalglist,
				.compressalglistlen = compressalglistlen,
#endif
			};
			gtkhx_invoke_sync (login_secure_dispatch, &la);
		}
		return;
	}


	post_task_new(htlc, RCV_TASK_FN(rcv_task_login), 0, 0, "login");

	icon16 = htons(htlc->icon);
	if (login) {
		llen = strlen(login);
		if (llen > 64)
			llen = 64;
		hl_encode(enclogin, login, llen);
	} else
		llen = 0;

	/* Phase 5: legacy login send. hl_encode runs on the worker
	 * (it's a pure transformation on stack buffers), then we
	 * sync-invoke the actual hlwrite on the main thread. encpass
	 * == NULL on the no-password path; login_dispatch picks the
	 * right packet shape from that. */
	if (pass) {
		plen = strlen(pass);
		if (plen > 64)
			plen = 64;
		hl_encode(encpass, pass, plen);
	} else {
		plen = 0;
	}
	{
		struct login_args la = {
			.htlc = htlc,
			.icon16 = icon16,
			.enclogin = (const guint8 *)enclogin,
			.llen = llen,
			.encpass = pass ? (const guint8 *)encpass : NULL,
			.plen = plen,
		};
		gtkhx_invoke_sync (login_dispatch, &la);
	}

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

	pthread_create(&conn_tid, &attr, (void *)&hx_thread_connect,
				   (void *)cdata);
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

/* The post_* marshal helpers used here (post_prog, post_ts, post_log)
 * are defined above hx_thread_connect so both worker paths can share
 * them. See the block comment there for the design rationale. */

void hx_tracker_list(session *sess, char *serverstr, guint16 port)
{
	int s;
	guint16 nusers, nservers;
	unsigned char buf[HOSTLEN];
	char name[512], desc[512];
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

	g_snprintf(portstr, sizeof(portstr), "%u", port);
#else
	struct sockaddr_in saddr;

	memset(&saddr, 0, sizeof(struct sockaddr_in));
	saddr.sin_port = htons(port);
	saddr.sin_family = AF_INET;
#endif

	post_prog(trackconn_prog_update, sess, serverstr, 0, 2);

#ifndef USE_IPV6
	if(!inet_pton(AF_INET, serverstr, &saddr.sin_addr)) {
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
#ifdef USE_IPV6
				post_log(&the_session.htlc, 0, INFOPREFIX,
				         "%s: %s\n", serverstr, gai_strerror(error));
#else
# ifdef HAVE_HSTRERROR
				post_log(&the_session.htlc, 0, INFOPREFIX,
				         _("DNS lookup for %s failed: %s\n"),
				         serverstr, hstrerror(h_errno));
# else
				post_log(&the_session.htlc, 0, INFOPREFIX,
				         _("DNS lookup for %s failed\n"), serverstr);
# endif
#endif
				post_prog(trackconn_prog_update, sess, serverstr, 2, 2);
				return;
			}
#ifndef USE_IPV6
		}
#endif

		post_prog(trackconn_prog_update, sess, serverstr, 1, 2);

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
			post_log(&the_session.htlc, 0, INFOPREFIX,
			         _("tracker: %s\n"), strerror(errno));
			post_prog(trackconn_prog_update, sess, serverstr, 2, 2);
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
		post_log(&the_session.htlc, 0, INFOPREFIX,
		         _("tracker: %s: %s"),
		         serverstr, strerror(errno));
		post_prog(trackconn_prog_update, sess, serverstr, 2, 2);
		return;
	}

	post_prog(trackconn_prog_update, sess, serverstr, 2, 2);

	if (write(s, HTRK_MAGIC, HTRK_MAGIC_LEN) != HTRK_MAGIC_LEN)
		goto funk_dat;

	/* Phase 5: this and the (0, total) call below were previously
	 * called bare — touching GTK widgets from the worker thread
	 * without even the recursive-mutex shim around them. Marshalled
	 * now like every other UI call in this function. */
	post_prog(track_prog_update, sess, serverstr, 0, 0);

	if (b_read(s, buf, 14) != 14) {
		goto funk_dat;
	}


	nservers = ntohs(*((guint16 *)(&(buf[10]))));
	total = nservers;
	post_prog(track_prog_update, sess, serverstr, 0, total);
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

		post_ts(a, port, nusers,
		        (const char *)name, (const char *)desc, total);
		post_prog(track_prog_update, sess, serverstr, i, total);
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
	va_list ap, ap_trace;
	guint32 this_off, len;

	if (!htlc->fd) {
		return;
	}

	this_off = htlc->out.pos + htlc->out.len;

	/* Phase 5: the buffer-packing logic lives in hlpack
	 * (proto_helpers.c) so the Tier 2 unit tests can drive the
	 * SEND path without the fd / proto_trace / compress / cipher
	 * side effects. proto_trace stays here in hlwrite — it needs
	 * the per-chunk hook *during* the walk, which is awkward to
	 * expose through the va_list interface. We re-walk the args
	 * for the trace; the pack itself only happens once.
	 *
	 * Trans ID for the trace: it's whatever htlc->trans was BEFORE
	 * hlpack increments it. */
	guint32 my_trans = htlc->trans;
	proto_trace_send_begin (type, my_trans, hc);

	va_start (ap, hc);
	va_copy (ap_trace, ap);
	hlpack (htlc, type, flag, hc, ap);
	va_end (ap);

	{
		int hc_trace = hc;
		while (hc_trace) {
			guint16 t = (guint16) va_arg (ap_trace, int);
			guint16 l = (guint16) va_arg (ap_trace, int);
			guint8 *data = va_arg (ap_trace, guint8 *);
			proto_trace_send_chunk (t, l, data);
			hc_trace--;
		}
	}
	va_end (ap_trace);
	proto_trace_send_end ();

	/* Length of the packed message (header + chunks), used by the
	 * cipher / compress hooks below. */
	len = (htlc->out.pos + htlc->out.len) - this_off;

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
