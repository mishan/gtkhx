//! HTXF file transfers: a large file each way, and a folder of many small
//! files, over plain TCP, TLS and HOPE's AEAD.
//!
//! The transfer connects through `hxnet_htxf_connect` to a fake server on
//! 127.0.0.1 and copies through the production workers,
//! `hxnet_xfer_file_recv_one` / `_send_one` / `hxnet_xfer_folder_recv_all`,
//! on a thread of their own as the app's blocking pool runs them. Their
//! progress callback does what the app's does — posts to the main loop
//! with `gtkhx_bridge_post_to_main` when `progress_due` says to — and the
//! main loop counts what arrives. In the app each of those posts emits
//! `file-update`, and the Tasks panel redraws the transfer's row.
//!
//! The AEAD transfer derives its keys from a HOPE-AEAD login, made first
//! against the pipeline's fake server; the fake HTXF server derives the
//! same keys from what that login agreed.
//!
//! Files go to a tmpfs where there is one, so the disk isn't what's timed.
//!
//! Known values: what arrives is byte for byte what was sent, and a single
//! file can't be faster than the raw loopback socket, which is measured
//! alongside.

use std::ffi::{c_void, CString};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use hxcrypto::aead::AeadState;
use hxnet::ffi::{hxnet_connection_hope_aead_material, hxnet_hope_aead_free, HxnetHopeAead};
use hxnet::htxf::{
    hxnet_htxf_close, hxnet_htxf_connect, hxnet_htxf_finish_send, hxnet_htxf_pack_preamble,
    HtxfChannel,
};
use hxnet::xfer::{
    hxnet_xfer_file_recv_one, hxnet_xfer_file_send_one, hxnet_xfer_folder_recv_all,
    HxnetFolderParams, HxnetXferParams,
};
use tokio_rustls::rustls;

use super::{hope_keys_for_bench, Transport};

/// The large file, each way.
pub const FILE_MB: usize = 256;
/// The folder: this many files of `SMALL_FILE` bytes.
pub const FOLDER_FILES: usize = 1_000;
const SMALL_FILE: usize = 4 * 1024;
/// The worker's copy chunk (`xfer.rs`); a progress post goes with each.
const CHUNK: usize = 0xf000;
/// The folder mini-protocol (`xfer.rs`).
const FILE_SEND_CMD: u16 = 1;
const FILE_NEXT_CMD: u16 = 3;
const NFI_HEADER_LEN: usize = 6;
const NFI_LEN_FIXED: u16 = 4;
const XFER_REF: u32 = 7;

/// Where the files go: a tmpfs if there is one.
fn scratch_root() -> PathBuf {
    let shm = Path::new("/dev/shm");
    let base = if shm.is_dir() {
        shm.to_path_buf()
    } else {
        std::env::temp_dir()
    };
    base.join(format!("hxnet-loopback-{}", std::process::id()))
}

/// Pseudo-random bytes, so nothing compresses or dedupes along the way.
fn content(len: usize, seed: u64) -> Vec<u8> {
    let mut x = seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1;
    (0..len)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            x as u8
        })
        .collect()
}

// ---- progress, as the app posts it -------------------------------------

static POSTED: AtomicU64 = AtomicU64::new(0);
static DISPATCHED: AtomicU64 = AtomicU64::new(0);

unsafe extern "C" fn on_progress_idle(_data: glib::ffi::gpointer) -> glib::ffi::gboolean {
    DISPATCHED.fetch_add(1, Ordering::Relaxed);
    glib::ffi::G_SOURCE_REMOVE
}

/// A progress key of its own for each transfer, as the app's are keyed by
/// their handles.
fn next_key() -> *mut c_void {
    static NEXT: AtomicU64 = AtomicU64::new(1);
    NEXT.fetch_add(1, Ordering::Relaxed) as usize as *mut c_void
}

unsafe extern "C" fn progress(user_data: *mut c_void, _delta: u64) {
    if hxnet::xfer::progress_due(user_data as usize) {
        POSTED.fetch_add(1, Ordering::Relaxed);
        hxbridge::blocking::gtkhx_bridge_post_to_main(Some(on_progress_idle), std::ptr::null_mut());
    }
}

// ---- the fake server's side of a transfer ------------------------------

