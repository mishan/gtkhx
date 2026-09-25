/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_video_control.c — Tier 3 control-channel tests
 * for the video extension (hxd-ng docs/capabilities-video.md), against
 * hxd-ng.
 *
 * Covers the client's side of what hxd-ng's own tests/video.rs covers
 * from the server's: bit 10 negotiated only alongside bit 2, one
 * DATA_VIDEO_LIMITS per kind in the LOGIN reply, Video Start / State /
 * Stop round-trips and the Video Status (611) each one produces, the
 * refusals — no voice, the room's one screen slot taken, an account
 * without the bit — and 611 reaching a second participant. Nothing here
 * runs a WebRTC stack: joining voice is the 600 alone, and the video
 * transactions are legal from the moment the join reply lands. The
 * media half, with real VP8 flowing, is test_video_media.c.
 *
 * Every frame goes out through the same hxproto builders production's
 * senders use, via the gtkhx_proto_* C ABI where one exists and the raw
 * chunk layout otherwise, and every reply is read with the parsers
 * rcv.c uses.
 *
 * Server gating: HX_TEST_CAP_VIDEO, which only hxd-ng has. No silent
 * skip: a matrix without it fails.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "hl_access.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "integration_harness.h"
#include "server_matrix.h"

#define VOICE_VIDEO (HTLC_CAP_VOICE | HTLC_CAP_VIDEO)

static const hx_test_server *
pick_video_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_VIDEO);
    const hx_test_server *srv
        = (servers && servers->len > 0) ? g_ptr_array_index (servers, 0) : NULL;
    if (servers) {
        g_ptr_array_unref (servers);
    }
    if (!srv) {
        g_test_fail_printf ("no video-capable server in the matrix "
                            "(GTKHX_TEST_SERVERS excluded hxd-ng?)");
    }
    return srv;
}

/* One logged-in client. */
typedef struct {
    int fd;
    struct htlc_conn htlc;
    char nick[40];
} client;

static gboolean
client_open_as (client *c, const hx_test_server *srv, const char *tag,
                const char *login, const char *password, guint16 caps)
{
    memset (c, 0, sizeof (*c));
    c->fd = -1;
    g_snprintf (c->nick, sizeof (c->nick), "Vid%s-%d-%04x", tag, (int)getpid (),
                g_random_int () & 0xffff);
    c->fd = integration_open_login_account_caps_or_skip (
        srv, &c->htlc, login, password, c->nick, 412, caps);
    if (c->fd < 0) {
        return FALSE;
    }
    if (!integration_send_agreementagree_hope (c->fd, &c->htlc, NULL, c->nick,
                                               412)) {
        g_test_fail_printf ("%s: agreement not accepted", c->nick);
        return FALSE;
    }
    return TRUE;
}

static gboolean
client_open (client *c, const hx_test_server *srv, const char *tag)
{
    return client_open_as (c, srv, tag, "guest", NULL, VOICE_VIDEO);
}

static void
client_close (client *c)
{
    if (c->fd >= 0) {
        integration_release_htlc (&c->htlc);
        integration_close (c->fd);
        c->fd = -1;
    }
}

/* ---- Senders -------------------------------------------------------- */

static guint32
send_voice_join (client *c, guint32 cid)
{
    guint32 cid_be = g_htonl (cid);
    guint32 trans = c->htlc.trans;
    if (!integration_send_message (c->fd, &c->htlc, HTLC_HDR_VOICE_JOIN, 0, 1,
                                   (int)HTLC_DATA_CHAT_ID, 4, &cid_be)) {
        return 0;
    }
    return trans;
}

static guint32
send_video_kind (client *c, guint32 opcode, guint32 cid, guint16 kind)
{
    guint32 cid_be = g_htonl (cid);
    guint16 kind_be = g_htons (kind);
    guint32 trans = c->htlc.trans;
    if (!integration_send_message (c->fd, &c->htlc, opcode, 0, 2,
                                   (int)HTLC_DATA_CHAT_ID, 4, &cid_be,
                                   (int)HTLC_DATA_VIDEO_KIND, 2, &kind_be)) {
        return 0;
    }
    return trans;
}

static guint32
send_video_state (client *c, guint32 cid, guint16 kind, gboolean paused)
{
    guint32 cid_be = g_htonl (cid);
    guint16 kind_be = g_htons (kind);
    guint16 paused_be = g_htons (paused ? 1 : 0);
    guint32 trans = c->htlc.trans;
    if (!integration_send_message (
            c->fd, &c->htlc, HTLC_HDR_VIDEO_STATE, 0, 3, (int)HTLC_DATA_CHAT_ID,
            4, &cid_be, (int)HTLC_DATA_VIDEO_KIND, 2, &kind_be,
            (int)HTLC_DATA_VIDEO_PAUSED, 2, &paused_be)) {
        return 0;
    }
    return trans;
}

