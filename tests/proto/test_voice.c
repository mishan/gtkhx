/*
 * tests/proto/test_voice.c — pin the C-side wire shape of the
 * fogWraith voice-chat extension (Capabilities-Voice.md).
 *
 * The hotline-proto Rust crate has its own dense unit tests for the
 * voice builders + parsers (`cargo test -p hotline-proto voice`,
 * 30 cases as of Phase 8.A). What this file adds:
 *
 *   wire   — drive each hx_send_voice_* through the C-side wrapper
 *            in src/voice.c, capture the bytes hlpack_chunks
 *            produces, and verify the chunk shape matches the
 *            spec's documented opcode / field layout. This catches
 *            misuse on the C side that the Rust unit tests can't
 *            (passing the wrong scratch size, forgetting the
 *            CAP_VOICE gate, etc.).
 *
 *   gate   — verify the CAP_VOICE-not-negotiated gate is honoured.
 *            Same shape as test_chat_history's cap-gate test —
 *            sending a 600-606 to a server that didn't echo the
 *            cap earns a task-error every time, and the gate
 *            saves the user the toast.
 *
 *   ffi    — call gtkhx_proto_parse_voice_reply on a hand-packed
 *            JOIN reply body and verify the typed extractor walks
 *            it correctly. The Rust side has analog tests but
 *            this one pins the FFI ABI layout (offsets, presence
 *            flags) as the C side observes it.
 *
 *   pin    — the new field IDs and opcode constants match the
 *            spec (HTLC_CAP_VOICE, 0x01F5-0x01F9, 600-606).
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <glib.h>
#include "compat.h"   /* PACKED — required before hotline.h */
#include "protocol.h"
#include "hotline.h"
#include "hl_access.h"  /* HL_ACCESS_VOICE_CHAT */
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "voice.h"

/* Production network.c is too heavyweight to link into a unit test
 * (it transitively pulls GIOChannel + cipher + compress + signal
 * emit). Stub hlwrite_chunks with just the buffer side — what the
 * test cares about. Same trick test_chat_history.c uses. */
extern void hlwrite_chunks (struct htlc_conn *htlc, guint32 type,
                            guint32 flag, const struct hx_chunk *chunks,
                            int hc);
void
hlwrite_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                const struct hx_chunk *chunks, int hc)
{
    hlpack_chunks (htlc, type, flag, chunks, hc);
}

/* voice.c now registers task_new() entries before each
 * hlwrite_chunks so the TASK reply path (rcv_task_voice_*) finds
 * the session bookkeeping. The unit test doesn't exercise the rcv
 * path; we stub task_new + the rcv_task_voice_* handlers as
 * no-ops so the link resolves. The wire-format assertions still
 * pin the chunk layout the test cares about. */
struct task;
typedef void (*rcv_task_fn) (struct htlc_conn *htlc, void *ptr, void *data);
extern struct task *task_new (struct htlc_conn *htlc, rcv_task_fn rcv,
                              void *ptr, void *data, const char *str);
struct task *
task_new (struct htlc_conn *htlc, rcv_task_fn rcv, void *ptr, void *data,
          const char *str)
{
    (void) htlc;
    (void) rcv;
    (void) ptr;
    (void) data;
    (void) str;
    return NULL;
}

extern void rcv_task_voice_join (struct htlc_conn *htlc, void *channel_ptr);
void
rcv_task_voice_join (struct htlc_conn *htlc, void *channel_ptr)
{
    (void) htlc;
    (void) channel_ptr;
}

extern void rcv_task_voice_simple_ack (struct htlc_conn *htlc,
                                       void *opcode_ptr, void *cid_ptr);
void
rcv_task_voice_simple_ack (struct htlc_conn *htlc, void *opcode_ptr,
                           void *cid_ptr)
{
    (void) htlc;
    (void) opcode_ptr;
    (void) cid_ptr;
}

/* ---------- constants pin ---------- */

