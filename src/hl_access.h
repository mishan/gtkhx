#ifndef HX_HL_ACCESS_H
#define HX_HL_ACCESS_H 1

/*
 * Hotline access-bit constants and accessor.
 *
 * The server's HTLS_HDR_USER_SELFINFO message carries an 8-byte
 * (64-bit) bitmap of permission flags for the current account. Bit
 * positions are big-endian: bit 0 is the MSB of byte 0, bit 63 is
 * the LSB of byte 7. The original Hotline 1.x spec assigned the
 * lower 32 bits; Hotline 1.5 added another batch in bits 32-63 for
 * the threaded-news system.
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
 * The constant names match the canonical hxd / Hotline-1.5 spec
 * naming so they're greppable against any other reference codebase.
 */

#include <glib.h>

/* ---- Bit numbers (0 = MSB of byte 0) -------------------------- */

/* Files (0-7) */
#define HL_ACCESS_DELETE_FILE        0
#define HL_ACCESS_UPLOAD_FILE        1
#define HL_ACCESS_DOWNLOAD_FILE      2
#define HL_ACCESS_RENAME_FILE        3
#define HL_ACCESS_MOVE_FILE          4
#define HL_ACCESS_CREATE_FOLDER      5
#define HL_ACCESS_DELETE_FOLDER      6
#define HL_ACCESS_RENAME_FOLDER      7

/* Folders / chat (8-15) */
#define HL_ACCESS_MOVE_FOLDER        8
#define HL_ACCESS_READ_CHAT          9
#define HL_ACCESS_SEND_CHAT         10
#define HL_ACCESS_CREATE_CHAT       11
#define HL_ACCESS_CLOSE_CHAT        12
#define HL_ACCESS_SHOW_IN_LIST      13
#define HL_ACCESS_CREATE_USER       14
#define HL_ACCESS_DELETE_USER       15

/* Users / news-classic (16-23) */
#define HL_ACCESS_OPEN_USER         16
#define HL_ACCESS_MODIFY_USER       17
#define HL_ACCESS_CHANGE_OWN_PASS   18
#define HL_ACCESS_SEND_PRIV_MSG     19
#define HL_ACCESS_NEWS_READ_ART     20  /* legacy news — HTLC_HDR_NEWS_GETFILE */
#define HL_ACCESS_NEWS_POST_ART     21
#define HL_ACCESS_DISCONNECT_USER   22
#define HL_ACCESS_CANNOT_BE_DISC    23

/* Misc (24-31) */
#define HL_ACCESS_GET_CLIENT_INFO   24
#define HL_ACCESS_UPLOAD_ANYWHERE   25
#define HL_ACCESS_USE_ANY_NAME      26
#define HL_ACCESS_DONT_SHOW_AGREE   27
#define HL_ACCESS_SET_FILE_COMMENT  28
#define HL_ACCESS_SET_FOLDER_COMMENT 29
#define HL_ACCESS_VIEW_DROP_BOXES   30
#define HL_ACCESS_MAKE_ALIAS        31

/* 1.5+ extensions (32+) */
#define HL_ACCESS_BROADCAST         32
#define HL_ACCESS_NEWS_READ_FOLDER  33  /* threaded news — HTLC_HDR_NEWSDIRLIST */
#define HL_ACCESS_NEWS_POST_THREAD  34
#define HL_ACCESS_NEWS_DELETE_THREAD 35
#define HL_ACCESS_NEWS_CREATE_CAT   36
#define HL_ACCESS_NEWS_DELETE_CAT   37

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
