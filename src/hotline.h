#ifndef _HOTLINE_H
#define _HOTLINE_H

#if defined(__GNUC__) && !defined(__STRICT_ANSI__)
#define ZERO_SIZE_ARRAY_SIZE 0
#else
#define ZERO_SIZE_ARRAY_SIZE 1
#endif

struct hl_hdr {
    guint32 type PACKED, trans PACKED;
    guint32 flag PACKED;
    guint32 len PACKED, len2 PACKED;
    guint16 hc PACKED;
    guint8 data[ZERO_SIZE_ARRAY_SIZE];
};

struct hl_data_hdr {
    guint16 type PACKED, len PACKED;
    guint8 data[ZERO_SIZE_ARRAY_SIZE];
};

struct htxf_hdr {
    guint32 magic, ref, len, unknown;
};

struct hl_filelist_hdr {
    guint16 type PACKED, len PACKED;
    guint32 ftype PACKED, fcreator PACKED;
    guint32 fsize PACKED, unknown PACKED, fnlen PACKED;
    guint8 fname[ZERO_SIZE_ARRAY_SIZE];
};

struct hl_userlist_hdr {
    guint16 type PACKED, len PACKED;
    guint16 uid PACKED, icon PACKED, color PACKED, nlen PACKED;
    guint8 name[ZERO_SIZE_ARRAY_SIZE];
};

struct htrk_hdr {
    guint16 version;
    guint16 port;
    guint16 nusers;
    guint16 __reserved0;
    guint32 id;
};

typedef guint64 hl_access_bits;

struct hl_user_data {
    guint32 magic;
    hl_access_bits access;
    guint8 pad[516];
    guint16 nlen;
    guint8 name[134];
    guint16 llen;
    guint8 login[34];
    guint16 plen;
    guint8 password[32];
};

#define HTLC_MAGIC "TRTPHOTL\0\1\0\2"
#define HTLC_MAGIC_LEN 12
#define HTLS_MAGIC "TRTP\0\0\0\0"
#define HTLS_MAGIC_LEN 8
#define HTRK_MAGIC "HTRK\0\1"
#define HTRK_MAGIC_LEN 6
#define HTXF_MAGIC "HTXF"
#define HTXF_MAGIC_LEN 4
#define HTXF_MAGIC_INT 0x48545846

/* HTXF subchannel transfer types. Carried in the last 4 bytes of
 * the 16-byte HTXF magic header (`unknown` u32 in struct
 * htxf_hdr, layered as u16 type + u16 reserved on the wire).
 * mhxd resolves the kind of transfer server-side by matching the
 * inbound HTXF connection's ref to a pre-created htxf_conn that
 * already has its ->type set, so for mhxd the wire field is
 * advisory. Mac-native servers use it to dispatch the subchannel
 * into the right framing, so sending the wrong value here means
 * the server interprets a folder transfer as a single-file
 * transfer and waits forever for FILP framing while we send
 * FILE_NEXT — looks like a hang from both ends. */
#define HTXF_TYPE_FILE 0
#define HTXF_TYPE_FOLDER 1
#define HTXF_TYPE_BANNER 2

#define HTRK_TCPPORT 5498
#define HTRK_UDPPORT 5499
#define HTLS_TCPPORT 5500
#define HTXF_TCPPORT 5501

