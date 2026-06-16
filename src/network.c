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
#include "tls_trust.h"
#include "tls_trust_dialog.h"
#include "toolbar.h"
#include "tracker.h"
#include "network.h"
#include "banner.h"
#include "debug.h"
#include "cipher_aead.h"
#include "cipher.h"
#include "voice_runtime.h"
#include "voice_model.h"

/* Phase R1: Rust FFI for the Blowfish OFB-64 state — only used here
 * to free the state on hx_htlc_close. cipher.c owns the alloc and
 * crypt FFI. */
extern void gtkhx_blowfish_ofb64_free (BlowfishOfb64State *state);
#include "login_packet.h"
#include "agreement_packet.h"
#include "hl_code.h"
#include "proto_helpers.h"
#include "htxf_subchannel.h"
#include "network_decode.h"
#include "tracker_parser.h"
#include "tracker_v3.h"
#include "tracker_event.h"
#include "connect_magic.h"

char *server_addr;
guint16 server_port;

#if 0 /* XXX */
struct log *server_log = NULL;
#endif

/* Phase 5+ (async connect): pthread_t conn_tid is gone. The connect
 * + magic-exchange flow runs on the main loop via GSocketClient's
 * async API; cancellation goes through current_cancel. */
static GSocketConnection *current_conn; /* owns the post-handshake fd */
static GCancellable *current_cancel;

/* Forward decls for the control-channel GPollable source helpers.
 * Definitions live further down (alongside the read/write
 * callbacks) but hx_htlc_close above needs to call
 * control_remove_all_sources before they're defined. */
static void control_arm_read_source (struct htlc_conn *htlc);
static void control_arm_write_source (struct htlc_conn *htlc);
static void control_remove_write_source (void);
static void control_remove_all_sources (void);

int connected;

int
fd_closeonexec (int fd, int on)
{
    int x;

    if ((x = fcntl (fd, F_GETFD, 0)) == -1) {
        return -1;
    }
    if (on) {
        x &= ~FD_CLOEXEC;
    } else {
        x |= FD_CLOEXEC;
    }

    return fcntl (fd, F_SETFD, x);
}

int
fd_lock_write (int fd)
{
    struct flock lk;

    lk.l_type = F_WRLCK;
    lk.l_start = 0;
    lk.l_whence = SEEK_SET;
    lk.l_len = 0;
    lk.l_pid = getpid ();

    return fcntl (fd, F_SETLK, &lk);
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
    /* Phase R2: PING is a zero-chunk opcode. Send directly through
	 * hlwrite_chunks with hc=0 so the trace path matches the rest of
	 * the SEND opcodes (no fallback to the variadic hlwrite). */
    hlwrite_chunks (htlc, HTLC_HDR_PING, 0, NULL, 0);
    return G_SOURCE_CONTINUE;
}

void
ping_start (struct htlc_conn *htlc)
{
    if (ping_timer_id || !htlc || !htlc->fd) {
        return;
    }
    ping_timer_id = g_timeout_add_seconds (PING_INTERVAL_SEC, ping_tick, htlc);
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
    /* Same reset for the "we've reached the spec-correct fully-
	 * joined boundary" flag — see hx_post_login_fetches in rcv.c
	 * and the comment on the flag in protocol.h. The files browser's
	 * remote provider reads this to know when FILE_LIST is safe to
	 * send. */
    htlc->flags.post_login_fetched = 0;

    /* Same idea for the DATA_CAPABILITIES bitmask — the next
	 * connect renegotiates from zero. A stale CAP_TEXT_ENCODING
	 * bit could otherwise survive a reconnect to a Mac Roman
	 * server and cause us to skip text transcoding once Phase E2
	 * lands. */
    htlc->caps = 0;
    /* Chat-history retention hints from the LOGIN reply — wiped
	 * on disconnect so a reconnect to a server with different
	 * retention doesn't carry stale numbers into the UI. */
    htlc->history_max_msgs = 0;
    htlc->history_max_days = 0;
    /* Inline-media advisory limits from the LOGIN reply — same
	 * reasoning. The accessors in src/inline_media.h gate on
	 * CAP_INLINE_MEDIA being lit so callers see spec defaults
	 * when the new server doesn't echo the cap; this reset is
	 * defence-in-depth for any future path that reads the raw
	 * fields directly (and matches the pattern history_max_*
	 * uses one line up). */
    htlc->media_max_bytes = 0;
    htlc->media_max_dimension = 0;
    htlc->media_max_pixels = 0;
    htlc->media_chunk_size = 0;
    htlc->media_max_frames = 0;
    htlc->media_max_duration_ms = 0;

    /* Phase 8.D runtime wiring: tear down the voice runtime.
     * gtkhx_voice_runtime_free walks the state machine to its
     * Drop, which cancels armed timers, evicts the thread-local
     * registry entry, and releases the GStreamer pipeline +
     * webrtcbin. NULL-safe — the runtime is lazily-created so
     * sessions that never opened voice don't allocate one. The
     * `sess` local was bound at the top of this function. */
    if (sess->voice_runtime) {
        gtkhx_voice_runtime_free (sess->voice_runtime);
        sess->voice_runtime = NULL;
    }
    /* Clear the voice indicator model so a reconnect starts with
     * an empty user list (no lingering speaker indicators from a
     * dropped session). The model object itself stays alive — it's
     * subscribed by users_view via the long-lived chat / users
     * windows. */
    if (sess->voice_model) {
        hx_voice_model_clear (sess->voice_model);
    }

    /* Cancel any in-flight async connect (DNS / TCP-connect / magic
	 * exchange). Safe to call whether or not one's running. */
    if (current_cancel) {
        g_cancellable_cancel (current_cancel);
        g_clear_object (&current_cancel);
    }
    g_strlcpy (buf, htlc->ip_addr[0] ? htlc->ip_addr : "?", sizeof (buf));
    hx_printf_prefix (htlc, 0, INFOPREFIX, "%s: %s\n", buf,

                      _ ("connection closed"));

    if (!expected) {
        error_dialog ("Error", "You have been disconnected.");
    }

    connected = 0;
    gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
                                         GTKHX_CONNECTION_DISCONNECTED);
    close_connected_windows (sess);
    /* Detach the GPollable read/write sources before dropping
     * current_conn so the callbacks can't fire on a freed
     * stream. */
    control_remove_all_sources ();

    /* Phase 5+: GSocketConnection owns the fd; releasing it closes
	 * the socket. Replaces the legacy close(fd) call. */
    g_clear_object (&current_conn);
    htlc->ip_addr[0] = '\0';

    if (htlc->in.buf) {
        g_free (htlc->in.buf);
        htlc->in.buf = NULL;
    }
    if (htlc->out.buf) {
        g_free (htlc->out.buf);
        htlc->out.buf = NULL;
    }
    /* hxd_files[fd] no longer used for the control channel (the
     * GPollable sources installed in send_login replaced the
     * legacy hxd_fd_set GIOChannel watch); no slot to clear. */
    htlc->fd = 0;
    htlc->uid = 0;
    htlc->color = 0;
    /* Colored-Nicknames: nick_color is a per-session pref
	 * echo, not per-connection state. On connection teardown we
	 * deliberately re-seed (not reset) from gtkhx_prefs.nick_color so
	 * the next login's USER_CHANGE carries the user's preferred color
	 * out of the gate. Resetting to HX_NICK_COLOR_NONE here used to
	 * leave the value stuck — apply_loaded_xtext_prefs only fires at
	 * startup from prefs_read, so on Disconnect → Connect the htlc
	 * lost the color and only re-gained it when the user touched the
	 * Settings picker (which fires changed_nick_color). Re-seeding
	 * here both (a) avoids leaking a previous connection's
	 * server-side admin override into the next one (we overwrite with
	 * the local pref, which is independent of the prior server state)
	 * and (b) fixes the reconnect-loses-color bug. */
    htlc->nick_color = (guint32)gtkhx_prefs.nick_color;
    htlc->gdk_input = 0;
    htlc->version = 0;
    memset (htlc->login, 0, sizeof (htlc->login));

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
            gtkhx_session_emit_users_clear (gtkhx_session_get_default (), htlc,
                                            chat);
            if (GPOINTER_TO_UINT (key) != 0) {
                non_public = g_list_prepend (non_public, key);
            } else {
                /* Public chat: clear the per-chat users
				 * hashtable in place. The struct chat itself
				 * stays in the session->chats table; the users
				 * table's value-destroy notify reclaims each
				 * hx_user. */
                if (chat->users) {
                    g_hash_table_remove_all (chat->users);
                }
                chat->nusers = 0;
                chat->subject[0] = '\0';
            }
        }
        for (GList *l = non_public; l; l = l->next) {
            g_hash_table_remove (sess->chats, l->data);
        }
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

    /* Phase 5+ (HTXF rewrite): htlc no longer carries an addrinfo —
	 * the post-connect peer-identification now lives in plain
	 * htlc->serverhost / serverport / ip_addr fields, none of which
	 * need explicit teardown. The legacy freeaddrinfo() call (and
	 * the conn_addr shim that briefly replaced it) belonged here. */

    memset (htlc->cipher_encode_key, 0, sizeof (htlc->cipher_encode_key));
    memset (htlc->cipher_decode_key, 0, sizeof (htlc->cipher_decode_key));
    /* Phase R1: the Blowfish state lives behind an opaque pointer
     * allocated by rust/crates/hxcrypto-stream. Free it before
     * zeroing the union so the heap allocation isn't leaked.
     * AEAD state is inline in the union (no heap), so the memset
     * below still cleans that up. */
    if (htlc->cipher_encode_type == CIPHER_BLOWFISH
        && htlc->cipher_encode_state.stream) {
        gtkhx_blowfish_ofb64_free (
            (BlowfishOfb64State *) htlc->cipher_encode_state.stream);
    }
    if (htlc->cipher_decode_type == CIPHER_BLOWFISH
        && htlc->cipher_decode_state.stream) {
        gtkhx_blowfish_ofb64_free (
            (BlowfishOfb64State *) htlc->cipher_decode_state.stream);
    }
    memset (&htlc->cipher_encode_state, 0, sizeof (htlc->cipher_encode_state));
    memset (&htlc->cipher_decode_state, 0, sizeof (htlc->cipher_decode_state));
    htlc->cipher_encode_type = 0;
    htlc->cipher_decode_type = 0;
    htlc->cipher_encode_keylen = 0;
    htlc->cipher_decode_keylen = 0;
    htlc->cipher_mode = CIPHER_MODE_STREAM;
    /* Free the AEAD plaintext accumulator buffer if it grew. The
	 * struct itself stays zeroed for the next connection. */
    if (htlc->aead_plain.buf) {
        g_free (htlc->aead_plain.buf);
        memset (&htlc->aead_plain, 0, sizeof (htlc->aead_plain));
    }
    if (htlc->compress_encode_type != COMPRESS_NONE) {
        hx_printf_prefix (htlc, 0, INFOPREFIX,
                          "GZIP deflate: in: %lu  out: %lu\n",
                          (unsigned long)htlc->gzip_deflate_total_in,
                          (unsigned long)htlc->gzip_deflate_total_out);
        compress_encode_end (htlc);
    }
    if (htlc->compress_decode_type != COMPRESS_NONE) {
        hx_printf_prefix (htlc, 0, INFOPREFIX,
                          "GZIP inflate: in: %lu  out: %lu\n",
                          (unsigned long)htlc->gzip_inflate_total_in,
                          (unsigned long)htlc->gzip_inflate_total_out);
        compress_decode_end (htlc);
    }
    memset (&htlc->compress_encode_state, 0,
            sizeof (htlc->compress_encode_state));
    memset (&htlc->compress_decode_state, 0,
            sizeof (htlc->compress_decode_state));
    htlc->compress_encode_type = 0;
    htlc->compress_decode_type = 0;
    htlc->gzip_deflate_total_in = 0;
    htlc->gzip_deflate_total_out = 0;
    htlc->gzip_inflate_total_in = 0;
    htlc->gzip_inflate_total_out = 0;
    memset (htlc->sessionkey, 0, sizeof (htlc->sessionkey));
    htlc->sklen = 0;

#if 0 /* XXX */
	close_log(server_log);
	server_log = NULL;
#endif

    g_free (server_addr);
    server_addr = NULL;
}
/* Phase 5+ (HOPE ChaCha20-Poly1305): pump complete AEAD frames
 * from htlc->read_in into htlc->aead_plain. Returns the number
 * of plaintext bytes newly available in aead_plain (relative to
 * its pos). Authentication failure or oversized frame returns 0
 * and disconnects htlc (via hx_htlc_close); the caller treats
 * htlc->fd == 0 as "stop processing". */
/* hx_aead_pump_frames + hx_decode live in network_decode.c so the
 * Tier 2 test suite can drive them against canned bytes without
 * dragging in this file's async-connect / tracker / GTK pile. The
 * implementations are byte-for-byte unchanged from when they lived
 * here as static aead_pump_frames + decode. */

#define READ_BUFSIZE 0x4000

static void
update_task (struct htlc_conn *htlc)
{
    if (htlc->in.pos >= SIZEOF_HL_HDR) {
        struct hl_hdr *h = 0;
        u_int32_t off = 0;

        /* find the last packet */
        while (off + 20 <= htlc->in.pos) {
            h = (struct hl_hdr *)(&htlc->in.buf[off]);
            off += 20 + ntohl (h->len);
        }
        if (h && (ntohl (h->type) & 0xffffff) == HTLS_HDR_TASK) {
            struct task *tsk = task_with_trans (&the_session, ntohl (h->trans));
            if (tsk) {
                tsk->pos = htlc->in.pos;
                tsk->len = htlc->in.len;
                gtkhx_session_emit_task_update (gtkhx_session_get_default (),
                                                &the_session, tsk);
            }
        }
    }
}

/* ============================================================
 * Control-channel I/O via GPollable sources
 * ============================================================
 *
 * The control connection runs on top of current_conn's GIOStream.
 * Two watch sources, attached to the default main context:
 *
 *   control_read_src_id   permanent for the connection's life
 *                         (installed in send_login, removed in
 *                         hx_htlc_close).
 *   control_write_src_id  installed lazily when there's data in
 *                         htlc->out (control_arm_write_source),
 *                         removed when the buffer drains
 *                         (inside the writable callback).
 *
 * Each source fires on the main thread when the wrapped stream
 * has application-level read / write capacity — i.e. plaintext
 * bytes available, or decrypted TLS bytes ready for the read
 * side, and writable socket capacity (modulo any TLS-internal
 * buffering) for the write side.
 *
 * Replaces the previous hxd_fd_set GIOChannel-on-raw-fd watch
 * that drove the legacy htlc_read(int fd) / htlc_write(int fd)
 * callbacks. That watch fired on kernel-level socket activity,
 * which was a good approximation in plaintext but didn't see
 * TLS-buffered decrypted data (multi-record-per-segment case);
 * the GPollable sources are aware of that buffering and fire
 * at the right moment.
 *
 * hxd_files[] is still consumed by commands.c's /exec pipe
 * plumbing, so the array stays — we just don't populate it for
 * the control fd anymore. */
static guint control_read_src_id;
static guint control_write_src_id;

