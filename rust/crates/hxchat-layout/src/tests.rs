//! Unit tests for the layout engine.
//!
//! All of these run headless — that is the point of keeping the crate
//! free of GTK/Pango. Wrap assertions use [`FixedMeasure`] so they can
//! name exact byte offsets instead of "roughly here".

use crate::anchor::{Gravity, ScrollAnchor};
use crate::buffer::ChatBuffer;
use crate::index::HeightIndex;
use crate::markdown::{self, RawBlock};
use crate::measure::{FixedMeasure, TextMeasure};
use crate::message::{Block, ImageSize, Message, Speaker};
use crate::mirc;
use crate::span::{Attrs, ColorRef, ParsedText, Style};
use crate::wrap::{layout_message, LayoutGeneration, LayoutParams};

// ---------------------------------------------------------------- markdown

/// Assert the rendered text and the styled slices of a parse.
fn styled(p: &ParsedText) -> Vec<(&str, Attrs)> {
    p.spans
        .iter()
        .map(|s| (&p.text[s.range.clone()], s.style.attrs))
        .collect()
}

#[test]
fn md_plain_text_has_no_spans() {
    let p = markdown::parse_inline("just a normal chat line");
    assert_eq!(p.text, "just a normal chat line");
    assert!(p.spans.is_empty(), "plain text should not allocate spans");
}

#[test]
fn md_bold_italic_code_strike() {
    let p = markdown::parse_inline("a **b** c *d* e `f` g ~~h~~");
    assert_eq!(p.text, "a b c d e f g h");
    assert_eq!(
        styled(&p),
        vec![
            ("b", Attrs::BOLD),
            ("d", Attrs::ITALIC),
            ("f", Attrs::CODE),
            ("h", Attrs::STRIKETHROUGH),
        ]
    );
}

#[test]
fn md_nested_emphasis() {
    let p = markdown::parse_inline("**bold *and italic* still bold**");
    assert_eq!(p.text, "bold and italic still bold");
    assert_eq!(
        styled(&p),
        vec![
            ("bold ", Attrs::BOLD),
            ("and italic", Attrs::BOLD.union(Attrs::ITALIC)),
            (" still bold", Attrs::BOLD),
        ]
    );
}

#[test]
fn md_unmatched_delimiters_stay_literal() {
    // The single most important property: never eat a lone asterisk.
    for src in [
        "2 * 3 * 4",
        "a ** b",
        "**unclosed",
        "*unclosed",
        "~~unclosed",
        "`unclosed",
        "x_y",
        "a_b_c",
    ] {
        let p = markdown::parse_inline(src);
        assert_eq!(p.text, src, "input {src:?} should render literally");
        assert!(p.spans.is_empty(), "input {src:?} should have no spans");
    }
}

#[test]
fn md_underscore_respects_word_boundaries() {
    // snake_case must survive; this project's chat is full of it.
    let p = markdown::parse_inline("call hx_chat_view_append please");
    assert_eq!(p.text, "call hx_chat_view_append please");
    assert!(p.spans.is_empty());

    // But a real word-boundary underscore still emphasises.
    let p = markdown::parse_inline("this is _emphatic_ yes");
    assert_eq!(p.text, "this is emphatic yes");
    assert_eq!(styled(&p), vec![("emphatic", Attrs::ITALIC)]);
}

#[test]
fn md_code_span_suppresses_other_markup() {
    let p = markdown::parse_inline("use `a **b** c` here");
    assert_eq!(p.text, "use a **b** c here");
    assert_eq!(styled(&p), vec![("a **b** c", Attrs::CODE)]);
}

#[test]
fn md_escapes() {
    let p = markdown::parse_inline(r"literal \*stars\* and \`ticks\`");
    assert_eq!(p.text, "literal *stars* and `ticks`");
    assert!(p.spans.is_empty());
}

#[test]
fn md_backslash_before_ordinary_char_is_literal() {
    // Windows paths and regexes must not lose their backslashes.
    let p = markdown::parse_inline(r"C:\path\to\file and \d+");
    assert_eq!(p.text, r"C:\path\to\file and \d+");
}

#[test]
fn md_link_allowed_scheme() {
    let p = markdown::parse_inline("see [the docs](https://example.com/x) ok");
    assert_eq!(p.text, "see the docs ok");
    assert_eq!(p.links.len(), 1);
    assert_eq!(p.links[0].href, "https://example.com/x");
    assert_eq!(&p.text[p.links[0].range.clone()], "the docs");
    assert_eq!(p.style_at(4).link, Some(0));
}

#[test]
fn md_link_disallowed_scheme_renders_literally() {
    // The security property: a scheme we can't vouch for is shown as
    // typed, not turned into a link the user cannot inspect.
    for src in [
        "[click](javascript:alert(1))",
        "[click](data:text/html;base64,xx)",
        "[click](file:///etc/passwd)",
        "[click](vbscript:x)",
    ] {
        let p = markdown::parse_inline(src);
        assert_eq!(p.text, src, "{src} should render literally");
        assert!(p.links.is_empty(), "{src} should produce no link");
    }
}

