//! Loopback benchmarks for the connection pipeline (docs/performance.md,
//! Tier 2).
//!
//! ```sh
//! cd rust && cargo bench -p hxnet --bench loopback
//! cargo bench -p hxnet --bench loopback -- aead     # one transport
//! ```
//!
//! Each transport — plain, TLS, HOPE with Blowfish, HOPE with
//! ChaCha20-Poly1305 — connects through the production entry point
//! (`hxnet_connection_open_*`) to a fake server on 127.0.0.1, so a frame
//! takes the path a server's does: socket read, TLS or the HOPE cipher,
//! framing, the actor's event channel, the ferry to the GLib main loop,
//! and the C event callback. The callback here only reads the frame and
//! frees it; what `rcv.c` does next — dispatch, the handler, the session
//! signal — is in the binary, not this crate, and isn't measured.
//!
//! Three numbers per transport:
//!
//! - **connect**: from the open call to the handshake-done state, on a
//!   warm loopback.
//! - **throughput**: the server writes a burst of small chat frames as
//!   fast as it can; frames a second from the first to the last reaching
//!   the callback, and the CPU each thread spent per frame — the main
//!   thread (the ferry and the callback), the client's runtime threads
//!   (read, decrypt, framing), and the server.
//! - **latency**: one frame at a time, the next sent only once the last
//!   reached the callback; the time from just before the server's write —
//!   so its encryption is included — to the callback.
//!
//! Known values: every frame arrives, in order, with its bytes intact;
//! and the latency, p50 and p99, is held against a raw loopback floor —
//! the same server writing unencrypted to a plain socket read on a thread
//! of its own, which no transport can beat.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::net::TcpListener as StdListener;
use std::sync::Arc;
use std::time::{Duration, Instant};

use hxnet::ffi::{
    hxnet_connection_destroy, hxnet_connection_open_hope, hxnet_connection_open_plaintext,
    hxnet_connection_open_plaintext_tls, hxnet_frame_free, HxnetConnection, HxnetFrame,
};
use hxnet::hope::encode_alg_list;
use hxnet::hope_blowfish::HopeMacAlg;
use hxnet::hope_keys::{compute_blowfish_chain, derive_aead_keys};
use hxnet::magic::{HTLC_MAGIC, HTLS_MAGIC};
use hxnet::transform::{compose, BoxedDuplex, CipherLayer, CompressionKind};
use hxnet::ConnectionState;
use hxproto::build::{pack_message, pack_message_size, PackChunk};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio_rustls::rustls;

/// Frames in a throughput burst.
const BURST: u32 = 100_000;
/// The fake server's write buffer.
const SERVER_BUFFER: usize = 64 * 1024;
/// Frames timed one at a time.
const PINGS: u32 = 2_000;
/// Runs of each measurement; the report gives the median.
const RUNS: usize = 5;
/// The chat opcode the burst is made of, and its text: a typical line.
const CHAT: u32 = 106;
const TEXT: &[u8] = b"a line of chat about the length of a typical one, give or take";
/// The chunk carrying a frame's sequence number, ahead of the text.
const TAG_SEQ: u16 = 0x7f01;
const TAG_TEXT: u16 = 101;
const PASSWORD: &[u8] = b"bench";
const LOGIN: &[u8] = b"bench";

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Transport {
    Plain,
    Tls,
    HopeBlowfish,
    HopeAead,
}

impl Transport {
    const ALL: [Transport; 4] = [
        Transport::Plain,
        Transport::Tls,
        Transport::HopeBlowfish,
        Transport::HopeAead,
    ];

    fn name(self) -> &'static str {
        match self {
            Transport::Plain => "plain",
            Transport::Tls => "tls",
            Transport::HopeBlowfish => "hope-blowfish",
            Transport::HopeAead => "hope-aead",
        }
    }

    fn cipher_label(self) -> &'static [u8] {
        match self {
            Transport::HopeBlowfish => b"BLOWFISH",
            Transport::HopeAead => b"CHACHA20-POLY1305",
            _ => b"",
        }
    }
}

