/*
 * cipher_aead.c — ChaCha20-Poly1305 AEAD primitives for the
 * fogWraith HOPE-ChaCha20-Poly1305.md extension.
 *
 * Implements:
 *   - HKDF-SHA256 one-shot (extract + expand)
 *   - Session key derivation: stretch MAC-derived keys to 32 bytes
 *   - HTXF transfer key derivation: per-ref subchannel keys
 *   - Seal / Open frame codec: length-prefixed AEAD frames with
 *     deterministic nonce construction
 *
 * Pure code; no Hotline protocol structs touched here. Callers
 * in cipher.c (control connection), network.c (read accumulator),
 * and xfers.c (HTXF subchannel) drive the integration.
 *
 * Crypto comes from Nettle:
 *   - nettle/hmac-sha2.h     — HMAC-SHA256 for HKDF
 *   - nettle/hkdf.h          — RFC 5869 HKDF
 *   - nettle/chacha-poly1305.h — RFC 8439 ChaCha20-Poly1305
 *   - nettle/macros.h        — WRITE_UINT32 etc.
 */

#include "config.h"

#ifdef CONFIG_CIPHER

#include <string.h>
#include <nettle/hmac.h>
#include <nettle/hkdf.h>
#include <nettle/sha2.h>
#include <nettle/chacha-poly1305.h>

#include "cipher_aead.h"
#include "debug.h"

/* ---- HKDF-SHA256 ----------------------------------------------- */

void
cipher_aead_hkdf_sha256 (const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len,
                         const uint8_t *info, size_t info_len,
                         uint8_t *out, size_t out_len)
{
    struct hmac_sha256_ctx ctx;
    uint8_t prk[SHA256_DIGEST_SIZE];

    /* HKDF-Extract: PRK = HMAC-SHA256(salt, ikm). Nettle's
	 * hkdf_extract takes a pre-keyed MAC context — load the salt
	 * as the HMAC key, then hkdf_extract internally calls
	 * update(ikm) + digest(prk). */
    hmac_sha256_set_key (&ctx, salt_len, salt);
    hkdf_extract (&ctx,
                  (nettle_hash_update_func *) hmac_sha256_update,
                  (nettle_hash_digest_func *) hmac_sha256_digest,
                  SHA256_DIGEST_SIZE,
                  ikm_len, ikm,
                  prk);

    /* HKDF-Expand: out = T(1) || T(2) || ..., where each T(i) is
	 * HMAC-SHA256(PRK, T(i-1) || info || i). The pre-keyed MAC
	 * context for expand uses PRK as the key. */
    hmac_sha256_set_key (&ctx, SHA256_DIGEST_SIZE, prk);
    hkdf_expand (&ctx,
                 (nettle_hash_update_func *) hmac_sha256_update,
                 (nettle_hash_digest_func *) hmac_sha256_digest,
                 SHA256_DIGEST_SIZE,
                 info_len, info,
                 out_len, out);
}

/* ---- Key derivation ------------------------------------------- */

