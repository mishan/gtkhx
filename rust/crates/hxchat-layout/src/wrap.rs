//! Line breaking and per-message layout.
//!
//! Produces a [`LayoutCache`] for one message at one width: its total
//! pixel height and the line boxes needed for hit-testing. This is where
//! the two-column indent layout lives — the timestamp/nick gutter on the
//! left, the body indented past it — which is the shape GtkHx has always
//! rendered and which C1..C5 must reproduce exactly.
//!
//! Measurement goes through [`TextMeasure`], one call per *run*, never
//! per character. xtext's `find_next_wrap` (xtext.c:3685) measured
//! character by character through a full Pango layout round trip each
//! time; that is the single hottest thing in its append path and the
//! reason a resize of a large buffer visibly hitches.

use crate::measure::TextMeasure;
use crate::message::{Block, Message, MessageFlags};
use crate::span::{ParsedText, Span, Style};
use std::ops::Range;

/// Identifies the conditions a [`LayoutCache`] was computed under.
///
/// A cache is valid only for the generation it was built at; anything
/// that changes rendered geometry bumps a field. Critically, bumping a
/// generation does **not** trigger recomputation — caches are rebuilt
/// lazily when a row is next laid out, so a resize costs O(visible).
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct LayoutGeneration {
    /// Content width in pixels.
    pub width: u32,
    /// Bumped on font change.
    pub font: u32,
    /// Bumped on theme change (palette resolution can alter nothing
    /// geometric, but a theme may carry a font).
    pub theme: u32,
    /// Zoom level in per-mille, so the generation stays `Eq`-comparable.
    /// See docs/chat-view-scoping.md §3.7.
    pub zoom_permille: u32,
}

/// One visual line within a message.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct LineBox {
    /// Y offset from the top of the message.
    pub y: u32,
    pub height: u32,
    /// Byte range within the owning block's text.
    pub range: Range<usize>,
    /// Which block of the message this line came from.
    pub block: usize,
    /// X offset — the indent column, or 0 for full-width content.
    pub x: u32,
}

/// What a laid-out message knows about itself.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct LayoutCache {
    pub generation: LayoutGeneration,
    pub height: u32,
    pub lines: Vec<LineBox>,
    /// Width the gutter/nick column actually needed, before clamping to
    /// [`LayoutParams::max_indent`].
    pub natural_indent: u32,
}

/// Geometry knobs, supplied by the view.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct LayoutParams {
    /// Total content width available.
    pub width: u32,
    /// Two-column mode: body indented past a nick/timestamp gutter.
    pub indent: bool,
    /// Cap on the gutter width. `chat.c` uses 256 px; below that the
    /// gutter grows to fit the widest nick seen, which is what
    /// `gtk_xtext_fix_indent` did.
    pub max_indent: u32,
    /// Current gutter width, shared across the buffer so columns line
    /// up between rows.
    pub indent_width: u32,
    /// Left padding inside a quote block, per nesting level.
    pub quote_indent: u32,
    /// Vertical padding above and below an image or code block.
    pub block_padding: u32,
    pub word_wrap: bool,
}

impl Default for LayoutParams {
    fn default() -> Self {
        LayoutParams {
            width: 640,
            indent: true,
            max_indent: 256,
            indent_width: 0,
            quote_indent: 12,
            block_padding: 2,
            word_wrap: true,
        }
    }
}

