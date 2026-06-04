//! `#[no_mangle] extern "C"` entry points the C dispatcher calls.
//!
//! Same FFI discipline as the Phase R1 crypto crates: the C side
//! hand-declares these prototypes (in `src/rcv.c` / `src/proto_helpers.c`),
//! so a signature mismatch surfaces as an undefined symbol at link time. No
//! cbindgen — the surface is small and has no opaque pointers to manage.
//!
//! All functions are defensive about NULL / short buffers: the C callers
//! pass `htlc->in.buf` + `htlc->in.pos`, and a truncated frame must fail
//! closed rather than read out of bounds.

use crate::build::{
    self, AccountModifyRequest, AgreementAgreeRequest, BroadcastRequest, ChatRequest,
    ChatSubjectRequest, HxChunk, MsgRequest, NewsDeleteThreadRequest, NewsGetThreadRequest,
    NewsMakeCategoryRequest, NewsPostThreadRequest, UserChangeRequest, UserKickRequest,
};
use crate::parse::{self, AgreementResult, CatList, Header, NewsDirEntry, NewsDirKind};
use std::slice;

/// Borrow a `(ptr, len)` pair as a slice, or an empty slice if `ptr` is
/// NULL. Empty is always safe to parse (every parser handles it).
///
/// # Safety
/// `ptr` must be valid for `len` bytes, or NULL.
unsafe fn as_slice<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if ptr.is_null() || len == 0 {
        &[]
    } else {
        slice::from_raw_parts(ptr, len)
    }
}

/// True (non-zero) if the transaction header's task-error bit is set.
/// Replaces the body of C `task_inerror()`.
///
/// # Safety
/// `buf` must be valid for `len` bytes, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_header_in_error(buf: *const u8, len: usize) -> bool {
    let s = as_slice(buf, len);
    match Header::parse(s) {
        Some(h) => h.in_error(),
        None => false,
    }
}

/// Extract the transaction id from the header into `*out_trans`. Returns
/// true on success, false (leaving `*out_trans` untouched) if the buffer is
/// shorter than a header. Replaces the `HN32(&trans, &h->trans)` in
/// `hx_rcv_task`.
///
/// # Safety
/// `buf` must be valid for `len` bytes (or NULL); `out_trans` must be a
/// valid, writable `u32` pointer.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_header_trans(
    buf: *const u8,
    len: usize,
    out_trans: *mut u32,
) -> bool {
    if out_trans.is_null() {
        return false;
    }
    let s = as_slice(buf, len);
    match Header::parse(s) {
        Some(h) => {
            *out_trans = h.trans;
            true
        }
        None => false,
    }
}

/// C-ABI mirror of [`parse::SelfInfo`]. `cached_name_ptr` borrows into the
/// caller's buffer and is valid only for the duration of the call; the C
/// shim uses it immediately (forensic logging) and does not retain it.
///
/// `access` is the raw 8 wire bytes (big-endian), **not** a host-native
/// integer. The C side `memcpy`s them straight into the `guint64`
/// `htlc->access`, reproducing the original `memcpy(&htlc->access,
/// dh->data, 8)` byte-for-byte — `hl_access.h` consumes the bitmap as
/// big-endian-in-memory, and `test_selfinfo` asserts on the raw bytes.
#[repr(C)]
pub struct SelfInfoC {
    pub access: [u8; 8],
    pub nick_color: u32,
    pub uid: u16,
    pub icon: u16,
    pub cached_name_ptr: *const u8,
    pub cached_name_len: usize,
}

/// Parse `HTLS_HDR_USER_SELFINFO`. Fills `*out` and returns the `seen`
/// bitmask (the `HX_SELFINFO_*` flags). On NULL `out`, returns 0 and does
/// nothing.
///
/// # Safety
/// `buf` must be valid for `len` bytes (or NULL); `out` must be a valid,
/// writable `SelfInfoC` pointer.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_selfinfo(
    buf: *const u8,
    len: usize,
    out: *mut SelfInfoC,
) -> u32 {
    if out.is_null() {
        return 0;
    }
    let s = as_slice(buf, len);
    let si = parse::parse_selfinfo(s, s.len());
    // to_be_bytes(from_be_bytes(wire)) == wire, so this restores the exact
    // 8 wire bytes regardless of host endianness.
    (*out).access = si.access.to_be_bytes();
    (*out).nick_color = si.nick_color;
    (*out).uid = si.uid;
    (*out).icon = si.icon;
    if si.cached_name.is_empty() {
        (*out).cached_name_ptr = std::ptr::null();
        (*out).cached_name_len = 0;
    } else {
        (*out).cached_name_ptr = si.cached_name.as_ptr();
        (*out).cached_name_len = si.cached_name.len();
    }
    si.seen
}

/// Copy `src` into the C buffer `dst` (capacity `cap`), truncating to
/// `cap - 1` and NUL-terminating. Returns the number of bytes written
/// (excluding the NUL). No-op returning 0 on NULL / zero-capacity dst.
///
/// # Safety
/// `dst` must be valid for `cap` bytes, or NULL.
unsafe fn write_cstr(dst: *mut u8, cap: usize, src: &[u8]) -> usize {
    if dst.is_null() || cap == 0 {
        return 0;
    }
    let n = src.len().min(cap - 1);
    if n > 0 {
        std::ptr::copy_nonoverlapping(src.as_ptr(), dst, n);
    }
    *dst.add(n) = 0;
    n
}

/// C-ABI result of [`parse::parse_chat`]. The sanitised line is written
/// into the caller's `buf`; `text_off` (0 or 1) is where the display text
/// starts after the leading-LF strip, and `text_len` is its length.
#[repr(C)]
pub struct ChatOut {
    pub cid: u32,
    pub uid: u16,
    pub text_off: u16,
    pub text_len: u16,
}

/// Parse `HTLS_HDR_CHAT`. Writes the full sanitised line into `buf`
/// (capacity `bufcap`, NUL-terminated) and fills `*out`. The body is
/// capped at `bufcap - 1`. Returns false on NULL `out`, NULL `buf`, or
/// zero `bufcap`; otherwise true (a well-formed frame always parses).
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `buf` valid for `bufcap`
/// bytes; `out` a valid writable `ChatOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_chat(
    msg: *const u8,
    msglen: usize,
    buf: *mut u8,
    bufcap: usize,
    out: *mut ChatOut,
) -> bool {
    if out.is_null() || buf.is_null() || bufcap == 0 {
        return false;
    }
    let s = as_slice(msg, msglen);
    let c = parse::parse_chat(s, s.len(), bufcap - 1);
    let written = write_cstr(buf, bufcap, &c.buf);
    let text_off = c.text_off.min(written);
    (*out).cid = c.cid;
    (*out).uid = c.uid;
    (*out).text_off = text_off as u16;
    (*out).text_len = (written - text_off) as u16;
    true
}

/// C-ABI result of [`parse::parse_chat_subject`].
#[repr(C)]
pub struct ChatSubjectOut {
    pub cid: u32,
    pub subject_len: u16,
}

/// Parse `HTLS_HDR_CHAT_SUBJECT`. Writes the subject into `buf` (NUL-
/// terminated, capped at `bufcap - 1`) and fills `*out`. Returns false on
/// NULL `out`, NULL `buf`, or zero `bufcap`; otherwise true.
///
/// # Safety
/// As [`gtkhx_proto_parse_chat`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_chat_subject(
    msg: *const u8,
    msglen: usize,
    buf: *mut u8,
    bufcap: usize,
    out: *mut ChatSubjectOut,
) -> bool {
    if out.is_null() || buf.is_null() || bufcap == 0 {
        return false;
    }
    let s = as_slice(msg, msglen);
    let sub = parse::parse_chat_subject(s, s.len(), bufcap - 1);
    let written = write_cstr(buf, bufcap, &sub.subject);
    (*out).cid = sub.cid;
    (*out).subject_len = written as u16;
    true
}

/// C-ABI result of [`parse::parse_chat_invite`].
#[repr(C)]
pub struct ChatInviteOut {
    pub cid: u32,
    pub uid: u16,
    pub name_len: u16,
}

/// Parse `HTLS_HDR_CHAT_INVITE`. Writes the inviter name into `buf` (NUL-
/// terminated, capped at `bufcap - 1`) and fills `*out`. Returns false on
/// NULL `out`, NULL `buf`, or zero `bufcap`; otherwise true.
///
/// # Safety
/// As [`gtkhx_proto_parse_chat`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_chat_invite(
    msg: *const u8,
    msglen: usize,
    buf: *mut u8,
    bufcap: usize,
    out: *mut ChatInviteOut,
) -> bool {
    if out.is_null() || buf.is_null() || bufcap == 0 {
        return false;
    }
    let s = as_slice(msg, msglen);
    let inv = parse::parse_chat_invite(s, s.len(), bufcap - 1);
    let written = write_cstr(buf, bufcap, &inv.name);
    (*out).cid = inv.cid;
    (*out).uid = inv.uid;
    (*out).name_len = written as u16;
    true
}

