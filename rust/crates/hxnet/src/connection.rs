//! The `Connection` actor — the heart of `hxnet`.
//!
//! Spawned via [`Connection::spawn`], the actor owns an
//! `AsyncRead + AsyncWrite + Unpin + Send + 'static` (the
//! underlying transport), reads complete Hotline frames off it,
//! emits typed [`Event`]s on a channel, and writes [`Command`]
//! payloads back out.
//!
//! # Concurrency shape
//!
//! Inside the spawned task, [`tokio::select!`] drives two
//! futures: the next frame coming off the read side, and the
//! next command coming off the command channel. Whichever fires
//! first wins the iteration; the loop continues until either the
//! read side ends (EOF, error, oversized frame) or every
//! [`ConnectionHandle`] clone has dropped (the command channel's
//! sender side hits 0 strong refs and `recv()` returns `None`).
//!
//! # Backpressure
//!
//! Both channels are bounded:
//!
//! - **Events** (actor → consumer): capacity at
//!   [`DEFAULT_EVENT_CAPACITY`]. When the GLib consumer can't
//!   keep up, the actor's `send` parks; the read loop stalls
//!   naturally. This is the right behaviour — a UI that can't
//!   draw shouldn't be drowned in stale events.
//! - **Commands** (consumer → actor): capacity at
//!   [`DEFAULT_COMMAND_CAPACITY`]. Producers hit backpressure
//!   when the actor's write loop is parked on the kernel buffer.
//!   Same shape as the events channel.
//!
//! # Lifecycle observability
//!
//! Every actor exit path emits an [`Event::Shutdown`] **before**
//! dropping its event sender. Consumers can rely on a final
//! [`Event::Shutdown`] arriving before the channel closes — they
//! don't have to interpret a bare `None` from `recv()`.

use std::io;

use hxsession::frame::{FrameError, FrameReader as Transactions};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::{Command, Event, Frame, ShutdownReason};

/// Default capacity of the event channel (actor → consumer).
/// 64 buffers a typical chat burst without locking out the
/// connection's read loop. Tune per consumer if needed.
pub const DEFAULT_EVENT_CAPACITY: usize = 64;

/// Default capacity of the command channel (consumer → actor).
///
/// The C-side bridge maps every outgoing Hotline frame to one
/// command, so the budget needs to cover bursty post-login
/// fetches without blocking. A typical join sequence sends
/// AGREEMENTAGREE + USER_CHANGE + USER_GETLIST +
/// GET_CHAT_HISTORY + FILE_LIST + NEWSDIRLIST +
/// DOWNLOAD_BANNER + a handful of follow-up reads — easily 6-10
/// commands in tight succession before the actor's send loop
/// drains them. Sizing at 256 gives a 25x headroom on that
/// burst so `try_send` returning `Full` (which the C side
/// surfaces as `HXNET_SEND_FULL` / -1 and which `hlwrite` would
/// otherwise convert into a hard disconnect) effectively can't
/// happen under any realistic workload.
///
/// If the cap is ever hit in practice the right next step is to
/// implement a retry/drain idle on the C side so FULL becomes a
/// soft backpressure signal, not a fatal error — captured as a
/// follow-up in the roadmap.
pub const DEFAULT_COMMAND_CAPACITY: usize = 256;

/// Errors from [`Connection::spawn`]. There's only one variant
/// today (no tokio runtime context), but the enum makes the API
/// extensible without breaking callers when R3.3.c adds
/// HOPE-handshake failure modes.
#[derive(Debug)]
#[non_exhaustive]
pub enum SpawnError {
    /// `Connection::spawn` was called outside any tokio runtime.
    /// Callers must hold an `&Runtime` (via
    /// `hxbridge::runtime::Runtime::global`) and call
    /// `runtime.spawn(...)` inline, or call inside an
    /// `#[tokio::test]` function for tests.
    NoRuntime,
}

impl std::fmt::Display for SpawnError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SpawnError::NoRuntime => {
                f.write_str("Connection::spawn called outside any tokio runtime")
            }
        }
    }
}

impl std::error::Error for SpawnError {}

/// Clonable handle to a spawned [`Connection`]. Holds the command
/// channel's [`mpsc::Sender`]; cloning the handle clones the
/// sender (refcounted by tokio).
///
/// When the last clone drops, the command channel's receiver side
/// returns `None` on the actor's next poll and the actor exits.
#[derive(Clone)]
pub struct ConnectionHandle {
    tx: mpsc::Sender<Command>,
}

