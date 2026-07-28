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

/// What a [`LineBox`] draws text from.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LineSource {
    /// The left column — the pre-rendered gutter, or the speaker's nick.
    /// Always the message's first line box when present.
    Gutter,
    /// Body block `n`.
    Block(usize),
}

/// One visual line within a message.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct LineBox {
    /// Y offset from the top of the message.
    pub y: u32,
    pub height: u32,
    /// Byte range within the owning source's text.
    pub range: Range<usize>,
    /// Where this line's text comes from.
    pub source: LineSource,
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
    /// Where to paint the speaker's avatar, when there is one to paint.
    ///
    /// Only ever set on a *group head* — a continuation row shows
    /// neither the nick nor the icon, which is the whole point of
    /// grouping. The view resolves the actual texture from the uid at
    /// draw time, because avatars animate and a cached frame would
    /// freeze.
    pub avatar: Option<AvatarBox>,
}

/// A square avatar slot in the gutter, in row-local coordinates.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct AvatarBox {
    pub x: u32,
    pub y: u32,
    pub size: u32,
    pub uid: u16,
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
    /// Width reserved at the left of the gutter for the timestamp
    /// column, or 0 when timestamps are off.
    ///
    /// The layout engine never formats a timestamp — it can't, having no
    /// locale or time formatting — it only reserves the space the view
    /// says it needs, so the gutter is wide enough for stamp + nick.
    pub stamp_width: u32,
    /// Gap between the right edge of the gutter and the body column.
    pub gutter_gap: u32,
    /// Left padding inside a quote block, per nesting level.
    pub quote_indent: u32,
    /// Vertical padding above and below an image or code block.
    pub block_padding: u32,
    pub word_wrap: bool,
    /// Edge length of the avatar slot in the gutter, or 0 for no
    /// avatars. Driven by the Settings toggle.
    pub avatar_size: u32,
}

