//! A device proved in the login dance (capability bit 14, hxd-ng's
//! `proposals/portable-friends.md` §3): Get Device Challenge, the
//! server's key checked against its pin, Prove Device, between the login
//! reply and the agree, which the session holds until the proof is
//! settled (hx-libs' `hxsession`). The keys and the pins are `hxkeys`';
//! this is where the connection, its exporter and its clock meet them.

use std::cell::RefCell;
use std::path::PathBuf;

use hxkeys::proof::{answer, proof_host, read_challenge};
use hxkeys::{Device, Outcome, Pins, Proof};
use hxsession::{Event, Session};

/// A device to prove with on one connect, and where the pins it checks
/// the server's key against are kept.
#[derive(Debug, Clone)]
pub struct Armed {
    pub device: Device,
    pub pins: PathBuf,
}

/// A device armed for the connect to `host` and `port`.
#[derive(Debug)]
pub struct Arming {
    pub host: String,
    pub port: u16,
    pub armed: Armed,
}

impl Arming {
    /// The device, for an open to `host` and `port`: none for another,
    /// which is not the connect it was armed for.
    pub(crate) fn to(self, host: &str, port: u16) -> Option<Armed> {
        (self.host == host && self.port == port).then_some(self.armed)
    }
}

thread_local! {
    static ARMED: RefCell<Option<Arming>> = const { RefCell::new(None) };
}

/// Prove with `armed` on the next connect made on this thread through the
/// C ABI's opens, if it goes to `host` and `port`.
pub fn arm(host: &str, port: u16, armed: Armed) {
    let arming = Arming {
        host: host.to_owned(),
        port,
        armed,
    };
    ARMED.with_borrow_mut(|a| *a = Some(arming));
}

/// Leave nothing armed for the next connect.
pub fn disarm() {
    ARMED.with_borrow_mut(|a| *a = None);
}

/// What [`arm`] left for this connect, taken: the opens', and public for
/// a caller's tests alone.
#[doc(hidden)]
pub fn take_armed() -> Option<Arming> {
    ARMED.with_borrow_mut(Option::take)
}

/// The capabilities a connect offers: `caps`, with bit 14 where it has a
/// device to prove with and never where it has none, since a server that
/// agrees holds the join for a proof.
pub(crate) fn offered(caps: u64, device: Option<&Armed>) -> u64 {
    match device {
        Some(_) => caps | hxproto::messaging::CAP_DEVICE_PROOF,
        None => caps & !hxproto::messaging::CAP_DEVICE_PROOF,
    }
}

/// The prover for a connect to `host` and `port` with `armed`.
pub(crate) fn prover(armed: Option<Armed>, host: &str, port: u16) -> Option<Prover> {
    armed.map(|a| Prover::new(a, host, port))
}

/// The step a request answers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Step {
    Challenge,
    Proof,
}

/// One connection's proof, run by the session's actor.
pub struct Prover {
    armed: Armed,
    /// The host Get Device Challenge names: as dialed, port included.
    host: Option<String>,
    /// The connection's TLS 1.3 exporter, where it has one.
    exporter: Option<[u8; 32]>,
    asked: Option<(u32, Step)>,
    /// How the proof ended has been told: one ending a session.
    over: bool,
}

/// What became of an event the session said.
pub(crate) enum Heard {
    /// Not the proof's: the consumer's, as ever.
    Pass(Event),
    /// The proof's, and how it ended if it did.
    Taken(Option<Proof>),
    /// The server's challenge, to be checked and answered off the session:
    /// [`Challenge::answer`] reads files, and the session is locked by the
    /// main thread too.
    Challenge(Box<Challenge>),
}

/// A challenge to check against its pin and answer, with all it needs.
pub(crate) struct Challenge {
    bytes: Vec<u8>,
    host: String,
    pins: PathBuf,
    device: Device,
    exporter: Option<[u8; 32]>,
    now: u64,
}

impl Challenge {
    /// Check the challenge for the host asked for and against the key
    /// pinned for it, and answer it with a device proof; or how the proof
    /// ends here instead.
    /// Blocking: the pins are read and written under a file lock, and the
    /// device's key is read from its file.
    pub(crate) fn answer(self: Box<Self>) -> Result<Vec<u8>, Proof> {
        let not = |why: String| Proof::NotProved { why };
        let c = read_challenge(&self.bytes, &self.host, self.now)
            .map_err(|e| not(format!("the server's challenge: {e}")))?;
        let mut pins = Pins::load(&self.pins).map_err(|e| not(e.to_string()))?;
        let checked = pins
            .check(&self.host, &c, &[] as &[Vec<u8>], self.now)
            .map_err(|e| not(e.to_string()))?;
        let trusted = match checked {
            Outcome::Pinned(t) | Outcome::Kept(t) | Outcome::Followed(t) => t,
            Outcome::Warn(warning) => {
                // As the check read it, under its lock.
                let pinned = pins.pinned(&self.host).cloned().map(Box::new);
                return Err(Proof::KeyChanged {
                    host: self.host,
                    warning,
                    challenge: Box::new(c),
                    pinned,
                });
            }
        };
        let key = self.device.key().map_err(|e| not(e.to_string()))?;
        answer(&key, &trusted, self.exporter.as_ref(), self.now).map_err(|e| not(e.to_string()))
    }
}

