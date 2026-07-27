/*
 * test_htxf_layout.c — pin the struct htxf_conn ABI across the C/Rust seam (S0).
 *
 * The transfer-handle storage moved to hxnet's xfer_handle module: hx_htxf_new /
 * hx_htxf_free own the allocation, and a #[repr(C)] mirror in Rust must match
 * the C `struct htxf_conn` declaration exactly — a silent field drift on either
 * side would corrupt memory (the C consumers still access fields directly). This
 * test asserts the Rust mirror's sizeof / alignof / key field offsets against
 * the real C struct, and that hx_htxf_new hands back a zeroed handle.
 */

#include "config.h"
#include <glib.h>
#include <stddef.h>
#include "compat.h"
#include "protocol.h"
#include "htxf_accessors.h"

static void
test_layout_matches (void)
{
    /* The allocation size + alignment Rust uses must equal what the C side
	 * believes the struct is, or hx_htxf_new under/over-allocates. */
    g_assert_cmpuint (hx_htxf_sizeof (), ==, sizeof (struct htxf_conn));
    g_assert_cmpuint (hx_htxf_alignof (), ==, G_ALIGNOF (struct htxf_conn));

    /* The S0.2 lifecycle fields Rust will drive as atomics must sit where C
	 * puts them. */
    g_assert_cmpuint (hx_htxf_offsetof_refcount (), ==,
                      offsetof (struct htxf_conn, refcount));
    g_assert_cmpuint (hx_htxf_offsetof_canceled (), ==,
                      offsetof (struct htxf_conn, canceled));
    g_assert_cmpuint (hx_htxf_offsetof_total_pos (), ==,
                      offsetof (struct htxf_conn, total_pos));
}

static void
test_new_is_zeroed_and_free_is_safe (void)
{
    struct htxf_conn *htxf = hx_htxf_new ();
    g_assert_nonnull (htxf);

    /* Fresh handle: the old g_malloc0 semantics. */
    g_assert_cmpint (htxf->refcount, ==, 0);
    g_assert_cmpint (htxf->canceled, ==, 0);
    g_assert_cmpuint (htxf->total_pos, ==, 0);
    g_assert_cmpuint (htxf->total_size, ==, 0);
    g_assert_null (htxf->hx);
    g_assert_null (htxf->abort);
    g_assert_cmpint (htxf->path[0], ==, 0);

    hx_htxf_free (htxf);
    hx_htxf_free (NULL); /* NULL-safe */
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/htxf/layout_matches", test_layout_matches);
    g_test_add_func ("/htxf/new_is_zeroed", test_new_is_zeroed_and_free_is_safe);
    return g_test_run ();
}
