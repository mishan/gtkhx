//! Synchronous HTXF (file-transfer) subchannel transport — H2 of the
//! HTXF→Rust migration (`docs/htxf-rust-migration-scoping.md`).
//!
//! A file transfer runs on its own short-lived TCP connection to the
//! server, driven by a **blocking** C pthread worker (`xfers.c`,
//! `banner.c`). This module is the Rust replacement for the byte pump
//! that worker drives — the AEAD framing currently hand-rolled in
//! `htxf_io.c`'s `aead_read` / `aead_write`.
//!
//! It is deliberately **synchronous** (plain `std::io::Read` /
//! `Write`), not tokio: the transfer worker is a blocking thread and
//! should stay one, so this avoids dragging the async runtime into the
//! file-transfer path. The plan (H2) is for the C worker to drive a
//! blocking FFI over this type; the eventual TLS variant wraps the
//! inner stream in a synchronous `rustls::StreamOwned` reusing the same
//! WebPKI→TOFU verifier the control channel already uses — that's the
//! piece that removes the shared C `GTlsConnection` accept-cert handler
//! and unblocks a clean `delete-old-connect`.
//!
//! # Framing
//!
//! When AEAD is active each [`write`](HtxfChannel::write) seals its
//! input into one frame (`[4-byte BE len][ciphertext+tag]`, via
//! [`AeadState::seal`]) and each [`read`](HtxfChannel::read) reads,
//! deframes, and opens one frame at a time, buffering decrypted
//! plaintext the caller didn't take. Mirrors the C `aead_read` /
//! `aead_write` contract: `read` returns `Ok(0)` at a clean
//! end-of-stream (frame boundary), and a partial frame followed by EOF
//! is an error. When AEAD is inactive the channel is a transparent
//! passthrough (older / unencrypted servers).

use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use hxcrypto_aead::{AeadState, AEAD_LENGTH_PREFIX, AEAD_TAG_SIZE};
use tokio_rustls::rustls::pki_types::ServerName;
use tokio_rustls::rustls::{ClientConnection, StreamOwned};

/// AEAD-framed (or passthrough) byte channel over a synchronous inner
/// transport `S`. Read/write are bounded by `S`'s `Read`/`Write` impls
/// independently, so a write-only or read-only inner is usable in
/// tests; production hands it a stream that is both.
pub struct HtxfChannel<S> {
    inner: S,
    /// Seals outgoing bytes. Unused when `!aead_active`.
    encode: AeadState,
    /// Opens incoming bytes. Unused when `!aead_active`.
    decode: AeadState,
    aead_active: bool,
    /// Decrypted plaintext from the last opened frame not yet handed to
    /// a `read` caller. `rx_plain[rx_plain_pos..]` is unconsumed.
    rx_plain: Vec<u8>,
    rx_plain_pos: usize,
    /// Reusable seal scratch (one outbound frame). Grown via `resize` so
    /// a tight transfer loop doesn't allocate per write.
    tx_framed: Vec<u8>,
    /// Reusable inbound-frame scratch (prefix + ciphertext + tag),
    /// likewise grown via `resize` to avoid a per-frame allocation.
    rx_framed: Vec<u8>,
}

impl<S> HtxfChannel<S> {
    /// Plaintext passthrough channel (unencrypted subchannel).
    pub fn new_plain(inner: S) -> Self {
        Self {
            inner,
            encode: AeadState { key: [0; 32], counter: 0, dir: 0 },
            decode: AeadState { key: [0; 32], counter: 0, dir: 0 },
            aead_active: false,
            rx_plain: Vec::new(),
            rx_plain_pos: 0,
            tx_framed: Vec::new(),
            rx_framed: Vec::new(),
        }
    }