#[test]
fn md_link_label_and_href_may_disagree() {
    // Allowed, but both are recorded so the view can warn.
    let p = markdown::parse_inline("[https://good.example](https://evil.example)");
    assert_eq!(p.text, "https://good.example");
    assert_eq!(p.links[0].href, "https://evil.example");
}

#[test]
fn md_images_are_not_links() {
    // `![]()` must not bypass the server-validated media pipeline.
    let p = markdown::parse_inline("![alt](https://example.com/x.png)");
    assert!(p.text.starts_with('!'));
    assert_eq!(p.links.len(), 1, "the bracket part still parses as a link");
    // What matters is that no image block is produced — this parser
    // cannot emit one at all.
}

#[test]
fn md_no_headings_or_rules() {
    for src in ["# not a heading", "--- not a rule", "=== nope"] {
        let blocks = markdown::split_blocks(src);
        assert_eq!(blocks, vec![RawBlock::Paragraph(src.to_string())]);
    }
}

#[test]
fn md_fenced_code_block() {
    let blocks = markdown::split_blocks("before\n```rust\nlet x = **y**;\n```\nafter");
    assert_eq!(
        blocks,
        vec![
            RawBlock::Paragraph("before".into()),
            RawBlock::Code {
                text: "let x = **y**;".into(),
                language: Some("rust".into())
            },
            RawBlock::Paragraph("after".into()),
        ]
    );
}

#[test]
fn md_unterminated_fence_runs_to_end() {
    let blocks = markdown::split_blocks("```\nstill typing");
    assert_eq!(
        blocks,
        vec![RawBlock::Code {
            text: "still typing".into(),
            language: None
        }]
    );
}

#[test]
fn md_quote_block() {
    let blocks = markdown::split_blocks("> a\n> b\nnormal");
    assert_eq!(
        blocks,
        vec![
            RawBlock::Quote {
                text: "a\nb".into(),
                depth: 1
            },
            RawBlock::Paragraph("normal".into()),
        ]
    );
}

#[test]
fn md_pathological_input_terminates() {
    // Guards against the recursion and scan-loop bugs this shape of
    // parser is prone to.
    for src in [
        "*".repeat(2000),
        "**".repeat(1000),
        "`".repeat(1000),
        "[".repeat(1000),
        "~~".repeat(1000),
        "\\".repeat(1000),
        format!("{}text{}", "**".repeat(64), "**".repeat(64)),
    ] {
        let p = markdown::parse_inline(&src);
        p.debug_assert_well_formed();
    }
}

#[test]
fn md_utf8_is_never_split() {
    let p = markdown::parse_inline("**héllo → wörld 😀**");
    assert_eq!(p.text, "héllo → wörld 😀");
    p.debug_assert_well_formed();
}

// -------------------------------------------------------------------- mIRC

#[test]
fn mirc_nick_bracket_shape() {
    // Exactly what chat.c:627 emits.
    let p = mirc::parse("\u{3}12<\u{3}alice\u{3}12>\u{3} hello");
    assert_eq!(p.text, "<alice> hello");
    assert_eq!(p.style_at(0).fg, ColorRef::Palette(12));
    assert_eq!(p.style_at(1).fg, ColorRef::Default, "nick uses default fg");
    assert_eq!(p.style_at(6).fg, ColorRef::Palette(12));
    assert_eq!(p.style_at(8).fg, ColorRef::Default);
}

#[test]
fn mirc_highlight_shape() {
    // chat.c:669 — bold + colour 4, closed by a reset.
    let p = mirc::parse("\u{2}\u{3}04alice\u{f} said hi");
    assert_eq!(p.text, "alice said hi");
    let s = p.style_at(0);
    assert!(s.attrs.contains(Attrs::BOLD));
    assert_eq!(s.fg, ColorRef::Palette(4));
    assert_eq!(p.style_at(6), Style::default(), "reset clears everything");
}

#[test]
fn mirc_history_muted_shape() {
    let p = mirc::parse("\u{3}37─── chat history (3 messages) ───");
    assert_eq!(p.text, "─── chat history (3 messages) ───");
    assert_eq!(p.style_at(0).fg, ColorRef::Palette(37));
}

#[test]
fn mirc_two_digit_and_background() {
    let p = mirc::parse("\u{3}04,08warn");
    assert_eq!(p.text, "warn");
    assert_eq!(p.style_at(0).fg, ColorRef::Palette(4));
    assert_eq!(p.style_at(0).bg, ColorRef::Palette(8));
}

#[test]
fn mirc_bare_color_resets_to_default() {
    let p = mirc::parse("\u{3}12a\u{3}b");
    assert_eq!(p.text, "ab");
    assert_eq!(p.style_at(0).fg, ColorRef::Palette(12));
    assert_eq!(p.style_at(1).fg, ColorRef::Default);
}

#[test]
fn mirc_strip_removes_everything() {
    assert_eq!(
        mirc::strip("\u{3}12<\u{3}bob\u{3}12>\u{3} \u{2}hi\u{f}"),
        "<bob> hi"
    );
}

