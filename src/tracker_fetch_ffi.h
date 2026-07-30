/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tracker_fetch_ffi.h — C mirror of the hxnet tracker-fetch FFI
 * (rust/crates/hxnet/src/ffi.rs). Hand-synced, like the HXNET_STATE_* /
 * HxnetFrame mirrors; the _Static_asserts below pin the HxnetTrackerEvent
 * struct ABI against the Rust offset_of asserts so drift on either side
 * is a compile error.
 *
 * network.c's tracker bridge drives this; tests/integration/
 * test_tracker_fetch.c drives it directly against a live tracker.
 */

#ifndef GTKHX_TRACKER_FETCH_FFI_H
#define GTKHX_TRACKER_FETCH_FFI_H

#include <stddef.h> /* offsetof */
#include <glib.h>

G_BEGIN_DECLS

/* Opaque hxnet tracker-fetch handle (Rust `HxnetTrackerFetch`). */
typedef struct HxnetTrackerFetch HxnetTrackerFetch;

/* HxnetTrackerEvent.kind discriminants. */
#define HXNET_TRK_KIND_BEGIN 0u
#define HXNET_TRK_KIND_RECORD 1u
#define HXNET_TRK_KIND_ERROR 2u
#define HXNET_TRK_KIND_DONE 3u

/* hxnet_tracker_fetch_poll return codes. */
#define HXNET_TRK_POLL_EMPTY 0
#define HXNET_TRK_POLL_EVENT 1
#define HXNET_TRK_POLL_CLOSED (-1)

/* Host-aware TOFU verify: (tracker host, port, leaf "sha256:<hex>" fp)
 * -> non-zero to accept. Host AND port are passed because one walk spans
 * many endpoints through this single callback, and trust is keyed on
 * (host, port) so different ports on one host pin independently. */
typedef int (*hxnet_tracker_verify_cb_t) (const guint8 *host, gsize host_len,
                                          guint16 port, const guint8 *fp,
                                          gsize fp_len, void *user_data);

/* POD view of one fetch event. Pointer fields borrow the handle's
 * current event and are valid only until the next poll/close — copy
 * immediately. Layout mirrors the repr(C) HxnetTrackerEvent in ffi.rs;
 * empty buffers come back as a NULL pointer with len 0. */
typedef struct {
    guint32 kind;
    guint8 version;
    guint8 addr_type;
    guint16 count;
    guint16 total;
    guint16 port;
    guint16 nusers;
    guint16 tlv_count;
    const guint8 *url_ptr;
    gsize url_len;
    const guint8 *address_ptr;
    gsize address_len;
    const guint8 *name_ptr;
    gsize name_len;
    const guint8 *desc_ptr;
    gsize desc_len;
    const guint8 *tlv_ptr;
    gsize tlv_len;
    const guint8 *message_ptr;
    gsize message_len;
} HxnetTrackerEvent;

_Static_assert (offsetof (HxnetTrackerEvent, kind) == 0, "kind offset");
_Static_assert (offsetof (HxnetTrackerEvent, version) == 4, "version offset");
_Static_assert (offsetof (HxnetTrackerEvent, addr_type) == 5,
                "addr_type offset");
_Static_assert (offsetof (HxnetTrackerEvent, count) == 6, "count offset");
_Static_assert (offsetof (HxnetTrackerEvent, total) == 8, "total offset");
_Static_assert (offsetof (HxnetTrackerEvent, port) == 10, "port offset");
_Static_assert (offsetof (HxnetTrackerEvent, nusers) == 12, "nusers offset");
_Static_assert (offsetof (HxnetTrackerEvent, tlv_count) == 14,
                "tlv_count offset");
/* gsize is pointer-sized, so the 6 (ptr, len) pairs pack at
 * 16 + k*sizeof(void*). Pin every one plus the total size so a reorder
 * or type change of any field is a compile error, not a runtime corrupt. */
_Static_assert (offsetof (HxnetTrackerEvent, url_ptr) == 16, "url_ptr offset");
_Static_assert (offsetof (HxnetTrackerEvent, url_len) == 16 + sizeof (void *),
                "url_len offset");
_Static_assert (offsetof (HxnetTrackerEvent, address_ptr)
                    == 16 + 2 * sizeof (void *),
                "address_ptr offset");
_Static_assert (offsetof (HxnetTrackerEvent, address_len)
                    == 16 + 3 * sizeof (void *),
                "address_len offset");
_Static_assert (offsetof (HxnetTrackerEvent, name_ptr)
                    == 16 + 4 * sizeof (void *),
                "name_ptr offset");
_Static_assert (offsetof (HxnetTrackerEvent, name_len)
                    == 16 + 5 * sizeof (void *),
                "name_len offset");
_Static_assert (offsetof (HxnetTrackerEvent, desc_ptr)
                    == 16 + 6 * sizeof (void *),
                "desc_ptr offset");
_Static_assert (offsetof (HxnetTrackerEvent, desc_len)
                    == 16 + 7 * sizeof (void *),
                "desc_len offset");
_Static_assert (offsetof (HxnetTrackerEvent, tlv_ptr)
                    == 16 + 8 * sizeof (void *),
                "tlv_ptr offset");
_Static_assert (offsetof (HxnetTrackerEvent, tlv_len)
                    == 16 + 9 * sizeof (void *),
                "tlv_len offset");
_Static_assert (offsetof (HxnetTrackerEvent, message_ptr)
                    == 16 + 10 * sizeof (void *),
                "message_ptr offset");
_Static_assert (offsetof (HxnetTrackerEvent, message_len)
                    == 16 + 11 * sizeof (void *),
                "message_len offset");
_Static_assert (sizeof (HxnetTrackerEvent) == 16 + 12 * sizeof (void *),
                "HxnetTrackerEvent size");

/* Open a tracker fetch walk over `n` NUL-terminated "host" / "host:port"
 * URL strings (default port 5498). `features` is the v3 handshake
 * feature bitmask, `probe_ms` the v3-probe watchdog. `proxy_uri` is an
 * optional NUL-terminated "socks5://..." URI to tunnel the whole walk
 * through (NULL = direct; a malformed/unsupported URI fails the open).
 * `verify_cert` (+ user_data) is the host-keyed TOFU check for non-WebPKI
 * TLS certs; NULL accepts any non-WebPKI cert. Returns an owned handle
 * (free with hxnet_tracker_fetch_close) or NULL on bad arguments. */
extern HxnetTrackerFetch *
hxnet_tracker_fetch_open (const char *const *urls, gsize n, guint16 features,
                          guint32 probe_ms, const char *proxy_uri,
                          hxnet_tracker_verify_cb_t verify_cert,
                          void *user_data);

/* Drain one event into `out`. Returns HXNET_TRK_POLL_EVENT (a new event
 * was written; its borrowed pointers are valid until the next poll /
 * close), HXNET_TRK_POLL_EMPTY (nothing ready — try later), or
 * HXNET_TRK_POLL_CLOSED (the walk finished and is drained). */
extern int hxnet_tracker_fetch_poll (HxnetTrackerFetch *handle,
                                     HxnetTrackerEvent *out);

/* Cancel (if running) and free a handle. NULL-safe. */
extern void hxnet_tracker_fetch_close (HxnetTrackerFetch *handle);

G_END_DECLS

#endif /* GTKHX_TRACKER_FETCH_FFI_H */
