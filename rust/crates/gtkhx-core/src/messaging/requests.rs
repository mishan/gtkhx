//! The message requests draft, as a connection holds it: who waits with a
//! request for us, whom we approved, and the listing that reads them,
//! gathered page by page and paced.
//!
//! The pacing is here and the clock is the caller's, so it is told the
//! time: hxd-ng prices a page of the listing as a tenth of what a session
//! may spend in five seconds before it is banned, and a flood of Message
//! Requests Changed must not turn into a flood of listings.

use std::collections::BTreeSet;
use std::time::{Duration, Instant};

use super::Guid;

/// How long after a page arrives the next is asked for.
pub const PAGE_GAP: Duration = Duration::from_secs(1);

/// How far apart two listings begin.
pub const LISTING_GAP: Duration = Duration::from_secs(5);

/// The pages a listing is read to: a cursor that never ends is cut off
/// there, and what was gathered stands.
pub const PAGE_BOUND: u32 = 16;

/// Someone who is not a friend, with what they sent us.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Sender {
    /// As the wire carries it, which is how an answer names it.
    pub login: Vec<u8>,
    /// Oldest first.
    pub messages: Vec<Waiting>,
}

/// One message a request holds.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Waiting {
    pub guid: Guid,
    /// Line endings as GtkHx keeps them, LF.
    pub body: String,
    /// When it was sent, in Unix seconds.
    pub sent_at: u64,
}

/// One page of Get Message Requests' reply.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Page {
    /// Our account's setting, on the first page.
    pub takes: Option<bool>,
    /// Dropped since the listing before, on the first page.
    pub dropped: Option<u32>,
    /// Where there is another page.
    pub cursor: Option<Vec<u8>>,
    pub senders: Vec<Sender>,
    pub approvals: Vec<Vec<u8>>,
}

/// What the connection holds of message requests.
#[derive(Debug, Default)]
pub struct Requests {
    /// The login reply advertised a request bound: the server serves them.
    pub served: bool,
    /// Senders waiting, as the last listing or Message Requests Changed
    /// said.
    pub count: u32,
    /// Oldest first, each with their waiting messages.
    pub senders: Vec<Sender>,
    /// Logins we approved.
    pub approvals: BTreeSet<Vec<u8>>,
    /// Dropped since this connection first listed them: the server counts
    /// from the listing before, which may be another client's.
    pub dropped: u32,
    /// Our account's setting as the server holds it, from the last
    /// listing.
    pub takes: Option<bool>,
    listing: Option<Listing>,
    /// When the last listing began.
    began: Option<Instant>,
    /// A listing was asked for that has not begun, and whether it is to
    /// be read past its first page.
    owed: Option<bool>,
}

/// A listing being gathered.
#[derive(Debug)]
struct Listing {
    senders: Vec<Sender>,
    approvals: BTreeSet<Vec<u8>>,
    takes: Option<bool>,
    /// Arrived so far.
    pages: u32,
    /// When its last page arrived.
    answered: Instant,
    /// The next page's cursor, until it is asked for; `None` while a page
    /// is awaited, which hxd-ng always answers, or kicks the session.
    next: Option<Vec<u8>>,
    /// Read past its first page: a listing that only looks waits there
    /// until one is asked for.
    follow: bool,
    /// Message Requests Changed arrived while it was under way.
    changed: bool,
}

impl Requests {
    /// Ask for a listing, which goes once the pacing allows: a burst of
    /// asks is one listing. One being read is owed another, since a change
    /// between its pages can move a sender onto a page already read; one
    /// that stopped at its first page is read again from the start, since
    /// that page may be long out of date.
    pub fn want(&mut self) {
        match &mut self.listing {
            Some(l) if l.follow => {}
            Some(l) if l.next.is_none() => {
                l.follow = true;
                return;
            }
            _ => self.listing = None,
        }
        self.owed = Some(true);
    }

    /// Message Requests Changed said how many wait. A listing under way
    /// may have read past the change, so its count is not taken, and one
    /// being read is owed another; a login's first page is never extended.
    pub fn changed(&mut self, count: u32) {
        self.count = count;
        if let Some(l) = &mut self.listing {
            l.changed = true;
            if l.follow {
                self.owed = Some(true);
            }
        }
    }

    /// Ask for a listing's first page alone, which makes the session one
    /// that understands requests and spends the least: what a login does,
    /// where the login itself has spent most of what hxd-ng allows.
    pub fn look(&mut self) {
        if self.listing.is_none() && self.owed.is_none() {
            self.owed = Some(false);
        }
    }

    /// When the next page may be asked for, where one waits to be; `now`
    /// for a listing that may begin at once.
    pub fn due(&self, now: Instant) -> Option<Instant> {
        match &self.listing {
            Some(l) => l
                .next
                .as_ref()
                .filter(|_| l.follow)
                .map(|_| l.answered + PAGE_GAP),
            None => self
                .owed
                .map(|_| self.began.map_or(now, |b| b + LISTING_GAP)),
        }
    }

    /// The page to ask for at `now`, where one is due: `Some(None)` for a
    /// listing's first, which carries no cursor.
    pub fn next(&mut self, now: Instant) -> Option<Option<Vec<u8>>> {
        if self.due(now)? > now {
            return None;
        }
        if let Some(l) = &mut self.listing {
            return l.next.take().map(Some);
        }
        self.began = Some(now);
        self.listing = Some(Listing {
            senders: Vec::new(),
            approvals: BTreeSet::new(),
            takes: None,
            pages: 0,
            answered: now,
            next: None,
            follow: self.owed.take().unwrap_or(true),
            changed: false,
        });
        Some(None)
    }

    /// A page arrived at `now`, its groups merged into the listing by
    /// Login: the cursor is a place in a listing the server reads again
    /// for each page, so an answer between pages can list a sender twice,
    /// or once as a sender and then as an approval, and the later page
    /// says which. Whether it was the last, the listing then replacing
    /// what was held.
    pub fn page(&mut self, p: Page, now: Instant) -> bool {
        let Some(l) = &mut self.listing else {
            return false;
        };
        l.answered = now;
        if l.pages == 0 {
            // Only the first page says it, and saying it resets it.
            self.dropped = self.dropped.saturating_add(p.dropped.unwrap_or(0));
            l.takes = p.takes;
        }
        l.pages += 1;
        for s in p.senders {
            l.approvals.remove(&s.login);
            match l.senders.iter_mut().find(|h| h.login == s.login) {
                Some(held) => {
                    for m in s.messages {
                        if !held.messages.iter().any(|w| w.guid == m.guid) {
                            held.messages.push(m);
                        }
                    }
                }
                None => l.senders.push(s),
            }
        }
        l.senders.retain(|s| !p.approvals.contains(&s.login));
        l.approvals.extend(p.approvals);
        if let Some(c) = p.cursor.filter(|_| l.pages < PAGE_BOUND) {
            l.next = Some(c);
            return false;
        }
        let Some(l) = self.listing.take() else {
            return false;
        };
        if !l.changed {
            self.count = u32::try_from(l.senders.len()).unwrap_or(u32::MAX);
        }
        self.senders = l.senders;
        self.approvals = l.approvals;
        self.takes = l.takes;
        true
    }

    /// The listing was refused, or a page could not be read: the one held
    /// stands.
    pub fn failed(&mut self) {
        self.listing = None;
    }
}

#[cfg(test)]
mod tests;
