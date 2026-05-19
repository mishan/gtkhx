/*
 * cipher_aead.h — ChaCha20-Poly1305 AEAD helpers for the
 * fogWraith HOPE-ChaCha20-Poly1305.md extension.
 *
 * Three things live here:
 *
 *   1. HKDF-SHA256 wrapper          — stretch MAC-derived key
 *                                     material to 256 bits
 *   2. Session / transfer key
 *      derivation                   — turn the HOPE encode/decode
 *                                     keys into 32-byte AEAD keys,
 *                                     and the HTXF ref number into
 *                                     a per-transfer key
 *   3. Seal / Open frame codec      — length-prefixed
 *                                     ChaCha20-Poly1305 framing
 *
 * Pure functions; no GTK, no protocol structs. Callers in
 * cipher.c, network.c, and xfers.c drive integration.
 *
 * Header is safe to include from any source file that needs the
 * AEAD primitives. Pulls in cipher.h for chacha_aead_state but
 * NOT protocol.h / hx.h, so the test fixtures can exercise the
 * codec without the GUI build.
 */

#ifndef __cipher_aead_h
#define __cipher_aead_h

#include "config.h"

#ifdef CONFIG_CIPHER

#include <stdint.h>
#include <stddef.h>
#include "cipher.h"

/* Maximum AEAD frame size on the wire (ciphertext + tag). The
 * spec mandates a 16 MiB cap to prevent memory exhaustion via
 * malformed length prefixes. The encode path errors on plaintext
 * bigger than this minus the 16-byte tag; the decode path errors
 * on length prefixes that exceed the cap. */
#define CIPHER_AEAD_MAX_FRAME_SIZE (16 * 1024 * 1024)

/* ChaCha20-Poly1305 Poly1305 tag length in bytes. */
#define CIPHER_AEAD_TAG_SIZE 16

/* Length prefix in front of each AEAD frame (big-endian u32). */
#define CIPHER_AEAD_LENGTH_PREFIX 4

/* Nonce direction byte values. dir is constant per direction over
 * the lifetime of the connection. */
#define CIPHER_AEAD_DIR_SERVER_TO_CLIENT 0x00
#define CIPHER_AEAD_DIR_CLIENT_TO_SERVER 0x01

/* ---- HKDF-SHA256 ----------------------------------------------- *
 *
 * One-shot HKDF: extract a 32-byte PRK from (salt, ikm), then
 * expand to `out_len` bytes guided by `info`. Wraps Nettle's
 * hkdf_extract / hkdf_expand with HMAC-SHA256 as the underlying
 * MAC. The Hotline AEAD spec always uses SHA-256 here regardless
 * of the negotiated MAC algorithm (the negotiated MAC is used
 * only for the password / key-confirmation derivation; HKDF for
 * AEAD always uses SHA-256). */
void cipher_aead_hkdf_sha256 (const uint8_t *salt, size_t salt_len,
                              const uint8_t *ikm, size_t ikm_len,
                              const uint8_t *info, size_t info_len,
                              uint8_t *out, size_t out_len);

/* ---- Key derivation ------------------------------------------- *
 *
 * Per the spec:
 *
 *   encode_key_256 = HKDF-SHA256(ikm=encode_key, salt=session_key,
 *                                info="hope-chacha-encode")
 *   decode_key_256 = HKDF-SHA256(ikm=decode_key, salt=session_key,
 *                                info="hope-chacha-decode")
 *
 * Populates the two chacha_aead_state structs with the expanded
 * keys, sets dir to the appropriate constants (we're a CLIENT —
 * encode = client→server, decode = server→client), and zeros the
 * counters.
 *
 * `session_key` is the 64-byte session key from the HOPE Step 2
 * reply. `encode_key` / `decode_key` are the MAC-derived keys
 * computed in rcv_task_login (the same byte buffers the stream
 * ciphers used to take as their key). */
void cipher_aead_derive_session_keys (
    chacha_aead_state *encode_out, chacha_aead_state *decode_out,
    const uint8_t *session_key, size_t session_key_len,
    const uint8_t *encode_key,  size_t encode_key_len,
    const uint8_t *decode_key,  size_t decode_key_len);

