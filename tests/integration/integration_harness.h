#ifndef HX_INTEGRATION_HARNESS_H
#define HX_INTEGRATION_HARNESS_H 1

/*
 * Tier 3 integration-test harness — drives a real TCP connection
 * to a Hotline server (the mhxd Docker container under tests/mhxd
 * by default) and exercises the protocol layer end-to-end.
 *
 * The harness is intentionally minimal: it opens a socket, runs
 * the magic handshake, and gives tests blocking send / receive
 * primitives over the connection. No worker threads, no GTK, no
 * libadwaita — the test binary links against proto_helpers and
 * the system socket APIs and that's it.
 *
 * Skip-if-unavailable contract: if the server isn't reachable
 * within a short timeout, integration_connect returns -1 and the
 * test is expected to call g_test_skip and return. That way the
 * tests run on machines with mhxd running and skip silently
 * elsewhere.
 *
 * Configuration via env vars:
 *
 *   GTKHX_TEST_HOST   — server hostname (default: 127.0.0.1)
 *   GTKHX_TEST_PORT   — server port     (default: 5500)
 *
 * The first time integration_connect is called the harness reads
 * these and caches them.
 */

#include <glib.h>
#include <netinet/in.h>
#include "compat.h" /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h" /* struct htlc_conn — referenced by the inline hdr_* below */
#include "server_matrix.h"

/*
 * Convenience accessors over the wire header that integration_recv_message
 * just dropped into htlc->in.buf. Every Tier 3 test wants type / flag /
 * trans to filter incoming messages — pre-refactor each test had its own
 * static guint32 hdr_type / hdr_flag / hdr_trans copies (34 binaries,
 * dozens of duplicate 4-line wrappers). Inline static here keeps the
 * call shape unchanged at every site (hdr_type(&htlc)) while removing
 * the dupe. No new linker symbols — each .c just inlines the ntohl. */
static inline guint32
hdr_type (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
    return ntohl (h->type);
}

static inline guint32
hdr_flag (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
    return ntohl (h->flag);
}

static inline guint32
hdr_trans (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
    return ntohl (h->trans);
}

/*
 * Open a TCP connection to the configured host:port.
 *
 * Returns the socket fd on success, or -1 if:
 *   - the host can't be resolved
 *   - the connection is refused
 *   - the connection takes longer than the timeout (default 2 s)
 *
 * The caller closes the fd via integration_close.
 */
extern int integration_connect (void);

/*
 * Run the 12-byte client magic + 8-byte server magic exchange:
 *
 *   client → "TRTPHOTL\0\1\0\2"  (HTLC_MAGIC)
 *   server → "TRTP\0\0\0\0"       (HTLS_MAGIC)
 *
 * Returns TRUE on a clean exchange, FALSE on any read/write error
 * or magic-byte mismatch.
 */
extern gboolean integration_handshake (int fd);

/*
 * Read exactly `len` bytes from `fd` into `buf`. Blocks until all
 * bytes arrive, an error occurs, or the read times out (default
 * 5 s). Returns TRUE on success, FALSE on partial / failed reads.
 */
extern gboolean integration_recv (int fd, void *buf, gsize len);

/*
 * Write exactly `len` bytes from `buf` to `fd`. Blocks until all
 * bytes are written or an error occurs. Returns TRUE on success.
 */
extern gboolean integration_send (int fd, const void *buf, gsize len);

/*
 * Close the integration socket. Safe to call with -1 (no-op).
 */
extern void integration_close (int fd);

/*
 * Helper: combine integration_connect + integration_handshake. If
 * the connect fails, calls g_test_skip with an explanatory message
 * and returns -1. If the handshake fails after the connect,
 * g_test_fail and returns -1. On success returns the fd.
 *
 * The most common open of an integration test:
 *
 *   int fd = integration_open_or_skip ();
 *   if (fd < 0) return;
 *   ... drive the protocol over fd ...
 *   integration_close (fd);
 */
extern int integration_open_or_skip (void);

/*
 * Connect with a short timeout to an explicit host:port pair —
 * used by server_matrix.c so the matrix walker doesn't have to
 * duplicate the addrinfo + non-blocking-connect dance. Returns
 * the socket fd on success, -1 on resolve failure, connection
 * refused, or timeout. The legacy integration_connect() above
 * is a thin wrapper that reads GTKHX_TEST_HOST / GTKHX_TEST_PORT
 * and routes through this. Phase A multi-server work.
 */
