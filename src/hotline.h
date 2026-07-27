#ifndef GTKHX_HOTLINE_H
#define GTKHX_HOTLINE_H

#include <stdint.h>

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

/* Tracker protocol v3.
 *
 * v1 handshake is 6 bytes: "HTRK" + u16 version (0x0001). v3 extends
 * that to 8 bytes by appending a 2-byte feature-flag bitmask. A
 * v3-spec-compliant tracker reads 6 bytes first, looks at the
 * version, then conditionally reads 2 more if it's 0x0003 (and only
 * if it actually implements v3).
 *
 * Pre-spec v1 trackers in the wild (hxtrackd, hltracker.com, every
 * hxd-derived tracker we've tested) do NOT do this. They memcmp the
 * full 6-byte HTRK_MAGIC against "HTRK\0\1" and silently fall
 * through when byte 5 is 0x03 instead of 0x01 — the connection
 * stays open with no response. The probe-then-fallback in
 * network.c handles this: send the 8-byte v3 magic with a 2-second
 * read watchdog, fall back to a fresh connection sending the
 * 6-byte v1 magic if no response arrives. */
#define HTRK_V3_MAGIC_PREFIX  "HTRK"           /* 4 bytes; version u16 BE follows */
#define HTRK_V3_HANDSHAKE_LEN 8                /* full client-side handshake */
#define HTRK_VERSION_V1       ((guint16) 0x0001)
#define HTRK_VERSION_V2       ((guint16) 0x0002)
#define HTRK_VERSION_V3       ((guint16) 0x0003)

/* v3 handshake feature-flag bits (u16 BE in the trailing 2 bytes).
 * Client-offered + tracker-offered; the negotiated set is the AND of
 * the two. Bits 5–15 are reserved and MUST be zero (and MUST be
 * ignored on receipt). */
#define HTRK_V3_FEAT_IPV6        ((guint16) 0x0001)
#define HTRK_V3_FEAT_QUERY       ((guint16) 0x0002)
#define HTRK_V3_FEAT_CLIENT_AUTH ((guint16) 0x0004)
#define HTRK_V3_FEAT_REG_ACK     ((guint16) 0x0008)
#define HTRK_V3_FEAT_HMAC        ((guint16) 0x0010)

/* v3 listing-request type. Sent by the client after the handshake
 * negotiation completes; takes a u16 BE type field, a u16 BE
 * field-count, then field_count TLVs (search text, pagination). */
#define HTRK_V3_REQ_LIST       ((guint16) 0x0001)

/* v3 listing-response header type. Tracker replies with this u16 BE
 * followed by u32 BE total_size, u16 BE total_servers, u16 BE
 * record_count, then record_count server records back-to-back. */
#define HTRK_V3_RESP_LIST      ((guint16) 0x0001)
#define HTRK_V3_RESP_HDR_LEN   10              /* type(2)+size(4)+total(2)+rec(2) */

/* Server-record address-type discriminator (first byte of each
 * record in a v3 response). */
#define HTRK_V3_ADDR_IPV4      ((guint8) 0x04) /* 4 bytes follow */
#define HTRK_V3_ADDR_IPV6      ((guint8) 0x06) /* 16 bytes follow */
#define HTRK_V3_ADDR_HOSTNAME  ((guint8) 0x48) /* u16 BE length + UTF-8 bytes */

/* Listing-request TLV IDs (query parameters). Phase C tees the
 * search-entry text into SEARCH_TEXT and may add pagination. */
#define HTRK_V3_TLV_SEARCH_TEXT  ((guint16) 0x1001)
#define HTRK_V3_TLV_PAGE_OFFSET  ((guint16) 0x1010)
#define HTRK_V3_TLV_PAGE_LIMIT   ((guint16) 0x1011)

