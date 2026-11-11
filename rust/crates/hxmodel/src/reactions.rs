//! A line's reactions as GtkHx shows them (the Reactions draft, capability
//! bit 19): what the server last said, with the reactions the user has
//! asked for and not yet had answered drawn over it. What the server says
//! in between replaces only what it said, so neither a change it tells nor
//! a refusal undoes the other.

#![forbid(unsafe_code)]

use hxproto::reactions::{Key, Mine, Summary, TOO_MANY_KEYS, TOO_MANY_OF_YOURS};

/// One key's chip: how many hold it, and whether we are among them.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Shown {
    pub key: Key,
    pub count: u32,
    pub mine: bool,
}

/// What to do after the user wants a key on or off.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Next {
    /// Ask for it: an add, or a removal.
    Send(bool),
    /// Nothing more: it is asked for already, or already so.
    Nothing,
    /// The request already made would now undo itself: take it back if it
    /// has not gone yet, and [`LineReactions::refused`] it if so.
    Withdraw,
}

/// A request for one key, not answered yet.
#[derive(Debug, Clone)]
struct Act {
    key: Key,
    /// Whether it adds.
    sent: bool,
    /// What the user wants since: the request's answer asks again when it
    /// leaves the key otherwise.
    want: bool,
}

#[derive(Debug, Clone, Default)]
pub struct LineReactions {
    said: Vec<Shown>,
    acts: Vec<Act>,
}

/// Two keys are one with an emoji's U+FE0F aside, since a server may spell
/// a key otherwise than it was sent.
fn same(a: &Key, b: &Key) -> bool {
    match (a, b) {
        (Key::Emoji(a), Key::Emoji(b)) => {
            let bare = |s: &str| s.chars().filter(|&c| c != '\u{FE0F}').collect::<String>();
            bare(a) == bare(b)
        }
        _ => a == b,
    }
}

impl LineReactions {
    /// The server says the line holds `summary`, ours among it where `mine`
    /// says.
    pub fn replace(&mut self, summary: Summary, mine: &Mine) {
        self.said = (summary.entries.into_iter().enumerate())
            .map(|(i, (key, count))| Shown {
                key,
                count,
                mine: mine.holds(i),
            })
            .collect();
    }

    /// The chips, in the server's order: each key the user wants otherwise
    /// counted as wanted, and those it has not named yet after.
    pub fn shown(&self) -> Vec<Shown> {
        let act = |key: &Key| self.acts.iter().find(|a| same(&a.key, key));
        let mut shown: Vec<Shown> = (self.said.iter())
            .map(|s| match act(&s.key) {
                Some(a) => Shown {
                    count: s.count.saturating_sub(u32::from(s.mine)) + u32::from(a.want),
                    mine: a.want,
                    ..s.clone()
                },
                None => s.clone(),
            })
            .collect();
        let new = |a: &&Act| a.want && !self.said.iter().any(|s| same(&s.key, &a.key));
        shown.extend(self.acts.iter().filter(new).map(|a| Shown {
            key: a.key.clone(),
            count: 1,
            mine: true,
        }));
        shown.retain(|s| s.count > 0);
        shown
    }

    /// Whether `key` is shown as ours.
    pub fn mine(&self, key: &Key) -> bool {
        self.shown().iter().any(|s| s.mine && same(&s.key, key))
    }

    /// Why one more of ours with `key` would be refused by the limits the
    /// login gave, as the draft's code: a new kind past `max_keys`, or one
    /// more of ours past `max_per_user`.
    pub fn refusal(
        &self,
        key: &Key,
        max_keys: Option<u32>,
        max_per_user: Option<u32>,
    ) -> Option<u16> {
        let shown = self.shown();
        let full = |n: usize, max: Option<u32>| max.is_some_and(|m| n as u64 >= u64::from(m));
        if !shown.iter().any(|s| same(&s.key, key)) && full(shown.len(), max_keys) {
            Some(TOO_MANY_KEYS)
        } else if full(shown.iter().filter(|s| s.mine).count(), max_per_user) {
            Some(TOO_MANY_OF_YOURS)
        } else {
            None
        }
    }

