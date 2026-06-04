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

// ---- HTLS_HDR_NEWS reply (HTLS_DATA_NEWS chunks) ------------------------

/// Extract the first `HTLS_DATA_NEWS` chunk's CR2LF + `strip_ansi`
/// sanitised body. `None` when no NEWS chunk is present (the C
/// `hx_news_file_extract` returns FALSE and leaves the caller buffer
/// untouched in that case — same sentinel discipline as
/// [`parse_task_error`]).
pub fn parse_news_file(buf: &[u8], len: usize, max_len: usize) -> Option<Vec<u8>> {
    for chunk in ChunkIter::over_message(buf, len) {
        if chunk.tag == tag::NEWS {
            let take = chunk.data.len().min(max_len);
            let mut v = chunk.data[..take].to_vec();
            cr2lf(&mut v);
            strip_ansi(&mut v);
            return Some(v);
        }
    }
    None
}

/// Iterate the `HTLS_DATA_NEWS` chunks of a message body, yielding
/// each one's CR2LF + `strip_ansi` sanitised contents. Mirrors the
/// per-chunk-emit contract of the C `hx_news_post_walk` (one callback
/// invocation per NEWS chunk; non-NEWS chunks are skipped).
pub struct NewsPostIter<'a> {
    inner: ChunkIter<'a>,
    max_len: usize,
}

impl<'a> Iterator for NewsPostIter<'a> {
    type Item = Vec<u8>;
    fn next(&mut self) -> Option<Vec<u8>> {
        loop {
            let chunk = self.inner.next()?;
            if chunk.tag != tag::NEWS {
                continue;
            }
            let take = chunk.data.len().min(self.max_len);
            let mut v = chunk.data[..take].to_vec();
            cr2lf(&mut v);
            strip_ansi(&mut v);
            return Some(v);
        }
    }
}

/// Build an iterator over the sanitised bodies of every `HTLS_DATA_NEWS`
/// chunk in `buf[..len]`.
pub fn news_post_chunks(buf: &[u8], len: usize, max_len: usize) -> NewsPostIter<'_> {
    NewsPostIter {
        inner: ChunkIter::over_message(buf, len),
        max_len,
    }
}

// ---- HTLC_HDR_GETTHREAD reply ------------------------------------------
//
// GETTHREAD is a client opcode (HTLC_HDR_*); the server's reply arrives
// inside an HTLS_HDR_TASK frame, and this parser walks that post-TASK
// payload. Backs the C `rcv_task_news_post` handler (the function name
// is historical — it's the news-thread-fetch reply, not a news-post
// notification).

/// Parsed GETTHREAD reply — a single news-thread article body plus its
/// optional thread id. The body has CR2LF + `strip_ansi` applied
/// (matching the C extractor).
#[derive(Debug, Clone, Default)]
pub struct NewsThreadReply {
    /// `Some(bytes)` when a NEWSDATA chunk was present (and no
    /// TASK_ERROR short-circuited the walk). The bytes are CR2LF +
    /// `strip_ansi`'d and capped at `max_text`. `None` means no
    /// body in the reply — the C caller bails on that case.
    pub text: Option<Vec<u8>>,
    /// THREADID chunk value, zero when absent. The C handler
    /// decodes this into a local but never uses it; surfaced here
    /// for completeness in case a future caller wants the id.
    pub thread_id: u32,
    /// `true` when a HTLS_DATA_TASKERROR chunk was seen mid-walk.
    /// The C extractor bails on the spot — callers should treat
    /// this as "no body, drop the reply".
    pub has_task_error: bool,
}

/// Parse the post-`HTLC_HDR_GETTHREAD` TASK reply payload (the C
/// `rcv_task_news_post` body). `max_text` caps the NEWSDATA body.
/// TASK_ERROR short-circuits: when seen, the walk stops immediately
/// and the body is dropped, matching the C `case HTLS_DATA_TASKERROR:
/// return;` early-out.
pub fn parse_news_thread_reply(
    buf: &[u8],
    len: usize,
    max_text: usize,
) -> NewsThreadReply {
    let mut out = NewsThreadReply::default();
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::NEWSDATA => {
                let take = chunk.data.len().min(max_text);
                let mut v = chunk.data[..take].to_vec();
                cr2lf(&mut v);
                strip_ansi(&mut v);
                out.text = Some(v);
            }
            tag::THREADID => out.thread_id = chunk.as_uint(),
            tag::TASK_ERROR => {
                // Mirror the C extractor's early-out: drop any body
                // we may have parsed and signal the error.
                out.text = None;
                out.has_task_error = true;
                break;
            }
            _ => {}
        }
    }
    out
}

// ---- HTLC_DATA_CATLIST (1.5 threaded-news article listing) -------------

/// One mime part attached to a [`CatPost`]. Mirrors `struct
/// hx_newscat_part`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatPart {
    /// Raw mime-type bytes. Empty when the wire pstring was zero-length;
    /// callers materialise it to whatever string type they need.
    pub mime_type: Vec<u8>,
    pub size: u16,
}

/// One article in a [`CatList`]. Mirrors `struct hx_newscat_post`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatPost {
    pub postid: u32,
    pub parentid: u32,
    /// Mac classic 8-byte date split the way `news15.c` consumes it.
    pub date_base_year: u16,
    pub date_pad: u16,
    pub date_seconds: u32,
    pub partcount: u16,
    /// Sum of every `parts[].size`. The C parser tracks this as a u16;
    /// we preserve the type so a forged sum can't surprise the C
    /// side via truncation (it'd wrap in C too).
    pub size_total: u16,
    /// Raw subject bytes (empty when zero-length on the wire).
    pub subject: Vec<u8>,
    /// Raw sender bytes (empty when zero-length on the wire).
    pub sender: Vec<u8>,
    /// Parts; length always equals [`Self::partcount`].
    pub parts: Vec<CatPart>,
}

/// Parsed `HTLC_DATA_CATLIST` payload.
///
/// This is the reply body of `HTLC_HDR_NEWSCATLIST` (the 1.5 threaded
/// news article listing). Wire shape (see mhxd's
/// `hl_news_threadlist_hdr` + `hl_news_thread_hdr`, and the doc on
/// [`parse_catlist`]):
///
/// ```text
/// u32 __x0           opaque
/// u32 post_count
/// u16 __x1           opaque
/// per post:
///   u32 postid
///   u16 date.base_year
///   u16 date.pad
///   u32 date.seconds
///   u32 parentid
///   u32 __flags       opaque (post header is 22 bytes total)
///   u16 partcount
///   pstring subject
///   pstring sender
///   per part:
///     pstring mime_type
///     u16     size
/// ```
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatList {
    pub posts: Vec<CatPost>,
}

/// Per-post minimum-on-wire byte budget: 22-byte fixed header + 2 (two
/// zero-length pstrings). Matches the C `(post_count > remaining / 24)`
/// defensive reject.
const MIN_POST_BYTES: usize = 24;
/// Per-part minimum-on-wire byte budget: 1 byte length + u16 size.
const MIN_PART_BYTES: usize = 3;

/// Parse the first `HTLC_DATA_CATLIST` chunk in `buf[..len]`. Returns
/// `None` when no CATLIST chunk is present **or** the body is
/// malformed — matching the C `hx_newscat_parse`'s FALSE return on
/// either condition. Subsequent CATLIST chunks (the wire convention is
/// one per reply) are ignored, just like the C "first chunk wins".
///
/// The defensive caps mirror the C parser:
///
/// - `post_count > remaining / 24` ⇒ reject (a forged count larger
///   than the chunk can possibly fit).
/// - `partcount > remaining / 3` ⇒ reject.
///
/// These caps fail closed before any heap allocation, so a single
/// malicious CATLIST can't drive us to allocate gigabytes.
pub fn parse_catlist(buf: &[u8], len: usize) -> Option<CatList> {
    for chunk in ChunkIter::over_message(buf, len) {
        if chunk.tag != tag::CATLIST {
            continue;
        }
        return parse_catlist_body(chunk.data);
    }
    None
}

fn parse_catlist_body(body: &[u8]) -> Option<CatList> {
    let mut d = Decoder::new(body);
    // Threadlist header: u32 __x0, u32 post_count, u16 __x1 = 10 bytes.
    let _x0 = d.u32()?;
    let post_count = d.u32()?;
    let _x1 = d.u16()?;

    if post_count == 0 {
        return Some(CatList { posts: Vec::new() });
    }

    let remaining = d.remaining();
    if (post_count as usize) > remaining / MIN_POST_BYTES {
        return None;
    }

    let mut posts = Vec::with_capacity(post_count as usize);
    for _ in 0..post_count {
        // 22-byte fixed thread header.
        let postid = d.u32()?;
        let date_base_year = d.u16()?;
        let date_pad = d.u16()?;
        let date_seconds = d.u32()?;
        let parentid = d.u32()?;
        let _flags = d.u32()?;
        let partcount = d.u16()?;

        let subject = d.pstring()?.to_vec();
        let sender = d.pstring()?.to_vec();

        // Allocate parts only AFTER the partcount-vs-remaining check;
        // otherwise a forged partcount (e.g. u16::MAX) drives a giant
        // Vec::with_capacity reservation before the defensive reject
        // would fire — same "fail closed before large allocation"
        // property the C parser has for post_count.
        let mut size_total: u16 = 0;
        let mut parts: Vec<CatPart> = Vec::new();

        if partcount > 0 {
            let rem = d.remaining();
            if (partcount as usize) > rem / MIN_PART_BYTES {
                return None;
            }
            parts.reserve_exact(partcount as usize);
            for _ in 0..partcount {
                let mime_type = d.pstring()?.to_vec();
                let size = d.u16()?;
                size_total = size_total.wrapping_add(size);
                parts.push(CatPart { mime_type, size });
            }
        }

        posts.push(CatPost {
            postid,
            parentid,
            date_base_year,
            date_pad,
            date_seconds,
            partcount,
            size_total,
            subject,
            sender,
            parts,
        });
    }

    Some(CatList { posts })
}

