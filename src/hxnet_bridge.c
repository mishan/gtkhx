/*
 * hxnet_bridge.c — translation layer between hxnet's callback FFI
 * and gtkhx's existing rcv state machine. See hxnet_bridge.h for
 * the module-level rationale.
 *
 * The functions here run on the GLib main thread (the hxnet
 * forwarder spawns_local onto the thread-default MainContext,
 * which in production is the main loop). No threading
 * primitives needed — the htlc_conn struct is touched
 * single-threaded throughout the lifetime of one connection.
 */

#include "config.h"

#include <string.h>
#include <unistd.h> /* close() — fd cleanup on pre-spawn failure */

#include <glib.h>
#include <gio/gio.h> /* GProxyResolver (SOCKS proxy lookup) */

#include "compat.h" /* MAX_HOTLINE_PACKET_LEN */
#include "hxnet_bridge.h"
#include "protocol.h"
#include "hxconn.h"
#include "proto_helpers.h"
#include "hotline_proto.h" /* gtkhx_proto_pack_header (wire header encode) */
#include "gtkhx_session.h" /* GtkhxConnectionState + emit (Phase G state cb) */
#include "network.h" /* hx_orchestrator_register_login_task (LOGIN_SENDING) */
#include "hxnet_htxf.h" /* HxnetHopeAead (orchestrated HOPE AEAD material) */
#include "host_port.h"  /* gtkhx_join_host_port (proxy lookup URI) */

/* The production receive dispatch (rcv.c). We hand it the assembled frame
 * as an explicit (frame, frame_len) slice plus the parsed header fields; it
 * routes the opcode to a body handler and calls it. */
extern void hx_dispatch_frame (struct htlc_conn *htlc, const guint8 *frame,
                               gsize frame_len, guint32 type, guint32 trans,
                               guint32 flag, guint32 body_len);

/* Forward declaration of the production teardown. Defined in
 * network.c; we trampoline to it from the shutdown bridge.
 * Marked `expected` semantically by the reason translation
 * inside hx_bridge_dispatch_shutdown. */
extern void hx_htlc_close (struct htlc_conn *htlc, int expected);

/* network.c's "session is fully logged in" flag. Used here only to
 * pick the shutdown log level: a failure before the session came up
 * (connect refused / handshake / login reject) is routine and gets a
 * user-facing error dialog, so it logs at g_message; a mid-session
 * drop after login is the noteworthy case. */
extern int connected;

/* Mirror of the hxnet FFI's shutdown-reason constants
 * (rust/crates/hxnet/src/ffi.rs::HXNET_SHUTDOWN_*). The C side
 * has its own copies in tests/unit/test_hxnet_ffi.c; redeclaring
 * them here as bridge-internal constants avoids that header
 * having to ship hxnet_frame's full ABI just for the reason
 * codes. Production's on_shutdown callback will pass these in
 * verbatim. */
#define HXNET_SHUTDOWN_EOF 0
#define HXNET_SHUTDOWN_STREAM_ERROR 1
#define HXNET_SHUTDOWN_FRAME_TOO_LARGE 2
#define HXNET_SHUTDOWN_HANDLE_DROPPED 3

