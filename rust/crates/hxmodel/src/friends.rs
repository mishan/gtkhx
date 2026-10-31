//! The friend family as the users panel lists it: which of a connection's
//! roster entries go where, in what order, and how each reads; when our
//! presence and typing go out; and what is shown of what a stranger sent.
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
    /// Someone who is not a friend, whose messages we approved.
    Approved,
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
    fn set(s: &Option<String>) -> Option<String> {
        let s = plain_name(s.as_deref()?);
        Some(s.trim().to_owned()).filter(|s| !s.is_empty())
    }
    set(&f.nickname)
        .or_else(|| set(&f.name))
        .unwrap_or_else(|| plain_name(&hxproto::text::to_utf8(&f.login)))
}

/// What to call the account `login`: as [`display_name`] where `roster`
/// holds an entry for it, else its Login.
pub fn name_of(roster: &Roster, login: &[u8]) -> String {
    roster
        .get(login)
        .map_or_else(|| plain_name(&hxproto::text::to_utf8(login)), display_name)
}

/// Whether a character changes how the text around it is drawn, or is no
/// text at all: a bidi override or isolate can make a name read as another,
/// or reverse the window title it is in, and an invisible one can make two
/// names that look alike differ. The joiners an emoji or a script needs,
/// U+200C and U+200D, stay.
fn unshown(c: char) -> bool {
    c.is_control()
        || matches!(
            c,
            '\u{ad}'
                | '\u{61c}'
                | '\u{115f}'
                | '\u{1160}'
                | '\u{180e}'
                | '\u{200b}'
                | '\u{200e}'
                | '\u{200f}'
                | '\u{202a}'..='\u{202e}'
                | '\u{2060}'..='\u{2064}'
                | '\u{2066}'..='\u{206f}'
                | '\u{3164}'
                | '\u{feff}'
                | '\u{fff9}'..='\u{fffb}'
                | '\u{e0000}'..='\u{e007f}'
        )
}

/// A name someone else chose, as one line of plain text: what [`unshown`]
/// catches dropped, line breaks among them.
pub fn plain_name(name: &str) -> String {
    name.chars()
        .filter(|&c| !unshown(c) && !matches!(c, '\u{2028}' | '\u{2029}'))
        .collect()
}

/// A message someone else wrote, as plain text: as [`plain_name`], but its
/// lines kept.
pub fn plain_text(text: &str) -> String {
    text.chars()
        .map(|c| {
            if matches!(c, '\u{2028}' | '\u{2029}') {
                '\n'
            } else {
                c
            }
        })
        .filter(|&c| c == '\n' || c == '\t' || !unshown(c))
        .collect()
}

/// Whether a friend can be reached now. Invisible is never reported for a
/// friend, who reads as offline instead.
pub fn is_present(p: PresenceState) -> bool {
    !matches!(p, PresenceState::Offline | PresenceState::Invisible)
}

/// The rows of `roster`, in the panel's order: by section, then, among
/// friends, those present first, then by name without regard to case, and
/// by Login where names agree. An approval is listed only for an account
/// with no entry, which says more of it.
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
    let approvals = roster.requests.approvals.iter();
    rows.extend(
        approvals
            .filter(|login| roster.get(login).is_none())
            .map(|login| Row {
                login: login.clone(),
                name: name_of(roster, login),
                section: Section::Approved,
                presence: PresenceState::Offline,
                status: None,
                note: None,
                dimmed: true,
            }),
    );
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

/// How many wait with a message request, where the server serves them and
/// any do.
pub fn requests_waiting(roster: &Roster) -> Option<u32> {
    let r = &roster.requests;
    (r.served && r.count > 0).then_some(r.count)
}

/// Whether `login` is a friend.
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
    /// The presence the server was last told.
    pub fn told(&self) -> PresenceState {
        self.told
    }

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

/// How long after we approve someone's requests their messages, handed
/// over at once as instant messages, arrive unannounced.
pub const APPROVAL_HUSH: Duration = Duration::from_secs(5);

/// The senders whose messages arrive unannounced for now: those we just
/// approved, whose waiting messages are drawn into the conversation the
/// approval opened, where announcing each would be noise.
#[derive(Debug, Default)]
pub struct Hush {
    until: Vec<(Vec<u8>, Instant)>,
}

impl Hush {
    /// We approved `login` at `now`.
    pub fn approved(&mut self, login: &[u8], now: Instant) {
        self.until.retain(|(l, t)| l != login && *t > now);
        self.until.push((login.to_vec(), now + APPROVAL_HUSH));
    }

