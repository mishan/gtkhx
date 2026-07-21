//! Opcodes and data-field tags, keyed to the wire constants in
//! `src/hotline.h`.
//!
//! Two shapes are used deliberately:
//!
//! - **Header opcodes** are modelled as enums ([`ClientHdr`], [`ServerHdr`]),
//!   one per direction. Within a direction the values are distinct, so an
//!   enum is honest and gives us exhaustive-ish `match`. Both are
//!   `#[non_exhaustive]` — mhxd's ChangeLog adds opcodes over time and a new
//!   server constant should not be a breaking change for a downstream
//!   `match` (it falls through to the catch-all).
//!
//! - **Data-field tags** are modelled as plain `u16` constants in [`tag`],
//!   *not* an enum, because the wire deliberately reuses values across
//!   contexts: `0x0065` is simultaneously `CHAT`, `MSG`, `NEWS_POST`,
//!   `AGREEMENT`, and `USER_INFO` depending on the enclosing opcode. A Rust
//!   enum can't carry duplicate discriminants, and pretending these are
//!   distinct would misrepresent the protocol.

/// Client → server transaction opcodes (`HTLC_HDR_*`).
#[repr(u32)]
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClientHdr {
    NewsGetFile = 0x0000_0065,
    NewsPost = 0x0000_0067,
    Chat = 0x0000_0069,
    Login = 0x0000_006b,
    Msg = 0x0000_006c,
    UserKick = 0x0000_006e,
    ChatCreate = 0x0000_0070,
    ChatInvite = 0x0000_0071,
    ChatDecline = 0x0000_0072,
    ChatJoin = 0x0000_0073,
    ChatPart = 0x0000_0074,
    AgreementAgree = 0x0000_0079,
    ChatSubject = 0x0000_0078,
    FileList = 0x0000_00c8,
    FileGet = 0x0000_00ca,
    FilePut = 0x0000_00cb,
    FileDelete = 0x0000_00cc,
    FileMkdir = 0x0000_00cd,
    FileGetInfo = 0x0000_00ce,
    FileSetInfo = 0x0000_00cf,
    FileMove = 0x0000_00d0,
    FileGetFolder = 0x0000_00d2,
    DownloadBanner = 0x0000_00d4,
    FilePutFolder = 0x0000_00d5,
    KillDownload = 0x0000_00d6,
    UserGetList = 0x0000_012c,
    UserGetInfo = 0x0000_012f,
    UserChange = 0x0000_0130,
    AccountRead = 0x0000_0160,
    AccountModify = 0x0000_0161,
    MsgBroadcast = 0x0000_0163,
    GetThread = 0x0000_0190,
    PostThread = 0x0000_019a,
    Ping = 0x0000_01f4,
    /// Voice-chat extension (fogWraith `Capabilities-Voice.md`).
    /// Client requests to join voice in a chat room. Reply carries the
    /// server's SDP offer; the chat-id round-trips. See [`tag::CHAT_ID`]
    /// for the room field encoding.
    VoiceJoin = 0x0000_0258,
    /// Voice-chat extension: client leaves voice in a chat room.
    VoiceLeave = 0x0000_0259,
    /// Voice-chat extension: client's SDP answer to a server-side
    /// 602 offer.
    VoiceSdpAnswer = 0x0000_025b,
    /// Voice-chat extension: trickle-ICE candidate, client→server side.
    /// Same opcode as the server→client 604 — bidirectional per spec.
    VoiceIce = 0x0000_025c,
    /// Voice-chat extension: client toggles mute on a room.
    VoiceMute = 0x0000_025e,
    /// Inline-media extension (fogWraith
    /// `Capabilities-Inline-Media.md`): client uploads image bytes
    /// to the server. Single-shot when the bytes fit in one chunk,
    /// chunked otherwise (token echo + part index/count/final).
    /// Reply on the final chunk carries the opaque media handle the
    /// client then references from a subsequent chat send.
    /// `TranUploadMedia` = 750 (`0x02EE`).
    UploadMedia = 0x0000_02ee,
    /// Inline-media extension: client fetches the canonical bytes
    /// for a media handle previously announced in a chat
    /// transaction. Reply may itself be chunked (PART_INDEX /
    /// PART_FINAL on the server side).
    /// `TranDownloadMedia` = 751 (`0x02EF`).
    DownloadMedia = 0x0000_02ef,
}

