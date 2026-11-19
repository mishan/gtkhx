//! The connection actor as the I/O of hx-libs' `hxsession`, which drives
//! the magic, the login, the agreement and the keep-alive, and numbers
//! every transaction. Under HOPE it is the session too that runs the
//! cipher and compression: what the actor reads and writes is the
//! socket's bytes, whatever they are.
//!
//! The session runs in raw mode, because GtkHx still has receive handlers
//! of its own: every transaction the session does not handle itself
//! reaches the consumer whole, and frames the consumer sends go out as
//! built, on a trans it took from the session ([`SharedSession`]). What
//! the session says becomes:
//!
//! - every transaction it hands over whole → `Event::Frame`
//! - but the messaging extension's (800–834), and the Reactions draft's
//!   (711, and the 718 a server may send beside it), where the server
//!   agreed to each, as the session's own `Unhandled` and `Reply` in
//!   `Event::Session`: what the server pushes, and the replies to the
//!   consumer's requests of them, whose handlers are all Rust's
//!   (`hxhandlers::recv::friends`, `hxhandlers::recv::reactions`)
//! - what it makes of the rest, the replies the consumer expected, and
//!   with the tap on each transaction as it came → `Event::Session`
//! - logged in → what the login reply said, as `Event::Session`, then
//!   `ConnectionState::HandshakeDone`
//! - a refused login → the refusal, as `Event::Session`, before the actor
//!   ends
//! - the login settled, the agreement answered or not waited on any
//!   longer → `ConnectionState::LoginReady`
//! - closed → the actor ends, with the reason as its `Shutdown`
//!
//! The agreement is shown from its frame; answering it is
//! [`Command::Agree`].

use std::collections::HashSet;
use std::sync::{Arc, Mutex, MutexGuard};

use hxsession::{Closed, Session};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::sync::{mpsc, Notify};
use tokio::time::Instant;

use crate::device::{Heard, Prover};
use crate::proto_trace::{trace, Dir};
use crate::{Command, ConnectionState, Event, Frame, ShutdownReason};

/// The session, shared by the actor that drives it and the connection's
/// handle, which numbers the consumer's requests from it
/// ([`Session::take_trans`]) before they are sent: the consumer keys a
/// request's reply on its trans as it builds it.
pub type SharedSession = Arc<Mutex<Session>>;

const READ_CHUNK: usize = 16 * 1024;

/// The session's settings for a GtkHx login.
///
/// `nick` and `icon` go with the agreement, or to a 1.2 server in a user
/// change; the login itself carries no name.
pub fn config(
    login: &[u8],
    password: &[u8],
    nick: &[u8],
    icon: u16,
    version: u16,
    caps: u64,
    handled: hxsession::Handled,
) -> hxsession::Config {
    hxsession::Config {
        // C hands these over as UTF-8.
        login: String::from_utf8_lossy(login).into_owned(),
        password: String::from_utf8_lossy(password).into_owned(),
        nick: String::from_utf8_lossy(nick).into_owned(),
        icon,
        version,
        caps,
        handshake_timeout_ms: crate::HANDSHAKE_TIMEOUT_SECS * 1000,
        agreement_wait_ms: 2_000,
        proof_timeout_ms: 10_000,
        keepalive_ms: 60_000,
        raw: true,
        handled,
    }
}

/// What the three sides of the actor share. Each lock is held only
/// between awaits, so none is ever waited on for long.
struct Shared {
    session: SharedSession,
    start: Instant,
    /// What the session has queued and the write side has not written.
    out: Mutex<Vec<u8>>,
    /// Whether the login has gone out, for the `LoginSending` and
    /// `LoginReplyWait` states.
    login_sent: Mutex<bool>,
    /// Wakes the write side: there is something to write.
    to_write: Notify,
    /// Wakes the delivery side: the session has something to say.
    to_deliver: Arc<Notify>,
    /// Held while popping events and handing them on, so they reach the
    /// consumer in the session's order whichever side hands them on.
    delivering: tokio::sync::Mutex<()>,
    /// The trans of each request the consumer sent whose reply is Rust's,
    /// and has not had answered.
    rust_asked: Mutex<HashSet<u32>>,
    /// The connect's device proof, where it has a device to prove.
    prover: Mutex<Option<Prover>>,
    /// The challenge's answer, worked out off the actor, waiting to be
    /// delivered.
    worked: Arc<Mutex<Option<Worked>>>,
}

type Worked = Result<Vec<u8>, hxkeys::Proof>;

/// The messaging extension's transactions, both ways.
fn is_messaging(opcode: u32) -> bool {
    (800..=834).contains(&opcode)
}

/// What the consumer sends whose reply its Rust handlers read.
fn rust_answers(opcode: u32) -> bool {
    // Typing (813) is never answered.
    (is_messaging(opcode) && opcode != 813)
        || matches!(opcode, hxproto::reactions::SEND | hxproto::reactions::GET)
}

impl Shared {
    fn session(&self) -> MutexGuard<'_, Session> {
        self.session
            .lock()
            .expect("the session lock is never held across a panic")
    }

    fn agreed(&self, cap: u64) -> bool {
        self.session().server().is_some_and(|i| i.caps & cap != 0)
    }

    /// Whether the session has reactions: the bit, and the limits the
    /// draft requires beside it.
    fn reacts(&self) -> bool {
        self.session()
            .server()
            .is_some_and(|i| i.reactions.is_some())
    }

    /// Whether the session has replies: the bit, and the target kinds the
    /// draft requires beside it.
    fn replies(&self) -> bool {
        self.session().server().is_some_and(|i| i.replies.is_some())
    }

    fn now(&self) -> u64 {
        self.start.elapsed().as_millis() as u64
    }

    /// `e`, as the connect's proof hears it, where there is one.
    fn hear(&self, e: hxsession::Event) -> Heard {
        let mut prover = self.prover.lock().expect("never held across a panic");
        match prover.as_mut() {
            Some(p) => p.heard(&mut self.session(), e, unix_now()),
            None => Heard::Pass(e),
        }
    }

    /// The challenge answered off the session, or how the proof ended
    /// there.
    fn answered(&self, a: Worked) -> Option<hxkeys::Proof> {
        let mut prover = self.prover.lock().expect("never held across a panic");
        prover
            .as_mut()?
            .answered(&mut self.session(), a, self.now())
    }

    /// The login was answered: the proof's first step, where there is one.
    fn logged_in(&self) -> Option<hxkeys::Proof> {
        let mut prover = self.prover.lock().expect("never held across a panic");
        prover.as_mut()?.logged_in(&mut self.session(), unix_now())
    }

    /// Move what the session has queued to the write side; whether there
    /// was anything.
    fn queue_out(&self) -> bool {
        let bytes = {
            let mut s = self.session();
            trace_out(&s.pending_plaintext());
            s.take_outgoing()
        };
        if bytes.is_empty() {
            return false;
        }
        self.out
            .lock()
            .expect("never held across a panic")
            .extend_from_slice(&bytes);
        self.to_write.notify_one();
        true
    }
}

