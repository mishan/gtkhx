/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/tracker_v3_meta.h — the typed metadata a v3 tracker record carries
 * in its TLV trailer, as C sees it.
 *
 * The decoder is hxproto's TrackerMeta (hx-libs), the same one hxd-ng
 * encodes its registrations with; gtkhx-core::boxed::tracker builds this
 * struct from it and owns the functions declared below. The struct stays
 * defined here for tracker_event.c, which hangs one off every
 * HxTrackerServer, and its layout is pinned on both sides: const asserts
 * in gtkhx-core, _Static_asserts in tracker_event.c.
 *
 * Absent strings are NULL, absent numbers 0; has_max_users and
 * has_timezone_offset tell 0 from absent where that matters. Unknown
 * maturity and category values read as GENERAL and UNSPECIFIED, as the
 * specification requires.
 */

#ifndef HX_TRACKER_V3_META_H
#define HX_TRACKER_V3_META_H

#include <glib.h>

G_BEGIN_DECLS

/* Vocabulary for HTRK_V3_TLV_MATURITY (0x0205). Unknown values
 * MUST be treated as 0 (general) per the spec's closed-vocab rule. */
typedef enum {
    HX_TRACKER_V3_MATURITY_GENERAL = 0,
    HX_TRACKER_V3_MATURITY_TEEN = 1,
    HX_TRACKER_V3_MATURITY_MATURE = 2,
    HX_TRACKER_V3_MATURITY_ADULT = 3,
} HxTrackerV3Maturity;

/* Vocabulary for HTRK_V3_TLV_LISTING_CATEGORY (0x0501). Unknown
 * values MUST be treated as 0 (unspecified) per the spec. */
typedef enum {
    HX_TRACKER_V3_CATEGORY_UNSPECIFIED = 0,
    HX_TRACKER_V3_CATEGORY_GENERAL = 1,
    HX_TRACKER_V3_CATEGORY_DEVELOPMENT = 2,
    HX_TRACKER_V3_CATEGORY_ARCHIVE = 3,
    HX_TRACKER_V3_CATEGORY_WAREZ = 4,
    HX_TRACKER_V3_CATEGORY_GAMING = 5,
    HX_TRACKER_V3_CATEGORY_MEDIA = 6,
    HX_TRACKER_V3_CATEGORY_EDUCATION = 7,
    HX_TRACKER_V3_CATEGORY_RESEARCH = 8,
    HX_TRACKER_V3_CATEGORY_FILE_SHARING = 9,
    HX_TRACKER_V3_CATEGORY_SOCIAL = 10,
    HX_TRACKER_V3_CATEGORY_SECURITY = 11,
    HX_TRACKER_V3_CATEGORY_CREATIVE = 12,
} HxTrackerV3Category;

typedef struct _HxTrackerV3Meta HxTrackerV3Meta;
struct _HxTrackerV3Meta {
    /* Descriptive fields (0x0200 block) — all string-typed unless
     * noted. Strings are UTF-8, NUL-terminated, owned by the meta;
     * NULL means the TLV was absent. */
    char *server_software;        /* 0x0200 — e.g. "hxd/2.0" */
    char *country_code;           /* 0x0201 — ISO 3166-1 alpha-2 */
    char *region;                 /* 0x0202 — freeform city/region */
    char *language;               /* 0x0203 — ISO 639-1 */
    guint16 max_users;            /* 0x0204 — 0 if absent */
    gboolean has_max_users;       /* distinguishes 0 from absent */
    HxTrackerV3Maturity maturity; /* 0x0205 — 0 (GENERAL) default */
    guint32 uptime_secs;          /* 0x0206 — server uptime */
    char *rules_url;              /* 0x0207 */
    char *banner_url;             /* 0x0208 */
    char *icon_url;               /* 0x0209 */
    guint32 link_down_mbit;       /* 0x020A — 0 if absent */
    guint32 link_up_mbit;         /* 0x020B */
    gint16 timezone_offset_min;   /* 0x020C — signed minutes UTC */
    gboolean has_timezone_offset;
    char *contact_url;         /* 0x020D */
    guint32 server_launched;   /* 0x020E — unix ts */
    guint16 min_proto_version; /* 0x0210 */
    guint16 peak_24h;          /* 0x0211 */
    guint16 avg_24h;           /* 0x0212 */
    char *tags;                /* 0x0310 — comma-separated */

    /* Capability fields (0x0300 block). */
    guint16 protocol_version;       /* 0x0300 — e.g. 0x00be (190) */
    gboolean supports_hope;         /* 0x0301 */
    gboolean supports_tls;          /* 0x0302 */
    guint16 tls_port;               /* 0x0303 */
    gboolean supports_inline_media; /* 0x0304 */
    gboolean supports_voice;        /* 0x0305 */
    gboolean supports_large_files;  /* 0x0306 */
    gboolean supports_ipv6;         /* 0x0307 */
    char *hope_ciphers;             /* 0x0309 — comma-separated names */

    /* Content-index fields (0x0400 block). */
    guint32 news_count;          /* 0x0450 */
    guint32 msgboard_count;      /* 0x0451 */
    guint32 files_count;         /* 0x0452 */
    guint32 total_file_size;     /* 0x0453 */
    guint32 last_news_timestamp; /* 0x0454 — 0 = never */
    guint32 last_chat_timestamp; /* 0x0455 — public-chat only per spec */

    /* Privacy / visibility fields (0x0500 block). */
    gboolean private_listing;             /* 0x0500 */
    HxTrackerV3Category listing_category; /* 0x0501 — UNSPECIFIED default */
    gboolean language_strict;             /* 0x0502 — informational */

    /* Tracker-injected fields (0x0600 block) — set by the tracker,
     * not the server. */
    gboolean is_promoted;     /* 0x0600 — pinned by operator */
    guint32 first_seen;       /* 0x0601 — unix ts */
    guint32 last_heartbeat;   /* 0x0602 */
    gboolean verified_online; /* 0x0603 */
};

/* Decode a record's TLV trailer — `buf_len` bytes holding `tlv_count`
 * fields — into a new meta. NULL when the trailer is malformed (a length
 * overruns, or the count and the bytes disagree), which makes the record
 * untrustworthy. A zero count gives an all-absent meta, which is what v1
 * records carry. */
extern HxTrackerV3Meta *
hx_tracker_v3_meta_new (const guint8 *buf, gsize buf_len, guint16 tlv_count);

/* Deep copy, for the HxTrackerServer boxed copy. */
extern HxTrackerV3Meta *hx_tracker_v3_meta_copy (HxTrackerV3Meta *src);

/* Frees the struct and its strings. Safe on NULL. */
extern void hx_tracker_v3_meta_free (HxTrackerV3Meta *meta);

G_END_DECLS

#endif /* HX_TRACKER_V3_META_H */
