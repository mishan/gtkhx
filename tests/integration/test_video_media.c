/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_video_media.c — Tier 3 MEDIA test for the video
 * extension: two real VoiceRuntimes (webrtcbin, ICE-lite, DTLS-SRTP)
 * against hxd-ng, with VP8 actually encoded, forwarded and decoded.
 *
 * Scenario, in the order a user would do it:
 *
 *   1. A and B join voice in the public room.
 *   2. A starts its camera. The capture is a live videotestsrc
 *      (GTKHX_VOICE_TEST_VIDEO_SRC), so no camera is needed. The server
 *      renegotiates A alone to add its cam-send section; A's answer must
 *      carry that section as sendonly with an a=ssrc — the spec gives the
 *      server no other way to tell a camera from a screen.
 *   3. B learns of the publication from Video Status (611) and, a few
 *      seconds later, asks for it with Video Subscribe (610). The server
 *      renegotiates B to add a cam-user-<A> section; A's first keyframe
 *      is long past, so the picture depends on a keyframe request
 *      reaching A's encoder. B's runtime builds a VP8 receive leg and
 *      decodes frames.
 *   4. A pauses (609): no renegotiation, B's frames stop. A resumes: B's
 *      frames start again, which needs A's capture rebuilt and a fresh
 *      keyframe through the server.
 *   5. A stops (608): B's section goes a=inactive and B tears the leg
 *      down, so its frame store for A's camera empties.
 *
 * The assertions are on the runtime's frame counters — frames decoded
 * per stream — plus A's own preview, and on every answer's send
 * sections: the microphone's `send` and, while publishing, `cam-send`.
 * Voice keeps working throughout, which the Connected state the runtimes
 * hold is the witness to.
 *
 * Driver model: the same GMainLoop-and-phases shape as
 * test_voice_rejoin_media.c, for the same reason — every runtime call on
 * the main thread while it owns the default context, as in production.
 *
 * Server gating: HX_TEST_CAP_VIDEO, which only hxd-ng has. Needs UDP
 * reachability to its voice port as well as the control channel.
 */

#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "integration_harness.h"
#include "server_matrix.h"
#include "voice_runtime.h"

#define VOICE_VIDEO (HTLC_CAP_VOICE | HTLC_CAP_VIDEO)

typedef struct {
    const char *label;
    int fd;
    struct htlc_conn htlc;
    gtkhx_voice_runtime *rt;
    guint32 join_trans;
    /* trans of the in-flight VIDEO_START, so a refusal reaches the
     * runtime the way rcv.c's task-error path delivers it. */
    guint32 start_trans;
    gtkhx_voice_state state;
    /* Answer audit. Every answer must declare the microphone's SSRC in
     * `send`; every answer while the camera is live must carry cam-send
     * as sendonly with its own a=ssrc. */
    int answers_seen;
    int answers_with_send_ssrc;
    int answers_with_cam_send;
    /* The last 611's view of the other client's camera. */
    gboolean peer_camera_listed;
} video_client;

/* Scan the answer's section for `mid`: is it present, sendonly, and
 * declaring an SSRC with a cname? */
static gboolean
answer_section_sends (const char *sdp, size_t len, const char *mid)
{
    char *buf = g_strndup (sdp, len);
    char **lines = g_strsplit (buf, "\n", -1);
    char *want = g_strdup_printf ("a=mid:%s", mid);
    gboolean in = FALSE, sendonly = FALSE, ssrc = FALSE, ok = FALSE;
    for (int i = 0;; i++) {
        char *ln = lines[i] ? g_strchomp (lines[i]) : NULL;
        if (!ln || g_str_has_prefix (ln, "m=")) {
            if (in && sendonly && ssrc) {
                ok = TRUE;
                break;
            }
            in = sendonly = ssrc = FALSE;
            if (!ln) {
                break;
            }
            continue;
        }
        if (g_strcmp0 (ln, want) == 0) {
            in = TRUE;
        } else if (in && g_strcmp0 (ln, "a=sendonly") == 0) {
            sendonly = TRUE;
        } else if (in && g_str_has_prefix (ln, "a=ssrc:")
                   && strstr (ln, "cname:")) {
            ssrc = TRUE;
        }
    }
    g_free (want);
    g_strfreev (lines);
    g_free (buf);
    return ok;
}

