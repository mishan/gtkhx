//! `hxchat-model` — the pure, testable chat conversation model + nick
//! completion (Phase R5 M1; the Rust re-think of `struct chat` /
//! `struct gtkhx_chat` — see `docs/rust/gchats-model-rethink.md`).
//!
//! This crate is *only* the model half: a [`Conversation`] owns its `cid`,
//! `subject`, and an ordered, uid-indexed [`MemberList`], plus the
//! nick-[`complete`]ion logic that used to live tangled in `chat.c`'s
//! `tab_nick_comp` family. No glib, no GTK, no globals — so every rule here is
//! unit-tested, which the ~500 lines of C buffer manipulation it replaces
//! never were.
//!
//! ## Divergence from the C on purpose
//!
//! The C completion had two latent bugs this port deliberately does **not**
//! carry forward (the tests pin the corrected behaviour):
//!
//!  * **Cycling was dead.** `tab_nick_comp_next` / `nick_comp_chng` computed
//!    the next/previous nick into a local buffer, then the caller `return 0`ed
//!    without ever writing it back to the entry — so Tab-cycling an
//!    already-completed nick did nothing. Here [`complete`] cycles for real.
//!  * **Mid-buffer cursor math was off by one.** The C set the caret to
//!    `strlen(before) + strlen(name)`, omitting the re-inserted space, landing
//!    it one char inside the name. Here the caret lands right after the
//!    inserted name.

#![forbid(unsafe_code)]

use std::collections::HashMap;

mod input_history;
pub use input_history::InputHistory;

/// A per-user nickname colour (Colored-Nicknames extension): `Some(0x00RRGGBB)`
/// or `None` for "no colour set" (the C `HX_NICK_COLOR_NONE` 0xFFFFFFFF
/// sentinel, replaced here by the type system).
pub type NickColor = Option<u32>;

/// A chat member — the model half of the C `struct hx_user` (the *view* half,
/// the `HxUserRow` GObject, stays in `users_row.rs`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Member {
    pub uid: u16,
    pub icon: u16,
    /// Admin / Guest / Away status bitmap (the C `hx_user.color`).
    pub status: u16,
    pub nick_color: NickColor,
    pub name: String,
    pub ignore: bool,
}

impl Member {
    /// Minimal member with just a uid + name (icon/status/colour default,
    /// not ignored) — the common shape the user-list parse fills first.
    pub fn new(uid: u16, name: impl Into<String>) -> Self {
        Member {
            uid,
            icon: 0,
            status: 0,
            nick_color: None,
            name: name.into(),
            ignore: false,
        }
    }
}

/// The members of one chat: insertion-ordered with O(1) uid lookup. Replaces
/// `struct chat`'s `GHashTable<uid, hx_user*>` — the hashtable gave O(1)
/// lookup but no order, forcing `public_chat_users_sorted` to rebuild a sorted
/// list on every keystroke. Here order is stable and the sorted-name view is a
/// cheap on-demand borrow.
#[derive(Debug, Default, Clone)]
pub struct MemberList {
    order: Vec<Member>,
    /// uid → index into `order`.
    index: HashMap<u16, usize>,
}

impl MemberList {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn len(&self) -> usize {
        self.order.len()
    }

    pub fn is_empty(&self) -> bool {
        self.order.is_empty()
    }

    /// Insert a member, or replace the existing one with the same uid in place
    /// (keeping its position — a rename/recolour shouldn't reorder the list).
    pub fn upsert(&mut self, member: Member) {
        if let Some(&i) = self.index.get(&member.uid) {
            self.order[i] = member;
        } else {
            self.index.insert(member.uid, self.order.len());
            self.order.push(member);
        }
    }

    /// Remove the member with `uid`, returning it. O(n) (a swap-remove would
    /// scramble order); membership churn is small and rare relative to lookups.
    pub fn remove(&mut self, uid: u16) -> Option<Member> {
        let i = self.index.remove(&uid)?;
        let removed = self.order.remove(i);
        // Re-index everything after the hole (their positions shifted down 1).
        for (j, m) in self.order.iter().enumerate().skip(i) {
            self.index.insert(m.uid, j);
        }
        Some(removed)
    }

    pub fn get(&self, uid: u16) -> Option<&Member> {
        self.index.get(&uid).map(|&i| &self.order[i])
    }

    pub fn get_mut(&mut self, uid: u16) -> Option<&mut Member> {
        let i = *self.index.get(&uid)?;
        Some(&mut self.order[i])
    }