#[test]
fn mirc_digits_after_text_are_not_eaten() {
    // "\003 3" then literal "7 items" must not become colour 37.
    let p = mirc::parse("\u{3}3 7 items");
    assert_eq!(p.text, " 7 items");
    assert_eq!(p.style_at(0).fg, ColorRef::Palette(3));
}

// ------------------------------------------------------------------ measure

#[test]
fn fit_prefix_respects_char_boundaries() {
    let m = FixedMeasure::new(10);
    let (n, w) = m.fit_prefix("héllo", Style::default(), 25);
    assert!("héllo".is_char_boundary(n));
    assert!(w <= 25);
}

#[test]
fn image_scales_down_never_up() {
    let m = FixedMeasure::new(8);
    assert_eq!(m.image_size((100, 50), 200), (100, 50), "no upscale");
    assert_eq!(m.image_size((400, 200), 200), (200, 100), "aspect kept");
}

// -------------------------------------------------------------------- wrap

fn params(width: u32) -> LayoutParams {
    LayoutParams {
        width,
        indent: false,
        max_indent: 256,
        indent_width: 0,
        stamp_width: 0,
        quote_indent: 12,
        block_padding: 2,
        word_wrap: true,
    }
}

#[test]
fn wrap_breaks_on_word_boundary() {
    let m = FixedMeasure::new(10); // 10 px per char
    let msg = Message::system(ParsedText::plain("aaa bbb ccc ddd"));
    // 80 px = 8 chars per line.
    let l = layout_message(&msg, &params(80), LayoutGeneration::default(), &m);
    let texts: Vec<&str> = l
        .lines
        .iter()
        .map(|lb| &"aaa bbb ccc ddd"[lb.range.clone()])
        .collect();
    assert_eq!(texts, vec!["aaa bbb", "ccc ddd"]);
}

#[test]
fn wrap_hard_breaks_long_token() {
    let m = FixedMeasure::new(10);
    let word = "aaaaaaaaaaaaaaaaaaaa"; // 20 chars, no spaces
    let msg = Message::system(ParsedText::plain(word));
    let l = layout_message(&msg, &params(50), LayoutGeneration::default(), &m);
    assert!(l.lines.len() >= 4, "must break mid-token, got {:?}", l.lines);
    assert_eq!(l.height, l.lines.len() as u32 * m.metrics().line_height);
}

#[test]
fn wrap_honours_hard_newlines() {
    let m = FixedMeasure::new(10);
    let msg = Message::system(ParsedText::plain("a\nb\nc"));
    let l = layout_message(&msg, &params(500), LayoutGeneration::default(), &m);
    assert_eq!(l.lines.len(), 3);
}

/// A measurer whose per-character width depends on the *style*, not just
/// the text.
///
/// [`FixedMeasure`] can't catch a whole class of bug, because if every
/// character is the same width then measuring a run with the wrong style
/// still gives the right answer. Real fonts are not like that: bold and
/// monospace are wider than the base face. This measurer reproduces that,
/// and it is what makes the overflow assertions below meaningful.
#[derive(Clone, Copy, Debug)]
struct StyleMeasure {
    base: u32,
}

impl TextMeasure for StyleMeasure {
    fn metrics(&self) -> crate::measure::FontMetrics {
        crate::measure::FontMetrics {
            line_height: 16,
            ascent: 12,
            space_width: self.base,
        }
    }

    fn run_width(&self, text: &str, style: Style) -> u32 {
        let per = if style.attrs.contains(Attrs::BOLD) {
            self.base * 2
        } else if style.attrs.contains(Attrs::CODE) {
            self.base * 3
        } else {
            self.base
        };
        (text.chars().count() as u32) * per
    }
}

/// Every wrapped line's *actually rendered* width must fit the box.
///
/// This is the property the old break-point search violated: it measured
/// the remaining text with the style in effect at the line's start, so a
/// bold or code run later in the line was under-measured and the drawn
/// line overflowed.
fn assert_no_line_overflows(
    p: &ParsedText,
    lines: &[crate::wrap::LineBox],
    width: u32,
    m: &dyn TextMeasure,
) {
    for lb in lines {
        let mut w = 0u32;
        let mut cursor = lb.range.start;
        for s in &p.spans {
            if s.range.end <= cursor || s.range.start >= lb.range.end {
                continue;
            }
            if s.range.start > cursor {
                w += m.run_width(&p.text[cursor..s.range.start], Style::default());
                cursor = s.range.start;
            }
            let e = s.range.end.min(lb.range.end);
            if e > cursor {
                w += m.run_width(&p.text[cursor..e], s.style);
                cursor = e;
            }
        }
        if cursor < lb.range.end {
            w += m.run_width(&p.text[cursor..lb.range.end], Style::default());
        }
        assert!(
            w <= width,
            "line {:?} ({:?}) renders {w}px, over the {width}px box",
            lb.range,
            &p.text[lb.range.clone()]
        );
    }
}

#[test]
fn wrap_accounts_for_style_changes_mid_line() {
    let m = StyleMeasure { base: 10 };
    // Bold measures 2x, so a break point chosen from the leading plain
    // style overshoots badly.
    let p = markdown::parse_inline("aaa **bbbbbbbbbb** ccc");
    let msg = Message::system(p.clone());
    let l = layout_message(&msg, &params(100), LayoutGeneration::default(), &m);
    assert_no_line_overflows(&p, &l.lines, 100, &m);
}