/// Server → client transaction opcodes (`HTLS_HDR_*`).
#[repr(u32)]
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServerHdr {
    // NB: these match hotline.h — MSG is 0x68, CHAT is 0x6a. (They were
    // previously swapped here; the values are unused so nothing routed on them.
    // Routing lives in dispatch::route, keyed off its own hotline.h constants.)
    Msg = 0x0000_0068,
    Chat = 0x0000_006a,
    Queue = 0x0000_00d3,
    UserChange = 0x0000_012d,
    UserPart = 0x0000_012e,
    UserSelfInfo = 0x0000_0162,
    MsgBroadcast = 0x0000_0163,
    Ping = 0x0000_01f4,
    /// Voice-chat extension (fogWraith `Capabilities-Voice.md`):
    /// server-initiated SDP offer (initial offer in the JOIN reply, or
    /// a renegotiation offer when the participant list changes).
    /// Notification — task id `0`, no reply expected.
    VoiceSdpOffer = 0x0000_025a,
    /// Voice-chat extension: trickle-ICE candidate, server→client.
    /// Bidirectional opcode — the client-side 604 has the same numeric
    /// value (see [`ClientHdr::VoiceIce`]).
    VoiceIce = 0x0000_025c,
    /// Voice-chat extension: notification of voice participants and
    /// state changes. Sent when the participant list changes, when a
    /// user mutes/unmutes, or when a participant joins/leaves voice.
    /// Notification — task id `0`, no reply expected.
    VoiceRoomStatus = 0x0000_025d,
    Task = 0x0001_0000,
}

impl ServerHdr {
    /// Map a wire `type` value to a known server opcode, or `None` for an
    /// opcode this build doesn't recognise (the C dispatcher's `default`).
    pub fn from_u32(v: u32) -> Option<ServerHdr> {
        use ServerHdr::*;
        Some(match v {
            0x0000_0068 => Msg,
            0x0000_006a => Chat,
            0x0000_00d3 => Queue,
            0x0000_012d => UserChange,
            0x0000_012e => UserPart,
            0x0000_0162 => UserSelfInfo,
            0x0000_0163 => MsgBroadcast,
            0x0000_01f4 => Ping,
            0x0000_025a => VoiceSdpOffer,
            0x0000_025c => VoiceIce,
            0x0000_025d => VoiceRoomStatus,
            0x0001_0000 => Task,
            _ => return None,
        })
    }

    /// The wire `type` value.
    pub fn as_u32(self) -> u32 {
        self as u32
    }
}

impl ClientHdr {
    /// The wire `type` value.
    pub fn as_u32(self) -> u32 {
        self as u32
    }
}

