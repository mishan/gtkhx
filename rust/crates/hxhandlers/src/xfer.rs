//! The file-transfer **registry** — the `xfers[]` list (ported from `xfers.c`),
//! Part of the xfers.c → Rust migration (`docs/rust/ROADMAP.md`).
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
//! natively through `hxnet`'s `HtxfHandle` (hxnet `pub`s the plain ones the shell
//! needs) rather than the `hx_htxf_*` C-ABI accessor bounce.

use std::cell::RefCell;
use std::os::raw::{c_char, c_int, c_void};

use hxnet::htxf::{
    hxnet_htxf_abort, hxnet_htxf_abort_free, hxnet_htxf_abort_new, hxnet_htxf_close,
    hxnet_htxf_finish_send, hxnet_htxf_set_folder_items, HtxfAbort, HtxfConn,
};
use hxnet::xfer::{
    folder_recv, hxnet_xfer_file_recv_one, hxnet_xfer_file_send_one, hxnet_xfer_folder_send_all,
    progress_due, FolderLedger, HxnetFolderParams, HxnetXferParams,
};
use hxnet::xfer_handle::{
    hx_htxf_add_total_pos, hx_htxf_cancel, hx_htxf_is_canceled, hx_htxf_new, hx_htxf_ref,
    hx_htxf_set_destructor, hx_htxf_set_total_pos, hx_htxf_total_pos, hx_htxf_unref, HtxfHandle,
};
use hxproto::build::{self, FileGetRequest, FilePutRequest, HxChunk};
use hxproto::messages::ClientHdr;

// Native collaborators for the xfer_go wire build (test build shadows them via the
// doubles below). hx_conn_has_cap is already Rust (gtkhx-core).
use crate::recv::files::Asked;
use crate::send::files::expect_transfer;
#[cfg(not(test))]
use gtkhx_core::conn::hx_conn_has_cap;
#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;
// Encode a "/a/b" path to the wire DIR bytes (g_malloc'd buffer + out length;
// caller g_free's). is_file = 0 for a directory chunk.
use hxrequest::path::path_to_hldir;

/// `FILE_DONE` (src/sound.h) — the transfer-complete chime.
const FILE_DONE: c_int = 3;
/// `XFER_GET` (src/protocol.h).
const XFER_GET: u8 = 0;
/// The download / upload request opcodes `xfer_go` writes (single source of
/// truth is the `hxproto::messages::ClientHdr` enum).
const HTLC_HDR_FILE_GET: u32 = ClientHdr::FileGet as u32;
const HTLC_HDR_FILE_PUT: u32 = ClientHdr::FilePut as u32;
/// The negotiated capability bit `xfer_go` reads for XFERSIZE64 emission. The
/// LOGIN cap bits aren't modelled in hxproto; spelled here against the
/// hotline.h reference.
const HTLC_CAP_LARGE_FILES: u64 = 0x0001;

/// `void hx_xfer_build_resume_rflt(guint32 data_pos, guint32 rsrc_pos,
/// guint8 *out)` — C-ABI wrapper over [`hxrequest::files::resume_rflt`], filling
/// `out[..74]`.
/// Exists so the Tier-3 resume test drives the *production* record bytes rather
/// than a hand-rolled copy.
///
/// # Safety
/// `out` must point to at least 74 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn hx_xfer_build_resume_rflt(data_pos: u32, rsrc_pos: u32, out: *mut u8) {
    let r = hxrequest::files::resume_rflt(data_pos, rsrc_pos);
    std::ptr::copy_nonoverlapping(r.as_ptr(), out, r.len());
}

// C environment resolved at the final link; #[cfg(test)] doubles below let
// `cargo test -p hxhandlers` run headless (same shape recv/files.rs uses).
#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_file_update, gtkhx_session_emit_xfer_destroyed, gtkhx_session_get_default,
};

#[cfg(not(test))]
extern "C" {
    /// The session owning this htlc (gtkhx-core).
    fn hx_sess_from_htlc(htlc: *mut c_void) -> *mut c_void;
    /// gtkhx_ui_bridge.c — the queue-downloads pref (xfer_new's inline-vs-queue).
    fn hx_prefs_queuedl() -> c_int;
    /// preview.c — drop the per-htxf ref on the preview window, and clear its
    /// cancel hook first (both NULL-safe; no-ops on a non-preview transfer):
    /// the steps of the last-ref teardown (`htxf_destructor`) that reach C.
    fn hx_preview_unref(p: *mut c_void);
    fn hx_preview_set_cancel_cb(
        p: *mut c_void,
        f: Option<unsafe extern "C" fn(*mut c_void)>,
        user_data: *mut c_void,
    );
    /// htxf_accessors.c — the opt bitfield setters (C owns the bit layout).
    fn hx_htxf_set_opt_preview(htxf: *mut HtxfHandle, v: c_int);
    fn hx_htxf_set_opt_folder(htxf: *mut HtxfHandle, v: c_int);
    /// hxbridge — queue a GSourceFunc onto the global default main context
    /// (`g_main_context_invoke`); thread-safe, used to marshal a worker-thread
    /// progress update back to the main loop.
    fn gtkhx_bridge_post_to_main(func: glib::ffi::GSourceFunc, user_data: *mut c_void);
    /// hxbridge — run `worker` on the tokio blocking pool, then `completion` on
    /// the GLib main loop once it returns (same shim banner.c uses).
    fn gtkhx_bridge_spawn_blocking_with_idle(
        worker: unsafe extern "C" fn(*mut c_void),
        completion: unsafe extern "C" fn(*mut c_void),
        user_data: *mut c_void,
    );
    /// network.c — open the HTXF subchannel for this transfer (thin wrapper over
    /// hxnet_htxf_connect + abort-arm). Returns FALSE on failure. Worker thread.
    fn htxf_connect(htxf: *mut HtxfHandle) -> glib::ffi::gboolean;
    /// sound.c — play a chime by id (FILE_DONE here). Worker thread.
    fn play_sound(sound: c_int);
    /// debug.c — a line under a `GTKHX_DEBUG` category.
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
    /// preview.c — the GTK preview-window feed. Reached only through the receive
    /// param callbacks; cast to the void*-first shape hxnet::xfer expects (they
    /// really take `hx_preview *`, ABI-identical to a leading pointer arg).
    fn hx_preview_chunk(preview: *mut c_void, buf: *const c_char, len: usize);
    fn hx_preview_set_info(
        preview: *mut c_void,
        type_: *const c_char,
        creator: *const c_char,
        size: u64,
    ) -> u64;
    fn hx_preview_done(preview: *mut c_void);
    fn hx_preview_set_download_cb(
        preview: *mut c_void,
        f: Option<unsafe extern "C" fn(*mut c_void) -> glib::ffi::gboolean>,
        user_data: *mut c_void,
        destroy: glib::ffi::GDestroyNotify,
    );
    /// session_registry.c — the live connection with this serial, or NULL.
    fn hx_conn_with_serial(serial: u16) -> *mut c_void;
    /// htxf_accessors.c — read the opt bitfield bits (C owns the layout).
    /// `*const c_void` htxf to match recv/xfer.rs's existing declaration.
    fn hx_htxf_opt_preview(htxf: *const c_void) -> c_int;
    fn hx_htxf_opt_folder(htxf: *const c_void) -> c_int;
    fn hx_htxf_opt_large(htxf: *const c_void) -> c_int;

    // ---- xfer_go wire build + path collaborators ----
    /// uniquify_path.c — mutate `path` in place to a non-colliding name using the
    /// supplied exists predicate (the core algorithm keeps its Tier-1 test in C).
    fn uniquify_path(
        path: *mut c_char,
        cap: usize,
        exists: Option<unsafe extern "C" fn(*const c_char, *mut c_void) -> c_int>,
        user_data: *mut c_void,
    );
    /// htxf_accessors.c — `stat(2)` the path; data-fork byte size, or -1 on error.
    fn hx_file_size(path: *const c_char) -> i64;
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
    with_list(|xs| {
        xs.iter()
            .position(|&e| e == htxf)
            .map_or(-1, |i| i as c_int)
    })
}

/// The transfer `htlc` knows by the server's XFER ref, or NULL. A ref is the
/// server's, so two connections can each have one with the same number.
///
/// # Safety
/// Main thread only; the list holds live handles.
pub(crate) unsafe fn htxf_with_ref(htlc: *mut c_void, ref_: u32) -> *mut HtxfHandle {
    with_list(|xs| {
        xs.iter()
            .copied()
            .find(|&e| (*e).htlc == htlc && (*e).ref_ == ref_)
            .unwrap_or(std::ptr::null_mut())
    })
}

/// `void xfer_remove_from_list(struct htxf_conn *htxf)` — unlink `htxf`, emit
/// `xfer-destroyed` (so subscribers null cached pointers), drop the list ref, and
/// kick the next queued transfer. Idempotent. Emits BEFORE the unref, so handlers
/// run with the htxf still alive (the unref drops only the list's ref). Kept
/// `#[no_mangle]` as part of the transfer C ABI surface.
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
    set_pause(htxf, None);
    set_tries(htxf, None);
    set_held(htxf, None);
    hx_htxf_unref(htxf); // drop the list's ref
    start_next();
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
    let all: Vec<*mut HtxfHandle> = with_list(std::mem::take);
    PAUSES.with_borrow_mut(Vec::clear);
    QUEUED.with_borrow_mut(Vec::clear);
    HELD.with_borrow_mut(Vec::clear);
    for htxf in all {
        hx_htxf_cancel(htxf);
        hxnet_htxf_abort((*htxf).abort as *const HtxfAbort);
        hx_htxf_unref(htxf); // drop the list's ref
    }
}