static gboolean control_on_readable (GObject *source, gpointer user_data);
static gboolean control_on_writable (GObject *source, gpointer user_data);

static void
control_arm_read_source (struct htlc_conn *htlc)
{
    GInputStream *in_stream;
    GSource *src;

    g_return_if_fail (current_conn != NULL);
    g_return_if_fail (control_read_src_id == 0);

    in_stream = g_io_stream_get_input_stream (G_IO_STREAM (current_conn));
    src = g_pollable_input_stream_create_source (
        G_POLLABLE_INPUT_STREAM (in_stream), NULL);
    /* G_SOURCE_FUNC silences -Wcast-function-type. The actual
     * callback signature is GPollableSourceFunc — same shape that
     * g_pollable_input_stream_create_source wires up internally. */
    g_source_set_callback (src, G_SOURCE_FUNC (control_on_readable),
                           htlc, NULL);
    control_read_src_id = g_source_attach (src, NULL);
    g_source_unref (src);
}

/* Idempotent: a hot send path that queues into htlc->out and
 * then calls control_arm_write_source from inside a callback
 * already holding the source can call this without paying
 * for a double-install. */
static void
control_arm_write_source (struct htlc_conn *htlc)
{
    GOutputStream *out_stream;
    GSource *src;

    if (!current_conn || control_write_src_id != 0) {
        return;
    }
    out_stream = g_io_stream_get_output_stream (G_IO_STREAM (current_conn));
    src = g_pollable_output_stream_create_source (
        G_POLLABLE_OUTPUT_STREAM (out_stream), NULL);
    g_source_set_callback (src, G_SOURCE_FUNC (control_on_writable),
                           htlc, NULL);
    control_write_src_id = g_source_attach (src, NULL);
    g_source_unref (src);
}

static void
control_remove_write_source (void)
{
    if (control_write_src_id) {
        g_source_remove (control_write_src_id);
        control_write_src_id = 0;
    }
}

static void
control_remove_all_sources (void)
{
    if (control_read_src_id) {
        g_source_remove (control_read_src_id);
        control_read_src_id = 0;
    }
    control_remove_write_source ();
}

/* Read bytes off the control connection's GIOStream. Returns:
 *   >0  : bytes read into buf
 *    0  : EOF — caller should close
 *   -1  : transient (would-block); caller leaves the buffer
 *         where it is and waits for the next source fire
 *   -2  : fatal stream error; caller closes (and logs the
 *         GError message so a TLS alert / expired cert /
 *         broken pipe surfaces to the user). */
static ssize_t
htlc_stream_read (struct htlc_conn *htlc, void *buf, size_t buflen)
{
    GInputStream *in_stream;
    GError *err = NULL;
    gssize r;

    if (!current_conn) {
        return -2;
    }
    in_stream = g_io_stream_get_input_stream (G_IO_STREAM (current_conn));
    r = g_pollable_input_stream_read_nonblocking (
        G_POLLABLE_INPUT_STREAM (in_stream), buf, buflen, NULL, &err);
    if (r < 0) {
        if (err && err->domain == G_IO_ERROR
            && err->code == G_IO_ERROR_WOULD_BLOCK) {
            g_clear_error (&err);
            return -1;
        }
        hx_printf_prefix (htlc, 0, INFOPREFIX, "stream read: %s\n",
                          err ? err->message : "unknown error");
        g_clear_error (&err);
        return -2;
    }
    return (ssize_t) r;
}

/* Readable callback. The GPollable source fires when application
 * data is available — which for TLS means "decrypted bytes
 * buffered or new ciphertext to decrypt."
 *
 * We loop read → decode → read inside one callback so a payload
 * larger than READ_BUFSIZE drains fully even when GTlsConnection
 * has decrypted bytes internally buffered: kernel readability
 * has already been spent on the first read, so the source won't
 * re-fire just because more decrypted bytes are sitting in the
 * TLS layer. Stopping after one read+decode pass would stall the
 * receive path on big payloads (banners over TLS, multi-record
 * news posts, large file-list replies).
 *
 * Termination: we exit the loop the moment a read returns
 * WOULD_BLOCK, EOF, or fatal — those are the three signals that
 * say "no more application data is going to materialise this
 * watch fire."
 */
static gboolean
control_on_readable (GObject *source G_GNUC_UNUSED, gpointer user_data)
{
    struct htlc_conn *htlc = user_data;
    struct qbuf *in = &htlc->read_in;
    ssize_t r;
    gboolean stream_drained = FALSE;

    for (;;) {
        if (!in->len) {
            qbuf_set (in, in->pos, READ_BUFSIZE);
            in->len = 0;
        }

        /* Skip the read if we don't have buffer space — hx_decode
         * below will consume some bytes and free capacity for the
         * next iteration. Avoids spurious WOULD_BLOCK calls when
         * the buffer is genuinely full.
         *
         * Index into &in->buf[in->pos], not in->pos + in->len:
         * hx_decode compacts any unprocessed bytes back to the
         * start of the buffer via memmove and sets both pos and
         * len to the count of leftover bytes, so in->pos IS the
         * end-of-data write offset. Adding len on top doubles
         * the offset and leaves a gap of uninitialised bytes
         * between the leftover frame data and the new read. */
        if (!stream_drained && in->len < READ_BUFSIZE) {
            r = htlc_stream_read (htlc, &in->buf[in->pos],
                                  READ_BUFSIZE - in->len);
            if (r == -1) {
                /* would-block — stream is fully drained for now.
                 * Continue into the decode loop to consume what
                 * we already have, then exit the outer loop. */
                stream_drained = TRUE;
            } else if (r <= 0) {
                hx_printf_prefix (htlc, 0, INFOPREFIX,
                                  "htlc_read: stream %s\n",
                                  r == 0 ? "EOF" : "error");
                hx_htlc_close (htlc, 0);
                return G_SOURCE_REMOVE;
            } else {
                in->len += r;
            }
        }

        /* Decode as many frames as we can with the buffer's
         * current contents. */
        gboolean decoded_any = FALSE;
        while (hx_decode (htlc)) {
            decoded_any = TRUE;
            update_task (htlc);
            if (htlc->rcv) {
                if (htlc->rcv == hx_rcv_hdr) {
                    hx_rcv_hdr (htlc);
                    /* hx_rcv_hdr may have closed the connection
                     * via an inner code path (e.g. a frame that
                     * triggered a tear-down in a task handler).
                     * current_conn is cleared by hx_htlc_close,
                     * so it's our liveness marker now that
                     * hxd_files no longer is. */
                    if (!current_conn) {
                        return G_SOURCE_REMOVE;
                    }
                } else {
                    /* Phase 5: body is fully buffered now;
                     * dump its data chunks before dispatch.
                     * No-op when "proto" debug category is
                     * disabled. */
                    proto_trace_recv_chunks (htlc);
                    htlc->rcv (htlc);
                    if (!current_conn) {
                        return G_SOURCE_REMOVE;
                    }
                    goto reset;
                }
            } else {
            reset:
                htlc->rcv = hx_rcv_hdr;
                qbuf_set (&htlc->in, 0, SIZEOF_HL_HDR);
            }
        }
        update_task (htlc);

        /* Exit when the stream said WOULD_BLOCK AND we made no
         * decode progress this iteration — either we have
         * partial frame data waiting for more bytes, or the
         * buffer was full but decode freed nothing. Both mean
         * there's nothing useful to do until the next source
         * fire. */
        if (stream_drained && !decoded_any) {
            break;
        }
    }
    return G_SOURCE_CONTINUE;
}

/* Mirror of htlc_stream_read for the outbound side. Same return
 * shape: >=0 bytes written, -1 transient, -2 fatal. */
static ssize_t
htlc_stream_write (struct htlc_conn *htlc, const void *buf, size_t buflen)
{
    GOutputStream *out_stream;
    GError *err = NULL;
    gssize r;

    if (!current_conn) {
        return -2;
    }
    out_stream = g_io_stream_get_output_stream (G_IO_STREAM (current_conn));
    r = g_pollable_output_stream_write_nonblocking (
        G_POLLABLE_OUTPUT_STREAM (out_stream), buf, buflen, NULL, &err);
    if (r < 0) {
        if (err && err->domain == G_IO_ERROR
            && err->code == G_IO_ERROR_WOULD_BLOCK) {
            g_clear_error (&err);
            return -1;
        }
        hx_printf_prefix (htlc, 0, INFOPREFIX, "stream write: %s\n",
                          err ? err->message : "unknown error");
        g_clear_error (&err);
        return -2;
    }
    return (ssize_t) r;
}

/* Writable callback. The GPollable output source is attached
 * by control_arm_write_source from the hlwrite path (whenever
 * data lands in htlc->out) and detaches itself when the buffer
 * drains. */
static gboolean
control_on_writable (GObject *source G_GNUC_UNUSED, gpointer user_data)
{
    struct htlc_conn *htlc = user_data;
    ssize_t r;

    r = htlc_stream_write (htlc, &htlc->out.buf[htlc->out.pos],
                           htlc->out.len);
    if (r == -1) {
        return G_SOURCE_CONTINUE; /* transient, keep source. */
    }
    if (r < 0) {
        hx_printf_prefix (htlc, 0, INFOPREFIX,
                          "htlc_write: stream error\n");
        hx_htlc_close (htlc, 0);
        return G_SOURCE_REMOVE;
    }
    htlc->out.pos += r;
    htlc->out.len -= r;
    if (!htlc->out.len) {
        htlc->out.pos = 0;
        htlc->out.len = 0;
        control_write_src_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* The post_* marshal helpers (post_prog / post_ts / post_log) lived
 * here until the tracker fetch went async (see hx_tracker_list_async
 * below). Their only consumer was the pthread tracker worker, and
 * the worker is gone now — every callback in the new design runs on
 * the main loop, so the trackconn_prog_update / track_prog_update /
 * gtkhx_session_emit_tracker_server_create / hx_printf_prefix calls
 * happen directly. */

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
    /* Phase E2: same as hx_change_name_icon — encode the nick to
	 * the negotiated wire encoding. is_body = FALSE (nicks are
	 * single-line). Encoding happens here (not inside the shared
	 * builder) so agreement_packet.c stays free of the iconv
	 * dependency that text_util.c brings in. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize name_len = 0;
    char *name_wire
        = gtkhx_text_for_wire ((const char *)htlc->name, strlen (htlc->name),
                               utf8, /*is_body=*/FALSE, &name_len);

    /* Build the AGREEMENTAGREE chunk array through the shared
	 * builder so the test harness (integration_send_agreementagree
	 * _hope) and production stay locked to the same wire shape. The
	 * OPTIONS-bitmap-is-mandatory rule (Mobius panics without it,
	 * see commit history) is enforced by the builder, not here. */
    const hx_agreement_agree_request req = {
        .icon             = htlc->icon,
        .display_name     = name_wire,
        .display_name_len = (guint16) name_len,
        .options          = 0,
    };
    struct hx_chunk chunks[HX_AGREEMENT_AGREE_MAX_CHUNKS];
    guint8 scratch[HX_AGREEMENT_AGREE_SCRATCH_SIZE];
    int hc = hx_agreement_agree_build_chunks (&req, chunks,
                                              HX_AGREEMENT_AGREE_MAX_CHUNKS,
                                              scratch, sizeof (scratch));
    if (hc > 0) {
        hlwrite_chunks (htlc, HTLC_HDR_AGREEMENTAGREE, 0, chunks, hc);
    }
    g_free (name_wire);

    /* Colored-Nicknames: AGREEMENTAGREE carries NAME + ICON
	 * + OPTIONS but not DATA_COLOR — the spec only lists USER_CHANGE
	 * / CHAT_USER_CHANGE / SELFINFO as color-carrying opcodes, so
	 * extending AGREEMENTAGREE unilaterally would be off-spec. Instead
	 * push a follow-up USER_CHANGE that carries our preferred color,
	 * which doubles as the spec's auto-opt-in trigger ("once the
	 * server sees DATA_COLOR from us, decorate other users' USER_
	 * CHANGE broadcasts to us with their colors"). The 1.0/1.2 login
	 * path calls hx_change_name_icon directly (rcv.c, version==0
	 * branch) so this only matters for the 1.5+/AGREEMENTAGREE path.
	 * Gate on nick_color != NONE so a no-color client doesn't ride
	 * the auto-opt-in train it doesn't want. */
    if (htlc->nick_color != HX_NICK_COLOR_NONE) {
        hx_change_name_icon (htlc);
    }

    /* fogWraith caught us mixing 1.2 + 1.5 conventions: per the
	 * 1.5 spec, USER_GETLIST and the news/messages fetch must not
	 * land at the server until AFTER the client sends TranAgreed
	 * — that's when the server officially treats us as joined.
	 * Used to fire from hx_rcv_user_selfinfo, which arrives BEFORE
	 * the agreement in 1.5 — too early. Single-fire guard makes
	 * the call idempotent: the 2s fallback timer in rcv_task_login
	 * (which still arms in case a 1.2 server skips the agreement
	 * step entirely) is harmless once this has run. */
    hx_post_login_fetches (htlc);
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
 *                   htlc->ip_addr from GSocketAddress, install the fd
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

/* Phase 3 TLS: identifying tuple threaded through the
 * GSocketClient::event signal to the accept-certificate handler.
 * Defined here so gtkhx_connect_ctx below can hold one inline.
 * The accept handler body lives later in the file; this
 * forward declaration just gives the struct its layout. */
struct tls_endpoint {
    const char *host;
    guint16 port;
};

struct gtkhx_connect_ctx {
    struct htlc_conn *htlc;
    char *serverstr;
    char *login;
    char *pass;
    int secure;
    int tls; /* TLS Phase 1: wrap control socket from byte zero */
    guint16 port;

    /* Phase 3: identifying tuple for the accept-certificate
     * handler. Populated when tls=1 right before the
     * GSocketClient connect kicks off; .host borrows from
     * serverstr (same lifetime). */
    struct tls_endpoint tls_endpoint;

    GSocketConnection *conn;
    GCancellable *cancel;
    guint magic_timeout_id;

    char magic[HTLS_MAGIC_LEN];

    GtkhxConnectState state;
};

#define MAGIC_TIMEOUT_SEC 30

static void connect_ctx_free (struct gtkhx_connect_ctx *ctx);
static void connect_fail (struct gtkhx_connect_ctx *ctx, const char *stage,
                          GError *err);
static void send_login (struct gtkhx_connect_ctx *ctx);

/* Populate htlc->ip_addr from the GSocketConnection's remote endpoint
 * — the numeric peer address, used by connection-event log lines
 * ("<ip>: connection closed", "<ip>:<port>: login successful"). The
 * post-connect contract is that ip_addr is a NUL-terminated string;
 * "?" if the address can't be read for some reason. */
static gboolean
populate_htlc_remote_ip (struct htlc_conn *htlc, GSocketConnection *conn)
{
    GSocketAddress *remote;
    GInetSocketAddress *inet_remote;
    GInetAddress *inet_addr;
    char *str;

    remote = g_socket_connection_get_remote_address (conn, NULL);
    if (!remote) {
        return FALSE;
    }

    if (!G_IS_INET_SOCKET_ADDRESS (remote)) {
        g_object_unref (remote);
        g_strlcpy (htlc->ip_addr, "?", sizeof (htlc->ip_addr));
        return TRUE;
    }

    inet_remote = G_INET_SOCKET_ADDRESS (remote);
    inet_addr = g_inet_socket_address_get_address (inet_remote);
    str = g_inet_address_to_string (inet_addr);
    g_strlcpy (htlc->ip_addr, str ? str : "?", sizeof (htlc->ip_addr));
    g_free (str);
    g_object_unref (remote);
    return TRUE;
}

static gboolean
magic_timeout_cb (gpointer data)
{
    struct gtkhx_connect_ctx *ctx = data;

    ctx->magic_timeout_id = 0;
    if (ctx->cancel) {
        g_cancellable_cancel (ctx->cancel);
    }
    return G_SOURCE_REMOVE;
}

static void
connect_fail (struct gtkhx_connect_ctx *ctx, const char *stage, GError *err)
{
    struct htlc_conn *htlc = ctx->htlc;

    /* Cancelled by the caller (e.g. user clicked Disconnect or
	 * a re-connect arrived): don't toast a noisy log. */
    if (err && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        connect_ctx_free (ctx);
        return;
    }

    hx_printf_prefix (htlc, 0, INFOPREFIX, "%s: %s\n", stage,
                      err ? err->message : _ ("failed"));
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

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (source), res, &got,
                                         &err)) {
        connect_fail (ctx, _ ("reading server magic"), err);
        g_clear_error (&err);
        return;
    }
    /* Validator is in connect_magic.c — see its docstring for why
	 * we use memcmp instead of strncmp. The pre-extraction strncmp
	 * had a NUL-terminator bug: HTLS_MAGIC contains embedded NULs
	 * so strncmp would accept the 8-byte sequence "TRTP\0XYZ" as
	 * valid (X/Y/Z never compared). memcmp doesn't stop early. */
    if (!hx_connect_validate_server_magic ((const guint8 *)ctx->magic, got)) {
        connect_fail (ctx, _ ("invalid hotline server"), NULL);
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

    if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (source), res,
                                           &wrote, &err)) {
        connect_fail (ctx, _ ("writing client magic"), err);
        g_clear_error (&err);
        return;
    }

    ctx->state = GTKHX_CONNECT_STATE_READING_MAGIC;
    in = g_io_stream_get_input_stream (G_IO_STREAM (ctx->conn));
    ctx->magic_timeout_id
        = g_timeout_add_seconds (MAGIC_TIMEOUT_SEC, magic_timeout_cb, ctx);
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

    conn = g_socket_client_connect_to_host_finish (G_SOCKET_CLIENT (source),
                                                   res, &err);
    if (!conn) {
        connect_fail (ctx, _ ("connect"), err);
        g_clear_error (&err);
        return;
    }
    ctx->conn = conn;
    ctx->state = GTKHX_CONNECT_STATE_WRITING_MAGIC;

    gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
                                         GTKHX_CONNECTION_TCP_CONNECTED);
    hx_printf_prefix (ctx->htlc, 0, INFOPREFIX, _ ("connected to %s\n"),
                      server_addr);

    out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
    g_output_stream_write_all_async (out, HTLC_MAGIC, HTLC_MAGIC_LEN,
                                     G_PRIORITY_DEFAULT, ctx->cancel,
                                     on_magic_sent, ctx);
}