impl Default for LayoutParams {
    fn default() -> Self {
        LayoutParams {
            width: 640,
            indent: true,
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
    //
    // A pre-rendered gutter (the compat path — see Message::gutter) wins
    // over the speaker's bare nick, because it carries the styling
    // chat.c already applied and the A/B has to match it exactly.
    // A grouped row is a continuation of the one above: same speaker,
    // close in time. Its gutter is suppressed so a burst of messages
    // reads as one block under one name instead of repeating the nick
    // (and, once avatars land, the icon) on every line. The body keeps
    // its indent, so the column stays straight.
    //
    // The flag is set by ChatBuffer, which is the only thing that can
    // see a message's neighbours. It still contributes its *natural*
    // gutter width below, so hiding a nick never narrows the shared
    // column and shifts every other row sideways.
    let grouped = msg.flags.contains(crate::message::MessageFlags::GROUPED);

    // An avatar is only drawn on a group head, but *every* row with a
    // speaker reserves its width — otherwise the shared gutter narrows
    // the moment a run forms and every column in the buffer jumps.
    let avatar_slot = if params.indent && params.avatar_size > 0 {
        match &msg.speaker {
            Some(s) if s.uid != 0 => params.avatar_size + params.gutter_gap,
            _ => 0,
        }
    } else {
        0
    };

    let natural_indent = if params.indent {
        let gutter_text = match (&msg.gutter, &msg.speaker) {
            (Some(g), _) if !g.text.is_empty() => {
                measure_styled(g, 0..g.text.len(), measure) + metrics.space_width
            }
            (_, Some(s)) => {
                measure.run_width(&s.nick, Style::default()) + metrics.space_width * 2
            }
            _ => 0,
        };
        // The gutter holds the timestamp *and* the nick, side by side,
        // so it has to be wide enough for both — reserving only the nick
        // width is what makes a stamp overlap it.
        params.stamp_width + avatar_slot + gutter_text
    } else {
        0
    };

    let body_x = if params.indent { params.indent_width } else { 0 };
    let body_width = params.width.saturating_sub(body_x).max(line_h);

    // The gutter is one unwrapped line, always first, carrying the x it
    // is actually drawn at.
    //
    // Right-aligned against the body column, the way xtext aligns its
    // left text (`ent->indent = buf->indent - left_width - space_width`,
    // xtext.c). Computing it *here* rather than in the view matters: the
    // view previously derived it from `params().indent_width`, which is
    // only ever set on a local copy inside `ensure_layout` and so read
    // back as 0 — the gutter drew hard left while bodies moved right as
    // the column grew, and hit-testing used the same wrong x, which is
    // why nicks could not be selected. One source of truth removes both
    // bugs at once.
    // The avatar sits at the left of the gutter, just past the stamp,
    // with the nick to its right. Group heads only.
    let avatar = if avatar_slot > 0 && !grouped {
        msg.speaker.as_ref().map(|s| AvatarBox {
            x: params.stamp_width,
            y: 0,
            size: params.avatar_size,
            uid: s.uid,
        })
    } else {
        None
    };

    if params.indent && !grouped {
        if let Some(g) = &msg.gutter {
            if !g.text.is_empty() {
                let gw = measure_styled(g, 0..g.text.len(), measure);
                let gx = params
                    .indent_width
                    .saturating_sub(gw + params.gutter_gap)
                    .max(params.stamp_width);
                lines.push(LineBox {
                    y: 0,
                    height: line_h,
                    range: 0..g.text.len(),
                    source: LineSource::Gutter,
                    x: gx,
                });
            }
        }
    }

    for (bi, block) in msg.blocks.iter().enumerate() {
        match block {
            Block::Text(p) => {
                let n = wrap_text(p, body_width, params.word_wrap, measure, |range| {
                    lines.push(LineBox {
                        y,
                        height: line_h,
                        range,
                        source: LineSource::Block(bi),
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
                        source: LineSource::Block(bi),
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
                        source: LineSource::Block(bi),
                        x: body_x,
                    });
                    y += line_h;
                    start = i + 1;
                }
                lines.push(LineBox {
                    y,
                    height: line_h,
                    range: start..text.len(),
                    source: LineSource::Block(bi),
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
                        source: LineSource::Block(bi),
                        x: qx,
                    });
                    y += line_h;
                });
                // An empty quote still gets a line box, for the same
                // reason an empty text block does: a row with no line
                // boxes occupies vertical space that nothing can hit-test
                // or select, which reads as a dead patch in the buffer.
                if n == 0 {
                    lines.push(LineBox {
                        y,
                        height: line_h,
                        range: 0..0,
                        source: LineSource::Block(bi),
                        x: qx,
                    });
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
                        source: LineSource::Block(bi),
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
                            source: LineSource::Block(bi),
                            x: body_x,
                        });
                        y += line_h;
                    });
                    // Same rule: an image whose alt text is empty must
                    // still be clickable — that is how the user opens
                    // the media dialog before the decode lands.
                    if n == 0 {
                        lines.push(LineBox {
                            y,
                            height: line_h,
                            range: 0..0,
                            source: LineSource::Block(bi),
                            x: body_x,
                        });
                        y += line_h;
                    }
                }
            },
        }
    }

    if lines.is_empty() {
        y = line_h;
    }

    // Centre the text against the icon when the icon is the taller of
    // the two.
    //
    // Without this the text sits on the icon's top edge, which reads as
    // misalignment rather than as a design: the eye pairs a 32px icon
    // with the line beside it, and "beside" means centred. Shifting
    // every line box (rather than only the first) keeps a two-line
    // message centred as a block, which is what looks right when the
    // message is still shorter than the icon.
    //
    // Done here, on the finished line boxes, so hit-testing and painting
    // move together — the alternative, offsetting only at draw time, is
    // the class of bug where you can see text you cannot select.
    let text_h = y.max(line_h);
    let row_h = text_h.max(avatar.map(|a| a.size).unwrap_or(0));
    if row_h > text_h {
        let shift = (row_h - text_h) / 2;
        for l in &mut lines {
            l.y += shift;
        }
    }

    LayoutCache {
        generation,
        // A head row must be at least as tall as its avatar, or the
        // icon overflows into the row below. This is the case the
        // "re-estimate on regroup" fix in ChatBuffer exists for: a head
        // and its continuations now genuinely differ in height.
        height: row_h,
        lines,
        natural_indent: natural_indent.min(params.max_indent),
        avatar,
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
            // Doesn't fit: find the break point, honouring every style
            // change between here and the boundary.
            let (fit_abs, _) = fit_styled_prefix(p, seg_start..hard, width, measure);
            let mut brk = fit_abs.max(seg_start + 1);
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

/// Walk `range` as maximal runs of uniform style, without allocating.
///
/// Gaps between spans are default-styled. This is the one place that
/// knows how to walk a `ParsedText`'s style structure, so measuring and
/// break-point search can't drift apart.
///
/// Callback rather than a returned `Vec` because both callers sit on the
/// wrapping hot path — `measure_styled` runs per candidate line and
/// `fit_styled_prefix` per overflowing line, so an allocation each would
/// be a per-line malloc during layout. `f` returning `Some` stops the
/// walk early, which is what lets the prefix search bail at the run that
/// straddles the width boundary.
fn walk_style_runs<R>(
    p: &ParsedText,
    range: Range<usize>,
    mut f: impl FnMut(Range<usize>, Style) -> Option<R>,
) -> Option<R> {
    let mut cursor = range.start;
    for s in &p.spans {
        if s.range.end <= cursor || s.range.start >= range.end {
            continue;
        }
        if s.range.start > cursor {
            if let Some(r) = f(cursor..s.range.start, Style::default()) {
                return Some(r);
            }
            cursor = s.range.start;
        }
        let end = s.range.end.min(range.end);
        if end > cursor {
            if let Some(r) = f(cursor..end, s.style) {
                return Some(r);
            }
            cursor = end;
        }
    }
    if cursor < range.end {
        return f(cursor..range.end, Style::default());
    }
    None
}

/// Total width of a byte range, measured one style run at a time.
///
/// The per-run granularity is the point: a 200-character message with
/// two bold words costs three measure calls, not two hundred.
fn measure_styled(p: &ParsedText, range: Range<usize>, measure: &dyn TextMeasure) -> u32 {
    let mut total = 0u32;
    walk_style_runs::<()>(p, range, |r, style| {
        total += measure.run_width(&p.text[r], style);
        None
    });
    total
}

/// Largest prefix of `range` fitting in `max_width`, honouring every
/// style change inside it.
///
/// The naive version — measure the whole rest with the style in effect
/// at its *start* — is wrong as soon as a run changes style partway
/// through, which `**bold**` and `` `code` `` both do. Bold and
/// monospace are wider than the base font, so a break point chosen from
/// the start style under-measures and the drawn line overflows its box.
/// Walking runs and only calling the measurer's own prefix search on the
/// single run that straddles the boundary is both correct and no more
/// expensive.
fn fit_styled_prefix(
    p: &ParsedText,
    range: Range<usize>,
    max_width: u32,
    measure: &dyn TextMeasure,
) -> (usize, u32) {
    let mut total = 0u32;
    let mut cursor = range.start;
    let straddled = walk_style_runs(p, range, |r, style| {
        let seg = &p.text[r.clone()];
        let w = measure.run_width(seg, style);
        if total + w <= max_width {
            total += w;
            cursor = r.end;
            return None;
        }
        // This run crosses the boundary — only it needs the measurer's
        // own prefix search.
        let (fit, fw) = measure.fit_prefix(seg, style, max_width.saturating_sub(total));
        Some((r.start + fit, total + fw))
    });
    straddled.unwrap_or((cursor, total))
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