    /// Whether a message from `login` arriving at `now` goes unannounced.
    pub fn hushed(&self, login: &[u8], now: Instant) -> bool {
        self.until.iter().any(|(l, t)| l == login && now < *t)
    }
}

/// How GtkHx remembers, in the settings, having set whether the account
/// `login` on the server at `host` and `port` takes message requests: the
/// Login in hex, since the list is written with commas between its items
/// and each item trimmed, so an empty Login still leaves a whole key.
pub fn requests_set_key(host: &str, port: u16, login: &[u8]) -> String {
    let hex: String = login.iter().map(|b| format!("{b:02x}")).collect();
    format!("{host}:{port}/{hex}")
}

/// Whether GtkHx turns taking message requests on for an account, which it
/// does once for an account that has it off as its server holds it
/// (`takes`): never where GtkHx has set it there before (`set`), since the
/// user may have turned it off, and never twice on one connect (`asked`),
/// where the first is still unanswered or was refused.
pub fn turn_requests_on(takes: Option<bool>, set: bool, asked: bool) -> bool {
    takes == Some(false) && !set && !asked
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
        roster.requests.approvals = [&b"grace"[..], b"bob", b"Hal"].map(<[u8]>::to_vec).into();
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
            ("grace", Section::Approved, true),
            ("Hal", Section::Approved, true),
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
    fn what_a_stranger_wrote_is_shown_as_plain_text() {
        let cases = [
            ("alice", "alice", "alice"),
            (
                "caf\u{e9} \u{1f600}",
                "caf\u{e9} \u{1f600}",
                "caf\u{e9} \u{1f600}",
            ),
            ("\u{202e}evil.exe", "evil.exe", "evil.exe"),
            ("a\u{2066}b\u{2069}c\u{200f}d\u{61c}", "abcd", "abcd"),
            ("one\ntwo\r\tthree", "onetwothree", "one\ntwo\tthree"),
            ("bell\u{7}\u{1b}[31m\u{9b}", "bell[31m", "bell[31m"),
            ("line\u{2028}next\u{2029}", "linenext", "line\nnext\n"),
            ("dennis\u{200b}", "dennis", "dennis"),
            ("de\u{ad}n\u{feff}n\u{2060}is\u{e0041}", "dennis", "dennis"),
            ("\u{3164}\u{115f}x\u{206a}\u{fff9}", "x", "x"),
            (
                "\u{1f469}\u{200d}\u{1f4bb} a\u{200c}b",
                "\u{1f469}\u{200d}\u{1f4bb} a\u{200c}b",
                "\u{1f469}\u{200d}\u{1f4bb} a\u{200c}b",
            ),
        ];
        for (raw, name, text) in cases {
            assert_eq!(plain_name(raw), name, "{raw:?}");
            assert_eq!(plain_text(raw), text, "{raw:?}");
        }
        let mut f = friend("\u{202e}gro", RosterState::Accepted, PresenceState::Online);
        assert_eq!(display_name(&f), "gro");
        f.name = Some("\u{2067}\u{2069}".into());
        assert_eq!(display_name(&f), "gro", "a name of nothing shown is none");
    }

    #[test]
    fn the_requests_row_is_there_while_any_wait_where_they_are_served() {
        let cases = [(false, 3, None), (true, 0, None), (true, 2, Some(2))];
        for (served, count, want) in cases {
            let mut roster = Roster::default();
            roster.requests.served = served;
            roster.requests.count = count;
            assert_eq!(requests_waiting(&roster), want, "{served} {count}");
        }
    }

    #[test]
    fn taking_requests_is_turned_on_once_for_an_account_that_has_it_off() {
        let cases = [
            (None, false, false, false),
            (Some(true), false, false, false),
            (Some(false), false, false, true),
            (Some(false), true, false, false),
            (Some(false), false, true, false),
        ];
        for (takes, set, asked, want) in cases {
            assert_eq!(
                turn_requests_on(takes, set, asked),
                want,
                "{takes:?} {set} {asked}"
            );
        }
    }

    #[test]
    fn an_approved_senders_messages_arrive_unannounced_for_a_while() {
        let t0 = Instant::now();
        let mut h = Hush::default();
        assert!(!h.hushed(b"alice", t0));
        h.approved(b"alice", t0);
        let cases = [
            (&b"alice"[..], Duration::ZERO, true),
            (b"alice", APPROVAL_HUSH - Duration::from_millis(1), true),
            (b"alice", APPROVAL_HUSH, false),
            (b"bob", Duration::ZERO, false),
        ];
        for (login, after, want) in cases {
            assert_eq!(h.hushed(login, t0 + after), want, "{login:?} {after:?}");
        }
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