/* Finalize the connect after the magic round-trip succeeds: stash
 * the GSocketConnection on the current_conn global (it owns the
 * fd; hx_htlc_close unrefs it), populate htlc->ip_addr from the
 * remote endpoint, initialise the qbuf state, install the GPollable
 * input source via control_arm_read_source, and send the LOGIN
 * packet.
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

    if (!populate_htlc_remote_ip (htlc, ctx->conn)) {
        connect_fail (ctx, _ ("remote address"), NULL);
        return;
    }

    sock = g_socket_connection_get_socket (ctx->conn);
    s = g_socket_get_fd (sock);

    /* Clear the per-socket I/O timeout that GSocketClient set
     * for the magic-exchange phase (MAGIC_TIMEOUT_SEC). The
     * value persists on the GSocket past the connect call and
     * GTlsConnection / GPollable*Stream honour it — leaving it
     * armed makes any 30+ second idle period on an established
     * connection (a quiet TLS session with no inbound chatter)
     * surface as G_IO_ERROR_TIMED_OUT and we close. The legacy
     * raw read(fd) path didn't see this timeout, so the bug
     * only showed up after the GIOStream / GPollable rewrite.
     *
     * Keepalive at the application layer is handled by
     * ping_start (60s HTLC_HDR_PING) and at the transport
     * layer by the kernel's TCP keepalive defaults; neither
     * needs help from a hard read-timeout on the socket. */
    g_socket_set_timeout (sock, 0);

    /* Stash the GSocketConnection so it stays alive past ctx_free —
	 * hx_htlc_close unrefs current_conn, which closes the fd. */
    g_clear_object (&current_conn);
    current_conn = g_object_ref (ctx->conn);

    /* Initialise htlc state. */
    htlc->fd = s;
    htlc->trans = 1;
    memset (&htlc->in, 0, sizeof (struct qbuf));
    memset (&htlc->out, 0, sizeof (struct qbuf));
    htlc->rcv = hx_rcv_hdr;
    qbuf_set (&htlc->in, 0, SIZEOF_HL_HDR);

    gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
                                         GTKHX_CONNECTION_HANDSHAKE_DONE);

    set_nonblocking (s);
    fd_closeonexec (s, 1);

    connected = 1;
    htlc->gdk_input = 1;
    /* Phase 5+: control-channel I/O is driven by GPollable
     * sources on current_conn's input / output streams (see the
     * control_arm_* helpers). The legacy hxd_fd_set GIOChannel
     * watch was tied to the raw socket fd and didn't see
     * TLS-internal buffering; the pollable sources do.
     * hxd_files[] stays unset for the control fd — only the
     * /exec pipe consumers in commands.c use it now. */
    control_arm_read_source (htlc);

    if (ctx->login) {
        strcpy (htlc->login, ctx->login);
    }

    /* All the chunk-assembly logic for both HOPE Step 1 and legacy
	 * LOGIN now lives in src/login_packet.c so the integration test
	 * harness drives the same wire encoder. The thin per-mode setup
	 * here (rcv_task_login registration, htlc->macalg seed, app_string
	 * formatting, capability bitmask) stays — that's all production-
	 * specific glue. */
    struct hx_chunk login_chunks[HX_LOGIN_MAX_CHUNKS];
    guint8 login_scratch[HX_LOGIN_SCRATCH_SIZE];
    hx_login_request req = { 0 };
    int hc;

    if (ctx->secure) {
        task_new (htlc, RCV_TASK_FN (rcv_task_login),
                  ctx->pass ? g_strdup (ctx->pass) : g_strdup (""), 0, "login");

        /* HOPE-Secure-Login: seed htlc->macalg with the strongest
		 * preference so a malformed Step 1 reply still leaves us in
		 * a known state. login_packet.c hardcodes the full
		 * preference list (SHA256 → SHA1 → MD5) per the spec. */
        strcpy (htlc->macalg, "HMAC-SHA256");

        char app_string[64];
        g_snprintf (app_string, sizeof app_string, "GtkHx %s", VERSION);

        req.mode = HX_LOGIN_MODE_HOPE_STEP1;
        req.hope_app_id = "GTKx";
        req.hope_app_string = app_string;
        /* HOPE cipher advertisement. Single-entry list of the user's
		 * configured algorithm. Phase 5+ NOTE: this could grow into a
		 * strongest-first multi-entry list once HTXF subchannel AEAD
		 * (Phase E) lands. */
        if (htlc->cipheralg[0]) {
            req.cipheralg = htlc->cipheralg;
        }
        if (htlc->compressalg[0]) {
            req.compressalg = htlc->compressalg;
        }
    } else {
        task_new (htlc, RCV_TASK_FN (rcv_task_login), 0, 0, "login");

        /* DATA_CAPABILITIES bitmask. We advertise:
		 *   bit 0  CAP_LARGE_FILES    "I can handle 64-bit sizes."
		 *   bit 1  CAP_TEXT_ENCODING  "I speak UTF-8."
		 *   bit 4  CAP_CHAT_HISTORY   "I can request server-stored
		 *                             chat history via TRAN 700."
		 *
		 * Servers that support the spec echo the bits they accept
		 * back in the LOGIN reply. Unknown bits are ignored per
		 * spec; servers that don't recognise the chunk silently
		 * fall back to legacy 32-bit Mac Roman framing — same
		 * behaviour as without this chunk.
		 *
		 * Advertise ourselves as Hotline 1.8.5 (185). Matches what
		 * the integration test harness sends; bumps mhxd's
		 * can_ping bit so HTLC_HDR_PING keepalives are accepted.
		 *
		 * LOGIN is the 1.5-spec shape: no HTLC_DATA_NAME chunk.
		 * 1.5+ servers will get our nick via AGREEMENTAGREE after
		 * the user dismisses the agreement window (or via the auto-
		 * send in hx_rcv_agreement_file's HX_AGREEMENT_NONE branch
		 * when the account has AccessNoAgreement). 1.0/1.2 servers
		 * never send agreement and never accept AGREEMENTAGREE; for
		 * those, rcv_task_login detects the absence of an
		 * HTLS_DATA_VERSION chunk in the reply and fires
		 * hx_change_name_icon (USER_CHANGE) directly. */
        req.mode = HX_LOGIN_MODE_LEGACY;
        req.icon = htlc->icon;
        req.login_name = ctx->login; /* NULL = anonymous */
        req.password = (ctx->pass && *ctx->pass) ? ctx->pass : NULL;
        req.display_name = NULL; /* production sends NAME via USER_CHANGE later */
        req.client_version = 185;
        req.caps = HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING
                 | HTLC_CAP_CHAT_HISTORY | HTLC_CAP_VOICE
                 | HTLC_CAP_INLINE_MEDIA;
        req.send_caps = 1;
    }

    hc = hx_login_build_chunks (&req, login_chunks, HX_LOGIN_MAX_CHUNKS,
                                login_scratch, sizeof (login_scratch));
    /* Builder returns 0 on argument / overflow validation failure.
	 * Sending hc=0 would produce a header-only frame the server
	 * would reject (or trip hlpack_chunks's g_return_if_fail
	 * guardrails). Route through connect_fail so the user sees a
	 * sensible toast instead of a silent hang. */
    if (hc <= 0) {
        connect_fail (ctx, _ ("building LOGIN packet"), NULL);
        return;
    }
    hlwrite_chunks (htlc, HTLC_HDR_LOGIN, 0, login_chunks, hc);

    ctx->state = GTKHX_CONNECT_STATE_DONE;
    connect_ctx_free (ctx);
}

/* Phase 3 TLS: deferred pin payload. Stamping a new known-hosts
 * entry from inside the accept-certificate signal handler turned
 * out to hang the TLS handshake — the handler runs on glib-
 * networking's TLS worker thread, and the GLib file I/O (mkstemp
 * + rename) inside hx_tls_trust_pin interacts badly with the
 * thread state the handshake needs after we return TRUE. Janus
 * waits forever for the client's next handshake message; we
 * eventually time out and Janus logs "perform handshake: read
 * handshake: EOF".
 *
 * The fix: return TRUE immediately from the accept handler and
 * let the handshake resume; queue the file write on the default
 * main context where the rest of GtkHx already runs file I/O.
 * The trust DECISION (accept this cert) is what the signal
 * contract needs; the pin is persistence and can happen any
 * time before the next connect. */
typedef struct {
    char *host;
    guint16 port;
    char *fingerprint;
} trust_pin_payload;

static void
trust_pin_payload_free (gpointer data)
{
    trust_pin_payload *p = data;
    if (!p) {
        return;
    }
    g_free (p->host);
    g_free (p->fingerprint);
    g_free (p);
}