/* Server-record TLV IDs the client cares about. Each is OPTIONAL in
 * the trailer; unknown IDs MUST be silently ignored (forward-compat
 * escape hatch). Phase A walks the trailer to advance the parse but
 * doesn't surface these yet; Phase B will. */
#define HTRK_V3_TLV_ADDRESS_IPV6      ((guint16) 0x0100)
#define HTRK_V3_TLV_HOSTNAME          ((guint16) 0x0101)
#define HTRK_V3_TLV_SERVER_SOFTWARE   ((guint16) 0x0200)
#define HTRK_V3_TLV_COUNTRY_CODE      ((guint16) 0x0201)
#define HTRK_V3_TLV_REGION            ((guint16) 0x0202)
#define HTRK_V3_TLV_LANGUAGE          ((guint16) 0x0203)
#define HTRK_V3_TLV_MAX_USERS         ((guint16) 0x0204)
#define HTRK_V3_TLV_MATURITY          ((guint16) 0x0205)
#define HTRK_V3_TLV_UPTIME            ((guint16) 0x0206)
#define HTRK_V3_TLV_RULES_URL         ((guint16) 0x0207)
#define HTRK_V3_TLV_BANNER_URL        ((guint16) 0x0208)
#define HTRK_V3_TLV_ICON_URL          ((guint16) 0x0209)
#define HTRK_V3_TLV_LINK_DOWN_MBIT    ((guint16) 0x020A)
#define HTRK_V3_TLV_LINK_UP_MBIT      ((guint16) 0x020B)
#define HTRK_V3_TLV_TIMEZONE_OFFSET   ((guint16) 0x020C)
#define HTRK_V3_TLV_CONTACT_URL       ((guint16) 0x020D)
#define HTRK_V3_TLV_SERVER_LAUNCHED   ((guint16) 0x020E)
#define HTRK_V3_TLV_MIN_PROTO_VERSION ((guint16) 0x0210)
#define HTRK_V3_TLV_PEAK_24H          ((guint16) 0x0211)
#define HTRK_V3_TLV_AVG_24H           ((guint16) 0x0212)
#define HTRK_V3_TLV_PROTOCOL_VERSION  ((guint16) 0x0300)
#define HTRK_V3_TLV_SUPPORTS_HOPE     ((guint16) 0x0301)
#define HTRK_V3_TLV_SUPPORTS_TLS      ((guint16) 0x0302)
#define HTRK_V3_TLV_TLS_PORT          ((guint16) 0x0303)
#define HTRK_V3_TLV_SUPPORTS_INLINE   ((guint16) 0x0304)
#define HTRK_V3_TLV_SUPPORTS_VOICE    ((guint16) 0x0305)
#define HTRK_V3_TLV_SUPPORTS_LARGEFILE ((guint16) 0x0306)
#define HTRK_V3_TLV_SUPPORTS_IPV6_TLV ((guint16) 0x0307)
#define HTRK_V3_TLV_HOPE_CIPHERS      ((guint16) 0x0309)
#define HTRK_V3_TLV_TAGS              ((guint16) 0x0310)
#define HTRK_V3_TLV_NEWS_COUNT        ((guint16) 0x0450)
#define HTRK_V3_TLV_MSGBOARD_COUNT    ((guint16) 0x0451)
#define HTRK_V3_TLV_FILES_COUNT       ((guint16) 0x0452)
#define HTRK_V3_TLV_TOTAL_FILE_SIZE   ((guint16) 0x0453)
#define HTRK_V3_TLV_LAST_NEWS_TIME    ((guint16) 0x0454)
#define HTRK_V3_TLV_LAST_CHAT_TIME    ((guint16) 0x0455)
#define HTRK_V3_TLV_PRIVATE_LISTING   ((guint16) 0x0500)
#define HTRK_V3_TLV_LISTING_CATEGORY  ((guint16) 0x0501)
#define HTRK_V3_TLV_LANGUAGE_STRICT   ((guint16) 0x0502)
#define HTRK_V3_TLV_IS_PROMOTED       ((guint16) 0x0600)
#define HTRK_V3_TLV_FIRST_SEEN        ((guint16) 0x0601)
#define HTRK_V3_TLV_LAST_HEARTBEAT    ((guint16) 0x0602)
#define HTRK_V3_TLV_VERIFIED_ONLINE   ((guint16) 0x0603)

