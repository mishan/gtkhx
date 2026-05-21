/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "hl_code.h"
#include "login_packet.h"

/* Append one entry (u8 namelen || name) to a HOPE algorithm list.
 * Returns the new offset, or 0 on overflow / bad name. */
static size_t
alg_list_append (guint8 *list, size_t offset, size_t cap, const char *name)
{
    size_t name_len = name ? strlen (name) : 0;
    if (name_len == 0 || name_len > 255) {
        return 0;
    }
    if (offset + 1 + name_len > cap) {
        return 0;
    }
    list[offset++] = (guint8) name_len;
    memcpy (list + offset, name, name_len);
    return offset + name_len;
}

/* Build the multi-entry MAC algorithm preference list — strongest
 * first per the HOPE-Secure-Login spec. HMAC-SHA256 is required for
 * AEAD key derivation; SHA1 and MD5 are fallbacks. */
static size_t
build_macalg_list (guint8 *list, size_t cap)
{
    if (cap < 2) {
        return 0;
    }
    guint16 count = htons (3);
    memcpy (list, &count, 2);
    size_t off = 2;
    off = alg_list_append (list, off, cap, "HMAC-SHA256");
    if (!off) {
        return 0;
    }
    off = alg_list_append (list, off, cap, "HMAC-SHA1");
    if (!off) {
        return 0;
    }
    off = alg_list_append (list, off, cap, "HMAC-MD5");
    return off;
}

/* Build a 1-entry algorithm list (cipher / compress). */
static size_t
build_single_alg_list (guint8 *list, size_t cap, const char *name)
{
    if (!name || !*name) {
        return 0;
    }
    if (cap < 2) {
        return 0;
    }
    guint16 count = htons (1);
    memcpy (list, &count, 2);
    return alg_list_append (list, 2, cap, name);
}

int
hx_login_build_chunks (const hx_login_request *req,
                       struct hx_chunk *chunks, int chunks_cap,
                       guint8 *scratch, size_t scratch_cap)
{
    int hc = 0;
    size_t soff = 0;

    g_return_val_if_fail (req != NULL, 0);
    g_return_val_if_fail (chunks != NULL, 0);
    g_return_val_if_fail (chunks_cap >= HX_LOGIN_MAX_CHUNKS, 0);
    g_return_val_if_fail (scratch != NULL, 0);
    g_return_val_if_fail (scratch_cap >= HX_LOGIN_SCRATCH_SIZE, 0);

    if (req->mode == HX_LOGIN_MODE_HOPE_STEP2) {
        /* HOPE Step 2 authenticated LOGIN. All HMAC-derived fields
         * are pre-computed by the caller (see hope.h) and passed in
         * as already-encoded byte buffers; we just splice them into
         * the chunk array in the order rcv.c::rcv_task_login has
         * always emitted them. The harness send_hope_step2 used to
         * duplicate this chunk-ordering logic — they're unified
         * now.
         *
         * Validate the pre-computed pointers eagerly so a builder
         * bug fails here with a clear contract violation rather
         * than tripping the g_return_if_fail inside hlpack_chunks
         * (or worse, NULL-deref'ing in a different packer). len > 0
         * with NULL data is a programming error in every case. */
        g_return_val_if_fail (
            req->login_field_len == 0 || req->login_field != NULL, 0);
        g_return_val_if_fail (
            req->password_mac_len == 0 || req->password_mac != NULL, 0);
        g_return_val_if_fail (
            req->cipher_alg_reply_len == 0
                || req->cipher_alg_reply != NULL, 0);
        g_return_val_if_fail (
            req->compress_alg_reply_len == 0
                || req->compress_alg_reply != NULL, 0);

        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_LOGIN, req->login_field_len, req->login_field
        };
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_PASSWORD, req->password_mac_len, req->password_mac
        };
#ifdef CONFIG_CIPHER
        if (req->cipher_alg_reply_len) {
            chunks[hc++] = (struct hx_chunk) {
                HTLS_DATA_CIPHER_ALG, req->cipher_alg_reply_len,
                req->cipher_alg_reply
            };
        }
