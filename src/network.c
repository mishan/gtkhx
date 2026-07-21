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
#include <stddef.h> /* offsetof — used by the HxnetTrackerEvent ABI asserts */
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
#include "host_port.h"
#include "rcv.h"
#include "gtkutil.h"
#include "chat.h"
#include "tasks.h"
#include "users.h"
#include "inet.h"
#include "log.h"
#include "proto_trace.h"
#include "tls_trust.h"
#include "inline_media.h"
#include "gif_icons.h"
#include "toolbar.h"
#include "tracker.h"
#include "network.h"
#include "hxconn.h"
#include "banner.h"
#include "debug.h"
#include "htxf_io.h"           /* HxnetHopeAead, hxnet_htxf_connect, hxnet_hope_aead_free */
#include "cipher.h"
#ifdef HAVE_VOICE
#include "voice_runtime.h"
#include "voice_model.h"
#endif

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

    session *sess = sess_from_htlc (htlc);

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
    hx_conn_set_caps (htlc, 0);
    /* Chat-history retention hints from the LOGIN reply — wiped
	 * on disconnect so a reconnect to a server with different
	 * retention doesn't carry stale numbers into the UI. */
    hx_conn_set_history_max_msgs (htlc, 0);
    hx_conn_set_history_max_days (htlc, 0);
    /* Inline-media advisory limits from the LOGIN reply — same
	 * reasoning. The accessors in src/inline_media.h gate on
	 * CAP_INLINE_MEDIA being lit so callers see spec defaults
	 * when the new server doesn't echo the cap; this reset is
	 * defence-in-depth for any future path that reads the raw
	 * fields directly (and matches the pattern history_max_*
	 * uses one line up). */
    inline_media_reset_advisory_limits (htlc);

    /* GIF-icons probe state — drop the watchdog timer (if still armed)
	 * and reset to UNKNOWN so a reconnect re-probes cleanly. Inlined
	 * (rather than calling into gif_icons.c) to avoid pulling the
	 * task_new / rcv_task_icon_* dependency chain into every test
	 * harness that links network.c. */
    if (htlc->gif_icons_probe_timer) {
        g_source_remove (htlc->gif_icons_probe_timer);
        htlc->gif_icons_probe_timer = 0;
    }
    htlc->gif_icons_state = GIF_ICONS_UNKNOWN;

