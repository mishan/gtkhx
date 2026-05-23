/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * agreement_packet.{c,h} — shared HTLC_HDR_AGREEMENTAGREE packet
 * builder used by both production (src/network.c::hx_send_agreement
 * _agree) and the integration test harness (tests/integration/
 * integration_harness.c::integration_send_agreementagree_hope).
 *
 * Modelled on login_packet.{c,h} and chat_history.{c,h} — pure
 * chunk-array builder, no network / GTK / globals. Callers fill in
 * an hx_agreement_agree_request describing the message, call
 * hx_agreement_agree_build_chunks to turn it into an hx_chunk array
 * + scratch buffer, then hand the array to whatever wire-encoder
 * they have (hlpack_chunks / hlwrite_chunks / integration_send
 * _message_hope_chunks).
 *
 * The wire shape is three chunks:
 *
 *   HTLC_DATA_ICON     u16 BE         icon resource id
 *   HTLC_DATA_NAME     bytes          display name (already encoded)
 *   HTLC_DATA_OPTIONS  u16 BE         options bitmap (0 today)
 *
 * NAME encoding is the caller's responsibility — production calls
 * gtkhx_text_for_wire (UTF-8 vs Mac Roman based on CAP_TEXT
 * _ENCODING) before invoking the builder; tests typically pass
 * ASCII directly which is identical in both encodings. Keeping the
 * encoder out of the builder keeps the module dependency-free
 * (text_util.c pulls in iconv + glib's encoding tables).
 *
 * OPTIONS: production always sends 0, but Mobius PANICS if the
 * chunk is missing entirely (see hx_send_agreement_agree's comment
 * for the gory details). The chunk is mandatory; the value defaults
 * to 0.
 */

#ifndef HX_AGREEMENT_PACKET_H
#define HX_AGREEMENT_PACKET_H 1

#include "config.h"
#include <glib.h>
#include <stdint.h>
#include <stddef.h>

struct hx_chunk;

typedef struct {
    /* HTLC_DATA_ICON value. Always emitted. */
    guint16 icon;

    /* HTLC_DATA_NAME body. Already encoded to the negotiated wire
     * encoding (UTF-8 if CAP_TEXT_ENCODING was negotiated, Mac
     * Roman otherwise). NULL is treated as len=0; both produce a
     * zero-length NAME chunk. */
    const char *display_name;
    guint16 display_name_len;

    /* HTLC_DATA_OPTIONS bitmap. Production sends 0 unconditionally
     * (no real "refuse PM / refuse chat / auto-response" pref is
     * exposed in the UI yet). The chunk is mandatory — Mobius
     * panics without it. */
    guint16 options;
} hx_agreement_agree_request;

/* Scratch buffer the caller must provide for hx_agreement_agree
 * _build_chunks to lay out the two network-order u16s. The chunk
 * data pointers point into this scratch, so it must outlive the
 * eventual hlpack_chunks call.
 *
 * 8 bytes covers two u16s. We round up to 16 to leave headroom if
 * the wire shape ever grows. */
#define HX_AGREEMENT_AGREE_SCRATCH_SIZE 16

/* Maximum number of chunks the builder emits. */
#define HX_AGREEMENT_AGREE_MAX_CHUNKS 3

/*
 * Translate the request into an hx_chunk array. Returns the number
 * of chunks filled (== HX_AGREEMENT_AGREE_MAX_CHUNKS on success,
 * 0 on validation / overflow failure). The caller hands the array
 * to hlpack_chunks (or the trace-aware hlwrite_chunks) for actual
 * wire encoding.
 *
 * chunks[] and scratch[] must outlive the hlpack_chunks call —
 * chunks[].data points into scratch and into the caller-owned
 * display_name buffer.
 */
extern int hx_agreement_agree_build_chunks (
    const hx_agreement_agree_request *req,
    struct hx_chunk *chunks, int chunks_cap,
    guint8 *scratch, size_t scratch_cap);

#endif /* HX_AGREEMENT_PACKET_H */
