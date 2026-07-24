//! W1: the single-file HTXF download copy loop — the Rust port of C's
//! `xfers_recv.c::file_recv_one` (+ its `rd_wr_recv` / `preview_get` helpers).
//!
//! First slice of the "xfer worker → Rust" migration
//! (`docs/rust/xfer-worker-to-rust-scoping.md`). It removes the per-chunk C↔Rust
//! weave for single-file byte copying: the transport read is now an in-crate
//! `hxnet::htxf` call, the FILP/FFO codec is native `hxfiles_xfer::ffo`, and the
//! HFS sidecar + resource-fork I/O is native `hxhfs`.
//!
//! **No upward FFI.** `hxnet` is a leaf crate (higher crates + the C binary
//! depend on it), so it must not reference C symbols — the linker places it as a
//! provider, and a C-object symbol it needs (before the `--start-group`) would
//! not resolve, unlike group-archive symbols. So instead of reaching the
//! still-C-owned `htxf` through accessor externs, the C driver hands everything
//! in by value through [`HxnetXferParams`] (the scalars it reads off `htxf`
//! directly) plus a handful of callback function pointers (progress, preview
//! feed). The worker calls those through the pointers — a runtime value, no link
//! reference. Cancellation rides the `HtxfConn`'s own `HtxfAbort` (armed by
//! `xfer_delete` at the same time it sets `htxf->canceled`), which
//! `hxnet_htxf_read` already observes.
//!
//! The two C callers (`get_thread` solo download, `folder_recv_all` per folder
//! file) build the params and call `hxnet_xfer_file_recv_one`.

use std::ffi::{c_void, CStr};
use std::io::{Seek, SeekFrom, Write};
use std::os::raw::{c_char, c_int};
use std::path::{Path, PathBuf};

use hxfiles_xfer::ffo;
use hxhfs::hfs::{self, HfsInfo};

use crate::htxf::HtxfConn;

// errno-ish return codes (the C returned raw errno; callers only test != 0).
const EIO: c_int = 5;
const EINVAL: c_int = 22;

/// Everything the download loop needs, supplied by the C driver by value — so
/// the worker references no C symbols. `#[repr(C)]`; the C side declares the
/// matching struct in `xfers_recv.h`.
#[repr(C)]
pub struct HxnetXferParams {
    /// The open transport handle (`htxf->hx`).
    pub hx: *mut HtxfConn,
    /// Local destination path (`htxf->path`).
    pub path: *const c_char,
    /// Solo: `htxf->total_size`; folder file: this file's FILE_SEND size.
    pub file_budget: u64,
    /// Resume offsets (`htxf->data_pos` / `htxf->rsrc_pos`).
    pub data_pos: u64,
    pub rsrc_pos: u64,
    pub opt_preview: c_int,
    pub opt_folder: c_int,
    pub opt_large: c_int,
    /// `htxf->preview` (an `hx_preview *`) or NULL. Opaque to the worker; only
    /// handed back to the preview callbacks.
    pub preview: *mut c_void,
    /// Passed back to `progress` (the C side uses it as `htxf`).
    pub user_data: *mut c_void,
    /// Report `delta` bytes transferred; the C shim bumps `total_pos` + emits.
    pub progress: Option<unsafe extern "C" fn(user_data: *mut c_void, delta: u64)>,
    /// Preview feed (the `hx_preview_*` C functions), or `None`.
    pub preview_chunk: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
    pub preview_set_info: Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char)>,
    pub preview_done: Option<unsafe extern "C" fn(*mut c_void)>,
}

impl HxnetXferParams {
    unsafe fn report(&self, delta: u64) {
        if let Some(p) = self.progress {
            p(self.user_data, delta);
        }
    }
}

unsafe fn path_from(ptr: *const c_char) -> Option<PathBuf> {
    if ptr.is_null() {
        return None;
    }
    let bytes = CStr::from_ptr(ptr).to_bytes();
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        Some(PathBuf::from(std::ffi::OsStr::from_bytes(bytes).to_owned()))
    }
    #[cfg(not(unix))]
    {
        std::str::from_utf8(bytes).ok().map(PathBuf::from)
    }
}

fn io_errno(e: &std::io::Error) -> c_int {
    e.raw_os_error().unwrap_or(EIO)
}

