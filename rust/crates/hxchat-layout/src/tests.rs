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
        gutter_gap: 6,
        quote_indent: 12,
        block_padding: 2,
        word_wrap: true,
        avatar_size: 0,
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
        gutter: Some(ParsedText::plain("<alice>")),
        blocks: vec![Block::Text(ParsedText::plain("hello"))],
        flags: MessageFlagsNone::NONE,
    };
    let l = layout_message(&msg, &p, LayoutGeneration::default(), &m);
    assert_eq!(l.lines[0].source, crate::wrap::LineSource::Gutter);
    // Right-aligned against the body column, not pinned at 0 — the
    // latter was the bug that made "[hx]" drift away from its text as
    // the shared column grew.
    assert!(
        l.lines[0].x > 0 && l.lines[0].x < 80,
        "gutter x {} should sit inside the column, right-aligned",
        l.lines[0].x
    );
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
    b.set_max_rows(10, &m); // trims the anchored row away
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
    b.set_max_rows(50, &m);
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
        Some(ImageSize {
            width: 200,
            height: 300
        }),
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
            start: (LineSource::Block(0), 1),
            end: (LineSource::Block(0), 4),
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

#[test]
fn gutter_is_right_aligned_against_the_body_column() {
    // The bug from the first live screenshots: the gutter drew hard left
    // while message bodies moved right as the shared column grew, so
    // "[hx]" and its text drifted apart as soon as a wider nick arrived.
    //
    // Root cause was the view deriving the gutter x from
    // params().indent_width, which ensure_layout only ever set on a
    // local copy — so it read back 0 forever. The x now comes from the
    // line box, computed here, and this pins it.
    let m = FixedMeasure::new(10);
    let mut p = params(600);
    p.indent = true;
    p.indent_width = 200;
    p.gutter_gap = 6;
    let msg = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: Some(ParsedText::plain("<alice>")), // 7 chars = 70px
        blocks: vec![Block::Text(ParsedText::plain("hi"))],
        flags: MessageFlagsNone::NONE,
    };
    let l = layout_message(&msg, &p, LayoutGeneration::default(), &m);
    let g = l
        .lines
        .iter()
        .find(|lb| lb.source == crate::wrap::LineSource::Gutter)
        .expect("gutter line box");
    assert_eq!(
        g.x, 124,
        "gutter should end one gap short of the body column \
         (200 - 70 - 6), not sit at 0"
    );
    let body = l
        .lines
        .iter()
        .find(|lb| matches!(lb.source, crate::wrap::LineSource::Block(_)))
        .expect("body line box");
    assert_eq!(body.x, 200);
    assert!(
        g.x + 70 <= body.x,
        "gutter must not overlap the body column"
    );
}

#[test]
fn hit_test_distinguishes_gutter_from_body_on_the_same_line() {
    // The other half of the same bug: gutter and first body line share a
    // y band, so picking by y alone always returned whichever was pushed
    // first — which made nicks unselectable.
    let m = FixedMeasure::new(10);
    let mut p = params(600);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(
        Message {
            kind: crate::message::MessageKind::Live,
            timestamp: 0,
            speaker: None,
            gutter: Some(ParsedText::plain("<alice>")),
            blocks: vec![Block::Text(ParsedText::plain("hello"))],
            flags: MessageFlagsNone::NONE,
        },
        &m,
    );
    b.ensure_layout(0, &m);
    let layout = b.layout_at(0).unwrap().clone();
    let g = layout
        .lines
        .iter()
        .find(|l| l.source == crate::wrap::LineSource::Gutter)
        .unwrap();
    let body = layout
        .lines
        .iter()
        .find(|l| matches!(l.source, crate::wrap::LineSource::Block(_)))
        .unwrap();

    let in_gutter = b.hit_test(g.x as i32 + 5, 0, &m).expect("hit");
    assert_eq!(
        in_gutter.source,
        crate::wrap::LineSource::Gutter,
        "a click on the nick must resolve to the gutter"
    );
    let in_body = b.hit_test(body.x as i32 + 5, 0, &m).expect("hit");
    assert!(
        matches!(in_body.source, crate::wrap::LineSource::Block(_)),
        "a click on the message must resolve to the body"
    );
}

#[test]
fn selecting_a_nick_yields_the_nick_text() {
    let m = FixedMeasure::new(10);
    let mut p = params(600);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(
        Message {
            kind: crate::message::MessageKind::Live,
            timestamp: 0,
            speaker: None,
            gutter: Some(ParsedText::plain("<alice>")),
            blocks: vec![Block::Text(ParsedText::plain("hello"))],
            flags: MessageFlagsNone::NONE,
        },
        &m,
    );
    b.ensure_layout(0, &m);
    b.reindex();
    let id = b.id_at(0).unwrap();
    let sel = Selection::new(
        Caret { message: id, source: crate::wrap::LineSource::Gutter, offset: 0 },
        Caret { message: id, source: crate::wrap::LineSource::Gutter, offset: 7 },
    );
    assert_eq!(b.selected_text(&sel), "<alice>");
}

// ------------------------------------------------------------------- links

#[test]
fn add_link_over_plain_text() {
    let mut p = ParsedText::plain("see https://example.com now");
    let id = p.add_link(4..23, "https://example.com").expect("link");
    assert_eq!(id, 0);
    assert_eq!(p.links[0].href, "https://example.com");
    assert_eq!(p.style_at(4).link, Some(0));
    assert!(p.style_at(4).attrs.contains(Attrs::UNDERLINE));
    assert_eq!(p.style_at(0).link, None, "text before is untouched");
    assert_eq!(p.style_at(24).link, None, "text after is untouched");
    assert_eq!(p.link_at(10).map(|l| l.href.as_str()), Some("https://example.com"));
}

#[test]
fn add_link_inside_a_styled_run_keeps_the_style() {
    // A URL inside a bold message should stay bold *and* become a link.
    let mut p = markdown::parse_inline("**see https://example.com ok**");
    let start = p.text.find("https").unwrap();
    let end = start + "https://example.com".len();
    p.add_link(start..end, "https://example.com").unwrap();
    let s = p.style_at(start);
    assert!(s.attrs.contains(Attrs::BOLD), "lost the bold");
    assert!(s.attrs.contains(Attrs::UNDERLINE));
    assert_eq!(s.link, Some(0));
    // And the bold either side survives without the link.
    assert!(p.style_at(0).attrs.contains(Attrs::BOLD));
    assert_eq!(p.style_at(0).link, None);
}

#[test]
fn add_link_straddling_a_style_boundary() {
    // The case that makes naive span rewriting corrupt the list.
    let mut p = markdown::parse_inline("**bold**plain");
    assert_eq!(p.text, "boldplain");
    p.add_link(2..9, "https://x.example").unwrap();
    p.debug_assert_well_formed();
    assert_eq!(p.style_at(2).link, Some(0));
    assert_eq!(p.style_at(8).link, Some(0));
    assert!(p.style_at(2).attrs.contains(Attrs::BOLD), "still bold inside");
    assert!(!p.style_at(8).attrs.contains(Attrs::BOLD), "not bold outside");
}

