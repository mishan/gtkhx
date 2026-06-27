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
#include <errno.h>
#include <sys/time.h>
#include <sys/select.h>
#include <time.h>

#include <signal.h>
#include <stdarg.h>

#include "compress.h"
#include "hx.h"
#include "gtkhx_session.h"
#include "hxnet_bridge.h"
#include "rcv.h"
#include "gtkutil.h"
#include "chat.h"
#include "tasks.h"
#include "users.h"
#include "inet.h"
#include "log.h"
#include "proto_trace.h"
#include "tls_trust.h"
#include "tls_trust_dialog.h"
#include "inline_media.h"
#include "toolbar.h"
#include "tracker.h"
#include "network.h"
#include "banner.h"
#include "debug.h"
#include "htxf_io.h"           /* HxnetHopeAead, hxnet_htxf_open, hxnet_hope_aead_free */
#include "cipher.h"
#include "voice_runtime.h"
#include "voice_model.h"

#include "login_packet.h"
#include "agreement_packet.h"
#include "hl_code.h"
#include "proto_helpers.h"
#include "htxf_subchannel.h"
#include "tracker_parser.h"
#include "tracker_v3.h"
#include "tracker_event.h"

char *server_addr;
guint16 server_port;

#if 0 /* XXX */
struct log *server_log = NULL;
#endif

/* The connect + magic-exchange flow runs on the main loop via
 * GSocketClient's async API; cancellation goes through current_cancel. */
static GCancellable *current_cancel;


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

/* PING keepalive. Some servers (hlserver.com is the known
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
    /* PING is a zero-chunk opcode. Send directly through
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
    inline_media_reset_advisory_limits (htlc);

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

    /* Tear down the hxnet handle if it was installed. Drops the
     * ConnectionHandle, which the actor sees as HandleDropped;
     * the wrapped TcpStream's Drop closes the duped fd. Safe
     * to call whether or not the bridge was active. */
    hx_bridge_uninstall ();

    htlc->ip_addr[0] = '\0';

    if (htlc->in.buf) {
        g_free (htlc->in.buf);
        htlc->in.buf = NULL;
    }
    /* Reset pos/len alongside the buf free. qbuf_set's grow
     * heuristic compares `pos + len < new_pos + new_len`; with
     * pos/len left over from the previous frame the heuristic
     * can decide "no realloc needed" and leave buf NULL, which
     * is what the bridge's `hx_bridge_dispatch_frame` write-
     * through-NULL crash was. The dispatch path now also
     * early-returns when fd==0, but resetting here keeps the
     * qbuf invariant tight regardless of who's reading it
     * next. */
    htlc->in.pos = 0;
    htlc->in.len = 0;
    if (htlc->out.buf) {
        g_free (htlc->out.buf);
        htlc->out.buf = NULL;
    }
    htlc->out.pos = 0;
    htlc->out.len = 0;
    /* hxd_files[fd] no longer used for the control channel — the
     * hxnet orchestrator owns the control socket, so there is no
     * hxd_fd_set GIOChannel watch on it to register; no slot to
     * clear. */
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

    /* chats live in a GHashTable<u32 cid, struct chat*>.
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

    /* tasks live in a GHashTable<u32 trans, struct task*>.
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

    /* htlc no longer carries an addrinfo —
	 * the post-connect peer-identification now lives in plain
	 * htlc->serverhost / serverport / ip_addr fields, none of which
	 * need explicit teardown. The legacy freeaddrinfo() call (and
	 * the conn_addr shim that briefly replaced it) belonged here. */

    memset (htlc->cipher_encode_key, 0, sizeof (htlc->cipher_encode_key));
    memset (htlc->cipher_decode_key, 0, sizeof (htlc->cipher_decode_key));
    /* No per-direction stream-cipher state to free: the orchestrator
     * (hxnet) owns all control-channel crypto now, so the legacy C
     * Blowfish union member is never allocated on htlc. The memsets
     * below clear the (always-zero) cipher state defensively. */
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
    /* No per-direction compression state to tear down: the orchestrator
     * (hxnet, via the Rust hxcompress crate) owns the control-channel
     * compression now, so the legacy C compress_*_state / gzip counters
     * are never populated on htlc. The zeroing below clears the
     * (always-zero) fields defensively. */
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
    /* Release the opaque HOPE AEAD material handle seeded at login. */
    if (htlc->hope_aead) {
        hxnet_hope_aead_free (htlc->hope_aead);
        htlc->hope_aead = NULL;
    }

#if 0 /* XXX */
	close_log(server_log);
	server_log = NULL;
#endif

    g_free (server_addr);
    server_addr = NULL;
}

/* The post_* marshal helpers (post_prog / post_ts / post_log) lived
 * here until the tracker fetch went async (see hx_tracker_list_async
 * below). The worker that consumed them is gone — every callback in
 * the new design runs on the main loop, so the trackconn_prog_update /
 * track_prog_update /
 * gtkhx_session_emit_tracker_server_create / hx_printf_prefix calls
 * happen directly. */

/* HTLC_HDR_AGREEMENTAGREE with NAME + ICON. Sent from
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

/* Identifying tuple threaded through the GSocketClient::event signal
 * to the TLS accept-certificate handler (still used by the tracker
 * TLS connect, tracker_fetch_connect). The accept handler body lives
 * later in the file; this gives the struct its layout. */
struct tls_endpoint {
    const char *host;
    guint16 port;
};


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

/* Shared TOFU decision over a (host, port, fingerprint) tuple: look
 * the fingerprint up in the known-hosts store and accept silently
 * (TRUSTED, or the same cert already pinned for this host on another
 * port), auto-accept (GTKHX_TLS_AUTO_ACCEPT, for headless tests),
 * or prompt the user (UNKNOWN / MISMATCH), pinning on accept. A
 * GTKHX_TLS_TEST_PROMPT=accept|reject seam (test-only) substitutes the
 * prompt verdict so headless tests can drive the reject path too.
 * Returns TRUE to accept the cert, FALSE to reject.
 *
 * Used by BOTH the legacy GTlsConnection accept-certificate handler
 * (tls_accept_certificate, which computes the fingerprint from a
 * GTlsCertificate) and the orchestrator's post-handshake verify
 * (hx_tls_orchestrator_verify_cert, fingerprint computed in Rust).
 * Safe off the main thread — the prompt marshals via
 * trust_dialog_run_thread_safe. */
