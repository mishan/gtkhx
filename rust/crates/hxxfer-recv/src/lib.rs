//! File-transfer receive handlers (ported from `rcv.c`).
//!
//! The five transfer task-replies (`rcv_task_file_get` / `_folder_get` /
//! `_file_put` / `_folder_put` / `_banner_get`), the file-info reply
//! (`rcv_task_file_getinfo`), and the unsolicited `hx_rcv_xfer_queue` live here.
//! Each one parses its reply natively (`hotline_proto::parse::*` — no
//! `gtkhx_proto_*` FFI round-trip), applies the pure dispatch gates, and then
//! delegates the transfer-specific state change to a focused C shim in
//! `xfers_recv_bridge.c`.
//!
//! Why the split: `struct htxf_conn` is refcounted and touched from both the
//! main thread and the per-transfer worker, so it stays C-owned; the stamping,
//! the `xfers[]` membership check, the retry/delete error policy, the preview
//! window construction, and the local-filesystem probes (`stat` / `resource_len`
//! / `comment_len`) are all genuinely C-side. The handler owns the parse and the
//! decision; the bridge owns the mutation. Once a transfer's `htxf` is ready to
//! move, everything funnels through the shared [`hx_xfer_announce`] tail.

use std::os::raw::{c_char, c_void};

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
    /// Fire `GtkhxSession::file-info (path, name, creator, type, comments,
    /// modified, created, size)` (gtkhx-session).
    #[allow(clippy::too_many_arguments)]
    fn gtkhx_session_emit_file_info(
        self_: *mut c_void,
        path: *const c_char,
        name: *const c_char,
        creator: *const c_char,
        type_: *const c_char,
        comments: *const c_char,
        modified: *const c_char,
        created: *const c_char,
        size: u64,
    );

    // ---- xfers_recv_bridge.c: the C-owned htxf state mutations ----
    /// Is `htxf` still a live entry in the `xfers[]` array? (file_get /
    /// folder_get gate the reply on this — a since-cancelled transfer's reply
    /// is dropped.)
    fn hx_xfer_in_list(htxf: *mut c_void) -> std::os::raw::c_int;
    /// Retry-or-delete on a task-error reply for a *download* (file_get /
    /// folder_get): re-arm the 1 s retry timer when `htxf->opt.retry` is set,
    /// else drop the transfer.
    fn hx_xfer_get_error(htlc: *mut c_void, htxf: *mut c_void);
    /// Always-delete on a task-error reply for an *upload* (file_put /
    /// folder_put): the server rejected the put outright.
    fn hx_xfer_put_error(htlc: *mut c_void, htxf: *mut c_void);
    /// Apply a successful FILE_GET reply: stamp ref / total_size / queue + the
    /// HTXF subchannel target onto `htxf`, build the preview window when
    /// `opt.preview` is set, and run the [`hx_xfer_announce`] tail.
    fn hx_xfer_file_get_apply(
        htlc: *mut c_void,
        htxf: *mut c_void,
        ref_: u32,
        total_size: u64,
        queue: u32,
    );
    /// Apply a successful FOLDER_GET reply: like [`hx_xfer_file_get_apply`] but
    /// no preview (folders don't preview), and `total_size` already normalised
    /// (0 → 1) by the caller.
    fn hx_xfer_folder_get_apply(
        htlc: *mut c_void,
        htxf: *mut c_void,
        ref_: u32,
        total_size: u64,
        queue: u32,
    );
    /// Apply a successful FILE_PUT reply: stamp resume offsets / queue / ref,
    /// probe the local file (`stat` / `resource_len` / `comment_len`) to compute
    /// the upload byte total, and run the announce tail.
    fn hx_xfer_file_put_apply(
        htlc: *mut c_void,
        htxf: *mut c_void,
        ref_: u32,
        queue: u32,
        data_pos: u32,
        rsrc_pos: u32,
    );
    /// Apply a successful FOLDER_PUT reply: stamp ref / queue + subchannel
    /// target and run the announce tail (per-file resume happens inside the
    /// worker, not here).
    fn hx_xfer_folder_put_apply(htlc: *mut c_void, htxf: *mut c_void, ref_: u32, queue: u32);
    /// Format the two Hotline date stamps and fire the `file-info` signal
    /// (`rcv_task_file_getinfo`). Strings arrive as (ptr, len) slices (not
    /// NUL-terminated); the dates are 8 raw wire bytes each.
    #[allow(clippy::too_many_arguments)]
    fn hx_xfer_file_info_apply(
        path: *const c_char,
        name: *const u8,
        name_len: usize,
        type_: *const u8,
        type_len: usize,
        creator: *const u8,
        creator_len: usize,
        comment: *const u8,
        comment_len: usize,
        date_create: *const u8,
        date_modify: *const u8,
        size: u64,
    );
    /// `banner.c` — the DOWNLOAD_BANNER reply spins up an HTXF subchannel worker
    /// off the (ref, size) scalars.
    fn banner_handle_htxf_reply(htlc: *mut c_void, ref_: u32, size: u32);
}

