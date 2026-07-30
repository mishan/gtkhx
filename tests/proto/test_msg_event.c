/*
 * tests/proto/test_msg_event.c — drive hx_msg_event_new with canned
 * uid + name + body inputs.
 *
 * HxMsgEvent is the private-message counterpart to HxChatEvent: the
 * GtkhxSession::msg signal carries one of these from rcv.c::hx_rcv_msg
 * to msg.c (renders the PM window) and notify.c (posts the system
 * notification). The constructor's job is small and crisp:
 *
 *   1. Stash uid + set is_broadcast = (uid == 0).
 *   2. UTF-8-sanitise `name` and `body` once via gtkhx_text_to_utf8.
 *   3. Compare the sanitised name against self_nick (byte-equal, with
 *      length check) and stamp is_self.
 *
 * These tests pin each of those steps so future changes (e.g. moving
 * the is_self compare to be ASCII-case-insensitive, or accepting a
 * non-NUL-terminated self_nick) trip the suite instead of de-syncing
 * msg.c and notify.c silently.
 *
 * No wire fixture needed — hx_msg_event_new takes already-extracted
 * chunk strings, not a wire frame.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "proto_helpers.h"

/* ---------- Basic happy path ---------- */

static void
test_msg_event_typical (void)
{
    const char *name = "alice";
    const char *body = "hey, got a sec?";
    HxMsgEvent *e
        = hx_msg_event_new (42, name, strlen (name), body, strlen (body), NULL);

    g_assert_nonnull (e);
    g_assert_cmpuint (e->uid, ==, 42);
    g_assert_cmpstr (e->name, ==, "alice");
    g_assert_cmpuint (e->name_len, ==, 5);
    g_assert_cmpstr (e->body, ==, "hey, got a sec?");
    g_assert_cmpuint (e->body_len, ==, strlen (body));
    g_assert_false (e->is_self);
    g_assert_false (e->is_broadcast);

    hx_msg_event_free (e);
}

/* ---------- is_broadcast ---------- */

static void
test_msg_event_broadcast_when_uid_zero (void)
{
    /* uid 0 is the convention for server broadcasts (the server
     * has no client-side UID). Constructor stamps is_broadcast
     * automatically — consumers don't have to re-check. */
    HxMsgEvent *e = hx_msg_event_new (0, "Server", 6, "back in 5", 9, NULL);

    g_assert_true (e->is_broadcast);
    g_assert_false (e->is_self);
    g_assert_cmpuint (e->uid, ==, 0);

    hx_msg_event_free (e);
}

static void
test_msg_event_not_broadcast_for_nonzero_uid (void)
{
    HxMsgEvent *e = hx_msg_event_new (1, "bob", 3, "hi", 2, NULL);
    g_assert_false (e->is_broadcast);
    hx_msg_event_free (e);
}

/* ---------- is_self ---------- */

static void
test_msg_event_is_self_matches (void)
{
    HxMsgEvent *e
        = hx_msg_event_new (7, "misha", 5, "self-message echo", 17, "misha");
    g_assert_true (e->is_self);
    hx_msg_event_free (e);
}

static void
test_msg_event_is_self_mismatch (void)
{
    HxMsgEvent *e = hx_msg_event_new (7, "alice", 5, "hi", 2, "misha");
    g_assert_false (e->is_self);
    hx_msg_event_free (e);
}

static void
test_msg_event_is_self_substring_does_not_match (void)
{
    /* The compare requires byte-equal AND length-equal. A self_nick
     * that's a strict prefix of the sender name must NOT match —
     * otherwise "mish" would self-flag a PM from "misha" and notify
     * would skip it. */
    HxMsgEvent *e = hx_msg_event_new (7, "misha", 5, "hi", 2, "mish");
    g_assert_false (e->is_self);
    hx_msg_event_free (e);
}