/// Lay one message out.
pub fn layout_message(
    msg: &Message,
    params: &LayoutParams,
    generation: LayoutGeneration,
    measure: &dyn TextMeasure,
) -> LayoutCache {
    let metrics = measure.metrics();
    let line_h = metrics.line_height.max(1);
    let mut lines = Vec::new();
    let mut y = 0u32;

    // The gutter is only as wide as this message needs; the buffer
    // reconciles the maximum across rows and re-lays out when it grows.
    let natural_indent = if params.indent {
        msg.speaker
            .as_ref()
            .map(|s| measure.run_width(&s.nick, Style::default()) + metrics.space_width * 2)
            .unwrap_or(0)
    } else {
        0
    };

    let body_x = if params.indent { params.indent_width } else { 0 };
    let body_width = params.width.saturating_sub(body_x).max(line_h);

    for (bi, block) in msg.blocks.iter().enumerate() {
        match block {
            Block::Text(p) => {
                let n = wrap_text(p, body_width, params.word_wrap, measure, |range| {
                    lines.push(LineBox {
                        y,
                        height: line_h,
                        range,
                        block: bi,
                        x: body_x,
                    });
                    y += line_h;
                });
                // An empty block still occupies a line, so a blank
                // message doesn't collapse to zero height and become
                // unclickable.
                if n == 0 {
                    lines.push(LineBox {
                        y,
                        height: line_h,
                        range: 0..0,
                        block: bi,
                        x: body_x,
                    });
                    y += line_h;
                }
            }
            Block::Code { text, .. } => {
                y += params.block_padding;
                // Code never wraps on words — breaking a token would
                // change what it says. Overflow clips; the view offers
                // horizontal scroll in C4.
                let mut start = 0usize;
                for (i, _) in text.match_indices('\n') {
                    lines.push(LineBox {
                        y,
                        height: line_h,
                        range: start..i,
                        block: bi,
                        x: body_x,
                    });
                    y += line_h;
                    start = i + 1;
                }
                lines.push(LineBox {
                    y,
                    height: line_h,
                    range: start..text.len(),
                    block: bi,
                    x: body_x,
                });
                y += line_h + params.block_padding;
            }
            Block::Quote { content, depth } => {
                let qx = body_x + params.quote_indent * u32::from(*depth).max(1);
                let qw = params.width.saturating_sub(qx).max(line_h);
                let n = wrap_text(content, qw, params.word_wrap, measure, |range| {
                    lines.push(LineBox {
                        y,
                        height: line_h,
                        range,
                        block: bi,
                        x: qx,
                    });
                    y += line_h;
                });
                if n == 0 {
                    y += line_h;
                }
            }
            Block::Image { size, alt, .. } => match size {
                // The decode has landed: a real pixel height, not
                // `ceil(h / fontsize)` blank text rows with a texture
                // painted across them (xtext.c:4345). Selection,
                // hit-testing and the marker line all just work.
                Some(sz) => {
                    y += params.block_padding;
                    let (_, h) = measure.image_size((sz.width, sz.height), body_width);
                    lines.push(LineBox {
                        y,
                        height: h,
                        range: 0..alt.len(),
                        block: bi,
                        x: body_x,
                    });
                    y += h + params.block_padding;
                }
                // Not decoded yet — measure as the placeholder text, so
                // the row doesn't jump when the bytes arrive any more
                // than it has to.
                None => {
                    let p = ParsedText::plain(alt.clone());
                    let n = wrap_text(&p, body_width, params.word_wrap, measure, |range| {
                        lines.push(LineBox {
                            y,
                            height: line_h,
                            range,
                            block: bi,
                            x: body_x,
                        });
                        y += line_h;
                    });
                    if n == 0 {
                        y += line_h;
                    }
                }
            },
        }
    }

    if lines.is_empty() {
        y = line_h;
    }

    LayoutCache {
        generation,
        height: y.max(line_h),
        lines,
        natural_indent: natural_indent.min(params.max_indent),
    }
}