extern int hx_integration_connect_to (const char *host, int port,
                                      int timeout_ms);

/* ---- Higher-level helpers built on send / recv ----------------- */

struct htlc_conn;

/*
 * Pack and send a Hotline message via the production hlpack helper.
 * Variadic args follow the same shape as hlwrite():
 *
 *   integration_send_message (fd, htlc, type, flag, hc,
 *                             type1, len1, data1,
 *                             type2, len2, data2, ...);
 *
 * `htlc` is used purely as scratch space — its in.buf may be nuked
 * by integration_recv_message. The caller owns lifetime; pass a
 * memset-zero'd struct htlc_conn on the stack and free its in/out
 * bufs at end of test via integration_release_htlc.
 *
 * Returns TRUE on full send.
 */
extern gboolean integration_send_message (int fd, struct htlc_conn *htlc,
                                          guint32 type, guint32 flag, int hc,
                                          ...);

/*
 * Read one full Hotline message into htlc->in (overwriting any
 * previous contents). The caller can then walk the chunks via
 * dh_start, drive proto_helpers extractors, etc.
 *
 * Times out after `timeout_ms` if no header arrives. Returns TRUE
 * on a clean read; FALSE on timeout, short read, or oversized
 * payload (> 1 MiB, which is the same cap network.c enforces).
 */
extern gboolean integration_recv_message (int fd, struct htlc_conn *htlc,
                                          int timeout_ms);

/*
 * Free any g_malloc'd qbuf memory inside htlc->in / htlc->out.
 * Safe to call on a memset-zero htlc.
 */
extern void integration_release_htlc (struct htlc_conn *htlc);

/*
 * Send a guest login (no password). This is the canonical "any
 * server should accept this" handshake — mhxd's default config
 * exposes a `guest` account with no password and basic permissions
 * (chat, file download, etc.).
 *
 * Sends HTLC_HDR_LOGIN with three chunks:
 *   HTLC_DATA_ICON   = `icon` big-endian
 *   HTLC_DATA_LOGIN  = hl_code-encoded "guest"
 *   HTLC_DATA_NAME   = `display_name` (utf8 bytes, unencoded)
 *
 * Servers that don't have a guest account will respond with a
 * task-error; the test should consume the next message and
 * inspect the flag bit.
 */
extern gboolean integration_login_guest (int fd, struct htlc_conn *htlc,
                                         const char *display_name,
                                         guint16 icon);

/*
 * Capability-aware guest login. Same wire shape as
 * integration_login_guest plus an HTLC_DATA_CAPABILITIES chunk
 * advertising `caps` (a bitmask of HTLC_CAP_* values from
 * hotline.h — typically HTLC_CAP_CHAT_HISTORY for the chat-history
 * extension test set, or 0 to advertise no capabilities).
 *
 * Cap-aware servers (Janus) echo the agreed-on bits in the TASK
 * login reply via HTLS_DATA_CAPABILITIES;
 * integration_drain_until_selfinfo_or_error stashes that echo
 * into htlc->caps so the caller can gate behaviour on the
 * negotiated bits the same way production gtkhx does in
 * src/rcv.c::rcv_task_login.
 *
 * Cap-unaware servers (mhxd) silently ignore the chunk per spec.
 */
extern gboolean integration_login_guest_caps (int fd, struct htlc_conn *htlc,
                                              const char *display_name,
                                              guint16 icon, guint16 caps);

/*
 * Drain server messages until we either see HTLS_HDR_USER_SELFINFO
 * (login fully accepted, server has sent us our session state) or
 * a task-error reply (login refused). Returns the wire type that
 * broke the loop, or 0 on timeout.
 *
 * The caller already-sent integration_login_guest before calling
 * this. The harness loops up to `max_messages` per call (8 by
 * default if you pass 0) so a server that interleaves AGREEMENT
 * or banner messages before SELFINFO is handled cleanly.
 *
 * On success the LAST received message is in htlc->in — typically
 * the SELFINFO or the task-error chunk — and the caller can run
 * extractors against it directly.
 */
extern guint32
integration_drain_until_selfinfo_or_error (int fd, struct htlc_conn *htlc,
                                           int max_messages);

