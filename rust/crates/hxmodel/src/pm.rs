//! Private-message conversations: the table of open ones, keyed by
//! connection and peer, and the rules for what a message in one reads as.
//!
//! No widgets. `gtkhx-ui`'s `msg` module keeps a view per conversation in a
//! [`Conversations`] table and renders by these rules.

#![forbid(unsafe_code)]

use std::collections::HashMap;

use crate::chat::InputHistory;

/// A connection, as its serial (`gtkhx-ui`'s `dock::ConnKey`).
pub type ConnKey = u16;

/// Who a conversation is with.
///
/// A classic private message is addressed by uid, which the server assigns
/// per login and which is only unique within one connection — so a
/// conversation is keyed by connection and peer together, never by peer
/// alone.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Peer {
    Uid(u16),
    /// A friend, by Login as the wire carries it: an account rather than a
    /// session, so the conversation reaches them while they are offline.
    /// The user list names nobody's Login, so the same person may have a
    /// conversation of each kind.
    Login(Vec<u8>),
}

/// One open conversation.
#[derive(Debug, Default)]
pub struct Conversation {
    /// The name the conversation was opened with, shown until the user list
    /// has something better.
    pub name: String,
    /// What has been typed into it, for Up and Down.
    pub history: InputHistory,
}

impl Conversation {
    pub fn new(name: impl Into<String>) -> Self {
        Conversation {
            name: name.into(),
            history: InputHistory::new(),
        }
    }
}

/// The open conversations of every connection.
#[derive(Debug)]
pub struct Conversations<V> {
    map: HashMap<(ConnKey, Peer), V>,
}

impl<V> Default for Conversations<V> {
    fn default() -> Self {
        Conversations {
            map: HashMap::new(),
        }
    }
}

impl<V> Conversations<V> {
    pub fn get(&self, conn: ConnKey, peer: &Peer) -> Option<&V> {
        self.map.get(&(conn, peer.clone()))
    }

    /// File `v` as the conversation with `peer`, handing back the one it
    /// replaces.
    pub fn insert(&mut self, conn: ConnKey, peer: Peer, v: V) -> Option<V> {
        self.map.insert((conn, peer), v)
    }

    pub fn remove(&mut self, conn: ConnKey, peer: &Peer) -> Option<V> {
        self.map.remove(&(conn, peer.clone()))
    }

    /// Take every conversation of `conn` out of the table, for a connection
    /// that has closed. Handed back rather than dropped, so the caller can
    /// let go of them outside whatever borrow it holds the table in.
    pub fn forget_conn(&mut self, conn: ConnKey) -> Vec<V> {
        let keys: Vec<_> = self
            .map
            .keys()
            .filter(|(c, _)| *c == conn)
            .cloned()
            .collect();
        keys.into_iter()
            .filter_map(|k| self.map.remove(&k))
            .collect()
    }

    /// The conversations of `conn`, in no particular order.
    pub fn of_conn(&self, conn: ConnKey) -> impl Iterator<Item = &V> {
        self.map
            .iter()
            .filter(move |((c, _), _)| *c == conn)
            .map(|(_, v)| v)
    }

    pub fn len(&self) -> usize {
        self.map.len()
    }

    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

/// A conversation's tab title.
pub fn title(name: &str, uid: u16) -> String {
    format!("{name} ({uid})")
}

/// A friend's conversation's tab title, with their Login where they go by
/// another name.
pub fn login_title(name: &str, login: &str) -> String {
    if name == login {
        name.to_owned()
    } else {
        format!("{name} ({login})")
    }
}

/// Whether `name` is ours. Only an answer once we have a nick: before that
/// nobody is us.
pub fn is_self(name: Option<&[u8]>, own: &[u8]) -> bool {
    matches!(name, Some(n) if !own.is_empty() && n == own)
}

/// The user a row speaks as, for its avatar.
///
/// `outgoing` is the direction, and is not derivable from [`is_self`]: when
/// you message yourself, both halves of the conversation are by you, and
/// only which path produced the row — the echo of what was typed or the
/// copy the server sent — tells them apart. The window's uid is the other
/// party, so an outgoing row speaks as `own_uid`, which is 0 (no avatar
/// rather than a wrong one) until the server has told us ours.
pub fn speaker(outgoing: bool, own_uid: u16, peer_uid: u16) -> u16 {
    if outgoing {
        own_uid
    } else {
        peer_uid
    }
}

/// What to show of a message's text. A bare `[image]` is the caption an
/// attachment sends without one, and the picture below says it better.
pub fn shown_body(body: Option<&[u8]>, has_media: bool) -> &[u8] {
    match body {
        Some(b"[image]") if has_media => b"",
        Some(b) => b,
        None => b"",
    }
}

/// A message's rows: one per line, the first carrying the nick. A trailing
/// newline makes an empty last row, and an empty message one empty row.
pub fn rows(body: &str) -> std::str::Split<'_, char> {
    body.split('\n')
}

/// What the info pane says of a user, from their status word.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Status {
    pub admin: bool,
    pub away: bool,
}

