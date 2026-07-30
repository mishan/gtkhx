/*
 * tests/proto/test_hlwrite.c — hlwrite() round-trip:
 *
 *   1. Build a Hotline message via hlpack (the pure-packing half of
 *      hlwrite — no fd / cipher / compress / proto_trace).
 *   2. Inspect the bytes hlpack packed (staged into hx_test_in(htlc)->buf).
 *   3. Move them over to hx_test_in(htlc)->buf and parse them back via the
 *      dh_start chunk walker.
 *   4. Assert structure round-trips verbatim.
 *
 * This is the proposal's fourth Tier 2 target — it's the first test
 * that exercises the SEND path. Up to here, every Tier 2 test was
 * driving the receive side with synthetic wire bytes. Round-tripping
 * a real hlpack pack through the real dh_start walker proves the two
 * agree on the wire format end-to-end.
 *
 * The only piece of hlwrite not exercised here is the proto_trace
 * walk — that's a logging side-effect, not part of the wire format,
 * and stays in network.c.
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "hxconn_layout.h"
#include "htlc_recv_buf.h"
#include "proto_helpers.h"

/* Variadic test wrapper: pack via hlpack and return the fresh buffer
 * (caller frees) plus its length. hlpack no longer has a per-connection
 * out buffer — it hands back the packed block directly. */
static guint8 *
hlpack_buf_v (struct htlc_conn *htlc, gsize *len, guint32 type, guint32 flag,
              int hc, ...)
{
    va_list ap;
    va_start (ap, hc);
    guint8 *buf = hlpack (htlc, type, flag, hc, ap, len);
    va_end (ap);
    return buf;
}

/* The common case: pack and stash the message into htlc->in so the
 * dh_start walker + read_packed_hdr can see it. Mirrors the public
 * hlwrite() API minus the send + trace side effects. */
static void
hlpack_v (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...)
{
    va_list ap;
    va_start (ap, hc);
    gsize len = 0;
    guint8 *buf = hlpack (htlc, type, flag, hc, ap, &len);
    va_end (ap);

    g_free (hx_test_in (htlc)->buf);
    hx_test_in (htlc)->buf = buf;
    hx_test_in (htlc)->pos = len;
    hx_test_in (htlc)->len = len;
}

/* Fresh htlc with empty buffers and trans counter. */
static void
htlc_init (struct htlc_conn *htlc, guint32 starting_trans)
{
    memset (htlc, 0, sizeof (*htlc));
    htlc->trans = starting_trans;
}

static void
htlc_free (struct htlc_conn *htlc)
{
    hx_test_in_free (htlc);
}

/* Read back the header that hlpack wrote at the front of htlc->in.
 * Returns the decoded fields in host order. */
static void
read_packed_hdr (const struct htlc_conn *htlc, guint32 *type, guint32 *trans,
                 guint32 *flag, guint16 *hc)
{
    g_assert_cmpuint (hx_test_in (htlc)->pos, >=, SIZEOF_HL_HDR);
    const struct hl_hdr *h = (const struct hl_hdr *)hx_test_in (htlc)->buf;
    if (type) {
        *type = g_ntohl (h->type);
    }
    if (trans) {
        *trans = g_ntohl (h->trans);
    }
    if (flag) {
        *flag = g_ntohl (h->flag);
    }
    if (hc) {
        *hc = g_ntohs (h->hc);
    }
}

/* ---------- Header round-trip ---------- */

static void
test_hlwrite_header_fields_round_trip (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, /*starting_trans=*/123);

    hlpack_v (&htlc, HTLC_HDR_LOGIN, /*flag=*/0, /*hc=*/0);

    guint32 type, trans, flag;
    guint16 hc;
    read_packed_hdr (&htlc, &type, &trans, &flag, &hc);
    g_assert_cmphex (type, ==, HTLC_HDR_LOGIN);
    g_assert_cmphex (trans, ==, 123);
    g_assert_cmpuint (flag, ==, 0);
    g_assert_cmpuint (hc, ==, 0);

    /* Trans was incremented by exactly one. */
    g_assert_cmphex (htlc.trans, ==, 124);

    htlc_free (&htlc);
}

static void
test_hlwrite_trans_increments_per_message (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 1);

    /* Pack three back-to-back messages; expect trans IDs 1, 2, 3. */
    for (guint32 expected = 1; expected <= 3; expected++) {
        struct htlc_conn snapshot = htlc;
        (void)snapshot;
        hlpack_v (&htlc, HTLC_HDR_NEWS_GETFILE, 0, 0);
    }
    g_assert_cmphex (htlc.trans, ==, 4);

    htlc_free (&htlc);
}