/// Data-field tags (`HTL[CS]_DATA_*`). Plain `u16` constants — see the
/// module note on why these are not an enum.
pub mod tag {
    /// `0x0064` — task error string (server → client, in a TASK reply).
    pub const TASK_ERROR: u16 = 0x0064;
    /// `0x0065` — chat / msg / news-post / agreement / user-info body.
    pub const BODY: u16 = 0x0065;
    /// `0x0066` — nickname.
    pub const NAME: u16 = 0x0066;
    /// `0x0067` — user id (u16).
    pub const UID: u16 = 0x0067;
    /// `0x0068` — icon id (u16).
    pub const ICON: u16 = 0x0068;
    /// `0x0069` — login name.
    pub const LOGIN: u16 = 0x0069;
    /// `0x006a` — password.
    pub const PASSWORD: u16 = 0x006a;
    /// `0x006b` — HTXF transfer reference (server → client).
    pub const HTXF_REF: u16 = 0x006b;
    /// `0x006c` — HTXF transfer size / chat-msg style.
    pub const HTXF_SIZE: u16 = 0x006c;
    /// `0x006d` — outgoing chat style bitmap (HTLC → server).
    pub const STYLE: u16 = 0x006d;
    /// `0x006e` — access bitmap (u64).
    pub const ACCESS: u16 = 0x006e;
    /// `0x0070` — legacy status colour bitmap (u16).
    pub const COLOUR: u16 = 0x0070;
    /// `0x0071` — agreement-agree options bitmap (u16). Mandatory on
    /// the AGREEMENTAGREE wire; Mobius panics without it.
    ///
    /// Shares the 0x0071 code point with [`BAN`] (the kick-and-ban
    /// flag inside HTLC_HDR_USER_KICK — `HTLC_DATA_BAN` in
    /// `src/hotline.h`). Same opcode-distinct reuse pattern as
    /// 0x0065 BODY does for chat/msg/news/agreement bodies.
    pub const OPTIONS: u16 = 0x0071;
    /// `0x0071` — kick "and ban" flag inside `HTLC_HDR_USER_KICK`
    /// (`HTLC_DATA_BAN` in `src/hotline.h`). Same code point as
    /// [`OPTIONS`] above, different opcode context. The chat-admin
    /// batch only emits OPTIONS, but the alias is here so the Rust
    /// `OPTIONS` doc can intra-link to it.
    pub const BAN: u16 = OPTIONS;
    /// `0x0072` — chat / channel id (u32).
    pub const CHAT_ID: u16 = 0x0072;
    /// `0x0073` — chat subject.
    pub const CHAT_SUBJECT: u16 = 0x0073;
    /// `0x0074` — file-transfer queue position (u32; 0 means "ready,
    /// you can start the transfer").
    pub const QUEUE: u16 = 0x0074;
    /// `0x0098` — banner type code (exactly 4 bytes, e.g. "URL ", "JPEG").
    pub const BANNER_TYPE: u16 = 0x0098;
    /// `0x00c9` — file basename (the leaf of a FILE_*-opcode path).
    /// Already encoded by the C caller via `gtkhx_text_for_wire`.
    pub const FILE_NAME: u16 = 0x00c9;
    /// `0x00ca` — directory path component bytes (built by
    /// `path_to_hldir` on the C side; the builders treat it as opaque
    /// payload).
    pub const DIR: u16 = 0x00ca;
    /// `0x00cb` — "Resume FLT" payload on FILE_GET (74 bytes:
    /// `"RFLT"` magic + version + DATA / MACR fork offsets, with
    /// each fork's resume position big-endian-encoded at fixed
    /// offsets 46 / 62). The C caller builds the binary blob; the
    /// builder treats it as opaque payload bytes.
    pub const RFLT: u16 = 0x00cb;
    /// `0x00cc` — FILE_PREVIEW marker on FILE_PUT (2 bytes `"\0\1"`),
    /// signalling preview / overwrite-existing semantics when the
    /// file already exists at the destination path.
    pub const FILE_PREVIEW: u16 = 0x00cc;
    /// `0x00cd` — file type code (server → client on FILE_GETINFO
    /// reply: 4-byte HFS-style type, e.g. `"TEXT"`, `"PICT"`).
    pub const FILE_TYPE: u16 = 0x00cd;
    /// `0x00ce` — file creator code (server → client on FILE_GETINFO
    /// reply: 4-byte HFS-style creator code, e.g. `"MSWD"`).
    pub const FILE_CREATOR: u16 = 0x00ce;
    /// `0x00cf` — legacy file size (1..=4 bytes BE unsigned) on the
    /// FILE_GETINFO reply (the file-info dialog payload). Some
    /// servers (mhxd) emit the size in the smallest BE width that
    /// fits; the parser zero-extends to u32. Companion field
    /// [`FILESIZE64`] when the server speaks Large-Files.
    ///
    /// Note: not to be confused with [`HTXF_SIZE`] (0x006c), which
    /// is the FILE_GET / FILE_PUT transfer-size chunk.
    pub const FILE_SIZE: u16 = 0x00cf;
    /// `0x00d0` — file creation date (8-byte Hotline date stamp).
    pub const FILE_DATE_CREATE: u16 = 0x00d0;
    /// `0x00d1` — file modification date (8-byte Hotline date stamp).
    pub const FILE_DATE_MODIFY: u16 = 0x00d1;
    /// `0x00d2` — file comment string (multi-line; CR2LF normalisation
    /// applied at the caller).
    pub const FILE_COMMENT: u16 = 0x00d2;
    /// `0x00d3` — file rename target (the new basename in
    /// FILE_SETINFO / FILE_SYMLINK).
    pub const FILE_RENAME: u16 = 0x00d3;
    /// `0x00d4` — directory rename target (the destination dir bytes
    /// in FILE_MOVE / FILE_SYMLINK).
    pub const DIR_RENAME: u16 = 0x00d4;
    /// `0x00d5` — file icon code (server → client on FILE_GETINFO
    /// reply: 4-byte icon resource id).
    pub const FILE_ICON: u16 = 0x00d5;
    /// `0x00dc` — folder file count (u32 BE) on PUTFOLDER, used by
    /// the server's progress UI.
    pub const FILE_NFILES: u16 = 0x00dc;
    /// `0x0099` — banner URL (only present when type == "URL ").
    pub const BANNER_URL: u16 = 0x0099;
    /// `0x009a` — "server has no agreement" sentinel (zero-length).
    pub const NOAGREEMENT: u16 = 0x009a;
    /// `0x00a0` — server version (server) / client version (client).
    pub const VERSION: u16 = 0x00a0;
    /// `0x012c` — user-list record (server → client).
    pub const USER_LIST: u16 = 0x012c;
    /// `0x0140` — 1.5 news directory-listing entry (folder or category).
    /// One per entry in the HTLC_HDR_NEWSDIRLIST reply; body shape is
    /// documented on [`crate::parse::parse_news_folderitem`].
    pub const NEWSFOLDERITEM: u16 = 0x0140;
    /// `0x0141` — 1.5 threaded-news article listing (the
    /// HTLC_HDR_NEWSCATLIST reply payload). One catlist chunk per
    /// reply; body shape is documented on [`crate::parse::parse_catlist`].
    pub const CATLIST: u16 = 0x0141;
    /// `0x0142` — 1.5 category name (the name field on
    /// `HTLC_HDR_MAKECATEGORY`).
    pub const CATEGORY: u16 = 0x0142;
    /// `0x0143` — 1.5 news directory-listing entry with per-category sync
    /// metadata (an alternate encoding of [`NEWSFOLDERITEM`] some servers
    /// emit); body shape on [`crate::parse::parse_news_categoryitem`].
    pub const CATEGORYITEM: u16 = 0x0143;
    /// `0x0145` — 1.5 news path (server → client and client → server).
    /// The path component encoding is the responsibility of the
    /// caller (`path_to_hldir` on the C side).
    pub const NEWSPATH: u16 = 0x0145;
    /// `0x0146` — 1.5 news thread id (u32 BE).
    pub const THREADID: u16 = 0x0146;
    /// `0x0147` — 1.5 news article MIME type (e.g. "text/plain").
    pub const NEWSTYPE: u16 = 0x0147;
    /// `0x0148` — 1.5 news article subject line. Single-line; the C
    /// `gtkhx_text_for_wire` is called with `is_body = FALSE` so the
    /// LF→CR send-path normalisation is skipped.
    pub const NEWSSUBJECT: u16 = 0x0148;
    /// `0x014d` — 1.5 news article body. Multi-line; the C
    /// `gtkhx_text_for_wire` is called with `is_body = TRUE` so the
    /// LF→CR send-path normalisation is applied for legacy Mac
    /// servers. (CR2LF is the *receive*-path inverse — applied in
    /// `parse_news_post` / `parse_news_file`, not here.)
    pub const NEWSDATA: u16 = 0x014d;
    /// `0x014e` — 1.5 news "parent thread id" (u32 BE). Required by
    /// the spec but not actually consulted by mhxd; gtkhx sends 0.
    pub const PARENTTHREAD: u16 = 0x014e;
    /// `0x01f1` — Large-Files extension: 64-bit file size companion
    /// to `FILE_SIZE` on FILE_GETINFO replies (u64 BE, 8 bytes).
    /// When present, callers prefer this over the legacy 32-bit
    /// field (which may have been clamped at `0xFFFFFFFF`).
    pub const FILESIZE64: u16 = 0x01f1;
    /// `0x01f3` — Large-Files extension: 64-bit transfer size
    /// companion to `HTXF_SIZE` (u64 BE, 8 bytes). Sent on FILE_PUT
    /// when `CAP_LARGE_FILES` was negotiated; receivers in large-
    /// file mode prefer this over the 32-bit legacy field, which is
    /// clamped at `0xFFFFFFFF` when the true size overflows.
    pub const XFERSIZE64: u16 = 0x01f3;
    /// `0x01f5` — Voice-chat extension: SDP blob (UTF-8 text,
    /// RFC 8866). Carried on JOIN replies (server's offer),
    /// HTLS_HDR_VOICE_SDP_OFFER notifications, and HTLC_HDR_VOICE_SDP_ANSWER
    /// requests. Source: fogWraith Capabilities-Voice.md.
    pub const VOICE_SDP: u16 = 0x01f5;
    /// `0x01f6` — Voice-chat extension: JSON-encoded
    /// RTCIceCandidateInit. Empty string is the end-of-candidates
    /// marker per spec. Bidirectional via HTLC_HDR_VOICE_ICE /
    /// HTLS_HDR_VOICE_ICE (the same numeric opcode 604).
    pub const VOICE_ICE: u16 = 0x01f6;
    /// `0x01f7` — Voice-chat extension: active codec name (ASCII).
    /// The spec only mandates PCMU; the field is carried for forward
    /// compatibility with future codec choices.
    pub const VOICE_CODEC: u16 = 0x01f7;
    /// `0x01f8` — Voice-chat extension: mute state (u16 BE). 0 =
    /// unmuted, 1 = muted. Carried on outgoing VOICE_MUTE; the server
    /// reflects the new state to other participants via
    /// VOICE_PARTICIPANTS in a VOICE_ROOM_STATUS notification.
    pub const VOICE_MUTED: u16 = 0x01f8;
    /// `0x0201` — Inline-media extension: canonical MIME type
    /// (server-supplied on relay; sender's declared type is a hint
    /// only and gets overwritten). Companion to [`CHAT_MEDIA_ID`].
    pub const CHAT_MEDIA_TYPE: u16 = 0x0201;
    /// `0x0202` — Inline-media extension: opaque server-issued
    /// media handle (≤ 64 bytes). Carried on chat transactions
    /// alongside [`CHAT_MEDIA_TYPE`]; either both present or
    /// neither.
    pub const CHAT_MEDIA_ID: u16 = 0x0202;
    /// `0x0203` — Inline-media extension: image bytes. Used ONLY
    /// in TranUploadMedia (request) and TranDownloadMedia (reply).
    /// Never on chat transactions.
    pub const CHAT_MEDIA_PAYLOAD: u16 = 0x0203;
    /// `0x0204` — Inline-media extension: sender's declared MIME
    /// type (hint, server doesn't trust it for sniff decisions).
    /// Used ONLY in TranUploadMedia request, first chunk only.
    pub const CHAT_MEDIA_DECLARED_TYPE: u16 = 0x0204;
    /// `0x0205` — Inline-media extension: canonical image width in
    /// pixels (u32 BE).
    pub const CHAT_MEDIA_WIDTH: u16 = 0x0205;
    /// `0x0206` — Inline-media extension: canonical image height
    /// in pixels (u32 BE).
    pub const CHAT_MEDIA_HEIGHT: u16 = 0x0206;
    /// `0x0207` — Inline-media extension: canonical image byte
    /// size (u32 BE).
    pub const CHAT_MEDIA_BYTES: u16 = 0x0207;
    /// `0x0208` — Inline-media extension: chunked-upload session
    /// token (≤ 64 bytes), issued by the server with the first
    /// chunk's reply, echoed by the client on every subsequent
    /// chunk.
    pub const CHAT_MEDIA_UPLOAD_TOKEN: u16 = 0x0208;
    /// `0x0209` — Inline-media extension: zero-based chunk index
    /// (u16 BE).
    pub const CHAT_MEDIA_PART_INDEX: u16 = 0x0209;
    /// `0x020a` — Inline-media extension: total chunk count (u16
    /// BE). Sent only on the first chunk of a chunked upload.
    pub const CHAT_MEDIA_PART_COUNT: u16 = 0x020a;
    /// `0x020b` — Inline-media extension: non-zero on the final
    /// chunk (u8). Single-shot uploads set this on the only chunk.
    pub const CHAT_MEDIA_PART_FINAL: u16 = 0x020b;
    /// `0x020c` — Inline-media extension: server-advertised
    /// maximum encoded payload size in bytes (u32 BE). LOGIN reply
    /// only.
    pub const CHAT_MEDIA_MAX_BYTES: u16 = 0x020c;
    /// `0x020d` — Inline-media extension: server-advertised
    /// maximum width OR height in pixels (u32 BE). LOGIN reply only.
    pub const CHAT_MEDIA_MAX_DIMENSION: u16 = 0x020d;
    /// `0x020e` — Inline-media extension: server-advertised
    /// maximum width × height pixel count (u32 BE). LOGIN reply
    /// only.
    pub const CHAT_MEDIA_MAX_PIXELS: u16 = 0x020e;
    /// `0x020f` — Inline-media extension: server-recommended
    /// per-chunk byte size for chunked uploads, also the per-chunk
    /// size in TranDownloadMedia replies (u32 BE). LOGIN reply
    /// only.
    pub const CHAT_MEDIA_CHUNK_SIZE: u16 = 0x020f;
    /// `0x0210` — Inline-media extension: server-advertised
    /// maximum animation frame count (u32 BE). LOGIN reply only.
    pub const CHAT_MEDIA_MAX_FRAMES: u16 = 0x0210;
    /// `0x0211` — Inline-media extension: server-advertised
    /// maximum animation duration in ms (u32 BE). LOGIN reply only.
    pub const CHAT_MEDIA_MAX_DURATION_MS: u16 = 0x0211;
    /// `0x0212` — Inline-media extension: optional u16 BE machine-
    /// readable rejection category on TranUploadMedia /
    /// TranDownloadMedia error replies. Mapped through
    /// [`crate::inline_media::MediaErrorCode`]. Unknown codes MUST
    /// be treated as `Generic` (0).
    pub const CHAT_MEDIA_ERROR_CODE: u16 = 0x0212;