/* Join voice in cid and wait for the (non-error) join reply. The offer
 * it carries is never answered: the video transactions under test are
 * control-channel only, and a server may not make them wait on the
 * media path. */
static gboolean
join_voice (client *c, guint32 cid)
{
    guint32 trans = send_voice_join (c, cid);
    if (!trans
        || !integration_drain_until_task_trans (c->fd, &c->htlc, trans, 64)) {
        g_test_fail_printf ("%s: no VOICE_JOIN reply", c->nick);
        return FALSE;
    }
    if (hdr_flag (&c->htlc) & 1) {
        g_test_fail_printf ("%s: VOICE_JOIN refused", c->nick);
        return FALSE;
    }
    return TRUE;
}

/* ---- Receive side --------------------------------------------------- */

/* Does a DATA_VIDEO_PUBLISHERS blob list (uid, kind), and paused? */
static gboolean
blob_has (const guint8 *blob, gsize len, guint16 uid, guint16 kind,
          gboolean *paused)
{
    for (gsize at = 0; at + 8 <= len; at += 8) {
        guint16 u = (guint16)((blob[at] << 8) | blob[at + 1]);
        guint16 k = (guint16)((blob[at + 2] << 8) | blob[at + 3]);
        guint16 f = (guint16)((blob[at + 4] << 8) | blob[at + 5]);
        if (u == uid && k == kind) {
            if (paused) {
                *paused = (f & 1) != 0;
            }
            return TRUE;
        }
    }
    return FALSE;
}

/* What the 611 we are waiting for must say about (uid, kind). */
typedef enum {
    WANT_LIVE,
    WANT_PAUSED,
    WANT_ABSENT,
} want;

static gboolean
status_matches (const struct htlc_conn *htlc, guint16 uid, guint16 kind, want w)
{
    struct gtkhx_proto_video_reply r;
    gtkhx_proto_parse_video_reply (hx_test_in (htlc)->buf,
                                   hx_test_in (htlc)->pos, &r);
    gboolean paused = FALSE;
    gboolean has
        = r.publishers_ptr
          && blob_has (r.publishers_ptr, r.publishers_len, uid, kind, &paused);
    switch (w) {
    case WANT_LIVE:
        return has && !paused;
    case WANT_PAUSED:
        return has && paused;
    case WANT_ABSENT:
        return !has;
    }
    return FALSE;
}

/* The outcome of a request: its TASK reply, and whether a 611 matching
 * the expectation arrived. The two can come in either order — a server
 * may send the room its status before or after replying — so both are
 * collected in one pass rather than draining for one and discarding
 * the other. */
typedef struct {
    gboolean replied;
    gboolean error;
    char error_text[256];
    char codec[16];
    gboolean status_seen;
} outcome;

static outcome
await (client *c, guint32 trans, gboolean want_status, guint16 uid,
       guint16 kind, want w)
{
    outcome o;
    memset (&o, 0, sizeof (o));
    gint64 deadline = g_get_monotonic_time () + 8 * G_USEC_PER_SEC;
    while (g_get_monotonic_time () < deadline
           && !(o.replied && (o.status_seen || !want_status))) {
        if (!integration_recv_message (c->fd, &c->htlc, 2000)) {
            break;
        }
        guint32 type = hdr_type (&c->htlc);
        if (trans && type == HTLS_HDR_TASK && hdr_trans (&c->htlc) == trans) {
            o.replied = TRUE;
            o.error = (hdr_flag (&c->htlc) & 1) != 0;
            gsize n = 0;
            if (o.error) {
                task_error_extract (hx_test_in (&c->htlc)->buf,
                                    hx_test_in (&c->htlc)->pos, o.error_text,
                                    sizeof (o.error_text), &n);
            } else {
                struct gtkhx_proto_video_reply r;
                gtkhx_proto_parse_video_reply (hx_test_in (&c->htlc)->buf,
                                               hx_test_in (&c->htlc)->pos, &r);
                if (r.codec_ptr) {
                    gsize cl = MIN (r.codec_len, sizeof (o.codec) - 1);
                    memcpy (o.codec, r.codec_ptr, cl);
                }
            }
            if (o.error) {
                break; /* no status follows a refusal */
            }
        } else if (want_status && type == HTLS_HDR_VIDEO_STATUS
                   && status_matches (&c->htlc, uid, kind, w)) {
            o.status_seen = TRUE;
        }
    }
    if (!trans) {
        o.replied = TRUE;
    }
    return o;
}

