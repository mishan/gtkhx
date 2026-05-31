/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/tracker_event.h — HxTrackerServer boxed value object.
 *
 * Carries one parsed server record from the network side (tracker
 * fetch state machine in network.c) to the view side (tracker
 * window in tracker.c) as a single immutable payload on the
 * GtkhxSession::tracker-server-create signal. Same architectural
 * pattern as HxChatEvent / HxMsgEvent in proto_helpers.h, but
 * scoped to the tracker subsystem so proto_helpers.h doesn't grow
 * a non-Hotline-session payload type.
 *
 * Why a boxed type and not seven scalar signal args (which is what
 * the v1-only path used to do):
 *
 *   - v3 records carry richer metadata (address-type byte, TLV
 *     trailer, optional IPv6 / hostname addressing). Squeezing all
 *     of that into a flat marshaller signature gets unergonomic
 *     fast, especially when Phase B adds parsed TLV accessors.
 *   - Future-proofing for Phase B: the v3 TLV trailer ships as an
 *     opaque GBytes here; once Phase B decodes individual TLVs the
 *     parsed view bolts on without changing the signal signature.
 *
 * Construction shapes:
 *
 *   v1 path — hx_tracker_server_new_v1: takes the legacy fixed-
 *   record fields (in_addr, port, nusers, MacRoman name + desc).
 *   The MacRoman → UTF-8 transcode that used to live in tracker.c
 *   's tracker_server_create runs here so the view side always
 *   sees UTF-8.
 *
 *   v3 path — hx_tracker_server_new_v3: takes the parsed record's
 *   addr_type, raw address bytes, UTF-8 name/desc, and the raw
 *   TLV trailer bytes. address bytes are rendered into a printable
 *   form internally (inet_ntop for IPv4/IPv6; literal copy for
 *   hostname) and stored in the .address field.
 *
 * String ownership: all char* / GBytes* fields are owned by the
 * event. The boxed copy/free hooks duplicate strings + ref/unref
 * the GBytes so multiple signal subscribers can each hold the
 * event past the emit.
 */

#ifndef HX_TRACKER_EVENT_H
#define HX_TRACKER_EVENT_H 1

#include <glib.h>
#include <glib-object.h>
#include <netinet/in.h>     /* struct in_addr */
#include "tracker_v3_meta.h"

G_BEGIN_DECLS

typedef struct _HxTrackerServer HxTrackerServer;

struct _HxTrackerServer {
    /* Address-type discriminator (HTRK_V3_ADDR_*). v1 records and
     * v3 IPv4 records both report 0x04; v3 IPv6 records report
     * 0x06; v3 hostname records report 0x48. Subscribers can use
     * this to choose a UI affordance (country flag from IPv4
     * geoip, "[promoted]" badge from a TLV, etc.) but MUST NOT
     * use it as a filter — tracker.c's dedup is keyed on the
     * printable `address` string below, not the addr_type byte. */
    guint8 addr_type;

    /* Printable / connectable display address (UTF-8, NUL-
     * terminated, always non-NULL):
     *   IPv4 (0x04)     → "203.0.113.42"
     *   IPv6 (0x06)     → "2001:db8::1"
     *   Hostname (0x48) → literal hostname ("tracker.example.org")
     * Used by hx_connect (which takes a string) and rendered into
     * the tracker list's "Address" column. */
    char *address;

    guint16 port;
    guint16 nusers;

    /* UTF-8, NUL-terminated, non-NULL (empty string when the
     * source record carried zero-length fields). */
    char *name;
    char *desc;

    /* TLV trailer — only populated for v3 records. tlv_count is
     * the entry count; tlv_bytes wraps the concatenated raw
     * {id, len, value} bytes for forward-compat with future spec
     * revs (the typed-accessor module below knows the current
     * catalog; new ids fall through to the raw blob until we
     * grow a case). NULL + tlv_count==0 for v1 records. */
    guint16 tlv_count;
    GBytes *tlv_bytes;

    /* Typed view over tlv_bytes. Populated by hx_tracker_server
     * _new_v3 at construction time so subscribers don't each have
     * to walk the blob; v1 records get an all-zero meta so callers
     * can read fields like `meta->is_promoted` unconditionally
     * without a NULL guard. Non-NULL on every well-formed event;
     * NULL only when the constructor rejected a malformed TLV
     * blob (and the constructor returns NULL in that case too,
     * so subscribers never see it). */
    HxTrackerV3Meta *meta;

    /* Tracker-side "this many records in this batch" — feeds the
     * progress widget. For v1: copied from the reply header's
     * nservers. For v3: copied from the response header's
     * record_count. */
    int total;
};

#define HX_TYPE_TRACKER_SERVER (hx_tracker_server_get_type ())
extern GType hx_tracker_server_get_type (void) G_GNUC_CONST;

/* Build from a legacy v1 fixed-record. `addr` is in network byte
 * order (as parsed from the wire). `name_bytes` / `desc_bytes` are
 * MacRoman; the constructor runs them through
 * g_convert_with_fallback ("UTF-8", "MACINTOSH") with '?' as the
 * fallback character. Either may be NULL; lengths of zero produce
 * empty strings (not NULL). Returns a freshly-allocated event the
 * caller owns. */
extern HxTrackerServer *
hx_tracker_server_new_v1 (struct in_addr addr, guint16 port, guint16 nusers,
                          const char *name_bytes, gsize name_len,
                          const char *desc_bytes, gsize desc_len, int total);

/* Build from a parsed v3 record. `address` bytes interpretation is
 * gated on `addr_type` (HTRK_V3_ADDR_IPV4 → 4 bytes; IPV6 → 16
 * bytes; HOSTNAME → UTF-8 hostname, address_len bytes). The
 * constructor renders the address into the .address printable form
 * itself. `name` / `desc` are already UTF-8 per spec. If
 * `tlv_bytes_len > 0`, the bytes are copied into a GBytes; pass
 * tlv_count=0 + tlv_bytes=NULL + tlv_bytes_len=0 for records with
 * no trailer. Returns NULL only on a wholly malformed address
 * (e.g. unknown addr_type sneaking past the wire parser). */
extern HxTrackerServer *
hx_tracker_server_new_v3 (guint8 addr_type, const guint8 *address,
                          gsize address_len, guint16 port, guint16 nusers,
                          const char *name, gsize name_len, const char *desc,
                          gsize desc_len, guint16 tlv_count,
                          const guint8 *tlv_bytes, gsize tlv_bytes_len,
                          int total);

extern HxTrackerServer *hx_tracker_server_copy (HxTrackerServer *e);
extern void hx_tracker_server_free (HxTrackerServer *e);

G_END_DECLS

#endif /* HX_TRACKER_EVENT_H */