/* Wire-out: the runtime's frames onto the control socket, 600-610. */
static void
on_send_wire_frame (void *user_data, uint32_t opcode, const uint8_t *body,
                    size_t body_len)
{
    video_client *c = user_data;
    if (!c || body_len < 4) {
        return;
    }
    guint32 cid = ((guint32)body[0] << 24) | ((guint32)body[1] << 16)
                  | ((guint32)body[2] << 8) | (guint32)body[3];
    guint32 cid_be = g_htonl (cid);
    const guint8 *p = body + 4;
    gsize plen = body_len - 4;
    guint32 trans = c->htlc.trans;

    switch (opcode) {
    case HTLC_HDR_VOICE_JOIN:
        if (integration_send_message (c->fd, &c->htlc, opcode, 0, 1,
                                      (int)HTLC_DATA_CHAT_ID, 4, &cid_be)) {
            c->join_trans = trans;
        }
        break;
    case HTLC_HDR_VOICE_LEAVE:
        integration_send_message (c->fd, &c->htlc, opcode, 0, 1,
                                  (int)HTLC_DATA_CHAT_ID, 4, &cid_be);
        break;
    case HTLC_HDR_VOICE_SDP_ANSWER:
        c->answers_seen++;
        if (answer_section_sends ((const char *)p, plen, "send")) {
            c->answers_with_send_ssrc++;
        }
        if (answer_section_sends ((const char *)p, plen, "cam-send")) {
            c->answers_with_cam_send++;
        }
        integration_send_message (
            c->fd, &c->htlc, opcode, 0, 2, (int)HTLC_DATA_CHAT_ID, 4, &cid_be,
            (int)HTLC_DATA_VOICE_SDP, (int)plen, (guint8 *)p);
        break;
    case HTLC_HDR_VOICE_ICE:
        integration_send_message (
            c->fd, &c->htlc, opcode, 0, 2, (int)HTLC_DATA_CHAT_ID, 4, &cid_be,
            (int)HTLC_DATA_VOICE_ICE, (int)plen, (guint8 *)p);
        break;
    case HTLC_HDR_VOICE_MUTE:
        integration_send_message (
            c->fd, &c->htlc, opcode, 0, 2, (int)HTLC_DATA_CHAT_ID, 4, &cid_be,
            (int)HTLC_DATA_VOICE_MUTED, (int)plen, (guint8 *)p);
        break;
    case HTLC_HDR_VIDEO_START:
    case HTLC_HDR_VIDEO_STOP:
        if (plen >= 2
            && integration_send_message (
                c->fd, &c->htlc, opcode, 0, 2, (int)HTLC_DATA_CHAT_ID, 4,
                &cid_be, (int)HTLC_DATA_VIDEO_KIND, 2, (guint8 *)p)
            && opcode == HTLC_HDR_VIDEO_START) {
            c->start_trans = trans;
        }
        break;
    case HTLC_HDR_VIDEO_STATE:
        if (plen >= 4) {
            integration_send_message (
                c->fd, &c->htlc, opcode, 0, 3, (int)HTLC_DATA_CHAT_ID, 4,
                &cid_be, (int)HTLC_DATA_VIDEO_KIND, 2, (guint8 *)p,
                (int)HTLC_DATA_VIDEO_PAUSED, 2, (guint8 *)p + 2);
        }
        break;
    case HTLC_HDR_VIDEO_SUBSCRIBE:
        integration_send_message (
            c->fd, &c->htlc, opcode, 0, 2, (int)HTLC_DATA_CHAT_ID, 4, &cid_be,
            (int)HTLC_DATA_VIDEO_SUBSCRIPTIONS, (int)plen, (guint8 *)p);
        break;
    default:
        break;
    }
}

static void
on_state_changed (void *user_data, gtkhx_voice_state state)
{
    video_client *c = user_data;
    c->state = state;
}

