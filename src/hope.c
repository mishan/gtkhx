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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <glib.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "algo_list.h"
#include "hope.h"

/* ---- Step 1 reply parsing ------------------------------------- */

/* Copy first entry of a HOPE algorithm list into a NUL-terminated
 * caller buffer. Returns 1 on success, 0 if the list is malformed
 * (zero-length entry, count == 0, etc.). */
static int
pick_first_alg (const guint8 *list, guint16 list_len,
                char *out, size_t out_cap)
{
    if (!list || !list_len || !out_cap) {
        return 0;
    }
    guint8 *p = list_n ((guint8 *) list, list_len, 0);
    if (!p || !*p) {
        return 0;
    }
    size_t name_len = *p;
    if (name_len >= out_cap) {
        name_len = out_cap - 1;
    }
    memcpy (out, p + 1, name_len);
    out[name_len] = '\0';
    return 1;
}

enum hope_step1_err
hope_parse_step1_reply (struct htlc_conn *htlc,
                        const char *expected_macalg,
                        struct hope_step1_reply *reply)
{
    g_return_val_if_fail (htlc != NULL, HOPE_ERR_NULL_ARG);
    g_return_val_if_fail (reply != NULL, HOPE_ERR_NULL_ARG);
    memset (reply, 0, sizeof (*reply));

    /* These hold pointers + lengths into the in-buffer chunks; we
	 * walk the algo lists with list_n AFTER the dh_start walk
	 * completes, so we capture the (ptr, len) pairs first and
	 * resolve names after. The pointers are valid for the
	 * duration of htlc->in.buf — caller must not free the in
	 * buffer between dh_start and the post-walk processing. */
    const guint8 *mal = NULL;
    guint16 mal_len = 0;
    const guint8 *s_cal = NULL, *c_cal = NULL;
    guint16 s_cal_len = 0, c_cal_len = 0;
    const guint8 *s_compl = NULL, *c_compl = NULL;
    guint16 s_compl_len = 0, c_compl_len = 0;
    guint16 sklen_out = 0;
    size_t expected_macalg_len
        = expected_macalg ? strlen (expected_macalg) : 0;

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_LOGIN:
            /* Server probes for secure-login by echoing our MAC
			 * algorithm name in the LOGIN chunk of its reply. We
			 * match against the caller's expected macalg. */
            if (expected_macalg_len && _len == expected_macalg_len
                && !memcmp (expected_macalg, dh->data, _len)) {
                reply->secure_login = 1;
            }
            break;
        case HTLS_DATA_PASSWORD:
            /* Detection hook for future "server proposes a different
			 * MAC for the password chain" handling — unused today. */
            break;
        case HTLS_DATA_MAC_ALG:
            mal = dh->data;
            mal_len = _len;
            break;
#ifdef CONFIG_CIPHER
        case HTLS_DATA_CIPHER_ALG:
            s_cal = dh->data;
            s_cal_len = _len;
            break;
        case HTLC_DATA_CIPHER_ALG:
            c_cal = dh->data;
            c_cal_len = _len;
            break;
        case HTLS_DATA_CIPHER_MODE:
        case HTLC_DATA_CIPHER_MODE:
            if (_len == 4 && !memcmp (dh->data, "AEAD", 4)) {
                reply->server_says_aead = 1;
            }
            break;
        case HTLS_DATA_CIPHER_IVEC:
        case HTLC_DATA_CIPHER_IVEC:
            /* Reserved for explicit-IV cipher modes; HOPE doesn't
			 * use them today. */
            break;
#endif
#ifdef CONFIG_COMPRESS
        case HTLS_DATA_COMPRESS_ALG:
            s_compl = dh->data;
            s_compl_len = _len;
            break;
        case HTLC_DATA_COMPRESS_ALG:
            c_compl = dh->data;
            c_compl_len = _len;
            break;
#endif
        case HTLS_DATA_CHECKSUM_ALG:
        case HTLC_DATA_CHECKSUM_ALG:
            /* HOPE pre-defined for future checksum-only modes;
			 * AEAD ciphers carry their own MAC, stream ciphers
			 * use a separate CHECKSUM field. Neither path needs
			 * client-side parsing today. */
            break;
        case HTLS_DATA_SESSIONKEY: {
            guint16 want = _len > sizeof (htlc->sessionkey)
                               ? (guint16) sizeof (htlc->sessionkey)
                               : _len;
            memcpy (htlc->sessionkey, dh->data, want);
            sklen_out = want;
            break;
        }
        }
    }
    dh_end ();

    /* MAC algorithm is mandatory — without it we can't compute the
	 * password chain or any HOPE auth. */
    if (!mal_len) {
        return HOPE_ERR_NO_MAC_ALG;
    }
    if (!pick_first_alg (mal, mal_len, reply->macalg, sizeof (reply->macalg))) {
        return HOPE_ERR_BAD_MAC_ALG;
    }

    /* Session key must be at least 20 bytes to give the HMAC-SHA1
	 * chain a chance; HOPE spec mandates 64 bytes, but we tolerate
	 * shorter on the off chance a non-compliant server returns a
	 * truncated one. Less than 20 → can't proceed. */
    if (sklen_out < 20) {
        return HOPE_ERR_SHORT_SESSIONKEY;
    }
    htlc->sklen = sklen_out;

    /* Cipher / compress algorithm name extraction is best-effort —
	 * server may have negotiated "no cipher" by omitting the
	 * chunks. Leave the corresponding reply field empty if so. */
