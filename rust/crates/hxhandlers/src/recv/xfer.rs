//! File-transfer receive handlers (ported from `rcv.c`).
//!
//! What the session reads of a download's or upload's reply, of Get Info's,
//! of the server moving a queued transfer up, and of the banner's request
//! (`recv::files` matches each reply to what asked). Each applies
//! the dispatch gates and the stamping/error/upload-size *logic* in Rust,
//! and reaches the still-C-owned
//! transfer state only through the narrow `hx_htxf_*` accessor seam
//! (`htxf_accessors.c`) plus genuine collaborators (`xfer_delete`,
//! `gtask_delete_htxf`, the retry timer, `hx_preview_*`, the `resource_len` /
//! `comment_len` / `hx_file_size` fs primitives, and the `gtkhx-core` emits).
//!
//! `struct htxf_conn` is refcounted and touched from both the main thread and
//! the per-transfer worker, so its storage stays C-owned for now; the accessor
//! seam is the same getter/setter step the `gtkhx-core::conn` (`htlc_conn`) migration began
//! with. The file-info reply emits the two raw Hotline date stamps straight
//! through the `file-info` signal — the view (`output_file_info`) formats them —
//! so no date/locale code lives here. Once a transfer's `htxf` is ready to move,
//! everything funnels through the shared [`hx_xfer_announce`] tail.

use std::ffi::{CStr, CString};
use std::os::raw::c_void;

use glib::ffi::g_strdup;
use hxsession::{FileInfo, Transfer};
// c_char / c_int / c_long are only named in the production extern block; the
// test build shadows every symbol in `doubles` and doesn't reference them here.
#[cfg(not(test))]
use std::os::raw::{c_char, c_int, c_long};

#[cfg(not(test))]
use gtkhx_core::conn::hx_conn_serverhost;
#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_file_info, gtkhx_session_emit_xfer_queue, gtkhx_session_get_default,
};

#[cfg(not(test))]
extern "C" {
    // ---- session lookup + transfer kickoff ----
    /// Resolve the `session *` for a connection (`sess_from_htlc` is
    /// static-inline; this is the linkable form).
    fn hx_sess_from_htlc(htlc: *mut c_void) -> *mut c_void;
    /// Start writing the transfer over its HTXF subchannel (`xfers.c`).
    fn xfer_ready_write(htxf: *mut c_void);

    // ---- htxf_accessors.c: the Rust-facing field seam over C-owned htxf ----
    // hx_htxf_in_list moved to the Rust registry (crate::xfer) in Y1; still
    // reached over the C ABI here so this module's test doubles are unchanged.
    fn hx_htxf_in_list(htxf: *mut c_void) -> c_int;
    fn hx_htxf_opt_retry(htxf: *const c_void) -> c_int;
    fn hx_htxf_opt_preview(htxf: *const c_void) -> c_int;
    fn hx_htxf_preview(htxf: *const c_void) -> *mut c_void;
    fn hx_htxf_path(htxf: *const c_void) -> *const c_char;
    fn hx_htxf_data_size(htxf: *const c_void) -> u64;
    fn hx_htxf_set_ref(htxf: *mut c_void, ref_: u32);
    fn hx_htxf_set_total_size(htxf: *mut c_void, total_size: u64);
    fn hx_htxf_set_queue(htxf: *mut c_void, queue: u32);
    fn hx_htxf_set_data_pos(htxf: *mut c_void, data_pos: u64);
    fn hx_htxf_set_rsrc_pos(htxf: *mut c_void, rsrc_pos: u64);
    fn hx_htxf_set_data_size(htxf: *mut c_void, data_size: u64);
    fn hx_htxf_set_rsrc_size(htxf: *mut c_void, rsrc_size: u64);
    fn hx_htxf_set_gone(htxf: *mut c_void, gone: u8);
    fn hx_htxf_set_preview(htxf: *mut c_void, preview: *mut c_void);
    fn hx_htxf_set_serverhost(htxf: *mut c_void, host: *const c_char);
    fn hx_htxf_set_serverport(htxf: *mut c_void, port: u16);
    fn hx_htxf_stamp_start(htxf: *mut c_void);
    /// `stat(2)` the path; data-fork byte size, or -1 on error.
    fn hx_file_size(path: *const c_char) -> i64;

    // ---- genuine collaborators (existing C) ----
    fn xfer_delete(htxf: *mut c_void);
    fn gtask_delete_htxf(sess: *mut c_void, htxf: *mut c_void);
    /// `timer_add_secs(secs, fn, ptr)` — arm a one-shot GLib timer.
    fn timer_add_secs(
        secs: c_long,
        f: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
        ptr: *mut c_void,
    );
    /// The retry callback armed on a download task-error when `opt.retry` is set.
    fn xfer_go_timer(arg: *mut c_void) -> c_int;
    /// Build the preview window (main thread) — returns an `hx_preview *`.
    fn hx_preview_new(name: *const c_char) -> *mut c_void;
    fn hx_preview_set_cancel_cb(
        p: *mut c_void,
        f: Option<unsafe extern "C" fn(*mut c_void)>,
        user_data: *mut c_void,
    );
    fn hx_conn_serverport(htlc: *const c_void) -> u16;
    /// The DOWNLOAD_BANNER reply spins up an HTXF subchannel worker (`banner.c`).
    fn banner_handle_htxf_reply(htlc: *mut c_void, ref_: u32, size: u32);
}