static guint32
reply_cid (const struct htlc_conn *htlc)
{
    struct gtkhx_proto_voice_reply r;
    memset (&r, 0, sizeof (r));
    gtkhx_proto_parse_voice_reply (hx_test_in (htlc)->buf,
                                   hx_test_in (htlc)->pos, &r);
    return r.cid;
}

static void
feed_voice_field (video_client *c, int field)
{
    const guint8 *ptr = NULL;
    size_t len = 0;
    if (!gtkhx_proto_voice_reply_field (hx_test_in (&c->htlc)->buf,
                                        hx_test_in (&c->htlc)->pos, field, &ptr,
                                        &len)) {
        return;
    }
    guint32 cid = reply_cid (&c->htlc);
    switch (field) {
    case 0: /* SDP */
        if (len > 0) {
            char *s = g_strndup ((const char *)ptr, len);
            gtkhx_voice_runtime_sdp_offer (c->rt, cid, s);
            g_free (s);
        }
        break;
    case 1: /* ICE */
        if (len == 0) {
            gtkhx_voice_runtime_ice_candidate (c->rt, cid, NULL);
        } else {
            char *json = g_strndup ((const char *)ptr, len);
            gtkhx_voice_runtime_ice_candidate (c->rt, cid, json);
            g_free (json);
        }
        break;
    case 3: /* participants */
        gtkhx_voice_runtime_room_status (c->rt, cid, ptr, len);
        break;
    }
}

/* Mirror of rcv.c's dispatch for the frames a voice + video client sees. */
static void
dispatch_frame (video_client *c, guint16 peer_uid)
{
    guint32 type = hdr_type (&c->htlc);
    guint32 trans = hdr_trans (&c->htlc);

    if (type == HTLS_HDR_TASK && c->join_trans && trans == c->join_trans) {
        c->join_trans = 0;
        feed_voice_field (c, 3);
        feed_voice_field (c, 0);
        return;
    }
    if (type == HTLS_HDR_TASK && c->start_trans && trans == c->start_trans) {
        c->start_trans = 0;
        if (hdr_flag (&c->htlc) & 1) {
            char err[256] = { 0 };
            gsize n = 0;
            task_error_extract (hx_test_in (&c->htlc)->buf,
                                hx_test_in (&c->htlc)->pos, err, sizeof (err),
                                &n);
            /* Every start here is in the public chat. */
            gtkhx_voice_runtime_video_start_failed (c->rt, 0,
                                                    HX_VIDEO_KIND_CAMERA, err);
        }
        return;
    }
    if (type == HTLS_HDR_VOICE_SDP_OFFER) {
        feed_voice_field (c, 0);
    } else if (type == HTLS_HDR_VOICE_ICE) {
        feed_voice_field (c, 1);
    } else if (type == HTLS_HDR_VOICE_ROOM_STATUS) {
        feed_voice_field (c, 3);
    } else if (type == HTLS_HDR_VIDEO_STATUS) {
        struct gtkhx_proto_video_reply r;
        gtkhx_proto_parse_video_reply (hx_test_in (&c->htlc)->buf,
                                       hx_test_in (&c->htlc)->pos, &r);
        gtkhx_voice_runtime_video_status (c->rt, r.cid, r.publishers_ptr,
                                          r.publishers_len);
        c->peer_camera_listed = FALSE;
        for (gsize at = 0; r.publishers_ptr && at + 8 <= r.publishers_len;
             at += 8) {
            const guint8 *e = r.publishers_ptr + at;
            guint16 u = (guint16)((e[0] << 8) | e[1]);
            guint16 k = (guint16)((e[2] << 8) | e[3]);
            if (u == peer_uid && k == HX_VIDEO_KIND_CAMERA) {
                c->peer_camera_listed = TRUE;
            }
        }
    }
}

static const hx_test_server *
pick_video_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_VIDEO);
    const hx_test_server *srv
        = (servers && servers->len > 0) ? g_ptr_array_index (servers, 0) : NULL;
    if (servers) {
        g_ptr_array_unref (servers);
    }
    return srv;
}

