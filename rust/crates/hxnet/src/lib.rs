//! Hotline Connection actor for GtkHx (Phase R3.3.a scaffold).
//!
//! `hxnet` is the eventual home of the Connection lifecycle that
//! `src/network.c` owns today. The roadmap target (`docs/rust/ROADMAP.md`
//! §R3 work item 1) is a tokio-driven actor that:
//!
//! 1. Owns a `tokio::net::TcpStream` (or any `AsyncRead + AsyncWrite`).
//! 2. Reads Hotline-framed bytes off the wire, decodes via
//!    [`hotline-proto`], and emits typed [`Event`]s on a channel
//!    the GLib main thread drains.
//! 3. Receives typed [`Command`]s from a paired channel and writes
//!    encoded bytes back out.
//! 4. Tears down cleanly on EOF / cancel / handle drop.
//!
//! # What R3.3.a + R3.3.b + R3.3.c ship
//!
//! R3.3.a built the actor itself (spawn / channel pair / shutdown)
//! over a `tokio::io::duplex` test exerciser. R3.3.b added the
//! C-callable FFI surface ([`ffi`]) plus the meson hookup so the
//! C binary can spawn an actor over a real fd. The FFI is the
//! polling-style API: `hxnet_connection_try_recv_frame` returns
//! events on demand; the callback-driven variant lands in R3.3.e
//! alongside the production switch in network.c.
//!
//! R3.3.c lands the HOPE cipher adapters in [`cipher`]:
//! [`cipher::BlowfishStream`] wraps any `AsyncRead + AsyncWrite`
//! in the Blowfish-OFB-64 stream cipher used by the legacy HOPE
//! handshake, and [`cipher::AeadStream`] wraps it in
//! ChaCha20-Poly1305 length-prefixed AEAD frames. Both are
//! transparent — the actor above still sees plaintext Hotline
//! frames; the cipher adapter is composed onto the inner
//! transport at spawn time. Compression adapters land in R3.3.d
//! as a sibling module; before either layer is composed, the
//! actor reads and writes plaintext Hotline frames (i.e. what
//! the C code sees AFTER `hx_decode` has stripped the cipher
//! and decompressed).
//!
//! # The actor pattern
//!
//! Consumers call [`Connection::spawn`] passing any `AsyncRead +
//! AsyncWrite`. They get back a [`ConnectionHandle`] plus an event
//! [`Receiver`](tokio::sync::mpsc::Receiver). The handle clones —
//! many callers can send commands; only the one that called spawn
//! holds the event receiver.
//!
//! There are two shapes for tearing the actor down. The reliable
//! shape is to drop every clone of [`ConnectionHandle`] — once
//! the command channel has no senders left, the actor's
//! `recv()` returns `None`, the write loop exits, pending writes
//! are flushed best-effort, and [`Event::Shutdown(HandleDropped)`](
//! crate::Event::Shutdown) ships. The best-effort shape is
//! [`ConnectionHandle::shutdown`], which try_sends a
//! `Command::Shutdown` and is a no-op if the command channel is
//! full. Callers that need a guaranteed shutdown should either
//! drop their handles or `await` an explicit
//! `handle.send(Command::Shutdown).await`. EOF or fatal stream
//! error on the read side closes the event channel from the
//! producer end — the GLib consumer sees the channel close and
//! tears the UI down.
//!
//! See [`Connection`] for the API entry points.

pub mod cipher;
pub mod command;
pub mod compress;
pub mod connect;
pub mod connection;
pub mod event;
pub mod ffi;
pub mod frame;
pub mod hope;
pub mod hope_blowfish;
pub mod hope_keys;
pub mod htxf;
pub mod lifecycle;
pub mod login;
pub mod login_reply;
pub mod magic;
pub mod proto_trace;
pub mod tls;
pub mod transform;

/// Per-step timeout for the pre-frame handshake (DNS + TCP connect,
/// TLS handshake, magic exchange, each LOGIN reply read). Matches the
/// legacy GIOStream path's `MAGIC_TIMEOUT_SEC` (src/network.c). Without
/// it a hung connect (unresponsive host) or a server that accepts the
/// TCP connection but never speaks would leave the orchestrator task —
/// and the connect/login UI task — stuck forever.
pub const HANDSHAKE_TIMEOUT_SECS: u64 = 30;

pub use command::Command;
pub use connection::{Connection, ConnectionHandle, SpawnError};
pub use event::{ConnectionState, Event, ShutdownReason};
pub use frame::{Frame, MAX_BODY_LEN};
pub use transform::{
    compose, AsyncDuplex, BoxedDuplex, CipherKind, CipherLayer, CompressionKind, TransformStack,
};
