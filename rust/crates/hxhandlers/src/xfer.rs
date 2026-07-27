//! The file-transfer **registry** — the `xfers[]` list (ported from `xfers.c`),
//! Y1 of the xfers.c → Rust migration (`docs/rust/xfers-to-rust-scoping.md`).
//!
//! The transfer byte-loops (`hxnet::xfer`) and the `struct htxf_conn` storage +
//! refcount/cancel lifecycle (`hxnet::xfer_handle`) are already Rust; this owns
//! the main-thread list of live transfers and the reorder / lookup / removal /
//! shutdown-cancel / tasks-broadcast operations over it.
//!
//! The list is a `thread_local` `Vec<*mut HtxfHandle>` — it is only ever touched
//! on the GTK main thread (workers touch only their own htxf's atomic fields,
//! never the list), so no locking is needed. A slot holds one refcount ref,
//! dropped via `hx_htxf_unref` when the transfer is unlinked. Fields are reached
//! natively through `hxnet`'s `HtxfHandle` (Y1 `pub`s the plain ones the shell
//! needs) rather than the `hx_htxf_*` C-ABI accessor bounce.

use std::cell::RefCell;
use std::os::raw::{c_int, c_void};

use hxnet::htxf::{hxnet_htxf_abort, HtxfAbort};
use hxnet::xfer_handle::{hx_htxf_cancel, hx_htxf_unref, HtxfHandle};

// C environment resolved at the final link; #[cfg(test)] doubles below let
// `cargo test -p hxhandlers` run headless (same shape recv/files.rs uses).
#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_file_update, gtkhx_session_emit_xfer_destroyed, gtkhx_session_get_default,
};

#[cfg(not(test))]
extern "C" {
    /// The session owning this htlc (tasks_bridge.c).
    fn hx_sess_from_htlc(htlc: *mut c_void) -> *mut c_void;
    /// Kick a queued transfer's wire request (xfers.c — still C until Y4). Called
    /// on the head of the list after a removal so the next queued transfer runs.
    fn xfer_go(htxf: *mut HtxfHandle);
}

thread_local! {
    static XFERS: RefCell<Vec<*mut HtxfHandle>> = const { RefCell::new(Vec::new()) };
}

fn with_list<R>(f: impl FnOnce(&mut Vec<*mut HtxfHandle>) -> R) -> R {
    XFERS.with(|x| f(&mut x.borrow_mut()))
}

/// `int hx_htxf_in_list(struct htxf_conn *htxf)` — is the transfer still in the
/// list (was htxf_accessors.c's `xfers[]` scan). Only the receive handlers call
/// it (to drop a reply for a since-cancelled transfer); kept on the C ABI so
/// recv/xfer.rs's test doubles stay unchanged.
///
/// # Safety
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_htxf_in_list(htxf: *mut c_void) -> c_int {
    with_list(|xs| xs.iter().any(|&e| e as *mut c_void == htxf)) as c_int
}

/// `void xfer_registry_add(struct htxf_conn *htxf)` — append a freshly-built
/// transfer to the list (was `xfer_init`'s `xfers[nxfers++] = htxf`). The slot's
/// refcount ref is seeded by the caller (`hx_htxf_new` → 1) before this.
///
/// # Safety
/// `htxf` is a live handle. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_registry_add(htxf: *mut HtxfHandle) {
    with_list(|xs| xs.push(htxf));
}

/// `int xfer_count(void)` — number of live transfers (was `nxfers`). `xfer_new`
/// reads it to decide whether the just-added transfer is the only one (start it
/// immediately) or should queue.
///
/// # Safety
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_count() -> c_int {
    with_list(|xs| xs.len() as c_int)
}

/// `void xfer_up(int num)` — swap slot `num` with the one above it (tasks-window
/// move-up). Caller guarantees `num >= 1`.
///
/// # Safety
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_up(num: c_int) {
    with_list(|xs| {
        let n = num as usize;
        if n >= 1 && n < xs.len() {
            xs.swap(n - 1, n);
        }
    });
}

/// `int xfer_down(int num)` — swap slot `num` with the one below it; returns 1 if
/// already at the bottom (nothing to do), else 0.
///
/// # Safety
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_down(num: c_int) -> c_int {
    with_list(|xs| {
        let n = num as usize;
        if n + 1 >= xs.len() {
            return 1;
        }
        xs.swap(n, n + 1);
        0
    })
}

/// `int xfer_num(struct htxf_conn *htxf)` — the transfer's index in the list, or
/// -1 if absent.
///
/// # Safety
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_num(htxf: *mut HtxfHandle) -> c_int {
    with_list(|xs| xs.iter().position(|&e| e == htxf).map_or(-1, |i| i as c_int))
}

/// `struct htxf_conn *htxf_with_ref(guint32 ref)` — find a transfer by its
/// server XFER ref (used by the unsolicited HTLS_HDR_QUEUE update), or NULL.
///
/// # Safety
/// Main thread only; the list holds live handles.
#[no_mangle]
pub unsafe extern "C" fn htxf_with_ref(ref_: u32) -> *mut HtxfHandle {
    with_list(|xs| {
        xs.iter()
            .copied()
            .find(|&e| (*e).ref_ == ref_)
            .unwrap_or(std::ptr::null_mut())
    })
}

