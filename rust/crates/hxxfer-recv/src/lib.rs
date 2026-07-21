//! File-transfer reply tail (ported from `rcv.c`).
//!
//! Every file-transfer reply — the four task-replies (`rcv_task_file_get` /
//! `_folder_get` / `_file_put` / `_folder_put`) and the unsolicited
//! `hx_rcv_xfer_queue` — ends the same way once it has stamped the transfer's
//! ref / size / queue position onto its `htxf`: announce the queue position to
//! the view, and — if the server didn't queue us — start moving bytes. This
//! crate owns that shared tail so the five handlers stop repeating it.
//!
//! The C handlers keep everything transfer-specific (the `htxf` lookup + state
//! stamping, the per-reply wire parse, the error/retry handling); they call in
//! only once their `htxf` is ready to go.

use std::os::raw::c_void;

#[cfg(not(test))]
extern "C" {
    /// The singleton `GtkhxSession` GObject (gtkhx-session).
    fn gtkhx_session_get_default() -> *mut c_void;
    /// Fire `GtkhxSession::xfer-queue (session*, htxf*)` (gtkhx-session) — the
    /// view reads the queue position off `htxf`.
    fn gtkhx_session_emit_xfer_queue(self_: *mut c_void, sess: *mut c_void, htxf: *mut c_void);
    /// Resolve the `session *` for a connection (tasks_bridge.c; `sess_from_htlc`
    /// is static-inline, so this wrapper is the linkable form).
    fn hx_sess_from_htlc(htlc: *mut c_void) -> *mut c_void;
    /// Start writing the transfer over its HTXF subchannel (xfers.c).
    fn xfer_ready_write(htxf: *mut c_void);
}

/// `void hx_xfer_announce (htlc, htxf, queue)` — the shared file-transfer reply
/// tail. Always emits the `xfer-queue` signal so the tasks view shows the
/// transfer's position; then, when `queue` is 0 (the server put us at the head
/// of the queue — i.e. cleared to transfer), kicks off the byte stream with
/// `xfer_ready_write`. A non-zero `queue` leaves the transfer parked; a later
/// unsolicited `HTLS_HDR_QUEUE` update (also routed through here) starts it once
/// the position reaches 0.
///
/// The caller passes `htxf->queue` it just stamped; keeping it a scalar arg
/// avoids this crate needing to reach into the opaque `htxf`.
///
/// # Safety
/// `htlc` / `htxf` are the opaque connection / transfer handles the C side owns;
/// `htxf` must be a live transfer (the caller has just populated it).
#[no_mangle]
pub unsafe extern "C" fn hx_xfer_announce(htlc: *mut c_void, htxf: *mut c_void, queue: u32) {
    gtkhx_session_emit_xfer_queue(gtkhx_session_get_default(), hx_sess_from_htlc(htlc), htxf);
    if queue == 0 {
        xfer_ready_write(htxf);
    }
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::Cell;

    thread_local! {
        /// The `htxf` pointer of the last emitted xfer-queue signal, or None.
        pub static EMITTED: Cell<Option<*mut std::os::raw::c_void>> = const { Cell::new(None) };
        /// The `htxf` pointer passed to the last xfer_ready_write, or None.
        pub static STARTED: Cell<Option<*mut std::os::raw::c_void>> = const { Cell::new(None) };
    }

    pub fn reset() {
        EMITTED.with(|c| c.set(None));
        STARTED.with(|c| c.set(None));
    }
}

#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
unsafe fn hx_sess_from_htlc(_htlc: *mut c_void) -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_xfer_queue(_self_: *mut c_void, _sess: *mut c_void, htxf: *mut c_void) {
    test_env::EMITTED.with(|c| c.set(Some(htxf)));
}

#[cfg(test)]
unsafe fn xfer_ready_write(htxf: *mut c_void) {
    test_env::STARTED.with(|c| c.set(Some(htxf)));
}

#[cfg(test)]
mod tests;
