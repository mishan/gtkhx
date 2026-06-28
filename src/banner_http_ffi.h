/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * banner_http_ffi.h — C mirror of the hxnet banner URL-fetch FFI
 * (rust/crates/hxnet/src/banner_http.rs). Hand-synced; the
 * _Static_asserts pin the HxnetBannerOut struct ABI against the Rust
 * side so a layout drift on either side is a compile error.
 *
 * banner.c's URL-mode fetch drives this: open a fetch for a banner URL,
 * drain the one-shot result on a main-loop timeout, hand the bytes to
 * the shared glycin decode helper (or show a caption on error).
 */

#ifndef HX_BANNER_HTTP_FFI_H
#define HX_BANNER_HTTP_FFI_H

#include <stddef.h> /* offsetof */
#include <glib.h>

G_BEGIN_DECLS

/* Opaque hxnet banner-fetch handle (Rust `HxnetBannerFetch`). */
typedef struct HxnetBannerFetch HxnetBannerFetch;

/* hxnet_banner_fetch_poll return codes. */
#define HXNET_BANNER_PENDING 0
#define HXNET_BANNER_DONE    1
#define HXNET_BANNER_ERROR   (-1)

/* Result view filled by hxnet_banner_fetch_poll. On DONE the bytes_*
 * pair points at the response body (borrowed from the handle, valid
 * until close); on ERROR the err_* pair points at a UTF-8 message.
 * Empty buffers come back as NULL / 0. Layout mirrors the repr(C)
 * HxnetBannerOut in banner_http.rs. */
typedef struct {
    const guint8 *bytes_ptr;
    gsize bytes_len;
    const guint8 *err_ptr;
    gsize err_len;
} HxnetBannerOut;

/* gsize is pointer-sized on every target we build for, so the four
 * fields pack at successive pointer-width offsets. Pin all of them plus
 * the overall size so a reorder / type change on either side fails to
 * compile rather than corrupting the decode at runtime. */
_Static_assert (offsetof (HxnetBannerOut, bytes_ptr) == 0, "bytes_ptr offset");
_Static_assert (offsetof (HxnetBannerOut, bytes_len) == sizeof (void *),
                "bytes_len offset");
_Static_assert (offsetof (HxnetBannerOut, err_ptr) == 2 * sizeof (void *),
                "err_ptr offset");
_Static_assert (offsetof (HxnetBannerOut, err_len) == 3 * sizeof (void *),
                "err_len offset");
_Static_assert (sizeof (HxnetBannerOut) == 4 * sizeof (void *),
                "HxnetBannerOut size");

/* Open a banner fetch for `url` (`url_len` UTF-8 bytes). Spawns the
 * blocking GET on the tokio blocking pool; drain with
 * hxnet_banner_fetch_poll. Returns an owned handle (free with
 * hxnet_banner_fetch_close) or NULL on bad arguments. */
extern HxnetBannerFetch *hxnet_banner_fetch_open (const guint8 *url,
                                                  gsize url_len);

/* Poll for the one-shot result. HXNET_BANNER_PENDING (try later),
 * HXNET_BANNER_DONE (out->bytes_* set, valid until close), or
 * HXNET_BANNER_ERROR (out->err_* set, or the worker vanished). */
extern int hxnet_banner_fetch_poll (HxnetBannerFetch *handle,
                                    HxnetBannerOut *out);

/* Cancel (if running) and free the handle. NULL-safe. */
extern void hxnet_banner_fetch_close (HxnetBannerFetch *handle);

G_END_DECLS

#endif /* HX_BANNER_HTTP_FFI_H */
