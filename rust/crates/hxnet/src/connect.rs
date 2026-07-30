//! TCP connect for hxnet (Phase A of the
//! `hxnet-owns-the-whole-lifecycle` work).
//!
//! The C side passes host + port; hxnet does the resolution and the
//! connect itself, emitting `Event::State(Resolving)` and
//! `Event::State(Connecting)` along the way, then hands the connected
//! `TcpStream` to the actor. No OS socket fd ever crosses the FFI —
//! this is the connect primitive every entry point
//! (`hxnet_connection_open_tcp` / `_open_plaintext` / `_open_hope` /
//! `_open_plaintext_tls`, and `hxnet_htxf_connect`) builds on.
//!
//! Subsequent phases (B-F) layer TLS + magic + LOGIN + HOPE on
//! top of this connect step. Phase G replaces the C-side
//! `hx_connect` / `gtkhx_connect_ctx` machinery with a thin
//! wrapper around the connect FFI.
//!
//! # SOCKS / proxy support
//!
//! The legacy `GSocketClient` path picked up SOCKS proxies for
//! free via `GProxyResolver`; `tokio::net::TcpStream::connect`
//! does not. R3 item 9 (S1) adds it here: when a [`ProxyConfig`]
//! is supplied, [`resolve_and_connect`] tunnels through the proxy
//! via `tokio-socks` instead of connecting directly. The target
//! hostname is handed to the proxy for **remote DNS** (socks5h /
//! socks4a semantics) so proxy-only hosts resolve and DNS doesn't
//! leak; the direct (`proxy = None`) path is byte-identical to
//! before. Only the transport mechanic lives here — *where* the
//! proxy config comes from (`GProxyResolver` query in C, env vars,
//! or a pref) is S2, see `docs/rust/socks-proxy-scoping.md`.
//! HTTP-CONNECT proxies are out of scope (tokio-socks is SOCKS
//! only); [`ProxyConfig::from_uri`] rejects `http(s)://` loudly.
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

/// SOCKS protocol version for a proxy. We always do *remote* DNS in the
/// proxied path, so `socks5`/`socks5h` collapse to [`ProxyScheme::Socks5`]
/// and `socks4`/`socks4a` to [`ProxyScheme::Socks4`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProxyScheme {
    Socks4,
    Socks5,
}

/// A parsed SOCKS proxy configuration for [`resolve_and_connect`].
///
/// `Debug` is implemented by hand to redact the password — `{:?}` on a
/// proxy config (e.g. in an error path) must never leak the credential.
#[derive(Clone, PartialEq, Eq)]
pub struct ProxyConfig {
    pub scheme: ProxyScheme,
    /// Proxy endpoint, `"host:port"` (resolved locally — only the *target*
    /// goes to the proxy for remote DNS).
    pub addr: String,
    /// SOCKS5 username/password. Ignored for SOCKS4 (no user/pass
    /// sub-negotiation).
    pub auth: Option<(String, String)>,
}

impl std::fmt::Debug for ProxyConfig {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ProxyConfig")
            .field("scheme", &self.scheme)
            .field("addr", &self.addr)
            // Show whether auth is set + the username, but never the
            // password.
            .field(
                "auth",
                &self
                    .auth
                    .as_ref()
                    .map(|(user, _)| (user.as_str(), "<redacted>")),
            )
            .finish()
    }
}