void
hx_bridge_dispatch_frame (struct htlc_conn *htlc, guint32 type, guint32 trans,
                          guint32 flag, guint16 hc, const guint8 *body,
                          guint32 body_len)
{
    g_return_if_fail (htlc != NULL);
    g_return_if_fail (body_len == 0 || body != NULL);

    /* Drop in-flight events that the GLib idle queue dispatched
     * after `hx_htlc_close` already ran. There's an inherent
     * race: hxnet's tokio actor forwards `Event::Frame`s through
     * `MainForwarder::forward_to_main` onto the GLib idle queue,
     * but a synchronous failure on the C-side send path (e.g.
     * `hx_bridge_send_frame` returning HXNET_SEND_CLOSED after
     * the actor exited) calls `hx_htlc_close` immediately —
     * which clears `htlc->fd` and uninstalls the bridge. Any
     * already-queued idle source then fires AFTER close and
     * calls us here; dispatching a frame for a torn-down
     * connection is at best wasted work and at worst reaches
     * freed session state. Skip the dispatch when either signal
     * of close is set;
     * the in-flight frame is information the C side no longer
     * cares about. */
    if (hx_conn_fd (htlc) == 0 || !hx_bridge_is_installed (htlc)) {
        return;
    }

    /* The body handlers decode the header via hl_hdr_decode(frame)
     * and read the body from frame[SIZEOF_HL_HDR..]. hxnet's actor
     * enforces its own MAX_BODY_LEN (1 MiB = MAX_HOTLINE_PACKET_LEN)
     * so any real server traffic that reaches us is in range — but
     * the bridge shouldn't depend on that coincidence. Refuse
     * oversize frames and tear the connection down loudly rather
     * than silently truncating or overflowing. The clamp mirrors
     * hl_hdr_decode, which caps the wire `len` (= body_len +
     * sizeof(hc)) to MAX_HOTLINE_PACKET_LEN. */
    if (body_len > MAX_HOTLINE_PACKET_LEN - 2) {
        g_critical (
            "hxnet_bridge: dispatch_frame body_len %u exceeds "
            "MAX_HOTLINE_PACKET_LEN - sizeof(hc) %u; closing connection",
            (unsigned)body_len, (unsigned)(MAX_HOTLINE_PACKET_LEN - 2));
        hx_htlc_close (htlc, /*expected=*/0);
        return;
    }

    /* Assemble the whole frame (22-byte header + body) into a
     * transient buffer and hand it to hx_dispatch_frame as an
     * explicit (frame, frame_len) slice. hx_dispatch_frame is
     * synchronous — it walks the chunks and fires any task
     * callback before returning, and callbacks that need the bytes
     * past their own return (chunked upload/download) copy them out
     * first — so the buffer only has to outlive this call. The Rust
     * actor already parsed the header, so there's no C-side re-decode
     * and no two-phase receive state machine: dispatch routes the
     * parsed opcode straight to the body handler. */
    gsize frame_len = SIZEOF_HL_HDR + body_len;
    guint8 *frame = g_malloc (frame_len);
    gtkhx_proto_pack_header (frame, type, trans, flag, hc, body_len);
    if (body_len > 0) {
        memcpy (&frame[SIZEOF_HL_HDR], body, body_len);
    }

    hx_dispatch_frame (htlc, frame, frame_len, type, trans, flag, body_len);

    g_free (frame);
}

void
hx_bridge_dispatch_shutdown (struct htlc_conn *htlc, int reason)
{
    g_return_if_fail (htlc != NULL);

    /* Always log the reason BEFORE the close-race early return
     * below. The race is: hxnet's actor errored out and posted
     * Event::Shutdown to the GLib idle queue, but a synchronous
     * `hx_bridge_send_frame` failure path on the C side already
     * fired `hx_htlc_close` first — by the time the queued
     * Event::Shutdown gets dispatched here, the bridge is
     * uninstalled and the early return below kicks in to avoid
     * a second teardown. The reason code is the only signal
     * we have for WHY the actor exited (StreamError /
     * FrameTooLarge / etc.), so log it regardless of whether
     * we're going to act on it. Without this, the visible
     * failure mode is just "hxnet_connection_send_frame returned
     * -2" with no clue what killed the actor. */
    const char *reason_str;
    switch (reason) {
    case HXNET_SHUTDOWN_EOF:
        reason_str = "EOF (peer closed)";
        break;
    case HXNET_SHUTDOWN_STREAM_ERROR:
        reason_str = "STREAM_ERROR (mid-stream IO error)";
        break;
    case HXNET_SHUTDOWN_FRAME_TOO_LARGE:
        reason_str = "FRAME_TOO_LARGE (oversized wire len)";
        break;
    case HXNET_SHUTDOWN_HANDLE_DROPPED:
        reason_str = "HANDLE_DROPPED (we dropped the handle)";
        break;
    default:
        reason_str = "(unknown reason code)";
        break;
    }
    /* Visibility / level. A clean shutdown (EOF, HANDLE_DROPPED) is
     * expected during normal disconnect. So is ANY failure before the
     * session is fully up (`connected == 0`): connect refused, DNS,
     * magic / HOPE / TLS handshake, or a rejected login all surface as
     * STREAM_ERROR with the actor never reaching frame mode. Those are
     * routine and already get a user-facing error dialog from
     * hx_htlc_close, so keep them at g_message rather than reading
     * like a bug (a routine "connection refused" shouldn't spam an
     * alarming warning to the console). Only a STREAM_ERROR /
     * FRAME_TOO_LARGE *after* a working login (connected == 1) is the
     * noteworthy mid-session-drop signal — promote that to g_warning. */
    gboolean noteworthy = connected != 0 && reason != HXNET_SHUTDOWN_EOF
                          && reason != HXNET_SHUTDOWN_HANDLE_DROPPED;
    if (noteworthy) {
        g_warning ("hxnet_bridge: actor exited mid-session with reason=%d "
                   "%s (htlc->fd=%d) — see stderr for the Rust-side "
                   "ShutdownReason (io::Error string for StreamError).",
                   reason, reason_str, hx_conn_fd (htlc));
    } else {
        g_message ("hxnet_bridge: actor exited with reason=%d %s "
                   "(htlc->fd=%d connected=%d)",
                   reason, reason_str, hx_conn_fd (htlc), connected);
    }

    /* Drop in-flight shutdowns that the GLib idle queue dispatched
     * after `hx_htlc_close` already ran (e.g. the synchronous
     * send-path failure path called close, which uninstalled the
     * bridge, but hxnet had already forwarded an `Event::Shutdown`
     * onto the idle queue). Re-entering `hx_htlc_close` would run all
     * of its teardown a second time.
     *
     * The re-entry guard is `htlc->fd == 0`: hx_htlc_close sets fd to
     * 0 as its last act, so a second shutdown for the same connection
     * sees fd==0 and bails. We deliberately do NOT also gate on
     * `!hx_bridge_is_installed (htlc)` — bridge_on_shutdown_cb dispatches
     * *before* it tears the handle down, precisely so this path can
     * call hx_htlc_close (which then uninstalls). Gating on
     * being-installed here was the bug behind connect-refused (and any
     * server-initiated shutdown) leaving the UI stuck: the handle was
     * cleared first, so this early-returned and hx_htlc_close never
     * ran. fd is -1 on the orchestrator path (no C-visible fd) and the
     * real fd on the legacy path — both non-zero, so both proceed. */
    if (hx_conn_fd (htlc) == 0) {
        return;
    }

    /* expected = 1 means "clean close" — hx_htlc_close
     * suppresses the error dialog. EOF + HandleDropped are
     * clean; any other reason (stream error, oversized frame,
     * unknown code) surfaces to the user. */
    int expected;
    switch (reason) {
    case HXNET_SHUTDOWN_EOF:
    case HXNET_SHUTDOWN_HANDLE_DROPPED:
        expected = 1;
        break;
    case HXNET_SHUTDOWN_STREAM_ERROR:
    case HXNET_SHUTDOWN_FRAME_TOO_LARGE:
    default:
        expected = 0;
        break;
    }
    hx_htlc_close (htlc, expected);
}

