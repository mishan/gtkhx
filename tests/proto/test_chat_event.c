/*
 * tests/proto/test_chat_event.c — drive hx_chat_event_new with canned
 * raw chat-message bytes.
 *
 * Where hx_chat_extract turns a wire-format HTLS_HDR_CHAT frame into
 * sanitised body bytes, hx_chat_event_new takes that next step: parse
 * the body into a packaged HxChatEvent that downstream consumers
 * (chat.c::output_chat, notify.c::gtkhx_notify_chat) can read derived
 * facts off of without redoing the work themselves.
 *
 * The constructor's job, per proto_helpers.h:
 *   1. UTF-8-sanitise the raw bytes (gtkhx_text_to_utf8).
 *   2. Detect the "[hx]" info-prefix and stamp is_info — info lines
 *      skip the sender/body split and any downstream highlight.
 *   3. Split the line into sender_off/_len and body_off/_len via
 *      hx_chat_split_nick_body. A failed split leaves both lens 0.
 *   4. Compare sender bytes against self_nick (NULL-safe) and stamp
 *      is_self.
 *
 * These tests pin each of those steps so a future refactor that, say,
 * changes the info-prefix or skips the split won't ship silently.
 *
 * Note: hx_chat_event_new does NOT take a wire-format buffer — it's a
 * pure parser over an already-sanitised byte run. The wire_fixture
 * infrastructure isn't needed here.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "proto_helpers.h"
#include "text_util.h" /* gtkhx_text_set_emoji_shortcodes_enabled (E6) */

/* ---------- Basic split: "Nick: body" ---------- */

/* The sender's uid comes off the wire's UID chunk and has to survive the
 * constructor and a deep copy. It sits in the padding after `cid` — the
 * offset is pinned by _Static_asserts on both sides of the FFI, so this
 * covers the plumbing rather than the layout. */
static void
test_chat_event_carries_the_wire_uid (void)
{
    const char *raw = " misha:  hello";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 3, /*uid=*/4242,
                                        NULL);
    HxChatEvent *c;

    g_assert_nonnull (e);
    g_assert_cmpuint (e->uid, ==, 4242);
    g_assert_cmpuint (e->cid, ==, 3);

    c = hx_chat_event_copy (e);
    g_assert_nonnull (c);
    g_assert_cmpuint (c->uid, ==, 4242);
    hx_chat_event_free (c);
    hx_chat_event_free (e);
}

/* A server that sends no UID chunk leaves it 0, which the render path
 * reads as "unknown" and falls back on a nick lookup for. */
static void
test_chat_event_uid_defaults_to_zero (void)
{
    const char *raw = " misha:  hello";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_nonnull (e);
    g_assert_cmpuint (e->uid, ==, 0);
    hx_chat_event_free (e);
}

static void
test_chat_event_typical_line (void)
{
    const char *raw = " misha:  hello world";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_nonnull (e);
    g_assert_cmpuint (e->cid, ==, 0);
    g_assert_cmpstr (e->line, ==, raw);
    g_assert_cmpuint (e->line_len, ==, strlen (raw));

    g_assert_false (e->is_info);
    g_assert_false (e->is_self);

    g_assert_cmpuint (e->sender_len, ==, 5);
    g_assert_true (memcmp (e->line + e->sender_off, "misha", 5) == 0);
    g_assert_cmpuint (e->body_len, ==, strlen ("hello world"));
    g_assert_true (memcmp (e->line + e->body_off, "hello world", e->body_len)
                   == 0);

    hx_chat_event_free (e);
}

static void
test_chat_event_preserves_cid (void)
{
    /* cid round-trips verbatim — the constructor only stashes it. */
    const char *raw = "alice: hi";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0xdeadbeef, /*uid=*/0, NULL);

    g_assert_cmphex (e->cid, ==, 0xdeadbeefu);

    hx_chat_event_free (e);
}

/* ---------- is_self ---------- */

static void
test_chat_event_is_self_matches (void)
{
    /* Sender byte-for-byte equal to self_nick: is_self TRUE. */
    const char *raw = "misha: hi all";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, "misha");

    g_assert_true (e->is_self);
    g_assert_false (e->is_info);
    g_assert_cmpuint (e->sender_len, ==, 5);

    hx_chat_event_free (e);
}