static gboolean
trust_pin_idle (gpointer data)
{
    trust_pin_payload *p = data;
    if (p) {
        if (!hx_tls_trust_pin (p->host, p->port, p->fingerprint)) {
            /* Pin failed — likely permissions on $CONFIG, a
             * missing config dir, or a rename(2) error. The
             * connection is already up (we returned TRUE from
             * accept-certificate before queueing this idle),
             * so we don't disturb it; but the trust state
             * won't persist and the user will see the TOFU
             * prompt again on next connect with no obvious
             * explanation. g_warning routes through GLib's
             * log machinery so it shows up on stderr for
             * console-launched runs and in journalctl for
             * Flatpak. Should be rare enough that the noise
             * cost is acceptable; if it stops being rare we
             * can promote to a user-visible toast. */
            g_warning ("TLS trust pin failed for %s:%u — connection "
                       "allowed but the trust state will not "
                       "persist; expect another trust prompt on "
                       "the next connect to this server. Check "
                       "permissions on the known_hosts path.",
                       p->host, (unsigned) p->port);
        }
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_trust_pin (const char *host, guint16 port, const char *fingerprint)
{
    trust_pin_payload *p = g_new0 (trust_pin_payload, 1);
    p->host = g_strdup (host);
    p->port = port;
    p->fingerprint = g_strdup (fingerprint);
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, trust_pin_idle, p,
                     trust_pin_payload_free);
}

/* Marshal hx_tls_trust_dialog_run_sync to the main thread.
 *
 * The accept-certificate signal can fire on a worker thread:
 * hx_sync_connect_to_host (HTXF subchannel workers in xfers.c
 * + banner.c) drives a sync GSocketClient connect from a non-
 * main thread, and glib-networking emits accept-certificate on
 * whichever thread is currently inside the handshake. Dialog
 * APIs (libadwaita, the nested GMainLoop in hx_tls_trust_dialog_
 * run_sync) require the main thread — calling them from a worker
 * crashes or deadlocks.
 *
 * The pattern: pack the dialog args + a result slot + a
 * GMutex/GCond into a stack-local struct, g_main_context_invoke
 * to dispatch the body, wait on the condvar. On the main thread
 * the invoke runs synchronously (the callback fills in result
 * before invoke returns), so the wait loop sees done==TRUE
 * immediately and exits without contention. On a worker the
 * invoke queues an idle on the main context, the worker blocks
 * on the condvar, and the main thread eventually wakes it with
 * the user's decision. The dialog's own nested GMainLoop keeps
 * pumping events while the user clicks, including any GSource
 * activity the worker is waiting on. */
typedef struct {
    GtkWindow *parent;
    const char *host;
    guint16 port;
    const char *fingerprint;
    hx_tls_trust_status status;
    /* Mutex + cond + done form the worker-thread wait protocol;
     * accepted is the result. */
    GMutex mutex;
    GCond cond;
    gboolean done;
    gboolean accepted;
} trust_dialog_marshal;

static gboolean
trust_dialog_invoke (gpointer data)
{
    trust_dialog_marshal *m = data;
    gboolean accepted = hx_tls_trust_dialog_run_sync (
        m->parent, m->host, m->port, m->fingerprint, m->status);
    g_mutex_lock (&m->mutex);
    m->accepted = accepted;
    m->done = TRUE;
    g_cond_signal (&m->cond);
    g_mutex_unlock (&m->mutex);
    return G_SOURCE_REMOVE;
}

static gboolean
trust_dialog_run_thread_safe (GtkWindow *parent, const char *host,
                              guint16 port, const char *fingerprint,
                              hx_tls_trust_status status)
{
    trust_dialog_marshal m = {
        .parent = parent,
        .host = host,
        .port = port,
        .fingerprint = fingerprint,
        .status = status,
        .done = FALSE,
        .accepted = FALSE,
    };
    g_mutex_init (&m.mutex);
    g_cond_init (&m.cond);

    /* g_main_context_invoke runs synchronously when called from
     * the context's owner (main thread) and asynchronously
     * otherwise; either way the callback eventually signals
     * m.cond, and the wait loop below covers both cases. */
    g_main_context_invoke (NULL, trust_dialog_invoke, &m);

    g_mutex_lock (&m.mutex);
    while (!m.done) {
        g_cond_wait (&m.cond, &m.mutex);
    }
    g_mutex_unlock (&m.mutex);

    g_mutex_clear (&m.mutex);
    g_cond_clear (&m.cond);
    return m.accepted;
}

/* Phase 3 TLS: TOFU accept-certificate handler. Pulls (host,
 * port) from the tls_endpoint (passed as user_data via the
 * GSocketClient event signal),
 * computes the cert fingerprint, looks it up in the user's
 * known-hosts file, and either silently accepts (TRUSTED),
 * pins-then-accepts after a TOFU prompt (UNKNOWN), or warns
 * loudly before accepting/rejecting (MISMATCH).
 *
 * GTKHX_TLS_AUTO_ACCEPT=1 in the environment bypasses the
 * dialog: any UNKNOWN cert is auto-pinned, any MISMATCH is
 * auto-trusted (with a noisy log line). Used by the Tier 3
 * test harness, which has no GtkApplication and would deadlock
 * waiting on the prompt's nested GMainLoop. Also handy for
 * scripted first-run pinning. Default-off — production users
 * always see the prompt.
 *
 * The signal contract is sync (return TRUE/FALSE). When the
 * signal fires on a worker thread (HTXF subchannel connects
 * — see trust_dialog_run_thread_safe above), the dialog is
 * marshalled to the main thread. */
static gboolean
tls_accept_certificate (GTlsConnection *conn G_GNUC_UNUSED,
                        GTlsCertificate *peer_cert,
                        GTlsCertificateFlags errors,
                        gpointer user_data)
{
    struct tls_endpoint *ep = user_data;
    const char *host = (ep && ep->host) ? ep->host : "?";
    guint16 port = ep ? ep->port : 0;

    g_autofree gchar *fingerprint = hx_tls_trust_fingerprint (peer_cert);
    if (!fingerprint) {
        /* No DER blob on the cert — shouldn't happen, but if it
         * does we have no way to identify the cert so we have to
         * reject. */
        debug_log ("tls",
                   "accept-certificate: no fingerprint available for %s:%u",
                   host, (unsigned) port);
        return FALSE;
    }

    hx_tls_trust_status status =
        hx_tls_trust_lookup (host, port, fingerprint);
    debug_log ("tls",
               "accept-certificate: %s:%u fp=%s status=%d errors=0x%x",
               host, (unsigned) port, fingerprint, (int) status,
               (unsigned) errors);

    if (status == HX_TLS_TRUST_TRUSTED) {
        return TRUE;
    }

    /* If the user already pinned THIS cert for THIS host on
     * some OTHER port, silently accept and pin the new port
     * too. The canonical case: control channel was just pinned
     * at host:5600, now the HTXF subchannel connects to
     * host:5601 with the bit-identical cert — without this
     * cross-port check the user would see the trust prompt
     * twice for what they perceive as a single connection.
     *
     * Only consulted on a strict-UNKNOWN result. A
     * strict-MISMATCH on this (host, port) is a security
     * signal — we never let an any-port match override it. */
    if (status == HX_TLS_TRUST_UNKNOWN
        && hx_tls_trust_host_has_fingerprint (host, fingerprint)) {
        debug_log ("tls",
                   "accept-certificate: %s:%u auto-accepted "
                   "(same cert pinned for host on another port)",
                   host, (unsigned) port);
        schedule_trust_pin (host, port, fingerprint);
        return TRUE;
    }

    /* Auto-accept escape hatch for headless tests + scripted
     * first-run pinning. Tier 3 sets this; production never
     * does. The pin is queued via g_idle_add (schedule_trust_pin)
     * rather than called inline because hx_tls_trust_pin's body
     * uses to call g_mkdir_with_parents which wedges the TLS
     * handshake when invoked from inside this accept-certificate
     * signal handler. The pin function no longer calls mkdir, but
     * keeping the work on the main thread is the conservative
     * default — file I/O latency here would extend the handshake
     * window even if it doesn't deadlock, and the trust DECISION
     * (which is what the signal contract needs) doesn't depend
     * on the pin write completing.
     *
     * MISMATCH inside the auto-accept branch is logged loudly
     * via g_warning: a test or scripted run that silently
     * overrides a real fingerprint change without anyone
     * noticing is exactly the failure mode the warning is
     * meant to surface. UNKNOWN doesn't need the warning —
     * that's the documented first-run / fresh-server case. */
    const char *auto_accept = g_getenv ("GTKHX_TLS_AUTO_ACCEPT");
    if (auto_accept && *auto_accept) {
        if (status == HX_TLS_TRUST_MISMATCH) {
            g_warning ("GTKHX_TLS_AUTO_ACCEPT overriding TLS "
                       "MISMATCH for %s:%u (fp=%s) — the pinned "
                       "fingerprint differs from this one. Set "
                       "this env var only for trusted test "
                       "harnesses; production should never see "
                       "this line.",
                       host, (unsigned) port, fingerprint);
        }
        schedule_trust_pin (host, port, fingerprint);
        return TRUE;
    }

    /* Real user-facing prompt. Parent transient-for the toolbar
     * window if it exists; otherwise float (still works, just
     * less polished). trust_dialog_run_thread_safe handles
     * marshalling to the main thread when this signal fires on
     * an HTXF worker — see the docstring above. */
    GtkWindow *parent = NULL;
    if (toolbar_window && GTK_IS_WINDOW (toolbar_window)) {
        parent = GTK_WINDOW (toolbar_window);
    }
    gboolean accepted = trust_dialog_run_thread_safe (
        parent, host, port, fingerprint, status);
    if (accepted) {
        schedule_trust_pin (host, port, fingerprint);
    }
    return accepted;
}

/* The GSocketClient event signal fires through every phase of the
 * connect-and-wrap sequence. We're interested in the TLS_HANDSHAKING
 * phase because that's the only phase where the connection arg is a
 * GTlsClientConnection we can attach accept-certificate to. Earlier
 * phases give a plain GSocketConnection (or NULL); later phases'
 * cert decision has already happened.
 *
 * `user_data` is a `struct tls_endpoint *` (the async connect
 * path passes &ctx->tls_endpoint, the sync HTXF connect path
 * passes a stack-local endpoint) — forwarded as-is to the
 * accept-certificate handler so it can read (host, port) for
 * the trust lookup. Lifetime is the caller's; we just borrow
 * the pointer for the duration of the handshake. */
static void
on_socket_client_event (GSocketClient *client G_GNUC_UNUSED,
                        GSocketClientEvent event,
                        GSocketConnectable *connectable G_GNUC_UNUSED,
                        GIOStream *connection,
                        gpointer user_data)
{
    if (event != G_SOCKET_CLIENT_TLS_HANDSHAKING) {
        return;
    }
    if (!connection || !G_IS_TLS_CONNECTION (connection)) {
        return;
    }
    g_signal_connect (connection, "accept-certificate",
                      G_CALLBACK (tls_accept_certificate), user_data);
}

void
hx_connect (struct htlc_conn *htlc, const char *serverstr, guint16 port,
            const char *login, const char *pass, char secure, char tls)
{
    struct gtkhx_connect_ctx *ctx;
    GSocketClient *client;

    /* GTKHX_TLS=1 env-var override. While Phase 4 (Connect dialog
	 * TLS toggle) is unimplemented, this is how the test harness
	 * and power users flip TLS on without rebuilding. Anything
	 * non-empty counts as truthy — matches the convention the rest
	 * of the GTKHX_* env vars follow. */
    if (!tls) {
        const char *env = g_getenv ("GTKHX_TLS");
        if (env && *env) {
            tls = 1;
        }
    }

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
    server_port = port;

    /* Phase 4 (chat-history reconnect catch-up): the AFTER= cursor
	 * is per-server, but the xtext scrollback we just wiped via
	 * hx_clear_chat is per-connect. Reset the cursor every time we
	 * connect (or reconnect) — the catch-up's invariant is "the
	 * messages newer than this cursor are not yet on screen", and
	 * since we just cleared the screen, NO messages are on screen,
	 * so the cursor should be 0. Otherwise:
	 *
	 *   1. First connect: server returns 50 history entries with
	 *      msgids up to N. Cursor becomes N. xtext shows them.
	 *   2. User clicks Reconnect (same host:port).
	 *   3. hx_clear_chat wipes xtext above.
	 *   4. If we'd kept the cursor at N, post-login catch-up sends
	 *      AFTER=N and the server correctly returns zero new
	 *      entries — and the chat window stays blank.
	 *
	 * The "per-server" distinction below is now only about logging
	 * server switches; the reset itself is unconditional. (Server-
	 * switch logging still useful for the "did the cursor really
	 * get reset?" debug case.)
	 *
	 * Compare against the previous serverhost / serverport BEFORE
	 * we overwrite them just below. First-ever connect: serverhost
	 * is "" and the cursor is already 0, so the reset is a harmless
	 * no-op. */
    if ((strcmp (htlc->serverhost, serverstr) != 0
         || htlc->serverport != port)
        && htlc->chat_history_last_msgid != 0) {
        debug_log ("chat-history",
                   "switching servers (%s:%u → %s:%u); "
                   "resetting AFTER cursor from %" G_GUINT64_FORMAT,
                   htlc->serverhost[0] ? htlc->serverhost : "(none)",
                   htlc->serverport, serverstr, port,
                   htlc->chat_history_last_msgid);
    } else if (htlc->chat_history_last_msgid != 0) {
        debug_log ("chat-history",
                   "same-server reconnect (%s:%u); "
                   "resetting AFTER cursor from %" G_GUINT64_FORMAT
                   " (xtext was wiped, initial-load path will fire)",
                   serverstr, port, htlc->chat_history_last_msgid);
    }
    htlc->chat_history_last_msgid = 0;

    /* Stamp the server endpoint onto htlc so the HTXF subchannel
	 * (port+1) and post-connect log messages don't have to query a
	 * separate "what server did we connect to" oracle. */
    g_strlcpy (htlc->serverhost, serverstr, sizeof (htlc->serverhost));
    htlc->serverport = port;

    /* Phase 2: also stamp the tls flag so HTXF subchannel connects
     * (xfers.c::htxf_connect via htxf->htlc, banner.c via the
     * fetch snapshot) know to wrap their data ports too. The
     * Mobius / Janus separate-port model pairs TLS-HTLS on port N
     * with TLS-HTXF on port N+1, so reusing the existing port+1
     * arithmetic just works without a separate tls_xfer_port
     * concept on the production side. */
    htlc->tls = tls;

#if 0 /* XXX */
	server_log = create_log(server_addr);
#endif

    ctx = g_new0 (struct gtkhx_connect_ctx, 1);
    ctx->htlc = htlc;
    ctx->serverstr = g_strdup (serverstr);
    ctx->port = port;
    ctx->login = g_strdup (login);
    ctx->pass = g_strdup (pass);
    ctx->secure = (unsigned char) secure;
    ctx->tls = (unsigned char) tls;
    ctx->cancel = g_cancellable_new ();
    current_cancel = g_object_ref (ctx->cancel);
    ctx->state = GTKHX_CONNECT_STATE_RESOLVING;

    htlc->gdk_input = 0;
    hx_printf_prefix (htlc, 0, INFOPREFIX,
                      tls ? _ ("connecting to %s (TLS)\n")
                          : _ ("connecting to %s\n"),
                      server_addr);
    gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
                                         GTKHX_CONNECTION_CONNECTING);

    client = g_socket_client_new ();
    /* GSocketClient defaults already prefer IPv4 if both are
	 * available and try each resolved address in turn — same fallback
	 * behaviour the legacy getaddrinfo loop had. */
    g_socket_client_set_timeout (client, MAGIC_TIMEOUT_SEC);
    if (tls) {
        /* Phase 1: TLS from byte zero (Mobius / Janus separate-port
		 * model — no STARTTLS, no in-band negotiation). GIO routes
		 * the underlying socket through whatever TLS backend
		 * glib-networking provides (GnuTLS or OpenSSL depending on
		 * distro / runtime); we don't link a TLS lib directly. The
		 * event signal lets us attach accept-certificate during the
		 * TLS_HANDSHAKING phase — see on_socket_client_event. */
        g_socket_client_set_tls (client, TRUE);
        /* Phase 3: user_data is a tls_endpoint stashed on the
         * connect ctx (ep_host points at the same memory as
         * ctx->serverstr, alive until the connect completes).
         * Lives long enough — the GSocketClient is unref'd
         * below but the underlying handshake completes before
         * the connect callback fires, and the signal handler
         * only runs during the handshake. */
        ctx->tls_endpoint.host = ctx->serverstr;
        ctx->tls_endpoint.port = port;
        g_signal_connect (client, "event",
                          G_CALLBACK (on_socket_client_event),
                          &ctx->tls_endpoint);
    }
    g_socket_client_connect_to_host_async (client, serverstr, port, ctx->cancel,
                                           on_async_connected, ctx);
    g_object_unref (client);
}

/* Synchronous worker-thread connect helper. Used by the HTXF
 * transfer workers in xfers.c and banner.c — both run on a pthread
 * whose only excuse for existing is the blocking byte-streaming
 * loop, so a sync GSocketClient call here keeps them simple.
 *
 * Returns a connected GSocketConnection the caller owns
 * (g_object_unref drops both the GIO machinery and the underlying
 * socket fd). On failure returns NULL and writes the GError
 * message to errbuf (truncated to errbuf_len) if both are
 * non-NULL. host/port go straight through to GSocketClient —
 * IPv4/IPv6 fallback and SOCKS proxy resolution (via
 * GProxyResolver) come for free.
 *
 * Workers cast the returned conn to GIOStream and stream bytes
 * through htxf_io_read / htxf_io_write (both stream-shaped and
 * AEAD-aware since Phase B). The fd is owned by the
 * GSocketConnection — don't close(2) it; unref the conn and
 * GSocket's finaliser closes the fd. */
