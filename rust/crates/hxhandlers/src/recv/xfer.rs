//! File-transfer receive handlers (ported from `rcv.c`).
//!
//! The five transfer task-replies (`rcv_task_file_get` / `_folder_get` /
//! `_file_put` / `_folder_put` / `_banner_get`), the file-info reply
//! (`rcv_task_file_getinfo`), and the unsolicited `hx_rcv_xfer_queue` live here.
//! Each one parses its reply natively (`hotline_proto::parse::*` — no
//! `gtkhx_proto_*` FFI round-trip), applies the pure dispatch gates and the
//! stamping/error/upload-size *logic* in Rust, and reaches the still-C-owned
//! transfer state only through the narrow `hx_htxf_*` accessor seam
//! (`htxf_accessors.c`) plus genuine collaborators (`xfer_delete`,
//! `gtask_delete_htxf`, the retry timer, `hx_preview_*`, the `resource_len` /
//! `comment_len` / `hx_file_size` fs primitives, and the `gtkhx-session` emits).
//!
//! `struct htxf_conn` is refcounted and touched from both the main thread and
//! the per-transfer worker, so its storage stays C-owned for now; the accessor
//! seam is the same getter/setter step the `hxconn` (`htlc_conn`) migration began
//! with. The file-info reply emits the two raw Hotline date stamps straight
//! through the `file-info` signal — the view (`output_file_info`) formats them —
//! so no date/locale code lives here. Once a transfer's `htxf` is ready to move,
//! everything funnels through the shared [`hx_xfer_announce`] tail.

use std::os::raw::{c_char, c_void};
// c_int / c_long are only named in the production extern block; the test build
// shadows every symbol in `doubles` and doesn't reference them here.
#[cfg(not(test))]
use std::os::raw::{c_int, c_long};

#[cfg(not(test))]
use gtkhx_session::{gtkhx_session_emit_file_info, gtkhx_session_emit_xfer_queue, gtkhx_session_get_default};
#[cfg(not(test))]
use hxconn::hx_conn_serverhost;
#[cfg(not(test))]
use hxhfs::ffi::{comment_len, resource_len};

#[cfg(not(test))]
extern "C" {
    // ---- session lookup + transfer kickoff ----
    /// Resolve the `session *` for a connection (`sess_from_htlc` is
    /// static-inline; this is the linkable form).
    fn hx_sess_from_htlc(htlc: *mut c_void) -> *mut c_void;
    /// Start writing the transfer over its HTXF subchannel (`xfers.c`).
    fn xfer_ready_write(htxf: *mut c_void);

    // ---- htxf_accessors.c: the Rust-facing field seam over C-owned htxf ----
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
    fn timer_add_secs(secs: c_long, f: Option<unsafe extern "C" fn(*mut c_void) -> c_int>, ptr: *mut c_void);
    /// The retry callback armed on a download task-error when `opt.retry` is set.
    fn xfer_go_timer(arg: *mut c_void) -> c_int;
    /// Build the preview window (main thread) — returns an `hx_preview *`.
    fn hx_preview_new(name: *const c_char) -> *mut c_void;
    fn hx_preview_set_cancel_cb(
        p: *mut c_void,
        f: Option<unsafe extern "C" fn(*mut c_void)>,
        user_data: *mut c_void,
    );
    /// Basename within `path` (a pointer *into* `path`; not freed).
    fn dirchar_basename(path: *mut c_char) -> *mut c_char;
    fn hx_conn_serverport(htlc: *const c_void) -> u16;
    /// The DOWNLOAD_BANNER reply spins up an HTXF subchannel worker (`banner.c`).
    fn banner_handle_htxf_reply(htlc: *mut c_void, ref_: u32, size: u32);
    /// GLib `g_free` — release the FILE_GETINFO path task label on the error path
    /// (it's `g_strdup`'d, with no task `ptr_free`).
    fn g_free(ptr: *mut c_void);
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

// ---- receive handlers (rcv_task_* callbacks) -------------------------------

/// True when the reply frame's task-error bit is set — the native equivalent of
/// the C `task_inerror()` (`hotline_proto` header parse + `flag & 1`). A frame
/// too short to hold a header is treated as not-in-error, matching the C shim.
unsafe fn task_in_error(frame: *const c_void, frame_len: usize) -> bool {
    if frame.is_null() {
        return false;
    }
    let s = std::slice::from_raw_parts(frame as *const u8, frame_len);
    hotline_proto::parse::Header::parse(s).is_some_and(|h| h.in_error())
}

/// Borrow the reply frame as a byte slice (empty on a NULL frame).
unsafe fn frame_slice<'a>(frame: *const c_void, frame_len: usize) -> &'a [u8] {
    if frame.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(frame as *const u8, frame_len)
    }
}

