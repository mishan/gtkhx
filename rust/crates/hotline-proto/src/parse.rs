//! Typed parsers for individual server messages.
//!
//! Phase R2 grows this module one opcode at a time (see the extraction
//! order in `docs/RUST-ROADMAP.md`). Each parser takes a fully-assembled
//! message buffer (header + body, the C side's `htlc->in.buf[..in.pos]`)
//! and returns a strongly-typed event; the C dispatcher keeps the dispatch
//! table and the `GtkhxSession` signal emit.
//!
//! Proof-of-concept opcodes for the foundation commit:
//! `HTLS_HDR_USER_SELFINFO` and `HTLS_HDR_TASK`.

use crate::messages::{tag, NICK_COLOR_NONE};
use crate::sanitize::{cr2lf, strip_ansi};
use crate::wire::{ChunkIter, Decoder};

/// Outcome of parsing an `HTLS_HDR_AGREEMENT` body.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AgreementResult {
    /// Server sent the `HTLS_DATA_NOAGREEMENT` sentinel (no agreement to
    /// display; the client skips the agreement modal).
    None,
    /// Server sent an `HTLS_DATA_AGREEMENT` chunk; the sanitised body is
    /// in the caller's buffer.
    Ok,
    /// Body contained neither chunk; matches the C extractor's
    /// "fell off the loop" return.
    Missing,
}

// ---- Transaction header -------------------------------------------------

/// The fixed 22-byte transaction header (`struct hl_hdr`), big-endian.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    pub type_: u32,
    pub trans: u32,
    pub flag: u32,
    pub len: u32,
    pub len2: u32,
    pub hc: u16,
}

impl Header {
    /// Parse the leading 22 bytes of a message. `None` if the buffer is
    /// shorter than a header.
    pub fn parse(buf: &[u8]) -> Option<Header> {
        let mut d = Decoder::new(buf);
        Some(Header {
            type_: d.u32()?,
            trans: d.u32()?,
            flag: d.u32()?,
            len: d.u32()?,
            len2: d.u32()?,
            hc: d.u16()?,
        })
    }

    /// True when the task-error bit is set in `flag` — the condition
    /// `task_inerror()` checks (`ntohl(flag) & 1`).
    pub fn in_error(&self) -> bool {
        self.flag & 1 != 0
    }
}

// ---- HTLS_HDR_USER_SELFINFO --------------------------------------------

/// Bit flags reporting which SELFINFO fields were present, mirroring the
/// `HX_SELFINFO_*` enum in `src/proto_helpers.h`.
pub const SELFINFO_ACCESS: u32 = 1 << 0;
pub const SELFINFO_USER_LIST: u32 = 1 << 1;
pub const SELFINFO_NICK_COLOR: u32 = 1 << 2;

/// Parsed `HTLS_HDR_USER_SELFINFO` payload.
///
/// Note the deliberate omissions that match the C parser: the server's
/// cached nickname is **not** adopted (local prefs win — see the long
/// comment in `hx_selfinfo_parse`), and the legacy status colour chunk is
/// ignored. We surface the cached-name bytes only so the C shim can keep
/// emitting its forensic hex debug line.
#[derive(Debug, Clone)]
pub struct SelfInfo<'a> {
    /// OR of the `SELFINFO_*` flags for fields actually seen.
    pub seen: u32,
    /// 64-bit access bitmap (only valid if `seen & SELFINFO_ACCESS`).
    pub access: u64,
    /// Our user id from the USER_LIST chunk.
    pub uid: u16,
    /// Our icon id from the USER_LIST chunk.
    pub icon: u16,
    /// Nickname colour (0x00RRGGBB) if the Colored-Nicknames chunk was sent.
    pub nick_color: u32,
    /// The server's cached nickname bytes (clamped to 31), for logging only.
    pub cached_name: &'a [u8],
}

/// Parse a SELFINFO message buffer. `len` is `htlc->in.pos`.
pub fn parse_selfinfo(buf: &[u8], len: usize) -> SelfInfo<'_> {
    let mut out = SelfInfo {
        seen: 0,
        access: 0,
        uid: 0,
        icon: 0,
        nick_color: 0,
        cached_name: &[],
    };

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::ACCESS => {
                if chunk.data.len() == 8 {
                    let mut a = [0u8; 8];
                    a.copy_from_slice(chunk.data);
                    out.access = u64::from_be_bytes(a);
                    out.seen |= SELFINFO_ACCESS;
                }
            }
            tag::USER_LIST => {
                // hl_userlist_hdr minus the data header: uid(2) icon(2)
                // color(2) nlen(2) name[]. Need at least the fixed 8 bytes.
                if chunk.data.len() >= 8 {
                    let d = chunk.data;
                    out.uid = u16::from_be_bytes([d[0], d[1]]);
                    out.icon = u16::from_be_bytes([d[2], d[3]]);
                    let mut nlen = u16::from_be_bytes([d[6], d[7]]) as usize;
                    if nlen > 31 {
                        nlen = 31;
                    }
                    let avail = d.len() - 8;
                    let nlen = nlen.min(avail);
                    out.cached_name = &d[8..8 + nlen];
                    out.seen |= SELFINFO_USER_LIST;
                }
            }
            tag::COLOR => {
                if chunk.data.len() == 4 {
                    out.nick_color = chunk.as_uint();
                    out.seen |= SELFINFO_NICK_COLOR;
                }
            }
            _ => {}
        }
    }

    out
}