#[cfg(not(test))]
unsafe fn hfs_path(path: *const c_char) -> Option<std::path::PathBuf> {
    if path.is_null() {
        return None;
    }
    let bytes = std::ffi::CStr::from_ptr(path).to_bytes();
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        Some(std::path::PathBuf::from(std::ffi::OsStr::from_bytes(bytes)))
    }
    #[cfg(not(unix))]
    {
        std::str::from_utf8(bytes)
            .ok()
            .map(std::path::PathBuf::from)
    }
}

#[cfg(not(test))]
unsafe fn resource_len(path: *const c_char) -> usize {
    hfs_path(path).map_or(0, |path| {
        hxnet::hfs_config::resource_len(&path).min(usize::MAX as u64) as usize
    })
}

#[cfg(not(test))]
unsafe fn comment_len(path: *const c_char) -> usize {
    hfs_path(path).map_or(0, |path| hxnet::hfs_config::comment_len(&path))
}

/// Adapter matching `hx_preview_cancel_fn (void (*)(void *))`: closing the
/// preview window mid-download cancels the transfer. Registered on the preview
/// with the `htxf` as user-data.
///
/// # Safety
/// `user_data` is the transfer's `struct htxf_conn *`.
unsafe extern "C" fn preview_cancel_xfer(user_data: *mut c_void) {
    xfer_delete(user_data);
}

/// `void hx_xfer_announce (htlc, htxf, queue)` — the shared file-transfer reply
/// tail. Always emits `xfer-queue` so the tasks view shows the transfer's
/// position; then, when `queue` is 0 (cleared to transfer), kicks off the byte
/// stream with `xfer_ready_write`. A non-zero `queue` parks the transfer until a
/// later unsolicited `HTLS_HDR_QUEUE` update (also routed through here) reaches 0.
///
/// # Safety
/// `htlc` / `htxf` are the opaque connection / transfer handles the C side owns;
/// `htxf` must be a live transfer.
#[no_mangle]
pub unsafe extern "C" fn hx_xfer_announce(htlc: *mut c_void, htxf: *mut c_void, queue: u32) {
    gtkhx_session_emit_xfer_queue(gtkhx_session_get_default(), hx_sess_from_htlc(htlc), htxf);
    if queue == 0 {
        xfer_ready_write(htxf);
    }
}

// ---- receive handlers --------------------------------------------------------

/// Stamp the HTXF subchannel target onto `htxf`: the worker hands (host, port+1)
/// straight to the connect without re-resolving. Also stamps the transfer start
/// time for the progress/ETA readout.
unsafe fn stamp_subchannel(htlc: *mut c_void, htxf: *mut c_void) {
    hx_htxf_stamp_start(htxf);
    hx_htxf_set_serverhost(htxf, hx_conn_serverhost(htlc.cast()));
    hx_htxf_set_serverport(htxf, hx_conn_serverport(htlc).wrapping_add(1));
}

/// A download's reply, file or folder, for the transfer it asked for: stamp
/// it, build the preview window when it is one, and let it go. A reply with
/// no reference, or a file's with no size, starts nothing.
///
/// # Safety
/// Main thread; `htlc` is a live connection; `htxf` was a transfer when the
/// request went, and is read only if it still is.
pub(crate) unsafe fn download_ready(
    htlc: *mut c_void,
    htxf: *mut c_void,
    folder: bool,
    t: &Transfer,
) {
    if hx_htxf_in_list(htxf) == 0 || t.reference == 0 || (!folder && t.size == 0) {
        return;
    }
    // A folder is legal at size 0; the progress bar reads better at 1.
    hx_htxf_set_ref(htxf, t.reference);
    hx_htxf_set_total_size(htxf, t.size.max(1));
    hx_htxf_set_queue(htxf, t.queue);
    if folder {
        // How the download knows it's done without waiting on a server that
        // closes late; see hxnet_htxf_set_folder_items.
        crate::xfer::remember_folder_items(htxf, t.items);
    }
    stamp_subchannel(htlc, htxf);

    // Build the preview window on the main thread (we are on it); the download
    // worker then feeds bytes via htxf->preview without touching GTK. Built
    // before the announce tail because that starts the download when unqueued.
    if hx_htxf_opt_preview(htxf) != 0 && hx_htxf_preview(htxf).is_null() {
        let path = hx_htxf_path(htxf);
        // Titled with the file's name: the local path's last component.
        let title = if path.is_null() {
            path
        } else {
            let bytes = std::ffi::CStr::from_ptr(path).to_bytes();
            path.add(hxmodel::files::basename_offset(bytes, b'/'))
        };
        let pv = hx_preview_new(title);
        hx_htxf_set_preview(htxf, pv);
        hx_preview_set_cancel_cb(pv, Some(preview_cancel_xfer), htxf);
    }

    hx_xfer_announce(htlc, htxf, t.queue);
}