impl Status {
    pub fn of(color: u16) -> Self {
        Status {
            admin: color >= 2,
            away: color % 2 == 1,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn conv(name: &str) -> Conversation {
        Conversation::new(name)
    }

    #[test]
    fn a_uid_is_found_where_it_was_filed_and_nowhere_else() {
        let mut t = Conversations::default();
        for uid in [0, 1, 42, 0x7fff, 0x8000, 0xfffe, 0xffff] {
            assert!(t
                .insert(1, Peer::Uid(uid), conv(&uid.to_string()))
                .is_none());
        }
        assert_eq!(t.len(), 7);
        for uid in [0, 1, 42, 0x7fff, 0x8000, 0xfffe, 0xffff] {
            assert_eq!(t.get(1, &Peer::Uid(uid)).unwrap().name, uid.to_string());
            assert!(
                t.get(2, &Peer::Uid(uid)).is_none(),
                "uid {uid} leaked across connections"
            );
        }
        assert!(t.get(1, &Peer::Uid(9999)).is_none());
    }

    #[test]
    fn the_same_uid_on_two_connections_is_two_conversations() {
        let mut t = Conversations::default();
        t.insert(1, Peer::Uid(7), conv("alice"));
        t.insert(2, Peer::Uid(7), conv("mallory"));
        assert_eq!(t.get(1, &Peer::Uid(7)).unwrap().name, "alice");
        assert_eq!(t.get(2, &Peer::Uid(7)).unwrap().name, "mallory");
    }

    #[test]
    fn filing_a_uid_again_replaces_the_conversation() {
        let mut t = Conversations::default();
        t.insert(1, Peer::Uid(7), conv("alice"));
        let old = t.insert(1, Peer::Uid(7), conv("alice-2"));
        assert_eq!(old.unwrap().name, "alice");
        assert_eq!(t.len(), 1);
        assert_eq!(t.get(1, &Peer::Uid(7)).unwrap().name, "alice-2");
    }

    #[test]
    fn closing_removes_only_that_conversation() {
        let mut t = Conversations::default();
        t.insert(1, Peer::Uid(101), conv("carol"));
        t.insert(1, Peer::Uid(102), conv("dave"));
        assert_eq!(t.remove(1, &Peer::Uid(101)).unwrap().name, "carol");
        assert!(t.get(1, &Peer::Uid(101)).is_none());
        assert!(t.remove(1, &Peer::Uid(999)).is_none());
        assert!(t.remove(2, &Peer::Uid(102)).is_none());
        assert_eq!(t.len(), 1);
    }

    #[test]
    fn a_closed_connection_takes_its_conversations_with_it() {
        let mut t = Conversations::default();
        for uid in 200..220 {
            t.insert(1, Peer::Uid(uid), conv("x"));
        }
        t.insert(2, Peer::Uid(200), conv("y"));
        assert_eq!(t.of_conn(1).count(), 20);
        assert_eq!(t.forget_conn(1).len(), 20);
        assert_eq!(t.of_conn(1).count(), 0);
        assert_eq!(t.get(2, &Peer::Uid(200)).unwrap().name, "y");
        assert!(t.forget_conn(1).is_empty());
    }

    #[test]
    fn a_friend_is_titled_with_the_login_they_do_not_go_by() {
        let cases = [("alice", "alice", "alice"), ("Al", "alice", "Al (alice)")];
        for (name, login, want) in cases {
            assert_eq!(login_title(name, login), want);
        }
    }

    #[test]
    fn is_self_needs_a_nick_of_our_own() {
        let cases = [
            (Some("me"), "me", true),
            (Some("you"), "me", false),
            (Some(""), "", false),
            (Some("me"), "", false),
            (None, "me", false),
            (Some("Me"), "me", false),
        ];
        for (name, own, want) in cases {
            let got = is_self(name.map(str::as_bytes), own.as_bytes());
            assert_eq!(got, want, "{name:?} vs {own:?}");
        }
    }

    #[test]
    fn a_bare_image_caption_is_dropped_only_beside_its_picture() {
        let cases = [
            (Some("[image]"), true, ""),
            (Some("[image]"), false, "[image]"),
            (Some("look [image]"), true, "look [image]"),
            (Some("hi"), false, "hi"),
            (None, true, ""),
            (None, false, ""),
        ];
        for (body, media, want) in cases {
            let got = shown_body(body.map(str::as_bytes), media);
            assert_eq!(got, want.as_bytes(), "{body:?} media={media}");
        }
    }

    #[test]
    fn each_line_is_a_row() {
        let cases: &[(&str, &[&str])] = &[
            ("", &[""]),
            ("hi", &["hi"]),
            ("a\nb", &["a", "b"]),
            ("a\n", &["a", ""]),
            ("\n\nc", &["", "", "c"]),
        ];
        for &(body, want) in cases {
            assert_eq!(rows(body).collect::<Vec<_>>(), want, "{body:?}");
        }
    }

    #[test]
    fn admin_is_at_least_2_and_away_is_odd() {
        let cases = [
            (0, false, false),
            (1, false, true),
            (2, true, false),
            (3, true, true),
            // Past the two bits, admin is "at least 2" rather than bit 1.
            (4, true, false),
            (5, true, true),
        ];
        for (color, admin, away) in cases {
            assert_eq!(Status::of(color), Status { admin, away }, "color {color}");
        }
    }
}
