//! A logged-in connection to one server, over the production connect path.

use std::time::Duration;

use hxnet::lifecycle::{
    run_hope_lifecycle, run_plaintext_lifecycle, HopeOpenRequest, PlaintextOpenRequest,
};
use hxnet::{Command, Connection, ConnectionHandle, Event, Frame};
use hxproto::parse::HeaderDecoded;
use hxproto::wire::ChunkIter;
use hxrequest::Request;
use hxsession::{Expect, Handled};
use tokio::runtime::Runtime;
use tokio::sync::mpsc;

use crate::Server;

/// `HTLS_HDR_AGREEMENT` — the server's agreement, shown after the login.
const HTLS_HDR_AGREEMENT: u32 = 0x6d;
/// `HTLS_HDR_TASK` — the reply to a client request.
const HTLS_HDR_TASK: u32 = 0x0001_0000;

/// Whether `f` is the reply to request `trans`. Only a reply echoes a
/// request's number: a server's pushes count their own, as mhxd's do, and
/// may share it.
pub fn is_reply(f: &Frame, trans: u32) -> bool {
    f.header.type_ == HTLS_HDR_TASK && f.header.trans == trans
}
/// `HTLS_DATA_FILE_LIST` — one entry of a FILE_LIST reply.
const TAG_FILE_LIST: u16 = 0x00c8;
/// `HTLC_CAP_TEXT_ENCODING` / `HTLC_CAP_LARGE_FILES`.
pub const CAP_TEXT_ENCODING: u16 = 0x0002;
pub const CAP_LARGE_FILES: u16 = 0x0001;
/// The trans the LOGIN goes out on.
const LOGIN_TRANS: u32 = 1;
/// How long a reply may take. Generous: the rig's servers are shared by
/// parallel tests.
const REPLY_TIMEOUT: Duration = Duration::from_secs(15);

pub struct Client {
    server: &'static Server,
    rt: Runtime,
    handle: ConnectionHandle,
    events: mpsc::Receiver<Event>,
    /// What numbers requests, as in production.
    session: hxnet::session::SharedSession,
    login: hxsession::ServerInfo,
    /// What the session made of what came before the login settled.
    login_events: Vec<hxsession::Event>,
}

/// A server's reply to one request: the whole frame, header included, which is
/// what the `hxproto::parse` reply parsers take.
#[derive(Debug, Clone)]
pub struct Reply {
    pub header: HeaderDecoded,
    pub raw: Vec<u8>,
}

/// One FILE_LIST entry, decoded the way the files browser decodes it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub name: Vec<u8>,
    pub ftype: [u8; 4],
    pub size: u32,
}

impl Entry {
    pub fn is_folder(&self) -> bool {
        &self.ftype == b"fldr"
    }
}

impl Reply {
    fn from_frame(f: Frame) -> Self {
        let mut raw = vec![0u8; hxproto::HL_HDR_LEN];
        hxproto::build::pack_header(
            &mut raw,
            f.header.type_,
            f.header.trans,
            f.header.flag,
            f.header.hc,
            f.body.len() as u32,
        );
        raw.extend_from_slice(&f.body);
        Reply {
            header: f.header,
            raw,
        }
    }

    pub fn is_error(&self) -> bool {
        self.header.flag & 1 != 0
    }

    /// The server's error text, or an empty string.
    pub fn error_text(&self) -> String {
        hxproto::parse::parse_task_error(&self.raw, self.raw.len(), 4096)
            .map(|v| String::from_utf8_lossy(&v).into_owned())
            .unwrap_or_default()
    }

    /// The data of the first chunk tagged `tag`.
    pub fn chunk(&self, tag: u16) -> Option<&[u8]> {
        ChunkIter::over_message(&self.raw, self.raw.len())
            .find(|c| c.tag == tag)
            .map(|c| c.data)
    }

    /// A FILE_GETINFO reply, decoded as the Get Info dialog's receive handler
    /// decodes it.
    pub fn file_info(&self) -> hxproto::parse::FileGetInfo {
        hxproto::parse::parse_file_getinfo(&self.raw, self.raw.len(), 255, 31, 31, 255)
    }

    /// The entries of a FILE_LIST reply.
    pub fn file_list(&self) -> Vec<Entry> {
        let mut out = Vec::new();
        let mut off = hxproto::HL_HDR_LEN;
        while off + 4 <= self.raw.len() {
            let tag = u16::from_be_bytes([self.raw[off], self.raw[off + 1]]);
            let len = u16::from_be_bytes([self.raw[off + 2], self.raw[off + 3]]) as usize;
            if tag == TAG_FILE_LIST {
                let (e, _) = hxproto::parse::parse_file_list_entry(&self.raw, off)
                    .expect("a FILE_LIST entry the browser can't decode");
                out.push(Entry {
                    name: e.name.to_vec(),
                    ftype: e.ftype.to_be_bytes(),
                    size: e.fsize,
                });
            }
            off += 4 + len;
        }
        out
    }
}

