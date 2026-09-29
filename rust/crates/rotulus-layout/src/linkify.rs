//! Finding links in plain text.
//!
//! One detector for everything that asks "is this a link": autolinking a
//! message body, the markdown `[label](url)` allowlist, and whether a
//! clicked word is a URL. Keeping them on one scheme list is what stops
//! them disagreeing — a URL that autolinks but may not be written as a
//! markdown link, or the reverse, reads as a bug either way.
//!
//! The scheme list belongs to the application. A Hotline client wants
//! `hotline://`, an IRC client `ircs://`; [`DEFAULT_SCHEMES`] is the
//! set every chat client can agree on, and [`Linkifier::new`] takes a
//! different one.
//!
//! Detection is deliberately lenient about what surrounds a URL and
//! strict about what ends one: a link may abut an opening quote or
//! bracket, and sentence punctuation after it is not part of it.

use std::ops::Range;

/// The schemes a view links when the application names none.
///
/// Each entry is the full prefix, so the two shapes of URL scheme are
/// both expressible: `https://` and `mailto:`.
pub const DEFAULT_SCHEMES: &[&str] = &[
    "http://", "https://", "ftp://", "ftps://", "irc://", "ircs://", "mailto:", "magnet:",
    "git://", "ssh://", "sftp://",
];

/// Scheme-less prefixes people type, and the scheme each one opens with.
///
/// `www.example.com` is how a lot of people still write a web address;
/// without a scheme a launcher has nothing to dispatch on.
const BARE_PREFIXES: &[(&str, &str)] = &[
    ("www.", "https://"),
    ("ftp.", "ftp://"),
    ("irc.", "https://"),
];

/// A link detector over one scheme list.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Linkifier {
    /// Lowercased prefixes, each ending in `:` or `://`.
    schemes: Vec<String>,
}

impl Default for Linkifier {
    fn default() -> Self {
        Linkifier::new(DEFAULT_SCHEMES)
    }
}

impl Linkifier {
    /// A detector for exactly these scheme prefixes (`"https://"`,
    /// `"mailto:"`). Matching is case-insensitive; empty entries are
    /// ignored, and so is an entry without a `:`, which could never be
    /// a scheme.
    pub fn new<S: AsRef<str>>(schemes: impl IntoIterator<Item = S>) -> Linkifier {
        let mut out: Vec<String> = Vec::new();
        for s in schemes {
            let s = s.as_ref().trim().to_ascii_lowercase();
            if s.contains(':') && !out.contains(&s) {
                out.push(s);
            }
        }
        Linkifier { schemes: out }
    }

    /// The scheme prefixes this detector links.
    pub fn schemes(&self) -> &[String] {
        &self.schemes
    }

    fn scheme_at(&self, bytes: &[u8]) -> Option<usize> {
        self.schemes
            .iter()
            .find(|s| starts_with_ignore_case(bytes, s.as_bytes()))
            .map(|s| s.len())
    }

    fn bare_prefix_at(bytes: &[u8]) -> Option<(usize, &'static str)> {
        BARE_PREFIXES
            .iter()
            .find(|(p, _)| starts_with_ignore_case(bytes, p.as_bytes()))
            .map(|(p, scheme)| (p.len(), *scheme))
    }

    /// Whether `href` starts with one of the schemes. What a markdown
    /// `[label](url)` is checked against: anything else — `javascript:`,
    /// `data:`, `file:` — renders as the literal text that was typed.
    pub fn allows(&self, href: &str) -> bool {
        self.scheme_at(href.trim().as_bytes()).is_some()
    }

    /// Whether a word starts with a scheme or a bare `www.`-style prefix.
    pub fn has_scheme(&self, word: &str) -> bool {
        let b = word.as_bytes();
        self.scheme_at(b).is_some() || Self::bare_prefix_at(b).is_some()
    }

    /// Whether a whitespace-delimited word is a link: a URL, or a bare
    /// email address.
    pub fn is_url(&self, word: &str) -> bool {
        self.has_scheme(word) || is_email(word)
    }

    /// The form of `word` a launcher can open: a scheme is prepended to
    /// bare `www.` hosts and email addresses, and anything already
    /// carrying one comes back unchanged.
    pub fn normalize(&self, word: &str) -> String {
        let b = word.as_bytes();
        if self.scheme_at(b).is_some() {
            return word.to_string();
        }
        if let Some((_, scheme)) = Self::bare_prefix_at(b) {
            return format!("{scheme}{word}");
        }
        if let (Some(at), Some(dot)) = (word.find('@'), word.rfind('.')) {
            if at < dot {
                return format!("mailto:{word}");
            }
        }
        word.to_string()
    }

    /// Every link in `text`, as byte ranges on character boundaries.
    ///
    /// A link starts at the beginning of the text or after whitespace or
    /// an opening delimiter, and runs to whitespace or a closing one.
    /// Trailing sentence punctuation is dropped, and so is a closing
    /// paren or bracket — unless the URL opened one itself, which is the
    /// shape of a Wikipedia link.
    pub fn scan(&self, text: &str) -> Vec<Range<usize>> {
        let bytes = text.as_bytes();
        let mut out = Vec::new();
        let mut i = 0usize;
        while i < bytes.len() {
            let at_boundary = i == 0
                || matches!(bytes[i - 1], b'<' | b'(' | b'[' | b'"' | b'\'')
                || bytes[i - 1].is_ascii_whitespace();
            let prefix = if at_boundary {
                self.scheme_at(&bytes[i..])
                    .or_else(|| Self::bare_prefix_at(&bytes[i..]).map(|(n, _)| n))
            } else {
                None
            };
            let Some(prefix) = prefix else {
                i += 1;
                continue;
            };
            let mut end = i + prefix;
            while end < bytes.len()
                && !matches!(
                    bytes[end],
                    b' ' | b'\t' | b'\n' | b'\r' | b'<' | b'>' | b'"' | b'\''
                )
            {
                end += 1;
            }
            let len = trim_trailing_punct(&bytes[i..end]);
            if len > prefix {
                out.push(i..i + len);
            }
            i = end;
        }
        out
    }
}

fn starts_with_ignore_case(hay: &[u8], prefix: &[u8]) -> bool {
    hay.len() >= prefix.len() && hay[..prefix.len()].eq_ignore_ascii_case(prefix)
}

fn is_email(word: &str) -> bool {
    match (word.find('@'), word.rfind('.')) {
        (Some(at), Some(dot)) => at < dot && dot < word.len() - 1,
        _ => false,
    }
}

/// Length of `s` with trailing sentence punctuation removed.
fn trim_trailing_punct(s: &[u8]) -> usize {
    let mut len = s.len();
    while len > 0 {
        let c = s[len - 1];
        let strip = matches!(
            c,
            b'.' | b',' | b';' | b':' | b'!' | b'?' | b')' | b']' | b'\'' | b'"'
        );
        if !strip {
            break;
        }
        if c == b')' && s[..len].contains(&b'(') {
            break;
        }
        if c == b']' && s[..len].contains(&b'[') {
            break;
        }
        len -= 1;
    }
    len
}