static gboolean
tls_trust_decide (const char *host, guint16 port, const char *fingerprint)
{
    hx_tls_trust_status status =
        hx_tls_trust_lookup (host, port, fingerprint);
    debug_log ("tls", "trust-decide: %s:%u fp=%s status=%d", host,
               (unsigned) port, fingerprint, (int) status);

    if (status == HX_TLS_TRUST_TRUSTED) {
        return TRUE;
    }

    /* Same cert pinned for this host on another port (e.g. control
     * channel pinned at :5600, HTXF subchannel now at :5601) — accept
     * + pin the new port silently. Only on a strict-UNKNOWN; never
     * overrides a MISMATCH. */
    if (status == HX_TLS_TRUST_UNKNOWN
        && hx_tls_trust_host_has_fingerprint (host, fingerprint)) {
        schedule_trust_pin (host, port, fingerprint);
        return TRUE;
    }

    /* Headless / scripted escape hatch. Tier 3 sets this; production
     * never does. MISMATCH is logged loudly so a silent override
     * can't hide a real fingerprint change. */
    const char *auto_accept = g_getenv ("GTKHX_TLS_AUTO_ACCEPT");
    if (auto_accept && *auto_accept) {
        if (status == HX_TLS_TRUST_MISMATCH) {
            g_warning ("GTKHX_TLS_AUTO_ACCEPT overriding TLS MISMATCH for "
                       "%s:%u (fp=%s) — the pinned fingerprint differs from "
                       "this one. Set this env var only for trusted test "
                       "harnesses; production should never see this line.",
                       host, (unsigned) port, fingerprint);
        }
        schedule_trust_pin (host, port, fingerprint);
        return TRUE;
    }

    /* Test-only prompt-verdict seam. GTKHX_TLS_TEST_PROMPT=accept|reject
     * substitutes the human's dialog click without a GUI, so headless
     * Tier 3 can drive BOTH outcomes — including the reject path, which
     * GTKHX_TLS_AUTO_ACCEPT can never exercise (it always accepts). The
     * real classify (the lookup above) and the real pin-on-accept still
     * run; only the click is stubbed. Production never sets this.
     *
     * Only the exact tokens "accept" / "reject" are honoured. Any other
     * value (a typo, a stale or misconfigured export) is ignored and we
     * fall through to the real prompt — never an implicit reject that
     * silently bypasses the dialog. */
    gboolean accepted;
    const char *test_prompt = g_getenv ("GTKHX_TLS_TEST_PROMPT");
    gboolean test_accept
        = test_prompt && g_ascii_strcasecmp (test_prompt, "accept") == 0;
    gboolean test_reject
        = test_prompt && g_ascii_strcasecmp (test_prompt, "reject") == 0;
    if (test_accept || test_reject) {
        accepted = test_accept;
        debug_log ("tls", "trust-decide: GTKHX_TLS_TEST_PROMPT=%s -> %s",
                   test_prompt, accepted ? "accept" : "reject");
    } else {
        if (test_prompt && *test_prompt) {
            debug_log ("tls",
                       "trust-decide: ignoring GTKHX_TLS_TEST_PROMPT=%s "
                       "(expected accept|reject) — using the real prompt",
                       test_prompt);
        }
        /* Real user-facing TOFU prompt, marshalled to the main thread. */
        GtkWindow *parent = NULL;
        if (toolbar_window && GTK_IS_WINDOW (toolbar_window)) {
            parent = GTK_WINDOW (toolbar_window);
        }
        accepted = trust_dialog_run_thread_safe (parent, host, port,
                                                 fingerprint, status);
    }
    if (accepted) {
        schedule_trust_pin (host, port, fingerprint);
    }
    return accepted;
}

/* Orchestrator (hxnet) TOFU verify. The Rust TLS lifecycle computes
 * the peer leaf cert's SHA-256 fingerprint — matching
 * hx_tls_trust_fingerprint's "sha256:<hex>" format — and calls this
 * via the verify_cert bridge callback after the handshake, before any
 * LOGIN. host/port come from htlc (stamped in
 * hx_connect_via_orchestrator). Returns TRUE to accept, FALSE to
 * reject (the Rust lifecycle then closes the stream). */
