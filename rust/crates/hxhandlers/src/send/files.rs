//! `hxhandlers::send::files` — the files-browser RPC senders.
//!
//! FILE_LIST, MKDIR, DELETE, GETINFO, SETINFO, MOVE (with its rename-in-place
//! SETINFO companion), and the upload / folder-transfer kickoffs. The requests
//! themselves are built by `hxrequest::files`, which the end-to-end suite
//! drives against real servers; what lives here is the C ABI the files
//! browser calls, the session expecting each reply, and the send. A reply is
//! matched to what asked for it (`recv::files`) by the trans it went out on;
//! a change's says nothing unless the server refused, which `request-failed`
//! reports. The single-file upload goes through `xfer_new`, which sends its
//! own FILE_PUT once the queue lets it.
//!
//! Paths and names go as the bytes they hold: a listing's, which name a
//! thing back to the server exactly. What the user names — an upload's local
//! file — is encoded here as the connection sends text.

use std::ffi::{c_char, c_void, CStr, CString};
use std::os::raw::c_int;

use hxnet::xfer_handle::HtxfHandle;
use hxrequest::{files, Request};
use hxsession::Expect;

use super::expect_next;
use crate::recv::files::{asked, Asked};

/// `HTLC_CAP_TEXT_ENCODING` (hotline.h) — names go out as UTF-8 when set.
const HTLC_CAP_TEXT_ENCODING: u64 = 0x0002;
/// `XFER_GET` / `XFER_PUT` (protocol.h).
const XFER_GET: u16 = 0;
const XFER_PUT: u16 = 1;
/// `MAXPATHLEN` (compat.h) — the fixed path fields on the transfer handle.
const MAXPATHLEN: usize = 4095;

// Real build: these resolve at the final link. Test build: `use tests::{…}`
// below shadows them with recording stubs.
#[cfg(not(test))]
use crate::xfer::{xfer_new, xfer_new_folder};
#[cfg(not(test))]
use gtkhx_core::conn::hx_conn_has_cap;
#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;

#[cfg(test)]
use tests::{hlwrite_chunks, hx_conn_has_cap, xfer_new, xfer_new_folder};

/// A NUL-terminated C string's bytes (without the NUL), or empty for NULL.
unsafe fn cstr_bytes<'a>(s: *const c_char) -> &'a [u8] {
    if s.is_null() {
        &[]
    } else {
        CStr::from_ptr(s).to_bytes()
    }
}

/// `(ptr, len)` → borrowed bytes (empty for NULL).
unsafe fn slice_bytes<'a>(p: *const c_char, len: usize) -> &'a [u8] {
    if p.is_null() || len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(p as *const u8, len)
    }
}

/// A C string of `bytes`, cut at the first NUL — what the C `memcpy` into a
/// NUL-terminated buffer amounted to.
fn c_string(bytes: &[u8]) -> CString {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    CString::new(&bytes[..end]).unwrap()
}

/// `bytes` clamped to what fits a `MAXPATHLEN` field with its NUL.
fn clamp_path(bytes: &[u8]) -> &[u8] {
    &bytes[..bytes.len().min(MAXPATHLEN - 1)]
}

/// Whether `htlc` negotiated UTF-8 text.
unsafe fn utf8(htlc: *mut c_void) -> bool {
    hx_conn_has_cap(htlc.cast(), HTLC_CAP_TEXT_ENCODING) != glib::ffi::GFALSE
}

/// A local file's name as an upload names it on the server: its last
/// component, encoded as `htlc` sends text.
unsafe fn upload_name(htlc: *mut c_void, lpath: &[u8]) -> Vec<u8> {
    let name = &lpath[hxrequest::path::basename_offset(lpath)..];
    hxtext::for_wire(name, utf8(htlc), false)
}

/// Send `req` with its reply expected as `what`; the trans it went out on.
unsafe fn send(htlc: *mut c_void, req: &Request, what: Expect) -> u32 {
    let trans = expect_next(htlc, what);
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(
            htlc.cast(),
            req.opcode,
            0,
            chunks.as_ptr(),
            chunks.len() as c_int,
        )
    });
    trans
}

/// Send a change to the files: a folder made, something deleted, moved or
/// renamed, a comment set.
pub(crate) unsafe fn change(htlc: *mut c_void, req: Option<Request>) {
    if let Some(req) = req {
        send(htlc, &req, Expect::FileChange);
    }
}

/// The next request on `htlc` is a transfer's, its reply for `what`: a
/// download or an upload's transfer.
pub(crate) unsafe fn expect_transfer(htlc: *mut c_void, what: Asked) {
    let trans = expect_next(htlc, Expect::Transfer);
    asked(htlc, trans, what);
}