    /// AEAD channel. `encode` seals writes, `decode` opens reads — the
    /// per-direction transfer states derived via
    /// `hxcrypto_aead`'s `derive_transfer_keys`.
    pub fn new_aead(inner: S, encode: AeadState, decode: AeadState) -> Self {
        Self {
            inner,
            encode,
            decode,
            aead_active: true,
            rx_plain: Vec::new(),
            rx_plain_pos: 0,
            tx_framed: Vec::new(),
            rx_framed: Vec::new(),
        }
    }

    /// Consume the channel, returning the inner transport.
    pub fn into_inner(self) -> S {
        self.inner
    }
}

impl<S: Write> HtxfChannel<S> {
    /// Send `buf`. With AEAD active this seals `buf` into exactly one
    /// frame and writes it whole (matching the C `aead_write`: one
    /// frame per call). Returns `buf.len()` on success — the caller's
    /// logical byte count, not the framed length.
    pub fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        // A zero-length write is a no-op in both modes — return 0 and
        // put nothing on the wire. This matches the legacy C
        // aead_write/htxf_io_write contract; in particular it stops the
        // AEAD path from sealing and sending an empty frame (a distinct
        // byte sequence on the wire) that a peer would not expect.
        if buf.is_empty() {
            return Ok(0);
        }
        if !self.aead_active {
            self.inner.write_all(buf)?;
            return Ok(buf.len());
        }
        // Reuse the per-channel seal scratch (disjoint field borrows:
        // self.encode + self.tx_framed) so a transfer loop doesn't
        // allocate a fresh Vec per frame.
        self.tx_framed.clear();
        self.tx_framed
            .resize(AEAD_LENGTH_PREFIX + buf.len() + AEAD_TAG_SIZE, 0);
        self.encode
            .seal(buf, &mut self.tx_framed)
            .ok_or_else(|| io::Error::other("HTXF AEAD seal failed"))?;
        self.inner.write_all(&self.tx_framed)?;
        Ok(buf.len())
    }

    /// Flush the inner transport.
    pub fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}

impl<S: Read> HtxfChannel<S> {
    /// Read up to `buf.len()` plaintext bytes. With AEAD active this
    /// serves from the buffered plaintext of the last opened frame,
    /// reading and opening the next frame when that's drained. Returns
    /// `Ok(0)` only at a clean end-of-stream (EOF on a frame boundary);
    /// a partial frame then EOF is an `UnexpectedEof` error.
    pub fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        // A zero-length read is a no-op — return 0 without touching the
        // transport. Critically, this stops the AEAD path below from
        // pulling and decrypting a whole frame (advancing the stream
        // and the decode counter) only to copy 0 bytes out, which would
        // desync a subsequent non-empty read.
        if buf.is_empty() {
            return Ok(0);
        }
        if !self.aead_active {
            // Retry EINTR so plaintext passthrough has the read(2)-like
            // semantics the C transfer worker expects: the legacy GIO
            // htxf_io_read hid EINTR, and an Interrupted bubbling out as
            // -1 would abort the transfer loop. (The AEAD path below
            // reads via read_exact / read_exact_or_eof, which already
            // retry Interrupted.)
            loop {
                match self.inner.read(buf) {
                    Err(ref e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    other => return other,
                }
            }
        }
        if self.rx_plain_pos >= self.rx_plain.len() && !self.fill_one_frame()? {
            return Ok(0); // clean EOF
        }
        let avail = &self.rx_plain[self.rx_plain_pos..];
        let n = avail.len().min(buf.len());
        buf[..n].copy_from_slice(&avail[..n]);
        self.rx_plain_pos += n;
        Ok(n)
    }

