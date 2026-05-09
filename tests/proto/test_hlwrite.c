/*
 * tests/proto/test_hlwrite.c — hlwrite() round-trip:
 *
 *   1. Build a Hotline message via hlpack (the pure-packing half of
 *      hlwrite — no fd / cipher / compress / proto_trace).
 *   2. Inspect the bytes hlpack laid down in htlc->out.buf.
 *   3. Move them over to htlc->in.buf and parse them back via the
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
#include <netinet/in.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"

/* Variadic test wrapper: builds a va_list and forwards to hlpack.
 * Mirrors the public hlwrite() API minus the side effects. */
static void
hlpack_v (struct htlc_conn *htlc, guint32 type, guint32 flag,
          int hc, ...)
{
	va_list ap;
	va_start (ap, hc);
	hlpack (htlc, type, flag, hc, ap);
	va_end (ap);
}

/* Fresh htlc with empty out and trans counter. */
static void
htlc_init (struct htlc_conn *htlc, guint32 starting_trans)
{
	memset (htlc, 0, sizeof (*htlc));
	htlc->trans = starting_trans;
}

/* After hlpack runs, copy htlc->out into htlc->in so the dh_start
 * walker can see the just-packed message. Frees the caller of any
 * "where does the message live" bookkeeping. */
static void
flip_out_to_in (struct htlc_conn *htlc)
{
	g_free (htlc->in.buf);
	htlc->in.buf = htlc->out.buf;
	htlc->in.pos = htlc->out.len;
	htlc->in.len = htlc->out.len;

	htlc->out.buf = NULL;
	htlc->out.pos = 0;
	htlc->out.len = 0;
}

static void
htlc_free (struct htlc_conn *htlc)
{
	g_free (htlc->in.buf);
	g_free (htlc->out.buf);
	htlc->in.buf = NULL;
	htlc->out.buf = NULL;
}

/* Read back the header that hlpack wrote at out.buf[0]. Returns the
 * decoded fields in host order. */