/// `void hx_list_dir (struct htlc_conn *htlc, const char *path, gpointer
/// provider)` — FILE_LIST for the folder at `path`, its reply for
/// `provider`, the remote files provider that asked.
///
/// # Safety
/// `htlc` is NULL or live; `path` is NULL or NUL-terminated. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_list_dir(
    htlc: *mut c_void,
    path: *const c_char,
    provider: *mut c_void,
) {
    if htlc.is_null() || path.is_null() {
        return;
    }
    let path = CStr::from_ptr(path);
    if let Some(req) = files::list(path.to_bytes()) {
        let trans = send(htlc, &req, Expect::FileList);
        asked(
            htlc,
            trans,
            Asked::Listing {
                provider: crate::recv::files::Provider::new(provider),
                path: path.to_owned(),
            },
        );
    }
}

/// `void hx_make_dir (struct htlc_conn *htlc, char *path)` — FILE_MKDIR for the
/// folder at `path`.
///
/// # Safety
/// `htlc` is NULL or live; `path` is NULL or NUL-terminated. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_make_dir(htlc: *mut c_void, path: *mut c_char) {
    if htlc.is_null() || path.is_null() {
        return;
    }
    change(htlc, files::mkdir(cstr_bytes(path)));
}

/// `void hx_file_delete (struct htlc_conn *htlc, char *path)` — FILE_DELETE for
/// the file or folder at `path`.
///
/// # Safety
/// `htlc` is NULL or live; `path` is NULL or NUL-terminated. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_file_delete(htlc: *mut c_void, path: *mut c_char) {
    if htlc.is_null() || path.is_null() {
        return;
    }
    change(htlc, files::delete(cstr_bytes(path)));
}

/// `void hx_file_info (struct htlc_conn *htlc, const char *dir_path, const char
/// *file_name, gsize file_name_len)` — FILE_GETINFO for `file_name` in
/// `dir_path`. The reply opens the Get Info dialog, for `dir/name`.
///
/// # Safety
/// `htlc` is NULL or live; `dir_path` is NULL or NUL-terminated; `file_name` is
/// valid for `file_name_len` bytes. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_file_info(
    htlc: *mut c_void,
    dir_path: *const c_char,
    file_name: *const c_char,
    file_name_len: usize,
) {
    if htlc.is_null() {
        return;
    }
    let dir = cstr_bytes(dir_path);
    let name = slice_bytes(file_name, file_name_len);
    let Some(req) = files::get_info(dir, name) else {
        return;
    };

    let mut label = Vec::new();
    if hxrequest::path::below_root(dir) {
        label.extend_from_slice(dir);
        label.push(hxrequest::path::SEP);
    }
    label.extend_from_slice(name);
    let trans = send(htlc, &req, Expect::FileInfo);
    asked(htlc, trans, Asked::Info(c_string(&label)));
}

/// FILE_SETINFO from the Get Info dialog: the file at `path` renamed to
/// `rename`, as the user typed it, when there is one, and its comment set.
///
/// # Safety
/// `htlc` is a live connection. Main thread only.
pub unsafe fn set_info(htlc: *mut c_void, path: &[u8], rename: Option<&str>, comment: &str) {
    let utf8 = utf8(htlc);
    let rename = rename.map(|r| hxtext::for_wire(r.as_bytes(), utf8, false));
    change(
        htlc,
        files::set_info(path, rename.as_deref(), Some(comment.as_bytes()), utf8),
    );
}

/// `void hx_put_file (struct htlc_conn *htlc, const char *lpath, const char
/// *rdir)` — upload the local file `lpath` into the remote folder `rdir`,
/// under its own name. The transfer sends FILE_PUT itself.
///
/// # Safety
/// `htlc` is NULL or live; `lpath` / `rdir` are NULL or NUL-terminated. Main
/// thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_put_file(htlc: *mut c_void, lpath: *const c_char, rdir: *const c_char) {
    if htlc.is_null() || lpath.is_null() || rdir.is_null() {
        return;
    }
    let name = upload_name(htlc, cstr_bytes(lpath));
    let rdir = c_string(clamp_path(cstr_bytes(rdir)));
    xfer_new(
        htlc,
        lpath,
        rdir.as_ptr(),
        name.as_ptr() as *const c_char,
        name.len(),
        XFER_PUT,
        0,
        0,
    );
}

/// `void hx_get_folder (struct htlc_conn *htlc, const char *lpath, const char
/// *rdir, const char *name, gsize name_len)` — download the remote folder
/// `name` in `rdir` into the local folder `lpath` (FILE_GETFOLDER).
///
/// # Safety
/// `htlc` is NULL or live; `lpath` / `rdir` are NULL or NUL-terminated;
/// `name` is valid for `name_len` bytes. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_get_folder(
    htlc: *mut c_void,
    lpath: *const c_char,
    rdir: *const c_char,
    name: *const c_char,
    name_len: usize,
) {
    let raw_name = slice_bytes(name, name_len);
    if htlc.is_null() || lpath.is_null() || raw_name.is_empty() {
        return;
    }
    if cstr_bytes(lpath).len() + 1 > MAXPATHLEN {
        return;
    }
    let rdir_bytes = clamp_path(cstr_bytes(rdir));
    let rdir = c_string(rdir_bytes);

    let htxf = xfer_new_folder(htlc, lpath, rdir.as_ptr(), name, name_len, XFER_GET);
    if let Some(req) = files::get_folder(rdir_bytes, raw_name) {
        expect_transfer(
            htlc,
            Asked::Download {
                htxf: crate::recv::files::Xfer::new(htxf.cast()),
                folder: true,
            },
        );
        req.with_hx_chunks(|chunks| {
            hlwrite_chunks(
                htlc.cast(),
                req.opcode,
                0,
                chunks.as_ptr(),
                chunks.len() as c_int,
            )
        });
    }
}

