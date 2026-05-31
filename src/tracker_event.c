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
#include <arpa/inet.h>
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
    gchar *u8 = g_convert_with_fallback (src, (gssize) len, "UTF-8",
                                         "MACINTOSH", "?", NULL, NULL, NULL);
    if (!u8) {
        /* Pathological — return a length-bounded raw copy so the
         * view side at least gets something it can render. */
        return g_strndup (src, len);
    }
    return u8;
}

/* Stamp the printable .address field from raw address bytes,
 * gating on the addr_type discriminator. Returns the freshly
 * allocated string, or NULL if addr_type is unknown (caller fails
 * the construction). */
static char *
format_address (guint8 addr_type, const guint8 *address, gsize address_len)
{
    char buf[INET6_ADDRSTRLEN] = { 0 };

    switch (addr_type) {
    case HTRK_V3_ADDR_IPV4: {
        if (!address || address_len != 4) {
            return NULL;
        }
        struct in_addr ia;
        memcpy (&ia.s_addr, address, 4);
        if (!inet_ntop (AF_INET, &ia, buf, sizeof (buf))) {
            /* Should never fail for AF_INET + sufficient buffer,
             * but fall back to a stringified u32 so we don't crash
             * the UI. */
            g_snprintf (buf, sizeof (buf), "%u.%u.%u.%u",
                        address[0], address[1], address[2], address[3]);
        }
        return g_strdup (buf);
    }

    case HTRK_V3_ADDR_IPV6: {
        if (!address || address_len != 16) {
            return NULL;
        }
        struct in6_addr ia6;
        memcpy (&ia6, address, 16);
        if (!inet_ntop (AF_INET6, &ia6, buf, sizeof (buf))) {
            return g_strdup ("::");
        }
        return g_strdup (buf);
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
        gchar *raw = g_strndup ((const char *) address, address_len);
        gchar *valid = g_utf8_make_valid (raw, (gssize) address_len);
        g_free (raw);
        return valid;
    }

    default:
        return NULL;
    }
}

HxTrackerServer *
hx_tracker_server_new_v1 (struct in_addr addr, guint16 port, guint16 nusers,
                          const char *name_bytes, gsize name_len,
                          const char *desc_bytes, gsize desc_len, int total)
{
    HxTrackerServer *e = g_new0 (HxTrackerServer, 1);

    e->addr_type = HTRK_V3_ADDR_IPV4;

    /* Render the IPv4 into a printable form. inet_ntoa returns a
     * pointer to a static buffer, which is racy in a multi-threaded
     * codebase; inet_ntop is the thread-safe equivalent. */
    char buf[INET_ADDRSTRLEN] = { 0 };
    if (!inet_ntop (AF_INET, &addr, buf, sizeof (buf))) {
        g_snprintf (buf, sizeof (buf), "0.0.0.0");
    }
    e->address = g_strdup (buf);

    e->port   = port;
    e->nusers = nusers;

    /* v1 names + descriptions arrived MacRoman; transcode once
     * here so the view side always sees UTF-8 regardless of
     * which protocol fed the record. */
    e->name = macroman_to_utf8_dup (name_bytes, name_len);
    e->desc = macroman_to_utf8_dup (desc_bytes, desc_len);

    e->tlv_count = 0;
    e->tlv_bytes = NULL;

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
    e->address   = format_address (addr_type, address, address_len);
    if (!e->address) {
        g_free (e);
        return NULL;
    }

    e->port   = port;
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

    e->total = total;
    return e;
}

HxTrackerServer *
hx_tracker_server_copy (HxTrackerServer *e)
{
    if (!e) {
        return NULL;
    }
    HxTrackerServer *c = g_new0 (HxTrackerServer, 1);
    c->addr_type = e->addr_type;
    c->address   = g_strdup (e->address ? e->address : "");
    c->port      = e->port;
    c->nusers    = e->nusers;
    c->name      = g_strdup (e->name ? e->name : "");
    c->desc      = g_strdup (e->desc ? e->desc : "");
    c->tlv_count = e->tlv_count;
    c->tlv_bytes = e->tlv_bytes ? g_bytes_ref (e->tlv_bytes) : NULL;
    c->total     = e->total;
    return c;
}

void
hx_tracker_server_free (HxTrackerServer *e)
{
    if (!e) {
        return;
    }
    g_free (e->address);
    g_free (e->name);
    g_free (e->desc);
    if (e->tlv_bytes) {
        g_bytes_unref (e->tlv_bytes);
    }
    g_free (e);
}

G_DEFINE_BOXED_TYPE (HxTrackerServer, hx_tracker_server, hx_tracker_server_copy,
                     hx_tracker_server_free)
