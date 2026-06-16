/*
 * tests/proto/test_inline_media.c — pin the C-side wire shape of
 * the fogWraith inline-media extension (Capabilities-Inline-Media.md).
 *
 * The Rust crate (hotline-proto::inline_media) has dense unit tests
 * for builders + parsers. What this file adds:
 *
 *   constants — HTLC_CAP_INLINE_MEDIA bit, the 0x0201-0x0212 field
 *               IDs, and the 750 / 751 opcode block all match the
 *               spec's literal values.
 *
 *   cap_gate  — inline_media_cap_ok refuses to send when CAP_INLINE_MEDIA
 *               wasn't echoed by the server. Same shape as
 *               voice.c's voice_cap_ok.
 *
 *   ffi       — every gtkhx_proto_build_* / _parse_* / _extract_*
 *               entry point behaves as the Rust unit tests assert,
 *               with the FFI ABI (struct fields + presence flags +
 *               borrowed pointers) observed from the C side.
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <glib.h>
#include "compat.h"   /* PACKED — required before hotline.h */
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "inline_media.h"
#include "debug.h"

/* ---------- constants pin ---------- */

static void
test_constants_are_stable (void)
{
    /* Capability bit. */
    g_assert_cmphex (HTLC_CAP_INLINE_MEDIA, ==, 0x0008u);

    /* Opcodes — 750 / 751 per spec. */
    g_assert_cmpuint (HTLC_HDR_UPLOAD_MEDIA, ==, 750u);
    g_assert_cmpuint (HTLC_HDR_DOWNLOAD_MEDIA, ==, 751u);
    g_assert_cmphex (HTLC_HDR_UPLOAD_MEDIA, ==, 0x02eeu);
    g_assert_cmphex (HTLC_HDR_DOWNLOAD_MEDIA, ==, 0x02efu);

    /* Companion + payload field IDs. */
    g_assert_cmphex (HTLC_DATA_CHAT_MEDIA_TYPE,         ==, 0x0201u);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_TYPE,         ==, 0x0201u);
    g_assert_cmphex (HTLC_DATA_CHAT_MEDIA_ID,           ==, 0x0202u);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_ID,           ==, 0x0202u);
    g_assert_cmphex (HTLC_DATA_CHAT_MEDIA_PAYLOAD,      ==, 0x0203u);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_PAYLOAD,      ==, 0x0203u);
    g_assert_cmphex (HTLC_DATA_CHAT_MEDIA_DECLARED_TYPE,==, 0x0204u);

    /* Server-supplied canonical metadata. */
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_WIDTH,        ==, 0x0205u);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_HEIGHT,       ==, 0x0206u);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_BYTES,        ==, 0x0207u);

    /* Chunked-upload bookkeeping. */
    g_assert_cmphex (HTLC_DATA_CHAT_MEDIA_UPLOAD_TOKEN, ==, 0x0208u);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_UPLOAD_TOKEN, ==, 0x0208u);
    g_assert_cmphex (HTLC_DATA_CHAT_MEDIA_PART_INDEX,   ==, 0x0209u);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_PART_INDEX,   ==, 0x0209u);
    g_assert_cmphex (HTLC_DATA_CHAT_MEDIA_PART_COUNT,   ==, 0x020au);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_PART_COUNT,   ==, 0x020au);
    g_assert_cmphex (HTLC_DATA_CHAT_MEDIA_PART_FINAL,   ==, 0x020bu);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_PART_FINAL,   ==, 0x020bu);

    /* Server-advertised advisory limits. */
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_MAX_BYTES,        ==, 0x020cu);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_MAX_DIMENSION,    ==, 0x020du);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_MAX_PIXELS,       ==, 0x020eu);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_CHUNK_SIZE,       ==, 0x020fu);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_MAX_FRAMES,       ==, 0x0210u);
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_MAX_DURATION_MS,  ==, 0x0211u);

    /* Optional error-code field. */
    g_assert_cmphex (HTLS_DATA_CHAT_MEDIA_ERROR_CODE, ==, 0x0212u);

    /* Recommended-default cap values; matching the spec's
	 * "Resource Limits" recommended defaults. */
    g_assert_cmpuint (HX_MEDIA_DEFAULT_MAX_BYTES,        ==, 262144u);
    g_assert_cmpuint (HX_MEDIA_DEFAULT_MAX_DIMENSION,    ==, 2048u);
    g_assert_cmpuint (HX_MEDIA_DEFAULT_MAX_PIXELS,       ==, 2048u * 2048u);
    g_assert_cmpuint (HX_MEDIA_DEFAULT_MAX_FRAMES,       ==, 150u);
    g_assert_cmpuint (HX_MEDIA_DEFAULT_MAX_DURATION_MS,  ==, 15000u);
}

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
    g_assert_cmpuint (inline_media_chunk_size (&h), ==,
                      HX_MEDIA_DEFAULT_CHUNK_SIZE);
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
    h.media_max_bytes        = 65536;
    h.media_max_dimension    = 1024;
    h.media_max_pixels       = 1024u * 768u;
    h.media_max_frames       = 50;
    h.media_max_duration_ms  = 5000;

    g_assert_cmpuint (inline_media_max_bytes (&h),       ==, 65536u);
    g_assert_cmpuint (inline_media_max_dimension (&h),   ==, 1024u);
    g_assert_cmpuint (inline_media_max_pixels (&h),      ==, 1024u * 768u);
    g_assert_cmpuint (inline_media_max_frames (&h),      ==, 50u);
    g_assert_cmpuint (inline_media_max_duration_ms (&h), ==, 5000u);
}