#define HTLC_HDR_NEWS_GETFILE ((guint32)0x00000065)
#define HTLC_HDR_NEWS_POST ((guint32)0x00000067)
#define HTLC_HDR_CHAT ((guint32)0x00000069)
#define HTLC_HDR_LOGIN ((guint32)0x0000006b)
#define HTLC_HDR_MSG ((guint32)0x0000006c)
#define HTLC_HDR_USER_KICK ((guint32)0x0000006e)
#define HTLC_HDR_CHAT_CREATE ((guint32)0x00000070)
#define HTLC_HDR_CHAT_INVITE ((guint32)0x00000071)
#define HTLC_HDR_CHAT_DECLINE ((guint32)0x00000072)
#define HTLC_HDR_CHAT_JOIN ((guint32)0x00000073)
#define HTLC_HDR_CHAT_PART ((guint32)0x00000074)
#define HTLC_HDR_CHAT_SUBJECT ((guint32)0x00000078)
#define HTLC_HDR_FILE_LIST ((guint32)0x000000c8)
#define HTLC_HDR_FILE_GET ((guint32)0x000000ca)
#define HTLC_HDR_FILE_PUT ((guint32)0x000000cb)
#define HTLC_HDR_FILE_DELETE ((guint32)0x000000cc)
#define HTLC_HDR_FILE_MKDIR ((guint32)0x000000cd)
#define HTLC_HDR_FILE_GETINFO ((guint32)0x000000ce)
#define HTLC_HDR_FILE_SETINFO ((guint32)0x000000cf)
#define HTLC_HDR_FILE_MOVE ((guint32)0x000000d0)
#define HTLC_HDR_FILE_SYMLINK ((guint32)0x000000d1)
/* Folder transfer opcodes — 1.5+. Wire format is a stream of
 * next_file_info records over an HTXF subchannel driven by the
 * receiver via FILE_NEXT / FILE_SEND / FILE_RESUME commands; see
 * src/xfers.c::folder_get_thread for the full state machine and
 * memory/gtkhx_folder_xfer_protocol.md for cross-references into
 * mhxd's folder_send / folder_recv. */
#define HTLC_HDR_FILE_GETFOLDER ((guint32)0x000000d2)
#define HTLC_HDR_FILE_PUTFOLDER ((guint32)0x000000d5)
#define HTLC_HDR_USER_GETLIST ((guint32)0x0000012c)
#define HTLC_HDR_USER_GETINFO ((guint32)0x0000012f)
#define HTLC_HDR_USER_CHANGE ((guint32)0x00000130)
#define HTLC_HDR_ACCOUNT_CREATE ((guint32)0x0000015e)
#define HTLC_HDR_ACCOUNT_DELETE ((guint32)0x0000015f)
#define HTLC_HDR_ACCOUNT_READ ((guint32)0x00000160)
#define HTLC_HDR_ACCOUNT_MODIFY ((guint32)0x00000161)
#define HTLC_HDR_MSG_BROADCAST ((guint32)0x00000163)

/* 1.5+ Only */
#define HTLC_HDR_NEWSDIRLIST ((guint32)0x00000172)
#define HTLC_HDR_NEWSCATLIST ((guint32)0x00000173)
#define HTLC_HDR_DELNEWSDIRCAT ((guint32)0x0000017c)
#define HTLC_HDR_MAKENEWSDIR ((guint32)0x0000017d)
#define HTLC_HDR_MAKECATEGORY ((guint32)0x0000017e)
#define HTLC_HDR_GETTHREAD ((guint32)0x00000190)
#define HTLC_HDR_POSTTHREAD ((guint32)0x0000019a)
#define HTLC_HDR_DELETETHREAD ((guint32)0x0000019b)

/* Phase 5: opcodes adopted from mhxd's protocol additions
 * (mhxd/src/common/hotline.h). Same wire values both ends agree on;
 * old (1.0/1.2) servers will reject the unknown header and the
 * client recovers gracefully. */
#define HTLC_HDR_PING ((guint32)0x000001f4)
#define HTLS_HDR_PING ((guint32)0x000001f4)
#define HTLC_HDR_AGREEMENTAGREE ((guint32)0x00000079)
#define HTLC_HDR_KILLDOWNLOAD ((guint32)0x000000d6)