/* ----------------------------------------------------------------- *
 * Lifecycle helpers (R3.3.e-4b)                                     *
 * ----------------------------------------------------------------- */

/*
 * Forward declarations for the hxnet FFI surface we wrap.
 * Hand-written instead of cbindgen — drift surfaces as a
 * link-time undefined symbol, same discipline as the other
 * hxnet / hxbridge / hotline-proto FFI consumers.
 *
 * Mirror of rust/crates/hxnet/src/ffi.rs.
 */
typedef struct hxnet_connection_opaque hxnet_connection_opaque;

typedef struct {
    guint32 type_;
    guint32 trans;
    guint32 flag;
    guint16 hc;
    guint16 _pad;
    guint32 body_len;
    guint8 *body_ptr;
} hxnet_frame_t;

typedef void (*hxnet_event_cb_t) (hxnet_connection_opaque *conn,
                                  hxnet_frame_t *frame, void *user_data);
typedef void (*hxnet_shutdown_cb_t) (hxnet_connection_opaque *conn, int reason,
                                     void *user_data);
/* Phase G state callback — fires once per ConnectionState
 * transition. Mirror of HxnetStateCallback in
 * rust/crates/hxnet/src/ffi.rs. `state` is a HXNET_BRIDGE_STATE_*
 * discriminant. */
typedef void (*hxnet_state_cb_t) (hxnet_connection_opaque *conn, guint32 state,
                                  void *user_data);
/* Phase G TLS TOFU: invoked post-handshake with the peer leaf cert's
 * "sha256:<hex>" fingerprint (NOT NUL-terminated — use fp_len).
 * Returns non-zero to accept, zero to reject. Mirror of
 * HxnetVerifyCertCallback in rust/crates/hxnet/src/ffi.rs. */
typedef int (*hxnet_verify_cert_cb_t) (const guint8 *fp, gsize fp_len,
                                       void *user_data);

/* ConnectionState discriminants the Phase G state callback maps
 * onto GtkhxConnectionState. Mirror of HXNET_STATE_* in
 * rust/crates/hxnet/src/ffi.rs (only the two with a coarse
 * GtkhxConnectionState equivalent are named here; the
 * intermediate handshake states are dropped). */
#define HXNET_BRIDGE_STATE_CONNECTED 2
#define HXNET_BRIDGE_STATE_LOGIN_SENDING 5
#define HXNET_BRIDGE_STATE_HANDSHAKE_DONE 10

extern int hxnet_connection_send_frame (hxnet_connection_opaque *handle,
                                        const guint8 *data, guint32 len);