static void
test_constants_are_stable (void)
{
    /* Capability bit. */
    g_assert_cmphex (HTLC_CAP_VOICE, ==, 0x0004u);

    /* Data field IDs — 0x01F5-0x01F9 per spec. */
    g_assert_cmphex (HTLC_DATA_VOICE_SDP,          ==, 0x01f5u);
    g_assert_cmphex (HTLC_DATA_VOICE_ICE,          ==, 0x01f6u);
    g_assert_cmphex (HTLC_DATA_VOICE_CODEC,        ==, 0x01f7u);
    g_assert_cmphex (HTLC_DATA_VOICE_MUTED,        ==, 0x01f8u);
    g_assert_cmphex (HTLS_DATA_VOICE_PARTICIPANTS, ==, 0x01f9u);

    /* Opcodes — 600-606 per spec. */
    g_assert_cmpuint (HTLC_HDR_VOICE_JOIN,        ==, 600u);
    g_assert_cmpuint (HTLC_HDR_VOICE_LEAVE,       ==, 601u);
    g_assert_cmpuint (HTLS_HDR_VOICE_SDP_OFFER,   ==, 602u);
    g_assert_cmpuint (HTLC_HDR_VOICE_SDP_ANSWER,  ==, 603u);
    g_assert_cmpuint (HTLC_HDR_VOICE_ICE,         ==, 604u);
    g_assert_cmpuint (HTLS_HDR_VOICE_ROOM_STATUS, ==, 605u);
    g_assert_cmpuint (HTLC_HDR_VOICE_MUTE,        ==, 606u);

    /* Access bit 55 — first byte beyond mhxd's published bitmap. */
    g_assert_cmpuint (HL_ACCESS_VOICE_CHAT, ==, 55u);
}

/* ---------- htlc lifecycle helpers (copied from test_chat_history) -- */

static void
htlc_init (struct htlc_conn *htlc, guint64 caps)
{
    memset (htlc, 0, sizeof (*htlc));
    htlc->trans = 1;
    htlc->caps = caps;
}

static void
htlc_free (struct htlc_conn *htlc)
{
    g_free (htlc->in.buf);
    g_free (htlc->out.buf);
    htlc->in.buf = NULL;
    htlc->out.buf = NULL;
}

static void
flip_out_to_in (struct htlc_conn *htlc)
{
    g_free (htlc->in.buf);
    htlc->in.buf = htlc->out.buf;
    htlc->in.pos = htlc->out.len;
    htlc->out.buf = NULL;
    htlc->out.pos = 0;
    htlc->out.len = 0;
}

/* Pull the wire-side u32 transaction-type out of the header that
 * hlpack_chunks just wrote to htlc->out.buf. */
static guint32
read_out_type (const struct htlc_conn *htlc)
{
    g_assert_cmpuint (htlc->out.len, >=, SIZEOF_HL_HDR);
    return ((guint32) htlc->out.buf[0] << 24)
         | ((guint32) htlc->out.buf[1] << 16)
         | ((guint32) htlc->out.buf[2] << 8)
         |  (guint32) htlc->out.buf[3];
}

/* ---------- send-path gating ---------- */

static void
test_send_skipped_without_cap (void)
{
    /* Every voice send respects the CAP_VOICE gate. Without the cap
     * negotiated, hx_send_voice_join returns FALSE and writes
     * nothing — saves the user a task-error toast. */
    struct htlc_conn htlc;
    htlc_init (&htlc, /*caps=*/0);
    g_assert_false (hx_send_voice_join (&htlc, 0));
    g_assert_cmpuint (htlc.out.len, ==, 0);
    htlc_free (&htlc);
}

static void
test_send_join_with_cap_writes_chat_id (void)
{
    /* JOIN (600) carries one chunk: CHAT_ID (u32 BE). */
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_VOICE);

    g_assert_true (hx_send_voice_join (&htlc, /*cid=*/42));
    flip_out_to_in (&htlc);

    int saw_cid = 0, total = 0;
    dh_start (&htlc)
    {
        total++;
        switch (_type) {
        case HTLC_DATA_CHAT_ID:
            saw_cid++;
            g_assert_cmpuint (_len, ==, 4);
            /* big-endian 42 */
            g_assert_cmpuint (dh->data[0], ==, 0);
            g_assert_cmpuint (dh->data[1], ==, 0);
            g_assert_cmpuint (dh->data[2], ==, 0);
            g_assert_cmpuint (dh->data[3], ==, 42);
            break;
        default:
            g_error ("unexpected chunk type 0x%04x in JOIN body", _type);
        }
    }
    dh_end ();
    g_assert_cmpint (saw_cid, ==, 1);
    g_assert_cmpint (total, ==, 1);

    htlc_free (&htlc);
}