    /// The user wants `key` on or off. One request a key is unanswered at
    /// a time, so clicks while it is make no more.
    pub fn want(&mut self, key: Key, on: bool) -> Next {
        if let Some(a) = self.acts.iter_mut().find(|a| same(&a.key, &key)) {
            a.want = on;
            return if on == a.sent {
                Next::Nothing
            } else {
                Next::Withdraw
            };
        }
        if self.said.iter().any(|s| s.mine && same(&s.key, &key)) == on {
            return Next::Nothing;
        }
        self.acts.push(Act {
            key,
            sent: on,
            want: on,
        });
        Next::Send(on)
    }

    /// The request for `key` that `add`ed or not was made, the key held by
    /// `count` after where the server says. Whether to ask again, for what
    /// the user has wanted since.
    pub fn answered(&mut self, key: &Key, add: bool, count: Option<u32>) -> Option<bool> {
        if let Some(count) = count {
            match self.said.iter().position(|s| same(&s.key, key)) {
                Some(i) => (self.said[i].count, self.said[i].mine) = (count, add),
                // Until the server tells the line's reactions in its own
                // spelling.
                None if count > 0 => self.said.push(Shown {
                    key: key.clone(),
                    count,
                    mine: add,
                }),
                None => {}
            }
        }
        let i = self.acts.iter().position(|a| same(&a.key, key))?;
        if self.acts[i].want == add {
            self.acts.remove(i);
            return None;
        }
        self.acts[i].sent = !add;
        Some(!add)
    }