gboolean
hx_tls_orchestrator_verify_cert (struct htlc_conn *htlc,
                                 const char *fingerprint)
{
    if (!htlc || !fingerprint) {
        return FALSE;
    }
    return tls_trust_decide (htlc->serverhost, htlc->serverport, fingerprint);
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

    debug_log ("tls", "accept-certificate: %s:%u errors=0x%x", host,
               (unsigned) port, (unsigned) errors);
    /* The decision (lookup → any-port → auto-accept → prompt → pin)
     * is shared with the orchestrator path; see tls_trust_decide. */
    return tls_trust_decide (host, port, fingerprint);
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

/* Phase G: pinned LOGIN transaction id. LOGIN is always the first
 * transaction on a fresh connection, so the C side and the Rust
 * orchestrator agree on a fixed value up front — the orchestrator
 * stamps the LOGIN frame with this trans, the server echoes it in
 * the TASK reply, and the synthetic-frame replay dispatches to the
 * login task we register under the same value. See
 * docs/rust/phase-g-migration.md. */
#define HX_LOGIN_TRANS 1u

/* Trans the replayed LOGIN (or HOPE step-2) reply will carry, stashed
 * by hx_connect_via_orchestrator and consumed by
 * hx_orchestrator_register_login_task when the bridge's LOGIN_SENDING
 * state fires. Single-connection scope (see sess_from_htlc). */
static guint32 orchestrator_login_reply_trans = HX_LOGIN_TRANS;

/* Register the orchestrator's "login" protocol task. Called from the
 * bridge's LOGIN_SENDING state callback — i.e. after magic completes
 * and just before the credentials reply comes back, matching the
 * legacy send_login timing so the Tasks window shows the login task
 * only once the connection is up (not concurrently with the coarse
 * "Connecting" task the way an up-front registration did).
 *
 * The replayed reply dispatches here via hx_rcv_hdr -> task_with_trans,
 * so the task must be keyed on orchestrator_login_reply_trans. The
 * NULL ptr arg selects rcv_task_login's post-login (else) branch.
 * htlc->trans is currently the post-login send counter
 * (reply_trans + 1); set it to reply_trans for the task_new key, then
 * restore so post-login sends don't collide. */
void
hx_orchestrator_register_login_task (struct htlc_conn *htlc)
{
    if (!htlc) {
        return;
    }
    /* Idempotent: never double-register (LOGIN_SENDING fires once, but
     * guard anyway so a stray repeat can't strand a duplicate row). */
    if (task_with_trans (&the_session, orchestrator_login_reply_trans)) {
        return;
    }
    guint32 saved = htlc->trans;
    htlc->trans = orchestrator_login_reply_trans;
    task_new (htlc, RCV_TASK_FN (rcv_task_login), 0, 0, "login");
    htlc->trans = saved;
}

/* Phase G (hxnet-owns-the-whole-lifecycle). The sole control-channel
 * connect path — hx_connect dispatches here unconditionally now that
 * the legacy GSocketClient connect + GPollable handshake (and its
 * gate) are gone. hxnet owns the socket from byte zero, drives the
 * whole handshake itself
 * (magic + LOGIN for plaintext; magic + HOPE step1/step2 + cipher
 * transition for secure; TLS handshake then plaintext LOGIN for tls),
 * and replays the final reply to us as a synthetic frame so
 * rcv_task_login (and its post-login side effects) run unchanged.
 * `secure` selects the HOPE path (htlc->cipheralg must be set); `tls`
 * selects TLS-from-byte-zero (separate-port model). The two are never
 * both set when this is called — hx_connect rejects secure+tls
 * (redundant double-encryption) up front, before dispatching here,
 * so it never reaches this function. */
static void
hx_connect_via_orchestrator (struct htlc_conn *htlc, const char *serverstr,
                             guint16 port, const char *login, const char *pass,
                             gboolean secure, gboolean tls)
{
    /* 1. Preamble — mirrors hx_connect's non-socket setup. */
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

    htlc->chat_history_last_msgid = 0;
    g_strlcpy (htlc->serverhost, serverstr, sizeof (htlc->serverhost));
    htlc->serverport = port;
    /* Stamp the TLS flag so the HTXF subchannel workers (xfers.c /
     * banner.c) wrap their data ports too. The control channel's TLS
     * runs inside hxnet (rustls); HTXF keeps using the legacy
     * GTlsConnection path, which reads htlc->tls. */
    htlc->tls = tls ? 1 : 0;
    htlc->gdk_input = 0;
    g_strlcpy (htlc->login, login ? login : "", sizeof (htlc->login));

    /* Seed htlc->ip_addr from the server string so the post-login
	 * "<addr>: login successful" line in rcv_task_login isn't "?".
	 * The legacy path fills this with the resolved numeric IP via
	 * populate_htlc_remote_ip; the orchestrator owns the socket and
	 * doesn't surface the peer addr yet, so the connect target is
	 * the best display string we have. TODO: plumb the resolved
	 * SocketAddr out of the hxnet lifecycle to match the legacy
	 * numeric-IP display exactly. */
    g_strlcpy (htlc->ip_addr, serverstr, sizeof (htlc->ip_addr));

    hx_printf_prefix (htlc, 0, INFOPREFIX, _ ("connecting to %s\n"),
                      server_addr);
    gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
                                         GTKHX_CONNECTION_CONNECTING);

    /* 2. Pin the LOGIN trans. The replayed reply dispatches to
     * rcv_task_login only if a task is registered under the trans the
     * orchestrator's LOGIN carries (hx_rcv_task -> task_with_trans; a
     * miss is a silent fallthrough). The orchestrator owns the send,
     * so pin the trans to a shared constant.
     *
     * Unlike the earlier draft, the login task is NOT registered here.
     * Registering it up front made it appear in the Tasks window
     * concurrently with the coarse "Connecting" task for the whole
     * connect — different from the legacy path, where the login task
     * only appears once the connection is up and credentials are going
     * out. Instead we stash the reply trans and register the task
     * lazily from the bridge's LOGIN_SENDING state callback
     * (hx_orchestrator_register_login_task), which fires after magic
     * and before the replayed reply — matching legacy's send_login
     * timing. The +1 bump keeps post-login follow-up sends (issued
     * from inside rcv_task_login) from colliding with the login task;
     * the deferred registration restores this value afterwards.
     *
     * The replayed reply's trans differs by mode: the plaintext path
     * replays the LOGIN reply (trans HX_LOGIN_TRANS); the HOPE path
     * replays the step-2 reply, which carries HX_LOGIN_TRANS+1 (the
     * orchestrator sends step 1 as HX_LOGIN_TRANS, step 2 as +1). */
    orchestrator_login_reply_trans = secure ? (HX_LOGIN_TRANS + 1)
                                            : HX_LOGIN_TRANS;
    htlc->trans = orchestrator_login_reply_trans + 1;

    /* 3. fd sentinel. The orchestrator owns the socket; the C side
     * has no real fd. Use -1 (not 0) — hx_bridge_dispatch_frame
     * treats fd==0 as "connection closed, drop the frame", so 0 here
     * would silently drop the replayed LOGIN reply. -1 keeps the
     * `if (htlc->fd) hx_htlc_close()` close-time guards firing.
     * hx_htlc_close tears the connection down via the hxnet handle /
     * current_conn, not close(htlc->fd), so the sentinel value is
     * never passed to close(2); hxnet's TcpStream owns the socket. */
    htlc->fd = -1;

    /* 4-6. Hand off to the bridge, which calls
     * hxnet_connection_open_plaintext with the bridge's event /
     * shutdown / state callbacks and stores the returned handle in
     * the global slot synchronously, BEFORE returning. That ordering
     * is load-bearing: hx_bridge_dispatch_frame gates on
     * hx_bridge_is_installed(), and the orchestrator emits the
     * replayed LOGIN-reply frame before HandshakeDone. The forwarder
     * delivers events on the GLib main loop, which we don't re-enter
     * until this function returns — so installing synchronously here
     * closes the window. */
    /* Advertise the same capability bits the legacy LOGIN does
	 * (network.c::send_login) so extensions — chat-history,
	 * inline-media, voice — negotiate on the orchestrator path too.
	 * Without this chunk the server never sees our capabilities and
	 * silently falls back to the legacy feature set. */
    guint16 caps = HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING
                 | HTLC_CAP_CHAT_HISTORY | HTLC_CAP_VOICE
                 | HTLC_CAP_INLINE_MEDIA;
    /* HOPE sends the display name in step 2; the plaintext paths defer
     * it to a post-login USER_CHANGE, so they omit the name here. */
    gboolean ok;
    if (tls) {
        /* plaintext LOGIN over TLS (secure+tls is gated out upstream). */
        ok = hx_bridge_install_orchestrated_plaintext_tls (
            htlc, serverstr, port, login, pass, /*name=*/"", htlc->icon,
            /*version=*/185, caps, HX_LOGIN_TRANS);
    } else if (secure) {
        ok = hx_bridge_install_orchestrated_hope (
            htlc, serverstr, port, login, pass, htlc->name, htlc->icon,
            /*version=*/185, caps, HX_LOGIN_TRANS, htlc->cipheralg);
    } else {
        ok = hx_bridge_install_orchestrated_plaintext (
            htlc, serverstr, port, login, pass, /*name=*/"", htlc->icon,
            /*version=*/185, caps, HX_LOGIN_TRANS);
    }
    if (!ok) {
        /* Spawn refused. Roll back the sentinel and surface a
         * disconnect so the UI doesn't sit on the CONNECTING throbber
         * forever. No login task to roll back — it isn't registered
         * until the LOGIN_SENDING state, which a refused spawn never
         * reaches. */
        htlc->fd = 0;
        gtkhx_session_emit_connection_state (gtkhx_session_get_default (),
                                             GTKHX_CONNECTION_DISCONNECTED);
        hx_printf_prefix (htlc, 0, INFOPREFIX,
                          _ ("could not start connection to %s\n"),
                          server_addr);
    }
}