static void
test_msg_event_is_self_with_self_nick_longer (void)
{
    /* Reverse: self_nick longer than the sender name. Length-equal
     * check rejects this too. */
    HxMsgEvent *e = hx_msg_event_new (7, "mish", 4, "hi", 2, "misha");
    g_assert_false (e->is_self);
    hx_msg_event_free (e);
}

static void
test_msg_event_is_self_null_self_nick (void)
{
    /* NULL self_nick is the "we don't have a logged-in identity
     * yet" case (pre-SELFINFO). Must not crash; is_self stays FALSE. */
    HxMsgEvent *e = hx_msg_event_new (7, "misha", 5, "hi", 2, NULL);
    g_assert_false (e->is_self);
    hx_msg_event_free (e);
}

static void
test_msg_event_is_self_empty_self_nick (void)
{
    /* Empty self_nick is treated the same as NULL. */
    HxMsgEvent *e = hx_msg_event_new (7, "misha", 5, "hi", 2, "");
    g_assert_false (e->is_self);
    hx_msg_event_free (e);
}

/* ---------- Empty / NULL inputs ---------- */

static void
test_msg_event_empty_name_and_body (void)
{
    /* Defensively-handled corner: name_len 0 and body_len 0. Both
     * fields end up as freshly-allocated empty strings (per
     * gtkhx_text_to_utf8's contract). */
    HxMsgEvent *e = hx_msg_event_new (5, "", 0, "", 0, NULL);

    g_assert_nonnull (e);
    g_assert_nonnull (e->name);
    g_assert_nonnull (e->body);
    g_assert_cmpuint (e->name_len, ==, 0);
    g_assert_cmpuint (e->body_len, ==, 0);
    g_assert_cmpstr (e->name, ==, "");
    g_assert_cmpstr (e->body, ==, "");
    g_assert_false (e->is_self);

    hx_msg_event_free (e);
}

static void
test_msg_event_null_name_and_body (void)
{
    /* gtkhx_text_to_utf8 tolerates NULL bytes — the constructor
     * mustn't crash on NULL name or NULL body. */
    HxMsgEvent *e = hx_msg_event_new (5, NULL, 0, NULL, 0, NULL);

    g_assert_nonnull (e);
    g_assert_nonnull (e->name);
    g_assert_nonnull (e->body);
    g_assert_cmpuint (e->name_len, ==, 0);
    g_assert_cmpuint (e->body_len, ==, 0);

    hx_msg_event_free (e);
}

/* ---------- UTF-8 sanitisation ---------- */

static void
test_msg_event_utf8_passthrough (void)
{
    /* Already-valid UTF-8 in both fields passes through unchanged. */
    const char name[] = "héllo"; /* 6 bytes: h é l l o */
    const char body[] = "wörld"; /* 6 bytes */
    HxMsgEvent *e = hx_msg_event_new (1, name, sizeof (name) - 1, body,
                                      sizeof (body) - 1, NULL);

    g_assert_cmpstr (e->name, ==, name);
    g_assert_cmpstr (e->body, ==, body);
    g_assert_cmpuint (e->name_len, ==, sizeof (name) - 1);
    g_assert_cmpuint (e->body_len, ==, sizeof (body) - 1);
    g_assert_true (g_utf8_validate (e->name, e->name_len, NULL));
    g_assert_true (g_utf8_validate (e->body, e->body_len, NULL));

    hx_msg_event_free (e);
}

static void
test_msg_event_mac_roman_converts (void)
{
    /* MacRoman 0xE9 = é. Raw single high-bit byte is not valid
     * UTF-8; gtkhx_text_to_utf8 either converts via the fallback
     * charset or substitutes U+FFFD. Either way the result must be
     * valid UTF-8 — Pango / xtext crash on invalid encoding. */
    const char name[] = "Caf\xe9";  /* "Café" in MacRoman */
    const char body[] = "h\xe9llo"; /* "héllo" in MacRoman */
    HxMsgEvent *e = hx_msg_event_new (1, name, sizeof (name) - 1, body,
                                      sizeof (body) - 1, NULL);

    g_assert_nonnull (e->name);
    g_assert_nonnull (e->body);
    g_assert_true (g_utf8_validate (e->name, e->name_len, NULL));
    g_assert_true (g_utf8_validate (e->body, e->body_len, NULL));

    hx_msg_event_free (e);
}

