//! TCP connect for hxnet (Phase A of the
//! `hxnet-owns-the-whole-lifecycle` work).
//!
//! Production today calls `hxnet_connection_spawn_fd_with_callback`
//! after the C side has already done DNS + TCP via
//! `GSocketClient::connect_to_host_async`. Phase A inverts that:
//! the C side passes host + port, hxnet does the resolution and
//! the connect itself, emitting `Event::State(Resolving)` and
//! `Event::State(Connecting)` along the way. The actor then
//! takes over the connected `TcpStream` exactly as it does on
//! the post-handshake `spawn_fd_*` paths today.
//!
//! Subsequent phases (B-F) layer TLS + magic + LOGIN + HOPE on
//! top of this connect step. Phase G replaces the C-side
//! `hx_connect` / `gtkhx_connect_ctx` machinery with a thin
//! wrapper around the connect FFI.
//!
//! # SOCKS / proxy support
//!
//! The legacy `GSocketClient` path picks up SOCKS proxies for
//! free via `GProxyResolver`. Phase A's first cut skips proxy
//! support — `tokio::net::TcpStream::connect` doesn't grow proxy
//! awareness for free. If users hit "TLS works but SOCKS
//! doesn't" we add `tokio-socks` (~50 LOC integration) as a
//! Phase A-1 follow-up. The decision is in §4 of the scoping
//! doc.
//!
//! # IPv4 vs IPv6 preference
//!
//! `tokio::net::lookup_host` returns addresses in the OS
//! resolver's order — on Linux that's typically v6-first on
//! dual-stack. The legacy `GSocketClient` defaults prefer v4.
//! To avoid surprising users on dual-stack networks we walk the
//! returned `SocketAddr` list and try v4 entries first, then v6.
//! Matches the legacy fallback shape without depending on
//! resolver order.

use std::io;
use std::net::SocketAddr;

use tokio::net::TcpStream;
use tokio::sync::mpsc;

use crate::{ConnectionState, Event};