/// A chat frame carrying sequence number `seq`.
fn chat_frame(seq: u32) -> Vec<u8> {
    let seq_bytes = seq.to_be_bytes();
    let chunks = [
        PackChunk {
            tag: TAG_SEQ,
            data: &seq_bytes,
        },
        PackChunk {
            tag: TAG_TEXT,
            data: TEXT,
        },
    ];
    let mut out = vec![0u8; pack_message_size(&chunks)];
    pack_message(&mut out, CHAT, 0, 0, &chunks).expect("pack a chat frame");
    out
}

/// A TASK reply to `trans` with `chunks`.
fn task_reply(trans: u32, chunks: &[PackChunk<'_>]) -> Vec<u8> {
    let mut out = vec![0u8; pack_message_size(chunks)];
    pack_message(&mut out, 0x0001_0000, trans, 0, chunks).expect("pack a reply");
    out
}

/// Read one frame: its transaction id and body.
async fn read_frame<S: AsyncRead + Unpin>(s: &mut S) -> std::io::Result<(u32, Vec<u8>)> {
    let mut hdr = [0u8; 22];
    s.read_exact(&mut hdr).await?;
    let trans = u32::from_be_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]);
    let body_len = u32::from_be_bytes([hdr[16], hdr[17], hdr[18], hdr[19]]).saturating_sub(2);
    let mut body = vec![0u8; body_len as usize];
    s.read_exact(&mut body).await?;
    Ok((trans, body))
}

// ---- the fake server ---------------------------------------------------

/// What the server does once the client is logged in.
enum Script {
    /// Write `BURST` frames back to back, once `go` fires.
    Burst {
        go: tokio::sync::oneshot::Receiver<()>,
    },
    /// Write `PINGS` frames, each after the client says it has the last;
    /// the send times go back over `sent`.
    Pings {
        acks: tokio::sync::mpsc::UnboundedReceiver<()>,
        sent: std::sync::mpsc::Sender<Instant>,
    },
}

fn tls_acceptor() -> tokio_rustls::TlsAcceptor {
    let cert =
        rustls::pki_types::CertificateDer::from(include_bytes!("loopback-cert.der").to_vec());
    let key =
        rustls::pki_types::PrivateKeyDer::Pkcs8(include_bytes!("loopback-key.der").to_vec().into());
    let config = rustls::ServerConfig::builder_with_provider(Arc::new(
        rustls::crypto::ring::default_provider(),
    ))
    .with_safe_default_protocol_versions()
    .expect("TLS versions")
    .with_no_client_auth()
    .with_single_cert(vec![cert], key)
    .expect("the benchmark's certificate");
    tokio_rustls::TlsAcceptor::from(Arc::new(config))
}

