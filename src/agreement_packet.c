/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <string.h>
#include <stdint.h>
#include <glib.h>

#include "compat.h"
#include "hotline.h"
#include "hotline_proto.h"
#include "proto_helpers.h"
#include "agreement_packet.h"

int
hx_agreement_agree_build_chunks (const hx_agreement_agree_request *req,
                                 struct hx_chunk *chunks, int chunks_cap,
                                 guint8 *scratch, size_t scratch_cap)
{
    if (!req) {
        return 0;
    }
    /* chunks_cap is signed in the public C API; a negative value
     * coming from a buggy caller would wrap to a huge size_t on the
     * (size_t) cast below and slip past the Rust FFI's chunks_cap <
     * MAX_CHUNKS guard. Fail closed here so the cast only ever runs
     * against a non-negative value. */
    if (chunks_cap < 0) {
        return 0;
    }
    /* chunk layout moved to gtkhx_proto_build_agreement_agree
     * _chunks in the Rust hotline-proto crate. The wire shape (ICON +
     * NAME + OPTIONS, all three mandatory — Mobius panics without
     * OPTIONS) is unchanged. */
    return (int)gtkhx_proto_build_agreement_agree_chunks (
        req->icon, (const uint8_t *)req->display_name,
        req->display_name_len, req->options, chunks,
        (size_t)chunks_cap, scratch, scratch_cap);
}