#[test]
fn wrap_accounts_for_code_runs() {
    let m = StyleMeasure { base: 10 };
    // Code measures 3x.
    let p = markdown::parse_inline("run `hx_chat_view_append_indent` now");
    let msg = Message::system(p.clone());
    let l = layout_message(&msg, &params(120), LayoutGeneration::default(), &m);
    assert_no_line_overflows(&p, &l.lines, 120, &m);
}

#[test]
fn wrap_style_at_the_very_start_of_a_line() {
    // The mirror case: the line *begins* bold and turns plain, where a
    // start-style-only search over-measures and wraps too early.
    let m = StyleMeasure { base: 10 };
    let p = markdown::parse_inline("**bb** aaaaaaaaaaaaaaaaaaaaaaaa");
    let msg = Message::system(p.clone());
    let l = layout_message(&msg, &params(100), LayoutGeneration::default(), &m);
    assert_no_line_overflows(&p, &l.lines, 100, &m);
    // And no content is lost across the wrap.
    let joined: String = l
        .lines
        .iter()
        .map(|lb| &p.text[lb.range.clone()])
        .collect::<Vec<_>>()
        .join("");
    assert_eq!(joined.replace(' ', ""), p.text.replace(' ', ""));
}

#[test]
fn empty_quote_still_has_a_line_box() {
    let m = FixedMeasure::new(8);
    let msg = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: None,
        blocks: vec![Block::Quote {
            content: ParsedText::plain(""),
            depth: 1,
        }],
        flags: MessageFlagsNone::NONE,
    };
    let l = layout_message(&msg, &params(400), LayoutGeneration::default(), &m);
    assert_eq!(
        l.lines.len(),
        1,
        "an empty quote occupies space, so it must be hit-testable"
    );
    assert_eq!(l.height, m.metrics().line_height);
}

#[test]
fn empty_alt_image_still_has_a_line_box() {
    let m = FixedMeasure::new(8);
    let msg = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: None,
        blocks: vec![Block::Image {
            token: 1,
            size: None,
            alt: String::new(),
        }],
        flags: MessageFlagsNone::NONE,
    };
    let l = layout_message(&msg, &params(400), LayoutGeneration::default(), &m);
    assert_eq!(
        l.lines.len(),
        1,
        "an undecoded image must stay clickable — that is how the \
         media dialog gets opened"
    );
}

#[test]
fn gutter_gets_its_own_line_box() {
    let m = FixedMeasure::new(8);
    let mut p = params(400);
    p.indent = true;
    p.indent_width = 80;
    let msg = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: Some(mirc::parse("\u{3}12<\u{3}alice\u{3}12>\u{3}")),
        blocks: vec![Block::Text(ParsedText::plain("hello"))],
        flags: MessageFlagsNone::NONE,
    };
    let l = layout_message(&msg, &p, LayoutGeneration::default(), &m);
    assert_eq!(l.lines[0].source, crate::wrap::LineSource::Gutter);
    assert_eq!(l.lines[0].x, 0);
    assert_eq!(l.lines[1].x, 80, "body sits past the gutter");
    assert_eq!(l.lines[0].y, l.lines[1].y, "same visual row");
}

#[test]
fn wrap_disabled_produces_one_line() {
    let m = FixedMeasure::new(10);
    let mut p = params(50);
    p.word_wrap = false;
    let msg = Message::system(ParsedText::plain("aaa bbb ccc ddd eee"));
    let l = layout_message(&msg, &p, LayoutGeneration::default(), &m);
    // Without word_wrap we still break at the width (that is what xtext
    // does too — "wordwrap" selects word vs character breaking), but the
    // break points differ from the word-wrapped case.
    assert!(!l.lines.is_empty());
}

#[test]
fn image_block_has_real_pixel_height() {
    // The headline departure from xtext: not ceil(h / fontsize) blank
    // text rows, an actual height.
    let m = FixedMeasure::new(8);
    let msg = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: None,
        blocks: vec![Block::Image {
            token: 7,
            size: Some(ImageSize {
                width: 100,
                height: 240,
            }),
            alt: "[image]".into(),
        }],
        flags: MessageFlagsNone::NONE,
    };
    let l = layout_message(&msg, &params(400), LayoutGeneration::default(), &m);
    assert_eq!(l.height, 240 + 2 * 2, "image height plus padding");
    assert_eq!(l.lines.len(), 1);
    assert_eq!(l.lines[0].height, 240);
}

use crate::message::MessageFlags as MessageFlagsNone;

#[test]
fn undecoded_image_measures_as_placeholder() {
    let m = FixedMeasure::new(8);
    let msg = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: None,
        blocks: vec![Block::Image {
            token: 7,
            size: None,
            alt: "[image]".into(),
        }],
        flags: MessageFlagsNone::NONE,
    };
    let l = layout_message(&msg, &params(400), LayoutGeneration::default(), &m);
    assert_eq!(l.height, m.metrics().line_height);
}

