/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

//! Tracker fetch-runner (R3 item 8, T2).
//!
//! Orchestration on top of the per-connection [`crate::tracker`] engine:
//! a serial walk over the configured tracker URL list, with the full
//! connect / transport / probe fallback ladder the legacy C state
//! machine (`network.c::tracker_fetch_*`) implemented as a chain of
//! one-bounce GIO callbacks:
//!
//!   per URL:
//!     1. TLS-first connect unless a cached verdict says the tracker is
//!        plain-only. A TLS *handshake* failure records the NO verdict
//!        and falls back to plain TCP; a transport failure (DNS / refused
//!        / timeout) does not — a plain retry to an unreachable host just
//!        doubles the wait.
//!     2. v3 probe (8-byte handshake + watchdog). On an inconclusive
//!        probe (silence / short read / junk) reopen the connection and
//!        run v1. A recognised reply drives the matching listing flow.
//!     3. Emit the batch + records, or a per-tracker error line.
//!
//! The orchestration is generic over a [`TrackerConnector`] so the
//! walk / fallback logic is unit-testable with scripted in-memory
//! streams; the production connector (TCP + rustls, reusing
//! [`crate::tls`] and [`crate::connect`]) lives in the FFI layer and is
//! covered by the Tier 3 tracker integration tests.
//!
//! Unlike the C path — which interleaved per-record progress ticks as
//! bytes arrived off the wire — the engine reads a whole listing before
//! returning it, so records for one tracker are emitted as a burst.
//! The progress widget therefore ticks per-tracker rather than
//! per-record within a tracker; acceptable for the small listings real
//! trackers serve, and the cross-tracker progress is unchanged.

use std::collections::HashMap;
use std::time::Duration;

use std::pin::Pin;
use std::task::{Context, Poll};

use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};
use tokio::sync::mpsc;

use crate::tracker::{self, Outcome, TrackerRecord};

/// Default tracker TCP port (HTRK), used when a URL carries no `:port`.
/// Mirror of the C `HTRK_TCPPORT`.
pub const HTRK_TCPPORT: u16 = 5498;

/// Which transport a single connect attempt should use.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Transport {
    /// rustls over TCP (TOFU-verified, as the main session does).
    Tls,
    /// Plain TCP.
    Plain,
}

/// Why a connect attempt failed — the distinction drives the fallback.
#[derive(Debug)]
pub enum ConnectError {
    /// TLS handshake / certificate failure. The runner falls back to
    /// plain TCP and records a `No` verdict for the URL.
    Tls(String),
    /// Couldn't establish the TCP connection at all (DNS, refused,
    /// timeout). No fallback helps — the URL is reported as failed.
    Transport(String),
}

impl std::fmt::Display for ConnectError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ConnectError::Tls(m) => write!(f, "TLS: {m}"),
            ConnectError::Transport(m) => write!(f, "{m}"),
        }
    }
}

/// Per-URL memory of whether a tracker speaks TLS, so a Refresh doesn't
/// re-pay a failed handshake. Process-scoped (re-probed each launch) in
/// production; a fresh instance per call in tests. Mirror of the C
/// `tracker_tls_verdict_cache`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TlsVerdict {
    /// Never tried this tracker this run — attempt TLS first.
    Unknown,
    /// TLS handshake succeeded (and the cert was trusted / pinned).
    Ok,
    /// TLS failed — skip the attempt and go straight to plain.
    No,
}

/// URL → [`TlsVerdict`] map. The runner reads it to decide whether to
/// attempt TLS, and updates it as attempts resolve.
#[derive(Debug, Default)]
pub struct VerdictCache {
    map: HashMap<String, TlsVerdict>,
}

impl VerdictCache {
    pub fn new() -> Self {
        Self::default()
    }

    fn lookup(&self, url: &str) -> TlsVerdict {
        self.map.get(url).copied().unwrap_or(TlsVerdict::Unknown)
    }

