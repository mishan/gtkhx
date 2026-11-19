//! GtkHx's device keys: the devices it proves with Prove Device
//! (capability bit 14, hxd-ng's `proposals/portable-friends.md` §3), and
//! the server keys it pins at first contact (`server-key-handover.md`).
//!
//! The objects and their checks are hx-libs' `hl-identity`; this crate
//! keeps them and decides with them. [`Store`] holds a device of each of
//! several identities, laid out as `hlid` lays out its own, but never the
//! identity key: a device is imported from an `hlid` home or certified
//! from a bundle. [`proof`] reads a server challenge and signs the answer,
//! and [`Pins`] remembers each host's key. No glib, no sockets: the caller
//! owns the directories, the clock and the connection.

mod files;
mod pins;
pub mod proof;
mod store;

pub use pins::{Outcome, Pin, Pins, Trusted};
pub use store::{Device, Listed, Pending, Store};

use std::fmt;

pub use hl_identity::caps;
pub use hl_identity::handover::Warning;
pub use hl_identity::{Fingerprint, ServerChallenge};

/// How proving a device on one connection ended, as the view is told it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Proof {
    /// The server took the proof: the session is `identity`'s `device`.
    Proved {
        identity: Fingerprint,
        device: Fingerprint,
        /// The name the identity's card gives it.
        name: String,
    },
    /// The server refused the challenge or the proof, with its reason
    /// where it gave one.
    Refused { reason: Option<String> },
    /// The server's key for `host`, or with `Warning::Recommitted` its
    /// commitment to the key after it, is not the one pinned for it:
    /// nothing was signed. No served handovers are followed yet. The user
    /// may trust `challenge`'s key instead, with [`Pins::accept`].
    KeyChanged {
        host: String,
        warning: Warning,
        challenge: Box<ServerChallenge>,
    },
    /// Nothing was signed, for a reason of this side's: `why`.
    NotProved { why: String },
    /// No answer came before this side's deadline, and the session joined
    /// unproved. A proof sent may still have reached the server.
    TimedOut,
}

/// What went wrong with a device, its files, or a pin file.
#[derive(Debug)]
pub enum Error {
    Io(std::io::Error),
    /// A file that is not what it should be: `file` names it.
    Unreadable {
        file: &'static str,
        why: String,
    },
    /// No such identity in the store, or no such pending device.
    NotFound,
    /// The certificate is for another device than the key beside it.
    WrongDevice,
    /// The card is another identity's than the certificate's.
    WrongIdentity,
    /// The certificate does not let the device log in.
    NoLogin,
    /// The certificate's time is over, or has not begun.
    Expired {
        expires: u64,
    },
    NotYetValid {
        issued: u64,
    },
    /// Bigger than Prove Device carries.
    TooLarge {
        file: &'static str,
    },
    /// The store holds another device of this identity, whose fingerprint
    /// this is; it is removed before another is put in its place.
    OtherDevice(Fingerprint),
    /// The store holds a later certificate of this device, issued then.
    Older {
        kept_issued: u64,
    },
    /// A challenge signed for another host than the one it is checked
    /// for.
    WrongHost,
    /// The pin a change was accepted against has changed since.
    PinChanged,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "{e}"),
            Error::Unreadable { file, why } => write!(f, "{file}: {why}"),
            Error::NotFound => write!(f, "no such device"),
            Error::WrongDevice => write!(f, "the certificate is another device's"),
            Error::WrongIdentity => write!(f, "the card is another identity's"),
            Error::NoLogin => write!(f, "the certificate does not let the device log in"),
            Error::Expired { expires } => write!(f, "the certificate expired at {expires}"),
            Error::NotYetValid { issued } => {
                write!(f, "the certificate is not valid until {issued}")
            }
            Error::TooLarge { file } => write!(f, "{file} is too large to prove with"),
            Error::OtherDevice(fp) => write!(f, "another device of this identity is kept: {fp}"),
            Error::Older { kept_issued } => {
                write!(f, "a later certificate, issued at {kept_issued}, is kept")
            }
            Error::WrongHost => write!(f, "the challenge is for another host"),
            Error::PinChanged => write!(f, "the pin has changed since it was shown"),
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}
