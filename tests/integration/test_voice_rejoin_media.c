/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_voice_rejoin_media.c — Tier 3 MEDIA-level
 * reproduction for the "second participant leaves and rejoins, first
 * participant stops receiving them" bug.
 *
 * Unlike the other voice Tier 3 tests (which exercise only the TCP
 * control channel with synthetic SDP), this one drives TWO real
 * `VoiceRuntime`s — webrtcbin + ICE/DTLS/SRTP against the live Janus
 * — and asserts on actual RTP flow.
 *
 * Scenario: A joins the empty room first, B joins second, then B
 * LEAVES and REJOINS; the test asserts A keeps receiving B's RTP
 * across the rejoin.
 *
 * Against a spec-compliant server (fogWraith Capabilities-Voice.md as
 * of the direction/mid-stability clarification) every participant —
 * including the first joiner — gets a dedicated per-user
 * `a=mid:user-<uid>` receive section; on leave that section flips to
 * `a=inactive` (port stays 9) and on rejoin it reactivates to
 * `a=sendonly`. The client tears the receive leg down on `a=inactive`
 * (webrtcbin fires no pad-removed there) and rebuilds it on
 * reactivation, so the rejoin is clean and A never stops hearing B.
 *
 * Historical note: the pre-fix Janus instead bundled every remote onto
 * the first joiner's single `mid=send` transceiver (flipped to
 * sendrecv), whose receive pad-add was a webrtcbin race — that's the
 * bug this test was originally written to reproduce. The receive bins
 * are still keyed by webrtcbin pad name (and torn down on pad-removed)
 * to stay correct against such servers.
 *
 * Signal: `gtkhx_voice_runtime_rtp_buffers_received(A.rt)` — the
 * count of RTP buffers A has received off its receive bin(s). It
 * advances (~50/s) while A is receiving B's PCMU (digital silence
 * counts — no mic needed; the send leg uses audiotestsrc via
 * GTKHX_VOICE_TEST_AUDIO_SRC). The test asserts the counter advances
 * BOTH after B's first join AND after B's rejoin. The second
 * assertion is the one that fails against the buggy receive-pad
 * lifecycle and passes once it's fixed.
 *
 * Driver model: everything that touches a `VoiceRuntime` runs on the
 * GLib main thread *while it owns the default context* — a real
 * `g_main_loop_run` with a `g_timeout`-driven state machine. That
 * mirrors production (all voice work on the GTK main thread) and is
 * what keeps webrtcbin's worker-thread callbacks, which marshal back
 * via `MainContext::invoke`, creating their `*_local` GLib sources on
 * the main thread rather than on a streaming thread.
 *
 * Server gating: HX_TEST_CAP_VOICE → Janus. is_parallel:false and a
 * generous timeout, matching the other voice tests (and because the
 * WebRTC handshakes take a couple of seconds each).
 *
 * NOTE: this test requires UDP reachability to Janus's voice port
 * (ICE/DTLS/RTP), not just the TCP control channel. It runs against
 * the developer's local Janus the same way the rest of the voice
 * Tier 3 suite does. Honors GTKHX_TEST_HOST / GTKHX_TEST_PORT.
 *
 * This is the regression guard for the receive-pad-lifecycle fix
 * (receive bins keyed by webrtcbin pad + a `pad-removed` teardown).
 * Against the pre-fix runtime it failed at the final assertion — A's
 * RTP counter advanced after B's first join but stayed flat after the
 * rejoin ("before=100 after=100"); with the fix it advances both
 * times ("before=100 after=200"). Keep the test and the fix together.
 */

#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "integration_harness.h"
#include "server_matrix.h"
#include "voice_runtime.h"

/* One driven client: its control connection + its VoiceRuntime. */
typedef struct {
    const char *label;
    int fd;
    struct htlc_conn htlc;
    gtkhx_voice_runtime *rt;
    /* trans of the in-flight VOICE_JOIN; its TASK reply carries the
     * server's SDP offer. 0 when no join is outstanding. */
    guint32 join_trans;
    gtkhx_voice_state state;
    /* uids this client's runtime has reported as actively speaking
     * (VAD), via the speaker-changed signal. Used by the speaker
     * attribution test. */
    GArray *spoke_uids; /* of guint16 */
    /* Send-track SSRC-declaration audit (fogWraith Capabilities-Voice
     * "Send SSRC Declaration"): every SDP answer this client's runtime
     * produces MUST declare the microphone stream's SSRC in the send
     * section (a=ssrc:<n> cname:<name>). Without it the server falls
     * back to payload-type matching, which is the exact failure that
     * dropped the first joiner's audio. We count answers seen vs
     * answers carrying the declaration; the test asserts they match. */
    int answers_seen;
    int answers_with_send_ssrc;
} voice_client;

/* Does the SDP answer `sdp` (len `len`) declare a send-track SSRC?
 * The answer's `send` media section must carry an `a=ssrc:<n>
 * cname:<name>` line before the next `m=` line. Returns TRUE iff so. */
static gboolean
answer_declares_send_ssrc (const char *sdp, size_t len)
{
    char *buf = g_strndup (sdp, len);
    gboolean ok = FALSE;
    char **lines = g_strsplit (buf, "\n", -1);
    gboolean in_send = FALSE;
    gboolean saw_ssrc = FALSE, saw_cname = FALSE;
    for (int i = 0; lines[i]; i++) {
        char *ln = g_strchomp (lines[i]);
        if (g_str_has_prefix (ln, "m=")) {
            /* Entering a new media section closes the send one. */
            if (in_send && saw_ssrc && saw_cname) {
                ok = TRUE;
                break;
            }
            in_send = FALSE;
            saw_ssrc = saw_cname = FALSE;
        } else if (g_strcmp0 (ln, "a=mid:send") == 0) {
            in_send = TRUE;
        } else if (in_send && g_str_has_prefix (ln, "a=ssrc:")) {
            saw_ssrc = TRUE;
            if (strstr (ln, "cname:")) {
                saw_cname = TRUE;
            }
        }
    }
    if (in_send && saw_ssrc && saw_cname) {
        ok = TRUE; /* send section ran to end of SDP */
    }
    g_strfreev (lines);
    g_free (buf);
    return ok;
}