/// One blocking read off the subchannel, mirroring C `htxf_io_read`. Cancellation
/// is the `HtxfConn`'s `HtxfAbort` (checked inside `hxnet_htxf_read`); a `< 1`
/// return (error or post-abort EOF) is failure. Returns the byte count (>= 1).
unsafe fn xfer_read(hx: *mut HtxfConn, buf: &mut [u8]) -> Result<usize, c_int> {
    let n = crate::htxf::hxnet_htxf_read(hx, buf.as_mut_ptr(), buf.len());
    if n < 1 {
        return Err(EIO);
    }
    Ok(n as usize)
}

/// Read exactly `len` bytes into a fresh buffer, reporting progress (mirrors the
/// C fixed-size read loops for the FILP header / info block / MACR marker).
unsafe fn read_exact_progress(
    hx: *mut HtxfConn,
    len: usize,
    p: &HxnetXferParams,
) -> Result<Vec<u8>, c_int> {
    let mut out = vec![0u8; len];
    let mut pos = 0;
    while pos < len {
        let n = xfer_read(hx, &mut out[pos..])?;
        pos += n;
        p.report(n as u64);
    }
    Ok(out)
}

/// Copy `data_len` bytes off the subchannel into `dst` (C `rd_wr_recv`).
unsafe fn rd_wr_recv(
    hx: *mut HtxfConn,
    dst: &mut std::fs::File,
    mut data_len: u64,
    p: &HxnetXferParams,
) -> Result<(), c_int> {
    let mut buf = vec![0u8; 0xf000];
    while data_len > 0 {
        let want = std::cmp::min(buf.len() as u64, data_len) as usize;
        let n = xfer_read(hx, &mut buf[..want])?;
        dst.write_all(&buf[..n]).map_err(|e| io_errno(&e))?;
        p.report(n as u64);
        data_len -= n as u64;
    }
    Ok(())
}

/// Stream `data_len` bytes through the preview widget (C `preview_get`).
unsafe fn preview_get(hx: *mut HtxfConn, mut data_len: u64, p: &HxnetXferParams) -> Result<(), c_int> {
    let mut buf = vec![0u8; 0xf000];
    while data_len > 0 {
        let want = std::cmp::min(buf.len() as u64, data_len) as usize;
        let n = xfer_read(hx, &mut buf[..want])?;
        if let Some(chunk) = p.preview_chunk {
            chunk(p.preview, buf.as_ptr() as *const c_char, n);
        }
        p.report(n as u64);
        data_len -= n as u64;
    }
    if let Some(done) = p.preview_done {
        done(p.preview);
    }
    Ok(())
}

fn make_hfsinfo(type_creator: [u8; 8], fi: &ffo::FilpInfo) -> HfsInfo {
    HfsInfo {
        type_creator,
        create_time: fi.create_time,
        modify_time: fi.modify_time,
        rsrclen: 0,
        comment: fi.comment.clone(),
    }
}

/// The `done:` tail — rewrite the sidecar with the real type/creator.
fn finish(cfg: &hfs::Config, path: &Path, typecrea: [u8; 8], pi: &ffo::FilpInfo) -> c_int {
    let fi = make_hfsinfo(typecrea, pi);
    let _ = hfs::hfsinfo_write(cfg, path, &fi);
    0
}