/// The server end of an HTXF connection, whichever transport it runs over.
enum ServerIo {
    Plain(TcpStream),
    Tls(Box<rustls::StreamOwned<rustls::ServerConnection, TcpStream>>),
    Aead(HtxfChannel<TcpStream>),
}

impl ServerIo {
    /// Close as a TLS server does, with `close_notify` first: the folder
    /// protocol has no end marker, so its end is the server closing, and a
    /// TLS client takes a close without one for a truncation.
    fn close(self) {
        if let ServerIo::Tls(mut s) = self {
            s.conn.send_close_notify();
            let _ = s.flush();
        }
    }
}

impl Read for ServerIo {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self {
            ServerIo::Plain(s) => s.read(buf),
            ServerIo::Tls(s) => s.read(buf),
            ServerIo::Aead(s) => s.read(buf),
        }
    }
}

impl Write for ServerIo {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        match self {
            ServerIo::Plain(s) => s.write(buf),
            ServerIo::Tls(s) => s.write(buf),
            ServerIo::Aead(s) => s.write(buf),
        }
    }

    fn flush(&mut self) -> std::io::Result<()> {
        match self {
            ServerIo::Plain(s) => s.flush(),
            ServerIo::Tls(s) => s.flush(),
            ServerIo::Aead(_) => Ok(()),
        }
    }
}

/// The transfer keys an AEAD transfer runs under, from the server's side:
/// (outgoing, incoming).
fn server_transfer_keys() -> (AeadState, AeadState) {
    let (to_server, to_client) = hope_keys_for_bench().transfer(XFER_REF);
    (to_client, to_server)
}

/// Accept one transfer connection and read its preamble.
fn accept(listener: &TcpListener, transport: Transport) -> ServerIo {
    let (mut sock, _) = listener.accept().expect("accept");
    sock.set_nodelay(true).ok();
    let mut io = match transport {
        Transport::Tls => {
            let conn =
                rustls::ServerConnection::new(super::tls_server_config()).expect("TLS server");
            ServerIo::Tls(Box::new(rustls::StreamOwned::new(conn, sock)))
        }
        Transport::HopeAead => {
            // The preamble goes in the clear, ahead of the AEAD framing.
            let mut pre = [0u8; 16];
            sock.read_exact(&mut pre).expect("preamble");
            let (out, inc) = server_transfer_keys();
            return ServerIo::Aead(HtxfChannel::new_aead(sock, out, inc));
        }
        _ => ServerIo::Plain(sock),
    };
    let mut pre = [0u8; 16];
    io.read_exact(&mut pre).expect("preamble");
    io
}

/// Connect the client end: the preamble, and TLS or AEAD as asked.
fn connect(port: u16, transport: Transport, hope: *const HxnetHopeAead, size: u64) -> *mut c_void {
    let mut pre = [0u8; 24];
    let n =
        unsafe { hxnet_htxf_pack_preamble(pre.as_mut_ptr(), pre.len(), XFER_REF, size, 0, 0, 0) };
    assert_eq!(n, 16, "preamble");
    let host = b"127.0.0.1";
    let h = unsafe {
        hxnet_htxf_connect(
            host.as_ptr(),
            host.len(),
            port,
            std::ptr::null(),
            0,
            i32::from(transport == Transport::Tls),
            pre.as_ptr(),
            n,
            if transport == Transport::HopeAead {
                hope
            } else {
                std::ptr::null()
            },
            XFER_REF,
            Some(super::trust_any),
            std::ptr::null_mut(),
        )
    };
    assert!(!h.is_null(), "{}: HTXF connect", transport.name());
    h as *mut c_void
}

/// The FILP stream the app sends for `data`: the fork header, the data,
/// the empty resource fork's marker. Captured from the send worker
/// itself, so the download is fed exactly what an upload produces.
fn filp_for(data: &[u8], dir: &Path, folder: bool) -> Vec<u8> {
    let path = dir.join("filp-source");
    std::fs::write(&path, data).expect("write source");
    let filp = capture_filp(&path, data.len() as u64, folder);
    let _ = std::fs::remove_file(&path);
    filp
}

