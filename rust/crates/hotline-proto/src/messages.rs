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
}

/// Server → client transaction opcodes (`HTLS_HDR_*`).
#[repr(u32)]
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServerHdr {
    Chat = 0x0000_0068,
    Msg = 0x0000_006a,
    Queue = 0x0000_00d3,
    UserChange = 0x0000_012d,
    UserPart = 0x0000_012e,
    UserSelfInfo = 0x0000_0162,
    MsgBroadcast = 0x0000_0163,
    Ping = 0x0000_01f4,
    Task = 0x0001_0000,
}

impl ServerHdr {
    /// Map a wire `type` value to a known server opcode, or `None` for an
    /// opcode this build doesn't recognise (the C dispatcher's `default`).
    pub fn from_u32(v: u32) -> Option<ServerHdr> {
        use ServerHdr::*;
        Some(match v {
            0x0000_0068 => Chat,
            0x0000_006a => Msg,
            0x0000_00d3 => Queue,
            0x0000_012d => UserChange,
            0x0000_012e => UserPart,
            0x0000_0162 => UserSelfInfo,
            0x0000_0163 => MsgBroadcast,
            0x0000_01f4 => Ping,
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
    /// `0x0072` — chat / channel id (u32).
    pub const CHAT_ID: u16 = 0x0072;
    /// `0x0073` — chat subject.
    pub const CHAT_SUBJECT: u16 = 0x0073;
    /// `0x0074` — file-transfer queue position (u32; 0 means "ready,
    /// you can start the transfer").
    pub const QUEUE: u16 = 0x0074;
    /// `0x0098` — banner type code (exactly 4 bytes, e.g. "URL ", "JPEG").
    pub const BANNER_TYPE: u16 = 0x0098;
    /// `0x0099` — banner URL (only present when type == "URL ").
    pub const BANNER_URL: u16 = 0x0099;
    /// `0x009a` — "server has no agreement" sentinel (zero-length).
    pub const NOAGREEMENT: u16 = 0x009a;
    /// `0x00a0` — server version (server) / client version (client).
    pub const VERSION: u16 = 0x00a0;
    /// `0x012c` — user-list record (server → client).
    pub const USER_LIST: u16 = 0x012c;
    /// `0x0141` — 1.5 threaded-news article listing (the
    /// HTLC_HDR_NEWSCATLIST reply payload). One catlist chunk per
    /// reply; body shape is documented on [`crate::parse::parse_catlist`].
    pub const CATLIST: u16 = 0x0141;
    /// `0x0500` — Colored-Nicknames extension: 0x00RRGGBB (u32 BE).
    pub const COLOR: u16 = 0x0500;

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
    fn server_opcode_roundtrip() {
        assert_eq!(ServerHdr::from_u32(0x0001_0000), Some(ServerHdr::Task));
        assert_eq!(ServerHdr::from_u32(0x0000_0162), Some(ServerHdr::UserSelfInfo));
        assert_eq!(ServerHdr::Task.as_u32(), 0x0001_0000);
        assert_eq!(ServerHdr::from_u32(0xdead_beef), None);
    }

    #[test]
    fn client_opcode_values_match_header() {
        // Spot-check a few against src/hotline.h.
        assert_eq!(ClientHdr::Login.as_u32(), 0x0000_006b);
        assert_eq!(ClientHdr::UserGetList.as_u32(), 0x0000_012c);
        assert_eq!(ClientHdr::Ping.as_u32(), 0x0000_01f4);
    }
}