    /// `0x01f9` — Voice-chat extension: packed participant list,
    /// binary. Each entry is 6 bytes: `u16 uid` + `u16 flags` (bit 0 =
    /// muted, bits 1-15 reserved) + `u16 codec_id` (see Codec ID
    /// Table). All big-endian. Parser is
    /// [`crate::voice::parse_voice_participants`].
    pub const VOICE_PARTICIPANTS: u16 = 0x01f9;
    /// `0x0500` — Colored-Nicknames extension: 0x00RRGGBB (u32 BE).
    pub const COLOR: u16 = 0x0500;

    /// `0x0300` — GIF-icons extension: raw GIF avatar bytes
    /// (GIF87a/GIF89a). On an ICON_SET request (empty clears) and an
    /// ICON_GET reply. Numerically coincides with a tracker-v3 TLV ID
    /// but that's a separate namespace. Parser: [`crate::gif_icons`].
    pub const ICON_GIF: u16 = 0x0300;
    /// `0x0301` — GIF-icons extension: one packed entry per user in an
    /// ICON_GETLIST reply — u16 uid (BE) + u16 gif_len (BE) + gif bytes.
    pub const ICON_LIST: u16 = 0x0301;

    /// `0x0065` — HTLS_DATA_NEWS body (1.0 flat news only; aliases BODY).
    /// The chat / msg / agreement readers each look at this same tag
    /// from inside the opcode's parser — see the module comment about
    /// 0x0065 being reused across contexts.
    pub const NEWS: u16 = BODY;
}

