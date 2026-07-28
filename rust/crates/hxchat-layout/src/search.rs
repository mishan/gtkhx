//! In-buffer search: find every occurrence of a needle, and a cursor
//! over the results.
//!
//! This is net-new rather than a port. xtext has a `gtk_xtext_search`
//! (xtext.c:5190) built on GRegex plus a `search_found` list threaded
//! through the entry chain, but nothing in GtkHx has ever called it —
//! it arrived with the HexChat vendoring and has never run under GTK 4.
//! Reproducing it would mean debugging a dead subsystem that C5 deletes,
//! so the engine here is written against the structured message model
//! instead, where a match is a `(message, source, byte range)` and needs
//! no parallel bookkeeping on the buffer at all.
//!
//! Matching is literal, not regex. The needle is what the user typed;
//! there is no metacharacter vocabulary to explain and no pathological
//! backtracking to defend against.

use crate::message::MessageId;
use crate::wrap::LineSource;

/// One occurrence, in the same coordinate system carets use: a byte
/// range within one source of one message.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Match {
    pub message: MessageId,
    pub source: LineSource,
    pub start: usize,
    pub end: usize,
}

impl Match {
    /// Does `pos` (a byte offset in the same source) fall inside?
    pub fn contains(&self, pos: usize) -> bool {
        pos >= self.start && pos < self.end
    }
}

/// A search and its results.
///
/// The cursor is an index into `matches`, so stepping is trivial and
/// wraps. `None` means "no match is current yet" — the state right
/// after a query changes, before the caller has picked a starting point.
#[derive(Debug, Default, Clone)]
pub struct SearchState {
    needle: String,
    case_sensitive: bool,
    matches: Vec<Match>,
    current: Option<usize>,
}

impl SearchState {
    pub fn new() -> SearchState {
        SearchState::default()
    }

    pub fn needle(&self) -> &str {
        &self.needle
    }

    pub fn case_sensitive(&self) -> bool {
        self.case_sensitive
    }

    pub fn is_active(&self) -> bool {
        !self.needle.is_empty()
    }

    pub fn matches(&self) -> &[Match] {
        &self.matches
    }

    pub fn len(&self) -> usize {
        self.matches.len()
    }

    pub fn is_empty(&self) -> bool {
        self.matches.is_empty()
    }

    /// 1-based position of the current match, for a "3 of 17" readout.
    pub fn ordinal(&self) -> Option<usize> {
        self.current.map(|i| i + 1)
    }

    pub fn current(&self) -> Option<Match> {
        self.current.and_then(|i| self.matches.get(i)).copied()
    }

    pub fn current_index(&self) -> Option<usize> {
        self.current
    }

    /// Is this exact occurrence the current one?
    ///
    /// Compared by value rather than by index so the renderer doesn't
    /// need to know where in the list it is.
    pub fn is_current(&self, m: &Match) -> bool {
        self.current() == Some(*m)
    }

    pub fn clear(&mut self) {
        self.needle.clear();
        self.matches.clear();
        self.current = None;
    }

    /// Install a fresh result set for `needle`.
    pub fn set_results(&mut self, needle: &str, case_sensitive: bool, matches: Vec<Match>) {
        self.needle = needle.to_string();
        self.case_sensitive = case_sensitive;
        self.matches = matches;
        self.current = None;
    }

    /// Point the cursor at the first match at or after `message`, so
    /// opening the find bar starts from what is on screen rather than
    /// from the top of a long scrollback.
    ///
    /// Falls back to the last match when everything is above the
    /// viewport, which is the useful direction: scrollback grows
    /// downward and the interesting end is the recent one.
    pub fn seek_from(&mut self, order: impl Fn(MessageId) -> Option<usize>, from_row: usize) {
        if self.matches.is_empty() {
            self.current = None;
            return;
        }
        let at = self
            .matches
            .iter()
            .position(|m| order(m.message).is_some_and(|r| r >= from_row));
        self.current = Some(at.unwrap_or(self.matches.len() - 1));
    }

    /// Step the cursor. `dir > 0` forward, `dir < 0` back; both wrap.
    ///
    /// With no current match, a forward step selects the first and a
    /// backward step the last, so both keys do something useful the
    /// first time they are pressed.
    pub fn step(&mut self, dir: i32) -> Option<Match> {
        if self.matches.is_empty() {
            self.current = None;
            return None;
        }
        let n = self.matches.len();
        self.current = Some(match (self.current, dir >= 0) {
            (None, true) => 0,
            (None, false) => n - 1,
            (Some(i), true) => (i + 1) % n,
            (Some(i), false) => (i + n - 1) % n,
        });
        self.current()
    }
}

/// Every occurrence of `needle` in `haystack`, as byte ranges.
///
/// Occurrences do not overlap: after a match the scan resumes at its
/// end, so searching "aa" in "aaaa" finds two, not three. That is what
/// every find bar does and what makes the match count match what the
/// user can step through.
///
/// **Case-insensitive matching is per-character.** The obvious
/// implementation — lowercase both sides and search that — is wrong
/// here, because lowercasing can change a string's byte length (`İ`
/// U+0130 lowercases to two chars) and every offset it returns would
/// then be an offset into a string the caller does not have. Walking the
/// original preserves the offsets by construction. The cost is that
/// case-folds which change *character* count (`ß` vs `ss`) don't match;
/// that is a real limitation and a deliberate one.
pub fn find_all(haystack: &str, needle: &str, case_sensitive: bool) -> Vec<(usize, usize)> {
    let mut out = Vec::new();
    if needle.is_empty() || haystack.is_empty() {
        return out;
    }

    if case_sensitive {
        let mut base = 0usize;
        while let Some(rel) = haystack[base..].find(needle) {
            let start = base + rel;
            let end = start + needle.len();
            out.push((start, end));
            base = end;
        }
        return out;
    }

    let mut start = 0usize;
    while start < haystack.len() {
        if !haystack.is_char_boundary(start) {
            start += 1;
            continue;
        }
        match match_at(&haystack[start..], needle) {
            Some(len) => {
                out.push((start, start + len));
                // Zero-length can't happen (needle is non-empty) but
                // guard anyway: a zero-width advance here is an infinite
                // loop, and this runs on every keystroke.
                start += len.max(1);
            }
            None => {
                start += haystack[start..]
                    .chars()
                    .next()
                    .map(char::len_utf8)
                    .unwrap_or(1);
            }
        }
    }
    out
}

/// If `needle` matches a prefix of `hay` case-insensitively, the length
/// of that prefix **in `hay`'s bytes** — which is not necessarily the
/// needle's own length.
fn match_at(hay: &str, needle: &str) -> Option<usize> {
    let mut h = hay.chars();
    let mut n = needle.chars();
    let mut used = 0usize;
    loop {
        let Some(nc) = n.next() else { return Some(used) };
        let hc = h.next()?;
        if !eq_fold(hc, nc) {
            return None;
        }
        used += hc.len_utf8();
    }
}

/// Case-insensitive single-character comparison.
///
/// `to_lowercase` yields an iterator because one char can fold to
/// several; comparing only single-char folds is what limits this to
/// same-length case pairs, which covers every alphabet a Hotline server
/// is going to send.
fn eq_fold(a: char, b: char) -> bool {
    if a == b {
        return true;
    }
    let mut al = a.to_lowercase();
    let mut bl = b.to_lowercase();
    match (al.next(), bl.next()) {
        (Some(x), Some(y)) => x == y && al.next().is_none() && bl.next().is_none(),
        _ => false,
    }
}