    /// Read + open the next frame into `rx_plain`. Returns `Ok(false)`
    /// on a clean EOF (nothing left on a frame boundary), `Ok(true)`
    /// when a frame was buffered, or an error on a malformed / truncated
    /// frame or an auth failure.
    fn fill_one_frame(&mut self) -> io::Result<bool> {
        let mut prefix = [0u8; AEAD_LENGTH_PREFIX];
        if !read_exact_or_eof(&mut self.inner, &mut prefix)? {
            return Ok(false);
        }
        let frame_total = AeadState::peek_frame_size(&prefix).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "HTXF AEAD: bad frame length prefix")
        })?;
        // Reuse the per-channel inbound scratch (bounded by
        // peek_frame_size ≤ AEAD_MAX_FRAME_SIZE, so resize can't blow up).
        self.rx_framed.clear();
        self.rx_framed.resize(frame_total, 0);
        self.rx_framed[..AEAD_LENGTH_PREFIX].copy_from_slice(&prefix);
        // The body must arrive in full — a frame boundary already passed,
        // so EOF mid-body is a truncation error, not a clean close.
        self.inner.read_exact(&mut self.rx_framed[AEAD_LENGTH_PREFIX..])?;

        // Open straight into the reusable rx_plain buffer (disjoint
        // field borrows: self.decode + self.rx_framed + self.rx_plain).
        let pt_len = frame_total - AEAD_LENGTH_PREFIX - AEAD_TAG_SIZE;
        self.rx_plain.clear();
        self.rx_plain.resize(pt_len, 0);
        self.decode
            .open(&self.rx_framed, &mut self.rx_plain)
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidData, "HTXF AEAD: frame open/auth failed")
            })?;
        self.rx_plain_pos = 0;
        Ok(true)
    }
}