/* hxnet's orchestrator owns the whole connect lifecycle. The legacy
 * GSocketClient connect path and the GTKHX_NEW_CONNECT / GTKHX_OLD_CONNECT
 * gate (and the PHASE_G_DEFAULT_ON constant behind it) were removed with
 * delete-old-connect, so hx_connect unconditionally dispatches to
 * hx_connect_via_orchestrator below — the only choice left is which
 * transport mode (plaintext / TLS / HOPE) it runs. */

void
hx_connect (struct htlc_conn *htlc, const char *serverstr, guint16 port,
            const char *login, const char *pass, char secure, char tls)
{
    /* HOPE-over-TLS is not a supported combination, on ANY path. TLS
     * already secures the transport, so layering the HOPE cipher on
     * top is redundant double-encryption we don't support. Reject it
     * up front — before dispatching to the orchestrator below — rather
     * than silently picking one. The Connect dialog
     * greys out HOPE when TLS is on; this is the defensive backstop
     * for bookmarks / programmatic callers. We leave an *established*
     * connection alone (return before the teardown preamble), but we
     * still cancel any in-flight async connect first — otherwise a
     * connect that was still resolving/connecting when the user fired
     * this rejected request would complete later and surface
     * unexpectedly. */
    {
        const char *tls_env = g_getenv ("GTKHX_TLS");
        gboolean want_tls = tls || (tls_env && *tls_env);
        if (secure && want_tls) {
            if (current_cancel) {
                g_cancellable_cancel (current_cancel);
                g_clear_object (&current_cancel);
            }
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              _ ("HOPE-secure login over TLS is not "
                                 "supported; use one or the other\n"));
            error_dialog ("Error",
                          "HOPE-secure login can't be combined with TLS. "
                          "TLS already encrypts the connection — turn off "
                          "the HOPE cipher (or turn off TLS) and reconnect.");
            return;
        }
    }

    /* hxnet owns the whole connect lifecycle. The orchestrator covers
     * every supported case: plaintext, plaintext-over-TLS (separate-port
     * model), and HOPE-Secure-Login — with a negotiated cipher, or, for
     * a non-cipher_only server, HMAC authentication over a plaintext
     * transport. HOPE-over-TLS is rejected above. The GTKHX_TLS env
     * override folds into want_tls. */
    const char *tls_env = g_getenv ("GTKHX_TLS");
    gboolean want_tls = tls || (tls_env && *tls_env);
    if (want_tls && !secure) {
        /* Plaintext LOGIN over TLS-from-byte-zero. */
        hx_connect_via_orchestrator (htlc, serverstr, port, login, pass,
                                     /*secure=*/FALSE, /*tls=*/TRUE);
    } else if (secure) {
        /* HOPE-Secure-Login (cipher negotiated, or no-cipher HMAC auth
         * over plaintext). secure+tls was rejected up front. */
        hx_connect_via_orchestrator (htlc, serverstr, port, login, pass,
                                     /*secure=*/TRUE, /*tls=*/FALSE);
    } else {
        /* Plaintext. */
        hx_connect_via_orchestrator (htlc, serverstr, port, login, pass,
                                     /*secure=*/FALSE, /*tls=*/FALSE);
    }
}

/* Synchronous worker-thread connect helper. Used by the HTXF
 * transfer workers in xfers.c and banner.c — both run on tokio's
 * blocking pool, whose only excuse for existing is the blocking
 * byte-streaming loop, so a sync GSocketClient call here keeps them
 * simple.
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
    /* optional TLS wrap on the HTXF subchannel. Same
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

/* TLS TOFU trampoline for the HTXF subchannel. hxnet's rustls verifier
 * calls this with the peer leaf fingerprint ONLY when WebPKI validation
 * against the native roots failed (a CA-valid cert is trusted silently
 * and never reaches here). Key the known-hosts decision on the
 * subchannel's own host:port (htxf->serverhost / serverport) — the same
 * endpoint the pre-rewire C GTlsConnection accept-cert handler keyed on,
 * so the trust schema is unchanged. user_data is the struct htxf_conn. */