    /// The request for `key` was refused, or taken back before it went:
    /// what the server said stands.
    pub fn refused(&mut self, key: &Key) {
        self.acts.retain(|a| !same(&a.key, key));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const THUMBS: &str = "\u{1F44D}";
    const HEART: &str = "\u{2764}\u{FE0F}";

    fn key(e: &str) -> Key {
        Key::Emoji(e.into())
    }

    enum Op {
        Said(&'static [(&'static str, u32)], &'static [u8]),
        Want(&'static str, bool, Next),
        Answered(&'static str, bool, Option<u32>, Option<bool>),
        Refused(&'static str),
    }
    use Op::*;

    /// Each case's steps in order, each step's answer as it says, then the
    /// chips shown: key, count, ours.
    #[test]
    fn what_is_shown_is_what_the_server_said_under_what_the_user_asked() {
        for (case, ops, want) in [
            (
                "as said",
                &[Said(&[(THUMBS, 2), (HEART, 1)], &[1])][..],
                &[(THUMBS, 2, false), (HEART, 1, true)][..],
            ),
            (
                "an add shown at once",
                &[
                    Said(&[(THUMBS, 2)], &[]),
                    Want(THUMBS, true, Next::Send(true)),
                ],
                &[(THUMBS, 3, true)],
            ),
            (
                "a new key after the rest",
                &[
                    Said(&[(THUMBS, 2)], &[]),
                    Want(HEART, true, Next::Send(true)),
                ],
                &[(THUMBS, 2, false), (HEART, 1, true)],
            ),
            (
                "a removal shown at once, the last gone",
                &[
                    Said(&[(THUMBS, 1)], &[0]),
                    Want(THUMBS, false, Next::Send(false)),
                ],
                &[],
            ),
            (
                "already so",
                &[
                    Said(&[(THUMBS, 1)], &[0]),
                    Want(THUMBS, true, Next::Nothing),
                ],
                &[(THUMBS, 1, true)],
            ),
            (
                "clicked back while asked: taken back, and the answer asks again",
                &[
                    Said(&[(THUMBS, 2)], &[]),
                    Want(THUMBS, true, Next::Send(true)),
                    Want(THUMBS, false, Next::Withdraw),
                    Answered(THUMBS, true, Some(3), Some(false)),
                ],
                &[(THUMBS, 2, false)],
            ),
            (
                "clicked back and forth while asked: one request",
                &[
                    Said(&[], &[]),
                    Want(THUMBS, true, Next::Send(true)),
                    Want(THUMBS, false, Next::Withdraw),
                    Want(THUMBS, true, Next::Nothing),
                    Answered(THUMBS, true, Some(1), None),
                ],
                &[(THUMBS, 1, true)],
            ),
            (
                "the answer's count, the other keys as said",
                &[
                    Said(&[(THUMBS, 2), (HEART, 4)], &[]),
                    Want(THUMBS, true, Next::Send(true)),
                    Answered(THUMBS, true, Some(5), None),
                ],
                &[(THUMBS, 5, true), (HEART, 4, false)],
            ),
            (
                "an answer of none takes the key away",
                &[
                    Said(&[(THUMBS, 1)], &[0]),
                    Want(THUMBS, false, Next::Send(false)),
                    Answered(THUMBS, false, Some(0), None),
                ],
                &[],
            ),
            (
                "a key answered in another spelling is one key",
                &[
                    Said(&[("\u{2764}", 1)], &[]),
                    Want(HEART, true, Next::Send(true)),
                    Answered(HEART, true, Some(2), None),
                ],
                &[("\u{2764}", 2, true)],
            ),
            (
                "a change told between: replaced under the request",
                &[
                    Said(&[(THUMBS, 1)], &[]),
                    Want(THUMBS, true, Next::Send(true)),
                    Said(&[(THUMBS, 4)], &[]),
                ],
                &[(THUMBS, 5, true)],
            ),
            (
                "a change told between that has ours already",
                &[
                    Said(&[(THUMBS, 1)], &[]),
                    Want(THUMBS, true, Next::Send(true)),
                    Said(&[(THUMBS, 2)], &[0]),
                ],
                &[(THUMBS, 2, true)],
            ),
            (
                "a refusal leaves what was told since",
                &[
                    Said(&[(THUMBS, 1)], &[]),
                    Want(THUMBS, true, Next::Send(true)),
                    Said(&[(THUMBS, 4)], &[]),
                    Refused(THUMBS),
                ],
                &[(THUMBS, 4, false)],
            ),
            (
                "an answer with no count",
                &[
                    Said(&[(THUMBS, 1)], &[]),
                    Want(THUMBS, true, Next::Send(true)),
                    Answered(THUMBS, true, None, None),
                ],
                &[(THUMBS, 1, false)],
            ),
        ] {
            let mut r = LineReactions::default();
            for op in ops {
                match op {
                    Said(entries, mine) => {
                        let entries = entries.iter().map(|(e, n)| (key(e), *n)).collect();
                        r.replace(Summary { entries }, &Mine(mine.to_vec()));
                    }
                    Want(e, on, next) => assert_eq!(r.want(key(e), *on), *next, "{case}"),
                    Answered(e, add, count, again) => {
                        assert_eq!(r.answered(&key(e), *add, *count), *again, "{case}")
                    }
                    Refused(e) => r.refused(&key(e)),
                }
            }
            let shown: Vec<_> = r
                .shown()
                .into_iter()
                .map(|s| (s.key, s.count, s.mine))
                .collect();
            let want: Vec<_> = want.iter().map(|(e, n, m)| (key(e), *n, *m)).collect();
            assert_eq!(shown, want, "{case}");
        }
    }

    #[test]
    fn one_more_of_ours_is_refused_at_the_logins_limits() {
        let mut r = LineReactions::default();
        let summary = Summary {
            entries: vec![(key(THUMBS), 1), (key(HEART), 3), (Key::Server(9), 1)],
        };
        r.replace(summary, &Mine(vec![2]));
        for (case, e, max_keys, max_per_user, code) in [
            ("no limits", "\u{1F600}", None, None, None),
            (
                "a new kind past the kinds",
                "\u{1F600}",
                Some(3),
                None,
                Some(TOO_MANY_KEYS),
            ),
            ("a kind there already", THUMBS, Some(3), None, None),
            (
                "one more of ours past ours",
                THUMBS,
                None,
                Some(1),
                Some(TOO_MANY_OF_YOURS),
            ),
            ("room for both", "\u{1F600}", Some(4), Some(2), None),
        ] {
            assert_eq!(r.refusal(&key(e), max_keys, max_per_user), code, "{case}");
        }
    }
}