#define HTLC_DATA_CHAT ((guint16)0x0065)
#define HTLC_DATA_MSG ((guint16)0x0065)
#define HTLC_DATA_NEWS_POST ((guint16)0x0065)
#define HTLC_DATA_NAME ((guint16)0x0066)
#define HTLC_DATA_UID ((guint16)0x0067)
#define HTLC_DATA_ICON ((guint16)0x0068)
#define HTLC_DATA_LOGIN ((guint16)0x0069)
#define HTLC_DATA_PASSWORD ((guint16)0x006a)
#define HTLC_DATA_HTXF_SIZE ((guint16)0x006c)
#define HTLC_DATA_STYLE ((guint16)0x006d)
#define HTLC_DATA_ACCESS ((guint16)0x006e)
#define HTLC_DATA_BAN ((guint16)0x0071)
/* HTLC_DATA_OPTIONS — same code point as HTLC_DATA_BAN above;
 * mhxd named it BAN and ignored it on parse, but the Hotline 1.5+
 * spec (and Mobius's HandleTranAgreed) treats this chunk as an
 * OPTIONS bitmap on AGREEMENTAGREE:
 *
 *   bit 0  HTLC_OPT_REFUSE_PM        refuse private messages
 *   bit 1  HTLC_OPT_REFUSE_CHAT      refuse private chat
 *   bit 2  HTLC_OPT_AUTO_RESPONSE    auto-response (paired with a
 *                                    follow-on autoresponse chunk)
 *
 * Mobius reads the chunk's body as a big-endian u16 and PANICS if
 * the chunk is missing (binary.BigEndian.Uint16 on a nil slice).
 * Its dontPanic recover then exits the connection goroutine, so
 * the client sees a silent drop. Servers running Mobius (Classic
 * Macs Hotline, MacSecret, vespernet, …) all behave this way.
 * Defining the alias under both names so existing BAN call sites
 * stay correct while new code can spell out the OPTIONS semantic. */
#define HTLC_DATA_OPTIONS ((guint16)0x0071)
#define HTLC_OPT_REFUSE_PM ((guint16)0x0001)
#define HTLC_OPT_REFUSE_CHAT ((guint16)0x0002)
#define HTLC_OPT_AUTO_RESPONSE ((guint16)0x0004)
#define HTLC_DATA_CHAT_ID ((guint16)0x0072)
/* mhxd extension. Sent in HTLC_HDR_LOGIN to advertise the client's
 * Hotline-protocol version. mhxd uses values >= 150 as a "modern
 * client" gate that unlocks features like HTLC_HDR_PING acceptance
 * (see mhxd/src/hxd/rcv.c around the can_ping flag set in
 * rcv_login). GtkHx itself doesn't send this in its production
 * login path today; the integration test harness sends it so the
 * Tier 3 PING test exercises the modern-client codepath. */
#define HTLC_DATA_CLIENTVERSION ((guint16)0x00a0)

/* DATA_CAPABILITIES (0x01f0) — session capability bitmask sent in
 * LOGIN by clients that support modern protocol extensions, and
 * echoed back in the LOGIN reply by the server for the bits it
 * agrees to enable for the session.
 *
 * The mechanism is bitmask-based so multiple extensions can be
 * negotiated in one chunk:
 *
 *   bit 0  CAP_LARGE_FILES     64-bit file sizes (separate spec)
 *   bit 1  CAP_TEXT_ENCODING   UTF-8 for all string data
 *   bit 2  CAP_VOICE           voice chat (WebRTC SFU)
 *   bit 3  CAP_INLINE_MEDIA    inline image attachments
 *   bit 4  CAP_CHAT_HISTORY    server-side chat-history retrieval
 *   bit 5  CAP_EXTENDED_PRIV   128-bit access bitmap
 *
 * Wire field is a big-endian unsigned integer; spec says variable
 * width, "typically 2 bytes, expandable to 8 bytes." We send 2
 * bytes today (bits 0–5 fit). Servers that don't recognise the
 * field per-spec ignore it and the session falls back to standard
 * mode — sending it is safe against legacy servers.
 *
 * Source: fogWraith/Hotline Docs/Protocol/Capabilities.md and
 * Capabilities-Text-Encoding.md. */
#define HTLC_DATA_CAPABILITIES ((guint16)0x01f0)
#define HTLS_DATA_CAPABILITIES ((guint16)0x01f0)
#define HTLC_CAP_LARGE_FILES ((guint16)0x0001)
#define HTLC_CAP_TEXT_ENCODING ((guint16)0x0002)
#define HTLC_CAP_VOICE ((guint16)0x0004)
#define HTLC_CAP_INLINE_MEDIA ((guint16)0x0008)
#define HTLC_CAP_CHAT_HISTORY ((guint16)0x0010)
#define HTLC_CAP_EXTENDED_PRIV ((guint16)0x0020)