GSocketConnection *
hx_sync_connect_to_host (const char *host, guint16 port, char *errbuf,
                         gsize errbuf_len, char tls)
{
    GSocketClient *client;
    GSocketConnection *conn;
    GError *err = NULL;

    client = g_socket_client_new ();
    /* Phase 2: optional TLS wrap on the HTXF subchannel. Same
     * shape as hx_connect's control-channel TLS plumbing — flip
     * set_tls before the connect and hook accept-everything via
     * the GSocketClient event signal at TLS_HANDSHAKING. Phase 3:
     * the handler now does a real TOFU lookup; the endpoint
     * struct on the stack tells it which host:port we're
     * connecting to. The struct lives until this function
     * returns, which outlives the connect call. */
    struct tls_endpoint endpoint = { .host = host, .port = port };
    if (tls) {
        g_socket_client_set_tls (client, TRUE);
        g_signal_connect (client, "event",
                          G_CALLBACK (on_socket_client_event),
                          &endpoint);
    }
    conn = g_socket_client_connect_to_host (client, host, port, NULL, &err);
    g_object_unref (client);
    if (!conn) {
        if (errbuf && errbuf_len && err) {
            g_strlcpy (errbuf, err->message, errbuf_len);
        }
        g_clear_error (&err);
        return NULL;
    }
    /* GSocketConnection is blocking by default — exactly what the
	 * worker threads want. The old fd-returning variant had to
	 * dup() and explicitly clear O_NONBLOCK because g_socket_get_fd
	 * surfaces whatever flags GSocket happens to have set; on the
	 * GSocketConnection-shaped API neither dance is needed. */
    return conn;
}

GSocketConnection *
htxf_connect (struct htxf_conn *htxf)
{
    GSocketConnection *conn;
    char errbuf[256];
    /* htxf is required — every caller in the codebase (xfers.c,
	 * news worker, banner worker, xfer_go) allocates the struct
	 * before calling. The original code had a vestigial NULL guard
	 * inside the body, but the preceding lines unconditionally
	 * dereferenced htxf, so the guard was dead anyway. Assert and
	 * fail loud rather than papering over a programmer error. */
    g_return_val_if_fail (htxf != NULL, NULL);

    /* Large-file (CAP_LARGE_FILES) mode: when the negotiated
	 * caps include the bit AND the transfer actually needs 64-bit
	 * sizing (total_size > 0xFFFFFFFF), advertise both flags and
	 * use the 24-byte handshake variant. Stamp htxf->opt.large
	 * so file_send_one / file_recv_one know to use the split-
	 * encoded fork headers (and raw-data uploads).
	 *
	 * Why we DON'T set LARGE_FILE for sub-4-GiB transfers even
	 * when caps include it: spec says large-file uploads send
	 * raw data only, no FFO. If we set the flag for every
	 * transfer on a large-file-capable server, we lose the
	 * INFO fork on small uploads — type/creator/comment go
	 * missing server-side. Keeping LARGE_FILE off for fits-in-
	 * 32-bit transfers preserves the legacy FFO behaviour for
	 * the common case. The cap negotiation still works — both
	 * peers KNOW they can speak large-file, they just don't have
	 * to use the wire shape for this particular transfer. */
    gboolean size64 = htxf->htlc
                      && (htxf->htlc->caps & HTLC_CAP_LARGE_FILES) != 0
                      && htxf->total_size > 0xFFFFFFFFULL;
    htxf->opt.large = size64 ? 1 : 0;

    /* Mirror the control channel's TLS mode onto this HTXF
     * subchannel — separate-port model expects TLS-HTXF on
     * port+1 to pair with TLS-HTLS. htxf->htlc is non-NULL on
     * every existing caller (xfers.c file_get/put/folder paths
     * and xfer_go all populate it), so reading htlc->tls here is
     * safe; the g_return_val_if_fail above guards the
     * htxf-itself-NULL case. */
    char xfer_tls = (htxf->htlc != NULL) ? htxf->htlc->tls : 0;
    conn = hx_sync_connect_to_host (htxf->serverhost, htxf->serverport,
                                    errbuf, sizeof (errbuf), xfer_tls);
    if (!conn) {
        return NULL;
    }

    /* Plaintext preamble (16 bytes legacy, 24 bytes when SIZE64
	 * is set). hx_htxf_subchannel_pack_preamble handles the
	 * LARGE_FILE / SIZE64 flag-setting and the legacy-field
	 * zeroing for the 24-byte variant.
	 *
	 * The handshake write goes through GIOStream so a future TLS
	 * wrap (docs/tls-scoping.md Phase 2) catches these bytes on
	 * the same path as the rest of the subchannel.
	 * g_output_stream_write_all blocks until the whole buffer
	 * lands or an error fires. */
    guint8 hdr_buf[HX_HTXF_PREAMBLE_MAX_BYTES];
    guint16 type
        = htxf->opt.folder ? HTXF_TYPE_FOLDER : HTXF_TYPE_FILE;
    size_t hdr_len = hx_htxf_subchannel_pack_preamble (
        hdr_buf, sizeof (hdr_buf),
        htxf->ref, htxf->total_size,
        type, /*flags=*/0, size64);
    if (hdr_len == 0) {
        g_object_unref (conn);
        return NULL;
    }
    GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
    if (!g_output_stream_write_all (out, hdr_buf, hdr_len, NULL, NULL, NULL)) {
        g_object_unref (conn);
        return NULL;
    }

    /* HOPE-ChaCha20-Poly1305 HTXF subchannel arming (Phase E2).
	 *
	 * Once the plaintext handshake has been sent, derive a
	 * per-transfer ChaCha20 key pair off the control channel's
	 * session_key plus our HTXF ref number and flip the htxf_io
	 * wrappers (Phase E1) into framed-AEAD mode. The handshake
	 * itself stays plaintext per spec — only the body bytes
	 * (FILP forks, folder commands, file data, etc.) flow
	 * through AEAD frames.
	 *
	 * Derivation mixes ref into the salt so two transfers within
	 * the same control-channel session can never share a nonce
	 * even if their plaintext byte streams happen to match.
	 * Counters start at 0 per transfer (the derive helper zeros
	 * them).
	 *
	 * Only fires when:
	 *   - the htxf is bound to a control channel (htlc non-NULL),
	 *   - that control channel negotiated CIPHER_MODE_AEAD (the
	 *     server picked CHACHA20-POLY1305 in the HOPE Step 2
	 *     reply — see rcv_task_login),
	 *   - cipher support is compiled in.
	 *
	 * Other transfers (no HOPE, or HOPE with a stream cipher)
	 * leave aead_active = FALSE and the wrappers behave exactly
	 * like read()/write(). */
    if (htxf->htlc && htxf->htlc->cipher_mode == CIPHER_MODE_AEAD) {
        hx_htxf_subchannel_arm_aead (
            htxf,
            htxf->htlc->sessionkey, htxf->htlc->sklen,
            &htxf->htlc->cipher_encode_state.chacha,
            &htxf->htlc->cipher_decode_state.chacha,
            htxf->ref);
        debug_log ("xfer-aead",
                   "ref=%u: AEAD active (control session_key=%u bytes)",
                   htxf->ref, htxf->htlc->sklen);
    }

    return conn;
}

/*
 * Tracker fetch — async state machine on the main loop.
 * ======================================================
 *
 * Each "run" walks gtkhx_prefs.tracker[] serially. Each per-tracker
 * fetch builds its own fetch_ctx, runs through the protocol with
 * GSocketClient + GInputStream async callbacks, then chains to the
 * next tracker (or finalises the run if exhausted). Cancellation
 * goes through the run's GCancellable — tracker_kill_threads()
 * trips it; the in-flight callbacks see G_IO_ERROR_CANCELLED and
 * unwind cleanly.
 *
 * The legacy version of this code ran on a pthread, woke itself
 * out of blocking I/O via SIGUSR1, and pumped UI updates through
 * the post_* marshal helpers above. None of that is needed when
 * every step runs on the main loop already.
 *
 * Protocol shape (per-tracker, after TCP connect):
 *
 *   The client unconditionally sends an 8-byte v3 handshake
 *   ("HTRK" + version 0x0003 + feature bits). v1/v2 trackers read
 *   the 6 bytes they expect and start sending the listing; the
 *   extra 2 bytes from us sit harmlessly in their RX buffer until
 *   the connection closes. v3 trackers respond with 8 bytes and
 *   wait for a listing request.
 *
 *   We always read 6 bytes first. If the version field is 0x0003
 *   we read 2 more (feature flags) and switch to the v3 chain;
 *   otherwise we stay on the v1 chain, which is the original
 *   record-by-record read pattern.
 *
 * v1 chain (after the 6-byte response, version 0x0001 / 0x0002):
 *   read 8 more bytes (the rest of the 14-byte v1 response header;
 *        nservers at offset 10 (u16 BE) of the combined buffer)
 *   per server:
 *     read 8 bytes : IP(4) + port(2) + nusers(2)
 *                    -- if first byte is 0, this is a padding slot
 *                       (IPs can't start with 0); skip without
 *                       decrementing nservers
 *     read 3 bytes : 2 reserved + name_len(1)
 *     read name_len bytes : server name (Mac-Roman-ish; CR→LF + strip_ansi)
 *     read 1 byte  : desc_len
 *     read desc_len bytes : description
 *
 * v3 chain (after the 8-byte handshake response, version 0x0003):
 *   write 4 bytes : listing-request (type=1 + field_count=0)
 *   read 10 bytes : response header (type + total_size + total +
 *                   record_count)
 *   read total_size bytes (capped) : back-to-back server records
 *   walk records via hx_tracker_v3_parse_record, emit one
 *   HxTrackerServer event per record. TLV trailers are walked
 *   over to advance the cursor; Phase A doesn't surface them.
 */

struct tracker_run_ctx; /* fwd */

/* Cap on total v3 response payload (records blob) we'll read.
 * Spec allows u32 = 4 GiB; we cap at 16 MiB to avoid a hostile
 * tracker hanging us on a g_malloc + read_all. At an average ~80
 * bytes/record that's ~200k servers — beyond anything we expect
 * to see in production. Crossing the cap aborts the listing
 * with a chat-output line. */
#define HX_TRACKER_V3_MAX_PAYLOAD (16u * 1024u * 1024u)

/* Probe-then-fallback watchdog (milliseconds). After sending the
 * v3 magic, we wait this long for the first 6 bytes of response
 * before declaring "tracker doesn't speak v3, fall back to v1".
 *
 * Real-world v1 trackers (hxtrackd, hltracker.com, mhxd's bundled
 * tracker, basically every pre-v3-spec tracker) memcmp the full
 * 6-byte HTRK_MAGIC ("HTRK\0\1") and fall through silently when
 * byte 5 is 0x03 instead of 0x01. The connection stays open with
 * no data — we'd block forever without a watchdog.
 *
 * 2000 ms is generous: a real spec-compliant v3 tracker should
 * reply in well under a second on a healthy network. Override via
 * GTKHX_TRACKER_V3_PROBE_MS env var (see hx_tracker_v3_probe_ms
 * below) for slow test rigs or when manually testing against a
 * tracker on a high-latency link. */
#define HX_TRACKER_V3_PROBE_TIMEOUT_MS 2000

/* Read the watchdog timeout once per fetch start. Parsed via
 * strtol with sane clamps — values <100ms are pointless (a real
 * v3 reply still needs to arrive) and >60000ms means we hang the
 * UI for a minute, which isn't what anyone wants either. Anything
 * outside the range or unparseable falls back to the compile-time
 * default. */
static guint
hx_tracker_v3_probe_ms (void)
{
    const char *env = g_getenv ("GTKHX_TRACKER_V3_PROBE_MS");
    if (!env || !*env) {
        return HX_TRACKER_V3_PROBE_TIMEOUT_MS;
    }
    char *endp = NULL;
    long v = strtol (env, &endp, 10);
    if (endp == env || *endp != '\0' || v < 100 || v > 60000) {
        debug_log ("tracker",
                   "ignoring GTKHX_TRACKER_V3_PROBE_MS=%s "
                   "(must be an integer in [100, 60000]); "
                   "using default %u ms",
                   env, (unsigned) HX_TRACKER_V3_PROBE_TIMEOUT_MS);
        return HX_TRACKER_V3_PROBE_TIMEOUT_MS;
    }
    return (guint) v;
}

struct tracker_fetch_ctx {
    struct tracker_run_ctx *run; /* parent run; lifetime-tied */
    char *serverstr;
    guint16 port;

    GSocketConnection *conn;
    GInputStream *in;
    GOutputStream *out;

    /* Parse scratch. buf is sized for the biggest fixed-size read
     * (the 14-byte v1 response header — v3's 10-byte header and
     * 8-byte handshake both fit). name/desc are sized to the
     * v1 1-byte length field's max. */
    guint8 buf[16];
    char name[256];
    char desc[256];

    /* Negotiated handshake values. v1/v2 trackers leave
     * v3_features == 0; v3 trackers populate it from the 8-byte
     * response. */
    guint16 version;
    guint16 v3_features;

    /* Probe-then-fallback state. First connection attempt sends
     * the 8-byte v3 magic with use_v3 = 1; if the watchdog fires
     * (real-world v1 trackers reject the 0x0003 version byte
     * silently), we close the conn, set use_v3 = 0, and reopen
     * with the 6-byte v1 magic.
     *
     * attempt_cancel is a per-attempt GCancellable that the
     * watchdog cancels. Chained to run->cancel via
     * g_cancellable_connect so a user-driven run abort propagates.
     * Distinguishing "watchdog cancelled this attempt" from "user
     * aborted the run" in the read callback is just
     * g_cancellable_is_cancelled (run->cancel). */
    int use_v3;
    int v3_probe_timed_out;
    GCancellable *attempt_cancel;
    gulong attempt_cancel_link;
    guint v3_probe_timeout_id;

    /* Phase D TLS state. use_tls = 1 means the next connect attempt
     * goes through gtls (g_socket_client_set_tls). tls_attempted
     * is set as soon as we kick off a TLS connect so a failure
     * callback can tell "TLS just failed → try plain" apart from
     * "plain failed, no fallback to try". tls_endpoint feeds the
     * shared on_socket_client_event TOFU handler so trust pins for
     * tracker certs land in the same known_hosts file the main
     * Hotline session uses — .host borrows ctx->serverstr's
     * lifetime, valid for the duration of the connect. */
    int use_tls;
    int tls_attempted;
    struct tls_endpoint tls_endpoint;

    /* v3 listing-request scratch + listing-response buffer.
     * v3_payload is allocated to hold `total_size` bytes once we
     * have the response header; freed in tracker_fetch_free. */
    guint8 v3_req_buf[4];
    guint8 v3_resp_hdr[HTRK_V3_RESP_HDR_LEN];
    guint8 *v3_payload;
    gsize v3_payload_len;

    /* v1 path scratch (record-by-record). */
    guint16 nservers; /* remaining to read */
    int server_i;     /* 1-based index of next-completed server,
                       * for the progress widget */
    int total;        /* set once after the header is read */
    struct in_addr cur_addr;
    guint16 cur_port;
    guint16 cur_nusers;
    guint8 cur_name_len;
    guint8 cur_desc_len;
};

struct tracker_run_ctx {
    gboolean aborted; /* tracker_kill_threads set this */
    session *sess;
    GCancellable *cancel;
    char **trackers; /* owned strdup of gtkhx_prefs.tracker[] */
    int n_trackers;
    int current_index;
    struct tracker_fetch_ctx *cur_ctx;
};

static struct tracker_run_ctx *current_tracker_run;