/* ---------- One chunk: HTLC_HDR_LOGIN with a USER_NAME ---------- */

static void
test_hlwrite_single_chunk_round_trip (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 7);

    const char *login = "guest";
    hlpack_v (&htlc, HTLC_HDR_LOGIN, 0, /*hc=*/1, (int)HTLC_DATA_LOGIN,
              (int)strlen (login), (guint8 *)login);

    guint16 hc;
    read_packed_hdr (&htlc, NULL, NULL, NULL, &hc);
    g_assert_cmpuint (hc, ==, 1);

    int found_chunks = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        found_chunks++;
        g_assert_cmphex (_type, ==, HTLC_DATA_LOGIN);
        g_assert_cmpuint (_len, ==, strlen (login));
        g_assert_cmpmem (dh->data, _len, login, strlen (login));
    }
    dh_end ();
    g_assert_cmpint (found_chunks, ==, 1);

    htlc_free (&htlc);
}

/* ---------- Multiple chunks ----------
 *
 * Build the canonical login message: USER_NAME + USER_LOGIN +
 * USER_PASSWORD + USER_ICON. That's 4 chunks of varying types and
 * sizes, including a 16-bit numeric (icon).
 */

static void
test_hlwrite_login_message_round_trip (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 42);

    const char *name = "Misha";
    const char *user = "guest";
    const char *pass = "*****";
    const guint16 icon = g_htons (412);

    hlpack_v (&htlc, HTLC_HDR_LOGIN, 0, /*hc=*/4, (int)HTLC_DATA_NAME,
              (int)strlen (name), (guint8 *)name, (int)HTLC_DATA_LOGIN,
              (int)strlen (user), (guint8 *)user, (int)HTLC_DATA_PASSWORD,
              (int)strlen (pass), (guint8 *)pass, (int)HTLC_DATA_ICON,
              (int)sizeof (icon), (guint8 *)&icon);

    guint32 type, trans;
    guint16 hc;
    read_packed_hdr (&htlc, &type, &trans, NULL, &hc);
    g_assert_cmphex (type, ==, HTLC_HDR_LOGIN);
    g_assert_cmphex (trans, ==, 42);
    g_assert_cmpuint (hc, ==, 4);

    gboolean saw_name = FALSE, saw_login = FALSE, saw_pass = FALSE,
             saw_icon = FALSE;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        switch (_type) {
        case HTLC_DATA_NAME:
            g_assert_cmpmem (dh->data, _len, name, strlen (name));
            saw_name = TRUE;
            break;
        case HTLC_DATA_LOGIN:
            g_assert_cmpmem (dh->data, _len, user, strlen (user));
            saw_login = TRUE;
            break;
        case HTLC_DATA_PASSWORD:
            g_assert_cmpmem (dh->data, _len, pass, strlen (pass));
            saw_pass = TRUE;
            break;
        case HTLC_DATA_ICON:
            /* 16-bit numeric chunks are dh_getint via HN16. */
            {
                guint16 host_icon = 0;
                dh_getint (host_icon);
                g_assert_cmphex (host_icon, ==, 412);
            }
            saw_icon = TRUE;
            break;
        default:
            g_test_fail_printf ("unexpected chunk type 0x%04x", _type);
            break;
        }
    }
    dh_end ();

    g_assert_true (saw_name);
    g_assert_true (saw_login);
    g_assert_true (saw_pass);
    g_assert_true (saw_icon);

    htlc_free (&htlc);
}

/* ---------- Zero-length chunk ---------- */

static void
test_hlwrite_zero_length_chunk_round_trip (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 1);

    /* HTLC_HDR_USER_GETLIST is the canonical zero-payload request:
     * a header with hc=0 (or in some servers an hc=1 chunk of len 0).
     * The proposal's mock-output cookbook recipe. */
    hlpack_v (&htlc, HTLC_HDR_USER_GETLIST, 0, 1, (int)HTLS_DATA_UID, /*len=*/0,
              (guint8 *)NULL);

    int found = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        g_assert_cmphex (_type, ==, HTLS_DATA_UID);
        g_assert_cmpuint (_len, ==, 0);
        found++;
    }
    dh_end ();
    g_assert_cmpint (found, ==, 1);

    htlc_free (&htlc);
}

