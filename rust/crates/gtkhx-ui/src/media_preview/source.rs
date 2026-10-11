//! A remote file's data fork, read a range at a time for GStreamer. Each
//! range is a download resumed at its offset, read over a transfer
//! connection of its own on a worker thread into a window of bytes. The
//! player's source element ([`RangeSrc`]) takes what the demuxer asks for out
//! of that window, waiting for the worker when it has to; a read the range
//! can't reach starts a new one there. The element asks and waits on its own
//! streaming thread, so a seek is never in flight alongside stale bytes.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::time::Duration;

use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer::subclass::prelude::*;
use gstreamer_base as gst_base;
use gstreamer_base::prelude::*;
use gstreamer_base::subclass::base_src::CreateSuccess;
use gstreamer_base::subclass::prelude::*;
use gtk4::glib;

use gtkhx_core::conn::{hx_conn_hope_aead, hx_conn_serverhost, hx_conn_serverport, hx_conn_tls};
use hxnet::ffi::{hxnet_hope_aead_clone, hxnet_hope_aead_free, HxnetHopeAead};
use hxnet::htxf::{
    hxnet_htxf_abort, hxnet_htxf_abort_arm, hxnet_htxf_abort_free, hxnet_htxf_abort_new,
    hxnet_htxf_close, hxnet_htxf_connect, hxnet_htxf_pack_preamble, hxnet_htxf_read, HtxfAbort,
    HtxfConn,
};

extern "C" {
    /// hxnet_bridge.c — the SOCKS proxy URI to reach `host:port` through, or
    /// NULL; g_free it.
    fn hx_bridge_lookup_socks_proxy(host: *const c_char, port: u16) -> *mut c_char;
}

/// `HTXF_TYPE_FILE` (hotline.h).
const HTXF_TYPE_FILE: u16 = 0;
/// How far past what the current range has read a read can land and still
/// be reached by reading on, rather than asking the server again: a request
/// costs a round trip, and mhxd can refuse one while the last winds down.
const SKIP_AHEAD: u64 = 2 << 20;
/// How far the worker reads ahead of the player before it waits.
const AHEAD: u64 = 8 << 20;
/// How much already played stays in the window, for a demuxer that steps
/// back a little.
const BEHIND: u64 = 1 << 20;
/// How far before a read a new range starts, for the same: a demuxer reads
/// an index or a tag at the end in a few steps backwards.
const BACK_OFF: u64 = 256 << 10;
/// How much of the ranges before the current one is kept. Before it plays a
/// file GStreamer reads its start, its end and its middle, and a demuxer may
/// go back to the start after reading an index at the end; kept, those don't
/// cost a request each.
const KEPT: usize = 32 << 20;
const READ_CHUNK: usize = 64 << 10;

/// The URI scheme the player's source element answers to; the rest of the
/// URI is the player's id.
pub(super) const SCHEME: &str = "gtkhx-range";

/// Where a range's transfer connects, taken from the connection on the main
/// thread: the worker can outlive the connection.
pub(super) struct Endpoint {
    host: String,
    port: u16,
    tls: bool,
    hope: *mut HxnetHopeAead,
}

// SAFETY: `hope` is an owned clone the worker alone uses, freed on drop.
unsafe impl Send for Endpoint {}

impl Endpoint {
    /// # Safety
    /// Main thread; `htlc` is a live connection.
    pub(super) unsafe fn of(htlc: *mut c_void) -> Self {
        let host = hx_conn_serverhost(htlc.cast());
        Endpoint {
            host: if host.is_null() {
                String::new()
            } else {
                CStr::from_ptr(host).to_string_lossy().into_owned()
            },
            port: hx_conn_serverport(htlc.cast()).wrapping_add(1),
            tls: hx_conn_tls(htlc.cast()) != 0,
            hope: hxnet_hope_aead_clone(hx_conn_hope_aead(htlc.cast()).cast()),
        }
    }
}

impl Drop for Endpoint {
    fn drop(&mut self) {
        if !self.hope.is_null() {
            unsafe { hxnet_hope_aead_free(self.hope) };
        }
    }
}

/// A range's cancellation: aborting it wakes a worker parked in a read.
struct Abort(*const HtxfAbort);

// SAFETY: the token is an `Arc` hxnet shares across threads by design.
unsafe impl Send for Abort {}

impl Drop for Abort {
    fn drop(&mut self) {
        unsafe {
            hxnet_htxf_abort(self.0);
            hxnet_htxf_abort_free(self.0);
        }
    }
}

