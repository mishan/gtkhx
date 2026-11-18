//! The devices GtkHx proves with, one for each identity it holds.
//!
//! The store is a directory of identities, each named by its fingerprint
//! and holding what `hlid init` writes beside an identity key, without
//! the identity key: `device.key` (the device's seed, in hex, the owner's
//! alone), `cert.bin` and `card.bin`. `pending/` holds the seeds of
//! devices made here and waiting for a holder of their identity to
//! certify them.
//!
//! Every change is made under `.lock` in the store's root, an identity
//! put whole: written into a directory beside its place and renamed into
//! it, the device it replaces set aside as `.old-` and its fingerprint
//! until then. Whoever next takes the lock finishes what a crash left: a
//! device set aside with nothing in its place goes back, and what was
//! being written is removed.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use hl_identity::{caps, Bundle, Card, DeviceCert, DeviceKey, Fingerprint};
use hxproto::messaging::{MAX_DEVICE_CERT_LEN, MAX_USER_CARD_LEN};
use zeroize::Zeroizing;

use crate::files::{hex, lock, private_dir, sync_dir, temp_beside, unhex32, write_whole, TEMP};
use crate::proof::SKEW;
use crate::Error;

const DEVICE_KEY: &str = "device.key";
const CERT: &str = "cert.bin";
const CARD: &str = "card.bin";
const PENDING: &str = "pending";
const LOCK: &str = ".lock";
const OLD: &str = ".old-";
/// How long a device begun here waits for its certificate before it is
/// given up on.
const PENDING_LIFE: Duration = Duration::from_secs(30 * 86_400);

pub struct Store {
    root: PathBuf,
}

/// An identity in [`Store::list`], and its device or why it does not read.
pub type Listed = (Fingerprint, Result<Device, Error>);

/// A device of one identity, checked as it was read. Its key is read only
/// when it signs.
#[derive(Debug, Clone)]
pub struct Device {
    pub cert: DeviceCert,
    pub card: Card,
    cert_bytes: Vec<u8>,
    card_bytes: Vec<u8>,
    dir: PathBuf,
}

impl Device {
    /// The identity's fingerprint, which names it in the store.
    pub fn identity(&self) -> Fingerprint {
        Fingerprint::of(&self.cert.identity)
    }

    pub fn device(&self) -> Fingerprint {
        Fingerprint::of(&self.cert.device)
    }

    /// The certificate as Prove Device sends it.
    pub fn cert_bytes(&self) -> &[u8] {
        &self.cert_bytes
    }

    /// The card, for a Prove Device that links.
    pub fn card_bytes(&self) -> &[u8] {
        &self.card_bytes
    }

    /// The certificate holds at `now`: not expired, begun, and still
    /// letting the device log in.
    pub fn usable(&self, now: u64) -> Result<(), Error> {
        valid(&self.cert, now)
    }

    /// The device's key, read from its file, to sign with.
    pub fn key(&self) -> Result<DeviceKey, Error> {
        let key = DeviceKey::from_seed(&*read_seed(&self.dir.join(DEVICE_KEY))?);
        if key.public() != self.cert.device || key.public_enc() != self.cert.device_enc {
            return Err(Error::WrongDevice);
        }
        Ok(key)
    }
}

/// A device made here, its seed kept until a holder of its identity
/// certifies it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pending {
    pub device: Fingerprint,
    pub public: [u8; 32],
    pub public_enc: [u8; 32],
}

impl Pending {
    /// What a holder of the identity key runs to certify it, writing the
    /// bundle [`Store::finish_device`] reads.
    pub fn hlid_command(&self, out: &str) -> String {
        format!(
            "hlid cert --device-pub {} --device-enc-pub {} --bundle -o {out}",
            *hex(&self.public),
            *hex(&self.public_enc)
        )
    }
}