impl Client {
    /// Log in to `server` as `login`, with `password` if the account has one,
    /// offering `caps` (`CAP_*`). Returns the server's refusal as the error.
    pub fn login(
        server: &'static Server,
        login: &str,
        password: Option<&str>,
        caps: u16,
    ) -> Result<Client, String> {
        Client::login_handling(server, login, password, caps, Handled::NONE, "hx-e2e")
    }

    /// As [`Client::login`], going by `nick`, with the session acting on
    /// `handled` itself, as production's does: what those domains bring
    /// arrives as `Event::Session`, among the frames. The login's reply is
    /// the session's, as in production.
    pub fn login_handling(
        server: &'static Server,
        login: &str,
        password: Option<&str>,
        caps: u16,
        handled: Handled,
        nick: &str,
    ) -> Result<Client, String> {
        let (rt, handle, events, cmd_rx, evt_tx) = channels()?;
        let req = PlaintextOpenRequest {
            host: server.host.to_string(),
            port: server.port,
            login: login.as_bytes().to_vec(),
            password: password.map(|p| p.as_bytes().to_vec()).unwrap_or_default(),
            name: nick.as_bytes().to_vec(),
            icon: 414,
            version: hxnet::login::CLIENT_VERSION,
            caps,
            trans: LOGIN_TRANS,
            proxy: None,
        };
        let session = req.session(handled | Handled::LOGIN);
        rt.spawn(run_plaintext_lifecycle(
            req,
            session.clone(),
            cmd_rx,
            evt_tx,
        ));
        Client::settle(server, nick, rt, handle, events, session)
    }

    /// As [`Client::login_handling`] for the server's guest, logging in
    /// over HOPE with `cipher`, as production does when the bookmark asks
    /// for a secure login.
    pub fn hope_guest(
        server: &'static Server,
        cipher: hxhope::Cipher,
        caps: u16,
        handled: Handled,
        nick: &str,
    ) -> Result<Client, String> {
        let (rt, handle, events, cmd_rx, evt_tx) = channels()?;
        let req = HopeOpenRequest {
            host: server.host.to_string(),
            port: server.port,
            login: Vec::new(),
            password: Vec::new(),
            name: nick.as_bytes().to_vec(),
            icon: 414,
            version: hxnet::login::CLIENT_VERSION,
            caps,
            cipher: Some(cipher),
            compression: None,
            proxy: None,
        };
        let session = req.session(handled | Handled::LOGIN);
        rt.spawn(run_hope_lifecycle(req, session.clone(), cmd_rx, evt_tx));
        let client = Client::settle(server, nick, rt, handle, events, session)?;
        // A server may answer HOPE with no cipher at all, and the login
        // would go through unsealed.
        let agreed = client
            .session
            .lock()
            .expect("the session lock is never held across a panic")
            .negotiated()
            .and_then(|n| n.cipher);
        if agreed != Some(cipher) {
            return Err(format!(
                "{}: HOPE agreed {agreed:?}, not {cipher:?}",
                server.name
            ));
        }
        Ok(client)
    }

