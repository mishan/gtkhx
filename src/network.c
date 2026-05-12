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
#include <gio/gio.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/select.h>
#include <time.h>
#include <netinet/in.h>

#include <signal.h>
#include <stdarg.h>

#include "hx.h"
#include "gtkhx_session.h"
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

/* The Hotline file-transfer (HTXF) code in banner.c and rcv.c reads
 * htlc->addr to learn which host:port to open the subchannel against.
 * Under USE_IPV6 that field is `struct addrinfo *` (legacy contract:
 * banner.c reads ai_addr / ai_addrlen / ai_family / ai_canonname).
 * After the async-connect rewrite there's no getaddrinfo() to give
 * us one of those, so we synthesise one over the GSocketConnection's
 * remote address. conn_addr owns the backing storage (sockaddr +
 * canonname) and an addrinfo header pointing into it; clear_conn_addr
 * frees the lot. */
struct conn_addr {
#ifdef USE_IPV6
	struct addrinfo  ai;
#endif
	struct sockaddr_storage sa;
	socklen_t         salen;
	char              canonname[NI_MAXHOST];
};

/* Phase 5+ (async connect): pthread_t conn_tid is gone. The connect
 * + magic-exchange flow runs on the main loop via GSocketClient's
 * async API; cancellation goes through current_cancel. */
static GSocketConnection *current_conn;  /* owns the post-handshake fd */
static GCancellable      *current_cancel;
static struct conn_addr  *current_addr;

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
	char buf[HOSTLEN];

	session *sess = &the_session;

	ping_stop ();
	rcv_login_reset ();
	banner_clear ();

	/* Reset the per-session login flag so the next connect starts
	 * fresh. concurrence() reads this to decide whether to send
	 * AGREEMENTAGREE; a stale 1 from a previous session would skip
	 * the legacy flow on the next connect. */
	htlc->flags.logged_in = 0;

	/* Cancel any in-flight async connect (DNS / TCP-connect / magic
	 * exchange). Safe to call whether or not one's running. */
	if (current_cancel) {
		g_cancellable_cancel (current_cancel);
		g_clear_object (&current_cancel);
	}
#ifdef USE_IPV6
	if (htlc->addr && htlc->addr->ai_addr) {
		getnameinfo(htlc->addr->ai_addr, htlc->addr->ai_addrlen, buf,
					sizeof(buf), NULL, 0, NI_NUMERICHOST);
	} else {
		g_strlcpy (buf, "?", sizeof (buf));
	}
#else
	inet_ntop(AF_INET, &htlc->addr.sin_addr, buf, sizeof(buf));
#endif
	hx_printf_prefix(htlc, 0, INFOPREFIX, "%s: %s\n", buf,

					 _("connection closed"));

	if(!expected)
		error_dialog("Error", "You have been disconnected.");
	
	connected = 0;
	gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
	                                     GTKHX_CONNECTION_DISCONNECTED);
	close_connected_windows (sess);
	hxd_fd_clr(fd, FDR|FDW);

	/* Phase 5+: GSocketConnection owns the fd; releasing it closes
	 * the socket. Replaces the legacy close(fd) call. */
	g_clear_object (&current_conn);
	g_clear_pointer (&current_addr, g_free);
#ifdef USE_IPV6
	htlc->addr = NULL;
#else
	memset (&htlc->addr, 0, sizeof (htlc->addr));