extern void hxnet_connection_destroy (hxnet_connection_opaque *handle);
extern void hxnet_frame_free (hxnet_frame_t *f);

/* Phase G: hxnet drives the whole plaintext lifecycle (DNS + TCP +
 * magic + LOGIN + LOGIN-reply) and replays the reply as a synthetic
 * frame. Mirror of hxnet_connection_open_plaintext in
 * rust/crates/hxnet/src/ffi.rs. */
extern hxnet_connection_opaque *hxnet_connection_open_plaintext (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri, gsize proxy_uri_len,
    hxnet_event_cb_t on_event, hxnet_shutdown_cb_t on_shutdown,
    hxnet_state_cb_t on_state, void *user_data);

/* Phase G HOPE: hxnet drives the full HOPE-Secure-Login handshake
 * (magic + step1 + step2 + cipher transition) and the encrypted
 * post-login stream. Mirror of hxnet_connection_open_hope in
 * rust/crates/hxnet/src/ffi.rs. */
extern hxnet_connection_opaque *hxnet_connection_open_hope (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *cipher_alg, gsize cipher_alg_len,
    const guint8 *proxy_uri, gsize proxy_uri_len, hxnet_event_cb_t on_event,
    hxnet_shutdown_cb_t on_shutdown, hxnet_state_cb_t on_state,
    void *user_data);

/* Phase G TLS: plaintext Hotline over TLS-from-byte-zero (Mobius /
 * Janus separate-port model). Mirror of
 * hxnet_connection_open_plaintext_tls in rust/crates/hxnet/src/ffi.rs.
 * Cert trust is the verify_cert callback (TOFU, post-handshake). */
extern hxnet_connection_opaque *hxnet_connection_open_plaintext_tls (
    const guint8 *host, gsize host_len, guint16 port, const guint8 *login,
    gsize login_len, const guint8 *password, gsize password_len,
    const guint8 *name, gsize name_len, guint16 icon, guint16 version,
    guint16 caps, guint32 trans, const guint8 *proxy_uri, gsize proxy_uri_len,
    hxnet_event_cb_t on_event, hxnet_shutdown_cb_t on_shutdown,
    hxnet_state_cb_t on_state, hxnet_verify_cert_cb_t verify_cert,
    void *user_data);

/* Retained HOPE AEAD material getter (rust/crates/hxnet/src/ffi.rs):
 * returns an opaque HxnetHopeAead handle for a HOPE-ChaCha20 control
 * connection, or NULL otherwise. HxnetHopeAead is declared in
 * htxf_io.h (included above). */
extern HxnetHopeAead *
hxnet_connection_hope_aead_material (hxnet_connection_opaque *conn);

/* hx_tls_orchestrator_verify_cert (production TOFU verify, defined in
 * network.c) and hx_orchestrator_register_login_task are both declared
 * in network.h, included above. */

/*
 * There is no module-level connection state in this file.
 *
 * The transport handle lives on the connection it belongs to, reached
 * through hx_conn_bridge_handle / hx_conn_set_bridge_handle. It was a
 * file-static until the transport had to be per-connection: a single slot
 * meant every outbound frame went to whichever connection installed last,
 * the install entry points had to refuse a second connect outright, and the
 * stale-actor guards below — which compare an event's handle against the
 * slot — could not tell a second live connection's events from stale ones.
 *
 * Small helpers rather than open-coded accessor calls, because that identity
 * comparison is the whole of the stale-actor defence and should read the
 * same way at each of its sites.
 */
static hxnet_connection_opaque *
conn_handle (const struct htlc_conn *htlc)
{
    return htlc ? hx_conn_bridge_handle (htlc) : NULL;
}

static void
set_conn_handle (struct htlc_conn *htlc, hxnet_connection_opaque *h)
{
    if (htlc) {
        hx_conn_set_bridge_handle (htlc, h);
    }
}

/* C-side trampolines hxnet calls on the GLib main thread per
 * Event::Frame / Event::Shutdown. They translate the FFI
 * shapes into the bridge's existing dispatch helpers, free the
 * frame body (per the on_event ownership contract), and clear
 * the connection's handle on shutdown so the install gate flips off. */