impl ProxyConfig {
    /// Parse a `socks5://[user:pass@]host:port` URI. Also accepts
    /// `socks5h`, `socks4`, and `socks4a` (DNS is always remote here, so
    /// the `h`/`a` variants are equivalent to their base scheme). Returns
    /// `Err` for `http(s)://` (HTTP-CONNECT proxies aren't supported —
    /// `tokio-socks` is SOCKS only; fail loudly rather than silently
    /// connect direct) or otherwise malformed input.
    pub fn from_uri(uri: &str) -> Result<ProxyConfig, String> {
        let uri = uri.trim();
        // Don't echo the raw URI here: a schemeless value is often a bare
        // `user:pass@host:port`, and echoing it would leak the password
        // into logs/telemetry. The scheme is what's wrong; the value adds
        // nothing diagnostic.
        let (scheme_str, rest) = uri.split_once("://").ok_or_else(|| {
            "proxy URI missing scheme (expected socks5://, socks5h://, socks4:// or socks4a://)"
                .to_string()
        })?;
        let scheme = match scheme_str.to_ascii_lowercase().as_str() {
            // Bare `socks` is the generic scheme GProxyResolver returns for
            // a SOCKS proxy; treat it as SOCKS5 (the modern default) rather
            // than hard-failing.
            "socks" | "socks5" | "socks5h" => ProxyScheme::Socks5,
            "socks4" | "socks4a" => ProxyScheme::Socks4,
            "http" | "https" => {
                return Err(format!(
                    "HTTP-CONNECT proxies are not supported (got {scheme_str:?}); SOCKS only"
                ))
            }
            other => return Err(format!("unsupported proxy scheme {other:?}")),
        };
        // Optional `user:pass@`. rsplit so a host (which never contains
        // `@`) can't be mistaken for userinfo.
        let (auth, hostport) = match rest.rsplit_once('@') {
            Some((userinfo, hp)) => {
                let (u, p) = userinfo
                    .split_once(':')
                    .ok_or_else(|| "proxy userinfo must be user:pass".to_string())?;
                (Some((u.to_owned(), p.to_owned())), hp)
            }
            None => (None, rest),
        };
        validate_host_port(hostport)?;
        // SOCKS4 has no user/pass auth — drop any creds rather than
        // pretend we'll honour them.
        let auth = if scheme == ProxyScheme::Socks5 {
            auth
        } else {
            None
        };
        Ok(ProxyConfig {
            scheme,
            addr: hostport.to_owned(),
            auth,
        })
    }
}

/// Validate that `hp` is a `host:port` with a non-empty host and a
/// numeric `u16` port. Accepts bracketed IPv6 (`[::1]:1080`). Rejects a
/// bare IPv6 with no port (`[::1]`) or a missing/empty/non-numeric port,
/// which would otherwise surface much later as a confusing `lookup_host`
/// failure.
fn validate_host_port(hp: &str) -> Result<(), String> {
    let (host, port) = if let Some(rest) = hp.strip_prefix('[') {
        // Bracketed IPv6: `[addr]:port`.
        let close = rest
            .find(']')
            .ok_or_else(|| format!("unterminated IPv6 bracket in proxy address {hp:?}"))?;
        let host = &rest[..close];
        let port = rest[close + 1..]
            .strip_prefix(':')
            .ok_or_else(|| format!("proxy address missing :port: {hp:?}"))?;
        (host, port)
    } else {
        let (host, port) = hp
            .rsplit_once(':')
            .ok_or_else(|| format!("proxy address must be host:port: {hp:?}"))?;
        // An unbracketed host still carrying a `:` is an IPv6 literal
        // written without brackets (e.g. `2001:db8::1:1080`). rsplit would
        // silently treat the last group as the port; reject it here so the
        // intent ("validate early") holds instead of failing later in
        // lookup_host with a confusing message.
        if host.contains(':') {
            return Err(format!(
                "IPv6 proxy address must be bracketed as [addr]:port: {hp:?}"
            ));
        }
        (host, port)
    };
    if host.is_empty() {
        return Err(format!("proxy address missing host: {hp:?}"));
    }
    port.parse::<u16>()
        .map_err(|_| format!("proxy address has an invalid port: {hp:?}"))?;
    Ok(())
}

/// Tunnel a TCP connection to `(host, port)` through `proxy`, handing the
/// hostname to the proxy for remote DNS. Returns the plain `TcpStream`
/// underneath the SOCKS session — everything downstream (rustls, magic,
/// LOGIN, HOPE) layers on top exactly as over a direct connect. Bounded
/// by the same `HANDSHAKE_TIMEOUT_SECS` as the direct path.
///
/// Error wrapping a failed SOCKS connect attempt so the proxy/target
/// context travels with the underlying `io::Error` (the `source`) instead
/// of being flattened into a string — keeping `kind()` and `source()`
/// inspectable, the same as the direct connect path's raw `io::Error`.
#[cfg(feature = "socks")]
#[derive(Debug)]
struct ProxyConnectError {
    target: String,
    proxy: SocketAddr,
    source: tokio_socks::Error,
}

