/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/tracker_v3_meta.c — typed-accessor module for the per-record
 * TLV trailer in v3 tracker responses. See header for the design
 * sketch.
 */

#include "config.h"

#include <string.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "hotline_proto.h" /* gtkhx_proto_tracker_v3_meta_read_* */
#include "tracker_v3.h"
#include "tracker_v3_meta.h"

/* ---- Small typed readers --------------------------------------- */
/*
 * Each TLV payload arrives as a length-prefixed byte slice. The
 * spec encodes ints big-endian, strings as raw UTF-8, booleans as
 * 0/non-zero u8s. Type signatures live alongside the TLV id table
 * in src/hotline.h; the spec catalog is the source of truth.
 *
 * Each reader fails CLOSED — wrong size returns the spec default
 * (0 / GENERAL / UNSPECIFIED / FALSE / empty-string). A malformed
 * TLV gives the caller a deterministic value, not a half-decoded
 * one, and doesn't trash the rest of the walk.
 *
 * "Wrong size" means anything other than the exact spec-mandated
 * width for that field: a u16 TLV with len=1 (truncated) and one
 * with len=3 (overlong) are both malformed and both default. The
 * fixed-width readers test `len != N`, not `len < N`, so an over-
 * size payload doesn't silently decode from its first N bytes and
 * paper over a corrupt or future-spec-extended TLV.
 *
 * One caveat: the on_tlv switch arms run unconditionally on case
 * entry, so a malformed numeric TLV still flips the field's
 * has_* presence flag (e.g. a malformed MAX_USERS TLV leaves
 * max_users=0 but sets has_max_users=TRUE). Callers that want to
 * filter malformed presence need to validate at the wire layer
 * before reaching the typed view — this view is best-effort.
 *
 * scalar / bool readers + enum-clamp logic moved to the
 * Rust hotline-proto crate. The strict-size + closed-vocab
 * behaviour is preserved byte-for-byte; the thin wrappers below
 * keep the in-file call shape so the on_tlv switch is unchanged.
 * Strings stay in C because they need g_utf8_make_valid +
 * g_strndup, which would complicate the FFI contract for no gain. */

static inline guint8
read_u8 (const guint8 *v, guint16 len, guint8 def)
{
    return gtkhx_proto_tracker_v3_meta_read_u8 (v, len, def);
}

static inline guint16
read_u16 (const guint8 *v, guint16 len, guint16 def)
{
    return gtkhx_proto_tracker_v3_meta_read_u16 (v, len, def);
}

static inline gint16
read_i16 (const guint8 *v, guint16 len, gint16 def)
{
    return gtkhx_proto_tracker_v3_meta_read_i16 (v, len, def);
}

static inline guint32
read_u32 (const guint8 *v, guint16 len, guint32 def)
{
    return gtkhx_proto_tracker_v3_meta_read_u32 (v, len, def);
}

static inline gboolean
read_bool (const guint8 *v, guint16 len)
{
    return gtkhx_proto_tracker_v3_meta_read_bool (v, len);
}

/* g_utf8_make_valid on the borrowed slice, returning a NUL-
 * terminated heap copy. Empty input returns g_strdup("") rather
 * than NULL so consumers don't have to NULL-check downstream —
 * the meta's NULL slot is reserved for "TLV absent". */
static char *
dup_utf8 (const guint8 *v, guint16 len)
{
    if (len == 0) {
        return g_strdup ("");
    }
    gchar *raw = g_strndup ((const char *) v, len);
    gchar *valid = g_utf8_make_valid (raw, (gssize) len);
    g_free (raw);
    return valid;
}

/* ---- TLV walker callback --------------------------------------- */