static void
bridge_on_event_cb (hxnet_connection_opaque *conn, hxnet_frame_t *frame,
                    void *user_data)
{
    struct htlc_conn *htlc = user_data;
    /* Only the currently-installed handle may dispatch. After an
     * uninstall+reinstall cycle, an event the GLib forwarder had
     * already queued from the OLD actor could otherwise be injected
     * into the NEW connection's rcv state machine and corrupt it
     * (the connection is installed again, so a downstream gate
     * wouldn't catch it). `conn` is the same handle pointer install
     * stored on the connection, so this comparison is exact — and
     * because it is per-connection, a second live connection's events
     * are no longer indistinguishable from stale ones. The frame is
     * still freed below regardless, honouring the ownership contract
     * even when the event is dropped as stale. */
    if (htlc && conn == conn_handle (htlc) && frame) {
        hx_bridge_dispatch_frame (htlc, frame->type_, frame->trans, frame->flag,
                                  frame->hc, frame->body_ptr, frame->body_len);
    }
    /* Per the hxnet callback contract, the C side owns the
     * body memory behind body_ptr/body_len for the lifetime of
     * this call. Freeing here keeps the lifetime tight and
     * matches the "process and free inside the callback"
     * pattern documented on HxnetEventCallback. */
    hxnet_frame_free (frame);
}

static void
bridge_on_shutdown_cb (hxnet_connection_opaque *conn, int reason,
                       void *user_data)
{
    struct htlc_conn *htlc = user_data;

    /* Ignore a shutdown delivered by a stale actor. If an old
     * actor's shutdown was queued on the main loop and only runs
     * after a reconnect installed a new handle, `conn` no longer
     * matches the handle stored on the connection; proceeding would
     * clear/destroy the NEW
     * handle and tear down the live connection. The stale actor was
     * already destroyed when it was uninstalled, so there's nothing
     * to free here — just return. */
    if (!htlc || conn != conn_handle (htlc)) {
        return;
    }

    /* The actor has already exited (that's what got us into this
     * callback); we need to tear down the hxnet handle and the C-side
     * connection state. The handle teardown (dropping the cmd
     * channel, pump task, and forwarder) happens via
     * hx_bridge_uninstall, reached through hx_htlc_close below — so
     * the lifetime stays deterministic within this callback.
     *
     * Dispatch FIRST, while the handle is still installed. The actor
     * has already exited (that's what got us here), so
     * hx_bridge_dispatch_shutdown's only liveness gate is
     * `htlc->fd != 0`; when that holds it runs hx_htlc_close, which
     * emits DISCONNECTED, clears the pending tasks, and calls
     * hx_bridge_uninstall — and uninstall is what destroys + clears
     * the connection's handle. So after dispatch it is normally already
     * gone.
     *
     * (Earlier this cleared the handle first and then dispatched,
     * which combined with dispatch_shutdown's old !is_installed()
     * guard to skip hx_htlc_close entirely — that left connect-refused
     * and other server-initiated shutdowns stuck with a spinning
     * throbber and dangling task rows.)
     *
     * Destroying the handle from inside hx_htlc_close runs within this
     * forwarder closure — the same depth the previous explicit destroy
     * ran at. */
    hx_bridge_dispatch_shutdown (htlc, reason);

    /* Defensive: if dispatch didn't tear the handle down — fd was already
     * 0 from a prior close, so dispatch bailed — release the actor's
     * resources here so they're freed exactly once. Normal
     * server-initiated shutdowns already cleared it via hx_htlc_close →
     * hx_bridge_uninstall above. */
    hxnet_connection_opaque *to_destroy = conn_handle (htlc);
    if (to_destroy) {
        set_conn_handle (htlc, NULL);
        hxnet_connection_destroy (to_destroy);
    }
}

/* Phase G state trampoline. hxnet fires this on the GLib main
 * thread once per ConnectionState transition; we map the
 * fine-grained discriminants onto the coarse GtkhxConnectionState
 * the toolbar / chat windows already listen to and emit on the
 * default session. The legacy GIOStream connect path emits the same
 * coarse sequence (CONNECTING in hx_connect → TCP_CONNECTED in
 * on_async_connected → HANDSHAKE_DONE in send_login); the
 * orchestrator just sources the transitions from the Rust state
 * machine. Intermediate states (magic / login-sending /
 * login-reply-wait / HOPE) have no coarse equivalent and are
 * dropped — the throbber doesn't need that granularity, and
 * CONNECTING was already emitted in hx_connect_via_orchestrator. */