impl Store {
    /// The store at `root`, made when something is first put in it.
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Store { root: root.into() }
    }

    /// Every identity held, each as it reads: one whose files are broken
    /// is listed with why, so it can be shown and removed.
    pub fn list(&self) -> Result<Vec<Listed>, Error> {
        let mut out = Vec::new();
        for (fp, _) in entries(&self.root)? {
            out.push((fp, self.get(&fp)));
        }
        out.sort_by_key(|(fp, _)| fp.to_string());
        Ok(out)
    }

    /// The device of the identity `fp`.
    pub fn get(&self, fp: &Fingerprint) -> Result<Device, Error> {
        let dir = self.root.join(fp.to_string());
        if !dir.is_dir() {
            return Err(Error::NotFound);
        }
        let device = read_device(&dir)?;
        if device.identity() != *fp {
            return Err(Error::WrongIdentity);
        }
        Ok(device)
    }

    /// Forget the identity `fp` and its device.
    pub fn remove(&self, fp: &Fingerprint) -> Result<(), Error> {
        let _held = self.lock()?;
        match fs::remove_dir_all(self.root.join(fp.to_string())) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Err(Error::NotFound),
            r => Ok(r?),
        }
    }

    /// Take the device an `hlid` home holds (`$HLID_HOME`, `~/.hlid` by
    /// default): its key, certificate and card, never the identity key
    /// beside them. Taken again, the same device renews its certificate
    /// and card here.
    pub fn import_hlid(&self, home: &Path, now: u64) -> Result<Device, Error> {
        let seed = read_seed(&home.join(DEVICE_KEY))?;
        let cert = read(&home.join(CERT), CERT)?;
        let card = read(&home.join(CARD), CARD)?;
        let _held = self.lock()?;
        self.put(&seed, cert, card, now)
    }

    /// A new device, to be certified by a holder of the identity key with
    /// [`Pending::hlid_command`] and finished with
    /// [`Store::finish_device`].
    pub fn begin_device(&self) -> Result<Pending, Error> {
        let key = DeviceKey::generate();
        let pending = pending_of(&key);
        let _held = self.lock()?;
        let dir = self.root.join(PENDING);
        private_dir(&dir)?;
        write_whole(
            &self.pending_path(&pending.device),
            &seed_file(&key.seed()),
            true,
        )?;
        Ok(pending)
    }

    /// The devices begun and not yet finished. A seed that does not read
    /// is passed over.
    pub fn pending(&self) -> Result<Vec<Pending>, Error> {
        let mut out: Vec<Pending> = pending_files(&self.root)?
            .into_iter()
            .filter_map(|path| read_seed(&path).ok())
            .map(|seed| pending_of(&DeviceKey::from_seed(&seed)))
            .collect();
        out.sort_by_key(|p| p.device.to_string());
        Ok(out)
    }

    /// The pending device `device`, certified by `bundle`, as `hlid cert
    /// --bundle` writes one: kept as its identity's device. Finished
    /// again with the same bundle, it is the device already kept.
    pub fn finish_device(
        &self,
        device: &Fingerprint,
        bundle: &[u8],
        now: u64,
    ) -> Result<Device, Error> {
        let bundle = Bundle::parse(bundle).map_err(|e| unreadable("bundle", e))?;
        let _held = self.lock()?;
        let path = self.pending_path(device);
        if !path.is_file() {
            let (cert, _) = bundle.open().map_err(|e| unreadable("bundle", e))?;
            return match self.get(&Fingerprint::of(&cert.identity)) {
                Ok(kept) if kept.device() == *device && kept.cert_bytes == bundle.cert => Ok(kept),
                _ => Err(Error::NotFound),
            };
        }
        let seed = read_seed(&path)?;
        let kept = self.put(&seed, bundle.cert, bundle.card, now)?;
        fs::remove_file(path)?;
        Ok(kept)
    }

    /// Give up on the pending device `device`.
    pub fn abandon_device(&self, device: &Fingerprint) -> Result<(), Error> {
        let _held = self.lock()?;
        match fs::remove_file(self.pending_path(device)) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Err(Error::NotFound),
            r => Ok(r?),
        }
    }

    fn pending_path(&self, device: &Fingerprint) -> PathBuf {
        self.root.join(PENDING).join(format!("{device}.key"))
    }

    /// The store's lock, with whatever a crash left finished first.
    fn lock(&self) -> Result<fs::File, Error> {
        private_dir(&self.root)?;
        let held = lock(&self.root.join(LOCK))?;
        self.recover()?;
        Ok(held)
    }

    /// Under the lock: a device set aside with nothing in its place goes
    /// back, one with its replacement in place goes; what was being
    /// written, and devices begun too long ago, are removed.
    fn recover(&self) -> Result<(), Error> {
        for entry in fs::read_dir(&self.root)? {
            let entry = entry?;
            let name = entry.file_name().to_string_lossy().into_owned();
            let path = entry.path();
            if let Some(fp) = name.strip_prefix(OLD) {
                let place = self.root.join(fp);
                if place.exists() {
                    fs::remove_dir_all(&path)?;
                } else {
                    fs::rename(&path, &place)?;
                }
            } else if name.starts_with(TEMP) {
                remove_any(&path)?;
            } else if entry.file_type()?.is_dir() {
                for inner in fs::read_dir(&path)? {
                    let inner = inner?;
                    if inner.file_name().to_string_lossy().starts_with(TEMP) {
                        remove_any(&inner.path())?;
                    }
                }
            }
        }
        let now = SystemTime::now();
        for path in pending_files(&self.root)? {
            let made = fs::metadata(&path)?.modified()?;
            if now.duration_since(made).is_ok_and(|age| age > PENDING_LIFE) {
                fs::remove_file(&path)?;
            }
        }
        Ok(())
    }

    /// Check a device whole and keep it under its identity, the lock held.
    fn put(
        &self,
        seed: &[u8; 32],
        cert: Vec<u8>,
        card: Vec<u8>,
        now: u64,
    ) -> Result<Device, Error> {
        let key = DeviceKey::from_seed(seed);
        let (c, k) = check(&key, &cert, &card)?;
        valid(&c, now)?;
        let place = self.root.join(Fingerprint::of(&c.identity).to_string());
        // Only a whole device stands in the way: what does not read as one
        // is replaced.
        if let Ok(kept) = read_device(&place) {
            if kept.cert.device != key.public() {
                return Err(Error::OtherDevice(kept.device()));
            }
            if (c.issued, c.expires) < (kept.cert.issued, kept.cert.expires) {
                return Err(Error::Older {
                    kept_issued: kept.cert.issued,
                });
            }
        }
        let tmp = temp_beside(&place);
        private_dir(&tmp)?;
        let written = write_whole(&tmp.join(DEVICE_KEY), &seed_file(seed), true)
            .and_then(|()| write_whole(&tmp.join(CERT), &cert, false))
            .and_then(|()| write_whole(&tmp.join(CARD), &card, false));
        if let Err(e) = written {
            let _ = fs::remove_dir_all(&tmp);
            return Err(e.into());
        }
        let aside = place.exists().then(|| {
            self.root
                .join(format!("{OLD}{}", Fingerprint::of(&c.identity)))
        });
        if let Some(aside) = &aside {
            fs::rename(&place, aside)?;
        }
        fs::rename(&tmp, &place)?;
        sync_dir(&self.root)?;
        if let Some(aside) = aside {
            fs::remove_dir_all(aside)?;
        }
        Ok(Device {
            cert: c,
            card: k,
            cert_bytes: cert,
            card_bytes: card,
            dir: place,
        })
    }
}