/* Phase D TLS verdict cache.
 *
 * Per-tracker memory of "this tracker speaks TLS" vs "this tracker
 * doesn't, don't re-pay the failed handshake on every Refresh."
 * Keyed on the tracker URL string (the same g_strdup'd string we
 * use for the prefs entry); values are pointer-cast enums.
 *
 * Lifetime: scoped to the process (cleared on hx_tracker_kill_
 * threads only when a fresh hx_tracker_list_async would otherwise
 * pay full re-probe cost). Re-probed at next launch so a tracker
 * that adds TLS support gets picked up next time the user starts
 * GtkHx — no manual cache clear, no stale "this tracker has no
 * TLS" bit lingering across upgrades.
 *
 * NULL key (g_str_hash + g_str_equal + g_free for keys + NULL
 * value destroy since the values are just enum-sized pointers). */
typedef enum {
    HX_TRACKER_TLS_UNKNOWN = 0, /* never tried */
    HX_TRACKER_TLS_OK,          /* TLS handshake succeeded */
    HX_TRACKER_TLS_NO,          /* TLS handshake failed; use plain */
} hx_tracker_tls_verdict;

static GHashTable *tracker_tls_verdict_cache;

static hx_tracker_tls_verdict
tracker_tls_verdict_lookup (const char *url)
{
    if (!url || !tracker_tls_verdict_cache) {
        return HX_TRACKER_TLS_UNKNOWN;
    }
    return (hx_tracker_tls_verdict) GPOINTER_TO_UINT (
        g_hash_table_lookup (tracker_tls_verdict_cache, url));
}

static void
tracker_tls_verdict_record (const char *url, hx_tracker_tls_verdict v)
{
    if (!url) {
        return;
    }
    if (!tracker_tls_verdict_cache) {
        tracker_tls_verdict_cache
            = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    }
    g_hash_table_replace (tracker_tls_verdict_cache, g_strdup (url),
                          GUINT_TO_POINTER ((guint) v));
}

/* Forward decls — the protocol is a chain of one-bounce callbacks. */
static void tracker_fetch_start (struct tracker_run_ctx *run);
static void tracker_fetch_connect (struct tracker_fetch_ctx *ctx);
static void tracker_fetch_retry_v1 (struct tracker_fetch_ctx *ctx);
static void tracker_fetch_retry_plain (struct tracker_fetch_ctx *ctx);
static void on_tracker_connected (GObject *src, GAsyncResult *r, gpointer u);
static void on_tracker_magic_sent (GObject *src, GAsyncResult *r, gpointer u);
static void on_tracker_attempt_cancelled (GCancellable *src, gpointer u);
static gboolean on_tracker_v3_probe_timeout (gpointer u);
/* Shared: read 6 bytes, branch on version. */
static void on_tracker_response_6 (GObject *src, GAsyncResult *r, gpointer u);
/* v1 chain. */
static void on_tracker_v1_rest_read (GObject *src, GAsyncResult *r,
                                     gpointer u);
static void read_next_server_hdr (struct tracker_fetch_ctx *ctx);
static void on_server_hdr_read (GObject *src, GAsyncResult *r, gpointer u);
static void on_server_rest_read (GObject *src, GAsyncResult *r, gpointer u);
static void on_server_name_read (GObject *src, GAsyncResult *r, gpointer u);
static void on_server_desc_len_read (GObject *src, GAsyncResult *r, gpointer u);
static void on_server_desc_read (GObject *src, GAsyncResult *r, gpointer u);
static void tracker_emit_v1_server (struct tracker_fetch_ctx *ctx);
/* v3 chain. */
static void on_tracker_v3_features_read (GObject *src, GAsyncResult *r,
                                         gpointer u);
static void on_tracker_v3_request_sent (GObject *src, GAsyncResult *r,
                                        gpointer u);
static void on_tracker_v3_resp_hdr_read (GObject *src, GAsyncResult *r,
                                         gpointer u);
static void on_tracker_v3_payload_read (GObject *src, GAsyncResult *r,
                                        gpointer u);
static void tracker_v3_walk_and_emit (struct tracker_fetch_ctx *ctx,
                                      guint16 record_count);
/* Done / free. */
static void tracker_fetch_done (struct tracker_fetch_ctx *ctx);
static void tracker_fetch_free (struct tracker_fetch_ctx *ctx);
static void tracker_run_free (struct tracker_run_ctx *run);

static gboolean
err_is_cancel (GError *err)
{
    return err && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

void
hx_tracker_list_async (session *sess)
{
    struct tracker_run_ctx *run;
    int i;

    /* Cancel any in-flight run. Its in-flight callback (if any) will
	 * see the cancellation, walk through tracker_fetch_done, and free
	 * the orphaned run from there. */
    if (current_tracker_run) {
        struct tracker_run_ctx *old = current_tracker_run;
        current_tracker_run = NULL;
        old->aborted = TRUE;
        if (old->cur_ctx) {
            g_cancellable_cancel (old->cancel);
        } else {
            /* No in-flight callback to do the cleanup — handle it
			 * synchronously here. */
            tracker_run_free (old);
        }
    }

    if (gtkhx_prefs.num_tracker <= 0) {
        return;
    }

    run = g_new0 (struct tracker_run_ctx, 1);
    run->sess = sess;
    run->cancel = g_cancellable_new ();
    run->n_trackers = gtkhx_prefs.num_tracker;
    run->trackers = g_new0 (char *, run->n_trackers);
    for (i = 0; i < run->n_trackers; i++) {
        run->trackers[i] = g_strdup (gtkhx_prefs.tracker[i]);
    }
    run->current_index = 0;

    current_tracker_run = run;
    tracker_fetch_start (run);
}

void
tracker_kill_threads (void)
{
    struct tracker_run_ctx *run = current_tracker_run;

    if (!run) {
        return;
    }

    current_tracker_run = NULL;
    run->aborted = TRUE;
    if (run->cur_ctx) {
        /* In-flight callback will unwind and free. */
        g_cancellable_cancel (run->cancel);
    } else {
        tracker_run_free (run);
    }
}

static void
tracker_run_free (struct tracker_run_ctx *run)
{
    int i;
    if (!run) {
        return;
    }
    if (current_tracker_run == run) {
        current_tracker_run = NULL;
    }
    g_clear_object (&run->cancel);
    for (i = 0; i < run->n_trackers; i++) {
        g_free (run->trackers[i]);
    }
    g_free (run->trackers);
    g_free (run);
}

/* Watchdog source-fire callback — called on the main loop ~2s after
 * the v3 magic write completes if the response read hasn't returned
 * yet. We can't cancel the read on the run cancellable (that aborts
 * the whole tracker run); instead we cancel a per-attempt cancellable
 * which fires the chained callback below that sets v3_probe_timed_out
 * before the read callback sees the cancel. */
static gboolean
on_tracker_v3_probe_timeout (gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    ctx->v3_probe_timeout_id = 0;
    ctx->v3_probe_timed_out = 1;
    if (ctx->attempt_cancel) {
        debug_log ("tracker",
                   "%s: v3 probe timed out after %u ms; will fall back to v1",
                   ctx->serverstr, hx_tracker_v3_probe_ms ());
        g_cancellable_cancel (ctx->attempt_cancel);
    }
    return G_SOURCE_REMOVE;
}

/* Chained when run->cancel is fired by tracker_kill_threads. Cancels
 * the per-attempt cancellable so any in-flight async ops on the
 * attempt unwind. (Without this chain, attempt_cancel would never
 * see the user-driven abort.) */
static void
on_tracker_attempt_cancelled (GCancellable *src G_GNUC_UNUSED, gpointer u)
{
    GCancellable *attempt = u;
    if (attempt) {
        g_cancellable_cancel (attempt);
    }
}

/* Reset per-attempt state and kick off the GSocketClient connect.
 * Called once from tracker_fetch_start (initial attempt) and once
 * from tracker_fetch_retry_v1 (the v1 fallback). use_v3 must be
 * stamped on ctx before calling — that's what
 * on_tracker_connected reads to decide which magic to send. */
static void
tracker_fetch_connect (struct tracker_fetch_ctx *ctx)
{
    GSocketClient *client;

    /* Fresh attempt_cancel; the previous one (if any) was used by
     * the watchdog cancel and is now in permanent cancelled state. */
    g_clear_object (&ctx->attempt_cancel);
    ctx->attempt_cancel = g_cancellable_new ();
    /* Chain run->cancel into attempt_cancel so a user-driven abort
     * propagates. g_cancellable_connect returns the handler id; we
     * keep it so the cancel-callback can be cleanly removed on
     * fetch_free (avoids the attempt's destructor firing for an
     * already-freed ctx if the run cancellable lives longer). */
    ctx->attempt_cancel_link
        = g_cancellable_connect (ctx->run->cancel,
                                 G_CALLBACK (on_tracker_attempt_cancelled),
                                 ctx->attempt_cancel, NULL);

    /* GSocketClient honours GProxyResolver by default — SOCKS /
     * HTTP CONNECT proxies configured at the desktop level
     * (gsettings / libproxy) work transparently here. */
    client = g_socket_client_new ();

    /* Phase D TLS: when ctx->use_tls is set, flip the GSocketClient
     * into TLS mode and hook the shared TOFU accept-certificate
     * handler. The handler writes its trust decision into the same
     * known_hosts file the main Hotline session uses, so a tracker
     * fingerprint pin and a session-server fingerprint pin coexist
     * without stepping on each other. tls_attempted is set right
     * before the async kicks off so on_tracker_connected's failure
     * branch can tell "TLS handshake just failed" apart from "plain
     * connect failed." */
    if (ctx->use_tls) {
        g_socket_client_set_tls (client, TRUE);
        ctx->tls_endpoint.host = ctx->serverstr;
        ctx->tls_endpoint.port = ctx->port;
        g_signal_connect (client, "event",
                          G_CALLBACK (on_socket_client_event),
                          &ctx->tls_endpoint);
        ctx->tls_attempted = 1;
        debug_log ("tracker", "%s: attempting TLS connect", ctx->serverstr);
    }

    /* Connect attempt itself uses the run cancellable — if the user
     * aborts during DNS / TCP connect we want to unwind immediately;
     * we don't want a watchdog-triggered attempt_cancel to kill the
     * connect, only the response read. */
    g_socket_client_connect_to_host_async (client, ctx->serverstr, ctx->port,
                                           ctx->run->cancel,
                                           on_tracker_connected, ctx);
    g_object_unref (client);
}

static void
tracker_fetch_start (struct tracker_run_ctx *run)
{
    struct tracker_fetch_ctx *ctx;
    hx_tracker_tls_verdict cached;

    ctx = g_new0 (struct tracker_fetch_ctx, 1);
    ctx->run = run;
    ctx->serverstr = g_strdup (run->trackers[run->current_index]);
    ctx->port = HTRK_TCPPORT;
    /* First attempt: try v3. The probe-then-fallback below
     * downgrades to v1 if the tracker doesn't respond within
     * HX_TRACKER_V3_PROBE_TIMEOUT_MS. Real-world pre-v3-spec v1
     * trackers (hxtrackd, hltracker.com, ...) memcmp the full
     * 6-byte HTRK_MAGIC and silently ignore connections whose
     * version byte is 0x03 instead of 0x01 — so the watchdog is
     * what makes the fallback work. */
    ctx->use_v3 = 1;

    /* Phase D TLS: try TLS first unless the verdict cache says
     * this tracker is plain-only. UNKNOWN (first time we see
     * this tracker in this session) → try TLS. OK → try TLS
     * (silently accepts via TOFU on the second + later fetches
     * since the fingerprint is pinned). NO → skip TLS, go
     * straight to plain. Re-probes happen on each fresh process
     * launch (cache is in-memory only). */
    cached = tracker_tls_verdict_lookup (ctx->serverstr);
    ctx->use_tls = (cached != HX_TRACKER_TLS_NO);
    if (cached == HX_TRACKER_TLS_NO) {
        debug_log ("tracker",
                   "%s: cached TLS verdict = NO; skipping TLS attempt",
                   ctx->serverstr);
    }

    run->cur_ctx = ctx;

    trackconn_prog_update (run->sess, ctx->serverstr, 0, 2);

    tracker_fetch_connect (ctx);
}

/* TLS handshake failed on the first attempt — close the conn,
 * record the verdict so the next Refresh skips TLS, and reopen
 * with plain TCP. The v3-probe-then-v1-fallback machinery still
 * runs on top, so we end up with the full TLS-fail → plain → v3
 * probe → v1 fallback ladder for the worst case. */
static void
tracker_fetch_retry_plain (struct tracker_fetch_ctx *ctx)
{
    debug_log ("tracker",
               "%s: TLS handshake failed; falling back to plain TCP",
               ctx->serverstr);
    tracker_tls_verdict_record (ctx->serverstr, HX_TRACKER_TLS_NO);
    ctx->use_tls = 0;

    g_clear_object (&ctx->conn);
    ctx->in = NULL;
    ctx->out = NULL;

    if (ctx->attempt_cancel && ctx->attempt_cancel_link) {
        g_cancellable_disconnect (ctx->run->cancel, ctx->attempt_cancel_link);
        ctx->attempt_cancel_link = 0;
    }

    tracker_fetch_connect (ctx);
}

/* Close the v3-attempt connection and restart with use_v3 = 0 so
 * the next on_tracker_connected sends the 6-byte v1 magic. Called
 * by on_tracker_response_6 when the watchdog has fired (or the
 * tracker returned garbage). */
static void
tracker_fetch_retry_v1 (struct tracker_fetch_ctx *ctx)
{
    /* Cancel + clear the watchdog (defensive — it should already
     * have fired and be at id=0, but a same-tick race where the
     * read callback ran before the timeout source fired is
     * possible). */
    if (ctx->v3_probe_timeout_id) {
        g_source_remove (ctx->v3_probe_timeout_id);
        ctx->v3_probe_timeout_id = 0;
    }
    ctx->v3_probe_timed_out = 0;
    ctx->use_v3 = 0;
    debug_log ("tracker",
               "%s: retrying with v1 magic (pre-spec v1 trackers reject "
               "the 0x0003 version byte)", ctx->serverstr);

    /* Close the v3-attempt conn — the server is in a stuck state
     * (received 6 bytes that don't match its magic, fell through
     * with no action). New conn, fresh attempt_cancel. */
    g_clear_object (&ctx->conn);
    ctx->in = NULL;
    ctx->out = NULL;

    /* g_cancellable_disconnect the previous link before we drop
     * the cancellable in tracker_fetch_connect; the chain handler
     * holds a pointer to the cancellable we're about to free. */
    if (ctx->attempt_cancel && ctx->attempt_cancel_link) {
        g_cancellable_disconnect (ctx->run->cancel,
                                  ctx->attempt_cancel_link);
        ctx->attempt_cancel_link = 0;
    }

    tracker_fetch_connect (ctx);
}

static void
on_tracker_connected (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;

    ctx->conn = g_socket_client_connect_to_host_finish (G_SOCKET_CLIENT (src),
                                                        res, &err);
    if (!ctx->conn) {
        /* Phase D: a failed TLS handshake on the first attempt
         * triggers the plain-TCP fallback rather than surfacing as a
         * connect error. Distinguish "TLS handshake itself failed"
         * (G_TLS_ERROR domain — bad cert, no TLS server here, etc.)
         * from "couldn't even reach the host" (G_IO_ERROR domain —
         * DNS, refused, timeout). The latter isn't a TLS problem; a
         * plain retry on a host we can't reach won't help and would
         * just double the user's wait. Cancellations route through
         * tracker_fetch_done unchanged. */
        if (ctx->tls_attempted && err && err->domain == G_TLS_ERROR
            && !err_is_cancel (err)) {
            debug_log ("tracker",
                       "%s: TLS error during connect (%s); will retry plain",
                       ctx->serverstr, err->message);
            g_clear_error (&err);
            tracker_fetch_retry_plain (ctx);
            return;
        }
        if (!err_is_cancel (err)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: %2$s\n"), ctx->serverstr,
                              err ? err->message : _ ("connect failed"));
        }
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    /* Connect succeeded. If we got here on a TLS attempt, the
     * handshake completed cleanly + the cert was trusted (or pinned
     * via TOFU). Record the verdict so subsequent fetches skip the
     * "is this tracker even TLS?" probe overhead. */
    if (ctx->use_tls) {
        tracker_tls_verdict_record (ctx->serverstr, HX_TRACKER_TLS_OK);
    }

    ctx->in = g_io_stream_get_input_stream (G_IO_STREAM (ctx->conn));
    ctx->out = g_io_stream_get_output_stream (G_IO_STREAM (ctx->conn));

    trackconn_prog_update (ctx->run->sess, ctx->serverstr, 2, 2);

    gsize magic_len;
    if (ctx->use_v3) {
        /* Send 8-byte v3 handshake. The spec says v1 trackers will
         * read 6 bytes and respond; in practice every pre-spec v1
         * tracker we've tested (hxtrackd, hltracker.com, mhxd's
         * bundled hxtrackd) memcmp's the full 6-byte HTRK_MAGIC
         * against "HTRK\\0\\1" and silently ignores the connection
         * when byte 5 is 0x03. The probe-then-fallback watchdog
         * below catches that case. */
        if (!hx_tracker_v3_pack_handshake (ctx->buf, sizeof (ctx->buf),
                                           HTRK_V3_FEAT_IPV6)) {
            tracker_fetch_done (ctx);
            return;
        }
        magic_len = HTRK_V3_HANDSHAKE_LEN;
        debug_log ("tracker",
                   "%s: probing v3 handshake (8 bytes, features=0x%04x)",
                   ctx->serverstr, (unsigned) HTRK_V3_FEAT_IPV6);
    } else {
        /* v1 fallback: 6-byte HTRK_MAGIC ("HTRK\\0\\1"). Every
         * existing tracker accepts this. */
        memcpy (ctx->buf, HTRK_MAGIC, HTRK_MAGIC_LEN);
        magic_len = HTRK_MAGIC_LEN;
        debug_log ("tracker", "%s: sending v1 handshake (6 bytes)",
                   ctx->serverstr);
    }
    g_output_stream_write_all_async (ctx->out, ctx->buf, magic_len,
                                     G_PRIORITY_DEFAULT, ctx->run->cancel,
                                     on_tracker_magic_sent, ctx);
}

static void
on_tracker_magic_sent (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (src), res, &n,
                                           &err)) {
        if (!err_is_cancel (err)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: %2$s\n"), ctx->serverstr,
                              err ? err->message : _ ("magic write failed"));
        }
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    track_prog_update (ctx->run->sess, ctx->serverstr, 0, 0);

    /* Read 6 bytes of the response. If version is 0x0003 we'll
     * read 2 more (feature flags) and head down the v3 chain;
     * otherwise (1 or 2) we read the remaining 8 bytes of the
     * 14-byte v1 reply header and head down the v1 chain.
     *
     * Use attempt_cancel (not run->cancel) so the watchdog can
     * tear THIS read down without aborting the whole tracker
     * run. The watchdog only arms on a v3 probe; the v1 fallback
     * path doesn't time out because v1 trackers reply promptly. */
    if (ctx->use_v3) {
        /* Watchdog has to be set up BEFORE the read goes out so
         * we don't race a server that ignores us instantly. */
        if (ctx->v3_probe_timeout_id) {
            g_source_remove (ctx->v3_probe_timeout_id);
        }
        ctx->v3_probe_timeout_id
            = g_timeout_add (hx_tracker_v3_probe_ms (),
                             on_tracker_v3_probe_timeout, ctx);
    }
    g_input_stream_read_all_async (ctx->in, ctx->buf, 6, G_PRIORITY_DEFAULT,
                                   ctx->attempt_cancel,
                                   on_tracker_response_6, ctx);
}