/// `void xfers_delete_on_conn(struct htlc_conn *htlc)` — cancel every in-flight
/// transfer belonging to `htlc`, for the disconnect path.
///
/// The transfers run over their own HTXF subchannels, so nothing about closing
/// the control connection stops them: without this they carry on against a
/// server that has forgotten the session, and the ones that were merely queued
/// wait forever for a turn that will never come. Their rows sit in the tasks
/// list looking live.
///
/// Per connection, not `xfers_delete_all`: disconnecting one server must leave
/// another's downloads running. That is the whole reason the queue is one
/// global list *tagged* by connection rather than a list per connection —
/// there is one place to watch every transfer, and one place to sweep from.
///
/// # Safety
/// `htlc` is a live connection. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfers_delete_on_conn(htlc: *mut c_void) {
    // Collect first, then cancel: `xfer_delete` unlinks from the list, and
    // walking a list something is removing from under you is the classic way
    // to miss half of it.
    let doomed: Vec<*mut HtxfHandle> =
        with_list(|xs| xs.iter().copied().filter(|&e| (*e).htlc == htlc).collect());
    for htxf in doomed {
        xfer_delete(htxf);
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

// ---- pause and resume ----------------------------------------------------

/// Where a paused transfer stands. Hotline has no pause: pausing ends the
/// transfer's connection, keeping its row and what it moved, and resuming
/// asks anew for the rest: a download's from what is on disk, a file's from
/// its partial file's size and a folder's file by file
/// ([`hxnet::xfer::answer`]); an upload's from what the server says it has.
/// The server's queue slot goes with the connection.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Pause {
    /// Its connection is closing; the worker hasn't finished.
    Winding,
    /// Settled, its local file as `kept`: a download's partial file, an
    /// upload's source.
    Paused { kept: Disk },
    /// Resumed, the server's reply not yet in.
    Asked { kept: Disk },
    /// An upload asked for again from the start, as it couldn't resume; the
    /// server's reply not yet in.
    Restarted { kept: Disk },
    /// An upload paused, its file changed since: resumed, it would splice two
    /// files on the server, and asked for from the start, some servers would
    /// add it to what they hold.
    Changed { kept: Disk },
}

/// A local file as a pause left it: its data fork's size (-1 for none) and
/// when it was last changed.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Disk {
    len: i64,
    modified: Option<std::time::SystemTime>,
}

unsafe fn disk(path: *const c_char) -> Disk {
    let bytes = std::ffi::CStr::from_ptr(path).to_bytes();
    #[cfg(unix)]
    let modified = {
        use std::os::unix::ffi::OsStrExt;
        std::fs::metadata(std::ffi::OsStr::from_bytes(bytes))
    };
    #[cfg(not(unix))]
    let modified = std::str::from_utf8(bytes)
        .map_err(std::io::Error::other)
        .and_then(std::fs::metadata);
    Disk {
        len: hx_file_size(path),
        modified: modified.and_then(|m| m.modified()).ok(),
    }
}

thread_local! {
    static PAUSES: RefCell<Vec<(*mut HtxfHandle, Pause)>> = const { RefCell::new(Vec::new()) };
    /// The transfers whose worker is running.
    static WORKING: RefCell<Vec<*mut HtxfHandle>> = const { RefCell::new(Vec::new()) };
    /// The downloads the queue started and the server hasn't yet answered,
    /// with how many times a refusal has put each back.
    static QUEUED: RefCell<Vec<(*mut HtxfHandle, u8)>> = const { RefCell::new(Vec::new()) };
    /// The bytes of each resumed folder download already on disk.
    static HELD: RefCell<Vec<(*mut HtxfHandle, u64)>> = const { RefCell::new(Vec::new()) };
}

pub(crate) fn set_held(htxf: *mut HtxfHandle, to: Option<u64>) {
    HELD.with_borrow_mut(|h| {
        h.retain(|e| e.0 != htxf);
        if let Some(to) = to {
            h.push((htxf, to));
        }
    });
}

/// The bytes a resumed folder download had on disk when it was asked for
/// again, which the size its reply brings counts.
pub(crate) fn folder_held(htxf: *mut c_void) -> u64 {
    HELD.with_borrow(|h| h.iter().find(|e| e.0 == htxf.cast()).map_or(0, |e| e.1))
}

fn pause_of(htxf: *mut HtxfHandle) -> Option<Pause> {
    PAUSES.with_borrow(|p| p.iter().find(|e| e.0 == htxf).map(|e| e.1))
}

fn set_pause(htxf: *mut HtxfHandle, to: Option<Pause>) {
    PAUSES.with_borrow_mut(|p| {
        p.retain(|e| e.0 != htxf);
        if let Some(to) = to {
            p.push((htxf, to));
        }
    });
}

/// Paused, as the row shows it: from the click until it is resumed.
fn paused(htxf: *mut HtxfHandle) -> bool {
    matches!(
        pause_of(htxf),
        Some(Pause::Winding | Pause::Paused { .. } | Pause::Changed { .. })
    )
}

unsafe fn emit_file_update(htxf: *mut HtxfHandle) {
    gtkhx_session_emit_file_update(
        gtkhx_session_get_default(),
        hx_sess_from_htlc((*htxf).htlc),
        htxf as *mut c_void,
    );
}

/// `int xfer_can_pause(struct htxf_conn *htxf)` — whether `htxf` is a
/// download, of a file or a folder, or a file's upload, whose worker is
/// running.
///
/// # Safety
/// `htxf` is a live handle. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_can_pause(htxf: *mut HtxfHandle) -> c_int {
    let kind = if (*htxf).type_ == XFER_GET {
        hx_htxf_opt_preview(htxf as *const c_void) == 0
    } else {
        // An upload resumes from offsets an RFLT says in 32 bits.
        hx_htxf_opt_folder(htxf as *const c_void) == 0 && (*htxf).total_size <= u64::from(u32::MAX)
    };
    (kind && WORKING.with_borrow(|w| w.contains(&htxf)) && pause_of(htxf).is_none()) as c_int
}

/// `int xfer_can_start(struct htxf_conn *htxf)` — whether Start would send
/// `htxf`'s request: a single-file transfer not yet asked for, or a settled
/// pause. A folder's first request goes when it is made.
///
/// # Safety
/// `htxf` is a live handle. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_can_start(htxf: *mut HtxfHandle) -> c_int {
    let fresh = (*htxf).gone == 0
        && pause_of(htxf).is_none()
        && hx_htxf_opt_folder(htxf as *const c_void) == 0;
    (fresh || matches!(pause_of(htxf), Some(Pause::Paused { .. }))) as c_int
}

/// `int xfer_paused(struct htxf_conn *htxf)` — whether the row shows `htxf`
/// paused: 1, or 2 for an upload whose file has changed since.
///
/// # Safety
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_paused(htxf: *mut HtxfHandle) -> c_int {
    match pause_of(htxf) {
        Some(Pause::Changed { .. }) => 2,
        _ => paused(htxf) as c_int,
    }
}

/// `guint64 xfer_resumed_bytes(struct htxf_conn *htxf)` — the bytes a
/// download already had on disk when it was asked for, or the server had of
/// an upload, which its progress counts on top of what this connection
/// moves.
///
/// # Safety
/// `htxf` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn xfer_resumed_bytes(htxf: *mut HtxfHandle) -> u64 {
    let h = &*htxf;
    if hx_htxf_opt_folder(htxf as *const c_void) == 0 {
        h.data_pos + h.rsrc_pos
    } else if h.type_ == XFER_GET {
        folder_held(htxf.cast())
    } else {
        0
    }
}

/// `void xfer_pause(struct htxf_conn *htxf)` — pause a running transfer:
/// end its connection, and let the next queued transfer go once it has
/// ended. The row and what it moved stay; [`xfer_go`] resumes it once the
/// worker has finished.
///
/// # Safety
/// `htxf` is a live handle. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_pause(htxf: *mut HtxfHandle) {
    if xfer_can_pause(htxf) == 0 {
        return;
    }
    set_pause(htxf, Some(Pause::Winding));
    hxnet_htxf_abort((*htxf).abort as *const HtxfAbort);
    emit_file_update(htxf);
}

/// A finished worker's transfer, if it was paused: settle it, keep it in the
/// list and let the queue move on. False for any other, which the caller
/// unlinks.
///
/// # Safety
/// `htxf` is a live handle whose worker has returned. Main thread only.
unsafe fn settle_pause(htxf: *mut HtxfHandle) -> bool {
    if pause_of(htxf) != Some(Pause::Winding) {
        return false;
    }
    // Finished before the pause could stop it. A folder's progress counts
    // the files' headers, which its size doesn't, so only its worker knows.
    let finished = if hx_htxf_opt_folder(htxf as *const c_void) != 0 {
        folder_finished(htxf)
    } else {
        hx_htxf_total_pos(htxf) >= (*htxf).total_size
    };
    if finished {
        set_pause(htxf, None);
        return false;
    }
    // The old token stays aborted; the next connection needs a live one.
    hxnet_htxf_abort_free((*htxf).abort as *const HtxfAbort);
    (*htxf).abort = hxnet_htxf_abort_new() as *mut c_void;
    // A folder keeps a ledger of its files instead.
    let kept = if hx_htxf_opt_folder(htxf as *const c_void) != 0 {
        Disk {
            len: 0,
            modified: None,
        }
    } else {
        disk((*htxf).path.as_ptr())
    };
    set_pause(htxf, Some(Pause::Paused { kept }));
    emit_file_update(htxf);
    start_next();
    true
}

/// The server answered a transfer's request: it is no longer paused, nor
/// owed another try.
pub(crate) fn answered(htxf: *mut c_void) {
    let htxf = htxf as *mut HtxfHandle;
    if matches!(
        pause_of(htxf),
        Some(Pause::Asked { .. } | Pause::Restarted { .. })
    ) {
        set_pause(htxf, None);
    }
    set_tries(htxf, None);
}

/// The server refused a resumed download (mhxd's one-at-a-time, say): back
/// to paused, to be tried again, rather than dropped. False for any other.
///
/// # Safety
/// Main thread only; `htxf` is live if it is a resumed download.
pub(crate) unsafe fn resume_refused(htxf: *mut c_void) -> bool {
    let htxf = htxf as *mut HtxfHandle;
    let Some(Pause::Asked { kept }) = pause_of(htxf) else {
        return false;
    };
    set_pause(htxf, Some(Pause::Paused { kept }));
    emit_file_update(htxf);
    start_next();
    true
}