/*
 * Compose the previous helpers: open the socket, run the magic
 * handshake, send a guest login, drain to SELFINFO. Returns the
 * fd on full success, or -1 if any step failed (with the
 * appropriate g_test_skip / g_test_fail already called). The
 * caller closes the fd when done via integration_close.
 *
 * `display_name` is the string the server sees for our chat /
 * user-list entry. `icon` is the small-icon id (412 is a sensible
 * default — that's GtkHx's own icon).
 *
 * On return, `htlc` is in the state the harness left it: the last
 * received message (the SELFINFO) is in htlc->in and the htlc
 * fields hx_selfinfo_parse fills (uid, icon, name, access) are
 * already populated.
 */
extern int integration_open_login_or_skip (struct htlc_conn *htlc,
                                           const char *display_name,
                                           guint16 icon);

/*
 * Variant of integration_open_login_or_skip that targets a specific
 * matrix entry and advertises `caps` to the server. Used by the
 * chat-history tests which need to talk to a chat-history-capable
 * server (Janus) regardless of which entry is the default.
 *
 * Picks `srv` (must be non-NULL — typically from
 * hx_test_servers_with), opens a TCP connection to its
 * host:port with a 2 s connect timeout, runs the magic handshake,
 * sends a guest LOGIN with the requested capability advertisement,
 * and drains to SELFINFO. Returns the connected fd, or -1 with
 * g_test_skip / g_test_fail already called.
 *
 * On return htlc is in the state the harness left it, including
 * htlc->caps populated with whatever bits the server echoed back
 * in HTLS_DATA_CAPABILITIES during the drain.
 */
extern int
integration_open_login_to_caps_or_skip (const hx_test_server *srv,
                                        struct htlc_conn *htlc,
                                        const char *display_name,
                                        guint16 icon, guint16 caps);

/*
 * Send HTLC_HDR_GET_CHAT_HISTORY (TRAN 700) for `channel_id`
 * with the same "0 means omit" cursor / limit semantics as
 * src/chat_history.c::hx_get_chat_history. Returns the trans
 * id assigned by hlpack (or 0 on send failure) — the caller
 * filters the TASK reply by matching trans against this value.
 *
 * The harness does NOT cap-gate this call: tests can issue it
 * even against a server that didn't echo CAP_CHAT_HISTORY, in
 * order to verify the server's task-error response. Production
 * gtkhx gates the send (src/chat_history.c:137) and won't
 * exercise that path.
 */
extern guint32 integration_send_get_chat_history (int fd,
                                                  struct htlc_conn *htlc,
                                                  guint32 channel_id,
                                                  guint64 before, guint64 after,
                                                  guint16 limit);

/*
 * Send HTLC_HDR_CHAT with HTLC_DATA_STYLE=1 + HTLC_DATA_CHAT=text.
 * The 2-chunk shape every chat-using test exercises:
 *
 *   test_chat_roundtrip      one chat, expect the broadcast echo
 *   test_two_client_chat     A sends, B receives
 *   test_chat_in_pchat       same but with a HTLC_DATA_CHAT_ID chunk
 *                            in the same call site shape — that test
 *                            uses integration_send_message directly
 *                            (cid is per-chat). This primitive is
 *                            for the public-chat case.
 *
 * Returns TRUE on a full send.
 */
extern gboolean integration_send_chat (int fd, struct htlc_conn *htlc,
                                       const char *text);

/*
 * Send HTLC_HDR_PING and return the trans id that hlpack will write
 * into the header (the value of htlc->trans at call time — hlpack
 * increments after stamping). Returns 0 on send failure.
 *
 * Several tests use PING as a "stream still healthy?" probe after
 * driving an opcode that the server might silently drop (e.g.
 * unauthorized mkdir, msg to unknown uid). Caller matches the
 * returned trans against the TASK reply via
 * integration_drain_until_task_trans.
 */
extern guint32 integration_send_ping (int fd, struct htlc_conn *htlc);

/*
 * Create a private chat naming `target_uid` and read back the
 * server's chat_id. Steps:
 *
 *   1. Send HTLC_HDR_CHAT_CREATE with HTLC_DATA_UID = target_uid.
 *   2. integration_drain_until_task_trans on the just-sent trans.
 *   3. Walk the TASK reply via dh_start for HTLS_DATA_CHAT_ID and
 *      extract the chat_id into *chat_id_out.
 *
 * Returns TRUE on full success (got the TASK reply AND it carried a
 * non-zero chat_id). On FALSE the test should fail; on TRUE
 * *chat_id_out holds the chat the caller can now invite, join, set
 * subject, send to, etc. against.
 *
 * The 6 Tier-3 tests that pre-refactor opened-coded this dance:
 *   test_chat_create, test_chat_decline, test_chat_in_pchat,
 *   test_chat_join, test_chat_part, test_chat_subject.
 *
 * `max_messages` bounds the drain. 64 is the value those tests
 * have settled on after parallel-test-suite tuning.
 */