/// Magic, then the login the transport calls for. Returns the stream the
/// session runs over, ciphered if HOPE chose a cipher.
async fn handshake<S>(mut s: S, transport: Transport) -> BoxedDuplex
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let mut magic = [0u8; 12];
    s.read_exact(&mut magic).await.expect("client magic");
    assert_eq!(&magic, HTLC_MAGIC);
    s.write_all(HTLS_MAGIC).await.expect("server magic");
    s.flush().await.expect("flush");

    match transport {
        Transport::Plain | Transport::Tls => {
            let (trans, _) = read_frame(&mut s).await.expect("LOGIN");
            s.write_all(&task_reply(trans, &[]))
                .await
                .expect("LOGIN reply");
            s.flush().await.expect("flush");
            compose(s, CipherLayer::None, CompressionKind::None).expect("compose")
        }
        Transport::HopeBlowfish | Transport::HopeAead => {
            // Step 1: offer the one cipher the transport is about.
            let (trans, _) = read_frame(&mut s).await.expect("HOPE step 1");
            let sessionkey: Vec<u8> = (0u8..64).collect();
            let mac = encode_alg_list(&[b"HMAC-SHA256"]).expect("MAC list");
            let cipher = encode_alg_list(&[transport.cipher_label()]).expect("cipher list");
            let reply = task_reply(
                trans,
                &[
                    PackChunk {
                        tag: hxnet::login_reply::TAG_SESSIONKEY,
                        data: &sessionkey,
                    },
                    PackChunk {
                        tag: hxnet::login_reply::TAG_MAC_ALG,
                        data: &mac,
                    },
                    PackChunk {
                        tag: hxnet::login_reply::TAG_S_DATA_CIPHER_ALG,
                        data: &cipher,
                    },
                ],
            );
            s.write_all(&reply).await.expect("step 1 reply");
            s.flush().await.expect("flush");

            // Step 2 arrives in the clear; everything after it is ciphered.
            let (trans, _) = read_frame(&mut s).await.expect("HOPE step 2");
            let (_, keys) =
                compute_blowfish_chain(PASSWORD, &sessionkey, b"HMAC-SHA256").expect("key chain");
            // The client reads with `decode_key` and writes with
            // `encode_key`; the server is its mirror.
            let layer = if transport == Transport::HopeBlowfish {
                let state =
                    |k: &[u8]| hxcrypto::stream::BlowfishOfb64State::new(k).expect("Blowfish key");
                CipherLayer::HopeBlowfish {
                    read_state: state(&keys.encode_key),
                    read_key: keys.encode_key.clone(),
                    write_state: state(&keys.decode_key),
                    write_key: keys.decode_key.clone(),
                    session_key: sessionkey.clone(),
                    macalg: HopeMacAlg::Sha256,
                }
            } else {
                let aead = derive_aead_keys(&sessionkey, &keys.decode_key, &keys.encode_key);
                CipherLayer::ChaCha20Poly1305 {
                    read: hxcrypto::aead::AeadState {
                        key: aead.encode_key,
                        counter: 0,
                        dir: hxcrypto::aead::AEAD_DIR_CLIENT_TO_SERVER,
                    },
                    write: hxcrypto::aead::AeadState {
                        key: aead.decode_key,
                        counter: 0,
                        dir: hxcrypto::aead::AEAD_DIR_SERVER_TO_CLIENT,
                    },
                }
            };
            let mut s = compose(s, layer, CompressionKind::None).expect("compose");
            s.write_all(&task_reply(trans, &[]))
                .await
                .expect("step 2 reply");
            s.flush().await.expect("flush");
            s
        }
    }
}

async fn run_script(mut s: BoxedDuplex, script: Script) {
    match script {
        Script::Burst { go } => {
            if go.await.is_err() {
                return;
            }
            for seq in 0..BURST {
                // One write per frame, as a server sends them: each is
                // its own cipher record.
                s.write_all(&chat_frame(seq)).await.expect("burst write");
            }
            s.flush().await.expect("flush");
        }
        Script::Pings { mut acks, sent } => {
            for seq in 0..PINGS {
                let frame = chat_frame(seq);
                let _ = sent.send(Instant::now());
                s.write_all(&frame).await.expect("ping write");
                s.flush().await.expect("flush");
                if acks.recv().await.is_none() {
                    return;
                }
            }
        }
    }
    // Hold the connection until the client hangs up.
    let mut sink = [0u8; 256];
    while matches!(s.read(&mut sink).await, Ok(n) if n > 0) {}
}

/// Start a server for one connection on its own thread and runtime, so it
/// never competes with the client's runtime for a worker. Returns its
/// port and its thread id.
fn spawn_server(transport: Transport, script: Script) -> (u16, std::thread::JoinHandle<()>) {
    let listener = StdListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("port").port();
    listener.set_nonblocking(true).expect("nonblocking");
    let thread = std::thread::Builder::new()
        .name("bench-server".into())
        .spawn(move || {
            SERVER_TID.store(this_tid(), std::sync::atomic::Ordering::Relaxed);
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("server runtime");
            rt.block_on(async move {
                let listener = tokio::net::TcpListener::from_std(listener).expect("listener");
                let (tcp, _) = listener.accept().await.expect("accept");
                tcp.set_nodelay(true).ok();
                // Buffered, as a server that keeps up with a busy room
                // would be: the burst is to measure the client, and a
                // write per frame measures the server's syscalls instead.
                // Under TLS the buffer goes beneath it, so each frame is
                // still a record of its own, as with the HOPE ciphers.
                let s = if transport == Transport::Tls {
                    let buffered = tokio::io::BufWriter::with_capacity(SERVER_BUFFER, tcp);
                    let tls = tls_acceptor().accept(buffered).await.expect("TLS accept");
                    handshake(tls, transport).await
                } else {
                    handshake(
                        tokio::io::BufWriter::with_capacity(SERVER_BUFFER, tcp),
                        transport,
                    )
                    .await
                };
                run_script(s, script).await;
            });
        })
        .expect("spawn the server");
    (port, thread)
}