static void
bridge_on_state_cb (hxnet_connection_opaque *conn G_GNUC_UNUSED, guint32 state,
                    void *user_data)
{
    GtkhxSession *sess = gtkhx_session_get_default ();
    switch (state) {
    case HXNET_BRIDGE_STATE_CONNECTED:
        gtkhx_session_emit_connection_state (sess,
                                             GTKHX_CONNECTION_TCP_CONNECTED);
        break;
    case HXNET_BRIDGE_STATE_LOGIN_SENDING:
        /* Magic done, credentials about to go out — the orchestrator's
         * equivalent of the legacy path's send_login moment. Drive the
         * same two UI effects, in the same order, so the Tasks window
         * looks identical to legacy: emit HANDSHAKE_DONE (which deletes
         * the coarse "Connecting" task), then register the "login"
         * protocol task. The login task must exist before the replayed
         * LOGIN/step-2 reply frame arrives to dispatch to it; that
         * frame is emitted strictly after this state on the same
         * ordered event channel, so registering here is in time. */
        gtkhx_session_emit_connection_state (sess,
                                             GTKHX_CONNECTION_HANDSHAKE_DONE);
        hx_orchestrator_register_login_task ((struct htlc_conn *)user_data);
        break;
    case HXNET_BRIDGE_STATE_HANDSHAKE_DONE:
        /* Rust's end-of-handshake state. No view transition here: the
         * coarse HANDSHAKE_DONE already fired at LOGIN_SENDING above,
         * and login completion is signalled by LOGIN_READY, which
         * rcv_task_login emits when the replayed reply dispatches. */
        break;
    default:
        break;
    }
}

/*
 * Ask GProxyResolver whether the system is configured to reach
 * (host, port) through a SOCKS proxy, and if so return its URI
 * (g_strdup'd — caller frees) for the orchestrated open_* FFIs to
 * tunnel through. Returns NULL for "connect direct".
 *
 * The query URI uses the "none" scheme, matching what GSocketClient
 * itself passes for a raw TCP (non-URL) connect: scheme-agnostic proxy
 * rules (e.g. an `all_proxy` / ALL_PROXY env var, or a GNOME system-wide
 * SOCKS proxy) then apply. Hotline has no registered URI scheme, so a
 * scheme-specific HTTP/HTTPS rule must not steal this lookup.
 *
 * Only SOCKS results are honoured: tokio-socks (the Rust transport) can't
 * tunnel a raw Hotline stream through an HTTP-CONNECT proxy, so a non-SOCKS
 * proxy result is logged and skipped (connect direct) rather than silently
 * mishandled. "direct://" entries mean no proxy and are skipped.
 *
 * This is the *synchronous* g_proxy_resolver_lookup, called on the GLib
 * main thread from the connect-install path. For the default resolver
 * backends — GSettings (GNOME) and the env-var GSimpleProxyResolver — the
 * lookup is an in-memory rule match and doesn't block. A backend that
 * executes a PAC script / WPAD could block the UI here; making the install
 * path async (g_proxy_resolver_lookup_async, with the open_* FFI call
 * deferred into the completion callback) is the fix if that ever matters,
 * but it restructures the synchronous "handle set before return"
 * install contract, so it's deliberately deferred.
 */
/* Return a g_strdup'd copy of a proxy URI with any `user:pass@` userinfo
 * replaced by `***@`, so logging it can't leak proxy credentials (e.g. an
 * `all_proxy=http://user:pass@proxy:8080` the resolver hands back). Caller
 * frees. */
static char *
bridge_redact_uri_userinfo (const char *uri)
{
    const char *scheme_end = strstr (uri, "://");
    if (!scheme_end) {
        return g_strdup (uri);
    }
    const char *authority = scheme_end + 3;
    const char *at = strchr (authority, '@');
    const char *slash = strchr (authority, '/');
    /* Only an `@` before the first path `/` is userinfo. */
    if (at && (!slash || at < slash)) {
        return g_strdup_printf ("%.*s://***@%s", (int)(scheme_end - uri), uri,
                                at + 1);
    }
    return g_strdup (uri);
}

char *
hx_bridge_lookup_socks_proxy (const char *host, guint16 port)
{
    GProxyResolver *resolver = g_proxy_resolver_get_default ();
    if (!resolver) {
        return NULL;
    }

    /* Percent-escape the host before interpolating it, so a character
     * that's reserved in URI syntax doesn't make g_proxy_resolver_lookup
     * reject the lookup as malformed — notably an IPv6 zone id's `%`
     * (fe80::1%eth0 → fe80::1%25eth0). Keep `:` unescaped so IPv6 colons
     * survive, then let gtkhx_join_host_port bracket the IPv6 literal so
     * the URI stays well-formed (none://[2001:db8::1]:5500). */
    g_autofree char *esc_host = g_uri_escape_string (host, ":", FALSE);
    g_autofree char *hostport = gtkhx_join_host_port (esc_host, port);
    g_autofree char *uri = g_strdup_printf ("none://%s", hostport);
    GError *err = NULL;
    char **proxies = g_proxy_resolver_lookup (resolver, uri, NULL, &err);
    if (err) {
        g_warning ("hxnet_bridge: proxy lookup for %s failed: %s", uri,
                   err->message);
        g_error_free (err);
        return NULL;
    }

    char *result = NULL;
    for (char **p = proxies; p && *p; p++) {
        if (g_str_has_prefix (*p, "socks")) {
            /* socks://, socks4://, socks5:// — the URI ProxyConfig::from_uri
             * parses on the Rust side. */
            result = g_strdup (*p);
            break;
        }
        if (g_str_has_prefix (*p, "direct")) {
            continue; /* no proxy for this destination */
        }
        /* Non-SOCKS, non-direct (e.g. http://): we can't tunnel Hotline
         * through it. Warn loudly so a misrouted connection is diagnosable
         * rather than silently bypassing the configured proxy — but redact
         * any userinfo first so proxy credentials don't hit the log. */
        g_autofree char *redacted = bridge_redact_uri_userinfo (*p);
        g_warning ("hxnet_bridge: ignoring non-SOCKS proxy %s for %s "
                   "(only SOCKS proxies are supported); connecting direct",
                   redacted, uri);
    }
    g_strfreev (proxies);
    return result;
}

