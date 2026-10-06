//! The connection's lifecycle, from byte zero to the session: connect
//! (DNS, TCP, a SOCKS proxy), TLS on the paths that ask for it, then the
//! session ([`crate::session`]), which drives everything from the magic on
//! — HOPE's two steps and the transport they agree included.
//!
//! State events: Resolving → Connecting → Connected → (TlsHandshaking) →
//! MagicExchange → LoginSending → LoginReplyWait → HandshakeDone →
//! LoginReady.
//!
//! A step that fails does not start the session; the consumer gets an
//! `Event::Shutdown` with a `ShutdownReason::StreamError` saying which.

use tokio::sync::mpsc;

use crate::session::SharedSession;
use crate::{connect::resolve_and_connect, ConnectionState, Event, ShutdownReason};
use hxsession::{Handled, Session};

/// Optional TLS certificate-verify (TOFU) callback: given a fingerprint
/// string, returns whether to trust the peer.
pub type TlsVerifyFn = Option<Box<dyn Fn(&str) -> bool + Send>>;

/// Parameters for the plaintext lifecycle. Strings are passed by
/// owned `Vec<u8>` / `String` because the orchestrator runs as a
/// spawned task and can't hold caller borrows.
#[derive(Debug, Clone)]
pub struct PlaintextOpenRequest {
    pub host: String,
    pub port: u16,
    pub login: Vec<u8>,
    pub password: Vec<u8>,
    pub name: Vec<u8>,
    pub icon: u16,
    pub version: u16,
    /// Capability bitmask (`HTLC_CAP_*`) to advertise in the LOGIN.
    /// 0 omits the chunk; production passes the same bits the legacy
    /// LOGIN does so extensions negotiate.
    pub caps: u16,
    pub trans: u32,
    /// Optional SOCKS proxy to tunnel the connection through. `None`
    /// connects direct. Sourced in C from `GProxyResolver` and parsed by
    /// the FFI; see `connect::ProxyConfig`.
    pub proxy: Option<crate::connect::ProxyConfig>,
}

impl PlaintextOpenRequest {
    /// The session the lifecycle drives from the magic on, made before
    /// it starts so the consumer can number requests from it at once,
    /// acting itself on the domains `handled` names.
    pub fn session(&self, handled: Handled) -> SharedSession {
        let cfg = crate::session::config(
            &self.login,
            &self.password,
            &self.name,
            self.icon,
            self.version,
            self.caps,
            handled,
        );
        SharedSession::new(Session::new(cfg, 0).into())
    }
}

/// Drive the plaintext-Hotline lifecycle end-to-end. On success,
/// transitions into the actor loop and runs until the actor
/// exits. On failure, emits a synthetic `Event::Shutdown` and
/// returns.
///
/// The caller owns the `cmd_rx` + `evt_tx` channels (created via
/// [`Connection::make_channels`]) so the corresponding handle +
/// event receiver can be returned to the FFI caller BEFORE the
/// async task starts.
pub async fn run_plaintext_lifecycle(
    req: PlaintextOpenRequest,
    session: SharedSession,
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
) {
    run_tcp(
        &req.host,
        req.port,
        req.proxy.as_ref(),
        session,
        cmd_rx,
        evt_tx,
    )
    .await;
}

/// Connect, then run the session over the socket.
async fn run_tcp(
    host: &str,
    port: u16,
    proxy: Option<&crate::connect::ProxyConfig>,
    session: SharedSession,
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
) {
    // Phase A: DNS + TCP connect. resolve_and_connect emits
    // Resolving + Connecting itself.
    let stream = match resolve_and_connect(host, port, proxy, &evt_tx).await {
        Ok(s) => s,
        Err(e) => {
            let _ = evt_tx
                .send(Event::Shutdown(ShutdownReason::StreamError(format!(
                    "connect: {e}"
                ))))
                .await;
            return;
        }
    };

    // Connected event — caller knows the TCP three-way is done.
    if evt_tx
        .send(Event::State(ConnectionState::Connected))
        .await
        .is_err()
    {
        // Consumer dropped; nothing more to report to.
        return;
    }

    run_plaintext_over(stream, session, cmd_rx, evt_tx).await;
}