static void
test_chunk_size_clamps_oversized_advertisement (void)
{
    struct htlc_conn h;
    memset (&h, 0, sizeof (h));
    h.caps = HTLC_CAP_INLINE_MEDIA;
    /* Server advertises an absurd chunk size — gtkhx clamps to
	 * a sane ceiling to avoid alloc-of-the-month attacks. */
    h.media_chunk_size = 5u * 1024u * 1024u;
    g_assert_cmpuint (inline_media_chunk_size (&h), ==,
                      HX_MEDIA_DEFAULT_CHUNK_SIZE);
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
    h.media_max_bytes        = 65536;
    h.media_max_dimension    = 1024;
    h.media_max_pixels       = 1024u * 768u;
    h.media_chunk_size       = 32000;
    h.media_max_frames       = 50;
    h.media_max_duration_ms  = 5000;
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
    g_assert_cmpuint (inline_media_chunk_size (&h), ==,
                      HX_MEDIA_DEFAULT_CHUNK_SIZE);
    g_assert_cmpuint (inline_media_max_frames (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_FRAMES);
    g_assert_cmpuint (inline_media_max_duration_ms (&h), ==,
                      HX_MEDIA_DEFAULT_MAX_DURATION_MS);
}

/* ---------- FFI builder: single-shot upload ---------- */

static void
test_build_upload_single_shape (void)
{
    const uint8_t payload[] = "\x89PNG\r\n\x1a\nfake-png-bytes";
    struct hx_chunk chunks[3];
    uint8_t scratch[1];
    int32_t hc;

    /* Without declared type → 2 chunks: PAYLOAD + PART_FINAL. */
    hc = gtkhx_proto_build_upload_media_single_chunks (
        payload, sizeof (payload) - 1,
        NULL, 0,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 2);
    g_assert_cmpuint (chunks[0].type, ==, HTLC_DATA_CHAT_MEDIA_PAYLOAD);
    g_assert_cmpuint (chunks[0].len, ==, sizeof (payload) - 1);
    g_assert_cmpuint (chunks[1].type, ==, HTLC_DATA_CHAT_MEDIA_PART_FINAL);
    g_assert_cmpuint (chunks[1].len, ==, 1);
    /* PART_FINAL points at scratch holding 1. */
    g_assert_cmpuint (((const uint8_t *) chunks[1].data)[0], ==, 1);

    /* With declared type → 3 chunks. */
    const uint8_t declared[] = "image/png";
    hc = gtkhx_proto_build_upload_media_single_chunks (
        payload, sizeof (payload) - 1,
        declared, sizeof (declared) - 1,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 3);
    g_assert_cmpuint (chunks[0].type, ==, HTLC_DATA_CHAT_MEDIA_PAYLOAD);
    g_assert_cmpuint (chunks[1].type, ==, HTLC_DATA_CHAT_MEDIA_DECLARED_TYPE);
    g_assert_cmpuint (chunks[2].type, ==, HTLC_DATA_CHAT_MEDIA_PART_FINAL);
}

static void
test_build_upload_single_rejects_empty (void)
{
    struct hx_chunk chunks[3];
    uint8_t scratch[1];
    /* Empty payload — builder refuses. */
    int32_t hc = gtkhx_proto_build_upload_media_single_chunks (
        (const uint8_t *) "", 0, NULL, 0,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 0);
}

/* ---------- FFI builder: chunked first chunk ---------- */

static void
test_build_upload_first_shape (void)
{
    const uint8_t payload[] = "first-chunk-data";
    struct hx_chunk chunks[5];
    uint8_t scratch[5];

    int32_t hc = gtkhx_proto_build_upload_media_first_chunks (
        payload, sizeof (payload) - 1, NULL, 0,
        /* part_count */ 3,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 4);
    g_assert_cmpuint (chunks[0].type, ==, HTLC_DATA_CHAT_MEDIA_PAYLOAD);
    g_assert_cmpuint (chunks[1].type, ==, HTLC_DATA_CHAT_MEDIA_PART_INDEX);
    g_assert_cmpuint (chunks[2].type, ==, HTLC_DATA_CHAT_MEDIA_PART_COUNT);
    g_assert_cmpuint (chunks[3].type, ==, HTLC_DATA_CHAT_MEDIA_PART_FINAL);
    /* PART_INDEX == 0 (big-endian u16). */
    g_assert_cmpuint (((const uint8_t *) chunks[1].data)[0], ==, 0);
    g_assert_cmpuint (((const uint8_t *) chunks[1].data)[1], ==, 0);
    /* PART_COUNT == 3. */
    g_assert_cmpuint (((const uint8_t *) chunks[2].data)[0], ==, 0);
    g_assert_cmpuint (((const uint8_t *) chunks[2].data)[1], ==, 3);
    /* PART_FINAL == 0. */
    g_assert_cmpuint (((const uint8_t *) chunks[3].data)[0], ==, 0);
}

static void
test_build_upload_first_rejects_count_one (void)
{
    const uint8_t payload[] = "x";
    struct hx_chunk chunks[5];
    uint8_t scratch[5];
    int32_t hc = gtkhx_proto_build_upload_media_first_chunks (
        payload, 1, NULL, 0, /* part_count = */ 1,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 0);
}

/* ---------- FFI builder: chunked followup chunk ---------- */

static void
test_build_upload_followup_final_shape (void)
{
    const uint8_t token[] = "session-token-12345";
    const uint8_t payload[] = "last-chunk-bytes";
    struct hx_chunk chunks[4];
    uint8_t scratch[3];

    int32_t hc = gtkhx_proto_build_upload_media_followup_chunks (
        token, sizeof (token) - 1,
        payload, sizeof (payload) - 1,
        /* part_index */ 3, /* final */ true,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 4);
    g_assert_cmpuint (chunks[0].type, ==, HTLC_DATA_CHAT_MEDIA_UPLOAD_TOKEN);
    g_assert_cmpuint (chunks[1].type, ==, HTLC_DATA_CHAT_MEDIA_PART_INDEX);
    g_assert_cmpuint (chunks[2].type, ==, HTLC_DATA_CHAT_MEDIA_PAYLOAD);
    g_assert_cmpuint (chunks[3].type, ==, HTLC_DATA_CHAT_MEDIA_PART_FINAL);
    /* PART_INDEX == 3 (BE u16). */
    g_assert_cmpuint (((const uint8_t *) chunks[1].data)[0], ==, 0);
    g_assert_cmpuint (((const uint8_t *) chunks[1].data)[1], ==, 3);
    /* PART_FINAL == 1. */
    g_assert_cmpuint (((const uint8_t *) chunks[3].data)[0], ==, 1);
}

static void
test_build_upload_followup_rejects_zero_index (void)
{
    const uint8_t token[] = "tok";
    const uint8_t payload[] = "data";
    struct hx_chunk chunks[4];
    uint8_t scratch[3];
    int32_t hc = gtkhx_proto_build_upload_media_followup_chunks (
        token, 3, payload, 4, /* part_index = */ 0, false,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 0);
}

/* ---------- FFI builder: TranDownloadMedia ---------- */

static void
test_build_download_first_request_shape (void)
{
    const uint8_t handle[] = "opaque-handle";
    struct hx_chunk chunks[2];
    uint8_t scratch[2];

    /* First request omits PART_INDEX. */
    int32_t hc = gtkhx_proto_build_download_media_chunks (
        handle, sizeof (handle) - 1,
        /* part_index */ 0, /* present */ false,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 1);
    g_assert_cmpuint (chunks[0].type, ==, HTLC_DATA_CHAT_MEDIA_ID);
    g_assert_cmpuint (chunks[0].len, ==, sizeof (handle) - 1);

    /* Followup chunk-fetch request includes PART_INDEX. */
    hc = gtkhx_proto_build_download_media_chunks (
        handle, sizeof (handle) - 1,
        /* part_index */ 2, /* present */ true,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 2);
    g_assert_cmpuint (chunks[1].type, ==, HTLC_DATA_CHAT_MEDIA_PART_INDEX);
    g_assert_cmpuint (((const uint8_t *) chunks[1].data)[1], ==, 2);
}

static void
test_build_download_rejects_empty_handle (void)
{
    struct hx_chunk chunks[2];
    uint8_t scratch[2];
    int32_t hc = gtkhx_proto_build_download_media_chunks (
        (const uint8_t *) "", 0, 0, false,
        chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    g_assert_cmpint (hc, ==, 0);
}

/* ---------- FFI parser: limits ---------- */

/* Helper: emit one chunk into buf at *pos. tag and payload as
 * documented; returns the new position. */
static size_t
append_chunk (uint8_t *buf, size_t pos, uint16_t tag,
              const uint8_t *payload, uint16_t len)
{
    buf[pos++] = (tag >> 8) & 0xff;
    buf[pos++] = tag & 0xff;
    buf[pos++] = (len >> 8) & 0xff;
    buf[pos++] = len & 0xff;
    memcpy (buf + pos, payload, len);
    return pos + len;
}

static size_t
append_u32_chunk (uint8_t *buf, size_t pos, uint16_t tag, uint32_t value)
{
    uint8_t b[4];
    b[0] = (value >> 24) & 0xff;
    b[1] = (value >> 16) & 0xff;
    b[2] = (value >> 8) & 0xff;
    b[3] = value & 0xff;
    return append_chunk (buf, pos, tag, b, 4);
}

static void
test_extract_limits_picks_up_advertised_fields (void)
{
    /* 22-byte header (zeroed) + the LOGIN reply's chunk run. */
    uint8_t buf[256];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;

    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_MAX_BYTES, 65536);
    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_MAX_DIMENSION, 1024);
    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_CHUNK_SIZE, 32768);

    struct gtkhx_proto_inline_media_limits limits;
    memset (&limits, 0, sizeof (limits));
    g_assert_true (
        gtkhx_proto_extract_inline_media_limits (buf, pos, &limits));
    g_assert_true (limits.max_bytes_present);
    g_assert_cmpuint (limits.max_bytes, ==, 65536u);
    g_assert_true (limits.max_dimension_present);
    g_assert_cmpuint (limits.max_dimension, ==, 1024u);
    g_assert_true (limits.chunk_size_present);
    g_assert_cmpuint (limits.chunk_size, ==, 32768u);
    /* Fields the server didn't advertise. */
    g_assert_false (limits.max_pixels_present);
    g_assert_false (limits.max_frames_present);
    g_assert_false (limits.max_duration_ms_present);
}

