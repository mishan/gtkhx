//! Reading the old `gtkhxrc`.
//!
//! This module owns the *old* format and nothing else, the way
//! `hxbookmarks::legacy` owns the old bookmark format. It exists to be read
//! once, on the first run of a build that has `hxconfig`, and never written.
//!
//! There are two on-disk forms, because the file predates GKeyFile:
//!
//! 1. **A GKeyFile with a single `[gtkhx]` group.** What every save since the
//!    conversion produces.
//! 2. **Bare `KEY=VALUE` lines**, no group header. Only reachable through the
//!    `~/.gtkhxrc` fallback now, but a profile that has not been opened in
//!    twenty years is exactly the profile most in need of migrating.
//!
//! The C reader picks between them by trying GKeyFile first and falling back
//! when it refuses the file or the file has no `[gtkhx]` group. This does the
//! same.
//!
//! **One bug in the old line parser is deliberately not reproduced**: it
//! dropped a final line with no trailing newline. That is a `fgets` loop
//! mistake with no rationale behind it, so reading the line can only recover a
//! setting the old parser was throwing away.
//!
//! Its `#` truncation *is* reproduced, and only in the line form. It looks
//! like the same class of bug — it cuts a value at the first `#` anywhere on
//! the line, including inside a path — but `#` was the comment convention that
//! format's only parser ever defined, so truncating is that format being
//! parsed correctly rather than a value being lost. A hand-written
//! `XBUF_MAX=1000 # lines` means a thousand lines, and refusing to truncate
//! would turn it into an unparseable number and silently fall back to the
//! default. The GKeyFile form has no inline comments at all — `NICK=bob # hi`
//! really is the value `bob # hi` — so it must not truncate, and doesn't.

use std::collections::BTreeMap;

/// The group name the settings live under in the GKeyFile form.
const GROUP: &str = "gtkhx";

/// The key/value pairs of a `gtkhxrc`, keyed by the SHOUTING_CASE name.
pub type LegacyKeys = BTreeMap<String, String>;

/// Which of the two forms a file turned out to be. Only interesting for
/// diagnostics — both produce the same map.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Form {
    KeyFile,
    Lines,
}

/// Decode a `gtkhxrc`'s bytes, falling back to Mac Roman when they are not
/// UTF-8. The `bool` is whether the fallback was needed.
///
/// **A legacy file is not necessarily UTF-8, and treating that as unreadable
/// loses everything.** The C reader goes out of its way to handle it: the
/// `STRING32` arm validates and, on failure, runs the bytes through Mac Roman
/// before storing them, because a nickname typed on a Mac-era client breaks
/// GTK's input method otherwise. And `g_key_file_set_string` never validated,
/// so those bytes round-tripped through every save since. Unix paths are bytes
/// rather than text too, so a download directory or a font name can carry
/// Latin-1 just as easily.
///
/// Mac Roman rather than `from_utf8_lossy` because every byte maps to a
/// defined character, so nothing is replaced with U+FFFD — and because it is
/// the encoding the rest of this client assumes when bytes aren't UTF-8.
pub fn decode(bytes: &[u8]) -> (String, bool) {
    match std::str::from_utf8(bytes) {
        Ok(text) => (text.to_string(), false),
        Err(_) => (hotline_proto::text::to_utf8(bytes), true),
    }
}

/// Parse a `gtkhxrc`, whichever form it is in.
///
/// Never fails: a file that is neither form yields an empty map, which the
/// caller treats as "nothing to migrate". There is no error case worth
/// distinguishing, because a `gtkhxrc` that parses to nothing and a
/// `gtkhxrc` that does not exist lead to the same place — defaults.
pub fn parse(text: &str) -> (LegacyKeys, Form) {
    // GKeyFile first, exactly as the C reader does: it tries to load the file
    // and falls back only when that fails or there is no [gtkhx] group.
    if let Some(keys) = parse_keyfile(text) {
        return (keys, Form::KeyFile);
    }
    (parse_lines(text), Form::Lines)
}

/// The GKeyFile form. `None` when the file has no `[gtkhx]` group, which is
/// the C reader's signal to fall through to the line parser.
fn parse_keyfile(text: &str) -> Option<LegacyKeys> {
    let mut keys = LegacyKeys::new();
    let mut in_group = false;
    let mut saw_group = false;

    for raw in text.lines() {
        let line = raw.trim_end_matches('\r');
        let trimmed = line.trim_start();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix('[') {
            // GLib scans backwards for the `]`, so a trailing space after it
            // is fine. Trim before matching or `[gtkhx] ` would look like a
            // malformed group and take the whole file down the line-parser
            // path.
            let Some(name) = rest.trim_end().strip_suffix(']') else {
                continue;
            };
            // Case-sensitive, like GKeyFile: `[GtkHx]` is a different group.
            in_group = name == GROUP;
            saw_group |= in_group;
            continue;
        }
        if !in_group {
            continue;
        }
        let Some((key, value)) = trimmed.split_once('=') else {
            continue;
        };
        // GKeyFile trims whitespace around the separator on both sides, but
        // keeps trailing whitespace in the value — which matters, because the
        // default timestamp format ends in a space.
        let key = key.trim_end();
        // Locale variants (`KEY[de]=…`) are dropped, matching the load flags,
        // which do not ask to keep translations.
        if key.contains('[') {
            continue;
        }
        // A repeated key resolves to the last occurrence, as GKeyFile does.
        keys.insert(key.to_string(), unescape(value.trim_start()));
    }

    saw_group.then_some(keys)
}

