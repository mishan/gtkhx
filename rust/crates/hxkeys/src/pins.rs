//! The server keys GtkHx pins, per host (`server-key-handover.md` §5):
//! each host's key, the commitment to the key after it, the handovers
//! followed or taken at the first pin, and the forks the user accepted.
//! Kept in one TOML file; the decisions are hl-identity's.
//!
//! A host is keyed as Get Device Challenge names it, host and port
//! ([`crate::proof::proof_host`]), however it was written: a server's plain
//! and TLS listeners are two hosts, each pinned on the challenge it
//! signed, and a host without a port is none. Every change is
//! made under a lock beside the file, on what the file holds then, so
//! that two GtkHx processes sharing it lose neither's pins.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use hl_identity::handover::{self, Followed, Pinned, Verdict, Warning};
use hl_identity::keys::PublicKey;
use hl_identity::proof::canonical_host;
use hl_identity::ServerChallenge;
use serde::{Deserialize, Serialize};

use crate::files::{hex, lock, unhex32, write_whole, PINS_TEMP};
use crate::proof::host_key;
use crate::Error;

const VERSION: u32 = 1;

/// What is remembered of one host.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pin {
    pub key: PublicKey,
    pub commitment: Option<[u8; 32]>,
    pub followed: Vec<Followed>,
    pub dismissed: Vec<[u8; 32]>,
}

/// A challenge whose key the pins trust for its host: the only thing
/// [`crate::proof::answer`] signs for.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Trusted(ServerChallenge);

impl Trusted {
    pub fn challenge(&self) -> &ServerChallenge {
        &self.0
    }
}

/// What [`Pins::check`] made of a challenge.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outcome {
    /// The first contact with this host: its key is pinned now.
    Pinned(Trusted),
    /// The pinned key, as it was.
    Kept(Trusted),
    /// A key the served handovers carry the pin to, followed without a
    /// warning.
    Followed(Trusted),
    /// Not to be signed for unless the user accepts the warning, with
    /// [`Pins::accept`]. Nothing was changed.
    Warn(Warning),
}

pub struct Pins {
    path: PathBuf,
    hosts: BTreeMap<String, Pin>,
}

impl Pins {
    /// The pins kept at `path`; none if it is not there yet.
    pub fn load(path: impl Into<PathBuf>) -> Result<Pins, Error> {
        let path = path.into();
        let hosts = read(&path)?;
        Ok(Pins { path, hosts })
    }

    /// What is pinned for `host`, as of the last read of the file.
    pub fn pinned(&self, host: &str) -> Option<&Pin> {
        self.hosts.get(&host_key(host)?)
    }

    /// Check `challenge`, already read for `host`, against its pin, with
    /// `chain` the server's handovers as served, oldest first (none where
    /// the server serves none): a first contact pins it, and a key change
    /// the chain carries is followed, both written down before this
    /// returns.
    pub fn check<B: AsRef<[u8]>>(
        &mut self,
        host: &str,
        challenge: &ServerChallenge,
        chain: &[B],
        now: u64,
    ) -> Result<Outcome, Error> {
        let host = &for_host(host, challenge)?;
        let trusted = || Trusted(challenge.clone());
        self.change(host, |pin| match pin {
            None => {
                let followed = handover::pin(chain, challenge, now);
                (Some(first(challenge, followed)), Outcome::Pinned(trusted()))
            }
            Some(pin) => {
                let pinned = Pinned {
                    key: pin.key,
                    commitment: pin.commitment,
                    followed: &pin.followed,
                    dismissed: &pin.dismissed,
                };
                match handover::check(&pinned, chain, challenge, now) {
                    Verdict::Keep => (None, Outcome::Kept(trusted())),
                    Verdict::Warn(w) => (None, Outcome::Warn(w)),
                    Verdict::Follow {
                        key,
                        commitment,
                        followed,
                    } => {
                        let mut pin = pin.clone();
                        pin.key = key;
                        pin.commitment = commitment;
                        pin.followed.extend(followed);
                        (Some(pin), Outcome::Followed(trusted()))
                    }
                }
            }
        })
    }

    /// The user accepted `warning` for `host`: pin `challenge`'s key and
    /// commitment again, taking no aliases from `chain`, and keeping the
    /// records held (§5.3, §5.4). A fork accepted is not warned of again.
    /// A chain that forks against the records otherwise is refused with
    /// its own warning, which changes nothing. `was` is the pin the user
    /// was shown, none for none: one that has changed since is refused
    /// with [`Error::PinChanged`], which changes nothing.
    pub fn accept<B: AsRef<[u8]>>(
        &mut self,
        host: &str,
        was: Option<&Pin>,
        challenge: &ServerChallenge,
        chain: &[B],
        warning: &Warning,
    ) -> Result<Result<Trusted, Warning>, Error> {
        let host = &for_host(host, challenge)?;
        self.change(host, |pin| {
            if pin != was {
                return (None, Err(Error::PinChanged));
            }
            let mut pin = pin.cloned().unwrap_or_else(|| first(challenge, Vec::new()));
            if let Warning::Fork { sha256 } = warning {
                if !pin.dismissed.contains(sha256) {
                    pin.dismissed.push(*sha256);
                }
            }
            if let Err(w) = handover::repin(chain, &pin.followed, &pin.dismissed) {
                return (None, Ok(Err(w)));
            }
            pin.key = challenge.server_key;
            pin.commitment = challenge.next;
            (Some(pin), Ok(Ok(Trusted(challenge.clone()))))
        })?
    }