void
cipher_aead_derive_session_keys (
    chacha_aead_state *encode_out, chacha_aead_state *decode_out,
    const uint8_t *session_key, size_t session_key_len,
    const uint8_t *encode_key,  size_t encode_key_len,
    const uint8_t *decode_key,  size_t decode_key_len)
{
    /* fogWraith HOPE-ChaCha20-Poly1305.md spec, "AEAD Key
	 * Derivation":
	 *
	 *   encode_key_256 = HKDF-SHA256(ikm=encode_key,
	 *                                salt=session_key,
	 *                                info="hope-chacha-encode")
	 *   decode_key_256 = HKDF-SHA256(ikm=decode_key,
	 *                                salt=session_key,
	 *                                info="hope-chacha-decode")
	 *
	 *   "The server uses encode_key_256 for outbound (server →
	 *    client) and decode_key_256 for inbound (client → server).
	 *    The client uses them in reverse: reads with
	 *    encode_key_256, writes with decode_key_256."
	 *
	 * So encode_key_256 is the SERVER's outbound key (s→c
	 * direction), and the client uses it for reads. decode_key_256
	 * is the SERVER's inbound key (c→s direction), and the client
	 * uses it for writes. The spec's "encode"/"decode" naming is
	 * server-centric — we mirror it backwards.
	 *
	 * Our chacha_aead_state slot names (encode_out / decode_out)
	 * are CLIENT-centric: encode_out is what the client encodes
	 * outgoing with, decode_out is what the client decodes
	 * incoming with. So:
	 *
	 *   encode_out  ← decode_key_256  (client write, c→s)
	 *   decode_out  ← encode_key_256  (client read,  s→c)
	 *
	 * Phase B+C originally assigned these the wrong way around,
	 * which produced an AEAD authentication failure on the first
	 * sealed server frame against Janus. */
    static const char info_encode[] = "hope-chacha-encode";
    static const char info_decode[] = "hope-chacha-decode";

    memset (encode_out, 0, sizeof (*encode_out));
    memset (decode_out, 0, sizeof (*decode_out));

    /* Spec's encode_key_256 — the server's outbound key, which we
	 * (the client) use to DECODE incoming frames. Lands in
	 * decode_out. */
    cipher_aead_hkdf_sha256 (
        session_key, session_key_len,
        encode_key,  encode_key_len,
        (const uint8_t *) info_encode, sizeof (info_encode) - 1,
        decode_out->key, sizeof (decode_out->key));

    /* Spec's decode_key_256 — the server's inbound key, which we
	 * (the client) use to ENCODE outgoing frames. Lands in
	 * encode_out. */
    cipher_aead_hkdf_sha256 (
        session_key, session_key_len,
        decode_key,  decode_key_len,
        (const uint8_t *) info_decode, sizeof (info_decode) - 1,
        encode_out->key, sizeof (encode_out->key));

    /* We're the client side. Client → server frames use dir=0x01
	 * (encode); server → client frames use dir=0x00 (decode). */
    encode_out->dir = CIPHER_AEAD_DIR_CLIENT_TO_SERVER;
    decode_out->dir = CIPHER_AEAD_DIR_SERVER_TO_CLIENT;

    encode_out->counter = 0;
    decode_out->counter = 0;

    /* One-shot debug dump of the derived keys, gated on the
	 * "xfer-aead" debug category. Useful for off-line comparison
	 * against another implementation when the on-wire MAC doesn't
	 * verify — the inputs (session_key + the two HMAC keys) plus
	 * the resulting AEAD keys are the full HKDF state. Keys
	 * shouldn't normally be logged, but this is a development
	 * branch and the category is opt-in (GTKHX_DEBUG=xfer-aead). */
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
    static const char info_base[] = "hope-file-transfer";
    static const char info_ref[]  = "hope-ft-ref";

    /* ft_base_key = HKDF(ikm = encode_key_256 || decode_key_256,
	 *                    salt = session_key,
	 *                    info = "hope-file-transfer")
	 *
	 * Note: ctrl_encode->key holds the spec's decode_key_256
	 * (client encodes outgoing with the server's inbound key),
	 * and ctrl_decode->key holds the spec's encode_key_256
	 * (client decodes incoming with the server's outbound key)
	 * — see cipher_aead_derive_session_keys' big comment. To
	 * match the spec's "encode_key_256 || decode_key_256" order
	 * verbatim, concatenate ctrl_decode->key first, then
	 * ctrl_encode->key. Both peers compute the same 64-byte
	 * ikm and therefore the same ft_base_key. */
    uint8_t ikm_concat[64];
    memcpy (ikm_concat,      ctrl_decode->key, 32);
    memcpy (ikm_concat + 32, ctrl_encode->key, 32);

    uint8_t ft_base_key[32];
    cipher_aead_hkdf_sha256 (
        session_key, session_key_len,
        ikm_concat,  sizeof (ikm_concat),
        (const uint8_t *) info_base, sizeof (info_base) - 1,
        ft_base_key, sizeof (ft_base_key));

    /* transfer_key = HKDF(ikm = ft_base_key,
	 *                     salt = ref (4 bytes BE),
	 *                     info = "hope-ft-ref")
	 *
	 * Same key is computed by both peers; we split it into the
	 * two per-direction state slots so the seal/open path stays
	 * uniform with the control channel. */
    uint8_t ref_be[4];
    ref_be[0] = (ref >> 24) & 0xff;
    ref_be[1] = (ref >> 16) & 0xff;
    ref_be[2] = (ref >> 8)  & 0xff;
    ref_be[3] =  ref        & 0xff;

    uint8_t transfer_key[32];
    cipher_aead_hkdf_sha256 (
        ref_be,      sizeof (ref_be),
        ft_base_key, sizeof (ft_base_key),
        (const uint8_t *) info_ref, sizeof (info_ref) - 1,
        transfer_key, sizeof (transfer_key));

    memset (xfer_encode_out, 0, sizeof (*xfer_encode_out));
    memset (xfer_decode_out, 0, sizeof (*xfer_decode_out));

    memcpy (xfer_encode_out->key, transfer_key, sizeof (transfer_key));
    memcpy (xfer_decode_out->key, transfer_key, sizeof (transfer_key));

    xfer_encode_out->dir = CIPHER_AEAD_DIR_CLIENT_TO_SERVER;
    xfer_decode_out->dir = CIPHER_AEAD_DIR_SERVER_TO_CLIENT;

    xfer_encode_out->counter = 0;
    xfer_decode_out->counter = 0;

    /* Defensive: scrub the intermediate key material. The compiler
	 * may not be required to honour memset on a local that's going
	 * out of scope, but it usually does, and there's no perf cost.
	 * Stub for security hygiene; not load-bearing. */
    memset (ikm_concat,   0, sizeof (ikm_concat));
    memset (ft_base_key,  0, sizeof (ft_base_key));
    memset (transfer_key, 0, sizeof (transfer_key));
}

