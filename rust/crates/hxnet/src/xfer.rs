//! W1: the single-file HTXF download copy loop — the Rust port of C's
//! `xfers_recv.c::file_recv_one` (+ its `rd_wr_recv` / `preview_get` helpers).
//!
//! The file-transfer copy loops. This removes the per-chunk C↔Rust
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

use hxfiles_xfer::{ffo, rflt};
use hxhfs::hfs::{self, HfsInfo};

use crate::htxf::HtxfConn;

// errno-ish return codes (the C returned raw errno; callers only test != 0).
const EIO: c_int = 5;
const EPIPE: c_int = 32;
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
/// The least a folder download waits for another entry once the server's
/// announced count has arrived. See `hxnet_xfer_folder_recv_all`.
const FOLDER_END_GRACE: std::time::Duration = std::time::Duration::from_secs(1);

/// How reading an entry header within a grace ended.
enum EntryRead {
    /// The header, whole.
    Entry,
    /// The server closed before a byte of it.
    Closed,
    /// Not a byte of it within the grace.
    Silent,
    /// Anything else: an error, a reset, an abort, a header cut short.
    Failed,
}

/// Read an entry header into `buf`, waiting up to `grace` for each read.
/// Only a close, or silence, before its first byte may end the tree; a
/// header cut short, a reset or an abort is a failure like any other.
fn read_entry_within(hx: &mut HtxfConn, buf: &mut [u8], grace: std::time::Duration) -> EntryRead {
    if hx.set_timeout(Some(grace)).is_err() {
        return EntryRead::Failed;
    }
    let mut got = 0;
    let out = loop {
        if got == buf.len() {
            break EntryRead::Entry;
        }
        match hx.read_some(&mut buf[got..]) {
            Ok(0) if got == 0 => break EntryRead::Closed,
            Ok(0) => break EntryRead::Failed,
            Ok(n) => got += n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(e)
                if got == 0
                    && matches!(
                        e.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                    ) =>
            {
                break EntryRead::Silent
            }
            Err(_) => break EntryRead::Failed,
        }
    };
    if matches!(out, EntryRead::Entry) && hx.set_timeout(None).is_err() {
        return EntryRead::Failed;
    }
    out
}
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
    /// Takes the data fork's length; returns how much of it to stream.
    pub preview_set_info:
        Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, u64) -> u64>,
    pub preview_done: Option<unsafe extern "C" fn(*mut c_void)>,
    /// Local fork sizes for a *send* (`htxf->data_size` / `htxf->rsrc_size`).
    /// Send-only; the receive worker ignores them (it uses `file_budget`).
    pub data_size: u64,
    pub rsrc_size: u64,
}

/// How often a transfer's progress is worth an update on the main loop.
/// The workers report every chunk they copy — tens of thousands a second
/// on a fast link — and a post per chunk kept the main loop busy with
/// progress alone. Twenty a second is as smooth as a progress bar needs.
pub const PROGRESS_INTERVAL: std::time::Duration = std::time::Duration::from_millis(50);

thread_local! {
    /// The transfer this worker thread last posted progress for, and when.
    static LAST_PROGRESS: std::cell::Cell<(usize, Option<std::time::Instant>)> =
        const { std::cell::Cell::new((0, None)) };
}