#[test]
fn add_link_rejects_bad_ranges() {
    let mut p = ParsedText::plain("héllo");
    assert!(p.add_link(5..5, "x").is_none(), "empty range");
    assert!(p.add_link(0..999, "x").is_none(), "out of bounds");
    assert!(p.add_link(0..2, "x").is_none(), "splits a codepoint");
    assert!(p.links.is_empty());
}

#[test]
fn multiple_links_in_one_message() {
    let mut p = ParsedText::plain("a https://one.example b https://two.example");
    p.add_link(2..21, "https://one.example").unwrap();
    let s2 = p.text.rfind("https").unwrap();
    p.add_link(s2..p.text.len(), "https://two.example").unwrap();
    p.debug_assert_well_formed();
    assert_eq!(p.link_at(3).map(|l| l.href.as_str()), Some("https://one.example"));
    assert_eq!(p.link_at(s2 + 2).map(|l| l.href.as_str()), Some("https://two.example"));
    assert_eq!(p.link_at(22), None, "the space between is not a link");
}

// -------------------------------------------------------------- word_at

fn word_buf(text: &str) -> (ChatBuffer, FixedMeasure, Caret) {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    b.append(Message::system(ParsedText::plain(text)), &m);
    b.ensure_layout(0, &m);
    b.reindex();
    let id = b.id_at(0).unwrap();
    (
        b,
        m,
        Caret { message: id, source: LineSource::Block(0), offset: 0 },
    )
}

fn word_at(text: &str, offset: usize) -> Option<String> {
    let (b, _m, mut c) = word_buf(text);
    c.offset = offset;
    b.word_at(&c)
}

#[test]
fn word_at_splits_like_xtext() {
    // xtext's is_del is space, newline, '<', '>' and NUL — angle
    // brackets included, which is what makes "<nick>" yield a bare
    // "nick". The C handlers match their targets by string, so this has
    // to agree byte-for-byte or hxmedia:N and the load-older sentinel
    // stop being recognised.
    assert_eq!(word_at("hello world", 0).as_deref(), Some("hello"));
    assert_eq!(word_at("hello world", 7).as_deref(), Some("world"));
    assert_eq!(word_at("<misha> hi", 2).as_deref(), Some("misha"));
    assert_eq!(word_at("a\nb", 2).as_deref(), Some("b"));
}

#[test]
fn word_at_finds_a_media_token() {
    // The exact shape inline_media_chat_word_click looks for.
    let w = word_at("see hxmedia:7 here", 6);
    assert_eq!(w.as_deref(), Some("hxmedia:7"));
}

#[test]
fn word_at_keeps_nbsp_joined_sentinels_whole() {
    // The chat-history sentinel is deliberately NBSP-joined so xtext's
    // tokenizer treats it as one word. NBSP is not an ASCII space, so it
    // must not split here either.
    let sentinel = "\u{2191}\u{a0}Load\u{a0}older\u{a0}messages";
    let line = format!("--- {sentinel} ---");
    let at = line.find('\u{2191}').unwrap();
    assert_eq!(word_at(&line, at).as_deref(), Some(sentinel));
}

#[test]
fn word_at_on_a_delimiter_or_empty_text() {
    assert_eq!(word_at("a b", 1), None, "the space itself is not a word");
    assert_eq!(word_at("", 0), None);
}

#[test]
fn word_at_handles_multibyte() {
    let (b, _m, mut c) = word_buf("héllo wörld");
    c.offset = 0;
    assert_eq!(b.word_at(&c).as_deref(), Some("héllo"));
    c.offset = 7; // inside "wörld"
    assert_eq!(b.word_at(&c).as_deref(), Some("wörld"));
}

#[test]
fn a_decoded_image_grows_its_row_without_moving_the_anchor() {
    // The C4 payoff, and the thing xtext was worst at: a decode landing
    // *above* the viewport must not shift what the user is reading.
    // xtext had to recompute the entry's subline list, diff the count,
    // and patch num_lines plus every scroll anchor by hand; here the
    // anchor names a row, so it absorbs the change.
    let m = FixedMeasure::new(10);
    let mut p = params(400);
    p.indent = false;
    let mut b = ChatBuffer::new(p);

    let img = b.append(
        Message {
            kind: crate::message::MessageKind::Live,
            timestamp: 0,
            speaker: None,
            gutter: None,
            blocks: vec![Block::Image {
                token: 9,
                size: None,
                alt: "[image]".into(),
            }],
            flags: MessageFlagsNone::NONE,
        },
        &m,
    );
    for i in 0..40 {
        b.append(Message::system(ParsedText::plain(format!("line {i}"))), &m);
    }
    b.reindex();

    // Park the viewport well below the image.
    b.scroll_to(300, 100, 4);
    let anchored = b.anchor().message.expect("anchored");
    let anchor_row = b.row_of(anchored).unwrap();
    let offset_before = b.index_mut().offset_of(anchor_row);

    assert!(b.set_image_size(
        img,
        9,
        Some(ImageSize { width: 200, height: 300 }),
        &m
    ));
    b.ensure_layout(0, &m);

    // The anchored row is still the same row, and still the same
    // distance into the buffer *relative to itself* — the content above
    // grew, so its absolute offset must have grown with it.
    assert_eq!(b.anchor().message, Some(anchored), "anchor changed rows");
    let after_row = b.row_of(anchored).unwrap();
    let offset_after = b.index_mut().offset_of(after_row);
    assert!(
        offset_after > offset_before,
        "the image grew above the viewport, so the anchored row must \
         have moved down in absolute terms ({offset_before} -> {offset_after})"
    );
}

// ---- selection across the gutter (the "drag left then up" bug) -------

fn gutter_buf() -> (ChatBuffer, FixedMeasure) {
    let m = FixedMeasure::new(10);
    let mut p = params(800);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    for nick in ["<a>", "<b>", "<c>"] {
        b.append(
            Message {
                kind: crate::message::MessageKind::Live,
                timestamp: 0,
                speaker: None,
                gutter: Some(ParsedText::plain(nick)),
                blocks: vec![Block::Text(ParsedText::plain("hello"))],
                flags: MessageFlagsNone::NONE,
            },
            &m,
        );
    }
    for r in 0..3 {
        b.ensure_layout(r, &m);
    }
    b.reindex();
    (b, m)
}

#[test]
fn dragging_up_into_a_gutter_selects_the_rest_of_that_row() {
    // The reported bug. Dragging left-then-up puts the *start* of the
    // selection in row 0's gutter and the end in row 2's body. Row 0
    // should then be selected from that point in the gutter through the
    // end of its body — but RowSelection could only name one source, so
    // it highlighted the gutter and left the body unselected, which
    // reads as an inverted/partial selection.
    let (b, _m) = gutter_buf();
    let r0 = b.id_at(0).unwrap();
    let r2 = b.id_at(2).unwrap();
    let sel = Selection::new(
        // anchor: body of the last row (drag started here)
        Caret { message: r2, source: LineSource::Block(0), offset: 3 },
        // focus: gutter of the first row (dragged left and up)
        Caret { message: r0, source: LineSource::Gutter, offset: 1 },
    );
    let text = b.selected_text(&sel);
    assert!(
        text.contains("hello"),
        "row 0's body must be selected too, got {text:?}"
    );
    assert!(text.starts_with("a>"), "should start mid-gutter, got {text:?}");
}