#ifdef CONFIG_CIPHER
    if (s_cal_len) {
        pick_first_alg (s_cal, s_cal_len, reply->s_cipheralg,
                        sizeof (reply->s_cipheralg));
    }
    if (c_cal_len) {
        pick_first_alg (c_cal, c_cal_len, reply->c_cipheralg,
                        sizeof (reply->c_cipheralg));
    }
#else
    (void) s_cal;
    (void) c_cal;
    (void) s_cal_len;
    (void) c_cal_len;
#endif
#ifdef CONFIG_COMPRESS
    if (s_compl_len) {
        pick_first_alg (s_compl, s_compl_len, reply->s_compressalg,
                        sizeof (reply->s_compressalg));
    }
    if (c_compl_len) {
        pick_first_alg (c_compl, c_compl_len, reply->c_compressalg,
                        sizeof (reply->c_compressalg));
    }
#else
    (void) s_compl;
    (void) c_compl;
    (void) s_compl_len;
    (void) c_compl_len;
#endif

    return HOPE_OK;
}

/* ---- HMAC chain ------------------------------------------------- */

size_t
hope_compute_chain (const char *pass,
                    const uint8_t *sessionkey, size_t sklen,
                    const char *macalg,
                    uint8_t password_mac[HOPE_MAC_MAX],
                    uint8_t encode_key[HOPE_MAC_MAX],
                    uint8_t decode_key[HOPE_MAC_MAX])
{
    if (!pass || !sessionkey || !macalg) {
        return 0;
    }
    size_t pass_len = strlen (pass);

    /* Step 1: password_mac = HMAC(pass, sessionkey) */
    guint16 n = hmac_xxx (password_mac, pass, pass_len,
                          sessionkey, (guint32) sklen, macalg);
    if (!n) {
        return 0;
    }
    /* Step 2: encode_key = HMAC(pass, password_mac) — spec encode */
    guint16 n2 = hmac_xxx (encode_key, pass, pass_len,
                           password_mac, n, macalg);
    if (n2 != n) {
        return 0;
    }
    /* Step 3: decode_key = HMAC(pass, encode_key) — spec decode */
    guint16 n3 = hmac_xxx (decode_key, pass, pass_len,
                           encode_key, n, macalg);
    if (n3 != n) {
        return 0;
    }
    return n;
}

/* ---- LOGIN field encoding -------------------------------------- */

