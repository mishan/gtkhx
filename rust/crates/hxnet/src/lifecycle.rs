//! End-to-end Hotline connection lifecycle (Phase G-prelude of
//! `hxnet-owns-the-whole-lifecycle`).
//!
//! Stitches the per-phase modules into one async function that
//! walks the whole connection from byte zero to a running actor.
//!
//! Sequence (plaintext path, no TLS, no HOPE):
//!
//! 1. DNS + TCP connect — via [`crate::connect::resolve_and_connect`].
//!    Fires `Event::State(Resolving)` and `Event::State(Connecting)`.
//! 2. Connected state event.
//! 3. Magic exchange — [`crate::magic::run_magic_exchange`].
//!    Fires `Event::State(MagicExchange)`.
//! 4. LOGIN send — [`crate::login::send_login`].
//!    Fires `Event::State(LoginSending)`.
//! 5. LOGIN reply receive — [`crate::login_reply::recv_login_reply`].
//!    Fires `Event::State(LoginReplyWait)`.
//! 6. Verify reply success; on failure emit Shutdown(LoginFailed)
//!    via the actor's shutdown event before exiting.
//! 7. `Event::State(HandshakeDone)`.
//! 8. Hand the stream to [`crate::Connection::run_actor`] — the
//!    actor reads / writes plaintext Hotline frames from here on.
//!
//! # What's NOT in this module
//!
//! - **Phase B (TLS-from-byte-zero)** — folded in once Phase G
//!   lands its FFI surface; the orchestrator gets a `tls: bool`
//!   parameter and a TLS handshake step slots in between (1)
//!   and (3).
//! - **Phase F (HOPE)** — when the caller asks for HOPE, the
//!   LOGIN-send step becomes the HOPE step 1 send and a second
//!   round-trip (LOGIN reply 2 + step 2 send) lands between (5)
//!   and (7), then Phase F-2's key derivation wraps the
//!   transport before (8).
//!
//! Both extensions are mechanical layering on top of this
//! plaintext orchestrator; the open question for Phase G is the
//! FFI shape for credentials + tls flag + cipher prefs, not the
//! orchestrator's internals.
//!
//! # Lifecycle errors
//!
//! Every step that fails returns an `io::Error`. The orchestrator
//! does NOT spawn the actor in the error path — instead it emits
//! a synthetic `Event::Shutdown` so the consumer sees the
//! lifecycle close cleanly. The shutdown reason is mapped from
//! the error kind:
//!
//! - DNS / connect failure → `ShutdownReason::StreamError`
//! - Magic mismatch → `ShutdownReason::StreamError`
//! - LOGIN reply `flag != 0` → `ShutdownReason::StreamError` with
//!   the server's error_text in the inner message
//!
//! `LoginFailed` is a future variant; for Phase G-prelude every
//! lifecycle failure is `StreamError` with a descriptive message.

use tokio::sync::mpsc;

