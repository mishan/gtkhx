//! The file-transfer **registry** — the `xfers[]` list (ported from `xfers.c`),
//! Part of the xfers.c → Rust migration (`docs/rust/xfers-to-rust-scoping.md`).
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

use hotline_proto::build::{self, FileGetRequest, FilePutRequest, HxChunk};
use hotline_proto::messages::ClientHdr;
use hxnet::htxf::{hxnet_htxf_abort, hxnet_htxf_close, HtxfAbort, HtxfConn};
use hxnet::xfer::{
    hxnet_xfer_file_recv_one, hxnet_xfer_file_send_one, hxnet_xfer_folder_recv_all,
    hxnet_xfer_folder_send_all, HxnetFolderParams, HxnetXferParams,
};
use hxnet::xfer_handle::{
    hx_htxf_add_total_pos, hx_htxf_cancel, hx_htxf_is_canceled, hx_htxf_new, hx_htxf_ref,
    hx_htxf_set_destructor, hx_htxf_set_total_pos, hx_htxf_unref, HtxfHandle,
};

// Native collaborators for the xfer_go wire build (test build shadows them via the
// doubles below). hx_conn_has_cap is already Rust (gtkhx-core); the reply-task
// callbacks and the wire-encode helper are native intra-workspace calls.
#[cfg(not(test))]
use gtkhx_core::conn::hx_conn_has_cap;
#[cfg(not(test))]
use hxhfs::ffi::resource_len;
#[cfg(not(test))]
use hxtext::gtkhx_text_for_wire;
#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;
#[cfg(not(test))]
use hxtask::task_new;
#[cfg(not(test))]
use crate::recv::xfer::{rcv_task_file_get, rcv_task_file_put};

/// `FILE_DONE` (src/sound.h) — the transfer-complete chime.
const FILE_DONE: c_int = 3;
/// `XFER_GET` (src/protocol.h).
const XFER_GET: u8 = 0;
/// The download / upload request opcodes `xfer_go` writes (single source of
/// truth is the `hotline_proto::messages::ClientHdr` enum).
const HTLC_HDR_FILE_GET: u32 = ClientHdr::FileGet as u32;
const HTLC_HDR_FILE_PUT: u32 = ClientHdr::FilePut as u32;
/// The negotiated capability bits `xfer_go` reads — XFERSIZE64 emission
/// (`LARGE_FILES`) and wire text encoding (`TEXT_ENCODING`). These LOGIN cap bits
/// aren't modelled in hotline-proto; spelled here against the hotline.h reference.
const HTLC_CAP_LARGE_FILES: u64 = 0x0001;
const HTLC_CAP_TEXT_ENCODING: u64 = 0x0002;

/// The 74-byte resume `RFLT` blob `xfer_go` sends on a download resume, with the
/// two fork offsets big-endian-stamped at `[46..50]` (DATA) and `[62..66]` (MACR).
///
/// NOTE: these are the *exact* bytes the C string literal produced — its
/// continuation-line indentation leaked 26 leading spaces in and pushed the
/// "DATA"/"MACR" fork tags past byte 74, so the record is malformed (only the two
/// stamped offsets are meaningful). Reproduced byte-for-byte here to keep the
/// wire output identical; the latent bug is called out for a separate fix.
const RFLT_RESUME_TEMPLATE: [u8; 74] = [
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
    32, 32, 82, 70, 76, 84, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 32, 32, 32, 32, 32,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 0, 0,
];

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
    /// gtkhx_ui_bridge.c — the active connection's htlc (xfer_init stamps it).
    fn gtkhx_active_htlc() -> *mut c_void;
    /// gtkhx_ui_bridge.c — the queue-downloads pref (xfer_new's inline-vs-queue).
    fn hx_prefs_queuedl() -> c_int;
    /// xfers.c — the last-ref GTK/preview + channel teardown (still C);
    /// registered once via hx_htxf_set_destructor from xfer_init.
    fn htxf_destructor(htxf: *mut HtxfHandle);
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
    /// preview.c — the GTK preview-window feed. Reached only through the receive
    /// param callbacks; cast to the void*-first shape hxnet::xfer expects (they
    /// really take `hx_preview *`, ABI-identical to a leading pointer arg).
    fn hx_preview_chunk(preview: *mut c_void, buf: *const c_char, len: usize);
    fn hx_preview_set_info(preview: *mut c_void, type_: *const c_char, creator: *const c_char);
    fn hx_preview_done(preview: *mut c_void);
    /// htxf_accessors.c — read the opt bitfield bits (C owns the layout).
    /// `*const c_void` htxf to match recv/xfer.rs's existing declaration.
    fn hx_htxf_opt_preview(htxf: *const c_void) -> c_int;
    fn hx_htxf_opt_folder(htxf: *const c_void) -> c_int;
    fn hx_htxf_opt_large(htxf: *const c_void) -> c_int;

    // ---- xfer_go wire build + path collaborators ----
    /// path_hldir.c — encode a "/a/b" path to the wire DIR bytes (g_malloc'd
    /// buffer + out length; caller g_free's). is_file = 0 for a directory chunk.
    fn path_to_hldir(path: *const c_char, hldirlen: *mut u16, is_file: c_int) -> *mut u8;
    /// files.c — does a preview/companion already exist at this remote path?
    /// (Drives the FILE_PREVIEW marker on an upload.)
    fn exists_remote(path: *mut c_char) -> c_int;
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