/// The server refused an upload asked for again after a pause. A refused
/// resume (the server holds nothing of it, or takes no resume) is asked for
/// again from the start, and the refusal kept quiet: `Some(true)`. One
/// refused from the start too (a server holding a file by that name refuses
/// it, as it would any upload) is paused again, and the server's reason
/// shown: `Some(false)`. `None` for any other upload.
///
/// # Safety
/// Main thread only; `htxf` is live if it is a resumed upload.
pub(crate) unsafe fn upload_resume_refused(htxf: *mut c_void) -> Option<bool> {
    let htxf = htxf as *mut HtxfHandle;
    match pause_of(htxf)? {
        Pause::Asked { kept } => {
            set_pause(htxf, Some(Pause::Restarted { kept }));
            xfer_go_put(htxf, None);
            Some(true)
        }
        Pause::Restarted { kept } => {
            set_pause(htxf, Some(Pause::Paused { kept }));
            emit_file_update(htxf);
            start_next();
            Some(false)
        }
        _ => None,
    }
}

/// The server holds more of a resumed upload than its file has: the file
/// changed since it was paused. Paused again, saying so. False for an
/// upload that wasn't resumed.
///
/// # Safety
/// Main thread only; `htxf` is live if it is a resumed upload.
pub(crate) unsafe fn upload_file_changed(htxf: *mut c_void) -> bool {
    let htxf = htxf as *mut HtxfHandle;
    let Some(Pause::Asked { kept } | Pause::Restarted { kept }) = pause_of(htxf) else {
        return false;
    };
    set_pause(htxf, Some(Pause::Changed { kept }));
    emit_file_update(htxf);
    start_next();
    true
}

/// How many times a refusal may put back a download the queue started, and
/// how long it waits before the queue tries it again.
const QUEUE_TRIES: u8 = 5;
const QUEUE_RETRY_MS: u32 = 500;

fn tries_of(htxf: *mut HtxfHandle) -> Option<u8> {
    QUEUED.with_borrow(|q| q.iter().find(|e| e.0 == htxf).map(|e| e.1))
}

fn set_tries(htxf: *mut HtxfHandle, to: Option<u8>) {
    QUEUED.with_borrow_mut(|q| {
        q.retain(|e| e.0 != htxf);
        if let Some(to) = to {
            q.push((htxf, to));
        }
    });
}

/// The server refused a download the queue started: mhxd refuses one asked
/// for before it has seen the last one's connection close ("1 at a time"),
/// which is when the queue asks. Back to queued, and tried again shortly, a
/// few times; false once those are spent, or for a download the user started.
///
/// # Safety
/// Main thread only; `htxf` is live if the queue started it.
pub(crate) unsafe fn queued_refused(htxf: *mut c_void) -> bool {
    let htxf = htxf as *mut HtxfHandle;
    match tries_of(htxf) {
        Some(n) if n < QUEUE_TRIES => {
            set_tries(htxf, Some(n + 1));
            (*htxf).gone = 0;
            hx_htxf_ref(htxf); // the timer's
            glib::ffi::g_timeout_add(QUEUE_RETRY_MS, Some(queue_retry), htxf.cast());
            true
        }
        _ => {
            set_tries(htxf, None);
            false
        }
    }
}

unsafe extern "C" fn queue_retry(data: glib::ffi::gpointer) -> glib::ffi::gboolean {
    let htxf = data as *mut HtxfHandle;
    if xfer_num(htxf) >= 0 {
        start_next();
    }
    hx_htxf_unref(htxf);
    glib::ffi::G_SOURCE_REMOVE
}

/// Start the first transfer that isn't paused, if it hasn't started: the
/// queue moving on when one ends or pauses.
unsafe fn start_next() {
    let head = with_list(|xs| xs.iter().copied().find(|&e| !paused(e)));
    if let Some(head) = head {
        let was = (*head).gone;
        xfer_go(head);
        // Only a request this sent is the queue's to try again.
        if was == 0 && (*head).gone != 0 && tries_of(head).is_none() {
            set_tries(head, Some(0));
        }
    }
}

// ---- construction + progress/completion marshaling ---------------------

/// Copy `src` bytes into a fixed C `char` array + NUL-terminate, truncating to
/// fit (mirrors g_strlcpy / the memcpy+NUL the C `xfer_init` did).
unsafe fn set_carr(dst: &mut [c_char], src: &[u8]) {
    let n = src.len().min(dst.len().saturating_sub(1));
    for (i, &b) in src[..n].iter().enumerate() {
        dst[i] = b as c_char;
    }
    dst[n] = 0;
}

/// Read a NUL-terminated fixed C `char` array back into a byte `Vec`.
unsafe fn carr_read(src: &[c_char]) -> Vec<u8> {
    let end = src.iter().position(|&c| c == 0).unwrap_or(src.len());
    src[..end].iter().map(|&c| c as u8).collect()
}

/// C string → borrowed bytes (empty for NULL); NUL-terminated.
unsafe fn cstr_bytes<'a>(p: *const c_char) -> &'a [u8] {
    if p.is_null() {
        &[]
    } else {
        std::ffi::CStr::from_ptr(p).to_bytes()
    }
}

#[cfg(not(test))]
unsafe fn resource_len(path: *const c_char) -> usize {
    if path.is_null() {
        return 0;
    }
    let bytes = std::ffi::CStr::from_ptr(path).to_bytes();
    #[cfg(unix)]
    let path = {
        use std::os::unix::ffi::OsStrExt;
        std::path::PathBuf::from(std::ffi::OsStr::from_bytes(bytes))
    };
    #[cfg(not(unix))]
    let Ok(path) = std::str::from_utf8(bytes).map(std::path::PathBuf::from) else {
        return 0;
    };
    hxnet::hfs_config::resource_len(&path).min(usize::MAX as u64) as usize
}

/// (ptr, len) → borrowed bytes (empty for NULL). Names carry any byte incl.
/// `/`, so they arrive as an explicit length, never NUL-terminated.
unsafe fn slice_bytes<'a>(p: *const c_char, len: usize) -> &'a [u8] {
    if p.is_null() || len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(p as *const u8, len)
    }
}

/// Shared constructor for [`xfer_new`] / [`xfer_new_folder`] (was `xfer_init`):
/// allocate the handle, stash the structured fields, seed the list's ref, enqueue
/// to the registry, and emit the initial `file-update`. Does NOT drive the wire
/// request — the caller decides (xfer_new → xfer_go, xfer_new_folder → its own
/// GETFOLDER/PUTFOLDER later).
///
/// `htlc` is the connection the transfer runs on, and it is the caller's to
/// choose. This used to read the *focused* connection instead, which made the
/// pane a file was dropped on irrelevant to where it actually went — invisible
/// at one connection, wrong at two.
///
/// `size` is what the row measures the transfer by until the server says,
/// 0 for not known.
unsafe fn xfer_init(
    htlc: *mut c_void,
    path: &[u8],
    remotedir: &[u8],
    remotename: &[u8],
    type_: u16,
    size: u64,
) -> *mut HtxfHandle {
    // Register the last-ref destructor once — every handle comes through here, so
    // this runs before any can be dropped (was the g_once_init in xfer_init).
    static DTOR: std::sync::Once = std::sync::Once::new();
    DTOR.call_once(|| hx_htxf_set_destructor(Some(htxf_destructor)));

    // Zeroed handle with its cancellation token pre-created (hxnet::xfer_handle).
    let htxf = hx_htxf_new();
    let h = &mut *htxf;

    // remotename: the wire NAME chunk, clamped to the field; never split.
    let rn_len = remotename.len().min(h.remotename.len() - 1);
    h.remotename_len = rn_len as u16;
    set_carr(&mut h.remotename, &remotename[..rn_len]);

    if !remotedir.is_empty() {
        set_carr(&mut h.remotedir, remotedir);
    }

    // remotepath = stored-remotedir [+ '/'] + name (display/log only; truncates).
    let dir = carr_read(&h.remotedir);
    let mut rp = dir.clone();
    if !dir.is_empty() && *dir.last().unwrap() != b'/' {
        rp.push(b'/');
    }
    rp.extend_from_slice(&remotename[..rn_len]);
    set_carr(&mut h.remotepath, &rp);

    set_carr(&mut h.path, path);
    h.type_ = type_ as u8;

    // Seed the list's ref (0 → 1), then enqueue.
    hx_htxf_ref(htxf);
    xfer_registry_add(htxf);

    h.htlc = htlc;
    h.total_size = size.max(1);
    gtkhx_session_emit_file_update(
        gtkhx_session_get_default(),
        hx_sess_from_htlc(h.htlc),
        htxf as *mut c_void,
    );
    htxf
}

/// `struct htxf_conn *xfer_new(struct htlc_conn *htlc, const char *path, const
/// char *remotedir, const char *remotename, gsize remotename_len, guint16 type,
/// int preview, guint32 srv_data_size)` — create a download/upload transfer on
/// `htlc`. Sets preview + srv_data_size (which xfer_go gates its resume/rename
/// on) BEFORE possibly driving the wire request inline (only transfer, or
/// queueing off).
///
/// # Safety
/// `htlc` is a live connection; `path` / `remotedir` are NUL-terminated C
/// strings (or NULL); `remotename` is `remotename_len` bytes. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_new(
    htlc: *mut c_void,
    path: *const c_char,
    remotedir: *const c_char,
    remotename: *const c_char,
    remotename_len: usize,
    type_: u16,
    preview: c_int,
    srv_data_size: u32,
) -> *mut HtxfHandle {
    let htxf = xfer_init(
        htlc,
        cstr_bytes(path),
        cstr_bytes(remotedir),
        slice_bytes(remotename, remotename_len),
        type_,
        // A download's size as the file listing gave it.
        if type_ as u8 == XFER_GET {
            srv_data_size.into()
        } else {
            0
        },
    );
    hx_htxf_set_opt_preview(htxf, if preview != 0 { 1 } else { 0 });
    (*htxf).srv_data_size = srv_data_size as u64;

    let alone = with_list(|xs| xs.iter().all(|&e| e == htxf || paused(e)));
    if alone || hx_prefs_queuedl() == 0 {
        xfer_go(htxf);
    }
    htxf
}

/// The file a preview came from, for its window's Download: the window
/// outlives the preview's transfer, and its connection can close, or its tab
/// connect elsewhere, while the window is open.
#[cfg(not(test))]
struct FullDownload {
    serial: u16,
    generation: u32,
    path: std::ffi::CString,
    dir: std::ffi::CString,
    name: Vec<u8>,
}