/// The FILP stream the send worker writes for the file at `path`.
fn capture_filp(path: &Path, size: u64, folder: bool) -> Vec<u8> {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("port").port();
    let reader = std::thread::spawn(move || {
        let (mut s, _) = listener.accept().expect("accept");
        let mut pre = [0u8; 16];
        s.read_exact(&mut pre).expect("preamble");
        let mut got = Vec::new();
        s.read_to_end(&mut got).expect("capture");
        got
    });
    let h = connect(port, Transport::Plain, std::ptr::null(), size);
    let path_c = CString::new(path.to_str().expect("path")).expect("path");
    let p = send_params(h, &path_c, size, folder);
    assert_eq!(unsafe { hxnet_xfer_file_send_one(&p) }, 0, "capture send");
    unsafe { hxnet_htxf_close(h as _) };
    reader.join().expect("capture thread")
}

fn send_params(h: *mut c_void, path: &CString, size: u64, folder: bool) -> HxnetXferParams {
    HxnetXferParams {
        hx: h as _,
        path: path.as_ptr(),
        file_budget: 0,
        data_pos: 0,
        rsrc_pos: 0,
        opt_preview: 0,
        opt_folder: i32::from(folder),
        opt_large: 0,
        preview: std::ptr::null_mut(),
        user_data: next_key(),
        progress: Some(progress),
        preview_chunk: None,
        preview_set_info: None,
        preview_done: None,
        data_size: size,
        rsrc_size: 0,
    }
}

// ---- running a worker the way the app does ------------------------------

/// Run `work` on a thread of its own while the main loop takes the progress
/// posts, as the app's does. Returns its result, the wall time, the
/// posts that reached the main loop, and the main thread's CPU for them.
fn run_worker(work: impl FnOnce() -> i32 + Send + 'static) -> (i32, Duration, u64, Duration) {
    POSTED.store(0, Ordering::Relaxed);
    DISPATCHED.store(0, Ordering::Relaxed);
    let done = Arc::new(AtomicBool::new(false));
    let ctx = glib::MainContext::default();
    let main = super::this_tid();
    let cpu0 = super::thread_cpu();
    let t0 = Instant::now();
    let worker = {
        let done = done.clone();
        let ctx = ctx.clone();
        std::thread::spawn(move || {
            let rv = work();
            done.store(true, Ordering::Release);
            ctx.wakeup();
            rv
        })
    };
    // Every post the worker made has been queued by the time it returns;
    // run the loop until they've all been dispatched too.
    while !done.load(Ordering::Acquire)
        || DISPATCHED.load(Ordering::Relaxed) < POSTED.load(Ordering::Relaxed)
    {
        ctx.iteration(true);
    }
    let wall = t0.elapsed();
    let cpu1 = super::thread_cpu();
    let rv = worker.join().expect("worker");
    let (m, _, _) = super::cpu_split(&cpu0, &cpu1, main);
    (
        rv,
        wall,
        DISPATCHED.load(Ordering::Relaxed),
        Duration::from_nanos(m),
    )
}

pub struct Transfer {
    /// MB/s, and the progress posts a second the main loop took.
    pub rate: f64,
    pub posts: f64,
    /// Main-thread CPU, ms a second of transfer.
    pub main_load: f64,
    pub failures: Vec<String>,
}

fn transfer(bytes: usize, rv: i32, wall: Duration, posts: u64, main: Duration) -> Transfer {
    let secs = wall.as_secs_f64();
    let mut failures = Vec::new();
    if rv != 0 {
        failures.push(format!("the worker returned {rv}"));
    }
    Transfer {
        rate: bytes as f64 / 1e6 / secs,
        posts: posts as f64 / secs,
        main_load: main.as_secs_f64() * 1e3 / secs,
        failures,
    }
}

/// Download `data` (as its FILP stream `filp`) over `transport`.
pub fn download(
    transport: Transport,
    hope: *const HxnetHopeAead,
    data: &Arc<Vec<u8>>,
    filp: &Arc<Vec<u8>>,
    dir: &Path,
) -> Transfer {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("port").port();
    let server = {
        let filp = filp.clone();
        std::thread::spawn(move || {
            let mut io = accept(&listener, transport);
            for chunk in filp.chunks(64 * 1024) {
                io.write_all(chunk).expect("send");
            }
            io.flush().expect("flush");
            // Hold on until the client hangs up.
            let mut sink = [0u8; 64];
            while matches!(io.read(&mut sink), Ok(n) if n > 0) {}
        })
    };
    let h = connect(port, transport, hope, 0) as usize;
    let dest = dir.join("download");
    let dest_c = CString::new(dest.to_str().expect("path")).expect("path");
    let budget = filp.len() as u64;
    let (rv, wall, posts, main) = run_worker(move || {
        let p = HxnetXferParams {
            file_budget: budget,
            data_size: 0,
            ..send_params(h as _, &dest_c, 0, false)
        };
        unsafe { hxnet_xfer_file_recv_one(&p) }
    });
    unsafe { hxnet_htxf_close(h as _) };
    let _ = server.join();
    let mut t = transfer(data.len(), rv, wall, posts, main);
    let got = std::fs::read(&dest).unwrap_or_default();
    if got.as_slice() != data.as_slice() {
        t.failures.push(format!(
            "downloaded {} bytes, not the {} sent",
            got.len(),
            data.len()
        ));
    }
    let _ = std::fs::remove_file(&dest);
    t
}

