//! IRC formatting codes, as Rotulus styled text.
//!
//! IRC carries its styling in-band: a byte like `0x02` toggles bold, and
//! `0x03` followed by digits sets a color. Rotulus never interprets bytes
//! like these itself — a view that did would let anyone who can send it
//! text restyle its transcript — so an IRC client decides which messages
//! may carry formatting and converts those here, into ordinary styled
//! runs, before appending.
//!
//! The vocabulary is the one documented at
//! <https://modern.ircdocs.horse/formatting>: bold, italic, underline,
//! strikethrough, monospace, reverse, reset, and colors by number
//! (`0x03`) or by hex (`0x04`). Colors 0–15 address the view's palette,
//! whose first sixteen slots are the mIRC colors, so a theme can adjust
//! them; the extended colors 16–98 and hex colors are fixed RGB. 99 means
//! "the default color".
//!
//! Nothing here fails: an incomplete code is dropped, and anything that
//! is not a code is text.

#![forbid(unsafe_code)]

use rotulus_layout::{Attrs, ColorRef, ParsedText, Span, Style};
use std::ops::Range;

const BOLD: u8 = 0x02;
const COLOR: u8 = 0x03;
const HEX_COLOR: u8 = 0x04;
const RESET: u8 = 0x0f;
const MONOSPACE: u8 = 0x11;
const REVERSE: u8 = 0x16;
const ITALIC: u8 = 0x1d;
const STRIKETHROUGH: u8 = 0x1e;
const UNDERLINE: u8 = 0x1f;

/// The extended colors, 16 through 98, as `0xRRGGBB`.
const EXTENDED: [u32; 83] = [
    0x470000, 0x472100, 0x474700, 0x324700, 0x004700, 0x00472c, 0x004747, 0x002747, 0x000047,
    0x2e0047, 0x470047, 0x47002a, 0x740000, 0x743a00, 0x747400, 0x517400, 0x007400, 0x007449,
    0x007474, 0x004074, 0x000074, 0x4b0074, 0x740074, 0x740045, 0xb50000, 0xb56300, 0xb5b500,
    0x7db500, 0x00b500, 0x00b571, 0x00b5b5, 0x0063b5, 0x0000b5, 0x7500b5, 0xb500b5, 0xb5006b,
    0xff0000, 0xff8c00, 0xffff00, 0xb2ff00, 0x00ff00, 0x00ffa0, 0x00ffff, 0x008cff, 0x0000ff,
    0xa500ff, 0xff00ff, 0xff0098, 0xff5959, 0xffb459, 0xffff71, 0xcfff60, 0x6fff6f, 0x65ffc9,
    0x6dffff, 0x59b4ff, 0x5959ff, 0xc459ff, 0xff66ff, 0xff59bc, 0xff9c9c, 0xffd39c, 0xffff9c,
    0xe2ff9c, 0x9cff9c, 0x9cffdb, 0x9cffff, 0x9cd3ff, 0x9c9cff, 0xdc9cff, 0xff9cff, 0xff94d3,
    0x000000, 0x131313, 0x282828, 0x363636, 0x4d4d4d, 0x656565, 0x818181, 0x9f9f9f, 0xbcbcbc,
    0xe2e2e2, 0xffffff,
];

/// The sixteen standard colors as `0xRRGGBB`, for a view's default
/// palette.
pub const STANDARD: [u32; 16] = [
    0xffffff, 0x000000, 0x00007f, 0x009300, 0xff0000, 0x7f0000, 0x9c009c, 0xfc7f00, 0xffff00,
    0x00fc00, 0x009393, 0x00ffff, 0x0000fc, 0xff00ff, 0x7f7f7f, 0xd2d2d2,
];

/// The color an mIRC number names.
pub fn color(n: u8) -> ColorRef {
    match n {
        0..=15 => ColorRef::Palette(n),
        16..=98 => ColorRef::Rgb(EXTENDED[usize::from(n) - 16]),
        _ => ColorRef::Default,
    }
}

/// A run of the input's text, with the codes before it resolved.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Segment {
    /// Byte range into the input; never contains a code.
    pub range: Range<usize>,
    pub style: Style,
}