/* Has this client's VAD ever reported `uid` speaking? */
static gboolean
client_saw_speaking (const voice_client *c, guint16 uid)
{
    if (!c->spoke_uids) {
        return FALSE;
    }
    for (guint i = 0; i < c->spoke_uids->len; i++) {
        if (g_array_index (c->spoke_uids, guint16, i) == uid) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
on_speaker_changed (void *user_data, uint16_t uid, int is_speaking)
{
    voice_client *c = user_data;
    if (is_speaking && c->spoke_uids && !client_saw_speaking (c, uid)) {
        guint16 u = uid;
        g_array_append_val (c->spoke_uids, u);
    }
}

/* ------------------------------------------------------------------ */
/* Wire-out: the runtime's SendWireFrame action → the control socket. */
/* ------------------------------------------------------------------ */

/* The runtime emits every outbound voice opcode through this bridge.
 * Body layout is always a 4-byte BE cid followed by the opcode's
 * payload (see hxvoice::state::encode_*). */
static void
on_send_wire_frame (void *user_data, uint32_t opcode, const uint8_t *body,
                    size_t body_len)
{
    voice_client *c = user_data;
    if (!c || body_len < 4) {
        return;
    }
    guint32 cid = ((guint32) body[0] << 24) | ((guint32) body[1] << 16)
                  | ((guint32) body[2] << 8) | (guint32) body[3];
    guint32 cid_be = htonl (cid);
    const guint8 *payload = body + 4;
    gsize plen = body_len - 4;
    guint32 trans = c->htlc.trans;

    switch (opcode) {
    case HTLC_HDR_VOICE_JOIN:
        if (integration_send_message (c->fd, &c->htlc, HTLC_HDR_VOICE_JOIN,
                                      0, 1, (int) HTLC_DATA_CHAT_ID, 4,
                                      &cid_be)) {
            c->join_trans = trans;
        }
        break;
    case HTLC_HDR_VOICE_LEAVE:
        integration_send_message (c->fd, &c->htlc, HTLC_HDR_VOICE_LEAVE, 0, 1,
                                  (int) HTLC_DATA_CHAT_ID, 4, &cid_be);
        break;
    case HTLC_HDR_VOICE_SDP_ANSWER:
        /* Audit the send-track SSRC declaration on the way past (spec
         * REQUIRED — see answer_declares_send_ssrc). */
        c->answers_seen++;
        if (answer_declares_send_ssrc ((const char *) payload, plen)) {
            c->answers_with_send_ssrc++;
        }
        integration_send_message (c->fd, &c->htlc, HTLC_HDR_VOICE_SDP_ANSWER,
                                  0, 2, (int) HTLC_DATA_CHAT_ID, 4, &cid_be,
                                  (int) HTLC_DATA_VOICE_SDP, (int) plen,
                                  (guint8 *) payload);
        break;
    case HTLC_HDR_VOICE_ICE:
        integration_send_message (c->fd, &c->htlc, HTLC_HDR_VOICE_ICE, 0, 2,
                                  (int) HTLC_DATA_CHAT_ID, 4, &cid_be,
                                  (int) HTLC_DATA_VOICE_ICE, (int) plen,
                                  (guint8 *) payload);
        break;
    case HTLC_HDR_VOICE_MUTE:
        integration_send_message (c->fd, &c->htlc, HTLC_HDR_VOICE_MUTE, 0, 2,
                                  (int) HTLC_DATA_CHAT_ID, 4, &cid_be,
                                  (int) HTLC_DATA_VOICE_MUTED, (int) plen,
                                  (guint8 *) payload);
        break;
    default:
        break;
    }
}

static void
on_state_changed (void *user_data, gtkhx_voice_state state)
{
    voice_client *c = user_data;
    c->state = state;
}

/* ------------------------------------------------------------------ */
/* Wire-in: dispatch a received control frame into the runtime.        */
/* ------------------------------------------------------------------ */

/* Parse the room id (HTLC_DATA_CHAT_ID) out of a voice reply/notify
 * so we forward it to the runtime the way production does
 * (rcv.c uses the parsed r.cid), instead of assuming the public-chat
 * id 0. */
static guint32
reply_cid (const struct htlc_conn *htlc)
{
    struct gtkhx_proto_voice_reply r;
    memset (&r, 0, sizeof (r));
    gtkhx_proto_parse_voice_reply (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, &r);
    return r.cid;
}

/* Choose the send-leg audio source.
 *
 * Default (headless / CI / sandbox): `audiotestsrc` via
 * GTKHX_VOICE_TEST_AUDIO_SRC, so a sender reliably produces RTP with no
 * capture device. But `audiotestsrc` is the ONLY element that differs
 * from production — `make_send_bin` uses `autoaudiosrc` when the test env
 * is absent, everything downstream (audioconvert/volume/resample/caps/
 * mulawenc/rtppcmupay) is identical. The receive pad-add on the bundled
 * `mid=send` transceiver is timing-sensitive to exactly the source's
 * async device-open + caps-negotiation behaviour, so the test source can
 * mask a bug the real client hits.
 *
 * Set GTKHX_VOICE_REAL_AUDIO=1 to drive the REAL production send leg
 * (`autoaudiosrc` → host default capture). Run that on a desktop with a
 * working mic or a PipeWire null-sink monitor set as the default source
 * (`pactl set-default-source <name>.monitor`) to reproduce the GUI's
 * send-leg timing. The CI sandbox can't — it has no /dev/snd — so this
 * mode is for a real host (e.g. the dev machine that runs the GUI). */
static void
harness_select_audio_source (const char *test_wave)
{
    if (g_getenv ("GTKHX_VOICE_REAL_AUDIO")) {
        /* make_source() falls through to autoaudiosrc when the test env
         * is absent — clear it so we exercise the production path. */
        g_unsetenv ("GTKHX_VOICE_TEST_AUDIO_SRC");
        g_test_message ("using REAL production audio source (autoaudiosrc)");
    } else {
        g_setenv ("GTKHX_VOICE_TEST_AUDIO_SRC", test_wave, TRUE);
    }
}

static void
feed_room_status (voice_client *c)
{
    const guint8 *blob = NULL;
    size_t blob_len = 0;
    if (gtkhx_proto_voice_reply_field (hx_test_in(&c->htlc)->buf, hx_test_in(&c->htlc)->pos,
                                       /*field=participants*/ 3, &blob,
                                       &blob_len)) {
        gtkhx_voice_runtime_room_status (c->rt, reply_cid (&c->htlc), blob,
                                         blob_len);
    }
}

static void
feed_sdp_offer (voice_client *c)
{
    const guint8 *sdp = NULL;
    size_t sdp_len = 0;
    if (gtkhx_proto_voice_reply_field (hx_test_in(&c->htlc)->buf, hx_test_in(&c->htlc)->pos,
                                       /*field=SDP*/ 0, &sdp, &sdp_len)
        && sdp_len > 0) {
        char *s = g_strndup ((const char *) sdp, sdp_len);
        gtkhx_voice_runtime_sdp_offer (c->rt, reply_cid (&c->htlc), s);
        g_free (s);
    }
}

static void
dispatch_frame (voice_client *c)
{
    guint32 type = hdr_type (&c->htlc);
    guint32 trans = hdr_trans (&c->htlc);

    if (type == HTLS_HDR_TASK && c->join_trans != 0
        && trans == c->join_trans) {
        /* JOIN reply — carries the server's initial SDP offer AND the
         * participants blob. Mirror rcv.c's hx_rcv_task join-reply path
         * exactly: feed room_status (ParticipantsUpdated) BEFORE the
         * offer, so the mid_to_user cache is populated before the answer
         * walk, same ordering as production. */
        c->join_trans = 0;
        feed_room_status (c);
        feed_sdp_offer (c);
        return;
    }
    if (type == HTLS_HDR_VOICE_SDP_OFFER) {
        /* Server-initiated renegotiation (peer joined / left). */
        feed_sdp_offer (c);
        return;
    }
    if (type == HTLS_HDR_VOICE_ICE) {
        const guint8 *ice = NULL;
        size_t ice_len = 0;
        if (gtkhx_proto_voice_reply_field (hx_test_in(&c->htlc)->buf, hx_test_in(&c->htlc)->pos,
                                           /*field=ICE*/ 1, &ice, &ice_len)) {
            /* Mirror production (rcv.c): forward the parsed cid, and
             * pass NULL for the zero-length end-of-candidates marker
             * rather than an empty allocated string. */
            if (ice_len == 0) {
                gtkhx_voice_runtime_ice_candidate (c->rt, reply_cid (&c->htlc), NULL);
            } else {
                char *json = g_strndup ((const char *) ice, ice_len);
                gtkhx_voice_runtime_ice_candidate (c->rt, reply_cid (&c->htlc), json);
                g_free (json);
            }
        }
        return;
    }
    if (type == HTLS_HDR_VOICE_ROOM_STATUS) {
        const guint8 *blob = NULL;
        size_t blob_len = 0;
        if (gtkhx_proto_voice_reply_field (hx_test_in(&c->htlc)->buf, hx_test_in(&c->htlc)->pos,
                                           /*field=participants*/ 3, &blob,
                                           &blob_len)) {
            gtkhx_voice_runtime_room_status (c->rt, reply_cid (&c->htlc), blob,
                                             blob_len);
        }
        return;
    }
}

/* Tell the server we're unmuted so it forwards our audio. Sent raw
 * (not through the runtime's mute machine) so the server-side flag is
 * unambiguous regardless of the runtime's local mute default. */
static void
wire_unmute (voice_client *c)
{
    guint32 cid_be = htonl (0);
    guint16 muted_be = htons (0);
    integration_send_message (c->fd, &c->htlc, HTLC_HDR_VOICE_MUTE, 0, 2,
                              (int) HTLC_DATA_CHAT_ID, 4, &cid_be,
                              (int) HTLC_DATA_VOICE_MUTED, 2, &muted_be);
}

/* Mirror the GUI's join sequence exactly (voice_panel.c on_join_toggled):
 * drive the runtime into JoinSent, then IMMEDIATELY mute the send leg via
 * the RUNTIME (gtkhx_voice_runtime_mute → SetSendPipelineMute, volume
 * mute=true) to honour "join muted by default". This runs while the
 * runtime is still in JoinSent, BEFORE the offer/answer negotiation — the
 * one C-side behaviour the media harness never exercised (it used to join
 * and only ever send a raw wire-unmute). The runtime's emitted 606 is
 * shipped by on_send_wire_frame, matching the wire the GUI sends by hand. */
static void
client_join_muted (voice_client *c, guint32 cid)
{
    gtkhx_voice_runtime_join (c->rt, cid);
    gtkhx_voice_runtime_mute (c->rt, 1);
}

/* Mirror the GUI unmute (voice_panel on_mute_toggled): drive the runtime
 * mute state to 0 — clears the send-leg volume mute and emits the 606
 * (sent by on_send_wire_frame), instead of the raw wire_unmute. */
static void
client_runtime_unmute (voice_client *c)
{
    gtkhx_voice_runtime_mute (c->rt, 0);
}

/* ------------------------------------------------------------------ */
/* Connection + runtime lifecycle.                                     */
/* ------------------------------------------------------------------ */

static const hx_test_server *
pick_voice_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_VOICE);
    if (!servers) {
        return NULL;
    }
    const hx_test_server *srv = servers->len > 0
                                    ? g_ptr_array_index (servers, 0)
                                    : NULL;
    g_ptr_array_unref (servers);
    if (!srv) {
        return NULL;
    }

    /* Honour VOICE-SPECIFIC overrides (GTKHX_VOICE_TEST_HOST /
     * GTKHX_VOICE_TEST_PORT) so this test can target a Janus on a
     * non-default host (e.g. a dev box as seen from a remote sandbox)
     * without disturbing the matrix entry's port.
     *
     * Deliberately NOT the generic GTKHX_TEST_HOST / GTKHX_TEST_PORT:
     * CI's integration job sets GTKHX_TEST_PORT=5500 for the mhxd-
     * targeted login tests. Consuming that here overrode Janus's 5510
     * with mhxd's 5500 — mhxd has no voice, never echoes CAP_VOICE, and
     * the test failed (and, before the cleanup-path fix, crashed).
     * Falling through to the unmodified matrix row makes the test pick
     * Janus at its real host-networked port on CI. `hx_test_servers_with`
     * returns the matrix entry verbatim (127.0.0.1:5510) — overlay the
     * voice env host/port onto a private copy, keeping the voice caps. */
    static hx_test_server overridden;
    const char *host_env = g_getenv ("GTKHX_VOICE_TEST_HOST");
    const char *port_env = g_getenv ("GTKHX_VOICE_TEST_PORT");
    if ((host_env && *host_env) || (port_env && *port_env)) {
        overridden = *srv;
        if (host_env && *host_env) {
            overridden.host = host_env;
        }
        if (port_env && *port_env) {
            int v = atoi (port_env);
            if (v > 0 && v < 65536) {
                overridden.port = (guint16) v;
            }
        }
        return &overridden;
    }
    return srv;
}