static void
on_tracker_response_6 (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;
    gboolean read_ok = g_input_stream_read_all_finish (G_INPUT_STREAM (src),
                                                       res, &n, &err);

    /* Always tear down the probe watchdog before we look at the
     * read result. If it already fired, v3_probe_timeout_id == 0
     * — g_source_remove(0) is safe (returns FALSE, no warning
     * historically; modern GLib criticals on 0, so guard). */
    if (ctx->v3_probe_timeout_id) {
        g_source_remove (ctx->v3_probe_timeout_id);
        ctx->v3_probe_timeout_id = 0;
    }

    /* Probe-then-fallback decision. The watchdog cancels
     * attempt_cancel, which makes the read finish with
     * G_IO_ERROR_CANCELLED. Distinguish "watchdog fired" from
     * "user aborted the run":
     *
     *   - If run->cancel is also cancelled → user aborted; unwind.
     *   - Else if v3_probe_timed_out → watchdog; retry with v1.
     *   - Else (read just failed for some other reason) → real
     *     error; bail.
     *
     * A short read on the v3 attempt (n < 6) without a flagged
     * cancellation still means "server closed without speaking" —
     * also treat that as a v1 retry trigger when use_v3 is set. */
    if (!read_ok || n != 6) {
        if (g_cancellable_is_cancelled (ctx->run->cancel)) {
            g_clear_error (&err);
            tracker_fetch_done (ctx);
            return;
        }
        if (ctx->use_v3) {
            /* Either the watchdog fired (v3_probe_timed_out is
             * set) or the server hung up / sent short. Either
             * way, the tracker isn't speaking v3 — fall back. */
            g_clear_error (&err);
            tracker_fetch_retry_v1 (ctx);
            return;
        }
        /* Plain v1 attempt failed — that's a real error. */
        if (err && !err_is_cancel (err)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: %2$s\n"), ctx->serverstr,
                              err->message);
        }
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    /* Pre-decode the version field. The parser also validates the
     * "HTRK" magic prefix; if the bytes don't look like a tracker
     * response at all we abort. */
    guint16 ver = 0, feat = 0;
    if (!hx_tracker_v3_parse_handshake_response (ctx->buf, 6, &ver, &feat)) {
        if (ctx->use_v3) {
            /* The server sent SOMETHING but not a recognisable
             * HTRK magic. A pre-spec v1 tracker that wedged on
             * our v3 magic might send junk before closing; retry
             * with v1. */
            debug_log ("tracker",
                       "%s: unrecognised reply during v3 probe; "
                       "falling back to v1",
                       ctx->serverstr);
            tracker_fetch_retry_v1 (ctx);
            return;
        }
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                          _ ("tracker: %1$s: bad magic in response\n"),
                          ctx->serverstr);
        tracker_fetch_done (ctx);
        return;
    }
    ctx->version = ver;

    if (ver == HTRK_VERSION_V3) {
        debug_log ("tracker",
                   "%s: tracker speaks v3; reading 2-byte feature flags",
                   ctx->serverstr);
        /* Continue reading the trailing 2 bytes of features into
         * the same scratch buffer at offset 6 so the v3 parser
         * has the full 8-byte handshake response in one slice. */
        g_input_stream_read_all_async (ctx->in, ctx->buf + 6, 2,
                                       G_PRIORITY_DEFAULT, ctx->run->cancel,
                                       on_tracker_v3_features_read, ctx);
        return;
    }

    if (ver == HTRK_VERSION_V1 || ver == HTRK_VERSION_V2) {
        /* Server responded with a v1- or v2-shaped magic. This can
         * happen on either:
         *   - The v3 probe path: a spec-compliant v3-aware tracker
         *     reading 6 bytes, seeing 0x0003 in the request, and
         *     replying v1 because it doesn't actually implement v3.
         *     Or, more realistically, a pre-spec tracker that
         *     happened to reply quickly enough with v1 magic
         *     (unlikely in practice — we only get here if the
         *     server IS spec-compliant about reading 6 bytes).
         *   - The v1 fallback path: pre-spec v1 trackers (the
         *     common case after the watchdog fires).
         *
         * Either way, read the remaining 8 bytes of the 14-byte v1
         * reply header into buf[6..13] and dispatch to the v1
         * record-by-record chain. */
        debug_log ("tracker",
                   "%s: tracker speaks v%u; using v1 record path",
                   ctx->serverstr, (unsigned) ver);
        g_input_stream_read_all_async (ctx->in, ctx->buf + 6, 8,
                                       G_PRIORITY_DEFAULT, ctx->run->cancel,
                                       on_tracker_v1_rest_read, ctx);
        return;
    }

    /* Anything outside the {V1, V2, V3} allowlist is a tracker
     * we don't know how to talk to — a future v4 with a different
     * post-handshake shape, or some non-HTRK service that happens
     * to start with "HTRK". The 14-byte v1 reply header layout
     * would be wrong; better to bail than drive bogus record
     * reads from an unsupported wire format. */
    hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                      _ ("tracker: %1$s: unsupported HTRK version %2$u\n"),
                      ctx->serverstr, (unsigned) ver);
    debug_log ("tracker",
               "%s: unsupported HTRK version 0x%04x; bailing",
               ctx->serverstr, (unsigned) ver);
    tracker_fetch_done (ctx);
}

static void
on_tracker_v1_rest_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != 8) {
        if (err && !err_is_cancel (err)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: %2$s\n"), ctx->serverstr,
                              err->message);
        }
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    /* tracker_parser.c::hx_tracker_reply_parse_header pulls the
     * u16 BE at offset [10..11]. Pure helper so
     * test_tracker_parser pins the wire shape; see
     * tracker_parser.h for the full HTRK reply layout. */
    hx_tracker_reply_parse_header (ctx->buf, 14, &ctx->nservers);
    ctx->total = ctx->nservers;
    ctx->server_i = 1;

    /* Tell the view we're about to start emitting v1 records for this
     * tracker URL. The view uses this to create / recycle a per-tracker
     * section (and pick which columns to show — v1 sections suppress
     * Country / Caps since v1 records can't carry those TLVs). The
     * batch-begin / record-arrival ordering is guaranteed by the fact
     * that tracker_run_ctx walks trackers sequentially. */
    gtkhx_session_emit_tracker_batch_begin (gtkhx_session_get_default (),
                                            ctx->serverstr,
                                            /*version=*/1,
                                            ctx->nservers);

    track_prog_update (ctx->run->sess, ctx->serverstr, 0, ctx->total);

    read_next_server_hdr (ctx);
}

static void
read_next_server_hdr (struct tracker_fetch_ctx *ctx)
{
    if (!ctx->nservers) {
        tracker_fetch_done (ctx);
        return;
    }
    g_input_stream_read_all_async (ctx->in, ctx->buf, 8, G_PRIORITY_DEFAULT,
                                   ctx->run->cancel, on_server_hdr_read, ctx);
}

static void
on_server_hdr_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != 8) {
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    if (hx_tracker_record_is_padding (ctx->buf, 1)) {
        /* Padding slot — IPs can't start with 0. Read the next
		 * server header without advancing the counter. */
        read_next_server_hdr (ctx);
        return;
    }

    g_input_stream_read_all_async (ctx->in, ctx->buf + 8, 3, G_PRIORITY_DEFAULT,
                                   ctx->run->cancel, on_server_rest_read, ctx);
}

static void
on_server_rest_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != 3) {
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    /* Pure parse of the 11-byte fixed record prefix. See
	 * tracker_parser.h for the byte layout — addr / port /
	 * nusers / reserved[2] / name_len. */
    {
        hx_tracker_record_fixed rec;
        hx_tracker_record_parse_fixed (ctx->buf, 11, &rec);
        ctx->cur_addr     = rec.addr;
        ctx->cur_port     = rec.port;
        ctx->cur_nusers   = rec.nusers;
        ctx->cur_name_len = rec.name_len;
    }

    if (ctx->cur_name_len == 0) {
        ctx->name[0] = 0;
        g_input_stream_read_all_async (ctx->in, ctx->buf, 1, G_PRIORITY_DEFAULT,
                                       ctx->run->cancel,
                                       on_server_desc_len_read, ctx);
        return;
    }

    g_input_stream_read_all_async (ctx->in, ctx->name, ctx->cur_name_len,
                                   G_PRIORITY_DEFAULT, ctx->run->cancel,
                                   on_server_name_read, ctx);
}

static void
on_server_name_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != ctx->cur_name_len) {
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }
    ctx->name[ctx->cur_name_len] = 0;
    hx_tracker_normalize_text (ctx->name, ctx->cur_name_len);

    g_input_stream_read_all_async (ctx->in, ctx->buf, 1, G_PRIORITY_DEFAULT,
                                   ctx->run->cancel, on_server_desc_len_read,
                                   ctx);
}

static void
on_server_desc_len_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != 1) {
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }
    ctx->cur_desc_len = ctx->buf[0];
    if (ctx->cur_desc_len == 0) {
        ctx->desc[0] = 0;
        tracker_emit_v1_server (ctx);
        return;
    }
    memset (ctx->desc, 0, sizeof (ctx->desc));
    g_input_stream_read_all_async (ctx->in, ctx->desc, ctx->cur_desc_len,
                                   G_PRIORITY_DEFAULT, ctx->run->cancel,
                                   on_server_desc_read, ctx);
}

static void
on_server_desc_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != ctx->cur_desc_len) {
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }
    ctx->desc[ctx->cur_desc_len] = 0;
    hx_tracker_normalize_text (ctx->desc, ctx->cur_desc_len);

    tracker_emit_v1_server (ctx);
}

static void
tracker_emit_v1_server (struct tracker_fetch_ctx *ctx)
{
    /* Build the boxed event from the v1 record. The constructor
     * runs MacRoman → UTF-8 transcoding on the name + desc, so by
     * the time tracker.c sees them they're Pango-safe. The signal
     * emitter borrows the event for the duration of the emit;
     * boxed-type machinery handles refcount semantics for any
     * subscriber that wants to keep it.
     *
     * v1 records have no TLV trailer — tlv_count=0 + tlv_bytes=NULL
     * + tlv_bytes_len=0 — so subscribers can tell v1 vs. v3 records
     * apart by checking event->addr_type (always 0x04 for v1) and
     * event->tlv_bytes (NULL for v1). */
    HxTrackerServer *ev = hx_tracker_server_new_v1 (
        ctx->cur_addr, ctx->cur_port, ctx->cur_nusers,
        ctx->name, ctx->cur_name_len,
        ctx->desc, ctx->cur_desc_len,
        ctx->total);
    gtkhx_session_emit_tracker_server_create (gtkhx_session_get_default (), ev);
    hx_tracker_server_free (ev);

    track_prog_update (ctx->run->sess, ctx->serverstr, ctx->server_i,
                       ctx->total);
    ctx->server_i++;
    ctx->nservers--;
    read_next_server_hdr (ctx);
}