/* H3 extension magic — marks the start of the v3 TLV extension block
 * in a UDP REGISTRATION datagram. We don't register, so this is
 * documented but unused. Kept here so the v3-aware reader can match
 * the spec section that references it without grep-failing. */
#define HTRK_V3_EXT_MAGIC      ((guint16) 0x4833) /* "H3" */
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

/* opcodes adopted from mhxd's protocol additions
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

/* Inline-media extension (fogWraith
 * Capabilities-Inline-Media.md). Wire-side gated by
 * HTLC_CAP_INLINE_MEDIA (bit 3) in DATA_CAPABILITIES.
 *
 * Field IDs 0x0201–0x021F are reserved for this extension.
 * Bytes never appear in chat transactions — only handles do; the
 * bytes flow through dedicated TranUploadMedia (750 / 0x02EE)
 * and TranDownloadMedia (751 / 0x02EF) transactions. */
#define HTLC_HDR_UPLOAD_MEDIA ((guint32)0x000002ee)
#define HTLC_HDR_DOWNLOAD_MEDIA ((guint32)0x000002ef)

/* Companion fields on chat transactions (105 / 106 / 108 / 104).
 * Both ID + TYPE present together or neither. Server overwrites
 * TYPE with the canonical MIME after re-encoding before relay. */
#define HTLC_DATA_CHAT_MEDIA_TYPE ((guint16)0x0201)
#define HTLS_DATA_CHAT_MEDIA_TYPE ((guint16)0x0201)
#define HTLC_DATA_CHAT_MEDIA_ID ((guint16)0x0202)
#define HTLS_DATA_CHAT_MEDIA_ID ((guint16)0x0202)

/* Payload + upload metadata. Used ONLY in 750 / 751. Never in
 * chat transactions. */
#define HTLC_DATA_CHAT_MEDIA_PAYLOAD ((guint16)0x0203)
#define HTLS_DATA_CHAT_MEDIA_PAYLOAD ((guint16)0x0203)
#define HTLC_DATA_CHAT_MEDIA_DECLARED_TYPE ((guint16)0x0204)

/* Server-supplied canonical metadata (set on chat transactions
 * with media + on TranDownloadMedia replies + on TranUploadMedia
 * success). u32 BE, 4 bytes. */
#define HTLS_DATA_CHAT_MEDIA_WIDTH ((guint16)0x0205)
#define HTLS_DATA_CHAT_MEDIA_HEIGHT ((guint16)0x0206)
#define HTLS_DATA_CHAT_MEDIA_BYTES ((guint16)0x0207)

/* Chunked-upload bookkeeping. The server issues an upload token
 * with the first chunk's reply; subsequent chunks echo it back.
 * PART_INDEX is zero-based u16 BE; PART_COUNT is total chunk
 * count u16 BE; PART_FINAL is u8 non-zero on the last chunk. */
#define HTLC_DATA_CHAT_MEDIA_UPLOAD_TOKEN ((guint16)0x0208)
#define HTLS_DATA_CHAT_MEDIA_UPLOAD_TOKEN ((guint16)0x0208)
#define HTLC_DATA_CHAT_MEDIA_PART_INDEX ((guint16)0x0209)
#define HTLS_DATA_CHAT_MEDIA_PART_INDEX ((guint16)0x0209)
#define HTLC_DATA_CHAT_MEDIA_PART_COUNT ((guint16)0x020a)
#define HTLS_DATA_CHAT_MEDIA_PART_COUNT ((guint16)0x020a)
#define HTLC_DATA_CHAT_MEDIA_PART_FINAL ((guint16)0x020b)
#define HTLS_DATA_CHAT_MEDIA_PART_FINAL ((guint16)0x020b)