impl Prover {
    pub(crate) fn new(armed: Armed, host: &str, port: u16) -> Self {
        Prover {
            armed,
            host: proof_host(host, port),
            exporter: None,
            asked: None,
            over: false,
        }
    }

    pub(crate) fn bind(&mut self, exporter: [u8; 32]) {
        self.exporter = Some(exporter);
    }

    /// The login was answered: ask for a challenge if the session proves.
    pub(crate) fn logged_in(&mut self, s: &mut Session, now: u64) -> Option<Proof> {
        if !s.proving() {
            return None;
        }
        let asked = self
            .armed
            .device
            .usable(now)
            .map_err(|e| e.to_string())
            .and_then(|()| {
                let host = self.host.as_deref().ok_or("not a host to ask for")?;
                s.device_challenge(host).map_err(|e| format!("{e:?}"))
            });
        match asked {
            Ok(trans) => {
                self.asked = Some((trans, Step::Challenge));
                None
            }
            Err(why) => self.end(s, Proof::NotProved { why }),
        }
    }

    /// `e`, as the proof hears it.
    pub(crate) fn heard(&mut self, s: &mut Session, e: Event, now: u64) -> Heard {
        let step = |trans: u32| match self.asked {
            Some((asked, step)) if asked == trans => Some(step),
            _ => None,
        };
        let ours = match &e {
            Event::DeviceChallenge { trans, .. }
            | Event::DeviceProved { trans }
            | Event::Failed { trans, .. } => step(*trans),
            _ => None,
        };
        // The proof was told over once: an answer after that is no news.
        if ours.is_some() && self.over {
            return Heard::Taken(None);
        }
        match (e, ours) {
            (Event::DeviceChallenge { challenge, .. }, Some(Step::Challenge)) => {
                Heard::Challenge(Box::new(Challenge {
                    bytes: challenge,
                    host: self.host.clone().expect("a challenge was asked for a host"),
                    pins: self.armed.pins.clone(),
                    device: self.armed.device.clone(),
                    exporter: self.exporter,
                    now,
                }))
            }
            (Event::DeviceProved { .. }, Some(Step::Proof)) => {
                let d = &self.armed.device;
                let proved = Proof::Proved {
                    identity: d.identity(),
                    device: d.device(),
                    name: d.card.name.clone(),
                };
                Heard::Taken(self.end(s, proved))
            }
            (Event::Failed { reason, .. }, Some(_)) => {
                Heard::Taken(self.end(s, Proof::Refused { reason }))
            }
            (Event::DeviceProofTimedOut, _) if !self.over => {
                self.over = true;
                Heard::Taken(Some(Proof::TimedOut))
            }
            (e, _) => Heard::Pass(e),
        }
    }

    /// The challenge answered, or the proof ended, off the session: send
    /// the answer, unless the session settled the proof meanwhile, which
    /// it tells as its own, or its deadline is past at `now_ms`.
    pub(crate) fn answered(
        &mut self,
        s: &mut Session,
        a: Result<Vec<u8>, Proof>,
        now_ms: u64,
    ) -> Option<Proof> {
        // The session's own clock first, so an answer past its deadline is
        // not sent however late the session would otherwise notice.
        s.tick(now_ms);
        if self.over || !s.proving() {
            return None;
        }
        let d = &self.armed.device;
        // The card goes every time: a server checks the certificate
        // against the card it holds for the identity, and holds none
        // until one is shown it.
        let sent = a.and_then(|a| {
            s.prove_device(&a, d.cert_bytes(), Some(d.card_bytes()))
                .map_err(|e| Proof::NotProved {
                    why: format!("{e:?}"),
                })
        });
        match sent {
            Ok(trans) => {
                self.asked = Some((trans, Step::Proof));
                None
            }
            Err(p) => self.end(s, p),
        }
    }

    /// The proof is over, as `proof` says: what waited on it goes.
    fn end(&mut self, s: &mut Session, proof: Proof) -> Option<Proof> {
        self.asked = None;
        self.over = true;
        s.device_settled();
        Some(proof)
    }
}