#endif

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


	/* Phase 5+: chats live in a GHashTable<u32 cid, struct chat*>.
	 * For each chat:
	 *   1. Clear the UI's user-list rendering (users_clear is a no-op
	 *      on cid != 0 since the global user-list widget only shows
	 *      the public chat's members).
	 *   2. The hashtable's value-destroy notify (chat_free in chat.c)
	 *      reclaims the chat's hx_user nodes + the struct chat itself
	 *      on remove; we do not need to walk + free users by hand.
	 * The public chat (cid=0) must stay alive across reconnects, so
	 * we remove all *non-public* chats and then reset the public
	 * chat's user-list pointers + subject in-place. */
	if (sess->chats) {
		GHashTableIter iter;
		gpointer key, val;
		GList *non_public = NULL;
		g_hash_table_iter_init (&iter, sess->chats);
		while (g_hash_table_iter_next (&iter, &key, &val)) {
			struct chat *chat = val;
			gtkhx_session_emit_users_clear (
				gtkhx_session_get_default (), htlc, chat);
			if (GPOINTER_TO_UINT (key) != 0) {
				non_public = g_list_prepend (non_public, key);
			} else {
				/* Public chat: clear the per-chat users
				 * hashtable in place. The struct chat itself
				 * stays in the session->chats table; the users
				 * table's value-destroy notify reclaims each
				 * hx_user. */
				if (chat->users)
					g_hash_table_remove_all (chat->users);
				chat->nusers = 0;
				chat->subject[0] = '\0';
			}
		}
		for (GList *l = non_public; l; l = l->next)
			g_hash_table_remove (sess->chats, l->data);
		g_list_free (non_public);
	}

	/* Phase 5+: tasks live in a GHashTable<u32 trans, struct task*>.
	 * Use foreach_remove so we can run the per-task UI cleanup
	 * (gtask_delete_tsk) and let the table's value-destroy notify
	 * (task_free) reclaim the task struct itself. Safe to call on a
	 * table currently being iterated. */
	if (sess->tasks) {
		GHashTableIter iter;
		gpointer key;
		g_hash_table_iter_init (&iter, sess->tasks);
		while (g_hash_table_iter_next (&iter, &key, NULL)) {
			gtask_delete_tsk (sess, GPOINTER_TO_UINT (key));
		}
		g_hash_table_remove_all (sess->tasks);
	}


	/* Phase 5+: htlc->addr is now backed by current_addr (allocated
	 * fresh by the async-connect finalize step from the
	 * GSocketConnection's remote address); freed above with
	 * g_clear_pointer (&current_addr, g_free). The legacy
	 * freeaddrinfo() call belonged here when getaddrinfo() supplied
	 * the addrinfo. */

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
		hx_printf_prefix(htlc, 0, INFOPREFIX, "GZIP deflate: in: %lu  out: %lu\n",
				 (unsigned long) htlc->gzip_deflate_total_in,
				 (unsigned long) htlc->gzip_deflate_total_out);
		compress_encode_end(htlc);
	}
	if (htlc->compress_decode_type != COMPRESS_NONE) {
		hx_printf_prefix(htlc, 0, INFOPREFIX, "GZIP inflate: in: %lu  out: %lu\n",
				 (unsigned long) htlc->gzip_inflate_total_in,
				 (unsigned long) htlc->gzip_inflate_total_out);
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
				gtkhx_session_emit_task_update (gtkhx_session_get_default (), &the_session, tsk);
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
		hx_printf_prefix(htlc, 0, INFOPREFIX, "htlc_read: %zd %s\n", (ssize_t) r, strerror(errno));
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
		hx_printf_prefix(htlc, 0, INFOPREFIX, "htlc_write: %zd %s\n", (ssize_t) r, strerror(errno));
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

/*
 * Worker → main thread marshal helpers.
 *
 * Each post_* allocates a job, deep-copies any caller-owned strings
 * out of the worker's stack, and queues a one-shot idle on the
 * default main context. The dispatcher runs the original UI function
 * on the main thread, frees the job, returns G_SOURCE_REMOVE.
 *
 * Used by hx_tracker_list (tracker fetch flow). The hx_connect path
 * used to share these — that flow is now async on the main loop
 * (see further down), so si_job / i_job / tn_job / login_*_args /
 * htlc_init_args are gone alongside it. See gtkthreads.h for the
 * thread-contract rationale.
 */

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
	gtkhx_session_emit_tracker_server_create (
		gtkhx_session_get_default (),
		j->addr, j->port, j->nusers, j->name, j->desc, j->total);
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

/* Phase 5+: async connect via GSocketClient.
 *
 * Replaces the legacy pthread worker (hx_thread_connect) that did
 * blocking DNS / connect / magic-exchange / login-send off the main
 * thread, then handed the fd back via gtkhx_invoke_sync. The new
 * flow is a chain of GSocketClient / GInputStream / GOutputStream
 * async callbacks on the GMainContext default loop:
 *
 *   hx_connect
 *     → g_socket_client_connect_to_host_async  (DNS + TCP connect)
 *       → on_async_connected
 *           write HTLC_MAGIC via g_output_stream_write_all_async
 *           → on_magic_sent
 *               read HTLS_MAGIC via g_input_stream_read_all_async,
 *               arm a 30 s magic-timeout cancellable
 *               → on_magic_received
 *                   validate magic, init htlc qbuf state, populate
 *                   htlc->addr from GSocketAddress, install the fd
 *                   watch, send LOGIN, free the ctx.
 *
 * Cancellation goes through current_cancel — hx_htlc_close
 * cancels it, which short-circuits any in-flight async op and
 * lands in connect_fail on the next callback.
 *
 * No worker thread, no gtkhx_invoke_sync, no post_session_int /
 * post_int / post_task_new — the legacy login flow used those to
 * marshal back to main; in the async version we're already on main
 * inside every callback. */
typedef enum {
	GTKHX_CONNECT_STATE_IDLE,
	GTKHX_CONNECT_STATE_RESOLVING,
	GTKHX_CONNECT_STATE_WRITING_MAGIC,
	GTKHX_CONNECT_STATE_READING_MAGIC,
	GTKHX_CONNECT_STATE_DONE,
} GtkhxConnectState;

struct gtkhx_connect_ctx {
	struct htlc_conn *htlc;
	char *serverstr;
	char *login;
	char *pass;
	int secure;
	guint16 port;

	GSocketConnection *conn;
	GCancellable *cancel;
	guint magic_timeout_id;

	char magic[HTLS_MAGIC_LEN];

	GtkhxConnectState state;
};

#define MAGIC_TIMEOUT_SEC 30

static void connect_ctx_free (struct gtkhx_connect_ctx *ctx);
static void connect_fail     (struct gtkhx_connect_ctx *ctx,
                              const char *stage, GError *err);
static void send_login       (struct gtkhx_connect_ctx *ctx);

/* Populate htlc->addr from the GSocketConnection's remote endpoint.
 * banner.c / rcv.c read these fields to drive HTXF (file transfer)
 * subchannel connects on server_port+1; their contract is that
 * htlc->addr is a USE_IPV6 addrinfo* (or a sockaddr_in in the legacy
 * build) populated with the same server we just connected to. */
static gboolean
populate_htlc_addr (struct htlc_conn *htlc, GSocketConnection *conn,
                    const char *hostname)
{
	GSocketAddress *remote;
	gssize nlen;
	struct conn_addr *a;

	remote = g_socket_connection_get_remote_address (conn, NULL);
	if (!remote)
		return FALSE;

	a = g_new0 (struct conn_addr, 1);
	nlen = g_socket_address_get_native_size (remote);
	if (nlen <= 0 || (gsize) nlen > sizeof (a->sa)) {
		g_object_unref (remote);
		g_free (a);
		return FALSE;
	}
	if (!g_socket_address_to_native (remote, &a->sa, nlen, NULL)) {
		g_object_unref (remote);
		g_free (a);
		return FALSE;
	}
	a->salen = (socklen_t) nlen;
	g_strlcpy (a->canonname, hostname ? hostname : "?",
	           sizeof (a->canonname));
	g_object_unref (remote);

#ifdef USE_IPV6
	a->ai.ai_family   = ((struct sockaddr *) &a->sa)->sa_family;
	a->ai.ai_socktype = SOCK_STREAM;
	a->ai.ai_protocol = IPPROTO_TCP;
	a->ai.ai_addr     = (struct sockaddr *) &a->sa;
	a->ai.ai_addrlen  = a->salen;
	a->ai.ai_canonname = a->canonname;
	htlc->addr = &a->ai;
#else
	memcpy (&htlc->addr, &a->sa, sizeof (htlc->addr));
#endif

	g_clear_pointer (&current_addr, g_free);
	current_addr = a;
	return TRUE;
}

static gboolean
magic_timeout_cb (gpointer data)
{
	struct gtkhx_connect_ctx *ctx = data;

	ctx->magic_timeout_id = 0;
	if (ctx->cancel)
		g_cancellable_cancel (ctx->cancel);
	return G_SOURCE_REMOVE;
}

static void
connect_fail (struct gtkhx_connect_ctx *ctx, const char *stage,
              GError *err)
{
	struct htlc_conn *htlc = ctx->htlc;

	/* Cancelled by the caller (e.g. user clicked Disconnect or
	 * a re-connect arrived): don't toast a noisy log. */
	if (err && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		connect_ctx_free (ctx);
		return;
	}

	hx_printf_prefix (htlc, 0, INFOPREFIX, "%s: %s\n",
	                  stage, err ? err->message : _("failed"));
	gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
	                                     GTKHX_CONNECTION_DISCONNECTED);
	connect_ctx_free (ctx);
}