/// Run `session` over `stream` until either ends.
pub async fn run<S>(
    stream: S,
    session: SharedSession,
    prover: Option<Prover>,
    mut cmd_rx: mpsc::Receiver<Command>,
    evt_tx: mpsc::Sender<Event>,
) where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let shared = Shared {
        session,
        start: Instant::now(),
        out: Mutex::new(Vec::new()),
        login_sent: Mutex::new(false),
        to_write: Notify::new(),
        to_deliver: Arc::new(Notify::new()),
        worked: Arc::new(Mutex::new(None)),
        delivering: tokio::sync::Mutex::new(()),
        rust_asked: Mutex::new(HashSet::new()),
        prover: Mutex::new(prover),
    };
    shared.queue_out();

    let (mut rd, mut wr) = tokio::io::split(stream);
    let reason = tokio::select! {
        r = read_side(&shared, &mut rd, &evt_tx) => r,
        r = write_side(&shared, &mut wr, &mut cmd_rx) => r,
        r = deliver_side(&shared, &evt_tx) => r,
    };
    let _ = evt_tx.send(Event::Shutdown(reason)).await;
}

/// Read the stream into the session, and hand on what that brings
/// before reading more: a consumer that is behind holds the reading back.
async fn read_side<R>(shared: &Shared, rd: &mut R, evt_tx: &mpsc::Sender<Event>) -> ShutdownReason
where
    R: AsyncRead + Unpin,
{
    let mut buf = vec![0u8; READ_CHUNK];
    loop {
        let n = match rd.read(&mut buf).await {
            Ok(n) => n,
            Err(e) => return ShutdownReason::StreamError(e.to_string()),
        };
        if n == 0 {
            let s = shared.session();
            // A stream that ends part way through a transaction was cut
            // off; one that ends between them, the server closed.
            return if s.mid_transaction() {
                ShutdownReason::StreamError("EOF mid-frame".into())
            } else if s.server().is_some() {
                ShutdownReason::Eof
            } else {
                ShutdownReason::StreamError("the server closed the connection during login".into())
            };
        }
        let now = shared.now();
        {
            let mut s = shared.session();
            let before = s.negotiated().is_some();
            s.feed(&buf[..n], now);
            // The step-1 reply is the session's own and reaches no one;
            // the trace says what it agreed.
            if let Some(n) = s.negotiated().filter(|_| !before) {
                crate::proto_trace::note(&format!(
                    "HOPE agreed: MAC {:?}, cipher {:?}, compression {:?}",
                    n.mac, n.cipher, n.compression
                ));
            }
        }
        let wrote = shared.queue_out();
        // Whether or not it queued anything, what was fed may have moved
        // the session's deadlines, or let the consumer's commands through.
        shared.to_write.notify_one();
        // Fed the server's magic, the session answers with the login: the
        // consumer hears it is on its way before it hears the reply.
        let login_now = wrote
            && !std::mem::replace(
                &mut *shared.login_sent.lock().expect("never held across a panic"),
                true,
            );
        if login_now {
            for state in [
                ConnectionState::LoginSending,
                ConnectionState::LoginReplyWait,
            ] {
                let _turn = shared.delivering.lock().await;
                if evt_tx.send(Event::State(state)).await.is_err() {
                    return ShutdownReason::HandleDropped;
                }
            }
        }
        if let Some(reason) = deliver(shared, evt_tx).await {
            return reason;
        }
    }
}

/// Write what the session queues and what the consumer sends through
/// it, and keep the session's time.
async fn write_side<W>(
    shared: &Shared,
    wr: &mut W,
    cmd_rx: &mut mpsc::Receiver<Command>,
) -> ShutdownReason
where
    W: AsyncWrite + Unpin,
{
    loop {
        let deadline = shared.session().next_deadline();
        let sleep = async {
            match deadline {
                Some(ms) => {
                    tokio::time::sleep_until(shared.start + std::time::Duration::from_millis(ms))
                        .await
                }
                None => std::future::pending().await,
            }
        };
        // Nothing the consumer sends may go out before the login is
        // answered; its commands wait in the channel until then.
        let logged_in = shared.session().server().is_some();
        tokio::select! {
            cmd = cmd_rx.recv(), if logged_in => match cmd {
                Some(Command::WriteFrame(bytes)) => {
                    // Refused only once closed, and the reason is on its
                    // way to the consumer. Not traced here: the consumer
                    // traced it as it built it.
                    if let Some(h) = hxproto::parse::Header::parse(&bytes) {
                        if rust_answers(h.type_) {
                            shared.rust_asked.lock().expect("never held across a panic").insert(h.trans);
                        }
                    }
                    let queued = {
                        let mut s = shared.session();
                        match s.send_raw(&bytes) {
                            Ok(()) => s.take_outgoing(),
                            Err(_) => Vec::new(),
                        }
                    };
                    shared.out.lock().expect("never held across a panic").extend_from_slice(&queued);
                    // A send puts the keep-alive off from when the session
                    // next reads the clock: read it now.
                    shared.session().tick(shared.now());
                    shared.queue_out();
                    shared.to_deliver.notify_one();
                }
                Some(Command::Agree { nick, icon }) => {
                    // Nothing waiting to be answered is not an error: an
                    // agreement already answered.
                    let mut s = shared.session();
                    s.set_identity(&String::from_utf8_lossy(&nick), icon);
                    let _ = s.agree();
                    drop(s);
                    shared.queue_out();
                    shared.to_deliver.notify_one();
                }
                Some(Command::Shutdown) | None => {
                    // What the session queued goes first: an agree it
                    // answered on its own, say.
                    let out = std::mem::take(&mut *shared.out.lock().expect("never held across a panic"));
                    let _ = wr.write_all(&out).await;
                    let _ = wr.flush().await;
                    return ShutdownReason::HandleDropped;
                }
            },
            () = shared.to_write.notified() => {}
            () = sleep => {
                let now = shared.now();
                shared.session().tick(now);
                shared.queue_out();
                shared.to_deliver.notify_one();
            }
        }
        let out = std::mem::take(&mut *shared.out.lock().expect("never held across a panic"));
        if out.is_empty() {
            continue;
        }
        if let Err(e) = wr.write_all(&out).await {
            return ShutdownReason::StreamError(e.to_string());
        }
        if let Err(e) = wr.flush().await {
            return ShutdownReason::StreamError(e.to_string());
        }
    }
}

