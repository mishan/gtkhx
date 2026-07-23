/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/test_inline_media.c — Tier 3 coverage for the
 * fogWraith inline-media extension (Capabilities-Inline-Media.md).
 *
 * Per docs/inline-media-plan.md Phase 9.F:
 *
 *   cap_negotiation       — Janus echoes CAP_INLINE_MEDIA in the
 *                           LOGIN TASK reply.
 *   limits_advertised     — at least one DATA_CHAT_MEDIA_MAX_*
 *                           advisory field is set on the LOGIN
 *                           reply.
 *   upload_round_trip     — single-shot TranUploadMedia (750) with
 *                           a hand-crafted minimal PNG; assert the
 *                           reply carries CHAT_MEDIA_ID +
 *                           CHAT_MEDIA_TYPE + canonical width /
 *                           height (and the canonical bytes ≤ the
 *                           uploaded byte count, since the server
 *                           re-encodes).
 *   chat_with_media_round_trip — upload a PNG, attach the handle
 *                           on a TranChatSend, observe the relayed
 *                           broadcast carrying CHAT_MEDIA_ID +
 *                           CHAT_MEDIA_TYPE companion fields.
 *   download_round_trip   — fetch the canonical bytes via
 *                           TranDownloadMedia (751); verify the
 *                           reply carries CHAT_MEDIA_PAYLOAD + a
 *                           canonical PNG magic header.
 *   oversized_rejected    — upload an invalid garbage payload
 *                           (65,500 zero bytes — large enough
 *                           to exercise the single-shot wire
 *                           framing limit but well below the
 *                           256 KiB MAX_BYTES default, so the
 *                           server's magic-byte sniff is the
 *                           likely rejection branch). Assert
 *                           task-error + any present
 *                           CHAT_MEDIA_ERROR_CODE falls inside
 *                           the documented 0–5 range. Originally
 *                           scoped to a TooLarge probe; the
 *                           server's actual rejection path
 *                           (UnsupportedFormat for all-zeros)
 *                           is also a valid 'server rejected
 *                           with an actionable code' result.
 *   unauthorized_download — try to fetch a handle we never
 *                           received; expect a generic task-error
 *                           (the spec collapses "not authorized"
 *                           and "expired" to avoid handle-
 *                           enumeration).
 *
 * Failures here imply Janus's implementation has regressed or
 * GtkHx's wire shape has drifted. The cap_negotiation case stays
 * the canary for "did the extension light up at all" before any
 * of the round-trips have a hope of working.
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "compat.h"   /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "integration_harness.h"
#include "server_matrix.h"

/* Process-unique chat marker so a test can find its own relayed chat
 * among the parallel Tier 3 binaries' traffic — and so the drain works
 * on Janus, whose HTLS_HDR_CHAT broadcasts carry uid 0 (no sender
 * stamp). Mirrors make_marker in test_chat_history. */
static void
im_make_marker (char *out, gsize cap)
{
    guint64 r = (((guint64) g_random_int ()) << 32) ^ (guint64) g_random_int ();
    r ^= ((guint64) getpid () << 16) ^ (guint64) time (NULL);
    g_snprintf (out, cap, "HX-%016" G_GINT64_MODIFIER "x", r);
}

/* hlwrite stays stubbed — production async-write entry that never
 * makes sense in a Tier 3 binary. Same shape as test_chat_history.c. */
void
hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
    (void) htlc;
    (void) type;
    (void) flag;
    (void) hc;
    g_assert_not_reached ();
}

/* Pick the first inline-media-capable server in the matrix, or
 * NULL if the env-var filter excluded every entry. Caller fails
 * loud (no skips per the no-silent-skip rule). */
static const hx_test_server *
pick_inline_media_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_INLINE_MEDIA);
    if (!servers) {
        return NULL;
    }
    const hx_test_server *srv = NULL;
    if (servers->len > 0) {
        srv = g_ptr_array_index (servers, 0);
    }
    g_ptr_array_unref (servers);
    return srv;
}

static void
close_session (int fd, struct htlc_conn *htlc)
{
    integration_release_htlc (htlc);
    integration_close (fd);
}

/* ------------------------------------------------------------------ */
/* Runtime PNG fixture                                                  */
/* ------------------------------------------------------------------ */

/* Encode a small in-memory PNG via GdkPixbuf so we get a
 * well-formed file every PNG decoder accepts. An earlier
 * revision of this test embedded a hand-crafted 1×1 PNG byte
 * sequence; it parsed cleanly in GdkPixbuf locally but Janus's
 * Go image library rejected it on CI (presumably stricter
 * about zlib window size or chunk-CRC details). Encoding at
 * runtime sidesteps the whole question: gdk-pixbuf's PNG
 * encoder produces the standard 'normal' shape that every
 * decoder in the wild handles.
 *
 * Returns a freshly-allocated GBytes; caller g_bytes_unref's.
 * The dimensions are kept tiny (4×4) so the encoded payload
 * sits comfortably under every advisory cap the spec mentions. */
static GBytes *
encode_test_png (void)
{
    GdkPixbuf *pix
        = gdk_pixbuf_new (GDK_COLORSPACE_RGB, /*has_alpha=*/FALSE,
                          /*bits_per_sample=*/8, /*width=*/4, /*height=*/4);
    if (!pix) {
        return NULL;
    }
    /* Fill with solid red — content doesn't matter, only that
	 * the bytes round-trip through the server's re-encode. */
    gdk_pixbuf_fill (pix, 0xFF0000FF);

    gchar *buf = NULL;
    gsize bufsz = 0;
    GError *err = NULL;
    gboolean ok = gdk_pixbuf_save_to_buffer (pix, &buf, &bufsz, "png", &err,
                                             NULL);
    g_object_unref (pix);
    if (!ok || !buf) {
        g_clear_error (&err);
        return NULL;
    }
    return g_bytes_new_take (buf, bufsz);
}

/* Encode a PNG whose payload is guaranteed to exceed `min_bytes`
 * after PNG's deflate stage. We fill the pixbuf with a
 * deterministic but non-trivially-compressible pattern (a
 * pseudo-random byte sequence) so the encoded payload grows in
 * step with the pixel count. Doubling the dimensions until the
 * encoded size clears the threshold lets us tolerate variation
 * across gdk-pixbuf builds without hard-coding pixel counts.
 *
 * Used by the chunked-upload test to push past Janus's advertised
 * CHAT_MEDIA_CHUNK_SIZE without exceeding the 256 KiB MAX_BYTES
 * default. */