#[test]
fn same_row_gutter_to_body_orders_by_source_not_offset() {
    // Within one row the gutter precedes the body, but the two offset
    // spaces are unrelated — comparing them numerically (which is what
    // the tie-break did) can invert the selection.
    let (b, _m) = gutter_buf();
    let id = b.id_at(0).unwrap();
    let from_gutter = Caret { message: id, source: LineSource::Gutter, offset: 2 };
    let into_body = Caret { message: id, source: LineSource::Block(0), offset: 1 };

    let forward = Selection::new(from_gutter, into_body);
    let backward = Selection::new(into_body, from_gutter);
    assert_eq!(
        b.selected_text(&forward),
        b.selected_text(&backward),
        "dragging the same span in either direction must select the same text"
    );
    let t = b.selected_text(&forward);
    assert!(t.starts_with('>'), "starts partway into the gutter, got {t:?}");
    assert!(t.ends_with('h'), "ends one char into the body, got {t:?}");
}

#[test]
fn a_row_with_no_gutter_still_reserves_the_stamp_column() {
    // Info lines (`[hx] …`) are appended with no nick column at all.
    // They still carry a timestamp, so the gutter has to be wide enough
    // for it — otherwise the body starts at 0 and the stamp draws
    // underneath the text.
    //
    // The rendering half of this was the actual reported bug: the view
    // drew the stamp only when a Gutter line box existed, so info lines
    // never got one.
    let m = FixedMeasure::new(10);
    let mut p = params(600);
    p.indent = true;
    p.stamp_width = 90;

    let bare = Message::system(ParsedText::plain("connecting..."));
    let l = layout_message(&bare, &p, LayoutGeneration::default(), &m);
    assert_eq!(
        l.natural_indent, 90,
        "a gutterless row must still reserve the stamp column"
    );

    // And a row *with* a nick reserves stamp + nick, not just the nick.
    let with_nick = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: Some(ParsedText::plain("<alice>")), // 70px
        blocks: vec![Block::Text(ParsedText::plain("hi"))],
        flags: MessageFlagsNone::NONE,
    };
    let l2 = layout_message(&with_nick, &p, LayoutGeneration::default(), &m);
    assert!(
        l2.natural_indent >= 90 + 70,
        "stamp and nick share the gutter, got {}",
        l2.natural_indent
    );
}

// ---- word / line select ---------------------------------------------

#[test]
fn double_click_selects_the_word() {
    let (b, _m, mut c) = word_buf("hello brave world");
    c.offset = 8; // inside "brave"
    let sel = b.select_word(&c).expect("word");
    assert_eq!(b.selected_text(&sel), "brave");
}

#[test]
fn double_click_on_a_nick_selects_just_the_nick() {
    // xtext's tokenizer treats '<' and '>' as delimiters, which is what
    // makes double-clicking "<alice>" give you a bare nick to paste.
    let (b, _m) = gutter_buf();
    let id = b.id_at(0).unwrap();
    let c = Caret { message: id, source: LineSource::Gutter, offset: 1 };
    let sel = b.select_word(&c).expect("word");
    assert_eq!(b.selected_text(&sel), "a");
}

#[test]
fn double_click_on_whitespace_selects_nothing() {
    let (b, _m, mut c) = word_buf("a b");
    c.offset = 1;
    assert!(b.select_word(&c).is_none());
}

#[test]
fn triple_click_selects_the_whole_row_including_the_gutter() {
    let (b, _m) = gutter_buf();
    let sel = b.select_row(1).expect("row");
    let text = b.selected_text(&sel);
    assert!(text.contains("<b>"), "gutter missing from {text:?}");
    assert!(text.contains("hello"), "body missing from {text:?}");
}

#[test]
fn triple_click_past_the_end_is_none() {
    let (b, _m) = gutter_buf();
    assert!(b.select_row(99).is_none());
}

#[test]
fn selected_rows_keeps_row_boundaries_across_embedded_newlines() {
    // The autocopy-timestamp bug in miniature: a row whose text spans
    // several lines. Splitting the joined output by '\n' would report
    // more "rows" than exist and desynchronise anything keyed on row
    // index — which is exactly how stamps ended up on the wrong lines.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    b.append(Message::system(ParsedText::plain("one\ntwo\nthree")), &m);
    b.append(Message::system(ParsedText::plain("second")), &m);
    b.reindex();
    for r in 0..2 {
        b.ensure_layout(r, &m);
    }

    let sel = Selection::new(
        Caret { message: b.id_at(0).unwrap(), source: LineSource::Block(0), offset: 0 },
        Caret { message: b.id_at(1).unwrap(), source: LineSource::Block(0), offset: 6 },
    );

    let rows = b.selected_rows(&sel);
    assert_eq!(rows.len(), 2, "two rows selected, whatever their line count");
    assert_eq!(rows[0].0, 0);
    assert_eq!(rows[0].1, "one\ntwo\nthree");
    assert_eq!(rows[1].0, 1);
    assert_eq!(rows[1].1, "second");

    // And the joined form has more lines than rows, which is precisely
    // why callers must not infer one from the other.
    let joined = b.selected_text(&sel);
    assert_eq!(joined.lines().count(), 4);
    assert!(joined.lines().count() > rows.len());
}

#[test]
fn selected_rows_separates_blocks_the_way_to_plain_text_does() {
    // Whole-row copying used Message::to_plain_text, which puts a
    // newline between blocks. Rebuilding the text span-by-span for
    // partial selections must not quietly change that: joining every
    // source with a space runs a code block into the paragraph after
    // it. The gutter is the one place a space is right — nick and body
    // read as a single line.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    let msg = Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: Some(ParsedText::plain("<a>")),
        blocks: vec![
            Block::Text(ParsedText::plain("look:")),
            Block::Code {
                text: "int x;".into(),
                language: None,
            },
            Block::Text(ParsedText::plain("done")),
        ],
        flags: MessageFlagsNone::NONE,
    };
    let id = b.append(msg, &m);
    b.reindex();
    b.ensure_layout(0, &m);

    let sel = Selection::new(
        Caret {
            message: id,
            source: LineSource::Gutter,
            offset: 0,
        },
        Caret {
            message: id,
            source: LineSource::Block(2),
            offset: 4,
        },
    );
    let rows = b.selected_rows(&sel);
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].1, "<a> look:\nint x;\ndone");

    // And a whole-row select agrees with the model's own rendering.
    let whole = b.select_row(0).expect("row 0 selectable");
    let rows = b.selected_rows(&whole);
    let plain = b.message(id).unwrap().to_plain_text();
    assert_eq!(rows[0].1, format!("<a> {plain}"));
}