gboolean
hx_bridge_install_orchestrated_plaintext (struct htlc_conn *htlc,
                                          const char *host, guint16 port,
                                          const char *login, const char *pass,
                                          const char *name, guint16 icon,
                                          guint16 version, guint16 caps,
                                          guint32 trans)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (host != NULL && *host, FALSE);

    /* Refuse only a second install over the *same* connection — that would
     * orphan the first actor and its socket with nothing left pointing at
     * them. A second install on a *different* connection is the whole point
     * of moving the handle onto the connection, and is no longer refused. */
    if (conn_handle (htlc)) {
        g_critical ("hxnet_bridge: plaintext install attempted on a "
                    "connection that already has a transport; refusing");
        return FALSE;
    }

    login = login ? login : "";
    pass = pass ? pass : "";
    name = name ? name : "";

    /* open_plaintext spawns the lifecycle task and wires the
     * forwarder synchronously; events don't fire until we return to
     * the GLib main loop. Storing the handle on the connection before
     * that return is what makes the orchestrator's replayed LOGIN-reply
     * frame pass hx_bridge_dispatch_frame's installed gate.
     * user_data is the htlc for all three callbacks. */
    /* open_plaintext parses proxy_uri synchronously (before spawning the
     * lifecycle task), so this g_autofree URI is safe to free on return. */
    g_autofree char *proxy_uri = hx_bridge_lookup_socks_proxy (host, port);
    hxnet_connection_opaque *h = hxnet_connection_open_plaintext (
        (const guint8 *)host, strlen (host), port, (const guint8 *)login,
        strlen (login), (const guint8 *)pass, strlen (pass),
        (const guint8 *)name, strlen (name), icon, version, caps, trans,
        (const guint8 *)proxy_uri, proxy_uri ? strlen (proxy_uri) : 0,
        bridge_on_event_cb, bridge_on_shutdown_cb, bridge_on_state_cb, htlc);
    if (!h) {
        /* open_plaintext logs its own g_critical on the failure
         * paths (NULL/empty host, non-UTF-8 host, trans==0, runtime
         * panic). Leave the bridge uninstalled. */
        return FALSE;
    }
    set_conn_handle (htlc, h);
    return TRUE;
}

gboolean
hx_bridge_install_orchestrated_hope (struct htlc_conn *htlc, const char *host,
                                     guint16 port, const char *login,
                                     const char *pass, const char *name,
                                     guint16 icon, guint16 version,
                                     guint16 caps, guint32 trans,
                                     const char *cipher_alg)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (host != NULL && *host, FALSE);
    /* cipher_alg may be NULL/empty: that selects HOPE secure-login with
     * no transport cipher (HMAC auth over plaintext — mhxd's
     * non-cipher_only mode). The FFI treats len==0 as "no cipher". */
    g_return_val_if_fail (cipher_alg != NULL, FALSE);

    /* See the plaintext install for why this refuses. */
    if (conn_handle (htlc)) {
        g_critical ("hxnet_bridge: HOPE install attempted on a "
                    "connection that already has a transport; refusing");
        return FALSE;
    }

    login = login ? login : "";
    pass = pass ? pass : "";
    name = name ? name : "";

    /* Same synchronous-install-before-return discipline as the
     * plaintext variant: the bridge handle must be live before the
     * forwarder can deliver the replayed step-2 reply. */
    g_autofree char *proxy_uri = hx_bridge_lookup_socks_proxy (host, port);
    hxnet_connection_opaque *h = hxnet_connection_open_hope (
        (const guint8 *)host, strlen (host), port, (const guint8 *)login,
        strlen (login), (const guint8 *)pass, strlen (pass),
        (const guint8 *)name, strlen (name), icon, version, caps, trans,
        (const guint8 *)cipher_alg, strlen (cipher_alg),
        (const guint8 *)proxy_uri, proxy_uri ? strlen (proxy_uri) : 0,
        bridge_on_event_cb, bridge_on_shutdown_cb, bridge_on_state_cb, htlc);
    if (!h) {
        return FALSE;
    }
    set_conn_handle (htlc, h);
    return TRUE;
}