// ---- HTLS_HDR_CHAT ------------------------------------------------------

/// Parsed + sanitised public-chat line.
#[derive(Debug, Clone)]
pub struct ChatMessage {
    pub cid: u32,
    pub uid: u16,
    /// The full sanitised line (CR→LF + `strip_ansi` applied), without a
    /// trailing NUL. `buf[text_off..]` is the display text.
    pub buf: Vec<u8>,
    /// 0, or 1 when a leading `\n` was stripped (the common
    /// "\nUser: message" Hotline framing).
    pub text_off: usize,
}

impl ChatMessage {
    /// The display text after the leading-LF strip.
    pub fn text(&self) -> &[u8] {
        &self.buf[self.text_off..]
    }
}

/// Parse `HTLS_HDR_CHAT`. `max_body` caps the body (the C handler uses
/// 8192). The body is CR2LF'd then `strip_ansi`'d, and a single leading LF
/// is dropped from the display text. Missing CHAT_ID / UID default to 0;
/// an empty body is a valid "" message.
pub fn parse_chat(buf: &[u8], len: usize, max_body: usize) -> ChatMessage {
    let mut cid = 0u32;
    let mut uid = 0u16;
    let mut body: Vec<u8> = Vec::new();

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            // HTLS_DATA_CHAT shares the 0x0065 "body" tag.
            tag::BODY => {
                let take = chunk.data.len().min(max_body);
                body = chunk.data[..take].to_vec();
            }
            tag::CHAT_ID => cid = chunk.as_uint(),
            tag::UID => uid = chunk.as_uint() as u16,
            _ => {}
        }
    }

    cr2lf(&mut body);
    strip_ansi(&mut body);
    let text_off = usize::from(body.first() == Some(&b'\n'));

    ChatMessage {
        cid,
        uid,
        buf: body,
        text_off,
    }
}

// ---- HTLS_HDR_CHAT_SUBJECT ---------------------------------------------

/// Parsed chat-subject change.
#[derive(Debug, Clone)]
pub struct ChatSubject {
    pub cid: u32,
    /// Raw subject bytes, capped, no NUL. Subjects are not CR2LF'd or
    /// `strip_ansi`'d — they carry no line endings.
    pub subject: Vec<u8>,
}

/// Parse `HTLS_HDR_CHAT_SUBJECT`. `max_subject` caps the subject (the C
/// handler uses 255).
pub fn parse_chat_subject(buf: &[u8], len: usize, max_subject: usize) -> ChatSubject {
    let mut cid = 0u32;
    let mut subject: Vec<u8> = Vec::new();

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::CHAT_ID => cid = chunk.as_uint(),
            tag::CHAT_SUBJECT => {
                let take = chunk.data.len().min(max_subject);
                subject = chunk.data[..take].to_vec();
            }
            _ => {}
        }
    }

    ChatSubject { cid, subject }
}

// ---- TASK error string (HTLS_DATA_TASKERROR inside a TASK reply) -------

/// Parse a `HTLS_DATA_TASKERROR` chunk out of an error TASK reply.
/// Returns `Some(bytes)` containing the CR2LF + `strip_ansi`-sanitised
/// error string (capped at `max_len` bytes), or `None` when no
/// `TASK_ERROR` chunk is present in the body. The C-facing wrapper
/// (`gtkhx_proto_parse_task_error` in `ffi.rs`) is where the
/// "write into caller buffer / SIZE_MAX sentinel" surface lives.
pub fn parse_task_error(buf: &[u8], len: usize, max_len: usize) -> Option<Vec<u8>> {
    for chunk in ChunkIter::over_message(buf, len) {
        if chunk.tag == tag::TASK_ERROR {
            let take = chunk.data.len().min(max_len);
            let mut v = chunk.data[..take].to_vec();
            cr2lf(&mut v);
            strip_ansi(&mut v);
            return Some(v);
        }
    }
    None
}

// ---- HTLS_HDR_MSG / HTLS_HDR_MSG_BROADCAST / HTLS_HDR_POLITEQUIT -------

/// Parsed `HTLS_HDR_MSG`-family payload (also `MSG_BROADCAST` and
/// `POLITEQUIT`, all of which share the same `rcv_msg` handler).
#[derive(Debug, Clone)]
pub struct Msg {
    pub uid: u16,
    /// `strip_ansi`'d sender name, capped at `max_name`. May contain
    /// embedded NULs only if the wire payload did.
    pub name: Vec<u8>,
    /// CR2LF + `strip_ansi` sanitised message body, capped at `max_msg`.
    pub msg: Vec<u8>,
}