/* ---------- FFI parser: chat-relay companion fields ---------- */

static void
test_extract_chat_media_meta_present (void)
{
    uint8_t buf[256];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;

    pos = append_chunk (buf, pos, HTLC_DATA_CHAT, (const uint8_t *) "hi", 2);
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_ID,
                        (const uint8_t *) "opaque-handle", 13);
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_TYPE,
                        (const uint8_t *) "image/png", 9);
    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_WIDTH, 800);
    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_HEIGHT, 600);
    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_BYTES, 124000);

    struct gtkhx_proto_chat_media_meta meta;
    memset (&meta, 0, sizeof (meta));
    int status = gtkhx_proto_extract_chat_media_meta (buf, pos, &meta);
    g_assert_cmpint (status, ==, GTKHX_PROTO_MEDIA_META_PRESENT);
    g_assert_cmpuint (meta.id_len, ==, 13);
    g_assert (memcmp (meta.id_ptr, "opaque-handle", 13) == 0);
    g_assert_cmpuint (meta.type_len, ==, 9);
    g_assert (memcmp (meta.type_ptr, "image/png", 9) == 0);
    g_assert_true (meta.width_present);
    g_assert_cmpuint (meta.width, ==, 800);
    g_assert_true (meta.height_present);
    g_assert_cmpuint (meta.height, ==, 600);
    g_assert_true (meta.bytes_present);
    g_assert_cmpuint (meta.bytes, ==, 124000);
}