/* Server-advertised advisory limits, carried in the LOGIN reply
 * alongside the echoed DATA_CAPABILITIES when the cap is
 * confirmed. All u32 BE. Clients use these for pre-flight
 * validation; the server still enforces them on every upload.
 * Absent means "use the spec recommended default." See
 * docs/inline-media-plan.md. */
#define HTLS_DATA_CHAT_MEDIA_MAX_BYTES ((guint16)0x020c)
#define HTLS_DATA_CHAT_MEDIA_MAX_DIMENSION ((guint16)0x020d)
#define HTLS_DATA_CHAT_MEDIA_MAX_PIXELS ((guint16)0x020e)
#define HTLS_DATA_CHAT_MEDIA_CHUNK_SIZE ((guint16)0x020f)
#define HTLS_DATA_CHAT_MEDIA_MAX_FRAMES ((guint16)0x0210)
#define HTLS_DATA_CHAT_MEDIA_MAX_DURATION_MS ((guint16)0x0211)

/* Optional machine-readable rejection category on TranUploadMedia
 * / TranDownloadMedia error replies. u16 BE. See the
 * inline-media spec's "Error Codes" table; the human DATA_ERROR
 * text remains authoritative for display. Unknown codes MUST be
 * treated as 0 (generic). */
#define HTLS_DATA_CHAT_MEDIA_ERROR_CODE ((guint16)0x0212)

/* Recommended-default cap values for clients to fall back on when
 * the server doesn't advertise them. Spec § "Resource Limits"
 * defaults. Phase A pre-flight uses the live server caps when
 * present and these otherwise. */
#define HX_MEDIA_DEFAULT_MAX_BYTES ((guint32)262144)        /* 256 KB */
#define HX_MEDIA_DEFAULT_MAX_DIMENSION ((guint32)2048)
#define HX_MEDIA_DEFAULT_MAX_PIXELS ((guint32)(2048 * 2048))
#define HX_MEDIA_DEFAULT_CHUNK_SIZE ((guint32)60000)        /* under u16 cap with header room */
#define HX_MEDIA_DEFAULT_MAX_FRAMES ((guint32)150)
#define HX_MEDIA_DEFAULT_MAX_DURATION_MS ((guint32)15000)

/* Hard client-side ceiling on the chunked-upload session token
 * (HTLS_DATA_CHAT_MEDIA_UPLOAD_TOKEN). The spec describes tokens
 * as ≤ 64 bytes; we accept up to 1 KiB to absorb future spec
 * evolution but refuse anything beyond that so a hostile server
 * can't force us into 64 KiB allocations on every chunk reply
 * (and have us echo them back on every follow-up). */
#define HX_MEDIA_MAX_UPLOAD_TOKEN ((gsize)1024)

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

/* Custom per-user icon transactions (1861-1864 / 0x0745-0x0748):
 * a per-user avatar keyed by UID, independent of the standard
 * 16-bit icon ID (HTLC_DATA_ICON / 0x0068, which GtkHx already
 * renders from the bundled icon set via cicn.c + load_icon).
 *
 * The icon-get opcode is 0x0747. An earlier revision of this header
 * mis-defined it as 0x0e90 - but 0x0e90 is the legacy cicn *data*
 * field (HTLS_DATA_ICON_CICN, below), not an opcode. Corrected here.
 * The constant was dormant (only proto_trace.c referenced it), so
 * the bug never fired.
 *
 * Constant names match mhxd's (mhxd/src/common/hotline.h) so the
 * two codebases cross-read. Legacy cicn-over-wire (a 0x0e90 payload
 * on these same transactions) is vestigial: no reachable server
 * implements it - mhxd and Janus both discard a cicn payload and are
 * GIF-only (verified June 2026). GtkHx implements the GIF payload
 * (HTLS_DATA_ICON_GIF / 0x0300) only. See Phase 10 in ROADMAP.md and
 * docs/gif-icons-plan.md. */