impl ConnectionHandle {
    /// Send a command to the actor. Awaits if the bounded command
    /// channel is full (backpressure on the producer).
    ///
    /// Returns `Err` if the actor has already exited — the command
    /// channel is closed. Production callers should treat this as
    /// a clean "we lost the race with shutdown" and stop sending.
    pub async fn send(&self, cmd: Command) -> Result<(), mpsc::error::SendError<Command>> {
        self.tx.send(cmd).await
    }

    /// Try to send without awaiting. Returns `Err` if the channel
    /// is full **or** the actor has exited. Useful from
    /// synchronous code paths that can drop a command rather than
    /// blocking.
    pub fn try_send(&self, cmd: Command) -> Result<(), mpsc::error::TrySendError<Command>> {
        self.tx.try_send(cmd)
    }

    /// Try to send [`Command::Shutdown`] without awaiting
    /// backpressure. **Best-effort**: if the actor has already
    /// exited or the command channel is currently full, this is
    /// a silent no-op. Callers that need a guaranteed shutdown
    /// should either:
    ///
    /// - drop every [`ConnectionHandle`] clone — once the
    ///   command channel has no senders left, the actor exits
    ///   deterministically — or
    /// - `await` an explicit
    ///   `handle.send(Command::Shutdown).await` so backpressure
    ///   parks the caller until the channel has room.
    ///
    /// The convenience form here exists for synchronous call
    /// sites (e.g. a UI dispose path) where neither option is
    /// ergonomic.
    pub fn shutdown(&self) {
        let _ = self.tx.try_send(Command::Shutdown);
    }
}

/// Public entry point: spawn a [`Connection`] actor onto the
/// current tokio runtime.
pub struct Connection;

impl Connection {
    /// Spawn an actor reading from `stream` (and writing to it).
    /// Returns the producer-side handle plus the consumer-side
    /// event receiver.
    ///
    /// `stream` is any type implementing `AsyncRead + AsyncWrite
    /// + Unpin + Send + 'static`. Production passes a
    /// `tokio::net::TcpStream` (R3.3.b); cipher / compression
    /// adapters layer underneath in R3.3.c. Tests pass one half
    /// of a [`tokio::io::duplex`] pair for in-memory exercises.
    ///
    /// # Errors
    ///
    /// Returns [`SpawnError::NoRuntime`] if called outside any
    /// tokio runtime. Other failure modes (handshake, etc.)
    /// surface as [`Event::Shutdown`] events after spawn.
    pub fn spawn<S>(
        stream: S,
    ) -> Result<(ConnectionHandle, mpsc::Receiver<Event>, JoinHandle<()>), SpawnError>
    where
        S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
    {
        Self::spawn_with_capacities(stream, DEFAULT_COMMAND_CAPACITY, DEFAULT_EVENT_CAPACITY)
    }

    /// Same as [`Self::spawn`] but lets callers override the
    /// channel capacities. Tests use this to exercise
    /// backpressure with capacity-1 channels.
    pub fn spawn_with_capacities<S>(
        stream: S,
        command_capacity: usize,
        event_capacity: usize,
    ) -> Result<(ConnectionHandle, mpsc::Receiver<Event>, JoinHandle<()>), SpawnError>
    where
        S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
    {
        let handle = tokio::runtime::Handle::try_current().map_err(|_| SpawnError::NoRuntime)?;

        let (cmd_tx, cmd_rx) = mpsc::channel::<Command>(command_capacity);
        let (evt_tx, evt_rx) = mpsc::channel::<Event>(event_capacity);

        let join = handle.spawn(actor_loop(stream, cmd_rx, evt_tx));

        Ok((ConnectionHandle { tx: cmd_tx }, evt_rx, join))
    }