extern gboolean integration_create_chat_with_uid (int fd,
                                                  struct htlc_conn *htlc,
                                                  guint16 target_uid,
                                                  guint32 *chat_id_out,
                                                  int max_messages);

/*
 * Encode a single-component HTLC_DATA_DIR chunk into `out` and
 * return the byte count written. Layout:
 *
 *   u16 component_count (BE) = 1
 *   u8  unknown (always 0)
 *   u16 name_len (BE)
 *   bytes[name_len] name
 *
 * The full production encoder (src/path_hldir.c::path_to_hldir)
 * handles multi-component paths via a runtime-configured directory
 * separator (`dir_char`), set from the server's DIRECTORYCHAR
 * handshake. Tier 3 tests don't need the splitting — they target
 * top-level files / folders — so this single-component shortcut
 * stays here in the harness rather than depending on the global
 * `dir_char` extern.
 *
 * `out` must point to at least 5 + strlen(name) bytes.
 */
extern gsize integration_encode_hldir_one (guint8 *out, const char *name);

struct hx_chat_msg;

/*
 * Drain server messages on `fd` until we see an HTLS_HDR_CHAT
 * broadcast whose uid matches `wanted_uid`. On success returns
 * TRUE and fills `out` via hx_chat_extract; on timeout / overflow
 * of `max_messages` returns FALSE.
 *
 * The uid filter is load-bearing: meson runs Tier 3 binaries in
 * parallel, so chat broadcasts from concurrent test processes
 * (logged in under different names) hit our connection too and
 * would otherwise be the first HTLS_HDR_CHAT we see. Filtering
 * by uid scopes the drain to OUR own session.
 *
 * Pre-refactor each chat-using test had its own copy of this
 * function (drain_until_own_chat in test_chat_roundtrip,
 * drain_until_chat_from_uid in test_two_client_chat) — byte-
 * identical except for the function name. Centralised here so
 * future tweaks (e.g. timeout policy, broadcast filter rules)
 * land once.
 */
extern gboolean integration_drain_until_chat (int fd, struct htlc_conn *htlc,
                                              guint16 wanted_uid,
                                              struct hx_chat_msg *out,
                                              int max_messages);

/*
 * Drain server messages on `fd` until we see an HTLS_HDR_TASK whose
 * trans field matches `wanted_trans`. The matched message lives in
 * htlc->in afterwards; caller can read hdr_flag (error bit) and walk
 * dh_start for any reply chunks. Returns TRUE on match, FALSE on
 * timeout / overflow of max_messages.
 *
 * This was the second-most-duplicated drain pattern across Tier 3
 * (after the chat broadcast filter — see integration_drain_until_chat).
 * 10+ tests open-coded the same "for (i ... max) recv; type? trans?"
 * loop. Centralised here so future tweaks (longer timeout, opcode
 * dispatch table) land once.
 */
extern gboolean integration_drain_until_task_trans (int fd,
                                                    struct htlc_conn *htlc,
                                                    guint32 wanted_trans,
                                                    int max_messages);

/* ---- HOPE-Secure-Login + ChaCha20-Poly1305 ----------------------
 *
 * The harness's HOPE flow uses the same pure helpers production
 * uses (src/hope.c + src/cipher_aead.c). The only test-only piece
 * is the synchronous driver below: it talks Step 1 → server reply →
 * Step 2 against a real connection, and on success swaps the
 * harness's send/recv path over to AEAD framing for the remainder
 * of the session.
 *
 * The integration_hope_session struct holds the per-connection AEAD
 * state. It lives alongside (not on) struct htlc_conn because the
 * production htlc_conn carries this state via cipher_state.chacha
 * but the harness wants it independent so tests don't have to drag
 * in the full cipher_state union.
 */

#ifdef CONFIG_CIPHER
#include "cipher.h" /* chacha_aead_state */
typedef struct {
    /* AEAD framing state. Active only after a successful
     * HOPE-ChaCha20 negotiation. */
    int aead_active;
    chacha_aead_state encode_state;  /* client → server */
    chacha_aead_state decode_state;  /* server → client */

    /* Decode-side accumulator for the next inbound frame: AEAD frames
     * are length-prefixed and arrive in chunks of arbitrary boundary
     * over TCP. We accumulate until cipher_aead_peek_frame_size says
     * we have a full frame, then call cipher_aead_open. */
    guint8 *rx_accum;
    gsize rx_accum_len;
    gsize rx_accum_cap;
} integration_hope_session;
#else
typedef struct {
    int aead_active; /* always 0; AEAD disabled at build time. */
} integration_hope_session;
#endif