static int
htxf_verify_cert_cb (const guint8 *fp, gsize fp_len, void *user_data)
{
    struct htxf_conn *htxf = user_data;
    if (!htxf || !fp) {
        return 0; /* reject: no context / no fingerprint */
    }
    g_autofree char *fp_str = g_strndup ((const char *) fp, fp_len);
    return tls_trust_decide (htxf->serverhost, htxf->serverport, fp_str) ? 1
                                                                         : 0;
}

/* Public host:port-keyed subchannel cert verify for callers outside
 * network.c (banner.c's HTXF worker). Same decision as the static
 * htxf_verify_cert_cb above; exposed because tls_trust_decide is
 * file-static. */
gboolean
hx_tls_verify_subchannel_cert (const char *host, guint16 port,
                               const char *fingerprint)
{
    return tls_trust_decide (host, port, fingerprint);
}

gboolean
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
    g_return_val_if_fail (htxf != NULL, FALSE);

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

    /* Plaintext TCP connect via GSocketClient — keeps IPv4/IPv6
	 * fallback and SOCKS (GProxyResolver) for free. TLS is NOT done
	 * here anymore: the separate-port TLS wrap moved into hxnet's
	 * rustls path (hxnet_htxf_open below), off the shared C
	 * GTlsConnection accept-cert handler. So this connect is always
	 * plaintext; we extract the connected fd and hand ownership to
	 * hxnet. */
    conn = hx_sync_connect_to_host (htxf->serverhost, htxf->serverport,
                                    errbuf, sizeof (errbuf), /*tls=*/0);
    if (!conn) {
        debug_log ("xfer", "htxf_connect: TCP connect to %s:%u failed: %s",
                   htxf->serverhost, (unsigned) htxf->serverport, errbuf);
        return FALSE;
    }

    /* Take ownership of the connected fd by dup'ing it out of the
	 * GSocketConnection, then unref the conn (its GSocket finaliser
	 * closes the original fd; the dup is an independent reference to
	 * the same connection). hxnet_htxf_open adopts the dup. We've
	 * done no IO on the conn yet, so nothing is buffered. */
    int dupfd = -1;
    {
        GSocket *sock = g_socket_connection_get_socket (conn);
        int sfd = sock ? g_socket_get_fd (sock) : -1;
        if (sfd >= 0) {
            dupfd = dup (sfd);
        }
    }
    g_object_unref (conn);
    if (dupfd < 0) {
        debug_log ("xfer", "htxf_connect: could not dup connected fd");
        return FALSE;
    }

    /* Plaintext preamble (16 bytes legacy, 24 bytes when SIZE64
	 * is set). hx_htxf_subchannel_pack_preamble handles the
	 * LARGE_FILE / SIZE64 flag-setting and the legacy-field
	 * zeroing for the 24-byte variant. hxnet_htxf_open writes it
	 * raw (before any AEAD arms) — the server matches the subchannel
	 * to the queued transfer by ref before any cipher state exists. */
    guint8 hdr_buf[HX_HTXF_PREAMBLE_MAX_BYTES];
    guint16 type
        = htxf->opt.folder ? HTXF_TYPE_FOLDER : HTXF_TYPE_FILE;
    size_t hdr_len = hx_htxf_subchannel_pack_preamble (
        hdr_buf, sizeof (hdr_buf),
        htxf->ref, htxf->total_size,
        type, /*flags=*/0, size64);
    if (hdr_len == 0) {
        close (dupfd);
        return FALSE;
    }

    /* HOPE-ChaCha20-Poly1305 HTXF subchannel arming. When the control
	 * channel negotiated ChaCha20-Poly1305, the per-transfer keys are
	 * derived INSIDE hxnet_htxf_open from the control connection's
	 * retained HOPE material (htlc->hope_aead, an opaque handle seeded at
	 * login) plus this transfer's ref — mixing ref into the salt so two
	 * transfers in one session can never share a nonce, counters from 0 —
	 * and hxnet owns the seal/open framing thereafter. The session key
	 * never comes back to C. The preamble itself always travels plaintext
	 * per spec. A NULL handle (no HOPE, a stream cipher, or no-cipher)
	 * selects plaintext passthrough. The handle is seeded at login from
	 * the orchestrator's retained HOPE material. */
    const HxnetHopeAead *hope_aead =
        (htxf->htlc != NULL) ? (const HxnetHopeAead *) htxf->htlc->hope_aead
                             : NULL;
    if (hope_aead) {
        debug_log ("xfer-aead", "ref=%u: AEAD active (HOPE material present)",
                   htxf->ref);
    }

    /* Mirror the control channel's TLS mode onto this subchannel —
	 * separate-port model pairs TLS-HTXF on port+1 with TLS-HTLS.
	 * The rustls handshake + WebPKI→TOFU trust gate run inside
	 * hxnet_htxf_open; htxf_verify_cert_cb bridges a WebPKI failure
	 * back to the C known-hosts decision. hxnet adopts dupfd and
	 * closes it on any failure. */
    int xfer_tls = (htxf->htlc != NULL) ? htxf->htlc->tls : 0;
    htxf->hx = hxnet_htxf_open (
        dupfd, xfer_tls,
        (const guint8 *) htxf->serverhost, strlen (htxf->serverhost),
        hdr_buf, hdr_len,
        hope_aead, htxf->ref,
        htxf_verify_cert_cb, htxf);
    if (!htxf->hx) {
        debug_log ("xfer", "htxf_connect: hxnet_htxf_open failed (ref=%u)",
                   htxf->ref);
        return FALSE;
    }

    /* Arm the cancellation token (allocated at xfer_new) with the now-
	 * open channel's socket, so a main-thread xfer_delete can shut the
	 * subchannel down and unblock this worker's blocking reads/writes.
	 * Cheap and idempotent; NULL-safe on the banner transient path,
	 * which doesn't route through htxf_connect. */
    htxf_io_abort_arm (htxf);
    return TRUE;
}