static void
read_packed_hdr (const struct htlc_conn *htlc,
                 guint32 *type, guint32 *trans, guint32 *flag,
                 guint16 *hc)
{
	g_assert_cmpuint (htlc->out.len, >=, SIZEOF_HL_HDR);
	const struct hl_hdr *h = (const struct hl_hdr *) htlc->out.buf;
	if (type)  *type  = ntohl (h->type);
	if (trans) *trans = ntohl (h->trans);
	if (flag)  *flag  = ntohl (h->flag);
	if (hc)    *hc    = ntohs (h->hc);
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
	g_assert_cmphex  (type,  ==, HTLC_HDR_LOGIN);
	g_assert_cmphex  (trans, ==, 123);
	g_assert_cmpuint (flag,  ==, 0);
	g_assert_cmpuint (hc,    ==, 0);

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
		(void) snapshot;
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
	hlpack_v (&htlc, HTLC_HDR_LOGIN, 0, /*hc=*/1,
	          (int) HTLC_DATA_LOGIN, (int) strlen (login),
	          (guint8 *) login);

	guint16 hc;
	read_packed_hdr (&htlc, NULL, NULL, NULL, &hc);
	g_assert_cmpuint (hc, ==, 1);

	flip_out_to_in (&htlc);

	int found_chunks = 0;
	dh_start (&htlc) {
		found_chunks++;
		g_assert_cmphex  (_type, ==, HTLC_DATA_LOGIN);
		g_assert_cmpuint (_len,  ==, strlen (login));
		g_assert_cmpmem  (dh->data, _len, login, strlen (login));
	} dh_end ();
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
	const guint16 icon = htons (412);

	hlpack_v (&htlc, HTLC_HDR_LOGIN, 0, /*hc=*/4,
	          (int) HTLC_DATA_NAME,     (int) strlen (name), (guint8 *) name,
	          (int) HTLC_DATA_LOGIN,    (int) strlen (user), (guint8 *) user,
	          (int) HTLC_DATA_PASSWORD, (int) strlen (pass), (guint8 *) pass,
	          (int) HTLC_DATA_ICON,     (int) sizeof (icon), (guint8 *) &icon);

	guint32 type, trans;
	guint16 hc;
	read_packed_hdr (&htlc, &type, &trans, NULL, &hc);
	g_assert_cmphex  (type, ==, HTLC_HDR_LOGIN);
	g_assert_cmphex  (trans, ==, 42);
	g_assert_cmpuint (hc, ==, 4);

	flip_out_to_in (&htlc);

	gboolean saw_name = FALSE, saw_login = FALSE,
	         saw_pass = FALSE, saw_icon = FALSE;
	dh_start (&htlc) {
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
			g_test_fail_printf (
				"unexpected chunk type 0x%04x", _type);
			break;
		}
	} dh_end ();

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
	hlpack_v (&htlc, HTLC_HDR_USER_GETLIST, 0, 1,
	          (int) HTLS_DATA_UID, /*len=*/0, (guint8 *) NULL);

	flip_out_to_in (&htlc);

	int found = 0;
	dh_start (&htlc) {
		g_assert_cmphex (_type, ==, HTLS_DATA_UID);
		g_assert_cmpuint (_len, ==, 0);
		found++;
	} dh_end ();
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
	hlpack_v (&htlc, HTLC_HDR_CHAT, 0, 1,
	          (int) HTLS_DATA_CHAT, (int) strlen (a), (guint8 *) a);
	hlpack_v (&htlc, HTLC_HDR_CHAT, 0, 1,
	          (int) HTLS_DATA_CHAT, (int) strlen (b), (guint8 *) b);

	/* Trans bumped by 2. */
	g_assert_cmphex (htlc.trans, ==, 102);

	/* The two messages are laid down back-to-back. The first
	 * header sits at out.buf[0]. */
	const struct hl_hdr *h0 = (const struct hl_hdr *) htlc.out.buf;
	g_assert_cmphex (ntohl (h0->trans), ==, 100);
	g_assert_cmphex (ntohl (h0->type),  ==, HTLC_HDR_CHAT);

	/* Second header sits right after the first message's
	 * (header + chunk + payload). */
	gsize first_len = SIZEOF_HL_HDR + SIZEOF_HL_DATA_HDR + strlen (a);
	g_assert_cmpuint (htlc.out.len, >=, first_len + SIZEOF_HL_HDR);
	const struct hl_hdr *h1 =
		(const struct hl_hdr *) (htlc.out.buf + first_len);
	g_assert_cmphex (ntohl (h1->trans), ==, 101);
	g_assert_cmphex (ntohl (h1->type),  ==, HTLC_HDR_CHAT);

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

	const char *body = "abc";  /* 3 bytes */
	hlpack_v (&htlc, HTLC_HDR_CHAT, 0, 1,
	          (int) HTLS_DATA_CHAT, (int) strlen (body),
	          (guint8 *) body);

	/* Total bytes packed: SIZEOF_HL_HDR (22) + SIZEOF_HL_DATA_HDR
	 * (4) + 3 = 29. Header's len field encodes
	 *   29 - (22 - 2)  =  29 - 20  =  9
	 * (the 22-2 carve-out is "header without the hc field", since
	 * hc is part of the data section in the wire format). */
	const struct hl_hdr *h = (const struct hl_hdr *) htlc.out.buf;
	g_assert_cmpuint (ntohl (h->len),  ==, 9);
	g_assert_cmpuint (ntohl (h->len2), ==, 9);

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
	for (gsize i = 0; i < sizeof (huge); i++)
		huge[i] = (guint8) (i & 0xff);

	const guint16 small_int = htons (0x1234);
	hlpack_v (&htlc, HTLC_HDR_FILE_GET, 0, /*hc=*/5,
	          (int) HTLC_DATA_NAME,     0,             (guint8 *) NULL,
	          (int) HTLC_DATA_LOGIN,    1,             (guint8 *) "x",
	          (int) HTLC_DATA_PASSWORD, 256,           huge,
	          (int) HTLC_DATA_ICON,     2,             (guint8 *) &small_int,
	          (int) HTLC_DATA_NAME,     1024,          huge);

	flip_out_to_in (&htlc);

	int chunks = 0;
	dh_start (&htlc) {
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
	} dh_end ();

	g_assert_cmpint (chunks, ==, 5);

	htlc_free (&htlc);
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

	return g_test_run ();
}