    /// Members in insertion order.
    pub fn iter(&self) -> impl Iterator<Item = &Member> {
        self.order.iter()
    }

    /// Member names, case-insensitively sorted — the deterministic walk order
    /// nick completion wants. Borrows names out of the list (no copies of the
    /// member structs).
    pub fn names_sorted(&self) -> Vec<&str> {
        let mut names: Vec<&str> = self.order.iter().map(|m| m.name.as_str()).collect();
        // sort_by_key computes each lowercase key once (Schwartzian) rather
        // than re-lowercasing both sides on every comparator invocation.
        names.sort_by_key(|n| n.to_ascii_lowercase());
        names
    }
}

/// One open chat: the protocol model (`cid` + `subject` + membership). The
/// public/lobby chat is `cid == 0`. Widgets, readline history, render cursors,
/// and the inline-media table are *not* here — those are the view's concern
/// (see the design doc's `ConversationView`).
#[derive(Debug, Clone)]
pub struct Conversation {
    pub cid: u32,
    pub subject: String,
    pub members: MemberList,
}

impl Conversation {
    pub fn new(cid: u32) -> Self {
        Conversation {
            cid,
            subject: String::new(),
            members: MemberList::new(),
        }
    }

    /// Nick-complete the word under the caret. Thin wrapper that hands the
    /// list's sorted names to [`complete`]. `suffix` is the
    /// Nickname-Completion-Suffix char (`:` by default in the C).
    pub fn complete(
        &self,
        input: &str,
        cursor: usize,
        reverse: bool,
        suffix: char,
    ) -> Option<Completion> {
        complete(&self.members.names_sorted(), input, cursor, reverse, suffix)
    }
}

// ---------------------------------------------------------------------------
// Nick completion
// ---------------------------------------------------------------------------

/// The fixed delimiters that end a nick token. The configurable completion
/// `suffix` is the fourth, applied at runtime — mirroring the C
/// `not_nick_chars` built as `" .?%c"` with `%c` == the suffix char.
const BASE_DELIMS: [char; 3] = [' ', '.', '?'];

/// The result of a [`complete`] call: the new full buffer text and where to
/// put the caret, plus — when the prefix is ambiguous — the list of candidate
/// names for the caller to surface (the C printed these to the chat output).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Completion {
    /// The new full input-buffer text (leading spaces already trimmed).
    pub text: String,
    /// Caret position as a char offset, or `None` to place it at the end.
    pub cursor: Option<usize>,
    /// Candidate names when the completion is ambiguous (>1 match and no
    /// unique full completion); empty otherwise. The view may display them.
    pub info: Vec<String>,
}

/// Case-insensitive prefix match (ASCII — nicks are ASCII / Mac Roman).
fn has_prefix_ci(name: &str, prefix: &str) -> bool {
    name.len() >= prefix.len()
        && name.as_bytes()[..prefix.len()].eq_ignore_ascii_case(prefix.as_bytes())
}

/// Longest common prefix (case-insensitive, but preserving the *first*
/// candidate's casing) across `names`. Assumes `names` is non-empty.
fn common_prefix_ci<'a>(names: &[&'a str]) -> &'a str {
    let first = names[0];
    let mut len = first.len();
    for &n in &names[1..] {
        let mut i = 0;
        let (fb, nb) = (first.as_bytes(), n.as_bytes());
        while i < len && i < nb.len() && fb[i].eq_ignore_ascii_case(&nb[i]) {
            i += 1;
        }
        len = i;
        if len == 0 {
            break;
        }
    }
    // `len` is a byte count from ASCII-wise comparison; two different
    // non-ASCII codepoints can share a leading byte, leaving `len` inside a
    // multi-byte sequence. Back it off to the nearest char boundary of `first`
    // so the slice is always valid UTF-8.
    while len > 0 && !first.is_char_boundary(len) {
        len -= 1;
    }
    &first[..len]
}

/// Cycle from index `cur` in a list of `len`, wrapping. `reverse` steps back.
fn cycle(cur: usize, len: usize, reverse: bool) -> usize {
    if reverse {
        (cur + len - 1) % len
    } else {
        (cur + 1) % len
    }
}