/// Sentinel for "no nickname colour" in the Colored-Nicknames extension
/// (`HX_NICK_COLOR_NONE` in `src/hotline.h`).
pub const NICK_COLOR_NONE: u32 = 0xffff_ffff;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn client_opcode_values_match_header() {
        // Spot-check a few against src/hotline.h.
        assert_eq!(ClientHdr::Login.as_u32(), 0x0000_006b);
        assert_eq!(ClientHdr::UserGetList.as_u32(), 0x0000_012c);
        assert_eq!(ClientHdr::Ping.as_u32(), 0x0000_01f4);
    }

    #[test]
    fn voice_opcode_numeric_values_match_spec() {
        // The fogWraith voice spec assigns decimal opcodes 600-606; these
        // are the hex equivalents that go on the wire. Pin them here so a
        // typo in the enum literal can't silently retarget us at the wrong
        // transaction ID.
        assert_eq!(ClientHdr::VoiceJoin.as_u32(), 600);
        assert_eq!(ClientHdr::VoiceLeave.as_u32(), 601);
        assert_eq!(ServerHdr::VoiceSdpOffer.as_u32(), 602);
        assert_eq!(ClientHdr::VoiceSdpAnswer.as_u32(), 603);
        // 604 is bidirectional — both sides use the same numeric value.
        assert_eq!(ClientHdr::VoiceIce.as_u32(), 604);
        assert_eq!(ServerHdr::VoiceIce.as_u32(), 604);
        assert_eq!(ServerHdr::VoiceRoomStatus.as_u32(), 605);
        assert_eq!(ClientHdr::VoiceMute.as_u32(), 606);

        assert_eq!(ServerHdr::from_u32(602), Some(ServerHdr::VoiceSdpOffer));
        assert_eq!(ServerHdr::from_u32(604), Some(ServerHdr::VoiceIce));
        assert_eq!(ServerHdr::from_u32(605), Some(ServerHdr::VoiceRoomStatus));
    }

    #[test]
    fn voice_field_tag_values_match_spec() {
        assert_eq!(tag::VOICE_SDP, 0x01f5);
        assert_eq!(tag::VOICE_ICE, 0x01f6);
        assert_eq!(tag::VOICE_CODEC, 0x01f7);
        assert_eq!(tag::VOICE_MUTED, 0x01f8);
        assert_eq!(tag::VOICE_PARTICIPANTS, 0x01f9);
    }

    #[test]
    fn inline_media_opcode_values_match_spec() {
        // fogWraith Capabilities-Inline-Media.md: 750 / 751 decimal.
        assert_eq!(ClientHdr::UploadMedia.as_u32(), 750);
        assert_eq!(ClientHdr::DownloadMedia.as_u32(), 751);
        assert_eq!(ClientHdr::UploadMedia.as_u32(), 0x02ee);
        assert_eq!(ClientHdr::DownloadMedia.as_u32(), 0x02ef);
    }

    #[test]
    fn inline_media_field_tag_values_match_spec() {
        // Each tag matches the table in the spec's "New Data Objects".
        assert_eq!(tag::CHAT_MEDIA_TYPE, 0x0201);
        assert_eq!(tag::CHAT_MEDIA_ID, 0x0202);
        assert_eq!(tag::CHAT_MEDIA_PAYLOAD, 0x0203);
        assert_eq!(tag::CHAT_MEDIA_DECLARED_TYPE, 0x0204);
        assert_eq!(tag::CHAT_MEDIA_WIDTH, 0x0205);
        assert_eq!(tag::CHAT_MEDIA_HEIGHT, 0x0206);
        assert_eq!(tag::CHAT_MEDIA_BYTES, 0x0207);
        assert_eq!(tag::CHAT_MEDIA_UPLOAD_TOKEN, 0x0208);
        assert_eq!(tag::CHAT_MEDIA_PART_INDEX, 0x0209);
        assert_eq!(tag::CHAT_MEDIA_PART_COUNT, 0x020a);
        assert_eq!(tag::CHAT_MEDIA_PART_FINAL, 0x020b);
        assert_eq!(tag::CHAT_MEDIA_MAX_BYTES, 0x020c);
        assert_eq!(tag::CHAT_MEDIA_MAX_DIMENSION, 0x020d);
        assert_eq!(tag::CHAT_MEDIA_MAX_PIXELS, 0x020e);
        assert_eq!(tag::CHAT_MEDIA_CHUNK_SIZE, 0x020f);
        assert_eq!(tag::CHAT_MEDIA_MAX_FRAMES, 0x0210);
        assert_eq!(tag::CHAT_MEDIA_MAX_DURATION_MS, 0x0211);
        assert_eq!(tag::CHAT_MEDIA_ERROR_CODE, 0x0212);
    }
}