/// Extract the `HTLS_DATA_TASKERROR` chunk's CR2LF + `strip_ansi`
/// sanitised text into `out` (NUL-terminated, capped at `cap - 1`).
/// Returns the number of bytes written (excluding the trailing NUL),
/// or `SIZE_MAX` (i.e. `(usize)-1`) when no TASK_ERROR chunk was
/// present — in which case **`out` is left untouched**, matching the C
/// `task_error_extract` contract that `tests/proto/test_task_error.c`
/// pins ("untouched" sentinel on the no-chunk path). Returns 0 on
/// NULL `out` or zero `cap`.
///
/// The SIZE_MAX sentinel keeps the API from conflating a missing chunk
/// with an empty error string (which is legal wire shape).
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` valid for `cap`
/// bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_task_error(
    msg: *const u8,
    msglen: usize,
    out: *mut u8,
    cap: usize,
) -> usize {
    if out.is_null() || cap == 0 {
        return 0;
    }
    let s = as_slice(msg, msglen);
    match parse::parse_task_error(s, s.len(), cap - 1) {
        Some(bytes) => write_cstr(out, cap, &bytes),
        None => usize::MAX,
    }
}

/// C-ABI result of [`parse::parse_msg`]. The name lands in `name_buf`,
/// the sanitised body in `msg_buf`; lengths report bytes written
/// (excluding the trailing NUL).
#[repr(C)]
pub struct MsgOut {
    pub uid: u16,
    pub name_len: u16,
    pub msg_len: u16,
}

/// Parse `HTLS_HDR_MSG` (also `MSG_BROADCAST` / `POLITEQUIT`, which
/// share the same shape). Writes the `strip_ansi`'d name into
/// `name_buf` (cap `name_cap`) and the CR2LF + `strip_ansi`'d body into
/// `msg_buf` (cap `msg_cap`). Returns false on any NULL / zero-cap
/// pointer; otherwise true.
///
/// # Safety
/// As [`gtkhx_proto_parse_chat`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_msg(
    msg: *const u8,
    msglen: usize,
    name_buf: *mut u8,
    name_cap: usize,
    msg_buf: *mut u8,
    msg_cap: usize,
    out: *mut MsgOut,
) -> bool {
    if out.is_null()
        || name_buf.is_null()
        || name_cap == 0
        || msg_buf.is_null()
        || msg_cap == 0
    {
        return false;
    }
    let s = as_slice(msg, msglen);
    let p = parse::parse_msg(s, s.len(), name_cap - 1, msg_cap - 1);
    let nlen = write_cstr(name_buf, name_cap, &p.name);
    let mlen = write_cstr(msg_buf, msg_cap, &p.msg);
    (*out).uid = p.uid;
    (*out).name_len = nlen as u16;
    (*out).msg_len = mlen as u16;
    true
}

/// C-ABI result of [`parse::parse_banner`]. `type_code` is the 4-byte
/// banner code (zeroed when `got_type` is 0); `has_url` / `url_len` tell
/// the caller whether to read `url_buf`.
#[repr(C)]
pub struct BannerOut {
    pub type_code: [u8; 4],
    pub url_len: u16,
    pub got_type: u8,
    pub has_url: u8,
}

/// Parse `HTLS_HDR_BANNER`. The banner type is gated at exactly 4 bytes.
/// The URL (if present) is written into `url_buf` (NUL-terminated, capped
/// at `url_cap - 1`). Returns `got_type` (true iff the type chunk was
/// present and well-formed) — matching the C extractor's contract.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `url_buf` valid for
/// `url_cap` bytes (or NULL); `out` a valid writable `BannerOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_banner(
    msg: *const u8,
    msglen: usize,
    url_buf: *mut u8,
    url_cap: usize,
    out: *mut BannerOut,
) -> bool {
    if out.is_null() || url_buf.is_null() || url_cap == 0 {
        return false;
    }
    let s = as_slice(msg, msglen);
    let b = parse::parse_banner(s, s.len(), url_cap - 1);
    (*out).type_code = b.type_code;
    (*out).got_type = b.got_type as u8;
    match b.url {
        Some(url) => {
            let n = write_cstr(url_buf, url_cap, &url);
            (*out).has_url = 1;
            (*out).url_len = n as u16;
        }
        None => {
            *url_buf = 0;
            (*out).has_url = 0;
            (*out).url_len = 0;
        }
    }
    b.got_type
}

/// C-ABI result of [`parse::parse_xfer_queue`].
#[repr(C)]
pub struct XferQueueOut {
    pub htxf_ref: u32,
    pub queueid: u32,
}

/// Parse `HTLS_HDR_QUEUE`. Both fields default to 0 when missing;
/// `queueid == 0` means "ready, you can start the transfer". Returns
/// false on NULL `out`; otherwise true.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` a valid writable
/// `XferQueueOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_xfer_queue(
    msg: *const u8,
    msglen: usize,
    out: *mut XferQueueOut,
) -> bool {
    if out.is_null() {
        return false;
    }
    let s = as_slice(msg, msglen);
    let q = parse::parse_xfer_queue(s, s.len());
    (*out).htxf_ref = q.htxf_ref;
    (*out).queueid = q.queueid;
    true
}

/// Result codes for [`gtkhx_proto_parse_agreement`], matching the C
/// `hx_agreement_result` enum:
/// `0` = OK (body in `out`), `1` = NONE sentinel, `2` = MISSING.
pub const HX_AGREEMENT_OK_FFI: u32 = 0;
pub const HX_AGREEMENT_NONE_FFI: u32 = 1;
pub const HX_AGREEMENT_MISSING_FFI: u32 = 2;

/// Parse `HTLS_HDR_AGREEMENT`. On OK, writes the CR2LF + `strip_ansi`
/// sanitised body into `out` (NUL-terminated, capped at `cap - 1`) and
/// stores the byte count (excluding the NUL) into `*out_len`. On NONE
/// and MISSING the output buffer is **not** touched at all — matching
/// the C `hx_agreement_extract` contract, which the existing
/// `test_agreement.c` cases rely on (the "untouched" sentinel string
/// stays put for NONE/NOT_FOUND). Passing NULL `out` (or zero `cap`)
/// is permitted: the result code is still returned correctly, no write
/// happens, and `*out_len` is left untouched.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` valid for `cap`
/// bytes (or NULL); `out_len` valid for one `usize` write, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_agreement(
    msg: *const u8,
    msglen: usize,
    out: *mut u8,
    cap: usize,
    out_len: *mut usize,
) -> u32 {
    let s = as_slice(msg, msglen);
    // Cap the sanitised body at cap - 1 only when we have somewhere to
    // write it; with no output buffer the body still has to be cheaply
    // computed (we discard it) so the result code matches OK / NONE.
    let body_cap = if !out.is_null() && cap > 0 {
        cap - 1
    } else {
        usize::MAX
    };
    let (r, body) = parse::parse_agreement(s, s.len(), body_cap);
    match r {
        AgreementResult::Ok => {
            if !out.is_null() && cap > 0 {
                let n = write_cstr(out, cap, &body);
                if !out_len.is_null() {
                    *out_len = n;
                }
            }
            HX_AGREEMENT_OK_FFI
        }
        AgreementResult::None => HX_AGREEMENT_NONE_FFI,
        AgreementResult::Missing => HX_AGREEMENT_MISSING_FFI,
    }
}

/// Extract the first `HTLS_DATA_NEWS` chunk's CR2LF + `strip_ansi`
/// sanitised text into `out` (NUL-terminated, capped at `cap - 1`).
/// Returns the byte count (excluding the NUL), or `SIZE_MAX` when no
/// NEWS chunk was present — in which case **`out` is left untouched**,
/// matching `hx_news_file_extract`'s C contract. Returns 0 on NULL
/// `out` or zero `cap`.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` valid for `cap`
/// bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_news_file(
    msg: *const u8,
    msglen: usize,
    out: *mut u8,
    cap: usize,
) -> usize {
    if out.is_null() || cap == 0 {
        return 0;
    }
    let s = as_slice(msg, msglen);
    match parse::parse_news_file(s, s.len(), cap - 1) {
        Some(bytes) => write_cstr(out, cap, &bytes),
        None => usize::MAX,
    }
}

/// Walk every `HTLS_DATA_NEWS` chunk in the message, invoking `cb` once
/// per chunk with the CR2LF + `strip_ansi` sanitised body. The body
/// passed to `cb` is heap-allocated for the call's lifetime, NUL-
/// terminated, and freed by Rust after `cb` returns — `cb` must not
/// retain the pointer beyond its frame. Returns the number of chunks
/// emitted. NULL `cb` is allowed (it just counts).
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL). `cb` is `extern "C"` and
/// must not unwind. `user` is opaque.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_walk_news_post(
    msg: *const u8,
    msglen: usize,
    cb: Option<unsafe extern "C" fn(user: *mut std::ffi::c_void, bytes: *const u8, len: usize)>,
    user: *mut std::ffi::c_void,
) -> i32 {
    let s = as_slice(msg, msglen);
    let mut count = 0i32;
    for body in parse::news_post_chunks(s, s.len(), u16::MAX as usize) {
        count += 1;
        if let Some(callback) = cb {
            // Hand the callback a NUL-terminated buffer so it can use
            // C-string utilities. Length is the byte count *without*
            // the NUL — matches the C handler's old contract.
            let mut buf = body;
            let len = buf.len();
            buf.push(0);
            callback(user, buf.as_ptr(), len);
            // buf is dropped here.
        }
    }
    count
}

/// C-ABI mirror of [`parse::NewsDirEntry`].
#[repr(C)]
pub struct NewsDirEntryOut {
    /// 1 = folder-entry, 2 = category-entry. Matches the `kind` field
    /// of `struct hx_news_dirlist_entry`.
    pub kind: i32,
    pub name_len: u16,
}

fn news_dir_kind_to_c(k: NewsDirKind) -> i32 {
    match k {
        NewsDirKind::Folder => 1,
        NewsDirKind::Category => 2,
    }
}

fn write_news_dir(
    e: NewsDirEntry,
    name_buf: *mut u8,
    name_cap: usize,
    out: *mut NewsDirEntryOut,
) -> bool {
    unsafe {
        let written = write_cstr(name_buf, name_cap, &e.name);
        (*out).kind = news_dir_kind_to_c(e.kind);
        (*out).name_len = written as u16;
    }
    true
}

/// Parse a `HTLC_DATA_NEWSFOLDERITEM` chunk body. Writes the entry's
/// name into `name_buf` (NUL-terminated, capped at `name_cap - 1`) and
/// fills `*out`. Returns false on NULL / zero-cap arguments or on a
/// rejected body (empty chunk).
///
/// # Safety
/// `data` valid for `dlen` bytes (or NULL); `name_buf` valid for
/// `name_cap` bytes; `out` a valid writable `NewsDirEntryOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_news_folderitem(
    data: *const u8,
    dlen: usize,
    name_buf: *mut u8,
    name_cap: usize,
    out: *mut NewsDirEntryOut,
) -> bool {
    if out.is_null() || name_buf.is_null() || name_cap == 0 {
        return false;
    }
    let s = as_slice(data, dlen);
    match parse::parse_news_folderitem(s, name_cap - 1) {
        Some(e) => write_news_dir(e, name_buf, name_cap, out),
        None => false,
    }
}

/// Parse a `HTLC_DATA_CATEGORYITEM` chunk body. Writes the entry's
/// name into `name_buf` (NUL-terminated, capped at `name_cap - 1`) and
/// fills `*out`. Returns false on NULL / zero-cap arguments, on
/// unknown `ntype`, or on a truncated header (the C extractor's
/// defensive-reject contract).
///
/// # Safety
/// As [`gtkhx_proto_parse_news_folderitem`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_news_categoryitem(
    data: *const u8,
    dlen: usize,
    name_buf: *mut u8,
    name_cap: usize,
    out: *mut NewsDirEntryOut,
) -> bool {
    if out.is_null() || name_buf.is_null() || name_cap == 0 {
        return false;
    }
    let s = as_slice(data, dlen);
    match parse::parse_news_categoryitem(s, name_cap - 1) {
        Some(e) => write_news_dir(e, name_buf, name_cap, out),
        None => false,
    }
}

