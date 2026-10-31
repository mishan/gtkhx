//! The messaging extension's friend family, as a connection holds it: the
//! roster read from the server and kept up by what it pushes, the state of
//! each instant message sent, and the events `hxhandlers::recv::friends`
//! tells the view of on the session's `messaging` signal.
//!
//! What the server sends is the record: an entry pushed replaces the one
//! held, whole, as the extension asks, and nothing here guesses at a
//! change the server has not confirmed.

use std::cell::RefCell;
use std::collections::{BTreeMap, HashMap};
use std::ffi::c_void;

use hxproto::messaging::{AckType, PresenceState, ReasonCode, RosterEntry, RosterState, GUID_LEN};

/// A message's id, the one its receipts name.
pub type Guid = [u8; GUID_LEN];

/// One account as an entry describes it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Friend {
    /// As the wire carries it, which is how a request names it.
    pub login: Vec<u8>,
    /// Our alias for them.
    pub nickname: Option<String>,
    /// Where an entry says; a find result may not.
    pub state: Option<RosterState>,
    pub presence: PresenceState,
    pub status: Option<String>,
    /// The name they go by.
    pub name: Option<String>,
    /// What their friend request said.
    pub note: Option<String>,
}

impl Friend {
    pub fn from_entry(e: &RosterEntry<'_>) -> Friend {
        let text = |b: Option<&[u8]>| b.map(hxproto::text::to_utf8);
        Friend {
            login: e.login.to_vec(),
            nickname: text(e.nickname),
            state: e.roster_state(),
            presence: e.presence_state().unwrap_or(PresenceState::Offline),
            status: text(e.status_text),
            name: text(e.name),
            note: text(e.note),
        }
    }
}

/// How far an instant message we sent has got.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Delivery {
    /// Sent, and not yet answered.
    Sending,
    /// Taken by a session of theirs.
    Sent,
    /// Held for them until they are back.
    Queued,
    /// Their client has it.
    Delivered,
    /// They have seen it.
    Read,
    /// Refused, or lost with the connection.
    Failed,
}

impl Delivery {
    /// What an IM Send's success says.
    pub fn sent(reason: Option<ReasonCode>) -> Delivery {
        match reason {
            Some(ReasonCode::OfflineQueued | ReasonCode::Requested) => Delivery::Queued,
            _ => Delivery::Sent,
        }
    }

    /// What a receipt says.
    pub fn acked(ack: AckType) -> Delivery {
        match ack {
            AckType::Read => Delivery::Read,
            _ => Delivery::Delivered,
        }
    }

    /// Where it stands among the others: a receipt that arrives after a
    /// later one, or before the send's own answer, never takes it back.
    fn rank(self) -> u8 {
        match self {
            Delivery::Sending => 0,
            Delivery::Sent | Delivery::Queued => 1,
            Delivery::Delivered => 2,
            Delivery::Read => 3,
            Delivery::Failed => 4,
        }
    }
}

/// What the connection holds of the friend family.
#[derive(Debug, Default)]
pub struct Roster {
    entries: BTreeMap<Vec<u8>, Friend>,
    sent: HashMap<Guid, (Vec<u8>, Delivery)>,
    presence: Option<(PresenceState, Option<String>)>,
}

impl Roster {
    /// Every entry, as Get Roster's reply lists them.
    pub fn replace(&mut self, entries: impl IntoIterator<Item = Friend>) {
        self.entries = entries.into_iter().map(|f| (f.login.clone(), f)).collect();
    }

    /// A Roster Entry (801), which replaces what is held for its Login, or,
    /// `Removed`, ends it.
    pub fn entry(&mut self, f: Friend) {
        if f.state == Some(RosterState::Removed) {
            self.entries.remove(&f.login);
        } else {
            self.entries.insert(f.login.clone(), f);
        }
    }

    /// A Friend Request (804): someone asks.
    pub fn request(&mut self, mut f: Friend) {
        f.state = Some(RosterState::PendingIn);
        self.entries.insert(f.login.clone(), f);
    }

    /// A Presence Changed (809), which carries the presence and the text
    /// alone; an entry nobody holds is no friend of ours. Whether it was
    /// one.
    pub fn presence(&mut self, f: &Friend) -> bool {
        let Some(e) = self.entries.get_mut(&f.login) else {
            return false;
        };
        e.presence = f.presence;
        e.status = f.status.clone();
        true
    }

    pub fn get(&self, login: &[u8]) -> Option<&Friend> {
        self.entries.get(login)
    }