/* ---- Frame codec ---------------------------------------------- */

/* Build a 12-byte ChaCha20-Poly1305 nonce from (dir, counter):
 *
 *   bytes 0      1..3     4..11
 *         dir    0x000000 counter (big-endian u64)
 *
 * Per the spec, the direction byte ensures server and client
 * never produce the same nonce even with the same key; the
 * counter guarantees per-direction uniqueness. */
static void
build_nonce (uint8_t out[12], uint8_t dir, uint64_t counter)
{
    out[0] = dir;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x00;
    out[4]  = (uint8_t) ((counter >> 56) & 0xff);
    out[5]  = (uint8_t) ((counter >> 48) & 0xff);
    out[6]  = (uint8_t) ((counter >> 40) & 0xff);
    out[7]  = (uint8_t) ((counter >> 32) & 0xff);
    out[8]  = (uint8_t) ((counter >> 24) & 0xff);
    out[9]  = (uint8_t) ((counter >> 16) & 0xff);
    out[10] = (uint8_t) ((counter >>  8) & 0xff);
    out[11] = (uint8_t) ( counter        & 0xff);
}

size_t
cipher_aead_seal (chacha_aead_state *state,
                  const uint8_t *plaintext, size_t pt_len,
                  uint8_t *out, size_t out_cap)
{
    /* Length prefix carries (ciphertext_len + tag_len). Anything
	 * approaching the 16 MiB cap is almost certainly a bug
	 * upstream — Hotline transactions are small. */
    if (pt_len > CIPHER_AEAD_MAX_FRAME_SIZE - CIPHER_AEAD_TAG_SIZE) {
        return 0;
    }

    size_t framed_len
        = CIPHER_AEAD_LENGTH_PREFIX + pt_len + CIPHER_AEAD_TAG_SIZE;
    if (out_cap < framed_len) {
        return 0;
    }

    /* Length prefix = ciphertext + tag (NOT including the 4-byte
	 * prefix itself). Big-endian u32. The prefix is not encrypted
	 * and not part of the AAD — the spec is explicit about this. */
    uint32_t body_len = (uint32_t) (pt_len + CIPHER_AEAD_TAG_SIZE);
    out[0] = (uint8_t) ((body_len >> 24) & 0xff);
    out[1] = (uint8_t) ((body_len >> 16) & 0xff);
    out[2] = (uint8_t) ((body_len >> 8)  & 0xff);
    out[3] = (uint8_t) ( body_len        & 0xff);

    uint8_t nonce[12];
    build_nonce (nonce, state->dir, state->counter);

    struct chacha_poly1305_ctx ctx;
    chacha_poly1305_set_key (&ctx, state->key);
    chacha_poly1305_set_nonce (&ctx, nonce);

    /* No AAD on AEAD frames in this protocol. Ciphertext +
	 * Poly1305 tag follow immediately after the length prefix. */
    chacha_poly1305_encrypt (&ctx, pt_len,
                             out + CIPHER_AEAD_LENGTH_PREFIX,
                             plaintext);
    chacha_poly1305_digest (&ctx, CIPHER_AEAD_TAG_SIZE,
                            out + CIPHER_AEAD_LENGTH_PREFIX + pt_len);

    state->counter++;
    return framed_len;
}