static void
client_reset (video_client *c)
{
    memset (c, 0, sizeof (*c));
    c->fd = -1;
}

static gboolean
client_open (video_client *c, const char *label, const hx_test_server *srv,
             guint16 icon)
{
    client_reset (c);
    c->label = label;
    c->state = GTKHX_VOICE_STATE_IDLE;
    char nick[40];
    g_snprintf (nick, sizeof (nick), "VidMed-%s-%d-%04x", label, (int)getpid (),
                g_random_int () & 0xffff);
    c->fd = integration_open_login_to_caps_or_skip (srv, &c->htlc, nick, icon,
                                                    VOICE_VIDEO);
    if (c->fd < 0) {
        return FALSE;
    }
    if (!integration_send_agreementagree_hope (c->fd, &c->htlc, NULL, nick,
                                               icon)) {
        g_test_fail_printf ("%s: agreement not accepted", label);
        return FALSE;
    }
    if ((c->htlc.caps & VOICE_VIDEO) != VOICE_VIDEO) {
        g_test_fail_printf ("%s: server did not echo voice and video", label);
        return FALSE;
    }
    gtkhx_voice_runtime_signal_callbacks sig = {
        .state_changed = on_state_changed,
    };
    c->rt = gtkhx_voice_runtime_new_v2 (c, on_send_wire_frame, &sig);
    if (!c->rt) {
        g_test_fail_printf ("%s: VoiceRuntime construction failed", label);
        return FALSE;
    }
    gtkhx_voice_runtime_set_self_uid (c->rt, c->htlc.uid);
    /* What production hands a runtime at construction: the login
     * reply's ceilings. */
    for (guint16 kind = HX_VIDEO_KIND_CAMERA; kind <= HX_VIDEO_KIND_SCREEN;
         kind++) {
        const struct hx_video_limits *l = &c->htlc.video_limits[kind - 1];
        if (l->present) {
            gtkhx_voice_runtime_set_video_limits (c->rt, kind, l->max_width,
                                                  l->max_height, l->max_fps,
                                                  l->max_bitrate);
        }
    }
    return TRUE;
}

static void
client_close (video_client *c)
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
}

/* ---- Driver --------------------------------------------------------- */

typedef enum {
    PH_START,
    PH_WAIT_A_CONNECTED,
    PH_WAIT_B_CONNECTED,
    PH_WAIT_CAMERA_LISTED,
    PH_LATE_SUBSCRIBE,
    PH_WAIT_FRAMES,
    PH_PAUSED_SETTLE,
    PH_PAUSED_QUIET,
    PH_WAIT_RESUMED_FRAMES,
    PH_WAIT_STOPPED,
} phase;

#define SECS(n) ((gint64)(n) * G_USEC_PER_SEC)
/* A second of the 15-30 fps test pattern, at least. */
#define FRAME_MARGIN 15