/// Hand on what the session says when the write side has made it say
/// something — a tick, an agree — without holding the write side up
/// while a consumer that is behind catches up.
async fn deliver_side(shared: &Shared, evt_tx: &mpsc::Sender<Event>) -> ShutdownReason {
    loop {
        shared.to_deliver.notified().await;
        if let Some(reason) = deliver(shared, evt_tx).await {
            return reason;
        }
    }
}

/// Hand on everything the session has said, in order. `Some` once the
/// session has closed, or the consumer has gone.
async fn deliver(shared: &Shared, evt_tx: &mpsc::Sender<Event>) -> Option<ShutdownReason> {
    let _turn = shared.delivering.lock().await;
    loop {
        let worked = shared
            .worked
            .lock()
            .expect("never held across a panic")
            .take();
        if let Some(a) = worked {
            let ended = shared.answered(a);
            shared.queue_out();
            if let Some(p) = ended {
                if evt_tx.send(Event::DeviceProof(p)).await.is_err() {
                    return Some(ShutdownReason::HandleDropped);
                }
            }
        }
        let event = shared.session().poll_event()?;
        let event = match shared.hear(event) {
            Heard::Pass(e) => e,
            Heard::Challenge(c) => {
                // Off the session, the runtime and the delivery: it reads
                // and writes files, under a lock another GtkHx may hold. Its
                // answer is delivered when it comes, in its turn.
                let (worked, wake) = (shared.worked.clone(), shared.to_deliver.clone());
                tokio::task::spawn_blocking(move || {
                    let a = c.answer();
                    *worked.lock().expect("never held across a panic") = Some(a);
                    wake.notify_one();
                });
                continue;
            }
            Heard::Taken(ended) => {
                shared.queue_out();
                if let Some(p) = ended {
                    if evt_tx.send(Event::DeviceProof(p)).await.is_err() {
                        return Some(ShutdownReason::HandleDropped);
                    }
                }
                continue;
            }
        };
        let out = match event {
            hxsession::Event::Reply { trans, frame }
                if shared
                    .rust_asked
                    .lock()
                    .expect("never held across a panic")
                    .remove(&trans) =>
            {
                Event::Session(hxsession::Event::Reply { trans, frame })
            }
            // A server that never agreed to an extension is sent nothing
            // of it, and what it sends anyway is shown as any unknown
            // transaction is.
            e @ hxsession::Event::Unhandled { opcode, .. }
                if is_messaging(opcode) && shared.agreed(hxproto::messaging::CAP_MESSAGING) =>
            {
                Event::Session(e)
            }
            e @ hxsession::Event::Unhandled {
                opcode: hxproto::reactions::CHANGED,
                ..
            } if shared.reacts() => Event::Session(e),
            // The Replies draft's Notification, which a server agreeing to
            // either draft may send though GtkHx asks for none.
            e @ hxsession::Event::Unhandled {
                opcode: hxproto::replies::NOTIFICATION,
                ..
            } if shared.reacts() || shared.replies() => Event::Session(e),
            // Traced from the session's tap, the session's own replies
            // among them.
            hxsession::Event::Reply { frame, .. } | hxsession::Event::Unhandled { frame, .. } => {
                match Frame::from_raw(&frame) {
                    Some(f) => Event::Frame(f),
                    None => {
                        return Some(ShutdownReason::StreamError(
                            "a transaction the session passed does not decode".into(),
                        ))
                    }
                }
            }
            // What the reply said, then the state it puts the connection in.
            hxsession::Event::LoggedIn(info) => {
                let said = Event::Session(hxsession::Event::LoggedIn(info));
                if evt_tx.send(said).await.is_err() {
                    return Some(ShutdownReason::HandleDropped);
                }
                let done = Event::State(ConnectionState::HandshakeDone);
                if evt_tx.send(done).await.is_err() {
                    return Some(ShutdownReason::HandleDropped);
                }
                // A server that proves devices holds the join for it.
                let ended = shared.logged_in();
                shared.queue_out();
                match ended {
                    Some(p) => Event::DeviceProof(p),
                    None => continue,
                }
            }
            hxsession::Event::Ready => Event::State(ConnectionState::LoginReady),
            // A refusal's reason is the consumer's to show; the shutdown
            // carries only that it ended.
            hxsession::Event::Closed(why) => {
                if matches!(why, Closed::LoginRefused(_)) {
                    let said = Event::Session(hxsession::Event::Closed(why.clone()));
                    let _ = evt_tx.send(said).await;
                }
                return Some(closed(why));
            }
            // The agreement reaches the consumer as its frame.
            hxsession::Event::Agreement(_) => continue,
            e => Event::Session(e),
        };
        if evt_tx.send(out).await.is_err() {
            return Some(ShutdownReason::HandleDropped);
        }
    }
}

/// The wall clock, in Unix seconds, which the proof and the server's
/// challenge are timed by.
fn unix_now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

fn closed(why: Closed) -> ShutdownReason {
    let error = |what: String| ShutdownReason::StreamError(what);
    match why {
        Closed::BadMagic(got) => error(format!("magic: the server answered {got:02x?}")),
        Closed::LoginRefused(Some(reason)) => error(format!("login rejected: {reason}")),
        Closed::LoginRefused(None) => error("login rejected".into()),
        Closed::Timeout => error(format!(
            "login: no reply within {}s",
            crate::HANDSHAKE_TIMEOUT_SECS
        )),
        Closed::Protocol(what) => error(what),
        Closed::TooLarge(wire_len) => ShutdownReason::FrameTooLarge { wire_len },
        Closed::Hangup => error("hung up".into()),
    }
}

