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
    let mut hdr = vec![0u8; 133 + comlen];
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
    hdr[117 + comlen..133 + comlen].copy_from_slice(&data_hdr);
    if xfer_write(hx, &hdr).is_err() {
        return EIO;
    }
    p.report((133 + comlen) as u64);

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
        assert_eq!(&got[133..133 + body.len()], body, "raw data fork follows header");
        // Trailing 16 bytes are the MACR marker (rsrc_size == 0).
        assert_eq!(&got[133 + body.len()..133 + body.len() + 4], b"MACR", "MACR marker");
    }
}