static void
test_send_leave_emits_chat_id (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_VOICE);

    g_assert_true (hx_send_voice_leave (&htlc, /*cid=*/7));
    /* Verify the wire-side header type BEFORE flipping — read_out_type
     * looks at out.buf, which flip_out_to_in moves under in.buf. */
    g_assert_cmphex (read_out_type (&htlc), ==, HTLC_HDR_VOICE_LEAVE);

    flip_out_to_in (&htlc);

    int saw_cid = 0;
    dh_start (&htlc)
    {
        switch (_type) {
        case HTLC_DATA_CHAT_ID:
            saw_cid++;
            g_assert_cmpuint (_len, ==, 4);
            g_assert_cmpuint (dh->data[3], ==, 7);
            break;
        default:
            g_error ("unexpected chunk 0x%04x in LEAVE body", _type);
        }
    }
    dh_end ();
    g_assert_cmpint (saw_cid, ==, 1);

    htlc_free (&htlc);
}

static void
test_send_sdp_answer_emits_chat_id_and_sdp (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_VOICE);

    const guint8 sdp[] = "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\n";
    g_assert_true (hx_send_voice_sdp_answer (&htlc, /*cid=*/3, sdp,
                                             sizeof (sdp) - 1));
    flip_out_to_in (&htlc);

    int saw_cid = 0, saw_sdp = 0, total = 0;
    dh_start (&htlc)
    {
        total++;
        switch (_type) {
        case HTLC_DATA_CHAT_ID:
            saw_cid++;
            g_assert_cmpuint (_len, ==, 4);
            break;
        case HTLC_DATA_VOICE_SDP:
            saw_sdp++;
            g_assert_cmpuint (_len, ==, sizeof (sdp) - 1);
            g_assert_cmpint (memcmp (dh->data, sdp, _len), ==, 0);
            break;
        default:
            g_error ("unexpected chunk type 0x%04x in ANSWER body", _type);
        }
    }
    dh_end ();
    g_assert_cmpint (saw_cid, ==, 1);
    g_assert_cmpint (saw_sdp, ==, 1);
    g_assert_cmpint (total, ==, 2);
    htlc_free (&htlc);
}

static void
test_send_sdp_answer_rejects_empty (void)
{
    /* Empty SDP would tell the server we accept nothing — the
     * builder rejects it, and the C wrapper also rejects it before
     * reaching the builder. Either way, nothing on the wire. */
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_VOICE);
    g_assert_false (
        hx_send_voice_sdp_answer (&htlc, 0, (const guint8 *) "", 0));
    g_assert_cmpuint (htlc.out.len, ==, 0);
    htlc_free (&htlc);
}

static void
test_send_ice_allows_end_of_candidates (void)
{
    /* End-of-candidates marker per spec: empty VOICE_ICE chunk. */
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_VOICE);

    g_assert_true (hx_send_voice_ice (&htlc, /*cid=*/9, NULL, 0));
    flip_out_to_in (&htlc);

    int saw_cid = 0, saw_ice = 0;
    dh_start (&htlc)
    {
        switch (_type) {
        case HTLC_DATA_CHAT_ID:
            saw_cid++;
            break;
        case HTLC_DATA_VOICE_ICE:
            saw_ice++;
            g_assert_cmpuint (_len, ==, 0);
            break;
        default:
            g_error ("unexpected chunk 0x%04x in ICE body", _type);
        }
    }
    dh_end ();
    g_assert_cmpint (saw_cid, ==, 1);
    g_assert_cmpint (saw_ice, ==, 1);
    htlc_free (&htlc);
}

static void
test_send_mute_normalises_to_zero_or_one (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, HTLC_CAP_VOICE);

    /* Pass a non-canonical TRUE — the C wrapper normalises to 1. */
    g_assert_true (hx_send_voice_mute (&htlc, /*cid=*/4, /*muted=*/42));
    flip_out_to_in (&htlc);

    int saw_muted = 0;
    dh_start (&htlc)
    {
        if (_type == HTLC_DATA_VOICE_MUTED) {
            saw_muted++;
            g_assert_cmpuint (_len, ==, 2);
            g_assert_cmpuint (dh->data[0], ==, 0);
            g_assert_cmpuint (dh->data[1], ==, 1);
        }
    }
    dh_end ();
    g_assert_cmpint (saw_muted, ==, 1);
    htlc_free (&htlc);
}

/* ---------- receive-path FFI ABI ---------- */

