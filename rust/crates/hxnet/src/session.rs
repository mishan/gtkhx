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
//! - what it makes of the rest, the replies the consumer expected, and
//!   with the tap on each transaction as it came → `Event::Session`
//! - logged in → `ConnectionState::HandshakeDone`
//! - the login settled, the agreement answered or not waited on any
//!   longer → `ConnectionState::LoginReady`
//! - closed → the actor ends, with the reason as its `Shutdown`
//!
//! The agreement is shown from its frame; answering it is
//! [`Command::Agree`].

use std::sync::{Arc, Mutex, MutexGuard};

use hxsession::{Closed, Session};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::sync::{mpsc, Notify};
use tokio::time::Instant;

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
    caps: u16,
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
    /// Whether the login has gone out, for the state events the consumer
    /// keys its login task off.
    login_sent: Mutex<bool>,
    /// Wakes the write side: there is something to write.
    to_write: Notify,
    /// Wakes the delivery side: the session has something to say.
    to_deliver: Notify,
    /// Held while popping events and handing them on, so they reach the
    /// consumer in the session's order whichever side hands them on.
    delivering: tokio::sync::Mutex<()>,
}

impl Shared {
    fn session(&self) -> MutexGuard<'_, Session> {
        self.session
            .lock()
            .expect("the session lock is never held across a panic")
    }

    fn now(&self) -> u64 {
        self.start.elapsed().as_millis() as u64
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
        to_deliver: Notify::new(),
        delivering: tokio::sync::Mutex::new(()),
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
        let event = shared.session().poll_event()?;
        let out = match event {
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
            hxsession::Event::LoggedIn(_) => Event::State(ConnectionState::HandshakeDone),
            hxsession::Event::Ready => Event::State(ConnectionState::LoginReady),
            hxsession::Event::Closed(why) => return Some(closed(why)),
            // The agreement reaches the consumer as its frame.
            hxsession::Event::Agreement(_) => continue,
            e => Event::Session(e),
        };
        if evt_tx.send(out).await.is_err() {
            return Some(ShutdownReason::HandleDropped);
        }
    }
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

        async fn expect_frame(&mut self, opcode: u32) {
            match self.next().await {
                Event::Frame(f) if f.header.type_ == opcode => {}
                other => panic!("wanted a frame of {opcode:#x}, got {other:?}"),
            }
        }
    }

    fn start() -> (Server, Client) {
        start_handling(hxsession::Handled::NONE)
    }

    fn start_handling(handled: hxsession::Handled) -> (Server, Client) {
        let (near, far) = duplex(64 * 1024);
        let (cmd_tx, cmd_rx) = mpsc::channel(8);
        let (evt_tx, evt_rx) = mpsc::channel(8);
        let cfg = config(b"", b"", b"me", 414, hxsession::CLIENT_VERSION, 0, handled);
        let session = Arc::new(Mutex::new(Session::new(cfg, 0)));
        tokio::spawn(run(near, session.clone(), cmd_rx, evt_tx));
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
        client.expect_frame(TASK).await;
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
        client.expect_frame(TASK).await;
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
        // The refusal reaches the consumer whole, then the end.
        let mut refusal_seen = false;
        loop {
            match client.next().await {
                Event::State(_) => continue,
                Event::Frame(f) if f.header.type_ == TASK && f.header.flag == 1 => {
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
        let (mut server, mut client) = start_handling(hxsession::Handled::CHAT);
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
}