static void
test_chat_event_is_self_mismatch (void)
{
    const char *raw = "alice: hi all";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, "misha");

    g_assert_false (e->is_self);
    g_assert_cmpuint (e->sender_len, ==, 5);
    g_assert_true (memcmp (e->line + e->sender_off, "alice", 5) == 0);

    hx_chat_event_free (e);
}

static void
test_chat_event_is_self_substring_does_not_match (void)
{
    /* self_nick "mish" matches a prefix of "misha" — should NOT
	 * flip is_self. The compare requires exact length equality. */
    const char *raw = "misha: hi";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, "mish");

    g_assert_false (e->is_self);

    hx_chat_event_free (e);
}

static void
test_chat_event_is_self_null_self_nick (void)
{
    /* NULL self_nick: is_self always FALSE, no crash. */
    const char *raw = "misha: hi";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_false (e->is_self);
    g_assert_cmpuint (e->sender_len, ==, 5);

    hx_chat_event_free (e);
}

static void
test_chat_event_is_self_empty_self_nick (void)
{
    /* Empty-string self_nick is treated the same as NULL. */
    const char *raw = "misha: hi";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, "");

    g_assert_false (e->is_self);

    hx_chat_event_free (e);
}

/* ---------- Info-prefix detection ---------- */

static void
test_chat_event_info_prefix_detected (void)
{
    /* hx_printf_prefix emits exactly this byte sequence. The
	 * constructor must spot it AND skip the sender split (info
	 * lines aren't "Nick: body" — they're internal notices). */
    const char *raw = " \00310[\00303hx\00310]\003 reconnected";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_true (e->is_info);
    /* No split on info lines, even when the body trailing the
	 * prefix happens to look like "name: thing". */
    g_assert_cmpuint (e->sender_len, ==, 0);
    g_assert_cmpuint (e->body_len, ==, 0);
    g_assert_false (e->is_self);

    hx_chat_event_free (e);
}

static void
test_chat_event_info_prefix_skips_split (void)
{
    /* Even if the info-prefix trailer contains a colon-bearing
	 * line, we must not extract a sender from it. */
    const char *raw = " \00310[\00303hx\00310]\003 server: hello";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, "server");

    g_assert_true (e->is_info);
    g_assert_cmpuint (e->sender_len, ==, 0);
    g_assert_false (e->is_self); /* gated on sender_len>0 */

    hx_chat_event_free (e);
}

/* ---------- Lines that don't split ---------- */

static void
test_chat_event_no_sender_emote (void)
{
    /* Emote / raw server prose: no colon, no split. The line is
	 * still UTF-8-sanitised and preserved, sender_len and body_len
	 * both stay 0 — downstream consumers render the line verbatim. */
    const char *raw = "*** misha waves";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_false (e->is_info);
    g_assert_cmpstr (e->line, ==, raw);
    g_assert_cmpuint (e->sender_len, ==, 0);
    g_assert_cmpuint (e->body_len, ==, 0);
    g_assert_false (e->is_self);

    hx_chat_event_free (e);
}

static void
test_chat_event_no_sender_long_pre_colon (void)
{
    /* Pre-colon portion exceeds the 31-byte nick cap; split must
	 * reject. Verifies the constructor honours hx_chat_split_nick_body
	 * 's URL-rejection behaviour. */
    const char *raw = " the long preamble I wrote before: was here";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_cmpuint (e->sender_len, ==, 0);
    g_assert_cmpuint (e->body_len, ==, 0);

    hx_chat_event_free (e);
}

/* ---------- Edge cases: empty / NULL inputs ---------- */

static void
test_chat_event_empty_line (void)
{
    HxChatEvent *e = hx_chat_event_new ("", 0, 7, /*uid=*/0, "misha");

    g_assert_nonnull (e);
    g_assert_cmpuint (e->cid, ==, 7);
    g_assert_nonnull (e->line); /* always g_strdup-ed */
    g_assert_cmpuint (e->line_len, ==, 0);
    g_assert_false (e->is_info);
    g_assert_cmpuint (e->sender_len, ==, 0);
    g_assert_cmpuint (e->body_len, ==, 0);
    g_assert_false (e->is_self);

    hx_chat_event_free (e);
}

