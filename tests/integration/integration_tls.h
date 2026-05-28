#ifndef HX_INTEGRATION_TLS_H
#define HX_INTEGRATION_TLS_H 1

/*
 * GIOStream-based TLS variant of the Tier 3 harness.
 *
 * The legacy fd-based helpers in integration_harness.{c,h} (which
 * 30+ existing tests are built on) talk plaintext over a raw socket
 * fd. Wrapping that path in TLS would force either an intrusive
 * fd-to-stream sweep across every test or an in-process socketpair
 * + pump-thread shim — neither cheap.
 *
 * For Phase 1 close-out (docs/tls-scoping.md), we instead expose a
 * parallel `_stream` family that takes a GIOStream (returned by
 * hx_test_server_connect_tls) and otherwise mirrors the existing
 * fd-based helpers line-for-line. New TLS-targeting tests pick
 * these; existing plaintext tests stay unchanged.
 *
 * Once the fd-based API is fully gone (or both APIs warrant
 * unification), the right move is to hoist these into the main
 * harness behind a single hx_io abstraction. Until then, two
 * parallel surfaces is the smaller-blast-radius choice.
 *
 * The connect helper uses the same g_socket_client_set_tls +
 * accept-everything-cert plumbing as production hx_connect tls=1
 * (see src/network.c::tls_accept_certificate_phase1_stub) so we
 * exercise the same handshake path the production client uses.
 */

#include <glib.h>
#include <gio/gio.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "server_matrix.h"

/* ---- Connect / close ---------------------------------------------- */

/*
 * Open a TLS-wrapped HTLS connection to `srv`'s tls_port. Uses
 * GSocketClient with set_tls(TRUE) + an accept-everything cert
 * handler (same shape as production hx_connect Phase 1 stub).
 *
 * Returns an owned GIOStream (a GTlsClientConnection) on success,
 * or NULL on connect / handshake failure. Errors are reported via
 * g_warning so test output captures them; callers should assert on
 * non-NULL and skip / fail accordingly.
 *
 * `srv` must have tls_port != 0 — callers should pick it via
 * hx_test_servers_with(HX_TEST_CAP_TLS).
 */
extern GIOStream *hx_test_server_connect_tls (const hx_test_server *srv);

/*
 * Close the TLS stream cleanly (g_io_stream_close, which flushes
 * the TLS shutdown alert) and drop the ref. Safe with NULL.
 */
extern void integration_close_stream (GIOStream *io);

/* ---- Low-level I/O ------------------------------------------------- */

/*
 * Write exactly `len` bytes from `buf` to the stream. Blocks until
 * fully written or an error occurs.
 */
extern gboolean integration_send_stream (GIOStream *io, const void *buf,
                                         gsize len);

/*
 * Read exactly `len` bytes into `buf`. Blocks until all bytes
 * arrive, an error occurs, or the read times out. Uses a socket-
 * level timeout on the underlying GSocket so reads return cleanly
 * after `timeout_ms` of no data. Pass `timeout_ms <= 0` for the
 * harness default (5 s).
 */
extern gboolean integration_recv_stream (GIOStream *io, void *buf,
                                         gsize len, int timeout_ms);

/* ---- Hotline protocol ---------------------------------------------- */

/*
 * Same HTLC_MAGIC -> HTLS_MAGIC exchange as integration_handshake,
 * over the TLS stream.
 */
extern gboolean integration_handshake_stream (GIOStream *io);

/*
 * Pack + send one Hotline message via hlpack. Variadic args match
 * integration_send_message:
 *   integration_send_message_stream (io, htlc, type, flag, hc,
 *                                    type1, len1, data1, ...);
 */
extern gboolean
integration_send_message_stream (GIOStream *io, struct htlc_conn *htlc,
                                 guint32 type, guint32 flag, int hc, ...);

/*
 * Read one full Hotline message into htlc->in. Same shape as
 * integration_recv_message — header first, then body sized by the
 * hl_hdr decoded wire length.
 */