/// Trace what the session itself sends: the magic, then whole
/// transactions.
fn trace_out(mut out: &[u8]) {
    if out.starts_with(hxsession::CLIENT_MAGIC) {
        out = &out[hxsession::CLIENT_MAGIC.len()..];
    }
    let mut reader = hxsession::frame::FrameReader::new();
    reader.push(out);
    while let Ok(Some(t)) = reader.next_transaction() {
        trace(Dir::Out, &t.buf);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use hxsession::request::Request;
    use std::time::Duration;
    use tokio::io::{duplex, DuplexStream};

    const TASK: u32 = 0x0001_0000;
    const AGREEMENT: u32 = 0x6d;
    const AGREE: u32 = 121;
    const USER_CHANGE: u32 = 304;

    /// A transaction as a server writes it.
    fn server_says(opcode: u32, trans: u32, fields: &[(u16, &[u8])]) -> Vec<u8> {
        Request {
            opcode,
            fields: fields.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
        }
        .pack(trans)
        .unwrap()
    }

    fn login_reply(version: Option<u16>) -> Vec<u8> {
        let v = version.map(u16::to_be_bytes);
        let mut fields: Vec<(u16, &[u8])> = Vec::new();
        if let Some(v) = &v {
            fields.push((0x00a0, v));
        }
        server_says(TASK, 1, &fields)
    }

    /// The far end of a session actor, as a server sees it.
    struct Server {
        io: DuplexStream,
        reader: hxsession::frame::FrameReader,
    }

    impl Server {
        /// Take the client's magic and send ours.
        async fn accept(&mut self) {
            let mut magic = [0u8; 12];
            self.io.read_exact(&mut magic).await.unwrap();
            assert_eq!(&magic, hxsession::CLIENT_MAGIC);
            self.io.write_all(hxsession::SERVER_MAGIC).await.unwrap();
        }

        /// The next transaction the client sends: (opcode, trans).
        async fn next(&mut self) -> (u32, u32) {
            let mut buf = [0u8; 4096];
            loop {
                if let Some(t) = self.reader.next_transaction().unwrap() {
                    return (t.type_, t.trans);
                }
                let n = tokio::time::timeout(Duration::from_secs(5), self.io.read(&mut buf))
                    .await
                    .expect("the client sent nothing")
                    .unwrap();
                assert!(n > 0, "the client hung up");
                self.reader.push(&buf[..n]);
            }
        }

        async fn send(&mut self, bytes: &[u8]) {
            self.io.write_all(bytes).await.unwrap();
        }
    }

    struct Client {
        session: SharedSession,
        cmd: mpsc::Sender<Command>,
        events: mpsc::Receiver<Event>,
    }

    impl Client {
        /// The next event.
        async fn next(&mut self) -> Event {
            tokio::time::timeout(Duration::from_secs(5), self.events.recv())
                .await
                .expect("no event")
                .expect("the actor is gone")
        }

        async fn expect_state(&mut self, want: ConnectionState) {
            match self.next().await {
                Event::State(s) if s == want => {}
                other => panic!("wanted {want:?}, got {other:?}"),
            }
        }

        async fn expect_logged_in(&mut self, version: u16) {
            match self.next().await {
                Event::Session(hxsession::Event::LoggedIn(i)) if i.version == version => {}
                other => panic!("wanted the login, got {other:?}"),
            }
        }

        async fn expect_frame(&mut self, opcode: u32) {
            match self.next().await {
                Event::Frame(f) if f.header.type_ == opcode => {}
                other => panic!("wanted a frame of {opcode:#x}, got {other:?}"),
            }
        }
    }

    fn start() -> (Server, Client) {
        start_handling(hxsession::Handled::LOGIN, 0)
    }

    fn start_handling(handled: hxsession::Handled, caps: u64) -> (Server, Client) {
        let (near, far) = duplex(64 * 1024);
        let (cmd_tx, cmd_rx) = mpsc::channel(8);
        let (evt_tx, evt_rx) = mpsc::channel(8);
        let cfg = config(
            b"",
            b"",
            b"me",
            414,
            hxsession::CLIENT_VERSION,
            caps,
            handled,
        );
        let session = Arc::new(Mutex::new(Session::new(cfg, 0)));
        tokio::spawn(run(near, session.clone(), None, cmd_rx, evt_tx));
        (
            Server {
                io: far,
                reader: hxsession::frame::FrameReader::new(),
            },
            Client {
                cmd: cmd_tx,
                session,
                events: evt_rx,
            },
        )
    }

    /// Through the login to a 1.5 server, up to its agreement.
    async fn logged_in(server: &mut Server, client: &mut Client) {
        server.accept().await;
        assert_eq!(server.next().await, (107, 1));
        client.expect_state(ConnectionState::LoginSending).await;
        client.expect_state(ConnectionState::LoginReplyWait).await;
        server.send(&login_reply(Some(190))).await;
        client.expect_logged_in(190).await;
        client.expect_state(ConnectionState::HandshakeDone).await;
    }

    #[tokio::test]
    async fn an_agreement_waits_for_the_user_and_its_answer_settles_the_login() {
        let (mut server, mut client) = start();
        // Numbered and sent before the login is answered: it waits for it.
        let trans = client.session.lock().unwrap().take_trans();
        assert_eq!(trans, 2, "the login went out on 1");
        let early = Request::new(300).pack(trans).unwrap();
        client
            .cmd
            .send(Command::WriteFrame(early.clone()))
            .await
            .unwrap();
        logged_in(&mut server, &mut client).await;
        assert_eq!(server.next().await, (300, trans));

        server
            .send(&server_says(AGREEMENT, 0, &[(0x0065, b"Be nice.")]))
            .await;
        client.expect_frame(AGREEMENT).await;
        client
            .cmd
            .send(Command::Agree {
                nick: b"renamed".to_vec(),
                icon: 9,
            })
            .await
            .unwrap();
        // Numbered from the same counter as the consumer's.
        assert_eq!(server.next().await, (AGREE, 3));
        client.expect_state(ConnectionState::LoginReady).await;

        // Its answer reaches the consumer too.
        server.send(&server_says(TASK, trans, &[])).await;
        server.send(&server_says(TASK, 3, &[])).await;
        for trans in [trans, 3] {
            match client.next().await {
                Event::Frame(f) => assert_eq!(f.header.trans, trans),
                other => panic!("{other:?}"),
            }
        }
    }

    /// Where the server agreed to the friend family, its pushes and the
    /// replies to its requests reach the consumer as the session's; where
    /// it did not, a push is a frame like any unknown transaction.
    #[tokio::test]
    async fn the_messaging_extension_reaches_the_consumer_as_the_sessions() {
        const MESSAGING: u16 = hxproto::messaging::CAP_MESSAGING as u16;
        for agreed in [true, false] {
            let (mut server, mut client) =
                start_handling(hxsession::Handled::LOGIN, MESSAGING.into());
            server.accept().await;
            server.next().await;
            let (version, caps) = (190u16.to_be_bytes(), MESSAGING.to_be_bytes());
            let mut reply: Vec<(u16, &[u8])> = vec![(0x00a0, &version)];
            if agreed {
                reply.push((0x01f0, &caps));
            }
            server.send(&server_says(TASK, 1, &reply)).await;
            for state in [
                ConnectionState::LoginSending,
                ConnectionState::LoginReplyWait,
            ] {
                client.expect_state(state).await;
            }
            client.expect_logged_in(190).await;
            client.expect_state(ConnectionState::HandshakeDone).await;

            let send = |opcode: u32| {
                let trans = client.session.lock().unwrap().take_trans();
                (trans, Request::new(opcode).pack(trans).unwrap())
            };
            let (roster, roster_frame) = send(800);
            let (users, users_frame) = send(300);
            for f in [roster_frame, users_frame] {
                client.cmd.send(Command::WriteFrame(f)).await.unwrap();
                server.next().await;
            }
            server.send(&server_says(TASK, users, &[])).await;
            server.send(&server_says(TASK, roster, &[])).await;
            server.send(&server_says(809, 0, &[(0x0600, b"bob")])).await;
            match client.next().await {
                Event::Frame(f) => assert_eq!(f.header.trans, users),
                other => panic!("{other:?}"),
            }
            match client.next().await {
                Event::Session(hxsession::Event::Reply { trans, .. }) => {
                    assert_eq!(trans, roster)
                }
                other => panic!("{other:?}"),
            }
            match (agreed, client.next().await) {
                (true, Event::Session(hxsession::Event::Unhandled { opcode: 809, .. })) => {}
                (false, Event::Frame(f)) if f.header.type_ == 809 => {}
                (_, other) => panic!("agreed {agreed}: {other:?}"),
            }
        }
    }

    /// Where the session has reactions, Reaction Changed and Notification
    /// reach the consumer as the session's, and where it has replies alone,
    /// Notification; a server that echoes a bit without the target kinds
    /// its draft requires, or never agreed, has them shown as frames. A
    /// reply to Get Reactions is the session's either way, by its trans.
    #[tokio::test]
    async fn the_drafts_pushes_reach_the_consumer_as_the_sessions_where_agreed() {
        use hxproto::messages::tag::{REACTION_TARGETS, REPLY_TARGETS};
        use hxproto::reactions::CHANGED;
        use hxproto::replies::NOTIFICATION;
        use hxsession::cap::{REACTIONS, REPLIES, STRUCTURED_CHAT, TEXT_ENCODING};
        let base = TEXT_ENCODING | STRUCTURED_CHAT;
        let (reacts, replies) = ((base | REACTIONS) as u32, (base | REPLIES) as u32);
        let reaction_kinds = hxproto::reactions::TARGETS_LINES.to_be_bytes();
        let reply_kinds = 0x0012u16.to_be_bytes();
        let all = base | REACTIONS | REPLIES;
        // (caps, the kinds beside them, 711 the session's, 718 the session's)
        for (caps, kinds, changed, notified) in [
            (
                Some(reacts),
                Some((REACTION_TARGETS, &reaction_kinds)),
                true,
                true,
            ),
            (Some(reacts), None, false, false),
            (
                Some(replies),
                Some((REPLY_TARGETS, &reply_kinds)),
                false,
                true,
            ),
            (Some(replies), None, false, false),
            (None, None, false, false),
        ] {
            let (mut server, mut client) = start_handling(hxsession::Handled::LOGIN, all);
            server.accept().await;
            server.next().await;
            let (version, caps) = (190u16.to_be_bytes(), caps.map(u32::to_be_bytes));
            let mut reply: Vec<(u16, &[u8])> = vec![(0x00a0, &version)];
            reply.extend(caps.as_ref().map(|c| (0x01f0, &c[..])));
            reply.extend(kinds.map(|(tag, k)| (tag, &k[..])));
            server.send(&server_says(TASK, 1, &reply)).await;
            for state in [
                ConnectionState::LoginSending,
                ConnectionState::LoginReplyWait,
            ] {
                client.expect_state(state).await;
            }
            client.expect_logged_in(190).await;
            client.expect_state(ConnectionState::HandshakeDone).await;

            let trans = client.session.lock().unwrap().take_trans();
            let get = Request::new(hxproto::reactions::GET).pack(trans).unwrap();
            client.cmd.send(Command::WriteFrame(get)).await.unwrap();
            server.next().await;
            server.send(&server_says(TASK, trans, &[])).await;
            match client.next().await {
                Event::Session(hxsession::Event::Reply { trans: t, .. }) => assert_eq!(t, trans),
                other => panic!("{other:?}"),
            }
            for (opcode, theirs) in [(CHANGED, changed), (NOTIFICATION, notified)] {
                server.send(&server_says(opcode, 0, &[])).await;
                match (theirs, client.next().await) {
                    (true, Event::Session(hxsession::Event::Unhandled { opcode: o, .. }))
                        if o == opcode => {}
                    (false, Event::Frame(f)) if f.header.type_ == opcode => {}
                    (_, other) => panic!("{caps:?} {opcode}: {other:?}"),
                }
            }
        }
    }

    #[tokio::test]
    async fn an_empty_agreement_is_answered_without_asking() {
        let (mut server, mut client) = start();
        logged_in(&mut server, &mut client).await;
        server
            .send(&server_says(AGREEMENT, 0, &[(0x009a, &[0, 1])]))
            .await;
        assert_eq!(server.next().await, (AGREE, 2));
        client.expect_frame(AGREEMENT).await;
        client.expect_state(ConnectionState::LoginReady).await;
    }

    #[tokio::test(start_paused = true)]
    async fn no_agreement_is_waited_on_for_two_seconds() {
        let (mut server, mut client) = start();
        logged_in(&mut server, &mut client).await;
        tokio::time::advance(Duration::from_millis(2_000)).await;
        client.expect_state(ConnectionState::LoginReady).await;
    }

    #[tokio::test]
    async fn a_1_2_server_gets_the_name_in_a_user_change() {
        let (mut server, mut client) = start();
        server.accept().await;
        server.next().await;
        server.send(&login_reply(None)).await;
        assert_eq!(server.next().await, (USER_CHANGE, 2));
        client.expect_state(ConnectionState::LoginSending).await;
        client.expect_state(ConnectionState::LoginReplyWait).await;
        client.expect_logged_in(0).await;
        client.expect_state(ConnectionState::HandshakeDone).await;
        client.expect_state(ConnectionState::LoginReady).await;
    }

    #[tokio::test]
    async fn a_refused_login_ends_the_connection_with_the_reason() {
        let (mut server, mut client) = start();
        server.accept().await;
        server.next().await;
        let mut refusal = server_says(TASK, 1, &[(0x0064, b"Incorrect login.")]);
        refusal[8..12].copy_from_slice(&1u32.to_be_bytes());
        server.send(&refusal).await;
        // The refusal reaches the consumer, then the end.
        let mut refusal_seen = false;
        loop {
            match client.next().await {
                Event::State(_) => continue,
                Event::Session(hxsession::Event::Closed(Closed::LoginRefused(Some(why)))) => {
                    assert_eq!(why, "Incorrect login.");
                    refusal_seen = true
                }
                Event::Shutdown(ShutdownReason::StreamError(why)) => {
                    assert!(refusal_seen);
                    assert_eq!(why, "login rejected: Incorrect login.");
                    break;
                }
                other => panic!("{other:?}"),
            }
        }
    }

    #[tokio::test(start_paused = true)]
    async fn a_quiet_connection_is_pinged_and_a_send_puts_the_ping_off() {
        let (mut server, mut client) = start();
        logged_in(&mut server, &mut client).await;
        server
            .send(&server_says(AGREEMENT, 0, &[(0x009a, &[0, 1])]))
            .await;
        assert_eq!(server.next().await.0, AGREE);
        client.expect_frame(AGREEMENT).await;
        client.expect_state(ConnectionState::LoginReady).await;

        // Each chat goes out before a ping would have, had it not put
        // the ping off.
        let mut trans = 0;
        for wait in [30, 59] {
            tokio::time::advance(Duration::from_secs(wait)).await;
            trans = client.session.lock().unwrap().take_trans();
            let chat = Request::new(105).pack(trans).unwrap();
            client.cmd.send(Command::WriteFrame(chat)).await.unwrap();
            assert_eq!(server.next().await, (105, trans));
        }
        tokio::time::advance(Duration::from_secs(60)).await;
        assert_eq!(server.next().await, (500, trans + 1));
        // The ping's answer is the session's own.
        server.send(&server_says(TASK, trans + 1, &[])).await;
        server.send(&server_says(0x6a, 0, &[])).await;
        client.expect_frame(0x6a).await;
    }

    #[tokio::test]
    async fn what_the_session_handles_arrives_among_the_frames_in_order() {
        let (mut server, mut client) =
            start_handling(hxsession::Handled::CHAT | hxsession::Handled::LOGIN, 0);
        logged_in(&mut server, &mut client).await;
        let sent = [
            server_says(0x6a, 0, &[(0x0065, b"one")]),
            server_says(0x12d, 0, &[]),
            server_says(0x77, 0, &[(0x0073, b"two")]),
        ];
        server.send(&sent.concat()).await;
        match client.next().await {
            Event::Session(hxsession::Event::Chat { text, .. }) => assert_eq!(text, "one"),
            other => panic!("{other:?}"),
        }
        client.expect_frame(0x12d).await;
        match client.next().await {
            Event::Session(hxsession::Event::ChatSubject { subject, .. }) => {
                assert_eq!(subject, "two")
            }
            other => panic!("{other:?}"),
        }
    }

    /// Read past states and frames to how the connection ended.
    async fn ending(client: &mut Client) -> ShutdownReason {
        loop {
            match client.next().await {
                Event::Shutdown(why) => return why,
                _ => continue,
            }
        }
    }

    #[tokio::test]
    async fn how_the_connection_ends_says_why() {
        let chat = server_says(0x6a, 0, &[(0x0065, b"hello")]);
        let mut huge = server_says(0x6a, 0, &[]);
        let claim = (hxsession::frame::MAX_TRANSACTION as u32 + 1).to_be_bytes();
        huge[12..16].copy_from_slice(&claim);
        huge[16..20].copy_from_slice(&claim);
        // What the server sends before it hangs up, and how that reads.
        let cases: [(&[u8], &str); 3] = [
            (&[], "Eof"),
            (&chat[..10], r#"StreamError("EOF mid-frame")"#),
            (&huge, "FrameTooLarge { wire_len: 1048577 }"),
        ];
        for (sent, want) in cases {
            let (mut server, mut client) = start();
            logged_in(&mut server, &mut client).await;
            server.send(sent).await;
            drop(server);
            assert_eq!(format!("{:?}", ending(&mut client).await), want);
        }
    }

    /// A device proved in the login dance, against a scripted server: the
    /// challenge, its key checked against the pin, the proof, and the agree
    /// held until it is settled.
    mod device {
        use super::*;
        use crate::device::{prover, Armed};
        use hl_identity::{DeviceProof, ServerChallenge, ServerKey};
        use hxproto::messages::tag;
        use hxproto::messaging::CAP_DEVICE_PROOF;

        const HOST: &str = "hl.example";
        const PORT: u16 = 5500;
        const ASKED: &str = "hl.example:5500";
        const CHALLENGE: u32 = 1020;
        const PROVE: u32 = 1021;
        const TEXT: u64 = hxsession::cap::TEXT_ENCODING;

        fn unix() -> u64 {
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_secs()
        }

        fn armed(dir: &std::path::Path) -> Armed {
            crate::device::armed_in(dir)
        }

        fn start_proving(armed: Armed, host: &str) -> (Server, Client) {
            let (near, far) = duplex(64 * 1024);
            let (cmd_tx, cmd_rx) = mpsc::channel(8);
            let (evt_tx, evt_rx) = mpsc::channel(8);
            let caps = crate::device::offered(TEXT, Some(&armed));
            let cfg = config(
                b"alice",
                b"pw",
                b"me",
                414,
                hxsession::CLIENT_VERSION,
                caps,
                hxsession::Handled::LOGIN,
            );
            let session = Arc::new(Mutex::new(Session::new(cfg, 0)));
            let p = prover(Some(armed), host, PORT);
            tokio::spawn(run(near, session.clone(), p, cmd_rx, evt_tx));
            (
                Server {
                    io: far,
                    reader: hxsession::frame::FrameReader::new(),
                },
                Client {
                    cmd: cmd_tx,
                    session,
                    events: evt_rx,
                },
            )
        }

        impl Server {
            /// The next transaction the client sends, whole.
            async fn next_whole(&mut self) -> hxsession::frame::Transaction {
                let mut buf = [0u8; 4096];
                loop {
                    if let Some(t) = self.reader.next_transaction().unwrap() {
                        return t;
                    }
                    let n = tokio::time::timeout(Duration::from_secs(15), self.io.read(&mut buf))
                        .await
                        .expect("the client sent nothing")
                        .unwrap();
                    assert!(n > 0, "the client hung up");
                    self.reader.push(&buf[..n]);
                }
            }

            /// Answer the login agreeing to `caps`, with an empty agreement
            /// after it, as hxd-ng does: the login's caps, as it offered them.
            async fn log_in(&mut self, caps: u64) -> u64 {
                self.accept().await;
                let login = self.next_whole().await;
                let offered = field(&login, tag::CAPABILITIES)
                    .map(|c| hxproto::parse::capabilities_decode(&c))
                    .unwrap_or(0);
                let mut buf = [0u8; 8];
                let c = hxproto::build::capabilities_encode(caps, &mut buf);
                let v = 190u16.to_be_bytes();
                self.send(&server_says(
                    TASK,
                    1,
                    &[(0x00a0, &v), (tag::CAPABILITIES, c)],
                ))
                .await;
                self.send(&server_says(AGREEMENT, 0, &[(tag::NOAGREEMENT, &[1])]))
                    .await;
                offered
            }
        }

        fn field(t: &hxsession::frame::Transaction, want: u16) -> Option<Vec<u8>> {
            hxproto::wire::ChunkIter::over_message(&t.buf, t.buf.len())
                .find(|c| c.tag == want)
                .map(|c| c.data.to_vec())
        }

        fn challenge(key: &ServerKey, host: &str) -> Vec<u8> {
            ServerChallenge::sign_trtp(key, &[7; 32], host, unix() + 60, None).unwrap()
        }

        impl Client {
            /// Events until a device proof's, which is returned; the agree
            /// must not have gone out before it, which the caller checks.
            async fn proof(&mut self) -> hxkeys::Proof {
                loop {
                    // Longer than the session holds a proof for.
                    let next = tokio::time::timeout(Duration::from_secs(30), self.events.recv());
                    match next.await.expect("no event").expect("the actor is gone") {
                        Event::DeviceProof(p) => return p,
                        Event::Shutdown(r) => panic!("shut down: {r:?}"),
                        _ => {}
                    }
                }
            }
        }

        #[tokio::test]
        async fn a_device_is_proved_before_the_agree_and_its_key_pinned() {
            let dir = tempfile::tempdir().unwrap();
            let a = armed(dir.path());
            let device = a.device.clone();
            let (mut server, mut client) = start_proving(a, HOST);
            let offered = server.log_in(TEXT | CAP_DEVICE_PROOF).await;
            assert_ne!(offered & CAP_DEVICE_PROOF, 0, "bit 14 offered");

            let ask = server.next_whole().await;
            assert_eq!(ask.type_, CHALLENGE, "before the agree");
            assert_eq!(
                field(&ask, tag::PROOF_HOST).as_deref(),
                Some(ASKED.as_bytes())
            );
            let key = ServerKey::from_seed(&[9; 32]);
            let c = challenge(&key, ASKED);
            server
                .send(&server_says(
                    TASK,
                    ask.trans,
                    &[(tag::SERVER_CHALLENGE, &c)],
                ))
                .await;

            let prove = server.next_whole().await;
            assert_eq!(prove.type_, PROVE, "still before the agree");
            assert_eq!(
                field(&prove, tag::DEVICE_CERT).as_deref(),
                Some(device.cert_bytes())
            );
            assert_eq!(
                field(&prove, tag::USER_CARD).as_deref(),
                Some(device.card_bytes())
            );
            let p = DeviceProof::parse(&field(&prove, tag::DEVICE_PROOF).unwrap()).unwrap();
            p.check(&[7; 32], &key.public(), ASKED, None, unix(), 300)
                .unwrap();
            server.send(&server_says(TASK, prove.trans, &[])).await;

            assert_eq!(
                client.proof().await,
                hxkeys::Proof::Proved {
                    identity: device.identity(),
                    device: device.device(),
                    name: "alice".into(),
                }
            );
            assert_eq!(server.next_whole().await.type_, AGREE, "then the agree");
            let pins = hxkeys::Pins::load(dir.path().join("server-keys.toml")).unwrap();
            assert_eq!(pins.pinned(ASKED).unwrap().key, key.public());
        }

        /// What the server says, and how the proof ends: nothing more is
        /// asked, and the agree follows.
        #[tokio::test]
        async fn a_proof_that_ends_short_lets_the_session_join() {
            #[derive(Debug, Clone, Copy)]
            enum Case {
                NoChallenge,
                Refused,
                ForAnotherHost,
                KeyChanged,
            }
            for case in [
                Case::NoChallenge,
                Case::Refused,
                Case::ForAnotherHost,
                Case::KeyChanged,
            ] {
                let dir = tempfile::tempdir().unwrap();
                let a = armed(dir.path());
                let pinned = ServerKey::from_seed(&[8; 32]);
                if let Case::KeyChanged = case {
                    let c = ServerChallenge::parse_trtp(&challenge(&pinned, ASKED)).unwrap();
                    hxkeys::Pins::load(&a.pins)
                        .unwrap()
                        .check(ASKED, &c, &[] as &[Vec<u8>], unix())
                        .unwrap();
                }
                let (mut server, mut client) = start_proving(a, HOST);
                server.log_in(TEXT | CAP_DEVICE_PROOF).await;
                let ask = server.next_whole().await;
                assert_eq!(ask.type_, CHALLENGE, "{case:?}");
                let key = ServerKey::from_seed(&[9; 32]);
                let refusal = |trans, text: &[u8]| {
                    let mut r = server_says(TASK, trans, &[(0x0064, text)]);
                    r[8..12].copy_from_slice(&1u32.to_be_bytes());
                    r
                };
                match case {
                    Case::NoChallenge => {
                        server
                            .send(&refusal(ask.trans, b"no challenge for that address"))
                            .await
                    }
                    Case::ForAnotherHost => {
                        let c = challenge(&key, "other.example:5500");
                        server
                            .send(&server_says(
                                TASK,
                                ask.trans,
                                &[(tag::SERVER_CHALLENGE, &c)],
                            ))
                            .await
                    }
                    Case::KeyChanged | Case::Refused => {
                        let c = challenge(&key, ASKED);
                        server
                            .send(&server_says(
                                TASK,
                                ask.trans,
                                &[(tag::SERVER_CHALLENGE, &c)],
                            ))
                            .await
                    }
                }
                if let Case::Refused = case {
                    let prove = server.next_whole().await;
                    assert_eq!(prove.type_, PROVE);
                    server
                        .send(&refusal(prove.trans, b"The device could not be proved."))
                        .await;
                }
                let p = client.proof().await;
                let ok = match case {
                    Case::NoChallenge => {
                        p == hxkeys::Proof::Refused {
                            reason: Some("no challenge for that address".into()),
                        }
                    }
                    Case::Refused => {
                        p == hxkeys::Proof::Refused {
                            reason: Some("The device could not be proved.".into()),
                        }
                    }
                    Case::ForAnotherHost => matches!(p, hxkeys::Proof::NotProved { .. }),
                    Case::KeyChanged => {
                        matches!(&p, hxkeys::Proof::KeyChanged { host, challenge, .. }
                            if host == ASKED && challenge.server_key == key.public())
                    }
                };
                assert!(ok, "{case:?}: {p:?}");
                assert_eq!(
                    server.next_whole().await.type_,
                    AGREE,
                    "{case:?}: nothing signed"
                );
            }
        }

        #[tokio::test]
        async fn a_host_that_cannot_be_asked_for_asks_nothing() {
            let dir = tempfile::tempdir().unwrap();
            let (mut server, mut client) = start_proving(armed(dir.path()), "not a host/");
            server.log_in(TEXT | CAP_DEVICE_PROOF).await;
            assert!(matches!(
                client.proof().await,
                hxkeys::Proof::NotProved { .. }
            ));
            assert_eq!(server.next_whole().await.type_, AGREE);
        }

        #[tokio::test(start_paused = true)]
        async fn a_server_that_never_answers_is_given_up_on() {
            let dir = tempfile::tempdir().unwrap();
            let (mut server, mut client) = start_proving(armed(dir.path()), HOST);
            server.log_in(TEXT | CAP_DEVICE_PROOF).await;
            assert_eq!(server.next_whole().await.type_, CHALLENGE);
            assert_eq!(client.proof().await, hxkeys::Proof::TimedOut);
            assert_eq!(server.next_whole().await.type_, AGREE);
        }

        impl Server {
            /// Whether the client sends anything before `wait` is out.
            async fn quiet_for(&mut self, wait: Duration) -> bool {
                let mut buf = [0u8; 4096];
                if self.reader.next_transaction().unwrap().is_some() {
                    return false;
                }
                tokio::time::timeout(wait, self.io.read(&mut buf))
                    .await
                    .is_err()
            }
        }

        /// The challenge answered once the session has given up on the
        /// proof: nothing is signed, and one ending is told.
        #[tokio::test(start_paused = true)]
        async fn a_challenge_answered_too_late_is_not_answered() {
            let dir = tempfile::tempdir().unwrap();
            let (mut server, mut client) = start_proving(armed(dir.path()), HOST);
            server.log_in(TEXT | CAP_DEVICE_PROOF).await;
            let ask = server.next_whole().await;
            assert_eq!(ask.type_, CHALLENGE);
            assert_eq!(client.proof().await, hxkeys::Proof::TimedOut);
            assert_eq!(server.next_whole().await.type_, AGREE);
            let key = ServerKey::from_seed(&[9; 32]);
            let c = challenge(&key, ASKED);
            server
                .send(&server_says(
                    TASK,
                    ask.trans,
                    &[(tag::SERVER_CHALLENGE, &c)],
                ))
                .await;
            server
                .send(&server_says(0x6a, 0, &[(0x0065, b"after")]))
                .await;
            loop {
                match client.next().await {
                    Event::DeviceProof(p) => panic!("told again: {p:?}"),
                    Event::Frame(f) if f.header.type_ == 0x6a => break,
                    _ => {}
                }
            }
            assert!(
                server.quiet_for(Duration::from_secs(5)).await,
                "no proof sent"
            );
        }

        /// A challenge checked while another GtkHx holds the pins' lock
        /// holds up neither the main thread's use of the session nor the
        /// session's own end of the wait; the answer, once it can be made,
        /// is not sent.
        // Real time on two threads: the check blocks one, as it would the
        // runtime's, and the session's wait is the session's own.
        #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
        async fn pins_locked_elsewhere_hold_up_neither_the_session_nor_its_wait() {
            let dir = tempfile::tempdir().unwrap();
            let held = std::fs::File::create(dir.path().join("server-keys.toml.lock")).unwrap();
            held.lock().unwrap();
            let (mut server, mut client) = start_proving(armed(dir.path()), HOST);
            server.log_in(TEXT | CAP_DEVICE_PROOF).await;
            let ask = server.next_whole().await;
            let key = ServerKey::from_seed(&[9; 32]);
            let c = challenge(&key, ASKED);
            server
                .send(&server_says(
                    TASK,
                    ask.trans,
                    &[(tag::SERVER_CHALLENGE, &c)],
                ))
                .await;
            // The main thread numbers a request while the check waits.
            let session = client.session.clone();
            let numbered = std::thread::spawn(move || session.lock().unwrap().take_trans());
            let started = std::time::Instant::now();
            while !numbered.is_finished() {
                assert!(
                    started.elapsed() < Duration::from_secs(5),
                    "the session is held"
                );
                std::thread::sleep(Duration::from_millis(10));
            }
            assert_eq!(
                server.next_whole().await.type_,
                AGREE,
                "the wait still ends"
            );
            // Nor what reaches the consumer meanwhile.
            assert_eq!(client.proof().await, hxkeys::Proof::TimedOut);
            server
                .send(&server_says(0x6a, 0, &[(0x0065, b"meanwhile")]))
                .await;
            let (mut ready, mut line) = (false, false);
            while !(ready && line) {
                match client.next().await {
                    Event::State(ConnectionState::LoginReady) => ready = true,
                    Event::Frame(f) if f.header.type_ == 0x6a => line = true,
                    Event::DeviceProof(p) => panic!("told again: {p:?}"),
                    _ => {}
                }
            }
            held.unlock().unwrap();
            assert!(
                server.quiet_for(Duration::from_secs(2)).await,
                "no proof sent"
            );
            server
                .send(&server_says(0x6a, 0, &[(0x0065, b"after")]))
                .await;
            loop {
                match client.next().await {
                    Event::DeviceProof(p) => panic!("told again: {p:?}"),
                    Event::Frame(f) if f.header.type_ == 0x6a => break,
                    _ => {}
                }
            }
        }

        /// The proof answered once the session has given up on it is not
        /// told again.
        #[tokio::test(start_paused = true)]
        async fn a_proof_answered_too_late_is_not_told() {
            let dir = tempfile::tempdir().unwrap();
            let (mut server, mut client) = start_proving(armed(dir.path()), HOST);
            server.log_in(TEXT | CAP_DEVICE_PROOF).await;
            let ask = server.next_whole().await;
            let key = ServerKey::from_seed(&[9; 32]);
            let c = challenge(&key, ASKED);
            server
                .send(&server_says(
                    TASK,
                    ask.trans,
                    &[(tag::SERVER_CHALLENGE, &c)],
                ))
                .await;
            let prove = server.next_whole().await;
            assert_eq!(prove.type_, PROVE);
            assert_eq!(client.proof().await, hxkeys::Proof::TimedOut);
            assert_eq!(server.next_whole().await.type_, AGREE);
            server.send(&server_says(TASK, prove.trans, &[])).await;
            server
                .send(&server_says(0x6a, 0, &[(0x0065, b"after")]))
                .await;
            loop {
                match client.next().await {
                    Event::DeviceProof(p) => panic!("told again: {p:?}"),
                    Event::Frame(f) if f.header.type_ == 0x6a => break,
                    _ => {}
                }
            }
        }

        #[tokio::test]
        async fn a_server_that_proves_no_device_is_logged_in_to_as_ever() {
            let dir = tempfile::tempdir().unwrap();
            let (mut server, mut client) = start_proving(armed(dir.path()), HOST);
            server.log_in(TEXT).await;
            assert_eq!(server.next_whole().await.type_, AGREE, "no challenge asked");
            loop {
                match client.next().await {
                    Event::State(ConnectionState::LoginReady) => break,
                    Event::DeviceProof(p) => panic!("no proof to end: {p:?}"),
                    _ => {}
                }
            }
        }
    }
}
