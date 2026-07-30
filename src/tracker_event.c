/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/tracker_event.c — HxTrackerServer boxed type implementation.
 * Pure value-object plumbing; no GTK, no network I/O. Drivable
 * from Tier 2 tests if/when we want to pin the construction
 * contract.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include "compat.h"
#include "hotline.h"
#include "tracker_event.h"

/* Best-effort MacRoman → UTF-8 transcode with '?' fallback for
 * unmappable bytes. Returns a g_strdup-ed buffer the caller owns.
 * Used by the v1 constructor only — v3 strings are already UTF-8
 * per spec. */
static char *
macroman_to_utf8_dup (const char *src, gsize len)
{
    if (!src || len == 0) {
        return g_strdup ("");
    }
    /* g_convert_with_fallback takes a length-prefixed input and
     * returns a freshly-allocated NUL-terminated buffer. The
     * fallback char fires when MacRoman has a byte the converter
     * can't map to a Unicode code point; '?' matches what the old
     * tracker_server_create did. */
    gchar *u8 = g_convert_with_fallback (src, (gssize)len, "UTF-8", "MACINTOSH",
                                         "?", NULL, NULL, NULL);
    if (!u8) {
        /* Pathological — return a length-bounded raw copy so the
         * view side at least gets something it can render. */
        return g_strndup (src, len);
    }
    return u8;
}

/* Render network-byte-order address bytes into a printable string via
 * GInetAddress — portable (no POSIX inet_ntop). `family` selects the
 * length: G_SOCKET_FAMILY_IPV4 reads 4 bytes, IPV6 reads 16. Caller owns
 * the returned string. */
static char *
format_ip (const guint8 *bytes, GSocketFamily family)
{
    /* A family-appropriate placeholder for the failure paths below, so a
     * malformed tracker record yields a harmless string rather than a
     * crash while building the event. */
    const char *placeholder
        = (family == G_SOCKET_FAMILY_IPV6) ? "::" : "0.0.0.0";

    GInetAddress *a = g_inet_address_new_from_bytes (bytes, family);
    if (!a) {
        /* g_inet_address_new_from_bytes can return NULL on an unexpected
         * family / bad input — don't deref it. */
        return g_strdup (placeholder);
    }
    char *s = g_inet_address_to_string (a);
    g_object_unref (a);
    /* to_string is non-NULL for a valid address, but stay defensive. */
    return s ? s : g_strdup (placeholder);
}

/* Stamp the printable .address field from raw address bytes,
 * gating on the addr_type discriminator. Returns the freshly
 * allocated string, or NULL if addr_type is unknown (caller fails
 * the construction). */
static char *
format_address (guint8 addr_type, const guint8 *address, gsize address_len)
{
    switch (addr_type) {
    case HTRK_V3_ADDR_IPV4: {
        if (!address || address_len != 4) {
            return NULL;
        }
        return format_ip (address, G_SOCKET_FAMILY_IPV4);
    }

    case HTRK_V3_ADDR_IPV6: {
        if (!address || address_len != 16) {
            return NULL;
        }
        return format_ip (address, G_SOCKET_FAMILY_IPV6);
    }

    case HTRK_V3_ADDR_HOSTNAME: {
        /* UTF-8 hostname per spec. A malformed tracker can put
         * invalid UTF-8 here, which would later trip Pango
         * critical-warnings the same way an unvalidated server
         * name would. g_utf8_make_valid replaces bad sequences
         * with U+FFFD; same defence we apply to name/desc in
         * hx_tracker_server_new_v3. Empty hostnames are
         * technically legal-but-useless on the wire — return a
         * non-NULL empty string so subscribers don't have to
         * NULL-check downstream. */
        if (!address || address_len == 0) {
            return g_strdup ("");
        }
        gchar *raw = g_strndup ((const char *)address, address_len);
        gchar *valid = g_utf8_make_valid (raw, (gssize)address_len);
        g_free (raw);
        return valid;
    }

    default:
        return NULL;
    }
}