typedef struct {
    video_client *A, *B;
    GMainLoop *loop;
    phase ph;
    gint64 deadline;
    guint64 mark;
    guint64 frames_first;
    guint64 frames_resumed;
    guint64 preview;
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

static guint64
b_frames_of_a (driver *d)
{
    return gtkhx_voice_runtime_video_frames_received (d->B->rt, d->A->htlc.uid,
                                                      HX_VIDEO_KIND_CAMERA);
}

static gboolean
driver_tick (gpointer data)
{
    driver *d = data;
    video_client *both[2] = { d->A, d->B };
    for (int i = 0; i < 2; i++) {
        video_client *c = both[i];
        guint16 peer = both[1 - i]->htlc.uid;
        while (integration_recv_message (c->fd, &c->htlc, 0)) {
            dispatch_frame (c, peer);
        }
    }

    gint64 now = g_get_monotonic_time ();
    switch (d->ph) {
    case PH_START:
        gtkhx_voice_runtime_join (d->A->rt, 0);
        gtkhx_voice_runtime_mute (d->A->rt, 1);
        d->ph = PH_WAIT_A_CONNECTED;
        d->deadline = now + SECS (15);
        break;

    case PH_WAIT_A_CONNECTED:
        if (d->A->state == GTKHX_VOICE_STATE_CONNECTED) {
            gtkhx_voice_runtime_join (d->B->rt, 0);
            gtkhx_voice_runtime_mute (d->B->rt, 1);
            d->ph = PH_WAIT_B_CONNECTED;
            d->deadline = now + SECS (15);
        } else if (now >= d->deadline) {
            driver_fail (d,
                         "A never reached CONNECTED (state=%d); is UDP "
                         "to hxd-ng's voice port blocked?",
                         (int)d->A->state);
        }
        break;

    case PH_WAIT_B_CONNECTED:
        if (d->B->state == GTKHX_VOICE_STATE_CONNECTED) {
            gtkhx_voice_runtime_video_start (d->A->rt, HX_VIDEO_KIND_CAMERA);
            d->ph = PH_WAIT_CAMERA_LISTED;
            d->deadline = now + SECS (10);
        } else if (now >= d->deadline) {
            driver_fail (d, "B never reached CONNECTED (state=%d).",
                         (int)d->B->state);
        }
        break;

    case PH_WAIT_CAMERA_LISTED:
        if (!gtkhx_voice_runtime_video_publishing (d->A->rt,
                                                   HX_VIDEO_KIND_CAMERA)) {
            driver_fail (d, "A's camera publication was refused or failed.");
        } else if (d->B->peer_camera_listed) {
            /* Subscribe late, the way a user opens the panel some time
             * after a camera came on: A's opening keyframe is long gone,
             * so B only sees a picture if the server's keyframe request
             * reaches A's encoder and A answers it. */
            d->ph = PH_LATE_SUBSCRIBE;
            d->deadline = now + SECS (3);
        } else if (now >= d->deadline) {
            driver_fail (d, "B never saw A's camera in a Video Status.");
        }
        break;

    case PH_LATE_SUBSCRIBE:
        if (now >= d->deadline) {
            /* What the video panel does once shown: ask for what it will
             * display. */
            guint16 uid = d->A->htlc.uid, kind = HX_VIDEO_KIND_CAMERA;
            gtkhx_voice_runtime_video_subscribe (d->B->rt, &uid, &kind, 1);
            d->ph = PH_WAIT_FRAMES;
            d->deadline = now + SECS (20);
        }
        break;

    case PH_WAIT_FRAMES: {
        guint64 n = b_frames_of_a (d);
        if (n >= FRAME_MARGIN) {
            d->frames_first = n;
            d->preview = gtkhx_voice_runtime_video_frames_received (
                d->A->rt, 0, HX_VIDEO_KIND_CAMERA);
            gtkhx_voice_runtime_video_pause (d->A->rt, HX_VIDEO_KIND_CAMERA, 1);
            d->ph = PH_PAUSED_SETTLE;
            d->deadline = now + SECS (1);
        } else if (now >= d->deadline) {
            driver_fail (d,
                         "B decoded %" G_GUINT64_FORMAT
                         " frames of A's camera in 20 s (wanted %d).",
                         n, FRAME_MARGIN);
        }
        break;
    }

    case PH_PAUSED_SETTLE:
        /* Frames in flight when the pause landed still arrive. */
        if (now >= d->deadline) {
            d->mark = b_frames_of_a (d);
            d->ph = PH_PAUSED_QUIET;
            d->deadline = now + SECS (2);
        }
        break;

    case PH_PAUSED_QUIET:
        if (now >= d->deadline) {
            guint64 n = b_frames_of_a (d);
            /* Allow a straggler or two from the jitterbuffer. */
            if (n > d->mark + 2) {
                driver_fail (
                    d,
                    "B kept decoding A's paused camera: %" G_GUINT64_FORMAT
                    " -> %" G_GUINT64_FORMAT,
                    d->mark, n);
                break;
            }
            gtkhx_voice_runtime_video_pause (d->A->rt, HX_VIDEO_KIND_CAMERA, 0);
            d->mark = n;
            d->ph = PH_WAIT_RESUMED_FRAMES;
            d->deadline = now + SECS (15);
        }
        break;

    case PH_WAIT_RESUMED_FRAMES: {
        guint64 n = b_frames_of_a (d);
        if (n >= d->mark + FRAME_MARGIN) {
            d->frames_resumed = n;
            gtkhx_voice_runtime_video_stop (d->A->rt, HX_VIDEO_KIND_CAMERA);
            d->ph = PH_WAIT_STOPPED;
            d->deadline = now + SECS (10);
        } else if (now >= d->deadline) {
            driver_fail (d,
                         "B's frames of A did not resume: %" G_GUINT64_FORMAT
                         " -> %" G_GUINT64_FORMAT,
                         d->mark, n);
        }
        break;
    }

    case PH_WAIT_STOPPED:
        /* The stop turns B's section inactive; its receive leg and the
         * stream's frame slot go with it. */
        if (b_frames_of_a (d) == 0 && d->A->state == GTKHX_VOICE_STATE_CONNECTED
            && d->B->state == GTKHX_VOICE_STATE_CONNECTED) {
            /* The stop's renegotiation has settled on both sides. */
            g_main_loop_quit (d->loop); /* PASS */
        } else if (now >= d->deadline) {
            driver_fail (d, "B still holds A's camera stream after the stop.");
        }
        break;
    }
    return G_SOURCE_CONTINUE;
}

static void
test_video_media (void)
{
    g_setenv ("GTKHX_VOICE_TEST_AUDIO_SRC", "1", TRUE);
    g_setenv ("GTKHX_VOICE_TEST_VIDEO_SRC", "ball", TRUE);
    g_assert_cmpint (gtkhx_voice_init (), ==, 1);
    if (!gtkhx_voice_video_receive_available ()
        || !gtkhx_voice_video_publish_available (HX_VIDEO_KIND_CAMERA)) {
        g_test_fail_printf ("VP8 elements missing: install gst-plugins-good "
                            "(vpx) and gst-plugins-base (videotestsrc).");
        return;
    }

    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        g_test_fail_printf ("no video-capable server in the matrix.");
        return;
    }

    video_client A, B;
    client_reset (&A);
    client_reset (&B);
    if (!client_open (&A, "A", srv, 412) || !client_open (&B, "B", srv, 413)) {
        goto out;
    }

    driver d;
    memset (&d, 0, sizeof (d));
    d.A = &A;
    d.B = &B;
    d.loop = g_main_loop_new (NULL, FALSE);
    d.ph = PH_START;
    guint tick = g_timeout_add (5, driver_tick, &d);
    g_main_loop_run (d.loop);
    g_source_remove (tick);

    if (d.failed) {
        g_test_message ("driver: %s", d.failmsg);
        g_test_fail_printf ("%s", d.failmsg);
        goto leave;
    } else {
        g_test_message (
            "B decoded %" G_GUINT64_FORMAT " frames of A, %" G_GUINT64_FORMAT
            " by the end of the resume; A previewed %" G_GUINT64_FORMAT,
            d.frames_first, d.frames_resumed, d.preview);
        g_assert_cmpuint (d.preview, >, 0);
    }

    /* The answers: the microphone's SSRC in every one, and cam-send as
     * a sendonly section with its SSRC once A published. */
    g_assert_cmpint (A.answers_seen, >, 0);
    g_assert_cmpint (A.answers_with_send_ssrc, ==, A.answers_seen);
    g_assert_cmpint (B.answers_with_send_ssrc, ==, B.answers_seen);
    g_assert_cmpint (A.answers_with_cam_send, >, 0);
    g_assert_cmpint (B.answers_with_cam_send, ==, 0);
    /* Video never disturbed the calls. */
    g_assert_cmpint (A.state, ==, GTKHX_VOICE_STATE_CONNECTED);
    g_assert_cmpint (B.state, ==, GTKHX_VOICE_STATE_CONNECTED);

leave:
    gtkhx_voice_runtime_leave (B.rt, 0);
    gtkhx_voice_runtime_leave (A.rt, 0);
    for (int spin = 0; spin < 50; spin++) {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (2000);
    }
    g_main_loop_unref (d.loop);

out:
    client_close (&B);
    client_close (&A);
}