/// Nick-complete the token under the caret in `input`.
///
/// `names_sorted` is the deterministic (case-insensitive sorted) candidate
/// list. `cursor` is a char offset into `input`. `reverse` steps the cycle
/// backwards (Shift+Tab). `suffix` is appended after a nick completed at the
/// very start of the buffer (address form, e.g. `nick: `).
///
/// Returns `None` when there's nothing to do (empty token, no matches).
pub fn complete(
    names_sorted: &[&str],
    input: &str,
    cursor: usize,
    reverse: bool,
    suffix: char,
) -> Option<Completion> {
    let chars: Vec<char> = input.chars().collect();
    let pos = cursor.min(chars.len());

    // "Address form": the whole buffer is a single bare nick (none of the
    // delimiters — the fixed set OR the configured suffix — appears) — the C
    // `first == 1` case. Complete to `name<suffix> `. A closure predicate
    // (rather than a `[char; N]` pattern) folds the runtime suffix in and is
    // unambiguous across toolchains.
    let is_delim = |c: char| BASE_DELIMS.contains(&c) || c == suffix;
    let address_form = !input.contains(is_delim);

    // The token under the caret runs from just after the previous space to the
    // caret. `before` is everything up to (not including) that space; `after`
    // is everything from the caret on. In address form the token is the whole
    // buffer up to the caret.
    let word_start = if address_form {
        0
    } else {
        chars[..pos]
            .iter()
            .rposition(|&c| c == ' ')
            .map(|i| i + 1)
            .unwrap_or(0)
    };
    let word: String = chars[word_start..pos].iter().collect();
    if word.is_empty() {
        return None;
    }
    let before: String = if word_start == 0 {
        String::new()
    } else {
        chars[..word_start - 1].iter().collect()
    };
    let after: String = chars[pos..].iter().collect();

    // Assemble a mid-buffer result: `before` + space + `name` + `after`, with
    // the caret right after `name`. Leading space (when `before` is empty) is
    // trimmed, and the caret compensates.
    let rebuild = |name: &str| -> Completion {
        let raw = format!("{before} {name}{after}");
        let trimmed = raw.trim_start_matches(' ');
        let trimmed_lead = raw.len() - trimmed.len();
        // caret = chars of `before` + the space + chars of `name`, minus any
        // leading spaces we trimmed off the front.
        let before_chars = before.chars().count();
        let name_chars = name.chars().count();
        let caret = before_chars + 1 + name_chars - trimmed_lead;
        Completion {
            text: trimmed.to_string(),
            cursor: Some(caret),
            info: Vec::new(),
        }
    };

    // Already exactly a nick (case-sensitive, as the C `strcmp`) → cycle to the
    // next / previous candidate. This is the path the C computed but discarded.
    if let Some(idx) = names_sorted.iter().position(|&n| n == word) {
        if names_sorted.len() > 1 {
            let name = names_sorted[cycle(idx, names_sorted.len(), reverse)];
            if address_form {
                return Some(Completion {
                    text: format!("{name}{suffix} "),
                    cursor: None,
                    info: Vec::new(),
                });
            }
            return Some(rebuild(name));
        }
        // Sole member equal to the word: nothing to cycle to.
        return None;
    }

    // Prefix matches (case-insensitive), de-duplicated by name.
    let mut matches: Vec<&str> = Vec::new();
    for &n in names_sorted {
        if has_prefix_ci(n, &word) && !matches.iter().any(|m| m.eq_ignore_ascii_case(n)) {
            matches.push(n);
        }
    }

    match matches.len() {
        0 => None,
        1 => {
            let name = matches[0];
            if address_form {
                Some(Completion {
                    text: format!("{name}{suffix} "),
                    cursor: None,
                    info: Vec::new(),
                })
            } else {
                Some(rebuild(name))
            }
        }
        _ => {
            // Ambiguous: extend the token to the longest common prefix (if it's
            // longer than what's typed) and surface the candidate list.
            let cp = common_prefix_ci(&matches);
            let info: Vec<String> = matches.iter().map(|s| s.to_string()).collect();
            if cp.len() > word.len() {
                if address_form {
                    Some(Completion {
                        text: cp.to_string(),
                        cursor: None,
                        info,
                    })
                } else {
                    let mut c = rebuild(cp);
                    c.info = info;
                    Some(c)
                }
            } else {
                // No further common prefix — just report the candidates, leave
                // the buffer as-is.
                Some(Completion {
                    text: input.to_string(),
                    cursor: Some(pos),
                    info,
                })
            }
        }
    }
}

#[cfg(test)]
mod tests;