// ---- HTLC_DATA_CATLIST (1.5 threaded news article listing) ------------
//
// The C consumer (rcv.c::news_item_take_from_wire) steals the parsed
// pstring pointers (subject / sender / mime_type) and frees them via
// g_free later. To preserve that allocator boundary without g_malloc-
// from-Rust, the FFI exposes an opaque handle plus view-struct
// accessors: the C shim copies each pstring into a g_strndup'd buffer
// itself, so g_malloc/g_free pair correctly on the C side and Rust
// owns its parse tree end-to-end.

/// View of one post inside a [`CatList`]. The byte pointers borrow
/// into the Rust-owned parse tree and are valid only while the
/// owning handle is alive — copy out before [`gtkhx_proto_catlist_free`].
/// Empty pstrings on the wire surface as `*_ptr == NULL` and
/// `*_len == 0` to match the C `hx_newscat`'s "NULL when empty"
/// convention.
#[repr(C)]
pub struct CatListPostView {
    pub postid: u32,
    pub parentid: u32,
    pub date_seconds: u32,
    pub date_base_year: u16,
    pub date_pad: u16,
    pub partcount: u16,
    pub size_total: u16,
    pub subject_ptr: *const u8,
    pub subject_len: usize,
    pub sender_ptr: *const u8,
    pub sender_len: usize,
}

/// View of one mime part inside a [`CatListPostView`].
#[repr(C)]
pub struct CatListPartView {
    pub mime_type_ptr: *const u8,
    pub mime_type_len: usize,
    pub size: u16,
}

fn empty_view_ptr(bytes: &[u8]) -> (*const u8, usize) {
    if bytes.is_empty() {
        (std::ptr::null(), 0)
    } else {
        (bytes.as_ptr(), bytes.len())
    }
}

/// Parse the first `HTLC_DATA_CATLIST` chunk and return an opaque
/// handle to the parsed tree, or NULL when the chunk is absent or
/// malformed (the C `hx_newscat_parse`'s FALSE return). The caller
/// owns the handle and must free it with [`gtkhx_proto_catlist_free`].
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_catlist(
    msg: *const u8,
    msglen: usize,
) -> *mut CatList {
    let s = as_slice(msg, msglen);
    match parse::parse_catlist(s, s.len()) {
        Some(cl) => Box::into_raw(Box::new(cl)),
        None => std::ptr::null_mut(),
    }
}

/// Free a catlist handle returned by [`gtkhx_proto_parse_catlist`].
/// NULL is a no-op.
///
/// # Safety
/// `cl` must be either NULL or a pointer previously returned by
/// `gtkhx_proto_parse_catlist`. Each handle must be freed exactly once.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_catlist_free(cl: *mut CatList) {
    if !cl.is_null() {
        drop(Box::from_raw(cl));
    }
}

/// Number of posts in `*cl`. 0 on NULL.
///
/// # Safety
/// `cl` must be NULL or a live handle from
/// [`gtkhx_proto_parse_catlist`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_catlist_post_count(cl: *const CatList) -> u32 {
    if cl.is_null() {
        return 0;
    }
    (*cl).posts.len() as u32
}

/// Fill `*view` with a borrowed snapshot of post `idx`. Returns false
/// on NULL `cl` / NULL `view` / out-of-range `idx`; otherwise true.
/// The pointers in `*view` are valid only until the next call mutating
/// the handle (currently no such API exists — they are valid until the
/// handle is freed).
///
/// # Safety
/// `cl` and `view` must be valid pointers, or NULL. `cl` must point at
/// a live handle from [`gtkhx_proto_parse_catlist`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_catlist_post_get(
    cl: *const CatList,
    idx: u32,
    view: *mut CatListPostView,
) -> bool {
    if cl.is_null() || view.is_null() {
        return false;
    }
    let posts = &(*cl).posts;
    let i = idx as usize;
    if i >= posts.len() {
        return false;
    }
    let p = &posts[i];
    let (sptr, slen) = empty_view_ptr(&p.subject);
    let (nptr, nlen) = empty_view_ptr(&p.sender);
    *view = CatListPostView {
        postid: p.postid,
        parentid: p.parentid,
        date_seconds: p.date_seconds,
        date_base_year: p.date_base_year,
        date_pad: p.date_pad,
        partcount: p.partcount,
        size_total: p.size_total,
        subject_ptr: sptr,
        subject_len: slen,
        sender_ptr: nptr,
        sender_len: nlen,
    };
    true
}

/// Fill `*view` with a borrowed snapshot of part `(post_idx, part_idx)`.
/// Same NULL / OOB rules as [`gtkhx_proto_catlist_post_get`].
///
/// # Safety
/// As [`gtkhx_proto_catlist_post_get`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_catlist_part_get(
    cl: *const CatList,
    post_idx: u32,
    part_idx: u16,
    view: *mut CatListPartView,
) -> bool {
    if cl.is_null() || view.is_null() {
        return false;
    }
    let posts = &(*cl).posts;
    let pi = post_idx as usize;
    if pi >= posts.len() {
        return false;
    }
    let parts = &posts[pi].parts;
    let qi = part_idx as usize;
    if qi >= parts.len() {
        return false;
    }
    let q = &parts[qi];
    let (mptr, mlen) = empty_view_ptr(&q.mime_type);
    *view = CatListPartView {
        mime_type_ptr: mptr,
        mime_type_len: mlen,
        size: q.size,
    };
    true
}

// ---- SEND-path builders (HTLC_HDR_CHAT / _MSG / _MSG_BROADCAST) -------
//
// Each builder fills a caller-provided `struct hx_chunk[]` (and, where
// integer fields are present, a `uint8_t scratch[]` buffer) and returns
// the chunk count. Matches `hx_agreement_agree_build_chunks` in
// agreement_packet.c so the production code can keep using
// `hlwrite_chunks` for the actual wire push (the cipher/compression/fd
// dispatch stays in C until Phase R3). Returns 0 on validation failure
// (NULL pointer, too-small chunks or scratch slice). Body lifetime is
// the caller's: the chunk data pointers reference into `body_ptr`, so
// the buffer must outlive the hlwrite_chunks call.

