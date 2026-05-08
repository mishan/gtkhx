#ifndef HX_HL_ACCESS_H
#define HX_HL_ACCESS_H 1

/*
 * Hotline access-bit constants and accessor.
 *
 * Layout reconciled against mhxd/src/common/hotline.h's
 * struct hl_access_bits — that file is the canonical bit ordering
 * since it sits inside a working server implementation that interprets
 * the same wire bytes the live servers (hlserver.com, mhxd) emit.
 *
 * The 8-byte (64-bit) bitmap is big-endian: bit 0 is the MSB of byte 0,
 * bit 63 is the LSB of byte 7. Bits flagged "(reserved)" below are
 * gaps in the published spec — present in the mhxd struct only as
 * __reservedN bitfields. Don't assign new meanings to them; some
 * deployed servers may already use them privately.
 *
 * GtkHx historically captured the bitmap into htlc->access but never
 * looked at the contents. The new helpers here decode it so we can:
 *
 *   - skip the auto-news-fetch on connect when the account doesn't
 *     have read permission (otherwise the server responds with a
 *     task error and we toast "Uh, no." every login),
 *   - eventually grey out menu items the user isn't authorised to
 *     trigger (delete file, kick user, etc.) instead of letting them
 *     try and bounce off the server.
 *
 * Constant names mirror the mhxd field names so they're greppable
 * against the reference implementation.
 */

#include <glib.h>

/* ---- Bit numbers (0 = MSB of byte 0) -------------------------- */

/* Files (0-7) */
#define HL_ACCESS_DELETE_FILES        0
#define HL_ACCESS_UPLOAD_FILES        1
#define HL_ACCESS_DOWNLOAD_FILES      2
#define HL_ACCESS_RENAME_FILES        3
#define HL_ACCESS_MOVE_FILES          4
#define HL_ACCESS_CREATE_FOLDERS      5
#define HL_ACCESS_DELETE_FOLDERS      6
#define HL_ACCESS_RENAME_FOLDERS      7

/* Folders / chat / users (8-15) */
#define HL_ACCESS_MOVE_FOLDERS        8
#define HL_ACCESS_READ_CHAT           9
#define HL_ACCESS_SEND_CHAT          10
#define HL_ACCESS_CREATE_PCHATS      11
/* bits 12-13: reserved */
#define HL_ACCESS_CREATE_USERS       14
#define HL_ACCESS_DELETE_USERS       15

/* Users / news-classic / disconnect (16-23) */
#define HL_ACCESS_READ_USERS         16
#define HL_ACCESS_MODIFY_USERS       17
/* bits 18-19: reserved */
#define HL_ACCESS_READ_NEWS          20  /* HTLC_HDR_NEWS_GETFILE +
                                          * threaded news read */
#define HL_ACCESS_POST_NEWS          21
#define HL_ACCESS_DISCONNECT_USERS   22
#define HL_ACCESS_CANT_BE_DISCONNECTED 23

/* Misc (24-31) */
#define HL_ACCESS_GET_USER_INFO      24
#define HL_ACCESS_UPLOAD_ANYWHERE    25
#define HL_ACCESS_USE_ANY_NAME       26
#define HL_ACCESS_DONT_SHOW_AGREEMENT 27
#define HL_ACCESS_COMMENT_FILES      28
#define HL_ACCESS_COMMENT_FOLDERS    29
#define HL_ACCESS_VIEW_DROP_BOXES    30
#define HL_ACCESS_MAKE_ALIASES       31

/* 1.5+ news / folder transfers (32-39) */
#define HL_ACCESS_CAN_BROADCAST      32
#define HL_ACCESS_DELETE_ARTICLES    33
#define HL_ACCESS_CREATE_CATEGORIES  34
#define HL_ACCESS_DELETE_CATEGORIES  35
#define HL_ACCESS_CREATE_NEWS_BUNDLES 36
#define HL_ACCESS_DELETE_NEWS_BUNDLES 37
#define HL_ACCESS_UPLOAD_FOLDERS     38
#define HL_ACCESS_DOWNLOAD_FOLDERS   39

/* Private messages (40) */
#define HL_ACCESS_SEND_MSGS          40

/* Bits 41-63: reserved by mhxd struct (__reserved2/3/4). */

/* ---- Accessor -------------------------------------------------- */

/* Return TRUE if `bit` is set in the 8-byte big-endian bitmap.
 * Out-of-range bits return FALSE. */
static inline gboolean
hl_access_has (const guint8 *access, int bit)
{
	if (bit < 0 || bit >= 64)
		return FALSE;
	return (access[bit >> 3] & (0x80 >> (bit & 7))) != 0;
}

#endif /* HX_HL_ACCESS_H */
