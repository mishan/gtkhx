//! `hxhandlers::send::files` — the files-browser RPC senders.
//!
//! MKDIR, DELETE, GETINFO, MOVE (with its rename-in-place SETINFO companion),
//! and the upload / folder-transfer kickoffs. Each one encodes its path to the
//! wire DIR bytes (via C's `path_to_hldir`), encodes the basename for the wire
//! (`gtkhx_text_for_wire`, hxtext), builds the chunks with the native
//! `hxproto::build` builders, registers a reply task, and hands the chunks to
//! `hlwrite_chunks`. The single-file upload goes through `xfer_new`, which sends
//! its own FILE_PUT once the queue lets it.
//!
//! Remote paths are `/`-separated, with `/` as the root. A name travels as its
//! own byte run wherever the wire allows it, since a Classic-Mac name may hold a
//! `/`; the flat-path senders (delete, move) split on the last `/`, as they always
//! have.
//!
//! Exports the exact `hx_*` C ABI the files browser calls.

use std::ffi::{c_char, c_void, CStr, CString};
use std::os::raw::c_int;

use hxproto::build::{self, FileMoveRequest, FilePutFolderRequest, FileSetInfoRequest, HxChunk};
use hxproto::messages::ClientHdr;

use crate::recv::xfer::{rcv_task_file_getinfo, rcv_task_folder_get, rcv_task_folder_put};
use hxnet::xfer_handle::HtxfHandle;

const HTLC_HDR_FILE_DELETE: u32 = ClientHdr::FileDelete as u32;
const HTLC_HDR_FILE_MKDIR: u32 = ClientHdr::FileMkdir as u32;
const HTLC_HDR_FILE_GETINFO: u32 = ClientHdr::FileGetInfo as u32;
const HTLC_HDR_FILE_SETINFO: u32 = ClientHdr::FileSetInfo as u32;
const HTLC_HDR_FILE_MOVE: u32 = ClientHdr::FileMove as u32;
const HTLC_HDR_FILE_GETFOLDER: u32 = ClientHdr::FileGetFolder as u32;
const HTLC_HDR_FILE_PUTFOLDER: u32 = ClientHdr::FilePutFolder as u32;

/// `HTLC_CAP_TEXT_ENCODING` (hotline.h) — names go out as UTF-8 when set.
const HTLC_CAP_TEXT_ENCODING: u64 = 0x0002;
/// `XFER_GET` / `XFER_PUT` (protocol.h).
const XFER_GET: u16 = 0;
const XFER_PUT: u16 = 1;
/// `MAXPATHLEN` (compat.h) — the fixed path fields on the transfer handle.
const MAXPATHLEN: usize = 4095;
/// The remote path separator.
const SEP: u8 = b'/';

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
#[cfg(not(test))]
use hxtext::gtkhx_text_for_wire;

#[cfg(not(test))]
extern "C" {
    // path_hldir.c — encode a "/a/b" path to the wire DIR bytes. Returns a
    // g_malloc'd buffer + out length; caller g_free's. `is_file` drops the last
    // component (it names the file, not a directory).
    fn path_to_hldir(path: *const c_char, hldirlen: *mut u16, is_file: c_int) -> *mut u8;
}

#[cfg(test)]
use tests::{
    gtkhx_text_for_wire, hlwrite_chunks, hx_conn_has_cap, path_to_hldir, task_new, xfer_new,
    xfer_new_folder,
};

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

/// Whether a remote directory names something below the root.
fn below_root(dir: &[u8]) -> bool {
    !dir.is_empty() && dir != [SEP]
}

/// Offset of the last component of a remote path (0 with no separator).
fn basename_offset(path: &[u8]) -> usize {
    hxmodel::files::basename_offset(path, SEP)
}

/// A g_malloc'd DIR-chunk encoding of a path; g_free'd on drop.
struct HlDir {
    ptr: *mut u8,
    len: u16,
}

impl HlDir {
    /// # Safety
    /// `path` is a NUL-terminated C string.
    unsafe fn new(path: *const c_char, is_file: bool) -> Self {
        let mut len: u16 = 0;
        let ptr = path_to_hldir(path, &mut len, c_int::from(is_file));
        HlDir { ptr, len }
    }

