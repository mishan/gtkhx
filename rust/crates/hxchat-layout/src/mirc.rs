//! The mIRC escape compat shim. **Temporary — deleted at C5.**
//!
//! Decodes the in-band escape vocabulary GtkHx has been using between
//! `chat.c` and xtext into the same [`ParsedText`] the markdown parser
//! produces, so that during the coexistence period the xtext-backed and
//! the new view render identical content from identical input. It exists
//! for the A/B comparison and for nothing else.
//!
//! **This vocabulary is not protocol.** Worth stating explicitly,
//! because a note claiming the opposite ("servers send specific
//! indices") got into `CLAUDE.md`'s theming section during an
//! AI-assisted session in mid-2026 and would have misled the next reader
//! — it has been corrected there. The Hotline wire format has no
//! text-styling concept at all: `HTLS_HDR_CHAT` is `uid + flags + body`,
//! `HTLS_HDR_MSG` is `uid + body`, and news, broadcasts, agreements and
//! file comments are all plain text. There is no colour field and no
//! style field anywhere in it. Every `\003NN` byte that has ever been in
//! a GtkHx buffer was written by GtkHx: the nick brackets (`chat.c:627`,
//! `msg.c:598`), the highlight wrap (`chat.c:669`), the `INFOPREFIX`
//! constant, the muted history rows and dividers, and the inline-media
//! placeholder. The escapes arrived with the XChat 1.8.5 xtext fork
//! around 2000. Hotline's real per-user colour is a separate `u32` RGB
//! attribute on the user record.
//!
//! A server cannot inject these either: `hotline_proto::sanitize::strip_ansi`
//! folds bytes 14..=30 into the printable range on every received text
//! field, and the protocol gives a server nowhere to put them anyway.
//!
//! So once `chat.c` builds [`Message`](crate::message::Message)s directly
//! there is no producer left, and this module goes with xtext. Nothing
//! new should call it.

use crate::span::{Attrs, ColorRef, ParsedText, SpanBuilder, Style};

/// The eight escape bytes xtext understood. Only three (`COLOR`, `BOLD`,
/// `RESET`) ever had a producer in GtkHx; the rest are decoded for
/// completeness so the shim is a faithful mirror during the A/B.
mod attr {
    pub const BOLD: u8 = 0x02;
    pub const COLOR: u8 = 0x03;
    pub const HIDDEN: u8 = 0x08;
    pub const RESET: u8 = 0x0f;
    pub const REVERSE: u8 = 0x16;
    pub const ITALIC: u8 = 0x1d;
    pub const STRIKETHROUGH: u8 = 0x1e;
    pub const UNDERLINE: u8 = 0x1f;
}

/// Decode an escape-bearing string into styled text.
///
/// Hidden runs (`\010`) are dropped from the output rather than styled
/// invisible — the layout engine has no notion of zero-width text, and
/// nothing in GtkHx ever emitted one.
pub fn parse(src: &str) -> ParsedText {
    let mut b = SpanBuilder::new();
    let mut style = Style::default();
    let bytes = src.as_bytes();
    let mut i = 0usize;
    let mut lit = 0usize;
    let mut hidden = false;

    macro_rules! flush {
        ($upto:expr) => {
            if $upto > lit && !hidden {
                b.push(&src[lit..$upto], style);
            }
        };
    }

    while i < bytes.len() {
        let c = bytes[i];
        match c {
            attr::COLOR => {
                flush!(i);
                i += 1;
                let (fg, bg, consumed) = parse_color(&bytes[i..]);
                i += consumed;
                // A bare \003 with no digits resets colour to default,
                // which is how chat.c closes its nick brackets.
                style.fg = fg.map_or(ColorRef::Default, ColorRef::Palette);
                style.bg = bg.map_or(ColorRef::Default, ColorRef::Palette);
                lit = i;
            }
            attr::RESET => {
                flush!(i);
                style = Style::default();
                hidden = false;
                i += 1;
                lit = i;
            }
            attr::BOLD
            | attr::ITALIC
            | attr::UNDERLINE
            | attr::STRIKETHROUGH
            | attr::REVERSE => {
                flush!(i);
                let bit = match c {
                    attr::BOLD => Attrs::BOLD,
                    attr::ITALIC => Attrs::ITALIC,
                    attr::UNDERLINE => Attrs::UNDERLINE,
                    attr::STRIKETHROUGH => Attrs::STRIKETHROUGH,
                    _ => Attrs::REVERSE,
                };
                // These are toggles, not pushes — same as xtext.
                style.attrs = if style.attrs.contains(bit) {
                    style.attrs.remove(bit)
                } else {
                    style.attrs.union(bit)
                };
                i += 1;
                lit = i;
            }
            attr::HIDDEN => {
                flush!(i);
                hidden = !hidden;
                i += 1;
                lit = i;
            }
            _ => i += 1,
        }
    }
    flush!(bytes.len());

    let out = b.finish();
    out.debug_assert_well_formed();
    out
}

/// Parse the `NN[,NN]` digits after a `\003`.
///
/// Returns `(fg, bg, bytes_consumed)`. mIRC allows one or two digits per
/// field; xtext accepted the same. Indices are clamped to the palette
/// size by the caller's palette lookup, not here — an out-of-range index
/// is the producer's bug and silently remapping it would hide it.
fn parse_color(rest: &[u8]) -> (Option<u8>, Option<u8>, usize) {
    let mut i = 0usize;
    let (fg, n) = take_digits(&rest[i..]);
    i += n;
    if fg.is_none() {
        return (None, None, i);
    }
    if rest.get(i) == Some(&b',') {
        let (bg, n) = take_digits(&rest[i + 1..]);
        if bg.is_some() {
            i += 1 + n;
            return (fg, bg, i);
        }
    }
    (fg, None, i)
}

fn take_digits(rest: &[u8]) -> (Option<u8>, usize) {
    let mut val: u16 = 0;
    let mut n = 0usize;
    while n < 2 {
        match rest.get(n) {
            Some(d) if d.is_ascii_digit() => {
                val = val * 10 + u16::from(d - b'0');
                n += 1;
            }
            _ => break,
        }
    }
    if n == 0 {
        (None, 0)
    } else {
        (Some(val.min(255) as u8), n)
    }
}

/// Strip every escape, returning the plain text.
///
/// Used by the send-path assertion described in the scoping doc §3.8:
/// display strings are built after the wire encode, so no escape should
/// ever reach a socket, and a debug check is cheap insurance.
pub fn strip(src: &str) -> String {
    parse(src).text
}