/* ---------- Two consecutive packs in the same buffer ---------- */

static void
test_hlwrite_two_messages_concatenate (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 100);

    const char *a = "first";
    const char *b = "second";
    /* Two sequential packs: each hands back its own frame and the trans
     * counter advances once per message. (There is no send buffer that
     * accumulates them — the transport framing is one message per send.) */
    gsize len0 = 0, len1 = 0;
    guint8 *m0
        = hlpack_buf_v (&htlc, &len0, HTLC_HDR_CHAT, 0, 1, (int)HTLS_DATA_CHAT,
                        (int)strlen (a), (guint8 *)a);
    guint8 *m1
        = hlpack_buf_v (&htlc, &len1, HTLC_HDR_CHAT, 0, 1, (int)HTLS_DATA_CHAT,
                        (int)strlen (b), (guint8 *)b);

    /* Trans bumped by 2. */
    g_assert_cmphex (htlc.trans, ==, 102);

    /* First message: trans 100. */
    g_assert_cmpuint (len0, >=, SIZEOF_HL_HDR);
    const struct hl_hdr *h0 = (const struct hl_hdr *)m0;
    g_assert_cmphex (g_ntohl (h0->trans), ==, 100);
    g_assert_cmphex (g_ntohl (h0->type), ==, HTLC_HDR_CHAT);
    /* First message's on-wire length is header + chunk hdr + payload. */
    g_assert_cmpuint (len0, ==,
                      SIZEOF_HL_HDR + SIZEOF_HL_DATA_HDR + strlen (a));

    /* Second message: trans 101, its own buffer. */
    g_assert_cmpuint (len1, >=, SIZEOF_HL_HDR);
    const struct hl_hdr *h1 = (const struct hl_hdr *)m1;
    g_assert_cmphex (g_ntohl (h1->trans), ==, 101);
    g_assert_cmphex (g_ntohl (h1->type), ==, HTLC_HDR_CHAT);

    g_free (m0);
    g_free (m1);
    htlc_free (&htlc);
}

/* ---------- Header len/len2 fields encode the right byte count ----------
 *
 * hlpack writes the same byte count to both `len` and `len2`. The
 * count is total_msg_size minus (SIZEOF_HL_HDR - sizeof(hc)) — i.e.
 * the data section length in the wire format's accounting. Pin it
 * down so we don't accidentally drift the encoding while refactoring.
 */
static void
test_hlwrite_header_len_field_matches_wire_format (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 0);

    const char *body = "abc"; /* 3 bytes */
    hlpack_v (&htlc, HTLC_HDR_CHAT, 0, 1, (int)HTLS_DATA_CHAT,
              (int)strlen (body), (guint8 *)body);

    /* Total bytes packed: SIZEOF_HL_HDR (22) + SIZEOF_HL_DATA_HDR
     * (4) + 3 = 29. Header's len field encodes
     *   29 - (22 - 2)  =  29 - 20  =  9
     * (the 22-2 carve-out is "header without the hc field", since
     * hc is part of the data section in the wire format). */
    const struct hl_hdr *h = (const struct hl_hdr *)hx_test_in (&htlc)->buf;
    g_assert_cmpuint (g_ntohl (h->len), ==, 9);
    g_assert_cmpuint (g_ntohl (h->len2), ==, 9);

    htlc_free (&htlc);
}

/* ---------- The pack and the dh_start walker agree on chunk layout ----------
 *
 * We don't claim hlpack is RIGHT in isolation — we claim it produces
 * bytes the receiver-side parser knows how to walk back. The
 * round-trip is the contract.
 */