static void
test_extract_chat_media_meta_none_when_text_only (void)
{
    uint8_t buf[64];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;
    pos = append_chunk (buf, pos, HTLC_DATA_CHAT, (const uint8_t *) "hi", 2);

    struct gtkhx_proto_chat_media_meta meta;
    int status = gtkhx_proto_extract_chat_media_meta (buf, pos, &meta);
    g_assert_cmpint (status, ==, GTKHX_PROTO_MEDIA_META_NONE);
}

static void
test_extract_chat_media_meta_rejects_orphan (void)
{
    uint8_t buf[64];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;
    /* ID present, TYPE absent → orphan; spec says receiver MUST
	 * reject. */
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_ID,
                        (const uint8_t *) "orphan-handle", 13);

    struct gtkhx_proto_chat_media_meta meta;
    int status = gtkhx_proto_extract_chat_media_meta (buf, pos, &meta);
    g_assert_cmpint (status, ==, GTKHX_PROTO_MEDIA_META_ORPHAN);
}

/* ---------- FFI parser: upload reply ---------- */

static void
test_parse_upload_final_reply_extracts_handle (void)
{
    uint8_t buf[256];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_ID,
                        (const uint8_t *) "new-handle", 10);
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_TYPE,
                        (const uint8_t *) "image/jpeg", 10);
    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_WIDTH, 1024);
    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_HEIGHT, 768);
    pos = append_u32_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_BYTES, 95432);

    struct gtkhx_proto_upload_final_reply r;
    memset (&r, 0, sizeof (r));
    g_assert_true (gtkhx_proto_parse_upload_final_reply (buf, pos, &r));
    g_assert_cmpuint (r.id_len, ==, 10);
    g_assert (memcmp (r.id_ptr, "new-handle", 10) == 0);
    g_assert (memcmp (r.type_ptr, "image/jpeg", 10) == 0);
    g_assert_true (r.width_present);
    g_assert_cmpuint (r.width, ==, 1024);
    g_assert_cmpuint (r.height, ==, 768);
    g_assert_cmpuint (r.bytes, ==, 95432);
}

