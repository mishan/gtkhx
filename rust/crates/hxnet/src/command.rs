//! Commands sent to the [`Connection`](crate::Connection) actor from
//! the GLib main thread (or any owner of a
//! [`ConnectionHandle`](crate::ConnectionHandle) clone).
//!
//! The R3.3.a scaffold ships a single variant — [`Command::WriteFrame`]
//! — that hands the actor a pre-encoded byte buffer to write
//! verbatim. R3.3.b grows this into the typed-by-opcode taxonomy
//! (`Command::SendLogin`, `Command::SendChat`, …) once the C
//! consumer is wired up; today's C-side `hlwrite_chunks` already
//! serialises into a byte buffer, so a single byte-blob variant is
//! the minimum surface the scaffold needs to be drivable from
//! tests.

/// One command for the actor's write side.
#[derive(Debug, Clone)]
pub enum Command {
    /// Write the given bytes verbatim to the underlying
    /// `AsyncWrite`. The actor does **not** validate that the bytes
    /// constitute a well-formed Hotline frame — the producer side
    /// (today: C's `hlpack_chunks`, eventually: a Rust builder
    /// over [`hotline_proto::build`]) is responsible for that.
    ///
    /// Backpressure: [`ConnectionHandle::send`](
    /// crate::ConnectionHandle::send) awaits if the bounded
    /// command channel is full. The actor's write loop never
    /// blocks the main thread because it lives on tokio.
    WriteFrame(Vec<u8>),

    /// Explicit shutdown. The actor flushes pending writes,
    /// drops its sender, and exits. Equivalent to dropping every
    /// [`ConnectionHandle`](crate::ConnectionHandle) clone, but
    /// useful when call sites want a synchronous "I'm done"
    /// signal independent of when their last handle drops.
    Shutdown,
}