/* Put a client in a known, safe-to-close state: fd = -1 (so
 * client_close skips socket teardown) and every pointer NULL (so
 * g_array_free / gtkhx_voice_runtime_free aren't handed garbage). Used
 * to pre-initialize both clients before any client_open, because
 * client_open on the FIRST client can fail and send us straight to the
 * cleanup path for the second — which was never opened. */
static void
client_reset (voice_client *c)
{
    memset (c, 0, sizeof (*c));
    c->fd = -1;
}

static gboolean
client_open (voice_client *c, const char *label, const hx_test_server *srv,
             guint16 icon)
{
    memset (c, 0, sizeof (*c));
    c->label = label;
    c->fd = -1;
    c->state = GTKHX_VOICE_STATE_IDLE;
    c->spoke_uids = g_array_new (FALSE, FALSE, sizeof (guint16));

    char nick[40];
    g_snprintf (nick, sizeof (nick), "VMed-%s-%d-%04x", label, (int) getpid (),
                g_random_int () & 0xffff);

    c->fd = integration_open_login_to_caps_or_skip (srv, &c->htlc, nick, icon,
                                                    HTLC_CAP_VOICE);
    if (c->fd < 0) {
        return FALSE;
    }
    if (!integration_send_agreementagree_hope (c->fd, &c->htlc, NULL, nick,
                                               icon)) {
        return FALSE;
    }
    if ((c->htlc.caps & HTLC_CAP_VOICE) != HTLC_CAP_VOICE) {
        g_test_fail_printf ("%s: server did not echo CAP_VOICE", label);
        return FALSE;
    }

    gtkhx_voice_runtime_signal_callbacks sig = {
        .state_changed = on_state_changed,
        .mute_changed = NULL,
        .speaker_changed = on_speaker_changed,
        .error = NULL,
    };
    c->rt = gtkhx_voice_runtime_new_v2 (c, on_send_wire_frame, &sig);
    if (!c->rt) {
        g_test_fail_printf ("%s: VoiceRuntime construction failed "
                            "(GStreamer / webrtcbin unavailable?)",
                            label);
        return FALSE;
    }
    return TRUE;
}

