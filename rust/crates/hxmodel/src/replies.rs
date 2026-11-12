//! What a chat view knows of the lines its replies answer (the Replies
//! draft, capability bit 18): each line's quote as it was drawn, and the
//! quotes of lines it never drew, asked for by id with Get Chat History
//! and held a while.
//!
//! A server answers one request for at most fifty lines, leaving out
//! those it does not keep and those the reader may not read alike, so a
//! line an answer leaves out is one GtkHx can't show, and is not asked for
//! again while that answer is held.

#![forbid(unsafe_code)]

use std::collections::{BTreeSet, HashMap, VecDeque};

use hxproto::replies::MAX_IDS;

/// The most characters of a line a quote keeps: more than a header shows
/// on one line, which is all it ever shows.
pub const MAX_EXCERPT: usize = 160;

/// The most answers a view holds; one let go of may be asked for again.
pub const HELD: usize = 500;

/// A line as a reply's header quotes it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Quote {
    pub nick: String,
    pub excerpt: String,
}

impl Quote {
    /// `nick`'s line `text`, its line breaks folded to spaces and cut to
    /// [`MAX_EXCERPT`] characters.
    pub fn new(nick: &str, text: &str) -> Quote {
        let excerpt = text
            .replace("\r\n", " ")
            .chars()
            .map(|c| if c == '\r' || c == '\n' { ' ' } else { c })
            .take(MAX_EXCERPT)
            .collect();
        Quote {
            nick: nick.to_owned(),
            excerpt,
        }
    }
}

/// What the server said of a line asked for by id.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Quoted {
    Line(Quote),
    /// Taken away: the server sent its tombstone.
    Removed,
    /// Said by someone the user ignores.
    Hidden,
    /// Left out of a whole answer, or refused: not to be shown.
    Unavailable,
    /// Refused for the rate: asked for again with the next that is wanted.
    Later,
}

/// The request in flight.
#[derive(Debug)]
struct Flight {
    ids: Vec<u64>,
    /// It has been put off once already.
    again: bool,
}

/// How a refused request ends.
#[derive(Debug, PartialEq, Eq)]
pub enum Refused {
    /// Ask for the same again, after a while: [`Quotes::again`].
    Retry,
    /// What each id it asked for holds now.
    Settled(Vec<(u64, Quoted)>),
}

/// One view's quotes of lines asked for by id.
#[derive(Debug, Default)]
pub struct Quotes {
    held: HashMap<u64, Quoted>,
    /// [`Self::held`]'s ids, oldest answer first.
    order: VecDeque<u64>,
    wanted: BTreeSet<u64>,
    flight: Option<Flight>,
    /// A refusal other than the rate: nothing more is asked.
    stopped: bool,
}

impl Quotes {
    /// What the server said of line `id`, while it is held.
    pub fn get(&self, id: u64) -> Option<&Quoted> {
        self.held.get(&id)
    }

    /// Line `id` is to be asked for, unless it is held or asked for
    /// already; those put off for the rate go with it.
    pub fn want(&mut self, id: u64) {
        if self.stopped {
            return;
        }
        let later: Vec<u64> = (self.held.iter())
            .filter(|(_, q)| **q == Quoted::Later)
            .map(|(&id, _)| id)
            .collect();
        self.order.retain(|o| !later.contains(o));
        for id in later {
            self.held.remove(&id);
            self.wanted.insert(id);
        }
        let asked = self.flight.as_ref().is_some_and(|f| f.ids.contains(&id));
        if !self.held.contains_key(&id) && !asked {
            self.wanted.insert(id);
        }
    }

    /// The next request: the lowest wanted ids, up to [`MAX_IDS`] of them,
    /// those `drawn` since they were wanted aside. None while one is in
    /// flight or once a refusal has stopped them.
    pub fn next(&mut self, drawn: impl Fn(u64) -> bool) -> Option<Vec<u64>> {
        self.wanted.retain(|&id| !drawn(id));
        if self.flight.is_some() || self.stopped || self.wanted.is_empty() {
            return None;
        }
        let ids: Vec<u64> = self.wanted.iter().take(MAX_IDS).copied().collect();
        for id in &ids {
            self.wanted.remove(id);
        }
        self.flight = Some(Flight {
            ids: ids.clone(),
            again: false,
        });
        Some(ids)
    }

    /// Whether `asked` is the request in flight.
    pub fn in_flight(&self, asked: &[u64]) -> bool {
        self.flight.as_ref().is_some_and(|f| f.ids == asked)
    }

    /// The answer to the request in flight: the lines it holds, and
    /// whether the server left some out for the frame. An id it left out
    /// for the frame, past the last it holds, is wanted again; any other
    /// it left out is unavailable. What each id holds now.
    pub fn answered(&mut self, got: Vec<(u64, Quoted)>, has_more: bool) -> Vec<(u64, Quoted)> {
        let Some(flight) = self.flight.take() else {
            return Vec::new();
        };
        let mut got: HashMap<u64, Quoted> = got
            .into_iter()
            .filter(|(id, _)| flight.ids.contains(id))
            .collect();
        let last = got.keys().max().copied();
        let mut now = Vec::new();
        for id in flight.ids {
            let q = match got.remove(&id) {
                Some(q) => q,
                None if has_more && last.is_some_and(|l| id > l) => {
                    self.wanted.insert(id);
                    continue;
                }
                None => Quoted::Unavailable,
            };
            self.hold(id, q.clone());
            now.push((id, q));
        }
        now
    }

