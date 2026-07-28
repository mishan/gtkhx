//! The markdown inline scanner.
//!
//! Implements the restricted subset in docs/chat-view-scoping.md §3.9 —
//! `**bold**`, `*italic*` / `_italic_`, `` `code` ``, `~~strike~~`,
//! `[label](url)`, and backslash escapes. Block constructs (fenced code,
//! `>` quotes) are recognised by [`split_blocks`] before this scanner
//! runs; everything else CommonMark defines is deliberately absent.
//!
//! **Why hand-written rather than `pulldown-cmark`.** Three reasons, from
//! the scoping doc: it has no inline-only mode, so we would be filtering
//! a block-level event stream and fighting CommonMark's block rules to
//! suppress headings, thematic breaks and setext underlines — all of
//! which occur constantly in real chat prose (`# 1`, `---`, `====`); its
//! event stream would still need converting into byte-ranged
//! [`Span`](crate::span::Span)s, which is most of the work; and a scanner
//! for six constructs is small enough to be exhaustively tested and
//! predictable on pathological input. `hotline-proto` is hand-written for
//! the same reasons.
//!
//! **What is deliberately not supported**, and why: headings (`#` opens
//! far too many ordinary chat lines), images (`![]()` — inline media has
//! a server-validated upload/download pipeline and must not be
//! bypassable by an arbitrary URL), tables, raw HTML, reference links,
//! footnotes, thematic breaks, setext headings, and autolinking (the
//! existing URL detector in `gtkurl.c` owns that and has its own scheme
//! list).

use crate::span::{Attrs, ParsedText, SpanBuilder, Style};

/// Cap on nesting depth. `**a *b* a**` is depth 2. Real messages never
/// approach this; the cap exists so a line of 5,000 asterisks can't
/// recurse the parser into the stack guard.
const MAX_DEPTH: u8 = 8;

/// URL schemes a `[label](url)` link may point at.
///
/// Matches the set `gtkurl.c` already accepts, minus the bare-host forms
/// (`www.`, `ftp.`) which only make sense for autolinking. Anything else
/// — `javascript:`, `data:`, `file:`, or an unrecognised scheme — makes
/// the whole construct render as literal text, delimiters included, so
/// the user sees exactly what was typed rather than a link they can't
/// inspect.
pub fn scheme_allowed(url: &str) -> bool {
    const ALLOWED: [&str; 5] = ["http://", "https://", "ftp://", "hotline://", "mailto:"];
    let lower = url.trim().to_ascii_lowercase();
    ALLOWED.iter().any(|s| lower.starts_with(s))
}

/// A block-level piece of a message body.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum RawBlock {
    /// Ordinary text, to be run through [`parse_inline`].
    Paragraph(String),
    /// A fenced code block. Contents are inert.
    Code {
        text: String,
        language: Option<String>,
    },
    /// One or more `>`-prefixed lines, already stripped of their markers.
    Quote { text: String, depth: u8 },
}

/// Split a message body into block-level pieces.
///
/// Only two block constructs exist in the subset, and both are
/// unambiguous at line starts, so this is a line scanner rather than a
/// parser. An unterminated fence runs to the end of the body — the
/// alternative (treating it as literal) means a message someone is
/// mid-way through typing flickers between two renderings.
pub fn split_blocks(body: &str) -> Vec<RawBlock> {
    let mut out: Vec<RawBlock> = Vec::new();
    let mut para = String::new();
    let mut lines = body.split('\n').peekable();

    let flush = |para: &mut String, out: &mut Vec<RawBlock>| {
        if !para.is_empty() {
            out.push(RawBlock::Paragraph(std::mem::take(para)));
        }
    };

    while let Some(line) = lines.next() {
        let trimmed = line.trim_start();

        if let Some(rest) = trimmed.strip_prefix("```") {
            flush(&mut para, &mut out);

            // A fence that opens and closes on one line — ```like this```
            // — is by far the most common way someone types a code block
            // in a chat box, because chat boxes send on Enter. Treating
            // it as an *opening* fence made the rest of the line the
            // "language", scanned for a close that never came, and
            // produced an empty code block: a blank row where the user's
            // text should be.
            if let Some(inner) = rest.strip_suffix("```") {
                out.push(RawBlock::Code {
                    text: inner.to_string(),
                    language: None,
                });
                continue;
            }

            let language = {
                let l = rest.trim();
                if l.is_empty() {
                    None
                } else {
                    Some(l.to_string())
                }
            };
            let mut code = String::new();
            for l in lines.by_ref() {
                if l.trim_start().starts_with("```") {
                    break;
                }
                if !code.is_empty() {
                    code.push('\n');
                }
                code.push_str(l);
            }
            out.push(RawBlock::Code {
                text: code,
                language,
            });
            continue;
        }

        if trimmed.starts_with('>') {
            flush(&mut para, &mut out);
            let mut depth = 0u8;
            let mut rest = trimmed;
            while let Some(r) = rest.strip_prefix('>') {
                depth = depth.saturating_add(1);
                rest = r.trim_start();
            }
            let mut quoted = rest.to_string();
            // Consume following lines at the same depth.
            while let Some(next) = lines.peek() {
                let nt = next.trim_start();
                if !nt.starts_with('>') {
                    break;
                }
                let mut d = 0u8;
                let mut r = nt;
                while let Some(s) = r.strip_prefix('>') {
                    d = d.saturating_add(1);
                    r = s.trim_start();
                }
                if d != depth {
                    break;
                }
                quoted.push('\n');
                quoted.push_str(r);
                lines.next();
            }
            out.push(RawBlock::Quote {
                text: quoted,
                depth,
            });
            continue;
        }

        if !para.is_empty() {
            para.push('\n');
        }
        para.push_str(line);
    }
    flush(&mut para, &mut out);
    out
}