static void
client_close (voice_client *c)
{
    if (c->rt) {
        gtkhx_voice_runtime_free (c->rt);
        c->rt = NULL;
    }
    if (c->fd >= 0) {
        integration_release_htlc (&c->htlc);
        integration_close (c->fd);
        c->fd = -1;
    }
    if (c->spoke_uids) {
        g_array_free (c->spoke_uids, TRUE);
        c->spoke_uids = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* GMainLoop-driven test state machine.                                */
/* ------------------------------------------------------------------ */

typedef enum {
    PH_START,
    PH_WAIT_A_CONNECTED,
    PH_WAIT_B_CONNECTED,
    PH_WAIT_FIRST_RX,
    PH_B_LEAVING,
    PH_WAIT_B_RECONNECTED,
    PH_WAIT_SECOND_RX,
} phase;

#define SECS(n) ((gint64) (n) * G_USEC_PER_SEC)
#define RX_MARGIN 100 /* ~2 s of PCMU at 50 pps */

typedef struct {
    voice_client *A;
    voice_client *B;
    voice_client *both[2];
    GMainLoop *loop;
    phase ph;
    gint64 deadline;
    guint64 after_first_rx;
    guint64 before_rejoin_rx;
    guint64 after_rejoin_rx;
    gboolean failed;
    gchar failmsg[256];
} driver;

static void
driver_fail (driver *d, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    g_vsnprintf (d->failmsg, sizeof (d->failmsg), fmt, ap);
    va_end (ap);
    d->failed = TRUE;
    g_main_loop_quit (d->loop);
}

/* Runs on the GLib main thread while g_main_loop_run owns the default
 * context. Drains control frames into the runtimes, then advances the
 * phase machine. */
static gboolean
driver_tick (gpointer data)
{
    driver *d = data;

    /* 1. Drain any control frames that arrived (non-blocking). */
    for (int i = 0; i < 2; i++) {
        voice_client *c = d->both[i];
        while (integration_recv_message (c->fd, &c->htlc, 0)) {
            dispatch_frame (c);
        }
    }

    gint64 now = g_get_monotonic_time ();
    switch (d->ph) {
    case PH_START:
        client_join_muted (d->A, 0);
        d->ph = PH_WAIT_A_CONNECTED;
        d->deadline = now + SECS (15);
        break;

    case PH_WAIT_A_CONNECTED:
        if (d->A->state == GTKHX_VOICE_STATE_CONNECTED) {
            client_runtime_unmute (d->A);
            client_join_muted (d->B, 0);
            d->ph = PH_WAIT_B_CONNECTED;
            d->deadline = now + SECS (15);
        } else if (now >= d->deadline) {
            driver_fail (d,
                         "A never reached CONNECTED (state=%d). ICE/DTLS to "
                         "Janus from this host may be blocked.",
                         (int) d->A->state);
        }
        break;

    case PH_WAIT_B_CONNECTED:
        if (d->B->state == GTKHX_VOICE_STATE_CONNECTED) {
            client_runtime_unmute (d->B);
            d->ph = PH_WAIT_FIRST_RX;
            d->deadline = now + SECS (15);
        } else if (now >= d->deadline) {
            driver_fail (d, "B never reached CONNECTED (state=%d).",
                         (int) d->B->state);
        }
        break;

    case PH_WAIT_FIRST_RX: {
        guint64 rx = gtkhx_voice_runtime_rtp_buffers_received (d->A->rt);
        if (rx >= RX_MARGIN) {
            d->after_first_rx = rx;
            g_test_message ("A rtp_buffers after B's first join: %"
                            G_GUINT64_FORMAT,
                            rx);
            gtkhx_voice_runtime_leave (d->B->rt, 0);
            d->ph = PH_B_LEAVING;
            d->deadline = now + SECS (3); /* let the leave settle */
        } else if (now >= d->deadline) {
            driver_fail (d,
                         "A received no RTP from B on the first join "
                         "(rtp_buffers=%" G_GUINT64_FORMAT
                         "). The media path never worked — check Janus UDP "
                         "reachability before trusting the rejoin assertion.",
                         rx);
        }
        break;
    }

    case PH_B_LEAVING:
        if (now >= d->deadline) {
            client_join_muted (d->B, 0);
            d->ph = PH_WAIT_B_RECONNECTED;
            d->deadline = now + SECS (15);
        }
        break;

    case PH_WAIT_B_RECONNECTED:
        if (d->B->state == GTKHX_VOICE_STATE_CONNECTED) {
            client_runtime_unmute (d->B);
            d->before_rejoin_rx
                = gtkhx_voice_runtime_rtp_buffers_received (d->A->rt);
            d->ph = PH_WAIT_SECOND_RX;
            d->deadline = now + SECS (15);
        } else if (now >= d->deadline) {
            driver_fail (d, "B never reached CONNECTED on rejoin (state=%d).",
                         (int) d->B->state);
        }
        break;

    case PH_WAIT_SECOND_RX: {
        guint64 rx = gtkhx_voice_runtime_rtp_buffers_received (d->A->rt);
        if (rx >= d->before_rejoin_rx + RX_MARGIN) {
            d->after_rejoin_rx = rx;
            g_main_loop_quit (d->loop); /* PASS */
        } else if (now >= d->deadline) {
            d->after_rejoin_rx = rx;
            driver_fail (d,
                         "A stopped receiving B after the rejoin: "
                         "rtp_buffers before=%" G_GUINT64_FORMAT
                         " after=%" G_GUINT64_FORMAT
                         " (expected an advance of >= %d).",
                         d->before_rejoin_rx, rx, RX_MARGIN);
        }
        break;
    }
    }

    return G_SOURCE_CONTINUE;
}

static void
test_voice_rejoin_media (void)
{
    /* Deterministic, device-free audio so a sender always produces
     * RTP — the receiver-side assertions then don't depend on a mic.
     * GTKHX_VOICE_REAL_AUDIO=1 overrides this with the production
     * autoaudiosrc leg (run on a host with real/virtual capture). */
    harness_select_audio_source ("1");

    g_assert_cmpint (gtkhx_voice_init (), ==, 1);

    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in the matrix.");
        return;
    }

    voice_client A, B;
    /* Zero both up front so the `out:` cleanup path is safe even if
     * client_open(&A) fails before B is ever opened — see client_reset. */
    client_reset (&A);
    client_reset (&B);

    if (!client_open (&A, "A", srv, 412)) {
        goto out;
    }
    if (!client_open (&B, "B", srv, 413)) {
        goto out;
    }

    driver d;
    memset (&d, 0, sizeof (d));
    d.A = &A;
    d.B = &B;
    d.both[0] = &A;
    d.both[1] = &B;
    d.loop = g_main_loop_new (NULL, FALSE);
    d.ph = PH_START;

    guint tick = g_timeout_add (5, driver_tick, &d);
    g_main_loop_run (d.loop);
    g_source_remove (tick);

    if (d.failed) {
        g_test_fail_printf ("%s", d.failmsg);
    } else {
        g_test_message ("A rtp_buffers before rejoin: %" G_GUINT64_FORMAT
                        ", after: %" G_GUINT64_FORMAT,
                        d.before_rejoin_rx, d.after_rejoin_rx);
        g_assert_cmpuint (d.after_rejoin_rx, >=, d.before_rejoin_rx + RX_MARGIN);
    }

    /* Spec REQUIRED (fogWraith "Send SSRC Declaration"): every SDP
     * answer both runtimes produced must declare the microphone
     * stream's SSRC + cname in the send section. This is the client-
     * side guarantee that keeps the server off its payload-type
     * fallback — the fallback is what dropped the first joiner's audio
     * in the original bug. Guards against a future GStreamer/webrtcbin
     * change silently dropping the a=ssrc line. */
    g_assert_cmpint (A.answers_seen, >, 0);
    g_assert_cmpint (A.answers_with_send_ssrc, ==, A.answers_seen);
    g_assert_cmpint (B.answers_seen, >, 0);
    g_assert_cmpint (B.answers_with_send_ssrc, ==, B.answers_seen);

    /* Best-effort leave so Janus reaps the room, then drain briefly. */
    if (B.rt) {
        gtkhx_voice_runtime_leave (B.rt, 0);
    }
    if (A.rt) {
        gtkhx_voice_runtime_leave (A.rt, 0);
    }
    for (int spin = 0; spin < 50; spin++) {
        g_main_context_iteration (NULL, FALSE);
        for (int i = 0; i < 2; i++) {
            voice_client *c = d.both[i];
            while (integration_recv_message (c->fd, &c->htlc, 0)) {
                dispatch_frame (c);
            }
        }
        g_usleep (2000);
    }
    g_main_loop_unref (d.loop);

out:
    client_close (&B);
    client_close (&A);
}

/* ------------------------------------------------------------------ */
/* Speaker-attribution (VAD) test.                                     */
/*                                                                     */
/* Both clients send a real tone (GTKHX_VOICE_TEST_AUDIO_SRC=sine), so */
/* each receiver's `level` VAD should clear the speaking threshold and */
/* fire SpeakerChanged for the sender's uid — asserted BOTH ways.      */
/*                                                                     */
/* Against a spec-compliant server (per-user `mid:user-<uid>` sections */
/* for everyone, including the first joiner) both directions resolve   */
/* the speaker straight from the mid — reliably. The runtime still     */
/* keeps a bundled `mid=send` RTCP-cname fallback for older servers    */
/* that collapse the first joiner's receive onto its send transceiver, */
/* but a current server never exercises it.                            */
/* ------------------------------------------------------------------ */

typedef enum {
    VPH_START,
    VPH_WAIT_A,
    VPH_WAIT_B,
    VPH_WAIT_SPEAKING,
} vad_phase;

typedef struct {
    voice_client *A;
    voice_client *B;
    voice_client *both[2];
    GMainLoop *loop;
    vad_phase ph;
    gint64 deadline;
    gboolean failed;
    gchar failmsg[256];
} vad_driver;

static void
vad_fail (vad_driver *d, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    g_vsnprintf (d->failmsg, sizeof (d->failmsg), fmt, ap);
    va_end (ap);
    d->failed = TRUE;
    g_main_loop_quit (d->loop);
}

static gboolean
vad_tick (gpointer data)
{
    vad_driver *d = data;
    for (int i = 0; i < 2; i++) {
        voice_client *c = d->both[i];
        while (integration_recv_message (c->fd, &c->htlc, 0)) {
            dispatch_frame (c);
        }
    }
    gint64 now = g_get_monotonic_time ();
    switch (d->ph) {
    case VPH_START:
        gtkhx_voice_runtime_join (d->A->rt, 0);
        d->ph = VPH_WAIT_A;
        d->deadline = now + SECS (15);
        break;
    case VPH_WAIT_A:
        if (d->A->state == GTKHX_VOICE_STATE_CONNECTED) {
            wire_unmute (d->A);
            gtkhx_voice_runtime_join (d->B->rt, 0);
            d->ph = VPH_WAIT_B;
            d->deadline = now + SECS (15);
        } else if (now >= d->deadline) {
            vad_fail (d, "A never reached CONNECTED (state=%d).",
                      (int) d->A->state);
        }
        break;
    case VPH_WAIT_B:
        if (d->B->state == GTKHX_VOICE_STATE_CONNECTED) {
            wire_unmute (d->B);
            d->ph = VPH_WAIT_SPEAKING;
            d->deadline = now + SECS (20);
        } else if (now >= d->deadline) {
            vad_fail (d, "B never reached CONNECTED (state=%d).",
                      (int) d->B->state);
        }
        break;
    case VPH_WAIT_SPEAKING: {
        guint16 a_uid = (guint16) d->A->htlc.uid;
        guint16 b_uid = (guint16) d->B->htlc.uid;
        gboolean a_saw_b = client_saw_speaking (d->A, b_uid);
        gboolean b_saw_a = client_saw_speaking (d->B, a_uid);
        if (a_saw_b && b_saw_a) {
            g_main_loop_quit (d->loop); /* PASS */
        } else if (now >= d->deadline) {
            vad_fail (
                d,
                "VAD attribution incomplete after 20s: A saw B(uid=%u) "
                "speaking=%d (bundled mid=send / cname path), B saw "
                "A(uid=%u) speaking=%d (per-user mid path).",
                (unsigned) b_uid, a_saw_b, (unsigned) a_uid, b_saw_a);
        }
        break;
    }
    }
    return G_SOURCE_CONTINUE;
}

static void
test_voice_vad_speaker (void)
{
    /* A real tone so the receivers' RMS clears the speaking threshold. */
    harness_select_audio_source ("sine");

    g_assert_cmpint (gtkhx_voice_init (), ==, 1);

    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in the matrix.");
        return;
    }

    voice_client A, B;
    /* Zero both up front so the `out:` cleanup path is safe even if
     * client_open(&A) fails before B is ever opened — see client_reset. */
    client_reset (&A);
    client_reset (&B);

    if (!client_open (&A, "A", srv, 412)) {
        goto out;
    }
    if (!client_open (&B, "B", srv, 413)) {
        goto out;
    }

    vad_driver d;
    memset (&d, 0, sizeof (d));
    d.A = &A;
    d.B = &B;
    d.both[0] = &A;
    d.both[1] = &B;
    d.loop = g_main_loop_new (NULL, FALSE);
    d.ph = VPH_START;

    guint tick = g_timeout_add (5, vad_tick, &d);
    g_main_loop_run (d.loop);
    g_source_remove (tick);

    if (d.failed) {
        g_test_fail_printf ("%s", d.failmsg);
    } else {
        g_test_message (
            "VAD: A attributed B(uid=%u) and B attributed A(uid=%u) "
            "speaking. Against a spec-compliant server both resolve via "
            "the per-user mid:user-<uid> leg; the bundled mid=send cname "
            "fallback remains only for older/non-compliant servers.",
            (unsigned) B.htlc.uid, (unsigned) A.htlc.uid);
    }

    if (B.rt) {
        gtkhx_voice_runtime_leave (B.rt, 0);
    }
    if (A.rt) {
        gtkhx_voice_runtime_leave (A.rt, 0);
    }
    for (int spin = 0; spin < 50; spin++) {
        g_main_context_iteration (NULL, FALSE);
        for (int i = 0; i < 2; i++) {
            voice_client *c = d.both[i];
            while (integration_recv_message (c->fd, &c->htlc, 0)) {
                dispatch_frame (c);
            }
        }
        g_usleep (2000);
    }
    g_main_loop_unref (d.loop);

out:
    client_close (&B);
    client_close (&A);
}