static void
test_parse_upload_token_reply (void)
{
    uint8_t buf[64];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_UPLOAD_TOKEN,
                        (const uint8_t *) "tok-X", 5);

    const uint8_t *tok = NULL;
    size_t tlen = 0;
    g_assert_true (
        gtkhx_proto_parse_upload_token_reply (buf, pos, &tok, &tlen));
    g_assert_cmpuint (tlen, ==, 5);
    g_assert (memcmp (tok, "tok-X", 5) == 0);
}

/* ---------- FFI parser: download reply + error code ---------- */

static void
test_parse_download_reply_single_shot (void)
{
    uint8_t buf[256];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_PAYLOAD,
                        (const uint8_t *) "\x89PNG...", 7);
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_TYPE,
                        (const uint8_t *) "image/png", 9);
    /* PART_COUNT == 1 (BE u16). */
    uint8_t pc[2] = {0, 1};
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_PART_COUNT, pc, 2);
    /* PART_FINAL == 1 (u8). */
    uint8_t pf = 1;
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_PART_FINAL, &pf, 1);

    struct gtkhx_proto_download_reply r;
    memset (&r, 0, sizeof (r));
    g_assert_true (gtkhx_proto_parse_download_reply (buf, pos, &r));
    g_assert_cmpuint (r.payload_len, ==, 7);
    g_assert (memcmp (r.payload_ptr, "\x89PNG...", 7) == 0);
    g_assert_cmpuint (r.type_len, ==, 9);
    g_assert_cmpuint (r.part_count, ==, 1);
    g_assert_true (r.final_chunk);
}

