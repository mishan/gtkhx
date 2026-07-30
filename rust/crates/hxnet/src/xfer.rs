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
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::raw::{c_char, c_int};
use std::path::{Path, PathBuf};

use hxfiles_xfer::ffo;
use hxhfs::hfs::{self, HfsInfo};

use crate::htxf::HtxfConn;

// errno-ish return codes (the C returned raw errno; callers only test != 0).
const EIO: c_int = 5;
const EINVAL: c_int = 22;
const EFBIG: c_int = 27;
const ENAMETOOLONG: c_int = 36;
const EPROTO: c_int = 71;

// Folder mini-protocol opcodes (Hotline 1.5 FILE_NEXT/SEND/RESUME state
// machine), sent/received as u16 big-endian over the HTXF subchannel.
const FILE_SEND_CMD: u16 = 1;
const FILE_RESUME_CMD: u16 = 2;
const FILE_NEXT_CMD: u16 = 3;
/// next_file_info header: u16 len, u16 type, u16 pathcount.
const NFI_HEADER_LEN: usize = 6;
/// The fixed part the nfi `len` field counts before the per-component bytes.
const NFI_LEN_FIXED: u16 = 4;
/// Cap on a received folder entry's joined relative path (matches `compat.h`'s
/// clamped `MAXPATHLEN`); a hostile server can't drive an unbounded path.
const MAXPATHLEN: usize = 4095;
/// Bytes the sender's per-file FILP body always emits ahead of the fork data:
/// the fixed FILP header (no comment) — used by the folder upload's declared
/// per-file size, which must match `hxnet_xfer_file_send_one`'s output exactly.
const FILP_HEADER_LEN: usize = 133;
/// A resume fork-list (RFLT) carries the data-fork resume offset at byte 46 and
/// the resource-fork offset at byte 62, each a big-endian u32; the offsets are
/// only present when the list is at least this long.
const RFLT_MAX: usize = 128;
const RFLT_DATA_POS_OFF: usize = 46;
const RFLT_RSRC_POS_OFF: usize = 62;
const RFLT_MIN_FOR_DATA: usize = 50;
const RFLT_MIN_FOR_RSRC: usize = 66;

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
    /// Preview feed (the `hx_preview_*` C functions), or `None`. Receive-only;
    /// the send worker ignores them.
    pub preview_chunk: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
    pub preview_set_info: Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char)>,
    pub preview_done: Option<unsafe extern "C" fn(*mut c_void)>,
    /// Local fork sizes for a *send* (`htxf->data_size` / `htxf->rsrc_size`).
    /// Send-only; the receive worker ignores them (it uses `file_budget`).
    pub data_size: u64,
    pub rsrc_size: u64,
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

/// Fill `buf` completely off the subchannel, looping over short reads (the
/// plaintext transport can return fewer bytes than requested, unlike the C
/// callers' one-write-per-frame assumption). Returns `buf.len()` when full,
/// `0` only when EOF lands on a read boundary before any bytes are consumed
/// (the folder recv's clean end-of-stream signal), or a negative on error —
/// including a truncated read (EOF partway through), which is a genuine desync
/// rather than a clean end.
unsafe fn htxf_read_full(hx: *mut HtxfConn, buf: &mut [u8]) -> isize {
    let want = buf.len();
    let mut got = 0usize;
    while got < want {
        let n = crate::htxf::hxnet_htxf_read(hx, buf[got..].as_mut_ptr(), want - got);
        if n < 0 {
            return -1;
        }
        if n == 0 {
            // Clean EOF only at a buffer boundary; a partial fill is truncation.
            return if got == 0 { 0 } else { -1 };
        }
        got += n as usize;
    }
    got as isize
}

/// A raw Hotline path component (Mac Roman bytes) → an owned `OsString`, without
/// a UTF-8 round-trip that would mangle high-bit names.
fn os_from_bytes(b: &[u8]) -> std::ffi::OsString {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        std::ffi::OsStr::from_bytes(b).to_owned()
    }
    #[cfg(not(unix))]
    {
        std::ffi::OsString::from(String::from_utf8_lossy(b).into_owned())
    }
}