/// Split `text` into styled segments, with the codes removed.
///
/// Every range lands on character boundaries: the codes are all ASCII
/// control bytes, and the text between them is left whole.
pub fn segments(text: &str) -> Vec<Segment> {
    let b = text.as_bytes();
    let mut out: Vec<Segment> = Vec::new();
    let mut style = Style::default();
    let mut start = 0usize;
    let mut i = 0usize;

    let flush = |from: usize, to: usize, style: Style, out: &mut Vec<Segment>| {
        if from >= to {
            return;
        }
        match out.last_mut() {
            Some(last) if last.style == style && last.range.end == from => last.range.end = to,
            _ => out.push(Segment {
                range: from..to,
                style,
            }),
        }
    };

    while i < b.len() {
        let c = b[i];
        let toggle = match c {
            BOLD => Some(Attrs::BOLD),
            ITALIC => Some(Attrs::ITALIC),
            UNDERLINE => Some(Attrs::UNDERLINE),
            STRIKETHROUGH => Some(Attrs::STRIKETHROUGH),
            MONOSPACE => Some(Attrs::CODE),
            REVERSE => Some(Attrs::REVERSE),
            _ => None,
        };
        let is_code = toggle.is_some() || matches!(c, COLOR | HEX_COLOR | RESET);
        if !is_code {
            i += 1;
            continue;
        }
        flush(start, i, style, &mut out);
        i += 1;
        if let Some(a) = toggle {
            style.attrs = if style.attrs.contains(a) {
                style.attrs.remove(a)
            } else {
                style.attrs.union(a)
            };
        } else if c == RESET {
            style = Style::default();
        } else if c == COLOR {
            match digits(b, i) {
                Some((fg, n)) => {
                    i += n;
                    style.fg = color(fg);
                    // The comma belongs to the code only when a
                    // background follows it; otherwise it is text.
                    if b.get(i) == Some(&b',') {
                        if let Some((bg, n)) = digits(b, i + 1) {
                            i += 1 + n;
                            style.bg = color(bg);
                        }
                    }
                }
                None => {
                    style.fg = ColorRef::Default;
                    style.bg = ColorRef::Default;
                }
            }
        } else {
            match hex(b, i) {
                Some(fg) => {
                    i += 6;
                    style.fg = ColorRef::Rgb(fg);
                    if b.get(i) == Some(&b',') {
                        if let Some(bg) = hex(b, i + 1) {
                            i += 7;
                            style.bg = ColorRef::Rgb(bg);
                        }
                    }
                }
                None => {
                    style.fg = ColorRef::Default;
                    style.bg = ColorRef::Default;
                }
            }
        }
        start = i;
    }
    flush(start, b.len(), style, &mut out);
    out
}

/// One or two decimal digits at `i`, and how many there were.
fn digits(b: &[u8], i: usize) -> Option<(u8, usize)> {
    let d0 = *b.get(i).filter(|c| c.is_ascii_digit())?;
    match b.get(i + 1).filter(|c| c.is_ascii_digit()) {
        Some(d1) => Some(((d0 - b'0') * 10 + (d1 - b'0'), 2)),
        None => Some((d0 - b'0', 1)),
    }
}

/// Six hex digits at `i`, as `0xRRGGBB`.
fn hex(b: &[u8], i: usize) -> Option<u32> {
    let s = b.get(i..i + 6)?;
    if !s.iter().all(u8::is_ascii_hexdigit) {
        return None;
    }
    u32::from_str_radix(std::str::from_utf8(s).ok()?, 16).ok()
}

/// `text` as styled text, with the codes removed.
pub fn parse(text: &str) -> ParsedText {
    let mut out = ParsedText::default();
    for seg in segments(text) {
        let start = out.text.len();
        out.text.push_str(&text[seg.range]);
        if seg.style != Style::default() {
            out.spans.push(Span {
                range: start..out.text.len(),
                style: seg.style,
            });
        }
    }
    out
}