/// Parse the MSG family. `max_name` caps the name (C handler uses 128),
/// `max_msg` caps the body (C handler uses 8192).
pub fn parse_msg(buf: &[u8], len: usize, max_name: usize, max_msg: usize) -> Msg {
    let mut out = Msg {
        uid: 0,
        name: Vec::new(),
        msg: Vec::new(),
    };

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::UID => out.uid = chunk.as_uint() as u16,
            // HTLS_DATA_MSG and HTLS_DATA_AGREEMENT share the 0x0065 tag
            // with chat's BODY; the MSG handler is the only consumer in
            // this opcode's parser.
            tag::BODY => {
                let take = chunk.data.len().min(max_msg);
                out.msg = chunk.data[..take].to_vec();
            }
            tag::NAME => {
                let take = chunk.data.len().min(max_name);
                out.name = chunk.data[..take].to_vec();
            }
            _ => {}
        }
    }

    strip_ansi(&mut out.name);
    cr2lf(&mut out.msg);
    strip_ansi(&mut out.msg);
    out
}

// ---- HTLS_HDR_BANNER ---------------------------------------------------

/// Parsed `HTLS_HDR_BANNER` payload.
#[derive(Debug, Clone)]
pub struct Banner {
    /// True iff the `HTLS_DATA_BANNER_TYPE` chunk was present at exactly
    /// 4 bytes. Mirrors the C extractor's `got_type` return value.
    pub got_type: bool,
    /// The 4 banner-type bytes (e.g. `b"URL "`, `b"JPEG"`, `b"GIFf"`).
    /// Zeroed when `got_type` is false.
    pub type_code: [u8; 4],
    /// Optional URL bytes (capped at `max_url`).
    pub url: Option<Vec<u8>>,
}

/// Parse `HTLS_HDR_BANNER`. `max_url` caps the URL (C handler uses 1024).
/// Banner type is gated at exactly 4 bytes — per mhxd's
/// `rcv_agreementagree`, the type is always 4 bytes (right-padded with
/// spaces for shorter codes like "URL"). Anything else is malformed and
/// the type is rejected.
pub fn parse_banner(buf: &[u8], len: usize, max_url: usize) -> Banner {
    let mut out = Banner {
        got_type: false,
        type_code: [0; 4],
        url: None,
    };

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::BANNER_TYPE => {
                if chunk.data.len() == 4 {
                    out.type_code.copy_from_slice(chunk.data);
                    out.got_type = true;
                }
            }
            tag::BANNER_URL => {
                let take = chunk.data.len().min(max_url);
                out.url = Some(chunk.data[..take].to_vec());
            }
            _ => {}
        }
    }

    out
}

// ---- HTLS_HDR_QUEUE ----------------------------------------------------

/// Parsed `HTLS_HDR_QUEUE`. Both fields default to 0 when missing.
/// `queueid` of 0 means "ready, you can start the transfer".
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct XferQueue {
    pub htxf_ref: u32,
    pub queueid: u32,
}

/// Parse `HTLS_HDR_QUEUE`.
pub fn parse_xfer_queue(buf: &[u8], len: usize) -> XferQueue {
    let mut out = XferQueue { htxf_ref: 0, queueid: 0 };
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::HTXF_REF => out.htxf_ref = chunk.as_uint(),
            tag::QUEUE => out.queueid = chunk.as_uint(),
            _ => {}
        }
    }
    out
}

// ---- HTLS_HDR_AGREEMENT ------------------------------------------------

/// Parse `HTLS_HDR_AGREEMENT`. Whichever of `HTLS_DATA_NOAGREEMENT` or
/// `HTLS_DATA_AGREEMENT` (sharing the [`tag::BODY`] value 0x0065)
/// appears first wins, matching the C `hx_agreement_extract`'s
/// early-return-from-inside-loop semantics. NOAGREEMENT yields
/// [`AgreementResult::None`] with an empty body; AGREEMENT yields
/// [`AgreementResult::Ok`] with the CR2LF + `strip_ansi` sanitised body
/// (capped at `max_len`). Neither chunk → [`AgreementResult::Missing`]
/// and an empty body.
pub fn parse_agreement(buf: &[u8], len: usize, max_len: usize) -> (AgreementResult, Vec<u8>) {
    for chunk in ChunkIter::over_message(buf, len) {
        if chunk.tag == tag::NOAGREEMENT {
            return (AgreementResult::None, Vec::new());
        }
        if chunk.tag == tag::BODY {
            let take = chunk.data.len().min(max_len);
            let mut body = chunk.data[..take].to_vec();
            cr2lf(&mut body);
            strip_ansi(&mut body);
            return (AgreementResult::Ok, body);
        }
    }
    (AgreementResult::Missing, Vec::new())
}

// ---- HTLS_HDR_USER_PART ------------------------------------------------

/// Parsed `HTLS_HDR_USER_PART`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct UserPart {
    pub uid: u16,
    pub cid: u32,
}