/// `int hxnet_xfer_file_recv_one (const HxnetXferParams *p)` — receive one file
/// off the HTXF subchannel into `p->path` (was C `file_recv_one`). Returns 0 on
/// success, an errno-like positive code on failure. Does NOT play the completion
/// sound, post a final update, or close the channel — the caller owns those.
///
/// # Safety
/// C-ABI worker helper, run on a blocking-pool thread. `p` is a valid
/// `HxnetXferParams` whose `hx` transport handle is open.
#[no_mangle]
pub unsafe extern "C" fn hxnet_xfer_file_recv_one(p: *const HxnetXferParams) -> c_int {
    let p = match p.as_ref() {
        Some(p) => p,
        None => return EINVAL,
    };
    let hx = p.hx;
    if hx.is_null() {
        return EINVAL;
    }
    let is_preview = p.opt_preview != 0;
    let is_folder = p.opt_folder != 0;
    let large = p.opt_large != 0;
    let path = match path_from(p.path) {
        Some(pb) => pb,
        None => return EINVAL,
    };
    let cfg = hxhfs::ffi::current_config();

    // 1. 40-byte FILP fixed header.
    let hdr = match read_exact_progress(hx, 40, p) {
        Ok(h) => h,
        Err(e) => return e,
    };
    // 2. Variable info+comment block, length from FILP bytes 38/39.
    let info_len = ffo::info_block_len(hdr[38], hdr[39]);
    let mut tot_len: u64 = 40 + info_len as u64;
    let info = match read_exact_progress(hx, info_len, p) {
        Ok(b) => b,
        Err(e) => return e,
    };
    // 3. Interpret the FILP info block.
    let pi = match ffo::parse_filp_info(&info, large) {
        Ok(pi) => pi,
        Err(_) => return EIO,
    };
    let typecrea = pi.type_creator;

    // Early sidecar write (placeholder type "HTftHTLC"), unless preview.
    if !is_preview {
        let fi = make_hfsinfo(*b"HTftHTLC", &pi);
        let _ = hfs::hfsinfo_write(&cfg, &path, &fi);
    }

    // 4. Data fork.
    let fork_len = pi.data_fork_len;
    tot_len += fork_len;
    if fork_len != 0 {
        if !is_preview {
            let mut f = match std::fs::OpenOptions::new().create(true).write(true).open(&path) {
                Ok(f) => f,
                Err(e) => return io_errno(&e),
            };
            if p.data_pos != 0 && f.seek(SeekFrom::Start(p.data_pos)).is_err() {
                return EIO;
            }
            if let Err(e) = rd_wr_recv(hx, &mut f, fork_len, p) {
                return e;
            }
            let _ = f.sync_all();
        } else {
            if p.preview.is_null() {
                return 0; // nothing to write into; quietly stop
            }
            let mut type_s = [0u8; 5];
            let mut creator_s = [0u8; 5];
            type_s[..4].copy_from_slice(&typecrea[0..4]);
            creator_s[..4].copy_from_slice(&typecrea[4..8]);
            if let Some(set_info) = p.preview_set_info {
                set_info(
                    p.preview,
                    type_s.as_ptr() as *const c_char,
                    creator_s.as_ptr() as *const c_char,
                );
            }
            if let Err(e) = preview_get(hx, fork_len, p) {
                return e;
            }
        }
    }

    // 5. Resource fork.
    if is_preview {
        return 0; // previews never carry a resource fork
    }
    if is_folder {
        // Folder-stream files never persist a resource fork, but the server may
        // have announced file_budget bytes and buffered a MACR marker. Drain up
        // to (file_budget - tot_len) with a short per-read timeout.
        if tot_len < p.file_budget && crate::htxf::hxnet_htxf_set_read_timeout(hx, 200) == 0 {
            let mut remaining = p.file_budget - tot_len;
            let mut sink = [0u8; 2048];
            while remaining > 0 {
                let want = std::cmp::min(sink.len() as u64, remaining) as usize;
                let got = crate::htxf::hxnet_htxf_read(hx, sink.as_mut_ptr(), want);
                if got <= 0 {
                    break; // timeout (EIO) or EOF — give up
                }
                remaining -= got as u64;
                p.report(got as u64);
            }
            if crate::htxf::hxnet_htxf_set_read_timeout(hx, 0) != 0 {
                return EIO;
            }
        }
        return finish(&cfg, &path, typecrea, &pi);
    }
    if tot_len >= p.file_budget {
        return finish(&cfg, &path, typecrea, &pi);
    }
    // 16-byte MACR fork header.
    let marker = match read_exact_progress(hx, ffo::FORK_HEADER_LEN, p) {
        Ok(m) => m,
        Err(e) => return e,
    };
    let mut m = [0u8; ffo::FORK_HEADER_LEN];
    m.copy_from_slice(&marker);
    let rfork_len = ffo::fork_len(&m, large);
    if rfork_len == 0 {
        return finish(&cfg, &path, typecrea, &pi);
    }
    let mut opts = std::fs::OpenOptions::new();
    opts.create(true).write(true);
    let mut rf = match hfs::resource_open(&cfg, &path, &opts) {
        Ok(Some(f)) => f,
        Ok(None) => return EIO,
        Err(e) => return io_errno(&e),
    };
    if p.rsrc_pos != 0 && rf.seek(SeekFrom::Start(p.rsrc_pos)).is_err() {
        return EIO;
    }
    if let Err(e) = rd_wr_recv(hx, &mut rf, rfork_len, p) {
        return e;
    }
    finish(&cfg, &path, typecrea, &pi)
}