static void
test_msg_event_is_self_after_utf8_fixup (void)
{
    /* If gtkhx_text_to_utf8 substitutes U+FFFD for invalid bytes,
     * the self-nick compare runs on the *post-fixup* name. So a
     * MacRoman "Café" sender against a pre-fixed "Café" self_nick
     * (both encoded as UTF-8 "Café") should match. The name input
     * comes in raw, the self_nick comes from htlc->name (which is
     * already sanitised to UTF-8 by the loader).
     *
     * Compute the expected post-fixup name by running the same
     * helper on the raw bytes, then feed that back as self_nick. */
    const char raw[] = "Caf\xe9"; /* "Café" in MacRoman */
    HxMsgEvent *probe
        = hx_msg_event_new (1, raw, sizeof (raw) - 1, "x", 1, NULL);
    char *clean_nick = g_strndup (probe->name, probe->name_len);
    hx_msg_event_free (probe);

    HxMsgEvent *e
        = hx_msg_event_new (2, raw, sizeof (raw) - 1, "hi", 2, clean_nick);
    g_assert_true (e->is_self);
    hx_msg_event_free (e);
    g_free (clean_nick);
}

/* ---------- uid round-trip ---------- */

static void
test_msg_event_preserves_uid (void)
{
    /* uid is u16 — verify the high-byte uid (e.g. 0xdead would
     * truncate but 0xbeef wouldn't) round-trips cleanly. */
    HxMsgEvent *e = hx_msg_event_new (0xbeef, "x", 1, "y", 1, NULL);
    g_assert_cmphex (e->uid, ==, 0xbeef);
    g_assert_false (e->is_broadcast);
    hx_msg_event_free (e);
}

/* ---------- Copy / free contract ---------- */

static void
test_msg_event_copy_preserves_fields (void)
{
    HxMsgEvent *e = hx_msg_event_new (42, "misha", 5, "test body", 9, "misha");
    HxMsgEvent *c = hx_msg_event_copy (e);

    g_assert_nonnull (c);
    g_assert_true (c != e);
    /* Deep copy: distinct allocations. */
    g_assert_true (c->name != e->name);
    g_assert_true (c->body != e->body);

    g_assert_cmpuint (c->uid, ==, e->uid);
    g_assert_cmpstr (c->name, ==, e->name);
    g_assert_cmpuint (c->name_len, ==, e->name_len);
    g_assert_cmpstr (c->body, ==, e->body);
    g_assert_cmpuint (c->body_len, ==, e->body_len);
    g_assert_true (c->is_self == e->is_self);
    g_assert_true (c->is_broadcast == e->is_broadcast);

    hx_msg_event_free (c);
    hx_msg_event_free (e);
}

static void
test_msg_event_copy_preserves_broadcast (void)
{
    /* Specific regression: is_broadcast is computed from uid=0
     * at new() time; copy must carry it forward without rederiving
     * (a future change that lazily recomputes from `c->uid` would
     * still pass — but if anyone forgets to copy the flag at all
     * this catches that). */
    HxMsgEvent *e = hx_msg_event_new (0, "Server", 6, "msg", 3, NULL);
    HxMsgEvent *c = hx_msg_event_copy (e);
    g_assert_true (c->is_broadcast);
    hx_msg_event_free (c);
    hx_msg_event_free (e);
}

static void
test_msg_event_free_null_is_noop (void)
{
    /* Same idiom as g_free: free(NULL) must be safe. */
    hx_msg_event_free (NULL);
}