/// Parse `HTLS_HDR_USER_PART`. Missing chunks default to zero.
pub fn parse_user_part(buf: &[u8], len: usize) -> UserPart {
    let mut uid = 0u16;
    let mut cid = 0u32;

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::UID => uid = chunk.as_uint() as u16,
            tag::CHAT_ID => cid = chunk.as_uint(),
            _ => {}
        }
    }

    UserPart { uid, cid }
}

// ---- HTLS_HDR_USER_CHANGE ----------------------------------------------

/// Parsed `HTLS_HDR_USER_CHANGE`. Mirrors the C `hx_user_change_msg`
/// extractor: name is `strip_ansi`'d and capped (no CR2LF), and the
/// Colored-Nicknames `COLOR` field is only accepted at exactly 4 bytes.
#[derive(Debug, Clone)]
pub struct UserChange {
    pub uid: u16,
    pub icon: u16,
    pub color: u16,
    pub got_color: bool,
    pub nick_color: u32,
    pub got_nick_color: bool,
    pub cid: u32,
    /// Nickname bytes after `strip_ansi`, capped at `max_name`. The
    /// `strip_ansi` fold does not touch 0x00, so an interior NUL on
    /// the wire survives in this `Vec` verbatim.
    pub name: Vec<u8>,
}

/// Parse `HTLS_HDR_USER_CHANGE`. `max_name` caps the nickname (the C
/// handler uses 31). Missing chunks default to zero / "" / `false`;
/// `nick_color` defaults to [`NICK_COLOR_NONE`] when the COLOR extension
/// chunk is absent.
pub fn parse_user_change(buf: &[u8], len: usize, max_name: usize) -> UserChange {
    let mut out = UserChange {
        uid: 0,
        icon: 0,
        color: 0,
        got_color: false,
        nick_color: NICK_COLOR_NONE,
        got_nick_color: false,
        cid: 0,
        name: Vec::new(),
    };

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::UID => out.uid = chunk.as_uint() as u16,
            tag::ICON => out.icon = chunk.as_uint() as u16,
            tag::NAME => {
                let take = chunk.data.len().min(max_name);
                out.name = chunk.data[..take].to_vec();
            }
            tag::COLOUR => {
                out.color = chunk.as_uint() as u16;
                out.got_color = true;
            }
            tag::COLOR => {
                // Colored-Nicknames spec pins this field to exactly 4
                // bytes (BE u32); reject any other width as malformed.
                if chunk.data.len() == 4 {
                    out.nick_color = chunk.as_uint();
                    out.got_nick_color = true;
                }
            }
            tag::CHAT_ID => out.cid = chunk.as_uint(),
            _ => {}
        }
    }

    strip_ansi(&mut out.name);
    out
}

// ---- HTLS_HDR_CHAT_INVITE ----------------------------------------------

/// Parsed chat invitation.
#[derive(Debug, Clone)]
pub struct ChatInvite {
    pub uid: u16,
    pub cid: u32,
    /// Inviter name, capped, `strip_ansi`'d, no NUL.
    pub name: Vec<u8>,
}