static void
test_chat_event_null_raw (void)
{
    /* gtkhx_text_to_utf8 tolerates NULL raw (treats as empty),
	 * so the constructor mustn't crash either. */
    HxChatEvent *e = hx_chat_event_new (NULL, 0, 0, /*uid=*/0, NULL);

    g_assert_nonnull (e);
    g_assert_nonnull (e->line);
    g_assert_cmpuint (e->line_len, ==, 0);
    g_assert_cmpuint (e->sender_len, ==, 0);
    g_assert_cmpuint (e->body_len, ==, 0);

    hx_chat_event_free (e);
}

/* ---------- UTF-8 sanitisation ---------- */

static void
test_chat_event_utf8_passthrough (void)
{
    /* Already-valid UTF-8 multibyte input flows through unchanged
	 * (sender slice still indexes by bytes, not characters). */
    const char raw[] = "misha: héllo wörld";
    HxChatEvent *e = hx_chat_event_new (raw, sizeof (raw) - 1, 0, /*uid=*/0, NULL);

    g_assert_cmpstr (e->line, ==, raw);
    g_assert_cmpuint (e->line_len, ==, sizeof (raw) - 1);
    g_assert_cmpuint (e->sender_len, ==, 5);
    g_assert_true (memcmp (e->line + e->sender_off, "misha", 5) == 0);

    hx_chat_event_free (e);
}

static void
test_chat_event_mac_roman_converts (void)
{
    /* MacRoman 0xE9 = é; in raw Mac Roman it's a single high-bit
	 * byte that's invalid UTF-8. gtkhx_text_to_utf8 either converts
	 * via the fallback charset or substitutes U+FFFD — either way,
	 * the resulting line must be valid UTF-8 and the sender split
	 * must still find "misha". */
    const char raw[] = "misha: h\xe9llo"; /* the 0xe9 byte */
    HxChatEvent *e = hx_chat_event_new (raw, sizeof (raw) - 1, 0, /*uid=*/0, NULL);

    g_assert_nonnull (e->line);
    g_assert_true (g_utf8_validate (e->line, e->line_len, NULL));
    g_assert_cmpuint (e->sender_len, ==, 5);
    g_assert_true (memcmp (e->line + e->sender_off, "misha", 5) == 0);

    hx_chat_event_free (e);
}

/* ---------- Copy / free roundtrip ---------- */

static void
test_chat_event_copy_preserves_fields (void)
{
    const char *raw = "misha: a test line";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 11, /*uid=*/0, "misha");
    HxChatEvent *c = hx_chat_event_copy (e);

    g_assert_nonnull (c);
    g_assert_true (c != e);
    g_assert_true (c->line != e->line); /* deep copy of line */

    g_assert_cmpuint (c->cid, ==, e->cid);
    g_assert_cmpuint (c->line_len, ==, e->line_len);
    g_assert_cmpstr (c->line, ==, e->line);
    g_assert_cmpuint (c->sender_off, ==, e->sender_off);
    g_assert_cmpuint (c->sender_len, ==, e->sender_len);
    g_assert_cmpuint (c->body_off, ==, e->body_off);
    g_assert_cmpuint (c->body_len, ==, e->body_len);
    g_assert_true (c->is_info == e->is_info);
    g_assert_true (c->is_self == e->is_self);

    hx_chat_event_free (c);
    hx_chat_event_free (e);
}

static void
test_chat_event_free_null_is_noop (void)
{
    /* Same idiom as g_free: free(NULL) must be safe. */
    hx_chat_event_free (NULL);
}

static void
test_chat_event_copy_null_returns_null (void)
{
    g_assert_null (hx_chat_event_copy (NULL));
}

/* ---------- Phase 9.D inline-media attach + placeholder ---------- */