static gboolean
on_tlv (guint16 id, guint16 value_len, const guint8 *value, gpointer user_data)
{
    HxTrackerV3Meta *m = user_data;

    switch (id) {
    /* Descriptive (0x0200 block) */
    case HTRK_V3_TLV_SERVER_SOFTWARE:
        g_clear_pointer (&m->server_software, g_free);
        m->server_software = dup_utf8 (value, value_len);
        break;
    case HTRK_V3_TLV_COUNTRY_CODE:
        g_clear_pointer (&m->country_code, g_free);
        m->country_code = dup_utf8 (value, value_len);
        break;
    case HTRK_V3_TLV_REGION:
        g_clear_pointer (&m->region, g_free);
        m->region = dup_utf8 (value, value_len);
        break;
    case HTRK_V3_TLV_LANGUAGE:
        g_clear_pointer (&m->language, g_free);
        m->language = dup_utf8 (value, value_len);
        break;
    case HTRK_V3_TLV_MAX_USERS:
        m->max_users     = read_u16 (value, value_len, 0);
        m->has_max_users = TRUE;
        break;
    case HTRK_V3_TLV_MATURITY: {
        guint8 raw = read_u8 (value, value_len, 0);
        /* Spec: unknown values MUST be treated as 0 (GENERAL).
         * Closed-vocab clamp lives in the Rust crate so the C side
         * and any future Rust caller stay locked to the same rule. */
        m->maturity = (HxTrackerV3Maturity)
            gtkhx_proto_tracker_v3_meta_clamp_maturity (raw);
        break;
    }
    case HTRK_V3_TLV_UPTIME:
        m->uptime_secs = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_RULES_URL:
        g_clear_pointer (&m->rules_url, g_free);
        m->rules_url = dup_utf8 (value, value_len);
        break;
    case HTRK_V3_TLV_BANNER_URL:
        g_clear_pointer (&m->banner_url, g_free);
        m->banner_url = dup_utf8 (value, value_len);
        break;
    case HTRK_V3_TLV_ICON_URL:
        g_clear_pointer (&m->icon_url, g_free);
        m->icon_url = dup_utf8 (value, value_len);
        break;
    case HTRK_V3_TLV_LINK_DOWN_MBIT:
        m->link_down_mbit = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_LINK_UP_MBIT:
        m->link_up_mbit = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_TIMEZONE_OFFSET:
        m->timezone_offset_min = read_i16 (value, value_len, 0);
        m->has_timezone_offset = TRUE;
        break;
    case HTRK_V3_TLV_CONTACT_URL:
        g_clear_pointer (&m->contact_url, g_free);
        m->contact_url = dup_utf8 (value, value_len);
        break;
    case HTRK_V3_TLV_SERVER_LAUNCHED:
        m->server_launched = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_MIN_PROTO_VERSION:
        m->min_proto_version = read_u16 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_PEAK_24H:
        m->peak_24h = read_u16 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_AVG_24H:
        m->avg_24h = read_u16 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_TAGS:
        g_clear_pointer (&m->tags, g_free);
        m->tags = dup_utf8 (value, value_len);
        break;

    /* Capability (0x0300 block) */
    case HTRK_V3_TLV_PROTOCOL_VERSION:
        m->protocol_version = read_u16 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_SUPPORTS_HOPE:
        m->supports_hope = read_bool (value, value_len);
        break;
    case HTRK_V3_TLV_SUPPORTS_TLS:
        m->supports_tls = read_bool (value, value_len);
        break;
    case HTRK_V3_TLV_TLS_PORT:
        m->tls_port = read_u16 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_SUPPORTS_INLINE:
        m->supports_inline_media = read_bool (value, value_len);
        break;
    case HTRK_V3_TLV_SUPPORTS_VOICE:
        m->supports_voice = read_bool (value, value_len);
        break;
    case HTRK_V3_TLV_SUPPORTS_LARGEFILE:
        m->supports_large_files = read_bool (value, value_len);
        break;
    case HTRK_V3_TLV_SUPPORTS_IPV6_TLV:
        m->supports_ipv6 = read_bool (value, value_len);
        break;
    case HTRK_V3_TLV_HOPE_CIPHERS:
        g_clear_pointer (&m->hope_ciphers, g_free);
        m->hope_ciphers = dup_utf8 (value, value_len);
        break;

    /* Content index (0x0400 block) */
    case HTRK_V3_TLV_NEWS_COUNT:
        m->news_count = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_MSGBOARD_COUNT:
        m->msgboard_count = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_FILES_COUNT:
        m->files_count = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_TOTAL_FILE_SIZE:
        m->total_file_size = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_LAST_NEWS_TIME:
        m->last_news_timestamp = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_LAST_CHAT_TIME:
        m->last_chat_timestamp = read_u32 (value, value_len, 0);
        break;

    /* Privacy / visibility (0x0500 block) */
    case HTRK_V3_TLV_PRIVATE_LISTING:
        m->private_listing = read_bool (value, value_len);
        break;
    case HTRK_V3_TLV_LISTING_CATEGORY: {
        guint8 raw = read_u8 (value, value_len, 0);
        /* Spec: unknown values MUST be treated as 0 (UNSPECIFIED).
         * Closed-vocab clamp lives in the Rust crate so the C side
         * and any future Rust caller stay locked to the same rule. */
        m->listing_category = (HxTrackerV3Category)
            gtkhx_proto_tracker_v3_meta_clamp_listing_category (raw);
        break;
    }
    case HTRK_V3_TLV_LANGUAGE_STRICT:
        m->language_strict = read_bool (value, value_len);
        break;

    /* Tracker-injected (0x0600 block) */
    case HTRK_V3_TLV_IS_PROMOTED:
        m->is_promoted = read_bool (value, value_len);
        break;
    case HTRK_V3_TLV_FIRST_SEEN:
        m->first_seen = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_LAST_HEARTBEAT:
        m->last_heartbeat = read_u32 (value, value_len, 0);
        break;
    case HTRK_V3_TLV_VERIFIED_ONLINE:
        m->verified_online = read_bool (value, value_len);
        break;

    /* Address-fold TLVs (ADDRESS_IPV6, HOSTNAME) — the wire-side
     * record's addr_type byte is the authoritative source for
     * how to reach the server. The TLV variants are spec-allowed
     * but redundant on the client; the boxed-event constructor
     * already populated event->address from the fixed header.
     * Drop on the floor here.
     *
     * Unknown ids: also drop. Forward-compat: any new field a
     * future spec rev adds gets silently skipped until we grow a
     * case for it. */
    default:
        break;
    }

    return TRUE; /* keep walking */
}