static void
test_msg_event_copy_null_returns_null (void)
{
    g_assert_null (hx_msg_event_copy (NULL));
}

/* Phase E3: :shortcode: → emoji in the PM body; the sender name stays
 * literal. */
static void
test_msg_event_decodes_emoji_in_body (void)
{
    const char *body = "nice work :tada:";
    HxMsgEvent *e = hx_msg_event_new (3, "bob", 3, body, strlen (body), NULL);

    g_assert_cmpstr (e->body, ==, "nice work \xf0\x9f\x8e\x89");
    g_assert_cmpuint (e->body_len, ==, strlen (e->body));
    g_assert_cmpstr (e->name, ==, "bob");

    hx_msg_event_free (e);
}

/* Unknown / non-shortcode colon text in a PM is left untouched. */
static void
test_msg_event_leaves_non_shortcodes (void)
{
    const char *body = "call at 9:00 re :notacode:";
    HxMsgEvent *e = hx_msg_event_new (3, "bob", 3, body, strlen (body), NULL);

    g_assert_cmpstr (e->body, ==, body);
    g_assert_cmpuint (e->body_len, ==, strlen (body));

    hx_msg_event_free (e);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/msg_event/typical", test_msg_event_typical);

    g_test_add_func ("/proto/msg_event/decodes_emoji_in_body",
                     test_msg_event_decodes_emoji_in_body);
    g_test_add_func ("/proto/msg_event/leaves_non_shortcodes",
                     test_msg_event_leaves_non_shortcodes);

    g_test_add_func ("/proto/msg_event/broadcast_when_uid_zero",
                     test_msg_event_broadcast_when_uid_zero);
    g_test_add_func ("/proto/msg_event/not_broadcast_for_nonzero_uid",
                     test_msg_event_not_broadcast_for_nonzero_uid);

    g_test_add_func ("/proto/msg_event/is_self_matches",
                     test_msg_event_is_self_matches);
    g_test_add_func ("/proto/msg_event/is_self_mismatch",
                     test_msg_event_is_self_mismatch);
    g_test_add_func ("/proto/msg_event/is_self_substring_does_not_match",
                     test_msg_event_is_self_substring_does_not_match);
    g_test_add_func ("/proto/msg_event/is_self_with_self_nick_longer",
                     test_msg_event_is_self_with_self_nick_longer);
    g_test_add_func ("/proto/msg_event/is_self_null_self_nick",
                     test_msg_event_is_self_null_self_nick);
    g_test_add_func ("/proto/msg_event/is_self_empty_self_nick",
                     test_msg_event_is_self_empty_self_nick);

    g_test_add_func ("/proto/msg_event/empty_name_and_body",
                     test_msg_event_empty_name_and_body);
    g_test_add_func ("/proto/msg_event/null_name_and_body",
                     test_msg_event_null_name_and_body);

    g_test_add_func ("/proto/msg_event/utf8_passthrough",
                     test_msg_event_utf8_passthrough);
    g_test_add_func ("/proto/msg_event/mac_roman_converts",
                     test_msg_event_mac_roman_converts);
    g_test_add_func ("/proto/msg_event/is_self_after_utf8_fixup",
                     test_msg_event_is_self_after_utf8_fixup);

    g_test_add_func ("/proto/msg_event/preserves_uid",
                     test_msg_event_preserves_uid);

    g_test_add_func ("/proto/msg_event/copy_preserves_fields",
                     test_msg_event_copy_preserves_fields);
    g_test_add_func ("/proto/msg_event/copy_preserves_broadcast",
                     test_msg_event_copy_preserves_broadcast);
    g_test_add_func ("/proto/msg_event/free_null_is_noop",
                     test_msg_event_free_null_is_noop);
    g_test_add_func ("/proto/msg_event/copy_null_returns_null",
                     test_msg_event_copy_null_returns_null);

    return g_test_run ();
}