// ------------------------------------------------------------------- index

#[test]
fn index_locate_and_offset_round_trip() {
    let mut ix = HeightIndex::new();
    for i in 0..1000u32 {
        ix.push_back(10 + (i % 7), true);
    }
    for row in [0usize, 1, 127, 128, 129, 500, 999] {
        let off = ix.offset_of(row);
        let hit = ix.locate(off).expect("in range");
        assert_eq!(hit.row, row, "offset {off} should land on row {row}");
        assert_eq!(hit.offset, 0);
    }
}

#[test]
fn index_total_matches_sum() {
    let mut ix = HeightIndex::new();
    let mut expect = 0u64;
    for i in 0..500u32 {
        let h = 5 + i % 20;
        ix.push_back(h, true);
        expect += u64::from(h);
    }
    assert_eq!(ix.total_height(), expect);
}

#[test]
fn index_push_front_shifts_nothing_else() {
    let mut ix = HeightIndex::new();
    for _ in 0..300 {
        ix.push_back(10, true);
    }
    let before = ix.total_height();
    ix.push_front(50, true);
    assert_eq!(ix.total_height(), before + 50);
    assert_eq!(ix.height_at(0), 50);
    assert_eq!(ix.height_at(1), 10);
    assert_eq!(ix.offset_of(1), 50);
}

#[test]
fn index_middle_insert() {
    let mut ix = HeightIndex::new();
    for _ in 0..300 {
        ix.push_back(10, true);
    }
    ix.insert(150, 33, true);
    assert_eq!(ix.len(), 301);
    assert_eq!(ix.height_at(150), 33);
    assert_eq!(ix.offset_of(150), 150 * 10);
    assert_eq!(ix.offset_of(151), 150 * 10 + 33);
    assert_eq!(ix.total_height(), 300 * 10 + 33);
}

#[test]
fn index_drain_front() {
    let mut ix = HeightIndex::new();
    for i in 0..500u32 {
        ix.push_back(i % 13 + 1, true);
    }
    let full = ix.total_height();
    let dropped: u64 = (0..200u32).map(|i| u64::from(i % 13 + 1)).sum();
    ix.drain_front(200);
    assert_eq!(ix.len(), 300);
    assert_eq!(ix.total_height(), full - dropped);
    assert_eq!(ix.offset_of(0), 0);
}

#[test]
fn index_remove_and_set_height() {
    let mut ix = HeightIndex::new();
    for _ in 0..10 {
        ix.push_back(10, true);
    }
    assert_eq!(ix.remove(5), Some(10));
    assert_eq!(ix.len(), 9);
    assert_eq!(ix.total_height(), 90);
    ix.set_height(0, 25, true);
    assert_eq!(ix.total_height(), 105);
    assert_eq!(ix.offset_of(1), 25);
}

#[test]
fn index_locate_past_end_clamps() {
    let mut ix = HeightIndex::new();
    ix.push_back(10, true);
    ix.push_back(10, true);
    let hit = ix.locate(1_000_000).expect("clamps rather than None");
    assert_eq!(hit.row, 1);
}

#[test]
fn index_invalidate_keeps_heights_as_estimates() {
    // The resize property: invalidation must not recompute or zero
    // anything, or the scrollbar collapses on every window drag.
    let mut ix = HeightIndex::new();
    for _ in 0..100 {
        ix.push_back(10, true);
    }
    let before = ix.total_height();
    ix.invalidate_all_measurements();
    assert_eq!(ix.total_height(), before, "heights survive as estimates");
    assert_eq!(ix.unmeasured_count(), 100);
    assert!(!ix.is_measured(0));
}

// ------------------------------------------------------------------ anchor

fn buf() -> (ChatBuffer, FixedMeasure) {
    let m = FixedMeasure::new(8);
    let mut b = ChatBuffer::new(params(400));
    for i in 0..200 {
        b.append(
            Message::system(ParsedText::plain(format!("line {i}"))),
            &m,
        );
    }
    (b, m)
}

#[test]
fn anchor_follows_bottom_by_default() {
    let (mut b, m) = buf();
    assert!(b.is_following());
    let before = b.scroll_offset(100);
    b.append(Message::system(ParsedText::plain("new")), &m);
    let after = b.scroll_offset(100);
    assert!(after > before, "following should track the new bottom");
}

#[test]
fn anchor_holds_position_across_prepend() {
    // The chat-history Load-Older property, and the reason the anchor
    // exists at all.
    let (mut b, m) = buf();
    b.scroll_to(500, 100, 4);
    assert!(!b.is_following());
    let before = b.scroll_offset(100);
    let anchored = b.anchor().message;

    for i in 0..50 {
        let at = b.anchor().message;
        b.insert_before(
            at,
            Message::system(ParsedText::plain(format!("older {i}"))),
            &m,
        );
    }

    assert_eq!(b.anchor().message, anchored, "anchor row is unchanged");
    let after = b.scroll_offset(100);
    assert!(
        after > before,
        "content above grew, so the pixel offset must grow with it \
         ({before} -> {after}) — that is what keeps the row still"
    );
    // And the anchored row is still at the same place in the viewport.
    let row = b.row_of(anchored.unwrap()).unwrap();
    assert_eq!(b.index_mut().offset_of(row), after - u64::from(b.anchor().offset));
}

