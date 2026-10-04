/*
 * tests/proto/test_chat_event.c — the placeholder row a chat line with
 * inline media draws in place of its picture
 * (hx_chat_media_placeholder_line). The HxChatEvent itself is built in
 * Rust, and tested there (gtkhx-core boxed/chat.rs).
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "proto_helpers.h"

static void
test_chat_event_media_placeholder_full (void)
{
    HxChatMedia m = {
        .id = (guint8 *)"x",
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
        .id = (guint8 *)"x",
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
        .id = (guint8 *)"x",
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
        .id = (guint8 *)"x",
        .id_len = 1,
        .mime = "\xC3\xC3invalid",
        .mime_len = 9,
    };
    char *p = hx_chat_media_placeholder_line (&m);
    g_assert_nonnull (p);
    g_assert_cmpstr (p, ==, "[image · click to view]");
    g_free (p);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/chat_event/media_placeholder_full",
                     test_chat_event_media_placeholder_full);
    g_test_add_func ("/proto/chat_event/media_placeholder_minimal",
                     test_chat_event_media_placeholder_minimal);
    g_test_add_func ("/proto/chat_event/media_placeholder_null",
                     test_chat_event_media_placeholder_null);
    g_test_add_func (
        "/proto/chat_event/media_placeholder_unknown_mime",
        test_chat_event_media_placeholder_unknown_mime_passes_through);
    g_test_add_func (
        "/proto/chat_event/media_placeholder_rejects_invalid_utf8",
        test_chat_event_media_placeholder_rejects_invalid_utf8_mime);

    return g_test_run ();
}