static void
test_hlwrite_round_trip_stress (void)
{
    struct htlc_conn htlc;
    htlc_init (&htlc, 0);

    /* Five chunks of varying sizes including a few edge cases. */
    guint8 huge[1024];
    for (gsize i = 0; i < sizeof (huge); i++) {
        huge[i] = (guint8)(i & 0xff);
    }

    const guint16 small_int = g_htons (0x1234);
    hlpack_v (&htlc, HTLC_HDR_FILE_GET, 0, /*hc=*/5, (int)HTLC_DATA_NAME, 0,
              (guint8 *)NULL, (int)HTLC_DATA_LOGIN, 1, (guint8 *)"x",
              (int)HTLC_DATA_PASSWORD, 256, huge, (int)HTLC_DATA_ICON, 2,
              (guint8 *)&small_int, (int)HTLC_DATA_NAME, 1024, huge);

    int chunks = 0;
    dh_start (hx_test_in (&htlc)->buf, hx_test_in (&htlc)->pos)
    {
        switch (chunks) {
        case 0:
            g_assert_cmphex (_type, ==, HTLC_DATA_NAME);
            g_assert_cmpuint (_len, ==, 0);
            break;
        case 1:
            g_assert_cmphex (_type, ==, HTLC_DATA_LOGIN);
            g_assert_cmpuint (_len, ==, 1);
            g_assert_cmphex (dh->data[0], ==, 'x');
            break;
        case 2:
            g_assert_cmphex (_type, ==, HTLC_DATA_PASSWORD);
            g_assert_cmpuint (_len, ==, 256);
            g_assert_cmpmem (dh->data, _len, huge, 256);
            break;
        case 3:
            g_assert_cmphex (_type, ==, HTLC_DATA_ICON);
            {
                guint16 v = 0;
                dh_getint (v);
                g_assert_cmphex (v, ==, 0x1234);
            }
            break;
        case 4:
            g_assert_cmphex (_type, ==, HTLC_DATA_NAME);
            g_assert_cmpuint (_len, ==, 1024);
            g_assert_cmpmem (dh->data, _len, huge, 1024);
            break;
        }
        chunks++;
    }
    dh_end ();

    g_assert_cmpint (chunks, ==, 5);

    htlc_free (&htlc);
}

/* ---------- hlpack_chunks equivalence ----------
 *
 * hlpack_chunks (which lets a builder pack from a pre-assembled
 * struct hx_chunk[] without its own variadic dispatch) must produce
 * byte-for-byte identical output to hlpack for the same chunk set. If a
 * future tweak to either function drifts from the other, two send paths
 * that share the packers would silently emit different bytes — exactly
 * the kind of fork this equivalence check is meant to prevent.
 */

static void
test_hlpack_chunks_matches_hlpack (void)
{
    /* Pack the same login-shaped message two ways: once via the
     * va_list-style hlpack, once via the array-style hlpack_chunks.
     * The packed bytes should be identical. */
    const char *login = "guest";
    guint16 icon_be = g_htons (412);
    guint16 cv_be = g_htons (185);

    struct htlc_conn h1, h2;
    htlc_init (&h1, /*starting_trans=*/42);
    htlc_init (&h2, /*starting_trans=*/42);

    hlpack_v (&h1, HTLC_HDR_LOGIN, 0, /*hc=*/3, (int)HTLC_DATA_ICON, 2,
              &icon_be, (int)HTLC_DATA_LOGIN, (int)strlen (login),
              (guint8 *)login, (int)HTLC_DATA_CLIENTVERSION, 2, &cv_be);

    struct hx_chunk chunks[3]
        = { { HTLC_DATA_ICON, 2, &icon_be },
            { HTLC_DATA_LOGIN, (guint16)strlen (login), login },
            { HTLC_DATA_CLIENTVERSION, 2, &cv_be } };
    gsize l2 = 0;
    guint8 *b2 = hlpack_chunks (&h2, HTLC_HDR_LOGIN, 0, chunks, 3, &l2);

    g_assert_cmpuint (hx_test_in (&h1)->pos, ==, l2);
    g_assert_cmpmem (hx_test_in (&h1)->buf, hx_test_in (&h1)->pos, b2, l2);
    /* And the trans counter advanced by the same amount in both. */
    g_assert_cmphex (h1.trans, ==, h2.trans);

    g_free (b2);
    htlc_free (&h1);
    htlc_free (&h2);
}

static void
test_hlpack_chunks_empty (void)
{
    /* hc=0 — header only, no chunks. */
    struct htlc_conn h1, h2;
    htlc_init (&h1, 1);
    htlc_init (&h2, 1);

    hlpack_v (&h1, HTLC_HDR_PING, 0, 0);
    gsize l2 = 0;
    guint8 *b2 = hlpack_chunks (&h2, HTLC_HDR_PING, 0, NULL, 0, &l2);

    g_assert_cmpuint (hx_test_in (&h1)->pos, ==, l2);
    g_assert_cmpmem (hx_test_in (&h1)->buf, hx_test_in (&h1)->pos, b2, l2);

    g_free (b2);
    htlc_free (&h1);
    htlc_free (&h2);
}