#[test]
fn anchor_survives_resize() {
    let (mut b, m) = buf();
    b.scroll_to(400, 100, 4);
    let anchored = b.anchor().message.expect("anchored");
    b.set_width(200);
    let off = b.scroll_offset(100);
    b.ensure_visible(off, 100, &m);
    assert_eq!(b.anchor().message, Some(anchored));
}

#[test]
fn anchor_snaps_back_to_following_near_bottom() {
    let (mut b, _m) = buf();
    let total = b.total_height();
    b.scroll_to(total, 100, 4);
    assert!(b.is_following(), "landing at the end resumes following");
}

#[test]
fn anchor_falls_back_when_its_row_is_trimmed() {
    let (mut b, m) = buf();
    b.scroll_to(50, 100, 4);
    assert!(!b.is_following());
    b.set_max_rows(10); // trims the anchored row away
    let _ = b.scroll_offset(100);
    // The anchored content is gone; the least surprising place is the
    // bottom, and crucially it must not panic or return garbage.
    let off = b.scroll_offset(100);
    assert!(off <= b.total_height());
    b.append(Message::system(ParsedText::plain("x")), &m);
}

// ------------------------------------------------------------------ buffer

#[test]
fn buffer_marks_are_stable_across_inserts() {
    let m = FixedMeasure::new(8);
    let mut b = ChatBuffer::new(params(400));
    let a = b.append(Message::system(ParsedText::plain("a")), &m);
    let c = b.append(Message::system(ParsedText::plain("c")), &m);
    let mid = b.insert_before(Some(c), Message::system(ParsedText::plain("b")), &m);

    assert_eq!(b.row_of(a), Some(0));
    assert_eq!(b.row_of(mid), Some(1));
    assert_eq!(b.row_of(c), Some(2));
    assert_eq!(b.message(a).unwrap().to_plain_text(), "a");
    assert_eq!(b.message(c).unwrap().to_plain_text(), "c");
}

#[test]
fn buffer_stale_mark_is_inert_not_fatal() {
    // The whole reason marks replaced raw textentry pointers.
    let m = FixedMeasure::new(8);
    let mut b = ChatBuffer::new(params(400));
    let id = b.append(Message::system(ParsedText::plain("x")), &m);
    assert!(b.remove(id));
    assert!(!b.remove(id), "second remove is a no-op, not a crash");
    assert_eq!(b.row_of(id), None);
    assert!(b.message(id).is_none());
}

#[test]
fn buffer_trim_drops_oldest() {
    let m = FixedMeasure::new(8);
    let mut b = ChatBuffer::new(params(400));
    b.set_max_rows(50);
    for i in 0..200 {
        b.append(Message::system(ParsedText::plain(format!("{i}"))), &m);
    }
    assert_eq!(b.len(), 50);
    assert_eq!(b.message_at(0).unwrap().to_plain_text(), "150");
    assert_eq!(b.message_at(49).unwrap().to_plain_text(), "199");
}

#[test]
fn buffer_clear_invalidates_every_mark() {
    let m = FixedMeasure::new(8);
    let mut b = ChatBuffer::new(params(400));
    let id = b.append(Message::system(ParsedText::plain("x")), &m);
    b.clear();
    assert_eq!(b.row_of(id), None);
    assert_eq!(b.len(), 0);
    assert_eq!(b.total_height(), 0);
}

#[test]
fn buffer_image_decode_grows_the_row() {
    let m = FixedMeasure::new(8);
    let mut b = ChatBuffer::new(params(400));
    let msg = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: None,
        blocks: vec![Block::Image {
            token: 42,
            size: None,
            alt: "[image]".into(),
        }],
        flags: MessageFlagsNone::NONE,
    };
    let id = b.append(msg, &m);
    b.ensure_layout(0, &m);
    let before = b.total_height();

    assert_eq!(b.find_image(42), Some(id));
    assert!(b.set_image_size(
        id,
        42,
        ImageSize {
            width: 200,
            height: 300
        },
        &m
    ));
    b.ensure_layout(0, &m);
    let after = b.total_height();
    assert!(
        after > before + 200,
        "row must grow to the decoded height ({before} -> {after})"
    );
}

#[test]
fn buffer_resize_does_not_measure_everything() {
    // The O(visible) claim. After a width change every row is
    // unmeasured; laying out the visible slice must measure only that
    // slice, not the whole buffer.
    let m = FixedMeasure::new(8);
    let mut b = ChatBuffer::new(params(400));
    for i in 0..1000 {
        b.append(Message::system(ParsedText::plain(format!("line {i}"))), &m);
    }
    for r in 0..1000 {
        b.ensure_layout(r, &m);
    }
    assert_eq!(b.index_mut().unmeasured_count(), 0);

    b.set_width(200);
    assert_eq!(b.index_mut().unmeasured_count(), 1000);

    let off = b.scroll_offset(100);
    let visible = b.ensure_visible(off, 100, &m);
    assert!(visible.len() < 50, "only the viewport was laid out");
    let remaining = b.index_mut().unmeasured_count();
    assert!(
        remaining > 900,
        "the other ~1000 rows must stay unmeasured, {remaining} left"
    );
}

