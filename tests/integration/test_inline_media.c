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
 *   "The first step of 9.F is verifying Janus actually implements
 *    the extension. Connect a debug-built client to the existing
 *    janus Tier 3 container, watch the LOGIN reply for the
 *    capability echo of bit 3 and the DATA_CHAT_MEDIA_MAX_*
 *    advisory fields. If they're not there, fall back to building
 *    a Go mock server in tests/integration/mock-server/
 *    inline-media/ (same shape as the chat-history mock)."
 *
 * This file lands the first step: cap-negotiation + advisory-limits
 * parsing against Janus. Per [[gtkhx_janus]], Misha noted 2026-06
 * that Janus ships inline-media support; this test pins the
 * assumption.
 *
 * Subsequent commits in this same Phase 9.F branch will add the
 * full upload + chat-with-media-handle + download round-trip once
 * the cap negotiation test passes. If it FAILS — Janus actually
 * doesn't advertise the cap — the Phase 9.F doc instructs us to
 * write a mock server instead; the failure mode is a clean
 * actionable assertion, not a silent skip (per
 * [[feedback_no_test_skips]]).
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "compat.h"   /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "integration_harness.h"
#include "server_matrix.h"

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

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/inline_media/cap_negotiation",
                     test_inline_media_cap_negotiation);
    g_test_add_func ("/integration/inline_media/limits_advertised",
                     test_inline_media_advisory_limits_advertised);
    return g_test_run ();
}