/// (ptr, len) → borrowed bytes (empty for NULL). Names carry any byte incl.
/// `dir_char`, so they arrive as an explicit length, never NUL-terminated.
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
unsafe fn xfer_init(path: &[u8], remotedir: &[u8], remotename: &[u8], type_: u16) -> *mut HtxfHandle {
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
    h.queue = u32::MAX; // -1

    // Seed the list's ref (0 → 1), then enqueue.
    hx_htxf_ref(htxf);
    xfer_registry_add(htxf);

    h.htlc = gtkhx_active_htlc();
    h.total_size = 1;
    gtkhx_session_emit_file_update(
        gtkhx_session_get_default(),
        hx_sess_from_htlc(h.htlc),
        htxf as *mut c_void,
    );
    htxf
}

/// `struct htxf_conn *xfer_new(const char *path, const char *remotedir, const
/// char *remotename, gsize remotename_len, guint16 type, int preview, guint32
/// srv_data_size)` — create a download/upload transfer. Sets preview +
/// srv_data_size (which xfer_go gates its resume/rename on) BEFORE possibly
/// driving the wire request inline (only transfer, or queueing off).
///
/// # Safety
/// `path` / `remotedir` are NUL-terminated C strings (or NULL); `remotename` is
/// `remotename_len` bytes. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_new(
    path: *const c_char,
    remotedir: *const c_char,
    remotename: *const c_char,
    remotename_len: usize,
    type_: u16,
    preview: c_int,
    srv_data_size: u32,
) -> *mut HtxfHandle {
    let htxf = xfer_init(
        cstr_bytes(path),
        cstr_bytes(remotedir),
        slice_bytes(remotename, remotename_len),
        type_,
    );
    hx_htxf_set_opt_preview(htxf, if preview != 0 { 1 } else { 0 });
    (*htxf).srv_data_size = srv_data_size as u64;

    if xfer_count() == 1 || hx_prefs_queuedl() == 0 {
        xfer_go(htxf);
    }
    htxf
}

