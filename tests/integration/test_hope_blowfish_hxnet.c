/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_hope_blowfish_hxnet.c — Tier 3 reproducer
 * for the R3.3.e-4g HopeBlowfishStream-against-Janus bug.
 *
 * Misha caught this against the live Janus container at
 * hotline.vespernet.net (and the local janus container the
 * other Tier 3 tests use): once the production install hands a
 * Blowfish session to hxnet, the first server frame that trips
 * the legacy HOPE rekey marker (~3/16 of incoming frames)
 * surfaces upstream with the marker un-stripped — the dispatcher
 * sees `type=0x26000062` instead of `0x000062`. Janus drops the
 * connection because the bridge can't dispatch the (mangled)
 * opcode.
 *
 * The Tier 1 round-trip + read-pattern unit tests in
 * `hxnet::hope_blowfish::tests` exercise the strip in isolation
 * with hand-crafted bytes and pass clean. So the bug lives in
 * the integration: state captured at install time, or the way
 * the state machine interacts with TCP-flavoured reads (vs.
 * tokio::io::duplex), or the C bridge's marshalling.
 *
 * This binary closes the Tier 3 coverage gap that let R3.3.e-4g
 * Part 2 land without catching the bug. Strategy:
 *
 *   1. Open a HOPE-Blowfish login against Janus via the existing
 *      harness — same code path as production HOPE up through
 *      SELFINFO.
 *   2. Capture the live cipher state from htlc (keys, ivecs, num,
 *      session key, mac alg) into an `hxnet_transform_config_t`.
 *      Same marshalling production does in
 *      `hx_bridge_install_with_hope_state`, hand-rolled here so
 *      the test doesn't drag in production network.c.
 *   3. Spawn an hxnet connection directly via
 *      `hxnet_connection_spawn_fd_with_transforms_and_callback`
 *      with a callback that records every incoming frame's type
 *      (`type` after HopeBlowfishStream's strip).
 *   4. Send `BURST` HTLC_HDR_PING frames through hxnet so Janus
 *      has reason to keep replying. PING is the smallest legal
 *      opcode that always elicits a TASK reply, so we get
 *      `BURST` round-trips of incoming server frames in addition
 *      to whatever post-login pushes Janus volunteers.
 *   5. Pump the GLib main loop until the callback has recorded
 *      ENOUGH_FRAMES total received frames or a deadline expires.
 *   6. Assert every recorded type's high byte is 0. With 3/16
 *      marker probability and a few dozen frames, the statistical
 *      odds of NOT tripping the marker are vanishingly small;
 *      one un-stripped type is a hard fail.
 *
 * Fails (NOT skips) on a missing matrix entry / unreachable
 * server, per the "no silent skips" memory: a missing Tier 3
 * fixture should surface as a failing test in CI.
 */

#include "config.h"

#include <stdint.h>             /* uint32_t / uint8_t / uint64_t — used in the
                                 * hand-rolled HxnetTransformConfig mirror
                                 * below; the build was relying on incidental
                                 * transitive includes for these typedefs. */
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "cipher.h"
#include "cipher_aead.h"
#include "integration_harness.h"
#include "server_matrix.h"

/* Reference declarations for the hxnet FFI — same shape the C
 * smoke test in tests/unit/test_hxnet_ffi.c uses. Pins us to
 * the production ABI (any drift trips a const-assert in
 * ffi.rs). */

/* Cipher kinds — mirror HXNET_CIPHER_* in
 * rust/crates/hxnet/src/ffi.rs. */
#define HXNET_CIPHER_NONE              0
#define HXNET_CIPHER_BLOWFISH          1
#define HXNET_CIPHER_CHACHA20_POLY1305 2
#define HXNET_CIPHER_HOPE_BLOWFISH     3

/* Compression kinds. */
#define HXNET_COMPRESSION_NONE 0

/* HMAC algorithm tags. */
#define HXNET_MACALG_SHA256 0
#define HXNET_MACALG_SHA1   1
#define HXNET_MACALG_MD5    2

typedef struct {
    /* uint32_t (not `unsigned int`) for portability — matches
     * the discipline in tests/unit/test_hxnet_ffi.c. */
    uint32_t cipher_kind;
    uint32_t compression_kind;
    uint32_t blowfish_read_key_len;
    uint32_t blowfish_write_key_len;
    uint8_t      blowfish_read_key[56];
    uint8_t      blowfish_write_key[56];
    uint8_t      blowfish_read_ivec[8];
    uint8_t      blowfish_write_ivec[8];
    uint8_t      blowfish_read_num;
    uint8_t      blowfish_write_num;
    uint8_t      hope_macalg;
    uint8_t      _pad_macalg;
    uint32_t     hope_session_key_len;
    uint8_t      hope_session_key[64];
    uint8_t      aead_read_key[32];
    uint8_t      aead_write_key[32];
    uint64_t     aead_read_counter;
    uint64_t     aead_write_counter;
    uint8_t      aead_read_dir;
    uint8_t      aead_write_dir;
    uint8_t      _pad[6];
} hxnet_transform_config;

/* The hxnet frame the callback sees. Body memory belongs to the
 * callback only for the duration of the call; we copy fields out
 * synchronously. */
typedef struct hxnet_frame {
    uint32_t type_;
    uint32_t trans;
    uint32_t flag;
    uint16_t hc;
    uint8_t  _pad[2];
    uint32_t body_len;
    const uint8_t *body_ptr;
} hxnet_frame;

typedef struct hxnet_connection hxnet_connection;
typedef void (*hxnet_event_cb)(hxnet_connection *conn,
                               hxnet_frame *frame, void *user_data);
typedef void (*hxnet_shutdown_cb)(hxnet_connection *conn, int reason,
                                  void *user_data);

extern hxnet_connection *
hxnet_connection_spawn_fd_with_transforms_and_callback (
    int fd, const hxnet_transform_config *config,
    hxnet_event_cb on_event, hxnet_shutdown_cb on_shutdown,
    void *user_data);
extern int hxnet_connection_send_frame (hxnet_connection *handle,
                                        const uint8_t *data, uint32_t len);
extern void hxnet_connection_destroy (hxnet_connection *handle);
extern void hxnet_frame_free (hxnet_frame *f);

/* Rust helper for capturing the live OFB state into our config. */
extern void
gtkhx_blowfish_ofb64_save_state (const void *state, uint8_t *out_ivec,
                                 uint32_t *out_num);

/* ---- Observer state -------------------------------------------- */

#define ENOUGH_FRAMES 30
#define BURST         3
#define DEADLINE_MS   15000

/* Bigger post-login burst tier. Mirrors the production failure
 * Misha caught against VesperNet's Janus: AGREEMENTAGREE +
 * USER_CHANGE + USER_GETLIST + GET_CHAT_HISTORY + FILE_LIST +
 * NEWSDIRLIST + DOWNLOAD_BANNER fired in close succession.
 * Production sent ~8 frames; we send `POST_LOGIN_BURST` so the
 * test surfaces volume-dependent desyncs (cipher-state-vs-wire
 * drift after many marker-rotating writes).
 *
 * The actor's silent exit on the production crash showed up
 * as `hx_bridge_send_frame returned -2 (HXNET_SEND_CLOSED)` on
 * the next C-side hlwrite; the test asserts the actor stays
 * alive (no shutdown event with a non-clean reason). */
#define POST_LOGIN_BURST 30

typedef struct {
    GMutex     lock;
    GCond      cond;
    guint32    received_types[256];
    guint      received_count;
    int        shutdown_seen;
    int        shutdown_reason;
} observer_state;

static void
observer_init (observer_state *obs)
{
    memset (obs, 0, sizeof (*obs));
    g_mutex_init (&obs->lock);
    g_cond_init (&obs->cond);
}

static void
observer_clear (observer_state *obs)
{
    g_mutex_clear (&obs->lock);
    g_cond_clear (&obs->cond);
}

static void
on_event_cb (hxnet_connection *conn G_GNUC_UNUSED, hxnet_frame *frame,
             void *user_data)
{
    observer_state *obs = user_data;
    g_mutex_lock (&obs->lock);
    if (obs->received_count
        < G_N_ELEMENTS (obs->received_types)) {
        obs->received_types[obs->received_count++] = frame->type_;
    }
    g_cond_broadcast (&obs->cond);
    g_mutex_unlock (&obs->lock);
    hxnet_frame_free (frame);
}

static void
on_shutdown_cb (hxnet_connection *conn G_GNUC_UNUSED, int reason,
                void *user_data)
{
    observer_state *obs = user_data;
    g_mutex_lock (&obs->lock);
    obs->shutdown_seen = 1;
    obs->shutdown_reason = reason;
    g_cond_broadcast (&obs->cond);
    g_mutex_unlock (&obs->lock);
}

/* ---- Test body ------------------------------------------------- */

/* Pack a 22-byte Hotline header into `dst`. Same field order as
 * struct hl_hdr in src/hotline.h, all big-endian. */
static void
pack_header (uint8_t *dst, guint32 type, guint32 trans, guint32 flag,
             guint16 hc, guint32 body_len)
{
    guint32 wire_len = body_len + (guint32) sizeof (guint16);
    dst[0]  = (uint8_t) ((type  >> 24) & 0xff);
    dst[1]  = (uint8_t) ((type  >> 16) & 0xff);
    dst[2]  = (uint8_t) ((type  >>  8) & 0xff);
    dst[3]  = (uint8_t) ( type         & 0xff);
    dst[4]  = (uint8_t) ((trans >> 24) & 0xff);
    dst[5]  = (uint8_t) ((trans >> 16) & 0xff);
    dst[6]  = (uint8_t) ((trans >>  8) & 0xff);
    dst[7]  = (uint8_t) ( trans        & 0xff);
    dst[8]  = (uint8_t) ((flag  >> 24) & 0xff);
    dst[9]  = (uint8_t) ((flag  >> 16) & 0xff);
    dst[10] = (uint8_t) ((flag  >>  8) & 0xff);
    dst[11] = (uint8_t) ( flag         & 0xff);
    dst[12] = (uint8_t) ((wire_len >> 24) & 0xff);
    dst[13] = (uint8_t) ((wire_len >> 16) & 0xff);
    dst[14] = (uint8_t) ((wire_len >>  8) & 0xff);
    dst[15] = (uint8_t) ( wire_len        & 0xff);
    dst[16] = 0; dst[17] = 0; dst[18] = 0; dst[19] = 0;
    dst[20] = (uint8_t) ((hc >> 8) & 0xff);
    dst[21] = (uint8_t) ( hc       & 0xff);
}

/* Build the hxnet transform config from `htlc`'s live state.
 * Same marshalling production does in
 * hx_bridge_install_with_hope_state; replicated here to keep the
 * test linked-surface small and to flag any drift between the
 * two implementations as a test failure. */
static gboolean
build_config (const struct htlc_conn *htlc, hxnet_transform_config *out)
{
    memset (out, 0, sizeof (*out));
    out->cipher_kind      = HXNET_CIPHER_HOPE_BLOWFISH;
    out->compression_kind = HXNET_COMPRESSION_NONE;

    if (htlc->cipher_encode_keylen == 0
        || htlc->cipher_encode_keylen > sizeof (out->blowfish_write_key)
        || htlc->cipher_decode_keylen == 0
        || htlc->cipher_decode_keylen > sizeof (out->blowfish_read_key)) {
        g_test_message ("invalid blowfish key lengths (encode=%u decode=%u)",
                        (unsigned) htlc->cipher_encode_keylen,
                        (unsigned) htlc->cipher_decode_keylen);
        return FALSE;
    }
    if (htlc->cipher_encode_state.stream == NULL
        || htlc->cipher_decode_state.stream == NULL) {
        g_test_message ("blowfish state not initialised");
        return FALSE;
    }
    out->blowfish_write_key_len = htlc->cipher_encode_keylen;
    memcpy (out->blowfish_write_key, htlc->cipher_encode_key,
            htlc->cipher_encode_keylen);
    out->blowfish_read_key_len = htlc->cipher_decode_keylen;
    memcpy (out->blowfish_read_key, htlc->cipher_decode_key,
            htlc->cipher_decode_keylen);

    guint32 write_num = 0, read_num = 0;
    gtkhx_blowfish_ofb64_save_state (htlc->cipher_encode_state.stream,
                                     out->blowfish_write_ivec, &write_num);
    gtkhx_blowfish_ofb64_save_state (htlc->cipher_decode_state.stream,
                                     out->blowfish_read_ivec, &read_num);
    out->blowfish_write_num = (uint8_t) (write_num & 7);
    out->blowfish_read_num  = (uint8_t) (read_num  & 7);

    if (strcmp (htlc->macalg, "HMAC-SHA256") == 0) {
        out->hope_macalg = HXNET_MACALG_SHA256;
    } else if (strcmp (htlc->macalg, "HMAC-SHA1") == 0) {
        out->hope_macalg = HXNET_MACALG_SHA1;
    } else if (strcmp (htlc->macalg, "HMAC-MD5") == 0) {
        out->hope_macalg = HXNET_MACALG_MD5;
    } else {
        g_test_message ("unsupported macalg \"%s\"", htlc->macalg);
        return FALSE;
    }
    if (htlc->sklen == 0 || htlc->sklen > sizeof (out->hope_session_key)) {
        g_test_message ("invalid sklen %u", (unsigned) htlc->sklen);
        return FALSE;
    }
    out->hope_session_key_len = htlc->sklen;
    memcpy (out->hope_session_key, htlc->sessionkey, htlc->sklen);

    return TRUE;
}

static void
test_marker_strip_against_janus (void)
{
    /* Pick a Blowfish-capable matrix entry. Janus advertises
     * HX_TEST_CAP_BLOWFISH; mhxd does too but we want Janus
     * specifically — that's where Misha's reproduction lives,
     * and mhxd's stream-cipher marker rate may differ enough
     * to not trip the bug. */
    const hx_test_server *picked = NULL;
    GPtrArray *cand = hx_test_servers_with (HX_TEST_CAP_HOPE
                                            | HX_TEST_CAP_BLOWFISH);
    if (cand) {
        for (guint i = 0; i < cand->len; i++) {
            const hx_test_server *s = g_ptr_array_index (cand, i);
            if (g_strcmp0 (s->name, "janus") == 0) {
                picked = s;
                break;
            }
        }
        g_ptr_array_unref (cand);
    }
    if (!picked) {
        g_test_fail_printf (
            "no Janus matrix entry with HOPE+BLOWFISH; run with "
            "GTKHX_TEST_SERVERS=janus or bring up the Janus "
            "container (tests/janus/Dockerfile).");
        return;
    }

    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof (htlc));
    integration_hope_session hope;
    memset (&hope, 0, sizeof (hope));

    int fd = integration_open_login_hope_or_skip (
        picked, &htlc, &hope, "guest", "",
        "hope-bf-tier3", 412, "BLOWFISH", NULL);
    if (fd < 0) {
        /* integration_open_login_hope_or_skip already called
         * g_test_fail_printf with the specific failure. */
        return;
    }

    /* Janus rate-limits aggressively and won't stream post-
     * login pushes until it sees AGREEMENTAGREE — without it
     * the server treats us as "not fully joined" and ignores
     * most client requests (including PING). Send AGREEMENTAGREE
     * through the harness (still on the C-side cipher path) so
     * the cipher state advances normally and Janus opens up the
     * push stream. We don't drain the agreement-triggered pushes
     * here — we want hxnet to catch them right after install,
     * which gives the marker more frames to fire on. */
    if (!integration_send_agreementagree_hope (fd, &htlc, &hope,
                                                "hope-bf-tier3", 412)) {
        g_test_fail_printf ("AGREEMENTAGREE send failed");
        integration_release_htlc (&htlc);
        integration_hope_session_release (&hope);
        integration_close (fd);
        return;
    }
    /* We only run this against a server that actually negotiated
     * stream-cipher Blowfish. AEAD would route through a
     * different codepath that this test doesn't exercise. */
    if (!hope.stream_active) {
        g_test_fail_printf (
            "server negotiated something other than stream Blowfish "
            "(aead_active=%d) — this test specifically targets the "
            "stream-cipher install path.",
            hope.aead_active);
        integration_release_htlc (&htlc);
        integration_hope_session_release (&hope);
        integration_close (fd);
        return;
    }

    /* Diagnostic: does htlc->in have unread bytes from the
     * harness's last read? The harness's TCP receive may have
     * pulled more bytes off the socket than it consumed for the
     * drained SELFINFO frame; if so, those bytes were decrypted
     * by cipher_decode but stayed in htlc->in. The cipher state
     * has been advanced past them, but hxnet would start
     * reading at the next undecrypted socket byte — meaning
     * the state-vs-socket-position invariant is broken. */
    g_test_message ("htlc->in after SELFINFO drain: pos=%u len=%u",
                    (unsigned) htlc.in.pos, (unsigned) htlc.in.len);

    /* Drain the leftover via the harness — keep recv'ing while
     * partial bytes remain buffered in htlc->in. Each successful
     * recv consumes one frame, advancing rcv past whatever Janus
     * had queued. Once pos==0, the rcv state machine has fully
     * dispatched everything it had and the cipher state lines up
     * with the next undelivered wire byte.
     *
     * Note: drive the loop off `htlc.in.pos != 0`, not
     * `htlc.in.len > 0`. `len` is the *bytes-still-needed*
     * counter that the rcv state machine re-arms to SIZEOF_HL_HDR
     * after every fully-consumed frame, so `len > 0` is true
     * even when nothing is buffered — the loop would either spin
     * until timeout on every successful recv or stop falsely
     * on a peer that took its time. We also break out if the
     * peer simply has nothing more to send. */
    int drained_extra = 0;
    while (htlc.in.pos != 0 && drained_extra < 20) {
        if (!integration_recv_message_hope (fd, &htlc, &hope,
                                            /*timeout_ms=*/200)) {
            break;
        }
        drained_extra++;
    }
    g_test_message ("drained %d extra frames; htlc->in now pos=%u len=%u",
                    drained_extra, (unsigned) htlc.in.pos,
                    (unsigned) htlc.in.len);

    /* Capture live cipher state into the FFI config. */
    hxnet_transform_config cfg;
    if (!build_config (&htlc, &cfg)) {
        g_test_fail_printf ("build_config failed");
        integration_release_htlc (&htlc);
        integration_hope_session_release (&hope);
        integration_close (fd);
        return;
    }

    /* Dup the fd so the existing harness `fd` can be closed
     * separately and hxnet owns its own kernel reference. Same
     * pattern production uses in install_check_idle. */
    int dup_fd = dup (fd);
    g_assert_cmpint (dup_fd, >=, 0);

    observer_state obs;
    observer_init (&obs);

    hxnet_connection *handle =
        hxnet_connection_spawn_fd_with_transforms_and_callback (
            dup_fd, &cfg, on_event_cb, on_shutdown_cb, &obs);
    g_assert_nonnull (handle);

    /* Send a sustained burst through hxnet to give Janus reasons
     * to reply AND drive a write-side rekey-marker churn. Each
     * outgoing frame has independent ~3/16 probability of firing
     * a write marker, so a long burst exercises the cipher-state
     * rotation on both sides repeatedly. Bug surface this is
     * trying to catch: Misha's VesperNet repro showed the actor
     * dying silently after AGREEMENTAGREE + a 7-frame post-login
     * burst (USER_CHANGE / USER_GETLIST / GET_CHAT_HISTORY /
     * FILE_LIST / NEWSDIRLIST / DOWNLOAD_BANNER), with the next
     * send returning -2 (HXNET_SEND_CLOSED). A volume burst here
     * makes the same desync — if there is one — surface as a
     * non-EOF shutdown callback that the assertion below
     * catches loudly, with the reason code in the failure
     * message so we know whether it's StreamError or
     * FrameTooLarge or something else. */
    g_usleep (200 * G_USEC_PER_SEC / 1000);
    g_main_context_iteration (NULL, FALSE);

    for (int i = 0; i < POST_LOGIN_BURST; i++) {
        uint8_t getlist_buf[SIZEOF_HL_HDR];
        pack_header (getlist_buf, HTLC_HDR_USER_GETLIST,
                     /*trans=*/200 + i, /*flag=*/0, /*hc=*/0,
                     /*body_len=*/0);
        int rc = hxnet_connection_send_frame (handle, getlist_buf,
                                              SIZEOF_HL_HDR);
        if (rc != 0) {
            g_test_message ("send_frame USER_GETLIST[%d] returned %d",
                            i, rc);
        }
        /* Tiny pause between sends so we don't max out the
         * channel queue and so the actor has a chance to write
         * each frame between commands — mirrors the cadence
         * production hits coming through hlwrite. */
        if ((i % 4) == 3) {
            g_usleep (10 * G_USEC_PER_SEC / 1000);
            g_main_context_iteration (NULL, FALSE);
        }
    }

    /* Pump the main loop until we've collected ENOUGH_FRAMES
     * frames or the deadline fires. Each callback runs on the
     * main thread (forward_to_main marshals from the tokio
     * runtime), so we just need to keep iterating. */
    gint64 deadline = g_get_monotonic_time ()
                    + (gint64) DEADLINE_MS * G_TIME_SPAN_MILLISECOND;
    while (TRUE) {
        g_main_context_iteration (NULL, FALSE);
        g_mutex_lock (&obs.lock);
        guint count = obs.received_count;
        int shutdown = obs.shutdown_seen;
        g_mutex_unlock (&obs.lock);
        if (count >= ENOUGH_FRAMES) {
            break;
        }
        if (shutdown) {
            break;
        }
        if (g_get_monotonic_time () > deadline) {
            break;
        }
        g_usleep (5 * G_USEC_PER_SEC / 1000); /* 5 ms */
    }

    /* Tear down the connection before we walk the recorded
     * types so a slow disconnect can't queue another event mid-
     * check. */
    hxnet_connection_destroy (handle);
    /* Give the shutdown callback a chance to fire and the
     * actor a chance to clean up its TcpStream. */
    for (int i = 0; i < 100; i++) {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (10 * G_USEC_PER_SEC / 1000);
    }

    /* The harness fd was duped; close ours too. */
    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);

    /* Now check what we recorded. Snapshot the relevant fields
     * under the lock into local variables, then we can do
     * everything else (diagnostic prints + asserts) without
     * holding the lock, and observer_clear only runs at the
     * very end after no more accesses to obs.lock. The previous
     * shape called observer_clear mid-function and then went
     * on to take obs.lock again for the diagnostic dump — UB
     * (the mutex had already been destroyed). */
    g_mutex_lock (&obs.lock);
    guint count = obs.received_count;
    int shutdown_seen = obs.shutdown_seen;
    int shutdown_reason = obs.shutdown_reason;
    guint32 recorded[G_N_ELEMENTS (obs.received_types)];
    memcpy (recorded, obs.received_types,
            obs.received_count * sizeof (recorded[0]));
    g_mutex_unlock (&obs.lock);

    /* If the actor exited BEFORE the destroy call, the reason
     * tells us how it died:
     *   HXNET_SHUTDOWN_EOF             (0) — peer closed cleanly
     *   HXNET_SHUTDOWN_STREAM_ERROR    (1) — IO error mid-stream
     *   HXNET_SHUTDOWN_FRAME_TOO_LARGE (2) — wire_len > 1 MiB
     *   HXNET_SHUTDOWN_HANDLE_DROPPED  (3) — we dropped the handle
     *
     * The user's bug surfaced as "actor exits silently mid-burst,
     * next hlwrite returns HXNET_SEND_CLOSED" — which is
     * shutdown_seen=1 with reason in {STREAM_ERROR,
     * FRAME_TOO_LARGE}. Catch that here with a clear failure
     * message before the marker-strip check runs (which would
     * give a less actionable error if the actor died for an
     * unrelated reason). Reason 3 / HANDLE_DROPPED is what we
     * EXPECT to see — it fires when our `hxnet_connection_destroy`
     * runs. Reason 0 / EOF is also legitimate if Janus closed us
     * cleanly. Anything else is the bug we're hunting. */
    if (shutdown_seen
        && shutdown_reason != /* HANDLE_DROPPED */ 3
        && shutdown_reason != /* EOF */ 0) {
        g_test_fail_printf (
            "actor exited mid-burst with shutdown_reason=%d "
            "(0=EOF, 1=STREAM_ERROR, 2=FRAME_TOO_LARGE, 3=HANDLE_DROPPED). "
            "Received %u frames before the actor died. This is the "
            "VesperNet/Blowfish desync bug — HopeBlowfishStream's read "
            "path decoded something the actor's frame parser then "
            "rejected. Capture hxnet logs (RUST_LOG=hxnet=debug) on the "
            "next run to see which byte triggered the StreamError / "
            "which wire_len triggered FrameTooLarge.",
            shutdown_reason, count);
        observer_clear (&obs);
        return;
    }
    g_assert_cmpuint (count, >, 0);
    int marker_seen = 0;
    guint32 first_bad_type = 0;
    for (guint i = 0; i < count; i++) {
        guint32 t = recorded[i];
        if ((t >> 24) != 0) {
            marker_seen = 1;
            if (first_bad_type == 0) {
                first_bad_type = t;
            }
        }
    }

    /* Diagnostic dump: print every recorded type + the
     * shutdown reason if the actor died. The shutdown reason
     * is the key signal — a clean `Reason::Eof` means Janus
     * closed us cleanly (probably because we sent something
     * it didn't like), while a stream-error reason means
     * hxnet itself failed to decode incoming bytes (the bug
     * we're hunting). */
    g_test_message ("recorded %u frames, shutdown_seen=%d reason=%d:",
                    count, shutdown_seen, shutdown_reason);
    for (guint i = 0; i < count; i++) {
        guint32 t = recorded[i];
        g_test_message ("  frame[%u] type=0x%08x high=0x%02x",
                        i, t, (t >> 24) & 0xff);
    }

    observer_clear (&obs);

    if (marker_seen) {
        g_test_fail_printf (
            "recorded %u incoming frames; at least one carried an "
            "un-stripped rekey marker (first bad type=0x%08x). The "
            "HopeBlowfishStream adapter or its install plumbing is "
            "not stripping the marker against live Janus traffic.",
            count, first_bad_type);
    } else if (count < 20) {
        /* Loud warning: with fewer than 20 frames, the
         * probability of NOT tripping the marker by chance is
         * ~(13/16)^20 = ~1.2%, which is too high to claim
         * coverage. Fail visibly so future runs can't pass
         * on a too-light burst. */
        g_test_fail_printf (
            "only %u frames received; not enough traffic to "
            "statistically exercise the rekey marker (need >=20). "
            "Increase BURST or extend DEADLINE_MS.",
            count);
    }
}