#endif
#ifdef CONFIG_COMPRESS
        if (req->compress_alg_reply_len) {
            chunks[hc++] = (struct hx_chunk) {
                HTLS_DATA_COMPRESS_ALG, req->compress_alg_reply_len,
                req->compress_alg_reply
            };
        }
#endif
        /* NAME — always emit. Production passes htlc->name (may be
         * empty when unset). Empty-string chunk has length 0; an
         * empty NAME chunk is the same shape the server expects. */
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_NAME,
            (guint16) (req->display_name ? strlen (req->display_name) : 0),
            req->display_name ? (const void *) req->display_name : NULL
        };

        /* ICON — always emit. */
        size_t icon_off = soff;
        guint16 icon_be = htons (req->icon);
        g_return_val_if_fail (soff + 2 <= scratch_cap, 0);
        memcpy (scratch + soff, &icon_be, 2);
        soff += 2;
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_ICON, 2, scratch + icon_off
        };

        /* CLIENTVERSION — emit only if explicitly set (same gate
         * legacy uses). mhxd reads this in rcv_login to set the
         * `can_ping` access bit: clientversion >= 150 → PING
         * keepalive accepted; missing or below → server rejects
         * HTLC_HDR_PING with a task-error. Without this chunk a
         * HOPE-Secure-Login client looks pre-1.5 to mhxd and the
         * post-login PING timer fires task-errors every 60 s. */
        if (req->client_version) {
            size_t cv_off = soff;
            guint16 cv_be = htons (req->client_version);
            g_return_val_if_fail (soff + 2 <= scratch_cap, 0);
            memcpy (scratch + soff, &cv_be, 2);
            soff += 2;
            chunks[hc++] = (struct hx_chunk) {
                HTLC_DATA_CLIENTVERSION, 2, scratch + cv_off
            };
        }

        /* CAPABILITIES — always emit. STEP2 ignores the send_caps
         * gate because the server needs the caps echo to finalise
         * AEAD activation. caps=0 stays meaningful ("I support the
         * spec but no optional bits"). */
        size_t caps_off = soff;
        guint16 caps_be = htons (req->caps);
        g_return_val_if_fail (soff + 2 <= scratch_cap, 0);
        memcpy (scratch + soff, &caps_be, 2);
        soff += 2;
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_CAPABILITIES, 2, scratch + caps_off
        };
    } else if (req->mode == HX_LOGIN_MODE_HOPE_STEP1) {
        /* The HOPE Step 1 LOGIN and PASSWORD chunks carry a single
         * 0 byte (the spec says zero-length but every server
         * implementation we've seen tolerates / expects the 1-byte
         * placeholder, which the pre-refactor production code
         * emitted). */
        static const guint8 zero_byte = 0;

        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_LOGIN, 1, &zero_byte
        };
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_PASSWORD, 1, &zero_byte
        };

        /* MAC algorithm preference list. */
        size_t mac_off = soff;
        size_t mac_n = build_macalg_list (scratch + soff, scratch_cap - soff);
        g_return_val_if_fail (mac_n > 0, 0);
        soff += mac_n;
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_MAC_ALG, (guint16) mac_n, scratch + mac_off
        };

        /* HOPE app identification. The wire format is a fixed
		 * 4-byte OSType (not a length-prefixed string), so we must
		 * always emit exactly 4 bytes. Copy the caller's app_id
		 * into scratch with NUL padding so a short string (anything
		 * < 4 bytes) doesn't make hlpack_chunks read past the end
		 * of the caller's string literal. */
        const char *app_id = req->hope_app_id ? req->hope_app_id : "GTKx";
        size_t app_id_off = soff;
        g_return_val_if_fail (soff + 4 <= scratch_cap, 0);
        memset (scratch + soff, 0, 4);
        size_t app_id_len = strlen (app_id);
        if (app_id_len > 4) {
            app_id_len = 4;
        }
        memcpy (scratch + soff, app_id, app_id_len);
        soff += 4;
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_HOPE_APP_ID, 4, scratch + app_id_off
        };

        const char *app_string = req->hope_app_string ? req->hope_app_string
                                                       : "";
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_HOPE_APP_STRING,
            (guint16) strlen (app_string),
            app_string
        };