    /// Create the channels + handle without spawning the actor.
    /// Used by spawn paths that need to do asynchronous setup
    /// before the actor can start — Phase A's connect-in-Rust
    /// is the first such consumer (TCP connect happens after
    /// channel creation but before actor spawn so state events
    /// can flow out the event channel during the connect).
    ///
    /// Buffered sends on the returned handle queue in the
    /// channel until the actor comes online
    /// (`DEFAULT_COMMAND_CAPACITY` slots of buffering).
    ///
    /// The third return value is the [`Command`] receiver and
    /// the fourth is the [`Event`] sender — the caller is
    /// responsible for driving them into [`Connection::run_actor`]
    /// inside the spawned setup task.
    pub fn make_channels() -> (
        ConnectionHandle,
        mpsc::Receiver<Event>,
        mpsc::Receiver<Command>,
        mpsc::Sender<Event>,
    ) {
        let (cmd_tx, cmd_rx) = mpsc::channel::<Command>(DEFAULT_COMMAND_CAPACITY);
        let (evt_tx, evt_rx) = mpsc::channel::<Event>(DEFAULT_EVENT_CAPACITY);
        (ConnectionHandle { tx: cmd_tx }, evt_rx, cmd_rx, evt_tx)
    }

    /// Run the actor loop against pre-existing channels created
    /// via [`Self::make_channels`]. Used by async-spawn paths
    /// that need to do setup (DNS, connect, TLS handshake) on
    /// the runtime before the actor starts processing the
    /// transport.
    pub async fn run_actor<S>(
        stream: S,
        cmd_rx: mpsc::Receiver<Command>,
        evt_tx: mpsc::Sender<Event>,
    ) where
        S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
    {
        actor_loop(stream, cmd_rx, evt_tx).await
    }

    /// Type-erased spawn entry — accepts a [`BoxedDuplex`] instead
    /// of a concrete `S`. Used by the FFI to hand the actor a
    /// stack composed at runtime by [`crate::transform::compose`]
    /// (cipher + compression chosen after the HOPE handshake
    /// resolved them).
    ///
    /// Behaviourally identical to [`Self::spawn`] — only the type
    /// surface differs. The boxed trait object's `poll_*` calls
    /// go through one virtual dispatch per poll, which costs the
    /// price of an indirect call (negligible against the syscall
    /// the call would trigger).
    pub fn spawn_boxed(
        stream: crate::transform::BoxedDuplex,
    ) -> Result<(ConnectionHandle, mpsc::Receiver<Event>, JoinHandle<()>), SpawnError> {
        Self::spawn_with_capacities(stream, DEFAULT_COMMAND_CAPACITY, DEFAULT_EVENT_CAPACITY)
    }
}

/// The actor loop. Owns the stream and both channel endpoints
/// for its half.
async fn actor_loop<S>(
    mut stream: S,
    mut cmd_rx: mpsc::Receiver<Command>,
    evt_tx: mpsc::Sender<Event>,
) where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let reason = run(&mut stream, &mut cmd_rx, &evt_tx).await;

    // Try to send the final Shutdown event. If the consumer's
    // receiver already dropped, this fails silently — the caller
    // doesn't care anymore.
    let _ = evt_tx.send(Event::Shutdown(reason)).await;

    // Implicit on scope exit: evt_tx drops → consumer's recv
    // returns None; cmd_rx drops → any in-flight send by a
    // surviving handle returns Err. Both are how downstream
    // callers detect the actor's exit beyond the Shutdown event.
}

/// The actor's main loop, factored out so the [`actor_loop`]
/// wrapper can run a uniform `Shutdown` send on every exit path.
///
/// Reading and writing run side by side, each on its half of the
/// stream, and the first to finish ends the connection. Neither waits
/// on the other: a command being written — a large one into a full
/// socket — doesn't stop frames being read, and a frame waiting for
/// room in the event channel — a consumer that's behind — doesn't stop
/// commands, `Shutdown` among them, going out. Taking turns in one loop
/// could deadlock against a server doing the same: each side writing,
/// neither reading.
async fn run<S>(
    stream: &mut S,
    cmd_rx: &mut mpsc::Receiver<Command>,
    evt_tx: &mpsc::Sender<Event>,
) -> ShutdownReason
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let (mut rd, mut wr) = tokio::io::split(stream);
    tokio::select! {
        reason = read_side(&mut rd, evt_tx) => reason,
        reason = write_side(&mut wr, cmd_rx) => reason,
    }
}