/* ------------------------------------------------------------------ */
/* Concurrent-join (Connecting-window renegotiation race) test.        */
/*                                                                     */
/* A joins the empty room and becomes the lone first joiner (bundled   */
/* mid=send). B joins while A is STILL Connecting — before A's own     */
/* ICE/DTLS finishes. Janus then sends A the renegotiation SDP offer   */
/* that adds B's receive leg while A sits in GTKHX_VOICE_STATE_CONNECTING. */
/* If the state machine has no Connecting arm for SdpOfferReceived that */
/* offer is dropped, A never builds B's receive bin, and A never hears */
/* B ("second participant not heard at all"). The assertion is the     */
/* same RTP-flow signal as the rejoin test: A's rtp_buffers must       */
/* advance once B is connected + unmuted.                              */
/*                                                                     */
/* The Connecting window is narrow, so this is a best-effort trigger:  */
/* we join B the instant A reports CONNECTING. If A had already raced  */
/* to CONNECTED we still join B (the scenario degrades to the ordinary */
/* Connected-arm path, which always worked) — the test logs which arm  */
/* it exercised. The deterministic proof lives in the hxvoice unit     */
/* test renegotiation_offer_while_connecting_is_processed_not_dropped; */
/* this is the live-Janus end-to-end guard.                            */
/* ------------------------------------------------------------------ */