static void
test_extract_error_code_picks_up_when_present (void)
{
    uint8_t buf[64];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;
    pos = append_chunk (buf, pos, HTLS_DATA_TASKERROR,
                        (const uint8_t *) "Media rejected", 14);
    /* CODE = 1 (PayloadTooLarge), BE u16. */
    uint8_t code[2] = {0, 1};
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_ERROR_CODE, code, 2);

    uint16_t got = gtkhx_proto_extract_media_error_code (buf, pos);
    g_assert_cmpuint (got, ==, 1u);
}

static void
test_extract_error_code_unknown_collapses_to_generic (void)
{
    uint8_t buf[64];
    memset (buf, 0, sizeof (buf));
    size_t pos = SIZEOF_HL_HDR;
    /* CODE = 99 (unspecified-by-spec) — must collapse to 0. */
    uint8_t code[2] = {0, 99};
    pos = append_chunk (buf, pos, HTLS_DATA_CHAT_MEDIA_ERROR_CODE, code, 2);

    uint16_t got = gtkhx_proto_extract_media_error_code (buf, pos);
    g_assert_cmpuint (got, ==, 0u);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    debug_init ();

    g_test_add_func ("/proto/inline_media/constants",
                     test_constants_are_stable);
    g_test_add_func ("/proto/inline_media/cap_gate",
                     test_cap_gate_refuses_without_echo);
    g_test_add_func ("/proto/inline_media/limits_default",
                     test_limits_accessors_use_defaults_when_zero);
    g_test_add_func ("/proto/inline_media/limits_pass_through",
                     test_limits_accessors_pass_through_server_values);
    g_test_add_func ("/proto/inline_media/limits_clamp_chunk_size",
                     test_chunk_size_clamps_oversized_advertisement);
    g_test_add_func ("/proto/inline_media/limits_drop_stale_on_cap_lost",
                     test_limits_accessors_drop_stale_on_cap_lost);
    g_test_add_func ("/proto/inline_media/build_upload_single",
                     test_build_upload_single_shape);
    g_test_add_func ("/proto/inline_media/build_upload_single_empty",
                     test_build_upload_single_rejects_empty);
    g_test_add_func ("/proto/inline_media/build_upload_first",
                     test_build_upload_first_shape);
    g_test_add_func ("/proto/inline_media/build_upload_first_one",
                     test_build_upload_first_rejects_count_one);
    g_test_add_func ("/proto/inline_media/build_upload_followup_final",
                     test_build_upload_followup_final_shape);
    g_test_add_func ("/proto/inline_media/build_upload_followup_zero",
                     test_build_upload_followup_rejects_zero_index);
    g_test_add_func ("/proto/inline_media/build_download_first",
                     test_build_download_first_request_shape);
    g_test_add_func ("/proto/inline_media/build_download_empty_handle",
                     test_build_download_rejects_empty_handle);
    g_test_add_func ("/proto/inline_media/extract_limits",
                     test_extract_limits_picks_up_advertised_fields);
    g_test_add_func ("/proto/inline_media/chat_meta_present",
                     test_extract_chat_media_meta_present);
    g_test_add_func ("/proto/inline_media/chat_meta_none",
                     test_extract_chat_media_meta_none_when_text_only);
    g_test_add_func ("/proto/inline_media/chat_meta_orphan",
                     test_extract_chat_media_meta_rejects_orphan);
    g_test_add_func ("/proto/inline_media/upload_final_reply",
                     test_parse_upload_final_reply_extracts_handle);
    g_test_add_func ("/proto/inline_media/upload_token_reply",
                     test_parse_upload_token_reply);
    g_test_add_func ("/proto/inline_media/download_reply_single",
                     test_parse_download_reply_single_shot);
    g_test_add_func ("/proto/inline_media/error_code_present",
                     test_extract_error_code_picks_up_when_present);
    g_test_add_func ("/proto/inline_media/error_code_unknown",
                     test_extract_error_code_unknown_collapses_to_generic);

    return g_test_run ();
}
