/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * hope.{c,h} — pure helpers for the HOPE-Secure-Login handshake.
 *
 * The fogWraith HOPE-Secure-Login.md spec defines a multi-step
 * authentication + algorithm negotiation flow:
 *
 *   Step 1: client → server LOGIN with empty password + algorithm
 *           lists (MAC, cipher, compress).
 *   Step 1 reply (server → client): session key + agreed algorithms.
 *   Step 2: client → server LOGIN with HMAC(password, sessionkey)
 *           + selected algorithm confirmations + capabilities.
 *   Step 2 reply (server → client): login result.
 *
 * The orchestration of this flow used to live entirely inside
 * src/rcv.c::rcv_task_login. That made the HOPE state machine
 * unreachable from the integration test harness — testing
 * ChaCha20-Poly1305 end-to-end would have meant either linking
 * rcv.c (which transitively reaches into GTK / libadwaita / the
 * signal bus) or duplicating ~500 LOC of spec-compliance
 * orchestration in the harness.
 *
 * This module pulls the pure parts out of rcv_task_login:
 *
 *   - chunk walking + algorithm-name extraction from a Step 1 reply
 *   - HMAC chain computation (password_mac → encode_key → decode_key)
 *   - session-key IP:port validation
 *   - LOGIN field encoding for Step 2 (HMAC variant for secure_login,
 *     XOR variant otherwise)
 *   - reply-side single-entry algorithm-list construction
 *   - cipher-name → cipher-id mapping
 *   - "does this cipher imply AEAD framing" predicate
 *
 * Out of scope (stays in rcv.c):
 *
 *   - hlwrite of the outgoing Step 2 LOGIN packet (orchestration
 *     of the assembled fields)
 *   - cipher_encode_init / cipher_decode_init (init runs after the
 *     mapping into htlc->cipher_*_state, which is caller's choice)
 *   - hx_printf_prefix calls for warnings / info messages
 *   - hx_htlc_close on hard validation failures
 *   - task_new for the Step 2 reply callback
 *   - UI side effects (play_sound, changetitlesconnected, etc.)
 *
 * The module includes only glib.h and the protocol headers — no
 * GTK, no Adwaita, no GtkhxSession signals — so the harness can
 * link it directly.
 */

#ifndef __hope_h
#define __hope_h

#include "config.h"
#include <glib.h>
#include <stddef.h>
#include <stdint.h>

struct htlc_conn;

/* Maximum HMAC output we accept anywhere in the chain. HMAC-SHA256
 * produces 32 bytes; older algos (HMAC-SHA1 = 20, HMAC-MD5 = 16)
 * are smaller. We pad to 64 to leave room for hypothetical SHA-512-
 * family algorithms without redefining buffer widths. */
#define HOPE_MAC_MAX 64

/* Maximum length of an algorithm name on the wire (e.g.
 * "CHACHA20-POLY1305" is 17 bytes, "HMAC-SHA256" is 11). The
 * spec doesn't pin an upper bound; 31+NUL gives us comfortable
 * headroom. */
#define HOPE_ALG_NAME_MAX 32

/* ---- Step 1 reply parsing ---------------------------------------- */

/*
 * Algorithms the server selected (or echoed back) in its HOPE
 * Step 1 TASK reply.
 *
 * Each `*alg` field is a NUL-terminated ASCII algorithm name lifted
 * from the first entry of the corresponding HOPE algorithm-list
 * chunk on the wire. Empty string means "chunk not present" — the
 * caller decides whether that's an error (e.g. no MAC means HOPE
 * isn't usable at all) or a downgrade signal (e.g. no compression
 * means the connection runs uncompressed).
 *
 * The 's_' / 'c_' prefix names the direction the algorithm
 * applies to on the wire:
 *
 *   s_cipheralg  - server-outbound cipher (server encodes with it,
 *                  client decodes with it)
 *   c_cipheralg  - client-outbound cipher (mirror of the above)
 *
 * Both sides MAY be different in theory; in practice mhxd / Mobius
 * / Janus all match them.
 *
 * `server_says_aead` reflects HTLS_DATA_CIPHER_MODE = "AEAD" in
 * the reply. Used by the ChaCha20-Poly1305 path to confirm AEAD
 * framing.
 *
 * `secure_login` is set when the server's HTLS_DATA_LOGIN chunk
 * names a MAC algorithm matching `expected_macalg` (the value the
 * caller passed in). This signals the server is offering an
 * HMAC-based login challenge — caller should HMAC the login name
 * with the session key rather than XOR-encoding it. */
struct hope_step1_reply {
    char macalg[HOPE_ALG_NAME_MAX];
    char s_cipheralg[HOPE_ALG_NAME_MAX];
    char c_cipheralg[HOPE_ALG_NAME_MAX];
    char s_compressalg[HOPE_ALG_NAME_MAX];
    char c_compressalg[HOPE_ALG_NAME_MAX];
    int server_says_aead;
    int secure_login;
};

enum hope_step1_err {
    HOPE_OK = 0,
    HOPE_ERR_NO_MAC_ALG,
    HOPE_ERR_BAD_MAC_ALG,       /* zero-length name in list */
    HOPE_ERR_SHORT_SESSIONKEY,  /* < 20 bytes; HOPE spec mandates 64 */
    HOPE_ERR_OVERSIZED          /* malformed length-prefixed lists */
};

/*
 * Walk the chunks of a Step 1 TASK reply currently buffered in
 * htlc->in. Extracts the negotiated algorithm names and stores
 * the session key into htlc->sessionkey / htlc->sklen (production
 * storage; matches the legacy in-place population that rcv_task_login
 * has always done).
 *
 * `expected_macalg` is the client's preferred MAC algorithm
 * (typically what was advertised in the Step 1 outbound LOGIN).
 * Used to gate the secure_login probe. Pass NULL or "" to skip
 * the probe; reply->secure_login will be 0.
 *
 * Returns HOPE_OK on a clean parse, or an error code on validation
 * failure. On error, *reply may be partially populated (algos
 * before the offending chunk land in there); session key bytes
 * before the validation failure also land in htlc->sessionkey,
 * but htlc->sklen is only updated on a clean parse.
 *
 * Cipher / compress / checksum algorithms are not validated by
 * this function — the caller decides whether "" is acceptable
 * (legacy fallback) or fatal (cipher pinned in bookmark). Call
 * valid_cipher() / valid_compress() from connect.c on the
 * extracted names if a hard check is needed.
 */
extern enum hope_step1_err
hope_parse_step1_reply (struct htlc_conn *htlc,
                        const char *expected_macalg,
                        struct hope_step1_reply *reply);

/* ---- HMAC chain ------------------------------------------------- */

/*
 * Compute the HOPE password-derived MAC chain in spec order:
 *
 *   password_mac = HMAC(key=password_bytes, msg=session_key)
 *   encode_key   = HMAC(key=password_bytes, msg=password_mac)
 *   decode_key   = HMAC(key=password_bytes, msg=encode_key)
 *
 * `pass` is the user-typed password (NUL-terminated, raw bytes —
 * not pre-hashed). `sessionkey` is the raw 64-byte session key
 * the server delivered in its HOPE Step 1 reply.
 *
 * IMPORTANT — labels:
 *
 * GtkHx historically labels its storage REVERSED from the spec:
 * `htlc->cipher_decode_key` holds the spec's encode_key (first-
 * derived from password_mac), and `htlc->cipher_encode_key`
 * holds the spec's decode_key (second-derived from encode_key).
 * For the legacy stream ciphers (RC4 / Blowfish OFB) this swap
 * was harmless — both peers compute the same byte values and
 * the labeling is convention only. For ChaCha20-Poly1305 AEAD,
 * the spec's labels matter because HKDF info strings are bound
 * to them. The CHAIN here outputs spec-aligned values; caller is
 * responsible for mapping into the legacy storage layout if it
 * needs to (rcv.c does so for its own state).
 *
 * Returns the size of one MAC output (16 for HMAC-MD5, 20 for
 * HMAC-SHA1, 32 for HMAC-SHA256), or 0 on bad macalg. All three
 * output buffers are filled with the same width. Caller-supplied
 * buffers must be at least HOPE_MAC_MAX bytes each.
 */
extern size_t
hope_compute_chain (const char *pass,
                    const uint8_t *sessionkey, size_t sklen,
                    const char *macalg,
                    uint8_t password_mac[HOPE_MAC_MAX],
                    uint8_t encode_key[HOPE_MAC_MAX],
                    uint8_t decode_key[HOPE_MAC_MAX]);

/* ---- LOGIN field encoding -------------------------------------- */

/*
 * Compute the HTLC_DATA_LOGIN field for Step 2 LOGIN.
 *
 * When `secure_login` is set, the field is HMAC(login_name,
 * sessionkey) with `macalg` as the hash. Otherwise the field
 * is the hl_code XOR-with-0xff "encoding" of the raw bytes.
 *
 * Returns the number of bytes written into out_buf:
 *   - HMAC variant: width of the MAC output (16/20/32)
 *   - XOR variant: strlen(login_name)
 *   - 0 on out_cap overflow or bad macalg
 *
 * out_buf is NOT NUL-terminated.
 */
extern size_t
hope_build_login_field (const char *login_name,
                        int secure_login,
                        const uint8_t *sessionkey, size_t sklen,
                        const char *macalg,
                        uint8_t *out_buf, size_t out_cap);

/* ---- Session-key IP:port validation ---------------------------- */

/*
 * The first 6 bytes of a HOPE session key encode the server's
 * idea of (its IP, its port). Clients are expected to compare
 * against what they actually connected to and flag any
 * mismatch (NAT, transparent proxy, MITM).
 *
 * Returns NULL when the IP:port matches (or sklen < 6, i.e. the
 * server didn't include the prefix at all). Otherwise writes a
 * "WARNING: ..." line into out_warning (NUL-terminated) and
 * returns out_warning. Caller chooses to log + continue or
 * disconnect; GtkHx's current policy is log + continue, matching
 * shxd-family clients run behind home-NAT setups.
 */
extern const char *
hope_validate_sessionkey_ip (const uint8_t *sessionkey, size_t sklen,
                             const char *connected_ip,
                             uint16_t connected_port,
                             char *out_warning, size_t out_cap);

/* ---- Reply-list builder ---------------------------------------- */

/*
 * Build a 1-entry algorithm-list chunk payload (HOPE list format):
 *
 *   bytes 0..1   count (BE u16) = 1
 *   byte  2      name length
 *   bytes 3..    name
 *
 * Used for HTLS_DATA_CIPHER_ALG and HTLS_DATA_COMPRESS_ALG chunks
 * in the outgoing Step 2 LOGIN, where we confirm the algorithm
 * we picked back to the server.
 *
 * Returns total bytes written, or 0 if `alg` is empty or doesn't
 * fit in out_cap.
 */
extern size_t
hope_build_alg_reply (const char *alg, uint8_t *out_buf, size_t out_cap);

/* ---- Cipher / compression name → id mapping -------------------- */

/*
 * Maps the wire-name of a cipher algorithm to the CIPHER_* enum
 * value used in htlc->cipher_*_type. Returns CIPHER_NONE for
 * empty / unknown names.
 *
 * Recognised today: "RC4", "BLOWFISH", "CHACHA20-POLY1305".
 *
 * The return type is `int` rather than the cipher enum because
 * cipher.h's enum-via-#define style doesn't lend itself to a
 * typedef; numeric ids match the CIPHER_* macros 1:1.
 */
extern int hope_cipher_id_from_name (const char *name);

/*
 * Does this cipher name imply AEAD framing (length-prefixed
 * sealed transactions) on the wire?
 *
 * Currently true only for "CHACHA20-POLY1305". Stream ciphers
 * (RC4, Blowfish OFB) return 0.
 */
extern int hope_cipher_is_aead (const char *name);

#endif /* __hope_h */