/// Why a range couldn't be read.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Problem {
    Connect,
    NotAFile,
    Stopped,
}

/// Why [`Reader::fetch`] has no bytes to give.
#[derive(Debug, PartialEq, Eq)]
pub(super) enum Short {
    /// The element is flushing or the player closed.
    Flushing,
    /// Past the end of the file.
    End,
    Failed(Problem),
}

#[derive(Default)]
struct State {
    /// Bumped by every new range and by closing: a worker holding an older
    /// one stops.
    range: u64,
    /// The current range's worker is still going, or about to be.
    active: bool,
    /// The window: the bytes the current range holds, from `start`.
    start: u64,
    window: Vec<u8>,
    /// What earlier ranges read, newest last: (start, bytes).
    kept: Vec<(u64, Vec<u8>)>,
    /// How far the player has taken: the worker reads up to `AHEAD` past it.
    taken: u64,
    /// The data fork's length, once the first range's header says it.
    len: u64,
    failed: Option<Problem>,
    flushing: bool,
    closed: bool,
    abort: Option<Abort>,
}

impl State {
    fn read_to(&self) -> u64 {
        self.start + self.window.len() as u64
    }

    /// The bytes `[offset, end)`, if the window or a kept range has them.
    fn bytes(&self, offset: u64, end: u64) -> Option<Vec<u8>> {
        std::iter::once((self.start, &self.window))
            .chain(self.kept.iter().rev().map(|(s, w)| (*s, w)))
            .find(|(start, w)| *start <= offset && end <= start + w.len() as u64)
            .map(|(start, w)| w[(offset - start) as usize..(end - start) as usize].to_vec())
    }

    /// End the current range, keeping what it read, and start one at
    /// `offset`.
    fn restart(&mut self, offset: u64) -> u64 {
        if !self.window.is_empty() {
            self.kept
                .push((self.start, std::mem::take(&mut self.window)));
            let mut total: usize = self.kept.iter().map(|(_, w)| w.len()).sum();
            while total > KEPT {
                total -= self.kept.remove(0).1.len();
            }
        }
        self.range += 1;
        self.active = true;
        self.start = offset;
        self.taken = offset;
        self.failed = None;
        self.abort = None;
        self.range
    }
}

/// One player's file. `ask` sends for a range (its number, its offset); it
/// is called from the streaming thread and must hand off to the main thread.
pub(super) struct Reader {
    state: Mutex<State>,
    cond: Condvar,
    ask: Box<dyn Fn(u64, u64) + Send + Sync>,
}