/// Whether a progress report for transfer `key` should go to the main
/// loop now: the first for a transfer does, and after it one per
/// [`PROGRESS_INTERVAL`]. For the progress callback, which runs on the
/// transfer's worker thread; a thread works one transfer at a time, so
/// the state is the thread's. The bytes are counted whether or not this
/// says to post, so a skipped report loses nothing — the next one carries
/// it — and the worker's caller posts once more at the end.
pub fn progress_due(key: usize) -> bool {
    LAST_PROGRESS.with(|last| {
        let (k, at) = last.get();
        let now = std::time::Instant::now();
        let due = k != key || at.is_none_or(|t| now.duration_since(t) >= PROGRESS_INTERVAL);
        if due {
            last.set((key, Some(now)));
        }
        due
    })
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

/// Read the 16-byte MACR fork header that may follow a data fork.
///
/// `wait` is for a marker the FFO declared (a third fork): it is read like any
/// other header. Without it the marker is optional, so a short read timeout is
/// armed: servers send an empty trailing MACR even when the FFO says INFO +
/// DATA, and some send none. `Ok(None)` is "nothing arrived"; a marker cut off
/// partway is a desynced stream and an error.
unsafe fn read_macr_marker(
    hx: *mut HtxfConn,
    p: &HxnetXferParams,
    wait: bool,
) -> Result<Option<[u8; ffo::FORK_HEADER_LEN]>, c_int> {
    if !wait && crate::htxf::hxnet_htxf_set_read_timeout(hx, MACR_DRAIN_TIMEOUT_MS) != 0 {
        return Err(EIO);
    }
    // Driven directly rather than through read_exact_progress, which collapses
    // "nothing arrived" and "a partial marker" into one error.
    let mut m = [0u8; ffo::FORK_HEADER_LEN];
    let mut got = 0usize;
    let marker = loop {
        let n = crate::htxf::hxnet_htxf_read(hx, m[got..].as_mut_ptr(), m.len() - got);
        if n < 1 {
            break if got == 0 && !wait {
                Ok(None)
            } else {
                Err(EIO)
            };
        }
        p.report(n as u64);
        got += n as usize;
        if got == m.len() {
            break Ok(Some(m));
        }
    };
    // Disarm before the caller reads the resource fork itself, which can take
    // longer than the drain timeout to arrive.
    if !wait && crate::htxf::hxnet_htxf_set_read_timeout(hx, 0) != 0 {
        return Err(EIO);
    }
    marker
}

/// Copy `data_len` bytes off the subchannel into `dst` (C `rd_wr_recv`).
unsafe fn rd_wr_recv<W: Write>(
    hx: *mut HtxfConn,
    dst: &mut W,
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

fn make_hfsinfo(type_creator: [u8; 8], fi: &ffo::FilpInfo, rsrclen: u64) -> HfsInfo {
    HfsInfo {
        type_creator,
        create_time: fi.create_time,
        modify_time: fi.modify_time,
        rsrclen,
        comment: fi.comment.clone(),
    }
}

/// The `done:` tail — rewrite the sidecar with the real type/creator.
fn finish(
    cfg: &hfs::Config,
    path: &Path,
    typecrea: [u8; 8],
    pi: &ffo::FilpInfo,
    rsrclen: u64,
) -> c_int {
    let fi = make_hfsinfo(typecrea, pi, rsrclen);
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

/// What [`hxnet_xfer_file_recv_one`] returns for a preview that stopped at the
/// part of the file it shows: not a failure, but not the whole file either.
pub const HXNET_XFER_STOPPED: c_int = -1;

/// `int hxnet_xfer_file_recv_one (const HxnetXferParams *p)` — receive one file
/// off the HTXF subchannel into `p->path` (was C `file_recv_one`). Returns 0 on
/// success, [`HXNET_XFER_STOPPED`] for a preview that stopped early, an
/// errno-like positive code on failure. Does NOT play the completion
/// sound, post a final update, or close the channel — the caller owns those.
///
/// # Safety
/// C-ABI worker helper, run on a blocking-pool thread. `p` is a valid
/// `HxnetXferParams` whose `hx` transport handle is open.
#[no_mangle]
pub unsafe extern "C" fn hxnet_xfer_file_recv_one(p: *const HxnetXferParams) -> c_int {
    match p.as_ref() {
        Some(p) => file_recv(p, &mut None),
        None => EINVAL,
    }
}

/// A file as its FILP described it: the data fork's whole length, and the
/// file's modify date.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Filp {
    pub len: u64,
    pub modified: [u8; 4],
}

/// Where a resumed file's data fork goes: from `data_pos` when the server
/// resumed it there, as asked, but from the start when it sent the whole
/// fork instead, as Janus does in a folder. `was` is the file as its first
/// FILP described it, whose length tells the two apart; a fork that is
/// neither, or a file with another modify date, changed on the server, and
/// can't be placed.
fn data_from(
    was: Option<Filp>,
    data_pos: u64,
    fork_len: u64,
    modified: [u8; 4],
) -> Result<u64, c_int> {
    match was {
        None => Ok(data_pos),
        Some(was) if was.modified != modified => Err(EIO),
        Some(was) if data_pos.checked_add(fork_len) == Some(was.len) => Ok(data_pos),
        Some(was) if fork_len == was.len => Ok(0),
        Some(_) => Err(EIO),
    }
}

/// [`hxnet_xfer_file_recv_one`], for a file `*was` describes when it is
/// resumed; `*was` is left as the FILP described it.
unsafe fn file_recv(p: &HxnetXferParams, was: &mut Option<Filp>) -> c_int {
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
    let cfg = crate::hfs_config::current();

    // 1. 40-byte FILP fixed header.
    let hdr = match read_exact_progress(hx, 40, p) {
        Ok(h) => h,
        Err(e) => return e,
    };
    // 2. Variable info+comment block, length from FILP bytes 38/39.
    let info_len = match ffo::info_block_len(hdr[38], hdr[39]) {
        Ok(n) => n,
        Err(_) => return EIO,
    };
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
        let fi = make_hfsinfo(*b"HTftHTLC", &pi, p.rsrc_pos);
        let _ = hfs::hfsinfo_write(&cfg, &path, &fi);
    }

    // 4. Data fork.
    let fork_len = pi.data_fork_len;
    let data_pos = match data_from(*was, p.data_pos, fork_len, pi.modify_time) {
        Ok(at) => at,
        Err(e) => {
            *was = None;
            return e;
        }
    };
    *was = Some(Filp {
        len: data_pos + fork_len,
        modified: pi.modify_time,
    });
    // What a fork sent whole repeats of what was asked from `p.data_pos`:
    // its progress was counted when it first came, and the size the server
    // gave before the FILP left it out.
    let again = p.data_pos - data_pos;
    let budget = match p.file_budget {
        0 => 0,
        b => b.saturating_add(again),
    };
    tot_len += fork_len;
    if is_preview {
        // Even an empty file is shown: the window waits on set_info and done.
        if p.preview.is_null() {
            return 0; // nothing to write into; quietly stop
        }
        let mut type_s = [0u8; 5];
        let mut creator_s = [0u8; 5];
        type_s[..4].copy_from_slice(&typecrea[0..4]);
        creator_s[..4].copy_from_slice(&typecrea[4..8]);
        let wanted = match p.preview_set_info {
            Some(set_info) => set_info(
                p.preview,
                type_s.as_ptr() as *const c_char,
                creator_s.as_ptr() as *const c_char,
                fork_len,
            ),
            None => fork_len,
        }
        .min(fork_len);
        if let Err(e) = preview_get(hx, wanted, p) {
            return e;
        }
        // The rest of the file is left unread: the caller closes the
        // channel, which is how a preview ends early. Previews never carry a
        // resource fork.
        return if wanted < fork_len {
            HXNET_XFER_STOPPED
        } else {
            0
        };
    }
    if fork_len != 0 {
        // Emptied only when written from the start: a resumed download seeks
        // to `data_pos` and writes into its partial file.
        let mut f = match std::fs::OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(data_pos == 0)
            .open(&path)
        {
            Ok(f) => f,
            Err(e) => return io_errno(&e),
        };
        if data_pos != 0 && f.seek(SeekFrom::Start(data_pos)).is_err() {
            return EIO;
        }
        let quiet = HxnetXferParams {
            progress: None,
            ..std::ptr::read(p)
        };
        let again = again.min(fork_len);
        if let Err(e) = rd_wr_recv(hx, &mut f, again, &quiet)
            .and_then(|()| rd_wr_recv(hx, &mut f, fork_len - again, p))
        {
            return e;
        }
        let _ = f.sync_all();
    }

    // 5. Resource fork.
    if is_folder && p.file_budget == 0 {
        // Large File: an item whose FFO does not fit the 4-byte per-item size
        // is announced as 0, and its extent comes from the FFO's own fork
        // headers (Capabilities-Large-File, "Per-item transfer size"). Read
        // the MACR marker, waiting for it when the FFO declared a third fork,
        // and discard the resource fork: folder-stream files never keep one.
        let declared_rsrc = u16::from_be_bytes([hdr[22], hdr[23]]) >= 3;
        match read_macr_marker(hx, p, declared_rsrc) {
            Ok(Some(m)) => {
                if let Err(e) = rd_wr_recv(hx, &mut std::io::sink(), ffo::fork_len(&m, large), p) {
                    return e;
                }
            }
            Ok(None) => {}
            Err(e) => return e,
        }
        return finish(&cfg, &path, typecrea, &pi, 0);
    }
    if is_folder {
        // Folder-stream files never persist a resource fork, but the server may
        // have announced file_budget bytes and buffered a MACR marker. Drain up
        // to (file_budget - tot_len) with a short per-read timeout.
        if tot_len < budget
            && crate::htxf::hxnet_htxf_set_read_timeout(hx, MACR_DRAIN_TIMEOUT_MS) == 0
        {
            let mut remaining = budget - tot_len;
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
        return finish(&cfg, &path, typecrea, &pi, 0);
    }
    if tot_len >= budget {
        return finish(&cfg, &path, typecrea, &pi, p.rsrc_pos);
    }
    // 16-byte MACR fork header. The server declared more than the data fork, so
    // a MACR marker *should* follow, but some servers over-declare file_budget
    // and send nothing for a no-resource-fork file: a missing marker finishes
    // cleanly instead of blocking until the server closes the socket (the ~2s
    // stall the blocking C read had). A real marker is buffered right behind
    // the data fork, so the timeout never clips it.
    let m = match read_macr_marker(hx, p, false) {
        Ok(Some(m)) => m,
        Ok(None) => return finish(&cfg, &path, typecrea, &pi, p.rsrc_pos), // no marker at all
        Err(e) => return e, // partial marker: desynced
    };
    let rfork_len = ffo::fork_len(&m, large);
    if rfork_len == 0 {
        return finish(&cfg, &path, typecrea, &pi, p.rsrc_pos);
    }
    // As hfs.c's O_CREAT did: CAP creates the `.rsrc` file, and an AppleDouble
    // fork lives in the container the early sidecar write made.
    let mut opts = hfs::ResourceOpenOptions::new();
    opts.write(true).create(true);
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
    let Some(rsrclen) = p.rsrc_pos.checked_add(rfork_len) else {
        return EFBIG;
    };
    finish(&cfg, &path, typecrea, &pi, rsrclen)
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
/// channel writes it all, one AEAD frame when armed). `!= len` is failure,
/// EPIPE, which a local read's EIO isn't.
unsafe fn xfer_write(hx: *mut HtxfConn, buf: &[u8]) -> Result<(), c_int> {
    if crate::htxf::hxnet_htxf_write(hx, buf.as_ptr(), buf.len()) != buf.len() as isize {
        return Err(EPIPE);
    }
    Ok(())
}

/// Whether `hx`'s transfer was aborted; a null `hx` was not.
unsafe fn aborted(hx: *mut HtxfConn) -> bool {
    if hx.is_null() {
        return false;
    }
    (*hx).is_aborted()
}

/// Copy `data_len` bytes from a local fork file to the subchannel (C
/// `rd_wr_send`). A local read of 0 (EOF before the promised length) is EIO,
/// matching the C `read(...) < 1` gate.
unsafe fn rd_wr_send<R: Read>(
    hx: *mut HtxfConn,
    src: &mut R,
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
    let cfg = crate::hfs_config::current();

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
    let data_hdr = match ffo::pack_fork_header(b"DATA", p.data_size.wrapping_sub(p.data_pos), large)
    {
        Ok(header) => header,
        Err(_) => return EFBIG,
    };
    hdr[117 + comlen..FILP_HEADER_LEN + comlen].copy_from_slice(&data_hdr);
    if let Err(e) = xfer_write(hx, &hdr) {
        return e;
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

    // With no resource fork left the FILP declared two forks, and its size
    // ends at the data fork; a marker past it is bytes the server never
    // reads. One that closes on them resets the upload, and Janus, reading
    // a folder's next item, waits on them for good.
    if p.rsrc_size.wrapping_sub(p.rsrc_pos) == 0 {
        return 0;
    }

    // MACR fork header. A short write at the marker boundary is a clean stop
    // (the server may not want the resource fork), not an error. The length is
    // the remaining resource fork (rsrc_size - rsrc_pos) so a resumed upload's
    // marker matches what we actually stream below.
    let macr_hdr = match ffo::pack_fork_header(b"MACR", p.rsrc_size.wrapping_sub(p.rsrc_pos), large)
    {
        Ok(header) => header,
        Err(_) => return EFBIG,
    };
    if xfer_write(hx, &macr_hdr).is_err() {
        return 0;
    }
    p.report(16);
    let mut ropts = hfs::ResourceOpenOptions::new();
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

/// A file a folder download wrote, as it was left: its data fork's length
/// and when it was last changed, and whether it arrived whole.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Kept {
    pub len: u64,
    pub modified: Option<std::time::SystemTime>,
    pub whole: bool,
    /// The file as its FILP described it, once it has.
    pub filp: Option<Filp>,
}

impl Kept {
    fn of(path: &Path, whole: bool, filp: Option<Filp>) -> Option<Kept> {
        let md = std::fs::metadata(path).ok()?;
        md.is_file().then(|| Kept {
            len: md.len(),
            modified: md.modified().ok(),
            whole,
            filp,
        })
    }
}

/// What a folder download has written, file by file, so that asking for it
/// again answers each file the server offers as far as it got. The protocol
/// names no size or checksum before the client answers an offer, so only a
/// file this download wrote, and nothing has changed since, is trusted:
/// anything else on disk is asked for whole.
#[derive(Debug, Default)]
pub struct FolderLedger {
    files: std::collections::HashMap<Vec<u8>, Kept>,
}

/// How a folder download answers the server's offer of a file.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Answer {
    Send,
    Resume { data: u32 },
    Skip,
}

/// The answer for a file this download left as `kept`, now on disk as
/// `now`. An RFLT offset is 32 bits.
pub fn answer(kept: Option<Kept>, now: Option<Kept>) -> Answer {
    match (kept, now) {
        (Some(k), Some(n)) if (k.len, k.modified) == (n.len, n.modified) => {
            match (k.whole, k.filp, u32::try_from(k.len)) {
                (true, ..) => Answer::Skip,
                (false, Some(_), Ok(data)) => Answer::Resume { data },
                _ => Answer::Send,
            }
        }
        _ => Answer::Send,
    }
}

impl FolderLedger {
    fn kept(&self, rel: &[u8]) -> Option<Kept> {
        self.files.get(rel).copied()
    }

    fn note(&mut self, rel: &[u8], kept: Option<Kept>) {
        match kept {
            Some(k) => self.files.insert(rel.to_vec(), k),
            None => self.files.remove(rel),
        };
    }

    /// The bytes under `base` a download resumed now would not ask for.
    pub fn held(&self, base: &Path) -> u64 {
        self.files
            .iter()
            .filter_map(|(rel, k)| {
                let now = Kept::of(&base.join(os_from_bytes(rel)), false, None)?;
                (answer(Some(*k), Some(now)) != Answer::Send).then_some(k.len)
            })
            .sum()
    }
}

/// `int hxnet_xfer_folder_recv_all (const HxnetFolderParams *fp)` — receive a
/// folder tree into `fp->base_path` (was C `folder_recv_all`), every file
/// asked for whole. See [`folder_recv`].
///
/// # Safety
/// C-ABI worker helper on a blocking-pool thread; `fp` valid, `fp->hx` open.
#[no_mangle]
pub unsafe extern "C" fn hxnet_xfer_folder_recv_all(fp: *const HxnetFolderParams) -> c_int {
    match fp.as_ref() {
        Some(fp) => folder_recv(fp, &mut FolderLedger::default()),
        None => EINVAL,
    }
}

/// Receive a folder tree into `fp.base_path`, noting in `ledger` what it
/// writes. Drives the FILE_NEXT/FILE_SEND loop: request the next entry, read
/// its nfi header + path components, mkdir folders, or answer a file as
/// `ledger` says ([`answer`]) and receive it via `hxnet_xfer_file_recv_one`.
/// Returns 0 on success (including the clean end-of-stream when the server
/// closes the socket), an errno-like positive code on failure.
///
/// # Safety
/// Worker helper on a blocking-pool thread; `fp.hx` open.
pub unsafe fn folder_recv(fp: &HxnetFolderParams, ledger: &mut FolderLedger) -> c_int {
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

    let expected = (*fp.hx).folder_items();
    let mut received: u32 = 0;
    // The longest the server has taken to answer a FILE_NEXT so far.
    let mut slowest = std::time::Duration::ZERO;
    loop {
        // Request the next entry.
        if xfer_write(fp.hx, &FILE_NEXT_CMD.to_be_bytes()).is_err() {
            return EIO;
        }
        let asked = std::time::Instant::now();
        let mut nfi = [0u8; NFI_HEADER_LEN];
        if expected != 0 && received >= expected {
            // Past the announced count the tree is probably done, and some
            // servers only close on a timeout of their own: give the next
            // entry a grace instead. The count is only a guide — mhxd counts
            // the top level alone (docs/mhxd-bugs.md) — so an entry that
            // does come is read as any other, and the grace is several
            // times the slowest answer yet, for a slow link.
            let grace = FOLDER_END_GRACE.max(slowest * 3);
            match read_entry_within(&mut *fp.hx, &mut nfi, grace) {
                EntryRead::Entry => {}
                EntryRead::Closed | EntryRead::Silent => return 0,
                EntryRead::Failed => return EIO,
            }
        } else {
            // nfi header: a 0-byte read is the clean end-of-stream (server
            // closed).
            let n = htxf_read_full(fp.hx, &mut nfi);
            if n == 0 {
                return 0;
            }
            if n != NFI_HEADER_LEN as isize {
                return EIO;
            }
        }
        slowest = slowest.max(asked.elapsed());
        received = received.saturating_add(1);
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

        let kept = ledger.kept(&rel);
        let mut was = None;
        let data_pos = match answer(kept, Kept::of(&full, false, None)) {
            // The FILE_NEXT the loop starts with passes over it.
            Answer::Skip => continue,
            Answer::Send => {
                if xfer_write(fp.hx, &FILE_SEND_CMD.to_be_bytes()).is_err() {
                    return EIO;
                }
                0
            }
            // A folder's files keep no resource fork: the one that comes is
            // read past, so it is asked for whole.
            Answer::Resume { data } => {
                let mut cmd = FILE_RESUME_CMD.to_be_bytes().to_vec();
                cmd.extend_from_slice(&(rflt::CANONICAL_LEN as u16).to_be_bytes());
                cmd.extend_from_slice(&rflt::encode(rflt::Resume { data, resource: 0 }));
                if xfer_write(fp.hx, &cmd).is_err() {
                    return EIO;
                }
                was = kept.and_then(|k| k.filp);
                u64::from(data)
            }
        };
        let mut sz = [0u8; 4];
        if htxf_read_full(fp.hx, &mut sz) != 4 {
            return EIO;
        }
        let file_size = u32::from_be_bytes(sz) as u64;

        let path_c = match cstring_from_path(&full) {
            Some(c) => c,
            None => return EINVAL,
        };
        let per = per_file_params(fp, path_c.as_ptr(), file_size, data_pos, 0, 0, 0);
        let rv = file_recv(&per, &mut was);
        ledger.note(&rel, Kept::of(&full, rv == 0, was));
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

/// A file a folder upload began sending, as it was then.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Offered {
    len: u64,
    modified: Option<std::time::SystemTime>,
}

impl Offered {
    fn of(md: &std::fs::Metadata) -> Offered {
        Offered {
            len: md.len(),
            modified: md.modified().ok(),
        }
    }
}

/// What a folder upload has sent, so that asking for it again can trust what
/// the server says it holds: the files it began sending, as each was then,
/// the folders the server has made, and those it may have: offered, but
/// not seen made before the connection ended.
#[derive(Debug, Default)]
pub struct UploadLedger {
    files: std::collections::HashMap<Vec<u8>, Offered>,
    folders: std::collections::HashSet<Vec<u8>>,
    unsure: std::collections::HashSet<Vec<u8>>,
}

impl UploadLedger {
    /// Whether a file under `base` this upload has sent from has changed
    /// since: resumed, the server would splice the two. One removed since
    /// isn't offered again.
    pub fn changed(&self, base: &Path) -> bool {
        self.files.iter().any(|(rel, was)| {
            std::fs::metadata(base.join(os_from_bytes(rel)))
                .is_ok_and(|md| Offered::of(&md) != *was)
        })
    }
}

/// The bytes of the regular files under `base`, and how many files and
/// folders a folder upload of it offers, resumed from `sent` or new: what
/// FILE_PUTFOLDER's 108 and 220 say, the 1.9 server and Janus ending the
/// upload after that many items.
pub fn put_totals(base: &Path, sent: Option<&UploadLedger>) -> (u64, u32) {
    let mut entries = Vec::new();
    collect_put_entries(base, &[], &mut entries);
    let bytes = entries
        .iter()
        .filter(|e| !e.is_folder)
        .filter_map(|e| std::fs::symlink_metadata(&e.full).ok())
        .map(|md| md.len())
        .fold(0u64, u64::saturating_add);
    let made = |e: &PutEntry| {
        e.is_folder && sent.is_some_and(|s| s.folders.contains(&e.components.join(&b'/')))
    };
    let items = entries.iter().filter(|e| !made(e)).count();
    (bytes, items.try_into().unwrap_or(u32::MAX))
}

/// What [`folder_send`] returns when the server holds more of a file than it
/// has, or holds part of one that has changed since this upload sent it.
pub const HXNET_XFER_CHANGED: c_int = -2;

/// What [`folder_send`] returns when the server ended the upload at a folder
/// it has, which the ledger now says it made: asked for again, the upload
/// gets past it.
pub const HXNET_XFER_AGAIN: c_int = -3;

/// What [`folder_send`] returns when the server ended the upload at a file in
/// a folder it wasn't offered: its folder may not be the one the upload left,
/// and asked for again, every folder is offered.
pub const HXNET_XFER_REOFFER: c_int = -4;

/// `int hxnet_xfer_folder_send_all (const HxnetFolderParams *fp)` — send the
/// local folder tree at `fp->base_path` (was C `folder_send_all`). See
/// [`folder_send`].
///
/// # Safety
/// C-ABI worker helper on a blocking-pool thread; `fp` valid, `fp->hx` open.
#[no_mangle]
pub unsafe extern "C" fn hxnet_xfer_folder_send_all(fp: *const HxnetFolderParams) -> c_int {
    match fp.as_ref() {
        Some(fp) => folder_send(fp, &mut UploadLedger::default()),
        None => EINVAL,
    }
}

/// Send the local folder tree at `fp.base_path`, noting in `ledger` what it
/// sends. For each server FILE_NEXT, replies with one entry's nfi header and
/// path components; the server answers a file with FILE_SEND, FILE_RESUME and
/// an RFLT of what it holds, or FILE_NEXT to pass over it and ask for the
/// next. A folder `ledger` says the server has made is not offered again:
/// mhxd ends the upload at one that exists. The server closes when done; our
/// next FILE_NEXT read short-reads and we stop. Returns 0 on success,
/// [`HXNET_XFER_CHANGED`], [`HXNET_XFER_AGAIN`], [`HXNET_XFER_REOFFER`], or
/// an errno-like positive code on failure.
///
/// # Safety
/// Worker helper on a blocking-pool thread; `fp.hx` open.
pub unsafe fn folder_send(fp: &HxnetFolderParams, ledger: &mut UploadLedger) -> c_int {
    if fp.hx.is_null() || fp.progress.is_none() {
        return EINVAL;
    }
    let base = match path_from(fp.base_path) {
        Some(p) => p,
        None => return EINVAL,
    };
    let cfg = crate::hfs_config::current();

    let mut entries: Vec<PutEntry> = Vec::new();
    collect_put_entries(&base, &[], &mut entries);

    // Whether the server's last answer already asked for the next entry.
    let mut asked = false;
    // A folder offered, which the server has made once it asks again, and
    // whether an earlier upload offered it and lost the answer.
    let mut offered: Option<(Vec<u8>, bool)> = None;
    // The folders this upload doesn't offer, and whether the last item was
    // a file in one of them.
    let mut skipped: Vec<&[Vec<u8>]> = Vec::new();
    let mut in_skipped = false;
    for e in &entries {
        let rel = e.components.join(&b'/');
        if e.is_folder && ledger.folders.contains(&rel) {
            skipped.push(&e.components);
            continue;
        }
        if !asked {
            let mut cmd = [0u8; 2];
            let n = htxf_read_full(fp.hx, &mut cmd);
            if n == 0 && !aborted(fp.hx) {
                if let Some(again) = ended(ledger, &mut offered, in_skipped) {
                    return again;
                }
            }
            if n != 2 {
                if let Some((rel, _)) = offered {
                    ledger.unsure.insert(rel);
                }
                return EIO;
            }
            if u16::from_be_bytes(cmd) != FILE_NEXT_CMD {
                return EPROTO;
            }
        }
        asked = false;
        if let Some((made, _)) = offered.take() {
            ledger.unsure.remove(&made);
            ledger.folders.insert(made);
        }
        in_skipped = !e.is_folder && skipped.iter().any(|s| e.components.starts_with(s));

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
            let lost = ledger.unsure.contains(&rel);
            offered = Some((rel, lost));
            continue; // folder marker — no payload
        }

        let mut cmd = [0u8; 2];
        let n = htxf_read_full(fp.hx, &mut cmd);
        if n != 2 {
            if n == 0 && !aborted(fp.hx) {
                if let Some(again) = ended(ledger, &mut None, in_skipped) {
                    return again;
                }
            }
            return EIO;
        }
        let cmd = u16::from_be_bytes(cmd);
        let mut data_pos = 0u64;
        let mut rsrc_pos = 0u64;
        if cmd == FILE_NEXT_CMD {
            asked = true;
            continue;
        } else if cmd == FILE_RESUME_CMD {
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
        // Sent from where the server's copy ends, a file that isn't what it
        // was when this upload sent its start would come out neither.
        let now = Offered::of(&md);
        let resumed = data_pos + rsrc_pos != 0;
        if data_pos > data_size
            || rsrc_pos > rsrc_size
            || resumed && ledger.files.get(&rel).is_some_and(|was| *was != now)
        {
            return HXNET_XFER_CHANGED;
        }
        ledger.files.insert(rel, now);
        // What the server holds counts towards the tree.
        if let Some(progress) = fp.progress.filter(|_| resumed) {
            progress(fp.user_data, data_pos + rsrc_pos);
        }

        // Per-file payload size — MUST equal exactly what hxnet_xfer_file_send_one
        // writes (FILP header + comment + data fork, and the 16-byte MACR
        // marker + rsrc fork when any of it is left), or the trailing bytes desync the server's
        // parse of the next file's nfi and the whole folder stream fails.
        let file_size = FILP_HEADER_LEN as u64
            + com
            + data_size.wrapping_sub(data_pos)
            + match rsrc_size.wrapping_sub(rsrc_pos) {
                0 => 0,
                left => ffo::FORK_HEADER_LEN as u64 + left,
            };
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
            // The connection's failing, not a local file's, which a fresh
            // upload would meet too.
            if rv == EPIPE && !aborted(fp.hx) {
                if let Some(again) = ended(ledger, &mut None, in_skipped) {
                    return again;
                }
            }
            return rv;
        }
    }
    0
}

/// The server ended a folder upload: what the ledger learns from where, and
/// [`HXNET_XFER_AGAIN`] or [`HXNET_XFER_REOFFER`] if it learned anything. A folder an earlier upload
/// offered and lost the answer to, ended at again, is one the server has:
/// mhxd ends the upload at a folder that exists. It isn't taken as made the
/// first time the answer is lost, as a server that never read it wouldn't
/// have it, and its files would be sent into a folder that isn't there. A
/// file in a folder not offered, ended at, says the server's folder may be a
/// new one, made since the folders were: each is offered again.
fn ended(
    ledger: &mut UploadLedger,
    offered: &mut Option<(Vec<u8>, bool)>,
    in_skipped: bool,
) -> Option<c_int> {
    if let Some((made, true)) = offered {
        ledger.unsure.remove(made);
        ledger.folders.insert(std::mem::take(made));
        *offered = None;
        Some(HXNET_XFER_AGAIN)
    } else if in_skipped {
        let made: Vec<_> = ledger.folders.drain().collect();
        ledger.unsure.extend(made);
        Some(HXNET_XFER_REOFFER)
    } else {
        None
    }
}

#[cfg(test)]
mod progress_tests {
    use super::*;

    #[test]
    fn progress_posts_once_an_interval_per_transfer() {
        // A thread of its own, so no other test's transfer is its last.
        std::thread::spawn(|| {
            assert!(progress_due(1), "a transfer's first report posts");
            assert!(!progress_due(1), "the next, straight after, doesn't");
            assert!(progress_due(2), "another transfer's first report posts");
            std::thread::sleep(PROGRESS_INTERVAL);
            assert!(progress_due(2), "one interval on, a report posts again");
        })
        .join()
        .unwrap();
    }
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

        // Header is 133 bytes (comlen == 0) + body, and nothing past the size
        // upload_ready declares: no MACR marker for an empty resource fork.
        assert_eq!(
            got.len(),
            133 + body.len(),
            "wire = 133 header + body; got {}",
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

    // A server's FILP body for `body`: what hxnet_xfer_file_send_one emits
    // (133-byte header + data fork), generated by running the send path into
    // a loopback capture, and the empty MACR marker after it.
    fn gen_filp(body: &[u8]) -> Vec<u8> {
        // One directory per call: tests run in parallel, and two bodies of
        // the same length must not share a file.
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let dir = std::env::temp_dir().join(format!(
            "hxnet_folder_gen_{}_{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
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
        let mut got = reader.join().unwrap();
        let _ = std::fs::remove_dir_all(&dir);
        // The empty resource fork's marker a server's FILP carries.
        got.extend_from_slice(&ffo::pack_fork_header(b"MACR", 0, false).unwrap());
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
            // Declared size == actual bytes == 133 header + body: no resource
            // fork, so no MACR marker the FILP does not declare.
            assert_eq!(*size as usize, blob.len(), "declared size matches bytes");
            assert_eq!(
                *size as usize,
                133 + body.len(),
                "folder file size accounting"
            );
            assert_eq!(&blob[133..133 + body.len()], body, "data fork bytes");
        }
    }

    /// How the scripted upload server answers a file it is offered.
    #[derive(Clone, Copy)]
    enum Reply {
        Send,
        Resume(u32),
        Next,
        /// Close the connection rather than answer, as mhxd does at a
        /// folder that exists.
        Close,
    }

    /// What the scripted upload server saw of one item: its path, and for a
    /// file it took, the data fork that came.
    type Took = (String, Option<Vec<u8>>);

    /// A folder-upload server on a loopback port, answering each file with
    /// `reply`, as the 1.9 server does a NEXT: as the ask for the next item.
    fn serve_upload(
        reply: impl Fn(&str) -> Reply + Send + 'static,
    ) -> (u16, std::thread::JoinHandle<Vec<Took>>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut got = Vec::new();
            let mut asked = false;
            loop {
                if !asked && s.write_all(&FILE_NEXT_CMD.to_be_bytes()).is_err() {
                    break;
                }
                asked = false;
                let mut nfi = [0u8; NFI_HEADER_LEN];
                if s.read_exact(&mut nfi).is_err() {
                    break;
                }
                let mut path = Vec::new();
                for _ in 0..u16::from_be_bytes([nfi[4], nfi[5]]) {
                    let mut ph = [0u8; 3];
                    s.read_exact(&mut ph).unwrap();
                    let mut nm = vec![0u8; ph[2] as usize];
                    s.read_exact(&mut nm).unwrap();
                    path.push(String::from_utf8(nm).unwrap());
                }
                let path = path.join("/");
                let answer = reply(&path);
                if matches!(answer, Reply::Close) {
                    break;
                }
                if nfi[3] == 1 {
                    got.push((path, None));
                    continue;
                }
                match answer {
                    Reply::Close => unreachable!(),
                    Reply::Next => {
                        s.write_all(&FILE_NEXT_CMD.to_be_bytes()).unwrap();
                        got.push((path, None));
                        asked = true;
                        continue;
                    }
                    Reply::Send => {
                        s.write_all(&FILE_SEND_CMD.to_be_bytes()).unwrap();
                    }
                    Reply::Resume(data) => {
                        let mut cmd = FILE_RESUME_CMD.to_be_bytes().to_vec();
                        cmd.extend_from_slice(&(rflt::CANONICAL_LEN as u16).to_be_bytes());
                        cmd.extend_from_slice(&rflt::encode(rflt::Resume { data, resource: 0 }));
                        s.write_all(&cmd).unwrap();
                    }
                }
                let mut sz = [0u8; 4];
                if s.read_exact(&mut sz).is_err() {
                    break;
                }
                let mut blob = vec![0u8; u32::from_be_bytes(sz) as usize];
                if s.read_exact(&mut blob).is_err() {
                    break;
                }
                got.push((path, Some(blob[FILP_HEADER_LEN..].to_vec())));
            }
            got
        });
        (port, server)
    }

    /// Upload the tree at `base` to the server on `port`, noting in `ledger`.
    fn send_to(port: u16, base: &Path, ledger: &mut UploadLedger) -> c_int {
        let stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let base_c = std::ffi::CString::new(base.to_str().unwrap()).unwrap();
        let fp = folder_params(&mut conn as *mut HtxfConn, base_c.as_ptr());
        unsafe { folder_send(&fp, ledger) }
    }

    /// A local tree of `a`, `b/c` and `d`, each `len` bytes of its own.
    fn upload_tree(tag: &str, len: usize) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("hxnet_fput_{tag}_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("b")).unwrap();
        for (p, k) in [("a", 1), ("b/c", 2), ("d", 3)] {
            std::fs::write(dir.join(p), body(len, k)).unwrap();
        }
        dir
    }

    #[test]
    fn a_folder_upload_sends_each_file_as_far_as_the_server_answers() {
        let dir = upload_tree("answers", 300);
        let (port, server) = serve_upload(|p| match p {
            "a" => Reply::Next,
            "b/c" => Reply::Resume(100),
            _ => Reply::Send,
        });
        let mut ledger = UploadLedger::default();
        let rv = send_to(port, &dir, &mut ledger);
        let got = server.join().unwrap();
        let items = |l| put_totals(&dir, l).1;
        let counted = (items(None), items(Some(&ledger)));
        std::fs::remove_dir_all(dir.join("b")).unwrap();
        let gone = items(Some(&ledger));
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(rv, 0);
        assert_eq!((counted, gone), ((4, 3), 2), "what is left to offer");
        assert_eq!(
            got,
            [
                ("a".to_string(), None),
                ("b".to_string(), None),
                ("b/c".to_string(), Some(body(300, 2)[100..].to_vec())),
                ("d".to_string(), Some(body(300, 3))),
            ]
        );
    }

    #[test]
    fn a_folder_upload_asked_again_offers_no_folder_the_server_made() {
        let dir = upload_tree("again", 300);
        let mut ledger = UploadLedger::default();
        let (port, server) = serve_upload(|_| Reply::Send);
        assert_eq!(send_to(port, &dir, &mut ledger), 0);
        server.join().unwrap();
        let (port, server) = serve_upload(|p| match p {
            "d" => Reply::Send,
            _ => Reply::Resume(300),
        });
        let rv = send_to(port, &dir, &mut ledger);
        let got = server.join().unwrap();
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(rv, 0);
        let paths: Vec<&str> = got.iter().map(|g| g.0.as_str()).collect();
        assert_eq!(
            paths,
            ["a", "b/c", "d"],
            "mhxd ends at a folder that exists"
        );
        assert_eq!(got[1].1.as_deref(), Some(&[][..]), "nothing past the end");
    }

    #[test]
    fn a_resume_past_the_file_or_of_a_file_changed_since_sends_nothing() {
        // (the server's offset for b/c, whether b/c changed after the first pass)
        for (at, change) in [(301, false), (100, true)] {
            let dir = upload_tree("changed", 300);
            let mut ledger = UploadLedger::default();
            let (port, server) = serve_upload(|_| Reply::Send);
            assert_eq!(send_to(port, &dir, &mut ledger), 0);
            server.join().unwrap();
            assert!(!ledger.changed(&dir));
            if change {
                std::fs::write(dir.join("b/c"), body(310, 9)).unwrap();
                assert!(ledger.changed(&dir));
            }
            let (port, server) = serve_upload(move |p| match p {
                "b/c" => Reply::Resume(at),
                _ => Reply::Next,
            });
            let rv = send_to(port, &dir, &mut ledger);
            let got = server.join().unwrap();
            let _ = std::fs::remove_dir_all(&dir);
            assert_eq!(rv, HXNET_XFER_CHANGED, "at {at}, changed {change}");
            assert!(got.iter().all(|g| g.1.is_none()), "{got:?}");
        }
    }

    #[test]
    fn a_folder_ended_at_twice_is_taken_as_made_and_not_offered_again() {
        let dir = upload_tree("ended", 300);
        let mut ledger = UploadLedger::default();
        let mut runs = Vec::new();
        // The answer to b lost, as a pause between its item and the next
        // FILE_NEXT loses it; then mhxd ending the upload at b, which it has.
        for close in [true, true, false] {
            let (port, server) = serve_upload(move |p| match p {
                "b" if close => Reply::Close,
                _ => Reply::Send,
            });
            let rv = send_to(port, &dir, &mut ledger);
            let got = server.join().unwrap();
            runs.push((rv, got.into_iter().map(|g| g.0).collect::<Vec<_>>()));
        }
        let _ = std::fs::remove_dir_all(&dir);
        let names = |n: &[&str]| n.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(
            runs,
            [
                (EIO, names(&["a"])),
                (HXNET_XFER_AGAIN, names(&["a"])),
                (0, names(&["a", "b/c", "d"])),
            ]
        );
    }

    #[test]
    fn a_file_ended_at_in_a_folder_not_offered_offers_the_folders_again() {
        let dir = upload_tree("fresh", 300);
        let mut ledger = UploadLedger::default();
        let mut runs = Vec::new();
        // The server's folder made afresh: a file sent into b, which it
        // hasn't, ends the upload.
        for close in [false, true, false] {
            let (port, server) = serve_upload(move |p| match p {
                "b/c" if close => Reply::Close,
                _ => Reply::Send,
            });
            let rv = send_to(port, &dir, &mut ledger);
            let got = server.join().unwrap();
            runs.push((rv, got.into_iter().map(|g| g.0).collect::<Vec<_>>()));
        }
        let _ = std::fs::remove_dir_all(&dir);
        let names = |n: &[&str]| n.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(
            runs,
            [
                (0, names(&["a", "b", "b/c", "d"])),
                (HXNET_XFER_REOFFER, names(&["a"])),
                (0, names(&["a", "b", "b/c", "d"])),
            ]
        );
    }

    #[test]
    fn a_file_cut_short_in_a_folder_not_offered_fails_as_it_is() {
        use std::sync::atomic::{AtomicBool, Ordering};
        struct Shrink {
            armed: std::sync::Arc<AtomicBool>,
            path: PathBuf,
        }
        // Armed when the server reads b/c's item, so its first call after is
        // for b/c's FILP header: after b/c was sized, before it is read.
        extern "C" fn shrink(u: *mut c_void, _d: u64) {
            let s = unsafe { &*(u as *const Shrink) };
            if s.armed.swap(false, Ordering::SeqCst) {
                std::fs::File::create(&s.path).unwrap();
            }
        }
        let dir = upload_tree("short", 300);
        let mut ledger = UploadLedger::default();
        let (port, server) = serve_upload(|_| Reply::Send);
        assert_eq!(send_to(port, &dir, &mut ledger), 0);
        server.join().unwrap();
        let armed = std::sync::Arc::new(AtomicBool::new(false));
        let cut = Shrink {
            armed: armed.clone(),
            path: dir.join("b/c"),
        };
        let (port, server) = serve_upload(move |p| {
            if p == "b/c" {
                armed.store(true, Ordering::SeqCst);
            }
            Reply::Send
        });
        let stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let base_c = std::ffi::CString::new(dir.to_str().unwrap()).unwrap();
        let mut fp = folder_params(&mut conn as *mut HtxfConn, base_c.as_ptr());
        fp.progress = Some(shrink);
        fp.user_data = &cut as *const Shrink as *mut c_void;
        let rv = unsafe { folder_send(&fp, &mut ledger) };
        drop(conn);
        server.join().unwrap();
        let (port, server) = serve_upload(|_| Reply::Send);
        assert_eq!(send_to(port, &dir, &mut ledger), 0);
        let again: Vec<_> = server.join().unwrap().into_iter().map(|g| g.0).collect();
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(rv, EIO, "the short read's own reason, not a retry");
        assert_eq!(again, ["a", "b/c", "d"], "b still taken as made");
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

    /// What the scripted folder server does for each FILE_NEXT.
    enum Step {
        /// A folder marker, after a pause.
        Folder(&'static str, std::time::Duration),
        /// A file: its marker, then its FILP body on FILE_SEND.
        File(&'static str, Vec<u8>),
        /// The same with a per-item size of 0, as a Large File server
        /// announces an item whose FFO does not fit in 32 bits.
        FileUnsized(&'static str, Vec<u8>),
        /// Part of an entry header, then nothing.
        PartialHeader,
        /// Reset the connection.
        Reset,
        /// Say nothing for a while, then close — as Janus does after a
        /// tree, closing only on its own timeout.
        Hold(std::time::Duration),
    }

    fn entry_header(name: &str, ftype: u16) -> Vec<u8> {
        let wire_len = NFI_LEN_FIXED + 3 + name.len() as u16;
        let mut v = Vec::new();
        v.extend_from_slice(&wire_len.to_be_bytes());
        v.extend_from_slice(&ftype.to_be_bytes());
        v.extend_from_slice(&1u16.to_be_bytes());
        v.extend_from_slice(&[0, 0, name.len() as u8]);
        v.extend_from_slice(name.as_bytes());
        v
    }

    /// Play a folder download's server side from `steps`, one per
    /// FILE_NEXT, then close on the trailing one.
    fn serve_steps(steps: Vec<Step>) -> (u16, std::thread::JoinHandle<()>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut cmd = [0u8; 2];
            for step in steps {
                if s.read_exact(&mut cmd).is_err() {
                    return;
                }
                assert_eq!(u16::from_be_bytes(cmd), FILE_NEXT_CMD);
                match step {
                    Step::Folder(name, delay) => {
                        std::thread::sleep(delay);
                        s.write_all(&entry_header(name, 1)).unwrap();
                    }
                    Step::File(name, filp) => {
                        s.write_all(&entry_header(name, 0)).unwrap();
                        s.read_exact(&mut cmd).unwrap();
                        assert_eq!(u16::from_be_bytes(cmd), FILE_SEND_CMD);
                        s.write_all(&(filp.len() as u32).to_be_bytes()).unwrap();
                        s.write_all(&filp).unwrap();
                    }
                    Step::FileUnsized(name, filp) => {
                        s.write_all(&entry_header(name, 0)).unwrap();
                        s.read_exact(&mut cmd).unwrap();
                        assert_eq!(u16::from_be_bytes(cmd), FILE_SEND_CMD);
                        s.write_all(&0u32.to_be_bytes()).unwrap();
                        s.write_all(&filp).unwrap();
                    }
                    Step::PartialHeader => {
                        s.write_all(&entry_header("z", 1)[..3]).unwrap();
                        std::thread::sleep(std::time::Duration::from_secs(3));
                        return;
                    }
                    Step::Reset => {
                        // SO_LINGER of zero: the close sends a reset.
                        use std::os::fd::AsRawFd;
                        let linger = libc::linger {
                            l_onoff: 1,
                            l_linger: 0,
                        };
                        unsafe {
                            libc::setsockopt(
                                s.as_raw_fd(),
                                libc::SOL_SOCKET,
                                libc::SO_LINGER,
                                &linger as *const _ as *const c_void,
                                std::mem::size_of::<libc::linger>() as libc::socklen_t,
                            );
                        }
                        return;
                    }
                    Step::Hold(d) => {
                        std::thread::sleep(d);
                        return;
                    }
                }
            }
            let _ = s.read_exact(&mut cmd);
        });
        (port, server)
    }

    /// Download from the server on `port`, told to expect `items`, with
    /// `abort_after` cancelling it from another thread. Returns the result,
    /// the time taken and what arrived, each file with its contents.
    fn recv_folders(
        port: u16,
        items: u32,
        abort_after: Option<std::time::Duration>,
        tag: &str,
    ) -> (c_int, std::time::Duration, Vec<(String, Vec<u8>)>) {
        let dest = std::env::temp_dir().join(format!("hxnet_fcount_{tag}_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dest);
        let stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        unsafe { crate::htxf::hxnet_htxf_set_folder_items(&mut conn, items) };
        let token = crate::htxf::hxnet_htxf_abort_new();
        unsafe { crate::htxf::hxnet_htxf_abort_arm(&mut conn, token) };
        let canceller = abort_after.map(|d| {
            let token = token as usize;
            std::thread::spawn(move || {
                std::thread::sleep(d);
                unsafe { crate::htxf::hxnet_htxf_abort(token as *const _) };
            })
        });
        let base_c = std::ffi::CString::new(dest.to_str().unwrap()).unwrap();
        let fp = folder_params(&mut conn as *mut HtxfConn, base_c.as_ptr());
        let t0 = std::time::Instant::now();
        let rv = unsafe { hxnet_xfer_folder_recv_all(&fp) };
        let took = t0.elapsed();
        if let Some(c) = canceller {
            c.join().unwrap();
        }
        drop(conn);
        unsafe { crate::htxf::hxnet_htxf_abort_free(token) };
        let mut got: Vec<(String, Vec<u8>)> = std::fs::read_dir(&dest)
            .map(|d| {
                d.flatten()
                    .map(|e| {
                        let body = std::fs::read(e.path()).unwrap_or_default();
                        (e.file_name().to_string_lossy().into_owned(), body)
                    })
                    .collect()
            })
            .unwrap_or_default();
        got.sort();
        let _ = std::fs::remove_dir_all(&dest);
        (rv, took, got)
    }

    /// What arrived, less the Finder-info sidecars written beside files.
    fn names(got: &[(String, Vec<u8>)]) -> Vec<&str> {
        got.iter()
            .map(|(n, _)| n.as_str())
            .filter(|n| !n.ends_with(".fndrinfo"))
            .collect()
    }

    const NOW: std::time::Duration = std::time::Duration::ZERO;

    // Capabilities-Large-File: an item announced with a per-item size of 0
    // is read to its end from the FFO's own fork headers, a declared
    // resource fork included, so the next item's header is read in sync.
    #[test]
    fn folder_recv_reads_an_unsized_item_from_its_fork_headers() {
        // A third fork declared, with a resource fork the folder stream
        // discards: fork count 3, and a MACR header with 10 bytes behind it.
        let mut with_rsrc = gen_filp(b"first body");
        with_rsrc[22..24].copy_from_slice(&3u16.to_be_bytes());
        let macr = with_rsrc.len() - ffo::FORK_HEADER_LEN;
        assert_eq!(&with_rsrc[macr..macr + 4], b"MACR");
        with_rsrc[macr + 12..macr + 16].copy_from_slice(&10u32.to_be_bytes());
        with_rsrc.extend_from_slice(b"rsrc-bytes");
        // Two forks declared and the empty trailing MACR period servers send.
        let plain = gen_filp(b"second body");

        let (port, server) = serve_steps(vec![
            Step::FileUnsized("one.txt", with_rsrc),
            Step::FileUnsized("two.txt", plain),
        ]);
        let (rv, _, got) = recv_folders(port, 2, None, "unsized");
        server.join().unwrap();
        assert_eq!(rv, 0);
        let bodies: Vec<_> = got
            .iter()
            .filter(|(n, _)| !n.ends_with(".fndrinfo"))
            .map(|(n, b)| (n.as_str(), b.as_slice()))
            .collect();
        assert_eq!(
            bodies,
            vec![
                ("one.txt", b"first body".as_slice()),
                ("two.txt", b"second body".as_slice())
            ]
        );
    }

    // A server that closes only on its own timeout once the tree is sent:
    // with the announced count, the download ends shortly after the last
    // item instead of waiting it out.
    #[test]
    fn folder_recv_stops_soon_after_the_announced_count() {
        let (port, server) = serve_steps(vec![
            Step::Folder("a", NOW),
            Step::Folder("b", NOW),
            Step::Hold(std::time::Duration::from_secs(4)),
        ]);
        let (rv, took, got) = recv_folders(port, 2, None, "stop");
        server.join().unwrap();
        assert_eq!(rv, 0, "a clean end");
        assert_eq!(names(&got), ["a", "b"]);
        assert!(
            took < FOLDER_END_GRACE + std::time::Duration::from_millis(1500),
            "ended {took:?} in, not after the server's hold"
        );
    }

    // A count lower than what the server sends — mhxd counts the top level
    // only — mustn't cut the tree short, files included: the grace's
    // timeout has to be off again before a file's body is read.
    #[test]
    fn folder_recv_reads_past_an_undercount() {
        let body = b"a file past the count".to_vec();
        let (port, server) = serve_steps(vec![
            Step::Folder("a", NOW),
            Step::Folder("b", NOW),
            Step::File("f", gen_filp(&body)),
            Step::Folder("c", NOW),
        ]);
        let (rv, _, got) = recv_folders(port, 1, None, "under");
        server.join().unwrap();
        assert_eq!(rv, 0, "a clean end");
        assert_eq!(names(&got), ["a", "b", "c", "f"]);
        let file = got.iter().find(|(n, _)| n == "f").map(|(_, b)| b);
        assert_eq!(file, Some(&body), "the file past the count");
    }

    // A slow link that undercounts: every answer takes longer than the
    // least grace, so the grace has to grow with what the link has shown.
    #[test]
    fn folder_recv_waits_longer_on_a_slow_link() {
        let slow = std::time::Duration::from_millis(1300);
        let (port, server) = serve_steps(vec![
            Step::Folder("a", std::time::Duration::from_millis(800)),
            Step::Folder("b", slow),
            Step::Folder("c", slow),
        ]);
        let (rv, _, got) = recv_folders(port, 1, None, "slow");
        server.join().unwrap();
        assert_eq!(rv, 0, "a clean end");
        assert_eq!(names(&got), ["a", "b", "c"]);
    }

    // Past the count, only a close or silence ends the tree: a reset, or
    // an entry header cut short, is the failure it always was.
    #[test]
    fn folder_recv_past_the_count_still_fails_on_a_reset_or_a_cut_header() {
        for (tag, last) in [("reset", Step::Reset), ("cut", Step::PartialHeader)] {
            let (port, server) = serve_steps(vec![Step::Folder("a", NOW), last]);
            let (rv, _, _) = recv_folders(port, 1, None, tag);
            server.join().unwrap();
            assert_ne!(rv, 0, "{tag}: reported as a clean end");
        }
    }

    // A cancel while the grace runs is a cancel, not the end of the tree.
    #[test]
    fn folder_recv_cancelled_in_the_grace_fails() {
        let (port, server) = serve_steps(vec![
            Step::Folder("a", NOW),
            Step::Hold(std::time::Duration::from_secs(3)),
        ]);
        let (rv, _, _) = recv_folders(
            port,
            1,
            Some(std::time::Duration::from_millis(300)),
            "cancel",
        );
        server.join().unwrap();
        assert_ne!(rv, 0, "a cancel reported as a clean end");
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

    #[test]
    fn a_file_is_skipped_or_resumed_only_as_this_download_left_it() {
        let t = std::time::SystemTime::UNIX_EPOCH + std::time::Duration::from_secs(1000);
        let kept = |len, whole| Kept {
            len,
            modified: Some(t),
            whole,
            filp: Some(Filp {
                len: len * 2,
                modified: [0; 4],
            }),
        };
        let later = Kept {
            modified: Some(t + std::time::Duration::from_secs(1)),
            ..kept(10, false)
        };
        let unread = Kept {
            filp: None,
            ..kept(10, false)
        };
        for (left, now, want) in [
            (None, None, Answer::Send),
            // Not this download's: asked for whole.
            (None, Some(kept(10, false)), Answer::Send),
            (Some(kept(10, true)), Some(kept(10, false)), Answer::Skip),
            (
                Some(kept(10, false)),
                Some(kept(10, false)),
                Answer::Resume { data: 10 },
            ),
            // Changed since, or gone.
            (Some(kept(10, true)), Some(kept(11, false)), Answer::Send),
            (Some(kept(10, true)), Some(later), Answer::Send),
            (Some(kept(10, false)), Some(later), Answer::Send),
            (Some(kept(10, true)), None, Answer::Send),
            // Cut off before its FILP described it.
            (Some(unread), Some(kept(10, false)), Answer::Send),
            // Past what an RFLT can say.
            (
                Some(kept(1 << 32, false)),
                Some(kept(1 << 32, false)),
                Answer::Send,
            ),
        ] {
            assert_eq!(answer(left, now), want, "{left:?} {now:?}");
        }
    }

    #[test]
    fn a_resumed_fork_goes_where_the_server_sent_it_from() {
        const DATE: [u8; 4] = [1, 2, 3, 4];
        let was = Some(Filp {
            len: 9,
            modified: DATE,
        });
        // (as first described, asked from, fork length sent, its date, written from)
        for (was, from, sent, date, want) in [
            (None, 0, 9, DATE, Ok(0)),
            (None, 4, 5, DATE, Ok(4)),
            (was, 4, 5, DATE, Ok(4)),
            (was, 0, 9, DATE, Ok(0)),
            // Sent whole, the resume ignored.
            (was, 4, 9, DATE, Ok(0)),
            // Changed on the server since.
            (was, 4, 7, DATE, Err(EIO)),
            (was, 4, 12, DATE, Err(EIO)),
            (was, 4, 5, [1, 2, 3, 5], Err(EIO)),
        ] {
            assert_eq!(
                data_from(was, from, sent, date),
                want,
                "{was:?} {from} {sent}"
            );
        }
    }

    /// How the scripted server answers a resume.
    #[derive(Clone, Copy)]
    enum Resumes {
        /// From the RFLT's offset, as mhxd and hxd-ng do.
        Honored,
        /// With the whole file, as Janus does in a folder.
        Ignored,
    }

    /// Play a folder download's server for `files`, each modified at
    /// `date`, answering a resume as `resumes` says and cutting off file
    /// `cut` halfway, closing the connection. The answers, in order.
    fn serve_answers(
        files: Vec<(&'static str, Vec<u8>)>,
        date: [u8; 4],
        resumes: Resumes,
        cut: Option<usize>,
    ) -> (u16, std::thread::JoinHandle<Vec<Answer>>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut answers = Vec::new();
            let mut cmd = [0u8; 2];
            s.read_exact(&mut cmd).unwrap();
            for (i, (name, body)) in files.iter().enumerate() {
                assert_eq!(u16::from_be_bytes(cmd), FILE_NEXT_CMD);
                s.write_all(&entry_header(name, 0)).unwrap();
                s.read_exact(&mut cmd).unwrap();
                let from = match u16::from_be_bytes(cmd) {
                    FILE_NEXT_CMD => {
                        answers.push(Answer::Skip);
                        continue;
                    }
                    FILE_SEND_CMD => {
                        answers.push(Answer::Send);
                        0
                    }
                    FILE_RESUME_CMD => {
                        let mut len = [0u8; 2];
                        s.read_exact(&mut len).unwrap();
                        let mut r = vec![0u8; u16::from_be_bytes(len).into()];
                        s.read_exact(&mut r).unwrap();
                        let at = rflt::parse_compatible(&r);
                        assert_eq!(at.resource, 0);
                        answers.push(Answer::Resume { data: at.data });
                        at.data as usize
                    }
                    other => panic!("action {other}"),
                };
                let from = from.min(body.len());
                let mut filp = with_rsrc(gen_filp(&body[from..]), date);
                // The size of what is left, however much comes.
                s.write_all(&(filp.len() as u32).to_be_bytes()).unwrap();
                if let Resumes::Ignored = resumes {
                    filp = with_rsrc(gen_filp(body), date);
                }
                if cut == Some(i) {
                    s.write_all(&filp[..filp.len() - body[from..].len() / 2])
                        .unwrap();
                    return answers;
                }
                s.write_all(&filp).unwrap();
                if s.read_exact(&mut cmd).is_err() {
                    break;
                }
            }
            answers
        });
        (port, server)
    }

    /// `filp` modified at `date`, with a resource fork, which a folder's
    /// files read past.
    fn with_rsrc(mut filp: Vec<u8>, date: [u8; 4]) -> Vec<u8> {
        filp[104..108].copy_from_slice(&date);
        filp[22..24].copy_from_slice(&3u16.to_be_bytes());
        let macr = filp.len() - ffo::FORK_HEADER_LEN;
        assert_eq!(&filp[macr..macr + 4], b"MACR");
        filp[macr + 12..macr + 16].copy_from_slice(&(RSRC.len() as u32).to_be_bytes());
        filp.extend_from_slice(RSRC);
        filp
    }

    const RSRC: &[u8; 10] = b"rsrc-bytes";

    fn recv_into(port: u16, dest: &Path, ledger: &mut FolderLedger) -> c_int {
        recv_counted(port, dest, ledger).0
    }

    /// [`recv_into`], and the bytes its progress reported.
    fn recv_counted(port: u16, dest: &Path, ledger: &mut FolderLedger) -> (c_int, u64) {
        unsafe extern "C" fn count(user: *mut c_void, delta: u64) {
            *(user as *mut u64) += delta;
        }
        let stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
        let mut conn = HtxfConn::new_plain_for_test(stream);
        let base_c = std::ffi::CString::new(dest.to_str().unwrap()).unwrap();
        let mut reported = 0u64;
        let mut fp = folder_params(&mut conn as *mut HtxfConn, base_c.as_ptr());
        fp.user_data = (&mut reported as *mut u64).cast();
        fp.progress = Some(count);
        let rv = unsafe { folder_recv(&fp, ledger) };
        (rv, reported)
    }

    fn body(n: usize, k: u8) -> Vec<u8> {
        (0..n).map(|i| (i as u8).wrapping_mul(k)).collect()
    }

    /// A fresh local folder for a test.
    fn dest(tag: &str) -> PathBuf {
        let dest = std::env::temp_dir().join(format!("hxnet_fresume_{tag}_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dest);
        std::fs::create_dir_all(&dest).unwrap();
        dest
    }

    const DATE: [u8; 4] = [0xb0, 1, 2, 3];

    /// A folder download stopped partway, asked for again, skips what
    /// arrived, resumes what was cut off, and asks whole for the rest and
    /// for what was on disk before it, which it can't vouch for — whether
    /// the server resumes from the offset asked or sends the file whole.
    #[test]
    fn a_folder_download_asked_again_asks_only_for_what_is_missing() {
        let files = vec![
            ("a", body(3000, 3)),
            ("b", body(5000, 7)),
            ("c", body(700, 11)),
        ];
        for (resumes, tag) in [(Resumes::Honored, "honored"), (Resumes::Ignored, "ignored")] {
            let dest = dest(tag);
            std::fs::write(dest.join("c"), vec![b'x'; 9000]).unwrap();
            let mut ledger = FolderLedger::default();

            let (port, server) = serve_answers(files.clone(), DATE, resumes, Some(1));
            assert_ne!(recv_into(port, &dest, &mut ledger), 0, "cut off");
            assert_eq!(server.join().unwrap(), [Answer::Send, Answer::Send]);
            let had = std::fs::metadata(dest.join("b")).unwrap().len();
            assert!(0 < had && had < 5000, "b kept {had}");
            assert_eq!(ledger.held(&dest), 3000 + had);

            // The whole tree's size, as a server counts it: each file's FILP.
            let size: u64 = files
                .iter()
                .map(|f| with_rsrc(gen_filp(&f.1), DATE).len() as u64)
                .sum();
            let held = ledger.held(&dest);
            let (port, server) = serve_answers(files.clone(), DATE, resumes, None);
            let (rv, reported) = recv_counted(port, &dest, &mut ledger);
            assert_eq!(rv, 0, "{tag}");
            assert!(
                held + reported <= size,
                "{tag}: {held} held and {reported} more of {size}"
            );
            assert_eq!(
                server.join().unwrap(),
                [
                    Answer::Skip,
                    Answer::Resume { data: had as u32 },
                    Answer::Send
                ]
            );
            for (name, want) in &files {
                assert!(
                    std::fs::read(dest.join(name)).unwrap() == *want,
                    "{tag}: {name}"
                );
            }
            let _ = std::fs::remove_dir_all(&dest);
        }
    }

    /// A file that changed on the server while its download was paused,
    /// its length or its date, can't be resumed: the resume fails, and the
    /// next asks for it whole.
    #[test]
    fn a_file_changed_on_the_server_is_asked_for_whole() {
        for (changed, date, tag) in [
            (body(6000, 5), DATE, "longer"),
            (body(5000, 5), [0xb0, 1, 2, 4], "redated"),
        ] {
            let dest = dest(tag);
            let mut ledger = FolderLedger::default();
            let (port, server) =
                serve_answers(vec![("b", body(5000, 7))], DATE, Resumes::Honored, Some(0));
            assert_ne!(recv_into(port, &dest, &mut ledger), 0);
            server.join().unwrap();

            let changed = vec![("b", changed)];
            let (port, server) = serve_answers(changed.clone(), date, Resumes::Honored, None);
            assert_eq!(recv_into(port, &dest, &mut ledger), EIO, "{tag}");
            assert!(matches!(
                server.join().unwrap()[..],
                [Answer::Resume { .. }]
            ));

            let (port, server) = serve_answers(changed.clone(), date, Resumes::Honored, None);
            assert_eq!(recv_into(port, &dest, &mut ledger), 0, "{tag}");
            assert_eq!(server.join().unwrap(), [Answer::Send]);
            assert!(
                std::fs::read(dest.join("b")).unwrap() == changed[0].1,
                "{tag}"
            );
            let _ = std::fs::remove_dir_all(&dest);
        }
    }
}

#[cfg(test)]
mod recv_timeout_tests {
    use super::*;
    use crate::htxf::HtxfConn;
    use std::net::{TcpListener, TcpStream};
    use std::time::{Duration, Instant};

    // Generate a valid FILP header + data fork (no trailing MACR) by running the
    // send path into a loopback capture.
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
        full
    }

    // A server that over-declares file_budget (a phantom trailing MACR) but never
    // sends it must not hang the download worker until the socket closes — the
    // ~2s end-of-transfer stall. The worker should finish promptly (bounded by
    // MACR_DRAIN_TIMEOUT_MS) and return success.
    /// How long the phantom-MACR server holds the socket open without
    /// sending, when the receive under test doesn't return first.
    const SERVER_HOLD: Duration = Duration::from_secs(6);

    #[test]
    fn phantom_macr_does_not_hang_completion() {
        let body = b"downloaded file body, no resource fork\n";
        let wire = filp_header_and_data(body);
        // Declare 16 phantom bytes beyond the header+data the server sends.
        let file_budget = wire.len() as u64 + ffo::FORK_HEADER_LEN as u64;

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        // Server: send header+data, then hold the socket open far past the drain
        // timeout without sending the (nonexistent) MACR. It closes early once
        // the receive has returned, so a passing run doesn't wait out the hold.
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            s.write_all(&wire).unwrap();
            s.flush().unwrap();
            let _ = done_rx.recv_timeout(SERVER_HOLD);
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
        let _ = done_tx.send(());
        drop(conn);

        let got = std::fs::read(&out_path).unwrap_or_default();
        let _ = std::fs::remove_dir_all(&out_dir);
        let _ = server.join();

        assert_eq!(
            rv, 0,
            "recv should complete cleanly despite the phantom MACR"
        );
        assert_eq!(got, body, "data fork must be written correctly");
        // The elapsed time also covers the fsyncs of the file and its
        // sidecar, which can take most of a second on a CI runner, so the
        // bound sits between that and the server's hold rather than near
        // the drain timeout itself.
        assert!(
            elapsed < SERVER_HOLD / 2,
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

    // A preview streams only as much of the data fork as its viewer asked
    // for, and says when that was less than the whole fork. The server sends
    // no more than that and holds, so a worker that read on would stall. An
    // empty file still reaches the viewer, which is waiting to be told what
    // it is.
    #[test]
    fn a_preview_streams_only_what_it_asked_for() {
        struct Got {
            wanted: u64,
            size: u64,
            bytes: Vec<u8>,
            done: bool,
        }
        unsafe extern "C" fn set_info(
            p: *mut c_void,
            _ty: *const c_char,
            _cr: *const c_char,
            size: u64,
        ) -> u64 {
            let got = &mut *(p as *mut Got);
            got.size = size;
            got.wanted
        }
        unsafe extern "C" fn chunk(p: *mut c_void, buf: *const c_char, len: usize) {
            let got = &mut *(p as *mut Got);
            got.bytes
                .extend_from_slice(std::slice::from_raw_parts(buf.cast(), len));
        }
        unsafe extern "C" fn done(p: *mut c_void) {
            (*(p as *mut Got)).done = true;
        }
        extern "C" fn noop(_u: *mut c_void, _d: u64) {}

        let long: Vec<u8> = (0..200_000u32).map(|i| i as u8).collect();
        for (case, body, wanted, rv_want) in [
            ("the start", &long[..], 1000, HXNET_XFER_STOPPED),
            ("none", &long[..], 0, HXNET_XFER_STOPPED),
            ("all", &long[..], long.len() as u64, 0),
            ("an empty file", &[][..], 0, 0),
        ] {
            let mut wire = filp_header_and_data(body);
            let file_budget = wire.len() as u64;
            wire.truncate(wire.len() - body.len() + wanted as usize);
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let addr = listener.local_addr().unwrap();
            let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
            let server = std::thread::spawn(move || {
                let (mut s, _) = listener.accept().unwrap();
                let _ = s.write_all(&wire);
                let _ = done_rx.recv_timeout(SERVER_HOLD);
            });

            let mut got = Got {
                wanted,
                size: u64::MAX,
                bytes: Vec::new(),
                done: false,
            };
            let mut conn = HtxfConn::new_plain_for_test(TcpStream::connect(addr).unwrap());
            let path = std::ffi::CString::new("unused").unwrap();
            let params = HxnetXferParams {
                hx: &mut conn as *mut HtxfConn,
                path: path.as_ptr(),
                file_budget,
                data_pos: 0,
                rsrc_pos: 0,
                opt_preview: 1,
                opt_folder: 0,
                opt_large: 0,
                preview: &mut got as *mut Got as *mut c_void,
                user_data: std::ptr::null_mut(),
                progress: Some(noop),
                preview_chunk: Some(chunk),
                preview_set_info: Some(set_info),
                preview_done: Some(done),
                data_size: 0,
                rsrc_size: 0,
            };
            let rv = unsafe { hxnet_xfer_file_recv_one(&params) };
            let _ = done_tx.send(());
            drop(conn);
            let _ = server.join();

            assert_eq!(rv, rv_want, "{case}");
            assert_eq!(
                got.size,
                body.len() as u64,
                "{case}: the data fork's length"
            );
            assert_eq!(got.bytes, body[..wanted as usize], "{case}");
            assert!(got.done, "{case}: the viewer is told the stream ended");
        }
    }
}