/// Give the preview window of `htxf` a Download that fetches the whole file,
/// as the file browser's would.
///
/// # Safety
/// `htxf` is a live transfer and `preview` its preview window.
#[cfg(not(test))]
pub(crate) unsafe fn offer_full_download(preview: *mut c_void, htxf: *mut c_void) {
    let h = &*(htxf as *const HtxfHandle);
    let d = Box::new(FullDownload {
        serial: gtkhx_core::conn::hx_conn_serial(h.htlc.cast()),
        generation: gtkhx_core::conn::generation(h.htlc.cast()),
        path: std::ffi::CStr::from_ptr(h.path.as_ptr()).into(),
        dir: std::ffi::CStr::from_ptr(h.remotedir.as_ptr()).into(),
        name: remote_name(h).to_vec(),
    });
    hx_preview_set_download_cb(
        preview,
        Some(full_download),
        Box::into_raw(d).cast(),
        Some(drop_full_download),
    );
}

#[cfg(not(test))]
unsafe extern "C" fn full_download(data: *mut c_void) -> glib::ffi::gboolean {
    let d = &*(data as *const FullDownload);
    let htlc = hx_conn_with_serial(d.serial);
    if htlc.is_null()
        || gtkhx_core::conn::hx_conn_fd(htlc.cast()) == 0
        || gtkhx_core::conn::generation(htlc.cast()) != d.generation
    {
        return glib::ffi::GFALSE;
    }
    xfer_new(
        htlc,
        d.path.as_ptr(),
        d.dir.as_ptr(),
        d.name.as_ptr().cast(),
        d.name.len(),
        XFER_GET.into(),
        0,
        0,
    );
    glib::ffi::GTRUE
}

#[cfg(not(test))]
unsafe extern "C" fn drop_full_download(data: *mut c_void) {
    drop(Box::from_raw(data as *mut FullDownload));
}

/// `struct htxf_conn *xfer_new_folder(struct htlc_conn *htlc, const char *path,
/// const char *remotedir, const char *remotename, gsize remotename_len, guint16
/// type)` — create a folder transfer on `htlc`. Flags `opt.folder` (so the
/// worker dispatcher picks the folder thread) and leaves the wire request to
/// the caller (hx_get_folder / hx_put_folder send GETFOLDER/PUTFOLDER
/// themselves) — which is why the two must agree on the connection.
///
/// # Safety
/// Same contract as [`xfer_new`]. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_new_folder(
    htlc: *mut c_void,
    path: *const c_char,
    remotedir: *const c_char,
    remotename: *const c_char,
    remotename_len: usize,
    type_: u16,
) -> *mut HtxfHandle {
    let htxf = xfer_init(
        htlc,
        cstr_bytes(path),
        cstr_bytes(remotedir),
        slice_bytes(remotename, remotename_len),
        type_,
        0,
    );
    hx_htxf_set_opt_folder(htxf, 1);
    htxf
}

/// `void post_file_update(struct htxf_conn *htxf)` — queue a tasks-window
/// progress update onto the main loop. Called from the transfer **worker thread**,
/// so it only touches thread-safe state: takes a ref (atomic) and posts through
/// hxbridge (`g_main_context_invoke`) — never the main-thread registry.
///
/// # Safety
/// `htxf` is a live handle; callable from any thread.
#[no_mangle]
pub unsafe extern "C" fn post_file_update(htxf: *mut HtxfHandle) {
    hx_htxf_ref(htxf); // held until fu_dispatch drops it
    gtkhx_bridge_post_to_main(Some(fu_dispatch), htxf as *mut c_void);
}

/// The queued progress dispatcher (main thread). Emits `file-update` unless the
/// transfer was since cancelled, then drops the ref `post_file_update` took.
///
/// # Safety
/// GLib idle on the main thread; `data` is the live htxf `post_file_update` ref'd.
unsafe extern "C" fn fu_dispatch(data: glib::ffi::gpointer) -> glib::ffi::gboolean {
    let htxf = data as *mut HtxfHandle;
    if hx_htxf_is_canceled(htxf) == 0 {
        gtkhx_session_emit_file_update(
            gtkhx_session_get_default(),
            hx_sess_from_htlc((*htxf).htlc),
            htxf as *mut c_void,
        );
    }
    hx_htxf_unref(htxf);
    glib::ffi::G_SOURCE_REMOVE
}

/// The transfer teardown (main-thread idle): unlink from the registry + drop the
/// worker's ref. Deferred by [`xfer_completion_entry`] so it runs after every
/// pending progress idle.
///
/// # Safety
/// GLib idle on the main thread; `data` is the completing htxf.
unsafe extern "C" fn xfer_cleanup_dispatch(data: glib::ffi::gpointer) -> glib::ffi::gboolean {
    let htxf = data as *mut HtxfHandle;
    WORKING.with_borrow_mut(|w| w.retain(|&e| e != htxf));
    if !settle_pause(htxf) {
        xfer_remove_from_list(htxf);
    }
    hx_htxf_unref(htxf); // drop the worker's ref
    glib::ffi::G_SOURCE_REMOVE
}

/// `void xfer_completion_entry(void *arg)` — worker→main completion (run by the
/// hxbridge spawn once the worker returns). Defers the teardown via `g_idle_add`
/// (always async, at `G_PRIORITY_DEFAULT_IDLE` — strictly below the file-update
/// idles' `G_PRIORITY_DEFAULT`), so cleanup runs AFTER every progress update the
/// worker already queued. Re-posting via the bridge would run synchronously on
/// this (main) thread and tear down too early — the ordering is load-bearing.
///
/// # Safety
/// Runs on the main thread; `arg` is the completing htxf (worker ref held).
#[no_mangle]
pub unsafe extern "C" fn xfer_completion_entry(arg: *mut c_void) {
    glib::ffi::g_idle_add(Some(xfer_cleanup_dispatch), arg);
}

// ---- worker dispatch + params ------------------------------------------

/// Close the hxnet HTXF channel and clear the slot. Idempotent: the worker closes
/// on completion, then [`htxf_destructor`] closes again on the last unref; the
/// NULL after the first close makes the second a no-op (`hxnet_htxf_close` is
/// NULL-safe), preventing a double-free.
///
/// # Safety
/// `htxf` is a live handle.
unsafe fn xfer_close_channel(htxf: *mut HtxfHandle) {
    hxnet_htxf_close((*htxf).hx as *mut HtxfConn);
    (*htxf).hx = std::ptr::null_mut();
}

/// The last-ref teardown, registered via `hx_htxf_set_destructor` and run by
/// `hx_htxf_unref` on a handle's final ref (just before `hx_htxf_free` releases
/// the cancellation token + the struct). Drops the per-htxf preview-window ref
/// (NULL / non-preview → no-op in `hx_preview_unref`) and closes the hxnet
/// channel.
///
/// # Safety
/// `htxf` is the handle being freed; runs on the thread doing the last unref
/// (the main thread in practice).
unsafe extern "C" fn htxf_destructor(htxf: *mut HtxfHandle) {
    // A folder download's count whose worker never ran: queued, then
    // cancelled or disconnected.
    take_folder_items(htxf);
    take_folder_get(htxf);
    // The preview window can outlive the transfer, and its close would cancel
    // this one: after a failed stream nothing ever told it the stream ended.
    // Both run on the main thread, so this can't race the close.
    hx_preview_set_cancel_cb((*htxf).preview, None, std::ptr::null_mut());
    hx_preview_unref((*htxf).preview);
    (*htxf).preview = std::ptr::null_mut();
    xfer_close_channel(htxf);
}

/// The per-chunk progress callback the hxnet::xfer worker calls (passed by value
/// in the params so the leaf hxnet crate never references a C symbol): bump the
/// byte counter (atomic) and post a tasks-window update. Runs on the worker
/// thread; `user_data` is the htxf.
///
/// # Safety
/// `user_data` is a live htxf; callable from the worker thread.
unsafe extern "C" fn xfer_progress_bump(user_data: *mut c_void, delta: u64) {
    let htxf = user_data as *mut HtxfHandle;
    hx_htxf_add_total_pos(htxf, delta);
    // Throttled: the count above is exact, and every worker posts a last
    // update however it ends.
    if progress_due(htxf as usize) {
        post_file_update(htxf);
    }
}

/// Fill an [`HxnetXferParams`] for a receive of `file_budget` bytes off `htxf`.
/// The preview hooks are the `hx_preview_*` view functions (they only ever get
/// handed back the `htxf->preview` read here). Solo-download only; the folder loop
/// builds its own preview-less per-file params.
unsafe fn xfer_recv_params(htxf: *mut HtxfHandle, file_budget: u64) -> HxnetXferParams {
    let h = &*htxf;
    HxnetXferParams {
        hx: h.hx as *mut HtxfConn,
        path: h.path.as_ptr(),
        file_budget,
        data_pos: h.data_pos,
        rsrc_pos: h.rsrc_pos,
        opt_preview: hx_htxf_opt_preview(htxf as *const c_void),
        opt_folder: hx_htxf_opt_folder(htxf as *const c_void),
        opt_large: hx_htxf_opt_large(htxf as *const c_void),
        preview: h.preview,
        user_data: htxf as *mut c_void,
        progress: Some(xfer_progress_bump),
        preview_chunk: Some(hx_preview_chunk),
        preview_set_info: Some(hx_preview_set_info),
        preview_done: Some(hx_preview_done),
        data_size: 0,
        rsrc_size: 0,
    }
}

/// Fill an [`HxnetXferParams`] for an upload (send) of `htxf`. No preview; the
/// send worker uses `data_size`/`rsrc_size` + the resume offsets. Solo-upload
/// only; the folder loop builds its own per-file params.
unsafe fn xfer_send_params(htxf: *mut HtxfHandle) -> HxnetXferParams {
    let h = &*htxf;
    HxnetXferParams {
        hx: h.hx as *mut HtxfConn,
        path: h.path.as_ptr(),
        file_budget: 0,
        data_pos: h.data_pos,
        rsrc_pos: h.rsrc_pos,
        opt_preview: 0,
        opt_folder: hx_htxf_opt_folder(htxf as *const c_void),
        opt_large: hx_htxf_opt_large(htxf as *const c_void),
        preview: std::ptr::null_mut(),
        user_data: htxf as *mut c_void,
        progress: Some(xfer_progress_bump),
        preview_chunk: None,
        preview_set_info: None,
        preview_done: None,
        data_size: h.data_size,
        rsrc_size: h.rsrc_size,
    }
}