/// Stamp the HTXF subchannel target onto `htxf`: the worker hands (host, port+1)
/// straight to the connect without re-resolving. Also stamps the transfer start
/// time for the progress/ETA readout.
unsafe fn stamp_subchannel(htlc: *mut c_void, htxf: *mut c_void) {
    hx_htxf_stamp_start(htxf);
    hx_htxf_set_serverhost(htxf, hx_conn_serverhost(htlc.cast()));
    hx_htxf_set_serverport(htxf, hx_conn_serverport(htlc).wrapping_add(1));
}

/// Task-error policy for a *download* (file_get / folder_get): re-arm the 1 s
/// retry timer when `htxf->opt.retry` is set, else drop the transfer.
unsafe fn xfer_download_error(htlc: *mut c_void, htxf: *mut c_void) {
    if hx_htxf_opt_retry(htxf) != 0 {
        hx_htxf_set_gone(htxf, 0);
        timer_add_secs(1, Some(xfer_go_timer), htxf);
    } else {
        gtask_delete_htxf(hx_sess_from_htlc(htlc), htxf);
        xfer_delete(htxf);
    }
}

/// `void rcv_task_file_get (htlc, frame, frame_len, htxf, data)` — HTLS reply to
/// HTLC_HDR_FILE_GET (was `rcv.c`). Drops the reply if the transfer was already
/// cancelled (`hx_htxf_in_list`); on a task error, retries or deletes; otherwise
/// parses natively, applies the `(!size && !size64_seen) || !ref` malformed-frame
/// gate, stamps the transfer, builds the preview window when `opt.preview` is
/// set, and runs the announce tail.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `ptr` is the transfer's `struct htxf_conn *`.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_file_get(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    ptr: *mut c_void,
    _data: *mut c_void,
) {
    let htxf = ptr;
    if hx_htxf_in_list(htxf) == 0 {
        return;
    }
    if task_in_error(frame, frame_len) {
        xfer_download_error(htlc, htxf);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_file_get_reply(s, s.len());
    if (r.size == 0 && !r.size64_seen) || r.ref_ == 0 {
        return;
    }
    let total = if r.size64_seen { r.size64 } else { r.size as u64 };
    hx_htxf_set_ref(htxf, r.ref_);
    hx_htxf_set_total_size(htxf, total);
    hx_htxf_set_queue(htxf, r.queue);
    stamp_subchannel(htlc, htxf);

    // Build the preview window on the main thread (we are on it); the download
    // worker then feeds bytes via htxf->preview without touching GTK. Built
    // before the announce tail because that starts the download when unqueued.
    if hx_htxf_opt_preview(htxf) != 0 && hx_htxf_preview(htxf).is_null() {
        let path = hx_htxf_path(htxf);
        let name = dirchar_basename(path as *mut c_char);
        let title = if name.is_null() { path } else { name as *const c_char };
        let pv = hx_preview_new(title);
        hx_htxf_set_preview(htxf, pv);
        hx_preview_set_cancel_cb(pv, Some(preview_cancel_xfer), htxf);
    }

    hx_xfer_announce(htlc, htxf, r.queue);
}

/// `void rcv_task_folder_get (htlc, frame, frame_len, htxf, data)` — HTLS reply
/// to HTLC_HDR_FILE_GETFOLDER (was `rcv.c`). Mirror of [`rcv_task_file_get`]:
/// same cancellation + error handling, but no preview, the only gate is `!ref`
/// (folders are legal at total_size 0), and the total is clamped to 1 for the
/// progress UI when the server reports 0. `FILE_NFILES` is parsed but currently
/// informational (the C handler ignored it too).
///
/// # Safety
/// See [`rcv_task_file_get`].
#[no_mangle]
pub unsafe extern "C" fn rcv_task_folder_get(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    ptr: *mut c_void,
    _data: *mut c_void,
) {
    let htxf = ptr;
    if hx_htxf_in_list(htxf) == 0 {
        return;
    }
    if task_in_error(frame, frame_len) {
        xfer_download_error(htlc, htxf);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_folder_get_reply(s, s.len());
    if r.ref_ == 0 {
        return;
    }
    // Aggregate byte count for the whole tree; clamp a server-reported 0 to 1 so
    // the progress UI reads sensibly. The 64-bit companion wins when present.
    let total = if r.size64_seen {
        r.size64
    } else if r.size != 0 {
        r.size as u64
    } else {
        1
    };
    hx_htxf_set_ref(htxf, r.ref_);
    hx_htxf_set_total_size(htxf, total);
    hx_htxf_set_queue(htxf, r.queue);
    stamp_subchannel(htlc, htxf);
    hx_xfer_announce(htlc, htxf, r.queue);
}

/// `void rcv_task_file_put (htlc, frame, frame_len, htxf, data)` — HTLS reply to
/// HTLC_HDR_FILE_PUT (was `rcv.c`). A task error always deletes the transfer;
/// otherwise parses natively (including the RFLT resume offsets), gates on
/// `!ref`, then probes the local file (`hx_file_size` / `resource_len` /
/// `comment_len` on the C-owned path pointer) to size the upload and stamps the
/// transfer. The `133 + …` byte total is computed here.
///
/// # Safety
/// See [`rcv_task_file_get`].
#[no_mangle]
pub unsafe extern "C" fn rcv_task_file_put(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    ptr: *mut c_void,
    _data: *mut c_void,
) {
    let htxf = ptr;
    if task_in_error(frame, frame_len) {
        gtask_delete_htxf(hx_sess_from_htlc(htlc), htxf);
        xfer_delete(htxf);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_file_put_reply(s, s.len());
    if r.ref_ == 0 {
        return;
    }
    let data_pos = r.data_pos as u64;
    let rsrc_pos = r.rsrc_pos as u64;
    hx_htxf_set_data_pos(htxf, data_pos);
    hx_htxf_set_rsrc_pos(htxf, rsrc_pos);
    hx_htxf_set_queue(htxf, r.queue);

    // Probe the local file. The path stays a C string; we pass the pointer
    // straight to the fs primitives rather than marshaling it into Rust.
    let path = hx_htxf_path(htxf);
    let mut data_size = hx_htxf_data_size(htxf);
    let sz = hx_file_size(path);
    if sz >= 0 {
        data_size = sz as u64;
        hx_htxf_set_data_size(htxf, data_size);
    }
    let rsrc_size = resource_len(path) as u64;
    hx_htxf_set_rsrc_size(htxf, rsrc_size);

    // Wrapping subtraction matches the C guint64 arithmetic (data_pos <=
    // data_size in practice; wrapping avoids a Rust debug overflow panic).
    let total = 133u64
        + if rsrc_size.wrapping_sub(rsrc_pos) != 0 { 16 } else { 0 }
        + comment_len(path) as u64
        + data_size.wrapping_sub(data_pos)
        + rsrc_size.wrapping_sub(rsrc_pos);
    hx_htxf_set_total_size(htxf, total);
    hx_htxf_set_ref(htxf, r.ref_);
    stamp_subchannel(htlc, htxf);
    hx_xfer_announce(htlc, htxf, r.queue);
}

/// `void rcv_task_folder_put (htlc, frame, frame_len, htxf, data)` — HTLS reply
/// to HTLC_HDR_FILE_PUTFOLDER (was `rcv.c`). Strict subset of
/// [`rcv_task_file_put`] — no RFLT / fs probe (per-file resume happens inside the
/// worker).
///
/// # Safety
/// See [`rcv_task_file_get`].
#[no_mangle]
pub unsafe extern "C" fn rcv_task_folder_put(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    ptr: *mut c_void,
    _data: *mut c_void,
) {
    let htxf = ptr;
    if task_in_error(frame, frame_len) {
        gtask_delete_htxf(hx_sess_from_htlc(htlc), htxf);
        xfer_delete(htxf);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_folder_put_reply(s, s.len());
    if r.ref_ == 0 {
        return;
    }
    hx_htxf_set_ref(htxf, r.ref_);
    hx_htxf_set_queue(htxf, r.queue);
    stamp_subchannel(htlc, htxf);
    hx_xfer_announce(htlc, htxf, r.queue);
}

/// `void rcv_task_banner_get (htlc, frame, frame_len, ptr, data)` — HTLS reply
/// to HTLC_HDR_DOWNLOAD_BANNER (was `rcv.c`). Parses the (ref, size) scalars
/// natively and hands them to `banner_handle_htxf_reply`, which spins up the HTXF
/// subchannel worker. A task error is dropped silently (the proto trace still
/// shows the frame).
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `ptr` / `data` are unused (NULL at register time).
#[no_mangle]
pub unsafe extern "C" fn rcv_task_banner_get(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    _ptr: *mut c_void,
    _data: *mut c_void,
) {
    if task_in_error(frame, frame_len) {
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_banner_get_reply(s, s.len());
    banner_handle_htxf_reply(htlc, r.ref_, r.size);
}

/// Copy a parsed wire string into a NUL-terminated `CString`, dropping any
/// interior NULs (the sanitised wire strings shouldn't contain them, but the
/// FFI contract requires a clean C string).
fn cstr_lossy(bytes: &[u8]) -> std::ffi::CString {
    let filtered: Vec<u8> = bytes.iter().copied().filter(|&b| b != 0).collect();
    std::ffi::CString::new(filtered).unwrap_or_default()
}

/// `void rcv_task_file_getinfo (htlc, frame, frame_len, path, data)` — HTLS
/// reply to HTLC_HDR_FILE_GETINFO (was `rcv.c`). Parses the reply natively (same
/// caps + sanitisation as the C extractor: name 255 / type 31 / creator 31 /
/// comment 255) and fires the `file-info` signal, passing the two Hotline date
/// stamps **raw** — the view (`output_file_info`) decodes + locale-formats them,
/// so no date logic lives here.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `ptr` is the request's `char *path` task label.
/// The signal emit is synchronous, so the parsed buffers outlive the view handler.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_file_getinfo(
    _htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    ptr: *mut c_void,
    _data: *mut c_void,
) {
    if task_in_error(frame, frame_len) {
        // `ptr` is the request's path label (`g_strdup`'d in hx_file_info, stored
        // as the task ptr with no ptr_free). On success it transfers to the
        // file-info window (freed in close_file_info); on a task error no window
        // opens, so release it here rather than leak it per FILE_GETINFO failure.
        g_free(ptr);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let f = hotline_proto::parse::parse_file_getinfo(s, s.len(), 255, 31, 31, 255);
    let size = if f.size64_seen { f.size64 } else { f.size as u64 };
    let name = cstr_lossy(&f.name);
    let type_ = cstr_lossy(&f.type_);
    let creator = cstr_lossy(&f.creator);
    let comment = cstr_lossy(&f.comment);
    gtkhx_session_emit_file_info(
        gtkhx_session_get_default(),
        ptr as *const c_char,
        name.as_ptr(),
        creator.as_ptr(),
        type_.as_ptr(),
        comment.as_ptr(),
        f.date_modify.as_ptr(),
        f.date_create.as_ptr(),
        size,
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