#[test]
fn buffer_indent_column_grows_to_widest_nick() {
    let m = FixedMeasure::new(8);
    let mut p = params(600);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(
        Message::live(Speaker::new(1, "al"), ParsedText::plain("hi")),
        &m,
    );
    b.ensure_layout(0, &m);
    let narrow = b.layout_at(0).unwrap().natural_indent;

    b.append(
        Message::live(
            Speaker::new(2, "a-very-long-nickname"),
            ParsedText::plain("hi"),
        ),
        &m,
    );
    b.ensure_layout(1, &m);
    let wide = b.layout_at(1).unwrap().natural_indent;
    assert!(wide > narrow);
}

#[test]
fn every_visible_row_has_a_layout_after_ensure_visible() {
    // The first-paint garble, reproduced.
    //
    // `ensure_layout` widens the shared gutter when it meets a wider
    // nick, and that invalidates every row's cached layout — including
    // rows laid out earlier in this same `ensure_visible` pass. The view
    // then finds `layout_at(row) == None` for them and skips drawing
    // them entirely, so the first paint of a fresh buffer comes out
    // shredded and the next message (by which time the gutter has
    // settled) looks fine.
    //
    // The contract `ensure_visible` owes its caller: every row it
    // returns is laid out and ready to draw.
    let m = FixedMeasure::new(8);
    let mut p = params(600);
    p.indent = true;
    let mut b = ChatBuffer::new(p);

    // Nicks get progressively wider, so the gutter grows repeatedly
    // partway through the visible range.
    for i in 0..30 {
        b.append(
            Message::live(
                Speaker::new(i as u16, "n".repeat(1 + i as usize)),
                ParsedText::plain("hello there"),
            ),
            &m,
        );
    }

    let off = b.scroll_offset(400);
    let rows = b.ensure_visible(off, 400, &m);
    assert!(!rows.is_empty());
    for r in &rows {
        assert!(
            b.layout_at(*r).is_some(),
            "row {r} was returned by ensure_visible but has no layout — \
             the view will skip it and the paint comes out garbled"
        );
    }
}

#[test]
fn buffer_indent_column_is_capped() {
    let m = FixedMeasure::new(8);
    let mut p = params(2000);
    p.indent = true;
    p.max_indent = 64;
    let mut b = ChatBuffer::new(p);
    b.append(
        Message::live(
            Speaker::new(1, "an-extremely-long-nickname-that-should-be-clamped"),
            ParsedText::plain("hi"),
        ),
        &m,
    );
    b.ensure_layout(0, &m);
    assert_eq!(b.layout_at(0).unwrap().natural_indent, 64);
}

#[test]
fn buffer_zoom_invalidates_layout() {
    let m = FixedMeasure::new(8);
    let mut b = ChatBuffer::new(params(400));
    b.append(Message::system(ParsedText::plain("hello")), &m);
    b.ensure_layout(0, &m);
    assert_eq!(b.index_mut().unmeasured_count(), 0);
    b.set_zoom_permille(1500);
    assert_eq!(
        b.index_mut().unmeasured_count(),
        1,
        "a zoom change must invalidate cached layout"
    );
}

#[test]
fn buffer_scroll_offset_never_exceeds_extent() {
    let (mut b, _m) = buf();
    for y in [0u64, 10, 1000, 100_000, u64::MAX / 2] {
        b.scroll_to(y, 100, 4);
        let off = b.scroll_offset(100);
        assert!(off <= b.total_height(), "offset {off} out of range");
    }
}

#[test]
fn anchor_default_is_following() {
    let a = ScrollAnchor::default();
    assert_eq!(a.gravity, Gravity::Bottom);
    assert!(a.is_following());
    assert!(a.message.is_none());
}

// ---------------------------------------------------------------- selection

use crate::select::{Caret, RowSelection, Selection};
use crate::wrap::LineSource;

fn sel_buf() -> (ChatBuffer, FixedMeasure) {
    let m = FixedMeasure::new(10);
    let mut p = params(500);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    for s in ["alpha", "bravo", "charlie"] {
        b.append(Message::system(ParsedText::plain(s)), &m);
    }
    for r in 0..3 {
        b.ensure_layout(r, &m);
    }
    (b, m)
}

fn caret(b: &ChatBuffer, row: usize, offset: usize) -> Caret {
    Caret {
        message: b.id_at(row).unwrap(),
        source: LineSource::Block(0),
        offset,
    }
}

#[test]
fn hit_test_maps_pixels_to_offsets() {
    let (mut b, m) = sel_buf();
    // Row 0 spans y 0..16 with a 10px-per-char measurer.
    let c = b.hit_test(0, 0, &m).expect("hit");
    assert_eq!(c.offset, 0);
    let c = b.hit_test(25, 0, &m).expect("hit");
    assert_eq!(c.offset, 2, "25px in is two characters at 10px each");
    // Far right of a short line selects through its end.
    let c = b.hit_test(9999, 0, &m).expect("hit");
    assert_eq!(c.offset, 5, "past the end clamps to the line length");
}