/// Build `HTLC_HDR_CHAT` chunks. Three chunks when `cid != 0` (STYLE +
/// CHAT body + CHAT_ID), two when `cid == 0` (STYLE + CHAT body — the
/// public-chat case).
///
/// Requires `chunks_cap >= 3` and `scratch_cap >= 6`. The scratch
/// buffer holds the BE-encoded style (offset 0, 2 bytes) and cid
/// (offset 2, 4 bytes); the chunk array's `data` pointers reference
/// into it.
///
/// # Safety
/// `chunks` valid for `chunks_cap` `HxChunk` slots (or NULL); `scratch`
/// valid for `scratch_cap` bytes (or NULL); `body_ptr` valid for
/// `body_len` bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_chat_chunks(
    cid: u32,
    style: u16,
    body_ptr: *const u8,
    body_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima for this builder: 3 chunks (STYLE + BODY + CHAT_ID),
    // 6 scratch bytes (u16 style at +0, u32 cid at +2). The slices we
    // hand the builder are sized to these maxima, NOT the caller-
    // provided caps — otherwise a too-large cap would let
    // from_raw_parts_mut produce an out-of-bounds slice (UB) before
    // the builder's length check runs.
    const MAX_CHUNKS: usize = 3;
    const MAX_SCRATCH: usize = 6;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    // NULL body_ptr with body_len > 0 is a caller bug: as_slice
    // silently turns it into an empty slice, which would put an
    // empty CHAT chunk on the wire instead of the intended message.
    // Fail fast so the inconsistency surfaces in the caller instead.
    if body_ptr.is_null() && body_len != 0 {
        return 0;
    }
    // body_len > u16::MAX would overflow the chunk len field; reject
    // here so the Rust builder's internal check is unreachable from
    // the C side.
    if body_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let body = as_slice(body_ptr, body_len);
    let req = ChatRequest { cid, style, body };
    build::build_chat_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_MSG` chunks: UID + MSG body, exactly 2 chunks.
/// Requires `chunks_cap >= 2` and `scratch_cap >= 2` (the BE-encoded
/// uid at offset 0).
///
/// # Safety
/// As [`gtkhx_proto_build_chat_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_msg_chunks(
    uid: u16,
    body_ptr: *const u8,
    body_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima for this builder: 2 chunks (UID + BODY), 2 scratch
    // bytes (the u16 uid). Slices are sized to the maxima; see
    // gtkhx_proto_build_chat_chunks for the UB rationale.
    const MAX_CHUNKS: usize = 2;
    const MAX_SCRATCH: usize = 2;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    // See gtkhx_proto_build_chat_chunks for the NULL-ptr-with-nonzero-
    // len rationale: as_slice would silently treat this as an empty
    // body and turn the intended MSG into an empty-body chunk.
    if body_ptr.is_null() && body_len != 0 {
        return 0;
    }
    if body_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let body = as_slice(body_ptr, body_len);
    let req = MsgRequest { uid, body };
    build::build_msg_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_MSG_BROADCAST` chunks: a single MSG-body chunk.
/// Requires `chunks_cap >= 1`. No scratch needed.
///
/// # Safety
/// `chunks` valid for `chunks_cap` `HxChunk` slots (or NULL);
/// `body_ptr` valid for `body_len` bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_broadcast_chunks(
    body_ptr: *const u8,
    body_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    // Fixed maximum: 1 chunk (BODY). Slice is sized to the maximum;
    // see gtkhx_proto_build_chat_chunks for the UB rationale.
    const MAX_CHUNKS: usize = 1;

    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    // See gtkhx_proto_build_chat_chunks for the NULL-ptr-with-nonzero-
    // len rationale: as_slice would silently turn an intended
    // broadcast into an empty-body broadcast.
    if body_ptr.is_null() && body_len != 0 {
        return 0;
    }
    if body_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let body = as_slice(body_ptr, body_len);
    let req = BroadcastRequest { body };
    build::build_broadcast_chunks(&req, chunks_slice) as i32
}

// ---- Chat-admin SEND builders ----------------------------------------
//
// Same shape as the chat/msg/broadcast shims above: fill the caller's
// chunks[] + scratch[] and return the chunk count (0 on failure). The
// scratch buffer holds BE-encoded integer fields; data pointers in the
// chunk array reference into scratch (and into the subject body for
// CHAT_SUBJECT).

/// Build `HTLC_HDR_CHAT_CREATE` chunks (just UID). `chunks_cap >= 1`,
/// `scratch_cap >= 2`.
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_chat_create_chunks(
    uid: u16,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima: 1 chunk (UID), 2 scratch bytes (u16 uid). Slices
    // are sized to the maxima, NOT chunks_cap / scratch_cap — a
    // too-large caller cap would let from_raw_parts_mut produce an
    // out-of-bounds slice (UB) before the builder's length check runs.
    const MAX_CHUNKS: usize = 1;
    const MAX_SCRATCH: usize = 2;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    build::build_chat_create_chunks(uid, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_CHAT_INVITE` chunks (CHAT_ID + UID). `chunks_cap >= 2`,
/// `scratch_cap >= 6`.
///
/// # Safety
/// As [`gtkhx_proto_build_chat_create_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_chat_invite_chunks(
    cid: u32,
    uid: u16,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima: 2 chunks (CHAT_ID + UID), 6 scratch bytes (cid at
    // +0, uid at +4). See gtkhx_proto_build_chat_create_chunks for the
    // UB rationale.
    const MAX_CHUNKS: usize = 2;
    const MAX_SCRATCH: usize = 6;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    build::build_chat_invite_chunks(cid, uid, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_CHAT_JOIN` chunks (single CHAT_ID). `chunks_cap >= 1`,
/// `scratch_cap >= 4`.
///
/// # Safety
/// As [`gtkhx_proto_build_chat_create_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_chat_join_chunks(
    cid: u32,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima: 1 chunk (CHAT_ID), 4 scratch bytes (the u32 cid).
    // See gtkhx_proto_build_chat_create_chunks for the UB rationale.
    const MAX_CHUNKS: usize = 1;
    const MAX_SCRATCH: usize = 4;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    build::build_chat_join_chunks(cid, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_CHAT_PART` chunks (single CHAT_ID).
///
/// # Safety
/// As [`gtkhx_proto_build_chat_create_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_chat_part_chunks(
    cid: u32,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima: 1 chunk (CHAT_ID), 4 scratch bytes (the u32 cid).
    // See gtkhx_proto_build_chat_create_chunks for the UB rationale.
    const MAX_CHUNKS: usize = 1;
    const MAX_SCRATCH: usize = 4;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    build::build_chat_part_chunks(cid, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_CHAT_DECLINE` chunks (single CHAT_ID).
///
/// # Safety
/// As [`gtkhx_proto_build_chat_create_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_chat_decline_chunks(
    cid: u32,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima: 1 chunk (CHAT_ID), 4 scratch bytes (the u32 cid).
    // See gtkhx_proto_build_chat_create_chunks for the UB rationale.
    const MAX_CHUNKS: usize = 1;
    const MAX_SCRATCH: usize = 4;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    build::build_chat_decline_chunks(cid, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_CHAT_SUBJECT` chunks (CHAT_ID + subject body).
/// `chunks_cap >= 2`, `scratch_cap >= 4`. The subject buffer is the
/// caller's; its bytes are referenced (not copied) by the second
/// chunk, so it must outlive the eventual `hlwrite_chunks` call.
///
/// # Safety
/// As [`gtkhx_proto_build_chat_create_chunks`]; `subject_ptr` valid
/// for `subject_len` bytes, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_chat_subject_chunks(
    cid: u32,
    subject_ptr: *const u8,
    subject_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima: 2 chunks (CHAT_ID + subject body), 4 scratch
    // bytes (the u32 cid). See gtkhx_proto_build_chat_create_chunks
    // for the UB rationale.
    const MAX_CHUNKS: usize = 2;
    const MAX_SCRATCH: usize = 4;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    // See gtkhx_proto_build_chat_chunks for the rationale: as_slice
    // turns NULL+nonzero-len into an empty slice, which would
    // silently turn an intended subject into a zero-length chunk.
    if subject_ptr.is_null() && subject_len != 0 {
        return 0;
    }
    if subject_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let subject = as_slice(subject_ptr, subject_len);
    let req = ChatSubjectRequest { cid, subject };
    build::build_chat_subject_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_AGREEMENTAGREE` chunks (ICON + NAME + OPTIONS, all
/// three mandatory — Mobius panics without OPTIONS). `chunks_cap >= 3`,
/// `scratch_cap >= 4`.
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths, or NULL.
/// `name_ptr` valid for `name_len` bytes, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_agreement_agree_chunks(
    icon: u16,
    name_ptr: *const u8,
    name_len: usize,
    options: u16,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    // Fixed maxima: 3 chunks (ICON + NAME + OPTIONS — all three
    // mandatory; Mobius panics without OPTIONS), 4 scratch bytes
    // (icon at +0, options at +2). See gtkhx_proto_build_chat_create
    // _chunks for the UB rationale.
    const MAX_CHUNKS: usize = 3;
    const MAX_SCRATCH: usize = 4;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    // See gtkhx_proto_build_chat_chunks for the rationale: as_slice
    // would silently turn NULL+nonzero-len into an empty NAME chunk,
    // putting an empty-nick AGREEMENTAGREE on the wire instead of
    // surfacing the caller's bug.
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let display_name = as_slice(name_ptr, name_len);
    let req = AgreementAgreeRequest { icon, display_name, options };
    build::build_agreement_agree_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// C-ABI result of [`parse::parse_user_part`].
#[repr(C)]
pub struct UserPartOut {
    pub cid: u32,
    pub uid: u16,
}

/// Parse `HTLS_HDR_USER_PART`. Fills `*out`. Returns false on NULL `out`;
/// otherwise true (a well-formed frame always parses, missing chunks
/// default to zero).
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` a valid writable
/// `UserPartOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_user_part(
    msg: *const u8,
    msglen: usize,
    out: *mut UserPartOut,
) -> bool {
    if out.is_null() {
        return false;
    }
    let s = as_slice(msg, msglen);
    let p = parse::parse_user_part(s, s.len());
    (*out).cid = p.cid;
    (*out).uid = p.uid;
    true
}

/// C-ABI result of [`parse::parse_user_change`]. The sanitised name is
/// written into the caller's `buf` (NUL-terminated, capped at
/// `bufcap - 1`). `got_color` / `got_nick_color` are 0/1; `nick_color`
/// defaults to `HX_NICK_COLOR_NONE` (0xffff_ffff) when no COLOR chunk
/// was present.
#[repr(C)]
pub struct UserChangeOut {
    pub cid: u32,
    pub nick_color: u32,
    pub uid: u16,
    pub icon: u16,
    pub color: u16,
    pub got_color: u8,
    pub got_nick_color: u8,
    pub name_len: u16,
}

/// Parse `HTLS_HDR_USER_CHANGE`. Writes the strip_ansi'd nickname into
/// `buf` and fills `*out`. Returns false on NULL `out`, NULL `buf`, or
/// zero `bufcap`; otherwise true.
///
/// # Safety
/// As [`gtkhx_proto_parse_chat`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_user_change(
    msg: *const u8,
    msglen: usize,
    buf: *mut u8,
    bufcap: usize,
    out: *mut UserChangeOut,
) -> bool {
    if out.is_null() || buf.is_null() || bufcap == 0 {
        return false;
    }
    let s = as_slice(msg, msglen);
    let uc = parse::parse_user_change(s, s.len(), bufcap - 1);
    let written = write_cstr(buf, bufcap, &uc.name);
    (*out).cid = uc.cid;
    (*out).nick_color = uc.nick_color;
    (*out).uid = uc.uid;
    (*out).icon = uc.icon;
    (*out).color = uc.color;
    (*out).got_color = uc.got_color as u8;
    (*out).got_nick_color = uc.got_nick_color as u8;
    (*out).name_len = written as u16;
    true
}

/// C-ABI result of [`parse::parse_user_list_record`]. `nick_color`
/// is always valid: the Colored-Nicknames trailer when present,
/// otherwise `HX_NICK_COLOR_NONE` (0xffffffff). `got_nick_color`
/// is a 0/1 trailer-presence flag — callers that need to
/// distinguish "server set us to 0xffffffff explicitly" from
/// "server didn't emit the trailer" can read it; callers that
/// just want a colour to render don't need to substitute anything.
#[repr(C)]
pub struct UserListRecordOut {
    pub nick_color: u32,
    pub uid: u16,
    pub icon: u16,
    pub color: u16,
    pub got_nick_color: u8,
    pub name_len: u16,
}

/// Parse one `HTLS_DATA_USER_LIST` chunk's payload (the C
/// `hl_userlist_hdr` body — uid/icon/color/nlen + name +
/// optional Colored-Nicknames trailer). Writes the strip_ansi'd
/// name into `name_buf` (NUL-terminated, capped at `name_cap - 1`)
/// and fills `*out`. Returns false on NULL `out`, NULL `name_buf`,
/// zero `name_cap`, or a chunk shorter than the 8 fixed bytes; the
/// last case mirrors the C extractor silently skipping malformed
/// USER_LIST records.
///
/// Caller iterates `HTLS_DATA_USER_LIST` chunks (e.g. via the
/// `dh_start`/`dh_end` macros in the rcv path) and invokes this for
/// each chunk's `(data, len)` pair.
///
/// # Safety
/// `data` valid for `data_len` bytes (or NULL); `name_buf` valid
/// for `name_cap` bytes (or NULL); `out` a valid writable
/// `UserListRecordOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_user_list_record(
    data: *const u8,
    data_len: usize,
    name_buf: *mut u8,
    name_cap: usize,
    out: *mut UserListRecordOut,
) -> bool {
    if out.is_null() || name_buf.is_null() || name_cap == 0 {
        return false;
    }
    let s = as_slice(data, data_len);
    match parse::parse_user_list_record(s, name_cap - 1) {
        Some(rec) => {
            let written = write_cstr(name_buf, name_cap, &rec.name);
            (*out).uid = rec.uid;
            (*out).icon = rec.icon;
            (*out).color = rec.color;
            match rec.nick_color {
                Some(c) => {
                    (*out).nick_color = c;
                    (*out).got_nick_color = 1;
                }
                None => {
                    (*out).nick_color = crate::messages::NICK_COLOR_NONE;
                    (*out).got_nick_color = 0;
                }
            }
            (*out).name_len = written as u16;
            true
        }
        None => false,
    }
}

/// C-ABI result of [`parse::parse_user_info`]. The sanitised name lands
/// in `name_buf`, the CR2LF + strip_ansi'd info body in `info_buf`;
/// both are NUL-terminated, capped at the corresponding `_cap - 1`.
#[repr(C)]
pub struct UserInfoOut {
    pub name_len: u16,
    pub info_len: u16,
}

/// Parse the post-`HTLC_HDR_USER_GETINFO` TASK reply payload (the C
/// `rcv_task_user_info` body — USER_GETINFO is a client opcode and
/// the server's reply arrives inside an HTLS_HDR_TASK frame).
/// Writes the strip_ansi'd name into
/// `name_buf` (cap `name_cap`) and the CR2LF + strip_ansi'd info body
/// into `info_buf` (cap `info_cap`). Returns false on any NULL /
/// zero-cap pointer; otherwise true. The C caller's `nlen && ilen`
/// dispatch gate is left to the call site (matches the rcv_task
/// behaviour where empty fields silently skip the emit).
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `name_buf` /
/// `info_buf` valid for their respective capacities (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_user_info(
    msg: *const u8,
    msglen: usize,
    name_buf: *mut u8,
    name_cap: usize,
    info_buf: *mut u8,
    info_cap: usize,
    out: *mut UserInfoOut,
) -> bool {
    if out.is_null()
        || name_buf.is_null()
        || name_cap == 0
        || info_buf.is_null()
        || info_cap == 0
    {
        return false;
    }
    let s = as_slice(msg, msglen);
    let ui = parse::parse_user_info(s, s.len(), name_cap - 1, info_cap - 1);
    let nw = write_cstr(name_buf, name_cap, &ui.name);
    let iw = write_cstr(info_buf, info_cap, &ui.info);
    (*out).name_len = nw as u16;
    (*out).info_len = iw as u16;
    true
}

/// C-ABI result of [`parse::parse_account_read`]. NAME / LOGIN / PASS
/// land in the caller's three buffers (NUL-terminated, capped at
/// `_cap - 1`); ACCESS is the raw 8 wire bytes; `got_access` is the
/// C-extractor's accessbool gate.
#[repr(C)]
pub struct AccountReadOut {
    pub access: [u8; 8],
    pub got_access: u8,
    pub name_len: u16,
    pub login_len: u16,
    pub pass_len: u16,
}

/// Parse the post-`HTLC_HDR_ACCOUNT_READ` TASK reply payload (the C
/// `rcv_task_user_open` body). Writes NAME into `name_buf`, the
/// XOR-0xff-decoded LOGIN into `login_buf`, and (when present) the
/// XOR-0xff-decoded PASSWORD into `pass_buf` — all three NUL-
/// terminated and capped at the matching `_cap - 1`. The 8-byte
/// ACCESS field lands verbatim in `out->access`; `out->got_access`
/// is the dispatch gate (the C call site only fires the user-edit
/// callback when this is non-zero).
///
/// PASSWORD no-password convention: a single zero byte (or empty /
/// missing) yields an empty `pass` buffer (NUL only); matches the
/// C extractor's `plen > 1 && dh->data[0]` gate.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `name_buf` /
/// `login_buf` / `pass_buf` valid for their respective capacities
/// (or NULL); `out` a valid writable `AccountReadOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_account_read(
    msg: *const u8,
    msglen: usize,
    name_buf: *mut u8,
    name_cap: usize,
    login_buf: *mut u8,
    login_cap: usize,
    pass_buf: *mut u8,
    pass_cap: usize,
    out: *mut AccountReadOut,
) -> bool {
    if out.is_null()
        || name_buf.is_null()
        || name_cap == 0
        || login_buf.is_null()
        || login_cap == 0
        || pass_buf.is_null()
        || pass_cap == 0
    {
        return false;
    }
    let s = as_slice(msg, msglen);
    let ar = parse::parse_account_read(
        s,
        s.len(),
        name_cap - 1,
        login_cap - 1,
        pass_cap - 1,
    );
    let nw = write_cstr(name_buf, name_cap, &ar.name);
    let lw = write_cstr(login_buf, login_cap, &ar.login);
    let pw = write_cstr(pass_buf, pass_cap, &ar.pass);
    (*out).access = ar.access;
    (*out).got_access = ar.got_access as u8;
    (*out).name_len = nw as u16;
    (*out).login_len = lw as u16;
    (*out).pass_len = pw as u16;
    true
}

/// C-ABI result of [`parse::parse_file_get_reply`].
#[repr(C)]
pub struct FileGetReplyOut {
    pub ref_: u32,
    pub size: u32,
    pub size64: u64,
    pub queue: u32,
    pub size64_seen: u8,
}

/// Parse the FILE_GET reply scalars (HTXF_REF + HTXF_SIZE +
/// optional XFERSIZE64 + optional QUEUE). Returns false on NULL
/// `out`; otherwise true (missing chunks default to zero — caller
/// applies the `(!size && !size64_seen) || !ref` dispatch gate).
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` a valid writable
/// `FileGetReplyOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_file_get_reply(
    msg: *const u8,
    msglen: usize,
    out: *mut FileGetReplyOut,
) -> bool {
    if out.is_null() {
        return false;
    }
    let s = as_slice(msg, msglen);
    let r = parse::parse_file_get_reply(s, s.len());
    (*out).ref_ = r.ref_;
    (*out).size = r.size;
    (*out).size64 = r.size64;
    (*out).queue = r.queue;
    (*out).size64_seen = r.size64_seen as u8;
    true
}

/// C-ABI result of [`parse::parse_folder_get_reply`]. Same shape as
/// [`FileGetReplyOut`] with `nfiles` appended.
#[repr(C)]
pub struct FolderGetReplyOut {
    pub ref_: u32,
    pub size: u32,
    pub size64: u64,
    pub queue: u32,
    pub nfiles: u32,
    pub size64_seen: u8,
}

/// Parse the FOLDER_GET reply scalars. Same contract as
/// [`gtkhx_proto_parse_file_get_reply`] plus `FILE_NFILES`.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` a valid writable
/// `FolderGetReplyOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_folder_get_reply(
    msg: *const u8,
    msglen: usize,
    out: *mut FolderGetReplyOut,
) -> bool {
    if out.is_null() {
        return false;
    }
    let s = as_slice(msg, msglen);
    let r = parse::parse_folder_get_reply(s, s.len());
    (*out).ref_ = r.ref_;
    (*out).size = r.size;
    (*out).size64 = r.size64;
    (*out).queue = r.queue;
    (*out).nfiles = r.nfiles;
    (*out).size64_seen = r.size64_seen as u8;
    true
}

/// C-ABI result of [`parse::parse_file_put_reply`]. `data_pos` /
/// `rsrc_pos` carry the fork resume offsets parsed from the RFLT
/// payload (zero when no RFLT was present or it was shorter than 66
/// bytes).
#[repr(C)]
pub struct FilePutReplyOut {
    pub ref_: u32,
    pub queue: u32,
    pub data_pos: u32,
    pub rsrc_pos: u32,
}

/// Parse the FILE_PUT reply scalars (HTXF_REF + optional QUEUE +
/// optional RFLT with fork offsets at +46 / +62). Missing chunks
/// default to zero; the caller applies the `!ref` dispatch gate.
/// Returns false on NULL `out`; otherwise true.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` a valid writable
/// `FilePutReplyOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_file_put_reply(
    msg: *const u8,
    msglen: usize,
    out: *mut FilePutReplyOut,
) -> bool {
    if out.is_null() {
        return false;
    }
    let s = as_slice(msg, msglen);
    let r = parse::parse_file_put_reply(s, s.len());
    (*out).ref_ = r.ref_;
    (*out).queue = r.queue;
    (*out).data_pos = r.data_pos;
    (*out).rsrc_pos = r.rsrc_pos;
    true
}

/// C-ABI result of [`parse::parse_folder_put_reply`]. Strict subset
/// of [`FilePutReplyOut`] (no RFLT — per-file resume happens inside
/// folder_put_thread, not at the task boundary).
#[repr(C)]
pub struct FolderPutReplyOut {
    pub ref_: u32,
    pub queue: u32,
}

/// Parse the FOLDER_PUT reply scalars.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` a valid writable
/// `FolderPutReplyOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_folder_put_reply(
    msg: *const u8,
    msglen: usize,
    out: *mut FolderPutReplyOut,
) -> bool {
    if out.is_null() {
        return false;
    }
    let s = as_slice(msg, msglen);
    let r = parse::parse_folder_put_reply(s, s.len());
    (*out).ref_ = r.ref_;
    (*out).queue = r.queue;
    true
}

/// C-ABI result of [`parse::parse_banner_get_reply`]. Just the
/// transfer reference + total byte count for the HTXF subchannel
/// fetch banner.c spins up.
#[repr(C)]
pub struct BannerGetReplyOut {
    pub ref_: u32,
    pub size: u32,
}

/// Parse the DOWNLOAD_BANNER reply scalars.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `out` a valid writable
/// `BannerGetReplyOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_banner_get_reply(
    msg: *const u8,
    msglen: usize,
    out: *mut BannerGetReplyOut,
) -> bool {
    if out.is_null() {
        return false;
    }
    let s = as_slice(msg, msglen);
    let r = parse::parse_banner_get_reply(s, s.len());
    (*out).ref_ = r.ref_;
    (*out).size = r.size;
    true
}

/// C-ABI result of [`parse::parse_news_thread_reply`]. `text_len`
/// reports bytes written to `text_buf` (NUL-terminated, capped at
/// `text_cap - 1`); `has_text` is the dispatch gate — when zero,
/// the reply carried no NEWSDATA chunk (or a TASK_ERROR
/// short-circuited the walk), and the caller should bail. The text
/// buffer is left NUL-terminated at offset 0 in that case.
#[repr(C)]
pub struct NewsThreadReplyOut {
    pub thread_id: u32,
    pub text_len: u16,
    pub has_text: u8,
    pub has_task_error: u8,
}

/// Parse the post-`HTLC_HDR_GETTHREAD` TASK reply payload (the C
/// `rcv_task_news_post` body). Writes the CR2LF + strip_ansi'd
/// NEWSDATA body into `text_buf` (NUL-terminated, capped at
/// `text_cap - 1`) and fills `*out`. Returns false on NULL `out`,
/// NULL `text_buf`, or zero `text_cap`; otherwise true. Empty NEWSDATA
/// still sets `has_text = 1` (the caller can emit an empty article);
/// only missing NEWSDATA or a TASK_ERROR short-circuit yields
/// `has_text = 0`.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); `text_buf` valid for
/// `text_cap` bytes (or NULL); `out` a valid writable
/// `NewsThreadReplyOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_news_thread_reply(
    msg: *const u8,
    msglen: usize,
    text_buf: *mut u8,
    text_cap: usize,
    out: *mut NewsThreadReplyOut,
) -> bool {
    if out.is_null() || text_buf.is_null() || text_cap == 0 {
        return false;
    }
    let s = as_slice(msg, msglen);
    let r = parse::parse_news_thread_reply(s, s.len(), text_cap - 1);
    match &r.text {
        Some(bytes) => {
            let written = write_cstr(text_buf, text_cap, bytes);
            (*out).text_len = written as u16;
            (*out).has_text = 1;
        }
        None => {
            // Leave the buffer NUL-terminated at offset 0 so the C
            // caller can treat text_buf as a valid empty C string.
            *text_buf = 0;
            (*out).text_len = 0;
            (*out).has_text = 0;
        }
    }
    (*out).thread_id = r.thread_id;
    (*out).has_task_error = r.has_task_error as u8;
    true
}

/// C-ABI result of [`parse::parse_file_getinfo`]. Strings land in
/// caller-owned `name_buf` / `type_buf` / `creator_buf` /
/// `comment_buf`, NUL-terminated, capped at the matching `_cap - 1`;
/// dates and icon land in fixed-size byte arrays.
#[repr(C)]
pub struct FileGetInfoOut {
    pub icon: [u8; 4],
    pub date_create: [u8; 8],
    pub date_modify: [u8; 8],
    pub size: u32,
    pub size64: u64,
    pub size64_seen: u8,
    pub got_icon: u8,
    pub name_len: u16,
    pub type_len: u16,
    pub creator_len: u16,
    pub comment_len: u16,
}

/// Parse the FILE_GETINFO reply. Writes strip_ansi'd `FILE_NAME` into
/// `name_buf`, raw `FILE_TYPE` / `FILE_CREATOR` into their buffers,
/// CR2LF + strip_ansi'd `FILE_COMMENT` into `comment_buf` — all NUL-
/// terminated and capped at the matching `_cap - 1`. `FILE_ICON`,
/// `FILE_DATE_CREATE`, `FILE_DATE_MODIFY` land in fixed-size byte
/// arrays inside `*out`. Returns false on any NULL / zero-cap
/// pointer; otherwise true.
///
/// # Safety
/// `msg` valid for `msglen` bytes (or NULL); the four string buffers
/// valid for their capacities (or NULL); `out` a valid writable
/// `FileGetInfoOut`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_parse_file_getinfo(
    msg: *const u8,
    msglen: usize,
    name_buf: *mut u8,
    name_cap: usize,
    type_buf: *mut u8,
    type_cap: usize,
    creator_buf: *mut u8,
    creator_cap: usize,
    comment_buf: *mut u8,
    comment_cap: usize,
    out: *mut FileGetInfoOut,
) -> bool {
    if out.is_null()
        || name_buf.is_null()
        || name_cap == 0
        || type_buf.is_null()
        || type_cap == 0
        || creator_buf.is_null()
        || creator_cap == 0
        || comment_buf.is_null()
        || comment_cap == 0
    {
        return false;
    }
    let s = as_slice(msg, msglen);
    let f = parse::parse_file_getinfo(
        s,
        s.len(),
        name_cap - 1,
        type_cap - 1,
        creator_cap - 1,
        comment_cap - 1,
    );
    let nw = write_cstr(name_buf, name_cap, &f.name);
    let tw = write_cstr(type_buf, type_cap, &f.type_);
    let cw = write_cstr(creator_buf, creator_cap, &f.creator);
    let mw = write_cstr(comment_buf, comment_cap, &f.comment);
    (*out).icon = f.icon;
    (*out).date_create = f.date_create;
    (*out).date_modify = f.date_modify;
    (*out).size = f.size;
    (*out).size64 = f.size64;
    (*out).size64_seen = f.size64_seen as u8;
    (*out).got_icon = f.got_icon as u8;
    (*out).name_len = nw as u16;
    (*out).type_len = tw as u16;
    (*out).creator_len = cw as u16;
    (*out).comment_len = mw as u16;
    true
}

