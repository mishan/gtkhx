/*
 * Per-connection cipher state for the Hotline HOPE handshake.
 *
 * The wire protocol uses Blowfish in 64-bit OFB mode (ofb64) and
 * ARC4 (RC4); both implementations come from Nettle. IDEA was once
 * defined as CIPHER_IDEA = 3 but never wired into HOPE negotiation
 * (patent-encumbered at the time and now expired but still unused);
 * the union/struct entries are gone.
 */

#ifndef __cipher_h
#define __cipher_h

#include "config.h"

#ifdef CONFIG_CIPHER

#include <stdint.h>
#include <nettle/arcfour.h>
#include <nettle/blowfish.h>

#include "hx.h"

#define CIPHER_NONE	0
#define CIPHER_RC4	1
#define CIPHER_BLOWFISH	2
/* CIPHER_IDEA = 3 is reserved at the protocol level but never offered. */

/* RC4: Nettle's arcfour_ctx is the entire state. */
typedef struct arcfour_ctx rc4_state;

/* Blowfish in 64-bit OFB needs the key schedule plus an IV register
 * and a byte index into it (0..7). Mirrors the OpenSSL BF_KEY/ivec/num
 * trio that the previous cipher_openssl.h shim exposed. */
struct blowfish_state {
	struct blowfish_ctx ctx;
	uint8_t ivec[BLOWFISH_BLOCK_SIZE];
	int num;
};
typedef struct blowfish_state blowfish_state;

union cipher_state {
	rc4_state	rc4;
	blowfish_state	blowfish;
};

struct htlc_conn;
struct qbuf;

extern void cipher_encode (struct htlc_conn *htlc, unsigned int start, unsigned int len);
extern u_int32_t cipher_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in, u_int32_t max, u_int32_t *nusedp);
extern void cipher_encode_init (struct htlc_conn *htlc);
extern void cipher_decode_init (struct htlc_conn *htlc);
extern void cipher_change_decode_key (struct htlc_conn *htlc, u_int32_t type);

#endif /* CONFIG_CIPHER */

#endif /* __cipher_h */