#ifdef HAVE_VOICE
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
#endif /* HAVE_VOICE */

    /* Cancel any in-flight async connect (DNS / TCP-connect / magic
	 * exchange). Safe to call whether or not one's running. */
    if (current_cancel) {
        g_cancellable_cancel (current_cancel);
        g_clear_object (&current_cancel);
    }
    g_strlcpy (buf,
               hx_conn_ip_addr (htlc)[0] ? hx_conn_ip_addr (htlc) : "?",
               sizeof (buf));
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

    hx_conn_set_ip_addr (htlc, "");

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
    hx_conn_set_version (htlc, 0);
    memset (htlc->login, 0, sizeof (htlc->login));

    /* chats live in a GHashTable<u32 cid, struct chat*>.
	 * For each chat:
	 *   1. Clear the UI's user-list rendering (users_clear is a no-op
	 *      on cid != 0 since the global user-list widget only shows
	 *      the public chat's members).
	 *   2. The hashtable's value-destroy notify (chat_free in chat.c)
	 *      reclaims the chat's member model + the struct chat itself
	 *      on remove; we do not need to walk + free members by hand.
	 * The public chat (cid=0) must stay alive across reconnects, so
	 * we remove all *non-public* chats; the public chat's membership
	 * and subject were both reset by its users-clear emit above. */
    if (sess->chats) {
        GArray *non_public = g_array_new (FALSE, FALSE, sizeof (guint32));
        /* Snapshot pass: emit users-clear for every chat, collect the
         * non-public cids, remove them after. The cid comes from the
         * registry (hx_chats_cid_at) rather than the gtkhx-ui conversation
         * accessors — this file must stay free of UI-crate symbols so the
         * headless wire-level tests link. Collect-then-remove so we don't
         * mutate the registry mid-walk. */
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *chat = hx_chats_get_at (sess->chats, i);
            guint32 cid = hx_chats_cid_at (sess->chats, i);
            /* The users-clear emit drops this chat's membership and
			 * (view-side) resets its subject — the public chat persists
			 * across reconnect, so its subject must not carry over. */
            gtkhx_session_emit_users_clear (gtkhx_session_get_default (), htlc,
                                            chat);
            if (cid != 0) {
                g_array_append_val (non_public, cid);
            }
        }
        for (guint i = 0; i < non_public->len; i++) {
            hx_chats_remove (sess->chats,
                             g_array_index (non_public, guint32, i));
        }
        g_array_free (non_public, TRUE);
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

    /* No per-direction cipher / compression state to tear down: the
     * orchestrator (hxnet + the Rust hxcrypto-* / hxcompress crates) owns all
     * control-channel crypto and compression now. The legacy C session key,
     * cipher/compress union state, and gzip counters were removed from
     * struct htlc_conn — nothing populated them. */
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
    gboolean utf8 = (hx_conn_has_cap (htlc, HTLC_CAP_TEXT_ENCODING)) != 0;
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
    return hx_tls_verify_cert (hx_conn_serverhost (htlc),
                               hx_conn_serverport (htlc), fingerprint);
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
    if (task_with_trans (sess_from_htlc (htlc), orchestrator_login_reply_trans)) {
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

    hx_conn_set_chat_history_last_msgid (htlc, 0);
    hx_conn_set_serverhost (htlc, serverstr);
    hx_conn_set_serverport (htlc, port);
    /* Stamp the TLS flag so the HTXF subchannel workers (xfers.c /
     * banner.c) wrap their data ports too. The control channel's TLS
     * runs inside hxnet (rustls); HTXF keeps using the legacy
     * GTlsConnection path, which reads htlc->tls. */
    hx_conn_set_tls (htlc, tls ? 1 : 0);
    g_strlcpy (htlc->login, login ? login : "", sizeof (htlc->login));

    /* Seed htlc->ip_addr from the server string so the post-login
	 * "<addr>: login successful" line in rcv_task_login isn't "?".
	 * The legacy path fills this with the resolved numeric IP via
	 * populate_htlc_remote_ip; the orchestrator owns the socket and
	 * doesn't surface the peer addr yet, so the connect target is
	 * the best display string we have. TODO: plumb the resolved
	 * SocketAddr out of the hxnet lifecycle to match the legacy
	 * numeric-IP display exactly. */
    hx_conn_set_ip_addr (htlc, serverstr);

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
                 | HTLC_CAP_CHAT_HISTORY
                 | HTLC_CAP_INLINE_MEDIA;
#ifdef HAVE_VOICE
    /* Only advertise voice when the runtime is actually compiled in —
     * otherwise a server would offer voice we can't honour. */
    caps |= HTLC_CAP_VOICE;
#endif
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
        gboolean want_tls = tls || hx_tls_test_force_tls ();
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
    gboolean want_tls = tls || hx_tls_test_force_tls ();
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
    return hx_tls_verify_cert (htxf->serverhost, htxf->serverport, fp_str) ? 1
                                                                         : 0;
}

/* Public host:port-keyed subchannel cert verify for callers outside
 * network.c (banner.c's HTXF worker). Same decision as the static
 * htxf_verify_cert_cb above; a thin wrapper over hx_tls_verify_cert
 * for callers outside network.c. */
gboolean
hx_tls_verify_subchannel_cert (const char *host, guint16 port,
                               const char *fingerprint)
{
    return hx_tls_verify_cert (host, port, fingerprint);
}