extern gboolean
integration_recv_message_stream (GIOStream *io, struct htlc_conn *htlc,
                                 int timeout_ms);

/*
 * Send HTLC_HDR_LOGIN with guest credentials. Mirrors
 * integration_login_guest's request shape (including the inline
 * HTLC_DATA_NAME the harness sends for assertion purposes — see
 * the existing function's docstring for the rationale).
 */
extern gboolean
integration_login_guest_stream (GIOStream *io, struct htlc_conn *htlc,
                                const char *display_name, guint16 icon);

/*
 * Drain to either HTLS_HDR_USER_SELFINFO (login succeeded) or
 * HTLS_HDR_TASK with the error bit (login refused). Returns the
 * triggering type, or 0 on timeout. Same contract as
 * integration_drain_until_selfinfo_or_error.
 */
extern guint32
integration_drain_until_selfinfo_or_error_stream (GIOStream *io,
                                                  struct htlc_conn *htlc,
                                                  int max_messages);

/*
 * Drain until the first HTLS message of `wanted_type` arrives.
 * Same shape as integration_drain_until_type.
 */
extern gboolean
integration_drain_until_type_stream (GIOStream *io, struct htlc_conn *htlc,
                                     guint16 wanted_type, int max_messages);

/*
 * Send HTLC_HDR_CHAT with the body text. Style is hardcoded to 1
 * (plain text), matching the fd-based integration_send_chat.
 */
extern gboolean
integration_send_chat_stream (GIOStream *io, struct htlc_conn *htlc,
                              const char *text);

/*
 * Drain until an HTLS_HDR_CHAT broadcast addressed from `wanted_uid`
 * arrives. Same uid-filter as integration_drain_until_chat — needed
 * because logged-in sessions see broadcasts from every user in the
 * chat (including their own).
 */
extern gboolean
integration_drain_until_chat_stream (GIOStream *io, struct htlc_conn *htlc,
                                     guint16 wanted_uid,
                                     struct hx_chat_msg *out,
                                     int max_messages);

/*
 * Drain server messages until a TASK reply correlates to
 * `wanted_trans`. Mirrors integration_drain_until_task_trans.
 * Tier 3 TLS tests need this to walk past interleaved server
 * pushes (banner, USER_CHANGE, agreement, etc.) before the TASK
 * reply they actually care about arrives.
 */
extern gboolean
integration_drain_until_task_trans_stream (GIOStream *io,
                                           struct htlc_conn *htlc,
                                           guint32 wanted_trans,
                                           int max_messages);

/*
 * Open a TLS-wrapped HTXF subchannel to `srv`'s tls_xfer_port.
 * Same accept-everything cert handler as the HTLS connect helper
 * — proves the same trust stub covers both subchannels. Returns
 * NULL on connect / handshake failure; callers should
 * g_test_fail_printf and bail.
 *
 * `srv` must have tls_xfer_port != 0. Phase 2 leaves the AEAD
 * subchannel arming to the caller (TLS-only tests don't run HOPE
 * + AEAD; the existing HOPE+ChaCha20 Tier 3 already covers that
 * layer over plaintext HTXF, and TLS-over-AEAD is intentionally
 * out of scope until Phase 3+ work decides whether the layered
 * combo is even sensible).
 */
extern GIOStream *hx_test_server_connect_xfer_tls (const hx_test_server *srv);

/*
 * Compose helper: connect TLS, magic handshake, guest LOGIN, drain
 * to SELFINFO. Returns the open GIOStream on success or NULL with
 * g_test_fail_printf already called. Caller closes via
 * integration_close_stream.
 *
 * Like integration_open_login_or_skip but TLS-wrapped end-to-end.
 * Callers pre-filter the matrix with hx_test_servers_with
 * (HX_TEST_CAP_TLS) to choose `srv`.
 */
extern GIOStream *
integration_open_login_tls_or_skip (const hx_test_server *srv,
                                    struct htlc_conn *htlc,
                                    const char *display_name, guint16 icon);

#endif /* HX_INTEGRATION_TLS_H */