static void
test_chat_event_media_attach_round_trips (void)
{
    HxChatEvent *e = hx_chat_event_new ("alice: look", strlen ("alice: look"),
                                        0, /*uid=*/0, NULL);
    g_assert_nonnull (e);
    g_assert_null (e->media);

    const guint8 id[] = {0xAB, 0xCD, 0xEF, 0x01};
    hx_chat_event_attach_media (e, id, sizeof (id), "image/png",
                                strlen ("image/png"),
                                800, TRUE, 600, TRUE, 124000, TRUE);
    g_assert_nonnull (e->media);
    g_assert_cmpuint (e->media->id_len, ==, 4);
    g_assert (memcmp (e->media->id, id, 4) == 0);
    g_assert_cmpstr (e->media->mime, ==, "image/png");
    g_assert_cmpuint (e->media->width, ==, 800);
    g_assert_true (e->media->width_present);
    g_assert_cmpuint (e->media->height, ==, 600);
    g_assert_cmpuint (e->media->bytes, ==, 124000);
    hx_chat_event_free (e);
}

static void
test_chat_event_media_attach_copy_deep (void)
{
    HxChatEvent *e = hx_chat_event_new ("alice: look", strlen ("alice: look"),
                                        7, /*uid=*/0, NULL);
    g_assert_nonnull (e);
    const guint8 id[] = {0x01, 0x02, 0x03};
    hx_chat_event_attach_media (e, id, sizeof (id), "image/gif",
                                strlen ("image/gif"),
                                64, TRUE, 64, TRUE, 0, FALSE);

    HxChatEvent *c = hx_chat_event_copy (e);
    g_assert_nonnull (c);
    g_assert_nonnull (c->media);
    /* Distinct allocations — modify e's media and verify c's
	 * remained intact. */
    g_assert (c->media != e->media);
    g_assert (c->media->id != e->media->id);
    g_assert (c->media->mime != e->media->mime);
    g_assert_cmpuint (c->media->id_len, ==, 3);
    g_assert_cmpstr (c->media->mime, ==, "image/gif");
    g_assert_cmpuint (c->cid, ==, 7);

    hx_chat_event_free (e);
    /* c should still have valid media bytes — UAF would crash here. */
    g_assert_cmpuint (c->media->id[0], ==, 1);
    g_assert_cmpstr (c->media->mime, ==, "image/gif");
    hx_chat_event_free (c);
}

static void
test_chat_event_media_attach_detach (void)
{
    HxChatEvent *e = hx_chat_event_new ("alice: hi", strlen ("alice: hi"),
                                        0, /*uid=*/0, NULL);
    const guint8 id[] = {0xFF};
    hx_chat_event_attach_media (e, id, 1, "image/png", 9, 0, FALSE, 0, FALSE,
                                0, FALSE);
    g_assert_nonnull (e->media);

    /* Re-attach with NULL id detaches. */
    hx_chat_event_attach_media (e, NULL, 0, NULL, 0, 0, FALSE, 0, FALSE, 0,
                                FALSE);
    g_assert_null (e->media);

    /* Idempotent on NULL ev. */
    hx_chat_event_attach_media (NULL, id, 1, "image/png", 9, 0, FALSE, 0,
                                FALSE, 0, FALSE);
    hx_chat_event_free (e);
}

static void
test_chat_event_media_placeholder_full (void)
{
    HxChatMedia m = {
        .id = (guint8 *) "x",
        .id_len = 1,
        .mime = "image/png",
        .mime_len = 9,
        .width = 800,
        .height = 600,
        .bytes = 124000,
        .width_present = TRUE,
        .height_present = TRUE,
        .bytes_present = TRUE,
    };
    char *p = hx_chat_media_placeholder_line (&m);
    g_assert_nonnull (p);
    /* Format: "[image · PNG · 800×600 · 121.1 KB · click to view]" */
    g_assert (g_str_has_prefix (p, "[image · PNG · 800×600 · "));
    g_assert (g_str_has_suffix (p, " · click to view]"));
    g_free (p);
}