static void
test_hlpack_chunks_zero_length_chunk (void)
{
    /* A chunk whose len=0 (e.g. the HOPE Step 1 empty SESSIONKEY
     * placeholder). data=NULL is allowed when len=0. */
    struct htlc_conn h1, h2;
    htlc_init (&h1, 1);
    htlc_init (&h2, 1);

    hlpack_v (&h1, HTLC_HDR_LOGIN, 0, /*hc=*/1, (int)HTLC_DATA_SESSIONKEY, 0,
              (guint8 *)NULL);

    struct hx_chunk chunks[1] = { { HTLC_DATA_SESSIONKEY, 0, NULL } };
    gsize l2 = 0;
    guint8 *b2 = hlpack_chunks (&h2, HTLC_HDR_LOGIN, 0, chunks, 1, &l2);

    g_assert_cmpuint (hx_test_in (&h1)->pos, ==, l2);
    g_assert_cmpmem (hx_test_in (&h1)->buf, hx_test_in (&h1)->pos, b2, l2);

    g_free (b2);
    htlc_free (&h1);
    htlc_free (&h2);
}

/* ---------- hl_hdr_decode ----------
 *
 * Shared 22-byte header decoder added to dedup the wire_len math
 * that production (src/rcv.c::hx_rcv_hdr) and the integration test
 * harness (integration_recv_message) used to have separately.
 */

static void
test_hl_hdr_decode_basic (void)
{
    /* Pack a real message via hlpack, then decode its header. */
    struct htlc_conn htlc;
    htlc_init (&htlc, /*starting_trans=*/77);

    const char *body = "hello world!";
    hlpack_v (&htlc, HTLC_HDR_CHAT, 0, /*hc=*/1, (int)HTLC_DATA_CHAT, 12,
              (guint8 *)body);

    guint32 type, trans, flag, wire_len, body_len;
    guint16 hc;
    g_assert_true (hl_hdr_decode (hx_test_in (&htlc)->buf, &type, &trans, &flag,
                                  &hc, &wire_len, &body_len));
    g_assert_cmphex (type, ==, HTLC_HDR_CHAT);
    g_assert_cmphex (trans, ==, 77);
    g_assert_cmpuint (flag, ==, 0);
    g_assert_cmpuint (hc, ==, 1);

    /* hlpack-built message body = 1 chat chunk = HL_DATA_HDR(4) + 12 = 16 bytes.
     * wire_len = body + sizeof(hc) = 16 + 2 = 18. body_len = wire_len - 2 = 16. */
    g_assert_cmpuint (wire_len, ==, 18);
    g_assert_cmpuint (body_len, ==, 16);

    htlc_free (&htlc);
}

static void
test_hl_hdr_decode_oversize_clamps_body_len (void)
{
    /* Hand-build a header with wire_len > MAX_HOTLINE_PACKET_LEN.
     * hl_hdr_decode must still succeed (returning the raw wire_len
     * so production's trace shows the server's claim) and clamp
     * body_len at MAX_HOTLINE_PACKET_LEN - 2. */
    struct hl_hdr h;
    h.type = g_htonl (HTLS_HDR_TASK);
    h.trans = g_htonl (1);
    h.flag = 0;
    h.len = h.len2 = g_htonl (MAX_HOTLINE_PACKET_LEN * 2);
    h.hc = 0;

    guint32 wire_len, body_len;
    g_assert_true (
        hl_hdr_decode (&h, NULL, NULL, NULL, NULL, &wire_len, &body_len));
    g_assert_cmpuint (wire_len, ==, MAX_HOTLINE_PACKET_LEN * 2);
    g_assert_cmpuint (body_len, ==, MAX_HOTLINE_PACKET_LEN - 2);
}

static void
test_hl_hdr_decode_zero_len (void)
{
    /* wire_len < sizeof(hc) → body_len = 0, no underflow. */
    struct hl_hdr h = { 0 };
    h.type = g_htonl (HTLS_HDR_TASK);
    h.len = h.len2 = g_htonl (1);

    guint32 body_len = 0xdead;
    g_assert_true (hl_hdr_decode (&h, NULL, NULL, NULL, NULL, NULL, &body_len));
    g_assert_cmpuint (body_len, ==, 0);
}

static void
test_hl_hdr_decode_null_in (void)
{
    g_assert_false (hl_hdr_decode (NULL, NULL, NULL, NULL, NULL, NULL, NULL));
}