/// The identities' directories in `root`, by the fingerprint each is named.
fn entries(root: &Path) -> Result<Vec<(Fingerprint, PathBuf)>, Error> {
    let read = match fs::read_dir(root) {
        Ok(e) => e,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e.into()),
    };
    let mut out = Vec::new();
    for entry in read {
        let entry = entry?;
        let Some(fp) = entry.file_name().to_str().and_then(Fingerprint::parse) else {
            continue;
        };
        if entry.file_type()?.is_dir() {
            out.push((fp, entry.path()));
        }
    }
    Ok(out)
}

fn pending_files(root: &Path) -> Result<Vec<PathBuf>, Error> {
    let read = match fs::read_dir(root.join(PENDING)) {
        Ok(e) => e,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e.into()),
    };
    let mut out = Vec::new();
    for entry in read {
        let path = entry?.path();
        if path.extension().is_some_and(|e| e == "key") {
            out.push(path);
        }
    }
    Ok(out)
}

fn remove_any(path: &Path) -> std::io::Result<()> {
    if path.is_dir() {
        fs::remove_dir_all(path)
    } else {
        fs::remove_file(path)
    }
}

fn pending_of(key: &DeviceKey) -> Pending {
    Pending {
        device: key.fingerprint(),
        public: key.public(),
        public_enc: key.public_enc(),
    }
}

