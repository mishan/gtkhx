/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/banner_dispatch.h — pure helpers carved out of banner.c so
 * tests/proto/test_banner_dispatch.c can pin the URL-vs-file mode
 * dispatch and the HTXF reply sanity checks without dragging in the
 * GTK widget / libsoup / pthread side of banner.c.
 *
 * Both helpers were inlined in banner_handle_message / banner_handle
 * _htxf_reply previously. They each cover a decision the protocol
 * places real weight on (the dispatch picks the wire path; the
 * sanity check is the only thing standing between a hostile server
 * and an unbounded g_malloc), so it's worth pinning their contract
 * with cheap unit tests instead of waiting for the live Tier 3
 * banner tests to catch a regression downstream.
 */

#ifndef GTKHX_BANNER_DISPATCH_H
#define GTKHX_BANNER_DISPATCH_H

#include <glib.h>

/* Sanity cap on the file-mode banner size advertised by the server.
 * Largest banner we've seen in the wild is ~30 KB; legitimate
 * banners stay below 100 KB. A 1 MiB ceiling leaves plenty of
 * headroom for unusual formats while preventing a hostile server
 * from talking us into allocating gigabytes for a toolbar
 * decoration. Lives in the header so both banner.c and the test
 * suite see the same value. */
#define HX_BANNER_MAX_HTXF_SIZE (1024u * 1024u)

/* Classify the 4-byte TYPE chunk from HTLS_HDR_BANNER as URL mode
 * or not. Per the 1.9 spec, "URL " (or just "URL") on the wire
 * means the server provides a URL chunk and the client fetches the
 * image itself; any other code ("GIFf", "JPEG", "PICT", etc.) means
 * file mode — bytes come over the HTXF subchannel.
 *
 * The comparison is case-insensitive and strips any trailing space
 * — mhxd pads short codes with a space ("URL "), other servers
 * might NUL-terminate at 3 ("URL\0"); both should classify the
 * same. NULL input returns FALSE.
 *
 * Why this matters: an earlier revision of banner.c keyed the URL/
 * file dispatch on whether the HTLS_DATA_BANNER_URL chunk was
 * present, not the TYPE field. mhxd in file mode still sends the
 * URL chunk (its banner-send doesn't gate it on type), which
 * routed us into URL mode even when the server was actually
 * streaming bytes over HTXF. TYPE is authoritative — this helper
 * is the single source of truth for the URL/file question. */
extern gboolean hx_banner_type_is_url (const char *type);

/* Result of HTXF reply validation. Encodes the four states the
 * handler in banner.c cares about: one OK plus three distinct
 * reject reasons. Some reject codes share a user-visible caption
 * today (ZERO_REF and ZERO_SIZE both surface as "empty transfer"
 * since the user-actionable advice is the same), but the codes
 * stay distinct for logging / attribution and so a future revision
 * can split the captions without rewriting the validator. */
typedef enum {
    HX_BANNER_HTXF_OK         = 0, /* ref + size both > 0 and size <= cap */
    HX_BANNER_HTXF_ZERO_REF   = 1, /* server replied with ref=0 */
    HX_BANNER_HTXF_ZERO_SIZE  = 2, /* server replied with size=0 */
    HX_BANNER_HTXF_TOO_LARGE  = 3, /* size > HX_BANNER_MAX_HTXF_SIZE */
} hx_banner_htxf_validation;

/* Validate the HTXF reply's REF + SIZE pair. Returns
 * HX_BANNER_HTXF_OK iff the transfer is safe to spawn. The reject
 * reasons are distinguishable so the caller can pick the right
 * caption (and a future logging layer can attribute drops to the
 * right cause). */
extern hx_banner_htxf_validation hx_banner_validate_htxf_reply (guint32 ref,
                                                                guint32 size);

#endif /* GTKHX_BANNER_DISPATCH_H */