/// The running server's thread, for the CPU split.
static SERVER_TID: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

// ---- CPU time per thread ---------------------------------------------

fn this_tid() -> u32 {
    std::fs::read_link("/proc/thread-self")
        .ok()
        .and_then(|p| p.file_name()?.to_str()?.parse().ok())
        .unwrap_or(0)
}

/// Every thread's CPU time so far, in ns, by thread id.
fn thread_cpu() -> HashMap<u32, u64> {
    let mut out = HashMap::new();
    let Ok(dir) = std::fs::read_dir("/proc/self/task") else {
        return out;
    };
    for entry in dir.flatten() {
        let Some(tid) = entry.file_name().to_str().and_then(|s| s.parse().ok()) else {
            continue;
        };
        let ns = std::fs::read_to_string(entry.path().join("schedstat"))
            .ok()
            .and_then(|s| s.split_whitespace().next()?.parse().ok());
        if let Some(ns) = ns {
            out.insert(tid, ns);
        }
    }
    out
}

/// CPU spent between two snapshots: (main thread, server, the rest).
fn cpu_split(before: &HashMap<u32, u64>, after: &HashMap<u32, u64>, main: u32) -> (u64, u64, u64) {
    let server = SERVER_TID.load(std::sync::atomic::Ordering::Relaxed);
    let (mut m, mut s, mut rest) = (0, 0, 0);
    for (tid, ns) in after {
        let d = ns - before.get(tid).copied().unwrap_or(0).min(*ns);
        if *tid == main {
            m += d;
        } else if *tid == server {
            s += d;
        } else {
            rest += d;
        }
    }
    (m, s, rest)
}

// ---- the client --------------------------------------------------------

#[derive(Default)]
struct Client {
    handshake_at: Option<Instant>,
    shut_down: bool,
    /// Chat frames received, and the next sequence number expected.
    frames: u32,
    next_seq: u32,
    corrupt: u32,
    first_at: Option<Instant>,
    last_at: Option<Instant>,
    arrivals: Vec<Instant>,
    acks: Option<tokio::sync::mpsc::UnboundedSender<()>>,
}

thread_local! {
    static CLIENT: RefCell<Client> = RefCell::new(Client::default());
}

/// A chat frame's sequence number, if its bytes are what was sent.
fn check_frame(body: &[u8]) -> Option<u32> {
    let seq = u32::from_be_bytes(body.get(4..8)?.try_into().ok()?);
    let expected = chat_frame(seq);
    (body == &expected[22..]).then_some(seq)
}

unsafe extern "C" fn on_event(_c: *mut HxnetConnection, frame: *mut HxnetFrame, _u: *mut c_void) {
    let now = Instant::now();
    let f = &*frame;
    if f.type_ == CHAT {
        let body = if f.body_ptr.is_null() {
            &[][..]
        } else {
            std::slice::from_raw_parts(f.body_ptr, f.body_len as usize)
        };
        CLIENT.with(|c| {
            let mut c = c.borrow_mut();
            match check_frame(body) {
                Some(seq) if seq == c.next_seq => c.next_seq += 1,
                _ => c.corrupt += 1,
            }
            c.frames += 1;
            c.first_at.get_or_insert(now);
            c.last_at = Some(now);
            if let Some(acks) = c.acks.clone() {
                c.arrivals.push(now);
                let _ = acks.send(());
            }
        });
    }
    hxnet_frame_free(frame);
}

unsafe extern "C" fn on_shutdown(_c: *mut HxnetConnection, _code: i32, _u: *mut c_void) {
    CLIENT.with(|c| c.borrow_mut().shut_down = true);
}

unsafe extern "C" fn on_state(_c: *mut HxnetConnection, state: u32, _u: *mut c_void) {
    if state == ConnectionState::HandshakeDone as u32 {
        CLIENT.with(|c| c.borrow_mut().handshake_at = Some(Instant::now()));
    }
}

