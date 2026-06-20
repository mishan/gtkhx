//! Hotline Connection actor for GtkHx (Phase R3.3.a scaffold).
//!
//! `hxnet` is the eventual home of the Connection lifecycle that
//! `src/network.c` owns today. The roadmap target (`docs/RUST-ROADMAP.md`
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
//! # What R3.3.a ships
//!
//! This is the **scaffold-only** phase. The crate exists, the actor
//! pattern is real (spawn / channel pair / shutdown), and the
//! frame-level read/write loop is exercised against
//! `tokio::io::duplex` in-memory streams. The crate is `rlib`-only
//! — no C FFI, no `staticlib`, no `meson.build` hookup.
//!
//! The HOPE cipher layer and the compression layer are NOT in this
//! phase. The actor reads and writes **plaintext Hotline frames**
//! (i.e. what the C code sees AFTER `hx_decode` has stripped the
//! cipher + decompressed). R3.3.c lands `CipherStream` /
//! `CompressStream` adapters that wrap the inner `AsyncRead +
//! AsyncWrite`, transparently extending what the actor already
//! does at the frame layer.
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

pub mod command;
pub mod connection;
pub mod event;
pub mod frame;

pub use command::Command;
pub use connection::{Connection, ConnectionHandle, SpawnError};
pub use event::{Event, ShutdownReason};
pub use frame::{Frame, MAX_BODY_LEN};