#ifdef CONFIG_CIPHER
        if (req->cipheralg && *req->cipheralg) {
            size_t cip_off = soff;
            size_t cip_n = build_single_alg_list (
                scratch + soff, scratch_cap - soff, req->cipheralg);
            if (cip_n) {
                soff += cip_n;
                chunks[hc++] = (struct hx_chunk) {
                    HTLC_DATA_CIPHER_ALG, (guint16) cip_n, scratch + cip_off
                };
            }
        }
#endif
#ifdef CONFIG_COMPRESS
        if (req->compressalg && *req->compressalg) {
            size_t comp_off = soff;
            size_t comp_n = build_single_alg_list (
                scratch + soff, scratch_cap - soff, req->compressalg);
            if (comp_n) {
                soff += comp_n;
                chunks[hc++] = (struct hx_chunk) {
                    HTLC_DATA_COMPRESS_ALG, (guint16) comp_n, scratch + comp_off
                };
            }
        }
#endif

        /* Empty SESSIONKEY — server fills it in the Step 1 reply. */
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_SESSIONKEY, 0, NULL
        };
    } else {
        /* Legacy mode. */

        /* ICON — always emit, even if 0. */
        size_t icon_off = soff;
        guint16 icon_be = htons (req->icon);
        memcpy (scratch + soff, &icon_be, 2);
        soff += 2;
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_ICON, 2, scratch + icon_off
        };

        /* LOGIN — always emit. Empty / NULL = zero-length chunk.
         * Non-empty = XOR-encoded login name (cap at 64 to match
         * production's previous inline behaviour). */
        size_t llen = req->login_name ? strlen (req->login_name) : 0;
        if (llen > 64) {
            llen = 64;
        }
        size_t login_off = soff;
        if (llen) {
            g_return_val_if_fail (soff + llen <= scratch_cap, 0);
            hl_encode (scratch + soff, req->login_name, llen);
            soff += llen;
        }
        chunks[hc++] = (struct hx_chunk) {
            HTLC_DATA_LOGIN, (guint16) llen,
            llen ? (scratch + login_off) : NULL
        };

        /* PASSWORD — emit only if set and non-empty. */
        if (req->password && *req->password) {
            size_t plen = strlen (req->password);
            if (plen > 64) {
                plen = 64;
            }
            size_t pass_off = soff;
            g_return_val_if_fail (soff + plen <= scratch_cap, 0);
            hl_encode (scratch + soff, req->password, plen);
            soff += plen;
            chunks[hc++] = (struct hx_chunk) {
                HTLC_DATA_PASSWORD, (guint16) plen, scratch + pass_off
            };
        }

        /* NAME — emit only if set. Production leaves this off
         * (sends NAME later via USER_CHANGE / AGREEMENTAGREE);
         * the test harness uses it for the inline shortcut so
         * test assertions can check "name we sent round-trips
         * back unchanged" without driving the full agreement
         * flow. */
        if (req->display_name && *req->display_name) {
            chunks[hc++] = (struct hx_chunk) {
                HTLC_DATA_NAME,
                (guint16) strlen (req->display_name),
                req->display_name
            };
        }

        /* CLIENTVERSION — emit only if explicitly set. */
        if (req->client_version) {
            size_t cv_off = soff;
            guint16 cv_be = htons (req->client_version);
            memcpy (scratch + soff, &cv_be, 2);
            soff += 2;
            chunks[hc++] = (struct hx_chunk) {
                HTLC_DATA_CLIENTVERSION, 2, scratch + cv_off
            };
        }

        /* CAPABILITIES — emit only when caller asks. caps==0 is a
         * valid advertisement ("I support the spec but have no
         * optional bits set") so we gate on send_caps, not the
         * value. Two-byte form covers bits 0..15, which is the
         * spec's "typical" width. */
        if (req->send_caps) {
            size_t caps_off = soff;
            guint16 caps_be = htons (req->caps);
            memcpy (scratch + soff, &caps_be, 2);
            soff += 2;
            chunks[hc++] = (struct hx_chunk) {
                HTLC_DATA_CAPABILITIES, 2, scratch + caps_off
            };
        }
    }

    g_assert (hc <= HX_LOGIN_MAX_CHUNKS);
    return hc;
}