size_t
hope_build_login_field (const char *login_name,
                        int secure_login,
                        const uint8_t *sessionkey, size_t sklen,
                        const char *macalg,
                        uint8_t *out_buf, size_t out_cap)
{
    if (!login_name || !out_buf || !out_cap) {
        return 0;
    }
    size_t login_len = strlen (login_name);

    if (secure_login) {
        /* HMAC variant: HMAC(login_name, sessionkey) under macalg.
		 * Output width is the MAC digest size; clip against out_cap
		 * so a misconfigured caller can't overflow.
		 *
		 * Reject the call rather than crash if the HMAC inputs are
		 * missing — hmac_xxx would dereference sessionkey/macalg
		 * without checking. 0-byte sklen is treated as missing too;
		 * an empty session key can't produce a meaningful MAC even
		 * for the legal "0-byte key" HMAC edge case (the server
		 * always sends a real sessionkey when secure_login is in
		 * play). */
        if (!sessionkey || !sklen || !macalg) {
            return 0;
        }
        guint8 mac[HOPE_MAC_MAX];
        guint16 n = hmac_xxx (mac, login_name, login_len,
                              sessionkey, (guint32) sklen, macalg);
        if (!n || n > out_cap) {
            return 0;
        }
        memcpy (out_buf, mac, n);
        return n;
    }

    /* XOR variant: bytes ^ 0xff. */
    if (login_len > out_cap) {
        return 0;
    }
    hl_encode (out_buf, login_name, login_len);
    return login_len;
}

/* ---- Session-key IP:port validation ---------------------------- */

const char *
hope_validate_sessionkey_ip (const uint8_t *sessionkey, size_t sklen,
                             const char *connected_ip,
                             uint16_t connected_port,
                             char *out_warning, size_t out_cap)
{
    if (sklen < 6 || !connected_ip || !out_warning || !out_cap) {
        return NULL;
    }

    /* IP at offset 0..3 (network-order octets), port at 4..5 (BE). */
    guint16 key_port_be;
    memcpy (&key_port_be, &sessionkey[4], 2);
    guint16 key_port = ntohs (key_port_be);

    char key_ip[INET_ADDRSTRLEN];
    g_snprintf (key_ip, sizeof key_ip, "%u.%u.%u.%u",
                sessionkey[0], sessionkey[1], sessionkey[2], sessionkey[3]);

    if (strcmp (key_ip, connected_ip) == 0 && key_port == connected_port) {
        return NULL;
    }

    g_snprintf (out_warning, out_cap,
                "WARNING: HOPE sessionkey IP:port (%s:%u) doesn't match "
                "connected server (%s:%u) — possible NAT or MITM. "
                "Continuing anyway.\n",
                key_ip, key_port, connected_ip, connected_port);
    return out_warning;
}

/* ---- Reply-list builder ---------------------------------------- */

size_t
hope_build_alg_reply (const char *alg, uint8_t *out_buf, size_t out_cap)
{
    if (!alg || !out_buf) {
        return 0;
    }
    size_t name_len = strlen (alg);
    if (!name_len || name_len > 255) {
        return 0;
    }
    /* 2-byte count + 1-byte len + name. */
    size_t total = 2 + 1 + name_len;
    if (total > out_cap) {
        return 0;
    }
    /* Count = 1 (network order). */
    guint16 one = htons (1);
    memcpy (out_buf, &one, 2);
    out_buf[2] = (guint8) name_len;
    memcpy (out_buf + 3, alg, name_len);
    return total;
}

/* ---- Cipher name lookups --------------------------------------- */

int
hope_cipher_id_from_name (const char *name)
{
#ifdef CONFIG_CIPHER
    if (!name || !*name) {
        return CIPHER_NONE;
    }
    if (!strcmp (name, "RC4")) {
        return CIPHER_RC4;
    }
    if (!strcmp (name, "BLOWFISH")) {
        return CIPHER_BLOWFISH;
    }
    if (!strcmp (name, "CHACHA20-POLY1305")) {
        return CIPHER_CHACHA20_POLY1305;
    }
    return CIPHER_NONE;
#else
    (void) name;
    return 0;
#endif
}

int
hope_cipher_is_aead (const char *name)
{
    return name && !strcmp (name, "CHACHA20-POLY1305");
}