/* Hand-pack a chunk into `out` at `*off`. */
static void
pack_chunk (guint8 *out, gsize *off, guint16 tag, const guint8 *data,
            guint16 len)
{
    out[(*off)++] = (guint8) (tag >> 8);
    out[(*off)++] = (guint8) (tag & 0xff);
    out[(*off)++] = (guint8) (len >> 8);
    out[(*off)++] = (guint8) (len & 0xff);
    if (len) {
        memcpy (out + *off, data, len);
        *off += len;
    }
}

static void
test_parse_voice_reply_extracts_join_reply_shape (void)
{
    /* Hand-pack a JOIN reply body: header zeros + CHAT_ID + SDP +
     * CODEC + PARTICIPANTS. The Rust parse_voice_reply walks past
     * the 22-byte header (ChunkIter::over_message contract); we
     * provide that buffer shape. */
    guint8 buf[256];
    memset (buf, 0, sizeof (buf));
    gsize off = SIZEOF_HL_HDR;

    /* CHAT_ID = 42 (u32 BE). */
    guint8 cid_be[4] = {0, 0, 0, 42};
    pack_chunk (buf, &off, HTLC_DATA_CHAT_ID, cid_be, 4);

    /* SDP. */
    const guint8 sdp[] = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 0\r\n";
    pack_chunk (buf, &off, HTLS_DATA_VOICE_SDP, sdp, sizeof (sdp) - 1);

    /* Codec name. */
    const guint8 codec[] = "PCMU";
    pack_chunk (buf, &off, HTLS_DATA_VOICE_CODEC, codec, sizeof (codec) - 1);

    /* One participant: uid=5, unmuted, codec=PCMU(0). */
    guint8 parts[6] = {0, 5, 0, 0, 0, 0};
    pack_chunk (buf, &off, HTLS_DATA_VOICE_PARTICIPANTS, parts,
                sizeof (parts));

    struct gtkhx_proto_voice_reply r;
    g_assert_true (gtkhx_proto_parse_voice_reply (buf, off, &r));
    g_assert_cmpuint (r.cid, ==, 42);
    g_assert_true (r.sdp_present);
    g_assert_true (r.codec_present);
    g_assert_true (r.participants_present);
    g_assert_false (r.muted_present);
    g_assert_false (r.ice_present);
    g_assert_cmpuint (r.sdp_len, ==, sizeof (sdp) - 1);
    g_assert_cmpuint (r.codec_len, ==, sizeof (codec) - 1);
    g_assert_cmpuint (r.participants_len, ==, sizeof (parts));

    /* The variable-length payloads are fetched separately via the
     * per-field accessor. */
    const guint8 *sdp_out = NULL;
    gsize sdp_out_len = 0;
    g_assert_true (gtkhx_proto_voice_reply_field (
        buf, off, /*field=SDP*/ 0, &sdp_out, &sdp_out_len));
    g_assert_cmpuint (sdp_out_len, ==, sizeof (sdp) - 1);
    g_assert_cmpint (memcmp (sdp_out, sdp, sdp_out_len), ==, 0);

    /* Asking for an absent field returns false. */
    g_assert_false (gtkhx_proto_voice_reply_field (
        buf, off, /*field=ICE*/ 1, NULL, NULL));

    /* Invalid field id returns false. */
    g_assert_false (gtkhx_proto_voice_reply_field (
        buf, off, /*field=*/99, NULL, NULL));
}

static void
test_parse_voice_participants_walks_packed_entries (void)
{
    /* Three participants: uid 5 muted, 12 unmuted, 23 unmuted. */
    guint8 blob[18];
    /* uid=5, flags=1 (muted), codec=0 */
    blob[0] = 0;
    blob[1] = 5;
    blob[2] = 0;
    blob[3] = 1;
    blob[4] = 0;
    blob[5] = 0;
    /* uid=12, flags=0, codec=0 */
    blob[6] = 0;
    blob[7] = 12;
    blob[8] = 0;
    blob[9] = 0;
    blob[10] = 0;
    blob[11] = 0;
    /* uid=23, flags=0, codec=0 */
    blob[12] = 0;
    blob[13] = 23;
    blob[14] = 0;
    blob[15] = 0;
    blob[16] = 0;
    blob[17] = 0;

    struct gtkhx_proto_voice_participant ents[8];
    size_t n = gtkhx_proto_parse_voice_participants (blob, sizeof (blob),
                                                     ents, 8);
    g_assert_cmpuint (n, ==, 3);
    g_assert_cmpuint (ents[0].user_id, ==, 5);
    g_assert_cmphex (ents[0].flags, ==, 0x0001);
    g_assert_cmpuint (ents[1].user_id, ==, 12);
    g_assert_cmphex (ents[1].flags, ==, 0x0000);
    g_assert_cmpuint (ents[2].user_id, ==, 23);
}

