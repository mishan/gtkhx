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
use std::sync::atomic::{AtomicI32, AtomicU64, Ordering};

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
///
/// The three cross-thread lifecycle fields (`refcount`, `canceled`, `total_pos`)
/// are held as atomics (S0.2). `AtomicI32` / `AtomicU64` are layout-identical to
/// the C `gint` / `guint64` they mirror, so offsets are unchanged; the C side no
/// longer touches them directly — all access goes through the `hx_htxf_*`
/// ref/cancel/total_pos ABI below (a faithful port of the old `g_atomic_int_*`).
#[repr(C)]
pub struct HtxfHandle {
    data_size: u64,
    data_pos: u64,
    rsrc_size: u64,
    rsrc_pos: u64,
    total_size: u64,
    total_pos: AtomicU64,
    srv_data_size: u64,
    refcount: AtomicI32,
    canceled: AtomicI32,
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

// ---- Cross-thread lifecycle (S0.2) -----------------------------------------
// The refcount / cancel flag / total_pos byte counter are touched by both the
// main thread and the tokio transfer worker. These are a 1:1 port of the old
// C `g_atomic_int_*` calls (SeqCst = the full-barrier semantics g_atomic gave);
// the C side no longer accesses the fields directly. All take a live handle.

/// Take a reference (`g_atomic_int_inc`). Returns the new count. Also used to
/// seed the initial ref at `xfer_init` (0 → 1).
///
/// # Safety
/// `p` is a live handle from `hx_htxf_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_ref(p: *mut HtxfHandle) -> c_int {
    (*p).refcount.fetch_add(1, Ordering::SeqCst) + 1
}

/// Drop a reference (`g_atomic_int_dec_and_test`). Returns nonzero iff this was
/// the last ref (the count reached 0), so the caller runs the teardown + frees.
///
/// # Safety
/// `p` is a live handle from `hx_htxf_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_unref(p: *mut HtxfHandle) -> c_int {
    // fetch_sub returns the previous value; previous == 1 means we hit 0.
    ((*p).refcount.fetch_sub(1, Ordering::SeqCst) == 1) as c_int
}

/// Mark the transfer cancelled (`g_atomic_int_set(&canceled, TRUE)`).
///
/// # Safety
/// `p` is a live handle from `hx_htxf_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_cancel(p: *mut HtxfHandle) {
    (*p).canceled.store(1, Ordering::SeqCst);
}

/// Read the cancel flag (`g_atomic_int_get(&canceled)`) — the worker's
/// cooperative-cancel boundary and the dispatchers' skip check.
///
/// # Safety
/// `p` is a live handle from `hx_htxf_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_is_canceled(p: *const HtxfHandle) -> c_int {
    (*p).canceled.load(Ordering::SeqCst)
}

/// Bump the transferred-bytes counter by `delta` (the worker progress hook's
/// `htxf->total_pos += delta`).
///
/// # Safety
/// `p` is a live handle from `hx_htxf_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_add_total_pos(p: *mut HtxfHandle, delta: u64) {
    (*p).total_pos.fetch_add(delta, Ordering::SeqCst);
}

/// Set the transferred-bytes counter (init to 0 / clamp to total_size at
/// completion).
///
/// # Safety
/// `p` is a live handle from `hx_htxf_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_set_total_pos(p: *mut HtxfHandle, val: u64) {
    (*p).total_pos.store(val, Ordering::SeqCst);
}

/// Read the transferred-bytes counter (the tasks / files-browser progress view).
///
/// # Safety
/// `p` is a live handle from `hx_htxf_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_total_pos(p: *const HtxfHandle) -> u64 {
    (*p).total_pos.load(Ordering::SeqCst)
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
            assert_eq!((*p).refcount.load(Ordering::SeqCst), 0);
            assert_eq!((*p).canceled.load(Ordering::SeqCst), 0);
            assert_eq!((*p).total_pos.load(Ordering::SeqCst), 0);
            assert_eq!((*p).total_size, 0);
            hx_htxf_free(p);
        }
        // NULL-safe.
        unsafe { hx_htxf_free(std::ptr::null_mut()) };
    }

    #[test]
    fn ref_unref_cancel_total_pos_transitions() {
        unsafe {
            let p = hx_htxf_new();
            // Refcount: inc returns the new count; unref reports was-last only at 0.
            assert_eq!(hx_htxf_ref(p), 1); // 0 → 1 (initial ref)
            assert_eq!(hx_htxf_ref(p), 2); // 1 → 2
            assert_eq!(hx_htxf_unref(p), 0); // 2 → 1, not last
            assert_eq!(hx_htxf_unref(p), 1); // 1 → 0, last ref

            // Cancel flag: starts clear, sticks once set.
            assert_eq!(hx_htxf_is_canceled(p), 0);
            hx_htxf_cancel(p);
            assert_ne!(hx_htxf_is_canceled(p), 0);

            // total_pos: additive bump + absolute set.
            assert_eq!(hx_htxf_total_pos(p), 0);
            hx_htxf_add_total_pos(p, 100);
            hx_htxf_add_total_pos(p, 40);
            assert_eq!(hx_htxf_total_pos(p), 140);
            hx_htxf_set_total_pos(p, 4096);
            assert_eq!(hx_htxf_total_pos(p), 4096);

            hx_htxf_free(p);
        }
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