impl Reader {
    pub(super) fn new(ask: impl Fn(u64, u64) + Send + Sync + 'static) -> Self {
        Reader {
            state: Mutex::default(),
            cond: Condvar::new(),
            ask: Box::new(ask),
        }
    }

    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|p| p.into_inner())
    }

    /// The first range, from the start; returns its number.
    pub(super) fn begin(&self) -> u64 {
        let mut s = self.lock();
        let range = s.restart(0);
        self.cond.notify_all();
        range
    }

    /// `range` is still the one being read.
    pub(super) fn is_current(&self, range: u64) -> bool {
        let s = self.lock();
        s.range == range && !s.closed
    }

    fn len(&self) -> u64 {
        self.lock().len
    }

    /// Stop everything for good: the player closed or failed.
    pub(super) fn close(&self) {
        let mut s = self.lock();
        s.closed = true;
        s.range += 1;
        s.abort = None;
        self.cond.notify_all();
    }

    fn set_flushing(&self, flushing: bool) {
        self.lock().flushing = flushing;
        self.cond.notify_all();
    }

    /// `length` bytes from `offset` on, or fewer at the end of the file,
    /// waiting for the worker as long as it takes. Streaming thread.
    pub(super) fn fetch(&self, offset: u64, length: usize) -> Result<Vec<u8>, Short> {
        let mut s = self.lock();
        loop {
            if s.closed || s.flushing {
                return Err(Short::Flushing);
            }
            if offset >= s.len {
                return Err(Short::End);
            }
            let end = (offset + length as u64).min(s.len);
            if let Some(bytes) = s.bytes(offset, end) {
                if s.start <= offset && offset <= s.read_to() {
                    s.taken = s.taken.max(end);
                    self.cond.notify_all();
                }
                return Ok(bytes);
            }
            if let Some(p) = s.failed {
                return Err(Short::Failed(p));
            }
            let reachable =
                s.active && s.start <= offset && offset <= s.read_to().saturating_add(SKIP_AHEAD);
            if reachable {
                s.taken = s.taken.max(offset);
            } else {
                let from = offset.saturating_sub(BACK_OFF);
                let range = s.restart(from);
                (self.ask)(range, from);
            }
            self.cond.notify_all();
            s = self
                .cond
                .wait_timeout(s, Duration::from_millis(200))
                .unwrap_or_else(|p| p.into_inner())
                .0;
        }
    }

    /// Read range `range` off the transfer the server granted (`reference`,
    /// `size` bytes, its data fork from `offset` on) into the window.
    /// `on_length` hears the data fork's whole length once the header says
    /// it. Worker thread; returns what went wrong, if it was the current
    /// range that it went wrong for.
    pub(super) fn read(
        &self,
        range: u64,
        endpoint: &Endpoint,
        reference: u32,
        size: u64,
        offset: u64,
        on_length: impl FnOnce(u64),
    ) -> Option<Problem> {
        let hx = unsafe { connect(endpoint, reference, size) };
        let problem = if hx.is_null() {
            Some(Problem::Connect)
        } else {
            let token = hxnet_htxf_abort_new();
            unsafe { hxnet_htxf_abort_arm(hx, token) };
            let current = {
                let mut s = self.lock();
                let current = s.range == range && !s.closed;
                if current {
                    s.abort = Some(Abort(token));
                }
                current
            };
            let problem = if current {
                self.stream(range, hx, offset, on_length)
            } else {
                // Moved on before it opened: opened and closed all the same,
                // so the server lets go of it rather than waiting it out.
                unsafe { hxnet_htxf_abort_free(token) };
                None
            };
            unsafe { hxnet_htxf_close(hx) };
            problem
        };
        let mut s = self.lock();
        if s.range != range || s.closed {
            return None;
        }
        s.active = false;
        s.failed = problem;
        self.cond.notify_all();
        problem
    }

    fn stream(
        &self,
        range: u64,
        hx: *mut HtxfConn,
        offset: u64,
        on_length: impl FnOnce(u64),
    ) -> Option<Problem> {
        let Ok(len) = (unsafe { data_fork_len(hx) }) else {
            return Some(Problem::NotAFile);
        };
        let end = offset + len;
        {
            let mut s = self.lock();
            if s.len == 0 {
                s.len = end;
            }
        }
        on_length(end);
        let mut next = offset;
        let mut buf = vec![0u8; READ_CHUNK];
        while next < end {
            let want = (end - next).min(READ_CHUNK as u64) as usize;
            let n = unsafe { hxnet_htxf_read(hx, buf.as_mut_ptr(), want) };
            if n <= 0 {
                return Some(Problem::Stopped);
            }
            if !self.keep(range, &buf[..n as usize]) {
                return None;
            }
            next += n as u64;
        }
        None
    }

    /// Add `chunk` to the window once the player is close enough behind,
    /// letting go of what it is well past. False when the range has ended.
    fn keep(&self, range: u64, chunk: &[u8]) -> bool {
        let mut s = self.lock();
        loop {
            if s.range != range || s.closed {
                return false;
            }
            if s.read_to() < s.taken + AHEAD {
                break;
            }
            s = self.cond.wait(s).unwrap_or_else(|p| p.into_inner());
        }
        s.window.extend_from_slice(chunk);
        let behind = s.taken.saturating_sub(s.start);
        if behind > 2 * BEHIND {
            let drop = (behind - BEHIND) as usize;
            s.window.drain(..drop);
            s.start += drop as u64;
        }
        self.cond.notify_all();
        true
    }
}

/// The readers of the open players, by id, for their source elements.
fn readers() -> MutexGuard<'static, HashMap<u64, Arc<Reader>>> {
    static READERS: OnceLock<Mutex<HashMap<u64, Arc<Reader>>>> = OnceLock::new();
    READERS
        .get_or_init(Mutex::default)
        .lock()
        .unwrap_or_else(|p| p.into_inner())
}

pub(super) fn publish(id: u64, reader: Arc<Reader>) {
    readers().insert(id, reader);
}

pub(super) fn withdraw(id: u64) {
    readers().remove(&id);
}

/// Make the source element known to GStreamer, once.
pub(super) fn register() -> bool {
    static REGISTERED: OnceLock<bool> = OnceLock::new();
    *REGISTERED.get_or_init(|| {
        gst::Element::register(
            None,
            "gtkhxrangesrc",
            gst::Rank::PRIMARY,
            RangeSrc::static_type(),
        )
        .is_ok()
    })
}