static void
connect_ctx_free (struct gtkhx_connect_ctx *ctx)
{
	if (ctx->magic_timeout_id) {
		g_source_remove (ctx->magic_timeout_id);
		ctx->magic_timeout_id = 0;
	}
	g_clear_object (&ctx->conn);
	g_clear_object (&ctx->cancel);
	/* current_cancel is a borrowed ref to ctx->cancel; clear it
	 * so hx_htlc_close (or a subsequent hx_connect) doesn't try
	 * to cancel a dead cancellable. */
	g_clear_object (&current_cancel);
	g_free (ctx->serverstr);
	g_free (ctx->login);
	g_free (ctx->pass);
	g_free (ctx);
}

static void
on_magic_received (GObject *source, GAsyncResult *res, gpointer data)
{
	struct gtkhx_connect_ctx *ctx = data;
	GError *err = NULL;
	gsize got = 0;

	if (ctx->magic_timeout_id) {
		g_source_remove (ctx->magic_timeout_id);
		ctx->magic_timeout_id = 0;
	}

	if (!g_input_stream_read_all_finish (
	        G_INPUT_STREAM (source), res, &got, &err)) {
		connect_fail (ctx, _("reading server magic"), err);
		g_clear_error (&err);
		return;
	}
	if (got != HTLS_MAGIC_LEN
	    || strncmp (HTLS_MAGIC, ctx->magic, HTLS_MAGIC_LEN) != 0) {
		connect_fail (ctx, _("invalid hotline server"), NULL);
		return;
	}

	send_login (ctx);
}