static void
test_parse_voice_mid_label_accepts_send_and_user (void)
{
    uint16_t uid = 0;
    g_assert_cmpuint (
        gtkhx_proto_parse_voice_mid_label ((const uint8_t *) "send", 4,
                                           &uid),
        ==, GTKHX_PROTO_VOICE_MID_SEND);
    /* SEND doesn't write uid. */
    g_assert_cmpuint (uid, ==, 0);

    g_assert_cmpuint (
        gtkhx_proto_parse_voice_mid_label ((const uint8_t *) "user-12345",
                                           10, &uid),
        ==, GTKHX_PROTO_VOICE_MID_USER);
    g_assert_cmpuint (uid, ==, 12345);

    /* Spec violations: leading zero, uid 0, overflow. */
    g_assert_cmpuint (
        gtkhx_proto_parse_voice_mid_label ((const uint8_t *) "user-0", 6,
                                           &uid),
        ==, GTKHX_PROTO_VOICE_MID_INVALID);
    g_assert_cmpuint (
        gtkhx_proto_parse_voice_mid_label ((const uint8_t *) "user-05", 7,
                                           &uid),
        ==, GTKHX_PROTO_VOICE_MID_INVALID);
    g_assert_cmpuint (
        gtkhx_proto_parse_voice_mid_label ((const uint8_t *) "user-65536", 10,
                                           &uid),
        ==, GTKHX_PROTO_VOICE_MID_INVALID);
}

static void
test_parse_voice_sdp_summary_extracts_pcmu_and_mids (void)
{
    const char *sdp = "v=0\r\n"
                      "a=group:BUNDLE user-5 send\r\n"
                      "m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
                      "a=mid:user-5\r\n"
                      "a=rtpmap:0 PCMU/8000\r\n"
                      "m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
                      "a=mid:send\r\n";
    struct gtkhx_proto_voice_sdp_summary sum;
    g_assert_true (gtkhx_proto_parse_voice_sdp_summary (
        (const uint8_t *) sdp, strlen (sdp), &sum));
    g_assert_cmpuint (sum.mid_count, ==, 2);
    g_assert_cmpuint (sum.unknown_mid_count, ==, 0);
    g_assert_cmpuint (sum.bundle_count, ==, 2);
    g_assert_true (sum.has_pcmu);
    g_assert_false (sum.has_disabled_slot);
}

/* Regression (Copilot review): VoiceSdpSummaryOut.mid_count is
 * documented as "the total number of `a=mid:` lines seen (both
 * recognised and not)". Earlier implementation reported only the
 * recognised count, under-reporting whenever the SDP carried a
 * malformed label like `user-0` (UID 0 reserved) or `user-05`
 * (leading zero forbidden). Now mid_count = recognised + unknown
 * so the field matches its doc. */
static void
test_parse_voice_sdp_summary_mid_count_includes_unknowns (void)
{
    const char *sdp = "v=0\r\n"
                      "a=mid:user-12\r\n"   /* recognised */
                      "a=mid:foo\r\n"       /* unknown */
                      "a=mid:user-0\r\n"    /* spec violation: UID 0 reserved */
                      "a=mid:user-05\r\n"   /* spec violation: leading zero */
                      "a=mid:send\r\n";     /* recognised */
    struct gtkhx_proto_voice_sdp_summary sum;
    g_assert_true (gtkhx_proto_parse_voice_sdp_summary (
        (const uint8_t *) sdp, strlen (sdp), &sum));
    g_assert_cmpuint (sum.unknown_mid_count, ==, 3);
    /* mid_count is the total across both buckets — the field's
     * point is "how many a=mid lines did the SDP have?", not
     * "how many were spec-conformant?". */
    g_assert_cmpuint (sum.mid_count, ==, 5);
}