static GBytes *
encode_chunky_png (gsize min_bytes)
{
    /* Start at 128×128. Each step doubles linear → 4× pixel
	 * count → encoded size roughly quadruples on a noise pattern.
	 * Bail at 1024×1024 (~3 MB raw → typically > 1 MB encoded)
	 * since anything larger is clearly past the spec MAX_BYTES
	 * default and we'd have nothing to test. */
    int dim = 128;
    while (dim <= 1024) {
        GdkPixbuf *pix = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
                                         /*has_alpha=*/FALSE,
                                         /*bits_per_sample=*/8, dim, dim);
        if (!pix) {
            return NULL;
        }
        /* Fill with a noise pattern that PNG can't deflate away.
		 * Per-pixel random RGB triples seeded deterministically so
		 * the test is reproducible.
		 *
		 * GdkPixbuf rows can carry trailing padding bytes (rowstride
		 * > width * n_channels for alignment), so walk rowstride x
		 * height not width x height x channels — otherwise the
		 * padding bytes stay uninitialised and the encoded payload
		 * gains a stochastic dependency on whatever the allocator
		 * left behind, which both breaks reproducibility and could
		 * leave us below the encoded-size threshold the caller
		 * asked for. */
        guchar *p = gdk_pixbuf_get_pixels (pix);
        gsize rowstride = (gsize) gdk_pixbuf_get_rowstride (pix);
        gsize n_bytes = rowstride * (gsize) dim;
        guint32 r = 0xc0ffee01;
        for (gsize i = 0; i < n_bytes; i++) {
            /* xorshift32 — cheap, good entropy for compressor. */
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            p[i] = (guchar) (r & 0xff);
        }
        gchar *buf = NULL;
        gsize bufsz = 0;
        GError *err = NULL;
        gboolean ok = gdk_pixbuf_save_to_buffer (pix, &buf, &bufsz, "png",
                                                 &err, NULL);
        g_object_unref (pix);
        if (!ok || !buf) {
            g_clear_error (&err);
            return NULL;
        }
        if (bufsz > min_bytes) {
            return g_bytes_new_take (buf, bufsz);
        }
        g_free (buf);
        dim *= 2;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Wire helpers — TranUploadMedia / TranDownloadMedia /                 */
/* TranChatSend-with-media. Stitched directly on top of the harness    */
/* primitive integration_send_message so the test can drive            */
/* deliberately-malformed shapes too (oversized payload, missing       */
/* PART_FINAL, …) without per-shape special-casing; the well-formed    */
/* shape matches production via the same wire spec the Phase 9.A FFI    */
/* builders pin in src/inline_media_upload.c. */
/* ------------------------------------------------------------------ */

/* Send a single-shot TranUploadMedia (750). Returns the trans id
 * the server should echo in its TASK reply, or 0 on send failure.
 *
 * Inlines the chunk shape rather than calling the Phase 9.A
 * builder via FFI — the test wants to be able to inject malformed
 * shapes too (oversized payload, missing PART_FINAL, etc.) without
 * having to special-case each. The wire shape is documented in
 * docs/inline-media-plan.md and exercised by the test suite. */
static guint32
send_upload_media (int fd, struct htlc_conn *htlc,
                   const guint8 *payload, gsize payload_len,
                   const char *declared_type)
{
    guint8 part_final = 1;
    guint32 trans_before = htlc->trans;
    gboolean ok;
    if (declared_type && *declared_type) {
        ok = integration_send_message (
            fd, htlc, HTLC_HDR_UPLOAD_MEDIA, /*flag=*/0, /*hc=*/3,
            (int) HTLC_DATA_CHAT_MEDIA_PAYLOAD, (int) payload_len, payload,
            (int) HTLC_DATA_CHAT_MEDIA_DECLARED_TYPE,
            (int) strlen (declared_type), (const guint8 *) declared_type,
            (int) HTLC_DATA_CHAT_MEDIA_PART_FINAL, 1, &part_final);
    } else {
        ok = integration_send_message (
            fd, htlc, HTLC_HDR_UPLOAD_MEDIA, /*flag=*/0, /*hc=*/2,
            (int) HTLC_DATA_CHAT_MEDIA_PAYLOAD, (int) payload_len, payload,
            (int) HTLC_DATA_CHAT_MEDIA_PART_FINAL, 1, &part_final);
    }
    if (!ok) {
        return 0;
    }
    /* hlpack increments htlc->trans after assignment, so the
	 * trans the wire carried is the pre-call value. */
    return trans_before;
}

/* Send a chunked TranUploadMedia (750). Drives the multi-step
 * send-then-receive state machine inline, but with all the
 * chunk-shape construction (PART_INDEX endianness, PART_COUNT,
 * PART_FINAL toggling, which chunks belong on the first vs the
 * follow-ups) deferred to the same hotline-proto builders the
 * production helper uses — the test stays a pure orchestration
 * loop and doesn't reimplement protocol decisions.
 *
 * Returns the trans id of the FINAL chunk (the one whose reply
 * carries the canonical handle). Returns 0 on any send/recv
 * failure. On success the final reply is left in hx_test_in(htlc)->buf
 * for the caller to parse via gtkhx_proto_parse_upload_final_reply. */
static guint32
send_upload_media_chunked (int fd, struct htlc_conn *htlc,
                           const guint8 *payload, gsize payload_len,
                           guint32 chunk_size, const char *declared_type)
{
    if (chunk_size == 0 || payload_len <= chunk_size) {
        return 0;
    }
    guint64 pc = ((guint64) payload_len + chunk_size - 1) / chunk_size;
    if (pc < 2 || pc > G_MAXUINT16) {
        return 0;
    }
    guint16 part_count = (guint16) pc;

    /* Chunk 0 — production first-chunk builder. */
    struct hx_chunk first_chunks[5];
    guint8 first_scratch[5];
    int32_t first_hc = gtkhx_proto_build_upload_media_first_chunks (
        payload, chunk_size,
        (const uint8_t *) (declared_type && *declared_type ? declared_type
                                                           : NULL),
        declared_type && *declared_type ? strlen (declared_type) : 0,
        part_count, first_chunks, G_N_ELEMENTS (first_chunks), first_scratch,
        sizeof (first_scratch));
    if (first_hc <= 0) {
        return 0;
    }
    if (!integration_send_chunks (fd, htlc, HTLC_HDR_UPLOAD_MEDIA, 0,
                                  first_chunks, first_hc)) {
        return 0;
    }
    guint32 chunk0_trans = htlc->trans - 1;
    if (!integration_drain_until_task_trans (fd, htlc, chunk0_trans, 16)) {
        return 0;
    }
    if (gtkhx_proto_header_in_error (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)) {
        return 0;
    }
    const guint8 *tok_ptr = NULL;
    size_t tok_len = 0;
    if (!gtkhx_proto_parse_upload_token_reply (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos,
                                               &tok_ptr, &tok_len)
        || tok_len == 0) {
        return 0;
    }
    /* Token bytes borrow into hx_test_in(htlc)->buf; copy them so subsequent
	 * recvs into the same buffer don't pull the rug out. */
    guint8 *token = g_memdup2 (tok_ptr, tok_len);
    gsize token_len = tok_len;
    guint32 last_trans = 0;

    /* Follow-ups via the production follow-up builder. */
    for (guint16 i = 1; i < part_count; i++) {
        gsize off = (gsize) i * (gsize) chunk_size;
        gsize remaining = payload_len > off ? payload_len - off : 0;
        gsize this_len
            = remaining < (gsize) chunk_size ? remaining : (gsize) chunk_size;

        struct hx_chunk fu_chunks[4];
        guint8 fu_scratch[3];
        int32_t fu_hc = gtkhx_proto_build_upload_media_followup_chunks (
            token, token_len, payload + off, this_len, i,
            /*final_chunk=*/(i + 1 == part_count), fu_chunks,
            G_N_ELEMENTS (fu_chunks), fu_scratch, sizeof (fu_scratch));
        if (fu_hc <= 0) {
            g_free (token);
            return 0;
        }
        if (!integration_send_chunks (fd, htlc, HTLC_HDR_UPLOAD_MEDIA, 0,
                                      fu_chunks, fu_hc)) {
            g_free (token);
            return 0;
        }
        guint32 trans = htlc->trans - 1;
        if (!integration_drain_until_task_trans (fd, htlc, trans, 32)) {
            g_free (token);
            return 0;
        }
        if (gtkhx_proto_header_in_error (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)) {
            g_free (token);
            return 0;
        }
        last_trans = trans;
    }
    g_free (token);
    return last_trans;
}

/* Send a TranChatSend (105) with optional CHAT_MEDIA_ID + TYPE
 * companion chunks attached. Style = 1 (normal); no CHAT_ID
 * means public chat. */
static gboolean
send_chat_with_media (int fd, struct htlc_conn *htlc, const char *text,
                      const guint8 *handle, gsize handle_len,
                      const char *mime)
{
    guint16 style = g_htons(1);
    if (handle && handle_len > 0 && mime) {
        return integration_send_message (
            fd, htlc, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/4,
            (int) HTLC_DATA_STYLE, (int) sizeof (style), &style,
            (int) HTLC_DATA_CHAT, (int) strlen (text), text,
            (int) HTLC_DATA_CHAT_MEDIA_ID, (int) handle_len, handle,
            (int) HTLC_DATA_CHAT_MEDIA_TYPE, (int) strlen (mime), mime);
    }
    return integration_send_message (
        fd, htlc, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/2,
        (int) HTLC_DATA_STYLE, (int) sizeof (style), &style,
        (int) HTLC_DATA_CHAT, (int) strlen (text), text);
}

/* Send TranDownloadMedia (751) for a known handle. Returns the
 * trans id (pre-call htlc->trans). */
static guint32
send_download_media (int fd, struct htlc_conn *htlc,
                     const guint8 *handle, gsize handle_len)
{
    guint32 trans_before = htlc->trans;
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_DOWNLOAD_MEDIA, /*flag=*/0, /*hc=*/1,
            (int) HTLC_DATA_CHAT_MEDIA_ID, (int) handle_len, handle)) {
        return 0;
    }
    return trans_before;
}

