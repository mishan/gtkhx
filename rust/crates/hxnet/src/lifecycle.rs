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

use tokio::io::AsyncWriteExt;
use tokio::sync::mpsc;

use crate::hope::{
    build_step1_login, build_step2_login, select_algorithms, HopeStep1Request, HopeStep2Request,
};
use crate::hope_blowfish::HopeMacAlg;
use crate::hope_keys::{compute_blowfish_chain, derive_aead_keys, HopeCipherKind};
use crate::transform::{compose, CipherLayer, CompressionKind};
use crate::{
    connect::resolve_and_connect, login::send_login, login::LoginRequest,
    login_reply::recv_login_reply, magic::run_magic_exchange, Connection, ConnectionState, Event,
    Frame, ShutdownReason,
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
    /// Capability bitmask (`HTLC_CAP_*`) to advertise in the LOGIN.
    /// 0 omits the chunk; production passes the same bits the legacy
    /// LOGIN does so extensions negotiate.
    pub caps: u16,
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

    run_plaintext_over(stream, &req, cmd_rx, evt_tx).await;
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
    verify: Option<Box<dyn Fn(&str) -> bool + Send>>,
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
) {
    let tcp = match resolve_and_connect(&req.host, req.port, &evt_tx).await {
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

    run_plaintext_over(tls, &req, cmd_rx, evt_tx).await;
}

/// The post-connect plaintext lifecycle, generic over the transport
/// so it runs identically over a raw TCP socket or a TLS stream:
/// magic → LOGIN → reply → Option-B replay → HandshakeDone → actor.
async fn run_plaintext_over<S>(
    mut stream: S,
    req: &PlaintextOpenRequest,
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
) where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send + 'static,
{
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
        caps: req.caps,
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
    // tolerate_pre_task = true: plaintext servers (RetroMac, MacDomain)
    // may send USER_SELFINFO / AGREEMENT before the TASK login reply;
    // replay those and keep waiting for TASK, like the legacy rcv loop.
    let reply = match recv_login_reply(&mut stream, &evt_tx, true).await {
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

    // Phase G (Option B): replay the LOGIN reply to the consumer as a
    // synthetic frame, BEFORE HandshakeDone — see the plaintext-path
    // rationale in docs/rust/phase-g-migration.md "Option B".
    match Frame::from_raw(&reply.raw_frame) {
        Some(frame) => {
            if evt_tx.send(Event::Frame(frame)).await.is_err() {
                return;
            }
        }
        None => {
            let _ = evt_tx
                .send(Event::Shutdown(ShutdownReason::StreamError(
                    "login reply raw_frame failed to decode for replay".to_string(),
                )))
                .await;
            return;
        }
    }

    if evt_tx
        .send(Event::State(ConnectionState::HandshakeDone))
        .await
        .is_err()
    {
        return;
    }

    Connection::run_actor(stream, cmd_rx, evt_tx).await;
}

/// Parameters for the HOPE-Secure-Login lifecycle. Adds the cipher
/// preference list to [`PlaintextOpenRequest`]'s fields; the MAC
/// preference list is fixed (SHA256 → SHA1 → MD5, the spec order)
/// and compression is not advertised in this first cut (the C side's
/// compression negotiation is a follow-up — omitting it means the
/// server simply doesn't compress).
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
    pub trans: u32,
    /// Cipher preference list, strongest-first wire labels (e.g.
    /// `[b"CHACHA20-POLY1305", b"BLOWFISH"]`). The server picks one
    /// and echoes it in the step-1 reply.
    pub cipher_algs: Vec<Vec<u8>>,
}

/// Map a wire MAC-algorithm label onto the rekey enum the
/// HopeBlowfish adapter uses.
fn mac_label_to_hopemacalg(label: &[u8]) -> Option<HopeMacAlg> {
    match label {
        b"HMAC-SHA256" => Some(HopeMacAlg::Sha256),
        b"HMAC-SHA1" => Some(HopeMacAlg::Sha1),
        b"HMAC-MD5" => Some(HopeMacAlg::Md5),
        _ => None,
    }
}

/// Drive the HOPE-Secure-Login lifecycle end-to-end. Mirrors the
/// legacy C handshake in `rcv.c::rcv_task_login` (the `if (pass)`
/// branch) but in Rust, so the orchestrator owns the whole secure
/// connect:
///
/// 1. DNS + TCP connect, magic exchange (plaintext, raw socket).
/// 2. Step-1 LOGIN (empty creds + algorithm lists) → step-1 reply
///    (server sessionkey + chosen MAC / cipher). Both plaintext.
/// 3. Derive the HMAC chain + per-direction keys from the
///    sessionkey + chosen MAC.
/// 4. Step-2 LOGIN (real login + HMAC'd password) — sent **plaintext
///    on the raw socket**, exactly like the C side which calls
///    `hlwrite_chunks` before `cipher_*_init` (rcv.c L1965 vs
///    L2014). Encryption begins *after* this send.
/// 5. Wrap the transport in the negotiated cipher adapter
///    (`compose`); the step-2 reply and everything after is read /
///    written through it.
/// 6. Replay the (decrypted) step-2 reply to the consumer as an
///    `Event::Frame` before `HandshakeDone` — same Option B shape as
///    the plaintext path — then hand the wrapped transport to the
///    actor.
/// Control-channel HOPE AEAD material, retained so an HTXF subchannel
/// can derive its per-transfer keys in-process without the session key
/// ever crossing the FFI back to C. Populated by [`run_hope_lifecycle`]
/// just before the cipher transition when ChaCha20-Poly1305 is
/// negotiated; left `None` for plaintext / Blowfish / no-cipher.
#[derive(Clone)]
pub struct HopeAeadMaterial {
    pub session_key: Vec<u8>,
    /// client -> server control-channel AEAD state.
    pub ctrl_encode: hxcrypto_aead::AeadState,
    /// server -> client control-channel AEAD state.
    pub ctrl_decode: hxcrypto_aead::AeadState,
}

/// Shared slot the HOPE lifecycle writes once (before the cipher layer
/// is consumed) and the FFI getter reads after login.
pub type HopeAeadSlot = std::sync::Arc<std::sync::Mutex<Option<HopeAeadMaterial>>>;

pub async fn run_hope_lifecycle(
    req: HopeOpenRequest,
    cmd_rx: mpsc::Receiver<crate::Command>,
    evt_tx: mpsc::Sender<Event>,
    hope_slot: HopeAeadSlot,
) {
    macro_rules! bail {
        ($($arg:tt)*) => {{
            let _ = evt_tx
                .send(Event::Shutdown(ShutdownReason::StreamError(format!(
                    $($arg)*
                ))))
                .await;
            return;
        }};
    }

    let mut stream = match resolve_and_connect(&req.host, req.port, &evt_tx).await {
        Ok(s) => s,
        Err(e) => bail!("connect: {e}"),
    };
    if evt_tx
        .send(Event::State(ConnectionState::Connected))
        .await
        .is_err()
    {
        return;
    }

    if let Err(e) = run_magic_exchange(&mut stream, &evt_tx).await {
        bail!("magic: {e}");
    }

    // Magic done, about to send credentials. The plaintext path emits
    // this from send_login; HOPE builds its step frames directly, so
    // emit it here too. The C bridge maps LoginSending to the coarse
    // "transport ready / entering login phase" UI transition (delete
    // the Connecting task, register the login task) — matching the
    // legacy connect path's HANDSHAKE_DONE timing. Emitted once, before
    // the step-1 send, so it precedes the replayed step-2 reply frame.
    if evt_tx
        .send(Event::State(ConnectionState::LoginSending))
        .await
        .is_err()
    {
        return;
    }

    // ---- HOPE step 1 (plaintext) ----
    let mac_algs: [&[u8]; 3] = [b"HMAC-SHA256", b"HMAC-SHA1", b"HMAC-MD5"];
    let cipher_refs: Vec<&[u8]> = req.cipher_algs.iter().map(|v| v.as_slice()).collect();
    let app_string = format!("hxnet {}", env!("CARGO_PKG_VERSION"));
    let step1 = match build_step1_login(&HopeStep1Request {
        trans: req.trans,
        mac_algs: &mac_algs,
        cipher_algs: &cipher_refs,
        compress_algs: &[],
        app_id: None,
        app_string: Some(app_string.as_bytes()),
    }) {
        Ok(f) => f,
        Err(e) => bail!("hope step1 build: {e}"),
    };
    if evt_tx
        .send(Event::State(ConnectionState::HopeStep1))
        .await
        .is_err()
    {
        return;
    }
    if let Err(e) = stream.write_all(&step1).await {
        bail!("hope step1 send: {e}");
    }
    if let Err(e) = stream.flush().await {
        bail!("hope step1 flush: {e}");
    }

    // HOPE handshake is tight — the step-1 reply is the next frame, no
    // pre-TASK session pushes; keep it strict (tolerate_pre_task=false).
    let step1_reply = match recv_login_reply(&mut stream, &evt_tx, false).await {
        Ok(r) => r,
        Err(e) => bail!("hope step1 reply: {e}"),
    };
    if !step1_reply.is_success() {
        let txt = step1_reply
            .error_text
            .as_ref()
            .map(|b| String::from_utf8_lossy(b).into_owned())
            .unwrap_or_else(|| format!("server flag={}", step1_reply.flag));
        bail!("hope step1 rejected: {txt}");
    }
    let choice = match select_algorithms(&step1_reply) {
        Some(c) => c,
        None => bail!("hope step1 reply missing sessionkey / mac / cipher"),
    };

    // secure_login probe: the server signals the HMAC-login variant
    // by echoing the *chosen MAC algorithm name* in the step-1
    // reply's DATA_LOGIN chunk (e.g. mhxd echoes "HMAC-SHA1"). When
    // it matches, the step-2 LOGIN field must be HMAC(login,
    // sessionkey) rather than XOR. Mirrors
    // src/hope.c::hope_parse_step1_reply L163-168 (memcmp of the
    // login echo against reply->macalg). mhxd is secure_login; Janus
    // guest is not (it echoes no login).
    let secure_login = step1_reply
        .login_echo
        .as_deref()
        .is_some_and(|echo| echo == choice.mac_alg.as_slice());

    // ---- derive keys (mirrors hope_store_chain_keys + the AEAD /
    // Blowfish key derivation in rcv.c) ----
    let (password_mac, bfkeys) =
        match compute_blowfish_chain(&req.password, &choice.sessionkey, &choice.mac_alg) {
            Ok(t) => t,
            Err(e) => bail!("hope key derivation: {e}"),
        };

    // ---- HOPE step 2 (plaintext, raw socket) ----
    let step2 = match build_step2_login(&HopeStep2Request {
        trans: req.trans.wrapping_add(1),
        login: &req.login,
        password_mac: &password_mac,
        choice: &choice,
        name: &req.name,
        icon: req.icon,
        version: req.version,
        caps: req.caps,
        secure_login,
    }) {
        Ok(f) => f,
        Err(e) => bail!("hope step2 build: {e}"),
    };
    if evt_tx
        .send(Event::State(ConnectionState::HopeStep2))
        .await
        .is_err()
    {
        return;
    }
    if let Err(e) = stream.write_all(&step2).await {
        bail!("hope step2 send: {e}");
    }
    if let Err(e) = stream.flush().await {
        bail!("hope step2 flush: {e}");
    }

    // ---- build the negotiated cipher layer; encryption starts here
    // (everything after the step-2 send is ciphered) ----
    let mut hope_material: Option<HopeAeadMaterial> = None;
    let cipher_layer = match HopeCipherKind::from_label(&choice.cipher_alg) {
        Some(HopeCipherKind::Blowfish) => {
            let read_state = match hxcrypto_stream::BlowfishOfb64State::new(&bfkeys.decode_key) {
                Some(s) => s,
                None => bail!("blowfish read state init failed"),
            };
            let write_state = match hxcrypto_stream::BlowfishOfb64State::new(&bfkeys.encode_key) {
                Some(s) => s,
                None => bail!("blowfish write state init failed"),
            };
            let macalg = match mac_label_to_hopemacalg(&choice.mac_alg) {
                Some(m) => m,
                None => bail!(
                    "unknown MAC alg {:?} for HOPE-Blowfish rekey",
                    String::from_utf8_lossy(&choice.mac_alg)
                ),
            };
            CipherLayer::HopeBlowfish {
                read_state,
                read_key: bfkeys.decode_key.clone(),
                write_state,
                write_key: bfkeys.encode_key.clone(),
                session_key: choice.sessionkey.clone(),
                macalg,
            }
        }
        Some(HopeCipherKind::ChaCha20Poly1305) => {
            // derive_aead_keys takes (sessionkey, spec_encode_key,
            // spec_decode_key). After compute_blowfish_chain's
            // storage flip, bfkeys.decode_key holds spec_encode and
            // bfkeys.encode_key holds spec_decode — the same
            // (decode_key, encode_key) argument order the C side
            // passes to cipher_aead_derive_session_keys.
            let aead = derive_aead_keys(&choice.sessionkey, &bfkeys.decode_key, &bfkeys.encode_key);
            // server -> client
            let read = hxcrypto_aead::AeadState {
                key: aead.decode_key,
                counter: 0,
                dir: hxcrypto_aead::AEAD_DIR_SERVER_TO_CLIENT,
            };
            // client -> server
            let write = hxcrypto_aead::AeadState {
                key: aead.encode_key,
                counter: 0,
                dir: hxcrypto_aead::AEAD_DIR_CLIENT_TO_SERVER,
            };
            // Retain the control-channel AEAD material so an HTXF
            // subchannel can derive its per-transfer keys in-process,
            // without the session key ever crossing the FFI back to C.
            // ctrl_encode = client->server (write); ctrl_decode =
            // server->client (read). AeadState is Copy, so the same
            // values seed the live cipher layer below.
            hope_material = Some(HopeAeadMaterial {
                session_key: choice.sessionkey.clone(),
                ctrl_encode: write,
                ctrl_decode: read,
            });
            CipherLayer::ChaCha20Poly1305 { read, write }
        }
        // No cipher negotiated (empty cipher_alg): the server ran the
        // secure-login MAC authentication but selected no transport
        // cipher (mhxd's non-cipher_only mode). Everything after step 2
        // stays plaintext — CipherLayer::None is the passthrough.
        None if choice.cipher_alg.is_empty() => CipherLayer::None,
        None => bail!(
            "server chose unsupported cipher {:?}",
            String::from_utf8_lossy(&choice.cipher_alg)
        ),
    };

    // Publish the retained AEAD material (if any) so a later HTXF
    // subchannel can derive transfer keys off it. Written before the
    // cipher layer is consumed by compose(); the FFI getter reads it
    // after login, long after this point.
    if let Some(m) = hope_material {
        if let Ok(mut slot) = hope_slot.lock() {
            *slot = Some(m);
        }
    }

    if evt_tx
        .send(Event::State(ConnectionState::CipherTransition))
        .await
        .is_err()
    {
        return;
    }
    let mut wrapped = match compose(stream, cipher_layer, CompressionKind::None) {
        Ok(w) => w,
        Err(e) => bail!("cipher transport compose: {e}"),
    };

    // ---- step-2 reply, read THROUGH the cipher (encrypted) ----
    // HOPE step-2 reply is the next frame over the now-encrypted
    // transport; keep it strict (tolerate_pre_task=false).
    let step2_reply = match recv_login_reply(&mut wrapped, &evt_tx, false).await {
        Ok(r) => r,
        Err(e) => bail!("hope step2 reply: {e}"),
    };
    if !step2_reply.is_success() {
        let txt = step2_reply
            .error_text
            .as_ref()
            .map(|b| String::from_utf8_lossy(b).into_owned())
            .unwrap_or_else(|| format!("server flag={}", step2_reply.flag));
        bail!("hope login rejected: {txt}");
    }

    // Option B replay: hand the decrypted step-2 reply back to the C
    // side as a synthetic frame before HandshakeDone.
    match Frame::from_raw(&step2_reply.raw_frame) {
        Some(frame) => {
            if evt_tx.send(Event::Frame(frame)).await.is_err() {
                return;
            }
        }
        None => bail!("hope step2 reply raw_frame failed to decode for replay"),
    }

    if evt_tx
        .send(Event::State(ConnectionState::HandshakeDone))
        .await
        .is_err()
    {
        return;
    }

    Connection::run_actor(wrapped, cmd_rx, evt_tx).await;
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
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(req, cmd_rx, evt_tx));

        // Drain state events.
        let mut seen: Vec<ConnectionState> = Vec::new();
        let mut saw_handshake_done = false;
        let mut saw_shutdown = false;
        // Phase G: the LOGIN reply is replayed as Event::Frame before
        // HandshakeDone. Capture it + its ordering relative to the
        // HandshakeDone state event.
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

        // Phase G replay assertions: the LOGIN reply came back as an
        // Event::Frame, it carried the TASK opcode + success flag,
        // and it arrived before HandshakeDone (so the C side's
        // rcv_task_login runs before the connection is declared up).
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
                tag: 0x0100, // TAG_ERROR_TEXT
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
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(req, cmd_rx, evt_tx));

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

    /// TEMPORARY live probe for run_hope_lifecycle against a real
    /// HOPE server. Run with:
    ///   GTKHX_LIVE_PORT=5510 GTKHX_LIVE_CIPHER=CHACHA20-POLY1305 \
    ///     cargo test -p hxnet --lib live_hope -- --ignored --nocapture
    /// (Janus = 5510 ChaCha20; mhxd = 5500 BLOWFISH.)
    #[tokio::test]
    #[ignore]
    async fn live_hope_login() {
        let port: u16 = std::env::var("GTKHX_LIVE_PORT")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(5510);
        let cipher =
            std::env::var("GTKHX_LIVE_CIPHER").unwrap_or_else(|_| "CHACHA20-POLY1305".into());
        eprintln!("live_hope: port={port} cipher={cipher}");
        let req = HopeOpenRequest {
            host: "127.0.0.1".into(),
            port,
            login: b"guest".to_vec(),
            password: b"".to_vec(),
            name: b"HopeProbe".to_vec(),
            icon: 412,
            version: 185,
            caps: 0x001F,
            trans: 1,
            cipher_algs: vec![cipher.into_bytes()],
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let hope_slot: HopeAeadSlot = std::sync::Arc::new(std::sync::Mutex::new(None));
        let lc = tokio::spawn(run_hope_lifecycle(req, cmd_rx, evt_tx, hope_slot));
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
        assert!(saw_frame, "no replay frame from server");
        assert!(saw_hd, "no HandshakeDone from server");
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
            version: 185,
            caps: 0x001F,
            trans: 1,
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let verify: Option<Box<dyn Fn(&str) -> bool + Send>> = Some(Box::new(|fp: &str| {
            eprintln!("CERT fingerprint: {fp}");
            true
        }));
        let lc = tokio::spawn(run_plaintext_tls_lifecycle(req, verify, cmd_rx, evt_tx));
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
        };
        let (_handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();
        let lifecycle = tokio::spawn(run_plaintext_lifecycle(req, cmd_rx, evt_tx));

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
        };
        let (handle, mut evt_rx, cmd_rx, evt_tx) = Connection::make_channels();

        // Pre-queue a Shutdown command before the actor exists.
        // The command channel buffers it; once the actor takes
        // over post-handshake it'll see Shutdown immediately.
        handle
            .send(Command::Shutdown)
            .await
            .expect("queue shutdown");

        let lifecycle = tokio::spawn(run_plaintext_lifecycle(req, cmd_rx, evt_tx));

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