/// Fill `buf` completely. Returns `Ok(false)` if EOF hits before the
/// first byte (clean end-of-stream), `Ok(true)` once full, and an
/// `UnexpectedEof` error if EOF hits after a partial read (truncation).
fn read_exact_or_eof<R: Read>(r: &mut R, buf: &mut [u8]) -> io::Result<bool> {
    let mut filled = 0;
    while filled < buf.len() {
        match r.read(&mut buf[filled..]) {
            Ok(0) => {
                if filled == 0 {
                    return Ok(false); // clean EOF on a boundary
                }
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "HTXF: EOF mid-frame-prefix",
                ));
            }
            Ok(n) => filled += n,
            Err(ref e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    Ok(true)
}

/// Synchronous TLS subchannel stream — `rustls` over a blocking
/// `TcpStream`. This is what `HtxfChannel`'s inner `S` is for a TLS
/// transfer; the plaintext case uses the bare `TcpStream`.
pub type HtxfTlsStream = StreamOwned<ClientConnection, TcpStream>;

/// Result of a synchronous HTXF TLS handshake: the wrapped stream plus
/// the trust outcome for the caller's TOFU gate.
pub struct HtxfTlsConnect {
    /// Handshake-complete TLS stream, ready to carry HTXF frames.
    pub stream: HtxfTlsStream,
    /// `true` iff the peer cert chained to a native trust root AND the
    /// hostname matched (WebPKI-valid). When `false`, the caller MUST
    /// run the trust-on-first-use decision (the C `verify_cert`
    /// callback) against `fingerprint` before sending anything — same
    /// WebPKI→TOFU split the control channel uses.
    pub webpki_ok: bool,
    /// `"sha256:<hex>"` of the peer leaf certificate, for the TOFU
    /// lookup. `None` only if the peer presented no certificate.
    pub fingerprint: Option<String>,
}

/// Run a synchronous TLS handshake over an already-connected, blocking
/// `tcp`, reusing the control channel's WebPKI→TOFU verifier
/// ([`crate::tls::webpki_client_config`]). On return the handshake is
/// complete and `webpki_ok` / `fingerprint` carry the trust outcome.
///
/// The C side keeps doing the plaintext TCP connect (so SOCKS via
/// `GProxyResolver` still works) and hands the connected fd here; this
/// is the piece that moves the HTXF TLS handshake onto rustls and off
/// the shared C `GTlsConnection` accept-cert handler.
///
/// The handshake itself is exercised by the Tier 3 TLS file-transfer
/// matrix (Janus / Mobius), mirroring how the control-channel TLS is
/// tested — there's no in-tree cert authority to drive it as a unit
/// test.
pub fn connect_tls(tcp: TcpStream, host: &str) -> io::Result<HtxfTlsConnect> {
    let (config, webpki_ok_flag) = crate::tls::webpki_client_config();
    let server_name = ServerName::try_from(host.to_string()).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("HTXF TLS: invalid server name {host:?}: {e}"),
        )
    })?;
    let conn = ClientConnection::new(Arc::new(config), server_name)
        .map_err(|e| io::Error::other(format!("HTXF TLS: client setup: {e}")))?;
    let mut stream = StreamOwned::new(conn, tcp);

    // Drive the handshake to completion on the blocking socket so the
    // trust outcome + peer cert are known before any payload moves.
    // `complete_io` performs whatever TLS read/write the handshake needs
    // next; on a blocking socket the loop ends when it's no longer
    // handshaking (done) or an IO/TLS error propagates out. Retry on
    // EINTR — a signal interrupting the blocking syscall mid-handshake
    // is recoverable and must not be reported as a handshake failure.
    while stream.conn.is_handshaking() {
        match stream.conn.complete_io(&mut stream.sock) {
            Ok(_) => {}
            Err(ref e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }

    let fingerprint = stream
        .conn
        .peer_certificates()
        .and_then(|certs| certs.first())
        .map(|leaf| crate::tls::fingerprint_sha256(leaf.as_ref()));
    let webpki_ok = webpki_ok_flag.load(Ordering::Relaxed);

    Ok(HtxfTlsConnect { stream, webpki_ok, fingerprint })
}

// ====================================================================
// C-callable FFI
// ====================================================================
//
// The C transfer worker (xfers.c / banner.c) keeps doing the plaintext
// GSocketClient TCP connect (so SOCKS via GProxyResolver still works)
// and hands the connected fd here. `open` optionally TLS-wraps it
// (rustls), writes the raw HTXF preamble, then arms AEAD; `read` /
// `write` are blocking and drive one frame at a time. The worker stays
// a blocking pthread — no tokio.

use crate::ffi::HxnetVerifyCertCallback;
use std::ffi::c_void;
use std::os::raw::c_int;
use std::os::unix::io::FromRawFd;
use std::slice;

/// Inner transport behind an open HTXF channel: plaintext TCP or
/// rustls over TCP. Both carry the AEAD framing (or passthrough).
enum HtxfInner {
    Plain(HtxfChannel<TcpStream>),
    // Boxed: the rustls `StreamOwned` is ~1 KiB, far larger than the
    // plaintext variant. One extra heap alloc per TLS transfer is
    // nothing against a file transfer, and it keeps every handle small.
    Tls(Box<HtxfChannel<HtxfTlsStream>>),
}

impl HtxfInner {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            HtxfInner::Plain(c) => c.read(buf),
            HtxfInner::Tls(c) => c.read(buf),
        }
    }
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            HtxfInner::Plain(c) => c.write(buf),
            HtxfInner::Tls(c) => c.write(buf),
        }
    }
}

/// Opaque handle the C side holds for one transfer subchannel.
pub struct HtxfConn {
    inner: HtxfInner,
}

/// Copy a C-provided `AeadState` (== `chacha_aead_state`) by value. The
/// fields are all `Copy`; this avoids assuming the struct derives it.
unsafe fn copy_aead_state(p: *const AeadState) -> AeadState {
    let s = &*p;
    AeadState { key: s.key, counter: s.counter, dir: s.dir }
}