/// Break `p` into visual lines, calling `emit` per line. Returns the
/// count.
fn wrap_text(
    p: &ParsedText,
    width: u32,
    word_wrap: bool,
    measure: &dyn TextMeasure,
    mut emit: impl FnMut(Range<usize>),
) -> usize {
    if p.text.is_empty() {
        return 0;
    }
    let mut count = 0usize;
    let mut line_start = 0usize;

    while line_start < p.text.len() {
        // A hard newline always breaks.
        let hard = p.text[line_start..]
            .find('\n')
            .map(|i| line_start + i)
            .unwrap_or(p.text.len());
        let segment = &p.text[line_start..hard];

        if segment.is_empty() {
            emit(line_start..line_start);
            count += 1;
            line_start = hard + 1;
            continue;
        }

        let mut seg_start = line_start;
        loop {
            let rest = &p.text[seg_start..hard];
            if rest.is_empty() {
                break;
            }
            let w = measure_styled(p, seg_start..hard, measure);
            if w <= width {
                emit(seg_start..hard);
                count += 1;
                break;
            }
            // Doesn't fit: find the break point.
            let (fit, _) = measure.fit_prefix(rest, style_at_start(p, seg_start), width);
            let mut brk = seg_start + fit.max(1);
            if word_wrap {
                if let Some(sp) = p.text[seg_start..brk].rfind([' ', '\t']) {
                    if sp > 0 {
                        brk = seg_start + sp;
                    }
                }
            }
            while brk < p.text.len() && !p.text.is_char_boundary(brk) {
                brk += 1;
            }
            if brk <= seg_start {
                brk = next_boundary(&p.text, seg_start);
            }
            emit(seg_start..brk);
            count += 1;
            // Swallow the space we broke on.
            seg_start = brk;
            while seg_start < hard && p.text.as_bytes()[seg_start] == b' ' {
                seg_start += 1;
            }
        }
        line_start = hard + 1;
    }
    count
}

/// Total width of a byte range, measured one span at a time.
///
/// The per-run granularity is the point: a 200-character message with
/// two bold words costs three measure calls, not two hundred.
fn measure_styled(p: &ParsedText, range: Range<usize>, measure: &dyn TextMeasure) -> u32 {
    let mut total = 0u32;
    let mut cursor = range.start;
    for s in &p.spans {
        if s.range.end <= cursor || s.range.start >= range.end {
            continue;
        }
        if s.range.start > cursor {
            total += measure.run_width(&p.text[cursor..s.range.start], Style::default());
            cursor = s.range.start;
        }
        let end = s.range.end.min(range.end);
        if end > cursor {
            total += measure.run_width(&p.text[cursor..end], s.style);
            cursor = end;
        }
    }
    if cursor < range.end {
        total += measure.run_width(&p.text[cursor..range.end], Style::default());
    }
    total
}

fn style_at_start(p: &ParsedText, at: usize) -> Style {
    p.style_at(at)
}

fn next_boundary(s: &str, from: usize) -> usize {
    let mut i = from + 1;
    while i < s.len() && !s.is_char_boundary(i) {
        i += 1;
    }
    i.min(s.len())
}

/// Rough height for a row that has never been laid out.
///
/// Feeds the estimated heights in [`crate::index`]. Deliberately crude —
/// it only has to keep the scrollbar plausible until the row is actually
/// measured, and being fast matters more than being close, since it runs
/// for every row in the buffer on a resize.
pub fn estimate_height(msg: &Message, params: &LayoutParams, measure: &dyn TextMeasure) -> u32 {
    let metrics = measure.metrics();
    let line_h = metrics.line_height.max(1);
    let cols = (params.width / metrics.space_width.max(1)).max(1) as usize;
    let mut lines = 0usize;
    let mut extra = 0u32;

    for b in &msg.blocks {
        match b {
            Block::Text(p) => lines += p.text.len().div_ceil(cols).max(1),
            Block::Code { text, .. } => lines += text.lines().count().max(1),
            Block::Quote { content, .. } => lines += content.text.len().div_ceil(cols).max(1),
            Block::Image { size, alt, .. } => match size {
                Some(sz) => {
                    let (_, h) = measure.image_size((sz.width, sz.height), params.width);
                    extra += h + params.block_padding * 2;
                }
                None => lines += alt.len().div_ceil(cols).max(1),
            },
        }
    }
    (lines.max(1) as u32) * line_h + extra
}

/// Whether a message draws in the muted secondary colour.
pub fn is_muted(msg: &Message) -> bool {
    msg.flags.contains(MessageFlags::MUTED) || msg.flags.contains(MessageFlags::DELETED)
}

/// Spans of `p` clipped to `range`, for the view's snapshot pass.
pub fn spans_in(p: &ParsedText, range: Range<usize>) -> Vec<Span> {
    p.spans
        .iter()
        .filter(|s| s.range.start < range.end && s.range.end > range.start)
        .map(|s| Span {
            range: s.range.start.max(range.start)..s.range.end.min(range.end),
            style: s.style,
        })
        .collect()
}