/*
 * Tracker fetch — hxnet/tokio orchestrated (R3 item 8, T2).
 * =========================================================
 *
 * The serial URL walk, the connect / TLS / v3-probe fallback ladder,
 * and all HTRK wire parsing now live in the Rust hxnet crate
 * (rust/crates/hxnet/src/tracker_fetch.rs + tracker.rs), reusing the
 * same tokio connect + rustls + TOFU stack as the main session. What's
 * left here is the thin C glue:
 *
 *   - hx_tracker_list_async builds the URL list from gtkhx_prefs and
 *     opens a fetch (hxnet_tracker_fetch_open), then starts a main-loop
 *     timeout that drains fetch events.
 *   - the drain re-emits the EXISTING tracker-batch-begin /
 *     tracker-server-create signals and ticks track_prog_update,
 *     exactly as the old hand-rolled state machine did, so tracker.c
 *     (the view) is unchanged.
 *   - tracker_kill_threads closes the handle (which cancels the walk)
 *     and removes the drain source.
 *
 * The verdict cache, the v3 watchdog, and the ~1100 lines of
 * GSocketClient async callbacks the C used to carry are gone — that
 * logic is the Rust runner's now. The verdict cache is per-fetch on the
 * Rust side (TLS re-probed each Refresh), a minor change from the old
 * process-global cache.
 *
 * Unlike the old reader — which interleaved per-record progress ticks
 * as bytes arrived — the Rust engine returns a whole listing at once,
 * so a tracker's records arrive as a burst; progress ticks per tracker
 * rather than per record within a tracker.
 */

/* ---- hxnet tracker-fetch FFI (mirror of rust/crates/hxnet/src/ffi.rs;
 * hand-synced, like the HXNET_STATE_* / HxnetFrame mirrors). The
 * _Static_asserts below pin the struct ABI against the Rust offset_of
 * asserts so drift on either side is a compile error. ---------------- */

typedef struct HxnetTrackerFetch HxnetTrackerFetch;

#define HXNET_TRK_KIND_BEGIN  0u
#define HXNET_TRK_KIND_RECORD 1u
#define HXNET_TRK_KIND_ERROR  2u
#define HXNET_TRK_KIND_DONE   3u

#define HXNET_TRK_POLL_EMPTY  0
#define HXNET_TRK_POLL_EVENT  1
#define HXNET_TRK_POLL_CLOSED (-1)

/* Host-aware TOFU verify: (tracker host, leaf "sha256:<hex>" fp) ->
 * non-zero to accept. The host is passed because one walk spans many
 * trackers through this single callback. */
typedef int (*hxnet_tracker_verify_cb_t) (const guint8 *host, gsize host_len,
                                          const guint8 *fp, gsize fp_len,
                                          void *user_data);

/* POD view of one fetch event. Pointer fields borrow the handle's
 * current event and are valid only until the next poll/close — the
 * HxTrackerServer constructors copy immediately. Layout mirrors the
 * repr(C) HxnetTrackerEvent in ffi.rs. */
typedef struct {
    guint32 kind;
    guint8 version;
    guint8 addr_type;
    guint16 count;
    guint16 total;
    guint16 port;
    guint16 nusers;
    guint16 tlv_count;
    const guint8 *url_ptr;
    gsize url_len;
    const guint8 *address_ptr;
    gsize address_len;
    const guint8 *name_ptr;
    gsize name_len;
    const guint8 *desc_ptr;
    gsize desc_len;
    const guint8 *tlv_ptr;
    gsize tlv_len;
    const guint8 *message_ptr;
    gsize message_len;
} HxnetTrackerEvent;

_Static_assert (offsetof (HxnetTrackerEvent, kind) == 0, "kind offset");
_Static_assert (offsetof (HxnetTrackerEvent, version) == 4, "version offset");
_Static_assert (offsetof (HxnetTrackerEvent, addr_type) == 5,
                "addr_type offset");
_Static_assert (offsetof (HxnetTrackerEvent, count) == 6, "count offset");
_Static_assert (offsetof (HxnetTrackerEvent, total) == 8, "total offset");
_Static_assert (offsetof (HxnetTrackerEvent, port) == 10, "port offset");
_Static_assert (offsetof (HxnetTrackerEvent, nusers) == 12, "nusers offset");
_Static_assert (offsetof (HxnetTrackerEvent, tlv_count) == 14,
                "tlv_count offset");
_Static_assert (offsetof (HxnetTrackerEvent, url_ptr) == 16, "url_ptr offset");

extern HxnetTrackerFetch *
hxnet_tracker_fetch_open (const char *const *urls, gsize n, guint16 features,
                          guint32 probe_ms,
                          hxnet_tracker_verify_cb_t verify_cert,
                          void *user_data);
extern int hxnet_tracker_fetch_poll (HxnetTrackerFetch *handle,
                                     HxnetTrackerEvent *out);
extern void hxnet_tracker_fetch_close (HxnetTrackerFetch *handle);

/* ---- bridge state (main thread only) ---------------------------- */

static HxnetTrackerFetch *current_tracker_fetch;
static guint tracker_drain_source_id;
/* Wire version of the batch in progress, set on BEGIN; picks the v1 vs
 * v3 HxTrackerServer constructor for the records that follow. */
static guint8 tracker_batch_version;
/* 1-based progress counter within the current batch. */
static int tracker_batch_server_i;

/* Read the v3-probe watchdog timeout once per fetch. strtol with sane
 * clamps — values <100ms are pointless and >60000ms hangs the UI for a
 * minute; anything outside the range or unparseable uses the default.
 * Overridable via GTKHX_TRACKER_V3_PROBE_MS for slow test rigs. */
#define HX_TRACKER_V3_PROBE_TIMEOUT_MS 2000

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
                   "(must be an integer in [100, 60000]); using default %u ms",
                   env, (unsigned) HX_TRACKER_V3_PROBE_TIMEOUT_MS);
        return HX_TRACKER_V3_PROBE_TIMEOUT_MS;
    }
    return (guint) v;
}

/* TOFU verify keyed on (tracker host, HTRK_TCPPORT). Runs on the hxnet
 * worker thread; tls_trust_decide marshals any user prompt to the main
 * thread, exactly as htxf_verify_cert_cb does. A WebPKI-valid cert is
 * trusted in Rust and never reaches here. */
static int
tracker_verify_cert_cb (const guint8 *host, gsize host_len, const guint8 *fp,
                        gsize fp_len, void *user_data G_GNUC_UNUSED)
{
    if (!host || !fp) {
        return 0; /* reject: no context / no fingerprint */
    }
    g_autofree char *host_str = g_strndup ((const char *) host, host_len);
    g_autofree char *fp_str = g_strndup ((const char *) fp, fp_len);
    return tls_trust_decide (host_str, HTRK_TCPPORT, fp_str) ? 1 : 0;
}

