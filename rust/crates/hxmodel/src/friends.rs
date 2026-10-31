//! The friend family as the users panel lists it: which of a connection's
//! roster entries go where, in what order, and how each reads; and when our
//! presence and typing go out.
//!
//! No widgets. `gtkhx-ui`'s `friends` module draws these rows, and its
//! conversations ask [`Typing`] when to tell a friend we are typing.

#![forbid(unsafe_code)]

use std::time::{Duration, Instant};

use gtkhx_core::messaging::{Friend, Roster};
use hxproto::messaging::{PresenceState, RosterState};

/// Where an entry is listed, in the order the panel lists them: what waits
/// for an answer first, then the friends, then what waits on them, then the
/// blocked.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Section {
    /// They asked to be a friend.
    Request,
    Friend,
    /// We asked, and they have not answered.
    Sent,
    Blocked,
}

impl Section {
    fn of(state: Option<RosterState>) -> Option<Section> {
        match state? {
            RosterState::PendingIn => Some(Section::Request),
            RosterState::Accepted => Some(Section::Friend),
            RosterState::PendingOut => Some(Section::Sent),
            RosterState::Blocked => Some(Section::Blocked),
            _ => None,
        }
    }
}

/// One row of the friends list.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Row {
    pub login: Vec<u8>,
    /// What to call them: see [`display_name`].
    pub name: String,
    pub section: Section,
    pub presence: PresenceState,
    pub status: Option<String>,
    /// What their request said.
    pub note: Option<String>,
    /// Drawn dimmed: a friend who is not there, or an account that is no
    /// friend yet or any more.
    pub dimmed: bool,
}

/// What to call an account: our alias for it, else the name it goes by,
/// else its Login.
pub fn display_name(f: &Friend) -> String {
    fn set(s: &Option<String>) -> Option<&str> {
        s.as_deref().map(str::trim).filter(|s| !s.is_empty())
    }
    set(&f.nickname)
        .or_else(|| set(&f.name))
        .map(str::to_owned)
        .unwrap_or_else(|| hxproto::text::to_utf8(&f.login))
}

/// What to call the account `login`: as [`display_name`] where `roster`
/// holds an entry for it, else its Login.
pub fn name_of(roster: &Roster, login: &[u8]) -> String {
    roster
        .get(login)
        .map_or_else(|| hxproto::text::to_utf8(login), display_name)
}

/// Whether a friend can be reached now. Invisible is never reported for a
/// friend, who reads as offline instead.
pub fn is_present(p: PresenceState) -> bool {
    !matches!(p, PresenceState::Offline | PresenceState::Invisible)
}

/// The rows of `roster`, in the panel's order: by section, then, among
/// friends, those present first, then by name without regard to case, and
/// by Login where names agree.
pub fn rows(roster: &Roster) -> Vec<Row> {
    let mut rows: Vec<Row> = roster
        .entries()
        .filter_map(|f| {
            let section = Section::of(f.state)?;
            Some(Row {
                login: f.login.clone(),
                name: display_name(f),
                section,
                presence: f.presence,
                status: f.status.clone(),
                note: f.note.clone(),
                dimmed: section != Section::Request
                    && !(section == Section::Friend && is_present(f.presence)),
            })
        })
        .collect();
    rows.sort_by(|a, b| {
        let absent = |r: &Row| r.section == Section::Friend && !is_present(r.presence);
        (a.section, absent(a), a.name.to_lowercase(), &a.login).cmp(&(
            b.section,
            absent(b),
            b.name.to_lowercase(),
            &b.login,
        ))
    });
    rows
}

/// Whether a message to `login` goes as an instant message: only to a
/// friend, since the server refuses one to anyone else.
pub fn is_friend(roster: &Roster, login: &[u8]) -> bool {
    roster
        .get(login)
        .is_some_and(|f| f.state == Some(RosterState::Accepted))
}

/// Our presence, which follows the away state the user list shows of us.
/// The server starts a login present, so only a change goes out.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Presence {
    told: PresenceState,
}

impl Default for Presence {
    fn default() -> Self {
        Presence {
            told: PresenceState::Online,
        }
    }
}

impl Presence {
    /// We read as `away`; the presence to send, if that changes it.
    pub fn away(&mut self, away: bool) -> Option<PresenceState> {
        let now = if away {
            PresenceState::Away
        } else {
            PresenceState::Online
        };
        (now != self.told).then(|| {
            self.told = now;
            now
        })
    }
}

/// Whether we are typing to someone, which they are told only as it starts
/// and stops rather than at every key.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Typing {
    on: bool,
}

impl Typing {
    /// The input is now `empty`, or not; whether to say we started (`true`)
    /// or stopped (`false`), where that changed.
    pub fn edited(&mut self, empty: bool) -> Option<bool> {
        let on = !empty;
        (on != self.on).then(|| {
            self.on = on;
            on
        })
    }
}

/// Whether someone is typing to us. Their Stopped can be lost with their
/// connection, so a Started lapses this long after it unless renewed.
pub const TYPING_LAPSE: Duration = Duration::from_secs(10);