/* Send TranDownloadMedia (751) for a specific chunk by index.
 * Routes through the production Rust builder so the wire shape
 * (the CHAT_MEDIA_PART_INDEX presence rule on follow-ups) stays
 * pinned to one source of truth. */
static guint32
send_download_media_part (int fd, struct htlc_conn *htlc,
                          const guint8 *handle, gsize handle_len,
                          guint16 part_index)
{
    struct hx_chunk chunks[2];
    guint8 scratch[2];
    int32_t hc = gtkhx_proto_build_download_media_chunks (
        handle, handle_len, part_index,
        /*part_index_present=*/true, chunks, G_N_ELEMENTS (chunks), scratch,
        sizeof (scratch));
    if (hc <= 0) {
        return 0;
    }
    guint32 trans_before = htlc->trans;
    if (!integration_send_chunks (fd, htlc, HTLC_HDR_DOWNLOAD_MEDIA, 0, chunks,
                                  hc)) {
        return 0;
    }
    return trans_before;
}

/* Drain helpers come from the harness:
 *   integration_drain_until_task_trans — TASK reply filter
 *   integration_drain_until_chat       — relayed-broadcast filter
 * Both centralised in tests/integration/integration_harness.c. */

/* ------------------------------------------------------------------ */
/* Test cases                                                          */
/* ------------------------------------------------------------------ */

/*
 * Capability negotiation: client advertises CAP_INLINE_MEDIA in
 * the LOGIN; spec-compliant server echoes the bit in the LOGIN
 * TASK reply, which the harness captures into htlc->caps. Janus
 * is currently the only matrix entry with HX_TEST_CAP_INLINE_MEDIA;
 * if this assertion fails we know Janus doesn't actually implement
 * the extension yet and Phase 9.F's mock-server fallback path
 * needs to be taken (see docs/inline-media-plan.md).
 */