    /// Every entry, by Login.
    pub fn entries(&self) -> impl Iterator<Item = &Friend> {
        self.entries.values()
    }

    /// Our own presence and status, as the server last took them.
    pub fn own_presence(&self) -> Option<(PresenceState, Option<&str>)> {
        self.presence.as_ref().map(|(p, s)| (*p, s.as_deref()))
    }

    /// The server took our presence; a status of `None` was left as it was,
    /// and one of only whitespace cleared it, as the server takes it.
    pub fn set_own_presence(&mut self, state: PresenceState, status: Option<String>) {
        let status = match status {
            Some(s) => (!s.trim().is_empty()).then_some(s),
            None => self.presence.take().and_then(|(_, s)| s),
        };
        self.presence = Some((state, status));
    }

    /// A message to `to` has gone out.
    pub fn sending(&mut self, guid: Guid, to: &[u8]) {
        self.sent.insert(guid, (to.to_vec(), Delivery::Sending));
    }

    /// How far `guid` has got: whom it went to, and where it stands. A
    /// change that would move it back is no change, and `None`; nor is a
    /// receipt for a message we never sent, or one `from` someone else.
    /// Read or failed, it can go no further, and is let go of.
    pub fn delivered(
        &mut self,
        guid: &Guid,
        from: Option<&[u8]>,
        now: Delivery,
    ) -> Option<(Vec<u8>, Delivery)> {
        let (to, was) = self.sent.get_mut(guid)?;
        if from.is_some_and(|f| f != to.as_slice()) || now.rank() <= was.rank() {
            return None;
        }
        *was = now;
        let to = to.clone();
        if matches!(now, Delivery::Read | Delivery::Failed) {
            self.sent.remove(guid);
        }
        Some((to, now))
    }

    pub fn delivery(&self, guid: &Guid) -> Option<Delivery> {
        self.sent.get(guid).map(|(_, d)| *d)
    }
}

/// An instant message that arrived.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Im {
    pub from: Vec<u8>,
    pub guid: Guid,
    /// Line endings as GtkHx keeps them, LF.
    pub body: String,
    /// When it was sent, in Unix seconds.
    pub sent_at: u64,
}

/// What the `messaging` signal tells the view.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// The whole roster was read again.
    Roster,
    /// One entry changed, arrived or went; the roster says which.
    Changed {
        login: Vec<u8>,
    },
    /// Someone asks to be a friend.
    Request {
        login: Vec<u8>,
    },
    /// The server took our presence.
    Presence,
    Im(Im),
    /// A message we sent moved on.
    Delivery {
        to: Vec<u8>,
        guid: Guid,
        delivery: Delivery,
    },
    /// Someone started or stopped typing to us.
    Typing {
        from: Vec<u8>,
        typing: bool,
    },
    /// What Find User or Get User Info on `trans` found: `None`, nobody
    /// the caller may see.
    Found {
        trans: u32,
        friend: Option<Friend>,
    },
    /// The request of opcode `request` on `trans` was refused, why in the
    /// extension's words where the server gave them, and in its own.
    Refused {
        trans: u32,
        request: u32,
        reason: Option<ReasonCode>,
        text: Option<String>,
    },
}

thread_local! {
    static ROSTERS: RefCell<HashMap<usize, Roster>> = RefCell::new(HashMap::new());
}

/// Read what connection `htlc` holds. Main thread.
pub fn with_roster<R>(htlc: *const c_void, f: impl FnOnce(&Roster) -> R) -> R {
    ROSTERS.with_borrow(|r| match r.get(&(htlc as usize)) {
        Some(roster) => f(roster),
        None => f(&Roster::default()),
    })
}

/// Change what connection `htlc` holds. Main thread.
pub fn with_roster_mut<R>(htlc: *const c_void, f: impl FnOnce(&mut Roster) -> R) -> R {
    ROSTERS.with_borrow_mut(|r| f(r.entry(htlc as usize).or_default()))
}

/// Let go of what `htlc` holds: it closed, or logs in afresh. The
/// messages it sent that were never answered, and whom each went to.
pub fn forget(htlc: *const c_void) -> Vec<(Guid, Vec<u8>)> {
    let gone = ROSTERS.with_borrow_mut(|r| r.remove(&(htlc as usize)));
    gone.map_or_else(Vec::new, |r| {
        r.sent
            .into_iter()
            .filter(|(_, (_, d))| *d == Delivery::Sending)
            .map(|(g, (to, _))| (g, to))
            .collect()
    })
}

#[cfg(test)]
mod tests;