fn unreadable(file: &'static str, e: impl std::fmt::Display) -> Error {
    Error::Unreadable {
        file,
        why: e.to_string(),
    }
}

fn read(path: &Path, file: &'static str) -> Result<Vec<u8>, Error> {
    fs::read(path).map_err(|e| unreadable(file, e))
}

/// A seed as `hlid` writes one: 64 hex digits and a newline.
fn seed_file(seed: &[u8; 32]) -> Zeroizing<Vec<u8>> {
    let mut text = Zeroizing::new(hex(seed).as_bytes().to_vec());
    text.push(b'\n');
    text
}

fn read_seed(path: &Path) -> Result<Zeroizing<[u8; 32]>, Error> {
    let text = Zeroizing::new(fs::read_to_string(path).map_err(|e| unreadable(DEVICE_KEY, e))?);
    unhex32(text.trim()).ok_or_else(|| unreadable(DEVICE_KEY, "not a 32-byte seed in hex"))
}

fn read_device(dir: &Path) -> Result<Device, Error> {
    let cert_bytes = read(&dir.join(CERT), CERT)?;
    let card_bytes = read(&dir.join(CARD), CARD)?;
    let key = DeviceKey::from_seed(&*read_seed(&dir.join(DEVICE_KEY))?);
    let (cert, card) = check(&key, &cert_bytes, &card_bytes)?;
    Ok(Device {
        cert,
        card,
        cert_bytes,
        card_bytes,
        dir: dir.to_owned(),
    })
}

/// What holds whatever the time: the certificate is `key`'s, signed by
/// the identity whose card comes with it, and both fit Prove Device.
fn check(key: &DeviceKey, cert: &[u8], card: &[u8]) -> Result<(DeviceCert, Card), Error> {
    if cert.len() > MAX_DEVICE_CERT_LEN {
        return Err(Error::TooLarge { file: CERT });
    }
    if card.len() > MAX_USER_CARD_LEN {
        return Err(Error::TooLarge { file: CARD });
    }
    let c = DeviceCert::parse(cert).map_err(|e| unreadable(CERT, e))?;
    let k = Card::parse(card).map_err(|e| unreadable(CARD, e))?;
    if c.device != key.public() || c.device_enc != key.public_enc() {
        return Err(Error::WrongDevice);
    }
    if k.identity != c.identity {
        return Err(Error::WrongIdentity);
    }
    Ok((c, k))
}

fn valid(cert: &DeviceCert, now: u64) -> Result<(), Error> {
    if !cert.allows(caps::LOGIN) {
        return Err(Error::NoLogin);
    }
    cert.check_valid(now, SKEW).map_err(|e| match e {
        hl_identity::Error::Expired => Error::Expired {
            expires: cert.expires,
        },
        _ => Error::NotYetValid {
            issued: cert.issued,
        },
    })
}