/* ---- Public API ------------------------------------------------ */

HxTrackerV3Meta *
hx_tracker_v3_meta_new (const guint8 *buf, gsize buf_len, guint16 tlv_count)
{
    HxTrackerV3Meta *m = g_new0 (HxTrackerV3Meta, 1);

    /* Zero counts / NULL bufs are valid — the v1 emit path routes
     * through here too, with no TLVs. Return the zeroed meta. */
    if (tlv_count == 0) {
        return m;
    }

    if (!hx_tracker_v3_walk_tlvs (buf, buf_len, tlv_count, on_tlv, m)) {
        /* Malformed input — a TLV length overran the buf, or the
         * declared count didn't match the supplied byte slice. The
         * walker stops at the bad TLV; whatever fields landed
         * before that point are still in m, but a malformed blob
         * isn't trustworthy, so free and return NULL. */
        hx_tracker_v3_meta_free (m);
        return NULL;
    }

    return m;
}

HxTrackerV3Meta *
hx_tracker_v3_meta_new_from_bytes (GBytes *bytes, guint16 tlv_count)
{
    if (!bytes) {
        return hx_tracker_v3_meta_new (NULL, 0, 0);
    }
    gsize n = 0;
    const guint8 *p = g_bytes_get_data (bytes, &n);
    return hx_tracker_v3_meta_new (p, n, tlv_count);
}

/* Phase R4.2b: hx_tracker_v3_meta_copy / hx_tracker_v3_meta_free moved to
 * Rust — rust/crates/gtkhx-boxed/src/tracker.rs. The struct stays
 * C-visible (hx_tracker_v3_meta_new above fills it; consumers read
 * fields). The Rust free is still called from C (this file's _new error
 * path and tracker_event.c / tracker_row.c), now resolved against the
 * gtkhx-boxed staticlib. The Rust side treats this struct as an opaque
 * sized buffer and fixes up the ten owned char* fields by byte offset,
 * so size + those ten offsets are pinned here. */
_Static_assert (sizeof (HxTrackerV3Meta) == 216,
                "HxTrackerV3Meta size must match the Rust opaque mirror "
                "in gtkhx-boxed::tracker");
_Static_assert (offsetof (HxTrackerV3Meta, server_software) == 0, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, country_code) == 8, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, region) == 16, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, language) == 24, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, rules_url) == 48, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, banner_url) == 56, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, icon_url) == 64, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, contact_url) == 88, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, tags) == 112, "meta str offset");
_Static_assert (offsetof (HxTrackerV3Meta, hope_ciphers) == 152, "meta str offset");