    /// Forget `host`, so that the next contact is a first one.
    pub fn forget(&mut self, host: &str) -> Result<(), Error> {
        let host = host_key(host).ok_or(Error::WrongHost)?;
        let _held = lock(&self.lock_path())?;
        self.hosts = read(&self.path)?;
        if self.hosts.remove(&host).is_some() {
            self.save()?;
        }
        Ok(())
    }

    /// Under the lock, on the file as it is now: `decide` on `host`'s pin,
    /// and write the one it gives back, if any, beside the rest unchanged.
    fn change<T>(
        &mut self,
        host: &str,
        decide: impl FnOnce(Option<&Pin>) -> (Option<Pin>, T),
    ) -> Result<T, Error> {
        let _held = lock(&self.lock_path())?;
        self.hosts = read(&self.path)?;
        let (pin, out) = decide(self.hosts.get(host));
        if let Some(pin) = pin {
            self.hosts.insert(host.to_owned(), pin);
            self.save()?;
        }
        Ok(out)
    }

    fn lock_path(&self) -> PathBuf {
        let mut name = self.path.file_name().unwrap_or_default().to_owned();
        name.push(".lock");
        self.path.with_file_name(name)
    }

    fn save(&self) -> Result<(), Error> {
        let file = File {
            version: VERSION,
            host: self
                .hosts
                .iter()
                .map(|(host, pin)| HostEntry::of(host, pin))
                .collect(),
        };
        let text = toml::to_string(&file).map_err(|e| unreadable(&self.path, e))?;
        Ok(write_whole(&self.path, text.as_bytes(), false, PINS_TEMP)?)
    }
}

/// `host`'s key, `challenge` having been signed for it.
fn for_host(host: &str, challenge: &ServerChallenge) -> Result<String, Error> {
    let key = host_key(host).ok_or(Error::WrongHost)?;
    match canonical_host(&key) {
        Some(h) if h == challenge.host => Ok(key),
        _ => Err(Error::WrongHost),
    }
}

fn read(path: &Path) -> Result<BTreeMap<String, Pin>, Error> {
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => String::new(),
        Err(e) => return Err(e.into()),
    };
    let file: File = toml::from_str(&text).map_err(|e| unreadable(path, e))?;
    let mut hosts = BTreeMap::new();
    for h in file.host {
        let pin = h.pin().ok_or_else(|| unreadable(path, &h.host))?;
        let key = host_key(&h.host).ok_or_else(|| unreadable(path, &h.host))?;
        if hosts.insert(key, pin).is_some() {
            return Err(unreadable(path, format!("{} twice", h.host)));
        }
    }
    Ok(hosts)
}

fn first(challenge: &ServerChallenge, followed: Vec<Followed>) -> Pin {
    Pin {
        key: challenge.server_key,
        commitment: challenge.next,
        followed,
        dismissed: Vec::new(),
    }
}

fn unreadable(path: &Path, e: impl std::fmt::Display) -> Error {
    Error::Unreadable {
        file: "server keys",
        why: format!("{}: {e}", path.display()),
    }
}

// The file, in hex throughout, so that it can be read and edited by hand.

#[derive(Serialize, Deserialize)]
struct File {
    #[serde(default = "version")]
    version: u32,
    #[serde(default)]
    host: Vec<HostEntry>,
}

fn version() -> u32 {
    VERSION
}

#[derive(Serialize, Deserialize)]
struct HostEntry {
    host: String,
    key: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    commitment: Option<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    dismissed: Vec<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    followed: Vec<Record>,
}

#[derive(Serialize, Deserialize)]
struct Record {
    old: String,
    sha256: String,
    time: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    next: Option<String>,
}

fn h32(s: &str) -> Option<[u8; 32]> {
    unhex32(s).map(|z| *z)
}

fn opt(s: &Option<String>) -> Option<Option<[u8; 32]>> {
    match s {
        Some(s) => h32(s).map(Some),
        None => Some(None),
    }
}

impl HostEntry {
    fn of(host: &str, pin: &Pin) -> Self {
        let hex = |b: &[u8]| hex(b).to_string();
        HostEntry {
            host: host.to_owned(),
            key: hex(&pin.key),
            commitment: pin.commitment.map(|c| hex(&c)),
            dismissed: pin.dismissed.iter().map(|d| hex(d)).collect(),
            followed: pin
                .followed
                .iter()
                .map(|f| Record {
                    old: hex(&f.old),
                    sha256: hex(&f.sha256),
                    time: f.time,
                    next: f.next.map(|n| hex(&n)),
                })
                .collect(),
        }
    }

    fn pin(&self) -> Option<Pin> {
        Some(Pin {
            key: h32(&self.key)?,
            commitment: opt(&self.commitment)?,
            dismissed: self
                .dismissed
                .iter()
                .map(|d| h32(d))
                .collect::<Option<_>>()?,
            followed: self
                .followed
                .iter()
                .map(|r| {
                    Some(Followed {
                        old: h32(&r.old)?,
                        sha256: h32(&r.sha256)?,
                        time: r.time,
                        next: opt(&r.next)?,
                    })
                })
                .collect::<Option<_>>()?,
        })
    }
}
