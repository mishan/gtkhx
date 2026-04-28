/*
 * MAC computation for the Hotline HOPE handshake.
 *
 * Replaces the in-tree md5.c/sha.c with GLib's GChecksum / GHmac so we
 * can drop ~900 lines of vendored hash code. Behavior is preserved
 * byte-for-byte for the four algorithms anyone has ever sent on the
 * wire: SHA1, MD5, HMAC-SHA1, HMAC-MD5.
 *
 * The unprefixed "SHA1"/"MD5" branches compute a plain hash over
 * key||text — this is not RFC 2104 HMAC, but it is what the original
 * client did, and it's the construction some servers expect for the
 * pre-HOPE password challenge. Don't "fix" the construction; we
 * negotiate compatibility, not correctness.
 *
 * HAVAL was deleted in the previous commit (advertised by no server,
 * computed by no client).
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

u_int16_t
hmac_xxx(u_int8_t *md, u_int8_t *key, u_int32_t keylen,
         u_int8_t *text, u_int32_t textlen, u_int8_t *macalg)
{
	GChecksumType type;
	gsize digest_len;

	if (!strcmp((char *)macalg, "SHA1") || !strcmp((char *)macalg, "HMAC-SHA1")) {
		type = G_CHECKSUM_SHA1;
		digest_len = 20;
	} else if (!strcmp((char *)macalg, "MD5") || !strcmp((char *)macalg, "HMAC-MD5")) {
		type = G_CHECKSUM_MD5;
		digest_len = 16;
	} else {
		return 0;
	}

	if (!strncmp((char *)macalg, "HMAC-", 5)) {
		GHmac *hmac = g_hmac_new(type, key, keylen);
		gsize out_len = digest_len;

		g_hmac_update(hmac, text, textlen);
		g_hmac_get_digest(hmac, md, &out_len);
		g_hmac_unref(hmac);
	} else {
		/* Plain key||text hash — pre-HOPE challenge construction. */
		GChecksum *cs = g_checksum_new(type);
		gsize out_len = digest_len;

		g_checksum_update(cs, key, keylen);
		g_checksum_update(cs, text, textlen);
		g_checksum_get_digest(cs, md, &out_len);
		g_checksum_free(cs);
	}

	return (u_int16_t)digest_len;
}
