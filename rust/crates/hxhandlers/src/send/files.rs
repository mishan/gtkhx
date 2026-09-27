//! `hxhandlers::send::files` — the files-browser RPC senders.
//!
//! MKDIR, DELETE, GETINFO, MOVE (with its rename-in-place SETINFO companion),
//! and the upload / folder-transfer kickoffs. The requests themselves are built
//! by `hxrequest::files`, which the end-to-end suite drives against real
//! servers; what lives here is the C ABI the files browser calls, the reply
//! task each request registers, and the send. The single-file upload goes
//! through `xfer_new`, which sends its own FILE_PUT once the queue lets it.

use std::ffi::{c_char, c_void, CStr, CString};
use std::os::raw::c_int;

use hxnet::xfer_handle::HtxfHandle;
use hxrequest::path::split;
use hxrequest::{files, Request};
use hxtask::Task;

use crate::recv::xfer::{rcv_task_file_getinfo, rcv_task_folder_get, rcv_task_folder_put};

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
#[cfg(not(test))]
use hxtask::task_new;

#[cfg(test)]
use tests::{hlwrite_chunks, hx_conn_has_cap, task_new, xfer_new, xfer_new_folder};

type RcvTaskFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize, *mut c_void, *mut c_void);

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

/// Whether `htlc` negotiated UTF-8 names.
unsafe fn utf8(htlc: *mut c_void) -> bool {
    hx_conn_has_cap(htlc.cast(), HTLC_CAP_TEXT_ENCODING) != glib::ffi::GFALSE
}

/// Register a task labeled `label` (with `rcv` and its `ptr` when the reply
/// needs handling) and write `req`. Returns the task, NULL if none was made.
unsafe fn send(
    htlc: *mut c_void,
    req: &Request,
    label: &CStr,
    rcv: Option<RcvTaskFn>,
    ptr: *mut c_void,
) -> *mut Task {
    let task = task_new(htlc.cast(), rcv, ptr, std::ptr::null_mut(), label.as_ptr());
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(
            htlc.cast(),
            req.opcode,
            0,
            chunks.as_ptr(),
            chunks.len() as c_int,
        )
    });
    task.cast()
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
    if let Some(req) = files::mkdir(cstr_bytes(path)) {
        send(htlc, &req, c"mkdir", None, std::ptr::null_mut());
    }
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
    if let Some(req) = files::delete(cstr_bytes(path), utf8(htlc)) {
        send(htlc, &req, c"rm", None, std::ptr::null_mut());
    }
}

/// `void hx_file_info (struct htlc_conn *htlc, const char *dir_path, const char
/// *file_name, gsize file_name_len)` — FILE_GETINFO for `file_name` in
/// `dir_path`. The reply opens the Get Info dialog; the task carries a
/// `dir/name` label for it (and for the tasks window).
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
    let Some(req) = files::get_info(dir, name, utf8(htlc)) else {
        return;
    };

    let mut label = Vec::new();
    if hxrequest::path::below_root(dir) {
        label.extend_from_slice(dir);
        label.push(hxrequest::path::SEP);
    }
    label.extend_from_slice(name);
    // The label is the reply handler's to free.
    let label = glib::ffi::g_strdup(c_string(&label).as_ptr());
    send(
        htlc,
        &req,
        c"finfo",
        Some(rcv_task_file_getinfo),
        label as *mut c_void,
    );
}

/// `void hx_put_file (struct htlc_conn *htlc, char *lpath, char *rpath)` —
/// upload the local file `lpath` to the remote path `rpath`. Splits `rpath` into
/// the directory and name `xfer_new` wants; the transfer sends FILE_PUT itself.
///
/// # Safety
/// `htlc` is NULL or live; `lpath` / `rpath` are NULL or NUL-terminated. Main
/// thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_put_file(htlc: *mut c_void, lpath: *mut c_char, rpath: *mut c_char) {
    if htlc.is_null() || lpath.is_null() || rpath.is_null() {
        return;
    }
    let (rdir, name) = split(cstr_bytes(rpath));
    let rdir = c_string(clamp_path(rdir));
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

