/*
 * cipher_aead.c — ChaCha20-Poly1305 AEAD primitives for the
 * fogWraith HOPE-ChaCha20-Poly1305.md extension.
 *
 * All cryptographic computation is now in Rust
 * (rust/crates/hxcrypto-aead/). This file is a thin C wrapper
 * that preserves the existing API for callers in cipher.c,
 * network.c, and xfers.c.
 */

#include "config.h"

#include <string.h>
#include <stdint.h>
#include <glib.h>

#include "cipher_aead.h"
#include "debug.h"

/* ---- Rust FFI declarations (hxcrypto-aead) ---- */

extern void gtkhx_aead_hkdf_sha256 (const uint8_t *salt, size_t salt_len,
                                     const uint8_t *ikm, size_t ikm_len,
                                     const uint8_t *info, size_t info_len,
                                     uint8_t *out, size_t out_len);

extern void gtkhx_aead_derive_session_keys (chacha_aead_state *encode_out,
                                            chacha_aead_state *decode_out,
                                            const uint8_t *session_key,
                                            size_t session_key_len,
                                            const uint8_t *encode_key,
                                            size_t encode_key_len,
                                            const uint8_t *decode_key,
                                            size_t decode_key_len);

extern void gtkhx_aead_derive_transfer_keys (chacha_aead_state *xfer_encode_out,
                                             chacha_aead_state *xfer_decode_out,
                                             const uint8_t *session_key,
                                             size_t session_key_len,
                                             const chacha_aead_state *ctrl_encode,
                                             const chacha_aead_state *ctrl_decode,
                                             uint32_t ref_num);

extern size_t gtkhx_aead_seal (chacha_aead_state *state,
                               const uint8_t *plaintext, size_t pt_len,
                               uint8_t *out, size_t out_cap);

extern size_t gtkhx_aead_peek_frame_size (const uint8_t *framed,
                                          size_t framed_len);

extern size_t gtkhx_aead_open (chacha_aead_state *state,
                               const uint8_t *framed, size_t framed_len,
                               uint8_t *out, size_t out_cap);

extern uint8_t *gtkhx_aead_seal_alloc (chacha_aead_state *state,
                                       const uint8_t *plaintext, size_t pt_len,
                                       size_t *out_len);

extern void gtkhx_aead_seal_alloc_free (uint8_t *ptr, size_t len);

/* ---- HKDF-SHA256 ----------------------------------------------- */

void
cipher_aead_hkdf_sha256 (const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len,
                         const uint8_t *info, size_t info_len,
                         uint8_t *out, size_t out_len)
{
    gtkhx_aead_hkdf_sha256 (salt, salt_len, ikm, ikm_len,
                            info, info_len, out, out_len);
}

/* ---- Key derivation ------------------------------------------- */

void
cipher_aead_derive_session_keys (
    chacha_aead_state *encode_out, chacha_aead_state *decode_out,
    const uint8_t *session_key, size_t session_key_len,
    const uint8_t *encode_key,  size_t encode_key_len,
    const uint8_t *decode_key,  size_t decode_key_len)
{
    gtkhx_aead_derive_session_keys (encode_out, decode_out,
                                    session_key, session_key_len,
                                    encode_key, encode_key_len,
                                    decode_key, decode_key_len);

    debug_log ("xfer-aead", "session keys derived: "
               "encode_key=%02x%02x%02x%02x... decode_key=%02x%02x%02x%02x...",
               encode_out->key[0], encode_out->key[1],
               encode_out->key[2], encode_out->key[3],
               decode_out->key[0], decode_out->key[1],
               decode_out->key[2], decode_out->key[3]);
}

void
cipher_aead_derive_transfer_keys (
    chacha_aead_state *xfer_encode_out,
    chacha_aead_state *xfer_decode_out,
    const uint8_t *session_key, size_t session_key_len,
    const chacha_aead_state *ctrl_encode,
    const chacha_aead_state *ctrl_decode,
    uint32_t ref)
{
    gtkhx_aead_derive_transfer_keys (xfer_encode_out, xfer_decode_out,
                                     session_key, session_key_len,
                                     ctrl_encode, ctrl_decode, ref);
}

/* ---- Frame codec ---------------------------------------------- */

size_t
cipher_aead_seal (chacha_aead_state *state,
                  const uint8_t *plaintext, size_t pt_len,
                  uint8_t *out, size_t out_cap)
{
    return gtkhx_aead_seal (state, plaintext, pt_len, out, out_cap);
}

uint8_t *
cipher_aead_seal_alloc (chacha_aead_state *state,
                        const uint8_t *plaintext, size_t pt_len,
                        size_t *out_len)
{
    /* The Rust allocator returns memory that must be freed with
     * gtkhx_aead_seal_alloc_free. To keep the caller contract
     * (g_free) we copy into a GLib buffer. */
    size_t len = 0;
    uint8_t *rust_buf = gtkhx_aead_seal_alloc (state, plaintext, pt_len, &len);
    if (!rust_buf) {
        return NULL;
    }
    uint8_t *glib_buf = g_malloc (len);
    memcpy (glib_buf, rust_buf, len);
    gtkhx_aead_seal_alloc_free (rust_buf, len);
    if (out_len) {
        *out_len = len;
    }
    return glib_buf;
}

size_t
cipher_aead_peek_frame_size (const uint8_t *framed, size_t framed_len)
{
    return gtkhx_aead_peek_frame_size (framed, framed_len);
}

size_t
cipher_aead_open (chacha_aead_state *state,
                  const uint8_t *framed, size_t framed_len,
                  uint8_t *out, size_t out_cap)
{
    return gtkhx_aead_open (state, framed, framed_len, out, out_cap);
}