/// The pre-GKeyFile form: bare `KEY=VALUE` lines, no group.
///
/// No unescaping here — nothing ever wrote this form through GKeyFile, so
/// there are no escapes in it, and running the unescaper would corrupt a path
/// that legitimately contains a backslash.
fn parse_lines(text: &str) -> LegacyKeys {
    let mut keys = LegacyKeys::new();
    for raw in text.lines() {
        let line = raw.trim_end_matches('\r');
        // `#` starts a comment, anywhere on the line. This is the one place
        // the old parser's behaviour is copied rather than corrected: it is
        // the only comment convention this format ever had, so a hand-written
        // `XBUF_MAX=1000 # lines` means a thousand lines. A `#` before any
        // `=` makes the whole line a comment, which is how comment lines work
        // at all.
        let line = line.split_once('#').map_or(line, |(before, _)| before);
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        // atoi ignored trailing whitespace, so `1000 # lines` reached the C
        // code as `1000 ` and parsed. Trim so it still does.
        keys.insert(key.to_string(), value.trim_end().to_string());
    }
    keys
}

/// Whether a value still looks like it has been through more than one
/// save/load cycle of the old escape asymmetry.
///
/// The writer escaped through `g_key_file_set_string` and the reader did not
/// unescape, so each cycle doubled every backslash: `\` → `\\` → `\\\\`. After
/// [`unescape`] has halved it once, a remaining doubled backslash means there
/// were more cycles than we can undo — we cannot know how many, so the honest
/// thing is to say so rather than guess.
///
/// The check cannot be exact, and knowingly so: a Windows UNC path really does
/// start `\\server\share`, and one saved a single time unescapes to exactly
/// that. The ambiguity is unresolvable from the file alone, so the value is
/// always carried across and the diagnostic says "check this" rather than
/// claiming the value is wrong.
pub fn looks_over_escaped(unescaped: &str) -> bool {
    unescaped.contains("\\\\")
}

/// The inverse of `g_key_file_set_string`.
///
/// Determined against the shipping GLib rather than from memory, because only
/// four things are escaped and the set is smaller than one would guess:
/// `\` → `\\`, LF → `\n`, CR → `\r`, and *leading* whitespace (space → `\s`,
/// tab → `\t`, only while still in the leading run). Notably **not** escaped:
/// the list separator `;`, commas, `#`, `=`, brackets, tabs after the first
/// non-whitespace character, and trailing spaces.
///
/// An unknown escape is left alone, backslash and all. GKeyFile would reject
/// the value outright; keeping it verbatim loses less.
fn unescape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('\\') => out.push('\\'),
            Some('n') => out.push('\n'),
            Some('r') => out.push('\r'),
            Some('t') => out.push('\t'),
            Some('s') => out.push(' '),
            // Deliberately no `\0` arm. GLib rejects that escape outright, so
            // our writer can never have produced it, and honouring it on a
            // hand-edited file would put a NUL byte inside a path or a
            // nickname — which survives TOML and reaches whatever C string it
            // eventually becomes. Falling through keeps the two characters.
            Some(other) => {
                out.push('\\');
                out.push(other);
            }
            None => out.push('\\'),
        }
    }
    out
}

/// The C boolean parser, reproduced exactly.
///
/// Only the first byte is looked at, so `"tarantino"` is true and
/// `"nautical"` is false. That is silly, and it is also what every existing
/// file was written and read against — including the era when the writer
/// emitted `true`/`false` while the reader accepted only `0`/`1`, which
/// silently reverted every toggle to its default on each startup. Reproducing
/// the parser is what keeps a migration from repeating that.
///
/// `None` means unrecognised, which the C code treated as "leave the
/// preference at its default".
pub fn parse_boolean(s: &str) -> Option<bool> {
    match s.as_bytes().first()? {
        b'0' | b'f' | b'F' | b'n' | b'N' => Some(false),
        b'1' | b't' | b'T' | b'y' | b'Y' => Some(true),
        _ => None,
    }
}

/// Split one of the two comma-separated list values.
///
/// Empty entries are dropped, so an empty `HIGHLIGHTWORDS=` becomes an empty
/// array rather than an array holding one empty string — which is what the
/// real profile that seeded these tests actually contains.
pub fn split_list(s: &str) -> Vec<String> {
    s.split(',')
        .map(str::trim)
        .filter(|part| !part.is_empty())
        .map(str::to_string)
        .collect()
}