/* Return an opaque HOPE AEAD material handle for the currently installed
 * connection, or NULL when it has no transport installed or the control
 * channel did not negotiate ChaCha20-Poly1305 (plaintext / Blowfish /
 * no-cipher leave the retained slot empty). The caller owns the handle
 * and must free it with hxnet_hope_aead_free. Called at login completion
 * (rcv_task_login) to seed htlc->hope_aead so HTXF subchannels can derive
 * their per-transfer keys in-process — by which point the handshake is
 * done and the material slot is populated. */
HxnetHopeAead *
hx_bridge_orchestrated_hope_aead (const struct htlc_conn *htlc)
{
    hxnet_connection_opaque *h = conn_handle (htlc);
    if (!h) {
        return NULL;
    }
    return hxnet_connection_hope_aead_material (h);
}

/* TLS TOFU trampoline: hxnet calls this on the lifecycle task with the
 * peer cert fingerprint ONLY when WebPKI validation against the native
 * roots failed (a CA-valid cert is trusted silently and never reaches
 * here); route to the production verify, which keys on
 * htlc->serverhost/serverport. */
static int
bridge_on_verify_cert_cb (const guint8 *fp, gsize fp_len, void *user_data)
{
    struct htlc_conn *htlc = user_data;
    if (!htlc || !fp) {
        return 0; /* reject: no context / no fingerprint */
    }
    g_autofree char *fp_str = g_strndup ((const char *)fp, fp_len);
    return hx_tls_orchestrator_verify_cert (htlc, fp_str) ? 1 : 0;
}

gboolean
hx_bridge_install_orchestrated_plaintext_tls (
    struct htlc_conn *htlc, const char *host, guint16 port, const char *login,
    const char *pass, const char *name, guint16 icon, guint16 version,
    guint16 caps, guint32 trans)
{
    g_return_val_if_fail (htlc != NULL, FALSE);
    g_return_val_if_fail (host != NULL && *host, FALSE);

    /* See the plaintext install for why this refuses. */
    if (conn_handle (htlc)) {
        g_critical ("hxnet_bridge: TLS install attempted on a "
                    "connection that already has a transport; refusing");
        return FALSE;
    }

    login = login ? login : "";
    pass = pass ? pass : "";
    name = name ? name : "";

    g_autofree char *proxy_uri = hx_bridge_lookup_socks_proxy (host, port);
    hxnet_connection_opaque *h = hxnet_connection_open_plaintext_tls (
        (const guint8 *)host, strlen (host), port, (const guint8 *)login,
        strlen (login), (const guint8 *)pass, strlen (pass),
        (const guint8 *)name, strlen (name), icon, version, caps, trans,
        (const guint8 *)proxy_uri, proxy_uri ? strlen (proxy_uri) : 0,
        bridge_on_event_cb, bridge_on_shutdown_cb, bridge_on_state_cb,
        bridge_on_verify_cert_cb, htlc);
    if (!h) {
        return FALSE;
    }
    set_conn_handle (htlc, h);
    return TRUE;
}

gboolean
hx_bridge_is_installed (const struct htlc_conn *htlc)
{
    return conn_handle (htlc) != NULL;
}

int
hx_bridge_send_frame (struct htlc_conn *htlc, const guint8 *data, guint32 len)
{
    hxnet_connection_opaque *h = conn_handle (htlc);
    if (!h) {
        g_critical ("hxnet_bridge: send_frame called with no installed "
                    "connection");
        /* Distinct sentinel — production hxnet returns the
         * HXNET_SEND_* set (0, -1, -2, -3); HX_BRIDGE_SEND_NOT_INSTALLED
         * sits outside that range so callers can distinguish
         * "bridge not installed" from kernel backpressure / closed
         * channel. */
        return HX_BRIDGE_SEND_NOT_INSTALLED;
    }
    int rc = hxnet_connection_send_frame (h, data, len);
    if (rc != 0) {
        g_critical ("hxnet_bridge: hxnet_connection_send_frame returned %d",
                    rc);
    }
    return rc;
}

void
hx_bridge_uninstall (struct htlc_conn *htlc)
{
    hxnet_connection_opaque *h = conn_handle (htlc);
    if (!h) {
        return;
    }
    set_conn_handle (htlc, NULL);
    hxnet_connection_destroy (h);
}
