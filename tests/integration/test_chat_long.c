/*
 * tests/integration/test_chat_long.c — long chat message
 * round-trips the wire intact.
 *
 * mhxd's rcv_chat caps the chat body at 2048 bytes (chatbuf is
 * 4096 but the dh_len > 2048 branch truncates to 2048). Anything
 * up to 2048 should make it through unchanged. This test sends a
 * 1500-byte payload — long enough to:
 *
 *   - Stress the proto parser past any < 256 byte happy-path code
 *     that handles single-byte length fields.
 *   - Exercise the wire layer's qbuf growth (network.c grows the
 *     in-buffer in chunks).
 *
 * Catches a regression where an off-by-one or sign-extension
 * truncates a long chat to ~127 bytes (a classic signed-byte bug
 * from rcv code) or where the broadcast loop drops payload past
 * a fixed in-buffer.
 *
 * Two clients used for a clean read: Alice sends, Bob receives,
 * we compare the body bytes verbatim.
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"

#define LONG_BODY_LEN 1500

static void
test_chat_long_message_roundtrips (void)
{
    struct htlc_conn htlc_a;
    int fd_a = integration_open_login_or_skip (&htlc_a, "LongAlice T-3", 412);
    if (fd_a < 0) {
        return;
    }

    struct htlc_conn htlc_b;
    int fd_b = integration_open_login_or_skip (&htlc_b, "LongBob T-3", 412);
    if (fd_b < 0) {
        integration_release_htlc (&htlc_a);
        integration_close (fd_a);
        return;
    }

    /* Build a deterministic 1500-byte body. Use a repeating
	 * pattern of mixed-case ASCII so any byte truncation or
	 * mid-string corruption shows up in a diff against the
	 * known input. Avoid '\r' / '\n' (which mhxd rewrites via
	 * CR2LF / strip_ansi) so the round-trip is bytewise stable. */
    char *body = g_malloc (LONG_BODY_LEN + 1);
    for (int i = 0; i < LONG_BODY_LEN; i++) {
        static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "abcdefghijklmnopqrstuvwxyz"
                                    "0123456789-_=+";
        body[i] = alpha[i % (sizeof alpha - 1)];
    }
    body[LONG_BODY_LEN] = 0;

    g_assert_true (integration_send_message (
        fd_a, &htlc_a, HTLC_HDR_CHAT, /*flag=*/0, /*hc=*/1, (int)HTLC_DATA_CHAT,
        LONG_BODY_LEN, (guint8 *)body));

    /* Bob drains for HTLS_HDR_CHAT and pulls the body via the
	 * Tier 2 hx_chat_extract helper. mhxd's rcv_chat formats the
	 * broadcast body as
	 *
	 *   "\r <name>:  <body>"
	 *
	 * (CR + space + name + ':' + 2 spaces + body), then the chat
	 * handler chains LF / CR replacement. So the broadcast text
	 * we observe contains our body as a substring; we
	 * substring-compare rather than equality-compare. */
    gboolean got_chat = FALSE;
    for (int i = 0; i < 64 && !got_chat; i++) {
        if (!integration_recv_message (fd_b, &htlc_b, /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc_b) != HTLS_HDR_CHAT) {
            continue;
        }

        struct hx_chat_msg cm = { 0 };
        if (!hx_chat_extract (hx_test_in(&htlc_b)->buf, hx_test_in(&htlc_b)->pos, &cm)) {
            continue;
        }

        /* Body must contain our entire 1500-byte payload as a
		 * contiguous substring. memmem is the right primitive
		 * here — strstr would fail on any embedded nul. */
        if (memmem (cm.text, cm.text_len, body, LONG_BODY_LEN)) {
            got_chat = TRUE;
            g_test_message ("received chat: text_len=%u "
                            "(payload=%d)",
                            (unsigned)cm.text_len, LONG_BODY_LEN);
        }
    }
    g_assert_true (got_chat);

    g_free (body);
    integration_release_htlc (&htlc_b);
    integration_close (fd_b);
    integration_release_htlc (&htlc_a);
    integration_close (fd_a);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/integration/chat_long/roundtrips",
                     test_chat_long_message_roundtrips);
    return g_test_run ();
}