static void
test_inline_media_cap_negotiation (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf (
            "no inline-media-capable server in matrix "
            "(GTKHX_TEST_SERVERS filter excluded all). Phase 9.F "
            "expects Janus to ship CAP_INLINE_MEDIA — confirm "
            "the GTKHX_TEST_SERVERS env var includes 'vespernet' "
            "or rebuild with the default matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "InlineMedia CapNeg", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }

    /* The drain helper inside the harness stashed any
	 * HTLS_DATA_CAPABILITIES echo into htlc.caps. We asked for
	 * the inline-media bit; the server is meant to confirm it.
	 * If this fails, Janus isn't yet implementing the extension
	 * and the Phase 9.F path that builds a Go mock server kicks
	 * in (per docs/inline-media-plan.md). The failure mode is a
	 * clean assertion, not a silent skip. */
    g_assert_cmphex ((htlc.caps & HTLC_CAP_INLINE_MEDIA), ==,
                     HTLC_CAP_INLINE_MEDIA);

    close_session (fd, &htlc);
}

/*
 * Advisory-limits parsing: when the server confirms the cap it
 * MUST also include the DATA_CHAT_MEDIA_MAX_* fields alongside.
 * The harness's LOGIN drain walks these into htlc->media_max_*
 * (the same rcv.c code path the production client uses, via the
 * shared chunk walker). Verify at least one of them is non-zero
 * — the spec lets the server pick which to advertise, but a
 * server that says "I implement this extension" and then
 * advertises zero advisory fields is misbehaving in a way we
 * want to know about.
 *
 * Each field is independently optional per spec, so we don't
 * assert any single field is present; only that the server
 * advertised at least one (the production client falls back to
 * HX_MEDIA_DEFAULT_* per-field when zero — but if all are zero,
 * the server skipped the advertisement entirely which the spec
 * forbids when the cap is confirmed).
 */
static void
test_inline_media_advisory_limits_advertised (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf (
            "no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "InlineMedia Limits", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }
    g_assert_cmphex ((htlc.caps & HTLC_CAP_INLINE_MEDIA), ==,
                     HTLC_CAP_INLINE_MEDIA);

    /* At least one advisory field must be set. Spec quote:
	 *
	 *   "When the server confirms CAPABILITY_INLINE_MEDIA in the
	 *    login reply it MUST also include the advisory limit
	 *    fields below alongside DATA_CAPABILITIES."
	 *
	 * A conformant server picks which subset to send (the
	 * "Clients MUST tolerate any individual field being absent"
	 * sentence covers the per-field optionality) but cannot
	 * advertise ZERO of them while echoing the cap.
	 */
    gboolean any = (htlc.media_max_bytes != 0)
                   || (htlc.media_max_dimension != 0)
                   || (htlc.media_max_pixels != 0)
                   || (htlc.media_chunk_size != 0)
                   || (htlc.media_max_frames != 0)
                   || (htlc.media_max_duration_ms != 0);
    g_assert_true (any);

    /* Sanity: any advertised cap should be plausibly within
	 * spec-recommended order of magnitude. We don't enforce
	 * tight bounds — operators can tighten — but a server that
	 * said "max_bytes = 1 byte" or "max_dimension = 8 pixels"
	 * is clearly misconfigured and we'd want a CI red flag.
	 *
	 * Lower bounds: byte cap ≥ 1 KB, dimension ≥ 64 pixels,
	 * pixels ≥ 4096 (64×64), chunk ≥ 256 B, frames ≥ 1,
	 * duration ≥ 100 ms. */
    if (htlc.media_max_bytes != 0) {
        g_assert_cmpuint (htlc.media_max_bytes, >=, 1024u);
    }
    if (htlc.media_max_dimension != 0) {
        g_assert_cmpuint (htlc.media_max_dimension, >=, 64u);
    }
    if (htlc.media_max_pixels != 0) {
        g_assert_cmpuint (htlc.media_max_pixels, >=, 4096u);
    }
    if (htlc.media_chunk_size != 0) {
        g_assert_cmpuint (htlc.media_chunk_size, >=, 256u);
    }
    if (htlc.media_max_frames != 0) {
        g_assert_cmpuint (htlc.media_max_frames, >=, 1u);
    }
    if (htlc.media_max_duration_ms != 0) {
        g_assert_cmpuint (htlc.media_max_duration_ms, >=, 100u);
    }

    close_session (fd, &htlc);
}

/*
 * End-to-end upload round-trip: send a minimal valid PNG via
 * TranUploadMedia (single-shot), assert the TASK reply carries
 * a non-empty CHAT_MEDIA_ID + CHAT_MEDIA_TYPE + canonical
 * width/height. The server may re-encode the bytes (PNG with
 * alpha vs. without; quality adjustments) so we don't assert on
 * the canonical byte count — only that the structural fields
 * came back.
 */
static void
test_inline_media_upload_round_trip (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Upload RT", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }
    g_assert_cmphex ((htlc.caps & HTLC_CAP_INLINE_MEDIA), ==,
                     HTLC_CAP_INLINE_MEDIA);

    GBytes *png = encode_test_png ();
    g_assert_nonnull (png);
    gsize png_len = 0;
    const guint8 *png_data = g_bytes_get_data (png, &png_len);
    guint32 trans = send_upload_media (fd, &htlc, png_data, png_len,
                                       "image/png");
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));
    g_bytes_unref (png);

    /* TASK header flag bit 0 (task-error) must be clear on
	 * success. */
    g_assert_false (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    struct gtkhx_proto_upload_final_reply reply;
    g_assert_true (gtkhx_proto_parse_upload_final_reply (
        hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &reply));

    g_assert_cmpuint (reply.id_len, >, 0);
    g_assert_cmpuint (reply.type_len, >, 0);
    /* Canonical MIME should be in our allowlist (PNG/JPEG/GIF).
	 * Janus typically passes PNG through; pin the substring
	 * rather than equality so a server that adds "; charset" or
	 * similar still passes. */
    char mime_buf[64] = {0};
    gsize mime_len = MIN (reply.type_len, sizeof (mime_buf) - 1);
    memcpy (mime_buf, reply.type_ptr, mime_len);
    g_assert (g_str_has_prefix (mime_buf, "image/"));

    /* Canonical width / height must be present AND equal to 4×4
	 * (the dimensions encode_test_png writes). The spec marks
	 * these advisory for the CHAT relay companion fields but
	 * the upload final-reply is documented as carrying them —
	 * and Janus does — so a regression that drops them is a
	 * real round-trip break, not just a missing hint. The
	 * previous 'if present' gate would let such a regression
	 * slip through silently. */
    g_assert_true (reply.width_present);
    g_assert_cmpuint (reply.width, ==, 4);
    g_assert_true (reply.height_present);
    g_assert_cmpuint (reply.height, ==, 4);

    close_session (fd, &htlc);
}

/*
 * Chunked upload round-trip: send a PNG large enough to cross the
 * server-advertised CHAT_MEDIA_CHUNK_SIZE, driving the multi-step
 * UPLOAD_TOKEN echo state machine. Assert the final reply carries
 * a valid handle + canonical MIME, same shape as the single-shot
 * round-trip.
 */
static void
test_inline_media_chunked_upload_round_trip (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Chunked RT", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }
    g_assert_cmphex ((htlc.caps & HTLC_CAP_INLINE_MEDIA), ==,
                     HTLC_CAP_INLINE_MEDIA);

    /* Match the production clamp in inline_media_chunk_size: the
	 * server-advertised value tops out at HX_MEDIA_DEFAULT_CHUNK_SIZE
	 * so a hostile or misconfigured server can't push the client
	 * into outsized per-chunk allocations. Mirroring the clamp
	 * here keeps the test's chunk-size match production framing
	 * decisions; without it, an outlier advertisement could push
	 * us toward a >1 MiB PNG (or out the encode_chunky_png 1024²
	 * bail-out) for no reason production would ever hit. */
    guint32 chunk_size = htlc.media_chunk_size ? htlc.media_chunk_size
                                               : HX_MEDIA_DEFAULT_CHUNK_SIZE;
    if (chunk_size > HX_MEDIA_DEFAULT_CHUNK_SIZE) {
        chunk_size = HX_MEDIA_DEFAULT_CHUNK_SIZE;
    }
    /* Build a PNG that comfortably exceeds chunk_size so the
	 * test exercises ≥ 2 chunks. Asking for chunk_size + 8 KiB
	 * worth of encoded bytes leaves enough headroom for the
	 * actual encoder's compression efficiency to wobble without
	 * dropping below the threshold. */
    GBytes *png = encode_chunky_png ((gsize) chunk_size + 8 * 1024);
    g_assert_nonnull (png);
    gsize png_len = 0;
    const guint8 *png_data = g_bytes_get_data (png, &png_len);
    g_assert_cmpuint (png_len, >, chunk_size);

    guint32 final_trans = send_upload_media_chunked (
        fd, &htlc, png_data, png_len, chunk_size, "image/png");
    g_assert_cmpuint (final_trans, !=, 0);
    /* The drain loop inside the helper has already validated the
	 * final reply is non-error and left it parsed in htlc.in. */
    struct gtkhx_proto_upload_final_reply reply;
    g_assert_true (gtkhx_proto_parse_upload_final_reply (
        hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &reply));
    g_assert_cmpuint (reply.id_len, >, 0);
    g_assert_cmpuint (reply.type_len, >, 0);
    char mime_buf[64] = {0};
    gsize mime_len = MIN (reply.type_len, sizeof (mime_buf) - 1);
    memcpy (mime_buf, reply.type_ptr, mime_len);
    g_assert (g_str_has_prefix (mime_buf, "image/"));

    g_bytes_unref (png);
    close_session (fd, &htlc);
}

/*
 * Chunked-upload over the server's MAX_BYTES cap: send a noise
 * PNG whose total length exceeds the advertised
 * CHAT_MEDIA_MAX_BYTES, and assert the chunked state machine
 * lands on a task-error with the spec PayloadTooLarge (1) code.
 *
 * Rejection can land on any chunk reply (Janus may pre-flight
 * via PART_COUNT * chunk_size, or accumulate and reject the
 * canonicalise step at the final chunk). The send helper
 * returns 0 the moment a task-error reply arrives, leaving
 * the error reply in hx_test_in(&htlc)->buf for us to inspect.
 */
static void
test_inline_media_chunked_upload_too_large (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Chunked TooBig", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }
    g_assert_cmphex ((htlc.caps & HTLC_CAP_INLINE_MEDIA), ==,
                     HTLC_CAP_INLINE_MEDIA);

    /* Mirror inline_media_chunk_size's production clamp. */
    guint32 chunk_size = htlc.media_chunk_size ? htlc.media_chunk_size
                                               : HX_MEDIA_DEFAULT_CHUNK_SIZE;
    if (chunk_size > HX_MEDIA_DEFAULT_CHUNK_SIZE) {
        chunk_size = HX_MEDIA_DEFAULT_CHUNK_SIZE;
    }
    /* Resolve the server's advertised MAX_BYTES ceiling. We need
	 * a PNG whose encoded form sits above that — encode_chunky_png
	 * grows by powers of two so add a generous headroom margin to
	 * land cleanly past the threshold. */
    guint32 max_bytes = htlc.media_max_bytes ? htlc.media_max_bytes
                                             : HX_MEDIA_DEFAULT_MAX_BYTES;
    GBytes *png = encode_chunky_png ((gsize) max_bytes + 64 * 1024);
    g_assert_nonnull (png);
    gsize png_len = 0;
    const guint8 *png_data = g_bytes_get_data (png, &png_len);
    g_assert_cmpuint (png_len, >, max_bytes);

    /* Send. Either the helper returns 0 (rejection at some chunk
	 * reply, error left in hx_test_in(&htlc)->buf) or we have a server bug. */
    guint32 final_trans = send_upload_media_chunked (
        fd, &htlc, png_data, png_len, chunk_size, "image/png");
    g_bytes_unref (png);
    g_assert_cmpuint (final_trans, ==, 0);

    /* The error reply that aborted the chunked loop is sitting in
	 * hx_test_in(&htlc)->buf — assert it's actually a task-error, not a
	 * transport hiccup. */
    g_assert_true (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    /* If the spec error-code field is present, it MUST sit inside
	 * 0..=5; the PayloadTooLarge (1) branch is what the server
	 * SHOULD pick for this scenario, but we accept any
	 * spec-conforming code so a server that signals 0 (Generic)
	 * doesn't drag the test into rejecting valid behaviour. The
	 * stronger assertion would be raw == 1, but pinning that here
	 * couples the test to a specific Janus version's error
	 * mapping; the spec allows the server discretion. */
    {
        gboolean saw_code = FALSE;
        guint16 raw = 0;
        dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
        {
            if (_type == HTLS_DATA_CHAT_MEDIA_ERROR_CODE && _len >= 2) {
                guint8 *p = dh->data;
                raw = ((guint16) p[0] << 8) | (guint16) p[1];
                saw_code = TRUE;
            }
        }
        dh_end ();
        if (saw_code) {
            g_assert_cmpuint (raw, <=, 5u);
        }
    }

    close_session (fd, &htlc);
}

/*
 * Chat-with-media round-trip: upload a PNG, attach the returned
 * handle on a TranChatSend, observe the relayed broadcast back
 * to us — assert the broadcast carries CHAT_MEDIA_ID +
 * CHAT_MEDIA_TYPE companion fields.
 */
static void
test_inline_media_chat_with_media_round_trip (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Chat+Media RT", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }
    g_assert_cmphex ((htlc.caps & HTLC_CAP_INLINE_MEDIA), ==,
                     HTLC_CAP_INLINE_MEDIA);

    /* Upload first. */
    GBytes *png = encode_test_png ();
    g_assert_nonnull (png);
    gsize png_len = 0;
    const guint8 *png_data = g_bytes_get_data (png, &png_len);
    guint32 up_trans = send_upload_media (fd, &htlc, png_data, png_len,
                                          "image/png");
    g_assert_cmpuint (up_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, up_trans, 16));
    g_bytes_unref (png);
    g_assert_false (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    /* Copy the handle + mime out of htlc->in before drain_until
	 * overwrites the buffer with the next received message. */
    struct gtkhx_proto_upload_final_reply reply;
    g_assert_true (gtkhx_proto_parse_upload_final_reply (
        hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &reply));
    guint8 *handle = g_memdup2 (reply.id_ptr, reply.id_len);
    gsize handle_len = reply.id_len;
    char *mime = g_strndup ((const char *) reply.type_ptr, reply.type_len);

    /* Send chat with media. Janus relays the broadcast back to us
	 * (we're a member of public chat). */
    /* Tag the body with a unique marker: Janus's HTLS_HDR_CHAT
	 * broadcasts carry uid 0 (it doesn't stamp the sender), so the
	 * relay can't be scoped to our own message by uid — match the
	 * marker instead, which also dodges concurrent test binaries'
	 * chat noise. */
    char marker[24];
    im_make_marker (marker, sizeof (marker));
    char *chat_body = g_strdup_printf ("see attached %s", marker);
    gboolean chat_sent = send_chat_with_media (fd, &htlc, chat_body, handle,
                                               handle_len, mime);
    g_free (chat_body);
    g_assert_true (chat_sent);

    /* Drain to our own relayed broadcast by matching the unique body
	 * marker — Janus stamps HTLS_HDR_CHAT broadcasts with uid 0, so a
	 * uid filter can't identify ours, and the marker also skips other
	 * concurrent test binaries' chat noise. The walker consumes the
	 * body fields, but hx_test_in(htlc)->buf is left intact for the media-meta
	 * walk below. */
    struct hx_chat_msg msg;
    g_assert_true (
        integration_drain_until_chat_marker (fd, &htlc, marker, &msg, 16));

    struct gtkhx_proto_chat_media_meta meta;
    int status = gtkhx_proto_extract_chat_media_meta (hx_test_in(&htlc)->buf,
                                                      hx_test_in(&htlc)->pos, &meta);
    g_assert_cmpint (status, ==, GTKHX_PROTO_MEDIA_META_PRESENT);
    g_assert_cmpuint (meta.id_len, ==, handle_len);
    g_assert (memcmp (meta.id_ptr, handle, handle_len) == 0);

    g_free (handle);
    g_free (mime);
    close_session (fd, &htlc);
}

/*
 * Download round-trip: upload a PNG, fetch the canonical bytes
 * via TranDownloadMedia. Assert the reply carries
 * CHAT_MEDIA_PAYLOAD with a valid PNG signature.
 */
static void
test_inline_media_download_round_trip (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Download RT", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }

    /* Upload. */
    GBytes *png = encode_test_png ();
    g_assert_nonnull (png);
    gsize png_len = 0;
    const guint8 *png_data = g_bytes_get_data (png, &png_len);
    guint32 up_trans = send_upload_media (fd, &htlc, png_data, png_len,
                                          "image/png");
    g_assert_cmpuint (up_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, up_trans, 16));
    g_bytes_unref (png);
    g_assert_false (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    struct gtkhx_proto_upload_final_reply up_reply;
    g_assert_true (gtkhx_proto_parse_upload_final_reply (
        hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &up_reply));
    guint8 *handle = g_memdup2 (up_reply.id_ptr, up_reply.id_len);
    gsize handle_len = up_reply.id_len;

    /* Authorize ourselves as a recipient by sending a chat with
	 * the handle. Per spec the auth set is fixed at chat-relay
	 * time; the sender is automatically a recipient (we relayed
	 * to ourselves above implicitly via public chat, but Janus's
	 * exact policy here is operator-discretion — emitting the
	 * chat is the spec-conformant way to enter the auth set). */
    char dl_marker[24];
    im_make_marker (dl_marker, sizeof (dl_marker));
    char *dl_body = g_strdup_printf ("downloading %s", dl_marker);
    gboolean dl_sent = send_chat_with_media (fd, &htlc, dl_body, handle,
                                             handle_len, "image/png");
    g_free (dl_body);
    g_assert_true (dl_sent);
    struct hx_chat_msg msg;
    g_assert_true (
        integration_drain_until_chat_marker (fd, &htlc, dl_marker, &msg, 16));

    /* Now request the bytes. */
    guint32 dl_trans = send_download_media (fd, &htlc, handle, handle_len);
    g_assert_cmpuint (dl_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, dl_trans, 16));
    g_assert_false (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    struct gtkhx_proto_download_reply dl_reply;
    g_assert_true (gtkhx_proto_parse_download_reply (
        hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &dl_reply));
    g_assert_cmpuint (dl_reply.payload_len, >=, 8);
    /* The canonical bytes must start with an allowlisted image
	 * magic prefix. Janus passes PNG through today so PNG is
	 * the expected branch, but the spec only mandates that the
	 * canonical format be one of {PNG, JPEG, GIF}. Accept any
	 * of the three so a future Janus update that canonicalises
	 * to JPEG/GIF (or a different conforming server in the
	 * matrix) doesn't fail this test for the wrong reason. */
    const guint8 *p = dl_reply.payload_ptr;
    gboolean is_png = (dl_reply.payload_len >= 8
                       && memcmp (p, "\x89PNG\r\n\x1A\n", 8) == 0);
    gboolean is_jpeg = (dl_reply.payload_len >= 3
                        && memcmp (p, "\xFF\xD8\xFF", 3) == 0);
    gboolean is_gif = (dl_reply.payload_len >= 6
                       && (memcmp (p, "GIF87a", 6) == 0
                           || memcmp (p, "GIF89a", 6) == 0));
    g_assert_true (is_png || is_jpeg || is_gif);

    g_free (handle);
    close_session (fd, &htlc);
}

/*
 * Chunked download round-trip: upload a payload whose canonical
 * form is large enough that the server splits the download reply
 * across multiple PART_COUNT chunks. Walk the chunked-reply
 * accumulator loop (PART_INDEX = 1, 2, …) until PART_FINAL,
 * stitch the payload back together, and assert the result starts
 * with an allowlisted image magic.
 *
 * The production receive path
 * (src/inline_media_download.c::rcv_task_download_media) drives
 * the same loop in the client — this test confirms it works
 * end-to-end against Janus rather than just at the proto layer.
 */
static void
test_inline_media_chunked_download_round_trip (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Chunked DL RT", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }
    g_assert_cmphex ((htlc.caps & HTLC_CAP_INLINE_MEDIA), ==,
                     HTLC_CAP_INLINE_MEDIA);

    guint32 chunk_size = htlc.media_chunk_size ? htlc.media_chunk_size
                                               : HX_MEDIA_DEFAULT_CHUNK_SIZE;
    if (chunk_size > HX_MEDIA_DEFAULT_CHUNK_SIZE) {
        chunk_size = HX_MEDIA_DEFAULT_CHUNK_SIZE;
    }

    /* Upload a high-entropy PNG large enough that the server-side
	 * canonical form should still exceed one chunk_size after the
	 * server's re-encode. Asking for ~3 × chunk_size headroom
	 * lets the canonical re-encode shrink by up to 2/3 without
	 * dropping the test into single-chunk territory. */
    GBytes *png = encode_chunky_png ((gsize) chunk_size * 3);
    g_assert_nonnull (png);
    gsize png_len = 0;
    const guint8 *png_data = g_bytes_get_data (png, &png_len);

    guint32 up_trans = send_upload_media_chunked (
        fd, &htlc, png_data, png_len, chunk_size, "image/png");
    g_bytes_unref (png);
    g_assert_cmpuint (up_trans, !=, 0);

    struct gtkhx_proto_upload_final_reply up_reply;
    g_assert_true (gtkhx_proto_parse_upload_final_reply (
        hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &up_reply));
    guint8 *handle = g_memdup2 (up_reply.id_ptr, up_reply.id_len);
    gsize handle_len = up_reply.id_len;

    /* Enter the authorization set via a chat-relay, same shape
	 * download_round_trip uses. */
    char cdl_marker[24];
    im_make_marker (cdl_marker, sizeof (cdl_marker));
    char *cdl_body = g_strdup_printf ("chunked-dl %s", cdl_marker);
    gboolean cdl_sent = send_chat_with_media (fd, &htlc, cdl_body, handle,
                                              handle_len, "image/png");
    g_free (cdl_body);
    g_assert_true (cdl_sent);
    struct hx_chat_msg msg;
    g_assert_true (
        integration_drain_until_chat_marker (fd, &htlc, cdl_marker, &msg, 16));

    /* Chunk 0: bare TranDownloadMedia (no PART_INDEX). */
    guint32 dl_trans = send_download_media (fd, &htlc, handle, handle_len);
    g_assert_cmpuint (dl_trans, !=, 0);
    g_assert_true (
        integration_drain_until_task_trans (fd, &htlc, dl_trans, 16));
    g_assert_false (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    struct gtkhx_proto_download_reply dl_reply;
    g_assert_true (gtkhx_proto_parse_download_reply (
        hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &dl_reply));
    /* The whole point of this test is exercising the
	 * multi-chunk path. If the server's canonical form fits in
	 * one chunk after all (the upload's noise PNG re-encoded
	 * smaller than we expected), the chunked download flow is
	 * tautologically tested by every other download test —
	 * fail loud so we know to grow the upload payload rather
	 * than silently passing through the single-chunk branch. */
    g_assert_cmpuint (dl_reply.part_count, >=, 2);

    GByteArray *accumulator = g_byte_array_new ();
    g_byte_array_append (accumulator, dl_reply.payload_ptr,
                         dl_reply.payload_len);
    guint16 part_count = dl_reply.part_count;
    gboolean saw_final = dl_reply.final_chunk;

    /* Follow-up requests: PART_INDEX 1, 2, …, until PART_FINAL. */
    for (guint16 i = 1; i < part_count && !saw_final; i++) {
        guint32 t = send_download_media_part (fd, &htlc, handle, handle_len,
                                              i);
        g_assert_cmpuint (t, !=, 0);
        g_assert_true (integration_drain_until_task_trans (fd, &htlc, t, 16));
        g_assert_false (
            gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));
        g_assert_true (gtkhx_proto_parse_download_reply (
            hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos, &dl_reply));
        g_byte_array_append (accumulator, dl_reply.payload_ptr,
                             dl_reply.payload_len);
        saw_final = dl_reply.final_chunk;
    }

    g_assert_true (saw_final);
    g_assert_cmpuint (accumulator->len, >=, chunk_size);

    /* Accumulated bytes must start with an allowlisted image
	 * magic prefix (same set the single-shot download asserts). */
    const guint8 *p = accumulator->data;
    gboolean is_png = (accumulator->len >= 8
                       && memcmp (p, "\x89PNG\r\n\x1A\n", 8) == 0);
    gboolean is_jpeg = (accumulator->len >= 3
                        && memcmp (p, "\xFF\xD8\xFF", 3) == 0);
    gboolean is_gif = (accumulator->len >= 6
                       && (memcmp (p, "GIF87a", 6) == 0
                           || memcmp (p, "GIF89a", 6) == 0));
    g_assert_true (is_png || is_jpeg || is_gif);

    g_byte_array_unref (accumulator);
    g_free (handle);
    close_session (fd, &htlc);
}

/*
 * Garbage upload rejected: send 65,500 zero bytes — close to
 * the 16-bit single-shot wire-framing ceiling but well below
 * the server's 256 KiB MAX_BYTES advertisement, so Janus's
 * magic-byte sniff is the most likely rejection branch
 * (all-zeros has no valid image signature). Originally written
 * as a PayloadTooLarge probe; pivoted to a 'server rejects
 * garbage with an actionable code' check after realising
 * single-shot wire framing tops out at 65 KB and a true
 * MAX_BYTES overflow requires chunked-upload support (not
 * shipped in Phase 9.C). Assert task-error + any present
 * CHAT_MEDIA_ERROR_CODE inside the spec's 0–5 range. */
static void
test_inline_media_oversized_rejected (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Oversize", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }

    /* 65500 bytes of zeros: well BELOW the 256 KiB MAX_BYTES
	 * default and just under the 65535-byte single-shot wire
	 * framing ceiling. The point of this test isn't to overflow
	 * MAX_BYTES — the chunked_too_large case covers that path —
	 * it's to confirm the server rejects clearly-invalid payloads
	 * on the single-shot frame with an actionable task-error.
	 * All-zeros has no valid image magic so the most likely
	 * rejection branch is UnsupportedFormat (code 2). */
    gsize garbage_len = 65500;
    guint8 *garbage = g_malloc0 (garbage_len);

    guint32 trans
        = send_upload_media (fd, &htlc, garbage, garbage_len, NULL);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));

    /* Header MUST carry the task-error flag. */
    g_assert_true (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    /* gtkhx_proto_extract_media_error_code() collapses unknown +
	 * absent to 0, so asserting <= 5 would always pass and the
	 * test would never catch a buggy server. Walk the wire
	 * chunks directly: if HTLS_DATA_CHAT_MEDIA_ERROR_CODE is
	 * present, the on-wire value MUST be in {0..=5}. Absent is
	 * OK (the field is optional per spec). */
    {
        gboolean saw_code = FALSE;
        guint16 raw = 0;
        dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
        {
            if (_type == HTLS_DATA_CHAT_MEDIA_ERROR_CODE && _len >= 2) {
                guint8 *p = dh->data;
                raw = ((guint16) p[0] << 8) | (guint16) p[1];
                saw_code = TRUE;
            }
        }
        dh_end ();
        if (saw_code) {
            g_assert_cmpuint (raw, <=, 5u);
        }
    }

    g_free (garbage);
    close_session (fd, &htlc);
}

/*
 * Unsupported-format upload: send a structurally valid SVG
 * (declared type image/svg+xml) and assert the server rejects.
 *
 * The spec forbids SVG / WebP / AVIF / HEIC under
 * CAPABILITY_INLINE_MEDIA — only PNG, JPEG, and GIF round-trip.
 * Janus enforces this server-side via a magic-byte sniff that
 * runs BEFORE canonicalisation, so a syntactically-valid SVG
 * payload with the right declared MIME hint should still bounce
 * with MediaErrorCode UnsupportedFormat (2).
 *
 * The single-shot frame is fine here — a tiny SVG sits well
 * inside the wire framing ceiling.
 */
static void
test_inline_media_unsupported_format_upload (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "SVG Reject", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }
    g_assert_cmphex ((htlc.caps & HTLC_CAP_INLINE_MEDIA), ==,
                     HTLC_CAP_INLINE_MEDIA);

    /* Smallest legal SVG that parses cleanly — a 1×1 black square.
	 * The point isn't that this renders, it's that the magic bytes
	 * ('<?xml' / '<svg') aren't in the inline-media allowlist. */
    const char *svg
        = "<?xml version=\"1.0\"?>"
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\">"
          "<rect width=\"1\" height=\"1\"/>"
          "</svg>";
    gsize svg_len = strlen (svg);

    guint32 trans = send_upload_media (fd, &htlc, (const guint8 *) svg,
                                       svg_len, "image/svg+xml");
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));

    /* Header MUST carry the task-error flag. */
    g_assert_true (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    /* If CHAT_MEDIA_ERROR_CODE is present, it MUST sit inside 0..=5
	 * — UnsupportedFormat (2) is the spec-preferred branch, but a
	 * server signalling Generic (0) is spec-conforming too. */
    {
        gboolean saw_code = FALSE;
        guint16 raw = 0;
        dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
        {
            if (_type == HTLS_DATA_CHAT_MEDIA_ERROR_CODE && _len >= 2) {
                guint8 *p = dh->data;
                raw = ((guint16) p[0] << 8) | (guint16) p[1];
                saw_code = TRUE;
            }
        }
        dh_end ();
        if (saw_code) {
            g_assert_cmpuint (raw, <=, 5u);
        }
    }

    close_session (fd, &htlc);
}