// ---- HTLC_DATA_NEWSFOLDERITEM / HTLC_DATA_CATEGORYITEM ------------------

/// Kind of a 1.5 threaded-news directory entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NewsDirKind {
    /// folder-entry (ntype == 1 in HTLC_DATA_NEWSFOLDERITEM, or
    /// HTLC_DATA_CATEGORYITEM with ntype == 2).
    Folder,
    /// category-entry (everything else).
    Category,
}

/// One entry from a 1.5 threaded-news directory listing.
#[derive(Debug, Clone)]
pub struct NewsDirEntry {
    pub kind: NewsDirKind,
    /// Name bytes (no NUL, capped at the caller's max).
    pub name: Vec<u8>,
}

/// Parse an `HTLC_DATA_NEWSFOLDERITEM` (0x0140) chunk body:
/// `u8 ntype`, then `name[dlen - 1]`. `ntype == 1` ⇒ folder, else
/// category. The name is the rest of the chunk body, capped at
/// `max_name` bytes. Returns `None` on a zero-length body — that's
/// the C extractor's `dlen < 1` reject.
pub fn parse_news_folderitem(data: &[u8], max_name: usize) -> Option<NewsDirEntry> {
    if data.is_empty() {
        return None;
    }
    let kind = if data[0] == 1 {
        NewsDirKind::Folder
    } else {
        NewsDirKind::Category
    };
    let body = &data[1..];
    let take = body.len().min(max_name);
    Some(NewsDirEntry {
        kind,
        name: body[..take].to_vec(),
    })
}

/// Parse an `HTLC_DATA_CATEGORYITEM` (0x0143) chunk body:
///
/// - `ntype` (u16 BE): 2 → bundle/folder, 3 → category. Anything else
///   is malformed and yields `None`.
/// - For `ntype == 2`: header is `ntype(2) + count(2)` = 4 bytes;
///   then `namelen(u8)` and `name[namelen]`.
/// - For `ntype == 3`: header is `ntype(2) + count(2) + guid(16) +
///   addsn(4) + deletesn(4)` = 28 bytes; then `namelen(u8)` and
///   `name[namelen]`.
///
/// Returns `None` on any truncation or unknown `ntype` (matches the C
/// extractor's defensive rejects).
pub fn parse_news_categoryitem(data: &[u8], max_name: usize) -> Option<NewsDirEntry> {
    let mut d = Decoder::new(data);
    let ntype = d.u16()?;
    let (kind, header_after_ntype) = match ntype {
        2 => (NewsDirKind::Folder, 2usize),   // count(2) before namelen
        3 => (NewsDirKind::Category, 26usize), // count(2)+guid(16)+addsn(4)+deletesn(4)
        _ => return None,
    };
    // Skip the remainder of the fixed header.
    d.bytes(header_after_ntype)?;
    // namelen (u8)
    let namelen = *d.bytes(1)?.first()? as usize;
    let nm = d.bytes(namelen)?;
    let take = nm.len().min(max_name);
    Some(NewsDirEntry {
        kind,
        name: nm[..take].to_vec(),
    })
}

// ---- HTLS_DATA_USER_LIST record ----------------------------------------
//
// Per-user record carried inside `HTLS_DATA_USER_LIST` chunks. The same
// wire shape appears in two places:
//
//   - `HTLS_HDR_USER_SELFINFO` carries exactly one record (our self).
//     `parse_selfinfo` pulls the uid/icon/name out for the login state
//     machine; the COLOR chunk (separate from the trailer here) carries
//     our colour.
//   - The post-`HTLC_HDR_USER_GETLIST` TASK reply carries N records,
//     one per online user, in `rcv_task_user_list`. That walker is in
//     C and still drives GTK side effects (hx_user_new, signal emit).
//
// Body layout (`chunk.data` after the 4-byte data header that
// `ChunkIter` already consumed):
//
//   u16 uid, u16 icon, u16 color, u16 nlen, u8 name[nlen],
//   [optional u32 nick_color trailer per Colored-Nicknames extension]
//
// The trailer is the 4-byte big-endian 0x00RRGGBB per-user nick
// colour. When absent the caller falls back to the status palette
// (the `nick_color: None` case here corresponds to the C
// `HX_NICK_COLOR_NONE` sentinel).

/// One parsed USER_LIST record.
#[derive(Debug, Clone)]
pub struct UserListRecord {
    pub uid: u16,
    pub icon: u16,
    pub color: u16,
    /// `strip_ansi`'d name bytes, capped at `max_name`. Two-stage
    /// clamp: first against bytes actually available in the chunk
    /// (defends against a malicious or buggy server lying about
    /// `nlen` vs. the chunk length); then against `max_name`.
    pub name: Vec<u8>,
    /// `Some(0x00RRGGBB)` when the Colored-Nicknames trailer was
    /// present (chunk had at least `8 + clamped_nlen + 4` bytes).
    /// `None` mirrors the C `HX_NICK_COLOR_NONE` (0xffffffff)
    /// sentinel — callers that need the u32 sentinel can substitute
    /// via [`NICK_COLOR_NONE`].
    pub nick_color: Option<u32>,
}

/// Parse a single `HTLS_DATA_USER_LIST` chunk payload. Returns
/// `None` when fewer than the 8 fixed bytes are present (a
/// malformed record the C extractor would have skipped because
/// the dh_start macro caps `_len`).
///
/// `max_name` caps the name length (the C handlers use 31). The
/// trailer-presence test uses the *clamped* nlen (matching the C
/// `&uh->name[nlen]` indexing after the avail/31 clamp).
pub fn parse_user_list_record(data: &[u8], max_name: usize) -> Option<UserListRecord> {
    if data.len() < 8 {
        return None;
    }
    let uid = u16::from_be_bytes([data[0], data[1]]);
    let icon = u16::from_be_bytes([data[2], data[3]]);
    let color = u16::from_be_bytes([data[4], data[5]]);
    let mut nlen = u16::from_be_bytes([data[6], data[7]]) as usize;
    let avail = data.len() - 8;
    // Two-stage clamp matches the C extractor's order: chunk-body
    // availability first, then the caller's max.
    if nlen > avail {
        nlen = avail;
    }
    if nlen > max_name {
        nlen = max_name;
    }
    let mut name = data[8..8 + nlen].to_vec();
    strip_ansi(&mut name);
    // Colored-Nicknames trailer lives immediately after the
    // (clamped) name bytes — body has length 8 + nlen + 4 when it's
    // present. The clamp matters here: if the caller's max_name
    // truncated nlen below the server's actual name length, the
    // trailer offset still uses the clamped value (matches the C
    // `&uh->name[nlen]` indexing where nlen is the post-clamp local).
    let trailer_offset = 8 + nlen;
    let nick_color = if data.len() >= trailer_offset + 4 {
        Some(u32::from_be_bytes([
            data[trailer_offset],
            data[trailer_offset + 1],
            data[trailer_offset + 2],
            data[trailer_offset + 3],
        ]))
    } else {
        None
    };
    Some(UserListRecord {
        uid,
        icon,
        color,
        name,
        nick_color,
    })
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

// ---- HTLC_HDR_USER_GETINFO reply ---------------------------------------
//
// USER_GETINFO is a client opcode (HTLC_HDR_*); the server's response
// arrives inside an HTLS_HDR_TASK frame, and this parser walks that
// post-TASK payload.

/// Parsed user-info reply. Mirrors the C `rcv_task_user_info` extractor:
/// the nickname is `strip_ansi`'d (no CR2LF) and the info body is CR2LF
/// + `strip_ansi`'d.
#[derive(Debug, Clone)]
pub struct UserInfo {
    /// Nickname bytes after `strip_ansi`, capped at `max_name`.
    pub name: Vec<u8>,
    /// User-info body bytes after CR2LF + `strip_ansi`, capped at
    /// `max_info`.
    pub info: Vec<u8>,
}

/// Parse the post-`HTLC_HDR_USER_GETINFO` TASK reply payload (the C
/// `rcv_task_user_info` body). `max_name` / `max_info` cap the two
/// strings the same way the C extractor does (31 / 4096). Missing
/// chunks are returned as empty `Vec`s — the C caller's gate is
/// `nlen && ilen`, so the empty-`name`-and-info pair behaves identically.
pub fn parse_user_info(buf: &[u8], len: usize, max_name: usize, max_info: usize) -> UserInfo {
    let mut name: Vec<u8> = Vec::new();
    let mut info: Vec<u8> = Vec::new();

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            // USER_INFO aliases 0x0065 BODY — same code point as
            // chat / msg / news-post / agreement, distinct by opcode
            // context (this parser is gated by the rcv_task fn).
            tag::BODY => {
                let take = chunk.data.len().min(max_info);
                info = chunk.data[..take].to_vec();
            }
            tag::NAME => {
                let take = chunk.data.len().min(max_name);
                name = chunk.data[..take].to_vec();
            }
            _ => {}
        }
    }

    strip_ansi(&mut name);
    cr2lf(&mut info);
    strip_ansi(&mut info);
    UserInfo { name, info }
}

// ---- HTLC_HDR_ACCOUNT_READ reply ---------------------------------------
//
// ACCOUNT_READ is a client opcode (HTLC_HDR_*); the server's response
// arrives inside an HTLS_HDR_TASK frame, and this parser walks that
// post-TASK payload.

/// Parsed account-read reply. Mirrors the C `rcv_task_user_open`
/// extractor: NAME bytes verbatim (no `strip_ansi` — the C handler
/// also leaves the display name untouched here), LOGIN /
/// PASSWORD chunks de-obfuscated with the XOR-0xff transform
/// (`hl_decode` in `hl_code.c`), and the 8-byte ACCESS bitmap copied
/// verbatim. The C caller gates the dispatch on `accessbool` —
/// `got_access` exposes that signal.
#[derive(Debug, Clone)]
pub struct AccountRead {
    /// Account display name, capped at `max_name`. No `strip_ansi`
    /// (matches the C extractor).
    pub name: Vec<u8>,
    /// XOR-0xff-decoded login bytes, capped at `max_login`.
    pub login: Vec<u8>,
    /// XOR-0xff-decoded password bytes, capped at `max_pass`. Empty
    /// when the PASSWORD chunk is missing, single-zero (the
    /// no-password sentinel), or empty.
    pub pass: Vec<u8>,
    /// Raw 8 bytes of the ACCESS chunk, copied verbatim from the
    /// wire (matches the C `memcpy (&access, dh->data, sizeof
    /// (access))`).
    pub access: [u8; 8],
    /// `true` iff the ACCESS chunk was present and at least 8 bytes
    /// (the C gate). When `false`, the caller skips the callback
    /// entirely.
    pub got_access: bool,
}

