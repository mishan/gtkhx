/*
 * Per-connection cipher state machine for the Hotline HOPE handshake.
 *
 * Crypto primitives now come from Nettle. The wire protocol predates
 * common integrated cipher modes and asks for two things:
 *
 *   - ARC4 (RC4)            stream cipher, byte-at-a-time
 *   - Blowfish in OFB-64    block cipher run as a keystream generator
 *
 * Nettle ships ECB Blowfish but no OFB helper, so blowfish_ofb64_crypt
 * below is a tiny reimplementation of OpenSSL's BF_ofb64_encrypt: walk
 * the keystream byte-by-byte, refilling ivec by encrypting it in-place
 * every 8 bytes. Symmetric (encrypt == decrypt). The on-the-wire bytes
 * are byte-identical to what the old OpenSSL path produced — verified
 * against historical packet captures during the port.
 *
 * The HOPE rekey trick (random nibble in the header triggering N
 * rounds of HMAC-stretching the cipher key) is unchanged.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h> /* g_memdup2 / g_realloc / g_free / guint32 — the
                   * only symbols this TU needs from the GLib stack.
                   * Pulling in just <glib.h> instead of <gtk/gtk.h>
                   * lets the Tier 3 integration harness link cipher
                   * .c without dragging in GTK + Adwaita; same
                   * pattern as proto_trace.c. */
#include <dirent.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <nettle/arcfour.h>
#include <nettle/blowfish.h>
/* Deliberately avoid pulling in hx.h: hx.h transitively includes
 * session.h, which #includes <gtk/gtk.h>. cipher.c only needs the
 * wire-protocol types and helpers (struct htlc_conn, qbuf, hmac_xxx,
 * random_bytes) — all of which live in protocol.h directly. Skipping
 * hx.h lets the Tier 3 integration harness link cipher.c without
 * dragging GTK + Adwaita into the test binaries. Same pattern
 * proto_trace.c follows. */
#include "protocol.h"
#include "cipher.h"
#include "cipher_aead.h"


#define CIPHER_DEBUG	0

#if CIPHER_DEBUG
static void
writestuff (const char *str, u_int8_t type, const u_int8_t *buf, unsigned int len)
{
	unsigned int i;
	char file[32];
	FILE *fp;

	snprintf(file, sizeof file, "/tmp/cipher.%d", getpid());
	fp = fopen(file, "a");
	if (!fp)
		return;
	fputs(str, fp);
	fprintf(fp, "%u: ", type);
	for (i = 0; i < len; i++)
		fprintf(fp, "%2.2x", buf[i]);
	fprintf(fp, "\n");
	fflush(fp);
	fclose(fp);
}
#endif

/*
 * Blowfish in 64-bit Output Feedback mode.
 *
 * OFB is symmetric: same routine for encrypt and decrypt. State carried
 * across calls is the 8-byte ivec (current keystream block) plus the
 * byte index `num` into that block (0..7). When `num` rolls over to
 * zero we re-encrypt ivec in place to produce the next keystream block.
 *
 * Matches OpenSSL's BF_ofb64_encrypt exactly, which is the contract
 * the wire protocol expects. Safe for in-place (src == dst).
 */
static void
blowfish_ofb64_crypt(blowfish_state *bs,
                     const uint8_t *src, uint8_t *dst, size_t len)
{
	int n = bs->num;

	while (len--) {
		if (n == 0)
			blowfish_encrypt(&bs->ctx, BLOWFISH_BLOCK_SIZE,
			                 bs->ivec, bs->ivec);
		*dst++ = *src++ ^ bs->ivec[n];
		n = (n + 1) & 7;
	}
	bs->num = n;
}

u_int32_t
cipher_decode (struct htlc_conn *htlc, struct qbuf *out, struct qbuf *in,
	       u_int32_t max, u_int32_t *inusedp)
{
	u_int32_t len;

	len = in->len;
	if (len > max)
		len = max;
	qbuf_set(out, 0, len);
#if CIPHER_DEBUG
	writestuff("dec: ", htlc->cipher_decode_type, &in->buf[in->pos], len);
#endif
	switch (htlc->cipher_decode_type) {
		case CIPHER_RC4:
			arcfour_crypt(&htlc->cipher_decode_state.rc4, len,
			              &out->buf[out->pos], &in->buf[in->pos]);
			break;
		case CIPHER_BLOWFISH:
			blowfish_ofb64_crypt(&htlc->cipher_decode_state.blowfish,
			                     &in->buf[in->pos], &out->buf[out->pos], len);
			break;
		default:
			break;
	}
#if CIPHER_DEBUG
	writestuff("dec: ", htlc->cipher_decode_type, &out->buf[out->pos], len);
#endif
	*inusedp = len;

	return len;
}

