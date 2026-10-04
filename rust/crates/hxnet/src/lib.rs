//! Hotline Connection actor for GtkHx (Phase R3.3.a scaffold).
//!
//! `hxnet` is the eventual home of the Connection lifecycle that
//! `src/network.c` owns today. The roadmap target (`docs/rust/ROADMAP.md`
//! §R3 work item 1) is a tokio-driven actor that:
//!
//! 1. Owns a `tokio::net::TcpStream` (or any `AsyncRead + AsyncWrite`).
//! 2. Reads Hotline-framed bytes off the wire, decodes via
//!    [`hxproto`], and emits typed [`Event`]s on a channel
//!    the GLib main thread drains.
//! 3. Receives typed [`Command`]s from a paired channel and writes
//!    encoded bytes back out.
//! 4. Tears down cleanly on EOF / cancel / handle drop.
//!
//! What the actor carries is the socket's bytes: since the session
//! ([`session`], over hx-libs' `hxsession`) took over the protocol, HOPE's
//! handshake and the cipher and compression it agrees run inside it, and
//! hxnet's part is the socket, TLS, a SOCKS proxy and the runtime.
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

pub mod banner_http;
pub mod command;
pub mod connect;
pub mod connection;
pub mod event;
pub mod ffi;
pub mod frame;
pub mod hfs_config;
pub mod htxf;
pub mod lifecycle;
pub mod login;
pub mod proto_trace;
pub mod session;
pub mod tls;
pub mod tracker;
pub mod tracker_fetch;
pub mod xfer;
pub mod xfer_handle;

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