    fn bytes(&self) -> &[u8] {
        if self.ptr.is_null() || self.len == 0 {
            &[]
        } else {
            // SAFETY: path_to_hldir returned `len` bytes at `ptr`.
            unsafe { std::slice::from_raw_parts(self.ptr, self.len as usize) }
        }
    }
}

impl Drop for HlDir {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { glib::ffi::g_free(self.ptr as *mut c_void) };
        }
    }
}

/// A name encoded for the wire on `htlc` (UTF-8 or Mac Roman, per the
/// negotiated text-encoding capability). Names are single-line, so no LF→CR.
unsafe fn name_for_wire(htlc: *mut c_void, name: &[u8]) -> Vec<u8> {
    let utf8 = hx_conn_has_cap(htlc.cast(), HTLC_CAP_TEXT_ENCODING);
    let mut len: usize = 0;
    let wire = gtkhx_text_for_wire(
        name.as_ptr() as *const c_char,
        name.len(),
        utf8,
        glib::ffi::GFALSE,
        &mut len,
    );
    if wire.is_null() {
        return Vec::new();
    }
    let out = std::slice::from_raw_parts(wire as *const u8, len).to_vec();
    glib::ffi::g_free(wire as *mut c_void);
    out
}

/// Register a reply-less task labeled `label` and write the request.
unsafe fn send(htlc: *mut c_void, ty: u32, label: &CStr, chunks: &[HxChunk], hc: usize) {
    task_new(
        htlc.cast(),
        None,
        std::ptr::null_mut(),
        std::ptr::null_mut(),
        label.as_ptr(),
    );
    hlwrite_chunks(htlc.cast(), ty, 0, chunks.as_ptr(), hc as c_int);
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
    let dir = HlDir::new(path, false);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_file_mkdir_chunks(dir.bytes(), &mut chunks);
    if hc > 0 {
        send(htlc, HTLC_HDR_FILE_MKDIR, c"mkdir", &chunks, hc);
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
    let p = cstr_bytes(path);
    let base = basename_offset(p);
    let name = name_for_wire(htlc, &p[base..]);
    let dir = (base > 0).then(|| HlDir::new(path, true));
    let mut chunks = [HxChunk::EMPTY; 2];
    let hc = build::build_file_delete_chunks(&name, dir.as_ref().map(HlDir::bytes), &mut chunks);
    if hc > 0 {
        send(htlc, HTLC_HDR_FILE_DELETE, c"rm", &chunks, hc);
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
    let dir_bytes = cstr_bytes(dir_path);
    let raw_name = slice_bytes(file_name, file_name_len);
    let has_dir = below_root(dir_bytes);

    let mut label = Vec::new();
    if has_dir {
        label.extend_from_slice(dir_bytes);
        label.push(SEP);
    }
    label.extend_from_slice(raw_name);
    let label = c_string(&label);

    let name = name_for_wire(htlc, raw_name);
    let dir = has_dir.then(|| HlDir::new(dir_path, false));
    let mut chunks = [HxChunk::EMPTY; 2];
    let hc = build::build_file_getinfo_chunks(&name, dir.as_ref().map(HlDir::bytes), &mut chunks);
    if hc > 0 {
        // The label is the reply handler's to free.
        let ptr = glib::ffi::g_strdup(label.as_ptr());
        task_new(
            htlc.cast(),
            Some(rcv_task_file_getinfo),
            ptr as *mut c_void,
            std::ptr::null_mut(),
            c"finfo".as_ptr(),
        );
        hlwrite_chunks(
            htlc.cast(),
            HTLC_HDR_FILE_GETINFO,
            0,
            chunks.as_ptr(),
            hc as c_int,
        );
    }
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
    let r = cstr_bytes(rpath);
    let base = basename_offset(r);
    let mut rdir = clamp_path(&r[..base]);
    // "/a/" → "/a", so xfer_go's is-it-the-root test sees what it expects; the
    // root itself stays "/".
    if rdir.len() > 1 && rdir.last() == Some(&SEP) {
        rdir = &rdir[..rdir.len() - 1];
    }
    let rdir = c_string(rdir);
    let name = &r[base..];
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

    // As with FILE_GET, the request names the parent directory and carries the
    // folder's basename as FILE_NAME.
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

    let wire_name = name_for_wire(htlc, raw_name);
    let dir = below_root(rdir_bytes).then(|| HlDir::new(rdir.as_ptr(), false));
    let mut chunks = [HxChunk::EMPTY; 2];
    let hc =
        build::build_file_getfolder_chunks(&wire_name, dir.as_ref().map(HlDir::bytes), &mut chunks);
    if hc > 0 {
        task_new(
            htlc.cast(),
            Some(rcv_task_folder_get),
            htxf as *mut c_void,
            std::ptr::null_mut(),
            c"xfer_go_folder".as_ptr(),
        );
        hlwrite_chunks(
            htlc.cast(),
            HTLC_HDR_FILE_GETFOLDER,
            0,
            chunks.as_ptr(),
            hc as c_int,
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
/// The request carries the tree's byte total and file count, which the server
/// shows in its queue; the per-file sizes stream with the files.
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
    // HTXF_SIZE is 32-bit on the wire.
    let size = total_bytes.min(u64::from(u32::MAX)) as u32;

    let rdir_bytes = clamp_path(cstr_bytes(rdir));
    let rdir = c_string(rdir_bytes);

    let htxf: *mut HtxfHandle =
        xfer_new_folder(htlc, lpath, rdir.as_ptr(), name, name_len, XFER_PUT);
    // The progress denominator until the stream fills total_pos; never 0, which
    // the tasks window would divide by.
    (*htxf).total_size = u64::from(size.max(1));

    let wire_name = name_for_wire(htlc, raw_name);
    let dir = below_root(rdir_bytes).then(|| HlDir::new(rdir.as_ptr(), false));
    let req = FilePutFolderRequest {
        name: &wire_name,
        dir: dir.as_ref().map(HlDir::bytes),
        size,
        nfiles,
    };
    let mut chunks = [HxChunk::EMPTY; 4];
    let mut scratch = [0u8; 8];
    // On a builder failure no task is made and nothing is written; the transfer
    // just created sits idle, since only the server's reply starts it.
    let hc = build::build_file_putfolder_chunks(&req, &mut chunks, &mut scratch);
    if hc > 0 {
        task_new(
            htlc.cast(),
            Some(rcv_task_folder_put),
            htxf as *mut c_void,
            std::ptr::null_mut(),
            c"xfer_go_folder".as_ptr(),
        );
        hlwrite_chunks(
            htlc.cast(),
            HTLC_HDR_FILE_PUTFOLDER,
            0,
            chunks.as_ptr(),
            hc as c_int,
        );
    }
}

/// `void hx_file_move (struct htlc_conn *htlc, char *src_path, char *dst_path)`
/// — move and/or rename the remote file at `src_path` to `dst_path`.
///
/// Hotline splits the two: FILE_MOVE changes the directory (keeping the name),
/// and FILE_SETINFO's rename changes the name within a directory. A move that
/// also renames sends both, the rename addressed to the source directory.
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
    let src = cstr_bytes(src_path);
    let dst = cstr_bytes(dst_path);
    let src_base = basename_offset(src);
    let dst_base = basename_offset(dst);
    let src_name = &src[src_base..];
    let dst_name = &dst[dst_base..];

    let src_dir = HlDir::new(src_path, true);
    let src_wire = name_for_wire(htlc, src_name);
    let dst_wire = name_for_wire(htlc, dst_name);

    // The directory prefixes (each through its trailing separator) differ.
    if dst_base > 0 && dst[..dst_base] != src[..src_base] {
        let dst_dir = HlDir::new(dst_path, true);
        let req = FileMoveRequest {
            name: &src_wire,
            dir: src_dir.bytes(),
            dir_rename: dst_dir.bytes(),
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build::build_file_move_chunks(&req, &mut chunks);
        if hc > 0 {
            send(htlc, HTLC_HDR_FILE_MOVE, c"mv", &chunks, hc);
        }
    }
    if !dst_name.is_empty() && src_name != dst_name {
        let req = FileSetInfoRequest {
            name: &src_wire,
            rename: &dst_wire,
            comment: None,
            dir: Some(src_dir.bytes()),
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let hc = build::build_file_setinfo_chunks(&req, &mut chunks);
        if hc > 0 {
            send(htlc, HTLC_HDR_FILE_SETINFO, c"mv", &chunks, hc);
        }
    }
}

#[cfg(test)]
mod tests;