/// `text` with every formatting code removed and nothing else changed:
/// what to put on a clipboard, or in a notification.
pub fn strip(text: &str) -> String {
    segments(text).into_iter().map(|s| &text[s.range]).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn styled(text: &str) -> Vec<(&str, Style)> {
        segments(text)
            .into_iter()
            .map(|s| (&text[s.range], s.style))
            .collect()
    }

    fn with(attrs: Attrs) -> Style {
        Style::default().with_attrs(attrs)
    }

    #[test]
    fn plain_text_is_one_default_segment() {
        assert_eq!(styled("hello there"), [("hello there", Style::default())]);
    }

    #[test]
    fn toggles_turn_on_and_off() {
        assert_eq!(
            styled("a\x02b\x02c"),
            [
                ("a", Style::default()),
                ("b", with(Attrs::BOLD)),
                ("c", Style::default())
            ]
        );
        assert_eq!(
            styled("\x1ditalic\x1d \x1funder\x1f \x1estrike\x1e \x11mono"),
            [
                ("italic", with(Attrs::ITALIC)),
                (" ", Style::default()),
                ("under", with(Attrs::UNDERLINE)),
                (" ", Style::default()),
                ("strike", with(Attrs::STRIKETHROUGH)),
                (" ", Style::default()),
                ("mono", with(Attrs::CODE)),
            ]
        );
    }

    #[test]
    fn toggles_nest() {
        assert_eq!(
            styled("\x02b\x1dbi\x02i"),
            [
                ("b", with(Attrs::BOLD)),
                ("bi", with(Attrs::BOLD.union(Attrs::ITALIC))),
                ("i", with(Attrs::ITALIC)),
            ]
        );
    }

    #[test]
    fn colors_by_number() {
        let s = styled("\x034red\x03 plain");
        assert_eq!(
            s[0],
            ("red", Style::default().with_fg(ColorRef::Palette(4)))
        );
        assert_eq!(s[1], (" plain", Style::default()));

        let s = styled("\x0304,12on blue");
        assert_eq!(s[0].0, "on blue");
        assert_eq!(s[0].1.fg, ColorRef::Palette(4));
        assert_eq!(s[0].1.bg, ColorRef::Palette(12));

        assert_eq!(
            styled("\x0352x")[0].1.fg,
            ColorRef::Rgb(0xff0000),
            "extended colors are RGB"
        );
        assert_eq!(
            styled("\x0399x")[0].1.fg,
            ColorRef::Default,
            "99 is the default"
        );
    }

    #[test]
    fn a_comma_without_a_background_is_text() {
        let s = styled("\x034,hello");
        assert_eq!(
            s[0],
            (",hello", Style::default().with_fg(ColorRef::Palette(4)))
        );
    }

    #[test]
    fn a_third_digit_is_text() {
        let s = styled("\x03123");
        assert_eq!(s[0], ("3", Style::default().with_fg(ColorRef::Palette(12))));
    }

    #[test]
    fn a_bare_color_code_resets_colors_but_not_attributes() {
        let s = styled("\x02\x034,5x\x03y");
        assert_eq!(s[1].0, "y");
        assert_eq!(s[1].1, with(Attrs::BOLD));
    }

    #[test]
    fn hex_colors() {
        let s = styled("\x04ff8800,000000hi");
        assert_eq!(s[0].0, "hi");
        assert_eq!(s[0].1.fg, ColorRef::Rgb(0xff8800));
        assert_eq!(s[0].1.bg, ColorRef::Rgb(0x000000));
        assert_eq!(
            styled("\x04zzz")[0],
            ("zzz", Style::default()),
            "not hex: a reset, then text"
        );
    }

    #[test]
    fn reset_clears_everything() {
        let s = styled("\x02\x1d\x034x\x0fy");
        assert_eq!(s.last().unwrap(), &("y", Style::default()));
    }

    #[test]
    fn reverse_is_carried_for_the_renderer() {
        assert_eq!(styled("\x16rev")[0], ("rev", with(Attrs::REVERSE)));
    }

    #[test]
    fn multibyte_text_survives_on_boundaries() {
        let text = "\x02ñandú\x02 → ok";
        for seg in segments(text) {
            assert!(text.is_char_boundary(seg.range.start) && text.is_char_boundary(seg.range.end));
        }
        assert_eq!(strip(text), "ñandú → ok");
    }

    #[test]
    fn parse_joins_segments_into_styled_text() {
        let p = parse("a \x02bold\x02 b");
        assert_eq!(p.text, "a bold b");
        assert_eq!(p.spans.len(), 1);
        assert_eq!(&p.text[p.spans[0].range.clone()], "bold");
        p.debug_assert_well_formed();
    }

    #[test]
    fn codes_alone_produce_nothing() {
        assert!(segments("\x02\x03\x0f").is_empty());
        assert_eq!(parse("\x02\x02").text, "");
    }
}
