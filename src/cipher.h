/*
 * Per-connection cipher state for the Hotline HOPE handshake.
 *
 * The wire protocol once offered ARC4 (RC4) and Blowfish in 64-bit
 * OFB mode (ofb64) as the legacy stream-cipher choices. RC4 was
 * removed in claude/remove-rc4 — it's a known-broken stream cipher
 * and shipping it under a "Secure (HOPE)" label gave users a false
 * sense of security. The CIPHER_RC4 = 1 protocol slot stays reserved
 * so the constant doesn't get accidentally re-used; no code path
 * advertises or accepts RC4 anymore. Blowfish comes from Nettle.
 *
 * IDEA was once defined as CIPHER_IDEA = 3 but never wired into HOPE
 * negotiation (patent-encumbered at the time and now expired but
 * still unused); the union/struct entries are gone.
 */

#ifndef __cipher_h
#define __cipher_h

#include "config.h"


#include <stdint.h>
#include <sys/types.h> /* u_int8_t / u_int32_t */
#include <nettle/blowfish.h>

/* Note: cipher.h is pulled into protocol.h via the htlc_conn cipher_state
 * fields, so this header MUST NOT include hx.h or session.h — that would
 * create a cycle (protocol.h -> cipher.h -> hx.h -> session.h, where the
 * session struct then references types not yet defined). Forward decls
 * below are sufficient. */

#define CIPHER_NONE 0
/* CIPHER_RC4 = 1 — the legacy RC4 slot. Removed; the integer is
 * reserved so the protocol assignment doesn't get re-used by a
 * future cipher. See the file header for the rationale. */
#define CIPHER_BLOWFISH 2
/* CIPHER_IDEA = 3 was reserved at the protocol level but never offered. */
/* Phase 5+: ChaCha20-Poly1305 AEAD (fogWraith HOPE-ChaCha20-Poly1305.md
 * extension). Unlike Blowfish which runs as a byte-stream XOR cipher,
 * this one operates on length-prefixed authenticated frames. The cipher
 * type and the cipher MODE are negotiated independently: a server can
 * theoretically negotiate this type with cipher_mode = STREAM (no real
 * server does that, but the spec keeps them orthogonal). */
#define CIPHER_CHACHA20_POLY1305 4

/* Cipher mode. Stream ciphers (Blowfish OFB) use STREAM; AEAD
 * ciphers (ChaCha20-Poly1305) use AEAD. The mode controls framing:
 *
 *   STREAM: byte-at-a-time XOR over the entire connection, with a
 *           separate checksum chunk in HOPE messages for integrity.
 *           Compression interleaves freely.
 *   AEAD:   length-prefixed Seal/Open per Hotline transaction. The
 *           Poly1305 tag is built into the frame. Compression is NOT
 *           used (spec says so — AEAD framing already wraps the
 *           transaction). Counter-based nonces eliminate the random-
 *           nibble rekey trick the stream path uses.
 *
 * htlc->cipher_mode is filled from the server's HTLS_DATA_CIPHER_MODE
 * value during the HOPE Step 2 reply ("STREAM" / "AEAD"). */
#define CIPHER_MODE_STREAM 0
#define CIPHER_MODE_AEAD   1

/* Blowfish in 64-bit OFB needs the key schedule plus an IV register
 * and a byte index into it (0..7). Mirrors the OpenSSL BF_KEY/ivec/num
 * trio that the previous cipher_openssl.h shim exposed. */
struct blowfish_state {
    struct blowfish_ctx ctx;
    uint8_t ivec[BLOWFISH_BLOCK_SIZE];
    int num;
};
typedef struct blowfish_state blowfish_state;

/* ChaCha20-Poly1305 AEAD state.
 *
 * Per-frame nettle contexts are constructed on the stack at seal/open
 * time (Nettle's chacha_poly1305_ctx is just a tiny stateful API), so
 * the persistent state we carry on htlc_conn is only:
 *
 *   key      32-byte HKDF-expanded session key (encode or decode)
 *   counter  monotonic frame counter for this direction (u64 BE in
 *            the nonce). Starts at 0; bumped after each seal/open.
 *   dir      0x00 (server→client) or 0x01 (client→server). Constant
 *            for the lifetime of the state — encode state always
 *            uses 0x01 from the client side, decode state 0x00.
 *
 * Receive-side accumulation (collecting bytes until a full length-
 * prefixed frame is available) lives in htlc->in / htlc->read_in;
 * the AEAD state doesn't need its own buffer. */
struct chacha_aead_state {
    uint8_t key[32];
    uint64_t counter;
    uint8_t dir;
};
typedef struct chacha_aead_state chacha_aead_state;

union cipher_state {
    blowfish_state blowfish;
    chacha_aead_state chacha;
};

struct htlc_conn;
struct qbuf;

extern void cipher_encode (struct htlc_conn *htlc, unsigned int start,
                           unsigned int len);
extern u_int32_t cipher_decode (struct htlc_conn *htlc, struct qbuf *out,
                                struct qbuf *in, u_int32_t max,
                                u_int32_t *nusedp);
extern void cipher_encode_init (struct htlc_conn *htlc);
extern void cipher_decode_init (struct htlc_conn *htlc);
extern void cipher_change_decode_key (struct htlc_conn *htlc, u_int32_t type);

/*
 * Stream-cipher rekey-marker detection on receive. cipher_encode at
 * src/cipher.c randomly (~3/16 per outgoing message) stamps a 1-63
 * iteration count into the type field's high byte and rotates the
 * encode key by that many HMAC iterations. The receiver must do the
 * matching rotation BEFORE the next bytes are fed to cipher_decode,
 * or the cipher state desyncs and every subsequent header decodes
 * to garbage. User-visible symptom of the missing call: "unknown
 * header type 0x3d000162" / "0x2f010000" from servers that exercise
 * the legacy HOPE stream-cipher rekey trick (Janus/vespernet observed;
 * mhxd does it too).
 *
 * Pass the freshly-decoded 32-bit type by reference. On return:
 *   - if a marker was present (returns TRUE), the rotation has run
 *     and the high byte has been cleared in *type_inout so the
 *     caller sees the real opcode in the low 24 bits;
 *   - if no marker was present (returns FALSE), *type_inout is
 *     unchanged.
 *
 * Internally gated: only fires when cipher_mode == CIPHER_MODE_STREAM
 * and cipher_decode_type != CIPHER_NONE. AEAD mode uses counter-based
 * nonces and has no per-packet rotation; CIPHER_NONE means HMAC-only
 * HOPE which also shouldn't see this marker.
 *
 * Note: this only touches the cipher state and *type_inout. Callers
 * that also need to keep htlc->in.buf clean for downstream macros
 * (e.g. the integration harness's hdr_type()) must additionally zero
 * the high byte of the on-buffer type field themselves — the
 * production receive path doesn't need this because it dispatches off
 * the local type variable directly.
 */
extern int cipher_check_rekey_marker (struct htlc_conn *htlc,
                                      u_int32_t *type_inout);


#endif /* __cipher_h */