/// Like [`run_plaintext_lifecycle`] but wraps the connected socket in
/// TLS (the Mobius / Janus separate-port model: TLS-from-byte-zero on
/// a dedicated port, then the ordinary Hotline protocol over the
/// encrypted stream) before the magic exchange. The plaintext
/// lifecycle then runs over the TLS stream unchanged.
///
/// State events: Resolving → Connecting → Connected → TlsHandshaking
/// → MagicExchange → … . Certificate trust is WebPKI-first: a cert that
/// validates against the native roots is trusted silently; only a cert
/// that fails WebPKI is routed to the `verify` (TOFU) callback. See
/// [`crate::tls`].
pub async fn run_plaintext_tls_lifecycle(
    req: PlaintextOpenRequest,
    session: SharedSession,
    verify: TlsVerifyFn,
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
) {
    let tcp = match resolve_and_connect(&req.host, req.port, req.proxy.as_ref(), &evt_tx).await {
        Ok(s) => s,
        Err(e) => {
            let _ = evt_tx
                .send(Event::Shutdown(ShutdownReason::StreamError(format!(
                    "connect: {e}"
                ))))
                .await;
            return;
        }
    };

    if evt_tx
        .send(Event::State(ConnectionState::Connected))
        .await
        .is_err()
    {
        return;
    }
    if evt_tx
        .send(Event::State(ConnectionState::TlsHandshaking))
        .await
        .is_err()
    {
        return;
    }

    let (tls, webpki_ok) = match crate::tls::wrap_tls(tcp, &req.host).await {
        Ok(pair) => pair,
        Err(e) => {
            let _ = evt_tx
                .send(Event::Shutdown(ShutdownReason::StreamError(format!(
                    "tls handshake: {e}"
                ))))
                .await;
            return;
        }
    };

    // WebPKI first: if the server cert chained to a native trust root
    // and the hostname matched (e.g. a Let's Encrypt cert), it's
    // trusted silently — no prompt, no TOFU lookup, like a browser
    // hitting a CA-signed site. Only when WebPKI did NOT validate do we
    // fall back to the C-side TOFU callback, which looks the
    // fingerprint up in the known-hosts store and (on UNKNOWN /
    // MISMATCH) prompts the user. Running TOFU post-handshake — rather
    // than inside the rustls verifier — keeps the (potentially
    // blocking, main-thread-marshalled) decision off the handshake's
    // critical path; a reject just closes the stream before any LOGIN
    // bytes flow. `verify == None` (e.g. the live probe) skips TOFU.
    if !webpki_ok.load(std::sync::atomic::Ordering::Relaxed) {
        if let Some(verify) = verify.as_ref() {
            match crate::tls::peer_cert_fingerprint(&tls) {
                Some(fp) => {
                    if !verify(&fp) {
                        let _ = evt_tx
                            .send(Event::Shutdown(ShutdownReason::StreamError(
                                "tls certificate rejected by trust check".to_string(),
                            )))
                            .await;
                        return;
                    }
                }
                None => {
                    let _ = evt_tx
                        .send(Event::Shutdown(ShutdownReason::StreamError(
                            "tls peer presented no certificate".to_string(),
                        )))
                        .await;
                    return;
                }
            }
        }
    }

    run_plaintext_over(tls, session, cmd_rx, evt_tx).await;
}

/// The post-connect plaintext lifecycle, generic over the transport
/// so it runs identically over a raw TCP socket or a TLS stream: the
/// session ([`crate::session`]) from the magic on.
async fn run_plaintext_over<S>(
    stream: S,
    session: SharedSession,
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
) where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send + 'static,
{
    if evt_tx
        .send(Event::State(ConnectionState::MagicExchange))
        .await
        .is_err()
    {
        return;
    }
    crate::session::run(stream, session, cmd_rx, evt_tx).await;
}

