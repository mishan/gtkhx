/*
 * tests/unit/test_algo_list.c — pin the bounds-checking of
 * algo_list.c::list_n, the HOPE algorithm-list walker. The legacy
 * implementation read list[2] before checking listlen, so any
 * input with listlen < 3 (or a NULL list) walked off the buffer.
 * The HOPE-Secure-Login spec calls this out by name:
 *
 *   "Sending an empty compression algorithm list (count=0) causes
 *    shxd-family clients (hx, gtkhx) to crash due to a NULL pointer
 *    dereference in list_n()."
 *
 * Test cases cover:
 *
 *   - happy-path: get-by-index across a well-formed multi-entry list
 *   - the crash case from the spec: empty list (count=0, listlen=2)
 *   - other malformed shapes: NULL list, listlen=0, listlen=1,
 *     listlen=2 with non-zero claimed count, name length overrunning
 *     the buffer
 *   - out-of-range n on a valid list
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "algo_list.h"

/* Build a wire-shape buffer holding a u16 count + a sequence of
 * { u8 namelen, name bytes }. Returns malloc'd bytes the caller
 * must g_free. *out_len receives the total byte count. */
static guint8 *
build_list (guint16 count, const char *const names[], gsize *out_len)
{
    /* Compute total length: 2 (count) + sum(1 + strlen(name)). */
    gsize total = 2;
    for (guint16 i = 0; i < count; i++) {
        total += 1 + strlen (names[i]);
    }

    guint8 *buf = g_malloc (total);
    buf[0] = (guint8) (count >> 8);
    buf[1] = (guint8) (count & 0xff);

    gsize pos = 2;
    for (guint16 i = 0; i < count; i++) {
        gsize nlen = strlen (names[i]);
        buf[pos++] = (guint8)nlen;
        memcpy (&buf[pos], names[i], nlen);
        pos += nlen;
    }
    *out_len = total;
    return buf;
}

/* ---- happy path ---- */

/* Three-entry list — pull each by index. Output pointer should
 * land on the {namelen, name...} record, NOT after it. */
static void
test_three_entries_index_walk (void)
{
    const char *names[] = { "HMAC-SHA1", "HMAC-MD5", "MD5" };
    gsize len;
    guint8 *buf = build_list (3, names, &len);

    guint8 *p0 = list_n (buf, (guint16)len, 0);
    g_assert_nonnull (p0);
    g_assert_cmpint (p0[0], ==, 9);
    g_assert_cmpint (memcmp (&p0[1], "HMAC-SHA1", 9), ==, 0);

    guint8 *p1 = list_n (buf, (guint16)len, 1);
    g_assert_nonnull (p1);
    g_assert_cmpint (p1[0], ==, 8);
    g_assert_cmpint (memcmp (&p1[1], "HMAC-MD5", 8), ==, 0);

    guint8 *p2 = list_n (buf, (guint16)len, 2);
    g_assert_nonnull (p2);
    g_assert_cmpint (p2[0], ==, 3);
    g_assert_cmpint (memcmp (&p2[1], "MD5", 3), ==, 0);

    g_free (buf);
}

/* Single-entry list. */
static void
test_single_entry (void)
{
    const char *names[] = { "HMAC-SHA256" };
    gsize len;
    guint8 *buf = build_list (1, names, &len);

    guint8 *p = list_n (buf, (guint16)len, 0);
    g_assert_nonnull (p);
    g_assert_cmpint (p[0], ==, 11);
    g_assert_cmpint (memcmp (&p[1], "HMAC-SHA256", 11), ==, 0);

    g_free (buf);
}

/* ---- the crash case from the HOPE spec ---- */

/* Empty list (count=0, listlen=2). The legacy implementation
 * dereferenced buf+2 before any bounds check — crashed when
 * gtkhx hit an Mobius server sending this. We must return NULL
 * cleanly. */
static void
test_empty_list_no_crash (void)
{
    guint8 buf[2] = { 0x00, 0x00 };

    /* This is the exact crash case the spec calls out by name. */
    g_assert_null (list_n (buf, 2, 0));
    g_assert_null (list_n (buf, 2, 1));
    g_assert_null (list_n (buf, 2, 99));
}

/* ---- other malformed-input shapes ---- */

/* NULL list pointer is treated as malformed. */
static void
test_null_list (void)
{
    g_assert_null (list_n (NULL, 0, 0));
    g_assert_null (list_n (NULL, 64, 0));
}

/* Length below the 2-byte count header. */
static void
test_listlen_below_header (void)
{
    guint8 buf[1] = { 0xff };
    g_assert_null (list_n (buf, 0, 0));
    g_assert_null (list_n (buf, 1, 0));
}

/* Header claims a count > 0 but the buffer is just the 2-byte
 * header — no length byte fits. */
static void
test_header_claims_count_but_no_room (void)
{
    guint8 buf[2] = { 0x00, 0x05 }; /* claims 5 entries */
    g_assert_null (list_n (buf, 2, 0));
}

/* Name length overruns the buffer. count=1, namelen claims 10, but
 * only 3 name bytes are present. */
static void
test_namelen_overruns (void)
{
    guint8 buf[6] = { 0x00, 0x01, 0x0a, 'a', 'b', 'c' };
    g_assert_null (list_n (buf, sizeof buf, 0));
}

/* Out-of-range n on a well-formed list. */
static void
test_out_of_range_n (void)
{
    const char *names[] = { "A", "B" };
    gsize len;
    guint8 *buf = build_list (2, names, &len);

    g_assert_nonnull (list_n (buf, (guint16)len, 0));
    g_assert_nonnull (list_n (buf, (guint16)len, 1));
    g_assert_null (list_n (buf, (guint16)len, 2));
    g_assert_null (list_n (buf, (guint16)len, 99));

    g_free (buf);
}

/* Names of length 0 are legal — namelen=0, no name bytes follow.
 * The walker should advance past them cleanly. */
static void
test_zero_length_names (void)
{
    /* Three entries, all with namelen=0. Buffer = [count_hi count_lo
	 * 0 0 0] = 5 bytes. */
    guint8 buf[5] = { 0x00, 0x03, 0x00, 0x00, 0x00 };

    guint8 *p0 = list_n (buf, sizeof buf, 0);
    g_assert_nonnull (p0);
    g_assert_cmpint (p0[0], ==, 0);

    guint8 *p2 = list_n (buf, sizeof buf, 2);
    g_assert_nonnull (p2);
    g_assert_cmpint (p2[0], ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/algo_list/three_entries_index_walk",
                     test_three_entries_index_walk);
    g_test_add_func ("/algo_list/single_entry", test_single_entry);
    g_test_add_func ("/algo_list/empty_list_no_crash",
                     test_empty_list_no_crash);
    g_test_add_func ("/algo_list/null_list", test_null_list);
    g_test_add_func ("/algo_list/listlen_below_header",
                     test_listlen_below_header);
    g_test_add_func ("/algo_list/header_claims_count_but_no_room",
                     test_header_claims_count_but_no_room);
    g_test_add_func ("/algo_list/namelen_overruns", test_namelen_overruns);
    g_test_add_func ("/algo_list/out_of_range_n", test_out_of_range_n);
    g_test_add_func ("/algo_list/zero_length_names", test_zero_length_names);

    return g_test_run ();
}