/* ------------------------------------------------------------------ */
/* One publisher, both kinds.                                          */
/*                                                                     */
/* A camera and a screen share from one participant are the same codec */
/* at the same payload type on the same bundled transport. The answer's */
/* a=ssrc per section is the only thing that tells them apart — at the */
/* server, which must not forward a desktop into a face's tile, and at */
/* the receiver, whose webrtcbin must deliver each on the right leg.   */
/* B subscribes to both and must decode both, as two streams.          */
/* ------------------------------------------------------------------ */

typedef enum {
    DPH_START,
    DPH_WAIT_A,
    DPH_WAIT_B,
    DPH_WAIT_LISTED,
    DPH_WAIT_FRAMES,
} dual_phase;

typedef struct {
    video_client *A, *B;
    GMainLoop *loop;
    dual_phase ph;
    gint64 deadline;
    gboolean failed;
    gchar failmsg[256];
    gboolean screen_listed;
} dual_driver;

static void
dual_fail (dual_driver *d, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    g_vsnprintf (d->failmsg, sizeof (d->failmsg), fmt, ap);
    va_end (ap);
    d->failed = TRUE;
    g_main_loop_quit (d->loop);
}

static gboolean
dual_tick (gpointer data)
{
    dual_driver *d = data;
    video_client *both[2] = { d->A, d->B };
    for (int i = 0; i < 2; i++) {
        video_client *c = both[i];
        guint16 peer = both[1 - i]->htlc.uid;
        while (integration_recv_message (c->fd, &c->htlc, 0)) {
            if (c == d->B && hdr_type (&c->htlc) == HTLS_HDR_VIDEO_STATUS) {
                struct gtkhx_proto_video_reply r;
                gtkhx_proto_parse_video_reply (hx_test_in (&c->htlc)->buf,
                                               hx_test_in (&c->htlc)->pos, &r);
                d->screen_listed = FALSE;
                for (gsize at = 0;
                     r.publishers_ptr && at + 8 <= r.publishers_len; at += 8) {
                    const guint8 *e = r.publishers_ptr + at;
                    if ((guint16)((e[0] << 8) | e[1]) == peer
                        && (guint16)((e[2] << 8) | e[3])
                               == HX_VIDEO_KIND_SCREEN) {
                        d->screen_listed = TRUE;
                    }
                }
            }
            dispatch_frame (c, peer);
        }
    }

    gint64 now = g_get_monotonic_time ();
    guint16 a = d->A->htlc.uid;
    switch (d->ph) {
    case DPH_START:
        gtkhx_voice_runtime_join (d->A->rt, 0);
        gtkhx_voice_runtime_mute (d->A->rt, 1);
        d->ph = DPH_WAIT_A;
        d->deadline = now + SECS (15);
        break;
    case DPH_WAIT_A:
        if (d->A->state == GTKHX_VOICE_STATE_CONNECTED) {
            gtkhx_voice_runtime_join (d->B->rt, 0);
            gtkhx_voice_runtime_mute (d->B->rt, 1);
            d->ph = DPH_WAIT_B;
            d->deadline = now + SECS (15);
        } else if (now >= d->deadline) {
            dual_fail (d, "A never reached CONNECTED");
        }
        break;
    case DPH_WAIT_B:
        if (d->B->state == GTKHX_VOICE_STATE_CONNECTED) {
            gtkhx_voice_runtime_video_start (d->A->rt, HX_VIDEO_KIND_CAMERA);
            gtkhx_voice_runtime_video_start (d->A->rt, HX_VIDEO_KIND_SCREEN);
            d->ph = DPH_WAIT_LISTED;
            d->deadline = now + SECS (10);
        } else if (now >= d->deadline) {
            dual_fail (d, "B never reached CONNECTED");
        }
        break;
    case DPH_WAIT_LISTED:
        if (d->B->peer_camera_listed && d->screen_listed) {
            guint16 uids[2] = { a, a };
            guint16 kinds[2] = { HX_VIDEO_KIND_CAMERA, HX_VIDEO_KIND_SCREEN };
            gtkhx_voice_runtime_video_subscribe (d->B->rt, uids, kinds, 2);
            d->ph = DPH_WAIT_FRAMES;
            d->deadline = now + SECS (20);
        } else if (now >= d->deadline) {
            dual_fail (d,
                       "B never saw both of A's publications (camera %d, "
                       "screen %d)",
                       d->B->peer_camera_listed, d->screen_listed);
        }
        break;
    case DPH_WAIT_FRAMES: {
        guint64 cam = gtkhx_voice_runtime_video_frames_received (
            d->B->rt, a, HX_VIDEO_KIND_CAMERA);
        guint64 scr = gtkhx_voice_runtime_video_frames_received (
            d->B->rt, a, HX_VIDEO_KIND_SCREEN);
        if (cam >= FRAME_MARGIN && scr >= FRAME_MARGIN) {
            /* Both sources draw the same test pattern, so the streams are
             * told apart by shape: a camera encodes at 640x480, a screen
             * at the rig's whole 1920x1080 ceiling. A server that crossed
             * the sections would swap them. */
            guint32 cw = 0, ch = 0, sw = 0, sh = 0;
            gtkhx_voice_runtime_video_frame_size (
                d->B->rt, a, HX_VIDEO_KIND_CAMERA, &cw, &ch);
            gtkhx_voice_runtime_video_frame_size (
                d->B->rt, a, HX_VIDEO_KIND_SCREEN, &sw, &sh);
            g_test_message ("B decoded %" G_GUINT64_FORMAT
                            " camera frames at %ux%u and %" G_GUINT64_FORMAT
                            " screen frames at %ux%u",
                            cam, cw, ch, scr, sw, sh);
            if (cw != 640 || ch != 480 || sw != 1920 || sh != 1080) {
                dual_fail (d,
                           "B's camera is %ux%u and screen %ux%u; expected "
                           "640x480 and 1920x1080",
                           cw, ch, sw, sh);
            } else {
                g_main_loop_quit (d->loop); /* PASS */
            }
        } else if (now >= d->deadline) {
            dual_fail (d,
                       "B decoded %" G_GUINT64_FORMAT
                       " camera and %" G_GUINT64_FORMAT
                       " screen frames of A in 20 s",
                       cam, scr);
        }
        break;
    }
    }
    return G_SOURCE_CONTINUE;
}