/// Read frames and hand them on until the stream ends or fails, or the
/// consumer goes away.
async fn read_side<R>(rd: &mut R, evt_tx: &mpsc::Sender<Event>) -> ShutdownReason
where
    R: AsyncRead + Unpin,
{
    let mut reader = FrameReader::default();
    loop {
        match reader.next(rd).await {
            Ok(frame) => {
                if evt_tx.send(Event::Frame(frame)).await.is_err() {
                    // Consumer's receiver dropped — no point reading
                    // more. Treat as a clean shutdown from our side.
                    return ShutdownReason::HandleDropped;
                }
            }
            Err(ReadFrameError::Eof) => return ShutdownReason::Eof,
            Err(ReadFrameError::Io(e)) => return ShutdownReason::StreamError(e.to_string()),
            Err(ReadFrameError::FrameTooLarge { wire_len }) => {
                return ShutdownReason::FrameTooLarge { wire_len };
            }
        }
    }
}

/// Write each command's frame until `Shutdown`, every handle gone, or a
/// failed write.
async fn write_side<W>(wr: &mut W, cmd_rx: &mut mpsc::Receiver<Command>) -> ShutdownReason
where
    W: AsyncWrite + Unpin,
{
    loop {
        match cmd_rx.recv().await {
            Some(Command::WriteFrame(bytes)) => {
                if let Err(e) = wr.write_all(&bytes).await {
                    return ShutdownReason::StreamError(e.to_string());
                }
                if let Err(e) = wr.flush().await {
                    return ShutdownReason::StreamError(e.to_string());
                }
            }
            // There is no session here to answer an agreement.
            Some(Command::Agree { .. }) => {}
            Some(Command::Shutdown) | None => {
                // Best-effort flush — if it errors, we were going to
                // shut down anyway.
                let _ = wr.flush().await;
                return ShutdownReason::HandleDropped;
            }
        }
    }
}

/// Internal error from [`FrameReader::next`].
enum ReadFrameError {
    /// Clean EOF between frames. Distinct from EOF partway through
    /// one, which surfaces as `Io(UnexpectedEof)`.
    Eof,
    /// Any other I/O failure.
    Io(io::Error),
    /// A frame, or a transaction split across frames, claimed more than
    /// the frame reader takes — refused before allocating for it.
    FrameTooLarge { wire_len: u32 },
}

impl From<io::Error> for ReadFrameError {
    fn from(e: io::Error) -> Self {
        ReadFrameError::Io(e)
    }
}

/// How much one read asks the stream for.
const READ_CHUNK: usize = 16 * 1024;

/// Reads frames off the stream.
///
/// The cutting is hx-libs' `hxsession` frame reader, the one the browser
/// client uses: frames delimited by DataSize, and a transaction a server
/// splits across several frames joined back into one before it is handed
/// on. This side only reads. Every read is a single `read` call, which is
/// cancel-safe — dropped before it completes, it has taken nothing — and
/// what it returns goes straight into the reader, so an interrupted read
/// carries on where it stopped the next time it is polled.
struct FrameReader {
    transactions: Transactions,
    buf: Vec<u8>,
}

impl Default for FrameReader {
    fn default() -> Self {
        FrameReader {
            transactions: Transactions::new(),
            buf: vec![0u8; READ_CHUNK],
        }
    }
}

impl FrameReader {
    /// The next complete transaction. EOF between transactions is a
    /// clean shutdown; EOF partway through one is an error.
    async fn next<S>(&mut self, stream: &mut S) -> Result<Frame, ReadFrameError>
    where
        S: AsyncRead + Unpin,
    {
        loop {
            match self.transactions.next_transaction() {
                Ok(Some(t)) => {
                    // A split transaction given up on — the same trans
                    // starting over, or too many in flight — answers no
                    // request; its task waits, as one whose frames were
                    // dropped always did.
                    let _ = self.transactions.take_abandoned();
                    // Joined, its header says the joined size, so the
                    // frame reads as any other.
                    return Frame::from_raw(&t.buf).ok_or_else(|| {
                        ReadFrameError::Io(io::Error::new(
                            io::ErrorKind::InvalidData,
                            "a transaction the frame reader passed does not decode",
                        ))
                    });
                }
                Ok(None) => {}
                Err(FrameError::TooLarge(wire_len)) => {
                    return Err(ReadFrameError::FrameTooLarge { wire_len });
                }
            }
            let n = stream.read(&mut self.buf).await?;
            if n == 0 {
                if self.transactions.is_idle() {
                    return Err(ReadFrameError::Eof);
                }
                return Err(ReadFrameError::Io(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "EOF mid-frame",
                )));
            }
            self.transactions.push(&self.buf[..n]);
        }
    }
}