    fn record(&mut self, url: &str, v: TlsVerdict) {
        self.map.insert(url.to_owned(), v);
    }
}

/// Obtains a stream for a tracker URL. The runner calls this once per
/// connect attempt (up to twice per URL — the v3 probe and, on an
/// inconclusive probe, the v1 reopen — plus a possible TLS→plain
/// retry within a single attempt).
#[allow(async_fn_in_trait)] // internal trait; futures are driven on one task
pub trait TrackerConnector {
    /// The connected, ready-to-speak stream (already TLS-wrapped when
    /// `transport == Tls`).
    type Stream: AsyncRead + AsyncWrite + Unpin;

    /// Connect to `url` over `transport`. `Err(ConnectError::Tls)`
    /// triggers the plain-TCP fallback; `Err(ConnectError::Transport)`
    /// fails the URL.
    async fn connect(
        &mut self,
        url: &str,
        transport: Transport,
    ) -> Result<Self::Stream, ConnectError>;
}

/// Events the runner emits as it walks the URL list. The C bridge
/// drains these on the GLib main loop and re-emits the existing
/// `tracker-batch-begin` / `tracker-server-create` signals.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TrackerEvent {
    /// A tracker replied; records for it follow. Mirrors the
    /// `tracker-batch-begin` signal (url, wire version, batch size).
    BatchBegin {
        url: String,
        version: u8,
        count: u16,
    },
    /// One server record for the current batch. `total` is the batch
    /// size (what the legacy `HxTrackerServer.total` field carries).
    Record {
        url: String,
        total: u16,
        record: TrackerRecord,
    },
    /// A tracker could not be fetched (connect failure or a hard
    /// protocol error). Mirrors the C `hx_printf_prefix` error line.
    BatchError { url: String, message: String },
    /// The whole walk finished (all URLs attempted).
    Done,
}

/// Connect to `url`, applying the TLS-first / plain-fallback ladder and
/// updating `verdicts`. Returns the established stream, or the connect
/// error to report for this URL.
async fn connect_with_fallback<C: TrackerConnector>(
    connector: &mut C,
    url: &str,
    verdicts: &mut VerdictCache,
) -> Result<C::Stream, ConnectError> {
    if verdicts.lookup(url) != TlsVerdict::No {
        match connector.connect(url, Transport::Tls).await {
            Ok(s) => {
                verdicts.record(url, TlsVerdict::Ok);
                return Ok(s);
            }
            // TLS handshake failed — remember it and drop to plain.
            Err(ConnectError::Tls(_)) => {
                verdicts.record(url, TlsVerdict::No);
            }
            // Host unreachable — a plain retry won't help.
            Err(e @ ConnectError::Transport(_)) => return Err(e),
        }
    }
    connector.connect(url, Transport::Plain).await
}

/// Emit a listing as `BatchBegin` + one `Record` per server. Returns
/// `false` if the consumer's receiver was dropped (caller cancelled).
async fn emit_listing(
    out: &mpsc::Sender<TrackerEvent>,
    url: &str,
    listing: tracker::TrackerListing,
) -> bool {
    // Capture the Copy header fields before moving `listing.records` out,
    // so the loop body doesn't read from a partially-moved value.
    let version = listing.version;
    let count = listing.expected;
    if out
        .send(TrackerEvent::BatchBegin {
            url: url.to_owned(),
            version,
            count,
        })
        .await
        .is_err()
    {
        return false;
    }
    for record in listing.records {
        if out
            .send(TrackerEvent::Record {
                url: url.to_owned(),
                total: count,
                record,
            })
            .await
            .is_err()
        {
            return false;
        }
    }
    true
}

/// Emit a per-tracker error line. Returns `false` if the receiver was
/// dropped.
async fn emit_error(out: &mpsc::Sender<TrackerEvent>, url: &str, message: String) -> bool {
    out.send(TrackerEvent::BatchError {
        url: url.to_owned(),
        message,
    })
    .await
    .is_ok()
}