use crate::{
    connect::resolve_and_connect, login::send_login, login::LoginRequest,
    login_reply::recv_login_reply, magic::run_magic_exchange, Connection,
    ConnectionState, Event, ShutdownReason,
};

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
    pub trans: u32,
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
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
) {
    // Phase A: DNS + TCP connect. resolve_and_connect emits
    // Resolving + Connecting itself.
    let stream = match resolve_and_connect(&req.host, req.port, &evt_tx).await {
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

    // Wrap in a tokio buffer-mut wrapper isn't needed — TcpStream
    // already implements AsyncRead+AsyncWrite. We pin via
    // &mut binding within the function.
    let mut stream = stream;

    // Phase C: magic exchange.
    if let Err(e) = run_magic_exchange(&mut stream, &evt_tx).await {
        let _ = evt_tx
            .send(Event::Shutdown(ShutdownReason::StreamError(format!(
                "magic: {e}"
            ))))
            .await;
        return;
    }

    // Phase D: LOGIN send.
    let login_req = LoginRequest {
        login: &req.login,
        password: &req.password,
        name: &req.name,
        icon: req.icon,
        version: req.version,
        trans: req.trans,
    };
    if let Err(e) = send_login(&mut stream, &login_req, &evt_tx).await {
        let _ = evt_tx
            .send(Event::Shutdown(ShutdownReason::StreamError(format!(
                "login send: {e}"
            ))))
            .await;
        return;
    }

    // Phase E: LOGIN reply receive.
    let reply = match recv_login_reply(&mut stream, &evt_tx).await {
        Ok(r) => r,
        Err(e) => {
            let _ = evt_tx
                .send(Event::Shutdown(ShutdownReason::StreamError(format!(
                    "login reply: {e}"
                ))))
                .await;
            return;
        }
    };

    if !reply.is_success() {
        let err_text = reply
            .error_text
            .as_ref()
            .map(|b| String::from_utf8_lossy(b).into_owned())
            .unwrap_or_else(|| format!("server flag={}", reply.flag));
        let _ = evt_tx
            .send(Event::Shutdown(ShutdownReason::StreamError(format!(
                "login rejected: {err_text}"
            ))))
            .await;
        return;
    }

    // HandshakeDone — the actor takes over from here.
    if evt_tx
        .send(Event::State(ConnectionState::HandshakeDone))
        .await
        .is_err()
    {
        return;
    }

    // Phase R3.3.a actor: read/write plaintext Hotline frames.
    Connection::run_actor(stream, cmd_rx, evt_tx).await;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::magic::{HTLC_MAGIC, HTLS_MAGIC};
    use crate::Command;
    use hotline_proto::build::{pack_message, pack_message_size, PackChunk};
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
            pack_message(&mut reply, 0x0001_0000, trans, 0, &chunks)
                .expect("pack reply");
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
            trans: 1,
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(req, cmd_rx, evt_tx));

        // Drain state events.
        let mut seen: Vec<ConnectionState> = Vec::new();
        let mut saw_handshake_done = false;
        let mut saw_shutdown = false;
        while let Some(evt) =
            tokio::time::timeout(Duration::from_secs(2), evt_rx.recv())
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
                Event::Shutdown(_) => {
                    saw_shutdown = true;
                    break;
                }
                _ => {}
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
        assert!(
            saw_shutdown,
            "expected actor Shutdown after server drop"
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
                tag: 0x0100, // TAG_ERROR_TEXT
                data: err_text,
            }];
            let needed = pack_message_size(&chunks);
            let mut reply = vec![0u8; needed];
            // flag = 1 = task failure.
            pack_message(&mut reply, 0x0001_0000, trans, 1, &chunks)
                .expect("pack reply");
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
            trans: 1,
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(req, cmd_rx, evt_tx));

        let mut shutdown_msg: Option<String> = None;
        let mut saw_handshake_done = false;
        while let Some(evt) =
            tokio::time::timeout(Duration::from_secs(2), evt_rx.recv())
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
            trans: 1,
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(req, cmd_rx, evt_tx));

        let mut saw_connected = false;
        let mut shutdown_msg: Option<String> = None;
        while let Some(evt) =
            tokio::time::timeout(Duration::from_secs(2), evt_rx.recv())
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
            pack_message(&mut reply, 0x0001_0000, trans, 0, &chunks)
                .expect("pack reply");
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
            trans: 1,
        };
        let (handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();

        // Pre-queue a Shutdown command before the actor exists.
        // The command channel buffers it; once the actor takes
        // over post-handshake it'll see Shutdown immediately.
        handle.send(Command::Shutdown).await.expect("queue shutdown");

        let lifecycle = tokio::spawn(run_plaintext_lifecycle(req, cmd_rx, evt_tx));

        let mut saw_handshake_done = false;
        let mut saw_shutdown = false;
        while let Some(evt) =
            tokio::time::timeout(Duration::from_secs(2), evt_rx.recv())
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