/// Byte total and file count of the regular files under `root`, recursively.
/// Symlinks are neither followed nor counted.
fn folder_aggregate(root: &std::path::Path) -> (u64, u32) {
    let mut bytes = 0u64;
    let mut files = 0u32;
    let Ok(entries) = std::fs::read_dir(root) else {
        return (0, 0);
    };
    for entry in entries.flatten() {
        let Ok(meta) = std::fs::symlink_metadata(entry.path()) else {
            continue;
        };
        if meta.is_dir() {
            let (b, n) = folder_aggregate(&entry.path());
            bytes = bytes.saturating_add(b);
            files = files.saturating_add(n);
        } else if meta.is_file() {
            bytes = bytes.saturating_add(meta.len());
            files = files.saturating_add(1);
        }
    }
    (bytes, files)
}

/// A local path from its C-string bytes.
fn local_path(bytes: &[u8]) -> std::path::PathBuf {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        std::path::PathBuf::from(std::ffi::OsStr::from_bytes(bytes))
    }
    #[cfg(not(unix))]
    {
        std::path::PathBuf::from(String::from_utf8_lossy(bytes).into_owned())
    }
}

/// `void hx_put_folder (struct htlc_conn *htlc, const char *lpath, const char
/// *rdir)` — upload the local folder `lpath` into the remote folder `rdir`,
/// under its own name (FILE_PUTFOLDER).
///
/// # Safety
/// `htlc` is NULL or live; `lpath` / `rdir` are NULL or NUL-terminated. Main
/// thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_put_folder(
    htlc: *mut c_void,
    lpath: *const c_char,
    rdir: *const c_char,
) {
    if htlc.is_null() || lpath.is_null() {
        return;
    }
    let name = upload_name(htlc, cstr_bytes(lpath));
    if name.is_empty() {
        return;
    }

    let (total_bytes, nfiles) = folder_aggregate(&local_path(cstr_bytes(lpath)));
    let rdir_bytes = clamp_path(cstr_bytes(rdir));
    let rdir = c_string(rdir_bytes);

    let htxf: *mut HtxfHandle = xfer_new_folder(
        htlc,
        lpath,
        rdir.as_ptr(),
        name.as_ptr().cast(),
        name.len(),
        XFER_PUT,
    );
    // The progress denominator until the stream fills total_pos; never 0, which
    // the tasks window would divide by.
    (*htxf).total_size = total_bytes.clamp(1, u64::from(u32::MAX));

    // On a builder failure nothing is written; the transfer just created sits
    // idle, since only the server's reply starts it.
    if let Some(req) = files::put_folder(rdir_bytes, &name, total_bytes, nfiles) {
        expect_transfer(
            htlc,
            Asked::Upload {
                htxf: crate::recv::files::Xfer::new(htxf.cast()),
                folder: true,
            },
        );
        req.with_hx_chunks(|chunks| {
            hlwrite_chunks(
                htlc.cast(),
                req.opcode,
                0,
                chunks.as_ptr(),
                chunks.len() as c_int,
            )
        });
    }
}

/// `void hx_file_move (struct htlc_conn *htlc, char *src_path, char *dst_path)`
/// — move and/or rename the remote file at `src_path` to `dst_path`: a
/// FILE_MOVE for a new directory and a FILE_SETINFO for a new name (see
/// `hxrequest::files::moves`).
///
/// # Safety
/// `htlc` is NULL or live; `src_path` / `dst_path` are NULL or NUL-terminated.
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_file_move(
    htlc: *mut c_void,
    src_path: *mut c_char,
    dst_path: *mut c_char,
) {
    if htlc.is_null() || src_path.is_null() || dst_path.is_null() {
        return;
    }
    let mut reqs = files::moves(cstr_bytes(src_path), cstr_bytes(dst_path)).into_iter();
    let Some(first) = reqs.next() else {
        return;
    };
    let trans = send(htlc, &first, Expect::FileChange);
    // A move that also renames: the rename waits for the move to go through
    // (see hxrequest::files::moves).
    if let Some(then) = reqs.next() {
        asked(htlc, trans, Asked::Rename(then));
    }
}

#[cfg(test)]
mod tests;