/// `void hx_file_info_recv (path, name, creator, type, comments, modified,
/// created, size)` — publish a `file-info` reply (`rcv_task_file_getinfo`). The
/// C handler keeps the parse (already Rust, `gtkhx_proto_parse_file_getinfo`)
/// and the Hotline-date formatting; this is the view-notify hop.
///
/// # Safety
/// All string args are NUL-terminated C strings.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn hx_file_info_recv(
    path: *const c_char,
    name: *const c_char,
    creator: *const c_char,
    type_: *const c_char,
    comments: *const c_char,
    modified: *const c_char,
    created: *const c_char,
    size: u64,
) {
    gtkhx_session_emit_file_info(
        gtkhx_session_get_default(),
        path,
        name,
        creator,
        type_,
        comments,
        modified,
        created,
        size,
    );
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

/// `void rcv_task_file_get (htlc, frame, frame_len, htxf, data)` — HTLS reply to
/// HTLC_HDR_FILE_GET (was `rcv.c`). Drops the reply if the transfer was already
/// cancelled (`hx_xfer_in_list`); on a task error, retries or deletes; otherwise
/// parses the reply natively (`parse::parse_file_get_reply`), applies the
/// `(!size && !size64_seen) || !ref` malformed-frame gate, and hands the
/// stamped scalars to [`hx_xfer_file_get_apply`].
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
    if hx_xfer_in_list(htxf) == 0 {
        return;
    }
    if task_in_error(frame, frame_len) {
        hx_xfer_get_error(htlc, htxf);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_file_get_reply(s, s.len());
    if (r.size == 0 && !r.size64_seen) || r.ref_ == 0 {
        return;
    }
    let total = if r.size64_seen { r.size64 } else { r.size as u64 };
    hx_xfer_file_get_apply(htlc, htxf, r.ref_, total, r.queue);
}

/// `void rcv_task_folder_get (htlc, frame, frame_len, htxf, data)` — HTLS reply
/// to HTLC_HDR_FILE_GETFOLDER (was `rcv.c`). Mirror of [`rcv_task_file_get`]:
/// same cancellation + error handling, but the only gate is `!ref` (folders are
/// legal at total_size 0), and the total is normalised to 1 for the progress UI
/// when the server reports 0. `FILE_NFILES` is parsed but currently
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
    if hx_xfer_in_list(htxf) == 0 {
        return;
    }
    if task_in_error(frame, frame_len) {
        hx_xfer_get_error(htlc, htxf);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_folder_get_reply(s, s.len());
    if r.ref_ == 0 {
        return;
    }
    // total_size is the aggregate byte count for the whole tree; clamp a
    // server-reported 0 to 1 so the progress UI reads sensibly (the 64-bit
    // companion wins when present).
    let total = if r.size64_seen {
        r.size64
    } else if r.size != 0 {
        r.size as u64
    } else {
        1
    };
    hx_xfer_folder_get_apply(htlc, htxf, r.ref_, total, r.queue);
}

/// `void rcv_task_file_put (htlc, frame, frame_len, htxf, data)` — HTLS reply to
/// HTLC_HDR_FILE_PUT (was `rcv.c`). A task error always deletes the transfer
/// (`hx_xfer_put_error`); otherwise parses natively
/// (`parse::parse_file_put_reply`, including the RFLT resume offsets), gates on
/// `!ref`, and hands off to [`hx_xfer_file_put_apply`] (which probes the local
/// file to size the upload).
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
        hx_xfer_put_error(htlc, htxf);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_file_put_reply(s, s.len());
    if r.ref_ == 0 {
        return;
    }
    hx_xfer_file_put_apply(htlc, htxf, r.ref_, r.queue, r.data_pos, r.rsrc_pos);
}