/// Upload `source` (holding `data`) over `transport`.
pub fn upload(
    transport: Transport,
    hope: *const HxnetHopeAead,
    data: &Arc<Vec<u8>>,
    filp: &Arc<Vec<u8>>,
    source: &Path,
) -> Transfer {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("port").port();
    let want = filp.len();
    let server = std::thread::spawn(move || {
        let mut io = accept(&listener, transport);
        let mut got = Vec::with_capacity(want);
        let mut buf = vec![0u8; 64 * 1024];
        // To the end, not just `want` bytes: stopping early would leave
        // the client's close unread, and dropping the socket then resets
        // the connection under its finish.
        loop {
            match io.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => got.extend_from_slice(&buf[..n]),
            }
        }
        got
    });
    let h = connect(port, transport, hope, filp.len() as u64) as usize;
    let src_c = CString::new(source.to_str().expect("path")).expect("path");
    let size = data.len() as u64;
    let (rv, wall, posts, main) = run_worker(move || {
        let p = send_params(h as _, &src_c, size, false);
        // As the upload worker does: finish before closing.
        match unsafe { hxnet_xfer_file_send_one(&p) } {
            0 => unsafe { hxnet_htxf_finish_send(h as _) },
            rv => rv,
        }
    });
    unsafe { hxnet_htxf_close(h as _) };
    let got = server.join().expect("upload server");
    let mut t = transfer(data.len(), rv, wall, posts, main);
    if got.as_slice() != filp.as_slice() {
        t.failures.push(format!(
            "the server got {} bytes, not the {} the send worker writes, or not the same",
            got.len(),
            filp.len()
        ));
    }
    t
}

/// One file of the folder: its name, the FILP stream the server sends
/// for it, and its contents.
pub type FolderFile = (String, Vec<u8>, Vec<u8>);

pub struct Folder {
    /// Wall time per file, µs; and the progress posts a second.
    pub per_file: f64,
    pub posts: f64,
    pub failures: Vec<String>,
}