    /// The request in flight was refused, for the rate (`slow`) or not.
    /// The first refusal for the rate is asked again; a second leaves its
    /// ids for later. Any other stops quotes altogether.
    pub fn refused(&mut self, slow: bool) -> Refused {
        let Some(flight) = self.flight.as_mut() else {
            return Refused::Settled(Vec::new());
        };
        if slow && !flight.again {
            flight.again = true;
            return Refused::Retry;
        }
        let flight = self.flight.take().unwrap();
        let q = if slow {
            Quoted::Later
        } else {
            self.stopped = true;
            self.wanted.clear();
            Quoted::Unavailable
        };
        let now: Vec<_> = flight.ids.into_iter().map(|id| (id, q.clone())).collect();
        for (id, q) in &now {
            self.hold(*id, q.clone());
        }
        Refused::Settled(now)
    }

    /// The request put off by [`Refused::Retry`], to send again.
    pub fn again(&self) -> Option<Vec<u64>> {
        self.flight
            .as_ref()
            .filter(|f| f.again)
            .map(|f| f.ids.clone())
    }

    fn hold(&mut self, id: u64, q: Quoted) {
        if self.held.insert(id, q).is_none() {
            self.order.push_back(id);
        }
        while self.held.len() > HELD {
            let Some(old) = self.order.pop_front() else {
                break;
            };
            self.held.remove(&old);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(nick: &str, text: &str) -> Quoted {
        Quoted::Line(Quote::new(nick, text))
    }

    #[test]
    fn a_quote_is_one_line_cut_where_a_character_ends() {
        let long = "\u{e9}".repeat(MAX_EXCERPT + 5);
        for (text, want) in [
            ("hi there", "hi there".to_owned()),
            ("one\ntwo\r\nthree\rfour", "one two three four".to_owned()),
            (&long[..], "\u{e9}".repeat(MAX_EXCERPT)),
        ] {
            assert_eq!(Quote::new("a", text).excerpt, want, "{text:?}");
        }
    }

    /// Fifty at a time, lowest first, one request at a time, and an id
    /// asked for or held is not asked for again.
    #[test]
    fn ids_are_asked_for_fifty_at_a_time_and_once() {
        let mut q = Quotes::default();
        for id in (1..=60).rev() {
            q.want(id);
        }
        let first = q.next(|_| false).unwrap();
        assert_eq!(first, (1..=50).collect::<Vec<_>>());
        assert_eq!(q.next(|_| false), None, "one in flight");
        q.want(7);
        let got = (1..=50).map(|id| (id, line("a", "hi"))).collect();
        assert_eq!(q.answered(got, false).len(), 50);
        q.want(7);
        assert_eq!(q.get(7), Some(&line("a", "hi")));
        assert_eq!(
            q.next(|id| id == 55),
            Some(vec![51, 52, 53, 54, 56, 57, 58, 59, 60])
        );
    }

    /// An id left out of a whole answer is unavailable; one left out for
    /// the frame, past the last line the answer holds, is asked again.
    #[test]
    fn what_an_answer_leaves_out_is_unavailable_unless_it_was_cut_short() {
        let mut q = Quotes::default();
        for id in [3, 5, 8, 9] {
            q.want(id);
        }
        q.next(|_| false).unwrap();
        let now = q.answered(
            vec![(5, Quoted::Removed), (99, line("x", "not asked"))],
            true,
        );
        assert_eq!(now, [(3, Quoted::Unavailable), (5, Quoted::Removed)]);
        assert_eq!(q.get(99), None);
        assert_eq!(q.next(|_| false), Some(vec![8, 9]));
        assert_eq!(
            q.answered(vec![], false),
            [(8, Quoted::Unavailable), (9, Quoted::Unavailable)]
        );
    }

    /// Asked to slow down, a request goes again once; a second time its
    /// ids wait for the next request, and any other refusal stops quotes.
    #[test]
    fn a_refusal_for_the_rate_is_asked_again_and_any_other_stops_quotes() {
        let mut q = Quotes::default();
        q.want(4);
        q.next(|_| false).unwrap();
        assert_eq!(q.again(), None);
        assert_eq!(q.refused(true), Refused::Retry);
        assert_eq!(q.again(), Some(vec![4]));
        assert_eq!(q.refused(true), Refused::Settled(vec![(4, Quoted::Later)]));
        q.want(6);
        assert_eq!(q.next(|_| false), Some(vec![4, 6]));
        assert_eq!(
            q.refused(false),
            Refused::Settled(vec![(4, Quoted::Unavailable), (6, Quoted::Unavailable)])
        );
        q.want(7);
        assert_eq!(q.next(|_| false), None);
    }

    /// One put off for the rate, however often, is held once.
    #[test]
    fn the_oldest_answers_are_let_go_of_and_may_be_asked_again() {
        let mut q = Quotes::default();
        for _ in 0..3 {
            q.want(1);
            q.next(|_| false).unwrap();
            q.refused(true);
            q.refused(true);
        }
        q.want(1);
        q.next(|_| false).unwrap();
        q.answered(vec![(1, line("a", "hi"))], false);
        assert_eq!(q.order.len(), q.held.len());
        for id in 2..=(HELD as u64 + 1) {
            q.want(id);
            q.next(|_| false).unwrap();
            q.answered(vec![(id, line("a", "hi"))], false);
        }
        assert_eq!(q.get(1), None);
        assert_eq!(q.get(2), Some(&line("a", "hi")));
        q.want(1);
        assert_eq!(q.next(|_| false), Some(vec![1]));
    }
}