glib::wrapper! {
    /// The player's source: a [`Reader`]'s file, as bytes at any offset.
    pub struct RangeSrc(ObjectSubclass<imp::RangeSrc>)
        @extends gst_base::BaseSrc, gst::Element, gst::Object,
        @implements gst::URIHandler;
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct RangeSrc {
        pub(super) id: Mutex<Option<u64>>,
    }

    impl RangeSrc {
        fn reader(&self) -> Option<Arc<Reader>> {
            let id = (*self.id.lock().unwrap_or_else(|p| p.into_inner()))?;
            readers().get(&id).cloned()
        }
    }

    #[glib::object_subclass]
    impl ObjectSubclass for RangeSrc {
        const NAME: &'static str = "GtkhxRangeSrc";
        type Type = super::RangeSrc;
        type ParentType = gst_base::BaseSrc;
        type Interfaces = (gst::URIHandler,);
    }

    impl ObjectImpl for RangeSrc {
        fn constructed(&self) {
            self.parent_constructed();
            self.obj().set_format(gst::Format::Bytes);
        }
    }

    impl GstObjectImpl for RangeSrc {}

    impl ElementImpl for RangeSrc {
        fn metadata() -> Option<&'static gst::subclass::ElementMetadata> {
            static META: OnceLock<gst::subclass::ElementMetadata> = OnceLock::new();
            Some(META.get_or_init(|| {
                gst::subclass::ElementMetadata::new(
                    "GtkHx range source",
                    "Source/Network",
                    "A file on a Hotline server, read a range at a time",
                    "GtkHx",
                )
            }))
        }

        fn pad_templates() -> &'static [gst::PadTemplate] {
            static TEMPLATES: OnceLock<Vec<gst::PadTemplate>> = OnceLock::new();
            TEMPLATES.get_or_init(|| {
                vec![gst::PadTemplate::new(
                    "src",
                    gst::PadDirection::Src,
                    gst::PadPresence::Always,
                    &gst::Caps::new_any(),
                )
                .expect("a src pad template")]
            })
        }
    }

    impl BaseSrcImpl for RangeSrc {
        fn is_seekable(&self) -> bool {
            true
        }

        fn size(&self) -> Option<u64> {
            self.reader().map(|r| r.len())
        }

        fn unlock(&self) -> Result<(), gst::ErrorMessage> {
            if let Some(r) = self.reader() {
                r.set_flushing(true);
            }
            Ok(())
        }

        fn unlock_stop(&self) -> Result<(), gst::ErrorMessage> {
            if let Some(r) = self.reader() {
                r.set_flushing(false);
            }
            Ok(())
        }

        fn create(
            &self,
            offset: u64,
            _buffer: Option<&mut gst::BufferRef>,
            length: u32,
        ) -> Result<CreateSuccess, gst::FlowError> {
            let reader = self.reader().ok_or(gst::FlowError::Error)?;
            match reader.fetch(offset, length as usize) {
                Ok(bytes) => Ok(CreateSuccess::NewBuffer(gst::Buffer::from_mut_slice(bytes))),
                Err(Short::Flushing) => Err(gst::FlowError::Flushing),
                Err(Short::End) => Err(gst::FlowError::Eos),
                Err(Short::Failed(_)) => {
                    gst::element_imp_error!(
                        self,
                        gst::ResourceError::Read,
                        ["The server stopped sending the file"]
                    );
                    Err(gst::FlowError::Error)
                }
            }
        }
    }

    impl URIHandlerImpl for RangeSrc {
        const URI_TYPE: gst::URIType = gst::URIType::Src;

        fn protocols() -> &'static [&'static str] {
            &[SCHEME]
        }

        fn uri(&self) -> Option<String> {
            let id = (*self.id.lock().unwrap_or_else(|p| p.into_inner()))?;
            Some(format!("{SCHEME}://{id}"))
        }

        fn set_uri(&self, uri: &str) -> Result<(), glib::Error> {
            let id = uri
                .strip_prefix(SCHEME)
                .and_then(|r| r.strip_prefix("://"))
                .and_then(|id| id.parse().ok())
                .ok_or_else(|| glib::Error::new(gst::URIError::BadUri, uri))?;
            *self.id.lock().unwrap_or_else(|p| p.into_inner()) = Some(id);
            Ok(())
        }
    }
}