// ---- User-management SEND builders -----------------------------------
//
// Same chunks[] + scratch[] contract as the chat / msg / broadcast
// builders above. Each shim validates *_cap first, then constructs
// fixed-size slices sized to MAX_* (NOT chunks_cap / scratch_cap) so a
// too-large caller cap can't drive from_raw_parts_mut into UB.

/// Build `HTLC_HDR_USER_CHANGE` chunks (ICON + NAME, + optional COLOR
/// when `has_nick_color` is non-zero). `chunks_cap >= 3`,
/// `scratch_cap >= 6`. Returns 2 (no color) or 3 (with color) on
/// success, 0 on validation failure.
///
/// `nick_color` is consulted only when `has_nick_color != 0` (the C
/// `HX_NICK_COLOR_NONE` sentinel is checked on the caller side; the
/// FFI takes the chunk-emission decision as a bool to keep the wire
/// shape decision close to the bytes).
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths, or NULL.
/// `name_ptr` valid for `name_len` bytes, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_user_change_chunks(
    icon: u16,
    name_ptr: *const u8,
    name_len: usize,
    has_nick_color: u8,
    nick_color: u32,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 3;
    const MAX_SCRATCH: usize = 6;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let name = as_slice(name_ptr, name_len);
    let req = UserChangeRequest {
        icon,
        name,
        nick_color: if has_nick_color != 0 { Some(nick_color) } else { None },
    };
    build::build_user_change_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_USER_KICK` chunks. When `ban != 0`, emits BAN
/// followed by UID (2 chunks); when `ban == 0`, emits just UID
/// (1 chunk). `chunks_cap >= 2`, `scratch_cap >= 4`.
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_user_kick_chunks(
    uid: u16,
    ban: u16,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 2;
    const MAX_SCRATCH: usize = 4;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let req = UserKickRequest { uid, ban };
    build::build_user_kick_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_USER_GETINFO` chunks: single UID chunk.