/// Open an HTXF subchannel over an already-connected, blocking `fd`
/// (which this call adopts — the C side must NOT close it afterward).
///
/// - `tls != 0`: TLS-handshake the fd with rustls, reusing the control
///   channel's WebPKI→TOFU verifier. On WebPKI failure the `verify_cert`
///   callback runs (the C TOFU known-hosts decision); a zero return
///   rejects the connection. `host` is the SNI / TOFU hostname.
/// - `preamble` (length `preamble_len`) is written raw over the
///   (TLS-wrapped) stream before AEAD is armed — the HTXF header.
/// - `aead_encode` / `aead_decode` non-NULL arm AEAD framing with those
///   per-direction states (the transfer keys the C side derived); NULL
///   selects plaintext passthrough.
///
/// Returns an owned handle, or NULL on bad arguments, a TLS/TOFU
/// rejection, or an IO error (the adopted fd is closed in every failure
/// path by dropping the stream).
///
/// # Safety
/// `fd` must be a valid, connected, blocking socket fd this call may
/// adopt. `host` / `preamble` / `aead_*` must be valid for their lengths
/// (or NULL where documented). The handle must be used single-threaded.
#[no_mangle]
pub unsafe extern "C" fn hxnet_htxf_open(
    fd: c_int,
    tls: c_int,
    host: *const u8,
    host_len: usize,
    preamble: *const u8,
    preamble_len: usize,
    aead_encode: *const AeadState,
    aead_decode: *const AeadState,
    verify_cert: HxnetVerifyCertCallback,
    user_data: *mut c_void,
) -> *mut HtxfConn {
    if fd < 0 {
        glib::g_critical!("hxnet", "hxnet_htxf_open: negative fd");
        return std::ptr::null_mut();
    }

    // Adopt the fd up front, before any other validation, so EVERY
    // early return below drops `tcp` and closes it — honouring the
    // documented "the adopted fd is closed in every failure path"
    // contract. The C side hands ownership at the call boundary; if we
    // bailed on a bad length/NULL arg *before* adopting, that fd would
    // leak (the C worker assumes the transfer took it).
    let tcp = std::net::TcpStream::from_raw_fd(fd);

    // slice::from_raw_parts is UB for len * size_of > isize::MAX.
    if (host_len as u64) > (isize::MAX as u64) || (preamble_len as u64) > (isize::MAX as u64) {
        glib::g_critical!("hxnet", "hxnet_htxf_open: length argument exceeds isize::MAX");
        return std::ptr::null_mut();
    }
    if (preamble_len != 0 && preamble.is_null())
        || ((aead_encode.is_null()) != (aead_decode.is_null()))
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_htxf_open: NULL preamble with non-zero len, or only one AEAD state"
        );
        return std::ptr::null_mut();
    }

    // Force blocking mode. This path is synchronous (sync rustls
    // handshake + blocking read/write); a non-blocking fd would make
    // the handshake and the transfer reads fail with WouldBlock, which
    // the C worker would treat as a hard error. Make the documented
    // "blocking fd" contract robust rather than trusting the caller —
    // matches the explicit fd-mode setup the other hxnet FFI entries do.
    if let Err(e) = tcp.set_nonblocking(false) {
        glib::g_critical!("hxnet", "hxnet_htxf_open: set_nonblocking(false): {}", e);
        return std::ptr::null_mut();
    }

    let preamble_slice: &[u8] = if preamble_len == 0 {
        &[]
    } else {
        slice::from_raw_parts(preamble, preamble_len)
    };
    let aead = if aead_encode.is_null() {
        None
    } else {
        Some((copy_aead_state(aead_encode), copy_aead_state(aead_decode)))
    };

    if tls != 0 {
        if host.is_null() || host_len == 0 {
            glib::g_critical!("hxnet", "hxnet_htxf_open: TLS requested with NULL/empty host");
            return std::ptr::null_mut(); // tcp dropped → fd closed
        }
        let host_str = match std::str::from_utf8(slice::from_raw_parts(host, host_len)) {
            Ok(s) => s,
            Err(_) => {
                glib::g_critical!("hxnet", "hxnet_htxf_open: host is not valid UTF-8");
                return std::ptr::null_mut();
            }
        };
        let conn = match connect_tls(tcp, host_str) {
            Ok(c) => c,
            Err(e) => {
                glib::g_critical!("hxnet", "hxnet_htxf_open: TLS handshake: {}", e);
                return std::ptr::null_mut();
            }
        };
        // WebPKI→TOFU: only consult the C known-hosts callback when the
        // cert didn't chain to a public root, mirroring the control
        // channel. No callback ⇒ refuse an untrusted cert (fail safe).
        if !conn.webpki_ok {
            let accepted = match (verify_cert, conn.fingerprint.as_deref()) {
                (Some(cb), Some(fp)) => cb(fp.as_ptr(), fp.len(), user_data) != 0,
                _ => false,
            };
            if !accepted {
                glib::g_message!("hxnet", "hxnet_htxf_open: TLS cert rejected by TOFU gate");
                return std::ptr::null_mut(); // conn dropped → fd closed
            }
        }
        let mut stream = conn.stream;
        if let Err(e) = stream.write_all(preamble_slice).and_then(|()| stream.flush()) {
            glib::g_critical!("hxnet", "hxnet_htxf_open: preamble write (TLS): {}", e);
            return std::ptr::null_mut();
        }
        let ch = match aead {
            Some((enc, dec)) => HtxfChannel::new_aead(stream, enc, dec),
            None => HtxfChannel::new_plain(stream),
        };
        Box::into_raw(Box::new(HtxfConn { inner: HtxfInner::Tls(Box::new(ch)) }))
    } else {
        let mut tcp = tcp;
        if let Err(e) = tcp.write_all(preamble_slice).and_then(|()| tcp.flush()) {
            glib::g_critical!("hxnet", "hxnet_htxf_open: preamble write: {}", e);
            return std::ptr::null_mut();
        }
        let ch = match aead {
            Some((enc, dec)) => HtxfChannel::new_aead(tcp, enc, dec),
            None => HtxfChannel::new_plain(tcp),
        };
        Box::into_raw(Box::new(HtxfConn { inner: HtxfInner::Plain(ch) }))
    }
}