    /// Wait out the login a lifecycle just started: its reply, then the
    /// agreement, until it has settled.
    fn settle(
        server: &'static Server,
        nick: &str,
        rt: Runtime,
        handle: ConnectionHandle,
        mut events: mpsc::Receiver<Event>,
        session: hxnet::session::SharedSession,
    ) -> Result<Client, String> {
        // What the LOGIN reply said. Not necessarily the first event: a
        // server can broadcast another user's arrival ahead of it.
        let login = rt
            .block_on(async {
                tokio::time::timeout(REPLY_TIMEOUT, async {
                    loop {
                        match events.recv().await {
                            Some(Event::Session(hxsession::Event::LoggedIn(info))) => {
                                return Ok(info);
                            }
                            Some(Event::Session(hxsession::Event::Closed(
                                hxsession::Closed::LoginRefused(why),
                            ))) => {
                                return Err(format!("login refused: {}", why.unwrap_or_default()))
                            }
                            Some(Event::Frame(_) | Event::State(_) | Event::Session(_)) => continue,
                            Some(Event::Shutdown(r)) => {
                                return Err(format!("connect failed: {r:?}"))
                            }
                            None => return Err("connection closed".to_string()),
                        }
                    }
                })
                .await
            })
            .map_err(|_| format!("{}: no LOGIN reply", server.name))?
            .map_err(|e| format!("{}: {e}", server.name))?;
        // Agree as a user would, and wait for the login to settle before
        // sending anything: hlservd hangs up on a request before then.
        let mut login_events = Vec::new();
        rt.block_on(async {
            tokio::time::timeout(REPLY_TIMEOUT, async {
                loop {
                    match events.recv().await {
                        Some(Event::Frame(f)) if f.header.type_ == HTLS_HDR_AGREEMENT => {
                            let agree = Command::Agree {
                                nick: nick.as_bytes().to_vec(),
                                icon: 414,
                            };
                            let _ = handle.send(agree).await;
                        }
                        Some(Event::State(hxnet::ConnectionState::LoginReady)) => return Ok(()),
                        Some(Event::Session(e)) => login_events.push(e),
                        Some(Event::Frame(_) | Event::State(_)) => continue,
                        Some(Event::Shutdown(r)) => return Err(format!("{r:?}")),
                        None => return Err("connection closed".to_string()),
                    }
                }
            })
            .await
        })
        .map_err(|_| format!("{}: the login never settled", server.name))?
        .map_err(|e| format!("{}: disconnected during login: {e}", server.name))?;
        Ok(Client {
            server,
            rt,
            handle,
            events,
            session,
            login,
            login_events,
        })
    }

    /// Log in as the server's guest (the empty login), offering `caps`.
    pub fn guest(server: &'static Server, caps: u16) -> Client {
        Client::login(server, "", None, caps).unwrap_or_else(|e| panic!("{e}"))
    }

    /// Log in with the server's file-admin account, offering `caps`. Panics
    /// when the server can't be reached or refuses: the rig is expected up.
    pub fn admin(server: &'static Server, caps: u16) -> Client {
        Client::login(server, server.admin, None, caps).unwrap_or_else(|e| panic!("{e}"))
    }

