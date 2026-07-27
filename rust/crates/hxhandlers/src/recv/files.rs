//! `hxfiles-recv` — the FILE_LIST receive handler (`rcv_task_file_list`, ported
//! from `rcv.c`) plus the Rust home of `struct cached_filelist` (`cfl`).
//!
//! `cfl` used to be a `protocol.h` struct that `rcv.c` filled and the files
//! browser's remote provider consumed. It is now owned here (the hxconn
//! playbook): an opaque handle behind the `hx_cfl_*` accessor facade, holding the
//! path, the accumulated `fh` buffer (a `Vec<u8>`), the `completing` mode, and the
//! borrowed `filter_argv`. Because the buffer lives in Rust, the FILE_LIST reply's
//! chunk accumulation is native — [`CachedFileList::append_entry`] grows `fh` with
//! the exact 4-byte-aligned, patched-length record layout the view's
//! `hxfiles-entry` populate walks — and the handler emits the `file-list` signal
//! directly (the old C `cfl_print` is gone).
//!
//! The one thing the handler still calls out to C for is the recursive engine:
//! when `completing > 1`, each folder entry is handed to `hx_cfl_complete_entry`
//! (files.c), which re-issues FILE_LIST for a subfolder or spins up a recursive
//! download (`mkdir` trees, `xfer_new`, `path_to_hldir`) — genuine files-subsystem
//! C that reads the Rust `cfl` through the same accessors.

use hotline_proto::parse::FTYPE_FLDR;
use hotline_proto::wire::ChunkIter;
use std::os::raw::{c_char, c_int, c_uint, c_void};

/// `HTLS_DATA_FILE_LIST` (src/hotline.h).
const HTLS_DATA_FILE_LIST: u16 = 0x00c8;
/// The `hl_data_hdr` size (tag + len).
const HL_DATA_HDR_LEN: usize = 4;
/// `COMPLETE_NONE` (src/protocol.h).
const COMPLETE_NONE: u8 = 0;

/// Rust-owned `struct cached_filelist`. Opaque to C, reached through `hx_cfl_*`.
pub struct CachedFileList {
    /// Remote directory path (owned; the old `char *path`).
    path: Option<std::ffi::CString>,
    /// Accumulated FILE_LIST records — the old `struct hl_filelist_hdr *fh` raw
    /// buffer, byte-for-byte, so `hxfiles-entry`'s `parse_file_list_entry` walk is
    /// unchanged. Grown by [`Self::append_entry`].
    fh: Vec<u8>,
    /// Recursive-listing mode (`COMPLETE_*`, 2 bits on the wire struct).
    completing: u8,
    /// Borrowed `char **` filter (owned by the browser, not by `cfl`).
    filter_argv: *mut c_void,
}

impl CachedFileList {
    /// Append one raw FILE_LIST record (`hl_data_hdr` + body) to `fh`, matching
    /// the old C accumulation: round the total up to the next multiple of 4 (the
    /// original bumps an already-aligned record by a full 4, so replicate that
    /// exactly), zero-pad, and patch the record's length field to the padded body
    /// length. `record` is the chunk *including* its 4-byte header.
    fn append_entry(&mut self, record: &[u8]) {
        let fhlen = record.len() - HL_DATA_HDR_LEN; // declared body length
        let mut fh_len = HL_DATA_HDR_LEN + fhlen; // == record.len()
        fh_len += 4 - (fh_len % 4);
        let start = self.fh.len();
        self.fh.extend_from_slice(record);
        self.fh.resize(start + fh_len, 0); // zero-pad to the aligned stride
        let patched = (fh_len - HL_DATA_HDR_LEN) as u16;
        self.fh[start + 2..start + 4].copy_from_slice(&patched.to_be_bytes());
    }
}

// ---- hx_cfl_* accessor facade (the C-visible opaque handle) -----------------

/// `struct cached_filelist *hx_cfl_new (void)` — allocate a zeroed cfl (replaces
/// the old `g_malloc0 (sizeof (struct cached_filelist))` / `cfl_lookup`).
#[no_mangle]
pub extern "C" fn hx_cfl_new() -> *mut CachedFileList {
    Box::into_raw(Box::new(CachedFileList {
        path: None,
        fh: Vec::new(),
        completing: 0,
        filter_argv: std::ptr::null_mut(),
    }))
}

/// `void hx_cfl_free (struct cached_filelist *cfl)` — free the cfl (path + fh
/// drop with the box; `filter_argv` is borrowed and not freed here).
///
/// # Safety
/// `cfl` is a live handle from [`hx_cfl_new`] or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_free(cfl: *mut CachedFileList) {
    if !cfl.is_null() {
        drop(Box::from_raw(cfl));
    }
}

/// # Safety
/// `cfl` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_path(cfl: *const CachedFileList) -> *const c_char {
    match &(*cfl).path {
        Some(p) => p.as_ptr(),
        None => std::ptr::null(),
    }
}

/// # Safety
/// `cfl` is a live handle; `path` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_set_path(cfl: *mut CachedFileList, path: *const c_char) {
    (*cfl).path = if path.is_null() {
        None
    } else {
        Some(std::ffi::CStr::from_ptr(path).to_owned())
    };
}

/// Pointer to the accumulated `fh` buffer (NULL when empty, matching the old
/// never-realloc'd `cfl->fh == NULL`).
///
/// # Safety
/// `cfl` is a live handle; the pointer is valid until `fh` next grows or the cfl
/// is freed.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_fh(cfl: *const CachedFileList) -> *const c_void {
    if (*cfl).fh.is_empty() {
        std::ptr::null()
    } else {
        (*cfl).fh.as_ptr() as *const c_void
    }
}