/// Parse inline markdown into styled text.
///
/// Never fails and never panics: any construct that doesn't close
/// renders as the literal characters that were typed.
pub fn parse_inline(src: &str) -> ParsedText {
    let mut b = SpanBuilder::new();
    scan(src, Style::default(), 0, &mut b);
    let out = b.finish();
    out.debug_assert_well_formed();
    out
}

fn scan(src: &str, base: Style, depth: u8, out: &mut SpanBuilder) {
    let bytes = src.as_bytes();
    let mut i = 0usize;
    // Start of the current literal run, flushed lazily so plain text
    // costs one push rather than one per character.
    let mut lit = 0usize;

    macro_rules! flush_lit {
        ($upto:expr) => {
            if $upto > lit {
                out.push(&src[lit..$upto], base);
            }
        };
    }

    while i < bytes.len() {
        let c = bytes[i];

        // Backslash escape: the next character is literal, whatever it is.
        if c == b'\\' && i + 1 < bytes.len() {
            let next = i + 1;
            let ch_end = next + utf8_len(bytes[next]);
            let ch_end = ch_end.min(bytes.len());
            if is_escapable(&src[next..ch_end]) {
                flush_lit!(i);
                out.push(&src[next..ch_end], base);
                i = ch_end;
                lit = i;
                continue;
            }
            i += 1;
            continue;
        }

        // `code` — inert contents, so it is tried before everything else.
        if c == b'`' {
            if let Some(close) = find_unescaped(bytes, i + 1, b'`') {
                flush_lit!(i);
                out.push(&src[i + 1..close], base.with_attrs(Attrs::CODE));
                i = close + 1;
                lit = i;
                continue;
            }
            i += 1;
            continue;
        }

        if depth < MAX_DEPTH {
            // **bold** and ~~strike~~ — two-character delimiters first, so
            // `**` is never mistaken for an empty `*` pair.
            let two = match c {
                b'*' if bytes.get(i + 1) == Some(&b'*') => Some((Attrs::BOLD, "**")),
                b'~' if bytes.get(i + 1) == Some(&b'~') => Some((Attrs::STRIKETHROUGH, "~~")),
                _ => None,
            };
            if let Some((attr, delim)) = two {
                if can_open(bytes, i + 2) {
                    if let Some(close) = find_delim(src, i + 2, delim) {
                        flush_lit!(i);
                        scan(&src[i + 2..close], base.with_attrs(attr), depth + 1, out);
                        i = close + 2;
                        lit = i;
                        continue;
                    }
                }
                i += 2;
                continue;
            }

            // *italic* / _italic_.
            //
            // `_` additionally requires non-alphanumeric neighbours so
            // that snake_case identifiers, which turn up constantly in
            // this project's chat, survive intact.
            if (c == b'*' && can_open(bytes, i + 1))
                || (c == b'_' && can_open(bytes, i + 1) && intraword_ok(bytes, i))
            {
                if let Some(close) = find_italic_close(src, i + 1, c) {
                    flush_lit!(i);
                    scan(
                        &src[i + 1..close],
                        base.with_attrs(Attrs::ITALIC),
                        depth + 1,
                        out,
                    );
                    i = close + 1;
                    lit = i;
                    continue;
                }
                i += 1;
                continue;
            }

            // [label](url)
            if c == b'[' {
                if let Some((label, href, end)) = parse_link(src, i) {
                    if scheme_allowed(href) {
                        flush_lit!(i);
                        // The id must exist before the label is emitted
                        // so it can ride in the label's Style; the
                        // visible range is patched in afterwards.
                        let id = out.reserve_link(href.to_string());
                        let start = out.len();
                        let mut label_style = base.with_attrs(Attrs::UNDERLINE);
                        label_style.link = Some(id);
                        // MAX_DEPTH, not depth + 1: a link label renders
                        // as plain text. Nested links are invalid
                        // markdown, and `parse_link` balances brackets,
                        // so re-scanning the label would happily parse
                        // the inner one.
                        scan(label, label_style, MAX_DEPTH, out);
                        out.set_link_range(id, start..out.len());
                        i = end;
                        lit = i;
                        continue;
                    }
                    // Disallowed scheme: fall through so the whole
                    // `[label](url)` renders as literal text. The user
                    // sees what was typed rather than a link they have
                    // no way to inspect.
                }
                i += 1;
                continue;
            }
        }

        i += 1;
    }
    flush_lit!(bytes.len());
}