/// Download a folder of `files` (name, FILP stream, contents) over
/// `transport`, the server playing mhxd's side of the folder protocol.
pub fn folder(
    transport: Transport,
    hope: *const HxnetHopeAead,
    files: &Arc<Vec<FolderFile>>,
    dir: &Path,
) -> Folder {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("port").port();
    let server = {
        let files = files.clone();
        std::thread::spawn(move || {
            let mut io = accept(&listener, transport);
            let mut cmd = [0u8; 2];
            for (name, filp, _) in files.iter() {
                io.read_exact(&mut cmd).expect("FILE_NEXT");
                assert_eq!(u16::from_be_bytes(cmd), FILE_NEXT_CMD);
                let mut nfi = Vec::with_capacity(NFI_HEADER_LEN + 3 + name.len());
                nfi.extend_from_slice(&(NFI_LEN_FIXED + 3 + name.len() as u16).to_be_bytes());
                nfi.extend_from_slice(&0u16.to_be_bytes());
                nfi.extend_from_slice(&1u16.to_be_bytes());
                nfi.extend_from_slice(&[0, 0, name.len() as u8]);
                nfi.extend_from_slice(name.as_bytes());
                io.write_all(&nfi).expect("nfi");
                io.flush().expect("flush");
                io.read_exact(&mut cmd).expect("FILE_SEND");
                assert_eq!(u16::from_be_bytes(cmd), FILE_SEND_CMD);
                io.write_all(&(filp.len() as u32).to_be_bytes())
                    .expect("size");
                io.write_all(filp).expect("file");
                io.flush().expect("flush");
            }
            // The trailing FILE_NEXT; closing then is the end of the tree.
            let _ = io.read_exact(&mut cmd);
            io.close();
        })
    };
    let h = connect(port, transport, hope, 0) as usize;
    let dest = dir.join("folder");
    let _ = std::fs::remove_dir_all(&dest);
    let dest_c = CString::new(dest.to_str().expect("path")).expect("path");
    let (rv, wall, posts, _) = run_worker(move || {
        let fp = HxnetFolderParams {
            hx: h as _,
            base_path: dest_c.as_ptr(),
            opt_preview: 0,
            opt_folder: 1,
            opt_large: 0,
            user_data: next_key(),
            progress: Some(progress),
        };
        unsafe { hxnet_xfer_folder_recv_all(&fp) }
    });
    unsafe { hxnet_htxf_close(h as _) };
    let _ = server.join();
    let mut failures = Vec::new();
    if rv != 0 {
        failures.push(format!("the folder worker returned {rv}"));
    }
    let wrong = files
        .iter()
        .filter(|(name, _, body)| std::fs::read(dest.join(name)).ok().as_ref() != Some(body))
        .count();
    if wrong > 0 {
        failures.push(format!(
            "{wrong} of {} files missing or damaged",
            files.len()
        ));
    }
    let _ = std::fs::remove_dir_all(&dest);
    Folder {
        per_file: wall.as_secs_f64() * 1e6 / files.len() as f64,
        posts: posts as f64 / wall.as_secs_f64(),
        failures,
    }
}

/// The raw loopback floor for a single file: `bytes` down a plain socket
/// into memory. MB/s.
pub fn raw_floor(bytes: &Arc<Vec<u8>>) -> f64 {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("port").port();
    let server = {
        let bytes = bytes.clone();
        std::thread::spawn(move || {
            let (mut s, _) = listener.accept().expect("accept");
            for chunk in bytes.chunks(64 * 1024) {
                s.write_all(chunk).expect("send");
            }
        })
    };
    let mut s = TcpStream::connect(("127.0.0.1", port)).expect("connect");
    let t0 = Instant::now();
    let mut buf = vec![0u8; CHUNK];
    let mut n = 0usize;
    loop {
        match s.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(k) => n += k,
        }
    }
    let secs = t0.elapsed().as_secs_f64();
    let _ = server.join();
    n as f64 / 1e6 / secs
}

/// Set up what every transfer needs: the scratch directory, the file and
/// its FILP stream, the folder's files. Returns them with a cleanup guard.
pub struct Fixture {
    pub dir: PathBuf,
    pub data: Arc<Vec<u8>>,
    pub filp: Arc<Vec<u8>>,
    pub source: PathBuf,
    pub files: Arc<Vec<FolderFile>>,
}

impl Fixture {
    pub fn new() -> Fixture {
        let dir = scratch_root();
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("scratch dir");
        let data = content(FILE_MB * 1024 * 1024, 1);
        let source = dir.join("upload-source");
        std::fs::write(&source, &data).expect("upload source");
        // From the upload's own source, so the fork header's dates match
        // and an upload can be held to it byte for byte.
        let filp = capture_filp(&source, data.len() as u64, false);
        let files = (0..FOLDER_FILES)
            .map(|i| {
                let body = content(SMALL_FILE, i as u64 + 2);
                let filp = filp_for(&body, &dir, true);
                (format!("file-{i:05}.bin"), filp, body)
            })
            .collect();
        Fixture {
            dir,
            data: Arc::new(data),
            filp: Arc::new(filp),
            source,
            files: Arc::new(files),
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

/// The HOPE-AEAD session material an AEAD transfer derives its keys from:
/// log in over the pipeline's fake server and keep the connection open
/// while it's used. Returns the material and what to close afterwards.
pub fn aead_session() -> (*mut HxnetHopeAead, super::Held) {
    let held = super::hold(Transport::HopeAead);
    let m = unsafe { hxnet_connection_hope_aead_material(held.conn) };
    assert!(!m.is_null(), "HOPE-AEAD material");
    (m, held)
}

pub fn free_session(m: *mut HxnetHopeAead, held: super::Held) {
    unsafe { hxnet_hope_aead_free(m) };
    super::release(held);
}