/// Fill an [`HxnetFolderParams`] for a folder receive or send. The Rust folder
/// loop builds each per-file path from `base_path` itself, so `htxf->path` (the
/// tree root) is passed straight through and never mutated.
unsafe fn xfer_folder_params(htxf: *mut HtxfHandle) -> HxnetFolderParams {
    let h = &*htxf;
    HxnetFolderParams {
        hx: h.hx as *mut HtxfConn,
        base_path: h.path.as_ptr(),
        opt_preview: hx_htxf_opt_preview(htxf as *const c_void),
        opt_folder: hx_htxf_opt_folder(htxf as *const c_void),
        opt_large: hx_htxf_opt_large(htxf as *const c_void),
        user_data: htxf as *mut c_void,
        progress: Some(xfer_progress_bump),
    }
}

/// Solo download worker: connect the subchannel, run the single-file receive
/// loop, chime + stamp the final byte count on success. Always closes the channel
/// on the way out (the old `goto ret`). Cleanup (unlink + worker-ref drop) is
/// deferred to `xfer_completion_entry` on the main thread, after every queued
/// progress idle.
unsafe fn get_thread(htxf: *mut HtxfHandle) {
    if htxf_connect(htxf) != glib::ffi::GFALSE {
        let params = xfer_recv_params(htxf, (*htxf).total_size);
        // A preview that stopped at the part it shows didn't finish the file.
        if hxnet_xfer_file_recv_one(&params) == 0 {
            play_sound(FILE_DONE);
            hx_htxf_set_total_pos(htxf, (*htxf).total_size);
        }
    }
    // The last progress update, however it ended: the reports along the
    // way are throttled, so this one makes the row exact.
    post_file_update(htxf);
    xfer_close_channel(htxf);
}

/// The item counts servers announced for folder downloads, by transfer,
/// held from the reply until the download's worker hands its count to the
/// connection (`hxnet_htxf_set_folder_items`). Here rather than on the
/// transfer, whose layout C shares.
static FOLDER_ITEMS: std::sync::Mutex<Vec<(usize, u32)>> = std::sync::Mutex::new(Vec::new());

fn folder_items() -> std::sync::MutexGuard<'static, Vec<(usize, u32)>> {
    FOLDER_ITEMS.lock().unwrap_or_else(|p| p.into_inner())
}

/// Note the item count a folder download's reply announced.
pub(crate) fn remember_folder_items(htxf: *mut c_void, items: u32) {
    let mut all = folder_items();
    all.retain(|(h, _)| *h != htxf as usize);
    all.push((htxf as usize, items));
}

/// The count noted for `htxf`, forgotten as it's taken; 0 if none.
fn take_folder_items(htxf: *mut HtxfHandle) -> u32 {
    let mut all = folder_items();
    match all.iter().position(|(h, _)| *h == htxf as usize) {
        Some(i) => all.swap_remove(i).1,
        None => 0,
    }
}

/// What each folder download's workers have written, and whether the last
/// one finished: kept from its worker's return until the transfer is freed,
/// for its resume.
#[derive(Default)]
struct FolderGet {
    ledger: FolderLedger,
    finished: bool,
}

static FOLDER_GETS: std::sync::Mutex<Vec<(usize, FolderGet)>> = std::sync::Mutex::new(Vec::new());

fn take_folder_get(htxf: *mut HtxfHandle) -> FolderGet {
    let mut all = FOLDER_GETS.lock().unwrap_or_else(|p| p.into_inner());
    match all.iter().position(|(h, _)| *h == htxf as usize) {
        Some(i) => all.swap_remove(i).1,
        None => FolderGet::default(),
    }
}

fn put_folder_get(htxf: *mut HtxfHandle, get: FolderGet) {
    let mut all = FOLDER_GETS.lock().unwrap_or_else(|p| p.into_inner());
    all.push((htxf as usize, get));
}

fn folder_finished(htxf: *mut HtxfHandle) -> bool {
    let all = FOLDER_GETS.lock().unwrap_or_else(|p| p.into_inner());
    all.iter().any(|(h, g)| *h == htxf as usize && g.finished)
}

/// The bytes under `htxf`'s folder a resume of it would not ask for.
unsafe fn folder_held_on_disk(htxf: *mut HtxfHandle) -> u64 {
    let all = FOLDER_GETS.lock().unwrap_or_else(|p| p.into_inner());
    let Some((_, get)) = all.iter().find(|(h, _)| *h == htxf as usize) else {
        return 0;
    };
    let base = std::ffi::CStr::from_ptr((*htxf).path.as_ptr()).to_bytes();
    #[cfg(unix)]
    let base = {
        use std::os::unix::ffi::OsStrExt;
        std::path::Path::new(std::ffi::OsStr::from_bytes(base))
    };
    #[cfg(not(unix))]
    let Ok(base) = std::str::from_utf8(base).map(std::path::Path::new) else {
        return 0;
    };
    get.ledger.held(base)
}

/// Folder download worker: connect, drive the FILE_NEXT/FILE_SEND folder-receive
/// state machine (`hxnet::xfer::folder_recv` — builds each per-file path from
/// the root internally, never mutating it), answering each file as far as the
/// download's earlier workers got, chime + stamp on success.
unsafe fn folder_get_thread(htxf: *mut HtxfHandle) {
    let items = take_folder_items(htxf);
    let mut get = take_folder_get(htxf);
    get.finished = false;
    if htxf_connect(htxf) != glib::ffi::GFALSE {
        hxnet_htxf_set_folder_items((*htxf).hx as *mut HtxfConn, items);
        let params = xfer_folder_params(htxf);
        if folder_recv(&params, &mut get.ledger) == 0 {
            get.finished = true;
            play_sound(FILE_DONE);
            hx_htxf_set_total_pos(htxf, (*htxf).total_size);
        }
    }
    put_folder_get(htxf, get);
    // The last progress update, however it ended: the reports along the
    // way are throttled, so this one makes the row exact.
    post_file_update(htxf);
    xfer_close_channel(htxf);
}

/// See a sent upload all the way to the server, then chime — unless the
/// user cancelled meanwhile. Closing on the last write can lose the
/// upload's end; see hxnet_htxf_finish_send, which waits, within a limit,
/// for the server to close.
unsafe fn finish_upload(htxf: *mut HtxfHandle) {
    if hxnet_htxf_finish_send((*htxf).hx as *mut HtxfConn) != 0 && hx_htxf_is_canceled(htxf) == 0 {
        // Not necessarily a loss — a server that stops reading at the
        // declared size can reset rather than close — but worth a trace
        // when one keeps the worker waiting.
        let msg = std::ffi::CString::new(format!(
            "upload ref={}: the server didn't close cleanly after it",
            (*htxf).ref_
        ))
        .unwrap_or_default();
        debug_log_str(c"xfer".as_ptr(), msg.as_ptr());
    }
    if hx_htxf_is_canceled(htxf) == 0 {
        play_sound(FILE_DONE);
    }
}

/// Solo upload worker: connect, run the single-file send loop, see it all
/// reach the server, chime on success.
unsafe fn put_thread(htxf: *mut HtxfHandle) {
    let sent = htxf_connect(htxf) != glib::ffi::GFALSE && {
        let params = xfer_send_params(htxf);
        hxnet_xfer_file_send_one(&params) == 0
    };
    // The last progress update, however it went — the reports along the
    // way are throttled — and before the wait for the server, so a sent
    // upload's row completes now.
    post_file_update(htxf);
    if sent {
        finish_upload(htxf);
    }
    xfer_close_channel(htxf);
}

/// Folder upload worker: connect, walk the local tree responding to the server's
/// FILE_NEXT loop (`hxnet_xfer_folder_send_all`), chime + stamp on success.
unsafe fn folder_put_thread(htxf: *mut HtxfHandle) {
    let sent = htxf_connect(htxf) != glib::ffi::GFALSE && {
        let params = xfer_folder_params(htxf);
        hxnet_xfer_folder_send_all(&params) == 0
    };
    if sent {
        hx_htxf_set_total_pos(htxf, (*htxf).total_size);
    }
    // As put_thread: the last update, before the wait for the server.
    post_file_update(htxf);
    if sent {
        finish_upload(htxf);
    }
    xfer_close_channel(htxf);
}

/// Worker entry on hxbridge's tokio blocking pool. Dispatches to the right
/// transfer body on `opt.folder` × `type` and returns; cleanup happens separately
/// in `xfer_completion_entry` once this returns. Runs OFF the main thread, so it
/// never touches GTK directly — only marshals via `post_file_update`.
///
/// # Safety
/// `arg` is a live htxf; runs on the blocking pool.
unsafe extern "C" fn xfer_worker_entry(arg: *mut c_void) {
    let htxf = arg as *mut HtxfHandle;
    let folder = hx_htxf_opt_folder(htxf as *const c_void) != 0;
    let is_get = (*htxf).type_ == XFER_GET;
    match (folder, is_get) {
        (true, true) => folder_get_thread(htxf),
        (true, false) => folder_put_thread(htxf),
        (false, true) => get_thread(htxf),
        (false, false) => put_thread(htxf),
    }
}

/// `void xfer_ready_write(struct htxf_conn *htxf)` — hand a transfer to the
/// blocking pool. Takes the worker's refcount ref BEFORE the spawn (so the htxf
/// can't be freed mid-spawn if another path drops the list ref);
/// `xfer_completion_entry` drops it once the worker returns. Called from the Rust
/// receive handlers (recv/xfer.rs) once the server signals ready.
///
/// # Safety
/// `htxf` is a live handle. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_ready_write(htxf: *mut HtxfHandle) {
    // mhxd repeats a transfer's queue position, 0, once it has started. That
    // must start neither a second worker nor one for a paused download, whose
    // old reference it names, until the server has answered the resume.
    if pause_of(htxf).is_some() || WORKING.with_borrow(|w| w.contains(&htxf)) {
        return;
    }
    WORKING.with_borrow_mut(|w| w.push(htxf));
    hx_htxf_ref(htxf); // the worker's ref
    gtkhx_bridge_spawn_blocking_with_idle(
        xfer_worker_entry,
        xfer_completion_entry,
        htxf as *mut c_void,
    );
    // Pause is offered from now, not from the first bytes.
    emit_file_update(htxf);
}