/// A download the server refused: try again in a second when the transfer
/// asks for retries, else drop it.
///
/// # Safety
/// As [`download_ready`].
pub(crate) unsafe fn download_refused(htlc: *mut c_void, htxf: *mut c_void) {
    if hx_htxf_in_list(htxf) == 0 {
        return;
    }
    if hx_htxf_opt_retry(htxf) != 0 {
        hx_htxf_set_gone(htxf, 0);
        timer_add_secs(1, Some(xfer_go_timer), htxf);
    } else {
        gtask_delete_htxf(hx_sess_from_htlc(htlc), htxf);
        xfer_delete(htxf);
    }
}

/// An upload's reply, file or folder: size a file's upload from what is on
/// disk and where the server says it resumes, stamp it, and let it go. A
/// reply with no reference starts nothing.
///
/// # Safety
/// As [`download_ready`].
pub(crate) unsafe fn upload_ready(
    htlc: *mut c_void,
    htxf: *mut c_void,
    folder: bool,
    t: &Transfer,
) {
    if hx_htxf_in_list(htxf) == 0 || t.reference == 0 {
        return;
    }
    hx_htxf_set_queue(htxf, t.queue);
    if !folder {
        let data_pos = u64::from(t.data_from);
        let rsrc_pos = u64::from(t.rsrc_from);
        hx_htxf_set_data_pos(htxf, data_pos);
        hx_htxf_set_rsrc_pos(htxf, rsrc_pos);

        // The path stays a C string, passed straight to the fs primitives.
        let path = hx_htxf_path(htxf);
        let mut data_size = hx_htxf_data_size(htxf);
        let sz = hx_file_size(path);
        if sz >= 0 {
            data_size = sz as u64;
            hx_htxf_set_data_size(htxf, data_size);
        }
        let rsrc_size = resource_len(path) as u64;
        hx_htxf_set_rsrc_size(htxf, rsrc_size);

        // Wrapping, as the C guint64 arithmetic was: a resume past the end
        // must not panic.
        let total = 133u64
            + if rsrc_size.wrapping_sub(rsrc_pos) != 0 {
                16
            } else {
                0
            }
            + comment_len(path) as u64
            + data_size.wrapping_sub(data_pos)
            + rsrc_size.wrapping_sub(rsrc_pos);
        hx_htxf_set_total_size(htxf, total);
    }
    hx_htxf_set_ref(htxf, t.reference);
    stamp_subchannel(htlc, htxf);
    hx_xfer_announce(htlc, htxf, t.queue);
}

/// An upload the server refused: drop it.
///
/// # Safety
/// As [`download_ready`].
pub(crate) unsafe fn upload_refused(htlc: *mut c_void, htxf: *mut c_void) {
    if hx_htxf_in_list(htxf) == 0 {
        return;
    }
    gtask_delete_htxf(hx_sess_from_htlc(htlc), htxf);
    xfer_delete(htxf);
}

/// A queued transfer moved up the server's queue; at 0 it goes.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn queued(htlc: *mut c_void, reference: u32, queue: u32) {
    let htxf = crate::xfer::htxf_with_ref(htlc, reference);
    if htxf.is_null() {
        glib::g_warning!(
            "gtkhx",
            "queue position {queue} for transfer {reference}, which is not one of ours"
        );
        return;
    }
    hx_htxf_set_queue(htxf.cast(), queue);
    hx_xfer_announce(htlc, htxf.cast(), queue);
}

/// The banner's reply: the transfer that fetches it. A size past what 32
/// bits say is too large for a banner, and is refused as one.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn banner(htlc: *mut c_void, t: &Transfer) {
    let size = u32::try_from(t.size).unwrap_or(u32::MAX);
    banner_handle_htxf_reply(htlc, t.reference, size);
}

/// Get Info's reply: open the dialog for the file `label` names (its folder
/// and name, as the request named it). The two dates go as the server sent
/// them; the dialog formats them.
///
/// # Safety
/// Main thread; `htlc` is a live connection. The emit is synchronous, so
/// the strings outlive the view handler.
pub(crate) unsafe fn file_info(htlc: *mut c_void, label: &CStr, f: &FileInfo) {
    let text = |s: &str| CString::new(s.replace('\0', "")).unwrap_or_default();
    let (name, kind, creator, comment) = (
        text(&f.name),
        text(&f.kind),
        text(&f.creator),
        text(&f.comment),
    );
    gtkhx_session_emit_file_info(
        gtkhx_session_get_default(),
        htlc,
        // The dialog takes the label over, and frees it.
        g_strdup(label.as_ptr()),
        name.as_ptr(),
        creator.as_ptr(),
        kind.as_ptr(),
        comment.as_ptr(),
        f.modified.as_ptr(),
        f.created.as_ptr(),
        f.size,
    );
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
mod doubles;
#[cfg(test)]
pub(crate) use doubles::test_env;
#[cfg(test)]
use doubles::*;

#[cfg(test)]
mod tests;