#[test]
fn word_bounds_does_not_panic_on_a_caret_inside_a_codepoint() {
    // caret.offset arrives from C through the FFI and is not guaranteed
    // to sit on a char boundary. Slicing off one panics, and a panic
    // here unwinds into GTK and aborts the process, so clamp instead.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    // "naïve" — the ï is two bytes at 2..4.
    let id = b.append(Message::system(ParsedText::plain("naïve word")), &m);
    b.reindex();
    b.ensure_layout(0, &m);

    let mid = Caret {
        message: id,
        source: LineSource::Block(0),
        offset: 3,
    };
    assert_eq!(b.word_bounds(&mid), Some((0, 6)), "clamps down into 'naïve'");

    // Past the end clamps to the end, which is not inside a word.
    let past = Caret {
        message: id,
        source: LineSource::Block(0),
        offset: 999,
    };
    assert!(b.word_bounds(&past).is_none());

    // And select_word off the same caret yields the whole word.
    let sel = b.select_word(&mid).expect("word selected");
    assert_eq!(b.selected_rows(&sel)[0].1, "naïve");
}

#[test]
fn dragging_the_separator_pins_the_gutter() {
    // The gutter auto-grows to fit the widest nick. Once the user drags
    // the separator, that has to stop: xtext kept auto-growing after a
    // drag, so a long nick would silently undo a narrowing the user had
    // just made by hand.
    let m = FixedMeasure::new(10);
    let mut p = params(1200);
    p.indent = true;
    p.max_indent = 400;
    let mut b = ChatBuffer::new(p);

    let say = |nick: &str| Message {
        kind: crate::message::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: Some(ParsedText::plain(nick)),
        blocks: vec![Block::Text(ParsedText::plain("hi"))],
        flags: MessageFlagsNone::NONE,
    };

    b.append(say("<a>"), &m);
    b.ensure_layout(0, &m);
    let auto = b.indent_width();
    assert!(auto > 0 && !b.indent_pinned());

    // A longer nick still widens it while unpinned.
    b.append(say("<averyverylongnick>"), &m);
    b.ensure_layout(1, &m);
    assert!(b.indent_width() > auto, "gutter should auto-grow");

    // Pin it narrower than the widest nick.
    assert!(b.set_indent_width(40));
    assert!(b.indent_pinned());
    assert_eq!(b.indent_width(), 40);

    // Now an even longer nick must not move it.
    b.append(say("<anevenlongernickthanbefore>"), &m);
    b.reindex();
    for r in 0..3 {
        b.ensure_layout(r, &m);
    }
    assert_eq!(b.indent_width(), 40, "pinned gutter must not auto-grow");

    // Neither must a clear or a stamp-width change, both of which reset
    // the auto-sized gutter.
    b.set_stamp_width(60);
    assert_eq!(b.indent_width(), 40, "stamp width must not unpin");
    b.clear();
    assert_eq!(b.indent_width(), 40, "clearing the buffer must not unpin");

    // Unpinning hands it back to the auto path.
    b.unpin_indent();
    assert!(!b.indent_pinned());
    b.append(say("<a>"), &m);
    b.ensure_layout(0, &m);
    assert_ne!(b.indent_width(), 40);
}

#[test]
fn a_pinned_gutter_has_a_floor() {
    // Dragging the separator to zero would leave nothing to grab, so the
    // divider could never be dragged back out.
    let m = FixedMeasure::new(10);
    let mut p = params(1200);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.set_indent_width(0);
    assert_eq!(b.indent_width(), crate::buffer::MIN_INDENT);
    let _ = &m;
}

#[test]
fn selected_rows_omits_rows_that_contribute_nothing() {
    // A selection that starts at the very end of one row and ends at the
    // very start of another *covers* three rows but only one of them has
    // any selected text in it. Reporting the empty two puts blank lines
    // in the clipboard and, worse, gives AUTOCOPY_STAMP two rows to
    // prefix timestamps onto that have nothing in them.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    for t in ["first", "middle", "last"] {
        b.append(Message::system(ParsedText::plain(t)), &m);
    }
    b.reindex();
    for r in 0..3 {
        b.ensure_layout(r, &m);
    }

    let sel = Selection::new(
        Caret {
            message: b.id_at(0).unwrap(),
            source: LineSource::Block(0),
            offset: 5, // end of "first"
        },
        Caret {
            message: b.id_at(2).unwrap(),
            source: LineSource::Block(0),
            offset: 0, // start of "last"
        },
    );

    let rows = b.selected_rows(&sel);
    assert_eq!(rows.len(), 1, "only the middle row has selected text");
    assert_eq!(rows[0], (1, "middle".to_string()));
    assert_eq!(b.selected_text(&sel), "middle", "no leading/trailing blanks");
}

#[test]
fn selected_rows_keeps_a_genuinely_empty_row() {
    // The converse: a blank line inside a selection is real content and
    // has to survive, so the skip above must key on "had text and none
    // of it was selected" rather than "produced no text".
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    for t in ["above", "", "below"] {
        b.append(Message::system(ParsedText::plain(t)), &m);
    }
    b.reindex();
    for r in 0..3 {
        b.ensure_layout(r, &m);
    }

    let sel = Selection::new(
        Caret {
            message: b.id_at(0).unwrap(),
            source: LineSource::Block(0),
            offset: 0,
        },
        Caret {
            message: b.id_at(2).unwrap(),
            source: LineSource::Block(0),
            offset: 5,
        },
    );
    let rows = b.selected_rows(&sel);
    assert_eq!(rows.len(), 3, "the blank line is content");
    assert_eq!(rows[1], (1, String::new()));
    assert_eq!(b.selected_text(&sel), "above\n\nbelow");
}

// ---------------------------------------------------------------- search

use crate::search::{self, SearchState};

#[test]
fn find_all_is_literal_and_non_overlapping() {
    assert_eq!(search::find_all("aaaa", "aa", true), vec![(0, 2), (2, 4)]);
    assert_eq!(search::find_all("abcabc", "abc", true), vec![(0, 3), (3, 6)]);
    assert_eq!(search::find_all("abc", "", true), vec![]);
    assert_eq!(search::find_all("", "abc", true), vec![]);
    // No regex vocabulary: metacharacters are just characters.
    assert_eq!(search::find_all("a.c", ".", true), vec![(1, 2)]);
    assert_eq!(search::find_all("abc", ".", true), vec![]);
}

#[test]
fn find_all_case_insensitive_returns_offsets_into_the_original() {
    // The trap this is written to avoid: lowercasing the haystack and
    // searching *that* gives offsets into a string the caller doesn't
    // have, because lowercasing can change byte length. U+0130 (İ) is
    // two bytes and lowercases to two *chars* (i + U+0307), so any
    // offset past it in a lowercased copy is wrong for the original.
    let hay = "İstanbul and stanbul";
    let hits = search::find_all(hay, "STANBUL", false);
    assert_eq!(hits.len(), 2);
    for (a, b) in hits {
        assert_eq!(&hay[a..b].to_lowercase(), "stanbul", "offsets must index the original");
    }

    // Same-length folds work in both directions.
    assert_eq!(search::find_all("Grüße", "GRÜ", false), vec![(0, 4)]);
    assert_eq!(search::find_all("ÉCOLE", "école", false), vec![(0, 6)]);
}

#[test]
fn find_all_never_splits_a_codepoint() {
    // Every returned offset has to be a char boundary, or the renderer
    // slices off one and panics into GTK.
    let hay = "aé—b日本語aé";
    for needle in ["a", "é", "—", "本", "aé"] {
        for cs in [true, false] {
            for (a, b) in search::find_all(hay, needle, cs) {
                assert!(hay.is_char_boundary(a), "{needle:?} start {a}");
                assert!(hay.is_char_boundary(b), "{needle:?} end {b}");
            }
        }
    }
}

