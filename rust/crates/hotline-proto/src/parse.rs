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

use crate::messages::tag;
use crate::wire::{ChunkIter, Decoder};

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
}