/// A local path → a `CString` for the per-file `HxnetXferParams.path` (which
/// `hxnet_xfer_file_{recv,send}_one` re-parses with `path_from`). `None` if the
/// path contains an interior NUL.
fn cstring_from_path(p: &Path) -> Option<std::ffi::CString> {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        std::ffi::CString::new(p.as_os_str().as_bytes()).ok()
    }
    #[cfg(not(unix))]
    {
        std::ffi::CString::new(p.to_str()?).ok()
    }
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
unsafe fn preview_get(
    hx: *mut HtxfConn,
    mut data_len: u64,
    p: &HxnetXferParams,
) -> Result<(), c_int> {
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

/// Short per-read timeout (ms) used when draining a trailing MACR marker that a
/// server may have over-declared in `file_budget` but never actually sends (a
/// no-resource-fork file). Without it the worker blocks on the 16-byte marker
/// read until the server closes the socket — the ~2s end-of-transfer stall the
/// old C `file_recv_one` also had. Used by both the folder drain and the solo
/// MACR read.
const MACR_DRAIN_TIMEOUT_MS: u32 = 200;

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
            // No truncate: a resumed download seeks to `data_pos` and writes
            // into an existing partial file, so truncating would discard it.
            let mut f = match std::fs::OpenOptions::new()
                .create(true)
                .write(true)
                .truncate(false)
                .open(&path)
            {
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
            // Clamp the streamed preview to 32 bits (the C `preview_get` took a
            // guint32 length via MIN(fork_len, 0xFFFFFFFF)). Previews are small
            // by definition; the clamp guards against a hostile/pathological
            // multi-GiB "preview" length flooding the UI pipeline.
            let preview_len = fork_len.min(u32::MAX as u64);
            if let Err(e) = preview_get(hx, preview_len, p) {
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
        if tot_len < p.file_budget
            && crate::htxf::hxnet_htxf_set_read_timeout(hx, MACR_DRAIN_TIMEOUT_MS) == 0
        {
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
    // 16-byte MACR fork header. The server declared more than the data fork, so
    // a MACR marker *should* follow — but some servers over-declare file_budget
    // and send nothing for a no-resource-fork file. Arm a short read timeout so
    // a missing marker finishes cleanly instead of blocking until the server
    // closes the socket (the ~2s stall the blocking C read had). A real marker
    // is buffered right behind the data fork, so the timeout never clips it.
    if crate::htxf::hxnet_htxf_set_read_timeout(hx, MACR_DRAIN_TIMEOUT_MS) != 0 {
        return EIO;
    }
    // Drive the marker read directly so we can tell "nothing arrived" (a phantom
    // the server over-declared → clean no-resource-fork completion) apart from a
    // partial read that then failed (a real marker we've lost sync on → error).
    // read_exact_progress collapses both into one Err, which would silently
    // truncate a real resource fork and desync the stream, so it can't be used
    // here.
    let mut m = [0u8; ffo::FORK_HEADER_LEN];
    let mut got = 0usize;
    let marker_complete = loop {
        let n = crate::htxf::hxnet_htxf_read(hx, m[got..].as_mut_ptr(), m.len() - got);
        if n < 1 {
            // No bytes yet → server sent no marker (clean finish). Some bytes
            // already consumed → partial marker, the stream is desynced.
            break if got == 0 { Ok(false) } else { Err(EIO) };
        }
        p.report(n as u64);
        got += n as usize;
        if got == m.len() {
            break Ok(true);
        }
    };
    // Disarm before reading the resource fork itself — fork data can take longer
    // than the drain timeout to arrive.
    if crate::htxf::hxnet_htxf_set_read_timeout(hx, 0) != 0 {
        return EIO;
    }
    match marker_complete {
        Ok(true) => {}                                          // full 16-byte marker
        Ok(false) => return finish(&cfg, &path, typecrea, &pi), // no marker at all
        Err(e) => return e,                                     // partial → desync
    }
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

// ============================ send (upload) ================================
// W2: the Rust port of C's xfers_send.c::file_send_one (+ rd_wr_send). Mirror
// of the receive worker above, sending end.

/// The fixed 115-byte FILP header template C `file_send_one` `memcpy`s before
/// patching in the per-file fields. Extracted byte-for-byte from the C string
/// literal (octal escapes + the literal `^A` = 0x5e 0x41) so the wire framing is
/// identical.
#[rustfmt::skip]
const FILP_TEMPLATE: [u8; 115] = [
    0x46,0x49,0x4c,0x50,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,
    0x49,0x4e,0x46,0x4f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x5e,0x41,0x4d,0x41,0x43,0x54,0x59,0x50,0x45,
    0x43,0x52,0x45,0x41,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x70,0x00,0x00,
    0x00,0x00,0x00,0x00,0x07,0x70,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x03,0x68,0x78,0x64,
];

/// One blocking write of the whole buffer to the subchannel (mirrors C
/// `htxf_io_write`, which returns the full logical length on success — the hxnet
/// channel writes it all, one AEAD frame when armed). `!= len` is failure.
unsafe fn xfer_write(hx: *mut HtxfConn, buf: &[u8]) -> Result<(), c_int> {
    if crate::htxf::hxnet_htxf_write(hx, buf.as_ptr(), buf.len()) != buf.len() as isize {
        return Err(EIO);
    }
    Ok(())
}

/// Copy `data_len` bytes from a local fork file to the subchannel (C
/// `rd_wr_send`). A local read of 0 (EOF before the promised length) is EIO,
/// matching the C `read(...) < 1` gate.
unsafe fn rd_wr_send(
    hx: *mut HtxfConn,
    src: &mut std::fs::File,
    mut data_len: u64,
    p: &HxnetXferParams,
) -> Result<(), c_int> {
    let mut buf = vec![0u8; 0xf000];
    while data_len > 0 {
        let want = std::cmp::min(buf.len() as u64, data_len) as usize;
        let n = match src.read(&mut buf[..want]) {
            Ok(0) => return Err(EIO),
            Ok(n) => n,
            Err(e) => return Err(io_errno(&e)),
        };
        xfer_write(hx, &buf[..n])?;
        p.report(n as u64);
        data_len -= n as u64;
    }
    Ok(())
}

/// `int hxnet_xfer_file_send_one (const HxnetXferParams *p)` — send one file out
/// the HTXF subchannel from `p->path` (was C `file_send_one`). The FILP header +
/// fork markers are built natively (`hxfiles_xfer::ffo` + `hxhfs`), the fork
/// bytes streamed via `hxnet::htxf` write. `p->data_size` / `rsrc_size` are the
/// local fork sizes; `data_pos` / `rsrc_pos` the resume offsets. Returns 0 on
/// success, an errno-like positive code on failure. Caller closes the channel +
/// plays the completion sound.
///
/// # Safety
/// C-ABI worker helper on a blocking-pool thread; `p` valid, `p->hx` open.
#[no_mangle]
pub unsafe extern "C" fn hxnet_xfer_file_send_one(p: *const HxnetXferParams) -> c_int {
    let p = match p.as_ref() {
        Some(p) => p,
        None => return EINVAL,
    };
    let hx = p.hx;
    if hx.is_null() {
        return EINVAL;
    }
    let large = p.opt_large != 0;
    let is_folder = p.opt_folder != 0;
    let path = match path_from(p.path) {
        Some(pb) => pb,
        None => return EINVAL,
    };
    let cfg = hxhfs::ffi::current_config();

    // Large-file solo upload: raw file data only, no FFO wrapper (the server
    // reconstructs metadata from the filesystem). Folder uploads keep FFO.
    if large && !is_folder {
        let mut f = match std::fs::File::open(&path) {
            Ok(f) => f,
            Err(e) => return io_errno(&e),
        };
        return match rd_wr_send(hx, &mut f, p.data_size, p) {
            Ok(()) => 0,
            Err(e) => e,
        };
    }

    // FILP header: template + patched fields (rsrc flag, comment length,
    // type/creator, munged times, comment bytes, DATA fork marker).
    let fi = hfs::hfsinfo_read(&cfg, &path);
    let comlen = fi.comment.len();
    let mut hdr = vec![0u8; FILP_HEADER_LEN + comlen];
    hdr[..115].copy_from_slice(&FILP_TEMPLATE);
    if p.rsrc_size.wrapping_sub(p.rsrc_pos) != 0 {
        hdr[23] = 3;
    }
    let comfield = 65 + comlen + 12; // = 77 + comlen; the u16-ish comment field
    if comfield > 0xff {
        hdr[38] = 1;
    }
    hdr[39] = comfield as u8;
    hdr[44..52].copy_from_slice(&hfs::type_creator(&cfg, &path));
    hdr[96..100].copy_from_slice(&ffo::hfs_h_to_mtime(fi.create_time));
    hdr[104..108].copy_from_slice(&ffo::hfs_h_to_mtime(fi.modify_time));
    hdr[115] = 0;
    hdr[116] = comlen as u8;
    hdr[117..117 + comlen].copy_from_slice(&fi.comment);
    let data_hdr = ffo::pack_fork_header(b"DATA", p.data_size.wrapping_sub(p.data_pos), large);
    hdr[117 + comlen..FILP_HEADER_LEN + comlen].copy_from_slice(&data_hdr);
    if xfer_write(hx, &hdr).is_err() {
        return EIO;
    }
    p.report((FILP_HEADER_LEN + comlen) as u64);

    // Data fork.
    if p.data_size.wrapping_sub(p.data_pos) != 0 {
        let mut f = match std::fs::File::open(&path) {
            Ok(f) => f,
            Err(e) => return io_errno(&e),
        };
        if p.data_pos != 0 && f.seek(SeekFrom::Start(p.data_pos)).is_err() {
            return EIO;
        }
        // Resume: stream only the bytes past data_pos — matches the DATA fork
        // length declared in the header and the folder payload size in
        // folder_send_all (both data_size - data_pos).
        if let Err(e) = rd_wr_send(hx, &mut f, p.data_size.wrapping_sub(p.data_pos), p) {
            return e;
        }
    }

    // MACR fork header. A short write at the marker boundary is a clean stop
    // (the server may not want the resource fork), not an error. The length is
    // the remaining resource fork (rsrc_size - rsrc_pos) so a resumed upload's
    // marker matches what we actually stream below.
    let macr_hdr = ffo::pack_fork_header(b"MACR", p.rsrc_size.wrapping_sub(p.rsrc_pos), large);
    if xfer_write(hx, &macr_hdr).is_err() {
        return 0;
    }
    p.report(16);
    if p.rsrc_size.wrapping_sub(p.rsrc_pos) == 0 {
        return 0;
    }
    let mut ropts = std::fs::OpenOptions::new();
    ropts.read(true);
    let mut rf = match hfs::resource_open(&cfg, &path, &ropts) {
        Ok(Some(f)) => f,
        Ok(None) => return EIO,
        Err(e) => return io_errno(&e),
    };
    if p.rsrc_pos != 0 && rf.seek(SeekFrom::Start(p.rsrc_pos)).is_err() {
        return EIO;
    }
    match rd_wr_send(hx, &mut rf, p.rsrc_size.wrapping_sub(p.rsrc_pos), p) {
        Ok(()) => 0,
        Err(e) => e,
    }
}

// ============================ folder mini-protocol =========================
// W3: the Rust port of C's xfers_recv.c::folder_recv_all and
// xfers_send.c::folder_send_all — the Hotline 1.5 FILE_NEXT / FILE_SEND /
// FILE_RESUME state machine plus the local tree walk. Per-file byte copying
// delegates to the in-crate hxnet_xfer_file_{recv,send}_one above.

/// Everything the folder loops need, supplied by the C driver by value (same
/// no-upward-FFI discipline as [`HxnetXferParams`]). The per-file path is built
/// internally from `base_path`, so the worker never touches the C
/// `htxf->path`. `#[repr(C)]`; the C side declares the match in `xfers_recv.h`.
#[repr(C)]
pub struct HxnetFolderParams {
    /// Open transport handle (`htxf->hx`).
    pub hx: *mut HtxfConn,
    /// Local tree root — created + written into (recv) or walked (send).
    pub base_path: *const c_char,
    pub opt_preview: c_int,
    pub opt_folder: c_int,
    pub opt_large: c_int,
    /// Passed back to `progress` (the C side uses it as `htxf`).
    pub user_data: *mut c_void,
    pub progress: Option<unsafe extern "C" fn(user_data: *mut c_void, delta: u64)>,
}

/// Build the per-file receive/send params from folder params + a resolved path.
fn per_file_params(
    fp: &HxnetFolderParams,
    path: *const c_char,
    file_budget: u64,
    data_pos: u64,
    rsrc_pos: u64,
    data_size: u64,
    rsrc_size: u64,
) -> HxnetXferParams {
    HxnetXferParams {
        hx: fp.hx,
        path,
        file_budget,
        data_pos,
        rsrc_pos,
        opt_preview: fp.opt_preview,
        opt_folder: fp.opt_folder,
        opt_large: fp.opt_large,
        preview: std::ptr::null_mut(),
        user_data: fp.user_data,
        progress: fp.progress,
        preview_chunk: None,
        preview_set_info: None,
        preview_done: None,
        data_size,
        rsrc_size,
    }
}

/// `int hxnet_xfer_folder_recv_all (const HxnetFolderParams *fp)` — receive a
/// folder tree into `fp->base_path` (was C `folder_recv_all`). Drives the
/// FILE_NEXT/FILE_SEND loop: request the next entry, read its nfi header + path
/// components, mkdir folders or receive files via `hxnet_xfer_file_recv_one`.
/// Returns 0 on success (including the clean end-of-stream when the server
/// closes the socket), an errno-like positive code on failure.
///
/// # Safety
/// C-ABI worker helper on a blocking-pool thread; `fp` valid, `fp->hx` open.
#[no_mangle]
pub unsafe extern "C" fn hxnet_xfer_folder_recv_all(fp: *const HxnetFolderParams) -> c_int {
    let fp = match fp.as_ref() {
        Some(f) => f,
        None => return EINVAL,
    };
    if fp.hx.is_null() || fp.progress.is_none() {
        return EINVAL;
    }
    let base = match path_from(fp.base_path) {
        Some(p) => p,
        None => return EINVAL,
    };
    if let Err(e) = std::fs::create_dir_all(&base) {
        return io_errno(&e);
    }

    loop {
        // Request the next entry.
        if xfer_write(fp.hx, &FILE_NEXT_CMD.to_be_bytes()).is_err() {
            return EIO;
        }
        // nfi header: a 0-byte read is the clean end-of-stream (server closed).
        let mut nfi = [0u8; NFI_HEADER_LEN];
        let n = htxf_read_full(fp.hx, &mut nfi);
        if n == 0 {
            return 0;
        }
        if n != NFI_HEADER_LEN as isize {
            return EIO;
        }
        let ftype = u16::from_be_bytes([nfi[2], nfi[3]]);
        let pathcount = u16::from_be_bytes([nfi[4], nfi[5]]);

        // Read the path components into a single '/'-joined relative path,
        // bounded to MAXPATHLEN like the C loop — a hostile server must not be
        // able to drive an unbounded PathBuf (huge pathcount / long names) into
        // a memory/CPU DoS.
        let mut rel: Vec<u8> = Vec::new();
        for _ in 0..pathcount {
            let mut ph = [0u8; 3];
            if htxf_read_full(fp.hx, &mut ph) != 3 {
                return EIO;
            }
            let nlen = ph[2] as usize;
            // Empty components are meaningless and would let a bare separator
            // slip in; refuse them.
            if nlen == 0 {
                return EINVAL;
            }
            let mut name = vec![0u8; nlen];
            if htxf_read_full(fp.hx, &mut name) != nlen as isize {
                return EIO;
            }
            // Defence in depth — refuse `..` and any path separator that could
            // escape base_path. Reject '\\' too: it's a separator on Windows, so
            // PathBuf::join would treat it as one there.
            if name == b".." || name.contains(&b'/') || name.contains(&b'\\') {
                return EINVAL;
            }
            // Grow the joined path (a '/' before every component after the
            // first), capping the total at MAXPATHLEN.
            let addition = if rel.is_empty() { nlen } else { 1 + nlen };
            if rel.len() + addition > MAXPATHLEN {
                return ENAMETOOLONG;
            }
            if !rel.is_empty() {
                rel.push(b'/');
            }
            rel.extend_from_slice(&name);
        }
        if rel.is_empty() {
            return EINVAL;
        }
        let full = base.join(os_from_bytes(&rel));

        if ftype == 1 {
            // Folder marker — mkdir, no payload.
            if let Err(e) = std::fs::create_dir_all(&full) {
                return io_errno(&e);
            }
            continue;
        }

        // A file at depth > 1 may arrive before its parent marker; mkdir -p.
        if pathcount > 1 {
            if let Some(parent) = full.parent() {
                if let Err(e) = std::fs::create_dir_all(parent) {
                    return io_errno(&e);
                }
            }
        }

        // Request the file fresh (data_pos/rsrc_pos zeroed → whole file).
        if xfer_write(fp.hx, &FILE_SEND_CMD.to_be_bytes()).is_err() {
            return EIO;
        }
        let mut sz = [0u8; 4];
        if htxf_read_full(fp.hx, &mut sz) != 4 {
            return EIO;
        }
        let file_size = u32::from_be_bytes(sz) as u64;

        let path_c = match cstring_from_path(&full) {
            Some(c) => c,
            None => return EINVAL,
        };
        let per = per_file_params(fp, path_c.as_ptr(), file_size, 0, 0, 0, 0);
        let rv = hxnet_xfer_file_recv_one(&per);
        if rv != 0 {
            return rv;
        }
    }
}

/// One planned folder-upload entry (DFS pre-order): a folder marker or a file
/// leaf, with its path components from the upload root (raw Mac Roman bytes).
struct PutEntry {
    is_folder: bool,
    full: PathBuf,
    components: Vec<Vec<u8>>,
}

/// DFS pre-order walk of the local tree (C `hx_collect_put_entries`): folders
/// before their contents, names sorted bytewise for a deterministic stream.
/// Symlinks and special files are skipped (metadata is read without following).
fn collect_put_entries(dir: &Path, prefix: &[Vec<u8>], out: &mut Vec<PutEntry>) {
    let rd = match std::fs::read_dir(dir) {
        Ok(r) => r,
        Err(_) => return,
    };
    let mut names: Vec<(Vec<u8>, PathBuf)> = Vec::new();
    for ent in rd.flatten() {
        names.push((os_bytes(&ent.file_name()), ent.path()));
    }
    names.sort_by(|a, b| a.0.cmp(&b.0));

    for (name, full) in names {
        let md = match std::fs::symlink_metadata(&full) {
            Ok(m) => m,
            Err(_) => continue,
        };
        let mut comps = prefix.to_vec();
        comps.push(name);
        if md.is_dir() {
            out.push(PutEntry {
                is_folder: true,
                full: full.clone(),
                components: comps.clone(),
            });
            collect_put_entries(&full, &comps, out);
        } else if md.is_file() {
            out.push(PutEntry {
                is_folder: false,
                full,
                components: comps,
            });
        }
    }
}

/// Raw bytes of an `OsStr` (Mac Roman filenames), no UTF-8 round-trip.
fn os_bytes(s: &std::ffi::OsStr) -> Vec<u8> {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        s.as_bytes().to_vec()
    }
    #[cfg(not(unix))]
    {
        s.to_string_lossy().into_owned().into_bytes()
    }
}

/// `int hxnet_xfer_folder_send_all (const HxnetFolderParams *fp)` — send the
/// local folder tree at `fp->base_path` (was C `folder_send_all`). For each
/// server FILE_NEXT, replies with one entry's nfi header + path components, then
/// for files the per-file size + FILP body via `hxnet_xfer_file_send_one`. The
/// server closes when done; our next FILE_NEXT read short-reads and we stop.
/// Returns 0 on success, an errno-like positive code on failure.
///
/// # Safety
/// C-ABI worker helper on a blocking-pool thread; `fp` valid, `fp->hx` open.
#[no_mangle]
pub unsafe extern "C" fn hxnet_xfer_folder_send_all(fp: *const HxnetFolderParams) -> c_int {
    let fp = match fp.as_ref() {
        Some(f) => f,
        None => return EINVAL,
    };
    if fp.hx.is_null() || fp.progress.is_none() {
        return EINVAL;
    }
    let base = match path_from(fp.base_path) {
        Some(p) => p,
        None => return EINVAL,
    };
    let cfg = hxhfs::ffi::current_config();

    let mut entries: Vec<PutEntry> = Vec::new();
    collect_put_entries(&base, &[], &mut entries);

    for e in &entries {
        // Wait for FILE_NEXT.
        let mut cmd = [0u8; 2];
        if htxf_read_full(fp.hx, &mut cmd) != 2 {
            return EIO;
        }
        if u16::from_be_bytes(cmd) != FILE_NEXT_CMD {
            return EPROTO;
        }

        // nfi header: len = 4 + sum(3 + nlen_i), type, pathcount. Compute the
        // length wide and refuse an nfi that won't fit the u16 wire fields
        // rather than wrapping and desyncing the server's folder-stream parser.
        let mut wire_len: usize = NFI_LEN_FIXED as usize;
        for c in &e.components {
            wire_len += 3 + c.len();
        }
        if wire_len > u16::MAX as usize || e.components.len() > u16::MAX as usize {
            return ENAMETOOLONG;
        }
        let mut nfi = [0u8; NFI_HEADER_LEN];
        nfi[0..2].copy_from_slice(&(wire_len as u16).to_be_bytes());
        nfi[2..4].copy_from_slice(&(e.is_folder as u16).to_be_bytes());
        nfi[4..6].copy_from_slice(&(e.components.len() as u16).to_be_bytes());
        if xfer_write(fp.hx, &nfi).is_err() {
            return EIO;
        }

        for c in &e.components {
            if c.len() > 255 {
                return ENAMETOOLONG;
            }
            let ch = [0u8, 0u8, c.len() as u8];
            if xfer_write(fp.hx, &ch).is_err() {
                return EIO;
            }
            if !c.is_empty() && xfer_write(fp.hx, c).is_err() {
                return EIO;
            }
        }

        if e.is_folder {
            continue; // folder marker — no payload
        }

        // File leaf — server replies FILE_SEND (fresh) or FILE_RESUME.
        let mut cmd = [0u8; 2];
        if htxf_read_full(fp.hx, &mut cmd) != 2 {
            return EIO;
        }
        let cmd = u16::from_be_bytes(cmd);
        let mut data_pos = 0u64;
        let mut rsrc_pos = 0u64;
        if cmd == FILE_RESUME_CMD {
            let mut rl = [0u8; 2];
            if htxf_read_full(fp.hx, &mut rl) != 2 {
                return EIO;
            }
            let rlen = u16::from_be_bytes(rl) as usize;
            if rlen > RFLT_MAX {
                return EPROTO;
            }
            let mut rflt = vec![0u8; rlen];
            if rlen > 0 && htxf_read_full(fp.hx, &mut rflt) != rlen as isize {
                return EIO;
            }
            if rlen >= RFLT_MIN_FOR_DATA {
                data_pos = u32::from_be_bytes(
                    rflt[RFLT_DATA_POS_OFF..RFLT_DATA_POS_OFF + 4]
                        .try_into()
                        .unwrap(),
                ) as u64;
            }
            if rlen >= RFLT_MIN_FOR_RSRC {
                rsrc_pos = u32::from_be_bytes(
                    rflt[RFLT_RSRC_POS_OFF..RFLT_RSRC_POS_OFF + 4]
                        .try_into()
                        .unwrap(),
                ) as u64;
            }
        } else if cmd != FILE_SEND_CMD {
            return EPROTO;
        }

        // Local fork sizes.
        let md = match std::fs::metadata(&e.full) {
            Ok(m) => m,
            Err(err) => return io_errno(&err),
        };
        let data_size = md.len();
        let rsrc_size = hfs::resource_len(&cfg, &e.full);
        let com = hfs::comment_len(&cfg, &e.full) as u64;

        // Per-file payload size — MUST equal exactly what hxnet_xfer_file_send_one
        // writes (FILP header + comment + data fork + the unconditional 16-byte
        // MACR marker + rsrc fork), or the trailing bytes desync the server's
        // parse of the next file's nfi and the whole folder stream fails.
        let file_size = FILP_HEADER_LEN as u64
            + com
            + data_size.wrapping_sub(data_pos)
            + ffo::FORK_HEADER_LEN as u64
            + rsrc_size.wrapping_sub(rsrc_pos);
        // The folder-stream size header is a u32 on the wire; a file whose FFO
        // payload exceeds 4 GiB can't be represented, so refuse it explicitly
        // rather than truncating and desyncing the stream. (The C path silently
        // truncated here.)
        let file_size = match u32::try_from(file_size) {
            Ok(v) => v,
            Err(_) => return EFBIG,
        };
        if xfer_write(fp.hx, &file_size.to_be_bytes()).is_err() {
            return EIO;
        }

        let path_c = match cstring_from_path(&e.full) {
            Some(c) => c,
            None => return EINVAL,
        };
        let per = per_file_params(
            fp,
            path_c.as_ptr(),
            0,
            data_pos,
            rsrc_pos,
            data_size,
            rsrc_size,
        );
        let rv = hxnet_xfer_file_send_one(&per);
        if rv != 0 {
            return rv;
        }
    }
    0
}

#[cfg(test)]
mod send_capture_tests {
    use super::*;
    use crate::htxf::HtxfConn;
    use std::net::{TcpListener, TcpStream};

    // Capture the exact bytes hxnet_xfer_file_send_one writes for a plain
    // no-comment / no-rsrc file, and assert the mhxd file_recv framing
    // invariants: 133-byte FILP header (buf[39] = 77), DATA fork length at
    // offset 129, then the raw data, so tot_pos == 133 + body_len exactly.
    #[test]
    fn plain_file_header_is_133_and_frames_correctly() {
        let body = b"hello upload from file_send_one\n";
        let dir = std::env::temp_dir().join(format!("hxnet_send_cap_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("src.txt");
        std::fs::write(&path, body).unwrap();

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let reader = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut got = Vec::new();
            s.read_to_end(&mut got).unwrap();
            got
        });

        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let path_c = std::ffi::CString::new(path.to_str().unwrap()).unwrap();

        extern "C" fn noop_progress(_u: *mut c_void, _d: u64) {}
        let params = HxnetXferParams {
            hx: &mut conn as *mut HtxfConn,
            path: path_c.as_ptr(),
            file_budget: 0,
            data_pos: 0,
            rsrc_pos: 0,
            opt_preview: 0,
            opt_folder: 0,
            opt_large: 0,
            preview: std::ptr::null_mut(),
            user_data: std::ptr::null_mut(),
            progress: Some(noop_progress),
            preview_chunk: None,
            preview_set_info: None,
            preview_done: None,
            data_size: body.len() as u64,
            rsrc_size: 0,
        };
        let rv = unsafe { hxnet_xfer_file_send_one(&params) };
        assert_eq!(rv, 0, "send returned error");
        // Drop the connection so the socket closes and read_to_end returns.
        drop(conn);

        let got = reader.join().unwrap();
        let _ = std::fs::remove_dir_all(&dir);

        let hex: String = got[..133.min(got.len())]
            .iter()
            .map(|b| format!("{:02x}", b))
            .collect();
        eprintln!("[capture] total={} header133={}", got.len(), hex);

        // Header is 133 bytes (comlen == 0) + body + a trailing 16-byte MACR
        // (written unconditionally, mirroring C; the server discards it once
        // tot_pos == tot_len).
        assert_eq!(
            got.len(),
            133 + body.len() + 16,
            "wire = 133 header + body + 16 MACR; got {}",
            got.len()
        );
        assert_eq!(&got[0..4], b"FILP", "FILP magic");
        assert_eq!(got[38], 0, "buf[38] high byte must be 0 for short comment");
        assert_eq!(got[39], 77, "buf[39] must be 65+comlen+12 = 77");
        // DATA fork marker + length: mhxd reads L32 at stream offset 129.
        assert_eq!(&got[117..121], b"DATA", "DATA fork marker at 117");
        let data_len = u32::from_be_bytes([got[129], got[130], got[131], got[132]]);
        assert_eq!(data_len as usize, body.len(), "DATA length at offset 129");
        assert_eq!(
            &got[133..133 + body.len()],
            body,
            "raw data fork follows header"
        );
        // Trailing 16 bytes are the MACR marker (rsrc_size == 0).
        assert_eq!(
            &got[133 + body.len()..133 + body.len() + 4],
            b"MACR",
            "MACR marker"
        );
    }
}

#[cfg(test)]
mod folder_loopback_tests {
    //! Server-independent round-trip coverage for the W3 folder state machines,
    //! driving each client function against a minimal mock of the *other* side
    //! of the Hotline 1.5 folder protocol over a loopback socket.
    use super::*;
    use crate::htxf::HtxfConn;
    use std::net::{TcpListener, TcpStream};

    // The full FILP body hxnet_xfer_file_send_one emits for `body` (133-byte
    // header + data fork + 16-byte MACR), generated by running the send path
    // into a loopback capture.
    fn gen_filp(body: &[u8]) -> Vec<u8> {
        let dir = std::env::temp_dir().join(format!(
            "hxnet_folder_gen_{}_{}",
            std::process::id(),
            body.len()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("gen.bin");
        std::fs::write(&path, body).unwrap();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let reader = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut got = Vec::new();
            s.read_to_end(&mut got).unwrap();
            got
        });
        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let path_c = std::ffi::CString::new(path.to_str().unwrap()).unwrap();
        extern "C" fn noop(_u: *mut c_void, _d: u64) {}
        let params = HxnetXferParams {
            hx: &mut conn as *mut HtxfConn,
            path: path_c.as_ptr(),
            file_budget: 0,
            data_pos: 0,
            rsrc_pos: 0,
            opt_preview: 0,
            opt_folder: 1,
            opt_large: 0,
            preview: std::ptr::null_mut(),
            user_data: std::ptr::null_mut(),
            progress: Some(noop),
            preview_chunk: None,
            preview_set_info: None,
            preview_done: None,
            data_size: body.len() as u64,
            rsrc_size: 0,
        };
        assert_eq!(unsafe { hxnet_xfer_file_send_one(&params) }, 0);
        drop(conn);
        let got = reader.join().unwrap();
        let _ = std::fs::remove_dir_all(&dir);
        got
    }

    extern "C" fn noop_progress(_u: *mut c_void, _d: u64) {}

    fn folder_params(hx: *mut HtxfConn, base: *const c_char) -> HxnetFolderParams {
        HxnetFolderParams {
            hx,
            base_path: base,
            opt_preview: 0,
            opt_folder: 1,
            opt_large: 0,
            user_data: std::ptr::null_mut(),
            progress: Some(noop_progress),
        }
    }

    // Client upload: hxnet_xfer_folder_send_all against a mock server that plays
    // mhxd's folder_recv role — drive FILE_NEXT, read the nfi + components, and
    // for files read the declared size + that many body bytes.
    #[test]
    fn folder_send_uploads_tree() {
        let dir = std::env::temp_dir().join(format!("hxnet_fsend_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("a.txt"), b"aaa").unwrap();
        std::fs::write(dir.join("b.txt"), b"bbbb").unwrap();

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut got: Vec<(Vec<u8>, u32, Vec<u8>)> = Vec::new();
            loop {
                if s.write_all(&FILE_NEXT_CMD.to_be_bytes()).is_err() {
                    break;
                }
                let mut nfi = [0u8; NFI_HEADER_LEN];
                if s.read_exact(&mut nfi).is_err() {
                    break; // client closed — tree exhausted
                }
                let ftype = u16::from_be_bytes([nfi[2], nfi[3]]);
                let pathcount = u16::from_be_bytes([nfi[4], nfi[5]]);
                let mut name = Vec::new();
                for _ in 0..pathcount {
                    let mut ph = [0u8; 3];
                    s.read_exact(&mut ph).unwrap();
                    let mut nm = vec![0u8; ph[2] as usize];
                    s.read_exact(&mut nm).unwrap();
                    name = nm;
                }
                if ftype == 1 {
                    continue; // folder marker
                }
                s.write_all(&FILE_SEND_CMD.to_be_bytes()).unwrap();
                let mut sz = [0u8; 4];
                s.read_exact(&mut sz).unwrap();
                let size = u32::from_be_bytes(sz);
                let mut blob = vec![0u8; size as usize];
                s.read_exact(&mut blob).unwrap();
                got.push((name, size, blob));
            }
            got
        });

        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let base_c = std::ffi::CString::new(dir.to_str().unwrap()).unwrap();
        let fp = folder_params(&mut conn as *mut HtxfConn, base_c.as_ptr());
        let rv = unsafe { hxnet_xfer_folder_send_all(&fp) };
        drop(conn);
        let got = server.join().unwrap();
        let _ = std::fs::remove_dir_all(&dir);

        assert_eq!(rv, 0, "folder_send_all returned error");
        assert_eq!(got.len(), 2, "server should receive two files");
        for (name, size, blob) in &got {
            let body: &[u8] = if name == b"a.txt" { b"aaa" } else { b"bbbb" };
            // Declared size == actual bytes == 133 header + body + 16 MACR.
            assert_eq!(*size as usize, blob.len(), "declared size matches bytes");
            assert_eq!(
                *size as usize,
                133 + body.len() + 16,
                "folder file size accounting"
            );
            assert_eq!(&blob[133..133 + body.len()], body, "data fork bytes");
        }
    }

    // Client download: hxnet_xfer_folder_recv_all against a mock server that
    // plays mhxd's folder_send role — answer each FILE_NEXT with an nfi +
    // component, then on FILE_SEND emit the declared size + FILP body. After the
    // last file, close on the trailing FILE_NEXT so the client sees a clean end.
    #[test]
    fn folder_recv_downloads_tree() {
        let files: Vec<(&[u8], &[u8])> = vec![(b"a.txt", b"alpha body"), (b"b.txt", b"beta!")];
        let bodies: Vec<(Vec<u8>, Vec<u8>)> = files
            .iter()
            .map(|(n, b)| (n.to_vec(), gen_filp(b)))
            .collect();

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            for (name, filp) in &bodies {
                let mut cmd = [0u8; 2];
                s.read_exact(&mut cmd).unwrap();
                assert_eq!(u16::from_be_bytes(cmd), FILE_NEXT_CMD);
                // nfi: len = 4 + 3 + nlen, type = 0 (file), pathcount = 1.
                let wire_len = NFI_LEN_FIXED + 3 + name.len() as u16;
                let mut nfi = [0u8; NFI_HEADER_LEN];
                nfi[0..2].copy_from_slice(&wire_len.to_be_bytes());
                nfi[2..4].copy_from_slice(&0u16.to_be_bytes());
                nfi[4..6].copy_from_slice(&1u16.to_be_bytes());
                s.write_all(&nfi).unwrap();
                s.write_all(&[0, 0, name.len() as u8]).unwrap();
                s.write_all(name).unwrap();
                // FILE_SEND, then the declared size + FILP body.
                let mut cmd = [0u8; 2];
                s.read_exact(&mut cmd).unwrap();
                assert_eq!(u16::from_be_bytes(cmd), FILE_SEND_CMD);
                s.write_all(&(filp.len() as u32).to_be_bytes()).unwrap();
                s.write_all(filp).unwrap();
            }
            // Trailing FILE_NEXT → close (clean end-of-stream for the client).
            let mut cmd = [0u8; 2];
            let _ = s.read_exact(&mut cmd);
        });

        let dest = std::env::temp_dir().join(format!("hxnet_frecv_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dest);
        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let base_c = std::ffi::CString::new(dest.to_str().unwrap()).unwrap();
        let fp = folder_params(&mut conn as *mut HtxfConn, base_c.as_ptr());
        let rv = unsafe { hxnet_xfer_folder_recv_all(&fp) };
        drop(conn);
        server.join().unwrap();

        assert_eq!(rv, 0, "folder_recv_all returned error");
        let a = std::fs::read(dest.join("a.txt")).unwrap_or_default();
        let b = std::fs::read(dest.join("b.txt")).unwrap_or_default();
        let _ = std::fs::remove_dir_all(&dest);
        assert_eq!(a, b"alpha body", "downloaded a.txt");
        assert_eq!(b, b"beta!", "downloaded b.txt");
    }

    // The plaintext transport can hand a control read fewer bytes than asked
    // for; htxf_read_full must reassemble. A mock that emits the nfi header +
    // component + size one byte at a time (flush between) drives that
    // reassembly — the download must still land byte-exact.
    #[test]
    fn folder_recv_tolerates_dribbled_control_reads() {
        let name: &[u8] = b"drib.txt";
        let body: &[u8] = b"dribbled control reads must reassemble\n";
        let filp = gen_filp(body);

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            // Byte-at-a-time writer with a flush + tiny sleep, to make the peer's
            // read return partial buffers.
            fn dribble(s: &mut TcpStream, bytes: &[u8]) {
                for b in bytes {
                    s.write_all(std::slice::from_ref(b)).unwrap();
                    s.flush().unwrap();
                    std::thread::sleep(std::time::Duration::from_millis(1));
                }
            }
            let (mut s, _) = listener.accept().unwrap();
            let mut cmd = [0u8; 2];
            s.read_exact(&mut cmd).unwrap();
            let wire_len = NFI_LEN_FIXED + 3 + name.len() as u16;
            let mut nfi = [0u8; NFI_HEADER_LEN];
            nfi[0..2].copy_from_slice(&wire_len.to_be_bytes());
            nfi[2..4].copy_from_slice(&0u16.to_be_bytes());
            nfi[4..6].copy_from_slice(&1u16.to_be_bytes());
            dribble(&mut s, &nfi);
            dribble(&mut s, &[0, 0, name.len() as u8]);
            dribble(&mut s, name);
            s.read_exact(&mut cmd).unwrap();
            dribble(&mut s, &(filp.len() as u32).to_be_bytes());
            s.write_all(&filp).unwrap();
            // Trailing FILE_NEXT → close for a clean end-of-stream.
            let _ = s.read_exact(&mut cmd);
        });

        let dest = std::env::temp_dir().join(format!("hxnet_frecv_drib_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dest);
        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let base_c = std::ffi::CString::new(dest.to_str().unwrap()).unwrap();
        let fp = folder_params(&mut conn as *mut HtxfConn, base_c.as_ptr());
        let rv = unsafe { hxnet_xfer_folder_recv_all(&fp) };
        drop(conn);
        server.join().unwrap();

        assert_eq!(rv, 0, "folder_recv_all returned error under dribbled reads");
        let got = std::fs::read(dest.join("drib.txt")).unwrap_or_default();
        let _ = std::fs::remove_dir_all(&dest);
        assert_eq!(
            got, body,
            "download must reassemble the dribbled control reads"
        );
    }

    // Drive hxnet_xfer_folder_recv_all's first nfi entry with attacker-shaped
    // path components and assert it rejects them instead of building an
    // unbounded / escaping path. Returns the recv result + whether the download
    // dir was even created.
    fn recv_first_entry(ftype: u16, components: &[&[u8]]) -> c_int {
        let comps: Vec<Vec<u8>> = components.iter().map(|c| c.to_vec()).collect();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut cmd = [0u8; 2];
            if s.read_exact(&mut cmd).is_err() {
                return;
            }
            let mut wire_len = NFI_LEN_FIXED as usize;
            for c in &comps {
                wire_len += 3 + c.len();
            }
            let mut nfi = [0u8; NFI_HEADER_LEN];
            nfi[0..2].copy_from_slice(&(wire_len as u16).to_be_bytes());
            nfi[2..4].copy_from_slice(&ftype.to_be_bytes());
            nfi[4..6].copy_from_slice(&(comps.len() as u16).to_be_bytes());
            let _ = s.write_all(&nfi);
            for c in &comps {
                let _ = s.write_all(&[0, 0, c.len() as u8]);
                let _ = s.write_all(c);
            }
            // Drain whatever the client sends next, then close.
            let _ = s.read_exact(&mut cmd);
        });

        let dest = std::env::temp_dir().join(format!(
            "hxnet_frecv_evil_{}_{}",
            std::process::id(),
            ftype as usize + components.len()
        ));
        let _ = std::fs::remove_dir_all(&dest);
        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let base_c = std::ffi::CString::new(dest.to_str().unwrap()).unwrap();
        let fp = folder_params(&mut conn as *mut HtxfConn, base_c.as_ptr());
        let rv = unsafe { hxnet_xfer_folder_recv_all(&fp) };
        drop(conn);
        let _ = server.join();
        let _ = std::fs::remove_dir_all(&dest);
        rv
    }

    #[test]
    fn folder_recv_rejects_hostile_path_components() {
        // Parent-directory traversal.
        assert_eq!(recv_first_entry(1, &[b".."]), EINVAL);
        // Embedded POSIX + Windows separators.
        assert_eq!(recv_first_entry(1, &[b"a/b"]), EINVAL);
        assert_eq!(recv_first_entry(1, &[b"a\\b"]), EINVAL);
        // Empty component.
        assert_eq!(recv_first_entry(1, &[b""]), EINVAL);
        // A joined path past MAXPATHLEN (20 × 250-byte components) must be
        // refused with ENAMETOOLONG rather than building an unbounded path.
        let long: Vec<u8> = vec![b'x'; 250];
        let many: Vec<&[u8]> = std::iter::repeat_n(long.as_slice(), 20).collect();
        assert_eq!(recv_first_entry(1, &many), ENAMETOOLONG);
    }
}

#[cfg(test)]
mod recv_timeout_tests {
    use super::*;
    use crate::htxf::HtxfConn;
    use std::net::{TcpListener, TcpStream};
    use std::time::{Duration, Instant};

    // Generate a valid FILP header + data fork (no trailing MACR) by running the
    // send path into a loopback capture and slicing off its 16-byte MACR tail.
    fn filp_header_and_data(body: &[u8]) -> Vec<u8> {
        // Unique per call: tests run in parallel and each call makes + removes
        // this dir, so a shared name would race.
        static SEQ: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let seq = SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let dir =
            std::env::temp_dir().join(format!("hxnet_recv_gen_{}_{}", std::process::id(), seq));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("gen.txt");
        std::fs::write(&path, body).unwrap();

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let reader = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut got = Vec::new();
            s.read_to_end(&mut got).unwrap();
            got
        });
        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let path_c = std::ffi::CString::new(path.to_str().unwrap()).unwrap();
        extern "C" fn noop(_u: *mut c_void, _d: u64) {}
        let params = HxnetXferParams {
            hx: &mut conn as *mut HtxfConn,
            path: path_c.as_ptr(),
            file_budget: 0,
            data_pos: 0,
            rsrc_pos: 0,
            opt_preview: 0,
            opt_folder: 0,
            opt_large: 0,
            preview: std::ptr::null_mut(),
            user_data: std::ptr::null_mut(),
            progress: Some(noop),
            preview_chunk: None,
            preview_set_info: None,
            preview_done: None,
            data_size: body.len() as u64,
            rsrc_size: 0,
        };
        assert_eq!(unsafe { hxnet_xfer_file_send_one(&params) }, 0);
        drop(conn);
        let full = reader.join().unwrap();
        let _ = std::fs::remove_dir_all(&dir);
        // Drop the trailing 16-byte MACR marker: keep 133-byte header + data.
        full[..133 + body.len()].to_vec()
    }

    // A server that over-declares file_budget (a phantom trailing MACR) but never
    // sends it must not hang the download worker until the socket closes — the
    // ~2s end-of-transfer stall. The worker should finish promptly (bounded by
    // MACR_DRAIN_TIMEOUT_MS) and return success.
    #[test]
    fn phantom_macr_does_not_hang_completion() {
        let body = b"downloaded file body, no resource fork\n";
        let wire = filp_header_and_data(body);
        // Declare 16 phantom bytes beyond the header+data the server sends.
        let file_budget = wire.len() as u64 + ffo::FORK_HEADER_LEN as u64;

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        // Server: send header+data, then hold the socket open well past the drain
        // timeout without sending the (nonexistent) MACR, then close.
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            s.write_all(&wire).unwrap();
            s.flush().unwrap();
            std::thread::sleep(Duration::from_millis(1500));
            // socket closes on drop
        });

        let out_dir = std::env::temp_dir().join(format!("hxnet_recv_out_{}", std::process::id()));
        std::fs::create_dir_all(&out_dir).unwrap();
        let out_path = out_dir.join("dl.txt");
        let out_c = std::ffi::CString::new(out_path.to_str().unwrap()).unwrap();

        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        extern "C" fn noop(_u: *mut c_void, _d: u64) {}
        let params = HxnetXferParams {
            hx: &mut conn as *mut HtxfConn,
            path: out_c.as_ptr(),
            file_budget,
            data_pos: 0,
            rsrc_pos: 0,
            opt_preview: 0,
            opt_folder: 0,
            opt_large: 0,
            preview: std::ptr::null_mut(),
            user_data: std::ptr::null_mut(),
            progress: Some(noop),
            preview_chunk: None,
            preview_set_info: None,
            preview_done: None,
            data_size: 0,
            rsrc_size: 0,
        };
        let start = Instant::now();
        let rv = unsafe { hxnet_xfer_file_recv_one(&params) };
        let elapsed = start.elapsed();
        drop(conn);

        let got = std::fs::read(&out_path).unwrap_or_default();
        let _ = std::fs::remove_dir_all(&out_dir);
        let _ = server.join();

        assert_eq!(
            rv, 0,
            "recv should complete cleanly despite the phantom MACR"
        );
        assert_eq!(got, body, "data fork must be written correctly");
        assert!(
            elapsed < Duration::from_millis(1000),
            "recv should finish within the drain timeout, not block until close; took {:?}",
            elapsed
        );
    }

    // A *partial* MACR marker (some bytes then a stall) is a genuine desync, not
    // a phantom: the drain timeout must not swallow it as a clean completion, or
    // a real resource fork would be silently truncated. The worker must error.
    #[test]
    fn partial_macr_is_an_error_not_a_clean_finish() {
        let body = b"file with a resource fork coming\n";
        let mut wire = filp_header_and_data(body);
        // Append a truncated MACR marker (8 of 16 bytes) so the drain read
        // consumes some bytes and then stalls waiting for the rest.
        wire.extend_from_slice(b"MACR\0\0\0\0");
        // Declare a full marker + a resource fork beyond the header+data.
        let file_budget = (filp_header_and_data(body).len() + ffo::FORK_HEADER_LEN + 32) as u64;

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            s.write_all(&wire).unwrap();
            s.flush().unwrap();
            std::thread::sleep(Duration::from_millis(1500));
        });

        let out_dir =
            std::env::temp_dir().join(format!("hxnet_recv_partial_{}", std::process::id()));
        std::fs::create_dir_all(&out_dir).unwrap();
        let out_path = out_dir.join("dl.txt");
        let out_c = std::ffi::CString::new(out_path.to_str().unwrap()).unwrap();

        let stream = TcpStream::connect(addr).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        extern "C" fn noop(_u: *mut c_void, _d: u64) {}
        let params = HxnetXferParams {
            hx: &mut conn as *mut HtxfConn,
            path: out_c.as_ptr(),
            file_budget,
            data_pos: 0,
            rsrc_pos: 0,
            opt_preview: 0,
            opt_folder: 0,
            opt_large: 0,
            preview: std::ptr::null_mut(),
            user_data: std::ptr::null_mut(),
            progress: Some(noop),
            preview_chunk: None,
            preview_set_info: None,
            preview_done: None,
            data_size: 0,
            rsrc_size: 0,
        };
        let rv = unsafe { hxnet_xfer_file_recv_one(&params) };
        drop(conn);
        let _ = std::fs::remove_dir_all(&out_dir);
        let _ = server.join();

        assert_ne!(
            rv, 0,
            "a partial MACR marker must surface as an error, not a clean finish"
        );
    }
}
