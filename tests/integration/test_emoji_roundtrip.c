/*
 * tests/integration/test_emoji_roundtrip.c — end-to-end emoji shortcode
 * exchange between two clients through a real (Mac Roman) mhxd, exercising
 * the phase E2 send encode and the phase E3 receive decode together.
 *
 * Client A "types" an emoji-bearing line, runs it through the production
 * legacy-mode send path (gtkhx_text_for_wire, utf8_mode=FALSE) — which
 * rewrites 🎉 to the ASCII ":tada:" so it survives Mac Roman instead of
 * becoming '?' — and sends those bytes. Client B receives the broadcast
 * and runs it through the production display path (hx_chat_event_new),
 * which decodes ":tada:" back to 🎉.
 *
 * This is the regression guard for "two GtkHx users on an old server get
 * real round-trip emoji", and doubles as proof we don't break Mac Roman:
 * the wire form is pure ASCII and mhxd relays it verbatim.
 *
 * Modeled on test_two_client_chat.c.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "text_util.h"
#include "integration_harness.h"

/* "party time 🎉" — the emoji is U+1F389 PARTY POPPER (canonical :tada:). */
#define TYPED_LINE "party time \xf0\x9f\x8e\x89"
#define EMOJI_BYTES "\xf0\x9f\x8e\x89"

static void
test_emoji_roundtrip_a_to_b (void)
{
    struct htlc_conn htlc_a;
    int fd_a = integration_open_login_or_skip (&htlc_a, "Emoji A Tier-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    int fd_b = integration_open_login_or_skip (&htlc_b, "Emoji B Tier-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* A encodes via the legacy send path: emoji → :shortcode:, then Mac
     * Roman (a no-op for the now-ASCII text). The result must carry
     * ":tada:" and contain no high bytes. */
    gsize wlen = 0;
    char *wire
        = gtkhx_text_for_wire (TYPED_LINE, strlen (TYPED_LINE),
                               /*utf8_mode=*/FALSE, /*is_body=*/TRUE, &wlen);
    g_assert_nonnull (wire);
    g_assert_nonnull (strstr (wire, ":tada:"));
    g_assert_null (strstr (wire, EMOJI_BYTES));
    for (gsize i = 0; i < wlen; i++) {
        g_assert_true ((guint8)wire[i] < 0x80);
    }

    g_assert_true (integration_send_chat (fd_a, &htlc_a, wire));
    g_free (wire);

    /* B receives the broadcast (A's uid), then decodes for display. */
    struct hx_chat_msg cm;
    g_assert_true (integration_drain_until_chat (fd_b, &htlc_b, htlc_a.uid, &cm,
                                                 /*max_messages=*/64));

    HxChatEvent *e
        = hx_chat_event_new (cm.text, cm.text_len, /*cid=*/0, /*uid=*/0, NULL);
    g_assert_nonnull (e);
    /* The decoded display line shows the emoji and no longer the shortcode. */
    g_assert_nonnull (g_strstr_len (e->line, e->line_len, EMOJI_BYTES));
    g_assert_null (g_strstr_len (e->line, e->line_len, ":tada:"));
    /* The surrounding words survived intact. */
    g_assert_nonnull (g_strstr_len (e->line, e->line_len, "party time"));
    hx_chat_event_free (e);

    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/emoji_roundtrip/a_to_b",
                     test_emoji_roundtrip_a_to_b);

    return g_test_run ();
}
