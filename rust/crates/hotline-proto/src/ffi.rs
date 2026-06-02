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