/* ---------------------------------------------------------------- *
 * Test 2 — post-login burst reproducer                              *
 * ---------------------------------------------------------------- *
 *
 * Mirrors Misha's exact failing flow against VesperNet's Janus:
 * AGREEMENTAGREE (through harness) then a tight burst of
 * USER_CHANGE / USER_GETLIST / GET_CHAT_HISTORY / FILE_LIST /
 * NEWSDIRLIST / DOWNLOAD_BANNER through hxnet. Production hit
 * `hope_blowfish: EOF mid-frame` mid-burst — the actor died with
 * `StreamError("hope_blowfish: EOF mid-frame")` and the next
 * hlwrite returned HXNET_SEND_CLOSED.
 *
 * This test fires the same opcode sequence so we can iterate on
 * the fix locally (against the Janus container) instead of needing
 * a live VesperNet repro every time. The shutdown-reason guard
 * (same shape as test 1) catches the actor's exit and surfaces
 * the new EOF-mid-frame state info that R3.3.e-4g feedback added
 * to HopeBlowfishStream's read path — Header pos=N / Body
 * remaining=N. */

/* The opcode sequence Misha's failing connection sent post-
 * AGREEMENTAGREE. trans/hc/body chosen to be wire-valid; the
 * server's response shape varies (USER_GETLIST → TASK with
 * USER_LIST chunk, FILE_LIST → TASK with file chunks, etc.) but
 * what we care about is the WRITE-side cipher state advancing
 * through enough marker rotations that the read-side desync (if
 * any) surfaces. */