/// Parse the post-`HTLC_HDR_ACCOUNT_READ` TASK reply payload (the C
/// `rcv_task_user_open` body). `max_name` / `max_login` / `max_pass`
/// cap the three strings (the C extractor uses 31 for all three).
///
/// LOGIN / PASSWORD chunks are XOR-0xff-decoded inline (the
/// Hotline-wire obfuscation used for credentials). The PASSWORD
/// no-password convention from the C extractor is preserved: a
/// single zero byte (or empty / missing) yields an empty `pass`
/// vector rather than a decoded one-byte 0xff string.
pub fn parse_account_read(
    buf: &[u8],
    len: usize,
    max_name: usize,
    max_login: usize,
    max_pass: usize,
) -> AccountRead {
    let mut out = AccountRead {
        name: Vec::new(),
        login: Vec::new(),
        pass: Vec::new(),
        access: [0u8; 8],
        got_access: false,
    };

    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::NAME => {
                let take = chunk.data.len().min(max_name);
                out.name = chunk.data[..take].to_vec();
            }
            tag::LOGIN => {
                let take = chunk.data.len().min(max_login);
                let mut buf = Vec::with_capacity(take);
                for &b in &chunk.data[..take] {
                    buf.push(b ^ 0xff);
                }
                out.login = buf;
            }
            tag::PASSWORD => {
                // C-side gate: plen > 1 && dh->data[0] — i.e., at
                // least two bytes AND the first byte non-zero. The
                // single-byte 0x00 case is the explicit "no password"
                // sentinel; anything else is decoded.
                //
                // The C extractor applies the `plen > 1` check
                // *after* truncating to `sizeof(pass) - 1`, so a
                // chunk that fits the wire but collapses to a single
                // byte after the cap also takes the no-password
                // path. Compute `take` first and gate on `take > 1`
                // here so a small `max_pass` (e.g. 1) behaves the
                // same way it does in C.
                //
                // Last-PASSWORD-wins semantics: the C extractor's
                // sentinel path actively sets pass[0]=0 (overwriting
                // any prior decoded password), so a server that
                // somehow emits multiple PASSWORD chunks with a
                // sentinel last would clear the buffer. Mirror that
                // by always overwriting out.pass — either with the
                // decoded bytes or with an empty Vec.
                let take = chunk.data.len().min(max_pass);
                if take > 1 && chunk.data[0] != 0 {
                    let mut buf = Vec::with_capacity(take);
                    for &b in &chunk.data[..take] {
                        buf.push(b ^ 0xff);
                    }
                    out.pass = buf;
                } else {
                    out.pass.clear();
                }
            }
            tag::ACCESS => {
                if chunk.data.len() >= 8 {
                    out.access.copy_from_slice(&chunk.data[..8]);
                    out.got_access = true;
                }
            }
            _ => {}
        }
    }

    out
}

// ---- xfer-reply parsers (post-TASK payloads) --------------------------
//
// FILE_GET / FOLDER_GET replies share the same scalar shape (HTXF_REF,
// HTXF_SIZE, optional XFERSIZE64 companion, optional QUEUE — 1.5+
// servers); folder_get adds NFILES. FILE_GETINFO is the richer reply
// the file-info dialog renders.

/// Read up to 4 bytes of a chunk's payload as a big-endian unsigned,
/// zero-extending to u32. Mirrors the C `for (i = 0; i < _len && i <
/// 4; i++) { size = (size << 8) | dh->data[i]; }` pattern that
/// FILE_SIZE uses — some servers (mhxd) emit the size in the
/// smallest BE width that fits. For exactly-4 or exactly-2 byte
/// payloads, [`Chunk::as_uint`] already handles the standard case;
/// this helper covers the variable-width tail.
fn be_uint_up_to_4(data: &[u8]) -> u32 {
    let mut v: u32 = 0;
    for &b in data.iter().take(4) {
        v = (v << 8) | u32::from(b);
    }
    v
}

/// Read the first 8 bytes of a chunk as a big-endian u64; returns
/// `None` when fewer than 8 bytes are available. Trailing bytes are
/// ignored, matching the C extractor's `if (_len >= 8)` gate that
/// FILESIZE64 / XFERSIZE64 use.
fn be_u64_first8(data: &[u8]) -> Option<u64> {
    if data.len() >= 8 {
        Some(u64::from_be_bytes([
            data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
        ]))
    } else {
        None
    }
}

/// Parsed FILE_GET reply (the post-TASK payload backing the C
/// `rcv_task_file_get`). Missing chunks default to 0; `size64_seen`
/// reports whether the Large-Files companion was present.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FileGetReply {
    pub ref_: u32,
    pub size: u32,
    pub size64: u64,
    pub size64_seen: bool,
    pub queue: u32,
}

/// Parse the FILE_GET reply scalars. The caller is responsible for the
/// `(!size && !size64_seen) || !ref` dispatch gate the C extractor
/// uses to reject malformed frames (matches the receive-side check).
pub fn parse_file_get_reply(buf: &[u8], len: usize) -> FileGetReply {
    let mut out = FileGetReply::default();
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::HTXF_REF => out.ref_ = chunk.as_uint(),
            tag::HTXF_SIZE => out.size = chunk.as_uint(),
            tag::XFERSIZE64 => {
                if let Some(v) = be_u64_first8(chunk.data) {
                    out.size64 = v;
                    out.size64_seen = true;
                }
            }
            tag::QUEUE => out.queue = chunk.as_uint(),
            _ => {}
        }
    }
    out
}

/// Parsed FOLDER_GET reply — `FileGetReply` plus the file-count
/// hint the server's progress UI uses.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FolderGetReply {
    pub ref_: u32,
    pub size: u32,
    pub size64: u64,
    pub size64_seen: bool,
    pub queue: u32,
    pub nfiles: u32,
}

/// Parse the FOLDER_GET reply scalars. Same shape as
/// [`parse_file_get_reply`] with the addition of `FILE_NFILES`.
pub fn parse_folder_get_reply(buf: &[u8], len: usize) -> FolderGetReply {
    let mut out = FolderGetReply::default();
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::HTXF_REF => out.ref_ = chunk.as_uint(),
            tag::HTXF_SIZE => out.size = chunk.as_uint(),
            tag::XFERSIZE64 => {
                if let Some(v) = be_u64_first8(chunk.data) {
                    out.size64 = v;
                    out.size64_seen = true;
                }
            }
            tag::QUEUE => out.queue = chunk.as_uint(),
            tag::FILE_NFILES => out.nfiles = chunk.as_uint(),
            _ => {}
        }
    }
    out
}

/// Parsed FILE_PUT reply — the server's response to our
/// HTLC_HDR_FILE_PUT. Carries the transfer reference, an optional
/// queue position, and an optional RFLT resume payload telling us
/// the partial-upload fork offsets to resume from.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FilePutReply {
    pub ref_: u32,
    pub queue: u32,
    /// Data-fork resume offset (RFLT bytes 46..50, BE u32). Zero
    /// when no RFLT was sent, or when the RFLT was shorter than 66
    /// bytes (the C extractor's gate; the trailer at offset 62..66
    /// is the resource fork's, so a record shorter than 66 can't
    /// carry both).
    pub data_pos: u32,
    /// Resource-fork resume offset (RFLT bytes 62..66, BE u32).
    pub rsrc_pos: u32,
}

/// Parse the FILE_PUT reply scalars. RFLT is gated at len >= 66
/// (matching the C extractor); shorter records leave data_pos /
/// rsrc_pos at zero. The caller applies the `!ref` dispatch gate.
pub fn parse_file_put_reply(buf: &[u8], len: usize) -> FilePutReply {
    let mut out = FilePutReply::default();
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::HTXF_REF => out.ref_ = chunk.as_uint(),
            tag::QUEUE => out.queue = chunk.as_uint(),
            tag::RFLT => {
                if chunk.data.len() >= 66 {
                    out.data_pos = u32::from_be_bytes([
                        chunk.data[46],
                        chunk.data[47],
                        chunk.data[48],
                        chunk.data[49],
                    ]);
                    out.rsrc_pos = u32::from_be_bytes([
                        chunk.data[62],
                        chunk.data[63],
                        chunk.data[64],
                        chunk.data[65],
                    ]);
                }
            }
            _ => {}
        }
    }
    out
}

/// Parsed FOLDER_PUT reply — strict subset of [`FilePutReply`]
/// (no RFLT; per-file resume happens inside folder_put_thread,
/// not at the task boundary).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FolderPutReply {
    pub ref_: u32,
    pub queue: u32,
}

/// Parse the FOLDER_PUT reply scalars.
pub fn parse_folder_put_reply(buf: &[u8], len: usize) -> FolderPutReply {
    let mut out = FolderPutReply::default();
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::HTXF_REF => out.ref_ = chunk.as_uint(),
            tag::QUEUE => out.queue = chunk.as_uint(),
            _ => {}
        }
    }
    out
}

/// Parsed DOWNLOAD_BANNER reply — the server hands back a transfer
/// reference and total byte count for the HTXF subchannel fetch.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BannerGetReply {
    pub ref_: u32,
    pub size: u32,
}

/// Parse the DOWNLOAD_BANNER reply scalars.
pub fn parse_banner_get_reply(buf: &[u8], len: usize) -> BannerGetReply {
    let mut out = BannerGetReply::default();
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::HTXF_REF => out.ref_ = chunk.as_uint(),
            tag::HTXF_SIZE => out.size = chunk.as_uint(),
            _ => {}
        }
    }
    out
}