/// `void hx_get_folder (struct htlc_conn *htlc, const char *lpath_root, const
/// char *rdir, const char *name, gsize name_len)` — download the remote folder
/// `name` in `rdir` into `lpath_root/name` (FILE_GETFOLDER).
///
/// # Safety
/// `htlc` is NULL or live; `lpath_root` / `rdir` are NULL or NUL-terminated;
/// `name` is valid for `name_len` bytes. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_get_folder(
    htlc: *mut c_void,
    lpath_root: *const c_char,
    rdir: *const c_char,
    name: *const c_char,
    name_len: usize,
) {
    let raw_name = slice_bytes(name, name_len);
    if htlc.is_null() || raw_name.is_empty() {
        return;
    }

    // The local destination: lpath_root + '/' + name. The folder receiver
    // rebuilds each file's path under it.
    let root = cstr_bytes(lpath_root);
    let mut lpath = root.to_vec();
    if !root.is_empty() && root.last() != Some(&b'/') {
        lpath.push(b'/');
    }
    lpath.extend_from_slice(raw_name);
    if lpath.len() + 1 > MAXPATHLEN {
        return;
    }
    let lpath = c_string(&lpath);
    let rdir_bytes = clamp_path(cstr_bytes(rdir));
    let rdir = c_string(rdir_bytes);

    let htxf = xfer_new_folder(
        htlc,
        lpath.as_ptr(),
        rdir.as_ptr(),
        name,
        name_len,
        XFER_GET,
    );
    if let Some(req) = files::get_folder(rdir_bytes, raw_name, utf8(htlc)) {
        send(
            htlc,
            &req,
            c"xfer_go_folder",
            Some(rcv_task_folder_get),
            htxf as *mut c_void,
        );
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
/// *rdir, const char *name, gsize name_len)` — upload the local folder `lpath`
/// into `rdir` as `name` (FILE_PUTFOLDER).
///
/// # Safety
/// `htlc` is NULL or live; `lpath` / `rdir` are NULL or NUL-terminated; `name`
/// is valid for `name_len` bytes. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_put_folder(
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

    let (total_bytes, nfiles) = folder_aggregate(&local_path(cstr_bytes(lpath)));
    let rdir_bytes = clamp_path(cstr_bytes(rdir));
    let rdir = c_string(rdir_bytes);

    let htxf: *mut HtxfHandle =
        xfer_new_folder(htlc, lpath, rdir.as_ptr(), name, name_len, XFER_PUT);
    // The progress denominator until the stream fills total_pos; never 0, which
    // the tasks window would divide by.
    (*htxf).total_size = total_bytes.clamp(1, u64::from(u32::MAX));

    // On a builder failure no task is made and nothing is written; the transfer
    // just created sits idle, since only the server's reply starts it.
    if let Some(req) = files::put_folder(rdir_bytes, raw_name, total_bytes, nfiles, utf8(htlc)) {
        send(
            htlc,
            &req,
            c"xfer_go_folder",
            Some(rcv_task_folder_put),
            htxf as *mut c_void,
        );
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
    let mut reqs = files::moves(cstr_bytes(src_path), cstr_bytes(dst_path), utf8(htlc)).into_iter();
    let Some(first) = reqs.next() else {
        return;
    };
    let Some(then) = reqs.next() else {
        send(htlc, &first, c"mv", None, std::ptr::null_mut());
        return;
    };
    // A move that also renames: the rename waits for the move's reply (see
    // hxrequest::files::moves), riding on the move's task until then.
    let then = Box::into_raw(Box::new(then));
    let task = send(
        htlc,
        &first,
        c"mv",
        Some(rcv_task_move_then_rename),
        then.cast(),
    );
    if task.is_null() {
        drop(Box::from_raw(then));
    } else {
        (*task).ptr_free = Some(free_request);
    }
}

/// The reply to the move half of a move-and-rename: send the rename (`ptr`) if
/// the move went through. The task owns `ptr` and frees it afterwards.
unsafe extern "C" fn rcv_task_move_then_rename(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    ptr: *mut c_void,
    _data: *mut c_void,
) {
    if ptr.is_null() || frame.is_null() {
        return;
    }
    let frame = std::slice::from_raw_parts(frame as *const u8, frame_len);
    let moved = hxproto::parse::Header::parse(frame).is_some_and(|h| !h.in_error());
    if moved {
        send(
            htlc,
            &*(ptr as *const Request),
            c"mv",
            None,
            std::ptr::null_mut(),
        );
    }
}

/// `GDestroyNotify` for the rename a move task carries.
unsafe extern "C" fn free_request(p: glib::ffi::gpointer) {
    if !p.is_null() {
        drop(Box::from_raw(p as *mut Request));
    }
}

#[cfg(test)]
mod tests;