struct burst_op {
    guint32 type;
    guint16 hc;
    /* For ops with body chunks, populate body_buf at frame-build
     * time using pack_chunk(). Empty body keeps the test small
     * — what's load-bearing is the COUNT of writes and the
     * marker churn on the WRITE side, not the chunk content. */
};

static const struct burst_op POST_LOGIN_SEQUENCE[] = {
    /* USER_CHANGE  hc=3 — production sends icon+name+color
     *   chunks; we send body_len=0 to keep the test minimal.
     *   The cipher state advance is what matters here. */
    { HTLC_HDR_USER_CHANGE,       0 },
    { HTLC_HDR_USER_GETLIST,      0 },
    /* GET_CHAT_HISTORY would have channel_id + history_limit
     * chunks in production; empty body again. */
    { HTLC_HDR_GET_CHAT_HISTORY,  0 },
    { HTLC_HDR_FILE_LIST,         0 },
    { HTLC_HDR_NEWSDIRLIST,       0 },
    { HTLC_HDR_DOWNLOAD_BANNER,   0 },
};
#define POST_LOGIN_SEQUENCE_LEN G_N_ELEMENTS(POST_LOGIN_SEQUENCE)

/* How many TIMES we cycle through the post-login sequence. Each
 * cycle is ~6 frames; with REPS=8 we send ~48 frames, well past
 * the point where the production failure surfaced (~7-8 frames). */