void
cipher_change_decode_key (struct htlc_conn *htlc, u_int32_t type)
{
	u_int16_t len = 0;
	u_int32_t i, num;

	num = (type >> 24) & 0xff;
	for (i = 0; i < num; i++) {
		len = hmac_xxx(htlc->cipher_decode_key,
			       htlc->cipher_decode_key, htlc->cipher_decode_keylen,
			       htlc->sessionkey, htlc->sklen, htlc->macalg);
	}
#if CIPHER_DEBUG
	writestuff("deckey: ", num, htlc->cipher_decode_key, len);
#endif
	htlc->cipher_decode_keylen = len;
	cipher_decode_init(htlc);
}

static void
cipher_change_encode_key (struct htlc_conn *htlc, unsigned int num)
{
	u_int16_t len = 0;
	unsigned int i;

	for (i = 0; i < num; i++) {
		len = hmac_xxx(htlc->cipher_encode_key,
			       htlc->cipher_encode_key, htlc->cipher_encode_keylen,
			       htlc->sessionkey, htlc->sklen, htlc->macalg);
	}
#if CIPHER_DEBUG
	writestuff("enckey: ", num, htlc->cipher_encode_key, len);
#endif
	htlc->cipher_encode_keylen = len;
	cipher_encode_init(htlc);
}

static void
do_encode (struct htlc_conn *htlc, unsigned int pos, unsigned int len)
{
#if CIPHER_DEBUG
	writestuff("enc: ", htlc->cipher_encode_type, &htlc->out.buf[pos], len);
#endif
	switch (htlc->cipher_encode_type) {
		case CIPHER_RC4:
			arcfour_crypt(&htlc->cipher_encode_state.rc4, len,
			              &htlc->out.buf[pos], &htlc->out.buf[pos]);
			break;
		case CIPHER_BLOWFISH:
			blowfish_ofb64_crypt(&htlc->cipher_encode_state.blowfish,
			                     &htlc->out.buf[pos], &htlc->out.buf[pos], len);
			break;
		default:
			break;
	}
#if CIPHER_DEBUG
	writestuff("enc: ", htlc->cipher_encode_type, &htlc->out.buf[pos], len);
#endif
}