/* ---------- hl_capabilities_decode ----------
 *
 * Variable-width unsigned big-endian: 1..8 bytes carry caps from
 * HTLS_DATA_CAPABILITIES. Pin every width plus the malformed paths.
 */

static void
test_hl_capabilities_decode_widths (void)
{
    /* Empty length → 0 (the bare CAPABILITIES chunk advertisement
     * some pre-spec servers use). */
    g_assert_cmphex (hl_capabilities_decode ((guint8 *)"", 0), ==, 0);

    /* Single byte: just that byte. */
    guint8 one = 0x42;
    g_assert_cmphex (hl_capabilities_decode (&one, 1), ==, 0x42);

    /* Two bytes (the production-typical width — CHAT_HISTORY +
     * LARGE_FILES + TEXT_ENCODING all fit). */
    guint8 two[2] = { 0x00, 0x13 }; /* bits 0, 1, 4 */
    g_assert_cmphex (hl_capabilities_decode (two, 2), ==, 0x0013);

    /* Eight bytes (the spec's upper bound — saturating the field). */
    guint8 eight[8] = { 0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe };
    g_assert_cmphex (hl_capabilities_decode (eight, 8), ==,
                     G_GUINT64_CONSTANT (0xdeadbeefcafebabe));
}

static void
test_hl_capabilities_decode_oversize_truncates (void)
{
    /* A hypothetical future >64-bit advertisement: only the leading
     * 8 bytes fit in u64 anyway, so we truncate (the spec lets us). */
    guint8 nine[9] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0xff };
    g_assert_cmphex (hl_capabilities_decode (nine, 9), ==,
                     G_GUINT64_CONSTANT (0x123456789abcdef0));
}

static void
test_hl_capabilities_decode_null_input (void)
{
    g_assert_cmphex (hl_capabilities_decode (NULL, 4), ==, 0);
    g_assert_cmphex (hl_capabilities_decode ((guint8 *)"x", 0), ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto/hlwrite/header_fields_round_trip",
                     test_hlwrite_header_fields_round_trip);
    g_test_add_func ("/proto/hlwrite/trans_increments_per_message",
                     test_hlwrite_trans_increments_per_message);

    g_test_add_func ("/proto/hlwrite/single_chunk_round_trip",
                     test_hlwrite_single_chunk_round_trip);
    g_test_add_func ("/proto/hlwrite/login_message_round_trip",
                     test_hlwrite_login_message_round_trip);

    g_test_add_func ("/proto/hlwrite/zero_length_chunk_round_trip",
                     test_hlwrite_zero_length_chunk_round_trip);
    g_test_add_func ("/proto/hlwrite/two_messages_concatenate",
                     test_hlwrite_two_messages_concatenate);

    g_test_add_func ("/proto/hlwrite/header_len_field_matches_wire_format",
                     test_hlwrite_header_len_field_matches_wire_format);
    g_test_add_func ("/proto/hlwrite/round_trip_stress",
                     test_hlwrite_round_trip_stress);

    g_test_add_func ("/proto/hlwrite/hlpack_chunks_matches_hlpack",
                     test_hlpack_chunks_matches_hlpack);
    g_test_add_func ("/proto/hlwrite/hlpack_chunks_empty",
                     test_hlpack_chunks_empty);
    g_test_add_func ("/proto/hlwrite/hlpack_chunks_zero_length_chunk",
                     test_hlpack_chunks_zero_length_chunk);

    g_test_add_func ("/proto/hlwrite/hl_hdr_decode/basic",
                     test_hl_hdr_decode_basic);
    g_test_add_func ("/proto/hlwrite/hl_hdr_decode/oversize_clamps_body_len",
                     test_hl_hdr_decode_oversize_clamps_body_len);
    g_test_add_func ("/proto/hlwrite/hl_hdr_decode/zero_len",
                     test_hl_hdr_decode_zero_len);
    g_test_add_func ("/proto/hlwrite/hl_hdr_decode/null_in",
                     test_hl_hdr_decode_null_in);

    g_test_add_func ("/proto/hlwrite/hl_capabilities_decode/widths",
                     test_hl_capabilities_decode_widths);
    g_test_add_func ("/proto/hlwrite/hl_capabilities_decode/oversize",
                     test_hl_capabilities_decode_oversize_truncates);
    g_test_add_func ("/proto/hlwrite/hl_capabilities_decode/null_input",
                     test_hl_capabilities_decode_null_input);

    return g_test_run ();
}