// ---- xfer_go wire build + local-path helpers ---------------------------

/// `int local_path_exists_adapter(const char *path, void *user_data)` — the
/// exists predicate handed to `uniquify_path`: a local path is taken if either
/// fork exists (data fork via `stat`, resource fork via `resource_len`). Matches
/// the C `local_path_exists` + adapter.
///
/// # Safety
/// `path` is a NUL-terminated C string; `user_data` is ignored.
unsafe extern "C" fn local_path_exists_adapter(
    path: *const c_char,
    _user_data: *mut c_void,
) -> c_int {
    if hx_file_size(path) >= 0 || resource_len(path) > 0 {
        1
    } else {
        0
    }
}

/// Rewrite `path` (a `[c_char; N]` buffer) in place to a collision-free name,
/// plugging the real filesystem predicate into the C `uniquify_path` core.
unsafe fn uniquify_local_path(path: *mut c_char, cap: usize) {
    uniquify_path(
        path,
        cap,
        Some(local_path_exists_adapter),
        std::ptr::null_mut(),
    );
}

/// `htxf`'s `remotename` as it goes on the wire: a download's as the listing
/// gave it, an upload's as `send::files` encoded it.
unsafe fn remote_name<'a>(htxf: *const HtxfHandle) -> &'a [u8] {
    std::slice::from_raw_parts(
        (*htxf).remotename.as_ptr().cast::<u8>(),
        (*htxf).remotename_len as usize,
    )
}

/// Is `htxf->remotedir` a real parent directory (non-empty and not just "/")?
unsafe fn remotedir_present(htxf: *const HtxfHandle) -> bool {
    let rd = &(*htxf).remotedir;
    rd[0] != 0 && !(rd[0] == b'/' as c_char && rd[1] == 0)
}

/// Borrow a `path_to_hldir` result as the DIR chunk bytes: `None` when there's
/// no parent dir, an empty slice when the encode produced nothing, else the
/// `hldirlen` bytes at `*hldir`. Taking `hldir` by reference ties the returned
/// slice's lifetime to that local pointer variable (freed at the end of the
/// caller's scope), so — unlike a plain `fn(...) -> &'a [u8]` — the slice can't
/// outlive the allocation.
///
/// # Safety
/// When `has_dir`, `*hldir` is NULL or valid for `hldirlen` bytes.
unsafe fn hldir_dir_bytes(hldir: &*mut u8, hldirlen: u16, has_dir: bool) -> Option<&[u8]> {
    if !has_dir {
        None
    } else if hldir.is_null() || hldirlen == 0 {
        Some(&[])
    } else {
        Some(std::slice::from_raw_parts(*hldir, hldirlen as usize))
    }
}

/// The download half of [`xfer_go`]: resume-vs-rename decision, then the
/// FILE_NAME / DIR / (resume) RFLT chunk build + FILE_GET request.
unsafe fn xfer_go_get(htxf: *mut HtxfHandle, kept: Option<Disk>) {
    let mut resuming = false;

    // Resume vs rename (skipped for previews, which don't write to disk):
    //   local absent            → fresh download
    //   local is what a pause kept, or < srv (known) → resume from local size
    //   otherwise (>= / unknown)→ rename to a free name
    if hx_htxf_opt_preview(htxf as *const c_void) == 0 {
        let sz = hx_file_size((*htxf).path.as_ptr());
        if sz >= 0 {
            let local_data = sz as u32; // C's (guint32)sb.st_size truncation
            let ours = kept.map(|k| k.len) == Some(sz) && sz <= i64::from(u32::MAX);
            if ours && sz == 0 {
                // Paused before any data came: start over into the same file.
            } else if ours
                || (*htxf).srv_data_size > 0 && (local_data as u64) < (*htxf).srv_data_size
            {
                (*htxf).data_pos = local_data as u64;
                (*htxf).rsrc_pos = resource_len((*htxf).path.as_ptr()) as u64;
                resuming = true;
            } else {
                let cap = (*htxf).path.len();
                uniquify_local_path((*htxf).path.as_mut_ptr(), cap);
            }
        }
    }

    let rflt = if resuming {
        hxrequest::files::resume_rflt((*htxf).data_pos as u32, (*htxf).rsrc_pos as u32)
    } else {
        [0u8; 74]
    };

    let htlc = (*htxf).htlc;
    let has_dir = remotedir_present(htxf);
    {
        let nm_wire = remote_name(htxf);
        let mut hldir: *mut u8 = std::ptr::null_mut();
        let mut hldirlen: u16 = 0;
        if has_dir {
            hldir = path_to_hldir((*htxf).remotedir.as_ptr(), &mut hldirlen, 0);
        }
        let req = FileGetRequest {
            name: nm_wire,
            dir: hldir_dir_bytes(&hldir, hldirlen, has_dir),
            rflt: resuming.then_some(&rflt[..]),
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build::build_file_get_chunks(&req, &mut chunks);
        if hc > 0 {
            expect_transfer(
                htlc.cast(),
                Asked::Download {
                    htxf: crate::recv::files::Xfer::new(htxf.cast()),
                    folder: false,
                },
            );
            hlwrite_chunks(
                htlc.cast(),
                HTLC_HDR_FILE_GET,
                0,
                chunks.as_ptr(),
                hc as c_int,
            );
        }
        if !hldir.is_null() {
            glib::ffi::g_free(hldir as *mut c_void);
        }
    }
}

/// The upload half of [`xfer_go`]: FILE_NAME / DIR / FILE_PREVIEW / HTXF_SIZE /
/// (large) XFERSIZE64 chunk build + FILE_PUT request. `resume` is the whole
/// upload's size when it resumes one the server holds part of.
unsafe fn xfer_go_put(htxf: *mut HtxfHandle, resume: Option<u64>) {
    let size = resume.unwrap_or((*htxf).total_size);
    // Legacy 32-bit HTXF_SIZE clamps to 0xFFFFFFFF; the 64-bit companion carries
    // the true size when CAP_LARGE_FILES was negotiated.
    let size_host = size.min(0xFFFF_FFFF) as u32;
    let htlc = (*htxf).htlc;
    let large = hx_conn_has_cap(htlc.cast(), HTLC_CAP_LARGE_FILES) != glib::ffi::GFALSE;
    let has_dir = remotedir_present(htxf);
    {
        let nm_wire = remote_name(htxf);
        let mut hldir: *mut u8 = std::ptr::null_mut();
        let mut hldirlen: u16 = 0;
        if has_dir {
            hldir = path_to_hldir((*htxf).remotedir.as_ptr(), &mut hldirlen, 0);
        }
        let req = FilePutRequest {
            name: nm_wire,
            dir: hldir_dir_bytes(&hldir, hldirlen, has_dir),
            // FILE_PREVIEW marks an upload that resumes a file already on the
            // server: only one this upload left there when it was paused.
            has_preview: resume.is_some(),
            size: size_host,
            size64: large.then_some(size),
        };
        let mut chunks = [HxChunk::EMPTY; 5];
        let mut scratch = [0u8; 12];
        let hc = build::build_file_put_chunks(&req, &mut chunks, &mut scratch);
        if hc > 0 {
            expect_transfer(
                htlc.cast(),
                Asked::Upload {
                    htxf: crate::recv::files::Xfer::new(htxf.cast()),
                    folder: false,
                },
            );
            hlwrite_chunks(
                htlc.cast(),
                HTLC_HDR_FILE_PUT,
                0,
                chunks.as_ptr(),
                hc as c_int,
            );
        }
        if !hldir.is_null() {
            glib::ffi::g_free(hldir as *mut c_void);
        }
    }
}

/// `void xfer_go(struct htxf_conn *htxf)` — send the download/upload request that
/// starts a (non-folder) transfer. Latches `gone` so a queued transfer's
/// auto-start + an explicit kick can't double-fire, then dispatches on `type`.
/// A settled pause is resumed; one still winding down is left alone.
/// Called from tasks.c (queue restart) and natively (the retry timer + the
/// next-in-queue kick after a removal).
///
/// # Safety
/// `htxf` is a live handle. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_go(htxf: *mut HtxfHandle) {
    // A folder transfer's request is its own (GETFOLDER / PUTFOLDER), sent
    // when it was made; the queue moving on must not send it a FILE_GET.
    if hx_htxf_opt_folder(htxf as *const c_void) != 0 {
        resume_folder(htxf);
        return;
    }
    let kept = match pause_of(htxf) {
        Some(Pause::Paused { kept }) => Some(kept),
        Some(_) => return,
        None => None,
    };
    if let Some(kept) = kept {
        if (*htxf).type_ != XFER_GET && kept != disk((*htxf).path.as_ptr()) {
            set_pause(htxf, Some(Pause::Changed { kept }));
            emit_file_update(htxf);
            return;
        }
    }
    // The whole file's size, which a resume's estimate is cut from until the
    // server says what is left.
    let full = kept.map(|_| (*htxf).total_size + xfer_resumed_bytes(htxf));
    if let Some(kept) = kept {
        set_pause(htxf, Some(Pause::Asked { kept }));
        let h = &mut *htxf;
        (h.gone, h.data_pos, h.rsrc_pos) = (0, 0, 0);
        hx_htxf_set_total_pos(htxf, 0);
    }
    if (*htxf).gone != 0 {
        return;
    }
    (*htxf).gone = 1;

    if (*htxf).type_ == XFER_GET {
        xfer_go_get(htxf, kept);
        if let Some(full) = full {
            (*htxf).total_size = full.saturating_sub(xfer_resumed_bytes(htxf)).max(1);
            emit_file_update(htxf);
        }
    } else {
        xfer_go_put(htxf, full);
        if let Some(full) = full {
            (*htxf).total_size = full.max(1);
            emit_file_update(htxf);
        }
    }
}