/// Blocking read of up to `len` plaintext bytes into `buf`. Returns the
/// byte count (`0` = clean end-of-stream), or `-1` on error. Matches the
/// `< 1` stop condition the C worker loops already use.
///
/// # Safety
/// `handle` must be a live handle from [`hxnet_htxf_open`]; `buf` valid
/// for `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn hxnet_htxf_read(
    handle: *mut HtxfConn,
    buf: *mut u8,
    len: usize,
) -> isize {
    if handle.is_null() || (buf.is_null() && len != 0) || (len as u64) > (isize::MAX as u64) {
        return -1;
    }
    let h = &mut *handle;
    let out = if len == 0 { &mut [][..] } else { slice::from_raw_parts_mut(buf, len) };
    match h.inner.read(out) {
        Ok(n) => n as isize,
        Err(_) => -1,
    }
}

/// Blocking write of `len` bytes from `buf` (one AEAD frame when AEAD is
/// active). Returns `len` on success, `-1` on error.
///
/// # Safety
/// `handle` must be a live handle from [`hxnet_htxf_open`]; `buf` valid
/// for `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn hxnet_htxf_write(
    handle: *mut HtxfConn,
    buf: *const u8,
    len: usize,
) -> isize {
    if handle.is_null() || (buf.is_null() && len != 0) || (len as u64) > (isize::MAX as u64) {
        return -1;
    }
    let h = &mut *handle;
    let data = if len == 0 { &[][..] } else { slice::from_raw_parts(buf, len) };
    match h.inner.write(data) {
        Ok(n) => n as isize,
        Err(_) => -1,
    }
}