/* ---- Tests ---------------------------------------------------------- */

/* Bit 10 comes back with bit 2, and the LOGIN reply carries a limits
 * field for each kind. The rig runs [voice.video] with the spec's
 * default ceilings, so those are the numbers to expect. */
static void
test_cap_and_limits (void)
{
    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        return;
    }
    client c;
    if (!client_open (&c, srv, "Cap")) {
        client_close (&c);
        return;
    }
    g_assert_cmphex (c.htlc.caps & VOICE_VIDEO, ==, VOICE_VIDEO);

    const struct hx_video_limits *cam = &c.htlc.video_limits[0];
    const struct hx_video_limits *scr = &c.htlc.video_limits[1];
    g_assert_cmpuint (cam->present, ==, 1);
    g_assert_cmpuint (cam->max_width, ==, 1280);
    g_assert_cmpuint (cam->max_height, ==, 720);
    g_assert_cmpuint (cam->max_fps, ==, 30);
    g_assert_cmpuint (cam->max_bitrate, ==, 1500000);
    g_assert_cmpuint (scr->present, ==, 1);
    g_assert_cmpuint (scr->max_width, ==, 1920);
    g_assert_cmpuint (scr->max_height, ==, 1080);
    g_assert_cmpuint (scr->max_fps, ==, 15);
    g_assert_cmpuint (scr->max_bitrate, ==, 2500000);
    client_close (&c);
}

/* A client that asks for video without voice gets neither: bit 10
 * depends on bit 2, and a server must not confirm it alone. */
static void
test_video_needs_voice_bit (void)
{
    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        return;
    }
    client c;
    if (!client_open_as (&c, srv, "NoVoice", "guest", NULL, HTLC_CAP_VIDEO)) {
        client_close (&c);
        return;
    }
    g_assert_cmphex (c.htlc.caps & HTLC_CAP_VIDEO, ==, 0);
    client_close (&c);
}

/* Video Start outside a voice room is refused. */
static void
test_start_needs_voice_room (void)
{
    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        return;
    }
    client c;
    if (!client_open (&c, srv, "NoRoom")) {
        client_close (&c);
        return;
    }
    guint32 t
        = send_video_kind (&c, HTLC_HDR_VIDEO_START, 0, HX_VIDEO_KIND_CAMERA);
    outcome o = await (&c, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.replied);
    g_assert_true (o.error);
    client_close (&c);
}

/* Start, pause, resume, stop: each answered, each reflected in a 611 to
 * the publisher itself — the room includes it. */
static void
test_start_pause_stop (void)
{
    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        return;
    }
    client c;
    if (!client_open (&c, srv, "Pub") || !join_voice (&c, 0)) {
        client_close (&c);
        return;
    }
    guint16 me = c.htlc.uid;
    g_assert_cmpuint (me, !=, 0);

    guint32 t
        = send_video_kind (&c, HTLC_HDR_VIDEO_START, 0, HX_VIDEO_KIND_CAMERA);
    outcome o = await (&c, t, TRUE, me, HX_VIDEO_KIND_CAMERA, WANT_LIVE);
    g_assert_true (o.replied);
    g_assert_false (o.error);
    g_assert_cmpstr (o.codec, ==, "VP8");
    g_assert_true (o.status_seen);

    t = send_video_state (&c, 0, HX_VIDEO_KIND_CAMERA, TRUE);
    o = await (&c, t, TRUE, me, HX_VIDEO_KIND_CAMERA, WANT_PAUSED);
    g_assert_true (o.replied);
    g_assert_false (o.error);
    g_assert_true (o.status_seen);

    t = send_video_state (&c, 0, HX_VIDEO_KIND_CAMERA, FALSE);
    o = await (&c, t, TRUE, me, HX_VIDEO_KIND_CAMERA, WANT_LIVE);
    g_assert_true (o.replied);
    g_assert_false (o.error);
    g_assert_true (o.status_seen);

    t = send_video_kind (&c, HTLC_HDR_VIDEO_STOP, 0, HX_VIDEO_KIND_CAMERA);
    o = await (&c, t, TRUE, me, HX_VIDEO_KIND_CAMERA, WANT_ABSENT);
    g_assert_true (o.replied);
    g_assert_false (o.error);
    g_assert_true (o.status_seen);

    /* Stopping what isn't published is not an error: disconnect races
     * make stop idempotent. */
    t = send_video_kind (&c, HTLC_HDR_VIDEO_STOP, 0, HX_VIDEO_KIND_CAMERA);
    o = await (&c, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.replied);
    g_assert_false (o.error);
    client_close (&c);
}

/* A publication is a room-wide fact: another participant's 611 lists
 * it, and loses it when the publisher leaves. */