static void
test_video_camera_and_screen (void)
{
    g_setenv ("GTKHX_VOICE_TEST_AUDIO_SRC", "1", TRUE);
    g_setenv ("GTKHX_VOICE_TEST_VIDEO_SRC", "ball", TRUE);
    g_assert_cmpint (gtkhx_voice_init (), ==, 1);
    const hx_test_server *srv = pick_video_server ();
    if (!srv) {
        g_test_fail_printf ("no video-capable server in the matrix.");
        return;
    }
    video_client A, B;
    client_reset (&A);
    client_reset (&B);
    if (!client_open (&A, "A2", srv, 412)
        || !client_open (&B, "B2", srv, 413)) {
        goto out;
    }
    dual_driver d;
    memset (&d, 0, sizeof (d));
    d.A = &A;
    d.B = &B;
    d.loop = g_main_loop_new (NULL, FALSE);
    guint tick = g_timeout_add (5, dual_tick, &d);
    g_main_loop_run (d.loop);
    g_source_remove (tick);
    if (d.failed) {
        g_test_message ("driver: %s", d.failmsg);
        g_test_fail_printf ("%s", d.failmsg);
    }
    gtkhx_voice_runtime_leave (B.rt, 0);
    gtkhx_voice_runtime_leave (A.rt, 0);
    for (int spin = 0; spin < 50; spin++) {
        g_main_context_iteration (NULL, FALSE);
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
    g_test_add_func ("/integration/video/media", test_video_media);
    g_test_add_func ("/integration/video/camera_and_screen",
                     test_video_camera_and_screen);
    return g_test_run ();
}