static void
on_magic_sent (GObject *source, GAsyncResult *res, gpointer data)
{
	struct gtkhx_connect_ctx *ctx = data;
	GError *err = NULL;
	gsize wrote = 0;
	GInputStream *in;

	if (!g_output_stream_write_all_finish (
	        G_OUTPUT_STREAM (source), res, &wrote, &err)) {
		connect_fail (ctx, _("writing client magic"), err);
		g_clear_error (&err);
		return;
	}

	ctx->state = GTKHX_CONNECT_STATE_READING_MAGIC;
	in = g_io_stream_get_input_stream (G_IO_STREAM (ctx->conn));
	ctx->magic_timeout_id =
		g_timeout_add_seconds (MAGIC_TIMEOUT_SEC,
		                       magic_timeout_cb, ctx);
	g_input_stream_read_all_async (in, ctx->magic, HTLS_MAGIC_LEN,
	                               G_PRIORITY_DEFAULT, ctx->cancel,
	                               on_magic_received, ctx);
}

static void
on_async_connected (GObject *source, GAsyncResult *res, gpointer data)
{
	struct gtkhx_connect_ctx *ctx = data;
	GError *err = NULL;
	GSocketConnection *conn;
	GOutputStream *out;

	conn = g_socket_client_connect_to_host_finish (
		G_SOCKET_CLIENT (source), res, &err);
	if (!conn) {
		connect_fail (ctx, _("connect"), err);
		g_clear_error (&err);
		return;
	}
	ctx->conn = conn;
	ctx->state = GTKHX_CONNECT_STATE_WRITING_MAGIC;

	gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
	                                     GTKHX_CONNECTION_TCP_CONNECTED);
	hx_printf_prefix (ctx->htlc, 0, INFOPREFIX,
	                  _("connected to %s\n"), server_addr);

	out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
	g_output_stream_write_all_async (out, HTLC_MAGIC, HTLC_MAGIC_LEN,
	                                 G_PRIORITY_DEFAULT, ctx->cancel,
	                                 on_magic_sent, ctx);
}

/* Finalize the connect after the magic round-trip succeeds: stash
 * the GSocketConnection on the current_conn global (it owns the
 * fd; hx_htlc_close unrefs it), populate htlc->addr from the
 * remote endpoint, initialise the qbuf state, install the fd watch
 * via the existing hxd_files / hxd_fd_set machinery, and send the
 * LOGIN packet.
 *
 * The secure-login flag in ctx->secure picks between the HOPE
 * MAC-ALG / CIPHER-ALG / COMPRESS-ALG negotiation packet and the
 * legacy plaintext LOGIN. */