size_t
cipher_aead_peek_frame_size (const uint8_t *framed, size_t framed_len)
{
    if (framed_len < CIPHER_AEAD_LENGTH_PREFIX) {
        return 0;
    }

    uint32_t body_len = ((uint32_t) framed[0] << 24)
                      | ((uint32_t) framed[1] << 16)
                      | ((uint32_t) framed[2] << 8)
                      | ((uint32_t) framed[3]);

    /* body_len must include at least the 16-byte tag; otherwise
	 * the frame is malformed. Cap defends against memory
	 * exhaustion from a hostile / corrupted length prefix. */
    if (body_len < CIPHER_AEAD_TAG_SIZE) {
        return 0;
    }
    if (body_len > CIPHER_AEAD_MAX_FRAME_SIZE) {
        return 0;
    }

    return (size_t) CIPHER_AEAD_LENGTH_PREFIX + (size_t) body_len;
}

size_t
cipher_aead_open (chacha_aead_state *state,
                  const uint8_t *framed, size_t framed_len,
                  uint8_t *out, size_t out_cap)
{
    size_t frame_total = cipher_aead_peek_frame_size (framed, framed_len);
    if (frame_total == 0 || framed_len < frame_total) {
        return 0;
    }

    /* Split the framed buffer:
	 *   framed + 0                   length prefix (4 bytes)
	 *   framed + 4                   ciphertext (pt_len bytes)
	 *   framed + 4 + pt_len          Poly1305 tag (16 bytes)
	 *
	 * pt_len = (frame_total - prefix) - tag. */
    size_t body_len = frame_total - CIPHER_AEAD_LENGTH_PREFIX;
    size_t pt_len   = body_len - CIPHER_AEAD_TAG_SIZE;
    if (out_cap < pt_len) {
        return 0;
    }

    const uint8_t *ct_ptr  = framed + CIPHER_AEAD_LENGTH_PREFIX;
    const uint8_t *tag_ptr = ct_ptr + pt_len;

    uint8_t nonce[12];
    build_nonce (nonce, state->dir, state->counter);

    struct chacha_poly1305_ctx ctx;
    chacha_poly1305_set_key (&ctx, state->key);
    chacha_poly1305_set_nonce (&ctx, nonce);

    /* No AAD. Decrypt-then-verify: Nettle's API lets us call
	 * digest after decrypt and compare against the provided tag.
	 * Constant-time compare via memcmp's compiler intrinsics is
	 * not portably constant-time, but Poly1305 tags don't admit
	 * timing-oracle attacks the way password / MAC comparisons
	 * do — a non-matching tag means we drop the frame and the
	 * attacker learns "auth failed" regardless of timing. */
    chacha_poly1305_decrypt (&ctx, pt_len, out, ct_ptr);

    uint8_t computed_tag[CIPHER_AEAD_TAG_SIZE];
    chacha_poly1305_digest (&ctx, CIPHER_AEAD_TAG_SIZE, computed_tag);

    /* Constant-time compare: XOR-accumulate the difference, then
	 * test against zero once at the end. Defensive — see comment
	 * above. */
    uint8_t diff = 0;
    for (size_t i = 0; i < CIPHER_AEAD_TAG_SIZE; i++) {
        diff |= computed_tag[i] ^ tag_ptr[i];
    }
    if (diff != 0) {
        return 0;
    }

    state->counter++;
    return pt_len;
}

#endif /* CONFIG_CIPHER */