static void
test_chat_event_media_placeholder_minimal (void)
{
    /* No dims, no bytes — formatter should elide those columns. */
    HxChatMedia m = {
        .id = (guint8 *) "x",
        .id_len = 1,
        .mime = "image/jpeg",
        .mime_len = 10,
    };
    char *p = hx_chat_media_placeholder_line (&m);
    g_assert_nonnull (p);
    g_assert_cmpstr (p, ==, "[image · JPEG · click to view]");
    g_free (p);
}

static void
test_chat_event_media_placeholder_null (void)
{
    /* NULL media is safe and returns a generic placeholder. */
    char *p = hx_chat_media_placeholder_line (NULL);
    g_assert_cmpstr (p, ==, "[image]");
    g_free (p);
}

static void
test_chat_event_media_placeholder_unknown_mime_passes_through (void)
{
    /* Unknown but UTF-8-valid MIME — formatter prints it
	 * verbatim. Future-proofs the placeholder against a server
	 * that advertises image/webp / image/avif / etc. without
	 * breaking the row. */
    HxChatMedia m = {
        .id = (guint8 *) "x",
        .id_len = 1,
        .mime = "image/webp",
        .mime_len = 10,
    };
    char *p = hx_chat_media_placeholder_line (&m);
    g_assert_nonnull (p);
    g_assert_cmpstr (p, ==, "[image · image/webp · click to view]");
    g_free (p);
}

static void
test_chat_event_media_placeholder_rejects_invalid_utf8_mime (void)
{
    /* Hostile / buggy server emits a CHAT_MEDIA_TYPE chunk with
	 * invalid UTF-8 bytes (a lone 0xC3 continuation byte). The
	 * Rust extractor doesn't UTF-8-validate the type field; the
	 * placeholder formatter must defensively elide the column
	 * rather than interpolate arbitrary bytes into UI text. The
	 * row falls through to mime-less "[image · click to view]". */
    HxChatMedia m = {
        .id = (guint8 *) "x",
        .id_len = 1,
        .mime = "\xC3\xC3invalid",
        .mime_len = 9,
    };
    char *p = hx_chat_media_placeholder_line (&m);
    g_assert_nonnull (p);
    g_assert_cmpstr (p, ==, "[image · click to view]");
    g_free (p);
}

static void
test_chat_event_media_placeholder_clickable_embeds_token (void)
{
    /* Clickable variant: NBSP-joined + `hxmedia:N` embedded so
	 * xtext word_click recovers the token. */
    HxChatMedia m = {
        .id = (guint8 *) "x",
        .id_len = 1,
        .mime = "image/png",
        .mime_len = 9,
        .width = 800,
        .height = 600,
        .bytes = 124000,
        .width_present = TRUE,
        .height_present = TRUE,
        .bytes_present = TRUE,
    };
    char *p = hx_chat_media_placeholder_clickable (&m, 42);
    g_assert_nonnull (p);
    /* Must contain the hxmedia:N substring. */
    g_assert_nonnull (g_strstr_len (p, -1, "hxmedia:42"));
    /* Must not contain any ASCII space (NBSP-joined). */
    g_assert_null (g_strstr_len (p, -1, " "));
    g_free (p);
}

static void
test_chat_event_media_parse_token_finds_embedded (void)
{
    /* Word the click handler receives: a NBSP-joined string with
	 * `hxmedia:N` somewhere in it. Validate the parser. */
    guint token = 0;
    g_assert_true (hx_chat_media_parse_token (
        "[image\xc2\xa0\xc2\xb7\xc2\xa0hxmedia:7\xc2\xa0\xc2\xb7\xc2\xa0xyz]",
        &token));
    g_assert_cmpuint (token, ==, 7);

    /* Edge case: token at end. */
    g_assert_true (hx_chat_media_parse_token ("foo hxmedia:12345", &token));
    g_assert_cmpuint (token, ==, 12345);

    /* No token. */
    g_assert_false (hx_chat_media_parse_token ("[image · plain]", &token));

    /* Empty digits after the colon. */
    g_assert_false (hx_chat_media_parse_token ("hxmedia:", &token));

    /* NULL safety. */
    g_assert_false (hx_chat_media_parse_token (NULL, &token));
    g_assert_false (hx_chat_media_parse_token ("hxmedia:9", NULL));
}

