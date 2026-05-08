#ifndef HX_PROTO_HELPERS_H
#define HX_PROTO_HELPERS_H 1

/*
 * Pure-protocol-parsing helpers extracted from rcv.c / tasks.c so the
 * Tier 2 unit tests can drive them with canned wire bytes, without
 * dragging in GTK, libadwaita, the toolbar / chat / news / xtext
 * widgets, or the worker-thread plumbing that those source files
 * also bring in.
 *
 * Each helper takes a struct htlc_conn whose `in.buf` / `in.pos` are
 * the only fields it touches — the test sets those up via the
 * wire_fixture builder under tests/proto/.
 *
 * The original handlers (task_error, hx_rcv_chat, etc.) remain in
 * their old translation units; they now call into these helpers and
 * keep doing the GUI side-effects (toast, sound, hx_output dispatch)
 * themselves. The shape of the change is the same as the Tier 1
 * extractions: separate the protocol decision from the side effect.
 */

#include <glib.h>

struct htlc_conn;

/*
 * Find the HTLS_DATA_TASKERROR chunk in htlc->in and copy its body
 * into `out`, sanitising CR→LF and stripping ANSI control bytes
 * along the way. NUL-terminates `out` on success.
 *
 * Returns TRUE iff a task-error chunk was found and out was filled.
 * On TRUE, *out_len (if non-NULL) is the message byte length without
 * the trailing NUL. On FALSE the caller MUST NOT read out.
 *
 * Truncates to (out_size - 1) bytes if the message is too big.
 * out_size must be at least 1.
 */
extern gboolean task_error_extract (struct htlc_conn *htlc,
                                    char *out, gsize out_size,
                                    gsize *out_len);

#endif /* HX_PROTO_HELPERS_H */