/* Re-emit one drained fetch event as the legacy view signals. */
static void
tracker_fetch_dispatch_event (session *sess, const HxnetTrackerEvent *ev)
{
    switch (ev->kind) {
    case HXNET_TRK_KIND_BEGIN: {
        g_autofree char *url
            = g_strndup ((const char *) ev->url_ptr, ev->url_len);
        tracker_batch_version = ev->version;
        tracker_batch_server_i = 1;
        gtkhx_session_emit_tracker_batch_begin (gtkhx_session_get_default (),
                                                url, ev->version, ev->count);
        track_prog_update (sess, url, 0, (int) ev->count);
        break;
    }
    case HXNET_TRK_KIND_RECORD: {
        g_autofree char *url
            = g_strndup ((const char *) ev->url_ptr, ev->url_len);
        HxTrackerServer *e = NULL;
        if (tracker_batch_version == 1) {
            /* v1 record: MacRoman name/desc (the v1 constructor
             * transcodes), IPv4 address in network byte order. */
            struct in_addr addr;
            memset (&addr, 0, sizeof addr);
            if (ev->address_len >= 4) {
                memcpy (&addr.s_addr, ev->address_ptr, 4);
            }
            e = hx_tracker_server_new_v1 (
                addr, ev->port, ev->nusers, (const char *) ev->name_ptr,
                ev->name_len, (const char *) ev->desc_ptr, ev->desc_len,
                (int) ev->total);
        } else {
            /* v3 record: UTF-8 name/desc, addr_type-tagged address. */
            e = hx_tracker_server_new_v3 (
                ev->addr_type, ev->address_ptr, ev->address_len, ev->port,
                ev->nusers, (const char *) ev->name_ptr, ev->name_len,
                (const char *) ev->desc_ptr, ev->desc_len, ev->tlv_count,
                ev->tlv_len ? ev->tlv_ptr : NULL, ev->tlv_len,
                (int) ev->total);
        }
        if (e) {
            gtkhx_session_emit_tracker_server_create (
                gtkhx_session_get_default (), e);
            hx_tracker_server_free (e);
            track_prog_update (sess, url, tracker_batch_server_i,
                               (int) ev->total);
            tracker_batch_server_i++;
        }
        break;
    }
    case HXNET_TRK_KIND_ERROR: {
        g_autofree char *url
            = g_strndup ((const char *) ev->url_ptr, ev->url_len);
        g_autofree char *msg
            = g_strndup ((const char *) ev->message_ptr, ev->message_len);
        hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                          _ ("tracker: %1$s: %2$s\n"), url, msg);
        break;
    }
    case HXNET_TRK_KIND_DONE:
    default:
        break;
    }
}

static void
tracker_fetch_cleanup (void)
{
    if (current_tracker_fetch) {
        hxnet_tracker_fetch_close (current_tracker_fetch);
        current_tracker_fetch = NULL;
    }
}

/* Main-loop drain: pull every ready event this tick, then either keep
 * the timer (more may come) or tear down on a closed channel. */
static gboolean
tracker_fetch_drain (gpointer user_data)
{
    session *sess = user_data;
    HxnetTrackerEvent ev;

    for (;;) {
        int rc = hxnet_tracker_fetch_poll (current_tracker_fetch, &ev);
        if (rc == HXNET_TRK_POLL_EVENT) {
            tracker_fetch_dispatch_event (sess, &ev);
            continue;
        }
        if (rc == HXNET_TRK_POLL_EMPTY) {
            return G_SOURCE_CONTINUE;
        }
        /* HXNET_TRK_POLL_CLOSED — the walk finished and its events are
         * drained. The G_SOURCE_REMOVE below drops this source, so just
         * clear our id and free the handle. */
        tracker_drain_source_id = 0;
        tracker_fetch_cleanup ();
        return G_SOURCE_REMOVE;
    }
}

void
hx_tracker_list_async (session *sess)
{
    /* Cancel any in-flight fetch before starting a new one. */
    tracker_kill_threads ();

    if (gtkhx_prefs.num_tracker <= 0) {
        return;
    }

    int n = gtkhx_prefs.num_tracker;
    const char **urls = g_new (const char *, (gsize) n);
    for (int i = 0; i < n; i++) {
        urls[i] = gtkhx_prefs.tracker[i];
    }
    current_tracker_fetch = hxnet_tracker_fetch_open (
        (const char *const *) urls, (gsize) n, HTRK_V3_FEAT_IPV6,
        hx_tracker_v3_probe_ms (), tracker_verify_cert_cb, NULL);
    g_free (urls);
    if (!current_tracker_fetch) {
        return;
    }

    tracker_batch_version = 0;
    tracker_batch_server_i = 1;
    /* Drain on the main loop. 50 ms keeps the list lively without
     * busy-spinning; each tick re-emits whatever the walk produced. */
    tracker_drain_source_id = g_timeout_add (50, tracker_fetch_drain, sess);
}

void
tracker_kill_threads (void)
{
    if (tracker_drain_source_id) {
        g_source_remove (tracker_drain_source_id);
        tracker_drain_source_id = 0;
    }
    tracker_fetch_cleanup ();
}