/* 64-bit companion fields for the Large-File extension. Sent
 * alongside the legacy 32-bit chunks in large-file mode; legacy
 * field is clamped to 0xFFFFFFFF when the real value overflows
 * 32 bits. Receivers should prefer the 64-bit value when both
 * are present.
 *
 * Source: fogWraith/Hotline Docs/Protocol/Capabilities-Large-File.md.
 */
#define HTLC_DATA_FILESIZE64 ((guint16)0x01f1)
#define HTLS_DATA_FILESIZE64 ((guint16)0x01f1)
#define HTLC_DATA_OFFSET64 ((guint16)0x01f2)
#define HTLS_DATA_OFFSET64 ((guint16)0x01f2)
#define HTLC_DATA_XFERSIZE64 ((guint16)0x01f3)
#define HTLS_DATA_XFERSIZE64 ((guint16)0x01f3)
#define HTLC_DATA_FOLDER_ITEM_COUNT64 ((guint16)0x01f4)
#define HTLS_DATA_FOLDER_ITEM_COUNT64 ((guint16)0x01f4)

/* Chat-history extension (fogWraith
 * Capabilities-Chat-History.md). Wire-side gated by
 * HTLC_CAP_CHAT_HISTORY (bit 4) in DATA_CAPABILITIES.
 *
 * Fields in the 0x0F01–0x0F1F range follow the HOPE/extension
 * convention of placing protocol extensions in the high field-ID
 * space; gaps after 0x0F08 are reserved for future channel-
 * management opcodes (channel name, topic, listing). */
#define HTLC_DATA_CHANNEL_ID ((guint16)0x0f01)
#define HTLS_DATA_CHANNEL_ID ((guint16)0x0f01)
#define HTLC_DATA_HISTORY_BEFORE ((guint16)0x0f02)
#define HTLS_DATA_HISTORY_BEFORE ((guint16)0x0f02)
#define HTLC_DATA_HISTORY_AFTER ((guint16)0x0f03)
#define HTLS_DATA_HISTORY_AFTER ((guint16)0x0f03)
#define HTLC_DATA_HISTORY_LIMIT ((guint16)0x0f04)
#define HTLS_DATA_HISTORY_LIMIT ((guint16)0x0f04)
/* DATA_HISTORY_ENTRY: a single chat history entry, packed binary.
 * Repeated 0..N times in a GET_CHAT_HISTORY reply. Layout in
 * src/history.{c,h}. */
#define HTLS_DATA_HISTORY_ENTRY ((guint16)0x0f05)
/* DATA_HISTORY_HAS_MORE: uint8, 1 = more results exist beyond the
 * returned batch in the cursor's direction. */
#define HTLS_DATA_HISTORY_HAS_MORE ((guint16)0x0f06)
/* DATA_HISTORY_MAX_MSGS / _MAX_DAYS: server retention hints sent in
 * the LOGIN reply alongside the echoed DATA_CAPABILITIES. 0 means
 * unlimited; absent means the server didn't disclose. */
#define HTLS_DATA_HISTORY_MAX_MSGS ((guint16)0x0f07)
#define HTLS_DATA_HISTORY_MAX_DAYS ((guint16)0x0f08)

/* HTXF handshake flags. Default (0x00) is the 16-byte legacy
 * handshake. In large-file mode the handshake grows to 24 bytes
 * with an 8-byte big-endian length field appended.
 *
 *   HTXF_FLAG_LARGE_FILE   large-file mode active for this xfer
 *   HTXF_FLAG_SIZE64       8-byte length follows the 16-byte hdr;
 *                          only set when LARGE_FILE is set AND
 *                          the transfer size exceeds 32 bits.
 *                          When set, the legacy 32-bit length
 *                          (bytes 8-11) is zeroed to prevent a
 *                          legacy peer from mis-reading the
 *                          transfer.
 *
 * Source: fogWraith/Hotline Docs/Protocol/Capabilities-Large-File.md
 * section "Handshake Flags and Length". */
#define HTXF_FLAG_LARGE_FILE ((guint32)0x00000001)
#define HTXF_FLAG_SIZE64 ((guint32)0x00000002)

