//! S0.1 of the xfer-worker migration: Rust-owned storage for the HTXF transfer
//! handle (`struct htxf_conn`).
//!
//! Lives in `hxnet` because this crate already owns the HTXF domain — the
//! handle's `hx` field *is* an [`crate::htxf::HtxfConn`] transport channel, and
//! the transfer loops ([`crate::xfer`]) that drive the handle are here too.
//!
//! The struct stays C-visible: the 9 C consumers keep direct `htxf->field`
//! access through the unchanged `struct htxf_conn` declaration in `protocol.h`.
//! What moves here is *allocation* — `hx_htxf_new` boxes a zeroed [`HtxfHandle`]
//! (this module's `#[repr(C)]` mirror) and returns the raw pointer;
//! `hx_htxf_free` drops it. S0.2 will move the `refcount` / `canceled` /
//! `total_pos` lifecycle onto atomics behind more of this ABI; S0.1 is pure
//! scaffolding with no behaviour change.
//!
//! Layout is pinned two ways: `#[repr(C)]` + `libc::timeval` (for the
//! platform-correct time struct) make the mirror match the C compiler by
//! construction, and the `hx_htxf_sizeof` / `hx_htxf_offsetof_*` introspection
//! below lets a C test assert `sizeof` + the S0.2 field offsets against the real
//! `struct htxf_conn` at runtime — robust even where `PATH_MAX` differs from a
//! hard-coded constant.

use std::os::raw::{c_char, c_int, c_void};

/// `HOSTLEN` from `compat.h`.
const HOSTLEN: usize = 256;
/// `MAXPATHLEN` from `compat.h`. Note `compat.h` *clamps* it to a fixed 4095 on
/// every platform (`#if MAXPATHLEN > 4095 → 4095`), so this is a hard constant,
/// not the host's `PATH_MAX`. The layout test pins it against the C struct.
const MAXPATHLEN: usize = 4095;
/// `remotename[256]` in the C struct.
const REMOTENAME_LEN: usize = 256;

/// `#[repr(C)]` mirror of `struct qbuf` (`protocol.h`).
#[repr(C)]
struct Qbuf {
    pos: u32,
    len: u32,
    buf: *mut u8,
}

/// `#[repr(C)]` mirror of `struct htxf_conn` (`protocol.h`), field-for-field.
///
/// Named `HtxfHandle` to stay distinct from [`crate::htxf::HtxfConn`], the
/// transport channel this handle's `hx` field points at. Field *names* are
/// internal to this module (C reaches the fields through its own declaration);
/// the *order, types, and sizes* are what must match, pinned by the runtime
/// layout check the C side drives. All fields are POD — a zeroed value is a
/// valid "fresh" handle, matching the old `g_new0`.
#[repr(C)]
pub struct HtxfHandle {
    data_size: u64,
    data_pos: u64,
    rsrc_size: u64,
    rsrc_pos: u64,
    total_size: u64,
    total_pos: u64,
    srv_data_size: u64,
    refcount: c_int,
    canceled: c_int,
    ref_: u32,
    gone: u8,
    type_: u8,
    queue: u32,
    fd: c_int,
    serverhost: [c_char; HOSTLEN],
    serverport: u16,
    htlc: *mut c_void,
    path: [c_char; MAXPATHLEN],
    remotepath: [c_char; MAXPATHLEN],
    remotedir: [c_char; MAXPATHLEN],
    remotename: [c_char; REMOTENAME_LEN],
    remotename_len: u16,
    in_: Qbuf,
    filter_argv: *mut *mut c_char,
    start: libc::timeval,
    /// The C anonymous `struct { guint32 retry:1, ... } opt` — 4 bytes of
    /// bitfields; held as a plain `u32`, the C side owns the bit semantics.
    opt: u32,
    preview: *mut c_void,
    aead_active: c_int,
    hx: *mut c_void,
    abort: *mut c_void,
}

/// Allocate a zeroed transfer handle (replaces C `g_new0 (struct htxf_conn, 1)`).
/// The returned pointer is owned by the caller and must be released with
/// [`hx_htxf_free`]. Never NULL (allocation failure aborts, as `g_new0` did).
#[no_mangle]
pub extern "C" fn hx_htxf_new() -> *mut HtxfHandle {
    // SAFETY: HtxfHandle is POD (integers, byte arrays, and null-valid raw
    // pointers), so all-zero is a valid inhabitant — the exact semantics of the
    // old g_new0 / memset(0).
    let boxed: Box<HtxfHandle> = Box::new(unsafe { std::mem::zeroed() });
    Box::into_raw(boxed)
}

/// Free a handle from [`hx_htxf_new`] (replaces the `g_free (htxf)` tail in
/// `xfers.c::htxf_unref`). NULL-safe.
///
/// # Safety
/// `htxf` must be NULL or a pointer previously returned by `hx_htxf_new` and not
/// yet freed.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_free(htxf: *mut HtxfHandle) {
    if !htxf.is_null() {
        drop(Box::from_raw(htxf));
    }
}

// ---- Layout introspection: lets a C test pin the ABI at runtime ------------

/// `sizeof(struct htxf_conn)` as this module lays it out.
#[no_mangle]
pub extern "C" fn hx_htxf_sizeof() -> usize {
    std::mem::size_of::<HtxfHandle>()
}

/// `_Alignof(struct htxf_conn)` as this module lays it out.
#[no_mangle]
pub extern "C" fn hx_htxf_alignof() -> usize {
    std::mem::align_of::<HtxfHandle>()
}

/// Byte offset of the `refcount` field (an S0.2 lifecycle field).
#[no_mangle]
pub extern "C" fn hx_htxf_offsetof_refcount() -> usize {
    std::mem::offset_of!(HtxfHandle, refcount)
}

/// Byte offset of the `canceled` field (an S0.2 lifecycle field).
#[no_mangle]
pub extern "C" fn hx_htxf_offsetof_canceled() -> usize {
    std::mem::offset_of!(HtxfHandle, canceled)
}

/// Byte offset of the `total_pos` field (an S0.2 lifecycle field).
#[no_mangle]
pub extern "C" fn hx_htxf_offsetof_total_pos() -> usize {
    std::mem::offset_of!(HtxfHandle, total_pos)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn new_returns_zeroed_and_free_is_safe() {
        let p = hx_htxf_new();
        assert!(!p.is_null());
        // Zeroed: spot-check a few fields through the mirror.
        unsafe {
            assert_eq!((*p).refcount, 0);
            assert_eq!((*p).canceled, 0);
            assert_eq!((*p).total_pos, 0);
            assert_eq!((*p).total_size, 0);
            hx_htxf_free(p);
        }
        // NULL-safe.
        unsafe { hx_htxf_free(std::ptr::null_mut()) };
    }

    #[test]
    fn introspection_is_self_consistent() {
        assert!(hx_htxf_sizeof() >= 3 * MAXPATHLEN);
        assert!(hx_htxf_alignof() >= std::mem::align_of::<*mut c_void>());
        // The three S0.2 lifecycle fields sit in the leading, PATH_MAX-independent
        // region of the struct.
        assert!(hx_htxf_offsetof_total_pos() < 64);
        assert!(hx_htxf_offsetof_refcount() < 64);
        assert!(hx_htxf_offsetof_canceled() < 64);
        assert_eq!(
            hx_htxf_offsetof_canceled(),
            hx_htxf_offsetof_refcount() + std::mem::size_of::<c_int>()
        );
    }
}