/// Ask again for a paused folder download, which answers each file the
/// server offers as far as it got. Nothing for any other folder transfer.
///
/// # Safety
/// `htxf` is a live folder transfer. Main thread only.
unsafe fn resume_folder(htxf: *mut HtxfHandle) {
    let Some(Pause::Paused { kept }) = pause_of(htxf) else {
        return;
    };
    set_pause(htxf, Some(Pause::Asked { kept }));
    set_held(htxf, Some(folder_held_on_disk(htxf)));
    hx_htxf_set_total_pos(htxf, 0);
    crate::send::files::ask_folder(
        (*htxf).htlc,
        htxf.cast(),
        &carr_read(&(*htxf).remotedir),
        remote_name(htxf),
    );
    emit_file_update(htxf);
}

/// `int xfer_go_timer(void *arg)` — the one-shot GLib-timer trampoline used by the
/// download retry path (recv/xfer.rs arms it with the htxf). Fires `xfer_go` once
/// and removes itself.
///
/// # Safety
/// GLib timer callback on the main thread; `arg` is the transfer's htxf.
#[no_mangle]
pub unsafe extern "C" fn xfer_go_timer(arg: *mut c_void) -> c_int {
    xfer_go(arg as *mut HtxfHandle);
    0
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
// The xfer_go wire-build collaborators: the local file's size is the test's to
// set, and each request written is recorded as its chunks.
#[cfg(test)]
thread_local! {
    static LOCAL_SIZE: std::cell::Cell<i64> = const { std::cell::Cell::new(-1) };
    static WRITTEN: RefCell<Vec<Vec<Vec<u8>>>> = const { RefCell::new(Vec::new()) };
    static WORKERS: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
    /// The transfers flagged as folders.
    static FOLDERS: RefCell<Vec<usize>> = const { RefCell::new(Vec::new()) };
}
#[cfg(test)]
unsafe fn hx_conn_has_cap(_h: *const c_void, _cap: u64) -> glib::ffi::gboolean {
    glib::ffi::GFALSE
}
#[cfg(test)]
unsafe fn resource_len(_path: *const c_char) -> usize {
    0
}
#[cfg(test)]
unsafe fn hlwrite_chunks(
    _htlc: *mut c_void,
    _ty: u32,
    _flag: u32,
    chunks: *const HxChunk,
    hc: c_int,
) {
    let req = (0..hc as usize)
        .map(|i| {
            let c = &*chunks.add(i);
            if c.data.is_null() {
                Vec::new()
            } else {
                std::slice::from_raw_parts(c.data, c.len as usize).to_vec()
            }
        })
        .collect();
    WRITTEN.with_borrow_mut(|w| w.push(req));
}
#[cfg(test)]
unsafe fn uniquify_path(
    _path: *mut c_char,
    _cap: usize,
    _exists: Option<unsafe extern "C" fn(*const c_char, *mut c_void) -> c_int>,
    _user_data: *mut c_void,
) {
}
#[cfg(test)]
unsafe fn hx_file_size(_path: *const c_char) -> i64 {
    LOCAL_SIZE.get()
}
#[cfg(test)]
unsafe fn hx_prefs_queuedl() -> c_int {
    1
}
#[cfg(test)]
unsafe fn hx_preview_unref(_p: *mut c_void) {}
#[cfg(test)]
unsafe fn hx_preview_set_cancel_cb(
    _p: *mut c_void,
    _f: Option<unsafe extern "C" fn(*mut c_void)>,
    _user_data: *mut c_void,
) {
}
#[cfg(test)]
unsafe fn hx_htxf_set_opt_preview(_htxf: *mut HtxfHandle, _v: c_int) {}
#[cfg(test)]
unsafe fn hx_htxf_set_opt_folder(htxf: *mut HtxfHandle, v: c_int) {
    FOLDERS.with_borrow_mut(|f| {
        f.retain(|&e| e != htxf as usize);
        if v != 0 {
            f.push(htxf as usize);
        }
    });
}
#[cfg(test)]
unsafe fn gtkhx_bridge_post_to_main(_func: glib::ffi::GSourceFunc, _user_data: *mut c_void) {}
#[cfg(test)]
unsafe fn gtkhx_bridge_spawn_blocking_with_idle(
    _worker: unsafe extern "C" fn(*mut c_void),
    _completion: unsafe extern "C" fn(*mut c_void),
    _user_data: *mut c_void,
) {
    WORKERS.set(WORKERS.get() + 1);
}
#[cfg(test)]
unsafe fn htxf_connect(_htxf: *mut HtxfHandle) -> glib::ffi::gboolean {
    glib::ffi::GFALSE
}
#[cfg(test)]
unsafe fn play_sound(_sound: c_int) {}
#[cfg(test)]
unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}
#[cfg(test)]
unsafe extern "C" fn hx_preview_chunk(_preview: *mut c_void, _buf: *const c_char, _len: usize) {}
#[cfg(test)]
unsafe extern "C" fn hx_preview_set_info(
    _preview: *mut c_void,
    _type_: *const c_char,
    _creator: *const c_char,
    size: u64,
) -> u64 {
    size
}
#[cfg(test)]
unsafe extern "C" fn hx_preview_done(_preview: *mut c_void) {}
#[cfg(test)]
unsafe fn hx_htxf_opt_preview(_htxf: *const c_void) -> c_int {
    0
}
#[cfg(test)]
unsafe fn hx_htxf_opt_folder(htxf: *const c_void) -> c_int {
    FOLDERS.with_borrow(|f| f.contains(&(htxf as usize))) as c_int
}
#[cfg(test)]
unsafe fn hx_htxf_opt_large(_htxf: *const c_void) -> c_int {
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::send::files::XFER_PUT;

    // Dummy, never-dereferenced handles for the pure reorder/index logic (up /
    // down / num operate on pointer identity only — no field reads / externs).
    fn dummy(n: usize) -> *mut HtxfHandle {
        (0x1000 + n) as *mut HtxfHandle
    }

    fn reset(handles: &[*mut HtxfHandle]) {
        XFERS.with(|x| *x.borrow_mut() = handles.to_vec());
        PAUSES.with_borrow_mut(Vec::clear);
        WORKING.with_borrow_mut(Vec::clear);
        QUEUED.with_borrow_mut(Vec::clear);
        WRITTEN.with_borrow_mut(Vec::clear);
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

    unsafe fn new_download() -> *mut HtxfHandle {
        xfer_new(
            std::ptr::null_mut(),
            c"/tmp/f".as_ptr(),
            c"/".as_ptr(),
            c"f".as_ptr(),
            1,
            XFER_GET.into(),
            0,
            0,
        )
    }

    /// A download the server answered unqueued, its worker running partway.
    unsafe fn running() -> *mut HtxfHandle {
        let h = new_download();
        assert_eq!((*h).gone, 1, "started");
        ((*h).ref_, (*h).queue, (*h).total_size) = (7, 0, 5000);
        hx_htxf_set_total_pos(h, 1200);
        xfer_ready_write(h);
        h
    }

    #[test]
    fn a_download_waiting_its_turn_has_no_place_and_the_listings_size() {
        reset(&[]);
        unsafe {
            running();
            let waiting = xfer_new(
                std::ptr::null_mut(),
                c"/tmp/g".as_ptr(),
                c"/".as_ptr(),
                c"g".as_ptr(),
                1,
                XFER_GET.into(),
                0,
                4096,
            );
            assert_eq!((*waiting).gone, 0, "behind the running one");
            assert_eq!((*waiting).queue, 0, "no place in a server's queue");
            assert_eq!((*waiting).total_size, 4096);
        }
    }

    /// Pause `h` with `kept` bytes on disk, and let its worker finish.
    unsafe fn pause_settled(h: *mut HtxfHandle, kept: i64) {
        xfer_pause(h);
        LOCAL_SIZE.set(kept);
        xfer_cleanup_dispatch(h.cast());
    }

    fn requests() -> usize {
        WRITTEN.with_borrow(Vec::len)
    }

    /// The data offset the last request resumes at, if it carries an RFLT.
    fn resumed_at() -> Option<u32> {
        WRITTEN.with_borrow(|w| {
            w.last()?
                .iter()
                .find(|c| c.starts_with(b"RFLT"))
                .map(|c| u32::from_be_bytes(c[46..50].try_into().unwrap()))
        })
    }

    #[test]
    fn a_paused_download_stays_listed_and_out_of_the_queue() {
        reset(&[]);
        unsafe {
            let r = running();
            let a = new_download();
            assert_eq!((*a).gone, 0, "queued behind a running download");
            // mhxd repeats the queue position, 0, once a transfer has started.
            let workers = WORKERS.get();
            xfer_ready_write(r);
            assert_eq!(WORKERS.get(), workers, "one worker to a transfer");

            xfer_pause(r);
            assert_eq!(xfer_paused(r), 1);
            assert_eq!(xfer_can_pause(r), 0);
            // Still winding down: Start waits for the worker, and the queue
            // waits for its connection to close.
            assert_eq!(xfer_can_start(r), 0);
            assert_eq!((*a).gone, 0, "nothing new asked for yet");
            let asked = requests();
            xfer_go(r);
            assert_eq!(requests(), asked);
            xfer_ready_write(r);
            assert_eq!(WORKERS.get(), workers, "nor one to a paused transfer");

            LOCAL_SIZE.set(1000);
            xfer_cleanup_dispatch(r.cast());
            assert_eq!(xfer_num(r), 0, "kept in the list");
            assert_eq!(xfer_paused(r), 1);
            assert_eq!(xfer_can_start(r), 1);
            assert_eq!((*a).gone, 1, "the queue moves on");

            xfer_remove_from_list(a);
            assert_eq!(xfer_paused(r), 1, "a transfer ending doesn't resume it");
            assert_eq!(requests(), asked + 1);
            let b = new_download();
            assert_eq!((*b).gone, 1, "a paused download holds nothing back");
        }
    }

    #[test]
    fn a_refused_download_the_queue_started_is_queued_again() {
        reset(&[]);
        unsafe {
            let r = running();
            let a = new_download();
            pause_settled(r, 1000);
            assert_eq!((*a).gone, 1, "the queue started it");
            for _ in 0..QUEUE_TRIES {
                assert!(queued_refused(a.cast()));
                assert_eq!((*a).gone, 0, "queued again");
                assert_eq!(xfer_num(a), 1);
                start_next();
                assert_eq!((*a).gone, 1, "and asked for again");
            }
            assert!(!queued_refused(a.cast()), "until the tries are spent");

            // Once answered, a later refusal isn't the queue's to retry.
            let b = new_download();
            xfer_remove_from_list(a);
            assert_eq!((*b).gone, 1);
            answered(b.cast());
            assert!(!queued_refused(b.cast()));
            // Nor one the user started.
            let c = new_download();
            xfer_go(c);
            assert!(!queued_refused(c.cast()));
            // Nor a folder download, whose request the queue never sends.
            let f = xfer_new_folder(
                std::ptr::null_mut(),
                c"/tmp/d".as_ptr(),
                c"/".as_ptr(),
                c"d".as_ptr(),
                1,
                XFER_GET.into(),
            );
            xfer_remove_from_list(b);
            xfer_remove_from_list(c);
            assert_eq!(xfer_num(f), 1, "next up after the paused one");
            assert!(!queued_refused(f.cast()));
        }
    }

    #[test]
    fn a_download_that_finished_as_it_paused_is_done() {
        reset(&[]);
        unsafe {
            let r = running();
            xfer_pause(r);
            hx_htxf_set_total_pos(r, 5000);
            xfer_cleanup_dispatch(r.cast());
            assert_eq!(xfer_num(r), -1);
        }
    }

    #[test]
    fn a_resume_asks_from_what_the_pause_kept() {
        // (local size at resume, the listing's size, offset asked)
        for (local, srv, want) in [
            (1000, 0, Some(1000)),    // as the pause left it
            (1000, 5000, Some(1000)), // the same, the server's size known
            (1500, 0, None),          // changed while paused: renamed
            (1500, 5000, Some(1500)), // changed, but still short of the server's
            (-1, 5000, None),         // removed while paused
        ] {
            reset(&[]);
            unsafe {
                let r = running();
                (*r).srv_data_size = srv;
                pause_settled(r, 1000);
                LOCAL_SIZE.set(local);
                xfer_go(r);
                assert_eq!(resumed_at(), want, "local {local}, server {srv}");
                assert_eq!((*r).data_pos, u64::from(want.unwrap_or(0)));
                assert_eq!(xfer_paused(r), 0);
                assert_eq!(hx_htxf_total_pos(r), 0);
                assert_eq!(
                    (*r).total_size + xfer_resumed_bytes(r),
                    5000,
                    "the row still measures the whole file"
                );
            }
        }
    }

    #[test]
    fn a_download_paused_before_any_data_starts_over_in_place() {
        reset(&[]);
        unsafe {
            let r = running();
            pause_settled(r, 0);
            xfer_go(r);
            assert_eq!(resumed_at(), None);
            assert_eq!(xfer_paused(r), 0);
        }
    }

    #[test]
    fn a_refused_resume_is_paused_again_and_an_answered_one_is_not() {
        reset(&[]);
        unsafe {
            let r = running();
            pause_settled(r, 1000);
            xfer_go(r);
            let a = new_download();
            assert_eq!((*a).gone, 0, "queued behind the resume");
            assert!(resume_refused(r.cast()));
            assert_eq!(xfer_paused(r), 1);
            assert_eq!(xfer_can_start(r), 1);
            assert_eq!((*a).gone, 1, "the queue moves on");

            xfer_go(r);
            answered(r.cast());
            assert!(!resume_refused(r.cast()));
            assert_eq!(xfer_paused(r), 0);
        }
    }

    /// A folder download the server answered, its worker running.
    unsafe fn running_folder() -> *mut HtxfHandle {
        let f = xfer_new_folder(
            std::ptr::null_mut(),
            c"/tmp/d".as_ptr(),
            c"/pub".as_ptr(),
            c"d".as_ptr(),
            1,
            XFER_GET.into(),
        );
        ((*f).ref_, (*f).total_size) = (9, 5000);
        hx_htxf_set_total_pos(f, 1200);
        xfer_ready_write(f);
        f
    }

    fn folder_requests() -> Vec<crate::send::files::tests::Chunks> {
        crate::send::files::tests::sends()
            .into_iter()
            .filter(|(ty, _)| *ty == ClientHdr::FileGetFolder as u32)
            .map(|(_, chunks)| chunks)
            .collect()
    }

    #[test]
    fn a_paused_folder_download_is_asked_for_again() {
        reset(&[]);
        folder_requests();
        unsafe {
            let f = running_folder();
            let a = new_download();
            assert_eq!((*a).gone, 0, "queued behind the folder");
            assert_eq!(xfer_can_start(f), 0, "its request went when it was made");
            assert_eq!(xfer_can_pause(f), 1);

            xfer_pause(f);
            assert_eq!(xfer_can_start(f), 0, "winding down");
            xfer_cleanup_dispatch(f.cast());
            assert_eq!(xfer_num(f), 0, "kept in the list");
            assert_eq!(xfer_paused(f), 1);
            assert_eq!(xfer_can_start(f), 1);
            assert_eq!((*a).gone, 1, "the queue moves on");
            assert!(folder_requests().is_empty(), "nor asked for again by it");

            xfer_go(f);
            assert_eq!(xfer_paused(f), 0);
            assert_eq!(hx_htxf_total_pos(f), 0);
            let asked = folder_requests();
            assert_eq!(asked.len(), 1);
            assert!(asked[0].iter().any(|(_, v)| v.ends_with(b"d")), "{asked:?}");
            let workers = WORKERS.get();
            xfer_ready_write(f);
            assert_eq!(WORKERS.get(), workers, "not before the server answers");

            // A refused resume is paused again, not queued again.
            assert!(!queued_refused(f.cast()));
            assert!(resume_refused(f.cast()));
            assert_eq!(xfer_paused(f), 1);
            xfer_go(f);
            answered(f.cast());
            assert_eq!(xfer_paused(f), 0);
            xfer_ready_write(f);
            assert_eq!(WORKERS.get(), workers + 1, "one worker, once answered");
        }
    }

    /// An upload the server answered, its worker running partway.
    unsafe fn running_upload() -> *mut HtxfHandle {
        let u = xfer_new(
            std::ptr::null_mut(),
            c"/tmp/u".as_ptr(),
            c"/up".as_ptr(),
            c"u".as_ptr(),
            1,
            XFER_PUT,
            0,
            0,
        );
        ((*u).ref_, (*u).total_size) = (8, 5133);
        hx_htxf_set_total_pos(u, 1200);
        xfer_ready_write(u);
        u
    }

    /// Whether the last request claims a resume, and the size it gives:
    /// FILE_PREVIEW, when there, after the name and the folder, and the size
    /// last, with no 64-bit size.
    fn put_claims() -> (bool, u32) {
        WRITTEN.with_borrow(|w| {
            let req = w.last().unwrap();
            let size = req.last().unwrap()[..].try_into().unwrap();
            let claims = match req.len() {
                3 => false,
                4 => req[2] == [0, 1],
                n => panic!("{n} chunks"),
            };
            (claims, u32::from_be_bytes(size))
        })
    }

    #[test]
    fn a_paused_upload_is_asked_to_resume_and_a_new_one_is_not() {
        reset(&[]);
        unsafe {
            let u = running_upload();
            assert_eq!(put_claims(), (false, 1), "a new upload, as before");
            assert_eq!(xfer_can_pause(u), 1);
            pause_settled(u, 0);
            assert_eq!(xfer_paused(u), 1);
            assert_eq!(xfer_can_start(u), 1);

            xfer_go(u);
            assert_eq!(put_claims(), (true, 5133), "the whole upload's size");
            assert_eq!(xfer_paused(u), 0);
            assert_eq!((*u).total_size, 5133);
            answered(u.cast());
            assert_eq!(upload_resume_refused(u.cast()), None, "answered");
        }
    }

    #[test]
    fn a_refused_upload_resume_starts_over_and_then_waits_paused() {
        reset(&[]);
        unsafe {
            let u = running_upload();
            pause_settled(u, 0);
            xfer_go(u);
            let asked = requests();
            assert_eq!(upload_resume_refused(u.cast()), Some(true), "quietly");
            assert_eq!(requests(), asked + 1);
            assert_eq!(put_claims(), (false, 5133), "a new upload, from the start");
            assert_eq!(xfer_paused(u), 0);
            let workers = WORKERS.get();
            xfer_ready_write(u);
            assert_eq!(WORKERS.get(), workers, "not on the old reference");

            assert_eq!(upload_resume_refused(u.cast()), Some(false), "said why");
            assert_eq!(xfer_num(u), 0, "kept in the list");
            assert_eq!(xfer_paused(u), 1);
            assert_eq!(xfer_can_start(u), 1);
        }
    }

    #[test]
    fn an_upload_whose_file_changed_while_paused_stays_paused_saying_so() {
        reset(&[]);
        unsafe {
            let u = running_upload();
            pause_settled(u, 4000);
            LOCAL_SIZE.set(4100);
            let asked = requests();
            xfer_go(u);
            assert_eq!(requests(), asked, "neither resumed nor started over");
            assert_eq!(xfer_paused(u), 2);
            assert_eq!(xfer_can_start(u), 0);

            // Or the server holds more than the file has.
            let v = running_upload();
            pause_settled(v, 4000);
            xfer_go(v);
            assert!(upload_file_changed(v.cast()));
            assert_eq!(xfer_paused(v), 2);
            assert!(!upload_file_changed(v.cast()), "only a resumed upload");
        }
    }

    #[test]
    fn only_a_file_upload_an_rflt_can_place_pauses() {
        reset(&[]);
        unsafe {
            let u = running_upload();
            (*u).total_size = 5 << 30;
            assert_eq!(xfer_can_pause(u), 0, "past 4 GiB");
            let f = xfer_new_folder(
                std::ptr::null_mut(),
                c"/tmp/d".as_ptr(),
                c"/up".as_ptr(),
                c"d".as_ptr(),
                1,
                XFER_PUT,
            );
            xfer_ready_write(f);
            assert_eq!(xfer_can_pause(f), 0, "a folder's upload");
        }
    }

    #[test]
    fn a_folder_download_that_finished_as_it_paused_is_done() {
        reset(&[]);
        unsafe {
            let f = running_folder();
            xfer_pause(f);
            put_folder_get(
                f,
                FolderGet {
                    finished: true,
                    ..FolderGet::default()
                },
            );
            xfer_cleanup_dispatch(f.cast());
            assert_eq!(xfer_num(f), -1);
        }
    }
}