/// Resolve + connect with the legacy `GSocketClient`-style
/// fallback shape. Emits `Event::State(Resolving)` and
/// `Event::State(Connecting)` on `evt_tx` as the lifecycle
/// progresses; returns the established `TcpStream` on success
/// or an `io::Error` on resolution / connect failure.
///
/// The caller is responsible for sending `Event::State(Connected)`
/// after this future resolves Ok, and for spawning the actor
/// against the returned stream. We don't do those here so the
/// caller (the FFI spawn function) keeps full control over
/// when the state event fires (e.g. after `set_nodelay`,
/// `set_keepalive`, or any other socket tuning) and over the
/// post-connect transport wrapping (e.g. Phase B's TLS layer).
pub async fn resolve_and_connect(
    host: &str,
    port: u16,
    evt_tx: &mpsc::Sender<Event>,
) -> io::Result<TcpStream> {
    // The actor's evt_tx is bounded; we use blocking sends here
    // because Phase A doesn't have a meaningful "drop event"
    // fallback. If the consumer's receiver is dropped before
    // the connect completes the send errors and we treat that
    // as caller-initiated cancellation by propagating an
    // io::Error::other; the spawn-side will surface it as
    // ShutdownReason::HandleDropped.
    if evt_tx.send(Event::State(ConnectionState::Resolving)).await.is_err() {
        return Err(io::Error::other(
            "consumer dropped before Resolving event delivered",
        ));
    }

    // Resolve. Pass the (host, port) tuple to lookup_host rather than
    // a formatted "host:port" string: the tuple's ToSocketAddrs impl
    // handles IPv6 literals correctly, whereas format!("{host}:{port}")
    // produces an unparseable "2001:db8::1:5500" for a v6 input. We
    // still get tokio's resolver instead of the addrinfo C bindings.
    // `display` is for error messages only.
    let display = format!("{host}:{port}");
    let resolved: Vec<SocketAddr> = match tokio::net::lookup_host((host, port)).await {
        Ok(iter) => iter.collect(),
        Err(e) => return Err(io::Error::new(io::ErrorKind::NotFound, format!(
            "lookup_host({display}): {e}"
        ))),
    };

    if resolved.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("lookup_host({display}): empty result"),
        ));
    }

    // v4 first, then v6 — matches GSocketClient's default
    // fallback shape on dual-stack networks. Tests with
    // IP-literal inputs see a single-entry result that's
    // trivially in either bucket.
    let (mut v4, mut v6): (Vec<_>, Vec<_>) =
        resolved.into_iter().partition(|a| a.is_ipv4());
    v4.append(&mut v6);
    let candidates = v4;

    if evt_tx.send(Event::State(ConnectionState::Connecting)).await.is_err() {
        return Err(io::Error::other(
            "consumer dropped before Connecting event delivered",
        ));
    }

    // Try each resolved address in turn; first successful
    // connect wins. Mirrors GSocketClient's iterate-until-
    // success shape.
    let mut last_err: Option<io::Error> = None;
    for addr in candidates {
        match TcpStream::connect(addr).await {
            Ok(stream) => return Ok(stream),
            Err(e) => last_err = Some(e),
        }
    }
    Err(last_err.unwrap_or_else(|| {
        io::Error::new(
            io::ErrorKind::ConnectionRefused,
            "no candidate address connected",
        )
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpListener;

    /// Stand up a real loopback `TcpListener`, drive
    /// `resolve_and_connect` at it, and verify state events
    /// fire in order and the resulting stream is read/write
    /// usable.
    #[tokio::test]
    async fn resolve_and_connect_against_local_listener() {
        let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
        let local_addr = listener.local_addr().unwrap();
        let port = local_addr.port();

        // Accept on a spawned task — the client connect needs
        // somebody listening to complete the three-way
        // handshake.
        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.expect("accept");
            // Echo: read 4 bytes, write 4 bytes back.
            let mut buf = [0u8; 4];
            stream.read_exact(&mut buf).await.expect("read");
            stream.write_all(&buf).await.expect("write");
        });

        let (evt_tx, mut evt_rx) = mpsc::channel(8);

        let connect_handle = tokio::spawn(async move {
            resolve_and_connect("127.0.0.1", port, &evt_tx).await
        });

        // Consume state events as they fire.
        let first = evt_rx.recv().await.expect("first event");
        assert!(
            matches!(first, Event::State(ConnectionState::Resolving)),
            "first event should be Resolving, got {first:?}"
        );
        let second = evt_rx.recv().await.expect("second event");
        assert!(
            matches!(second, Event::State(ConnectionState::Connecting)),
            "second event should be Connecting, got {second:?}"
        );

        let mut stream = connect_handle.await.expect("connect task").expect("connect ok");

        // Round-trip 4 bytes through the established stream to
        // prove the connection works.
        stream.write_all(b"PING").await.expect("write");
        let mut got = [0u8; 4];
        stream.read_exact(&mut got).await.expect("read");
        assert_eq!(&got, b"PING");

        server.await.expect("server task");
    }

    #[tokio::test]
    async fn resolve_and_connect_unknown_host_errors() {
        let (evt_tx, _evt_rx) = mpsc::channel(8);
        // `.invalid` is reserved by RFC 2606 and guaranteed not
        // to resolve.
        let result =
            resolve_and_connect("nope.invalid", 5500, &evt_tx).await;
        assert!(result.is_err(), "expected resolve failure");
        let err = result.unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::NotFound);
    }

    #[tokio::test]
    async fn resolve_and_connect_refused_when_no_listener() {
        let (evt_tx, _evt_rx) = mpsc::channel(8);
        // Pick a port we know nothing is listening on. Loopback
        // port 1 (tcpmux) is reserved and not bound in CI
        // containers.
        let result =
            resolve_and_connect("127.0.0.1", 1, &evt_tx).await;
        assert!(result.is_err(), "expected connect refusal");
    }
}
