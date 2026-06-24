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

use hotline_proto::parse::{decode_header_full, HeaderDecoded};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::{Command, Event, Frame, ShutdownReason, MAX_BODY_LEN};

/// Header size on the wire (`hl_hdr` = 22 bytes).
const HL_HDR_LEN: usize = hotline_proto::HL_HDR_LEN;

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
async fn run<S>(
    stream: &mut S,
    cmd_rx: &mut mpsc::Receiver<Command>,
    evt_tx: &mpsc::Sender<Event>,
) -> ShutdownReason
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    loop {
        tokio::select! {
            // No `biased;` here. With a sustained inbound frame
            // burst, biasing reads-first would starve the command
            // branch indefinitely — every iteration the read
            // future is immediately ready, the bias prefers it,
            // and queued outbound commands (including
            // `Command::Shutdown`) never get polled. tokio's
            // default fair (random) policy is the right shape:
            // every iteration the select picks one of the two
            // ready branches uniformly at random, so commands
            // get reliable head-of-line service even under read
            // pressure. Test determinism is bought instead by
            // the per-test setup driving one side at a time.

            // Read side: try to read one complete frame.
            read = read_one_frame(stream) => {
                match read {
                    Ok(frame) => {
                        if evt_tx.send(Event::Frame(frame)).await.is_err() {
                            // Consumer's receiver dropped — no
                            // point reading more. Treat as a
                            // clean shutdown from our side.
                            return ShutdownReason::HandleDropped;
                        }
                    }
                    Err(ReadFrameError::Eof) => return ShutdownReason::Eof,
                    Err(ReadFrameError::Io(e)) => {
                        return ShutdownReason::StreamError(e.to_string());
                    }
                    Err(ReadFrameError::FrameTooLarge { wire_len }) => {
                        return ShutdownReason::FrameTooLarge { wire_len };
                    }
                }
            }

            // Write side: pull the next command.
            cmd = cmd_rx.recv() => {
                match cmd {
                    Some(Command::WriteFrame(bytes)) => {
                        if let Err(e) = stream.write_all(&bytes).await {
                            return ShutdownReason::StreamError(e.to_string());
                        }
                        if let Err(e) = stream.flush().await {
                            return ShutdownReason::StreamError(e.to_string());
                        }
                    }
                    Some(Command::Shutdown) | None => {
                        // Best-effort flush — if it errors, we
                        // were going to shut down anyway.
                        let _ = stream.flush().await;
                        return ShutdownReason::HandleDropped;
                    }
                }
            }
        }
    }
}

/// Internal error from [`read_one_frame`].
enum ReadFrameError {
    /// Clean EOF before any header bytes were read. Distinct
    /// from a mid-frame EOF, which surfaces as
    /// `Io(UnexpectedEof)`.
    Eof,
    /// Any other I/O failure.
    Io(io::Error),
    /// Wire frame claimed a body larger than
    /// [`crate::MAX_BODY_LEN`] — refuse to allocate.
    FrameTooLarge { wire_len: u32 },
}

impl From<io::Error> for ReadFrameError {
    fn from(e: io::Error) -> Self {
        ReadFrameError::Io(e)
    }
}

/// Read one complete Hotline frame from `stream`. Returns the
/// header + body. EOF on the very first byte is a clean
/// shutdown; EOF mid-frame is an error.
async fn read_one_frame<S>(stream: &mut S) -> Result<Frame, ReadFrameError>
where
    S: AsyncRead + Unpin,
{
    let header = read_header(stream).await?;
    // Compare against the raw wire `len`, NOT the body_len —
    // decode_header_full clamps body_len to its max_packet
    // argument so it can hide pathological wire values. We want
    // to surface oversized frames as a fatal `FrameTooLarge`
    // rather than read clamped-and-misaligned bytes off the
    // socket. (`wire_len` includes the 2-byte `hc` field, hence
    // the +2 in the threshold.)
    let wire_limit = MAX_BODY_LEN.saturating_add(2);
    if header.wire_len > wire_limit {
        return Err(ReadFrameError::FrameTooLarge {
            wire_len: header.wire_len,
        });
    }
    let body = read_body(stream, header.body_len as usize).await?;
    Ok(Frame::new(header, body))
}

/// Read exactly [`HL_HDR_LEN`] bytes and decode the header. A
/// zero-byte read on the first byte is the EOF marker.
async fn read_header<S>(stream: &mut S) -> Result<HeaderDecoded, ReadFrameError>
where
    S: AsyncRead + Unpin,
{
    let mut hdr_buf = [0u8; HL_HDR_LEN];
    let mut read = 0;
    while read < HL_HDR_LEN {
        let n = stream.read(&mut hdr_buf[read..]).await?;
        if n == 0 {
            if read == 0 {
                return Err(ReadFrameError::Eof);
            }
            return Err(ReadFrameError::Io(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "EOF mid-header",
            )));
        }
        read += n;
    }
    // Pass u32::MAX so body_len mirrors wire_len exactly; the
    // call-site `read_one_frame` does its own ceiling check
    // against the raw wire_len before allocating.
    decode_header_full(&hdr_buf, u32::MAX).ok_or_else(|| {
        ReadFrameError::Io(io::Error::new(
            io::ErrorKind::InvalidData,
            "header decode returned None — should be unreachable on 22-byte input",
        ))
    })
}

/// Read exactly `len` body bytes. Mid-body EOF is an error.
async fn read_body<S>(stream: &mut S, len: usize) -> Result<Vec<u8>, ReadFrameError>
where
    S: AsyncRead + Unpin,
{
    let mut body = vec![0u8; len];
    let mut read = 0;
    while read < len {
        let n = stream.read(&mut body[read..]).await?;
        if n == 0 {
            return Err(ReadFrameError::Io(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "EOF mid-body",
            )));
        }
        read += n;
    }
    Ok(body)
}
