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
#include <gtk/gtk.h>
#include <dirent.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <nettle/arcfour.h>
#include <nettle/blowfish.h>
#include "hx.h"
#include "cipher.h"

#ifdef CONFIG_CIPHER

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
	if (htlc->cipher_encode_type != CIPHER_NONE) {
#if defined(CONFIG_COMPRESS)
		if (htlc->compress_encode_type == COMPRESS_NONE) {
#endif
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
#if defined(CONFIG_COMPRESS)
		} else if (htlc->zc_ran) {
			do_encode(htlc, pos, htlc->zc_hdrlen);
			cipher_change_encode_key(htlc, htlc->zc_ran);
			pos += htlc->zc_hdrlen;
			len -= htlc->zc_hdrlen;
			htlc->zc_ran = 0;
		}
#endif
	} else {
		return;
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
			/* Re-init wipes ivec/num so a fresh key starts at OFB
			 * block boundary, which matches the wire contract. */
			memset(&htlc->cipher_encode_state.blowfish, 0,
			       sizeof(htlc->cipher_encode_state.blowfish));
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
			memset(&htlc->cipher_decode_state.blowfish, 0,
			       sizeof(htlc->cipher_decode_state.blowfish));
			blowfish_set_key(&htlc->cipher_decode_state.blowfish.ctx,
			                 htlc->cipher_decode_keylen,
			                 htlc->cipher_decode_key);
			break;
		default:
			break;
	}
}

#endif /* CONFIG_CIPHER */
