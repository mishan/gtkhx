/*
 * Per-connection cipher state machine for the Hotline HOPE handshake.
 *
 * After Phase R1 this file is a thin dispatcher: the actual
 * cryptographic primitives live in Rust crates (rust/crates/
 * hxcrypto-stream for Blowfish OFB-64; rust/crates/hxcrypto-aead
 * for ChaCha20-Poly1305). The HOPE per-message rekey trick
 * (random nibble in the header triggering N rounds of HMAC-
 * stretching the cipher key), the AEAD-vs-STREAM mode split, and
 * the legacy "no header marker when compression is on" carve-out
 * stay here because they're protocol concerns, not crypto.
 *
 * RC4 was removed in claude/remove-rc4 (insecure stream cipher).
 * Blowfish is the only stream cipher left; ChaCha20-Poly1305 takes
 * the secure path on servers that advertise it.
 *
 * Wire output is byte-identical to the pre-port Nettle (and original
 * OpenSSL) implementations — verified against historical packet
 * captures and against live mhxd / Janus via the Tier 3 hope_blowfish
 * and hope_chacha20 tests.
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
/* Deliberately avoid pulling in hx.h: hx.h transitively includes
 * session.h, which #includes <gtk/gtk.h>. cipher.c only needs the
 * wire-protocol types and helpers (struct htlc_conn, qbuf, hmac_xxx,
 * random_bytes) — all of which live in protocol.h directly. */
#include "protocol.h"
#include "cipher.h"
#include "cipher_aead.h"


/* ---- Rust FFI declarations (hxcrypto-stream) ----
 *
 * Hand-written rather than #include'd from a cbindgen-generated
 * header — keeps the FFI surface obvious to anyone reading this
 * file and avoids the build-time dance of generating headers into
 * the meson build dir. Drift between Rust signatures and these
 * externs would surface at link time as undefined-symbol errors,
 * not at runtime; for opaque-pointer FFI that's enough.
 */

extern BlowfishOfb64State *gtkhx_blowfish_ofb64_new (const uint8_t *key,
                                                     uint32_t keylen);
extern void gtkhx_blowfish_ofb64_free (BlowfishOfb64State *state);
extern void gtkhx_blowfish_ofb64_set_key (BlowfishOfb64State *state,
                                          const uint8_t *key, uint32_t keylen);
extern void gtkhx_blowfish_ofb64_crypt (BlowfishOfb64State *state,
                                        const uint8_t *src, uint8_t *dst,
                                        uint32_t len);

/* Forward-declared rather than #include'd from network.h to keep
 * cipher.c free of GTK / hx.h transitives — the Tier 3 integration
 * harness re-builds this TU and pulling network.h would drag a wide
 * link surface in. Only used for the NULL-cipher-state hard-fail
 * path below. */
extern void hx_htlc_close (struct htlc_conn *htlc, int expected);


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
		case CIPHER_BLOWFISH:
			/* NULL state means cipher_decode_init never ran or
			 * its allocation failed; either way the output qbuf
			 * (qbuf_set'd to `len` above) would be uninitialised
			 * heap and the rcv loop would parse garbage. Close
			 * the connection rather than leak garbage onto the
			 * receive path. */
			if (!htlc->cipher_decode_state.stream) {
				hx_htlc_close (htlc, 0);
				*inusedp = 0;
				return 0;
			}
			gtkhx_blowfish_ofb64_crypt(
			    (BlowfishOfb64State *)htlc->cipher_decode_state.stream,
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

int
cipher_check_rekey_marker (struct htlc_conn *htlc, u_int32_t *type_inout)
{
	if (!htlc || !type_inout) {
		return 0;
	}
	if (htlc->cipher_mode != CIPHER_MODE_STREAM
	    || htlc->cipher_decode_type == CIPHER_NONE
	    || (*type_inout >> 24) == 0) {
		return 0;
	}

	cipher_change_decode_key (htlc, *type_inout);
	*type_inout &= 0x00ffffff;
	return 1;
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
		case CIPHER_BLOWFISH:
			/* NULL state on the encode path is the more dangerous
			 * NULL: a no-op leaves plaintext in htlc->out and the
			 * socket-write loop sends it on a connection the user
			 * believes is HOPE-encrypted. Fail closed by tearing
			 * down the connection before any plaintext touches the
			 * wire. */
			if (!htlc->cipher_encode_state.stream) {
				hx_htlc_close (htlc, 0);
				return;
			}
			gtkhx_blowfish_ofb64_crypt(
			    (BlowfishOfb64State *)htlc->cipher_encode_state.stream,
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

	/* Stream-cipher path (Blowfish OFB). The rekey-on-random-
	 * nibble trick is the legacy HOPE behaviour: with probability
	 * 3/16, mark the header type byte and the next N rounds of
	 * HMAC-stretching for the cipher key (see
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
		case CIPHER_BLOWFISH:
			/* Re-key WITHOUT resetting the OFB ivec/num.
			 *
			 * cipher_encode_init is called twice in a
			 * connection's lifetime: at post-Step-2 setup (where
			 * the union was just zero-initialised — the .stream
			 * pointer is NULL, so we go through the
			 * gtkhx_blowfish_ofb64_new branch and the freshly-
			 * allocated state starts with ivec/num at the OFB
			 * block boundary), and again from
			 * cipher_change_encode_key every time the legacy
			 * HOPE per-message rekey marker fires (~3/16 of
			 * outgoing messages). The wire contract for the
			 * rekey case is "rotate the key schedule, KEEP the
			 * OFB ivec/num where they are" — mhxd does the same.
			 * gtkhx_blowfish_ofb64_set_key only rotates the key
			 * schedule, leaving ivec/num intact, so the existing
			 * state pointer path is the correct shape. An early
			 * Nettle-port revision added a memset of ivec/num
			 * here as defensive scrubbing; it desynced the
			 * cipher state every rekey, which the Tier 3
			 * test_hope_blowfish caught against mhxd. Don't
			 * reintroduce that. */
			if (htlc->cipher_encode_state.stream) {
				gtkhx_blowfish_ofb64_set_key(
				    (BlowfishOfb64State *)htlc->cipher_encode_state.stream,
				    htlc->cipher_encode_key,
				    htlc->cipher_encode_keylen);
			} else {
				htlc->cipher_encode_state.stream =
				    gtkhx_blowfish_ofb64_new(htlc->cipher_encode_key,
				                             htlc->cipher_encode_keylen);
			}
			break;
		default:
			break;
	}
}

void
cipher_decode_init (struct htlc_conn *htlc)
{
	switch (htlc->cipher_decode_type) {
		case CIPHER_BLOWFISH:
			/* See cipher_encode_init above for why we don't
			 * memset ivec/num here, and why we go through the
			 * new-vs-set_key branch. Same contract on the
			 * decode side — both peers compute the same
			 * keystream from the same shared key. */
			if (htlc->cipher_decode_state.stream) {
				gtkhx_blowfish_ofb64_set_key(
				    (BlowfishOfb64State *)htlc->cipher_decode_state.stream,
				    htlc->cipher_decode_key,
				    htlc->cipher_decode_keylen);
			} else {
				htlc->cipher_decode_state.stream =
				    gtkhx_blowfish_ofb64_new(htlc->cipher_decode_key,
				                             htlc->cipher_decode_keylen);
			}
			break;
		default:
			break;
	}
}