/// # Safety
/// `cfl` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_fhlen(cfl: *const CachedFileList) -> u32 {
    (*cfl).fh.len() as u32
}

/// # Safety
/// `cfl` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_completing(cfl: *const CachedFileList) -> c_uint {
    (*cfl).completing as c_uint
}

/// # Safety
/// `cfl` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_set_completing(cfl: *mut CachedFileList, completing: c_uint) {
    (*cfl).completing = (completing & 0x3) as u8; // 2-bit field on the old struct
}

/// # Safety
/// `cfl` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_filter_argv(cfl: *const CachedFileList) -> *mut c_void {
    (*cfl).filter_argv
}

/// # Safety
/// `cfl` is a live handle; `argv` is a borrowed `char **` (not owned by cfl).
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_set_filter_argv(cfl: *mut CachedFileList, argv: *mut c_void) {
    (*cfl).filter_argv = argv;
}

// ---- the receive handler ----------------------------------------------------

#[cfg(not(test))]
use gtkhx_session::{gtkhx_session_emit_file_list, gtkhx_session_get_default};

#[cfg(not(test))]
extern "C" {    /// Give the remote provider a chance to show an empty-state hint before we
    /// drop the cfl on a task-error listing (files_remote_provider.c). Returns a
    /// `gboolean` (whether the reply had a recognised provider carrier); we don't
    /// act on it, but the declaration must match the C ABI.
    fn hx_remote_files_provider_handle_file_list_error(
        cfl: *mut c_void,
        data: *mut c_void,
    ) -> c_int;
    /// The recursive folder-relist / GET_R engine (files.c): re-issue FILE_LIST
    /// for a subfolder (`is_folder`), or `mkdir` + `xfer_new` a leaf for a
    /// recursive download. The folder-vs-file decision is made here in Rust (via
    /// [`FTYPE_FLDR`]) and passed as a flag, so the C side carries no FourCC.
    /// Reads the Rust `cfl` through the accessors above.
    fn hx_cfl_complete_entry(
        htlc: *mut c_void,
        cfl: *mut c_void,
        is_folder: c_int,
        fname: *const u8,
        fnlen: usize,
        fsize: u32,
    );
}

/// True when the reply frame's task-error bit is set (native `hotline_proto`
/// header parse; a too-short frame is not-in-error, matching the old C shim).
unsafe fn task_in_error(frame: *const c_void, frame_len: usize) -> bool {
    if frame.is_null() {
        return false;
    }
    let s = std::slice::from_raw_parts(frame as *const u8, frame_len);
    hotline_proto::parse::Header::parse(s).is_some_and(|h| h.in_error())
}

fn be32(b: &[u8]) -> u32 {
    u32::from_be_bytes([b[0], b[1], b[2], b[3]])
}

/// `void rcv_task_file_list (htlc, frame, frame_len, cfl, data)` — the HTLC_HDR_
/// FILE_LIST reply (was `rcv.c`). Walks the FILE_LIST chunks natively
/// (`ChunkIter`), accumulates each raw record into the Rust-owned `cfl.fh`, and
/// — for a recursive listing (`completing > 1`) — hands each folder entry to the
/// C recursive engine. On a task error it lets the provider render an empty-state
/// hint, then frees the cfl. Finally it resets `completing` and emits `file-list`
/// so the browser repaints (only when `data` names a provider carrier).
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `ptr` is the `struct cached_filelist *` (a Rust
/// [`CachedFileList`] handle); `data` is the provider carrier or NULL.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_file_list(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    ptr: *mut c_void,
    data: *mut c_void,
) {
    let cfl = ptr as *mut CachedFileList;

    if task_in_error(frame, frame_len) {
        hx_remote_files_provider_handle_file_list_error(ptr, data);
        hx_cfl_free(cfl);
        return;
    }

    let completing = (*cfl).completing;
    let s = std::slice::from_raw_parts(frame as *const u8, frame_len);
    for chunk in ChunkIter::over_message(s, s.len()) {
        if chunk.tag != HTLS_DATA_FILE_LIST {
            continue;
        }
        let d = chunk.data;
        if completing > 1 && d.len() >= 20 {
            let ftype = be32(&d[0..4]);
            let fsize = be32(&d[8..12]);
            let fnlen = be32(&d[16..20]) as usize;
            let name_end = 20usize.saturating_add(fnlen).min(d.len());
            let fname = &d[20..name_end];
            let is_folder = c_int::from(ftype == FTYPE_FLDR);
            hx_cfl_complete_entry(htlc, ptr, is_folder, fname.as_ptr(), fname.len(), fsize);
        }
        // The raw record is the 4-byte header immediately before `d` plus `d`
        // itself (ChunkIter positions data right after the header).
        let record = std::slice::from_raw_parts(d.as_ptr().sub(HL_DATA_HDR_LEN), HL_DATA_HDR_LEN + d.len());
        (*cfl).append_entry(record);
    }

    (*cfl).completing = COMPLETE_NONE;

    // Emit only when a provider carrier is present (the old cfl_print gate). The
    // provider reads hx_cfl_fh(cfl) itself, so the fh signal arg stays NULL.
    if !data.is_null() {
        gtkhx_session_emit_file_list(gtkhx_session_get_default(), ptr, std::ptr::null_mut(), data);
    }
}

#[cfg(test)]
mod doubles;
#[cfg(test)]
use doubles::*;

#[cfg(test)]
mod tests;
