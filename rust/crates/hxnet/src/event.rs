//! Events the [`Connection`](crate::Connection) actor emits on its
//! event channel. The GLib main thread drains these via the
//! `hxbridge` ferry (R3.1).
//!
//! The R3.3.a scaffold ships two variants: [`Event::Frame`] for
//! every complete frame read off the wire, and [`Event::Shutdown`]
//! when the actor exits. The typed-by-opcode variants
//! (`Event::Chat`, `Event::Msg`, `Event::UserCreate`, …) layer in
//! during R3.3.b when the C consumer wants typed dispatch instead
//! of the legacy header-switch in `rcv.c`.
//!
//! The actor never panics on a malformed frame — the worst it does
//! is emit [`Event::Shutdown`] with a [`ShutdownReason::FrameTooLarge`]
//! or [`ShutdownReason::StreamError`] and exit.

use crate::Frame;

/// Why the actor stopped. Last event on the channel before the
/// `Sender` drops and the consumer's `recv` returns `None`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ShutdownReason {
    /// Peer closed the stream (TCP FIN, TLS close-notify, etc.).
    /// Normal disconnect.
    Eof,
    /// Stream read or write errored (e.g. broken pipe, TLS alert,
    /// kernel buffer error). The actor logs the source error via
    /// `tracing` when the `tracing` feature lands; for now the
    /// String message is the only forensic. R3.3.b's logging pass
    /// adopts the existing `debug.h` category system.
    StreamError(String),
    /// A wire frame claimed a body larger than [`crate::MAX_BODY_LEN`].
    /// The actor refuses to allocate the buffer and tears down the
    /// connection rather than wedge.
    FrameTooLarge {
        /// The size the wire claimed.
        wire_len: u32,
    },
    /// Consumer-initiated shutdown. Catches every consumer-side
    /// path that ends the loop without an underlying transport
    /// failure:
    ///
    /// - Every [`ConnectionHandle`](crate::ConnectionHandle) clone
    ///   was dropped, so the command channel's receiver returned
    ///   `None`.
    /// - A [`crate::Command::Shutdown`] command was sent
    ///   explicitly (either via
    ///   [`ConnectionHandle::shutdown`](crate::ConnectionHandle::shutdown)
    ///   or `handle.send(Command::Shutdown)`).
    /// - The event receiver was dropped while the read side was
    ///   still healthy; the actor sees this only on the next
    ///   event-channel `send().await` and treats it as
    ///   consumer-initiated teardown.
    ///
    /// Pending writes are flushed best-effort before the actor
    /// exits. Future revisions may split this into finer-grained
    /// variants if consumers need to distinguish the three cases;
    /// today they're folded under one umbrella because the
    /// follow-up behaviour (close the socket, drop UI state) is
    /// identical.
    HandleDropped,
}

/// Where the actor is in the connect → handshake → frame-mode
/// progression. The C side gets one `Event::State(...)` per
/// transition, in roughly the order the variants are defined.
///
/// Phase A (this PR — TCP connect in Rust) only fires `Resolving`,
/// `Connecting`, and `Connected`. The remaining variants are
/// defined here so the FFI ABI is stable from the start —
/// subsequent phases (TLS, magic, login, HOPE) will start emitting
/// them as their state-machine pieces land. See
/// `docs/hxnet-connection-lifecycle-scoping.md` for the phase
/// breakdown.
///
/// Ordering note: variants are listed in lifecycle order, which
/// also pins their integer discriminants for the C-side enum
/// mirror. Adding new states at the tail is ABI-safe; reordering
/// existing variants is not.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
#[non_exhaustive]
pub enum ConnectionState {
    /// DNS resolution in flight. Always the first event for a
    /// fresh connection; skipped only if the caller passed an
    /// IP literal that doesn't need resolution.
    Resolving = 0,
    /// Resolution succeeded; TCP SYN sent. We're waiting for the
    /// kernel to confirm the connection.
    Connecting = 1,
    /// TCP three-way handshake completed. For non-TLS
    /// connections this is the last pre-frame state — the next
    /// event is normally `Event::Frame(...)`. For TLS connections
    /// (Phase B onward) this is followed by `TlsHandshaking`.
    Connected = 2,
    /// TLS ClientHello sent; awaiting the server's
    /// ServerHello + cert. The verifier callback fires during
    /// this state. (Phase B — not emitted yet.)
    TlsHandshaking = 3,
    /// Writing `HTLC_MAGIC` / reading `HTLS_MAGIC`. (Phase C —
    /// not emitted yet.)
    MagicExchange = 4,
    /// Sending the LOGIN opcode. (Phase D — not emitted yet.)
    LoginSending = 5,
    /// Waiting for the server's TASK reply to LOGIN. (Phase E —
    /// not emitted yet.)
    LoginReplyWait = 6,
    /// HOPE handshake step 1 (server MAC choice + sessionkey
    /// seed). (Phase F — not emitted yet.)
    HopeStep1 = 7,
    /// HOPE handshake step 2 (client response, server cipher
    /// confirmation). (Phase F — not emitted yet.)
    HopeStep2 = 8,
    /// Cipher transition — re-wrap the transport in the
    /// negotiated cipher adapter. (Phase F — not emitted yet.)
    CipherTransition = 9,
    /// Handshake complete; the actor is now in frame mode and
    /// will emit `Event::Frame(...)` for each subsequent server
    /// frame. (Phase F end-state — not emitted yet.)
    HandshakeDone = 10,
}

/// One event from the actor.
#[derive(Debug, Clone)]
pub enum Event {
    /// The actor's connection-lifecycle state changed. Fires
    /// once per transition through the connect → handshake →
    /// frame-mode progression. See [`ConnectionState`] for the
    /// per-variant semantics.
    State(ConnectionState),

    /// A complete frame was read off the wire. The R3.3.a scaffold
    /// emits this verbatim; R3.3.b can switch to typed-by-opcode
    /// variants once the C consumer is ready.
    Frame(Frame),

    /// The actor has stopped. After this event, the channel
    /// closes; the consumer's `recv` returns `None`. The actor
    /// never emits more events after [`Event::Shutdown`].
    Shutdown(ShutdownReason),
}
