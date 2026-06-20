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

/// One event from the actor.
#[derive(Debug, Clone)]
pub enum Event {
    /// A complete frame was read off the wire. The R3.3.a scaffold
    /// emits this verbatim; R3.3.b can switch to typed-by-opcode
    /// variants once the C consumer is ready.
    Frame(Frame),

    /// The actor has stopped. After this event, the channel
    /// closes; the consumer's `recv` returns `None`. The actor
    /// never emits more events after [`Event::Shutdown`].
    Shutdown(ShutdownReason),
}