static void
send_login (struct gtkhx_connect_ctx *ctx)
{
	struct htlc_conn *htlc = ctx->htlc;
	GSocket *sock;
	int s;
	char enclogin[64], encpass[64];
	guint16 icon16, llen, plen;

	if (!populate_htlc_addr (htlc, ctx->conn, ctx->serverstr)) {
		connect_fail (ctx, _("remote address"), NULL);
		return;
	}

	sock = g_socket_connection_get_socket (ctx->conn);
	s = g_socket_get_fd (sock);

	/* Stash the GSocketConnection so it stays alive past ctx_free —
	 * hx_htlc_close unrefs current_conn, which closes the fd. */
	g_clear_object (&current_conn);
	current_conn = g_object_ref (ctx->conn);

	/* Initialise htlc state. */
	htlc->fd = s;
	htlc->trans = 1;
	memset (&htlc->in,  0, sizeof (struct qbuf));
	memset (&htlc->out, 0, sizeof (struct qbuf));
	htlc->rcv = hx_rcv_hdr;
	qbuf_set (&htlc->in, 0, SIZEOF_HL_HDR);

	gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
	                                     GTKHX_CONNECTION_HANDSHAKE_DONE);

	set_nonblocking (s);
	fd_closeonexec (s, 1);

	hxd_files[s].ready_read = htlc_read;
	hxd_files[s].ready_write = htlc_write;
	hxd_files[s].conn.htlc = htlc;
	hxd_files[s].fd = s;

	connected = 1;
	htlc->gdk_input = 1;
	hxd_fd_set (s, FDR);

	if (ctx->login)
		strcpy (htlc->login, ctx->login);

	if (ctx->secure) {
#ifdef CONFIG_CIPHER
		guint8 cipheralglist[64];
		guint16 cipheralglistlen;
		guint8 cipherlen;
#endif
#ifdef CONFIG_COMPRESS
		guint8 compressalglist[64];
		guint16 compressalglistlen;
		guint8 compresslen;
#endif
		guint16 hc;
		guint8 macalglist[64];
		guint16 macalglistlen;
		const guint8 zero = 0;

		task_new (htlc, RCV_TASK_FN (rcv_task_login),
		          ctx->pass ? g_strdup (ctx->pass)
		                    : g_strdup (""),
		          0, "login");

		strcpy (htlc->macalg, "HMAC-SHA1");
		{
			guint16 val = 2;
			HN16 (macalglist, &val);
		}
		macalglistlen = 2;
		macalglist[macalglistlen++] = 9;
		memcpy (macalglist + macalglistlen, htlc->macalg, 9);
		macalglistlen += 9;
		macalglist[macalglistlen++] = 8;
		memcpy (macalglist + macalglistlen, "HMAC-MD5", 8);
		macalglistlen += 8;

		hc = 4;
#ifdef CONFIG_COMPRESS
		if (htlc->compressalg[0]) {
			compresslen = strlen (htlc->compressalg);
			{
				guint16 val = 1;
				HN16 (compressalglist, &val);
			}
			compressalglistlen = 2;
			compressalglist[compressalglistlen++] = compresslen;
			memcpy (compressalglist + compressalglistlen,
			        htlc->compressalg, compresslen);
			compressalglistlen += compresslen;
			hc++;
		} else
			compressalglistlen = 0;
#endif
#ifdef CONFIG_CIPHER
		if (htlc->cipheralg[0]) {
			cipherlen = strlen (htlc->cipheralg);
			{
				guint16 val = 1;
				HN16 (cipheralglist, &val);
			}
			cipheralglistlen = 2;
			cipheralglist[cipheralglistlen++] = cipherlen;
			memcpy (cipheralglist + cipheralglistlen,
			        htlc->cipheralg, cipherlen);
			cipheralglistlen += cipherlen;
			hc++;
		} else
			cipheralglistlen = 0;
#endif
		hlwrite (htlc, HTLC_HDR_LOGIN, 0, hc,
		    HTLC_DATA_LOGIN,    1, &zero,
		    HTLC_DATA_PASSWORD, 1, &zero,
		    HTLC_DATA_MAC_ALG,  macalglistlen, macalglist,
#ifdef CONFIG_CIPHER
		    HTLC_DATA_CIPHER_ALG,   cipheralglistlen,   cipheralglist,
#endif
#ifdef CONFIG_COMPRESS
		    HTLC_DATA_COMPRESS_ALG, compressalglistlen, compressalglist,
#endif
		    HTLC_DATA_SESSIONKEY, 0, 0);
	} else {
		guint16 cv16;

		task_new (htlc, RCV_TASK_FN (rcv_task_login), 0, 0, "login");

		icon16 = htons (htlc->icon);
		if (ctx->login) {
			llen = strlen (ctx->login);
			if (llen > 64) llen = 64;
			hl_encode (enclogin, ctx->login, llen);
		} else
			llen = 0;

		if (ctx->pass && *ctx->pass) {
			plen = strlen (ctx->pass);
			if (plen > 64) plen = 64;
			hl_encode (encpass, ctx->pass, plen);
		} else {
			plen = 0;
		}

		/* Advertise ourselves as Hotline 1.8.5 (185). Matches what
		 * the integration test harness sends; bumps mhxd's
		 * can_ping bit so HTLC_HDR_PING keepalives are accepted. */
		cv16 = htons (185);
		if (plen) {
			hlwrite (htlc, HTLC_HDR_LOGIN, 0, 4,
			    HTLC_DATA_ICON,          2,    &icon16,
			    HTLC_DATA_LOGIN,         llen, enclogin,
			    HTLC_DATA_PASSWORD,      plen, encpass,
			    HTLC_DATA_CLIENTVERSION, 2,    &cv16);
		} else {
			hlwrite (htlc, HTLC_HDR_LOGIN, 0, 3,
			    HTLC_DATA_ICON,          2,    &icon16,
			    HTLC_DATA_LOGIN,         llen, enclogin,
			    HTLC_DATA_CLIENTVERSION, 2,    &cv16);
		}
	}

	ctx->state = GTKHX_CONNECT_STATE_DONE;
	connect_ctx_free (ctx);
}