/// Parameters for the HOPE-Secure-Login lifecycle: the plaintext
/// lifecycle's, and the cipher to ask for.
#[derive(Debug, Clone)]
pub struct HopeOpenRequest {
    pub host: String,
    pub port: u16,
    pub login: Vec<u8>,
    pub password: Vec<u8>,
    pub name: Vec<u8>,
    pub icon: u16,
    pub version: u16,
    pub caps: u16,
    /// The cipher to offer, or `None` for HMAC authentication over a
    /// plaintext transport (mhxd's non-`cipher_only` mode).
    pub cipher: Option<hxhope::Cipher>,
    /// The compression to offer, as the user picked it, or `None`. It is
    /// opt-in: under a cipher, compressed lengths say something of what
    /// was compressed. See docs/rust/networking.md, "Compression".
    pub compression: Option<hxhope::Compression>,
    /// Optional SOCKS proxy to tunnel through; `None` connects direct.
    /// See [`PlaintextOpenRequest::proxy`].
    pub proxy: Option<crate::connect::ProxyConfig>,
}

impl HopeOpenRequest {
    /// As [`PlaintextOpenRequest::session`], for a session that logs in
    /// with HOPE.
    pub fn session(&self, handled: Handled) -> SharedSession {
        let cfg = crate::session::config(
            &self.login,
            &self.password,
            &self.name,
            self.icon,
            self.version,
            self.caps,
            handled,
        );
        // Janus, taking LZ4 under Blowfish, reads nothing a client sends
        // until the connection closes (docs/janus-bugs.md), and a client
        // cannot tell Janus from another server before it asks: under
        // Blowfish, LZ4 is not offered, and the login runs uncompressed.
        let compression = self.compression.filter(|&c| {
            let withheld =
                c == hxhope::Compression::Lz4 && self.cipher == Some(hxhope::Cipher::Blowfish);
            if withheld {
                crate::proto_trace::note(
                    "HOPE: LZ4 not offered under Blowfish (a Janus bug, docs/janus-bugs.md)",
                );
            }
            !withheld
        });
        let offer = hxhope::client::Offer {
            ciphers: self.cipher.into_iter().collect(),
            compressions: compression.into_iter().collect(),
            app_string: Some(format!("hxnet {}", env!("CARGO_PKG_VERSION")).into_bytes()),
            ..hxhope::client::Offer::new(*b"GTKx")
        };
        SharedSession::new(Session::with_hope(cfg, offer, Box::new(random), 0).into())
    }
}

/// Where Blowfish's rekey markers go. Not key material: should the OS
/// have no randomness to give, a frame goes unmarked, which is safe.
fn random(buf: &mut [u8]) {
    if getrandom::fill(buf).is_err() {
        buf.fill(0);
    }
}