/// `struct htxf_conn *xfer_new_folder(const char *path, const char *remotedir,
/// const char *remotename, gsize remotename_len, guint16 type)` — create a folder
/// transfer. Flags `opt.folder` (so the worker dispatcher picks the folder
/// thread) and leaves the wire request to the caller (hx_get_folder /
/// hx_put_folder send GETFOLDER/PUTFOLDER themselves).
///
/// # Safety
/// Same contract as [`xfer_new`]. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_new_folder(
    path: *const c_char,
    remotedir: *const c_char,
    remotename: *const c_char,
    remotename_len: usize,
    type_: u16,
) -> *mut HtxfHandle {
    let htxf = xfer_init(
        cstr_bytes(path),
        cstr_bytes(remotedir),
        slice_bytes(remotename, remotename_len),
        type_,
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
    xfer_remove_from_list(htxf);
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

/// `void xfer_close_channel(struct htxf_conn *htxf)` — close the hxnet HTXF
/// channel and clear the slot. Idempotent: the worker closes on completion, then
/// htxf_destructor closes again on the last unref; the NULL after the first close
/// makes the second a no-op (`hxnet_htxf_close` is NULL-safe), preventing a
/// double-free. Still called by the C `htxf_destructor`.
///
/// # Safety
/// `htxf` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn xfer_close_channel(htxf: *mut HtxfHandle) {
    hxnet_htxf_close((*htxf).hx as *mut HtxfConn);
    (*htxf).hx = std::ptr::null_mut();
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
    post_file_update(htxf);
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
        if hxnet_xfer_file_recv_one(&params) == 0 {
            play_sound(FILE_DONE);
            hx_htxf_set_total_pos(htxf, (*htxf).total_size);
            post_file_update(htxf);
        }
    }
    xfer_close_channel(htxf);
}

/// Folder download worker: connect, drive the FILE_NEXT/FILE_SEND folder-receive
/// state machine (`hxnet_xfer_folder_recv_all` — builds each per-file path from
/// the root internally, never mutating it), chime + stamp on success.
unsafe fn folder_get_thread(htxf: *mut HtxfHandle) {
    if htxf_connect(htxf) != glib::ffi::GFALSE {
        let params = xfer_folder_params(htxf);
        if hxnet_xfer_folder_recv_all(&params) == 0 {
            play_sound(FILE_DONE);
            hx_htxf_set_total_pos(htxf, (*htxf).total_size);
            post_file_update(htxf);
        }
    }
    xfer_close_channel(htxf);
}

/// Solo upload worker: connect, run the single-file send loop, chime on success.
unsafe fn put_thread(htxf: *mut HtxfHandle) {
    if htxf_connect(htxf) != glib::ffi::GFALSE {
        let params = xfer_send_params(htxf);
        if hxnet_xfer_file_send_one(&params) == 0 {
            play_sound(FILE_DONE);
            post_file_update(htxf);
        }
    }
    xfer_close_channel(htxf);
}