static void
test_parse_voice_ice_json_roundtrip (void)
{
    /* Build, then parse, then verify the borrowed strings. */
    guint8 buf[512];
    const guint8 cand[] = "candidate:1 1 UDP 1 192.0.2.1 5004 typ host";
    const guint8 mid[] = "send";
    size_t n = gtkhx_proto_build_voice_ice_json (
        cand, sizeof (cand) - 1, mid, sizeof (mid) - 1, /*mline=*/0,
        /*mline_present=*/true, NULL, 0, buf, sizeof (buf));
    g_assert_cmpuint (n, >, 0);

    struct gtkhx_proto_voice_ice_candidate out;
    struct gtkhx_proto_voice_ice_handle *h
        = gtkhx_proto_parse_voice_ice_json (buf, n, &out);
    g_assert_nonnull (h);
    g_assert_cmpuint (out.candidate_len, ==, sizeof (cand) - 1);
    g_assert_cmpint (
        memcmp (out.candidate_ptr, cand, out.candidate_len), ==, 0);
    g_assert_cmpuint (out.sdp_mid_len, ==, sizeof (mid) - 1);
    g_assert_true (out.sdp_mline_index_present);
    g_assert_cmpuint (out.sdp_mline_index, ==, 0);
    g_assert_false (out.is_end_of_candidates);

    gtkhx_proto_voice_ice_free (h);
}

static void
test_parse_voice_ice_json_end_of_candidates (void)
{
    /* Empty candidate string is the EOC marker per spec; sdpMid
     * remains a required key so we set it to "send" matching the
     * spec's annotated EOC example. */
    const guint8 *json
        = (const guint8 *) "{\"candidate\":\"\",\"sdpMid\":\"send\"}";
    struct gtkhx_proto_voice_ice_candidate out;
    struct gtkhx_proto_voice_ice_handle *h
        = gtkhx_proto_parse_voice_ice_json (json, strlen ((const char *) json),
                                            &out);
    g_assert_nonnull (h);
    g_assert_true (out.is_end_of_candidates);
    g_assert_cmpuint (out.candidate_len, ==, 0);
    g_assert_cmpuint (out.sdp_mid_len, ==, 4);
    gtkhx_proto_voice_ice_free (h);
}

static void
test_parse_voice_ice_json_rejects_garbage (void)
{
    /* Garbage JSON returns NULL handle, doesn't crash. */
    const guint8 *junk = (const guint8 *) "not json";
    struct gtkhx_proto_voice_ice_candidate out;
    struct gtkhx_proto_voice_ice_handle *h
        = gtkhx_proto_parse_voice_ice_json (junk, strlen ((const char *) junk),
                                            &out);
    g_assert_null (h);
}

/* Regression (Copilot review): parse rejects payloads that omit the
 * required `candidate` and/or `sdpMid` keys. Spec mandates both on
 * every non-shorthand DATA_VOICE_ICE payload. */
static void
test_parse_voice_ice_json_rejects_missing_required_keys (void)
{
    struct gtkhx_proto_voice_ice_candidate out;

    /* Empty object. */
    const guint8 *empty = (const guint8 *) "{}";
    g_assert_null (
        gtkhx_proto_parse_voice_ice_json (empty, 2, &out));

    /* Only candidate. */
    const guint8 *only_cand = (const guint8 *) "{\"candidate\":\"c\"}";
    g_assert_null (gtkhx_proto_parse_voice_ice_json (
        only_cand, strlen ((const char *) only_cand), &out));

    /* Only sdpMid. */
    const guint8 *only_mid = (const guint8 *) "{\"sdpMid\":\"send\"}";
    g_assert_null (gtkhx_proto_parse_voice_ice_json (
        only_mid, strlen ((const char *) only_mid), &out));
}

/* Regression (Copilot review): the build shim rejects NULL pointers
 * for the required `candidate` and `sdpMid` fields. Passing NULL
 * for either used to silently omit the key and emit non-conformant
 * wire output. Optional `usernameFragment` may still be NULL with
 * len 0 (key-absent semantics). */
