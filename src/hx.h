/*
 * hx.h — umbrella header.
 *
 * The original 655-line hx.h was split (Phase 1.5) into:
 *
 *   compat.h    — portability shims, gettext _(), MAXPATHLEN, byte-shift
 *                 macros: pure preprocessor, no struct definitions.
 *   protocol.h  — wire/network/connection types, htlc_conn, qbuf, task,
 *                 hmac_xxx, random_bytes (Rust), byte-order helpers.
 *                 Pulls in <glib.h> but NOT <gtk/gtk.h>.
 *   prefs.h     — gtkhx_prefs and Window_Geo. Pure data.
 *   session.h   — GtkWidget*-bearing types, the `session` struct, the
 *                 output_functions vtable. Pulls in <gtk/gtk.h>.
 *
 * New code should include the narrowest header that satisfies its needs;
 * this umbrella exists so the 30-odd .c files that already include
 * "hx.h" keep building unchanged.
 */

#ifndef __hxd_HX_H
#define __hxd_HX_H 1

#include "compat.h"
#include "protocol.h"
#include "prefs.h"
#include "session.h"

#endif /* ndef __hxd_HX_H */