/// Parse `HTLS_HDR_CHAT_INVITE`. `max_name` caps the inviter name (the C
/// handler uses 31). The name is `strip_ansi`'d (no CR2LF).
pub fn parse_chat_invite(buf: &[u8], len: usize, max_name: usize) -> ChatInvite {
    let mut uid = 0u16;
    let mut cid = 0u32;
    let mut name: Vec<u8> = Vec::new();

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::UID => uid = chunk.as_uint() as u16,
            tag::CHAT_ID => cid = chunk.as_uint(),
            tag::NAME => {
                let take = chunk.data.len().min(max_name);
                name = chunk.data[..take].to_vec();
            }
            _ => {}
        }
    }

    strip_ansi(&mut name);
    ChatInvite { uid, cid, name }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::HL_HDR_LEN;

    /// Build a 22-byte header with the given type/trans/flag, then append
    /// the supplied body bytes.
    fn msg(type_: u32, trans: u32, flag: u32, body: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&type_.to_be_bytes());
        v.extend_from_slice(&trans.to_be_bytes());
        v.extend_from_slice(&flag.to_be_bytes());
        v.extend_from_slice(&0u32.to_be_bytes()); // len
        v.extend_from_slice(&0u32.to_be_bytes()); // len2
        v.extend_from_slice(&0u16.to_be_bytes()); // hc
        assert_eq!(v.len(), HL_HDR_LEN);
        v.extend_from_slice(body);
        v
    }

    fn chunk(tag: u16, data: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&tag.to_be_bytes());
        v.extend_from_slice(&(data.len() as u16).to_be_bytes());
        v.extend_from_slice(data);
        v
    }

    #[test]
    fn header_parses_and_detects_error() {
        let m = msg(0x0001_0000, 0x42, 0x1, &[]);
        let h = Header::parse(&m).unwrap();
        assert_eq!(h.type_, 0x0001_0000);
        assert_eq!(h.trans, 0x42);
        assert!(h.in_error());

        let ok = msg(0x0001_0000, 0x42, 0x0, &[]);
        assert!(!Header::parse(&ok).unwrap().in_error());
    }

    #[test]
    fn header_too_short_is_none() {
        assert!(Header::parse(&[0u8; 10]).is_none());
    }

    #[test]
    fn selfinfo_full() {
        let mut body = Vec::new();
        body.extend(chunk(tag::ACCESS, &0x0000_0000_DEAD_BEEFu64.to_be_bytes()));
        // USER_LIST: uid=0x1234 icon=0x0005 color=0 nlen=3 name="bob"
        let mut ul = Vec::new();
        ul.extend_from_slice(&0x1234u16.to_be_bytes());
        ul.extend_from_slice(&0x0005u16.to_be_bytes());
        ul.extend_from_slice(&0u16.to_be_bytes());
        ul.extend_from_slice(&3u16.to_be_bytes());
        ul.extend_from_slice(b"bob");
        body.extend(chunk(tag::USER_LIST, &ul));
        body.extend(chunk(tag::COLOR, &0x0000_FF00u32.to_be_bytes()));

        let m = msg(0x0000_0162, 1, 0, &body);
        let si = parse_selfinfo(&m, m.len());
        assert_eq!(si.seen, SELFINFO_ACCESS | SELFINFO_USER_LIST | SELFINFO_NICK_COLOR);
        assert_eq!(si.access, 0xDEAD_BEEF);
        assert_eq!(si.uid, 0x1234);
        assert_eq!(si.icon, 0x0005);
        assert_eq!(si.nick_color, 0x0000_FF00);
        assert_eq!(si.cached_name, b"bob");
    }

    #[test]
    fn selfinfo_clamps_name_to_31() {
        let long = vec![b'x'; 200];
        let mut ul = Vec::new();
        ul.extend_from_slice(&1u16.to_be_bytes()); // uid
        ul.extend_from_slice(&0u16.to_be_bytes()); // icon
        ul.extend_from_slice(&0u16.to_be_bytes()); // color
        ul.extend_from_slice(&200u16.to_be_bytes()); // nlen (lies)
        ul.extend_from_slice(&long);
        let m = msg(0x0000_0162, 1, 0, &chunk(tag::USER_LIST, &ul));
        let si = parse_selfinfo(&m, m.len());
        assert_eq!(si.cached_name.len(), 31);
    }

    #[test]
    fn selfinfo_rejects_wrong_length_access() {
        // ACCESS chunk must be exactly 8 bytes; a 4-byte one is ignored.
        let m = msg(0x0000_0162, 1, 0, &chunk(tag::ACCESS, &[0, 0, 0, 1]));
        let si = parse_selfinfo(&m, m.len());
        assert_eq!(si.seen & SELFINFO_ACCESS, 0);
    }

    #[test]
    fn selfinfo_empty_body() {
        let m = msg(0x0000_0162, 1, 0, &[]);
        let si = parse_selfinfo(&m, m.len());
        assert_eq!(si.seen, 0);
    }

    // ---- chat ----

    #[test]
    fn chat_extracts_body_cid_uid() {
        let mut body = Vec::new();
        body.extend(chunk(tag::BODY, b"hello"));
        body.extend(chunk(tag::CHAT_ID, &7u32.to_be_bytes()));
        body.extend(chunk(tag::UID, &0x1234u16.to_be_bytes()));
        let m = msg(0x0000_0068, 1, 0, &body);
        let c = parse_chat(&m, m.len(), 8192);
        assert_eq!(c.cid, 7);
        assert_eq!(c.uid, 0x1234);
        assert_eq!(c.text(), b"hello");
        assert_eq!(c.text_off, 0);
    }

    #[test]
    fn chat_strips_leading_lf_after_cr2lf() {
        // Wire CR before the "User:" — CR2LF makes it '\n', then the
        // leading-LF strip advances text past it.
        let m = msg(0x0000_0068, 1, 0, &chunk(tag::BODY, b"\rBob: hi"));
        let c = parse_chat(&m, m.len(), 8192);
        assert_eq!(c.buf, b"\nBob: hi"); // full sanitised line keeps the LF
        assert_eq!(c.text(), b"Bob: hi"); // display text drops it
        assert_eq!(c.text_off, 1);
    }

    #[test]
    fn chat_applies_strip_ansi() {
        // 0x0e (14) folds to 'N'; CR (0x0d) -> LF.
        let m = msg(0x0000_0068, 1, 0, &chunk(tag::BODY, b"a\x0eb\rc"));
        let c = parse_chat(&m, m.len(), 8192);
        assert_eq!(c.text(), b"aNb\nc");
    }

    #[test]
    fn chat_caps_body() {
        let big = vec![b'x'; 9000];
        let m = msg(0x0000_0068, 1, 0, &chunk(tag::BODY, &big));
        let c = parse_chat(&m, m.len(), 8192);
        assert_eq!(c.buf.len(), 8192);
    }

    #[test]
    fn chat_empty_is_valid() {
        let m = msg(0x0000_0068, 1, 0, &[]);
        let c = parse_chat(&m, m.len(), 8192);
        assert_eq!(c.cid, 0);
        assert_eq!(c.uid, 0);
        assert_eq!(c.text(), b"");
        assert_eq!(c.text_off, 0);
    }

    // ---- chat subject ----

    #[test]
    fn chat_subject_extracts() {
        let mut body = Vec::new();
        body.extend(chunk(tag::CHAT_ID, &3u32.to_be_bytes()));
        body.extend(chunk(tag::CHAT_SUBJECT, b"Welcome"));
        let m = msg(0x0000_0077, 1, 0, &body);
        let s = parse_chat_subject(&m, m.len(), 255);
        assert_eq!(s.cid, 3);
        assert_eq!(s.subject, b"Welcome");
    }

    #[test]
    fn chat_subject_not_sanitised() {
        // CR must survive — subjects are not CR2LF'd.
        let m = msg(0x0000_0077, 1, 0, &chunk(tag::CHAT_SUBJECT, b"a\rb"));
        let s = parse_chat_subject(&m, m.len(), 255);
        assert_eq!(s.subject, b"a\rb");
    }

    #[test]
    fn chat_subject_caps() {
        let big = vec![b's'; 400];
        let m = msg(0x0000_0077, 1, 0, &chunk(tag::CHAT_SUBJECT, &big));
        let s = parse_chat_subject(&m, m.len(), 255);
        assert_eq!(s.subject.len(), 255);
    }

    // ---- chat invite ----

    #[test]
    fn chat_invite_extracts() {
        let mut body = Vec::new();
        body.extend(chunk(tag::UID, &42u16.to_be_bytes()));
        body.extend(chunk(tag::CHAT_ID, &99u32.to_be_bytes()));
        body.extend(chunk(tag::NAME, b"Alice"));
        let m = msg(0x0000_0071, 1, 0, &body);
        let inv = parse_chat_invite(&m, m.len(), 31);
        assert_eq!(inv.uid, 42);
        assert_eq!(inv.cid, 99);
        assert_eq!(inv.name, b"Alice");
    }

    #[test]
    fn chat_invite_strips_ansi_and_caps_name() {
        let mut n = vec![0x0eu8]; // folds to 'N'
        n.extend(vec![b'q'; 40]);
        let m = msg(0x0000_0071, 1, 0, &chunk(tag::NAME, &n));
        let inv = parse_chat_invite(&m, m.len(), 31);
        assert_eq!(inv.name.len(), 31);
        assert_eq!(inv.name[0], b'N');
    }

    // ---- user part ----

    #[test]
    fn user_part_extracts_uid_and_cid() {
        let mut body = Vec::new();
        body.extend(chunk(tag::UID, &0x1234u16.to_be_bytes()));
        body.extend(chunk(tag::CHAT_ID, &7u32.to_be_bytes()));
        let m = msg(0x0000_012e, 1, 0, &body);
        let p = parse_user_part(&m, m.len());
        assert_eq!(p.uid, 0x1234);
        assert_eq!(p.cid, 7);
    }

    #[test]
    fn user_part_missing_chunks_default_zero() {
        let m = msg(0x0000_012e, 1, 0, &[]);
        let p = parse_user_part(&m, m.len());
        assert_eq!(p.uid, 0);
        assert_eq!(p.cid, 0);
    }

    // ---- user change ----

    #[test]
    fn user_change_extracts_all_fields() {
        let mut body = Vec::new();
        body.extend(chunk(tag::UID, &0x00abu16.to_be_bytes()));
        body.extend(chunk(tag::ICON, &0x0005u16.to_be_bytes()));
        body.extend(chunk(tag::NAME, b"alice"));
        body.extend(chunk(tag::COLOUR, &0x0002u16.to_be_bytes()));
        body.extend(chunk(tag::COLOR, &0x00ff_8800u32.to_be_bytes()));
        body.extend(chunk(tag::CHAT_ID, &0u32.to_be_bytes()));
        let m = msg(0x0000_012d, 1, 0, &body);
        let uc = parse_user_change(&m, m.len(), 31);
        assert_eq!(uc.uid, 0x00ab);
        assert_eq!(uc.icon, 0x0005);
        assert_eq!(uc.color, 0x0002);
        assert!(uc.got_color);
        assert_eq!(uc.nick_color, 0x00ff_8800);
        assert!(uc.got_nick_color);
        assert_eq!(uc.cid, 0);
        assert_eq!(uc.name, b"alice");
    }

    #[test]
    fn user_change_defaults_when_chunks_missing() {
        let m = msg(0x0000_012d, 1, 0, &[]);
        let uc = parse_user_change(&m, m.len(), 31);
        assert_eq!(uc.uid, 0);
        assert_eq!(uc.icon, 0);
        assert_eq!(uc.color, 0);
        assert!(!uc.got_color);
        assert_eq!(uc.nick_color, NICK_COLOR_NONE);
        assert!(!uc.got_nick_color);
        assert_eq!(uc.cid, 0);
        assert!(uc.name.is_empty());
    }

    #[test]
    fn user_change_strips_ansi_and_caps_name() {
        // 0x0e folds to 'N'; pad past 31 bytes to verify the cap.
        let mut n = vec![0x0eu8];
        n.extend(vec![b'q'; 40]);
        let m = msg(0x0000_012d, 1, 0, &chunk(tag::NAME, &n));
        let uc = parse_user_change(&m, m.len(), 31);
        assert_eq!(uc.name.len(), 31);
        assert_eq!(uc.name[0], b'N');
    }

    #[test]
    fn user_change_rejects_wrong_length_color() {
        // COLOR (Colored-Nicknames) must be exactly 4 bytes; a 2-byte
        // payload leaves got_nick_color false and nick_color at NONE.
        let m = msg(0x0000_012d, 1, 0, &chunk(tag::COLOR, &[0xff, 0x00]));
        let uc = parse_user_change(&m, m.len(), 31);
        assert!(!uc.got_nick_color);
        assert_eq!(uc.nick_color, NICK_COLOR_NONE);
    }

    #[test]
    fn user_change_name_not_cr2lfd() {
        // CR (0x0d) is outside strip_ansi's band and not folded by
        // CR2LF (which the user-change handler doesn't apply), so it
        // survives untouched in the captured name.
        let m = msg(0x0000_012d, 1, 0, &chunk(tag::NAME, b"a\rb"));
        let uc = parse_user_change(&m, m.len(), 31);
        assert_eq!(uc.name, b"a\rb");
    }

    // ---- task error ----

    #[test]
    fn task_error_extracts_and_sanitises() {
        let m = msg(0x0001_0000, 1, 1, &chunk(tag::TASK_ERROR, b"bad\rthing\x0e"));
        let e = parse_task_error(&m, m.len(), 256).expect("present");
        assert_eq!(e, b"bad\nthing\x4e"); // CR → LF, 0x0e → 'N' (0x4e)
    }

    #[test]
    fn task_error_caps_length() {
        let big = vec![b'x'; 500];
        let m = msg(0x0001_0000, 1, 1, &chunk(tag::TASK_ERROR, &big));
        let e = parse_task_error(&m, m.len(), 64).expect("present");
        assert_eq!(e.len(), 64);
    }

    #[test]
    fn task_error_returns_none_when_missing() {
        let m = msg(0x0001_0000, 1, 1, &[]);
        assert!(parse_task_error(&m, m.len(), 256).is_none());
    }

    // ---- msg ----

    #[test]
    fn msg_extracts_uid_name_body() {
        let mut body = Vec::new();
        body.extend(chunk(tag::UID, &0x0042u16.to_be_bytes()));
        body.extend(chunk(tag::NAME, b"alice"));
        body.extend(chunk(tag::BODY, b"hello"));
        let m = msg(0x0000_006a, 1, 0, &body);
        let p = parse_msg(&m, m.len(), 128, 8192);
        assert_eq!(p.uid, 0x42);
        assert_eq!(p.name, b"alice");
        assert_eq!(p.msg, b"hello");
    }

    #[test]
    fn msg_sanitises_body_cr2lf_and_strip_ansi() {
        // CR → LF; 0x0e → 'N'.
        let m = msg(0x0000_006a, 1, 0, &chunk(tag::BODY, b"hi\rthere\x0e"));
        let p = parse_msg(&m, m.len(), 128, 8192);
        assert_eq!(p.msg, b"hi\nthere\x4e");
    }

    #[test]
    fn msg_caps_lengths() {
        let big_name = vec![b'a'; 200];
        let big_body = vec![b'x'; 9000];
        let mut body = Vec::new();
        body.extend(chunk(tag::NAME, &big_name));
        body.extend(chunk(tag::BODY, &big_body));
        let m = msg(0x0000_006a, 1, 0, &body);
        let p = parse_msg(&m, m.len(), 128, 8192);
        assert_eq!(p.name.len(), 128);
        assert_eq!(p.msg.len(), 8192);
    }

    #[test]
    fn msg_name_strip_ansi_but_no_cr2lf() {
        // CR survives in the name (the C handler never CR2LF's names),
        // but 0x0e in the name still gets folded.
        let m = msg(0x0000_006a, 1, 0, &chunk(tag::NAME, b"a\rb\x0e"));
        let p = parse_msg(&m, m.len(), 128, 8192);
        assert_eq!(p.name, b"a\rb\x4e");
    }

    #[test]
    fn msg_empty_yields_zero_uid_and_empty_strings() {
        let m = msg(0x0000_006a, 1, 0, &[]);
        let p = parse_msg(&m, m.len(), 128, 8192);
        assert_eq!(p.uid, 0);
        assert!(p.name.is_empty());
        assert!(p.msg.is_empty());
    }

    // ---- banner ----

    #[test]
    fn banner_extracts_type_and_url() {
        let mut body = Vec::new();
        body.extend(chunk(tag::BANNER_TYPE, b"URL "));
        body.extend(chunk(tag::BANNER_URL, b"https://example.com/banner.png"));
        let m = msg(0x0000_010d, 1, 0, &body);
        let b = parse_banner(&m, m.len(), 1024);
        assert!(b.got_type);
        assert_eq!(&b.type_code, b"URL ");
        assert_eq!(b.url.as_deref(), Some(&b"https://example.com/banner.png"[..]));
    }

    #[test]
    fn banner_rejects_wrong_length_type() {
        // Anything but exactly 4 bytes is malformed and not adopted.
        let m = msg(0x0000_010d, 1, 0, &chunk(tag::BANNER_TYPE, b"URL"));
        let b = parse_banner(&m, m.len(), 1024);
        assert!(!b.got_type);
        assert_eq!(b.type_code, [0; 4]);
    }

    #[test]
    fn banner_url_optional() {
        let m = msg(0x0000_010d, 1, 0, &chunk(tag::BANNER_TYPE, b"JPEG"));
        let b = parse_banner(&m, m.len(), 1024);
        assert!(b.got_type);
        assert_eq!(&b.type_code, b"JPEG");
        assert!(b.url.is_none());
    }

    #[test]
    fn banner_caps_url() {
        let big = vec![b'u'; 2000];
        let mut body = Vec::new();
        body.extend(chunk(tag::BANNER_TYPE, b"URL "));
        body.extend(chunk(tag::BANNER_URL, &big));
        let m = msg(0x0000_010d, 1, 0, &body);
        let b = parse_banner(&m, m.len(), 1024);
        assert_eq!(b.url.as_deref().unwrap().len(), 1024);
    }

    // ---- xfer queue ----

    #[test]
    fn xfer_queue_extracts() {
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &0xdeadbeefu32.to_be_bytes()));
        body.extend(chunk(tag::QUEUE, &3u32.to_be_bytes()));
        let m = msg(0x0000_00d3, 1, 0, &body);
        let q = parse_xfer_queue(&m, m.len());
        assert_eq!(q.htxf_ref, 0xdead_beef);
        assert_eq!(q.queueid, 3);
    }

    #[test]
    fn xfer_queue_defaults_zero() {
        let m = msg(0x0000_00d3, 1, 0, &[]);
        let q = parse_xfer_queue(&m, m.len());
        assert_eq!(q.htxf_ref, 0);
        assert_eq!(q.queueid, 0);
    }

    // ---- agreement ----

    #[test]
    fn agreement_noagreement_sentinel() {
        let m = msg(0x0000_006d, 1, 0, &chunk(tag::NOAGREEMENT, &[]));
        let (r, body) = parse_agreement(&m, m.len(), 4096);
        assert_eq!(r, AgreementResult::None);
        assert!(body.is_empty());
    }

    #[test]
    fn agreement_ok_with_sanitised_body() {
        // CR → LF, 0x0e → 'N'.
        let m = msg(0x0000_006d, 1, 0, &chunk(tag::BODY, b"line\rmore\x0e"));
        let (r, body) = parse_agreement(&m, m.len(), 4096);
        assert_eq!(r, AgreementResult::Ok);
        assert_eq!(body, b"line\nmore\x4e");
    }

    #[test]
    fn agreement_missing_when_neither_chunk_present() {
        let m = msg(0x0000_006d, 1, 0, &[]);
        let (r, body) = parse_agreement(&m, m.len(), 4096);
        assert_eq!(r, AgreementResult::Missing);
        assert!(body.is_empty());
    }

    #[test]
    fn agreement_first_chunk_wins() {
        // BODY before NOAGREEMENT → Ok (the body is taken).
        let mut bodybuf = Vec::new();
        bodybuf.extend(chunk(tag::BODY, b"ok"));
        bodybuf.extend(chunk(tag::NOAGREEMENT, &[]));
        let m = msg(0x0000_006d, 1, 0, &bodybuf);
        let (r, _) = parse_agreement(&m, m.len(), 4096);
        assert_eq!(r, AgreementResult::Ok);

        // NOAGREEMENT before BODY → None (sentinel wins).
        let mut bodybuf = Vec::new();
        bodybuf.extend(chunk(tag::NOAGREEMENT, &[]));
        bodybuf.extend(chunk(tag::BODY, b"ignored"));
        let m = msg(0x0000_006d, 1, 0, &bodybuf);
        let (r, body) = parse_agreement(&m, m.len(), 4096);
        assert_eq!(r, AgreementResult::None);
        assert!(body.is_empty());
    }

    #[test]
    fn agreement_caps_length() {
        let big = vec![b'x'; 5000];
        let m = msg(0x0000_006d, 1, 0, &chunk(tag::BODY, &big));
        let (r, body) = parse_agreement(&m, m.len(), 4096);
        assert_eq!(r, AgreementResult::Ok);
        assert_eq!(body.len(), 4096);
    }
}