gboolean
htxf_connect (struct htxf_conn *htxf)
{
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
                      && (hx_conn_has_cap (htxf->htlc, HTLC_CAP_LARGE_FILES)) != 0
                      && htxf->total_size > 0xFFFFFFFFULL;
    htxf->opt.large = size64 ? 1 : 0;

    /* Plaintext preamble (16 bytes legacy, 24 bytes when SIZE64
	 * is set). hx_htxf_subchannel_pack_preamble handles the
	 * LARGE_FILE / SIZE64 flag-setting and the legacy-field
	 * zeroing for the 24-byte variant. hxnet_htxf_connect writes it
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
        return FALSE;
    }

    /* Resolve the SOCKS proxy (if any) for this subchannel target the
	 * same way the control channel does, then let hxnet own the whole
	 * connect: DNS + IPv4/IPv6 fallback + optional SOCKS tunnel all run
	 * in Rust (resolve_and_connect), so there's no C-side GSocketClient
	 * connect + fd dup/adopt anymore. */
    g_autofree char *proxy_uri =
        hx_bridge_lookup_socks_proxy (htxf->serverhost, htxf->serverport);

    /* HOPE-ChaCha20-Poly1305 HTXF subchannel arming. When the control
	 * channel negotiated ChaCha20-Poly1305, the per-transfer keys are
	 * derived INSIDE hxnet_htxf_connect from the control connection's
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
	 * hxnet_htxf_connect does the TCP connect (+ optional SOCKS tunnel),
	 * the rustls handshake, and the WebPKI→TOFU trust gate internally;
	 * htxf_verify_cert_cb bridges a WebPKI failure back to the C
	 * known-hosts decision. */
    int xfer_tls = (htxf->htlc != NULL) ? hx_conn_tls (htxf->htlc) : 0;
    htxf->hx = hxnet_htxf_connect (
        (const guint8 *) htxf->serverhost, strlen (htxf->serverhost),
        htxf->serverport,
        (const guint8 *) proxy_uri, proxy_uri ? strlen (proxy_uri) : 0,
        xfer_tls,
        hdr_buf, hdr_len,
        hope_aead, htxf->ref,
        htxf_verify_cert_cb, htxf);
    if (!htxf->hx) {
        debug_log ("xfer", "htxf_connect: hxnet_htxf_connect failed (ref=%u)",
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
 * logic is the Rust runner's now. The TLS verdict cache moved to the
 * Rust side but stays process-global (snapshotted per walk), so a
 * Refresh still skips a known-failing TLS handshake — same behaviour as
 * the old C cache.
 *
 * Unlike the old reader — which interleaved per-record progress ticks
 * as bytes arrived — the Rust engine returns a whole listing at once,
 * so a tracker's records arrive as a burst; progress ticks per tracker
 * rather than per record within a tracker.
 */

/* hxnet tracker-fetch FFI decls + the HxnetTrackerEvent ABI mirror
 * (with _Static_asserts) live in a shared header so this bridge and the
 * Tier 3 test (tests/integration/test_tracker_fetch.c) agree on one
 * definition. */
#include "tracker_fetch_ffi.h"

/* ---- bridge state (main thread only) ---------------------------- */

static HxnetTrackerFetch *current_tracker_fetch;
static guint tracker_drain_source_id;
/* Wire version of the batch in progress, set on BEGIN; picks the v1 vs
 * v3 HxTrackerServer constructor for the records that follow. */
static guint8 tracker_batch_version;
/* 1-based progress counter within the current batch. */
static int tracker_batch_server_i;
/* URL of the batch in progress, stashed on BEGIN. RECORD events no
 * longer carry a url (the Rust side drops it to avoid a per-record
 * allocation), so progress updates key on this. Freed on cleanup. */
static char *tracker_batch_url;

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

/* TOFU verify keyed on the tracker's (host, port). Runs on the hxnet
 * worker thread; hx_tls_verify_cert marshals any user prompt to the main
 * thread, exactly as htxf_verify_cert_cb does. A WebPKI-valid cert is
 * trusted in Rust and never reaches here. */
static int
tracker_verify_cert_cb (const guint8 *host, gsize host_len, guint16 port,
                        const guint8 *fp, gsize fp_len,
                        void *user_data G_GNUC_UNUSED)
{
    if (!host || host_len == 0 || !fp || fp_len == 0) {
        /* Reject: no host / no fingerprint. An empty host would key the
         * trust cache on "" and produce a confusing prompt. */
        return 0;
    }
    g_autofree char *host_str = g_strndup ((const char *) host, host_len);
    g_autofree char *fp_str = g_strndup ((const char *) fp, fp_len);
    return hx_tls_verify_cert (host_str, port, fp_str) ? 1 : 0;
}

/* Owned copy of a (ptr, len) wire string. The hxnet FFI exports an empty
 * string as (NULL, 0) — g_strndup chokes on a NULL pointer — so map that
 * to an owned "" rather than a GLib critical / NULL downstream. */
static char *
tracker_dup_str (const guint8 *ptr, gsize len)
{
    if (!ptr || len == 0) {
        return g_strdup ("");
    }
    return g_strndup ((const char *) ptr, len);
}

/* Re-emit one drained fetch event as the legacy view signals. */
static void
tracker_fetch_dispatch_event (session *sess, const HxnetTrackerEvent *ev)
{
    switch (ev->kind) {
    case HXNET_TRK_KIND_BEGIN: {
        /* Take ownership of the batch URL for the records that follow. */
        g_free (tracker_batch_url);
        tracker_batch_url = tracker_dup_str (ev->url_ptr, ev->url_len);
        tracker_batch_version = ev->version;
        tracker_batch_server_i = 1;
        gtkhx_session_emit_tracker_batch_begin (gtkhx_session_get_default (),
                                                tracker_batch_url, ev->version,
                                                ev->count);
        track_prog_update (sess, tracker_batch_url, 0, (int) ev->count);
        break;
    }
    case HXNET_TRK_KIND_RECORD: {
        /* Records carry no url; progress keys on the stashed batch url. */
        const char *url = tracker_batch_url ? tracker_batch_url : "";
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
            track_prog_update (sess, (char *) url, tracker_batch_server_i,
                               (int) ev->total);
            tracker_batch_server_i++;
        }
        break;
    }
    case HXNET_TRK_KIND_ERROR: {
        g_autofree char *url = tracker_dup_str (ev->url_ptr, ev->url_len);
        g_autofree char *msg
            = tracker_dup_str (ev->message_ptr, ev->message_len);
        hx_printf_prefix (sess->htlc, 0, INFOPREFIX,
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
    g_clear_pointer (&tracker_batch_url, g_free);
}

/* Main-loop drain: pull every ready event this tick, then either keep
 * the timer (more may come) or tear down on a closed channel. */
static gboolean
tracker_fetch_drain (gpointer user_data)
{
    session *sess = user_data;
    HxnetTrackerEvent ev;

    for (;;) {
        /* tracker_fetch_dispatch_event emits view signals, and a
         * subscriber can re-enter (e.g. trigger a Refresh / disconnect)
         * and run tracker_kill_threads, which removes this source and
         * closes the handle mid-drain. Re-check at the top of every
         * iteration so we never poll a NULL handle (the `continue` after
         * a dispatched event comes back through here). */
        if (!current_tracker_fetch) {
            tracker_drain_source_id = 0;
            return G_SOURCE_REMOVE;
        }
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

    /* Single SOCKS proxy for the whole walk (the user's choice over a
     * per-URL array): resolve it once for the first tracker's endpoint,
     * which the common uniform-proxy config (all_proxy / GNOME settings)
     * applies to every tracker anyway. gtkhx_parse_host_port handles
     * "host" / "host:port" / "[ipv6]:port" correctly; on a malformed URL
     * we skip the lookup (connect direct — the bad URL fails later in the
     * walk regardless). */
    g_autofree char *proxy_uri = NULL;
    {
        g_autofree char *phost = NULL;
        guint16 pport = 0;
        if (gtkhx_parse_host_port (urls[0], HTRK_TCPPORT, &phost, &pport,
                                   NULL)) {
            proxy_uri = hx_bridge_lookup_socks_proxy (phost, pport);
        }
    }

    current_tracker_fetch = hxnet_tracker_fetch_open (
        (const char *const *) urls, (gsize) n, HTRK_V3_FEAT_IPV6,
        hx_tracker_v3_probe_ms (), proxy_uri, tracker_verify_cert_cb, NULL);
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

    /* When the bridge is installed and neither
     * cipher nor compression is active, ship the packed
     * plaintext through hxnet's send queue and pop the bytes
     * out of htlc->out so the legacy write-source path doesn't
     * also try to send them. The hxnet path doesn't support
     * cipher / compression yet (HOPE-negotiated stacks live
     * inside the C cipher state today); when those are set we
     * fall through to the legacy in-place encode path. */
    /* When the bridge is installed, hxnet's transform stack handles the
     * negotiated cipher / compression, so the C side ships PLAINTEXT through
     * hx_bridge_send_frame. There is no longer any legacy C
     * compress_encode / cipher_encode path to skip — that state was removed
     * from struct htlc_conn once hxnet took over control-channel crypto. */
    if (hx_bridge_is_installed ()) {
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