/// `void rcv_task_folder_put (htlc, frame, frame_len, htxf, data)` — HTLS reply
/// to HTLC_HDR_FILE_PUTFOLDER (was `rcv.c`). Strict subset of
/// [`rcv_task_file_put`] — no RFLT (per-file resume happens inside the worker).
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
        hx_xfer_put_error(htlc, htxf);
        return;
    }
    let s = frame_slice(frame, frame_len);
    let r = hotline_proto::parse::parse_folder_put_reply(s, s.len());
    if r.ref_ == 0 {
        return;
    }
    hx_xfer_folder_put_apply(htlc, htxf, r.ref_, r.queue);
}

/// `void rcv_task_banner_get (htlc, frame, frame_len, ptr, data)` — HTLS reply
/// to HTLC_HDR_DOWNLOAD_BANNER (was `rcv.c`). Parses the (ref, size) scalars
/// natively (`parse::parse_banner_get_reply`) and hands them to
/// `banner_handle_htxf_reply`, which spins up the HTXF subchannel worker. A task
/// error is dropped silently (the proto trace still shows the frame).
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `ptr` / `data` are unused (NULL at register
/// time).
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

/// `void rcv_task_file_getinfo (htlc, frame, frame_len, path, data)` — HTLS
/// reply to HTLC_HDR_FILE_GETINFO (was `rcv.c`). Parses the reply natively
/// (`parse::parse_file_getinfo`, same caps + sanitisation as the C extractor:
/// name 255 / type 31 / creator 31 / comment 255) and hands the fields — with
/// the two raw Hotline date stamps — to [`hx_xfer_file_info_apply`], which does
/// the locale date formatting and fires the `file-info` signal.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `ptr` is the request's `char *path` task label.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_file_getinfo(
    _htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    ptr: *mut c_void,
    _data: *mut c_void,
) {
    if task_in_error(frame, frame_len) {
        return;
    }
    let s = frame_slice(frame, frame_len);
    let f = hotline_proto::parse::parse_file_getinfo(s, s.len(), 255, 31, 31, 255);
    let size = if f.size64_seen { f.size64 } else { f.size as u64 };
    hx_xfer_file_info_apply(
        ptr as *const c_char,
        f.name.as_ptr(),
        f.name.len(),
        f.type_.as_ptr(),
        f.type_.len(),
        f.creator.as_ptr(),
        f.creator.len(),
        f.comment.as_ptr(),
        f.comment.len(),
        f.date_create.as_ptr(),
        f.date_modify.as_ptr(),
        size,
    );
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::{Cell, RefCell};

    thread_local! {
        /// The `htxf` pointer of the last emitted xfer-queue signal, or None.
        pub static EMITTED: Cell<Option<*mut std::os::raw::c_void>> = const { Cell::new(None) };
        /// The `htxf` pointer passed to the last xfer_ready_write, or None.
        pub static STARTED: Cell<Option<*mut std::os::raw::c_void>> = const { Cell::new(None) };
        /// The last emitted file-info as (name-bytes, size), or None.
        pub static FILE_INFO: RefCell<Option<(Vec<u8>, u64)>> = const { RefCell::new(None) };

        /// Value `hx_xfer_in_list` returns (1 = live, 0 = cancelled).
        pub static IN_LIST: Cell<std::os::raw::c_int> = const { Cell::new(1) };
        /// True after `hx_xfer_get_error` fired.
        pub static GET_ERROR: Cell<bool> = const { Cell::new(false) };
        /// True after `hx_xfer_put_error` fired.
        pub static PUT_ERROR: Cell<bool> = const { Cell::new(false) };
        /// (ref, total_size, queue) of the last file_get apply, or None.
        pub static FILE_GET: Cell<Option<(u32, u64, u32)>> = const { Cell::new(None) };
        /// (ref, total_size, queue) of the last folder_get apply, or None.
        pub static FOLDER_GET: Cell<Option<(u32, u64, u32)>> = const { Cell::new(None) };
        /// (ref, queue, data_pos, rsrc_pos) of the last file_put apply, or None.
        pub static FILE_PUT: Cell<Option<(u32, u32, u32, u32)>> = const { Cell::new(None) };
        /// (ref, queue) of the last folder_put apply, or None.
        pub static FOLDER_PUT: Cell<Option<(u32, u32)>> = const { Cell::new(None) };
        /// (ref, size) of the last banner reply, or None.
        pub static BANNER: Cell<Option<(u32, u32)>> = const { Cell::new(None) };
        /// (name-bytes, size) of the last file_info apply, or None.
        pub static FILE_INFO_APPLY: RefCell<Option<(Vec<u8>, u64)>> = const { RefCell::new(None) };
    }

    pub fn reset() {
        EMITTED.with(|c| c.set(None));
        STARTED.with(|c| c.set(None));
        FILE_INFO.with(|c| *c.borrow_mut() = None);
        IN_LIST.with(|c| c.set(1));
        GET_ERROR.with(|c| c.set(false));
        PUT_ERROR.with(|c| c.set(false));
        FILE_GET.with(|c| c.set(None));
        FOLDER_GET.with(|c| c.set(None));
        FILE_PUT.with(|c| c.set(None));
        FOLDER_PUT.with(|c| c.set(None));
        BANNER.with(|c| c.set(None));
        FILE_INFO_APPLY.with(|c| *c.borrow_mut() = None);
    }
}