/* The concurrent-join bug manifests as EXACTLY ZERO RTP: A drops B's
 * renegotiation offer and never builds a receive leg. So we don't need
 * a full 2 s of audio (RX_MARGIN) to prove the fix — a clearly-nonzero
 * count means the leg is up. A lower bar plus a generous deadline keeps
 * the test robust when B connects late (the renegotiation for B's leg
 * can trail A's own connect by seconds, especially as the third heavy
 * WebRTC test to hit one Janus), where the strict 100/15 s budget
 * flaked at ~84. ~0.8 s of PCMU is unambiguous vs a dropped offer. */
#define CJ_RX_MIN 40
#define CJ_RX_DEADLINE_S 25

typedef enum {
    CJ_START,
    CJ_WAIT_A_CONNECTING,
    CJ_WAIT_B_CONNECTED,
    CJ_WAIT_RX,
} cj_phase;

typedef struct {
    voice_client *A;
    voice_client *B;
    voice_client *both[2];
    GMainLoop *loop;
    cj_phase ph;
    gint64 deadline;
    gtkhx_voice_state a_state_at_b_join;
    guint64 rx;
    gboolean failed;
    gchar failmsg[256];
} cj_driver;

static void
cj_fail (cj_driver *d, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    g_vsnprintf (d->failmsg, sizeof (d->failmsg), fmt, ap);
    va_end (ap);
    d->failed = TRUE;
    g_main_loop_quit (d->loop);
}