/// Close the subchannel and free the handle (drops the stream, closing
/// the fd / tearing down TLS). Safe to call with NULL.
///
/// # Safety
/// `handle` must be a handle from [`hxnet_htxf_open`] not yet closed.
#[no_mangle]
pub unsafe extern "C" fn hxnet_htxf_close(handle: *mut HtxfConn) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use hxcrypto_aead::{AEAD_DIR_CLIENT_TO_SERVER, AEAD_DIR_SERVER_TO_CLIENT};
    use std::io::Cursor;

    fn key() -> [u8; 32] {
        [0x37u8; 32]
    }

    /// One shared key + direction: a `new_aead` writer's `encode` and a
    /// reader's `decode` must start identical (same key/dir/counter=0)
    /// to interoperate, exactly like the two ends of a real transfer.
    fn enc() -> AeadState {
        AeadState { key: key(), counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER }
    }
    fn dec() -> AeadState {
        AeadState { key: key(), counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER }
    }

    #[test]
    fn passthrough_roundtrip() {
        let mut tx = HtxfChannel::new_plain(Vec::<u8>::new());
        assert_eq!(tx.write(b"plain bytes").unwrap(), 11);
        let bytes = tx.into_inner();
        assert_eq!(&bytes, b"plain bytes");

        let mut rx = HtxfChannel::new_plain(Cursor::new(bytes));
        let mut out = [0u8; 32];
        let n = rx.read(&mut out).unwrap();
        assert_eq!(&out[..n], b"plain bytes");
    }

    #[test]
    fn aead_single_frame_roundtrip() {
        let mut tx = HtxfChannel::new_aead(Vec::<u8>::new(), enc(), dec());
        let msg = b"a sealed file chunk";
        assert_eq!(tx.write(msg).unwrap(), msg.len());
        let wire = tx.into_inner();
        // Framed: longer than plaintext (prefix + tag).
        assert_eq!(wire.len(), AEAD_LENGTH_PREFIX + msg.len() + AEAD_TAG_SIZE);

        let mut rx = HtxfChannel::new_aead(Cursor::new(wire), enc(), dec());
        let mut out = vec![0u8; msg.len()];
        let n = rx.read(&mut out).unwrap();
        assert_eq!(&out[..n], msg);
        // Clean EOF afterwards.
        assert_eq!(rx.read(&mut out).unwrap(), 0);
    }

    #[test]
    fn aead_zero_length_write_is_noop() {
        // A zero-length write must put nothing on the wire — no empty
        // sealed frame — matching the legacy C aead_write contract.
        let mut tx = HtxfChannel::new_aead(Vec::<u8>::new(), enc(), dec());
        assert_eq!(tx.write(b"").unwrap(), 0);
        // A following real write still produces exactly one frame, and
        // the empty write didn't bump the encode counter / emit bytes.
        let msg = b"after empty";
        assert_eq!(tx.write(msg).unwrap(), msg.len());
        let wire = tx.into_inner();
        assert_eq!(wire.len(), AEAD_LENGTH_PREFIX + msg.len() + AEAD_TAG_SIZE);

        // And it round-trips: a fresh reader (counter 0) opens it.
        let mut rx = HtxfChannel::new_aead(Cursor::new(wire), enc(), dec());
        let mut out = vec![0u8; msg.len()];
        let n = rx.read(&mut out).unwrap();
        assert_eq!(&out[..n], msg);
    }

    #[test]
    fn aead_zero_length_read_is_noop() {
        // A zero-length read must not pull/decrypt a frame: the frame
        // stays buffered so a subsequent real read still sees it.
        let mut tx = HtxfChannel::new_aead(Vec::<u8>::new(), enc(), dec());
        tx.write(b"intact").unwrap();
        let wire = tx.into_inner();

        let mut rx = HtxfChannel::new_aead(Cursor::new(wire), enc(), dec());
        assert_eq!(rx.read(&mut []).unwrap(), 0);
        let mut out = vec![0u8; 6];
        let n = rx.read(&mut out).unwrap();
        assert_eq!(&out[..n], b"intact");
    }

    #[test]
    fn aead_multi_frame_and_partial_reads() {
        let mut tx = HtxfChannel::new_aead(Vec::<u8>::new(), enc(), dec());
        tx.write(b"frame-one").unwrap();
        tx.write(b"frame-two-is-longer").unwrap();
        let wire = tx.into_inner();

        let mut rx = HtxfChannel::new_aead(Cursor::new(wire), enc(), dec());
        // Drain everything through a deliberately tiny buffer so reads
        // span frame boundaries and the plaintext buffer is exercised.
        let mut got = Vec::new();
        let mut small = [0u8; 4];
        loop {
            let n = rx.read(&mut small).unwrap();
            if n == 0 {
                break;
            }
            got.extend_from_slice(&small[..n]);
        }
        assert_eq!(&got, b"frame-oneframe-two-is-longer");
    }

    #[test]
    fn aead_tampered_frame_errors() {
        let mut tx = HtxfChannel::new_aead(Vec::<u8>::new(), enc(), dec());
        tx.write(b"tamper me").unwrap();
        let mut wire = tx.into_inner();
        // Flip a ciphertext byte (after the 4-byte length prefix).
        wire[AEAD_LENGTH_PREFIX] ^= 0xff;

        let mut rx = HtxfChannel::new_aead(Cursor::new(wire), enc(), dec());
        let mut out = [0u8; 32];
        let err = rx.read(&mut out).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }

    // Exercise the FFI end-to-end over a real loopback socket on the
    // plaintext+AEAD path: open adopts the client fd, writes the raw
    // preamble, arms AEAD; a server thread reads the preamble raw then
    // speaks the matching AEAD framing. (TLS path is Tier-3-covered.)
    #[test]
    fn ffi_open_write_read_close_over_socket() {
        use std::net::{TcpListener, TcpStream};
        use std::os::unix::io::IntoRawFd;
        use std::thread;

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let k = key();

        let server = thread::spawn(move || {
            let (mut sock, _) = listener.accept().unwrap();
            // Preamble is written raw before AEAD arms.
            let mut pre = [0u8; 3];
            sock.read_exact(&mut pre).unwrap();
            assert_eq!(&pre, b"PRE");
            // Server decodes the client's CLIENT_TO_SERVER frames and
            // encodes its replies SERVER_TO_CLIENT.
            let s_enc = AeadState { key: k, counter: 0, dir: AEAD_DIR_SERVER_TO_CLIENT };
            let s_dec = AeadState { key: k, counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER };
            let mut sch = HtxfChannel::new_aead(sock, s_enc, s_dec);
            let mut buf = [0u8; 64];
            let n = sch.read(&mut buf).unwrap();
            assert_eq!(&buf[..n], b"ping");
            assert_eq!(sch.write(b"pong").unwrap(), 4);
        });

        let client = TcpStream::connect(addr).unwrap();
        let fd = client.into_raw_fd();
        let enc = AeadState { key: k, counter: 0, dir: AEAD_DIR_CLIENT_TO_SERVER };
        let dec = AeadState { key: k, counter: 0, dir: AEAD_DIR_SERVER_TO_CLIENT };

        let h = unsafe {
            hxnet_htxf_open(
                fd, 0, std::ptr::null(), 0,
                b"PRE".as_ptr(), 3,
                &enc, &dec,
                None, std::ptr::null_mut(),
            )
        };
        assert!(!h.is_null());

        let w = unsafe { hxnet_htxf_write(h, b"ping".as_ptr(), 4) };
        assert_eq!(w, 4);

        let mut out = [0u8; 64];
        let n = unsafe { hxnet_htxf_read(h, out.as_mut_ptr(), out.len()) };
        assert!(n > 0);
        assert_eq!(&out[..n as usize], b"pong");

        unsafe { hxnet_htxf_close(h) };
        server.join().unwrap();
    }

    #[test]
    fn aead_truncated_frame_errors() {
        let mut tx = HtxfChannel::new_aead(Vec::<u8>::new(), enc(), dec());
        tx.write(b"truncate me").unwrap();
        let mut wire = tx.into_inner();
        wire.truncate(wire.len() - 3); // drop tail of the frame body

        let mut rx = HtxfChannel::new_aead(Cursor::new(wire), enc(), dec());
        let mut out = [0u8; 32];
        let err = rx.read(&mut out).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::UnexpectedEof);
    }
}