unsafe extern "C" fn trust_any(_fp: *const u8, _len: usize, _u: *mut c_void) -> i32 {
    1
}

/// Open a connection to the server on `port` the way the app does.
fn open(transport: Transport, port: u16) -> *mut HxnetConnection {
    let host = b"127.0.0.1";
    let name = b"bench";
    let ud = std::ptr::null_mut();
    unsafe {
        match transport {
            Transport::Plain => hxnet_connection_open_plaintext(
                host.as_ptr(),
                host.len(),
                port,
                LOGIN.as_ptr(),
                LOGIN.len(),
                PASSWORD.as_ptr(),
                PASSWORD.len(),
                name.as_ptr(),
                name.len(),
                0,
                190,
                0,
                1,
                std::ptr::null(),
                0,
                Some(on_event),
                Some(on_shutdown),
                Some(on_state),
                ud,
            ),
            Transport::Tls => hxnet_connection_open_plaintext_tls(
                host.as_ptr(),
                host.len(),
                port,
                LOGIN.as_ptr(),
                LOGIN.len(),
                PASSWORD.as_ptr(),
                PASSWORD.len(),
                name.as_ptr(),
                name.len(),
                0,
                190,
                0,
                1,
                std::ptr::null(),
                0,
                Some(on_event),
                Some(on_shutdown),
                Some(on_state),
                Some(trust_any),
                ud,
            ),
            Transport::HopeBlowfish | Transport::HopeAead => {
                let cipher = transport.cipher_label();
                hxnet_connection_open_hope(
                    host.as_ptr(),
                    host.len(),
                    port,
                    LOGIN.as_ptr(),
                    LOGIN.len(),
                    PASSWORD.as_ptr(),
                    PASSWORD.len(),
                    name.as_ptr(),
                    name.len(),
                    0,
                    190,
                    0,
                    1,
                    cipher.as_ptr(),
                    cipher.len(),
                    std::ptr::null(),
                    0,
                    Some(on_event),
                    Some(on_shutdown),
                    Some(on_state),
                    ud,
                )
            }
        }
    }
}

/// Iterate the main loop until `done`, or fail after `limit`.
fn pump(limit: Duration, done: impl Fn(&Client) -> bool) -> bool {
    let ctx = glib::MainContext::default();
    let deadline = Instant::now() + limit;
    // Something to wake a blocking iteration and check the deadline by,
    // should the connection go quiet.
    let tick = glib::timeout_add_local(Duration::from_millis(250), || glib::ControlFlow::Continue);
    let mut ok = true;
    while !CLIENT.with(|c| done(&c.borrow())) {
        if Instant::now() > deadline {
            ok = false;
            break;
        }
        ctx.iteration(true);
    }
    tick.remove();
    ok
}

struct Run {
    connect: Duration,
    /// Frames a second, and CPU ns per frame: main thread, client
    /// runtime, server.
    rate: f64,
    cpu: (f64, f64, f64),
    /// Latency samples in µs.
    latency: Vec<f64>,
    failures: Vec<String>,
}

/// Connect to a new server running `script`. `acks`, if given, is told
/// of every chat frame from the first: the server can send one as soon
/// as the login is done, before this returns.
fn connect_and(
    transport: Transport,
    script: Script,
    acks: Option<tokio::sync::mpsc::UnboundedSender<()>>,
) -> (*mut HxnetConnection, std::thread::JoinHandle<()>, Duration) {
    CLIENT.with(|c| {
        *c.borrow_mut() = Client {
            acks,
            ..Client::default()
        }
    });
    let (port, server) = spawn_server(transport, script);
    let t0 = Instant::now();
    let conn = open(transport, port);
    assert!(!conn.is_null(), "{}: open failed", transport.name());
    let up = pump(Duration::from_secs(10), |c| {
        c.handshake_at.is_some() || c.shut_down
    });
    let at = CLIENT.with(|c| c.borrow().handshake_at);
    assert!(up && at.is_some(), "{}: no handshake", transport.name());
    (conn, server, at.unwrap() - t0)
}