#define HTLC_DATA_CHAT_SUBJECT ((guint16)0x0073)
#define HTLC_DATA_FILE_NAME ((guint16)0x00c9)
#define HTLC_DATA_DIR ((guint16)0x00ca)
#define HTLC_DATA_RFLT ((guint16)0x00cb)
#define HTLC_DATA_FILE_PREVIEW ((guint16)0x00cc)
/* Aggregate file count carried in HTLC_HDR_FILE_PUTFOLDER. mhxd
 * also reads this as HTLS_DATA_FILE_NFILES on the GETFOLDER reply
 * — same numeric type code (0xdc), different naming convention
 * for which side is the sender. */
#define HTLC_DATA_FILE_NFILES ((guint16)0x00dc)
#define HTLC_DATA_FILE_COMMENT ((guint16)0x00d2)
#define HTLC_DATA_FILE_RENAME ((guint16)0x00d3)
#define HTLC_DATA_DIR_RENAME ((guint16)0x00d4)

#define HTLC_DATA_NEWSFOLDERITEM ((guint16)0x0140)
#define HTLC_DATA_CATLIST ((guint16)0x0141)
#define HTLC_DATA_CATEGORY ((guint16)0x0142)
#define HTLC_DATA_CATEGORYITEM ((guint16)0x0143)
#define HTLC_DATA_NEWSPATH ((guint16)0x0145)
#define HTLC_DATA_THREADID ((guint16)0x0146)
#define HTLC_DATA_NEWSTYPE ((guint16)0x0147)
#define HTLC_DATA_NEWSSUBJECT ((guint16)0x0148)
#define HTLC_DATA_NEWSAUTHOR ((guint16)0x0149)
#define HTLC_DATA_NEWSDATE ((guint16)0x014a)
#define HTLC_DATA_NEWSDIR ((guint16)0x01c8)

#define HTLC_DATA_PREVTHREAD ((guint16)0x014b)
#define HTLC_DATA_NEXTTHREAD ((guint16)0x014c)
#define HTLC_DATA_NEWSDATA ((guint16)0x014d)
#define HTLC_DATA_PARENTTHREAD ((guint16)0x014e)
#define HTLC_DATA_PARENT_POST ((guint16)0x014f)
#define HTLC_DATA_CHILD_POST ((guint16)0x0150)

#define HTLS_HDR_NEWS_POST ((guint32)0x00000066)
#define HTLS_HDR_MSG ((guint32)0x00000068)
#define HTLS_HDR_CHAT ((guint32)0x0000006a)
#define HTLS_HDR_AGREEMENT ((guint32)0x0000006d)
#define HTLS_HDR_POLITEQUIT ((guint32)0x0000006f)
#define HTLS_HDR_CHAT_INVITE ((guint32)0x00000071)
#define HTLS_HDR_CHAT_USER_CHANGE ((guint32)0x00000075)
#define HTLS_HDR_CHAT_USER_PART ((guint32)0x00000076)
#define HTLS_HDR_CHAT_SUBJECT ((guint32)0x00000077)
/* HTLS_HDR_BANNER (0x7a) — sent unsolicited after login when the
 * server config has banner.type set. Carries HTLS_DATA_BANNER_TYPE
 * (4 bytes) and optionally HTLS_DATA_BANNER_URL. GtkHx currently
 * doesn't read this; the integration suite uses it to verify the
 * post-login broadcast train. */
#define HTLS_HDR_BANNER ((guint32)0x0000007a)
#define HTLS_DATA_BANNER_TYPE ((guint16)0x0098)
#define HTLS_DATA_BANNER_URL ((guint16)0x0099)

/* HTLC_HDR_DOWNLOAD_BANNER (0xd4 = 212, myTran_DownloadBanner per
 * the Hotline 1.9 spec) — sent by the client after receiving
 * HTLS_HDR_BANNER without a URL chunk. The server replies with a
 * TASK carrying HTLS_DATA_HTXF_REF + HTLS_DATA_HTXF_SIZE; the
 * client opens base_port+1, sends the 16-byte HTXF header and
 * reads `size` bytes of banner image (GIFf / JPEG / ...). No
 * parameters in the request itself. */