#define POST_LOGIN_REPS 8

static void
test_post_login_burst_against_janus (void)
{
    /* Same setup boilerplate as test 1 up to the install — we
     * pay for the duplication so the two tests stay
     * independently-runnable (g_test_add_func lets you target
     * one or the other via gtester -p) and so an early failure
     * in test 1 doesn't poison the post-login-burst coverage. */
    const hx_test_server *picked = NULL;
    GPtrArray *cand = hx_test_servers_with (HX_TEST_CAP_HOPE
                                            | HX_TEST_CAP_BLOWFISH);
    if (cand) {
        for (guint i = 0; i < cand->len; i++) {
            const hx_test_server *s = g_ptr_array_index (cand, i);
            if (g_strcmp0 (s->name, "janus") == 0) {
                picked = s;
                break;
            }
        }
        g_ptr_array_unref (cand);
    }
    if (!picked) {
        g_test_fail_printf (
            "no Janus matrix entry with HOPE+BLOWFISH; run with "
            "GTKHX_TEST_SERVERS=janus or bring up the Janus "
            "container (tests/janus/Dockerfile).");
        return;
    }

    struct htlc_conn htlc;
    memset (&htlc, 0, sizeof (htlc));
    integration_hope_session hope;
    memset (&hope, 0, sizeof (hope));

    int fd = integration_open_login_hope_or_skip (
        picked, &htlc, &hope, "guest", "",
        "hope-bf-burst", 412, "BLOWFISH", NULL);
    if (fd < 0) {
        return;
    }

    if (!integration_send_agreementagree_hope (fd, &htlc, &hope,
                                                "hope-bf-burst", 412)) {
        g_test_fail_printf ("AGREEMENTAGREE send failed");
        integration_release_htlc (&htlc);
        integration_hope_session_release (&hope);
        integration_close (fd);
        return;
    }
    if (!hope.stream_active) {
        g_test_fail_printf (
            "server negotiated non-stream-Blowfish (aead_active=%d)",
            hope.aead_active);
        integration_release_htlc (&htlc);
        integration_hope_session_release (&hope);
        integration_close (fd);
        return;
    }

    /* Drain leftover buffered bytes before install (same fix as
     * test 1 — pos==0, not len==0). */
    int drained_extra = 0;
    while (htlc.in.pos != 0 && drained_extra < 20) {
        if (!integration_recv_message_hope (fd, &htlc, &hope,
                                            /*timeout_ms=*/200)) {
            break;
        }
        drained_extra++;
    }

    hxnet_transform_config cfg;
    if (!build_config (&htlc, &cfg)) {
        g_test_fail_printf ("build_config failed");
        integration_release_htlc (&htlc);
        integration_hope_session_release (&hope);
        integration_close (fd);
        return;
    }

    int dup_fd = dup (fd);
    g_assert_cmpint (dup_fd, >=, 0);

    observer_state obs;
    observer_init (&obs);

    hxnet_connection *handle =
        hxnet_connection_spawn_fd_with_transforms_and_callback (
            dup_fd, &cfg, on_event_cb, on_shutdown_cb, &obs);
    g_assert_nonnull (handle);

    /* Pump briefly so any post-install bytes the server pushes
     * before our burst start can get drained without being
     * confused with the actor's exit. */
    g_usleep (200 * G_USEC_PER_SEC / 1000);
    g_main_context_iteration (NULL, FALSE);

    /* Fire the post-login burst. POST_LOGIN_REPS cycles through
     * the production opcode sequence — ~48 frames total. The
     * production failure hit at ~7-8 frames, so even one cycle
     * is past the failure threshold; multiple cycles harden the
     * test against any "first time it works" flakiness. */
    guint32 trans = 200;
    int sent_count = 0;
    for (int rep = 0; rep < POST_LOGIN_REPS; rep++) {
        for (size_t i = 0; i < POST_LOGIN_SEQUENCE_LEN; i++) {
            uint8_t frame_buf[SIZEOF_HL_HDR];
            pack_header (frame_buf, POST_LOGIN_SEQUENCE[i].type,
                         trans++, /*flag=*/0,
                         POST_LOGIN_SEQUENCE[i].hc, /*body_len=*/0);
            int rc = hxnet_connection_send_frame (handle, frame_buf,
                                                  SIZEOF_HL_HDR);
            if (rc != 0) {
                g_test_message (
                    "send_frame [rep=%d op=%zu type=0x%06x trans=%u] rc=%d",
                    rep, i, POST_LOGIN_SEQUENCE[i].type, trans - 1, rc);
                break;
            }
            sent_count++;
        }
        /* Mid-cycle pause + main-loop tick so the actor has a
         * chance to ship each frame between commands —
         * cadence-matches what production hits coming through
         * hlwrite + the GLib idle loop. */
        g_usleep (10 * G_USEC_PER_SEC / 1000);
        g_main_context_iteration (NULL, FALSE);
    }
    g_test_message ("post-login burst sent %d frames", sent_count);

    /* Pump until the deadline OR until we observe an early
     * actor shutdown. We don't gate on a frame count because the
     * focus is whether the actor STAYS ALIVE through the burst —
     * the bug Misha hit was the actor dying silently. */
    gint64 deadline = g_get_monotonic_time ()
                    + (gint64) DEADLINE_MS * G_TIME_SPAN_MILLISECOND;
    while (TRUE) {
        g_main_context_iteration (NULL, FALSE);
        g_mutex_lock (&obs.lock);
        int shutdown = obs.shutdown_seen;
        g_mutex_unlock (&obs.lock);
        if (shutdown) {
            break;
        }
        if (g_get_monotonic_time () > deadline) {
            break;
        }
        g_usleep (5 * G_USEC_PER_SEC / 1000);
    }

    hxnet_connection_destroy (handle);
    for (int i = 0; i < 100; i++) {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (10 * G_USEC_PER_SEC / 1000);
    }

    integration_release_htlc (&htlc);
    integration_hope_session_release (&hope);
    integration_close (fd);

    g_mutex_lock (&obs.lock);
    guint count = obs.received_count;
    int shutdown_seen = obs.shutdown_seen;
    int shutdown_reason = obs.shutdown_reason;
    g_mutex_unlock (&obs.lock);

    g_test_message (
        "post-login burst test: sent=%d received=%u "
        "shutdown_seen=%d shutdown_reason=%d",
        sent_count, count, shutdown_seen, shutdown_reason);

    /* The bug we're hunting: actor exits with non-clean reason
     * before our destroy fires. Reason 3 (HANDLE_DROPPED) is our
     * destroy. Reason 0 (EOF) is also legitimate if Janus
     * closed us cleanly. Anything else is the desync. */
    if (shutdown_seen
        && shutdown_reason != /* HANDLE_DROPPED */ 3
        && shutdown_reason != /* EOF */ 0) {
        g_test_fail_printf (
            "actor exited mid-burst with shutdown_reason=%d "
            "(0=EOF, 1=STREAM_ERROR, 2=FRAME_TOO_LARGE, "
            "3=HANDLE_DROPPED). Sent %d frames before the actor "
            "died, received %u. The Rust eprintln in hxnet's "
            "Event::Shutdown handler logged the full ShutdownReason "
            "(including the io::Error string for StreamError) to "
            "stderr — look for the line `hxnet: actor shutting down: "
            "StreamError(\"hope_blowfish: ...\")` to see whether "
            "this is the post-login burst desync.",
            shutdown_reason, sent_count, count);
    }

    observer_clear (&obs);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/hope_blowfish_hxnet/marker_strip_against_janus",
                     test_marker_strip_against_janus);
    g_test_add_func ("/hope_blowfish_hxnet/post_login_burst_against_janus",
                     test_post_login_burst_against_janus);

    return g_test_run ();
}