/*
 * Run the full HOPE-Secure-Login handshake against `srv`:
 *
 *   1. Open TCP + magic handshake.
 *   2. Send HOPE Step 1 LOGIN (algorithm negotiation, empty creds).
 *   3. Drain to the TASK reply; parse via hope_parse_step1_reply.
 *   4. Compute HMAC chain via hope_compute_chain.
 *   5. Send Step 2 LOGIN with login HMAC + password MAC + cipher /
 *      compress confirmations + display_name + capabilities.
 *   6. If the negotiated cipher is CHACHA20-POLY1305, derive AEAD
 *      session keys and arm hope->aead_active.
 *   7. Drain to SELFINFO or task-error.
 *
 * On success returns the fd and leaves htlc + hope ready for AEAD-
 * aware send/recv (when aead_active is set). On failure calls
 * g_test_skip or g_test_fail_printf with diagnostics and returns -1.
 *
 * `password` is the cleartext password (raw bytes — not pre-hashed);
 * the harness drives the HMAC chain. Pass "" for accounts with no
 * password (Janus's `guest` is set up that way for these tests).
 *
 * `cipheralg` and `compressalg` are advertised by name (e.g.
 * "CHACHA20-POLY1305", "ZSTD", or NULL for none). Server picks
 * whether to honour them.
 */
extern int integration_open_login_hope_or_skip (
    const hx_test_server *srv,
    struct htlc_conn *htlc,
    integration_hope_session *hope,
    const char *username, const char *password,
    const char *display_name, guint16 icon,
    const char *cipheralg, const char *compressalg);

/*
 * Release any malloc'd state inside `hope`. Safe to call on a
 * zeroed struct.
 */
extern void integration_hope_session_release (integration_hope_session *hope);

/*
 * AEAD-aware send. If hope->aead_active, frames the message through
 * cipher_aead_seal and writes the framed bytes; otherwise plain
 * passthrough to integration_send. Same hlpack-driven message
 * assembly as integration_send_message.
 */
extern gboolean integration_send_message_hope (int fd,
                                               struct htlc_conn *htlc,
                                               integration_hope_session *hope,
                                               guint32 type, guint32 flag,
                                               int hc, ...);

/*
 * AEAD-aware recv. If hope->aead_active, accumulates bytes into
 * hope->rx_accum until a full AEAD frame is buffered, then opens it
 * and copies the plaintext into htlc->in. Otherwise passthrough to
 * integration_recv_message.
 */
extern gboolean integration_recv_message_hope (int fd,
                                               struct htlc_conn *htlc,
                                               integration_hope_session *hope,
                                               int timeout_ms);

/* ---- HTXF subchannel helpers ---------------------------------- */

/*
 * Open a TCP connection to the file-transfer subchannel. mhxd
 * binds this on the main port + 1 (5501 by default), per
 * mhxd/src/hxd/files.c rcv_file_get's
 *     htxf->listen_sockaddr.SIN_PORT =
 *         htons(ntohs(...) + 1);
 *
 * Honors GTKHX_TEST_HOST; the subchannel port comes from
 * GTKHX_TEST_PORT + 1 (or GTKHX_TEST_XFER_PORT if set explicitly).
 *
 * Returns the fd on success, or -1 if the subchannel can't be
 * reached. The caller closes via integration_close.
 */
extern int integration_connect_xfer (void);

/*
 * Send the 16-byte HTXF transfer header that initiates a file
 * transfer subchannel exchange:
 *
 *   guint32 magic    = htonl(0x48545846)   // "HTXF"
 *   guint32 ref      = htonl(ref_from_TASK_reply)
 *   guint32 size     = htonl(total_size_from_TASK_reply)
 *   guint32 unknown  = 0
 *
 * After this header, the server (for downloads) starts streaming
 * the flat-file payload, or (for uploads) waits for the client
 * to stream it.
 *
 * Returns TRUE on a clean send.
 */
extern gboolean integration_send_xfer_hdr (int fd, guint32 ref,
                                           guint32 total_size);

#endif /* HX_INTEGRATION_HARNESS_H */