/// Open the transfer connection for `reference`.
///
/// # Safety
/// Worker thread.
unsafe fn connect(e: &Endpoint, reference: u32, size: u64) -> *mut HtxfConn {
    let mut pre = [0u8; 24];
    let pre_len = hxnet_htxf_pack_preamble(
        pre.as_mut_ptr(),
        pre.len(),
        reference,
        size,
        HTXF_TYPE_FILE,
        0,
        0,
    );
    if pre_len == 0 {
        return std::ptr::null_mut();
    }
    let host = crate::cs(&e.host);
    let proxy = hx_bridge_lookup_socks_proxy(host.as_ptr(), e.port);
    let (proxy_ptr, proxy_len) = if proxy.is_null() {
        (std::ptr::null(), 0)
    } else {
        (proxy as *const u8, CStr::from_ptr(proxy).to_bytes().len())
    };
    let hx = hxnet_htxf_connect(
        e.host.as_ptr(),
        e.host.len(),
        e.port,
        proxy_ptr,
        proxy_len,
        e.tls as c_int,
        pre.as_ptr(),
        pre_len,
        e.hope,
        reference,
        Some(verify_cert),
        e as *const Endpoint as *mut c_void,
    );
    if !proxy.is_null() {
        glib::ffi::g_free(proxy.cast());
    }
    hx
}

/// TLS trust for the transfer connection, asked only when WebPKI fails.
unsafe extern "C" fn verify_cert(fp: *const u8, fp_len: usize, user_data: *mut c_void) -> c_int {
    if fp.is_null() || user_data.is_null() {
        return 0;
    }
    let e = &*(user_data as *const Endpoint);
    let fp = String::from_utf8_lossy(std::slice::from_raw_parts(fp, fp_len));
    hxtls_trust::ffi::verify_cert(&e.host, e.port, &fp) as c_int
}

fn read_exact(hx: *mut HtxfConn, buf: &mut [u8]) -> Result<(), ()> {
    let mut got = 0;
    while got < buf.len() {
        let n = unsafe { hxnet_htxf_read(hx, buf[got..].as_mut_ptr(), buf.len() - got) };
        if n <= 0 {
            return Err(());
        }
        got += n as usize;
    }
    Ok(())
}

/// Read the flattened file's header and the forks before its data fork, up
/// to where the data fork's bytes begin; returns the data fork's length.
///
/// # Safety
/// `hx` is a live transfer connection; worker thread.
unsafe fn data_fork_len(hx: *mut HtxfConn) -> Result<u64, ()> {
    let mut header = [0u8; 24];
    read_exact(hx, &mut header)?;
    let forks = filp_forks(&header).ok_or(())?;
    let mut skip = vec![0u8; 4096];
    for _ in 0..forks {
        let mut fork = [0u8; 16];
        read_exact(hx, &mut fork)?;
        let (kind, len) = fork_header(&fork);
        if &kind == b"DATA" {
            return Ok(len.into());
        }
        let mut left = len as usize;
        while left > 0 {
            let n = left.min(skip.len());
            read_exact(hx, &mut skip[..n])?;
            left -= n;
        }
    }
    // No data fork: an empty file.
    Ok(0)
}

/// How many forks a flattened file's 24-byte header announces, or `None`
/// when it isn't one.
fn filp_forks(header: &[u8; 24]) -> Option<u16> {
    (&header[..4] == b"FILP").then(|| u16::from_be_bytes([header[22], header[23]]))
}