/// Fetch one tracker URL. Returns `false` if the consumer went away
/// (stop the whole walk); `true` to continue to the next URL, including
/// after a reported connect / protocol error.
async fn fetch_one<C: TrackerConnector>(
    connector: &mut C,
    url: &str,
    features: u16,
    probe_timeout: Duration,
    verdicts: &mut VerdictCache,
    out: &mpsc::Sender<TrackerEvent>,
) -> bool {
    // Attempt 1: connect (TLS-first) + v3 probe.
    let mut stream = match connect_with_fallback(connector, url, verdicts).await {
        Ok(s) => s,
        Err(e) => return emit_error(out, url, e.to_string()).await,
    };

    match tracker::run_v3_probe(&mut stream, features, probe_timeout).await {
        Ok(Outcome::Listing(listing)) => return emit_listing(out, url, listing).await,
        Ok(Outcome::ProbeTimedOut) => { /* fall through to the v1 reopen */ }
        Err(e) => return emit_error(out, url, e.to_string()).await,
    }

    // The v3 probe was inconclusive (a pre-spec v1 tracker that ignored
    // the 0x0003 version byte). Close it and reopen for a clean v1
    // exchange. The reopen runs the same TLS ladder — the verdict is
    // already settled, so this just reconnects on the chosen transport.
    drop(stream);

    let mut stream = match connect_with_fallback(connector, url, verdicts).await {
        Ok(s) => s,
        Err(e) => return emit_error(out, url, e.to_string()).await,
    };
    match tracker::run_v1(&mut stream).await {
        Ok(listing) => emit_listing(out, url, listing).await,
        Err(e) => emit_error(out, url, e.to_string()).await,
    }
}

/// Walk `urls` serially, emitting [`TrackerEvent`]s on `out`. Stops
/// early if the receiver is dropped (the caller's cancel signal). Emits
/// a final [`TrackerEvent::Done`] when the walk completes normally.
pub async fn run_fetch<C: TrackerConnector>(
    connector: &mut C,
    urls: &[String],
    features: u16,
    probe_timeout: Duration,
    verdicts: &mut VerdictCache,
    out: &mpsc::Sender<TrackerEvent>,
) {
    for url in urls {
        if !fetch_one(connector, url, features, probe_timeout, verdicts, out).await {
            return; // receiver dropped — caller cancelled
        }
    }
    let _ = out.send(TrackerEvent::Done).await;
}

// ---- Production connector (TCP + rustls) ---------------------------------

/// A connected tracker stream — plain TCP or rustls-over-TCP. Both
/// halves are `Unpin`, so the [`AsyncRead`] / [`AsyncWrite`] impls just
/// project to the active variant. The TLS variant is boxed because
/// `TlsStream` is large.
pub enum TrackerTransport {
    Plain(tokio::net::TcpStream),
    Tls(Box<tokio_rustls::client::TlsStream<tokio::net::TcpStream>>),
}

impl AsyncRead for TrackerTransport {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            TrackerTransport::Plain(s) => Pin::new(s).poll_read(cx, buf),
            TrackerTransport::Tls(s) => Pin::new(s.as_mut()).poll_read(cx, buf),
        }
    }
}

impl AsyncWrite for TrackerTransport {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        match self.get_mut() {
            TrackerTransport::Plain(s) => Pin::new(s).poll_write(cx, buf),
            TrackerTransport::Tls(s) => Pin::new(s.as_mut()).poll_write(cx, buf),
        }
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            TrackerTransport::Plain(s) => Pin::new(s).poll_flush(cx),
            TrackerTransport::Tls(s) => Pin::new(s.as_mut()).poll_flush(cx),
        }
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        match self.get_mut() {
            TrackerTransport::Plain(s) => Pin::new(s).poll_shutdown(cx),
            TrackerTransport::Tls(s) => Pin::new(s.as_mut()).poll_shutdown(cx),
        }
    }
}