/* ---------- Phase E3: :shortcode: → emoji at display time ---------- */

/* A "Nick: body" line: the body's :shortcode: becomes an emoji, the nick
 * column stays literal, and body_len is updated to the decoded length. */
static void
test_chat_event_decodes_emoji_in_body (void)
{
    const char *raw = " misha:  :tada: party";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    /* :tada: → 🎉 in the body; the " misha:  " head is untouched. */
    g_assert_cmpstr (e->line, ==, " misha:  \xf0\x9f\x8e\x89 party");
    g_assert_cmpuint (e->line_len, ==, strlen (e->line));
    /* Nick still resolves and matches. */
    g_assert_cmpuint (e->sender_len, ==, 5);
    g_assert_true (memcmp (e->line + e->sender_off, "misha", 5) == 0);
    /* Body slice now points at the decoded text. */
    g_assert_cmpuint (e->body_len, ==, strlen ("\xf0\x9f\x8e\x89 party"));
    g_assert_true (memcmp (e->line + e->body_off, "\xf0\x9f\x8e\x89 party",
                           e->body_len)
                   == 0);

    hx_chat_event_free (e);
}

/* A colon-shaped token in the NICK must not be decoded — only the body is
 * converted. (The nick "joy" with a trailing colon is the separator, not a
 * :joy: shortcode.) */
static void
test_chat_event_does_not_decode_nick (void)
{
    /* If the whole line were decoded, ":joy:" formed across the nick colon
	 * boundary could misfire. Here the body has the shortcode and the nick
	 * is plain — verify the nick text survives verbatim. */
    const char *raw = " bob:  hi :fire:";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_cmpstr (e->line, ==, " bob:  hi \xf0\x9f\x94\xa5");
    g_assert_cmpuint (e->sender_len, ==, 3);
    g_assert_true (memcmp (e->line + e->sender_off, "bob", 3) == 0);

    hx_chat_event_free (e);
}

/* No-sender line (here: starts with a colon, so the nick split finds an
 * empty nick and bails). The whole line is treated as body, so the
 * shortcode still decodes. */
static void
test_chat_event_decodes_emoji_when_unsplit (void)
{
    const char *raw = ":tada: everyone";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_cmpuint (e->sender_len, ==, 0);
    g_assert_cmpstr (e->line, ==, "\xf0\x9f\x8e\x89 everyone");
    g_assert_cmpuint (e->line_len, ==, strlen (e->line));

    hx_chat_event_free (e);
}

/* Unknown / non-shortcode colon runs are left exactly as-is (no false
 * positives on timestamps, ratios, unknown tokens). */
static void
test_chat_event_leaves_non_shortcodes (void)
{
    const char *raw = " misha:  meet at 10:30, ratio 4:3, :notacode:";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);

    g_assert_cmpstr (e->line, ==, raw);
    g_assert_cmpuint (e->line_len, ==, strlen (raw));

    hx_chat_event_free (e);
}

/* Phase E6: with the toggle off, the decode is a no-op — the body keeps
 * its literal :shortcode: text. */