fn close(conn: *mut HxnetConnection, server: std::thread::JoinHandle<()>) {
    unsafe { hxnet_connection_destroy(conn) };
    // Let the destroy reach the actor and the server see the hang-up.
    let ctx = glib::MainContext::default();
    let until = Instant::now() + Duration::from_millis(50);
    while Instant::now() < until {
        ctx.iteration(false);
        std::thread::sleep(Duration::from_millis(1));
    }
    let _ = server.join();
}

fn run_once(transport: Transport) -> Run {
    let main_tid = this_tid();
    let mut failures = Vec::new();

    // Throughput.
    let (go, gate) = tokio::sync::oneshot::channel();
    let (conn, server, connect) = connect_and(transport, Script::Burst { go: gate }, None);
    // The burst waits for this, so all of its CPU falls between the
    // snapshots.
    let before = thread_cpu();
    let _ = go.send(());
    let got = pump(Duration::from_secs(60), |c| {
        c.frames >= BURST || c.shut_down
    });
    let after = thread_cpu();
    let (frames, corrupt, first, last) = CLIENT.with(|c| {
        let c = c.borrow();
        (c.frames, c.corrupt, c.first_at, c.last_at)
    });
    close(conn, server);
    if !got || frames != BURST {
        failures.push(format!("{frames} of {BURST} frames arrived"));
    }
    if corrupt > 0 {
        failures.push(format!("{corrupt} frames out of order or damaged"));
    }
    let span = match (first, last) {
        (Some(a), Some(b)) if b > a => (b - a).as_secs_f64(),
        _ => f64::NAN,
    };
    let rate = f64::from(frames) / span;
    let (m, s, rest) = cpu_split(&before, &after, main_tid);
    let per = |ns: u64| ns as f64 / f64::from(frames.max(1));
    let cpu = (per(m), per(rest), per(s));

    // Latency.
    let (ack_tx, ack_rx) = tokio::sync::mpsc::unbounded_channel();
    let (sent_tx, sent_rx) = std::sync::mpsc::channel();
    let (conn, server, _) = connect_and(
        transport,
        Script::Pings {
            acks: ack_rx,
            sent: sent_tx,
        },
        Some(ack_tx),
    );
    let got = pump(Duration::from_secs(60), |c| {
        c.frames >= PINGS || c.shut_down
    });
    let arrivals = CLIENT.with(|c| {
        let mut c = c.borrow_mut();
        c.acks = None;
        std::mem::take(&mut c.arrivals)
    });
    close(conn, server);
    let sends: Vec<Instant> = sent_rx.try_iter().collect();
    if !got || arrivals.len() != PINGS as usize || sends.len() != PINGS as usize {
        failures.push(format!(
            "latency: {} of {PINGS} frames arrived",
            arrivals.len()
        ));
    }
    let latency = sends
        .iter()
        .zip(&arrivals)
        .map(|(s, a)| (*a - *s).as_secs_f64() * 1e6)
        .collect();
    Run {
        connect,
        rate,
        cpu,
        latency,
        failures,
    }
}

/// The floor: the same pings over a plain socket, read on a thread of its
/// own with no pipeline at all. Latency samples in µs.
fn raw_floor() -> Vec<f64> {
    let (ack_tx, ack_rx) = tokio::sync::mpsc::unbounded_channel();
    let (sent_tx, sent_rx) = std::sync::mpsc::channel();
    let listener = StdListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("port").port();
    listener.set_nonblocking(true).expect("nonblocking");
    let server = std::thread::spawn(move || {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("runtime");
        rt.block_on(async move {
            let listener = tokio::net::TcpListener::from_std(listener).expect("listener");
            let (tcp, _) = listener.accept().await.expect("accept");
            tcp.set_nodelay(true).ok();
            let s = compose(tcp, CipherLayer::None, CompressionKind::None).expect("compose");
            run_script(
                s,
                Script::Pings {
                    acks: ack_rx,
                    sent: sent_tx,
                },
            )
            .await;
        });
    });
    let mut sock = std::net::TcpStream::connect(("127.0.0.1", port)).expect("connect");
    sock.set_nodelay(true).ok();
    let mut arrivals = Vec::with_capacity(PINGS as usize);
    for _ in 0..PINGS {
        use std::io::Read;
        let mut hdr = [0u8; 22];
        sock.read_exact(&mut hdr).expect("header");
        let len = u32::from_be_bytes([hdr[16], hdr[17], hdr[18], hdr[19]]) - 2;
        let mut body = vec![0u8; len as usize];
        sock.read_exact(&mut body).expect("body");
        arrivals.push(Instant::now());
        let _ = ack_tx.send(());
    }
    drop(sock);
    let _ = server.join();
    sent_rx
        .try_iter()
        .zip(&arrivals)
        .map(|(s, a)| (*a - s).as_secs_f64() * 1e6)
        .collect()
}