/// Split a tracker URL into `(host, port)`, defaulting the port to
/// [`HTRK_TCPPORT`]. Mirrors what `g_network_address_parse` did for the
/// legacy `g_socket_client_connect_to_host (serverstr, HTRK_TCPPORT)`:
/// a bare host keeps the default; a `host:port` suffix overrides it; an
/// IPv6 literal must be bracketed (`[::1]` or `[::1]:5498`) so its
/// colons aren't mistaken for a port separator.
fn parse_host_port(url: &str) -> (String, u16) {
    let url = url.trim();
    // Bracketed IPv6 literal, optionally with a port.
    if let Some(rest) = url.strip_prefix('[') {
        if let Some(close) = rest.find(']') {
            let host = &rest[..close];
            let after = &rest[close + 1..];
            let port = after
                .strip_prefix(':')
                .and_then(|p| p.parse::<u16>().ok())
                .unwrap_or(HTRK_TCPPORT);
            return (host.to_owned(), port);
        }
    }
    // host:port only when there's exactly one colon and a numeric tail —
    // a bare IPv6 literal (multiple colons) keeps the default port.
    if let Some((host, tail)) = url.rsplit_once(':') {
        if !host.contains(':') {
            if let Ok(port) = tail.parse::<u16>() {
                return (host.to_owned(), port);
            }
        }
    }
    (url.to_owned(), HTRK_TCPPORT)
}

/// TOFU verify callback: given a leaf cert's `"sha256:<hex>"`
/// fingerprint, returns `true` to accept the connection. Wraps the
/// C-side trust check (marshalled to the GLib main thread).
pub type VerifyFn = Box<dyn Fn(&str) -> bool + Send>;

/// Production [`TrackerConnector`]: tokio TCP connect, with an optional
/// rustls wrap for [`Transport::Tls`]. The TOFU gate matches the
/// control-connection path (`lifecycle::run_plaintext_tls_lifecycle`):
/// a WebPKI-valid cert is trusted silently; otherwise `verify` is
/// consulted with the leaf `"sha256:<hex>"` fingerprint and may reject.
pub struct TcpTlsConnector {
    /// TOFU verify callback (the C trust check, marshalled). `None`
    /// accepts any non-WebPKI cert — only safe for tests / probes.
    pub verify: Option<VerifyFn>,
}

impl TrackerConnector for TcpTlsConnector {
    type Stream = TrackerTransport;

