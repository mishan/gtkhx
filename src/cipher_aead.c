/*
 * cipher_aead.c — ChaCha20-Poly1305 AEAD primitives for the
 * fogWraith HOPE-ChaCha20-Poly1305.md extension.
 *
 * After Phase R1 this file is a thin C dispatcher; all
 * cryptographic computation lives in rust/crates/hxcrypto-aead.
 * The cipher_aead_* C symbols below preserve the legacy signature
 * so call sites in cipher.c, network_decode.c, htxf_io.c, and the
 * Tier 2 / Tier 3 tests don't need to change.
 *
 * The chacha_aead_state struct (declared in cipher.h) is
 * #[repr(C)]-matched against the Rust AeadState struct — the FFI
 * functions take pointers to that struct directly rather than
 * marshalling individual fields. The cipher.h _Static_assert and
 * the Rust crate's const _: () = assert!() block pin the layout
 * on both sides so a field reorder trips a build error rather
 * than a misalignment at decrypt time.
 *
 * Pure code; no Hotline protocol structs touched here. Callers in
 * cipher.c (control connection), network.c / network_decode.c
 * (read accumulator), and xfers.c (HTXF subchannel) drive the
 * integration.
 */

#include "config.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
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
    /* The full key-swap rationale (server-centric "encode_key_256"
     * vs client-centric encode_out, the bug it caused against Janus
     * when wired backwards) lives in the Rust impl now. Both peers
     * compute the same per-direction keys from the same inputs;
     * counters start at 0, dir bytes are stamped client-centric. */
    gtkhx_aead_derive_session_keys (encode_out, decode_out,
                                    session_key, session_key_len,
                                    encode_key, encode_key_len,
                                    decode_key, decode_key_len);

    /* One-shot debug dump of the derived keys, gated on the
     * "xfer-aead" debug category. Useful for off-line comparison
     * against another implementation when the on-wire MAC doesn't
     * verify — the inputs (session_key + the two HMAC keys) plus
     * the resulting AEAD keys are the full HKDF state. Keys
     * shouldn't normally be logged, but the category is opt-in
     * (GTKHX_DEBUG=xfer-aead). */
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
    /* Allocate locally (g_malloc) so the buffer is g_free-safe at
     * the call site, matching the legacy contract. The Rust
     * gtkhx_aead_seal_alloc returns a Box::into_raw'd pointer that
     * would need a paired Rust-side free; staying with g_malloc +
     * gtkhx_aead_seal keeps the ownership story uniform with the
     * non-_alloc seal path. */
    size_t framed_cap
        = CIPHER_AEAD_LENGTH_PREFIX + pt_len + CIPHER_AEAD_TAG_SIZE;
    uint8_t *framed = g_malloc (framed_cap);
    size_t n = gtkhx_aead_seal (state, plaintext, pt_len, framed, framed_cap);
    if (!n) {
        g_free (framed);
        return NULL;
    }
    if (out_len) {
        *out_len = n;
    }
    return framed;
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