static void
test_chat_event_decode_respects_toggle (void)
{
    gtkhx_text_set_emoji_shortcodes_enabled (FALSE);
    const char *raw = " misha:  :tada: party";
    HxChatEvent *e = hx_chat_event_new (raw, strlen (raw), 0, /*uid=*/0, NULL);
    g_assert_cmpstr (e->line, ==, raw); /* unchanged */
    hx_chat_event_free (e);
    gtkhx_text_set_emoji_shortcodes_enabled (TRUE); /* restore default */
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/chat_event/typical_line",
                     test_chat_event_typical_line);
    g_test_add_func ("/proto/chat_event/wire_uid",
                     test_chat_event_carries_the_wire_uid);
    g_test_add_func ("/proto/chat_event/uid_defaults_zero",
                     test_chat_event_uid_defaults_to_zero);
    g_test_add_func ("/proto/chat_event/preserves_cid",
                     test_chat_event_preserves_cid);

    g_test_add_func ("/proto/chat_event/is_self_matches",
                     test_chat_event_is_self_matches);
    g_test_add_func ("/proto/chat_event/is_self_mismatch",
                     test_chat_event_is_self_mismatch);
    g_test_add_func ("/proto/chat_event/is_self_substring_does_not_match",
                     test_chat_event_is_self_substring_does_not_match);
    g_test_add_func ("/proto/chat_event/is_self_null_self_nick",
                     test_chat_event_is_self_null_self_nick);
    g_test_add_func ("/proto/chat_event/is_self_empty_self_nick",
                     test_chat_event_is_self_empty_self_nick);

    g_test_add_func ("/proto/chat_event/info_prefix_detected",
                     test_chat_event_info_prefix_detected);
    g_test_add_func ("/proto/chat_event/info_prefix_skips_split",
                     test_chat_event_info_prefix_skips_split);

    g_test_add_func ("/proto/chat_event/no_sender_emote",
                     test_chat_event_no_sender_emote);
    g_test_add_func ("/proto/chat_event/no_sender_long_pre_colon",
                     test_chat_event_no_sender_long_pre_colon);

    g_test_add_func ("/proto/chat_event/empty_line",
                     test_chat_event_empty_line);
    g_test_add_func ("/proto/chat_event/null_raw", test_chat_event_null_raw);

    g_test_add_func ("/proto/chat_event/utf8_passthrough",
                     test_chat_event_utf8_passthrough);
    g_test_add_func ("/proto/chat_event/mac_roman_converts",
                     test_chat_event_mac_roman_converts);

    /* Phase E3 — :shortcode: → emoji decode at display time. */
    g_test_add_func ("/proto/chat_event/decodes_emoji_in_body",
                     test_chat_event_decodes_emoji_in_body);
    g_test_add_func ("/proto/chat_event/does_not_decode_nick",
                     test_chat_event_does_not_decode_nick);
    g_test_add_func ("/proto/chat_event/decodes_emoji_when_unsplit",
                     test_chat_event_decodes_emoji_when_unsplit);
    g_test_add_func ("/proto/chat_event/leaves_non_shortcodes",
                     test_chat_event_leaves_non_shortcodes);
    g_test_add_func ("/proto/chat_event/decode_respects_toggle",
                     test_chat_event_decode_respects_toggle);

    g_test_add_func ("/proto/chat_event/copy_preserves_fields",
                     test_chat_event_copy_preserves_fields);
    g_test_add_func ("/proto/chat_event/free_null_is_noop",
                     test_chat_event_free_null_is_noop);
    g_test_add_func ("/proto/chat_event/copy_null_returns_null",
                     test_chat_event_copy_null_returns_null);

    /* Phase 9.D — inline-media attach + placeholder formatter. */
    g_test_add_func ("/proto/chat_event/media_attach_round_trips",
                     test_chat_event_media_attach_round_trips);
    g_test_add_func ("/proto/chat_event/media_attach_copy_deep",
                     test_chat_event_media_attach_copy_deep);
    g_test_add_func ("/proto/chat_event/media_attach_detach",
                     test_chat_event_media_attach_detach);
    g_test_add_func ("/proto/chat_event/media_placeholder_full",
                     test_chat_event_media_placeholder_full);
    g_test_add_func ("/proto/chat_event/media_placeholder_minimal",
                     test_chat_event_media_placeholder_minimal);
    g_test_add_func ("/proto/chat_event/media_placeholder_null",
                     test_chat_event_media_placeholder_null);
    g_test_add_func ("/proto/chat_event/media_placeholder_unknown_mime",
                     test_chat_event_media_placeholder_unknown_mime_passes_through);
    g_test_add_func ("/proto/chat_event/media_placeholder_rejects_invalid_utf8",
                     test_chat_event_media_placeholder_rejects_invalid_utf8_mime);
    g_test_add_func ("/proto/chat_event/media_placeholder_clickable_embeds_token",
                     test_chat_event_media_placeholder_clickable_embeds_token);
    g_test_add_func ("/proto/chat_event/media_parse_token_finds_embedded",
                     test_chat_event_media_parse_token_finds_embedded);

    return g_test_run ();
}
