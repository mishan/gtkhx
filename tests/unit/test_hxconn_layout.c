/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * test_hxconn_layout.c — does C's mirror of `struct htlc_conn` agree with the
 * Rust struct that actually owns the storage?
 *
 * The connection is heap-allocated and opaque in production: C reaches it only
 * through the `hxconn.h` accessors, and `hxconn_layout.h`'s mirror exists so
 * *tests* can stack-allocate one. That makes the mirror a duplicate of a
 * definition it cannot see, which is exactly the kind of thing that drifts.
 *
 * Both sides already pin `sizeof`. That is necessary and not sufficient: a
 * field added into existing padding leaves `sizeof` unchanged while the two
 * layouts disagree about where the field lives. `serial` is precisely that
 * case — it went into the tail padding, so the struct is the same size with
 * or without it, and moving it to any other hole would keep both size
 * assertions passing while silently relocating it by hundreds of bytes.
 *
 * So this compares positions. Not every field: the ones whose misplacement is
 * both plausible and expensive — the two pointers C hands back out
 * (`sess`, `bridge_handle`), the bitmaps read byte-wise (`access`, `caps`),
 * the wire-shaped array (`name`), the fd, and `serial` itself.
 */

#include "config.h"

#include <glib.h>
#include <stddef.h>

#include "hxconn_layout.h"

/* Rust's view of its own struct (gtkhx-core, conn.rs). */
extern size_t hx_conn_sizeof (void);
extern size_t hx_conn_alignof (void);
extern size_t hx_conn_offsetof_sess (void);
extern size_t hx_conn_offsetof_fd (void);
extern size_t hx_conn_offsetof_access (void);
extern size_t hx_conn_offsetof_name (void);
extern size_t hx_conn_offsetof_hope_aead (void);
extern size_t hx_conn_offsetof_bridge_handle (void);
extern size_t hx_conn_offsetof_caps (void);
extern size_t hx_conn_offsetof_serial (void);
extern size_t hx_conn_offsetof_ping_timer (void);

static void
test_size_and_alignment_match (void)
{
    g_assert_cmpuint (hx_conn_sizeof (), ==, sizeof (struct htlc_conn));
    g_assert_cmpuint (hx_conn_alignof (), ==, G_ALIGNOF (struct htlc_conn));
}

static void
test_field_offsets_match (void)
{
    g_assert_cmpuint (hx_conn_offsetof_sess (), ==,
                      offsetof (struct htlc_conn, sess));
    g_assert_cmpuint (hx_conn_offsetof_fd (), ==,
                      offsetof (struct htlc_conn, fd));
    g_assert_cmpuint (hx_conn_offsetof_access (), ==,
                      offsetof (struct htlc_conn, access));
    g_assert_cmpuint (hx_conn_offsetof_name (), ==,
                      offsetof (struct htlc_conn, name));
    g_assert_cmpuint (hx_conn_offsetof_hope_aead (), ==,
                      offsetof (struct htlc_conn, hope_aead));
    g_assert_cmpuint (hx_conn_offsetof_bridge_handle (), ==,
                      offsetof (struct htlc_conn, bridge_handle));
    g_assert_cmpuint (hx_conn_offsetof_caps (), ==,
                      offsetof (struct htlc_conn, caps));
    g_assert_cmpuint (hx_conn_offsetof_serial (), ==,
                      offsetof (struct htlc_conn, serial));
    g_assert_cmpuint (hx_conn_offsetof_ping_timer (), ==,
                      offsetof (struct htlc_conn, ping_timer));
}

/* The field this test was written for. It went into what was then tail
 * padding, so adding it did not change `sizeof` — which is precisely the case
 * a size assertion cannot catch, and the reason the offsets above exist. The
 * fields after it have since claimed that space, so the property this asserts
 * is now just "it is still where the mirror thinks it is, at the tail". */
static void
test_serial_is_in_the_tail_padding (void)
{
    g_assert_cmpuint (offsetof (struct htlc_conn, serial), >,
                      offsetof (struct htlc_conn, gif_icons_probe_trans));
    g_assert_cmpuint (offsetof (struct htlc_conn, serial)
                          + sizeof (((struct htlc_conn *)0)->serial),
                      <=, sizeof (struct htlc_conn));
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/hxconn_layout/size_and_alignment",
                     test_size_and_alignment_match);
    g_test_add_func ("/hxconn_layout/field_offsets", test_field_offsets_match);
    g_test_add_func ("/hxconn_layout/serial_in_tail_padding",
                     test_serial_is_in_the_tail_padding);
    return g_test_run ();
}