/// `chunks_cap >= 1`, `scratch_cap >= 2`.
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_user_getinfo_chunks(
    uid: u16,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 1;
    const MAX_SCRATCH: usize = 2;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    build::build_user_getinfo_chunks(uid, chunks_slice, scratch_slice) as i32
}

// ---- Account-management SEND builders --------------------------------
//
// Same chunks[] + scratch[] contract as the user-management shims
// above. ACCOUNT_READ and ACCOUNT_DELETE share a single-LOGIN wire
// shape and so share a single FFI implementation under the hood; the
// distinct entry points keep call-site semantics clear.

/// Common implementation for the ACCOUNT_READ / ACCOUNT_DELETE wire
/// shape (a single `HTLC_DATA_LOGIN` chunk). Both opcodes have
/// identical byte layout; the C caller picks the header type when
/// calling hlwrite_chunks.
unsafe fn build_account_login_only_chunks(
    login_ptr: *const u8,
    login_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    body_using: fn(&[u8], &mut [HxChunk]) -> usize,
) -> i32 {
    const MAX_CHUNKS: usize = 1;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if login_ptr.is_null() && login_len != 0 {
        return 0;
    }
    if login_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let login = as_slice(login_ptr, login_len);
    body_using(login, chunks_slice) as i32
}

/// Build `HTLC_HDR_ACCOUNT_READ` chunks: single LOGIN. `chunks_cap >= 1`.
/// No scratch needed.
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `login_ptr` valid
/// for `login_len` bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_account_read_chunks(
    login_ptr: *const u8,
    login_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_account_login_only_chunks(
        login_ptr,
        login_len,
        chunks,
        chunks_cap,
        build::build_account_read_chunks,
    )
}

/// Build `HTLC_HDR_ACCOUNT_DELETE` chunks: single LOGIN. Same shape
/// as ACCOUNT_READ. `chunks_cap >= 1`. No scratch needed.
///
/// # Safety
/// As [`gtkhx_proto_build_account_read_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_account_delete_chunks(
    login_ptr: *const u8,
    login_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_account_login_only_chunks(
        login_ptr,
        login_len,
        chunks,
        chunks_cap,
        build::build_account_delete_chunks,
    )
}

/// Build `HTLC_HDR_ACCOUNT_MODIFY` chunks: LOGIN + PASSWORD + NAME +
/// ACCESS (8 bytes). The `access` argument is a pointer to 8 raw wire
/// bytes (the `hl_access_bits` bitmap, big-endian-in-memory). Returns
/// 4 on success.
///
/// `chunks_cap >= 4`, `scratch_cap >= 8` (the access bytes live in
/// scratch). Rejects any individual field longer than `u16::MAX`.
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths (or NULL).
/// Each of `login_ptr` / `password_ptr` / `name_ptr` valid for the
/// matching length (or NULL with length 0). `access_ptr` must be a
/// valid pointer to at least 8 bytes (NULL is rejected — ACCESS is
/// mandatory).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_account_modify_chunks(
    login_ptr: *const u8,
    login_len: usize,
    password_ptr: *const u8,
    password_len: usize,
    name_ptr: *const u8,
    name_len: usize,
    access_ptr: *const u8,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 4;
    const MAX_SCRATCH: usize = 8;

    if chunks.is_null() || scratch.is_null() || access_ptr.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    if (login_ptr.is_null() && login_len != 0)
        || (password_ptr.is_null() && password_len != 0)
        || (name_ptr.is_null() && name_len != 0)
    {
        return 0;
    }
    if login_len > u16::MAX as usize
        || password_len > u16::MAX as usize
        || name_len > u16::MAX as usize
    {
        return 0;
    }

    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);

    let mut access = [0u8; 8];
    std::ptr::copy_nonoverlapping(access_ptr, access.as_mut_ptr(), 8);

    let req = AccountModifyRequest {
        login: as_slice(login_ptr, login_len),
        password: as_slice(password_ptr, password_len),
        name: as_slice(name_ptr, name_len),
        access,
    };
    build::build_account_modify_chunks(&req, chunks_slice, scratch_slice) as i32
}

// ---- News-1.0 + News-1.5 SEND builders -------------------------------
//
// HTLC_HDR_NEWS_POST: single body chunk (1.0 flat news).
// HTLC_HDR_NEWSCATLIST / _NEWSDIRLIST / _DELNEWSDIRCAT /
//   _MAKENEWSDIR: single HTLC_DATA_NEWSPATH chunk; they share a single
// underlying FFI helper. The distinct entry points keep the wire
// opcode obvious at the call site.

/// Build `HTLC_HDR_NEWS_POST` chunks: a single body chunk.
/// `chunks_cap >= 1`. No scratch needed. Returns 1 on success, or 0
/// on validation failure (NULL `chunks`, NULL body_ptr with non-zero
/// body_len, `body_len > u16::MAX`, or `chunks_cap < 1`).
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `body_ptr` valid
/// for `body_len` bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_post_chunks(
    body_ptr: *const u8,
    body_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 1;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if body_ptr.is_null() && body_len != 0 {
        return 0;
    }
    if body_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let body = as_slice(body_ptr, body_len);
    build::build_news_post_chunks(body, chunks_slice) as i32
}

/// Common implementation for the four NEWSPATH-only 1.5 news opcodes.
/// The opcode-specific entry points below pick which `build::` helper
/// to dispatch to so the call-site code reads naturally.
unsafe fn build_news_path_only_chunks(
    path_ptr: *const u8,
    path_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    inner: fn(&[u8], &mut [HxChunk]) -> usize,
) -> i32 {
    const MAX_CHUNKS: usize = 1;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if path_ptr.is_null() && path_len != 0 {
        return 0;
    }
    if path_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let path = as_slice(path_ptr, path_len);
    inner(path, chunks_slice) as i32
}

