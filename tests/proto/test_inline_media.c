/*
 * tests/proto/test_inline_media.c — the inline-media extension's cap gate
 * and the advisory limits a server advertises (fogWraith
 * Capabilities-Inline-Media.md). The requests and replies are Rust:
 * hxproto, hxrequest and hxhandlers test them.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "compat.h" /* PACKED — required before hotline.h */
#include "protocol.h"
#include "hotline.h"
#include "hxconn_layout.h"
#include "inline_media.h"
#include "debug.h"

/* ---------- cap gate ---------- */

static void
test_cap_gate_refuses_without_echo (void)
{
    struct htlc_conn h;
    memset (&h, 0, sizeof (h));
    /* Caps bitmask cleared — server did not echo CAP_INLINE_MEDIA. */
    g_assert_false (inline_media_cap_ok (&h));

    /* Echo bit set — gate accepts. */
    h.caps = HTLC_CAP_INLINE_MEDIA;
    g_assert_true (inline_media_cap_ok (&h));

    /* NULL is safe and refuses. */
    g_assert_false (inline_media_cap_ok (NULL));
}

/* ---------- limits accessors ---------- */

static void
test_limits_accessors_use_defaults_when_zero (void)
{
    struct htlc_conn h;
    memset (&h, 0, sizeof (h));
    /* Cap echoed but every field absent on the wire. */
    h.caps = HTLC_CAP_INLINE_MEDIA;

    g_assert_cmpuint (inline_media_max_bytes (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_BYTES);
    g_assert_cmpuint (inline_media_max_dimension (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_DIMENSION);
    g_assert_cmpuint (inline_media_max_pixels (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_PIXELS);
    g_assert_cmpuint (inline_media_max_frames (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_FRAMES);
    g_assert_cmpuint (inline_media_max_duration_ms (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_DURATION_MS);

    /* NULL htlc is safe. */
    g_assert_cmpuint (inline_media_max_bytes (NULL), ==,
                      HX_MEDIA_DEFAULT_MAX_BYTES);
}

static void
test_limits_accessors_pass_through_server_values (void)
{
    struct htlc_conn h;
    memset (&h, 0, sizeof (h));
    h.caps = HTLC_CAP_INLINE_MEDIA;
    h.media_max_bytes = 65536;
    h.media_max_dimension = 1024;
    h.media_max_pixels = 1024u * 768u;
    h.media_max_frames = 50;
    h.media_max_duration_ms = 5000;

    g_assert_cmpuint (inline_media_max_bytes (&h), ==, 65536u);
    g_assert_cmpuint (inline_media_max_dimension (&h), ==, 1024u);
    g_assert_cmpuint (inline_media_max_pixels (&h), ==, 1024u * 768u);
    g_assert_cmpuint (inline_media_max_frames (&h), ==, 50u);
    g_assert_cmpuint (inline_media_max_duration_ms (&h), ==, 5000u);
}

/* Regression for the stale-limits-across-reconnect bug. The
 * htlc_conn struct is recycled across reconnect cycles; htlc->caps
 * gets overwritten on every fresh LOGIN reply, but the
 * media_max_* fields don't get zeroed. If the previous session
 * had the cap negotiated and the new server doesn't, the
 * accessors must hand back HX_MEDIA_DEFAULT_* rather than the
 * stale advertisement — caller has no business uploading anyway,
 * but the safer value is what we want surfacing to the
 * pre-flight UI. */
static void
test_limits_accessors_drop_stale_on_cap_lost (void)
{
    struct htlc_conn h;
    memset (&h, 0, sizeof (h));
    /* Stale advertisement from a prior session. */
    h.media_max_bytes = 65536;
    h.media_max_dimension = 1024;
    h.media_max_pixels = 1024u * 768u;
    h.media_max_frames = 50;
    h.media_max_duration_ms = 5000;
    /* New session: cap NOT echoed by the new server. */
    h.caps = 0;

    /* Every accessor falls through to its spec default rather
     * than honouring the stale advertised values. */
    g_assert_cmpuint (inline_media_max_bytes (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_BYTES);
    g_assert_cmpuint (inline_media_max_dimension (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_DIMENSION);
    g_assert_cmpuint (inline_media_max_pixels (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_PIXELS);
    g_assert_cmpuint (inline_media_max_frames (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_FRAMES);
    g_assert_cmpuint (inline_media_max_duration_ms (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_DURATION_MS);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    debug_init ();

    g_test_add_func ("/proto/inline_media/cap_gate",
                     test_cap_gate_refuses_without_echo);
    g_test_add_func ("/proto/inline_media/limits_default",
                     test_limits_accessors_use_defaults_when_zero);
    g_test_add_func ("/proto/inline_media/limits_pass_through",
                     test_limits_accessors_pass_through_server_values);
    g_test_add_func ("/proto/inline_media/limits_drop_stale_on_cap_lost",
                     test_limits_accessors_drop_stale_on_cap_lost);

    return g_test_run ();
}