#define HTLC_HDR_ICON_GETLIST ((guint32)0x00000745) /* 1861 client->server */
#define HTLC_HDR_ICON_SET ((guint32)0x00000746)     /* 1862 client->server */
#define HTLC_HDR_ICON_GET ((guint32)0x00000747)     /* 1863 client->server */
#define HTLS_HDR_ICON_CHANGE ((guint32)0x00000748)  /* 1864 server->client */
#define HTLC_HDR_FILE_HASH ((guint32)0x00000ee0)

/* HTLC_HDR_GET_CHAT_HISTORY = 700: chat-history extension request.
 * Request fields (all DATA_CHANNEL_ID, DATA_HISTORY_BEFORE,
 * DATA_HISTORY_AFTER, DATA_HISTORY_LIMIT). Reply contains
 * 0..N DATA_HISTORY_ENTRY chunks plus DATA_HISTORY_HAS_MORE.
 * See fogWraith Capabilities-Chat-History.md and src/history.h. */
#define HTLC_HDR_GET_CHAT_HISTORY ((guint32)0x000002bc) /* 700 */
#define HTLS_HDR_GET_CHAT_HISTORY ((guint32)0x000002bc)

/* Voice-chat extension (fogWraith Capabilities-Voice.md). All seven
 * opcodes live in the 600-606 range, clear of the base protocol's
 * 101-355 and the chat-history extension at 700. Capability is gated
 * on HTLC_CAP_VOICE (bit 2) in DATA_CAPABILITIES; the server echoes
 * the bit to advertise SFU support, and clients that don't negotiate
 * the cap never emit or receive these opcodes.
 *
 * The canonical typed definitions live in the Rust hotline-proto
 * crate (rust/crates/hotline-proto/src/messages.rs); these C #defines
 * are integer aliases for switch-case readability in rcv.c. Same
 * dual-define convention as HTLC_HDR_GET_CHAT_HISTORY above. */
#define HTLC_HDR_VOICE_JOIN ((guint32)0x00000258)        /* 600 client->server */
#define HTLC_HDR_VOICE_LEAVE ((guint32)0x00000259)       /* 601 client->server */
#define HTLS_HDR_VOICE_SDP_OFFER ((guint32)0x0000025a)   /* 602 server->client */
#define HTLC_HDR_VOICE_SDP_ANSWER ((guint32)0x0000025b)  /* 603 client->server */
#define HTLC_HDR_VOICE_ICE ((guint32)0x0000025c)         /* 604 bidirectional */
#define HTLS_HDR_VOICE_ICE ((guint32)0x0000025c)
#define HTLS_HDR_VOICE_ROOM_STATUS ((guint32)0x0000025d) /* 605 server->client */
#define HTLC_HDR_VOICE_MUTE ((guint32)0x0000025e)        /* 606 client->server */

/* Voice-chat data field IDs. The five sit in 0x01F5-0x01F9, the gap
 * between the Large-File 64-bit extension (ends at 0x01F4) and the
 * chat-history block at 0x0F01+.
 *
 *   DATA_VOICE_SDP           UTF-8 SDP blob (RFC 8866). Non-empty on
 *                            both offer and answer sides — an empty
 *                            answer would tell the server we accept
 *                            nothing, so both the C wrapper and the
 *                            Rust builder reject it.
 *   DATA_VOICE_ICE           UTF-8 JSON-encoded RTCIceCandidateInit.
 *                            Empty string is the end-of-candidates
 *                            marker per spec.
 *   DATA_VOICE_CODEC         Active codec name, ASCII. PCMU is the
 *                            only codec the spec mandates.
 *   DATA_VOICE_MUTED         UInt16: 0 = unmuted, 1 = muted.
 *   DATA_VOICE_PARTICIPANTS  Packed 6-byte-per-entry binary blob.
 *                            Layout: u16 uid + u16 flags + u16 codec_id,
 *                            all big-endian. Flags bit 0 = muted.
 *
 * Source: fogWraith Docs/Protocol/Capabilities-Voice.md. */