/// Drive the HOPE-Secure-Login lifecycle: connect, then the session from
/// the magic on, which runs HOPE's two steps and from step 2's reply on
/// everything through the transport they agree. HOPE-over-TLS is refused
/// before this runs, as redundant double encryption.
pub async fn run_hope_lifecycle(
    req: HopeOpenRequest,
    session: SharedSession,
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
) {
    run_tcp(
        &req.host,
        req.port,
        req.proxy.as_ref(),
        session,
        cmd_rx,
        evt_tx,
    )
    .await;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Command;
    use crate::Connection;
    use hxproto::build::{pack_message, pack_message_size, PackChunk};
    use hxsession::request::Request;
    use hxsession::{CLIENT_MAGIC as HTLC_MAGIC, SERVER_MAGIC as HTLS_MAGIC};
    use std::time::Duration;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpListener;

    /// Stand up a fake Hotline server on loopback that:
    ///   - reads HTLC_MAGIC, writes HTLS_MAGIC
    ///   - reads a LOGIN frame
    ///   - writes a TASK reply with flag=0 (success)
    /// Then drive the lifecycle against it and verify the state
    /// event sequence and the actor running to handshake done.
    #[tokio::test]
    async fn plaintext_lifecycle_happy_path() {
        let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
        let port = listener.local_addr().unwrap().port();

        let server = tokio::spawn(async move {
            let (mut s, _) = listener.accept().await.expect("accept");
            // Magic exchange — read client's magic, write ours.
            let mut buf = [0u8; 12];
            s.read_exact(&mut buf).await.expect("magic read");
            assert_eq!(&buf, HTLC_MAGIC);
            s.write_all(HTLS_MAGIC).await.expect("magic write");

            // Read LOGIN frame. 22-byte header then body of size
            // wire_len - 2.
            let mut hdr = [0u8; 22];
            s.read_exact(&mut hdr).await.expect("hdr read");
            let body_len = u32::from_be_bytes([hdr[12], hdr[13], hdr[14], hdr[15]]) - 2;
            let mut body = vec![0u8; body_len as usize];
            s.read_exact(&mut body).await.expect("body read");

            // Build TASK reply with flag=0. Empty body except for
            // the chunk count.
            let trans = u32::from_be_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]);
            let chunks: [PackChunk<'_>; 0] = [];
            let needed = pack_message_size(&chunks);
            let mut reply = vec![0u8; needed];
            pack_message(&mut reply, 0x0001_0000, trans, 0, &chunks).expect("pack reply");
            s.write_all(&reply).await.expect("reply write");

            // Hold the connection open briefly so the actor can
            // start. Then drop — the actor sees EOF and shuts
            // down cleanly.
            tokio::time::sleep(Duration::from_millis(50)).await;
        });

        let req = PlaintextOpenRequest {
            host: "127.0.0.1".into(),
            port,
            login: b"misha".to_vec(),
            password: b"".to_vec(),
            name: b"GtkHx".to_vec(),
            icon: 4012,
            version: 150,
            caps: 0,
            trans: 1,
            proxy: None,
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(
            req.clone(),
            req.session(Handled::NONE),
            cmd_rx,
            evt_tx,
        ));

        // Drain state events.
        let mut seen: Vec<ConnectionState> = Vec::new();
        let mut saw_handshake_done = false;
        let mut saw_shutdown = false;
        // The LOGIN reply, whole (the session handles no domain), and
        // whether it came before HandshakeDone.
        let mut login_frame_type: Option<u32> = None;
        let mut login_frame_flag: Option<u32> = None;
        let mut saw_login_frame_before_handshake = false;
        while let Some(evt) = tokio::time::timeout(Duration::from_secs(2), evt_rx.recv())
            .await
            .ok()
            .flatten()
        {
            match evt {
                Event::State(s) => {
                    if s == ConnectionState::HandshakeDone {
                        saw_handshake_done = true;
                    }
                    seen.push(s);
                }
                Event::Frame(f) => {
                    if !saw_handshake_done {
                        saw_login_frame_before_handshake = true;
                    }
                    login_frame_type = Some(f.header.type_);
                    login_frame_flag = Some(f.header.flag);
                }
                Event::Shutdown(_) => {
                    saw_shutdown = true;
                    break;
                }
                Event::Session(hxsession::Event::LoggedIn(_)) => {}
                Event::Session(e) => panic!("{e:?}"),
            }
        }
        lifecycle.await.expect("lifecycle task");
        server.await.expect("server task");

        // Verify the expected state ordering (subset — we don't
        // require LoginReplyWait specifically but the prefix must
        // be Resolving → Connecting → Connected → MagicExchange
        // → LoginSending).
        let expected_prefix = vec![
            ConnectionState::Resolving,
            ConnectionState::Connecting,
            ConnectionState::Connected,
            ConnectionState::MagicExchange,
            ConnectionState::LoginSending,
        ];
        assert!(
            seen.starts_with(&expected_prefix),
            "state event ordering mismatch: {seen:?}",
        );
        assert!(saw_handshake_done, "expected HandshakeDone, saw {seen:?}");
        assert!(saw_shutdown, "expected actor Shutdown after server drop");

        // A session handling no domain hands the LOGIN reply over whole
        // too: as an Event::Frame, with the TASK opcode and success
        // flag, before HandshakeDone.
        assert!(
            saw_login_frame_before_handshake,
            "expected LOGIN reply replayed as Event::Frame before HandshakeDone"
        );
        assert_eq!(
            login_frame_type,
            Some(0x0001_0000),
            "replayed frame should carry HTLS_HDR_TASK"
        );
        assert_eq!(
            login_frame_flag,
            Some(0),
            "replayed frame should carry the success flag"
        );
    }

    /// Server replies to LOGIN with flag=1 (failure) and an error
    /// text chunk. Lifecycle should NOT reach HandshakeDone;
    /// should emit Shutdown(StreamError) carrying the server's
    /// error text.
    #[tokio::test]
    async fn plaintext_lifecycle_login_rejected() {
        let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
        let port = listener.local_addr().unwrap().port();

        let server = tokio::spawn(async move {
            let (mut s, _) = listener.accept().await.expect("accept");
            let mut buf = [0u8; 12];
            s.read_exact(&mut buf).await.expect("magic read");
            s.write_all(HTLS_MAGIC).await.expect("magic write");

            let mut hdr = [0u8; 22];
            s.read_exact(&mut hdr).await.expect("hdr read");
            let body_len = u32::from_be_bytes([hdr[12], hdr[13], hdr[14], hdr[15]]) - 2;
            let mut body = vec![0u8; body_len as usize];
            s.read_exact(&mut body).await.expect("body read");

            let trans = u32::from_be_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]);
            let err_text = b"Login is incorrect.";
            let chunks = [PackChunk {
                tag: 0x0064, // field 100, the error text
                data: err_text,
            }];
            let needed = pack_message_size(&chunks);
            let mut reply = vec![0u8; needed];
            // flag = 1 = task failure.
            pack_message(&mut reply, 0x0001_0000, trans, 1, &chunks).expect("pack reply");
            s.write_all(&reply).await.expect("reply write");
        });

        let req = PlaintextOpenRequest {
            host: "127.0.0.1".into(),
            port,
            login: b"misha".to_vec(),
            password: b"wrong".to_vec(),
            name: b"GtkHx".to_vec(),
            icon: 0,
            version: 150,
            caps: 0,
            trans: 1,
            proxy: None,
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(
            req.clone(),
            req.session(Handled::NONE),
            cmd_rx,
            evt_tx,
        ));

        let mut shutdown_msg: Option<String> = None;
        let mut saw_handshake_done = false;
        while let Some(evt) = tokio::time::timeout(Duration::from_secs(2), evt_rx.recv())
            .await
            .ok()
            .flatten()
        {
            match evt {
                Event::State(ConnectionState::HandshakeDone) => {
                    saw_handshake_done = true;
                }
                Event::Shutdown(ShutdownReason::StreamError(m)) => {
                    shutdown_msg = Some(m);
                    break;
                }
                _ => {}
            }
        }
        lifecycle.await.expect("lifecycle");
        server.await.expect("server");

        assert!(
            !saw_handshake_done,
            "shouldn't reach HandshakeDone on login failure"
        );
        let msg = shutdown_msg.expect("expected Shutdown StreamError");
        assert!(
            msg.contains("Login is incorrect"),
            "shutdown msg should carry server error text, got: {msg}"
        );
    }

    /// The far end of a HOPE login, hxhope's server: the magic, step 1,
    /// step 2, the login reply and a chat through the transport they
    /// agreed, then whatever the client sends, decoded, until it hangs up.
    async fn hope_server(
        listener: TcpListener,
        compressions: Vec<hxhope::Compression>,
    ) -> Vec<(u32, u32)> {
        use hxsession::frame::FrameReader;
        let (mut s, _) = listener.accept().await.unwrap();
        let mut magic = [0u8; 12];
        s.read_exact(&mut magic).await.unwrap();
        assert_eq!(&magic, hxsession::CLIENT_MAGIC);
        s.write_all(hxsession::SERVER_MAGIC).await.unwrap();

        let policy = hxhope::server::Policy {
            macs: hxhope::Mac::ALL.to_vec(),
            ciphers: vec![hxhope::Cipher::Blowfish, hxhope::Cipher::ChaCha20Poly1305],
            compressions,
            require_cipher: false,
        };
        let mut reader = FrameReader::new();
        let mut transport: Option<hxhope::Transport> = None;
        let mut hs = None;
        let mut got = Vec::new();
        let mut buf = vec![0u8; 16 * 1024];
        loop {
            let n = s.read(&mut buf).await.unwrap();
            if n == 0 {
                return got;
            }
            match transport.as_mut() {
                Some(t) => {
                    let mut plain = Vec::new();
                    t.decode(&buf[..n], &mut plain).unwrap();
                    reader.push(&plain);
                }
                None => reader.push(&buf[..n]),
            }
            while let Some(t) = reader.next_transaction().unwrap() {
                match hs.take() {
                    None if transport.is_none() => {
                        let (h, reply) =
                            hxhope::server::answer(&policy, &t.buf, [9; 64], t.trans).unwrap();
                        s.write_all(&reply).await.unwrap();
                        hs = Some(h);
                    }
                    Some(h) => {
                        let step2 = h.step2(&t.buf).unwrap();
                        assert!(step2.names(&h, b"guest"));
                        let random = Box::new(|b: &mut [u8]| b.fill(0x20));
                        let (mut tr, _) = h.accept(&step2, b"pw", random).unwrap();
                        let mut out = Request::new(0x0001_0000).pack(t.trans).unwrap();
                        out.extend(Request::new(0x6a).field(101, *b"hi").pack(0).unwrap());
                        s.write_all(&tr.encode(&out).unwrap()).await.unwrap();
                        transport = Some(tr);
                    }
                    None => got.push((t.type_, t.trans)),
                }
            }
        }
    }

    /// Every transport HOPE agrees runs end to end: the step-2 reply
    /// reaches the consumer as the login's, a chat comes through, and
    /// what the consumer sends reaches the server whole.
    #[tokio::test]
    async fn the_hope_lifecycle_runs_every_transport() {
        use hxhope::{Cipher, Compression};
        for cipher in [None, Some(Cipher::Blowfish), Some(Cipher::ChaCha20Poly1305)] {
            for compression in [
                None,
                Some(Compression::Gzip),
                Some(Compression::Lz4),
                Some(Compression::Zstd),
            ] {
                let what = format!("{cipher:?} {compression:?}");
                let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
                let port = listener.local_addr().unwrap().port();
                let all = vec![Compression::Gzip, Compression::Lz4, Compression::Zstd];
                let server = tokio::spawn(hope_server(listener, all));
                let req = HopeOpenRequest {
                    host: "127.0.0.1".into(),
                    port,
                    login: b"guest".to_vec(),
                    password: b"pw".to_vec(),
                    name: b"me".to_vec(),
                    icon: 412,
                    version: hxsession::CLIENT_VERSION,
                    caps: 0,
                    cipher,
                    compression,
                    proxy: None,
                };
                let session = req.session(Handled::NONE);
                let login_trans = session.lock().unwrap().login_trans();
                assert_eq!(login_trans, 2, "{what}: step 2 on the trans after step 1's");
                let (handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
                tokio::spawn(run_hope_lifecycle(req, session.clone(), cmd_rx, evt_tx));
                let mut frames = Vec::new();
                while frames.len() < 2 {
                    match tokio::time::timeout(Duration::from_secs(5), evt_rx.recv())
                        .await
                        .expect("an event")
                        .expect("the actor")
                    {
                        Event::Frame(f) => frames.push((f.header.type_, f.header.trans)),
                        Event::Shutdown(why) => panic!("{what}: {why:?}"),
                        Event::State(_) => {}
                        Event::Session(hxsession::Event::LoggedIn(_)) => {}
                        Event::Session(e) => panic!("{what}: {e:?}"),
                    }
                }
                assert_eq!(frames, [(0x0001_0000, login_trans), (0x6a, 0)], "{what}");
                let agreed = session.lock().unwrap().negotiated().unwrap().compression;
                let asked = match (cipher, compression) {
                    (Some(Cipher::Blowfish), Some(Compression::Lz4)) => None,
                    _ => compression,
                };
                assert_eq!(
                    agreed, asked,
                    "{what}: what the user asked for, and only it"
                );
                let trans = session.lock().unwrap().take_trans();
                let frame = Request::new(300).pack(trans).unwrap();
                handle.send(Command::WriteFrame(frame)).await.unwrap();
                drop(handle);
                drop(evt_rx);
                let got = server.await.unwrap();
                assert!(got.contains(&(300, trans)), "{what}: {got:?}");
            }
        }
    }

    /// TEMPORARY live probe for run_plaintext_tls_lifecycle against a
    /// real separate-port-TLS server. Run with:
    ///   GTKHX_LIVE_PORT=5610 cargo test -p hxnet --lib \
    ///     live_plaintext_tls -- --ignored --nocapture
    /// (Janus TLS = 5610.)
    #[tokio::test]
    #[ignore]
    async fn live_plaintext_tls_login() {
        let port: u16 = std::env::var("GTKHX_LIVE_PORT")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(5610);
        let req = PlaintextOpenRequest {
            host: "127.0.0.1".into(),
            port,
            login: b"guest".to_vec(),
            password: b"".to_vec(),
            name: b"TlsProbe".to_vec(),
            icon: 0,
            version: crate::login::CLIENT_VERSION,
            caps: 0x001F,
            trans: 1,
            proxy: None,
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let verify: TlsVerifyFn = Some(Box::new(|fp: &str| {
            eprintln!("CERT fingerprint: {fp}");
            true
        }));
        let lc = tokio::spawn(run_plaintext_tls_lifecycle(
            req.clone(),
            req.session(Handled::NONE),
            verify,
            cmd_rx,
            evt_tx,
        ));
        let mut saw_hd = false;
        let mut saw_frame = false;
        while let Some(evt) = tokio::time::timeout(Duration::from_secs(8), evt_rx.recv())
            .await
            .ok()
            .flatten()
        {
            match &evt {
                Event::Frame(f) => {
                    eprintln!(
                        "EVT Frame type=0x{:x} trans={} flag={} body={}",
                        f.header.type_,
                        f.header.trans,
                        f.header.flag,
                        f.body.len()
                    );
                    saw_frame = true;
                }
                other => eprintln!("EVT {other:?}"),
            }
            match evt {
                Event::State(ConnectionState::HandshakeDone) => {
                    saw_hd = true;
                    break;
                }
                Event::Shutdown(_) => break,
                _ => {}
            }
        }
        drop(lc);
        assert!(saw_frame, "no replay frame from TLS server");
        assert!(saw_hd, "no HandshakeDone from TLS server");
    }

    /// Connect to a port nothing is listening on. Lifecycle
    /// should emit Shutdown(StreamError) with "connect:" prefix
    /// and never reach Connected.
    #[tokio::test]
    async fn plaintext_lifecycle_connect_refused() {
        let req = PlaintextOpenRequest {
            host: "127.0.0.1".into(),
            port: 1, // reserved tcpmux, never bound in CI
            login: b"x".to_vec(),
            password: b"".to_vec(),
            name: b"".to_vec(),
            icon: 0,
            version: 0,
            caps: 0,
            trans: 1,
            proxy: None,
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(
            req.clone(),
            req.session(Handled::NONE),
            cmd_rx,
            evt_tx,
        ));

        let mut saw_connected = false;
        let mut shutdown_msg: Option<String> = None;
        while let Some(evt) = tokio::time::timeout(Duration::from_secs(2), evt_rx.recv())
            .await
            .ok()
            .flatten()
        {
            match evt {
                Event::State(ConnectionState::Connected) => saw_connected = true,
                Event::Shutdown(ShutdownReason::StreamError(m)) => {
                    shutdown_msg = Some(m);
                    break;
                }
                _ => {}
            }
        }
        lifecycle.await.expect("lifecycle");

        assert!(!saw_connected, "shouldn't reach Connected on refused");
        let msg = shutdown_msg.expect("expected Shutdown");
        assert!(
            msg.starts_with("connect:"),
            "shutdown msg should start with 'connect:', got: {msg}"
        );
    }

    /// Verify that the ConnectionHandle returned alongside the
    /// channels is usable — sending a command before the actor
    /// is online queues; after handshake_done + actor start, the
    /// command surfaces. Use a Shutdown command which the actor
    /// honours by exiting cleanly.
    #[tokio::test]
    async fn plaintext_lifecycle_handle_queues_commands_pre_actor() {
        let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
        let port = listener.local_addr().unwrap().port();

        let server = tokio::spawn(async move {
            let (mut s, _) = listener.accept().await.expect("accept");
            let mut buf = [0u8; 12];
            s.read_exact(&mut buf).await.expect("magic read");
            s.write_all(HTLS_MAGIC).await.expect("magic write");

            let mut hdr = [0u8; 22];
            s.read_exact(&mut hdr).await.expect("hdr read");
            let body_len = u32::from_be_bytes([hdr[12], hdr[13], hdr[14], hdr[15]]) - 2;
            let mut body = vec![0u8; body_len as usize];
            s.read_exact(&mut body).await.expect("body read");

            let trans = u32::from_be_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]);
            let chunks: [PackChunk<'_>; 0] = [];
            let needed = pack_message_size(&chunks);
            let mut reply = vec![0u8; needed];
            pack_message(&mut reply, 0x0001_0000, trans, 0, &chunks).expect("pack reply");
            s.write_all(&reply).await.expect("reply write");

            // Keep open so the actor has a chance to drain the
            // pre-queued shutdown.
            tokio::time::sleep(Duration::from_millis(100)).await;
        });

        let req = PlaintextOpenRequest {
            host: "127.0.0.1".into(),
            port,
            login: b"misha".to_vec(),
            password: b"".to_vec(),
            name: b"GtkHx".to_vec(),
            icon: 0,
            version: 150,
            caps: 0,
            trans: 1,
            proxy: None,
        };
        let (handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();

        // Pre-queue a Shutdown command before the actor exists.
        // The command channel buffers it; once the actor takes
        // over post-handshake it'll see Shutdown immediately.
        handle
            .send(Command::Shutdown)
            .await
            .expect("queue shutdown");

        let lifecycle = tokio::spawn(run_plaintext_lifecycle(
            req.clone(),
            req.session(Handled::NONE),
            cmd_rx,
            evt_tx,
        ));

        let mut saw_handshake_done = false;
        let mut saw_shutdown = false;
        while let Some(evt) = tokio::time::timeout(Duration::from_secs(2), evt_rx.recv())
            .await
            .ok()
            .flatten()
        {
            match evt {
                Event::State(ConnectionState::HandshakeDone) => {
                    saw_handshake_done = true;
                }
                Event::Shutdown(_) => {
                    saw_shutdown = true;
                    break;
                }
                _ => {}
            }
        }
        lifecycle.await.expect("lifecycle");
        server.await.expect("server");

        assert!(saw_handshake_done);
        assert!(saw_shutdown);
    }
}