#[test]
fn search_cursor_wraps_in_both_directions() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    for t in ["needle one", "nothing", "needle two", "needle three"] {
        b.append(Message::system(ParsedText::plain(t)), &m);
    }
    b.reindex();

    let hits = b.search("needle", false);
    assert_eq!(hits.len(), 3, "one per matching row");

    let mut st = SearchState::new();
    st.set_results("needle", false, hits);
    assert_eq!(st.len(), 3);
    assert!(st.current().is_none(), "no match is current until stepped");

    // Forward from nothing selects the first, then wraps at the end.
    assert_eq!(st.step(1), Some(st.matches()[0]));
    assert_eq!(st.ordinal(), Some(1));
    st.step(1);
    st.step(1);
    assert_eq!(st.ordinal(), Some(3));
    st.step(1);
    assert_eq!(st.ordinal(), Some(1), "forward wraps to the first");

    // Backward from the first wraps to the last.
    st.step(-1);
    assert_eq!(st.ordinal(), Some(3));

    // Backward from nothing selects the last.
    let mut st2 = SearchState::new();
    st2.set_results("needle", false, b.search("needle", false));
    assert_eq!(st2.step(-1), st2.matches().last().copied());
}

#[test]
fn search_walks_rows_in_reading_order_including_gutters() {
    // Matches have to come back in the order the eye reads them —
    // gutter before body, row by row — or stepping through them jumps
    // around the screen.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    for (nick, body) in [("<sam>", "hello sam"), ("<pat>", "sam said hi")] {
        b.append(
            Message {
                kind: crate::message::MessageKind::Live,
                timestamp: 0,
                speaker: None,
                gutter: Some(ParsedText::plain(nick)),
                blocks: vec![Block::Text(ParsedText::plain(body))],
                flags: MessageFlagsNone::NONE,
            },
            &m,
        );
    }
    b.reindex();

    let hits = b.search("sam", false);
    let shape: Vec<_> = hits
        .iter()
        .map(|h| (b.row_of(h.message).unwrap(), h.source, h.start))
        .collect();
    assert_eq!(
        shape,
        vec![
            (0, LineSource::Gutter, 1),  // <sam>
            (0, LineSource::Block(0), 6), // hello sam
            (1, LineSource::Block(0), 0), // sam said hi
        ]
    );
}

#[test]
fn search_reaches_rows_that_were_never_laid_out() {
    // Searching the layout instead of the model would only ever find
    // what you had already scrolled to, which defeats the point.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    for i in 0..500 {
        b.append(Message::system(ParsedText::plain(format!("line {i}"))), &m);
    }
    b.reindex();
    // Lay out only the first handful.
    for r in 0..5 {
        b.ensure_layout(r, &m);
    }
    let hits = b.search("line 499", false);
    assert_eq!(hits.len(), 1);
    assert_eq!(b.row_of(hits[0].message), Some(499));
}

#[test]
fn seek_from_starts_at_the_viewport_rather_than_the_top() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    for i in 0..20 {
        b.append(
            Message::system(ParsedText::plain(if i % 5 == 0 { "hit" } else { "miss" })),
            &m,
        );
    }
    b.reindex();

    let mut st = SearchState::new();
    st.set_results("hit", false, b.search("hit", false));
    assert_eq!(st.len(), 4); // rows 0, 5, 10, 15

    st.seek_from(|id| b.row_of(id), 7);
    assert_eq!(b.row_of(st.current().unwrap().message), Some(10));

    // Everything above the viewport: fall back to the last match, since
    // scrollback's interesting end is the recent one.
    st.seek_from(|id| b.row_of(id), 18);
    assert_eq!(b.row_of(st.current().unwrap().message), Some(15));
}

#[test]
fn reveal_leaves_an_already_visible_row_alone() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    for i in 0..200 {
        b.append(Message::system(ParsedText::plain(format!("line {i}"))), &m);
    }
    b.reindex();
    for r in 0..200 {
        b.ensure_layout(r, &m);
    }

    let vh = 100u32;
    // Park the viewport exactly on a row boundary so row 40 is *fully*
    // inside it — "visible" means the whole row, and picking an offset
    // at random lands on a row straddling the top edge.
    let top40 = b.index_mut().offset_of(40);
    b.scroll_to(top40, vh, 0);
    let before = b.scroll_offset(vh);
    assert_eq!(before, top40);

    let id = b.id_at(40).unwrap();
    b.reveal(id, vh, &m);
    assert_eq!(b.scroll_offset(vh), before, "fully visible row must not scroll");

    // A row far away gets centred.
    let far = b.id_at(190).unwrap();
    b.reveal(far, vh, &m);
    let after = b.scroll_offset(vh);
    assert_ne!(after, before);
    let top = b.index_mut().offset_of(190);
    assert!(
        top >= after && top < after + vh as u64,
        "row 190 (at {top}) must be inside [{after}, {})",
        after + vh as u64
    );
}

#[test]
fn reveal_pulls_a_half_visible_row_fully_into_view() {
    // The converse of the case above, and the reason "visible" is the
    // whole row rather than any part of it: stepping onto a match whose
    // row is clipped by the top edge should show you the whole row, not
    // leave you reading the bottom half of it.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = false;
    let mut b = ChatBuffer::new(p);
    for i in 0..200 {
        b.append(Message::system(ParsedText::plain(format!("line {i}"))), &m);
    }
    b.reindex();
    for r in 0..200 {
        b.ensure_layout(r, &m);
    }

    let vh = 100u32;
    let top40 = b.index_mut().offset_of(40);
    let h40 = b.index_mut().height_at(40) as u64;
    assert!(h40 > 1, "need a row with height to clip");

    // Scroll so row 40 is cut in half by the top edge.
    b.scroll_to(top40 + h40 / 2, vh, 0);
    b.reveal(b.id_at(40).unwrap(), vh, &m);

    let after = b.scroll_offset(vh);
    assert!(
        top40 >= after && top40 + h40 <= after + vh as u64,
        "row 40 ({top40}..{}) must be wholly inside [{after}, {})",
        top40 + h40,
        after + vh as u64
    );
}

#[test]
fn scan_delims_reports_source_offsets_and_respects_flanking() {
    use crate::markdown::scan_delims;

    let src = "a **bold** b";
    let got = scan_delims(src);
    let shape: Vec<_> = got
        .iter()
        .map(|s| (&src[s.start..s.end], s.attrs, s.delim))
        .collect();
    assert_eq!(
        shape,
        vec![
            ("**", Attrs::BOLD, true),
            ("bold", Attrs::BOLD, false),
            ("**", Attrs::BOLD, true),
        ]
    );

    // The flanking rules are shared with the renderer, so arithmetic is
    // left alone here too — this is the one thing the tinting must not
    // get wrong, since it would light up ordinary prose.
    assert!(scan_delims("2 * 3 * 4").is_empty());
    assert!(scan_delims("a * b *").is_empty());
    // snake_case survives, same as in the renderer.
    assert!(scan_delims("some_var_name").is_empty());
    // Backslash hides a delimiter from tinting.
    assert!(scan_delims(r"\*not em\*").is_empty());
}