// ---- FILE_GETINFO reply -------------------------------------------------

/// Parsed FILE_GETINFO reply (the post-TASK payload backing
/// `rcv_task_file_getinfo`). Mirrors the C extractor field-for-field.
///
/// Strings are NOT NUL-terminated here — capping + termination happens
/// at the FFI boundary. `name` is `strip_ansi`'d; `comment` is CR2LF
/// + `strip_ansi`'d (multi-line). `type_` / `creator` are 4-byte
/// HFS-style codes (or shorter if the server emitted fewer bytes);
/// the C extractor copies up to 31 bytes and NUL-terminates, but in
/// practice servers send exactly 4. We expose the raw bytes here and
/// let the FFI side stringify with its cap.
#[derive(Debug, Clone, Default)]
pub struct FileGetInfo {
    /// `FILE_ICON` — 4 bytes when present, else empty.
    pub icon: [u8; 4],
    pub got_icon: bool,
    /// `FILE_TYPE` — HFS type code (typically 4 bytes), capped at
    /// `max_type`.
    pub type_: Vec<u8>,
    /// `FILE_CREATOR` — HFS creator code (typically 4 bytes), capped
    /// at `max_creator`.
    pub creator: Vec<u8>,
    /// `FILE_SIZE` — legacy 1..=4-byte BE size, zero-extended to u32.
    pub size: u32,
    /// `FILESIZE64` — Large-Files companion (u64 BE).
    pub size64: u64,
    pub size64_seen: bool,
    /// `FILE_NAME` — strip_ansi'd, capped at `max_name`.
    pub name: Vec<u8>,
    /// `FILE_DATE_CREATE` — raw 8 wire bytes (Hotline date stamp),
    /// zero-filled when missing or short.
    pub date_create: [u8; 8],
    /// `FILE_DATE_MODIFY` — raw 8 wire bytes.
    pub date_modify: [u8; 8],
    /// `FILE_COMMENT` — CR2LF + strip_ansi'd, capped at `max_comment`.
    pub comment: Vec<u8>,
}