static void
test_build_voice_ice_json_rejects_null_required_keys (void)
{
    guint8 buf[256];
    const guint8 mid[] = "send";

    /* NULL candidate, valid sdpMid → reject. */
    g_assert_cmpuint (gtkhx_proto_build_voice_ice_json (
                          NULL, 0, mid, sizeof (mid) - 1, 0, false,
                          NULL, 0, buf, sizeof (buf)),
                      ==, 0);

    /* Valid candidate, NULL sdpMid → reject. */
    const guint8 cand[] = "candidate:1 1 UDP 1 1.2.3.4 5004 typ host";
    g_assert_cmpuint (gtkhx_proto_build_voice_ice_json (
                          cand, sizeof (cand) - 1, NULL, 0, 0, false,
                          NULL, 0, buf, sizeof (buf)),
                      ==, 0);

    /* Both NULL → reject. */
    g_assert_cmpuint (gtkhx_proto_build_voice_ice_json (
                          NULL, 0, NULL, 0, 0, false, NULL, 0, buf,
                          sizeof (buf)),
                      ==, 0);

    /* Empty-string candidate (EOC) with valid sdpMid → accept;
     * optional usernameFragment NULL → still accept. Positive
     * control so the strict gate doesn't over-reject.
     *
     * Pass sizeof(buf)-1 as the cap so the strstr-friendly NUL
     * terminator below always has a spare slot — the builder
     * contract allows returning exactly out_cap on success, so
     * `buf[n] = '\0'` with n == sizeof(buf) would overflow the
     * stack buffer. Reserving one byte at the cap keeps the
     * terminator safely inside `buf`. */
    const guint8 empty_cand[] = "";
    size_t n = gtkhx_proto_build_voice_ice_json (
        empty_cand, 0, mid, sizeof (mid) - 1, 0, /*mline_present=*/true,
        NULL, 0, buf, sizeof (buf) - 1);
    g_assert_cmpuint (n, >, 0);
    g_assert_cmpuint (n, <, sizeof (buf));
    /* The emitted JSON should carry both keys. */
    buf[n] = '\0';
    g_assert_nonnull (strstr ((const char *) buf, "\"candidate\":\"\""));
    g_assert_nonnull (strstr ((const char *) buf, "\"sdpMid\":\"send\""));
}

/* ---------- main ---------- */

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/voice/constants", test_constants_are_stable);

    g_test_add_func ("/proto/voice/send/skipped-without-cap",
                     test_send_skipped_without_cap);
    g_test_add_func ("/proto/voice/send/join-writes-chat-id",
                     test_send_join_with_cap_writes_chat_id);
    g_test_add_func ("/proto/voice/send/leave-emits-chat-id",
                     test_send_leave_emits_chat_id);
    g_test_add_func ("/proto/voice/send/sdp-answer-emits-chat-id-and-sdp",
                     test_send_sdp_answer_emits_chat_id_and_sdp);
    g_test_add_func ("/proto/voice/send/sdp-answer-rejects-empty",
                     test_send_sdp_answer_rejects_empty);
    g_test_add_func ("/proto/voice/send/ice-allows-end-of-candidates",
                     test_send_ice_allows_end_of_candidates);
    g_test_add_func ("/proto/voice/send/mute-normalises",
                     test_send_mute_normalises_to_zero_or_one);

    g_test_add_func ("/proto/voice/parse/reply-join-shape",
                     test_parse_voice_reply_extracts_join_reply_shape);
    g_test_add_func ("/proto/voice/parse/participants-walk",
                     test_parse_voice_participants_walks_packed_entries);
    g_test_add_func ("/proto/voice/parse/mid-label",
                     test_parse_voice_mid_label_accepts_send_and_user);
    g_test_add_func ("/proto/voice/parse/sdp-summary",
                     test_parse_voice_sdp_summary_extracts_pcmu_and_mids);
    g_test_add_func (
        "/proto/voice/parse/sdp-summary-mid-count-includes-unknowns",
        test_parse_voice_sdp_summary_mid_count_includes_unknowns);
    g_test_add_func ("/proto/voice/parse/ice-json-roundtrip",
                     test_parse_voice_ice_json_roundtrip);
    g_test_add_func ("/proto/voice/parse/ice-json-end-of-candidates",
                     test_parse_voice_ice_json_end_of_candidates);
    g_test_add_func ("/proto/voice/parse/ice-json-rejects-garbage",
                     test_parse_voice_ice_json_rejects_garbage);
    g_test_add_func (
        "/proto/voice/parse/ice-json-rejects-missing-required-keys",
        test_parse_voice_ice_json_rejects_missing_required_keys);
    g_test_add_func (
        "/proto/voice/send/ice-json-rejects-null-required-keys",
        test_build_voice_ice_json_rejects_null_required_keys);

    return g_test_run ();
}