/// A device of identity "alice", kept in `dir` as GtkHx keeps one, with
/// its pins beside it.
#[cfg(test)]
pub(crate) fn armed_in(dir: &std::path::Path) -> Armed {
    use hl_identity::{cert, Card, DeviceCert, DeviceKey, IdentityKey};
    let unix = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs();
    let id = IdentityKey::from_seed(&[1; 32]);
    let dev = DeviceKey::from_seed(&[2; 32]);
    let home = dir.join("hlid");
    std::fs::create_dir_all(&home).unwrap();
    let hex = |b: &[u8]| b.iter().map(|x| format!("{x:02x}")).collect::<String>();
    std::fs::write(home.join("device.key"), hex(&*dev.seed())).unwrap();
    let c = DeviceCert::for_device(&id, &dev, unix - 60, cert::RECOMMENDED_LIFETIME).unwrap();
    std::fs::write(home.join("cert.bin"), c.sign(&id)).unwrap();
    let card = Card::new(&id, "alice", unix).sign(&id, vec![]).unwrap();
    std::fs::write(home.join("card.bin"), card).unwrap();
    Armed {
        device: hxkeys::Store::new(dir.join("keys"))
            .import_hlid(&home, unix)
            .unwrap(),
        pins: dir.join("server-keys.toml"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use hxproto::messaging::CAP_DEVICE_PROOF;
    use hxsession::request::Request;

    const TEXT: u64 = hxsession::cap::TEXT_ENCODING;

    /// A device armed for one connect goes to no other host or port.
    #[test]
    fn a_device_armed_for_one_connect_goes_to_no_other() {
        let dir = tempfile::tempdir().unwrap();
        for (host, port, goes) in [
            ("hl.example", 5500, true),
            ("other.example", 5500, false),
            ("hl.example", 5600, false),
        ] {
            arm("hl.example", 5500, armed_in(dir.path()));
            let to = take_armed().unwrap().to(host, port);
            assert_eq!(to.is_some(), goes, "{host}:{port}");
        }
    }

    /// Only a connect with a device offers bit 14, whatever its caller asked.
    #[test]
    fn bit_14_is_offered_with_a_device_alone() {
        let dir = tempfile::tempdir().unwrap();
        let armed = armed_in(dir.path());
        for (caps, device, want) in [
            (TEXT, Some(&armed), TEXT | CAP_DEVICE_PROOF),
            (
                TEXT | CAP_DEVICE_PROOF,
                Some(&armed),
                TEXT | CAP_DEVICE_PROOF,
            ),
            (TEXT, None, TEXT),
            (TEXT | CAP_DEVICE_PROOF, None, TEXT),
        ] {
            assert_eq!(
                offered(caps, device),
                want,
                "{caps:#x} {}",
                device.is_some()
            );
        }
    }

    /// A session proving since `0` ms, by a 1.5 server agreeing to bit 14.
    fn proving() -> Session {
        let cfg = crate::session::config(
            b"alice",
            b"pw",
            b"me",
            414,
            hxsession::CLIENT_VERSION,
            TEXT | CAP_DEVICE_PROOF,
            hxsession::Handled::LOGIN,
        );
        let mut s = Session::new(cfg, 0);
        s.take_outgoing();
        s.feed(hxsession::SERVER_MAGIC, 0);
        s.take_outgoing();
        let mut buf = [0u8; 8];
        let caps = hxproto::build::capabilities_encode(TEXT | CAP_DEVICE_PROOF, &mut buf);
        let reply = Request {
            opcode: 0x0001_0000,
            fields: vec![
                (0x00a0, 190u16.to_be_bytes().to_vec()),
                (0x01f0, caps.to_vec()),
            ],
        };
        s.feed(&reply.pack(1).unwrap(), 0);
        assert!(s.proving());
        s
    }

    fn opcodes(s: &mut Session) -> Vec<u32> {
        let mut r = hxsession::frame::FrameReader::new();
        r.push(&s.take_outgoing());
        std::iter::from_fn(|| r.next_transaction().unwrap())
            .map(|t| t.type_)
            .collect()
    }

    /// An answer worked out past the session's deadline is not sent, even
    /// where the session has not yet been ticked past it.
    #[test]
    fn an_answer_past_the_deadline_is_not_sent() {
        let dir = tempfile::tempdir().unwrap();
        let mut s = proving();
        let mut p = Prover::new(armed_in(dir.path()), "hl.example", 5500);
        assert_eq!(p.answered(&mut s, Ok(vec![1; 64]), 10_000), None);
        assert!(!opcodes(&mut s).contains(&1021), "no proof sent");
        // And one in time is.
        let mut s = proving();
        let mut p = Prover::new(armed_in(dir.path()), "hl.example", 5500);
        assert_eq!(p.answered(&mut s, Ok(vec![1; 64]), 9_999), None);
        assert_eq!(opcodes(&mut s), [1021]);
    }
}