/// Build `HTLC_HDR_NEWSCATLIST` chunks: single `HTLC_DATA_NEWSPATH`.
/// `chunks_cap >= 1`. Returns 1 on success, 0 on validation failure.
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `path_ptr` valid
/// for `path_len` bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_catlist_chunks(
    path_ptr: *const u8,
    path_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_news_path_only_chunks(
        path_ptr,
        path_len,
        chunks,
        chunks_cap,
        build::build_news_catlist_chunks,
    )
}

/// Build `HTLC_HDR_NEWSDIRLIST` chunks. Same shape and contract as
/// [`gtkhx_proto_build_news_catlist_chunks`].
///
/// # Safety
/// As [`gtkhx_proto_build_news_catlist_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_dirlist_chunks(
    path_ptr: *const u8,
    path_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_news_path_only_chunks(
        path_ptr,
        path_len,
        chunks,
        chunks_cap,
        build::build_news_dirlist_chunks,
    )
}

/// Build `HTLC_HDR_DELNEWSDIRCAT` chunks (delete a 1.5 news folder or
/// category — the path tells the server which). Same shape and
/// contract as [`gtkhx_proto_build_news_catlist_chunks`].
///
/// # Safety
/// As [`gtkhx_proto_build_news_catlist_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_delete_chunks(
    path_ptr: *const u8,
    path_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_news_path_only_chunks(
        path_ptr,
        path_len,
        chunks,
        chunks_cap,
        build::build_news_delete_chunks,
    )
}

/// Build `HTLC_HDR_MAKENEWSDIR` chunks. Same shape and contract as
/// [`gtkhx_proto_build_news_catlist_chunks`].
///
/// # Safety
/// As [`gtkhx_proto_build_news_catlist_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_mkdir_chunks(
    path_ptr: *const u8,
    path_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_news_path_only_chunks(
        path_ptr,
        path_len,
        chunks,
        chunks_cap,
        build::build_news_mkdir_chunks,
    )
}

// ---- 1.5 news SEND builders with extra fields ------------------------

/// Build `HTLC_HDR_DELETETHREAD` chunks: NEWSPATH + THREADID.
/// `chunks_cap >= 2`, `scratch_cap >= 4`. Returns 2 on success, or 0
/// on validation failure.
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths (or NULL);
/// `path_ptr` valid for `path_len` bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_delete_thread_chunks(
    path_ptr: *const u8,
    path_len: usize,
    threadid: u32,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 2;
    const MAX_SCRATCH: usize = 4;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    if path_ptr.is_null() && path_len != 0 {
        return 0;
    }
    if path_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let req = NewsDeleteThreadRequest {
        path: as_slice(path_ptr, path_len),
        threadid,
    };
    build::build_news_delete_thread_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_GETTHREAD` chunks: NEWSPATH + THREADID + NEWSTYPE.
/// `chunks_cap >= 3`, `scratch_cap >= 4`. Returns 3 on success, 0 on
/// validation failure (NULL ptr / cap / oversize path or mime_type).
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths (or NULL);
/// `path_ptr` / `mime_type_ptr` valid for their lengths (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_getthread_chunks(
    path_ptr: *const u8,
    path_len: usize,
    threadid: u32,
    mime_type_ptr: *const u8,
    mime_type_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 3;
    const MAX_SCRATCH: usize = 4;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    if (path_ptr.is_null() && path_len != 0)
        || (mime_type_ptr.is_null() && mime_type_len != 0)
    {
        return 0;
    }
    if path_len > u16::MAX as usize || mime_type_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let req = NewsGetThreadRequest {
        path: as_slice(path_ptr, path_len),
        threadid,
        mime_type: as_slice(mime_type_ptr, mime_type_len),
    };
    build::build_news_getthread_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_MAKECATEGORY` chunks: NEWSPATH + CATEGORY.
/// `chunks_cap >= 2`. No scratch needed.
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `path_ptr` /
/// `name_ptr` valid for their lengths (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_mkcat_chunks(
    path_ptr: *const u8,
    path_len: usize,
    name_ptr: *const u8,
    name_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 2;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if (path_ptr.is_null() && path_len != 0)
        || (name_ptr.is_null() && name_len != 0)
    {
        return 0;
    }
    if path_len > u16::MAX as usize || name_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let req = NewsMakeCategoryRequest {
        path: as_slice(path_ptr, path_len),
        name: as_slice(name_ptr, name_len),
    };
    build::build_news_mkcat_chunks(&req, chunks_slice) as i32
}

/// Build `HTLC_HDR_POSTTHREAD` chunks: 6 chunks in the wire order
/// NEWSPATH + PARENTTHREAD + NEWSTYPE + NEWSSUBJECT + NEWSDATA +
/// THREADID. `chunks_cap >= 6`, `scratch_cap >= 8` (two u32s).
///
/// # Safety
/// `chunks` / `scratch` valid for their declared lengths (or NULL);
/// each variable-length pointer valid for its matching length (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_news_post_thread_chunks(
    path_ptr: *const u8,
    path_len: usize,
    parent_thread: u32,
    mime_type_ptr: *const u8,
    mime_type_len: usize,
    subject_ptr: *const u8,
    subject_len: usize,
    text_ptr: *const u8,
    text_len: usize,
    thread_id: u32,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 6;
    const MAX_SCRATCH: usize = 8;

    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    if (path_ptr.is_null() && path_len != 0)
        || (mime_type_ptr.is_null() && mime_type_len != 0)
        || (subject_ptr.is_null() && subject_len != 0)
        || (text_ptr.is_null() && text_len != 0)
    {
        return 0;
    }
    if path_len > u16::MAX as usize
        || mime_type_len > u16::MAX as usize
        || subject_len > u16::MAX as usize
        || text_len > u16::MAX as usize
    {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let req = NewsPostThreadRequest {
        path: as_slice(path_ptr, path_len),
        parent_thread,
        mime_type: as_slice(mime_type_ptr, mime_type_len),
        subject: as_slice(subject_ptr, subject_len),
        text: as_slice(text_ptr, text_len),
        thread_id,
    };
    build::build_news_post_thread_chunks(&req, chunks_slice, scratch_slice) as i32
}

// ---- File send opcodes -----------------------------------------------
//
// FILE_MKDIR: single DIR chunk. FILE_DELETE / _GETINFO / _GETFOLDER:
// FILE_NAME + optional DIR (presence signalled by has_dir; when 0,
// `dir_ptr` / `dir_len` are ignored). The four shims share an
// internal helper for the FILE_NAME+optional-DIR shape.

/// Build `HTLC_HDR_FILE_MKDIR` chunks: single DIR. `chunks_cap >= 1`,
/// no scratch needed. Returns 1 on success, or 0 on validation
/// failure (NULL `chunks`, NULL `dir_ptr` with non-zero len, oversize
/// dir, or `chunks_cap < 1`).
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `dir_ptr` valid
/// for `dir_len` bytes (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_mkdir_chunks(
    dir_ptr: *const u8,
    dir_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 1;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if dir_ptr.is_null() && dir_len != 0 {
        return 0;
    }
    if dir_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let dir = as_slice(dir_ptr, dir_len);
    build::build_file_mkdir_chunks(dir, chunks_slice) as i32
}

/// Build `HTLC_HDR_FILE_LIST` chunks: single DIR. Same contract as
/// [`gtkhx_proto_build_file_mkdir_chunks`] — the wire shape is
/// identical, only the header opcode differs.
///
/// # Safety
/// As [`gtkhx_proto_build_file_mkdir_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_list_chunks(
    dir_ptr: *const u8,
    dir_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 1;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if dir_ptr.is_null() && dir_len != 0 {
        return 0;
    }
    if dir_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let dir = as_slice(dir_ptr, dir_len);
    build::build_file_list_chunks(dir, chunks_slice) as i32
}

/// Common implementation for the three file-ops opcodes that share
/// the FILE_NAME + optional DIR wire shape. `has_dir` is a 0/1 flag —
/// when non-zero, emit DIR with the given bytes; when zero, omit it
/// entirely (`dir_ptr` / `dir_len` are ignored in that case).
unsafe fn build_file_name_with_optional_dir_chunks(
    name_ptr: *const u8,
    name_len: usize,
    has_dir: u8,
    dir_ptr: *const u8,
    dir_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    inner: fn(&[u8], Option<&[u8]>, &mut [HxChunk]) -> usize,
) -> i32 {
    const MAX_CHUNKS: usize = 2;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    let dir_opt = if has_dir != 0 {
        if dir_ptr.is_null() && dir_len != 0 {
            return 0;
        }
        if dir_len > u16::MAX as usize {
            return 0;
        }
        Some(as_slice(dir_ptr, dir_len))
    } else {
        None
    };
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let name = as_slice(name_ptr, name_len);
    inner(name, dir_opt, chunks_slice) as i32
}

/// Build `HTLC_HDR_FILE_DELETE` chunks: FILE_NAME + optional DIR.
/// `chunks_cap >= 2`. Returns 1 (no dir) or 2 (with dir) on success,
/// 0 on validation failure.
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `name_ptr` /
/// `dir_ptr` valid for their respective lengths (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_delete_chunks(
    name_ptr: *const u8,
    name_len: usize,
    has_dir: u8,
    dir_ptr: *const u8,
    dir_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_file_name_with_optional_dir_chunks(
        name_ptr,
        name_len,
        has_dir,
        dir_ptr,
        dir_len,
        chunks,
        chunks_cap,
        build::build_file_delete_chunks,
    )
}

/// Build `HTLC_HDR_FILE_GETINFO` chunks. Same shape and contract as
/// [`gtkhx_proto_build_file_delete_chunks`].
///
/// # Safety
/// As [`gtkhx_proto_build_file_delete_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_getinfo_chunks(
    name_ptr: *const u8,
    name_len: usize,
    has_dir: u8,
    dir_ptr: *const u8,
    dir_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_file_name_with_optional_dir_chunks(
        name_ptr,
        name_len,
        has_dir,
        dir_ptr,
        dir_len,
        chunks,
        chunks_cap,
        build::build_file_getinfo_chunks,
    )
}

/// Build `HTLC_HDR_FILE_GETFOLDER` chunks. Same shape and contract as
/// [`gtkhx_proto_build_file_delete_chunks`].
///
/// # Safety
/// As [`gtkhx_proto_build_file_delete_chunks`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_getfolder_chunks(
    name_ptr: *const u8,
    name_len: usize,
    has_dir: u8,
    dir_ptr: *const u8,
    dir_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    build_file_name_with_optional_dir_chunks(
        name_ptr,
        name_len,
        has_dir,
        dir_ptr,
        dir_len,
        chunks,
        chunks_cap,
        build::build_file_getfolder_chunks,
    )
}