/// Parse the FILE_GETINFO reply. `max_name` / `max_type` /
/// `max_creator` / `max_comment` cap the corresponding strings (the C
/// extractor uses 255 / 31 / 31 / 255). Missing chunks default
/// to zero-length / zero-filled. Sanitisation (`strip_ansi` on name,
/// CR2LF + `strip_ansi` on comment) matches the C extractor's order.
pub fn parse_file_getinfo(
    buf: &[u8],
    len: usize,
    max_name: usize,
    max_type: usize,
    max_creator: usize,
    max_comment: usize,
) -> FileGetInfo {
    let mut out = FileGetInfo::default();
    for chunk in ChunkIter::over_message(buf, len) {
        match chunk.tag {
            tag::FILE_ICON => {
                if chunk.data.len() >= 4 {
                    out.icon.copy_from_slice(&chunk.data[..4]);
                    out.got_icon = true;
                }
            }
            tag::FILE_TYPE => {
                let take = chunk.data.len().min(max_type);
                out.type_ = chunk.data[..take].to_vec();
            }
            tag::FILE_CREATOR => {
                let take = chunk.data.len().min(max_creator);
                out.creator = chunk.data[..take].to_vec();
            }
            tag::FILE_SIZE => out.size = be_uint_up_to_4(chunk.data),
            tag::FILESIZE64 => {
                if let Some(v) = be_u64_first8(chunk.data) {
                    out.size64 = v;
                    out.size64_seen = true;
                }
            }
            tag::FILE_NAME => {
                let take = chunk.data.len().min(max_name);
                out.name = chunk.data[..take].to_vec();
            }
            tag::FILE_DATE_CREATE => {
                if chunk.data.len() >= 8 {
                    out.date_create.copy_from_slice(&chunk.data[..8]);
                }
            }
            tag::FILE_DATE_MODIFY => {
                if chunk.data.len() >= 8 {
                    out.date_modify.copy_from_slice(&chunk.data[..8]);
                }
            }
            tag::FILE_COMMENT => {
                let take = chunk.data.len().min(max_comment);
                out.comment = chunk.data[..take].to_vec();
            }
            _ => {}
        }
    }
    strip_ansi(&mut out.name);
    cr2lf(&mut out.comment);
    strip_ansi(&mut out.comment);
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

    // ---- user_list record ----

    /// Build a USER_LIST record body: uid+icon+color+nlen (BE u16
    /// each) + name + optional trailer.
    fn user_list_body(
        uid: u16,
        icon: u16,
        color: u16,
        nlen_field: u16,
        name: &[u8],
        trailer: Option<u32>,
    ) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&uid.to_be_bytes());
        v.extend_from_slice(&icon.to_be_bytes());
        v.extend_from_slice(&color.to_be_bytes());
        v.extend_from_slice(&nlen_field.to_be_bytes());
        v.extend_from_slice(name);
        if let Some(c) = trailer {
            v.extend_from_slice(&c.to_be_bytes());
        }
        v
    }

    #[test]
    fn user_list_record_basic() {
        let body = user_list_body(0x1234, 0x05, 0x02, 3, b"bob", None);
        let r = parse_user_list_record(&body, 31).expect("ok");
        assert_eq!(r.uid, 0x1234);
        assert_eq!(r.icon, 0x05);
        assert_eq!(r.color, 0x02);
        assert_eq!(r.name, b"bob");
        assert!(r.nick_color.is_none());
    }

    #[test]
    fn user_list_record_with_colored_nicknames_trailer() {
        let body = user_list_body(1, 0, 0, 3, b"bob", Some(0x00ff_8800));
        let r = parse_user_list_record(&body, 31).expect("ok");
        assert_eq!(r.nick_color, Some(0x00ff_8800));
    }

    #[test]
    fn user_list_record_short_chunk_rejected() {
        // Body shorter than the 8 fixed bytes → None (the C
        // extractor's `if (_len < 8) continue;` analogue).
        for len in 0..8 {
            let data = vec![0u8; len];
            assert!(
                parse_user_list_record(&data, 31).is_none(),
                "len={len} should be rejected"
            );
        }
        // Exactly 8 bytes (zero-length name) is the minimum legal
        // record.
        let body = user_list_body(0, 0, 0, 0, &[], None);
        assert_eq!(body.len(), 8);
        assert!(parse_user_list_record(&body, 31).is_some());
    }

    #[test]
    fn user_list_record_clamps_nlen_to_avail() {
        // Server lies: declares nlen=50 but only 5 name bytes
        // follow. Two-stage clamp first against avail, so we get
        // 5 name bytes, no trailer (would need len 8+50+4).
        let body = user_list_body(1, 0, 0, 50, b"short", None);
        let r = parse_user_list_record(&body, 31).expect("ok");
        assert_eq!(r.name, b"short");
        assert!(r.nick_color.is_none());
    }

    #[test]
    fn user_list_record_clamps_nlen_to_max_name() {
        // Server sends nlen=100 with 100 bytes available, but
        // caller's max_name=10. We get 10 bytes.
        let mut name = Vec::new();
        name.extend(std::iter::repeat(b'x').take(100));
        let body = user_list_body(1, 0, 0, 100, &name, None);
        let r = parse_user_list_record(&body, 10).expect("ok");
        assert_eq!(r.name.len(), 10);
    }

    #[test]
    fn user_list_record_strip_ansi_applied_to_name() {
        // 0x1b (ESC, in the strip_ansi band 14..=30 minus
        // {15,22}) folds to 0x5b ('[').
        let name = b"a\x1bb";
        let body = user_list_body(1, 0, 0, name.len() as u16, name, None);
        let r = parse_user_list_record(&body, 31).expect("ok");
        assert_eq!(r.name, b"a[b");
    }

    #[test]
    fn user_list_record_trailer_uses_clamped_nlen() {
        // When max_name truncates nlen, the trailer offset uses
        // the clamped value — matches the C `&uh->name[nlen]`
        // indexing where `nlen` is the post-clamp local. With
        // max_name=5 and an actual 10-byte name, the trailer is
        // read at offset 8+5=13, which IS the 5th-from-end name
        // byte rather than the actual trailer position. This is
        // the C behaviour: callers using a max_name smaller than
        // 31 on a real wire record would mis-read; the protocol
        // caps real names at 31 so this case doesn't fire in
        // practice but the contract is pinned.
        let name = b"0123456789";
        let mut body = user_list_body(1, 0, 0, 10, name, None);
        body.extend_from_slice(&0x00ff_0000u32.to_be_bytes());
        let r = parse_user_list_record(&body, 5).expect("ok");
        // Trailer read from offset 8+5=13, which is bytes
        // [name[5..9]] of name = "56789" interpreted as 0x35363738.
        assert_eq!(r.nick_color, Some(0x3536_3738));
    }

    #[test]
    fn user_list_record_no_trailer_when_too_short() {
        // Body is 8 + nlen + 3 — one byte short of the trailer.
        let mut body = user_list_body(1, 0, 0, 3, b"bob", None);
        body.extend_from_slice(&[0xaa, 0xbb, 0xcc]);
        let r = parse_user_list_record(&body, 31).expect("ok");
        assert!(r.nick_color.is_none());
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

    // ---- news file ----

    #[test]
    fn news_file_first_chunk_wins() {
        let mut body = Vec::new();
        body.extend(chunk(tag::NEWS, b"first"));
        body.extend(chunk(tag::NEWS, b"second"));
        let m = msg(0x0000_0067, 1, 0, &body);
        let n = parse_news_file(&m, m.len(), 256).expect("present");
        assert_eq!(n, b"first");
    }

    #[test]
    fn news_file_sanitises_body() {
        // CR → LF, 0x0e → 'N'.
        let m = msg(0x0000_0067, 1, 0, &chunk(tag::NEWS, b"a\rb\x0e"));
        let n = parse_news_file(&m, m.len(), 256).expect("present");
        assert_eq!(n, b"a\nb\x4e");
    }

    #[test]
    fn news_file_caps_length() {
        let big = vec![b'x'; 5000];
        let m = msg(0x0000_0067, 1, 0, &chunk(tag::NEWS, &big));
        let n = parse_news_file(&m, m.len(), 1024).expect("present");
        assert_eq!(n.len(), 1024);
    }

    #[test]
    fn news_file_missing_chunk_returns_none() {
        let m = msg(0x0000_0067, 1, 0, &[]);
        assert!(parse_news_file(&m, m.len(), 256).is_none());
    }

    // ---- news post walker ----

    #[test]
    fn news_post_chunks_yields_each_news_chunk() {
        let mut body = Vec::new();
        body.extend(chunk(tag::NEWS, b"one"));
        body.extend(chunk(tag::NEWS, b"two"));
        body.extend(chunk(tag::NEWS, b"three"));
        let m = msg(0x0000_0066, 1, 0, &body);
        let v: Vec<Vec<u8>> = news_post_chunks(&m, m.len(), 8192).collect();
        assert_eq!(v.len(), 3);
        assert_eq!(v[0], b"one");
        assert_eq!(v[1], b"two");
        assert_eq!(v[2], b"three");
    }

    #[test]
    fn news_post_chunks_skips_non_news_chunks() {
        // Tag 0x0123 is bogus / unrelated; it must not stop iteration
        // (pre-cleanup the C code's macro hung on it).
        let mut body = Vec::new();
        body.extend(chunk(tag::NEWS, b"first"));
        body.extend(chunk(0x0123, b"junk"));
        body.extend(chunk(tag::NEWS, b"second"));
        let m = msg(0x0000_0066, 1, 0, &body);
        let v: Vec<Vec<u8>> = news_post_chunks(&m, m.len(), 8192).collect();
        assert_eq!(v.len(), 2);
        assert_eq!(v[0], b"first");
        assert_eq!(v[1], b"second");
    }

    #[test]
    fn news_post_chunks_sanitises_each() {
        let m = msg(0x0000_0066, 1, 0, &chunk(tag::NEWS, b"a\rb\x0e"));
        let v: Vec<Vec<u8>> = news_post_chunks(&m, m.len(), 8192).collect();
        assert_eq!(v.len(), 1);
        assert_eq!(v[0], b"a\nb\x4e");
    }

    #[test]
    fn news_post_chunks_empty_when_none_present() {
        let m = msg(0x0000_0066, 1, 0, &[]);
        let v: Vec<Vec<u8>> = news_post_chunks(&m, m.len(), 8192).collect();
        assert!(v.is_empty());
    }

    // ---- news_thread_reply (GETTHREAD post-TASK payload) ----

    #[test]
    fn news_thread_reply_extracts_body_and_thread_id() {
        let mut body = Vec::new();
        body.extend(chunk(tag::NEWSDATA, b"line1\rline2\x1b"));
        body.extend(chunk(tag::THREADID, &0xdead_beefu32.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_news_thread_reply(&m, m.len(), 8192);
        // CR -> LF, then strip_ansi folds 0x1b to '['.
        assert_eq!(r.text.as_deref(), Some(&b"line1\nline2["[..]));
        assert_eq!(r.thread_id, 0xdead_beef);
        assert!(!r.has_task_error);
    }

    #[test]
    fn news_thread_reply_task_error_drops_body() {
        // TASK_ERROR seen after the body — the C extractor's early-
        // out drops what we'd parsed. Mirror that behaviour.
        let mut body = Vec::new();
        body.extend(chunk(tag::NEWSDATA, b"would-be-body"));
        body.extend(chunk(tag::TASK_ERROR, b"server said no"));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_news_thread_reply(&m, m.len(), 8192);
        assert!(r.text.is_none());
        assert!(r.has_task_error);
    }

    #[test]
    fn news_thread_reply_task_error_alone_returns_no_body() {
        let body = chunk(tag::TASK_ERROR, b"oops");
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_news_thread_reply(&m, m.len(), 8192);
        assert!(r.text.is_none());
        assert!(r.has_task_error);
    }

    #[test]
    fn news_thread_reply_caps_text_at_max() {
        // Chunk lengths on the wire are u16 (max 65535), so the
        // test fixture is sized just under that and the cap is
        // tighter; the parser truncates to `max_text` regardless
        // of what the chunk carries.
        let big = vec![b'x'; 32_000];
        let body = chunk(tag::NEWSDATA, &big);
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_news_thread_reply(&m, m.len(), 4_096);
        assert_eq!(r.text.as_deref().map(|s| s.len()), Some(4_096));
    }

    #[test]
    fn news_thread_reply_missing_newsdata_yields_none() {
        // Reply carrying only THREADID — the C caller bails with
        // "reply missing HTLC_DATA_NEWSDATA" debug. text is None.
        let body = chunk(tag::THREADID, &1u32.to_be_bytes());
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_news_thread_reply(&m, m.len(), 8192);
        assert!(r.text.is_none());
        assert_eq!(r.thread_id, 1);
    }

    #[test]
    fn news_thread_reply_empty_body_legal() {
        // Empty NEWSDATA chunk → empty Vec, distinct from missing
        // (the caller can still emit the empty article).
        let body = chunk(tag::NEWSDATA, b"");
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_news_thread_reply(&m, m.len(), 8192);
        assert_eq!(r.text.as_deref(), Some(&b""[..]));
    }

    // ---- news dirlist: folderitem ----

    #[test]
    fn news_folderitem_folder() {
        // ntype = 1, name = "Articles"
        let mut data = vec![1u8];
        data.extend_from_slice(b"Articles");
        let e = parse_news_folderitem(&data, 255).expect("ok");
        assert_eq!(e.kind, NewsDirKind::Folder);
        assert_eq!(e.name, b"Articles");
    }

    #[test]
    fn news_folderitem_category() {
        // ntype = 0 (not 1) → Category
        let mut data = vec![0u8];
        data.extend_from_slice(b"Stuff");
        let e = parse_news_folderitem(&data, 255).expect("ok");
        assert_eq!(e.kind, NewsDirKind::Category);
        assert_eq!(e.name, b"Stuff");
    }

    #[test]
    fn news_folderitem_caps_name() {
        let mut data = vec![1u8];
        data.extend_from_slice(&vec![b'q'; 400]);
        let e = parse_news_folderitem(&data, 255).expect("ok");
        assert_eq!(e.name.len(), 255);
    }

    #[test]
    fn news_folderitem_empty_body_rejected() {
        assert!(parse_news_folderitem(&[], 255).is_none());
    }

    // ---- news dirlist: categoryitem ----

    #[test]
    fn news_categoryitem_bundle_folder() {
        // ntype = 2 (folder), count = 5, namelen = 8, name = "Folder42"
        let mut data = Vec::new();
        data.extend_from_slice(&2u16.to_be_bytes());
        data.extend_from_slice(&5u16.to_be_bytes());
        data.push(8u8);
        data.extend_from_slice(b"Folder42");
        let e = parse_news_categoryitem(&data, 255).expect("ok");
        assert_eq!(e.kind, NewsDirKind::Folder);
        assert_eq!(e.name, b"Folder42");
    }

    #[test]
    fn news_categoryitem_category_with_guid() {
        // ntype = 3 (category), count, 16-byte guid, addsn, deletesn,
        // then namelen + name.
        let mut data = Vec::new();
        data.extend_from_slice(&3u16.to_be_bytes());
        data.extend_from_slice(&1u16.to_be_bytes());
        data.extend_from_slice(&[0u8; 16]);
        data.extend_from_slice(&0u32.to_be_bytes());
        data.extend_from_slice(&0u32.to_be_bytes());
        data.push(4u8);
        data.extend_from_slice(b"News");
        let e = parse_news_categoryitem(&data, 255).expect("ok");
        assert_eq!(e.kind, NewsDirKind::Category);
        assert_eq!(e.name, b"News");
    }

    #[test]
    fn news_categoryitem_unknown_ntype_rejected() {
        let mut data = Vec::new();
        data.extend_from_slice(&7u16.to_be_bytes()); // unknown
        data.extend_from_slice(&[0u8; 100]);
        assert!(parse_news_categoryitem(&data, 255).is_none());
    }

    #[test]
    fn news_categoryitem_truncated_header_rejected() {
        // ntype = 3 but body is too short for the 28-byte header.
        let mut data = Vec::new();
        data.extend_from_slice(&3u16.to_be_bytes());
        data.extend_from_slice(&[0u8; 10]);
        assert!(parse_news_categoryitem(&data, 255).is_none());
    }

    #[test]
    fn news_categoryitem_caps_name() {
        let mut data = Vec::new();
        data.extend_from_slice(&2u16.to_be_bytes());
        data.extend_from_slice(&0u16.to_be_bytes());
        data.push(200u8);
        data.extend_from_slice(&vec![b'x'; 200]);
        let e = parse_news_categoryitem(&data, 16).expect("ok");
        assert_eq!(e.name.len(), 16);
    }

    #[test]
    fn news_categoryitem_namelen_zero() {
        let mut data = Vec::new();
        data.extend_from_slice(&2u16.to_be_bytes());
        data.extend_from_slice(&0u16.to_be_bytes());
        data.push(0u8);
        let e = parse_news_categoryitem(&data, 255).expect("ok");
        assert_eq!(e.kind, NewsDirKind::Folder);
        assert!(e.name.is_empty());
    }

    // ---- catlist ----

    /// Build a CATLIST body — 10-byte threadlist header followed by
    /// the caller's per-post bytes. The body is what goes inside the
    /// `HTLC_DATA_CATLIST` chunk; tests assemble it themselves to
    /// match the wire layout `hx_newscat_parse` reads.
    fn catlist_body(post_count: u32, posts_bytes: &[u8]) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(&0u32.to_be_bytes()); // __x0
        b.extend_from_slice(&post_count.to_be_bytes());
        b.extend_from_slice(&0u16.to_be_bytes()); // __x1
        b.extend_from_slice(posts_bytes);
        b
    }

    /// 22-byte thread header.
    fn thread_hdr(
        postid: u32,
        parentid: u32,
        base_year: u16,
        date_pad: u16,
        seconds: u32,
        partcount: u16,
    ) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(&postid.to_be_bytes());
        b.extend_from_slice(&base_year.to_be_bytes());
        b.extend_from_slice(&date_pad.to_be_bytes());
        b.extend_from_slice(&seconds.to_be_bytes());
        b.extend_from_slice(&parentid.to_be_bytes());
        b.extend_from_slice(&0u32.to_be_bytes()); // __flags
        b.extend_from_slice(&partcount.to_be_bytes());
        b
    }

    fn pstring(s: &[u8]) -> Vec<u8> {
        let mut b = Vec::with_capacity(1 + s.len());
        b.push(s.len() as u8);
        b.extend_from_slice(s);
        b
    }

    fn wrap_catlist(body: &[u8]) -> Vec<u8> {
        msg(0x0001_0000, 1, 0, &chunk(tag::CATLIST, body))
    }

    #[test]
    fn catlist_empty_post_count_yields_empty_vec() {
        let body = catlist_body(0, &[]);
        let m = wrap_catlist(&body);
        let cl = parse_catlist(&m, m.len()).expect("ok");
        assert!(cl.posts.is_empty());
    }

    #[test]
    fn catlist_single_post_no_parts() {
        let mut posts = Vec::new();
        posts.extend(thread_hdr(0x42, 0, 2026, 0, 12345, 0));
        posts.extend(pstring(b"Welcome"));
        posts.extend(pstring(b"Admin"));
        let body = catlist_body(1, &posts);
        let m = wrap_catlist(&body);
        let cl = parse_catlist(&m, m.len()).expect("ok");
        assert_eq!(cl.posts.len(), 1);
        let p = &cl.posts[0];
        assert_eq!(p.postid, 0x42);
        assert_eq!(p.parentid, 0);
        assert_eq!(p.date_base_year, 2026);
        assert_eq!(p.date_seconds, 12345);
        assert_eq!(p.partcount, 0);
        assert_eq!(p.subject, b"Welcome");
        assert_eq!(p.sender, b"Admin");
        assert_eq!(p.size_total, 0);
        assert!(p.parts.is_empty());
    }

    #[test]
    fn catlist_post_with_parts_accumulates_size_total() {
        let mut posts = Vec::new();
        posts.extend(thread_hdr(100, 0, 1970, 0, 0, 2));
        posts.extend(pstring(b"Subject"));
        posts.extend(pstring(b"Sender"));
        posts.extend(pstring(b"text/plain"));
        posts.extend_from_slice(&45u16.to_be_bytes());
        posts.extend(pstring(b"text/html"));
        posts.extend_from_slice(&123u16.to_be_bytes());
        let body = catlist_body(1, &posts);
        let m = wrap_catlist(&body);
        let cl = parse_catlist(&m, m.len()).expect("ok");
        let p = &cl.posts[0];
        assert_eq!(p.partcount, 2);
        assert_eq!(p.size_total, 168);
        assert_eq!(p.parts[0].mime_type, b"text/plain");
        assert_eq!(p.parts[0].size, 45);
        assert_eq!(p.parts[1].mime_type, b"text/html");
        assert_eq!(p.parts[1].size, 123);
    }

    #[test]
    fn catlist_threading_round_trip() {
        let mut posts = Vec::new();
        posts.extend(thread_hdr(1, 0, 0, 0, 0, 0));
        posts.extend(pstring(b"Root"));
        posts.extend(pstring(b"alice"));
        posts.extend(thread_hdr(2, 1, 0, 0, 0, 0));
        posts.extend(pstring(b"Re: Root"));
        posts.extend(pstring(b"bob"));
        posts.extend(thread_hdr(3, 2, 0, 0, 0, 0));
        posts.extend(pstring(b"Re: Re: Root"));
        posts.extend(pstring(b"carol"));
        let body = catlist_body(3, &posts);
        let m = wrap_catlist(&body);
        let cl = parse_catlist(&m, m.len()).expect("ok");
        assert_eq!(cl.posts.len(), 3);
        assert_eq!(cl.posts[1].parentid, 1);
        assert_eq!(cl.posts[2].parentid, 2);
    }

    #[test]
    fn catlist_empty_pstrings_yield_empty_vec() {
        // C's hx_newscat semantics: empty pstring → NULL in the C
        // struct. In Rust we surface it as Vec::new(); the C shim must
        // skip g_strndup when len == 0 to preserve the NULL contract.
        let mut posts = Vec::new();
        posts.extend(thread_hdr(1, 0, 0, 0, 0, 0));
        posts.extend(pstring(b""));
        posts.extend(pstring(b""));
        let body = catlist_body(1, &posts);
        let m = wrap_catlist(&body);
        let cl = parse_catlist(&m, m.len()).expect("ok");
        assert!(cl.posts[0].subject.is_empty());
        assert!(cl.posts[0].sender.is_empty());
    }

    #[test]
    fn catlist_missing_chunk_rejected() {
        let m = msg(0x0001_0000, 1, 0, &[]);
        assert!(parse_catlist(&m, m.len()).is_none());
    }

    #[test]
    fn catlist_short_header_rejected() {
        // 5 bytes < 10-byte threadlist header.
        let m = msg(0x0001_0000, 1, 0, &chunk(tag::CATLIST, &[0u8; 5]));
        assert!(parse_catlist(&m, m.len()).is_none());
    }

    #[test]
    fn catlist_forged_post_count_rejected() {
        // post_count = 1000 but no per-post bytes — defensive cap
        // bites before any allocation.
        let body = catlist_body(1000, &[]);
        let m = wrap_catlist(&body);
        assert!(parse_catlist(&m, m.len()).is_none());
    }

    #[test]
    fn catlist_overlong_pstring_rejected() {
        let mut posts = Vec::new();
        posts.extend(thread_hdr(1, 0, 0, 0, 0, 0));
        // Length byte claims 200 but only 3 bytes follow.
        posts.push(200u8);
        posts.extend_from_slice(b"abc");
        let body = catlist_body(1, &posts);
        let m = wrap_catlist(&body);
        assert!(parse_catlist(&m, m.len()).is_none());
    }

    #[test]
    fn catlist_forged_partcount_rejected() {
        let mut posts = Vec::new();
        posts.extend(thread_hdr(1, 0, 0, 0, 0, 1000));
        posts.extend(pstring(b"S"));
        posts.extend(pstring(b"a"));
        // Claim 1000 parts but provide no per-part bytes.
        let body = catlist_body(1, &posts);
        let m = wrap_catlist(&body);
        assert!(parse_catlist(&m, m.len()).is_none());
    }

    #[test]
    fn catlist_truncated_thread_header_rejected() {
        let mut posts = Vec::new();
        // Only 10 of the 22 bytes of the thread header.
        posts.extend_from_slice(&[0u8; 10]);
        let body = catlist_body(1, &posts);
        let m = wrap_catlist(&body);
        assert!(parse_catlist(&m, m.len()).is_none());
    }

    #[test]
    fn catlist_first_chunk_wins() {
        // Build two CATLIST chunks; only the first should be read.
        let mut first_posts = Vec::new();
        first_posts.extend(thread_hdr(1, 0, 0, 0, 0, 0));
        first_posts.extend(pstring(b"first"));
        first_posts.extend(pstring(b"a"));
        let first_body = catlist_body(1, &first_posts);

        let mut second_posts = Vec::new();
        second_posts.extend(thread_hdr(2, 0, 0, 0, 0, 0));
        second_posts.extend(pstring(b"second"));
        second_posts.extend(pstring(b"b"));
        let second_body = catlist_body(1, &second_posts);

        let mut chunks = Vec::new();
        chunks.extend(chunk(tag::CATLIST, &first_body));
        chunks.extend(chunk(tag::CATLIST, &second_body));
        let m = msg(0x0001_0000, 1, 0, &chunks);
        let cl = parse_catlist(&m, m.len()).expect("ok");
        assert_eq!(cl.posts.len(), 1);
        assert_eq!(cl.posts[0].subject, b"first");
    }

    #[test]
    fn catlist_size_total_wraps_at_u16() {
        // Two parts each with size = 0xff_00; sum wraps to 0xfe00.
        let mut posts = Vec::new();
        posts.extend(thread_hdr(1, 0, 0, 0, 0, 2));
        posts.extend(pstring(b"S"));
        posts.extend(pstring(b"a"));
        posts.extend(pstring(b"m"));
        posts.extend_from_slice(&0xff00u16.to_be_bytes());
        posts.extend(pstring(b"m2"));
        posts.extend_from_slice(&0xff00u16.to_be_bytes());
        let body = catlist_body(1, &posts);
        let m = wrap_catlist(&body);
        let cl = parse_catlist(&m, m.len()).expect("ok");
        // 0xff00 + 0xff00 = 0x1fe00, wrapped to u16 = 0xfe00.
        assert_eq!(cl.posts[0].size_total, 0xfe00);
    }

    // ---- user-info ----

    #[test]
    fn user_info_extracts_name_and_info_with_cr2lf_strip_ansi() {
        let mut body = Vec::new();
        // Embed a 0x1b (ESC) in the name to verify strip_ansi folds
        // it to printable '[' (0x1b -> 0x1b|0x40 = 0x5b).
        body.extend(chunk(tag::NAME, b"alice\x1bX"));
        // Info body uses Mac CR line endings + a control-band byte.
        body.extend(chunk(tag::BODY, b"line1\rline2\x1b"));
        // 0x0001_0000 = HTLS_HDR_TASK — parse_user_info parses the
        // post-TASK reply payload, so the fixture frame should
        // carry the TASK opcode in its header.
        let m = msg(0x0001_0000, 1, 0, &body);
        let info = parse_user_info(&m, m.len(), 31, 4096);
        assert_eq!(info.name, b"alice[X");
        assert_eq!(info.info, b"line1\nline2[");
    }

    #[test]
    fn user_info_caps_at_max_lengths() {
        let long_name = vec![b'a'; 200];
        let long_info = vec![b'i'; 8192];
        let mut body = Vec::new();
        body.extend(chunk(tag::NAME, &long_name));
        body.extend(chunk(tag::BODY, &long_info));
        let m = msg(0x0001_0000, 1, 0, &body);
        let out = parse_user_info(&m, m.len(), 31, 4096);
        assert_eq!(out.name.len(), 31);
        assert_eq!(out.info.len(), 4096);
    }

    #[test]
    fn user_info_missing_chunks_are_empty() {
        // No chunks at all — name + info both empty, caller's
        // `nlen && ilen` gate fails and no event is emitted.
        let m = msg(0x0001_0000, 1, 0, &[]);
        let out = parse_user_info(&m, m.len(), 31, 4096);
        assert!(out.name.is_empty());
        assert!(out.info.is_empty());
    }

    // ---- account-read ----

    /// XOR every byte with 0xff (the Hotline credential-obfuscation
    /// transform); used to build account-read fixture wire bytes.
    fn xor_ff(b: &[u8]) -> Vec<u8> {
        b.iter().map(|&x| x ^ 0xff).collect()
    }

    #[test]
    fn account_read_full() {
        let mut body = Vec::new();
        body.extend(chunk(tag::NAME, b"Bob"));
        body.extend(chunk(tag::LOGIN, &xor_ff(b"bob")));
        body.extend(chunk(tag::PASSWORD, &xor_ff(b"hunter2")));
        body.extend(chunk(tag::ACCESS, &[0x80, 0, 0, 0, 0, 0, 0, 0x01]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let ar = parse_account_read(&m, m.len(), 31, 31, 31);
        assert_eq!(ar.name, b"Bob");
        assert_eq!(ar.login, b"bob");
        assert_eq!(ar.pass, b"hunter2");
        assert_eq!(ar.access, [0x80, 0, 0, 0, 0, 0, 0, 0x01]);
        assert!(ar.got_access);
    }

    #[test]
    fn account_read_no_password_sentinel_yields_empty_pass() {
        // Single zero byte is the explicit "no password" sentinel —
        // C extractor leaves pass empty; we must too.
        let mut body = Vec::new();
        body.extend(chunk(tag::NAME, b"Alice"));
        body.extend(chunk(tag::LOGIN, &xor_ff(b"alice")));
        body.extend(chunk(tag::PASSWORD, &[0x00]));
        body.extend(chunk(tag::ACCESS, &[0; 8]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let ar = parse_account_read(&m, m.len(), 31, 31, 31);
        assert!(ar.pass.is_empty());
        assert!(ar.got_access);
    }

    #[test]
    fn account_read_empty_password_chunk_yields_empty_pass() {
        let mut body = Vec::new();
        body.extend(chunk(tag::NAME, b"x"));
        body.extend(chunk(tag::LOGIN, &xor_ff(b"x")));
        body.extend(chunk(tag::PASSWORD, &[]));
        body.extend(chunk(tag::ACCESS, &[0; 8]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let ar = parse_account_read(&m, m.len(), 31, 31, 31);
        assert!(ar.pass.is_empty());
    }

    #[test]
    fn account_read_first_byte_zero_yields_empty_pass() {
        // Two-byte password with first byte 0x00 also fails the
        // C-side gate (plen > 1 && dh->data[0]).
        let mut body = Vec::new();
        body.extend(chunk(tag::PASSWORD, &[0x00, 0x55]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let ar = parse_account_read(&m, m.len(), 31, 31, 31);
        assert!(ar.pass.is_empty());
    }

    #[test]
    fn account_read_pass_gate_uses_capped_length() {
        // The C extractor caps the password length to sizeof(pass)-1
        // *before* the `plen > 1` check, so a multi-byte wire chunk
        // that gets capped down to 1 byte takes the no-password
        // path. Mirror that here: max_pass=1 should leave pass empty
        // even though the chunk has multiple non-zero bytes.
        let mut body = Vec::new();
        body.extend(chunk(tag::PASSWORD, &xor_ff(b"abc")));
        let m = msg(0x0001_0000, 1, 0, &body);
        let ar = parse_account_read(&m, m.len(), 31, 31, 1);
        assert!(
            ar.pass.is_empty(),
            "max_pass=1 should collapse to no-password sentinel"
        );

        // Sanity check: max_pass=2 should let one byte through
        // (take=2 > 1, first byte non-zero), producing a 2-byte
        // decoded prefix.
        let ar2 = parse_account_read(&m, m.len(), 31, 31, 2);
        assert_eq!(ar2.pass, b"ab");
    }

    #[test]
    fn account_read_last_password_chunk_wins() {
        // Two PASSWORD chunks in one frame: the C extractor's
        // sentinel branch sets pass[0]=0, so a sentinel chunk that
        // arrives AFTER a decoded one clears the buffer (last
        // chunk wins). Mirror that behaviour — without explicit
        // clear-on-sentinel, a stale decoded password would leak
        // through.
        //
        // First chunk: normal password "abc" (xor-encoded on wire).
        // Second chunk: single zero byte → no-password sentinel.
        let mut body = Vec::new();
        body.extend(chunk(tag::PASSWORD, &xor_ff(b"abc")));
        body.extend(chunk(tag::PASSWORD, &[0x00]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let ar = parse_account_read(&m, m.len(), 31, 31, 31);
        assert!(
            ar.pass.is_empty(),
            "trailing sentinel PASSWORD chunk should clear the buffer"
        );

        // Reverse order: sentinel first, then a real password —
        // last-wins should keep the decoded bytes.
        let mut body2 = Vec::new();
        body2.extend(chunk(tag::PASSWORD, &[0x00]));
        body2.extend(chunk(tag::PASSWORD, &xor_ff(b"abc")));
        let m2 = msg(0x0001_0000, 1, 0, &body2);
        let ar2 = parse_account_read(&m2, m2.len(), 31, 31, 31);
        assert_eq!(ar2.pass, b"abc");

        // Same idea with a leading-zero sentinel (plen > 1 but first
        // byte is 0) following a real password.
        let mut body3 = Vec::new();
        body3.extend(chunk(tag::PASSWORD, &xor_ff(b"hunter2")));
        body3.extend(chunk(tag::PASSWORD, &[0x00, 0x55]));
        let m3 = msg(0x0001_0000, 1, 0, &body3);
        let ar3 = parse_account_read(&m3, m3.len(), 31, 31, 31);
        assert!(ar3.pass.is_empty());

        // And once more for the small-cap sentinel: a multi-byte
        // chunk that truncates to take=1 (max_pass=1) hits the same
        // sentinel branch and clears.
        let mut body4 = Vec::new();
        body4.extend(chunk(tag::PASSWORD, &xor_ff(b"first")));
        body4.extend(chunk(tag::PASSWORD, &xor_ff(b"xy")));
        let m4 = msg(0x0001_0000, 1, 0, &body4);
        let ar4 = parse_account_read(&m4, m4.len(), 31, 31, 1);
        assert!(ar4.pass.is_empty());
    }

    #[test]
    fn account_read_caps_login_and_pass_at_max() {
        let long = xor_ff(&vec![b'l'; 200]);
        let long_pw = xor_ff(&vec![b'p'; 200]);
        let mut body = Vec::new();
        body.extend(chunk(tag::LOGIN, &long));
        body.extend(chunk(tag::PASSWORD, &long_pw));
        body.extend(chunk(tag::ACCESS, &[0; 8]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let ar = parse_account_read(&m, m.len(), 31, 31, 31);
        assert_eq!(ar.login.len(), 31);
        assert_eq!(ar.pass.len(), 31);
        // Decoded bytes should be the original 'l' / 'p' values.
        assert!(ar.login.iter().all(|&b| b == b'l'));
        assert!(ar.pass.iter().all(|&b| b == b'p'));
    }

    #[test]
    fn account_read_short_access_chunk_rejected() {
        // C gate: `_len >= sizeof(access)` (8). Anything shorter
        // leaves accessbool=0 → no callback dispatch.
        let m = msg(0x0001_0000, 1, 0, &chunk(tag::ACCESS, &[0, 0, 0, 0]));
        let ar = parse_account_read(&m, m.len(), 31, 31, 31);
        assert!(!ar.got_access);
    }

    #[test]
    fn account_read_missing_access_yields_no_got_access() {
        let m = msg(0x0001_0000, 1, 0, &[]);
        let ar = parse_account_read(&m, m.len(), 31, 31, 31);
        assert!(!ar.got_access);
    }

    // ---- file_get / folder_get reply ----

    #[test]
    fn file_get_reply_extracts_ref_size_queue() {
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &0x1234_5678u32.to_be_bytes()));
        body.extend(chunk(tag::HTXF_SIZE, &1_048_576u32.to_be_bytes()));
        body.extend(chunk(tag::QUEUE, &7u32.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_file_get_reply(&m, m.len());
        assert_eq!(r.ref_, 0x1234_5678);
        assert_eq!(r.size, 1_048_576);
        assert_eq!(r.queue, 7);
        assert!(!r.size64_seen);
    }

    #[test]
    fn file_get_reply_with_xfersize64_companion() {
        let big = 0x0000_0001_0000_0000u64;
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &1u32.to_be_bytes()));
        body.extend(chunk(tag::HTXF_SIZE, &u32::MAX.to_be_bytes()));
        body.extend(chunk(tag::XFERSIZE64, &big.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_file_get_reply(&m, m.len());
        assert!(r.size64_seen);
        assert_eq!(r.size64, big);
    }

    #[test]
    fn file_get_reply_short_xfersize64_ignored() {
        // 7-byte XFERSIZE64 is malformed; size64_seen stays false.
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &1u32.to_be_bytes()));
        body.extend(chunk(tag::XFERSIZE64, &[0u8; 7]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_file_get_reply(&m, m.len());
        assert!(!r.size64_seen);
    }

    #[test]
    fn file_get_reply_missing_chunks_default_zero() {
        let m = msg(0x0001_0000, 1, 0, &[]);
        let r = parse_file_get_reply(&m, m.len());
        assert_eq!(r, FileGetReply::default());
    }

    #[test]
    fn folder_get_reply_extracts_nfiles() {
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &1u32.to_be_bytes()));
        body.extend(chunk(tag::HTXF_SIZE, &1024u32.to_be_bytes()));
        body.extend(chunk(tag::FILE_NFILES, &42u32.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_folder_get_reply(&m, m.len());
        assert_eq!(r.ref_, 1);
        assert_eq!(r.size, 1024);
        assert_eq!(r.nfiles, 42);
    }

    // ---- file_put reply ----

    /// Build a 66-byte RFLT payload with `data_pos` at offset 46
    /// and `rsrc_pos` at offset 62 (the fork-offset positions the
    /// C extractor reads from). Pad to 74 bytes to match real
    /// server emissions; the parser only requires >= 66 bytes.
    fn rflt_payload(data_pos: u32, rsrc_pos: u32) -> [u8; 74] {
        let mut rflt = [0u8; 74];
        rflt[0..4].copy_from_slice(b"RFLT");
        rflt[46..50].copy_from_slice(&data_pos.to_be_bytes());
        rflt[62..66].copy_from_slice(&rsrc_pos.to_be_bytes());
        rflt
    }

    #[test]
    fn file_put_reply_extracts_ref_queue() {
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &0xdead_beefu32.to_be_bytes()));
        body.extend(chunk(tag::QUEUE, &3u32.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_file_put_reply(&m, m.len());
        assert_eq!(r.ref_, 0xdead_beef);
        assert_eq!(r.queue, 3);
        assert_eq!(r.data_pos, 0);
        assert_eq!(r.rsrc_pos, 0);
    }

    #[test]
    fn file_put_reply_with_rflt_resume() {
        let rflt = rflt_payload(0x0000_1234, 0x0000_5678);
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &1u32.to_be_bytes()));
        body.extend(chunk(tag::RFLT, &rflt));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_file_put_reply(&m, m.len());
        assert_eq!(r.data_pos, 0x0000_1234);
        assert_eq!(r.rsrc_pos, 0x0000_5678);
    }

    #[test]
    fn file_put_reply_short_rflt_ignored() {
        // RFLT requires >= 66 bytes (the C `if (_len >= 66)` gate).
        // A 65-byte RFLT leaves both fork offsets at zero.
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &1u32.to_be_bytes()));
        body.extend(chunk(tag::RFLT, &[0u8; 65]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_file_put_reply(&m, m.len());
        assert_eq!(r.data_pos, 0);
        assert_eq!(r.rsrc_pos, 0);
    }

    #[test]
    fn file_put_reply_missing_chunks_default_zero() {
        let m = msg(0x0001_0000, 1, 0, &[]);
        let r = parse_file_put_reply(&m, m.len());
        assert_eq!(r, FilePutReply::default());
    }

    // ---- folder_put reply ----

    #[test]
    fn folder_put_reply_extracts_ref_queue() {
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &0xcafe_babeu32.to_be_bytes()));
        body.extend(chunk(tag::QUEUE, &5u32.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_folder_put_reply(&m, m.len());
        assert_eq!(r.ref_, 0xcafe_babe);
        assert_eq!(r.queue, 5);
    }

    #[test]
    fn folder_put_reply_ignores_unknown_chunks() {
        // Folder-put has no RFLT / SIZE / NFILES — only REF + QUEUE.
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &1u32.to_be_bytes()));
        body.extend(chunk(tag::HTXF_SIZE, &9999u32.to_be_bytes()));
        body.extend(chunk(tag::FILE_NFILES, &7u32.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_folder_put_reply(&m, m.len());
        assert_eq!(r.ref_, 1);
        assert_eq!(r.queue, 0);
    }

    // ---- banner_get reply ----

    #[test]
    fn banner_get_reply_extracts_ref_size() {
        let mut body = Vec::new();
        body.extend(chunk(tag::HTXF_REF, &0xfeed_face_u32.to_be_bytes()));
        body.extend(chunk(tag::HTXF_SIZE, &65_536u32.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let r = parse_banner_get_reply(&m, m.len());
        assert_eq!(r.ref_, 0xfeed_face);
        assert_eq!(r.size, 65_536);
    }

    #[test]
    fn banner_get_reply_missing_chunks_default_zero() {
        let m = msg(0x0001_0000, 1, 0, &[]);
        let r = parse_banner_get_reply(&m, m.len());
        assert_eq!(r, BannerGetReply::default());
    }

    // ---- file_getinfo reply ----

    /// Build an 8-byte Hotline date stamp from a u64 (BE).
    fn date_stamp(v: u64) -> [u8; 8] {
        v.to_be_bytes()
    }

    #[test]
    fn file_getinfo_full() {
        let mut body = Vec::new();
        body.extend(chunk(tag::FILE_ICON, b"ICN1"));
        body.extend(chunk(tag::FILE_TYPE, b"TEXT"));
        body.extend(chunk(tag::FILE_CREATOR, b"MSWD"));
        body.extend(chunk(tag::FILE_SIZE, &12345u32.to_be_bytes()));
        body.extend(chunk(tag::FILE_NAME, b"hello.txt"));
        body.extend(chunk(tag::FILE_DATE_CREATE, &date_stamp(0x0102_0304_0506_0708)));
        body.extend(chunk(tag::FILE_DATE_MODIFY, &date_stamp(0x0807_0605_0403_0201)));
        body.extend(chunk(tag::FILE_COMMENT, b"first line\rsecond line"));
        let m = msg(0x0001_0000, 1, 0, &body);
        let f = parse_file_getinfo(&m, m.len(), 255, 31, 31, 255);
        assert!(f.got_icon);
        assert_eq!(&f.icon, b"ICN1");
        assert_eq!(f.type_, b"TEXT");
        assert_eq!(f.creator, b"MSWD");
        assert_eq!(f.size, 12345);
        assert_eq!(f.name, b"hello.txt");
        assert_eq!(f.date_create, date_stamp(0x0102_0304_0506_0708));
        assert_eq!(f.date_modify, date_stamp(0x0807_0605_0403_0201));
        assert_eq!(f.comment, b"first line\nsecond line");
        assert!(!f.size64_seen);
    }

    #[test]
    fn file_getinfo_size_smaller_widths() {
        // Servers may pack FILE_SIZE in 1, 2, 3, or 4 bytes BE — the
        // parser zero-extends in all cases.
        for (bytes, expect) in &[
            (&[0x12][..], 0x12u32),
            (&[0x12, 0x34][..], 0x1234u32),
            (&[0x12, 0x34, 0x56][..], 0x12_3456u32),
            (&[0x12, 0x34, 0x56, 0x78][..], 0x1234_5678u32),
        ] {
            let m = msg(0x0001_0000, 1, 0, &chunk(tag::FILE_SIZE, bytes));
            let f = parse_file_getinfo(&m, m.len(), 255, 31, 31, 255);
            assert_eq!(f.size, *expect, "FILE_SIZE width {} bytes", bytes.len());
        }
    }

    #[test]
    fn file_getinfo_size64_companion() {
        let big = 0x0000_0002_0000_0000u64;
        let mut body = Vec::new();
        body.extend(chunk(tag::FILE_SIZE, &u32::MAX.to_be_bytes()));
        body.extend(chunk(tag::FILESIZE64, &big.to_be_bytes()));
        let m = msg(0x0001_0000, 1, 0, &body);
        let f = parse_file_getinfo(&m, m.len(), 255, 31, 31, 255);
        assert!(f.size64_seen);
        assert_eq!(f.size64, big);
        // Legacy FILE_SIZE still captured.
        assert_eq!(f.size, u32::MAX);
    }

    #[test]
    fn file_getinfo_name_strip_ansi_and_caps() {
        // Embed a 0x1b in the name → folds to '['; name is capped at
        // max_name.
        let long = vec![b'x'; 500];
        let m = msg(0x0001_0000, 1, 0, &chunk(tag::FILE_NAME, &long));
        let f = parse_file_getinfo(&m, m.len(), 255, 31, 31, 255);
        assert_eq!(f.name.len(), 255);

        let m2 = msg(0x0001_0000, 1, 0, &chunk(tag::FILE_NAME, b"hi\x1bX"));
        let f2 = parse_file_getinfo(&m2, m2.len(), 255, 31, 31, 255);
        assert_eq!(f2.name, b"hi[X");
    }

    #[test]
    fn file_getinfo_comment_cr2lf_and_strip_ansi() {
        // CR → LF, then strip_ansi folds the 0x1b. Multi-line.
        let m = msg(0x0001_0000, 1, 0, &chunk(tag::FILE_COMMENT, b"a\rb\x1b"));
        let f = parse_file_getinfo(&m, m.len(), 255, 31, 31, 255);
        assert_eq!(f.comment, b"a\nb[");
    }

    #[test]
    fn file_getinfo_short_dates_default_zero() {
        // Short FILE_DATE_* chunks are ignored — date stays zero-filled.
        let mut body = Vec::new();
        body.extend(chunk(tag::FILE_DATE_CREATE, &[1, 2, 3]));
        body.extend(chunk(tag::FILE_DATE_MODIFY, &[]));
        let m = msg(0x0001_0000, 1, 0, &body);
        let f = parse_file_getinfo(&m, m.len(), 255, 31, 31, 255);
        assert_eq!(f.date_create, [0u8; 8]);
        assert_eq!(f.date_modify, [0u8; 8]);
    }

    #[test]
    fn file_getinfo_short_icon_skipped() {
        let m = msg(0x0001_0000, 1, 0, &chunk(tag::FILE_ICON, b"ab"));
        let f = parse_file_getinfo(&m, m.len(), 255, 31, 31, 255);
        assert!(!f.got_icon);
        assert_eq!(f.icon, [0u8; 4]);
    }

    #[test]
    fn file_getinfo_empty_body() {
        let m = msg(0x0001_0000, 1, 0, &[]);
        let f = parse_file_getinfo(&m, m.len(), 255, 31, 31, 255);
        assert!(f.name.is_empty());
        assert!(f.type_.is_empty());
        assert!(f.creator.is_empty());
        assert!(f.comment.is_empty());
        assert!(!f.got_icon);
        assert!(!f.size64_seen);
        assert_eq!(f.size, 0);
    }
}
