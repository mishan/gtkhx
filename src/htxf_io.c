/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * htxf_io.c — thin C shim over hxnet's Rust HTXF subchannel transport.
 * See htxf_io.h for the full rationale. The byte pump, AEAD framing,
 * and TLS wrap all live in rust/crates/hxnet/src/htxf.rs since the
 * HTXF→Rust H2 re-wire; this file only casts htxf->hx back to the
 * hxnet handle and maps the Rust `-1` error onto the errno-set `< 1`
 * idiom the worker loops use.
 */

#include "config.h"
#include <errno.h>
#include <sys/types.h>
#include <glib.h>
#include "compat.h" /* PACKED — required before hotline.h / protocol.h */
#include "hotline.h"
#include "protocol.h"
#include "htxf_io.h"
#include "debug.h"

void
htxf_io_init (struct htxf_conn *htxf)
{
    if (!htxf) {
        return;
    }
    /* Caller memsets the parent struct before use; this just makes the
     * "channel not yet open" state explicit for grep-ability. */
    htxf->hx = NULL;
}

void
htxf_io_release (struct htxf_conn *htxf)
{
    if (!htxf || !htxf->hx) {
        return;
    }
    hxnet_htxf_close ((HtxfConn *) htxf->hx);
    htxf->hx = NULL;
}

ssize_t
htxf_io_read (struct htxf_conn *htxf, void *buf, size_t len)
{
    if (!htxf || !htxf->hx) {
        errno = EINVAL;
        return -1;
    }
    ssize_t r = hxnet_htxf_read ((HtxfConn *) htxf->hx, (guint8 *) buf, len);
    if (r < 0) {
        debug_log ("xfer", "htxf_io_read: hxnet channel error");
        errno = EIO;
        return -1;
    }
    return r;
}

ssize_t
htxf_io_write (struct htxf_conn *htxf, const void *buf, size_t len)
{
    if (!htxf || !htxf->hx) {
        errno = EINVAL;
        return -1;
    }
    ssize_t w
        = hxnet_htxf_write ((HtxfConn *) htxf->hx, (const guint8 *) buf, len);
    if (w < 0) {
        debug_log ("xfer", "htxf_io_write: hxnet channel error");
        errno = EIO;
        return -1;
    }
    return w;
}

int
htxf_io_set_read_timeout (struct htxf_conn *htxf, guint32 timeout_ms)
{
    if (!htxf || !htxf->hx) {
        errno = EINVAL;
        return -1;
    }
    if (hxnet_htxf_set_read_timeout ((HtxfConn *) htxf->hx, timeout_ms) < 0) {
        /* hxnet returns -1 (e.g. a setsockopt failure) without touching
         * errno; set EIO so callers see a consistent value rather than a
         * stale one, matching htxf_io_read / _write. */
        debug_log ("xfer", "htxf_io_set_read_timeout: hxnet returned error");
        errno = EIO;
        return -1;
    }
    return 0;
}