#define HTLC_HDR_DOWNLOAD_BANNER ((guint32)0x000000d4)
#define HTLS_HDR_USER_CHANGE ((guint32)0x0000012d)
#define HTLS_HDR_USER_PART ((guint32)0x0000012e)
#define HTLS_HDR_USER_SELFINFO ((guint32)0x00000162)
#define HTLS_HDR_MSG_BROADCAST ((guint32)0x00000163)
#define HTLS_HDR_TASK ((guint32)0x00010000)
#define HTLS_HDR_QUEUE ((guint32)0x000000d3)

#define HTLS_DATA_TASKERROR ((guint16)0x0064)
#define HTLS_DATA_NEWS ((guint16)0x0065)
#define HTLS_DATA_AGREEMENT ((guint16)0x0065)
#define HTLS_DATA_USER_INFO ((guint16)0x0065)
#define HTLS_DATA_CHAT ((guint16)0x0065)
#define HTLS_DATA_MSG ((guint16)0x0065)
#define HTLS_DATA_NAME ((guint16)0x0066)
#define HTLS_DATA_UID ((guint16)0x0067)
#define HTLS_DATA_ICON ((guint16)0x0068)
#define HTLS_DATA_LOGIN ((guint16)0x0069)
#define HTLS_DATA_PASSWORD ((guint16)0x006a)
#define HTLS_DATA_HTXF_REF ((guint16)0x006b)
#define HTLS_DATA_HTXF_SIZE ((guint16)0x006c)
#define HTLS_DATA_STYLE ((guint16)0x006d)
#define HTLS_DATA_ACCESS ((guint16)0x006e)
#define HTLS_DATA_COLOUR ((guint16)0x0070)
#define HTLS_DATA_CHAT_ID ((guint16)0x0072)

/* Colored-Nicknames extension. 32-bit unsigned, big-endian
 * on the wire, packed as 0x00RRGGBB (high byte reserved for future
 * use, should be zero). 0xFFFFFFFF or absence = "no color, use the
 * client's default rendering".
 *
 * Appears in HTLS_HDR_USER_CHANGE (301), HTLS_HDR_CHAT_USER_CHANGE
 * (117), HTLS_HDR_USER_SELFINFO (server → client), and in HTLC_HDR
 * _USER_CHANGE (304, client → server) when the client sets its own
 * color. No explicit capability bit — auto-opt-in: the server marks
 * a session as color-aware the first time it receives a DATA_COLOR
 * from that client. See Docs/Protocol/Colored-Nicknames.md in the
 * fogWraith Hotline repo for the full spec.
 *
 * Same code point for both directions, distinct from the legacy
 * HTLS_DATA_COLOUR (0x0070) which encodes the u16 status bitmap
 * (Admin/Guest/Away). */
#define HTLC_DATA_COLOR ((guint16)0x0500)
#define HTLS_DATA_COLOR ((guint16)0x0500)
#define HX_NICK_COLOR_NONE ((guint32)0xffffffff)
#define HTLS_DATA_CHAT_SUBJECT ((guint16)0x0073)
#define HTLS_DATA_FILE_LIST ((guint16)0x00c8)
#define HTLS_DATA_FILE_NAME ((guint16)0x00c9)
#define HTLS_DATA_RFLT ((guint16)0x00cb)
#define HTLS_DATA_FILE_TYPE ((guint16)0x00cd)
#define HTLS_DATA_FILE_CREATOR ((guint16)0x00ce)
#define HTLS_DATA_FILE_SIZE ((guint16)0x00cf)
#define HTLS_DATA_FILE_DATE_CREATE ((guint16)0x00d0)
#define HTLS_DATA_FILE_DATE_MODIFY ((guint16)0x00d1)
#define HTLS_DATA_FILE_COMMENT ((guint16)0x00d2)
#define HTLS_DATA_FILE_ICON ((guint16)0x00d5)
#define HTLS_DATA_FILE_NFILES ((guint16)0x00dc)
#define HTLS_DATA_USER_LIST ((guint16)0x012c)