/// Build `HTLC_HDR_FILE_SETINFO` chunks: FILE_NAME + FILE_RENAME +
/// optional FILE_COMMENT + optional DIR. `has_comment` / `has_dir` are
/// 0/1 flags — when non-zero, the matching chunk is emitted. Caller
/// must size `chunks_cap >= 4` to hold the full setinfo. Returns
/// 2..=4 on success, 0 on validation failure (NULL `chunks`, NULL
/// payload pointer with non-zero len, oversize field, or short slice).
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); each payload
/// pointer is valid for the matching length, or NULL when both the
/// flag and length are zero.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_setinfo_chunks(
    name_ptr: *const u8,
    name_len: usize,
    rename_ptr: *const u8,
    rename_len: usize,
    has_comment: u8,
    comment_ptr: *const u8,
    comment_len: usize,
    has_dir: u8,
    dir_ptr: *const u8,
    dir_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 4;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    if rename_ptr.is_null() && rename_len != 0 {
        return 0;
    }
    if rename_len > u16::MAX as usize {
        return 0;
    }
    let comment_opt = if has_comment != 0 {
        if comment_ptr.is_null() && comment_len != 0 {
            return 0;
        }
        if comment_len > u16::MAX as usize {
            return 0;
        }
        Some(as_slice(comment_ptr, comment_len))
    } else {
        None
    };
    let dir_opt = if has_dir != 0 {
        if dir_ptr.is_null() && dir_len != 0 {
            return 0;
        }
        if dir_len > u16::MAX as usize {
            return 0;
        }
        Some(as_slice(dir_ptr, dir_len))
    } else {
        None
    };
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let req = build::FileSetInfoRequest {
        name: as_slice(name_ptr, name_len),
        rename: as_slice(rename_ptr, rename_len),
        comment: comment_opt,
        dir: dir_opt,
    };
    build::build_file_setinfo_chunks(&req, chunks_slice) as i32
}

/// Build `HTLC_HDR_FILE_MOVE` chunks: FILE_NAME + DIR + DIR_RENAME.
/// `chunks_cap >= 3`. Returns 3 on success, 0 on validation failure
/// (NULL `chunks`, NULL payload pointer with non-zero len, oversize
/// field, or short slice).
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); each payload
/// pointer is valid for the matching length, or NULL when the
/// matching length is 0.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_move_chunks(
    name_ptr: *const u8,
    name_len: usize,
    dir_ptr: *const u8,
    dir_len: usize,
    dir_rename_ptr: *const u8,
    dir_rename_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 3;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    if dir_ptr.is_null() && dir_len != 0 {
        return 0;
    }
    if dir_len > u16::MAX as usize {
        return 0;
    }
    if dir_rename_ptr.is_null() && dir_rename_len != 0 {
        return 0;
    }
    if dir_rename_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let req = build::FileMoveRequest {
        name: as_slice(name_ptr, name_len),
        dir: as_slice(dir_ptr, dir_len),
        dir_rename: as_slice(dir_rename_ptr, dir_rename_len),
    };
    build::build_file_move_chunks(&req, chunks_slice) as i32
}

/// Build `HTLC_HDR_FILE_SYMLINK` chunks: FILE_NAME + DIR + DIR_RENAME
/// + FILE_RENAME. `chunks_cap >= 4`. Returns 4 on success, 0 on
/// validation failure (NULL `chunks`, NULL payload pointer with
/// non-zero len, oversize field, or short slice).
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); each payload
/// pointer is valid for the matching length, or NULL when the
/// matching length is 0.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_symlink_chunks(
    name_ptr: *const u8,
    name_len: usize,
    dir_ptr: *const u8,
    dir_len: usize,
    dir_rename_ptr: *const u8,
    dir_rename_len: usize,
    rename_ptr: *const u8,
    rename_len: usize,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 4;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    if dir_ptr.is_null() && dir_len != 0 {
        return 0;
    }
    if dir_len > u16::MAX as usize {
        return 0;
    }
    if dir_rename_ptr.is_null() && dir_rename_len != 0 {
        return 0;
    }
    if dir_rename_len > u16::MAX as usize {
        return 0;
    }
    if rename_ptr.is_null() && rename_len != 0 {
        return 0;
    }
    if rename_len > u16::MAX as usize {
        return 0;
    }
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let req = build::FileSymlinkRequest {
        name: as_slice(name_ptr, name_len),
        dir: as_slice(dir_ptr, dir_len),
        dir_rename: as_slice(dir_rename_ptr, dir_rename_len),
        rename: as_slice(rename_ptr, rename_len),
    };
    build::build_file_symlink_chunks(&req, chunks_slice) as i32
}

/// Build `HTLC_HDR_FILE_PUTFOLDER` chunks: FILE_NAME + optional DIR +
/// HTXF_SIZE (u32 BE) + FILE_NFILES (u32 BE). `chunks_cap >= 4` (the
/// shim always reserves the with-dir slot count to give us a fixed
/// `MAX_CHUNKS` to slice to). `scratch_cap >= 8` for the two BE u32
/// scratch slots. Returns 3 (no dir) or 4 (with dir) on success, 0 on
/// validation failure (NULL `chunks` / `scratch`, NULL payload pointer
/// with non-zero len, oversize field, or short slice).
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `scratch` valid
/// for `scratch_cap` bytes (or NULL); `name_ptr` / `dir_ptr` valid for
/// their respective lengths (or NULL when the matching length is 0,
/// and for `dir_ptr` also valid when `has_dir == 0`).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_putfolder_chunks(
    name_ptr: *const u8,
    name_len: usize,
    has_dir: u8,
    dir_ptr: *const u8,
    dir_len: usize,
    size: u32,
    nfiles: u32,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 4;
    const MAX_SCRATCH: usize = 8;
    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    let dir_opt = if has_dir != 0 {
        if dir_ptr.is_null() && dir_len != 0 {
            return 0;
        }
        if dir_len > u16::MAX as usize {
            return 0;
        }
        Some(as_slice(dir_ptr, dir_len))
    } else {
        None
    };
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let req = build::FilePutFolderRequest {
        name: as_slice(name_ptr, name_len),
        dir: dir_opt,
        size,
        nfiles,
    };
    build::build_file_putfolder_chunks(&req, chunks_slice, scratch_slice) as i32
}

/// Build `HTLC_HDR_FILE_GET` chunks: FILE_NAME + optional DIR +
/// optional RFLT (74 bytes). `has_dir` / `has_rflt` are 0/1 flags.
/// When `has_rflt` is non-zero, `rflt_ptr` must point to exactly 74
/// bytes (the resume-payload size is fixed; the C caller builds the
/// binary blob with fork offsets baked in). `chunks_cap >= 3`.
/// Returns 1..=3 on success, 0 on validation failure (NULL `chunks`,
/// NULL payload pointer with non-zero len, oversize field, RFLT
/// length not exactly 74, or short slice).
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `name_ptr` /
/// `dir_ptr` valid for their respective lengths (or NULL when the
/// matching length is 0). `rflt_ptr` valid for 74 bytes when
/// `has_rflt != 0` (or NULL when both `has_rflt` is 0).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_get_chunks(
    name_ptr: *const u8,
    name_len: usize,
    has_dir: u8,
    dir_ptr: *const u8,
    dir_len: usize,
    has_rflt: u8,
    rflt_ptr: *const u8,
    chunks: *mut HxChunk,
    chunks_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 3;
    const RFLT_LEN: usize = 74;
    if chunks.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS {
        return 0;
    }
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    let dir_opt = if has_dir != 0 {
        if dir_ptr.is_null() && dir_len != 0 {
            return 0;
        }
        if dir_len > u16::MAX as usize {
            return 0;
        }
        Some(as_slice(dir_ptr, dir_len))
    } else {
        None
    };
    let rflt_opt = if has_rflt != 0 {
        if rflt_ptr.is_null() {
            return 0;
        }
        Some(slice::from_raw_parts(rflt_ptr, RFLT_LEN))
    } else {
        None
    };
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let req = build::FileGetRequest {
        name: as_slice(name_ptr, name_len),
        dir: dir_opt,
        rflt: rflt_opt,
    };
    build::build_file_get_chunks(&req, chunks_slice) as i32
}

/// Build `HTLC_HDR_FILE_PUT` chunks: FILE_NAME + optional DIR +
/// optional FILE_PREVIEW(2) + HTXF_SIZE(u32 BE) + optional
/// XFERSIZE64(u64 BE). `has_dir` / `has_preview` / `has_size64` are
/// 0/1 flags. `size` is the clamped-to-u32 transfer size (caller
/// clamps a u64 down before the call); `size64` is the true u64
/// total used only when `has_size64 != 0` (large-files mode).
/// `chunks_cap >= 5` (the shim always reserves the full slot count);
/// `scratch_cap >= 12` (u32 at +0, u64 at +4). Returns 2..=5 on
/// success, 0 on validation failure.
///
/// # Safety
/// `chunks` valid for `chunks_cap` slots (or NULL); `scratch` valid
/// for `scratch_cap` bytes (or NULL); `name_ptr` / `dir_ptr` valid
/// for their respective lengths (or NULL when the matching length
/// is 0).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_proto_build_file_put_chunks(
    name_ptr: *const u8,
    name_len: usize,
    has_dir: u8,
    dir_ptr: *const u8,
    dir_len: usize,
    has_preview: u8,
    size: u32,
    has_size64: u8,
    size64: u64,
    chunks: *mut HxChunk,
    chunks_cap: usize,
    scratch: *mut u8,
    scratch_cap: usize,
) -> i32 {
    const MAX_CHUNKS: usize = 5;
    const MAX_SCRATCH: usize = 12;
    if chunks.is_null() || scratch.is_null() {
        return 0;
    }
    if chunks_cap < MAX_CHUNKS || scratch_cap < MAX_SCRATCH {
        return 0;
    }
    if name_ptr.is_null() && name_len != 0 {
        return 0;
    }
    if name_len > u16::MAX as usize {
        return 0;
    }
    let dir_opt = if has_dir != 0 {
        if dir_ptr.is_null() && dir_len != 0 {
            return 0;
        }
        if dir_len > u16::MAX as usize {
            return 0;
        }
        Some(as_slice(dir_ptr, dir_len))
    } else {
        None
    };
    let chunks_slice = slice::from_raw_parts_mut(chunks, MAX_CHUNKS);
    let scratch_slice = slice::from_raw_parts_mut(scratch, MAX_SCRATCH);
    let req = build::FilePutRequest {
        name: as_slice(name_ptr, name_len),
        dir: dir_opt,
        has_preview: has_preview != 0,
        size,
        size64: if has_size64 != 0 { Some(size64) } else { None },
    };
    build::build_file_put_chunks(&req, chunks_slice, scratch_slice) as i32
}