#[cfg(feature = "socks")]
impl std::fmt::Display for ProxyConnectError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "SOCKS connect to {} via {} failed",
            self.target, self.proxy
        )
    }
}

#[cfg(feature = "socks")]
impl std::error::Error for ProxyConnectError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        Some(&self.source)
    }
}

/// Only compiled with the `socks` feature (which pulls `tokio-socks`).
///
/// Emits `Event::State(Connecting)` itself — *after* the proxy address
/// lookup, before the first connect attempt — so the state semantics
/// match the direct path (which also fires `Connecting` only once its
/// `lookup_host` has completed). The caller emits `Resolving` before
/// delegating here.
#[cfg(feature = "socks")]
async fn proxy_connect(
    proxy: &ProxyConfig,
    host: &str,
    port: u16,
    evt_tx: &mpsc::Sender<Event>,
) -> io::Result<TcpStream> {
    use tokio_socks::tcp::{Socks4Stream, Socks5Stream};

    // Resolve the proxy's *own* address locally — that's not a leak (the
    // proxy is ours to reach); only the target must go remote. Passing a
    // concrete SocketAddr also sidesteps tokio-socks' ToProxyAddr string
    // parsing for IPv6 literals.
    let resolved: Vec<SocketAddr> = tokio::net::lookup_host(proxy.addr.as_str())
        .await
        // Preserve the underlying kind (a malformed addr is InvalidInput,
        // a real DNS miss is NotFound) so proxy issues are diagnosable.
        .map_err(|e| io::Error::new(e.kind(), format!("proxy {}: {e}", proxy.addr)))?
        .collect();
    // Prefer IPv4 first, then v6 — the same v4-before-v6 ordering and
    // try-each-until-success fallback the direct path uses, so a
    // dual-stack / multi-A proxy endpoint with a dead address still
    // connects via a live one instead of failing on the first.
    let (mut v4, mut v6): (Vec<_>, Vec<_>) = resolved.into_iter().partition(|a| a.is_ipv4());
    v4.append(&mut v6);
    let candidates = v4;
    if candidates.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("proxy {}: no address", proxy.addr),
        ));
    }

    // Proxy address resolved — now entering the connect phase, same as the
    // direct path emits Connecting after its own lookup_host.
    if evt_tx
        .send(Event::State(ConnectionState::Connecting))
        .await
        .is_err()
    {
        return Err(io::Error::other(
            "consumer dropped before Connecting event delivered",
        ));
    }

    let timeout = std::time::Duration::from_secs(crate::HANDSHAKE_TIMEOUT_SECS);
    let mut last_err: Option<io::Error> = None;
    for proxy_addr in candidates {
        // Target passed as a domain tuple → the proxy resolves it
        // (socks5h / socks4a remote DNS). Each attempt is bounded by the
        // handshake timeout so a black-holed proxy address fails over to
        // the next instead of hanging the whole connect.
        let connect = async {
            match (proxy.scheme, proxy.auth.as_ref()) {
                (ProxyScheme::Socks5, Some((user, pass))) => {
                    Socks5Stream::connect_with_password(proxy_addr, (host, port), user, pass)
                        .await
                        .map(Socks5Stream::into_inner)
                }
                (ProxyScheme::Socks5, None) => Socks5Stream::connect(proxy_addr, (host, port))
                    .await
                    .map(Socks5Stream::into_inner),
                (ProxyScheme::Socks4, _) => Socks4Stream::connect(proxy_addr, (host, port))
                    .await
                    .map(Socks4Stream::into_inner),
            }
        };

        match tokio::time::timeout(timeout, connect).await {
            Ok(Ok(stream)) => return Ok(stream),
            Ok(Err(e)) => {
                // Keep the underlying tokio-socks error as the *source*
                // (inspectable via source()) instead of stringifying it, and
                // recover the real ErrorKind from its transport (Io) variant
                // instead of forcing Other — matching the direct path, where
                // callers can inspect kind() and source().
                let kind = match &e {
                    tokio_socks::Error::Io(io_e) => io_e.kind(),
                    _ => io::ErrorKind::Other,
                };
                last_err = Some(io::Error::new(
                    kind,
                    ProxyConnectError {
                        target: format!("{host}:{port}"),
                        proxy: proxy_addr,
                        source: e,
                    },
                ));
            }
            Err(_elapsed) => {
                last_err = Some(io::Error::new(
                    io::ErrorKind::TimedOut,
                    format!(
                        "SOCKS connect via {proxy_addr} timed out after {}s",
                        crate::HANDSHAKE_TIMEOUT_SECS
                    ),
                ));
            }
        }
    }

    // Every candidate failed; surface the last error.
    Err(last_err.unwrap_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            format!("proxy {}: no address", proxy.addr),
        )
    }))
}

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
    proxy: Option<&ProxyConfig>,
    evt_tx: &mpsc::Sender<Event>,
) -> io::Result<TcpStream> {
    // The actor's evt_tx is bounded; these awaits apply backpressure
    // rather than dropping events. If the consumer's receiver is
    // dropped before the connect completes, the send errors and we
    // treat that as caller-initiated cancellation by returning
    // io::Error::other; the spawn-side maps any error from here to a
    // StreamError shutdown (see hxnet_connection_open_* in ffi.rs).
    if evt_tx
        .send(Event::State(ConnectionState::Resolving))
        .await
        .is_err()
    {
        return Err(io::Error::other(
            "consumer dropped before Resolving event delivered",
        ));
    }

    // Proxied path: tunnel through the SOCKS proxy and let *it* resolve
    // the target (remote DNS). We skip the local lookup_host + v4/v6 walk
    // entirely — those only apply to a direct connect.
    if let Some(proxy) = proxy {
        // Without the `socks` feature there's no tunnel implementation;
        // fail loudly rather than silently connecting direct past a
        // configured proxy.
        #[cfg(not(feature = "socks"))]
        {
            let _ = proxy;
            return Err(io::Error::new(
                io::ErrorKind::Unsupported,
                "a SOCKS proxy is configured but hxnet was built without the `socks` feature",
            ));
        }
        #[cfg(feature = "socks")]
        {
            // proxy_connect emits Connecting itself, after it has resolved
            // the proxy address — keeping the Resolving→Connecting boundary
            // aligned with the direct path's (which fires Connecting only
            // after its own lookup_host).
            return proxy_connect(proxy, host, port, evt_tx).await;
        }
    }

    // Resolve. Pass the (host, port) tuple to lookup_host rather than
    // a formatted "host:port" string: the tuple's ToSocketAddrs impl
    // handles IPv6 literals correctly, whereas format!("{host}:{port}")
    // produces an unparseable "2001:db8::1:5500" for a v6 input. We
    // still get tokio's resolver instead of the addrinfo C bindings.
    // `display` is for error messages only. Bracket IPv6 literals so a
    // v6 host reads unambiguously (e.g. `[2001:db8::1]:5500` rather than
    // the colon-soup `2001:db8::1:5500`).
    let display = if host.contains(':') {
        format!("[{host}]:{port}")
    } else {
        format!("{host}:{port}")
    };
    let resolved: Vec<SocketAddr> = match tokio::net::lookup_host((host, port)).await {
        Ok(iter) => iter.collect(),
        Err(e) => {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("lookup_host({display}): {e}"),
            ))
        }
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
    let (mut v4, mut v6): (Vec<_>, Vec<_>) = resolved.into_iter().partition(|a| a.is_ipv4());
    v4.append(&mut v6);
    let candidates = v4;

    if evt_tx
        .send(Event::State(ConnectionState::Connecting))
        .await
        .is_err()
    {
        return Err(io::Error::other(
            "consumer dropped before Connecting event delivered",
        ));
    }

    // Try each resolved address in turn; first successful
    // connect wins. Mirrors GSocketClient's iterate-until-
    // success shape. Each attempt is bounded by the handshake
    // timeout so an unresponsive host (SYN black-hole) fails
    // instead of hanging the whole connect — the C side then
    // tears down cleanly via the shutdown path.
    let connect_timeout = std::time::Duration::from_secs(crate::HANDSHAKE_TIMEOUT_SECS);
    let mut last_err: Option<io::Error> = None;
    for addr in candidates {
        match tokio::time::timeout(connect_timeout, TcpStream::connect(addr)).await {
            Ok(Ok(stream)) => return Ok(stream),
            Ok(Err(e)) => last_err = Some(e),
            Err(_elapsed) => {
                last_err = Some(io::Error::new(
                    io::ErrorKind::TimedOut,
                    format!(
                        "connect to {addr} timed out after {}s",
                        crate::HANDSHAKE_TIMEOUT_SECS
                    ),
                ));
            }
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

        let connect_handle =
            tokio::spawn(
                async move { resolve_and_connect("127.0.0.1", port, None, &evt_tx).await },
            );

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

        let mut stream = connect_handle
            .await
            .expect("connect task")
            .expect("connect ok");

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
        let result = resolve_and_connect("nope.invalid", 5500, None, &evt_tx).await;
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
        let result = resolve_and_connect("127.0.0.1", 1, None, &evt_tx).await;
        assert!(result.is_err(), "expected connect refusal");
    }

    // ---- SOCKS proxy (S1) ------------------------------------------------

    #[test]
    fn proxy_uri_parsing() {
        let p = ProxyConfig::from_uri("socks5://1.2.3.4:1080").unwrap();
        assert_eq!(p.scheme, ProxyScheme::Socks5);
        assert_eq!(p.addr, "1.2.3.4:1080");
        assert!(p.auth.is_none());

        let p = ProxyConfig::from_uri("socks5h://user:pass@proxy.example:1080").unwrap();
        assert_eq!(p.scheme, ProxyScheme::Socks5);
        assert_eq!(p.addr, "proxy.example:1080");
        assert_eq!(p.auth, Some(("user".to_owned(), "pass".to_owned())));

        let p = ProxyConfig::from_uri("socks4a://10.0.0.1:9050").unwrap();
        assert_eq!(p.scheme, ProxyScheme::Socks4);
        assert!(p.auth.is_none());

        // SOCKS4 has no user/pass auth — creds are dropped, not honoured.
        let p = ProxyConfig::from_uri("socks4://u:p@10.0.0.1:9050").unwrap();
        assert_eq!(p.scheme, ProxyScheme::Socks4);
        assert!(p.auth.is_none());

        // HTTP-CONNECT proxies are rejected loudly (not silently direct).
        assert!(ProxyConfig::from_uri("http://proxy:8080")
            .unwrap_err()
            .contains("HTTP-CONNECT"));
        // Bracketed IPv6 endpoint, with and without auth.
        // Bare `socks://` (what GProxyResolver returns) normalises to SOCKS5.
        let p = ProxyConfig::from_uri("socks://1.2.3.4:1080").unwrap();
        assert_eq!(p.scheme, ProxyScheme::Socks5);
        assert_eq!(p.addr, "1.2.3.4:1080");

        let p = ProxyConfig::from_uri("socks5://[::1]:1080").unwrap();
        assert_eq!(p.scheme, ProxyScheme::Socks5);
        assert_eq!(p.addr, "[::1]:1080");
        let p = ProxyConfig::from_uri("socks5://u:p@[2001:db8::1]:9050").unwrap();
        assert_eq!(p.addr, "[2001:db8::1]:9050");
        assert_eq!(p.auth, Some(("u".to_owned(), "p".to_owned())));

        // Missing scheme / port / non-numeric port / bare-IPv6-no-port.
        assert!(ProxyConfig::from_uri("1.2.3.4:1080").is_err());
        assert!(ProxyConfig::from_uri("socks5://1.2.3.4").is_err());
        assert!(ProxyConfig::from_uri("socks5://1.2.3.4:").is_err());
        assert!(ProxyConfig::from_uri("socks5://1.2.3.4:notaport").is_err());
        assert!(ProxyConfig::from_uri("socks5://[::1]").is_err());
        assert!(ProxyConfig::from_uri("socks5://[::1").is_err());
        // Unbracketed IPv6 literal with a port must be rejected, not
        // silently misparsed (last group treated as the port).
        assert!(ProxyConfig::from_uri("socks5://2001:db8::1:1080").is_err());

        // The redacting Debug never prints the password.
        let p = ProxyConfig::from_uri("socks5://bob:hunter2@1.2.3.4:1080").unwrap();
        let dbg = format!("{p:?}");
        assert!(dbg.contains("bob") && dbg.contains("<redacted>"));
        assert!(!dbg.contains("hunter2"), "password leaked in Debug: {dbg}");

        // A schemeless `user:pass@host:port` must not leak the password
        // through the error message.
        let err = ProxyConfig::from_uri("alice:s3cret@1.2.3.4:1080").unwrap_err();
        assert!(!err.contains("s3cret"), "password leaked in error: {err}");
    }

    /// Minimal loopback SOCKS5 responder. Runs the greeting (optionally
    /// username/password auth, asserting the creds), reads the CONNECT
    /// request, reports the requested target back over a oneshot, replies
    /// success, then echoes bytes (acting as the tunnelled target). Returns
    /// the proxy's address + the target receiver.
    #[cfg(feature = "socks")]
    fn spawn_mock_socks5(
        require_auth: Option<(&'static str, &'static str)>,
    ) -> (SocketAddr, tokio::sync::oneshot::Receiver<String>) {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let listener = TcpListener::from_std(listener).unwrap();
        let addr = listener.local_addr().unwrap();
        let (tx, rx) = tokio::sync::oneshot::channel();
        tokio::spawn(async move {
            let (mut s, _) = listener.accept().await.unwrap();

            // Greeting: VER, NMETHODS, METHODS[NMETHODS].
            let mut head = [0u8; 2];
            s.read_exact(&mut head).await.unwrap();
            assert_eq!(head[0], 0x05, "socks version");
            let mut methods = vec![0u8; head[1] as usize];
            s.read_exact(&mut methods).await.unwrap();
            // The client must advertise the method we're about to select:
            // 0x02 (username/password) when we require auth, else 0x00
            // (no-auth). Asserting it both documents the expectation and
            // uses `methods` (no dead-read warning).
            let want_method = if require_auth.is_some() { 0x02 } else { 0x00 };
            assert!(
                methods.contains(&want_method),
                "client did not advertise SOCKS method {want_method:#04x}: {methods:?}"
            );

            if let Some((eu, ep)) = require_auth {
                s.write_all(&[0x05, 0x02]).await.unwrap(); // username/password
                let mut a = [0u8; 2]; // VER(1), ULEN
                s.read_exact(&mut a).await.unwrap();
                let mut user = vec![0u8; a[1] as usize];
                s.read_exact(&mut user).await.unwrap();
                let mut pl = [0u8; 1];
                s.read_exact(&mut pl).await.unwrap();
                let mut pass = vec![0u8; pl[0] as usize];
                s.read_exact(&mut pass).await.unwrap();
                assert_eq!(user, eu.as_bytes(), "socks username");
                assert_eq!(pass, ep.as_bytes(), "socks password");
                s.write_all(&[0x01, 0x00]).await.unwrap(); // auth OK
            } else {
                s.write_all(&[0x05, 0x00]).await.unwrap(); // no auth
            }

            // CONNECT request: VER CMD RSV ATYP, then addr + port.
            let mut req = [0u8; 4];
            s.read_exact(&mut req).await.unwrap();
            assert_eq!(req[1], 0x01, "CONNECT command");
            let target = match req[3] {
                0x03 => {
                    let mut l = [0u8; 1];
                    s.read_exact(&mut l).await.unwrap();
                    let mut d = vec![0u8; l[0] as usize];
                    s.read_exact(&mut d).await.unwrap();
                    String::from_utf8_lossy(&d).into_owned()
                }
                0x01 => {
                    let mut ip = [0u8; 4];
                    s.read_exact(&mut ip).await.unwrap();
                    format!("{}.{}.{}.{}", ip[0], ip[1], ip[2], ip[3])
                }
                other => panic!("unexpected ATYP {other}"),
            };
            let mut p = [0u8; 2];
            s.read_exact(&mut p).await.unwrap();
            let port = u16::from_be_bytes(p);
            let _ = tx.send(format!("{target}:{port}"));

            // Reply success: VER REP RSV ATYP BND.ADDR(4) BND.PORT(2).
            s.write_all(&[0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0])
                .await
                .unwrap();

            // Tunnel is open — echo bytes back as the "target" would.
            let mut buf = [0u8; 64];
            loop {
                match s.read(&mut buf).await {
                    Ok(0) | Err(_) => break,
                    Ok(n) => {
                        if s.write_all(&buf[..n]).await.is_err() {
                            break;
                        }
                    }
                }
            }
        });
        (addr, rx)
    }

    /// Without the `socks` feature, supplying a proxy must fail loudly
    /// (Unsupported) rather than silently connect direct.
    #[cfg(not(feature = "socks"))]
    #[tokio::test]
    async fn proxy_without_socks_feature_errors() {
        let proxy = ProxyConfig {
            scheme: ProxyScheme::Socks5,
            addr: "127.0.0.1:1080".to_owned(),
            auth: None,
        };
        let (evt_tx, _evt_rx) = mpsc::channel(8);
        let err = resolve_and_connect("host.invalid", 80, Some(&proxy), &evt_tx)
            .await
            .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::Unsupported);
    }

    #[cfg(feature = "socks")]
    #[tokio::test]
    async fn socks5_anonymous_connect_uses_remote_dns() {
        let (proxy_addr, target_rx) = spawn_mock_socks5(None);
        let proxy = ProxyConfig {
            scheme: ProxyScheme::Socks5,
            addr: proxy_addr.to_string(),
            auth: None,
        };
        let (evt_tx, mut evt_rx) = mpsc::channel(8);

        // `server.invalid` does NOT resolve locally — a direct connect
        // would fail at lookup_host. Reaching it proves the domain went to
        // the proxy for remote DNS.
        let mut stream = resolve_and_connect("server.invalid", 5500, Some(&proxy), &evt_tx)
            .await
            .expect("proxy connect should succeed");

        assert!(matches!(
            evt_rx.recv().await,
            Some(Event::State(ConnectionState::Resolving))
        ));
        assert!(matches!(
            evt_rx.recv().await,
            Some(Event::State(ConnectionState::Connecting))
        ));

        // Round-trip through the tunnel.
        stream.write_all(b"PING").await.unwrap();
        let mut got = [0u8; 4];
        stream.read_exact(&mut got).await.unwrap();
        assert_eq!(&got, b"PING");

        // The proxy saw the unresolved domain as the target (remote DNS).
        assert_eq!(target_rx.await.unwrap(), "server.invalid:5500");
    }

    #[cfg(feature = "socks")]
    #[tokio::test]
    async fn socks5_password_auth_connect() {
        let (proxy_addr, _target_rx) = spawn_mock_socks5(Some(("alice", "s3cr3t")));
        let proxy = ProxyConfig {
            scheme: ProxyScheme::Socks5,
            addr: proxy_addr.to_string(),
            auth: Some(("alice".to_owned(), "s3cr3t".to_owned())),
        };
        let (evt_tx, _evt_rx) = mpsc::channel(8);
        // Success requires the auth sub-negotiation with matching creds —
        // the mock asserts them and would otherwise never reply auth-OK.
        let result = resolve_and_connect("t.invalid", 80, Some(&proxy), &evt_tx).await;
        assert!(result.is_ok(), "authed proxy connect: {:?}", result.err());
    }
}