static gboolean
cj_tick (gpointer data)
{
    cj_driver *d = data;
    for (int i = 0; i < 2; i++) {
        voice_client *c = d->both[i];
        while (integration_recv_message (c->fd, &c->htlc, 0)) {
            dispatch_frame (c);
        }
    }
    gint64 now = g_get_monotonic_time ();
    switch (d->ph) {
    case CJ_START:
        gtkhx_voice_runtime_join (d->A->rt, 0);
        d->ph = CJ_WAIT_A_CONNECTING;
        d->deadline = now + SECS (15);
        break;

    case CJ_WAIT_A_CONNECTING:
        /* Join B the moment A enters CONNECTING (the target window),
         * or if we blinked and A is already CONNECTED. */
        if (d->A->state == GTKHX_VOICE_STATE_CONNECTING
            || d->A->state == GTKHX_VOICE_STATE_CONNECTED) {
            d->a_state_at_b_join = d->A->state;
            wire_unmute (d->A);
            gtkhx_voice_runtime_join (d->B->rt, 0);
            d->ph = CJ_WAIT_B_CONNECTED;
            d->deadline = now + SECS (15);
        } else if (now >= d->deadline) {
            cj_fail (d,
                     "A never reached CONNECTING (state=%d). ICE/DTLS to "
                     "Janus from this host may be blocked.",
                     (int) d->A->state);
        }
        break;

    case CJ_WAIT_B_CONNECTED:
        if (d->B->state == GTKHX_VOICE_STATE_CONNECTED) {
            wire_unmute (d->B);
            d->ph = CJ_WAIT_RX;
            d->deadline = now + SECS (CJ_RX_DEADLINE_S);
        } else if (now >= d->deadline) {
            cj_fail (d, "B never reached CONNECTED after concurrent join "
                        "(state=%d).",
                     (int) d->B->state);
        }
        break;

    case CJ_WAIT_RX: {
        guint64 rx = gtkhx_voice_runtime_rtp_buffers_received (d->A->rt);
        if (rx >= CJ_RX_MIN) {
            d->rx = rx;
            g_main_loop_quit (d->loop); /* PASS */
        } else if (now >= d->deadline) {
            d->rx = rx;
            cj_fail (d,
                     "A received no RTP from B after B joined while A was "
                     "%s (rtp_buffers=%" G_GUINT64_FORMAT ", need >= %d). "
                     "Zero means the Connecting-window renegotiation race "
                     "bit: A dropped B's SDP offer while still Connecting "
                     "and never built B's receive leg.",
                     d->a_state_at_b_join == GTKHX_VOICE_STATE_CONNECTING
                         ? "CONNECTING"
                         : "CONNECTED",
                     rx, CJ_RX_MIN);
        }
        break;
    }
    }
    return G_SOURCE_CONTINUE;
}