fn percentile(sorted: &[f64], p: f64) -> f64 {
    if sorted.is_empty() {
        return f64::NAN;
    }
    sorted[((sorted.len() - 1) as f64 * p).round() as usize]
}

fn median(mut v: Vec<f64>) -> f64 {
    v.sort_by(f64::total_cmp);
    percentile(&v, 0.5)
}

fn main() {
    // `cargo bench` passes `--bench`; anything else filters transports.
    let filter: Vec<String> = std::env::args()
        .skip(1)
        .filter(|a| !a.starts_with("--"))
        .collect();
    let chosen: Vec<Transport> = Transport::ALL
        .into_iter()
        .filter(|t| filter.is_empty() || filter.iter().any(|f| t.name().contains(f.as_str())))
        .collect();

    // The callbacks run on the thread-default main context, as the app's
    // do on the GTK main thread.
    let ctx = glib::MainContext::default();
    let _guard = ctx.acquire().expect("the main context");

    let floor = {
        let mut all: Vec<f64> = (0..RUNS).flat_map(|_| raw_floor()).collect();
        all.sort_by(f64::total_cmp);
        all
    };
    println!(
        "loopback: {BURST}-frame bursts, {PINGS} pings, median of {RUNS} runs; {} bytes a frame",
        chat_frame(0).len()
    );
    println!(
        "raw socket floor: latency p50 {:.1} µs, p99 {:.1} µs",
        percentile(&floor, 0.5),
        percentile(&floor, 0.99)
    );
    println!();
    println!(
        "{:<14} {:>9} {:>12} {:>9} {:>9} {:>9} {:>9} {:>9}",
        "transport", "connect", "frames/s", "main", "runtime", "server", "lat p50", "lat p99"
    );
    println!(
        "{:<14} {:>9} {:>12} {:>9} {:>9} {:>9} {:>9} {:>9}",
        "", "ms", "", "ns/frame", "ns/frame", "ns/frame", "µs", "µs"
    );

    let mut failed = false;
    for t in chosen {
        // One untimed run first: the first connection pays for loading
        // native roots and warming allocators.
        let _ = run_once(t);
        let runs: Vec<Run> = (0..RUNS).map(|_| run_once(t)).collect();
        let mut lat: Vec<f64> = runs
            .iter()
            .flat_map(|r| r.latency.iter().copied())
            .collect();
        lat.sort_by(f64::total_cmp);
        println!(
            "{:<14} {:>9.2} {:>12.0} {:>9.0} {:>9.0} {:>9.0} {:>9.1} {:>9.1}",
            t.name(),
            median(runs.iter().map(|r| r.connect.as_secs_f64() * 1e3).collect()),
            median(runs.iter().map(|r| r.rate).collect()),
            median(runs.iter().map(|r| r.cpu.0).collect()),
            median(runs.iter().map(|r| r.cpu.1).collect()),
            median(runs.iter().map(|r| r.cpu.2).collect()),
            percentile(&lat, 0.5),
            percentile(&lat, 0.99),
        );
        for r in &runs {
            for f in &r.failures {
                println!("  CHECK FAILED: {}: {f}", t.name());
                failed = true;
            }
        }
        if percentile(&lat, 0.5) < percentile(&floor, 0.5)
            || percentile(&lat, 0.99) < percentile(&floor, 0.99)
        {
            println!(
                "  CHECK FAILED: {}: latency under the raw socket's — the timing is wrong",
                t.name()
            );
            failed = true;
        }
    }
    if failed {
        std::process::exit(1);
    }
}