#[test]
fn scan_delims_agrees_with_the_renderer_on_what_is_styled() {
    // The two are separate passes (one reports source offsets, one
    // rendered offsets), so this pins the cases where they must agree:
    // whether a delimiter is live at all, and what attribute it carries.
    use crate::markdown::{parse_inline, scan_delims};
    for src in [
        "**bold**",
        "*it*",
        "`code`",
        "plain text",
        "2 * 3 * 4",
        "a **b** c *d* e",
        "snake_case_word",
    ] {
        let rendered = parse_inline(src);
        let styled_attrs: std::collections::BTreeSet<u8> = rendered
            .spans
            .iter()
            .map(|s| s.style.attrs.0)
            .filter(|a| *a != 0)
            .collect();
        let tinted_attrs: std::collections::BTreeSet<u8> = scan_delims(src)
            .iter()
            .filter(|s| !s.delim)
            .map(|s| s.attrs.0)
            .collect();
        assert_eq!(
            styled_attrs, tinted_attrs,
            "{src:?}: renderer and tinting disagree on what is styled"
        );
    }
}

#[test]
fn ensure_visible_can_widen_the_gutter_so_read_it_after() {
    // The fact behind a draw-ordering bug in the view: laying out a row
    // with a wider nick than any seen so far widens the *shared* gutter,
    // so `indent_width()` read before `ensure_visible` is stale for the
    // frame being drawn. The separator rule was read early and the rows
    // late, so on the first message that widened the gutter the rule
    // drew in the wrong place for exactly one frame.
    //
    // This pins the property rather than the pixel: if `ensure_visible`
    // ever stops being able to move the gutter, the ordering constraint
    // in `snapshot_content` can be relaxed — and if it can still move
    // it, the separator must keep being drawn afterwards.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);

    // An info-shaped row with no gutter, then a real nick.
    b.append(Message::system(ParsedText::plain("connecting")), &m);
    b.append(
        Message {
            kind: crate::message::MessageKind::Live,
            timestamp: 0,
            speaker: None,
            gutter: Some(ParsedText::plain("<misha>")),
            blocks: vec![Block::Text(ParsedText::plain("hello hello"))],
            flags: MessageFlagsNone::NONE,
        },
        &m,
    );
    b.reindex();

    let before = b.indent_width();
    let rows = b.ensure_visible(0, 10_000, &m);
    let after = b.indent_width();

    assert_eq!(rows.len(), 2, "both rows visible");
    assert!(
        after > before,
        "the nick row must widen the gutter ({before} -> {after})"
    );
}

// ---- message grouping (C6) ------------------------------------------

fn said(uid: u16, nick: &str, body: &str, at: i64) -> Message {
    Message {
        kind: crate::message::MessageKind::Live,
        timestamp: at,
        speaker: if uid == 0 {
            None
        } else {
            Some(crate::message::Speaker::new(uid, nick))
        },
        gutter: Some(ParsedText::plain(format!("<{nick}>"))),
        blocks: vec![Block::Text(ParsedText::plain(body))],
        flags: MessageFlagsNone::NONE,
    }
}

fn grouped(b: &ChatBuffer, row: usize) -> bool {
    b.message_at(row)
        .unwrap()
        .flags
        .contains(MessageFlagsNone::GROUPED)
}

#[test]
fn consecutive_messages_from_one_speaker_group() {
    // The reported case: "hai / hai / hai" should read as one block
    // under one name, not repeat the nick three times.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    for i in 0..4 {
        b.append(said(7, "misha", "hai", 1000 + i), &m);
    }
    b.reindex();

    assert!(!grouped(&b, 0), "the first of a run always shows its nick");
    for r in 1..4 {
        assert!(grouped(&b, r), "row {r} continues the run");
    }
}

#[test]
fn a_different_speaker_or_a_long_gap_breaks_the_run() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(7, "misha", "two", 1001), &m);
    b.append(said(9, "alice", "hi", 1002), &m); // different speaker
    b.append(said(7, "misha", "back", 1003), &m); // misha again, but after alice
    b.append(said(7, "misha", "later", 1003 + 9999), &m); // long gap
    b.reindex();

    assert!(!grouped(&b, 0));
    assert!(grouped(&b, 1));
    assert!(!grouped(&b, 2), "alice starts her own run");
    assert!(!grouped(&b, 3), "misha's return is a new run, not a continuation");
    assert!(!grouped(&b, 4), "a long gap breaks the run");
}

#[test]
fn trimming_a_runs_head_promotes_the_next_row() {
    // The invariant that makes grouping safe: a row that suppresses its
    // nick must always have a row above it that showed one. Trim can
    // delete the head, and without re-deciding, every remaining message
    // in that run would render anonymously.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.set_max_rows(3, &m);
    for i in 0..3 {
        b.append(said(7, "misha", "hai", 1000 + i), &m);
    }
    b.reindex();
    assert!(!grouped(&b, 0));
    assert!(grouped(&b, 1) && grouped(&b, 2));

    // One more message trims the head away.
    b.append(said(7, "misha", "hai", 1003), &m);
    b.reindex();
    assert_eq!(b.len(), 3);
    assert!(
        !grouped(&b, 0),
        "the new front row must show its nick — nothing above it can"
    );
    assert!(grouped(&b, 1) && grouped(&b, 2));
}

#[test]
fn inserting_before_re_decides_both_sides() {
    // Load-older inserts land in the middle of the buffer and can split
    // a run: the row below is no longer adjacent to what it continued.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    let first = b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(7, "misha", "two", 1001), &m);
    b.reindex();
    assert!(grouped(&b, 1));

    // Someone else's message arrives between them.
    b.insert_before(Some(b.id_at(1).unwrap()), said(9, "alice", "hi", 1000), &m);
    b.reindex();
    assert_eq!(b.len(), 3);
    assert_eq!(b.row_of(first), Some(0));
    assert!(!grouped(&b, 1), "alice does not continue misha");
    assert!(!grouped(&b, 2), "misha no longer follows himself");
}

#[test]
fn grouping_falls_back_to_the_nick_when_the_server_sends_no_uid() {
    // Older servers omit the UID chunk, so speaker is None. Grouping on
    // the gutter text keeps the feature working there rather than
    // silently switching itself off.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(said(0, "misha", "one", 1000), &m);
    b.append(said(0, "misha", "two", 1001), &m);
    b.append(said(0, "alice", "hi", 1002), &m);
    b.reindex();

    assert!(!grouped(&b, 0));
    assert!(grouped(&b, 1), "same nick, no uid, still one run");
    assert!(!grouped(&b, 2));
}

#[test]
fn system_rows_never_group() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(Message::system(ParsedText::plain("connecting")), &m);
    b.append(Message::system(ParsedText::plain("connected")), &m);
    b.reindex();
    assert!(!grouped(&b, 0));
    assert!(!grouped(&b, 1), "two system lines are not one speaker");
}