    pub fn server(&self) -> &'static Server {
        self.server
    }

    /// What the session made of what came after the login reply and
    /// before the login settled.
    pub fn login_events(&self) -> &[hxsession::Event] {
        &self.login_events
    }

    /// What the server's reply to the LOGIN said of it.
    pub fn server_info(&self) -> &hxsession::ServerInfo {
        &self.login
    }

    /// The capability bits both sides agreed to.
    pub fn caps(&self) -> u16 {
        self.login.caps
    }

    /// Whether names go out as UTF-8 on this connection.
    pub fn utf8(&self) -> bool {
        self.caps() & CAP_TEXT_ENCODING != 0
    }

    /// Send `req` and return its trans.
    pub fn send(&mut self, req: &Request) -> u32 {
        self.send_expecting(req, None)
    }

    /// Send `req`, having told the session what its reply is to become, as
    /// production does (`hxnet::ffi::connection_expect`).
    pub fn send_expecting(&mut self, req: &Request, expect: Option<Expect>) -> u32 {
        let trans = {
            let mut s = self
                .session
                .lock()
                .expect("the session lock is never held across a panic");
            let trans = s.take_trans();
            if let Some(what) = expect {
                s.expect(trans, what)
                    .unwrap_or_else(|e| panic!("expecting trans {trans}: {e:?}"));
            }
            trans
        };
        let frame = req.pack(trans);
        self.rt
            .block_on(self.handle.send(Command::WriteFrame(frame)))
            .expect("the connection is gone");
        trans
    }

    /// Wait for the reply to `trans`, skipping everything else the server
    /// sends meanwhile (other users' traffic, broadcasts).
    pub fn reply_to(&mut self, trans: u32) -> Result<Reply, String> {
        let name = self.server.name;
        let events = &mut self.events;
        self.rt.block_on(async {
            tokio::time::timeout(REPLY_TIMEOUT, async {
                loop {
                    match events.recv().await {
                        Some(Event::Frame(f)) if is_reply(&f, trans) => {
                            return Ok(Reply::from_frame(f));
                        }
                        Some(Event::Frame(_) | Event::State(_) | Event::Session(_)) => continue,
                        Some(Event::Shutdown(r)) => {
                            return Err(format!("{name}: disconnected: {r:?}"));
                        }
                        None => return Err(format!("{name}: connection closed")),
                    }
                }
            })
            .await
            .unwrap_or_else(|_| Err(format!("{name}: no reply to trans {trans}")))
        })
    }

    /// The next frame or session event, past state changes.
    pub fn next_event(&mut self) -> Result<Event, String> {
        let name = self.server.name;
        let events = &mut self.events;
        self.rt.block_on(async {
            tokio::time::timeout(REPLY_TIMEOUT, async {
                loop {
                    match events.recv().await {
                        Some(Event::State(_)) => continue,
                        Some(Event::Shutdown(r)) => {
                            return Err(format!("{name}: disconnected: {r:?}"));
                        }
                        Some(e) => return Ok(e),
                        None => return Err(format!("{name}: connection closed")),
                    }
                }
            })
            .await
            .unwrap_or_else(|_| Err(format!("{name}: nothing more arrived")))
        })
    }

    /// Send `req` and wait for its reply.
    pub fn try_request(&mut self, req: &Request) -> Result<Reply, String> {
        let trans = self.send(req);
        self.reply_to(trans)
    }

    /// Send `req` and wait for its reply; panics if none comes.
    pub fn request(&mut self, req: &Request) -> Reply {
        self.try_request(req).unwrap_or_else(|e| panic!("{e}"))
    }

    /// Move and/or rename `src` to `dst` the way the files browser does: each
    /// of `hxrequest::files::moves`' requests only after the one before it
    /// succeeded. Returns the first refusal, or the last reply.
    pub fn move_to(&mut self, src: &str, dst: &str) -> Reply {
        let mut last = None;
        for req in hxrequest::files::moves(src.as_bytes(), dst.as_bytes()) {
            let reply = self.request(&req);
            let failed = reply.is_error();
            last = Some(reply);
            if failed {
                break;
            }
        }
        last.expect("moving a path onto itself sends nothing")
    }

    /// Claim the transfer reference `ref_` a server granted and hang up, as a
    /// cancelled transfer does, waiting until the server has let go of it.
    /// mhxd keeps a global transfer slot for good for a reference that was
    /// never claimed, or whose claim it hadn't seen when the client
    /// disconnected (docs/mhxd-bugs.md), so this must finish before the
    /// client goes.
    pub fn cancel_transfer(&self, ref_: u32) {
        use std::io::{Read, Write};
        let mut preamble = [0u8; 24];
        let n = hxproto::build::build_htxf_preamble(&mut preamble, ref_, 0, 0, 0, false);
        assert!(n > 0, "no HTXF preamble for ref {ref_:#x}");
        let addr = (self.server.host, self.server.xfer_port);
        let mut sock = std::net::TcpStream::connect(addr)
            .unwrap_or_else(|e| panic!("{}: transfer port: {e}", self.server.name));
        sock.write_all(&preamble[..n])
            .unwrap_or_else(|e| panic!("{}: transfer preamble: {e}", self.server.name));
        // Stop sending, and read until the server closes its end: by then it
        // has matched the reference and wound the transfer down.
        let _ = sock.shutdown(std::net::Shutdown::Write);
        sock.set_read_timeout(Some(REPLY_TIMEOUT))
            .expect("a read timeout");
        let mut sink = [0u8; 4096];
        loop {
            match sock.read(&mut sink) {
                Ok(0) => break,
                Ok(_) => continue,
                Err(e) if e.kind() == std::io::ErrorKind::ConnectionReset => break,
                Err(e) => panic!("{}: transfer {ref_:#x} never closed: {e}", self.server.name),
            }
        }
    }

    /// The entries of the folder at `dir`; panics if the listing fails.
    pub fn list(&mut self, dir: &str) -> Vec<Entry> {
        let reply = self.request(&hxrequest::files::list(dir.as_bytes()).unwrap());
        assert!(
            !reply.is_error(),
            "{}: can't list {dir}: {}",
            self.server.name,
            reply.error_text()
        );
        reply.file_list()
    }

    /// The names in the folder at `dir`, as the server sent them.
    pub fn names(&mut self, dir: &str) -> Vec<Vec<u8>> {
        self.list(dir).into_iter().map(|e| e.name).collect()
    }
}

/// A connection's runtime and channels, as each login starts from.
type Channels = (
    Runtime,
    ConnectionHandle,
    mpsc::Receiver<Event>,
    mpsc::Receiver<Command>,
    mpsc::Sender<Event>,
);

fn channels() -> Result<Channels, String> {
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(1)
        .enable_all()
        .build()
        .map_err(|e| e.to_string())?;
    let (handle, events, cmd_rx, evt_tx) = Connection::make_channels();
    Ok((rt, handle, events, cmd_rx, evt_tx))
}

impl Drop for Client {
    fn drop(&mut self) {
        let _ = self.rt.block_on(self.handle.send(Command::Shutdown));
    }
}