/// `void xfer_remove_from_list(struct htxf_conn *htxf)` — unlink `htxf`, emit
/// `xfer-destroyed` (so subscribers null cached pointers), drop the list ref, and
/// kick the next queued transfer. Idempotent. Emits BEFORE the unref, so handlers
/// run with the htxf still alive (the unref drops only the list's ref). Kept
/// `#[no_mangle]` because the still-C completion cleanup dispatcher (Y2) calls it.
///
/// # Safety
/// `htxf` is a live handle or absent. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_remove_from_list(htxf: *mut HtxfHandle) {
    let removed = with_list(|xs| match xs.iter().position(|&e| e == htxf) {
        Some(i) => {
            xs.remove(i);
            true
        }
        None => false,
    });
    if !removed {
        return;
    }

    gtkhx_session_emit_xfer_destroyed(
        gtkhx_session_get_default(),
        hx_sess_from_htlc((*htxf).htlc),
        htxf as *mut c_void,
    );
    hx_htxf_unref(htxf); // drop the list's ref

    // Auto-start the next queued transfer (the old `if (nxfers) xfer_go(xfers[0])`).
    if let Some(head) = with_list(|xs| xs.first().copied()) {
        xfer_go(head);
    }
}

/// `void xfer_delete(struct htxf_conn *htxf)` — public cancel (server cancel /
/// tasks-window Cancel): latch the cancel flag, shut the subchannel socket to
/// wake a parked worker, and unlink from the list.
///
/// # Safety
/// `htxf` is NULL or a live handle. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_delete(htxf: *mut HtxfHandle) {
    if htxf.is_null() {
        return;
    }
    hx_htxf_cancel(htxf);
    hxnet_htxf_abort((*htxf).abort as *const HtxfAbort);
    xfer_remove_from_list(htxf);
}

/// `void xfers_delete_all(void)` — best-effort cancel of every in-flight transfer
/// at app shutdown: latch cancel + abort each socket + drop the list ref, then
/// clear. The worker's own ref keeps the htxf alive until it exits; the process
/// is going down, so a still-running worker leak doesn't matter.
///
/// # Safety
/// Main thread only (shutdown path).
#[no_mangle]
pub unsafe extern "C" fn xfers_delete_all() {
    let all: Vec<*mut HtxfHandle> = with_list(|xs| std::mem::take(xs));
    for htxf in all {
        hx_htxf_cancel(htxf);
        hxnet_htxf_abort((*htxf).abort as *const HtxfAbort);
        hx_htxf_unref(htxf); // drop the list's ref
    }
}

/// `void xfer_tasks_update(struct htlc_conn *htlc)` — re-emit `file-update` for
/// every transfer on `htlc` (called when a session becomes current, so the tasks
/// window repaints its rows).
///
/// # Safety
/// `htlc` is a live session pointer. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_tasks_update(htlc: *mut c_void) {
    let sess = hx_sess_from_htlc(htlc);
    let matching: Vec<*mut HtxfHandle> =
        with_list(|xs| xs.iter().copied().filter(|&e| (*e).htlc == htlc).collect());
    for htxf in matching {
        gtkhx_session_emit_file_update(gtkhx_session_get_default(), sess, htxf as *mut c_void);
    }
}

// ---- test doubles for the C / GObject-emit environment ----------------------

#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}
#[cfg(test)]
unsafe fn gtkhx_session_emit_file_update(_s: *mut c_void, _sess: *mut c_void, _h: *mut c_void) {}
#[cfg(test)]
unsafe fn gtkhx_session_emit_xfer_destroyed(_s: *mut c_void, _sess: *mut c_void, _h: *mut c_void) {}
#[cfg(test)]
unsafe fn hx_sess_from_htlc(_htlc: *mut c_void) -> *mut c_void {
    std::ptr::null_mut()
}
#[cfg(test)]
unsafe fn xfer_go(_htxf: *mut HtxfHandle) {}

#[cfg(test)]
mod tests {
    use super::*;

    // Dummy, never-dereferenced handles for the pure reorder/index logic (up /
    // down / num operate on pointer identity only — no field reads / externs).
    fn dummy(n: usize) -> *mut HtxfHandle {
        (0x1000 + n) as *mut HtxfHandle
    }

    fn reset(handles: &[*mut HtxfHandle]) {
        XFERS.with(|x| *x.borrow_mut() = handles.to_vec());
    }

    #[test]
    fn add_and_num() {
        reset(&[]);
        let (a, b, c) = (dummy(1), dummy(2), dummy(3));
        unsafe {
            xfer_registry_add(a);
            xfer_registry_add(b);
            xfer_registry_add(c);
            assert_eq!(xfer_num(a), 0);
            assert_eq!(xfer_num(b), 1);
            assert_eq!(xfer_num(c), 2);
            assert_eq!(xfer_num(dummy(9)), -1);
        }
    }

    #[test]
    fn up_swaps_with_previous() {
        let (a, b, c) = (dummy(1), dummy(2), dummy(3));
        reset(&[a, b, c]);
        unsafe { xfer_up(2) }; // move c up one
        assert_eq!(unsafe { xfer_num(c) }, 1);
        assert_eq!(unsafe { xfer_num(b) }, 2);
        // num 0 is a no-op (nothing above).
        unsafe { xfer_up(0) };
        assert_eq!(unsafe { xfer_num(a) }, 0);
    }

    #[test]
    fn down_swaps_and_reports_bottom() {
        let (a, b, c) = (dummy(1), dummy(2), dummy(3));
        reset(&[a, b, c]);
        assert_eq!(unsafe { xfer_down(0) }, 0); // a moves down
        assert_eq!(unsafe { xfer_num(a) }, 1);
        assert_eq!(unsafe { xfer_down(2) }, 1); // already at bottom
    }

    #[test]
    fn in_list_tracks_membership() {
        let a = dummy(1);
        reset(&[a, dummy(2)]);
        unsafe {
            assert_eq!(hx_htxf_in_list(a as *mut c_void), 1);
            assert_eq!(hx_htxf_in_list(dummy(9) as *mut c_void), 0);
        }
    }
}