/* 1.5+ only */
#define HTLS_DATA_QUEUE ((guint16)0x0074)
#define HTLS_DATA_VERSION ((guint16)0x00a0)
#define HTLS_DATA_SERVERNAME ((guint16)0x00a2)
#define HTLS_DATA_NOAGREEMENT ((guint16)0x009a)

#define HTLC_HDR_ICON_GET ((guint32)0x00000e90)
#define HTLC_HDR_FILE_HASH ((guint32)0x00000ee0)

/* HTLC_HDR_GET_CHAT_HISTORY = 700: chat-history extension request.
 * Request fields (all DATA_CHANNEL_ID, DATA_HISTORY_BEFORE,
 * DATA_HISTORY_AFTER, DATA_HISTORY_LIMIT). Reply contains
 * 0..N DATA_HISTORY_ENTRY chunks plus DATA_HISTORY_HAS_MORE.
 * See fogWraith Capabilities-Chat-History.md and src/history.h. */
#define HTLC_HDR_GET_CHAT_HISTORY ((guint32)0x000002bc) /* 700 */
#define HTLS_HDR_GET_CHAT_HISTORY ((guint32)0x000002bc)

#define HTLC_DATA_HASH_MD5 ((guint16)0x0e80)
#define HTLC_DATA_HASH_HAVAL ((guint16)0x0e81)
#define HTLC_DATA_HASH_SHA1 ((guint16)0x0e82)
#define HTLC_DATA_CHAT_AWAY ((guint16)0x0ea1)

#define HTLS_DATA_HASH_MD5 ((guint16)0x0e80)
#define HTLS_DATA_HASH_HAVAL ((guint16)0x0e81)
#define HTLS_DATA_HASH_SHA1 ((guint16)0x0e82)
#define HTLS_DATA_ICON_CICN ((guint16)0x0e90)

/* HOPE */
/* HOPE-Secure-Login identification chunks. App ID/String are the
 * only mechanism in the Hotline protocol for a client to advertise
 * its application name and version to the server — without these
 * we look identical to base hx in server-side stats. App ID is a
 * 4-byte OSType (we use "GTKx"); App String is free-form (app name
 * and version). */
#define HTLC_DATA_HOPE_APP_ID ((guint16)0x0e01)
#define HTLC_DATA_HOPE_APP_STRING ((guint16)0x0e02)
#define HTLS_DATA_HOPE_APP_ID ((guint16)0x0e01)
#define HTLS_DATA_HOPE_APP_STRING ((guint16)0x0e02)
#define HTLS_DATA_SESSIONKEY ((guint16)0x0e03)
#define HTLC_DATA_SESSIONKEY ((guint16)0x0e03)
#define HTLS_DATA_MAC_ALG ((guint16)0x0e04)
#define HTLC_DATA_MAC_ALG ((guint16)0x0e04)

/* cipher */
#define HTLS_DATA_CIPHER_ALG ((u_int16_t)0x0ec1)
#define HTLC_DATA_CIPHER_ALG ((u_int16_t)0x0ec2)
#define HTLS_DATA_CIPHER_MODE ((u_int16_t)0x0ec3)
#define HTLC_DATA_CIPHER_MODE ((u_int16_t)0x0ec4)
#define HTLS_DATA_CIPHER_IVEC ((u_int16_t)0x0ec5)
#define HTLC_DATA_CIPHER_IVEC ((u_int16_t)0x0ec6)

/* compress */
#define HTLS_DATA_CHECKSUM_ALG ((guint16)0x0ec7)
#define HTLC_DATA_CHECKSUM_ALG ((guint16)0x0ec8)
#define HTLS_DATA_COMPRESS_ALG ((guint16)0x0ec9)
#define HTLC_DATA_COMPRESS_ALG ((guint16)0x0eca)

#define SIZEOF_HL_HDR (22)
#define SIZEOF_HL_DATA_HDR (4)
#define SIZEOF_HL_FILELIST_HDR (24)
#define SIZEOF_HL_USERLIST_HDR (12)
#define SIZEOF_HTXF_HDR (16)
#define SIZEOF_HTRK_HDR (12)

#endif /* ndef _HOTLINE_H */
