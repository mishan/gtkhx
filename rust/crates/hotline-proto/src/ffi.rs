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
    self, AgreementAgreeRequest, BroadcastRequest, ChatRequest, ChatSubjectRequest, HxChunk,
    MsgRequest,
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