#define HTLC_DATA_VOICE_SDP ((guint16)0x01f5)
#define HTLS_DATA_VOICE_SDP ((guint16)0x01f5)
#define HTLC_DATA_VOICE_ICE ((guint16)0x01f6)
#define HTLS_DATA_VOICE_ICE ((guint16)0x01f6)
#define HTLC_DATA_VOICE_CODEC ((guint16)0x01f7)
#define HTLS_DATA_VOICE_CODEC ((guint16)0x01f7)
#define HTLC_DATA_VOICE_MUTED ((guint16)0x01f8)
#define HTLS_DATA_VOICE_MUTED ((guint16)0x01f8)
#define HTLS_DATA_VOICE_PARTICIPANTS ((guint16)0x01f9)

#define HTLC_DATA_HASH_MD5 ((guint16)0x0e80)
#define HTLC_DATA_HASH_HAVAL ((guint16)0x0e81)
#define HTLC_DATA_HASH_SHA1 ((guint16)0x0e82)
#define HTLC_DATA_CHAT_AWAY ((guint16)0x0ea1)

#define HTLS_DATA_HASH_MD5 ((guint16)0x0e80)
#define HTLS_DATA_HASH_HAVAL ((guint16)0x0e81)
#define HTLS_DATA_HASH_SHA1 ((guint16)0x0e82)
/* Legacy Mac cicn-resource icon payload field. Vestigial: the
 * icon transactions (0x0745-0x0748) in practice only ever carry the
 * GIF payload (HTLS_DATA_ICON_GIF / 0x0300); no reachable server
 * serves a cicn payload here. Kept as a reserved slot. */
#define HTLS_DATA_ICON_CICN ((guint16)0x0e90)

/* GIF-icons extension payload fields (fogWraith GIF-Icons.md).
 *
 * DATA_ICON_GIF (0x0300) — raw GIF bytes (GIF87a/GIF89a). Carried on
 *   ICON_SET requests (empty clears) and ICON_GET replies. Numerically
 *   coincides with HTRK_V3_TLV_PROTOCOL_VERSION but that's a separate
 *   tracker-v3 TLV namespace, not a Hotline DATA field.
 * DATA_ICON_LIST (0x0301) — one packed entry per user in an
 *   ICON_GETLIST reply: u16 uid (BE) + u16 gif_len (BE) + gif bytes. */
#define HTLC_DATA_ICON_GIF ((guint16)0x0300)
#define HTLS_DATA_ICON_GIF ((guint16)0x0300)
#define HTLS_DATA_ICON_LIST ((guint16)0x0301)

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
#define HTLS_DATA_CIPHER_ALG ((uint16_t)0x0ec1)
#define HTLC_DATA_CIPHER_ALG ((uint16_t)0x0ec2)
#define HTLS_DATA_CIPHER_MODE ((uint16_t)0x0ec3)
#define HTLC_DATA_CIPHER_MODE ((uint16_t)0x0ec4)
#define HTLS_DATA_CIPHER_IVEC ((uint16_t)0x0ec5)
#define HTLC_DATA_CIPHER_IVEC ((uint16_t)0x0ec6)

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
/* Max HTXF handshake preamble: the 16-byte header + an 8-byte size for the
 * large-file (size64) variant. A buffer of this size always fits either form. */
#define HX_HTXF_PREAMBLE_MAX_BYTES (SIZEOF_HTXF_HDR + 8)
#define SIZEOF_HTRK_HDR (12)

#endif /* ndef GTKHX_HOTLINE_H */