/* HTXF per-transfer key derivation:
 *
 *   ft_base_key  = HKDF-SHA256(ikm=encode_key_256 || decode_key_256,
 *                              salt=session_key,
 *                              info="hope-file-transfer")
 *   transfer_key = HKDF-SHA256(ikm=ft_base_key,
 *                              salt=ref_be (4 bytes),
 *                              info="hope-ft-ref")
 *
 * Populates two chacha_aead_state structs (one per direction) for
 * the file-transfer connection. The transfer_key is symmetric —
 * server and client compute the same value — and counters are
 * reset to 0 per transfer.
 *
 * `ref` is the 32-bit HTXF reference number; it gets serialised
 * big-endian inside this function. */
void cipher_aead_derive_transfer_keys (
    chacha_aead_state *xfer_encode_out,
    chacha_aead_state *xfer_decode_out,
    const uint8_t *session_key, size_t session_key_len,
    const chacha_aead_state *ctrl_encode,
    const chacha_aead_state *ctrl_decode,
    uint32_t ref);

/* ---- Frame codec ---------------------------------------------- *
 *
 * Seal: encrypt+authenticate `plaintext` (length `pt_len`) into
 * `out`. Writes 4-byte big-endian length prefix + ciphertext +
 * 16-byte Poly1305 tag. Out buffer must hold at least
 *     CIPHER_AEAD_LENGTH_PREFIX + pt_len + CIPHER_AEAD_TAG_SIZE
 * bytes. Advances state->counter on success. Returns the total
 * bytes written (length prefix + ciphertext + tag) on success,
 * or 0 on error (oversized plaintext).
 *
 * The 12-byte nonce is built from state->dir + state->counter
 * before the encrypt; no two encrypts ever use the same nonce
 * because the counter is monotonic. */
size_t cipher_aead_seal (chacha_aead_state *state,
                         const uint8_t *plaintext, size_t pt_len,
                         uint8_t *out, size_t out_cap);

/* Open: given a candidate framed buffer, verify its Poly1305 tag
 * and emit the plaintext.
 *
 *   `framed`     points at the start of the length-prefixed frame
 *   `framed_len` is at least CIPHER_AEAD_LENGTH_PREFIX + tag — the
 *                length prefix is read out and bounds-checked
 *                against this and against CIPHER_AEAD_MAX_FRAME_SIZE
 *   `out`        receives the plaintext (no length prefix, no tag)
 *   `out_cap`    must be at least the ciphertext payload size
 *                (length prefix value minus tag size)
 *
 * Returns the plaintext length on success and advances
 * state->counter. Returns 0 (and does NOT advance the counter)
 * on:
 *   - short / malformed framed input
 *   - oversized length prefix
 *   - failed Poly1305 verification (tampered or wrong key)
 *   - out buffer too small
 *
 * Callers MUST distinguish between "frame not yet fully buffered"
 * (handle separately by checking the length prefix vs available
 * bytes BEFORE calling open) and "open failed" (return 0 here).
 * cipher_aead_peek_frame_size below is the helper for the former. */
size_t cipher_aead_open (chacha_aead_state *state,
                         const uint8_t *framed, size_t framed_len,
                         uint8_t *out, size_t out_cap);

/* Peek at the length prefix of a candidate framed buffer without
 * decrypting. Returns the total frame size in bytes
 *     (CIPHER_AEAD_LENGTH_PREFIX + ciphertext + tag)
 * if the prefix is present and valid (<= MAX_FRAME_SIZE), or 0
 * if `framed_len` is less than CIPHER_AEAD_LENGTH_PREFIX or the
 * prefix is oversized. Use this from the receive accumulator to
 * decide whether enough bytes have arrived to call open(). */
size_t cipher_aead_peek_frame_size (const uint8_t *framed,
                                    size_t framed_len);

#endif /* CONFIG_CIPHER */

#endif /* __cipher_aead_h */