/// A fork header's kind and length.
fn fork_header(fork: &[u8; 16]) -> ([u8; 4], u32) {
    let mut kind = [0u8; 4];
    kind.copy_from_slice(&fork[..4]);
    (
        kind,
        u32::from_be_bytes([fork[12], fork[13], fork[14], fork[15]]),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn header(magic: &[u8; 4], forks: u16) -> [u8; 24] {
        let mut h = [0u8; 24];
        h[..4].copy_from_slice(magic);
        h[22..].copy_from_slice(&forks.to_be_bytes());
        h
    }

    #[test]
    fn a_flattened_file_header_reads_as_its_forks() {
        assert_eq!(filp_forks(&header(b"FILP", 3)), Some(3));
        assert_eq!(filp_forks(&header(b"\x89PNG", 3)), None);
        let mut fork = [0u8; 16];
        fork[..4].copy_from_slice(b"DATA");
        fork[12..].copy_from_slice(&1_000_000u32.to_be_bytes());
        assert_eq!(fork_header(&fork), (*b"DATA", 1_000_000));
    }

    /// A reader of a `len`-byte file whose current range holds `window`
    /// bytes from `start`, still reading; and the offset of the last range
    /// it asked for.
    fn reader_at(len: u64, start: u64, window: usize) -> (Arc<Reader>, Arc<AtomicU64>) {
        let asked = Arc::new(AtomicU64::new(u64::MAX));
        let a = Arc::clone(&asked);
        let r = Arc::new(Reader::new(move |_, offset| {
            a.store(offset, Ordering::SeqCst)
        }));
        r.begin();
        {
            let mut s = r.lock();
            s.len = len;
            s.start = start;
            s.taken = start;
            s.window = (0..window).map(|i| (start as usize + i) as u8).collect();
        }
        (r, asked)
    }

    #[test]
    fn a_read_the_window_holds_is_served_from_it() {
        let (r, asked) = reader_at(10_000, 100, 1000);
        let bytes = r.fetch(150, 10).unwrap();
        assert_eq!(bytes, (150..160).map(|i| i as u8).collect::<Vec<_>>());
        assert_eq!(asked.load(Ordering::SeqCst), u64::MAX, "nothing asked");
        assert_eq!(r.fetch(10_000, 10), Err(Short::End));
    }

    /// A read the range can't reach starts a new range there and waits for
    /// it; one it can reach waits for the worker instead.
    #[test]
    fn a_read_out_of_reach_asks_for_a_new_range() {
        for (case, offset, asks) in [
            ("before the window", 50, true),
            ("far past it", 1100 + SKIP_AHEAD + 1, true),
            ("just past it", 1100 + SKIP_AHEAD, false),
        ] {
            let (r, asked) = reader_at(1 << 30, 100, 1000);
            let waiting = Arc::clone(&r);
            let t = std::thread::spawn(move || waiting.fetch(offset, 10));
            std::thread::sleep(Duration::from_millis(50));
            let from = offset.saturating_sub(BACK_OFF);
            assert_eq!(asked.load(Ordering::SeqCst) == from, asks, "{case}");
            r.close();
            assert_eq!(t.join().unwrap(), Err(Short::Flushing), "{case}");
        }
    }

    /// What an earlier range read is served after the player has moved on,
    /// and a new range starts a little before the read that asked for it.
    #[test]
    fn earlier_ranges_are_kept_and_new_ones_start_early() {
        let (r, asked) = reader_at(1 << 30, 0, 1000);
        let waiting = Arc::clone(&r);
        let far = 100 << 20;
        let t = std::thread::spawn(move || waiting.fetch(far, 10));
        std::thread::sleep(Duration::from_millis(50));
        assert_eq!(asked.load(Ordering::SeqCst), far - BACK_OFF);
        assert_eq!(
            r.fetch(500, 10).unwrap(),
            (500..510).map(|i| i as u8).collect::<Vec<_>>()
        );
        r.close();
        assert_eq!(t.join().unwrap(), Err(Short::Flushing));
    }

    #[test]
    fn a_failed_range_fails_the_read_that_waits_on_it() {
        let (r, _) = reader_at(1 << 30, 100, 1000);
        let waiting = Arc::clone(&r);
        let t = std::thread::spawn(move || waiting.fetch(1100, 10));
        std::thread::sleep(Duration::from_millis(50));
        {
            let mut s = r.lock();
            s.active = false;
            s.failed = Some(Problem::Stopped);
        }
        r.cond.notify_all();
        assert_eq!(t.join().unwrap(), Err(Short::Failed(Problem::Stopped)));
    }

    #[test]
    fn the_worker_waits_while_far_enough_ahead() {
        let (r, _) = reader_at(1 << 30, 0, AHEAD as usize);
        let range = r.lock().range;
        let worker = Arc::clone(&r);
        let t = std::thread::spawn(move || worker.keep(range, &[0; 10]));
        std::thread::sleep(Duration::from_millis(50));
        assert!(!t.is_finished(), "it waits");
        r.fetch(1 << 20, 10).unwrap();
        assert!(t.join().unwrap(), "and goes on once the player catches up");
    }

    #[test]
    fn closing_ends_the_range() {
        let (r, _) = reader_at(1 << 30, 0, 10);
        let range = r.lock().range;
        r.close();
        assert!(!r.is_current(range));
        assert!(!r.keep(range, &[0; 10]));
    }
}
