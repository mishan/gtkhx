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

use crate::parse::{self, Header};
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