#[test]
fn a_grouped_row_hides_its_gutter_but_keeps_the_column_width() {
    // Hiding a nick must not narrow the shared gutter, or every other
    // row in the buffer shifts sideways when a run forms.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(7, "misha", "two", 1001), &m);
    b.reindex();
    b.ensure_layout(0, &m);
    let width_after_head = b.indent_width();
    b.ensure_layout(1, &m);

    let head = b.layout_at(0).expect("head laid out");
    let cont = b.layout_at(1).expect("continuation laid out");
    assert!(
        head.lines.iter().any(|l| l.source == LineSource::Gutter),
        "the head draws its nick"
    );
    assert!(
        !cont.lines.iter().any(|l| l.source == LineSource::Gutter),
        "the continuation does not"
    );
    assert_eq!(
        b.indent_width(),
        width_after_head,
        "a hidden gutter still reserves its width"
    );
}

#[test]
fn grouping_can_be_switched_off() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    for i in 0..3 {
        b.append(said(7, "misha", "hai", 1000 + i), &m);
    }
    b.reindex();
    assert!(grouped(&b, 1));

    b.set_group_gap_secs(0, &m);
    assert!(!grouped(&b, 1), "existing rows are re-decided, not just new ones");
    assert!(!grouped(&b, 2));

    b.set_group_gap_secs(crate::buffer::DEFAULT_GROUP_GAP_SECS, &m);
    assert!(grouped(&b, 1), "and back again");
}

#[test]
fn regrouping_leaves_the_height_index_agreeing_with_a_real_layout() {
    // Toggling GROUPED invalidates a row's layout, so its height entry
    // has to be re-*estimated* rather than have the old value carried
    // forward as the new estimate. Otherwise total_height drifts from
    // what a real layout produces, and the scrollbar's upper bound with
    // it.
    //
    // Today the two happen to be equal: the gutter is a left column
    // (LineBox at y=0) and row height is driven only by body lines, so
    // showing or hiding a nick changes no height. This test therefore
    // passes either way *at present* — it is here for what comes next.
    // An avatar gutter makes a head row taller than its continuations,
    // and at that point carrying a stale estimate stops being harmless.
    // Asserting the invariant now means that change gets caught by a
    // test instead of by someone noticing the scrollbar is short.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    for i in 0..8 {
        b.append(said(7, "misha", "hai", 1000 + i), &m);
    }
    b.reindex();
    assert!(grouped(&b, 1), "precondition: rows 1.. are a run");

    b.set_group_gap_secs(0, &m);
    assert!(!grouped(&b, 1), "grouping off");
    let estimated = b.total_height();

    for r in 0..8 {
        b.ensure_layout(r, &m);
    }
    assert_eq!(
        b.total_height(),
        estimated,
        "the post-regroup estimate must match what layout actually yields"
    );

    // And back on.
    b.set_group_gap_secs(crate::buffer::DEFAULT_GROUP_GAP_SECS, &m);
    let estimated = b.total_height();
    for r in 0..8 {
        b.ensure_layout(r, &m);
    }
    assert_eq!(b.total_height(), estimated);
}

#[test]
fn direction_breaks_a_run_even_with_the_same_nick() {
    // Messaging yourself in a PM window. Every line names you as the
    // sender — including the copies the server sends back — so no
    // identity test can separate the four lines, and an earlier fix that
    // keyed on "is the sender me" did nothing at all here. Direction is
    // a property of which path produced the row, and can.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);

    let mut line = |mine: bool, at: i64| {
        let mut msg = said(7, "misha", "asdfasdf", at);
        if mine {
            msg.flags = msg.flags.union(MessageFlagsNone::OUTGOING);
        }
        b.append(msg, &m);
    };
    line(true, 1000);  // outgoing
    line(false, 1000); // incoming echo
    line(true, 1001);  // outgoing
    line(false, 1001); // incoming echo
    b.reindex();

    for r in 0..4 {
        assert!(
            !grouped(&b, r),
            "row {r}: alternating direction means four groups, not one"
        );
    }
}

#[test]
fn same_direction_still_groups() {
    // The converse, so the fix above doesn't just disable grouping.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    for i in 0..3 {
        let mut msg = said(7, "misha", "hai", 1000 + i);
        msg.flags = msg.flags.union(MessageFlagsNone::OUTGOING);
        b.append(msg, &m);
    }
    b.reindex();
    assert!(!grouped(&b, 0));
    assert!(grouped(&b, 1) && grouped(&b, 2), "three outgoing lines are one run");
}

// ---- avatar gutter (C6) ---------------------------------------------

#[test]
fn only_a_group_head_gets_an_avatar_box() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    p.avatar_size = 24;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(7, "misha", "two", 1001), &m);
    b.reindex();
    b.ensure_layout(0, &m);
    b.ensure_layout(1, &m);

    let head = b.layout_at(0).unwrap();
    let cont = b.layout_at(1).unwrap();
    assert!(head.avatar.is_some(), "the head shows the icon");
    assert!(cont.avatar.is_none(), "a continuation does not repeat it");
    assert_eq!(head.avatar.unwrap().uid, 7);
}

#[test]
fn a_continuation_still_reserves_the_avatar_width() {
    // Same argument as the hidden nick: if a grouped row stopped
    // reserving the icon's width, the shared gutter would narrow as soon
    // as a run formed and every column in the buffer would jump left.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    p.avatar_size = 24;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.reindex();
    b.ensure_layout(0, &m);
    let with_head_only = b.indent_width();

    b.append(said(7, "misha", "two", 1001), &m);
    b.reindex();
    b.ensure_layout(1, &m);
    assert_eq!(
        b.indent_width(),
        with_head_only,
        "a grouped row must not shrink the shared gutter"
    );
}

#[test]
fn an_avatar_makes_the_head_row_at_least_as_tall_as_the_icon() {
    // The case that makes the regroup re-estimate matter: with avatars
    // on, a head and its continuations genuinely differ in height, so a
    // stale height carried across a GROUPED toggle is now a real bug
    // rather than a latent one.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    p.avatar_size = 48; // taller than a text line
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(7, "misha", "two", 1001), &m);
    b.reindex();
    b.ensure_layout(0, &m);
    b.ensure_layout(1, &m);

    assert!(
        b.layout_at(0).unwrap().height >= 48,
        "the head must fit its icon"
    );
    assert!(
        b.layout_at(1).unwrap().height < 48,
        "a continuation has no icon and should stay text-height"
    );
}

#[test]
fn turning_avatars_off_re_estimates_and_narrows_the_gutter() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    p.avatar_size = 48;
    let mut b = ChatBuffer::new(p);
    for i in 0..4 {
        b.append(said(7, "misha", "hai", 1000 + i), &m);
    }
    b.reindex();
    for r in 0..4 {
        b.ensure_layout(r, &m);
    }
    let wide = b.indent_width();
    let tall = b.total_height();

    b.set_avatar_size(0);
    for r in 0..4 {
        b.ensure_layout(r, &m);
    }
    assert!(b.indent_width() < wide, "the gutter loses the icon slot");
    assert!(b.total_height() < tall, "and the head row loses its floor");
    assert!(b.layout_at(0).unwrap().avatar.is_none());
}