static void
test_voice_concurrent_join (void)
{
    /* Silence is enough — we only assert RTP flow, not VAD. */
    harness_select_audio_source ("1");

    g_assert_cmpint (gtkhx_voice_init (), ==, 1);

    const hx_test_server *srv = pick_voice_server ();
    if (!srv) {
        g_test_fail_printf ("no voice-capable server in the matrix.");
        return;
    }

    voice_client A, B;
    /* Zero both up front so the `out:` cleanup path is safe even if
     * client_open(&A) fails before B is ever opened — see client_reset. */
    client_reset (&A);
    client_reset (&B);

    if (!client_open (&A, "A", srv, 412)) {
        goto out;
    }
    if (!client_open (&B, "B", srv, 413)) {
        goto out;
    }

    cj_driver d;
    memset (&d, 0, sizeof (d));
    d.A = &A;
    d.B = &B;
    d.both[0] = &A;
    d.both[1] = &B;
    d.loop = g_main_loop_new (NULL, FALSE);
    d.ph = CJ_START;

    guint tick = g_timeout_add (5, cj_tick, &d);
    g_main_loop_run (d.loop);
    g_source_remove (tick);

    if (d.failed) {
        g_test_fail_printf ("%s", d.failmsg);
    } else {
        /* NB: a_state_at_b_join is A's state when B *joined*, not the
         * state in which A processed the later renegotiation offer —
         * the test can't observe which state-machine arm ran. Whether
         * the offer landed during Connecting or after Connected is
         * timing-dependent, which is exactly why the deterministic arm
         * coverage lives in the hxvoice unit test. */
        g_test_message (
            "concurrent join: B joined while A was %s; A received RTP "
            "from B (rtp_buffers=%" G_GUINT64_FORMAT ").",
            d.a_state_at_b_join == GTKHX_VOICE_STATE_CONNECTING ? "CONNECTING"
                                                                : "CONNECTED",
            d.rx);
        g_assert_cmpuint (d.rx, >=, CJ_RX_MIN);
    }

    if (B.rt) {
        gtkhx_voice_runtime_leave (B.rt, 0);
    }
    if (A.rt) {
        gtkhx_voice_runtime_leave (A.rt, 0);
    }
    for (int spin = 0; spin < 50; spin++) {
        g_main_context_iteration (NULL, FALSE);
        for (int i = 0; i < 2; i++) {
            voice_client *c = d.both[i];
            while (integration_recv_message (c->fd, &c->htlc, 0)) {
                dispatch_frame (c);
            }
        }
        g_usleep (2000);
    }
    g_main_loop_unref (d.loop);

out:
    client_close (&B);
    client_close (&A);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/voice/rejoin_media",
                     test_voice_rejoin_media);
    g_test_add_func ("/integration/voice/vad_speaker",
                     test_voice_vad_speaker);
    g_test_add_func ("/integration/voice/concurrent_join",
                     test_voice_concurrent_join);
    return g_test_run ();
}