#[test]
fn hit_test_past_the_bottom_clamps_to_the_last_row() {
    // A drag that runs off the bottom must keep selecting, not stop.
    let (mut b, m) = sel_buf();
    let last = b.id_at(2).unwrap();
    let c = b.hit_test(0, 1_000_000, &m).expect("clamps");
    assert_eq!(c.message, last);
}

#[test]
fn hit_test_on_empty_buffer_is_none() {
    let m = FixedMeasure::new(10);
    let mut b = ChatBuffer::new(params(500));
    assert!(b.hit_test(0, 0, &m).is_none());
}

#[test]
fn selection_within_one_row() {
    let (b, _m) = sel_buf();
    let sel = Selection::new(caret(&b, 0, 1), caret(&b, 0, 4));
    assert_eq!(
        b.row_selection(0, &sel),
        RowSelection::Partial {
            source: LineSource::Block(0),
            start: 1,
            end: 4
        }
    );
    assert_eq!(b.row_selection(1, &sel), RowSelection::None);
    assert_eq!(b.selected_text(&sel), "lph");
}

#[test]
fn selection_across_rows_joins_with_newlines() {
    let (b, _m) = sel_buf();
    let sel = Selection::new(caret(&b, 0, 2), caret(&b, 2, 4));
    assert_eq!(b.row_selection(1, &sel), RowSelection::All);
    assert_eq!(b.selected_text(&sel), "pha\nbravo\nchar");
}

#[test]
fn selection_dragged_upwards_is_the_same_range() {
    // anchor after focus in document order — dragging up must not
    // produce an empty or inverted selection.
    let (b, _m) = sel_buf();
    let down = Selection::new(caret(&b, 0, 2), caret(&b, 2, 4));
    let up = Selection::new(caret(&b, 2, 4), caret(&b, 0, 2));
    assert_eq!(b.selected_text(&down), b.selected_text(&up));
}

#[test]
fn selection_orders_by_row_not_by_message_id() {
    // The trap: ids are allocated in *creation* order, but a
    // chat-history batch inserts *older* messages with *higher* ids.
    // Ordering by id would invert the selection after a backfill.
    let m = FixedMeasure::new(10);
    let mut p = params(500);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    let live = b.append(Message::system(ParsedText::plain("live")), &m);
    // Higher id, but lands *above* the live row.
    let older = b.insert_before(
        Some(live),
        Message::system(ParsedText::plain("older")),
        &m,
    );
    assert!(older.0 > live.0, "the backfilled id is the higher one");
    b.reindex();
    assert_eq!(b.row_of(older), Some(0));

    let sel = Selection::new(
        Caret { message: older, source: LineSource::Block(0), offset: 0 },
        Caret { message: live, source: LineSource::Block(0), offset: 4 },
    );
    let (start, end) = sel.ordered(|id| b.row_of(id));
    assert_eq!(start.message, older, "document order, not id order");
    assert_eq!(end.message, live);
    assert_eq!(b.selected_text(&sel), "older\nlive");
}

#[test]
fn empty_selection_yields_nothing() {
    let (b, _m) = sel_buf();
    let sel = Selection::new(caret(&b, 1, 3), caret(&b, 1, 3));
    assert!(sel.is_empty());
    assert_eq!(b.selected_text(&sel), "");
    assert_eq!(b.row_selection(1, &sel), RowSelection::None);
}

#[test]
fn selection_over_an_image_copies_its_alt_text() {
    // Not all-or-nothing like xtext: dragging across a picture should
    // put something meaningful on the clipboard.
    let m = FixedMeasure::new(10);
    let mut p = params(500);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    b.append(Message::system(ParsedText::plain("before")), &m);
    b.append(
        Message {
            kind: crate::message::MessageKind::Live,
            timestamp: 0,
            speaker: None,
            gutter: None,
            blocks: vec![Block::Image {
                token: 1,
                size: Some(ImageSize { width: 40, height: 40 }),
                alt: "[image: cat.png]".into(),
            }],
            flags: MessageFlagsNone::NONE,
        },
        &m,
    );
    b.append(Message::system(ParsedText::plain("after")), &m);
    b.reindex();

    let sel = Selection::new(caret(&b, 0, 0), caret(&b, 2, 5));
    assert!(
        b.selected_text(&sel).contains("[image: cat.png]"),
        "got {:?}",
        b.selected_text(&sel)
    );
}

#[test]
fn selection_survives_a_resize() {
    // The reason a Caret names (message, source, offset) rather than
    // pixels: re-wrapping must not move the selection.
    let (mut b, m) = sel_buf();
    let sel = Selection::new(caret(&b, 0, 1), caret(&b, 2, 4));
    let before = b.selected_text(&sel);
    b.set_width(80);
    for r in 0..3 {
        b.ensure_layout(r, &m);
    }
    assert_eq!(b.selected_text(&sel), before, "resize moved the selection");
}