void
kill_threads (void)
{
    /* cancel the async connect chain. Safe whether or not
	 * one's in flight. */
    if (current_cancel) {
        g_cancellable_cancel (current_cancel);
        g_clear_object (&current_cancel);
    }
    /* And the async tracker fetch, which has its own cancellation
	 * inside current_tracker_fetch. */
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

    /* the buffer-packing logic lives in hlpack
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

    /* R3.3.e-4c: when the bridge is installed and neither
     * cipher nor compression is active, ship the packed
     * plaintext through hxnet's send queue and pop the bytes
     * out of htlc->out so the legacy write-source path doesn't
     * also try to send them. The hxnet path doesn't support
     * cipher / compression yet (HOPE-negotiated stacks live
     * inside the C cipher state today); when those are set we
     * fall through to the legacy in-place encode path. */
    /* R3.3.e-4d: when the bridge is installed, hxnet's transform
     * stack handles the negotiated cipher / compression, so the
     * C side ships PLAINTEXT through hx_bridge_send_frame and
     * skips the legacy compress_encode + cipher_encode +
     * GIOStream-write-queue path. The install path in
     * hx_install_hxnet_post_hope clears htlc->cipher_*_type /
     * compress_*_type to NONE so the gate below only needs to
     * check `hx_bridge_is_installed` — but the asserts inside
     * the branch keep us honest if a future refactor lets a
     * non-NONE state leak through. */
    if (hx_bridge_is_installed ()) {
        /* hx_install_hxnet_post_hope clears cipher_*_type and
         * compress_*_type to NONE before the bridge starts
         * carrying traffic. A non-NONE state here means a
         * future refactor put a cipher / compression activation
         * after the install — that would silently
         * double-encode on the wire. Log the invariant
         * violation and close rather than ship garbage. (Not
         * `g_assert`, which compiles out under
         * G_DISABLE_ASSERT.) */
        if (htlc->cipher_encode_type != CIPHER_NONE
            || htlc->compress_encode_type != COMPRESS_NONE) {
            g_critical (
                "hxnet bridge active but cipher_encode_type=%d "
                "compress_encode_type=%d — would double-encode; "
                "closing.",
                htlc->cipher_encode_type, htlc->compress_encode_type);
            hx_htlc_close (htlc, /*expected=*/0);
            return;
        }
        int rc = hx_bridge_send_frame (&htlc->out.buf[this_off], len);
        if (rc == 0) {
            htlc->out.len -= len;
            if (!htlc->out.len) {
                /* Queue drained — reset pos to 0 so the qbuf reuses its
                 * buffer from the start (avoids accumulating unused
                 * prefix space over a long hxnet session). */
                htlc->out.pos = 0;
            }
        } else {
            /* hxnet refused the send. The return codes are:
             *
             *   HXNET_SEND_FULL          (-1) — actor's command
             *       channel is full. With DEFAULT_COMMAND_CAPACITY
             *       at 256 a burst that fills it means the actor's
             *       send loop is wedged (tokio task hung, inner
             *       socket blocked indefinitely). Recovery isn't
             *       a transient retry — the connection is
             *       effectively dead. Close.
             *   HXNET_SEND_CLOSED        (-2) — channel sender
             *       dropped. Actor already exited.
             *   HXNET_SEND_INVALID       (-3) — slice mis-shaped.
             *       Bug. (Matches `HXNET_SEND_INVALID` in
             *       rust/crates/hxnet/src/ffi.rs.)
             *   HX_BRIDGE_SEND_NOT_INSTALLED (-100) — bridge
             *       uninstalled mid-call. Race; close.
             *
             * In every case the safest action is to tear the
             * connection down rather than drop the packed bytes
             * silently and let higher-level protocol state
             * desync. hx_bridge_send_frame already logged the
             * specific FFI code via g_critical. If FULL starts
             * appearing in practice the right fix is a real
             * retry/drain idle (captured as roadmap follow-up),
             * not a higher channel cap. */
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "hxnet send failed (rc=%d); closing.\n",
                              rc);
            hx_htlc_close (htlc, /*expected=*/0);
        }
        return;
    }

    /* No bridge installed → no connection. The orchestrator installs
     * the bridge synchronously at connect time and hlwrite only runs on
     * a live session, so reaching here means a stray send racing
     * teardown. Drop the just-packed bytes (same out bookkeeping as the
     * bridge success path) rather than queue them on a socket that no
     * longer exists. The legacy GIOStream write-source + in-place
     * cipher/compress encode path is gone — hxnet's transform stack
     * owns encoding now. */
    debug_log ("net", "hlwrite: no bridge installed; dropping %u packed bytes",
               len);
    htlc->out.len -= len;
    if (!htlc->out.len) {
        htlc->out.pos = 0;
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

    /* hxnet routing — same shape as hlwrite above; see that
     * comment block for the gate's full rationale. */
    if (hx_bridge_is_installed ()) {
        /* hx_install_hxnet_post_hope clears cipher_*_type and
         * compress_*_type to NONE before the bridge starts
         * carrying traffic. A non-NONE state here means a
         * future refactor put a cipher / compression activation
         * after the install — that would silently
         * double-encode on the wire. Log the invariant
         * violation and close rather than ship garbage. (Not
         * `g_assert`, which compiles out under
         * G_DISABLE_ASSERT.) */
        if (htlc->cipher_encode_type != CIPHER_NONE
            || htlc->compress_encode_type != COMPRESS_NONE) {
            g_critical (
                "hxnet bridge active but cipher_encode_type=%d "
                "compress_encode_type=%d — would double-encode; "
                "closing.",
                htlc->cipher_encode_type, htlc->compress_encode_type);
            hx_htlc_close (htlc, /*expected=*/0);
            return;
        }
        int rc = hx_bridge_send_frame (&htlc->out.buf[this_off], len);
        if (rc == 0) {
            htlc->out.len -= len;
            if (!htlc->out.len) {
                /* Queue drained — reset pos to 0 so the qbuf reuses its
                 * buffer from the start (avoids accumulating unused
                 * prefix space over a long hxnet session). */
                htlc->out.pos = 0;
            }
        } else {
            /* Same rc-handling rationale as hlwrite above —
             * see the comment block there. With
             * DEFAULT_COMMAND_CAPACITY = 256, FULL means the
             * actor is wedged; close is the right action. */
            hx_printf_prefix (htlc, 0, INFOPREFIX,
                              "hxnet send failed (rc=%d); closing.\n",
                              rc);
            hx_htlc_close (htlc, /*expected=*/0);
        }
        return;
    }

    /* No bridge installed → no connection; drop the just-packed bytes.
     * See the matching comment in hlwrite — the legacy GIOStream
     * write-source + in-place encode path is gone. */
    debug_log ("net",
               "hlwrite_chunks: no bridge installed; dropping %u packed bytes",
               len);
    htlc->out.len -= len;
    if (!htlc->out.len) {
        htlc->out.pos = 0;
    }
}

/* hl_code lives in src/hl_code.c so the Tier 1 unit test can link
 * it without dragging in the rest of network.c's deps. The declaration
 * stays in network.h (extern) for back-compat with existing callers
 * that didn't include hl_code.h directly. */