void hx_connect (struct htlc_conn *htlc, const char *serverstr,
                 guint16 port, const char *login, const char *pass,
                 char secure)
{
	struct gtkhx_connect_ctx *ctx;
	GSocketClient *client;

	/* Cancel any in-flight async connect (user kicked off another
	 * connect, or the previous one is still resolving). */
	if (current_cancel) {
		g_cancellable_cancel (current_cancel);
		g_clear_object (&current_cancel);
	}

	if (htlc->fd) {
		hx_htlc_close (htlc, 1);
	}
	hx_clear_chat (htlc, 0, 1);

	g_free (server_addr);
	server_addr = g_strdup_printf ("%s:%u", serverstr, port);
#ifdef USE_IPV6
	server_port = port;
#endif

#if 0 /* XXX */
	server_log = create_log(server_addr);
#endif

	ctx = g_new0 (struct gtkhx_connect_ctx, 1);
	ctx->htlc = htlc;
	ctx->serverstr = g_strdup (serverstr);
	ctx->port = port;
	ctx->login = g_strdup (login);
	ctx->pass  = g_strdup (pass);
	ctx->secure = secure;
	ctx->cancel = g_cancellable_new ();
	current_cancel = g_object_ref (ctx->cancel);
	ctx->state = GTKHX_CONNECT_STATE_RESOLVING;

	htlc->gdk_input = 0;
	hx_printf_prefix (htlc, 0, INFOPREFIX,
	                  _("connecting to %s\n"), server_addr);
	gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
	                                     GTKHX_CONNECTION_CONNECTING);

	client = g_socket_client_new ();
	/* GSocketClient defaults already prefer IPv4 if both are
	 * available and try each resolved address in turn — same fallback
	 * behaviour the legacy getaddrinfo loop had. */
	g_socket_client_set_timeout (client, MAGIC_TIMEOUT_SEC);
	g_socket_client_connect_to_host_async (client, serverstr, port,
	                                       ctx->cancel,
	                                       on_async_connected, ctx);
	g_object_unref (client);
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
 * are defined further up. The tracker fetch stays on a pthread worker
 * (it does its own blocking IO + dfa regex scan over the server
 * list); the async-connect rewrite for hx_connect didn't touch it. */

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

void kill_threads (void)
{
	/* Phase 5+: cancel the async connect chain. Safe whether or not
	 * one's in flight. */
	if (current_cancel) {
		g_cancellable_cancel (current_cancel);
		g_clear_object (&current_cancel);
	}
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