#[cfg(test)]
unsafe fn hx_xfer_in_list(_htxf: *mut c_void) -> std::os::raw::c_int {
    test_env::IN_LIST.with(|c| c.get())
}

#[cfg(test)]
unsafe fn hx_xfer_get_error(_htlc: *mut c_void, _htxf: *mut c_void) {
    test_env::GET_ERROR.with(|c| c.set(true));
}

#[cfg(test)]
unsafe fn hx_xfer_put_error(_htlc: *mut c_void, _htxf: *mut c_void) {
    test_env::PUT_ERROR.with(|c| c.set(true));
}

#[cfg(test)]
unsafe fn hx_xfer_file_get_apply(
    _htlc: *mut c_void,
    _htxf: *mut c_void,
    ref_: u32,
    total_size: u64,
    queue: u32,
) {
    test_env::FILE_GET.with(|c| c.set(Some((ref_, total_size, queue))));
}

#[cfg(test)]
unsafe fn hx_xfer_folder_get_apply(
    _htlc: *mut c_void,
    _htxf: *mut c_void,
    ref_: u32,
    total_size: u64,
    queue: u32,
) {
    test_env::FOLDER_GET.with(|c| c.set(Some((ref_, total_size, queue))));
}

#[cfg(test)]
unsafe fn hx_xfer_file_put_apply(
    _htlc: *mut c_void,
    _htxf: *mut c_void,
    ref_: u32,
    queue: u32,
    data_pos: u32,
    rsrc_pos: u32,
) {
    test_env::FILE_PUT.with(|c| c.set(Some((ref_, queue, data_pos, rsrc_pos))));
}

#[cfg(test)]
unsafe fn hx_xfer_folder_put_apply(_htlc: *mut c_void, _htxf: *mut c_void, ref_: u32, queue: u32) {
    test_env::FOLDER_PUT.with(|c| c.set(Some((ref_, queue))));
}

#[cfg(test)]
unsafe fn banner_handle_htxf_reply(_htlc: *mut c_void, ref_: u32, size: u32) {
    test_env::BANNER.with(|c| c.set(Some((ref_, size))));
}

#[cfg(test)]
#[allow(clippy::too_many_arguments)]
unsafe fn hx_xfer_file_info_apply(
    _path: *const c_char,
    name: *const u8,
    name_len: usize,
    _type_: *const u8,
    _type_len: usize,
    _creator: *const u8,
    _creator_len: usize,
    _comment: *const u8,
    _comment_len: usize,
    _date_create: *const u8,
    _date_modify: *const u8,
    size: u64,
) {
    let name = if name.is_null() {
        Vec::new()
    } else {
        std::slice::from_raw_parts(name, name_len).to_vec()
    };
    test_env::FILE_INFO_APPLY.with(|c| *c.borrow_mut() = Some((name, size)));
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
#[allow(clippy::too_many_arguments)]
unsafe fn gtkhx_session_emit_file_info(
    _self_: *mut c_void,
    _path: *const c_char,
    name: *const c_char,
    _creator: *const c_char,
    _type_: *const c_char,
    _comments: *const c_char,
    _modified: *const c_char,
    _created: *const c_char,
    size: u64,
) {
    let name = if name.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(name).to_bytes().to_vec()
    };
    test_env::FILE_INFO.with(|c| *c.borrow_mut() = Some((name, size)));
}

#[cfg(test)]
mod tests;