void
cipher_encode (struct htlc_conn *htlc, unsigned int pos, unsigned int len)
{
	if (htlc->cipher_encode_type == CIPHER_NONE) {
		return;
	}

	/* AEAD path: replace the in-place stream XOR with a framed
	 * Seal that grows the on-wire bytes by
	 *     CIPHER_AEAD_LENGTH_PREFIX (4) + CIPHER_AEAD_TAG_SIZE (16)
	 * per transaction. The plaintext at [pos, pos+len) gets
	 * copied out, the slot is shrunk away from the accounting,
	 * the buffer is grown to fit the framed payload, and the
	 * seal output is written back at the same start position.
	 * Compression is NOT used in AEAD mode (spec — the framing
	 * already wraps the transaction), so we don't have to worry
	 * about the zc_ran rekey path here. The rekey-on-random
	 * nibble trick is also moot: ChaCha20-Poly1305 uses counter-
	 * based nonces, no per-packet rotation needed. */
	if (htlc->cipher_mode == CIPHER_MODE_AEAD) {
		uint8_t *plaintext = g_memdup2 (&htlc->out.buf[pos], len);
		size_t framed_len = CIPHER_AEAD_LENGTH_PREFIX + len
		                  + CIPHER_AEAD_TAG_SIZE;

		/* Rewind out.len to remove the plaintext bytes from the
		 * in-flight range, grow the BUFFER to fit framed_len bytes
		 * (without bumping out.len — qbuf_set would, so we realloc
		 * directly), then seal into the freed-up slot and add the
		 * framed length back to out.len.
		 *
		 * Earlier revisions used qbuf_set here, which sets out.len
		 * AS A SIDE EFFECT of the realloc. Combined with the
		 * out.len += written below, that double-counted framed_len
		 * worth of bytes on the wire — the socket-write loop sent
		 * the valid sealed frame followed by framed_len bytes of
		 * uninitialised buffer tail. Server saw a valid frame
		 * followed by garbage that wasn't a length prefix and
		 * either tore down or replied in a way we couldn't open. */
		htlc->out.len -= len;
		guint32 need = htlc->out.pos + htlc->out.len + framed_len;
		if (need > htlc->out.pos + htlc->out.len) {
			htlc->out.buf = g_realloc (htlc->out.buf, need);
		}
		size_t written = cipher_aead_seal (
		    &htlc->cipher_encode_state.chacha,
		    plaintext, len,
		    &htlc->out.buf[pos], framed_len);
		htlc->out.len += written;
		g_free (plaintext);
		return;
	}

	/* Stream-cipher path (RC4 / Blowfish OFB). The rekey-on-
	 * random-nibble trick is the legacy HOPE behaviour: with
	 * probability 3/16, mark the header type byte and the next
	 * N rounds of HMAC-stretching for the cipher key (see
	 * cipher_change_encode_key). */
	if (htlc->compress_encode_type == COMPRESS_NONE) {
		unsigned char ran;

		random_bytes(&ran, 1);
		ran >>= 4;
		if (ran == 2 || ran == 7 || ran == 13) {
			u_int32_t type = 0;

			random_bytes(&ran, 1);
			ran >>= 2;
			if (!ran) {
				random_bytes(&ran, 1);
				ran = (ran >> 3) + 1;
			}
			HN32(&type, &htlc->out.buf[pos]);
			type |= (((u_int32_t)ran << 24) & 0xff000000);
			HN32(&htlc->out.buf[pos], &type);
			do_encode(htlc, pos, SIZEOF_HL_HDR);
			cipher_change_encode_key(htlc, ran);
			pos += SIZEOF_HL_HDR;
			len -= SIZEOF_HL_HDR;
		}
	} else if (htlc->zc_ran) {
		do_encode(htlc, pos, htlc->zc_hdrlen);
		cipher_change_encode_key(htlc, htlc->zc_ran);
		pos += htlc->zc_hdrlen;
		len -= htlc->zc_hdrlen;
		htlc->zc_ran = 0;
	}
	do_encode(htlc, pos, len);
}

void
cipher_encode_init (struct htlc_conn *htlc)
{
	switch (htlc->cipher_encode_type) {
		case CIPHER_RC4:
			arcfour_set_key(&htlc->cipher_encode_state.rc4,
			                htlc->cipher_encode_keylen,
			                htlc->cipher_encode_key);
			break;
		case CIPHER_BLOWFISH:
			/* DO NOT reset ivec/num here. cipher_encode_init is
			 * called twice in a connection's lifetime: at post-
			 * Step-2 setup (where the blowfish_state union was
			 * already zero-initialised when htlc was allocated,
			 * so ivec/num start at the OFB block boundary
			 * naturally), and again from cipher_change_encode_key
			 * every time the legacy HOPE per-message rekey marker
			 * fires (~3/16 of outgoing messages). The wire
			 * contract for the rekey case is "rotate the key
			 * schedule, KEEP the OFB ivec/num where they are" —
			 * mhxd's cipher_encode_init does the same (just
			 * BF_set_key, no memset). The early Nettle-port
			 * revision of this file added a memset here as
			 * defensive scrubbing; it desynced the cipher state
			 * every rekey, which the new Tier 3 test_hope_
			 * blowfish caught against mhxd. */
			blowfish_set_key(&htlc->cipher_encode_state.blowfish.ctx,
			                 htlc->cipher_encode_keylen,
			                 htlc->cipher_encode_key);
			break;
		default:
			break;
	}
}

void
cipher_decode_init (struct htlc_conn *htlc)
{
	switch (htlc->cipher_decode_type) {
		case CIPHER_RC4:
			arcfour_set_key(&htlc->cipher_decode_state.rc4,
			                htlc->cipher_decode_keylen,
			                htlc->cipher_decode_key);
			break;
		case CIPHER_BLOWFISH:
			/* See cipher_encode_init above for why we don't
			 * memset ivec/num here. */
			blowfish_set_key(&htlc->cipher_decode_state.blowfish.ctx,
			                 htlc->cipher_decode_keylen,
			                 htlc->cipher_decode_key);
			break;
		default:
			break;
	}
}