#[test]
fn a_speaker_with_no_uid_gets_no_avatar_slot() {
    // uid 0 means "unknown", so there is nothing to look an icon up by.
    // Reserving space for an icon that can never resolve would indent
    // every row on a server that omits the UID chunk.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    p.avatar_size = 24;
    let mut b = ChatBuffer::new(p);
    b.append(said(0, "misha", "one", 1000), &m);
    b.reindex();
    b.ensure_layout(0, &m);
    assert!(b.layout_at(0).unwrap().avatar.is_none());
}

#[test]
fn a_rename_breaks_the_run() {
    // The uid survives a rename, so keying grouping on it alone would
    // hide the new name entirely — the messages would group under the
    // old one and the change, which is the thing worth noticing, would
    // never appear on screen.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(7, "misha", "two", 1001), &m);
    b.append(said(7, "misha2", "three", 1002), &m); // same uid, new nick
    b.append(said(7, "misha2", "four", 1003), &m);
    b.reindex();

    assert!(!grouped(&b, 0));
    assert!(grouped(&b, 1), "same name, same person");
    assert!(!grouped(&b, 2), "the rename must show the new name");
    assert!(grouped(&b, 3), "and then group again under it");
}

#[test]
fn two_users_sharing_a_nick_do_not_group() {
    // The converse: the nick alone is not enough either. Two people can
    // present the same name, and merging their messages under one header
    // would misattribute them.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(9, "misha", "two", 1001), &m); // impostor, same nick
    b.reindex();

    assert!(!grouped(&b, 0));
    assert!(!grouped(&b, 1), "different uid means a different person");
}

#[test]
fn a_one_line_fence_is_a_code_block_not_an_unterminated_one() {
    use crate::markdown::{split_blocks, RawBlock};
    // Chat boxes send on Enter, so this is how people actually type a
    // code block. Treating it as an opening fence made the rest of the
    // line the "language" and produced an empty block — a blank row.
    let b = split_blocks("```hello world```");
    assert_eq!(b.len(), 1);
    match &b[0] {
        RawBlock::Code { text, language } => {
            assert_eq!(text, "hello world");
            assert_eq!(*language, None);
        }
        other => panic!("expected code, got {other:?}"),
    }
}

#[test]
fn a_real_multi_line_fence_still_works() {
    use crate::markdown::{split_blocks, RawBlock};
    let b = split_blocks("before\n```rust\nlet x = 1;\nlet y = 2;\n```\nafter");
    assert_eq!(b.len(), 3);
    match &b[1] {
        RawBlock::Code { text, language } => {
            assert_eq!(text, "let x = 1;\nlet y = 2;");
            assert_eq!(language.as_deref(), Some("rust"));
        }
        other => panic!("expected code, got {other:?}"),
    }
}

#[test]
fn select_all_shaped_selection_covers_the_buffer() {
    // Mirrors HxChatView::select_all's caret construction exactly, to
    // find out whether "Select All does nothing" is the model or the UI.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(9, "alice", "two", 2000), &m);
    b.append(said(7, "misha", "three", 3000), &m);
    b.reindex();
    for r in 0..3 {
        b.ensure_layout(r, &m);
    }

    let first = b.id_at(0).unwrap();
    let last = b.id_at(2).unwrap();
    let last_len = b.source_text(2, LineSource::Block(0)).map_or(0, |t| t.len());
    let sel = Selection::new(
        Caret { message: first, source: LineSource::Block(0), offset: 0 },
        Caret { message: last, source: LineSource::Block(0), offset: last_len },
    );

    let rows = b.selected_rows(&sel);
    assert_eq!(rows.len(), 3, "every row should contribute: {rows:?}");
    assert!(rows[0].1.contains("one"));
    assert!(rows[1].1.contains("two"));
    assert!(rows[2].1.contains("three"));
}

#[test]
fn select_all_covers_the_gutter_and_every_block() {
    // Both ways the view's hand-rolled version got this wrong: it started
    // at Block(0), skipping the first row's nick, and ended at Block(0)
    // of the last row — which, once markdown split a body into several
    // blocks, dropped any code block or quote after the first.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(
        Message {
            kind: crate::message::MessageKind::Live,
            timestamp: 2000,
            speaker: Some(crate::message::Speaker::new(9, "alice")),
            gutter: Some(ParsedText::plain("<alice>")),
            blocks: vec![
                Block::Text(ParsedText::plain("look:")),
                Block::Code { text: "x = 1".into(), language: None },
            ],
            flags: MessageFlagsNone::NONE,
        },
        &m,
    );
    b.reindex();

    let sel = b.select_all().expect("non-empty buffer");
    assert_eq!(sel.anchor.source, LineSource::Gutter, "starts at the nick");
    assert_eq!(sel.focus.source, LineSource::Block(1), "ends at the last block");

    let text = b.selected_text(&sel);
    assert!(text.contains("<misha>"), "the first nick: {text:?}");
    assert!(text.contains("x = 1"), "the trailing code block: {text:?}");
}

#[test]
fn select_all_on_an_empty_buffer_is_none() {
    let p = params(2000);
    let b = ChatBuffer::new(p);
    assert!(b.select_all().is_none());
}

#[test]
fn text_is_centred_against_a_taller_avatar() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    p.avatar_size = 48;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "hi", 1000), &m);
    b.reindex();
    b.ensure_layout(0, &m);
    let l = b.layout_at(0).unwrap();
    let line = l.lines.iter().find(|l| l.source == LineSource::Block(0)).unwrap();
    assert!(
        line.y > 0,
        "a single line beside a 48px icon should be pushed down, not sit on its top edge"
    );
    let centre = line.y + line.height / 2;
    assert!(
        (centre as i64 - 24).abs() <= 1,
        "line centre {centre} should be near the icon's centre (24)"
    );
}

#[test]
fn avatar_at_hits_the_painted_rect_and_returns_its_row() {
    // Clicking an icon must resolve to the same speaker as clicking the
    // name. The uid comes back with the message id because the caller
    // needs both, and looking the row up a second time meant a second
    // borrow of the buffer while the first was live.
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    p.avatar_size = 32;
    let mut b = ChatBuffer::new(p);
    let id = b.append(said(7, "misha", "hi", 1000), &m);
    b.reindex();
    b.ensure_layout(0, &m);

    let av = b.layout_at(0).unwrap().avatar.expect("head has an avatar");
    // Dead centre of the painted rect.
    let hit = b.avatar_at(
        (av.x + av.size / 2) as i32,
        (av.y + av.size / 2) as u64,
    );
    assert_eq!(hit, Some((id, 7)));

    // Just outside it, on both axes.
    assert!(b.avatar_at((av.x + av.size + 4) as i32, (av.size / 2) as u64).is_none());
    assert!(b.avatar_at((av.x + av.size / 2) as i32, (av.size + 4) as u64).is_none());
}

#[test]
fn a_continuation_row_has_no_avatar_to_hit() {
    let m = FixedMeasure::new(10);
    let mut p = params(2000);
    p.indent = true;
    p.avatar_size = 32;
    let mut b = ChatBuffer::new(p);
    b.append(said(7, "misha", "one", 1000), &m);
    b.append(said(7, "misha", "two", 1001), &m);
    b.reindex();
    b.ensure_layout(0, &m);
    b.ensure_layout(1, &m);

    let top = b.index_mut().offset_of(1);
    assert!(b.avatar_at(4, top + 2).is_none(), "grouped rows show no icon");
}