HxTrackerServer *
hx_tracker_server_new_v1 (guint32 addr, guint16 port, guint16 nusers,
                          const char *name_bytes, gsize name_len,
                          const char *desc_bytes, gsize desc_len, int total)
{
    HxTrackerServer *e = g_new0 (HxTrackerServer, 1);

    e->addr_type = HTRK_V3_ADDR_IPV4;

    /* Render the IPv4 into a printable form. `addr`'s bytes are already
     * in network order — exactly what g_inet_address_new_from_bytes
     * expects — so hand them straight over. */
    e->address = format_ip ((const guint8 *)&addr, G_SOCKET_FAMILY_IPV4);

    e->port = port;
    e->nusers = nusers;

    /* v1 names + descriptions arrived MacRoman; transcode once
     * here so the view side always sees UTF-8 regardless of
     * which protocol fed the record. */
    e->name = macroman_to_utf8_dup (name_bytes, name_len);
    e->desc = macroman_to_utf8_dup (desc_bytes, desc_len);

    e->tlv_count = 0;
    e->tlv_bytes = NULL;
    /* v1 records carry no TLVs but consumers shouldn't have to
     * NULL-check meta. Allocate a zero-init meta — every is_*
     * flag is FALSE, every string is NULL, every numeric is 0 —
     * which matches the "no TLVs were advertised" reality. */
    e->meta = hx_tracker_v3_meta_new (NULL, 0, 0);

    e->total = total;
    return e;
}

HxTrackerServer *
hx_tracker_server_new_v3 (guint8 addr_type, const guint8 *address,
                          gsize address_len, guint16 port, guint16 nusers,
                          const char *name, gsize name_len, const char *desc,
                          gsize desc_len, guint16 tlv_count,
                          const guint8 *tlv_bytes, gsize tlv_bytes_len,
                          int total)
{
    HxTrackerServer *e = g_new0 (HxTrackerServer, 1);

    e->addr_type = addr_type;
    e->address = format_address (addr_type, address, address_len);
    if (!e->address) {
        g_free (e);
        return NULL;
    }

    e->port = port;
    e->nusers = nusers;

    /* v3 strings are already UTF-8 per spec — but defensively
     * validate before handing to Pango downstream. g_utf8_make
     * _valid is cheap when the input is valid (returns a strdup),
     * and replaces bad sequences with U+FFFD when not, so we
     * never feed Pango a critical-warning trigger. */
    if (name && name_len > 0) {
        gchar *n = g_strndup (name, name_len);
        gchar *valid = g_utf8_make_valid (n, name_len);
        g_free (n);
        e->name = valid;
    } else {
        e->name = g_strdup ("");
    }
    if (desc && desc_len > 0) {
        gchar *d = g_strndup (desc, desc_len);
        gchar *valid = g_utf8_make_valid (d, desc_len);
        g_free (d);
        e->desc = valid;
    } else {
        e->desc = g_strdup ("");
    }

    e->tlv_count = tlv_count;
    if (tlv_bytes && tlv_bytes_len > 0) {
        /* g_bytes_new copies; subscribers can ref/unref without
         * the source buffer's lifetime mattering. */
        e->tlv_bytes = g_bytes_new (tlv_bytes, tlv_bytes_len);
    } else {
        e->tlv_bytes = NULL;
    }

    /* Parse the TLV blob into a typed view. NULL return here means
     * the blob was malformed (length-prefix overran the buffer, or
     * count > available bytes); the whole record is therefore
     * untrustworthy and we reject the construction. The wire-side
     * record parser already bounds-checked the slice in
     * hx_tracker_v3_parse_record, so a NULL here points at a
     * tlv_count / tlv_bytes_len mismatch in our own code path
     * rather than tracker-side junk. Fail closed. */
    e->meta = hx_tracker_v3_meta_new (tlv_bytes, tlv_bytes_len, tlv_count);
    if (!e->meta) {
        g_free (e->address);
        g_free (e->name);
        g_free (e->desc);
        if (e->tlv_bytes) {
            g_bytes_unref (e->tlv_bytes);
        }
        g_free (e);
        return NULL;
    }

    e->total = total;
    return e;
}

/* Phase R4.2b: hx_tracker_server_copy / hx_tracker_server_free and the
 * boxed-type registration (hx_tracker_server_get_type) moved to Rust —
 * rust/crates/gtkhx-boxed/src/tracker.rs. The struct stays C-visible
 * (hx_tracker_server_new_v1/_v3 above fill it; consumers read fields),
 * so the Rust #[repr(C)] mirror's layout is pinned against this assert.
 * The Rust copy/free deep-copy the GBytes (ref/unref) and the
 * HxTrackerV3Meta (whose copy/free also moved). */
_Static_assert (sizeof (HxTrackerServer) == 72,
                "HxTrackerServer layout must match the Rust #[repr(C)] "
                "mirror in gtkhx-boxed::tracker");
/* Field offsets too — a size-only pin misses field reorderings /
 * padding changes that keep the total size. Mirror gtkhx-boxed::tracker's
 * offset_of! asserts exactly. */
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, addr_type) == 0,
                "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, address) == 8,
                "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, port) == 16, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, nusers) == 18,
                "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, name) == 24, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, desc) == 32, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, tlv_count) == 40,
                "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, tlv_bytes) == 48,
                "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, meta) == 56, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxTrackerServer, total) == 64, "field offset");