/// Bytes markdown lets you escape. A backslash before anything else is
/// itself literal — `C:\path` must not lose its separators.
fn is_escapable(s: &str) -> bool {
    matches!(s, "\\" | "*" | "_" | "`" | "~" | "[" | "]" | "(" | ")" | ">" | "#")
}

fn utf8_len(b: u8) -> usize {
    if b < 0x80 {
        1
    } else if b >> 5 == 0b110 {
        2
    } else if b >> 4 == 0b1110 {
        3
    } else if b >> 3 == 0b11110 {
        4
    } else {
        1
    }
}

/// Next unescaped occurrence of the single byte `needle` at or after `from`.
fn find_unescaped(bytes: &[u8], from: usize, needle: u8) -> Option<usize> {
    let mut i = from;
    while i < bytes.len() {
        if bytes[i] == b'\\' {
            i += 2;
            continue;
        }
        if bytes[i] == needle {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// Next unescaped occurrence of a multi-byte delimiter, skipping code
/// spans so that `` **a `b**` c** `` closes at the last `**`.
fn find_delim(src: &str, from: usize, delim: &str) -> Option<usize> {
    let bytes = src.as_bytes();
    let d = delim.as_bytes();
    let mut i = from;
    while i + d.len() <= bytes.len() {
        if bytes[i] == b'\\' {
            i += 2;
            continue;
        }
        if bytes[i] == b'`' {
            match find_unescaped(bytes, i + 1, b'`') {
                Some(close) => {
                    i = close + 1;
                    continue;
                }
                None => return None,
            }
        }
        if bytes[i..].starts_with(d) {
            // An empty span (`****`) is not emphasis, and a closer must
            // be right-flanking.
            if i == from || !can_close(bytes, i) {
                i += d.len();
                continue;
            }
            return Some(i);
        }
        i += 1;
    }
    None
}

/// Close delimiter for single-character emphasis. Rejects a `**` run so
/// that `*a**b*` doesn't close on the doubled pair.
fn find_italic_close(src: &str, from: usize, open: u8) -> Option<usize> {
    let bytes = src.as_bytes();
    let mut i = from;
    while i < bytes.len() {
        if bytes[i] == b'\\' {
            i += 2;
            continue;
        }
        if bytes[i] == b'`' {
            match find_unescaped(bytes, i + 1, b'`') {
                Some(close) => {
                    i = close + 1;
                    continue;
                }
                None => return None,
            }
        }
        if bytes[i] == open {
            if bytes.get(i + 1) == Some(&open) {
                i += 2;
                continue;
            }
            if i == from || !can_close(bytes, i) {
                i += 1;
                continue;
            }
            if open == b'_' && !intraword_close_ok(bytes, i) {
                i += 1;
                continue;
            }
            return Some(i);
        }
        i += 1;
    }
    None
}

/// CommonMark's left-flanking rule, which is what stops `2 * 3 * 4`
/// from becoming `2  3  4`.
///
/// An opening delimiter must be followed by non-whitespace. Arithmetic,
/// bullet-ish prose ("see * the docs") and trailing asterisks all rely
/// on this; without it the parser eats punctuation out of ordinary
/// sentences, which is the single most annoying way a chat markdown
/// implementation can be wrong.
fn can_open(bytes: &[u8], after: usize) -> bool {
    bytes.get(after).is_some_and(|b| !b.is_ascii_whitespace())
}

/// The right-flanking counterpart: a closing delimiter must be preceded
/// by non-whitespace, so `a * b *` doesn't close on the trailing one.
fn can_close(bytes: &[u8], at: usize) -> bool {
    at > 0 && !bytes[at - 1].is_ascii_whitespace()
}

/// `_` opens emphasis only at a word boundary, so snake_case survives.
fn intraword_ok(bytes: &[u8], i: usize) -> bool {
    let before_ok = i == 0 || !bytes[i - 1].is_ascii_alphanumeric();
    let after_ok = bytes.get(i + 1).is_some_and(|b| *b != b'_');
    before_ok && after_ok
}

/// `_` closes emphasis only at a word boundary.
fn intraword_close_ok(bytes: &[u8], i: usize) -> bool {
    bytes
        .get(i + 1)
        .is_none_or(|b| !b.is_ascii_alphanumeric())
}

/// Parse `[label](url)` starting at `open`. Returns label, href and the
/// offset one past the closing paren.
fn parse_link(src: &str, open: usize) -> Option<(&str, &str, usize)> {
    let bytes = src.as_bytes();
    // Label: balanced brackets, no nesting beyond one level needed.
    let mut i = open + 1;
    let mut depth = 1usize;
    while i < bytes.len() {
        match bytes[i] {
            b'\\' => {
                i += 2;
                continue;
            }
            b'[' => depth += 1,
            b']' => {
                depth -= 1;
                if depth == 0 {
                    break;
                }
            }
            _ => {}
        }
        i += 1;
    }
    if depth != 0 || i >= bytes.len() {
        return None;
    }
    let label_end = i;
    if bytes.get(label_end + 1) != Some(&b'(') {
        return None;
    }
    let url_start = label_end + 2;
    let mut j = url_start;
    while j < bytes.len() && bytes[j] != b')' {
        if bytes[j] == b'\\' {
            j += 2;
            continue;
        }
        // A URL never contains whitespace; bail rather than swallowing
        // the rest of the line looking for a paren.
        if bytes[j].is_ascii_whitespace() {
            return None;
        }
        j += 1;
    }
    if j >= bytes.len() {
        return None;
    }
    let label = &src[open + 1..label_end];
    let href = &src[url_start..j];
    if label.is_empty() || href.is_empty() {
        return None;
    }
    Some((label, href, j + 1))
}

// ---- input tinting --------------------------------------------------

/// One tintable region of *source* text, for the compose box.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceSpan {
    pub start: usize,
    pub end: usize,
    pub attrs: Attrs,
    /// True for the delimiter characters themselves, which the input
    /// box dims rather than styles.
    pub delim: bool,
}

/// Locate the markdown delimiters in `src` that will actually be
/// consumed, reporting ranges **in the source**.
///
/// This exists because [`parse_inline`] reports ranges in the *rendered*
/// text, with the delimiters removed — exactly the offsets the compose
/// box does not have. Rather than teach the renderer to carry source
/// offsets through its recursion (a change to a heavily-tested function
/// for a cosmetic feature), this is a separate, deliberately shallower
/// pass that reuses the *rules* that are actually subtle: `can_open` /
/// `can_close` flanking and the `_` intraword guards.
///
/// **It is an approximation, and that is fine here.** It doesn't nest,
/// doesn't handle links, and takes the first valid closer. Being wrong
/// in the compose box means a character is tinted that won't be, on text
/// the user is still editing and can see; being wrong in the renderer
/// would change what a message *says*. The two failure modes are not
/// comparable, which is why they don't share a code path.
pub fn scan_delims(src: &str) -> Vec<SourceSpan> {
    let bytes = src.as_bytes();
    let mut out = Vec::new();
    let mut i = 0usize;

    while i < bytes.len() {
        // A backslash escape hides the next character from tinting, the
        // same way it hides it from the parser.
        if bytes[i] == b'\\' && i + 1 < bytes.len() {
            i += 2;
            continue;
        }

        let (run, attrs) = match bytes[i] {
            b'*' if bytes.get(i + 1) == Some(&b'*') => (2usize, Attrs::BOLD),
            b'*' => (1, Attrs::ITALIC),
            b'_' if intraword_ok(bytes, i) => (1, Attrs::ITALIC),
            b'`' => (1, Attrs::CODE),
            _ => {
                i += 1;
                continue;
            }
        };

        if !can_open(bytes, i + run) {
            i += run;
            continue;
        }

        // Code spans are literal: no escapes, no nesting, first closer wins.
        let literal = attrs == Attrs::CODE;
        let mut j = i + run;
        let close = loop {
            if j >= bytes.len() {
                break None;
            }
            if !literal && bytes[j] == b'\\' {
                j += 2;
                continue;
            }
            let hit = match run {
                2 => bytes[j] == b'*' && bytes.get(j + 1) == Some(&b'*'),
                _ => bytes[j] == bytes[i] && (bytes[i] != b'_' || intraword_close_ok(bytes, j)),
            };
            if hit && can_close(bytes, j) {
                break Some(j);
            }
            j += 1;
        };

        let Some(close) = close else {
            i += run;
            continue;
        };

        out.push(SourceSpan { start: i, end: i + run, attrs, delim: true });
        out.push(SourceSpan { start: i + run, end: close, attrs, delim: false });
        out.push(SourceSpan { start: close, end: close + run, attrs, delim: true });
        i = close + run;
    }
    out
}