/// Folder upload worker: connect, walk the local tree responding to the server's
/// FILE_NEXT loop (`hxnet_xfer_folder_send_all`), chime + stamp on success.
unsafe fn folder_put_thread(htxf: *mut HtxfHandle) {
    if htxf_connect(htxf) != glib::ffi::GFALSE {
        let params = xfer_folder_params(htxf);
        if hxnet_xfer_folder_send_all(&params) == 0 {
            play_sound(FILE_DONE);
            hx_htxf_set_total_pos(htxf, (*htxf).total_size);
            post_file_update(htxf);
        }
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
    hx_htxf_ref(htxf); // the worker's ref
    gtkhx_bridge_spawn_blocking_with_idle(
        xfer_worker_entry,
        xfer_completion_entry,
        htxf as *mut c_void,
    );
}

// ---- xfer_go wire build + local-path helpers ---------------------------

/// `int local_path_exists_adapter(const char *path, void *user_data)` — the
/// exists predicate handed to `uniquify_path`: a local path is taken if either
/// fork exists (data fork via `stat`, resource fork via `resource_len`). Matches
/// the C `local_path_exists` + adapter.
///
/// # Safety
/// `path` is a NUL-terminated C string; `user_data` is ignored.
unsafe extern "C" fn local_path_exists_adapter(path: *const c_char, _user_data: *mut c_void) -> c_int {
    if hx_file_size(path) >= 0 || resource_len(path) > 0 {
        1
    } else {
        0
    }
}

/// Rewrite `path` (a `[c_char; N]` buffer) in place to a collision-free name,
/// plugging the real filesystem predicate into the C `uniquify_path` core.
unsafe fn uniquify_local_path(path: *mut c_char, cap: usize) {
    uniquify_path(path, cap, Some(local_path_exists_adapter), std::ptr::null_mut());
}

/// Encode `htxf`'s `remotename` (explicit length, single-line field) for the wire
/// on this connection, run the bytes through `f`, then g_free the buffer. `f` must
/// not retain the slice past its own return.
unsafe fn with_name_wire<R>(
    htxf: *const HtxfHandle,
    utf8: glib::ffi::gboolean,
    f: impl FnOnce(&[u8]) -> R,
) -> R {
    let mut wire_len: usize = 0;
    let wire = gtkhx_text_for_wire(
        (*htxf).remotename.as_ptr(),
        (*htxf).remotename_len as usize,
        utf8,
        glib::ffi::GFALSE, // is_body = FALSE: filenames are single-line
        &mut wire_len,
    );
    let slice: &[u8] = if wire.is_null() || wire_len == 0 || wire_len > isize::MAX as usize {
        &[]
    } else {
        std::slice::from_raw_parts(wire as *const u8, wire_len)
    };
    let r = f(slice);
    if !wire.is_null() {
        glib::ffi::g_free(wire as *mut c_void);
    }
    r
}

/// Is `htxf->remotedir` a real parent directory (non-empty and not just "/")?
unsafe fn remotedir_present(htxf: *const HtxfHandle) -> bool {
    let rd = &(*htxf).remotedir;
    rd[0] != 0 && !(rd[0] == b'/' as c_char && rd[1] == 0)
}

/// The download half of [`xfer_go`]: resume-vs-rename decision, then the
/// FILE_NAME / DIR / (resume) RFLT chunk build + FILE_GET request.
unsafe fn xfer_go_get(htxf: *mut HtxfHandle) {
    let mut resuming = false;
    let mut rflt = RFLT_RESUME_TEMPLATE;

    // Resume vs rename (skipped for previews, which don't write to disk):
    //   local absent            → fresh download
    //   local < srv (known)     → resume from local size
    //   otherwise (>= / unknown)→ rename to a free name
    if hx_htxf_opt_preview(htxf as *const c_void) == 0 {
        let sz = hx_file_size((*htxf).path.as_ptr());
        if sz >= 0 {
            let local_data = sz as u32; // C's (guint32)sb.st_size truncation
            if (*htxf).srv_data_size > 0 && (local_data as u64) < (*htxf).srv_data_size {
                (*htxf).data_pos = local_data as u64;
                (*htxf).rsrc_pos = resource_len((*htxf).path.as_ptr()) as u64;
                resuming = true;
            } else {
                let cap = (*htxf).path.len();
                uniquify_local_path((*htxf).path.as_mut_ptr(), cap);
            }
        }
    }

    if resuming {
        // S32HTON truncates the u64 fork positions to 32-bit big-endian.
        rflt[46..50].copy_from_slice(&((*htxf).data_pos as u32).to_be_bytes());
        rflt[62..66].copy_from_slice(&((*htxf).rsrc_pos as u32).to_be_bytes());
    }

    let htlc = (*htxf).htlc;
    let utf8 = hx_conn_has_cap(htlc.cast(), HTLC_CAP_TEXT_ENCODING);
    let has_dir = remotedir_present(htxf);
    with_name_wire(htxf, utf8, |nm_wire| {
        let mut hldir: *mut u8 = std::ptr::null_mut();
        let mut hldirlen: u16 = 0;
        if has_dir {
            hldir = path_to_hldir((*htxf).remotedir.as_ptr(), &mut hldirlen, 0);
        }
        // Borrow the DIR bytes in this scope so the slice's lifetime is tied to
        // `hldir` (g_free'd at the end of this closure), never an unconstrained
        // one that could outlive the allocation.
        let dir: Option<&[u8]> = if has_dir && !hldir.is_null() && hldirlen != 0 {
            Some(std::slice::from_raw_parts(hldir, hldirlen as usize))
        } else if has_dir {
            Some(&[])
        } else {
            None
        };
        let req = FileGetRequest {
            name: nm_wire,
            dir,
            rflt: resuming.then_some(&rflt[..]),
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build::build_file_get_chunks(&req, &mut chunks);
        if hc > 0 {
            task_new(
                htlc.cast(),
                Some(rcv_task_file_get),
                htxf as *mut c_void,
                std::ptr::null_mut(),
                c"xfer_go".as_ptr(),
            );
            hlwrite_chunks(htlc.cast(), HTLC_HDR_FILE_GET, 0, chunks.as_ptr(), hc as c_int);
        }
        if !hldir.is_null() {
            glib::ffi::g_free(hldir as *mut c_void);
        }
    });
}

/// The upload half of [`xfer_go`]: FILE_NAME / DIR / FILE_PREVIEW / HTXF_SIZE /
/// (large) XFERSIZE64 chunk build + FILE_PUT request.
unsafe fn xfer_go_put(htxf: *mut HtxfHandle) {
    // Legacy 32-bit HTXF_SIZE clamps to 0xFFFFFFFF; the 64-bit companion carries
    // the true size when CAP_LARGE_FILES was negotiated.
    let size_host = (*htxf).total_size.min(0xFFFF_FFFF) as u32;
    let htlc = (*htxf).htlc;
    let large = hx_conn_has_cap(htlc.cast(), HTLC_CAP_LARGE_FILES) != glib::ffi::GFALSE;
    let has_dir = remotedir_present(htxf);
    let has_preview = exists_remote((*htxf).remotepath.as_ptr() as *mut c_char) != 0;
    let utf8 = hx_conn_has_cap(htlc.cast(), HTLC_CAP_TEXT_ENCODING);

    with_name_wire(htxf, utf8, |nm_wire| {
        let mut hldir: *mut u8 = std::ptr::null_mut();
        let mut hldirlen: u16 = 0;
        if has_dir {
            hldir = path_to_hldir((*htxf).remotedir.as_ptr(), &mut hldirlen, 0);
        }
        // Borrow the DIR bytes in this scope so the slice's lifetime is tied to
        // `hldir` (g_free'd at the end of this closure), never an unconstrained
        // one that could outlive the allocation.
        let dir: Option<&[u8]> = if has_dir && !hldir.is_null() && hldirlen != 0 {
            Some(std::slice::from_raw_parts(hldir, hldirlen as usize))
        } else if has_dir {
            Some(&[])
        } else {
            None
        };
        let req = FilePutRequest {
            name: nm_wire,
            dir,
            has_preview,
            size: size_host,
            size64: large.then_some((*htxf).total_size),
        };
        let mut chunks = [HxChunk::EMPTY; 5];
        let mut scratch = [0u8; 12];
        let hc = build::build_file_put_chunks(&req, &mut chunks, &mut scratch);
        if hc > 0 {
            task_new(
                htlc.cast(),
                Some(rcv_task_file_put),
                htxf as *mut c_void,
                std::ptr::null_mut(),
                c"xfer_go".as_ptr(),
            );
            hlwrite_chunks(htlc.cast(), HTLC_HDR_FILE_PUT, 0, chunks.as_ptr(), hc as c_int);
        }
        if !hldir.is_null() {
            glib::ffi::g_free(hldir as *mut c_void);
        }
    });
}

/// `void xfer_go(struct htxf_conn *htxf)` — send the download/upload request that
/// starts a (non-folder) transfer. Latches `gone` so a queued transfer's
/// auto-start + an explicit kick can't double-fire, then dispatches on `type`.
/// Called from tasks.c (queue restart) and natively (the retry timer + the
/// next-in-queue kick after a removal).
///
/// # Safety
/// `htxf` is a live handle. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn xfer_go(htxf: *mut HtxfHandle) {
    if (*htxf).gone != 0 {
        return;
    }
    (*htxf).gone = 1;

    if (*htxf).type_ == XFER_GET {
        xfer_go_get(htxf);
    } else {
        xfer_go_put(htxf);
    }
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
#[cfg(test)]
unsafe fn gtkhx_active_htlc() -> *mut c_void {
    std::ptr::null_mut()
}
// The xfer_go wire-build collaborators (xfer_go compiles under test but isn't invoked by
// the reorder/registry unit tests, so these are compile-only no-ops).
#[cfg(test)]
unsafe fn hx_conn_has_cap(_h: *const c_void, _cap: u64) -> glib::ffi::gboolean {
    glib::ffi::GFALSE
}
#[cfg(test)]
unsafe fn resource_len(_path: *const c_char) -> usize {
    0
}
#[cfg(test)]
unsafe fn gtkhx_text_for_wire(
    _text: *const c_char,
    _len: usize,
    _utf8: glib::ffi::gboolean,
    _is_body: glib::ffi::gboolean,
    out_len: *mut usize,
) -> *mut c_char {
    if !out_len.is_null() {
        *out_len = 0;
    }
    std::ptr::null_mut()
}
#[cfg(test)]
unsafe fn hlwrite_chunks(
    _htlc: *mut c_void,
    _ty: u32,
    _flag: u32,
    _chunks: *const HxChunk,
    _hc: c_int,
) {
}
#[cfg(test)]
unsafe fn task_new(
    _htlc: *mut c_void,
    _rcv: Option<unsafe extern "C" fn(*mut c_void, *const c_void, usize, *mut c_void, *mut c_void)>,
    _ptr: *mut c_void,
    _data: *mut c_void,
    _str_: *const c_char,
) -> *mut c_void {
    std::ptr::null_mut()
}
#[cfg(test)]
unsafe extern "C" fn rcv_task_file_get(
    _htlc: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    _ptr: *mut c_void,
    _data: *mut c_void,
) {
}
#[cfg(test)]
unsafe extern "C" fn rcv_task_file_put(
    _htlc: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    _ptr: *mut c_void,
    _data: *mut c_void,
) {
}
#[cfg(test)]
unsafe fn path_to_hldir(_path: *const c_char, hldirlen: *mut u16, _is_file: c_int) -> *mut u8 {
    if !hldirlen.is_null() {
        *hldirlen = 0;
    }
    std::ptr::null_mut()
}
#[cfg(test)]
unsafe fn exists_remote(_path: *mut c_char) -> c_int {
    0
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
    -1
}
#[cfg(test)]
unsafe fn hx_prefs_queuedl() -> c_int {
    0
}
#[cfg(test)]
unsafe extern "C" fn htxf_destructor(_htxf: *mut HtxfHandle) {}
#[cfg(test)]
unsafe fn hx_htxf_set_opt_preview(_htxf: *mut HtxfHandle, _v: c_int) {}
#[cfg(test)]
unsafe fn hx_htxf_set_opt_folder(_htxf: *mut HtxfHandle, _v: c_int) {}
#[cfg(test)]
unsafe fn gtkhx_bridge_post_to_main(_func: glib::ffi::GSourceFunc, _user_data: *mut c_void) {}
#[cfg(test)]
unsafe fn gtkhx_bridge_spawn_blocking_with_idle(
    _worker: unsafe extern "C" fn(*mut c_void),
    _completion: unsafe extern "C" fn(*mut c_void),
    _user_data: *mut c_void,
) {
}
#[cfg(test)]
unsafe fn htxf_connect(_htxf: *mut HtxfHandle) -> glib::ffi::gboolean {
    glib::ffi::GFALSE
}
#[cfg(test)]
unsafe fn play_sound(_sound: c_int) {}
#[cfg(test)]
unsafe extern "C" fn hx_preview_chunk(_preview: *mut c_void, _buf: *const c_char, _len: usize) {}
#[cfg(test)]
unsafe extern "C" fn hx_preview_set_info(
    _preview: *mut c_void,
    _type_: *const c_char,
    _creator: *const c_char,
) {
}
#[cfg(test)]
unsafe extern "C" fn hx_preview_done(_preview: *mut c_void) {}
#[cfg(test)]
unsafe fn hx_htxf_opt_preview(_htxf: *const c_void) -> c_int {
    0
}
#[cfg(test)]
unsafe fn hx_htxf_opt_folder(_htxf: *const c_void) -> c_int {
    0
}
#[cfg(test)]
unsafe fn hx_htxf_opt_large(_htxf: *const c_void) -> c_int {
    0
}

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