/// Someone's typing to us, as their Started and Stopped tell it.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct TheirTyping {
    since: Option<Instant>,
}

impl TheirTyping {
    /// They started, or are still typing, `now`; whether that is news.
    pub fn started(&mut self, now: Instant) -> bool {
        let news = !self.is_on(now);
        self.since = Some(now);
        news
    }

    pub fn stopped(&mut self) {
        self.since = None;
    }

    /// Whether they are typing at `now`.
    pub fn is_on(&self, now: Instant) -> bool {
        self.since
            .is_some_and(|t| now.saturating_duration_since(t) < TYPING_LAPSE)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn friend(login: &str, state: RosterState, presence: PresenceState) -> Friend {
        Friend {
            login: login.as_bytes().to_vec(),
            nickname: None,
            state: Some(state),
            presence,
            status: None,
            name: None,
            note: None,
        }
    }

    #[test]
    fn rows_go_by_section_then_presence_then_name() {
        use PresenceState::*;
        use RosterState::*;
        let mut roster = Roster::default();
        let mut zed = friend("zed", Accepted, Online);
        zed.nickname = Some("Aardvark".into());
        roster.replace([
            friend("mallory", Blocked, Offline),
            friend("bob", Accepted, Offline),
            friend("carol", PendingOut, Offline),
            friend("dave", PendingIn, Offline),
            friend("Alice", Accepted, Away),
            friend("erin", Accepted, Invisible),
            friend("frank", Accepted, Busy),
            zed,
        ]);
        let got: Vec<_> = rows(&roster)
            .into_iter()
            .map(|r| (r.name, r.section, r.dimmed))
            .collect();
        let want = [
            ("dave", Section::Request, false),
            ("Aardvark", Section::Friend, false),
            ("Alice", Section::Friend, false),
            ("frank", Section::Friend, false),
            ("bob", Section::Friend, true),
            ("erin", Section::Friend, true),
            ("carol", Section::Sent, true),
            ("mallory", Section::Blocked, true),
        ];
        let want: Vec<_> = want
            .into_iter()
            .map(|(n, s, d)| (n.to_owned(), s, d))
            .collect();
        assert_eq!(got, want);
    }

    #[test]
    fn a_name_is_the_alias_else_the_name_else_the_login() {
        let cases = [
            (Some("Al"), Some("Alice Liddell"), "Al"),
            (None, Some("Alice Liddell"), "Alice Liddell"),
            (Some("  "), Some("Alice Liddell"), "Alice Liddell"),
            (None, None, "alice"),
            (Some(""), Some(""), "alice"),
        ];
        for (nickname, name, want) in cases {
            let mut f = friend("alice", RosterState::Accepted, PresenceState::Online);
            f.nickname = nickname.map(str::to_owned);
            f.name = name.map(str::to_owned);
            assert_eq!(display_name(&f), want, "{nickname:?} {name:?}");
            let mut roster = Roster::default();
            roster.replace([f]);
            assert_eq!(name_of(&roster, b"alice"), want);
            assert_eq!(name_of(&roster, b"bob"), "bob");
        }
    }

    #[test]
    fn only_a_friend_is_sent_an_instant_message() {
        use RosterState::*;
        let mut roster = Roster::default();
        roster.replace([
            friend("a", Accepted, PresenceState::Offline),
            friend("b", PendingOut, PresenceState::Offline),
            friend("c", PendingIn, PresenceState::Offline),
            friend("d", Blocked, PresenceState::Offline),
        ]);
        let cases = [
            ("a", true),
            ("b", false),
            ("c", false),
            ("d", false),
            ("e", false),
        ];
        for (login, want) in cases {
            assert_eq!(is_friend(&roster, login.as_bytes()), want, "{login}");
        }
    }

    #[test]
    fn presence_goes_out_only_when_away_changes() {
        let mut p = Presence::default();
        let steps = [
            (false, None),
            (true, Some(PresenceState::Away)),
            (true, None),
            (false, Some(PresenceState::Online)),
            (false, None),
        ];
        for (i, (away, want)) in steps.into_iter().enumerate() {
            assert_eq!(p.away(away), want, "step {i}");
        }
    }

    #[test]
    fn their_typing_lapses_unless_renewed() {
        let t0 = Instant::now();
        let s = Duration::from_secs;
        let mut t = TheirTyping::default();
        assert!(!t.is_on(t0));
        assert!(t.started(t0), "a first start is news");
        assert!(!t.started(t0 + s(4)), "a renewal is not");
        assert!(t.is_on(t0 + s(13)), "renewed at 4s, on until 14s");
        assert!(!t.is_on(t0 + s(14)));
        assert!(t.started(t0 + s(20)), "a start after a lapse is news");
        t.stopped();
        assert!(!t.is_on(t0 + s(21)));
    }

    #[test]
    fn typing_is_told_as_it_starts_and_stops() {
        let mut t = Typing::default();
        let steps = [
            (true, None),
            (false, Some(true)),
            (false, None),
            (true, Some(false)),
            (false, Some(true)),
        ];
        for (i, (empty, want)) in steps.into_iter().enumerate() {
            assert_eq!(t.edited(empty), want, "step {i}");
        }
    }
}