/*
 * Unauthorized download: try to fetch a handle we never
 * received. The spec collapses "not authorized" and "expired"
 * into the same generic task-error so the response can't be used
 * to enumerate live handle IDs. We just assert the server
 * rejected — the exact code may be 0 (Generic) or 4
 * (NotAuthorized) per spec discretion.
 */
static void
test_inline_media_unauthorized_download (void)
{
    const hx_test_server *srv = pick_inline_media_server ();
    if (!srv) {
        g_test_fail_printf ("no inline-media-capable server in matrix.");
        return;
    }

    struct htlc_conn htlc;
    int fd = integration_open_login_to_caps_or_skip (
        srv, &htlc, "Unauth DL", 412, HTLC_CAP_INLINE_MEDIA);
    if (fd < 0) {
        return;
    }

    /* Fabricate a random 32-byte handle. The spec mandates 128
	 * bits of entropy per handle; ours has 256, so the
	 * pre-collision probability against a real Janus handle is
	 * far smaller than the test's flake budget. */
    guint8 fake_handle[32];
    for (gsize i = 0; i < sizeof (fake_handle); i++) {
        fake_handle[i] = (guint8) (g_random_int () & 0xFF);
    }

    guint32 trans = send_download_media (fd, &htlc, fake_handle,
                                         sizeof (fake_handle));
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));

    /* Header MUST carry the task-error flag. */
    g_assert_true (gtkhx_proto_header_in_error (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos));

    /* gtkhx_proto_extract_media_error_code() collapses unknown +
	 * absent to 0 so it can't distinguish 'server omitted the
	 * field' from 'server sent 0 explicitly'. Walk the chunks
	 * directly: when the optional CHAT_MEDIA_ERROR_CODE is
	 * present, the spec collapses 'expired' and 'not authorized'
	 * to the same code to defeat handle enumeration — so the
	 * on-wire value MUST be 0 (Generic) or 4 (NotAuthorized).
	 * Anything else (1/2/3/5) would be a server bug. */
    {
        gboolean saw_code = FALSE;
        guint16 raw = 0;
        dh_start (hx_test_in(&htlc)->buf, hx_test_in(&htlc)->pos)
        {
            if (_type == HTLS_DATA_CHAT_MEDIA_ERROR_CODE && _len >= 2) {
                guint8 *p = dh->data;
                raw = ((guint16) p[0] << 8) | (guint16) p[1];
                saw_code = TRUE;
            }
        }
        dh_end ();
        if (saw_code) {
            g_assert (raw == 0 || raw == 4);
        }
    }

    close_session (fd, &htlc);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/inline_media/cap_negotiation",
                     test_inline_media_cap_negotiation);
    g_test_add_func ("/integration/inline_media/limits_advertised",
                     test_inline_media_advisory_limits_advertised);
    g_test_add_func ("/integration/inline_media/upload_round_trip",
                     test_inline_media_upload_round_trip);
    g_test_add_func ("/integration/inline_media/chunked_upload_round_trip",
                     test_inline_media_chunked_upload_round_trip);
    g_test_add_func ("/integration/inline_media/chunked_upload_too_large",
                     test_inline_media_chunked_upload_too_large);
    g_test_add_func ("/integration/inline_media/chat_with_media_round_trip",
                     test_inline_media_chat_with_media_round_trip);
    g_test_add_func ("/integration/inline_media/download_round_trip",
                     test_inline_media_download_round_trip);
    g_test_add_func ("/integration/inline_media/chunked_download_round_trip",
                     test_inline_media_chunked_download_round_trip);
    g_test_add_func ("/integration/inline_media/oversized_rejected",
                     test_inline_media_oversized_rejected);
    g_test_add_func ("/integration/inline_media/unsupported_format_upload",
                     test_inline_media_unsupported_format_upload);
    g_test_add_func ("/integration/inline_media/unauthorized_download",
                     test_inline_media_unauthorized_download);
    return g_test_run ();
}