    async fn connect(
        &mut self,
        url: &str,
        transport: Transport,
    ) -> Result<Self::Stream, ConnectError> {
        let (host, port) = parse_host_port(url);

        // resolve_and_connect emits two lifecycle events; a small bounded
        // channel held only for the call absorbs them (no consumer needed
        // here — the tracker walk has its own event stream).
        let (evt_tx, _evt_rx) = mpsc::channel(4);
        let tcp = crate::connect::resolve_and_connect(&host, port, &evt_tx)
            .await
            .map_err(|e| ConnectError::Transport(e.to_string()))?;

        match transport {
            Transport::Plain => Ok(TrackerTransport::Plain(tcp)),
            Transport::Tls => {
                let (tls, webpki_ok) = crate::tls::wrap_tls(tcp, &host)
                    .await
                    .map_err(|e| ConnectError::Tls(e.to_string()))?;
                // WebPKI-valid → trust silently; else fall to the TOFU gate.
                if !webpki_ok.load(std::sync::atomic::Ordering::Relaxed) {
                    if let Some(verify) = self.verify.as_ref() {
                        match crate::tls::peer_cert_fingerprint(&tls) {
                            Some(fp) => {
                                if !verify(&fp) {
                                    return Err(ConnectError::Tls(
                                        "certificate rejected by trust check".to_owned(),
                                    ));
                                }
                            }
                            None => {
                                return Err(ConnectError::Tls(
                                    "peer presented no certificate".to_owned(),
                                ));
                            }
                        }
                    }
                }
                Ok(TrackerTransport::Tls(Box::new(tls)))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use hotline_proto::parse::tracker_v3;
    use std::collections::VecDeque;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    const HTRK_MAGIC: [u8; 4] = *b"HTRK";

    /// What a scripted server does on a single connection.
    enum Plan {
        /// Write `bytes` and keep the connection open, draining whatever
        /// the engine sends. Staying open matters for v3, where the
        /// engine writes a listing request *after* reading the handshake
        /// response — a server that closed early would BrokenPipe that
        /// write.
        Reply(Vec<u8>),
        /// Drain the handshake then close without replying — an EOF
        /// during the probe → inconclusive → v1 reopen.
        Silent,
        /// Fail the connect at the TLS layer (→ plain fallback).
        FailTls,
        /// Fail the connect at the transport layer (→ URL fails).
        FailTransport,
    }

    /// Connector that replays a queue of [`Plan`]s and records every
    /// connect call's `(url, transport)` for assertions.
    struct ScriptedConnector {
        plans: VecDeque<Plan>,
        calls: Vec<(String, Transport)>,
    }

    impl ScriptedConnector {
        fn new(plans: Vec<Plan>) -> Self {
            Self {
                plans: plans.into(),
                calls: Vec::new(),
            }
        }
    }

    impl TrackerConnector for ScriptedConnector {
        type Stream = tokio::io::DuplexStream;

        async fn connect(
            &mut self,
            url: &str,
            transport: Transport,
        ) -> Result<Self::Stream, ConnectError> {
            self.calls.push((url.to_owned(), transport));
            match self.plans.pop_front().expect("connect with no scripted plan") {
                Plan::FailTls => Err(ConnectError::Tls("scripted".into())),
                Plan::FailTransport => Err(ConnectError::Transport("scripted".into())),
                Plan::Silent => {
                    let (client, mut server) = tokio::io::duplex(64 * 1024);
                    tokio::spawn(async move {
                        // Read the handshake (keeps the client's write half
                        // open so its write succeeds), then drop → EOF.
                        let mut buf = [0u8; 16];
                        let _ = server.read(&mut buf).await;
                    });
                    Ok(client)
                }
                Plan::Reply(bytes) => {
                    let (client, mut server) = tokio::io::duplex(64 * 1024);
                    tokio::spawn(async move {
                        let _ = server.write_all(&bytes).await;
                        let _ = server.flush().await;
                        // Keep the read half open (drain the engine's
                        // handshake + any v3 listing request) until the
                        // client closes, so post-read engine writes don't
                        // hit a BrokenPipe.
                        let mut buf = [0u8; 256];
                        while let Ok(n) = server.read(&mut buf).await {
                            if n == 0 {
                                break;
                            }
                        }
                    });
                    Ok(client)
                }
            }
        }
    }

    fn v1_header(nservers: u16) -> Vec<u8> {
        let mut h = vec![0u8; 14];
        h[0..4].copy_from_slice(&HTRK_MAGIC);
        h[4..6].copy_from_slice(&1u16.to_be_bytes());
        h[10..12].copy_from_slice(&nservers.to_be_bytes());
        h
    }

    fn v1_record(ip: [u8; 4], port: u16, nusers: u16, name: &[u8], desc: &[u8]) -> Vec<u8> {
        let mut r = Vec::new();
        r.extend_from_slice(&ip);
        r.extend_from_slice(&port.to_be_bytes());
        r.extend_from_slice(&nusers.to_be_bytes());
        r.extend_from_slice(&[0, 0]);
        r.push(name.len() as u8);
        r.extend_from_slice(name);
        r.push(desc.len() as u8);
        r.extend_from_slice(desc);
        r
    }

    /// A minimal v3 listing reply: 6-byte handshake response + 10-byte
    /// listing-response header + `records` blob.
    fn v3_reply(records: &[u8], record_count: u16) -> Vec<u8> {
        let mut r = Vec::new();
        // 8-byte handshake response: HTRK + version 3 + 2 feature bytes
        // (run_v3_probe reads the first 6, read_v3_listing the trailing 2).
        r.extend_from_slice(&HTRK_MAGIC);
        r.extend_from_slice(&3u16.to_be_bytes());
        r.extend_from_slice(&0u16.to_be_bytes()); // features
        // 10-byte listing-response header, matching the layout
        // parse_tracker_v3_response_header reads: response_type(2) +
        // total_size(4) + total_servers(2) + record_count(2).
        r.extend_from_slice(&tracker_v3::RESP_LIST.to_be_bytes());
        r.extend_from_slice(&(records.len() as u32).to_be_bytes());
        r.extend_from_slice(&record_count.to_be_bytes());
        r.extend_from_slice(&record_count.to_be_bytes());
        r.extend_from_slice(records);
        r
    }

    /// Drain all events from a finished run.
    async fn collect(mut rx: mpsc::Receiver<TrackerEvent>) -> Vec<TrackerEvent> {
        let mut out = Vec::new();
        while let Some(ev) = rx.recv().await {
            out.push(ev);
        }
        out
    }

    fn urls(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[tokio::test]
    async fn v1_tracker_over_tls_first_attempt() {
        let mut script = v1_header(1);
        script.extend_from_slice(&v1_record([1, 2, 3, 4], 5500, 2, b"srv", b"d"));

        // TLS connect succeeds; the server only reads 6 handshake bytes
        // and replies v1, so the probe's shared dispatch drives v1 on the
        // first connection (no reopen).
        let mut conn = ScriptedConnector::new(vec![Plan::Reply(script)]);
        let (tx, rx) = mpsc::channel(64);
        let mut verdicts = VerdictCache::new();
        run_fetch(&mut conn, &urls(&["t1"]), 0, Duration::from_secs(5), &mut verdicts, &tx).await;
        drop(tx);

        let events = collect(rx).await;
        assert_eq!(conn.calls, vec![("t1".to_string(), Transport::Tls)]);
        assert_eq!(verdicts.lookup("t1"), TlsVerdict::Ok);
        assert!(matches!(
            events[0],
            TrackerEvent::BatchBegin { version: 1, count: 1, .. }
        ));
        assert!(matches!(events[1], TrackerEvent::Record { total: 1, .. }));
        assert!(matches!(events.last(), Some(TrackerEvent::Done)));
    }

    #[tokio::test]
    async fn v1_tracker_needs_probe_reopen() {
        // First connection: silent (EOF during probe → ProbeTimedOut).
        // Second connection (reopen): a v1 reply.
        let mut script = v1_header(1);
        script.extend_from_slice(&v1_record([9, 9, 9, 9], 6000, 0, b"late", b""));

        let mut conn = ScriptedConnector::new(vec![Plan::Silent, Plan::Reply(script)]);
        let (tx, rx) = mpsc::channel(64);
        let mut verdicts = VerdictCache::new();
        run_fetch(&mut conn, &urls(&["t1"]), 0, Duration::from_secs(5), &mut verdicts, &tx).await;
        drop(tx);

        let events = collect(rx).await;
        // Both connects went out on TLS (verdict OK after the first).
        assert_eq!(
            conn.calls,
            vec![
                ("t1".to_string(), Transport::Tls),
                ("t1".to_string(), Transport::Tls)
            ]
        );
        assert!(matches!(
            events[0],
            TrackerEvent::BatchBegin { version: 1, count: 1, .. }
        ));
        assert!(matches!(&events[1], TrackerEvent::Record { record, .. } if record.name == b"late"));
    }

    #[tokio::test]
    async fn tls_handshake_failure_falls_back_to_plain() {
        let mut script = v1_header(1);
        script.extend_from_slice(&v1_record([1, 1, 1, 1], 5500, 1, b"plainonly", b""));

        // TLS connect fails → record NO → plain connect succeeds + v1.
        let mut conn = ScriptedConnector::new(vec![Plan::FailTls, Plan::Reply(script)]);
        let (tx, rx) = mpsc::channel(64);
        let mut verdicts = VerdictCache::new();
        run_fetch(&mut conn, &urls(&["t1"]), 0, Duration::from_secs(5), &mut verdicts, &tx).await;
        drop(tx);

        let events = collect(rx).await;
        assert_eq!(
            conn.calls,
            vec![
                ("t1".to_string(), Transport::Tls),
                ("t1".to_string(), Transport::Plain)
            ]
        );
        assert_eq!(verdicts.lookup("t1"), TlsVerdict::No);
        assert!(matches!(
            events[0],
            TrackerEvent::BatchBegin { version: 1, .. }
        ));
    }

    #[tokio::test]
    async fn cached_no_verdict_skips_tls() {
        let mut script = v1_header(0);
        // empty listing is fine; we only assert the transport choice.
        let _ = &mut script;

        let mut conn = ScriptedConnector::new(vec![Plan::Reply(v1_header(0))]);
        let (tx, rx) = mpsc::channel(64);
        let mut verdicts = VerdictCache::new();
        verdicts.record("t1", TlsVerdict::No);
        run_fetch(&mut conn, &urls(&["t1"]), 0, Duration::from_secs(5), &mut verdicts, &tx).await;
        drop(tx);

        let _ = collect(rx).await;
        // No TLS attempt at all — straight to plain.
        assert_eq!(conn.calls, vec![("t1".to_string(), Transport::Plain)]);
    }

    #[tokio::test]
    async fn transport_failure_reports_error_and_continues() {
        // First URL: hard transport failure (no fallback). Second URL:
        // a clean v1 listing. The walk continues past the failure.
        let mut script = v1_header(1);
        script.extend_from_slice(&v1_record([7, 7, 7, 7], 5500, 1, b"ok", b""));

        let mut conn = ScriptedConnector::new(vec![Plan::FailTransport, Plan::Reply(script)]);
        let (tx, rx) = mpsc::channel(64);
        let mut verdicts = VerdictCache::new();
        run_fetch(
            &mut conn,
            &urls(&["bad", "good"]),
            0,
            Duration::from_secs(5),
            &mut verdicts,
            &tx,
        )
        .await;
        drop(tx);

        let events = collect(rx).await;
        assert!(matches!(
            &events[0],
            TrackerEvent::BatchError { url, .. } if url == "bad"
        ));
        assert!(matches!(
            &events[1],
            TrackerEvent::BatchBegin { url, .. } if url == "good"
        ));
        assert!(matches!(events.last(), Some(TrackerEvent::Done)));
    }

    #[tokio::test]
    async fn v3_listing_round_trip() {
        // One v3 record (IPv4, no TLVs). IPv4 carries no addr-length
        // byte; name/desc are u16-length-prefixed.
        let mut rec = Vec::new();
        rec.push(tracker_v3::ADDR_IPV4); // addr_type
        rec.extend_from_slice(&[10, 0, 0, 1]); // address (4 bytes, no len)
        rec.extend_from_slice(&5500u16.to_be_bytes()); // port
        rec.extend_from_slice(&3u16.to_be_bytes()); // nusers
        rec.extend_from_slice(&3u16.to_be_bytes()); // name_len
        rec.extend_from_slice(b"abc");
        rec.extend_from_slice(&0u16.to_be_bytes()); // desc_len
        rec.extend_from_slice(&0u16.to_be_bytes()); // tlv_count

        let reply = v3_reply(&rec, 1);
        let mut conn = ScriptedConnector::new(vec![Plan::Reply(reply)]);
        let (tx, rx) = mpsc::channel(64);
        let mut verdicts = VerdictCache::new();
        run_fetch(&mut conn, &urls(&["t3"]), 0, Duration::from_secs(5), &mut verdicts, &tx).await;
        drop(tx);

        let events = collect(rx).await;
        assert!(matches!(
            events[0],
            TrackerEvent::BatchBegin { version: 3, count: 1, .. }
        ));
        assert!(matches!(
            &events[1],
            TrackerEvent::Record { record, .. } if record.name == b"abc" && record.port == 5500
        ));
    }

    #[test]
    fn parse_host_port_cases() {
        assert_eq!(parse_host_port("tracker.example.com"), ("tracker.example.com".into(), HTRK_TCPPORT));
        assert_eq!(parse_host_port("tracker.example.com:5499"), ("tracker.example.com".into(), 5499));
        assert_eq!(parse_host_port("127.0.0.1:1234"), ("127.0.0.1".into(), 1234));
        // Bare IPv6 literal: colons are address, not a port.
        assert_eq!(parse_host_port("::1"), ("::1".into(), HTRK_TCPPORT));
        // Bracketed IPv6, with and without a port.
        assert_eq!(parse_host_port("[::1]"), ("::1".into(), HTRK_TCPPORT));
        assert_eq!(parse_host_port("[2001:db8::1]:5499"), ("2001:db8::1".into(), 5499));
        // Non-numeric tail is part of the host, not a port.
        assert_eq!(parse_host_port("host:notaport"), ("host:notaport".into(), HTRK_TCPPORT));
    }

    #[tokio::test]
    async fn production_connector_v1_over_plain_tcp() {
        // Drive run_fetch with the REAL TcpTlsConnector against an
        // in-process listener that speaks a v1 listing. Verdict is
        // pre-seeded No so the connector skips TLS (the listener is
        // plain), exercising resolve_and_connect + run_v1 over a real
        // socket end-to-end.
        let mut reply = v1_header(1);
        reply.extend_from_slice(&v1_record([8, 8, 8, 8], 5500, 5, b"real", b"x"));

        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let server = tokio::spawn(async move {
            let (mut sock, _) = listener.accept().await.unwrap();
            // Drain the 6-byte v1 magic the engine sends, then reply.
            let mut magic = [0u8; 6];
            sock.read_exact(&mut magic).await.unwrap();
            sock.write_all(&reply).await.unwrap();
            sock.flush().await.unwrap();
        });

        let mut conn = TcpTlsConnector { verify: None };
        let mut verdicts = VerdictCache::new();
        let url = format!("127.0.0.1:{}", addr.port());
        verdicts.record(&url, TlsVerdict::No);
        let (tx, rx) = mpsc::channel(64);
        run_fetch(
            &mut conn,
            &[url.clone()],
            0,
            Duration::from_secs(5),
            &mut verdicts,
            &tx,
        )
        .await;
        drop(tx);
        server.await.unwrap();

        let events = collect(rx).await;
        assert!(matches!(
            &events[0],
            TrackerEvent::BatchBegin { url: u, version: 1, count: 1 } if *u == url
        ));
        assert!(matches!(
            &events[1],
            TrackerEvent::Record { record, .. } if record.name == b"real" && record.nusers == 5
        ));
        assert!(matches!(events.last(), Some(TrackerEvent::Done)));
    }

    #[tokio::test]
    async fn dropped_receiver_stops_walk() {
        // Receiver dropped before the run: the first emit fails and the
        // walk returns without touching the second URL.
        let mut script = v1_header(1);
        script.extend_from_slice(&v1_record([1, 2, 3, 4], 5500, 1, b"a", b""));
        let mut conn = ScriptedConnector::new(vec![Plan::Reply(script)]);
        let (tx, rx) = mpsc::channel(64);
        drop(rx);
        run_fetch(
            &mut conn,
            &urls(&["one", "two"]),
            0,
            Duration::from_secs(5),
            &mut VerdictCache::new(),
            &tx,
        )
        .await;
        // Only the first URL was attempted before the send failure bailed.
        assert_eq!(conn.calls.len(), 1);
    }
}
