//! `hxhandlers::recv::files` — the FILE_LIST receive handler (`rcv_task_file_list`, ported
//! from `rcv.c`) plus the Rust home of `struct cached_filelist` (`cfl`).
//!
//! `cfl` used to be a `protocol.h` struct that `rcv.c` filled and the files
//! browser's remote provider consumed. It is now owned here (the gtkhx-core::conn
//! playbook): an opaque handle behind the `hx_cfl_*` accessor facade, holding the
//! path and the accumulated `fh` buffer (a `Vec<u8>`). Because the buffer lives in
//! Rust, the FILE_LIST reply's
//! chunk accumulation is native — [`CachedFileList::append_entry`] grows `fh` with
//! the exact 4-byte-aligned, patched-length record layout the view's
//! `hxmodel::files_entry` populate walks — and the handler emits the `file-list` signal
//! directly (the old C `cfl_print` is gone).

use hxproto::wire::ChunkIter;
use std::os::raw::{c_char, c_void};

/// `HTLS_DATA_FILE_LIST` (src/hotline.h).
const HTLS_DATA_FILE_LIST: u16 = 0x00c8;
/// The `hl_data_hdr` size (tag + len).
const HL_DATA_HDR_LEN: usize = 4;

/// Rust-owned `struct cached_filelist`. Opaque to C, reached through `hx_cfl_*`.
pub struct CachedFileList {
    /// Remote directory path (owned; the old `char *path`).
    path: Option<std::ffi::CString>,
    /// Accumulated FILE_LIST records — the old `struct hl_filelist_hdr *fh` raw
    /// buffer, byte-for-byte, so `hxmodel::files_entry`'s `parse_file_list_entry` walk is
    /// unchanged. Grown by [`Self::append_entry`].
    fh: Vec<u8>,
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
    }))
}

/// `void hx_cfl_free (struct cached_filelist *cfl)` — free the cfl (path + fh
/// drop with the box).
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

// ---- the receive handler ----------------------------------------------------

#[cfg(not(test))]
use gtkhx_core::session::{gtkhx_session_emit_file_list, gtkhx_session_get_default};

#[cfg(not(test))]
extern "C" {
    /// Give the remote provider a chance to show an empty-state hint before we
    /// drop the cfl on a task-error listing (files_remote_provider.c). Returns a
    /// `gboolean` (whether the reply had a recognised provider carrier); we don't
    /// act on it, but the declaration must match the C ABI.
    fn hx_remote_files_provider_handle_file_list_error(
        cfl: *mut c_void,
        data: *mut c_void,
    ) -> std::os::raw::c_int;
}

/// True when the reply frame's task-error bit is set (native `hxproto`
/// header parse; a too-short frame is not-in-error, matching the old C shim).
unsafe fn task_in_error(frame: *const c_void, frame_len: usize) -> bool {
    if frame.is_null() {
        return false;
    }
    let s = std::slice::from_raw_parts(frame as *const u8, frame_len);
    hxproto::parse::Header::parse(s).is_some_and(|h| h.in_error())
}

/// `void rcv_task_file_list (htlc, frame, frame_len, cfl, data)` — the HTLC_HDR_
/// FILE_LIST reply (was `rcv.c`). Walks the FILE_LIST chunks natively
/// (`ChunkIter`) and accumulates each raw record into the Rust-owned `cfl.fh`. On
/// a task error it lets the provider render an empty-state hint, then frees the
/// cfl. Finally it emits `file-list` so the browser repaints (only when `data`
/// names a provider carrier).
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

    let s = std::slice::from_raw_parts(frame as *const u8, frame_len);
    for chunk in ChunkIter::over_message(s, s.len()) {
        if chunk.tag != HTLS_DATA_FILE_LIST {
            continue;
        }
        let d = chunk.data;
        // The raw record is the 4-byte header immediately before `d` plus `d`
        // itself (ChunkIter positions data right after the header).
        let record =
            std::slice::from_raw_parts(d.as_ptr().sub(HL_DATA_HDR_LEN), HL_DATA_HDR_LEN + d.len());
        (*cfl).append_entry(record);
    }

    // Emit only when a provider carrier is present (the old cfl_print gate). The
    // provider reads hx_cfl_fh(cfl) itself, so the fh signal arg stays NULL.
    if !data.is_null() {
        gtkhx_session_emit_file_list(
            gtkhx_session_get_default(),
            htlc,
            ptr,
            std::ptr::null_mut(),
            data,
        );
    }
}

#[cfg(test)]
mod doubles;
#[cfg(test)]
use doubles::*;

#[cfg(test)]
mod tests;