/* -------- v3 chain -------------------------------------------- */

static void
on_tracker_v3_features_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != 2) {
        if (err && !err_is_cancel (err)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: %2$s\n"), ctx->serverstr,
                              err->message);
        }
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    /* Re-parse the full 8-byte response now that we have all of
     * it; same magic / version validation as the 6-byte call. */
    guint16 ver = 0, feat = 0;
    if (!hx_tracker_v3_parse_handshake_response (ctx->buf, 8, &ver, &feat)) {
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                          _ ("tracker: %1$s: bad v3 handshake response\n"),
                          ctx->serverstr);
        tracker_fetch_done (ctx);
        return;
    }
    ctx->v3_features = feat;
    debug_log ("tracker", "%s: v3 negotiated; tracker features=0x%04x",
               ctx->serverstr, (unsigned) feat);

    if (feat & HTRK_V3_FEAT_CLIENT_AUTH) {
        /* Spec says: when FEAT_CLIENT_AUTH is set, we'd need to
         * send an AUTH request before the listing. We don't have
         * UI for tracker creds yet (none of the public trackers
         * we test against require auth) — bail loudly so the user
         * sees why no servers showed up rather than wonder. */
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                          _ ("tracker: %1$s: requires client auth (not "
                             "supported yet)\n"),
                          ctx->serverstr);
        tracker_fetch_done (ctx);
        return;
    }

    /* Send the 4-byte minimum-viable listing request. */
    gsize req_len = 0;
    if (!hx_tracker_v3_pack_listing_request_simple (
            ctx->v3_req_buf, sizeof (ctx->v3_req_buf), &req_len)) {
        tracker_fetch_done (ctx);
        return;
    }
    g_output_stream_write_all_async (ctx->out, ctx->v3_req_buf, req_len,
                                     G_PRIORITY_DEFAULT, ctx->run->cancel,
                                     on_tracker_v3_request_sent, ctx);
}

static void
on_tracker_v3_request_sent (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (src), res, &n,
                                           &err)) {
        if (!err_is_cancel (err)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: %2$s\n"), ctx->serverstr,
                              err ? err->message
                                  : _ ("v3 listing request write failed"));
        }
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    /* Read the 10-byte v3 response header. */
    g_input_stream_read_all_async (
        ctx->in, ctx->v3_resp_hdr, HTRK_V3_RESP_HDR_LEN, G_PRIORITY_DEFAULT,
        ctx->run->cancel, on_tracker_v3_resp_hdr_read, ctx);
}

static void
on_tracker_v3_resp_hdr_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != HTRK_V3_RESP_HDR_LEN) {
        if (err && !err_is_cancel (err)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: %2$s\n"), ctx->serverstr,
                              err->message);
        }
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    guint16 type = 0, total_servers = 0, record_count = 0;
    guint32 total_size = 0;
    if (!hx_tracker_v3_parse_response_header (ctx->v3_resp_hdr,
                                              HTRK_V3_RESP_HDR_LEN, &type,
                                              &total_size, &total_servers,
                                              &record_count)) {
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                          _ ("tracker: %1$s: bad v3 response header\n"),
                          ctx->serverstr);
        tracker_fetch_done (ctx);
        return;
    }
    debug_log ("tracker",
               "%s: v3 response — total_size=%u total=%u records=%u",
               ctx->serverstr, (unsigned) total_size,
               (unsigned) total_servers, (unsigned) record_count);

    /* Per-batch total drives the track_prog_update progress ticker
     * AND the HxTrackerServer.total field that subscribers see.
     * Use record_count (records IN THIS message), not total_servers
     * (full match count across all pages). When the tracker
     * paginates (total_servers > record_count) — Phase A doesn't
     * request pagination yet, but a v3 tracker MAY chunk anyway —
     * using total_servers leaves the progress widget short of done
     * after we've emitted the whole batch we have. */
    ctx->total = (int) record_count;
    ctx->server_i = 1;
    track_prog_update (ctx->run->sess, ctx->serverstr, 0, ctx->total);

    /* Tell the view we're about to start emitting v3 records for this
     * tracker. Even when record_count is 0 we still emit so the view
     * creates the (empty) section — feedback that the tracker WAS
     * contacted and replied, even if it had nothing to list. */
    gtkhx_session_emit_tracker_batch_begin (gtkhx_session_get_default (),
                                            ctx->serverstr,
                                            /*version=*/3,
                                            record_count);

    if (total_size == 0 && record_count == 0) {
        /* Empty listing — clean finish, no records to read. */
        tracker_fetch_done (ctx);
        return;
    }
    if (total_size == 0 || record_count == 0) {
        /* Inconsistent header: one is zero but not the other.
         * Either the tracker promised records with no payload to
         * carry them, or sent a payload-sized response with no
         * records to read. Both are malformed; bail loudly rather
         * than treat as empty. */
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                          _ ("tracker: %1$s: malformed v3 response header "
                             "(total_size=%2$u, record_count=%3$u)\n"),
                          ctx->serverstr, (unsigned) total_size,
                          (unsigned) record_count);
        tracker_fetch_done (ctx);
        return;
    }
    if (total_size > HX_TRACKER_V3_MAX_PAYLOAD) {
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                          _ ("tracker: %1$s: v3 response too large "
                             "(%2$u bytes, cap %3$u)\n"),
                          ctx->serverstr, (unsigned) total_size,
                          (unsigned) HX_TRACKER_V3_MAX_PAYLOAD);
        tracker_fetch_done (ctx);
        return;
    }

    /* Stash record_count so the post-read walker knows how many
     * records to expect. Reuse the v1-side nservers slot. */
    ctx->nservers = record_count;
    ctx->v3_payload_len = total_size;
    ctx->v3_payload = g_malloc (total_size);

    g_input_stream_read_all_async (ctx->in, ctx->v3_payload, total_size,
                                   G_PRIORITY_DEFAULT, ctx->run->cancel,
                                   on_tracker_v3_payload_read, ctx);
}

static void
on_tracker_v3_payload_read (GObject *src, GAsyncResult *res, gpointer u)
{
    struct tracker_fetch_ctx *ctx = u;
    GError *err = NULL;
    gsize n = 0;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (src), res, &n, &err)
        || n != ctx->v3_payload_len) {
        if (err && !err_is_cancel (err)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: %2$s\n"), ctx->serverstr,
                              err->message);
        }
        g_clear_error (&err);
        tracker_fetch_done (ctx);
        return;
    }

    tracker_v3_walk_and_emit (ctx, ctx->nservers);
    tracker_fetch_done (ctx);
}

static void
tracker_v3_walk_and_emit (struct tracker_fetch_ctx *ctx, guint16 record_count)
{
    /* Walk the response payload record-by-record. Each
     * parse_record call borrows pointers into ctx->v3_payload —
     * no copies until we hand the bytes to hx_tracker_server_new_v3
     * which g_strndups / g_bytes_news what it keeps. */
    const guint8 *buf = ctx->v3_payload;
    gsize remaining = ctx->v3_payload_len;
    guint16 i;
    for (i = 0; i < record_count; i++) {
        hx_tracker_v3_record rec = { 0 };
        gsize consumed = 0;
        if (!hx_tracker_v3_parse_record (buf, remaining, &rec, &consumed)) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              _ ("tracker: %1$s: malformed record %2$u/%3$u "
                                 "in v3 response\n"),
                              ctx->serverstr, (unsigned) (i + 1),
                              (unsigned) record_count);
            return;
        }

        HxTrackerServer *ev = hx_tracker_server_new_v3 (
            rec.addr_type, rec.address, rec.address_len, rec.port, rec.nusers,
            (const char *) rec.name, rec.name_len,
            (const char *) rec.desc, rec.desc_len,
            rec.tlv_count, rec.tlv_bytes, rec.tlv_bytes_len, ctx->total);
        if (ev) {
            gtkhx_session_emit_tracker_server_create (
                gtkhx_session_get_default (), ev);
            hx_tracker_server_free (ev);

            track_prog_update (ctx->run->sess, ctx->serverstr, ctx->server_i,
                               ctx->total);
            ctx->server_i++;
        } else {
            debug_log ("tracker",
                       "%s: dropping record %u (constructor rejected "
                       "addr_type=0x%02x)",
                       ctx->serverstr, (unsigned) (i + 1),
                       (unsigned) rec.addr_type);
        }

        buf += consumed;
        remaining -= consumed;
    }

    /* Trailing-bytes check, symmetric with the strict-leftover
     * contract in hx_tracker_v3_walk_tlvs. After consuming
     * record_count records, the cursor should land exactly at
     * the end of the payload — total_size is what the response
     * header declared and we read that many bytes off the wire.
     * Anything left over means the tracker emitted a record_count
     * that's smaller than the actual content, or padded the
     * payload past the last declared record. Either way we'd be
     * silently discarding wire data; log loudly so a malformed
     * tracker (or a v3 spec mismatch) surfaces. */
    if (remaining != 0) {
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                          _ ("tracker: %1$s: v3 response had %2$u trailing "
                             "bytes after %3$u records\n"),
                          ctx->serverstr, (unsigned) remaining,
                          (unsigned) record_count);
        debug_log ("tracker",
                   "%s: v3 trailing bytes — record_count=%u, "
                   "first leftover byte=0x%02x",
                   ctx->serverstr, (unsigned) record_count,
                   (unsigned) *buf);
    }
}

static void
tracker_fetch_done (struct tracker_fetch_ctx *ctx)
{
    struct tracker_run_ctx *run = ctx->run;

    run->cur_ctx = NULL;
    tracker_fetch_free (ctx);

    if (run->aborted) {
        tracker_run_free (run);
        return;
    }

    run->current_index++;
    if (run->current_index >= run->n_trackers) {
        tracker_run_free (run);
        return;
    }
    tracker_fetch_start (run);
}

static void
tracker_fetch_free (struct tracker_fetch_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->v3_probe_timeout_id) {
        g_source_remove (ctx->v3_probe_timeout_id);
        ctx->v3_probe_timeout_id = 0;
    }
    /* Disconnect the run-cancel → attempt-cancel chain BEFORE
     * dropping attempt_cancel — the chained handler holds a raw
     * pointer to it and would otherwise read freed memory if the
     * run cancellable fires after free. */
    if (ctx->attempt_cancel && ctx->attempt_cancel_link) {
        g_cancellable_disconnect (ctx->run->cancel,
                                  ctx->attempt_cancel_link);
        ctx->attempt_cancel_link = 0;
    }
    g_clear_object (&ctx->attempt_cancel);
    g_clear_object (&ctx->conn);
    g_free (ctx->serverstr);
    g_free (ctx->v3_payload);    /* NULL-safe; only set on v3 path */
    g_free (ctx);
}

void
kill_threads (void)
{
    /* Phase 5+: cancel the async connect chain. Safe whether or not
	 * one's in flight. */
    if (current_cancel) {
        g_cancellable_cancel (current_cancel);
        g_clear_object (&current_cancel);
    }
    /* And the async tracker fetch, which has its own GCancellable
	 * inside current_tracker_run. */
    tracker_kill_threads ();
}

void
hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
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
            guint16 t = (guint16)va_arg (ap_trace, int);
            guint16 l = (guint16)va_arg (ap_trace, int);
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

    control_arm_write_source (htlc);
    if (htlc->compress_encode_type != COMPRESS_NONE) {
        len = compress_encode (htlc, this_off, len);
        /* compress_encode fails closed via hx_htlc_close when the
         * Rust codec can't produce output — see the comment block in
         * src/compress.c::compress_encode for the rationale. Once
         * the connection is torn down, htlc->fd is zero and any
         * further cipher_encode call would operate on a stale buffer
         * that the socket-write loop will never flush. Skip out of
         * the rest of this send so we don't pretend to send a
         * message we couldn't compress. */
        if (!htlc->fd) {
            return;
        }
    }
    if (htlc->cipher_encode_type != CIPHER_NONE) {
        cipher_encode (htlc, this_off, len);
    }
}

/* Chunk-array variant of hlwrite. Same trace + write + cipher +
 * compress side-effects, but the chunks come from a caller-built
 * array (via login_packet.c::hx_login_build_chunks, or whatever
 * future shared message builder lands). Pure delivery side: no
 * idea what HTLC_HDR_* type the chunks belong to, that's caller-
 * supplied.
 *
 * Mirrors hlwrite's structure so any future per-chunk hook we add
 * (e.g. AEAD framing on a per-message basis) only has to land in
 * two adjacent functions instead of N callers.
 */
void
hlwrite_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                const struct hx_chunk *chunks, int hc)
{
    /* Public-API guardrails. Mirror hlpack_chunks's checks here too
	 * so a caller bug fails before we open a proto trace block (a
	 * pack-guard trip would leave an unmatched send_begin → noisy
	 * trace output and an FDW write of zero bytes). Per-chunk
	 * data-pointer validity is checked again inside hlpack_chunks;
	 * we just gate the top-level ones. */
    g_return_if_fail (htlc != NULL);
    g_return_if_fail (hc >= 0);
    g_return_if_fail (hc == 0 || chunks != NULL);

    guint32 this_off, len;

    if (!htlc->fd) {
        return;
    }

    this_off = htlc->out.pos + htlc->out.len;

    /* Pack first, trace after — that way a g_return_if_fail trip
	 * inside hlpack_chunks (per-chunk NULL data with len > 0)
	 * doesn't leave an open trace block. Capture trans BEFORE the
	 * pack call since hlpack_chunks bumps htlc->trans. */
    guint32 my_trans = htlc->trans;
    hlpack_chunks (htlc, type, flag, chunks, hc);

    proto_trace_send_begin (type, my_trans, hc);
    for (int i = 0; i < hc; i++) {
        proto_trace_send_chunk (chunks[i].type, chunks[i].len,
                                chunks[i].data);
    }
    proto_trace_send_end ();

    len = (htlc->out.pos + htlc->out.len) - this_off;

    control_arm_write_source (htlc);
    if (htlc->compress_encode_type != COMPRESS_NONE) {
        len = compress_encode (htlc, this_off, len);
        /* compress_encode fails closed via hx_htlc_close when the
         * Rust codec can't produce output — see the comment block in
         * src/compress.c::compress_encode for the rationale. Once
         * the connection is torn down, htlc->fd is zero and any
         * further cipher_encode call would operate on a stale buffer
         * that the socket-write loop will never flush. Skip out of
         * the rest of this send so we don't pretend to send a
         * message we couldn't compress. */
        if (!htlc->fd) {
            return;
        }
    }
    if (htlc->cipher_encode_type != CIPHER_NONE) {
        cipher_encode (htlc, this_off, len);
    }
}

/* hl_code lives in src/hl_code.c so the Tier 1 unit test can link
 * it without dragging in the rest of network.c's deps. The declaration
 * stays in network.h (extern) for back-compat with existing callers
 * that didn't include hl_code.h directly. */