static void
test_status_reaches_the_room (void)
{
    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        return;
    }
    client a, b;
    memset (&b, 0, sizeof (b));
    b.fd = -1;
    if (!client_open (&a, srv, "RoomA") || !client_open (&b, srv, "RoomB")
        || !join_voice (&a, 0) || !join_voice (&b, 0)) {
        client_close (&a);
        client_close (&b);
        return;
    }
    guint32 t
        = send_video_kind (&a, HTLC_HDR_VIDEO_START, 0, HX_VIDEO_KIND_CAMERA);
    outcome o = await (&a, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.replied);
    g_assert_false (o.error);

    o = await (&b, 0, TRUE, a.htlc.uid, HX_VIDEO_KIND_CAMERA, WANT_LIVE);
    g_assert_true (o.status_seen);

    /* The publisher disconnecting ends the publication for the room. */
    guint16 a_uid = a.htlc.uid;
    client_close (&a);
    o = await (&b, 0, TRUE, a_uid, HX_VIDEO_KIND_CAMERA, WANT_ABSENT);
    g_assert_true (o.status_seen);
    client_close (&b);
}

/* The room has one screen slot. A second sharer is refused, told why,
 * and the first share is not preempted. */
static void
test_second_screen_share_is_refused (void)
{
    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        return;
    }
    client a, b;
    memset (&b, 0, sizeof (b));
    b.fd = -1;
    if (!client_open (&a, srv, "ScrA") || !client_open (&b, srv, "ScrB")
        || !join_voice (&a, 0) || !join_voice (&b, 0)) {
        client_close (&a);
        client_close (&b);
        return;
    }
    guint32 t
        = send_video_kind (&a, HTLC_HDR_VIDEO_START, 0, HX_VIDEO_KIND_SCREEN);
    outcome o = await (&a, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.replied);
    g_assert_false (o.error);

    t = send_video_kind (&b, HTLC_HDR_VIDEO_START, 0, HX_VIDEO_KIND_SCREEN);
    o = await (&b, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.error);
    g_assert_nonnull (strstr (o.error_text, "Someone else"));

    /* Once A releases the slot, B may take it. */
    t = send_video_kind (&a, HTLC_HDR_VIDEO_STOP, 0, HX_VIDEO_KIND_SCREEN);
    o = await (&a, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.replied);
    g_assert_false (o.error);
    t = send_video_kind (&b, HTLC_HDR_VIDEO_START, 0, HX_VIDEO_KIND_SCREEN);
    o = await (&b, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.replied);
    g_assert_false (o.error);

    client_close (&a);
    client_close (&b);
}

/* An account with voice but neither video bit is refused a camera and
 * a screen share, and is still echoed the capability — the capability
 * says the server supports video, the bits say what this user may do. */
static void
test_access_bits_gate_publishing (void)
{
    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        return;
    }
    client c;
    if (!client_open_as (&c, srv, "NoBit", "novideo", "novideo", VOICE_VIDEO)
        || !join_voice (&c, 0)) {
        client_close (&c);
        return;
    }
    g_assert_cmphex (c.htlc.caps & VOICE_VIDEO, ==, VOICE_VIDEO);
    g_assert_false (
        hl_access_has ((const guint8 *)&c.htlc.access, HL_ACCESS_VIDEO_CHAT));
    g_assert_false (
        hl_access_has ((const guint8 *)&c.htlc.access, HL_ACCESS_SCREEN_SHARE));

    guint32 t
        = send_video_kind (&c, HTLC_HDR_VIDEO_START, 0, HX_VIDEO_KIND_CAMERA);
    outcome o = await (&c, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.error);
    t = send_video_kind (&c, HTLC_HDR_VIDEO_START, 0, HX_VIDEO_KIND_SCREEN);
    o = await (&c, t, FALSE, 0, 0, WANT_LIVE);
    g_assert_true (o.error);
    client_close (&c);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/video/cap_and_limits", test_cap_and_limits);
    g_test_add_func ("/integration/video/needs_voice_bit",
                     test_video_needs_voice_bit);
    g_test_add_func ("/integration/video/start_needs_voice_room",
                     test_start_needs_voice_room);
    g_test_add_func ("/integration/video/start_pause_stop",
                     test_start_pause_stop);
    g_test_add_func ("/integration/video/status_reaches_the_room",
                     test_status_reaches_the_room);
    g_test_add_func ("/integration/video/second_screen_share_is_refused",
                     test_second_screen_share_is_refused);
    g_test_add_func ("/integration/video/access_bits_gate_publishing",
                     test_access_bits_gate_publishing);
    return g_test_run ();
}
