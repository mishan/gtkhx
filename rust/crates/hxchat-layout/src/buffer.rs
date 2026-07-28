//! The buffer: rows, marks, lazy layout, trim.
//!
//! Owns the message list, the [`HeightIndex`] that shadows it, and the
//! [`ScrollAnchor`]. This is the object the view (C2) drives.
//!
//! The API mirrors what `chat_view.h` already exposes to C — append,
//! insert-before-a-mark, remove-a-mark, clear, trim — so the C2 widget's
//! FFI layer is a translation rather than a redesign. The difference is
//! that a mark here is a [`MessageId`], not a pointer into a linked
//! list, so a stale one is inert rather than dangling.

use crate::anchor::{AnchorResolver, Gravity, ScrollAnchor};
use crate::index::HeightIndex;
use crate::measure::TextMeasure;
use crate::message::{Block, Message, MessageId};
use crate::select::{Caret, RowSelection, Selection};
use crate::span::Style;
use crate::wrap::LineSource;
use crate::wrap::{estimate_height, layout_message, LayoutCache, LayoutGeneration, LayoutParams};
use std::collections::HashMap;
use std::collections::VecDeque;

#[derive(Debug)]
struct Row {
    id: MessageId,
    msg: Message,
    layout: Option<LayoutCache>,
}

/// A scrollable list of laid-out messages.
#[derive(Debug)]
pub struct ChatBuffer {
    rows: VecDeque<Row>,
    index: HeightIndex,
    next_id: u64,

    /// `MessageId` → row position.
    ///
    /// Rebuilt lazily: any structural change that shifts positions sets
    /// `pos_dirty`, and the next lookup pays one O(n) rebuild and then
    /// serves O(1) until the next change. A history batch is one
    /// rebuild for the batch rather than one per row, which is the case
    /// that actually matters. (xtext did a full linear scan *per*
    /// lookup — `gtk_xtext_find_media_entry_by_token`, xtext.c:6386 —
    /// so even the unamortised path is no worse.)
    pos: HashMap<MessageId, usize>,
    pos_dirty: bool,

    params: LayoutParams,
    generation: LayoutGeneration,
    anchor: ScrollAnchor,

    /// Scrollback cap. 0 disables trimming.
    max_rows: usize,
    /// Widest gutter any row has asked for, so columns align.
    indent_width: u32,
}

impl ChatBuffer {
    pub fn new(params: LayoutParams) -> ChatBuffer {
        let generation = LayoutGeneration {
            width: params.width,
            font: 0,
            theme: 0,
            zoom_permille: 1000,
        };
        ChatBuffer {
            rows: VecDeque::new(),
            index: HeightIndex::new(),
            next_id: 1,
            pos: HashMap::new(),
            pos_dirty: false,
            params,
            generation,
            anchor: ScrollAnchor::bottom(),
            max_rows: 0,
            indent_width: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.rows.len()
    }

    pub fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }

    pub fn anchor(&self) -> ScrollAnchor {
        self.anchor
    }

    pub fn set_anchor(&mut self, a: ScrollAnchor) {
        self.anchor = a;
    }

    pub fn params(&self) -> &LayoutParams {
        &self.params
    }

    pub fn generation(&self) -> LayoutGeneration {
        self.generation
    }

    /// Scrollback cap, in rows. Matches `hx_chat_view_set_max_lines`.
    pub fn set_max_rows(&mut self, n: usize) {
        self.max_rows = n;
        self.trim();
    }

    pub fn message(&self, id: MessageId) -> Option<&Message> {
        self.row_of(id).and_then(|r| self.rows.get(r)).map(|r| &r.msg)
    }

    pub fn message_at(&self, row: usize) -> Option<&Message> {
        self.rows.get(row).map(|r| &r.msg)
    }

    pub fn id_at(&self, row: usize) -> Option<MessageId> {
        self.rows.get(row).map(|r| r.id)
    }

    /// Row position of `id`, or `None` if it has been trimmed/cleared.
    pub fn row_of(&self, id: MessageId) -> Option<usize> {
        if self.pos_dirty {
            // Cold path: the caller holds `&self`, so rebuild by scan
            // rather than mutating. Callers that do this in a loop
            // should take `&mut self` and call `reindex` first.
            return self.rows.iter().position(|r| r.id == id);
        }
        self.pos.get(&id).copied()
    }

    /// Rebuild the id → position map if stale.
    pub fn reindex(&mut self) {
        if !self.pos_dirty {
            return;
        }
        self.pos.clear();
        for (i, r) in self.rows.iter().enumerate() {
            self.pos.insert(r.id, i);
        }
        self.pos_dirty = false;
    }

    // ---- mutation --------------------------------------------------

    /// Append a message; returns its mark.
    pub fn append(&mut self, msg: Message, measure: &dyn TextMeasure) -> MessageId {
        let id = self.alloc_id();
        let h = estimate_height(&msg, &self.params, measure);
        self.rows.push_back(Row {
            id,
            msg,
            layout: None,
        });
        self.index.push_back(h, false);
        if !self.pos_dirty {
            self.pos.insert(id, self.rows.len() - 1);
        }
        self.trim();
        id
    }

    /// Insert immediately before `anchor`, or at the front if `anchor`
    /// is `None` or stale. Returns the new mark.
    ///
    /// This is the chat-history Load-Older path. Nothing here needs to
    /// compensate the scroll position: the anchor names a row, that row
    /// did not move, so the viewport does not move. xtext had to bump
    /// `pagetop_line` / `pagetop_subline` / `last_pixel_pos` /
    /// `old_value` by the inserted row's subline count to approximate
    /// the same effect (xtext.c:5720).
    pub fn insert_before(
        &mut self,
        anchor: Option<MessageId>,
        msg: Message,
        measure: &dyn TextMeasure,
    ) -> MessageId {
        let at = match anchor {
            Some(a) => {
                self.reindex();
                self.row_of(a).unwrap_or(0)
            }
            None => 0,
        };
        let id = self.alloc_id();
        let h = estimate_height(&msg, &self.params, measure);
        self.rows.insert(
            at,
            Row {
                id,
                msg,
                layout: None,
            },
        );
        self.index.insert(at, h, false);
        self.pos_dirty = true;
        id
    }

    /// Remove the row `id` names. `false` if the mark was already stale,
    /// which is not an error — it is how a caller learns the row was
    /// trimmed.
    pub fn remove(&mut self, id: MessageId) -> bool {
        self.reindex();
        let Some(row) = self.row_of(id) else {
            return false;
        };
        self.rows.remove(row);
        self.index.remove(row);
        self.pos_dirty = true;
        if self.anchor.message == Some(id) {
            // Re-anchor to the row that took its place, so removing the
            // "Load older" sentinel from under the viewport doesn't
            // snap the view to the bottom.
            self.anchor.message = self.rows.get(row).map(|r| r.id);
            if self.anchor.message.is_none() {
                self.anchor = ScrollAnchor::bottom();
            }
        }
        true
    }

    /// Replace a message in place, e.g. to attach a decoded image size.
    pub fn replace(&mut self, id: MessageId, msg: Message, measure: &dyn TextMeasure) -> bool {
        self.reindex();
        let Some(row) = self.row_of(id) else {
            return false;
        };
        let h = estimate_height(&msg, &self.params, measure);
        self.rows[row].msg = msg;
        self.rows[row].layout = None;
        self.index.set_height(row, h, false);
        true
    }

    /// Attach a decoded size to an image block, growing the row.
    ///
    /// The single operation the old design was worst at: xtext had to
    /// recompute the entry's subline list, diff the count, and patch the
    /// buffer's `num_lines` plus the scroll anchors. Here it is a height
    /// change, and the anchor absorbs it.
    /// Attach (or with `None`, clear) the decoded size of an image
    /// block.
    ///
    /// `None` is the decode-failed path: the row must shrink back to its
    /// placeholder height, or the alt text ends up floating inside a
    /// tall empty box the size of an image that never arrived.
    pub fn set_image_size(
        &mut self,
        id: MessageId,
        token: u32,
        size: Option<crate::message::ImageSize>,
        measure: &dyn TextMeasure,
    ) -> bool {
        self.reindex();
        let Some(row) = self.row_of(id) else {
            return false;
        };
        let mut hit = false;
        for b in &mut self.rows[row].msg.blocks {
            if let Block::Image {
                token: t,
                size: s,
                ..
            } = b
            {
                if *t == token {
                    *s = size;
                    hit = true;
                }
            }
        }
        if !hit {
            return false;
        }
        self.rows[row].layout = None;
        let h = estimate_height(&self.rows[row].msg, &self.params, measure);
        self.index.set_height(row, h, false);
        true
    }

    /// Find the row carrying an image block with `token`.
    pub fn find_image(&self, token: u32) -> Option<MessageId> {
        self.rows
            .iter()
            .find(|r| {
                r.msg.blocks.iter().any(|b| {
                    matches!(b, Block::Image { token: t, .. } if *t == token)
                })
            })
            .map(|r| r.id)
    }

    pub fn clear(&mut self) {
        self.rows.clear();
        self.index = HeightIndex::new();
        self.pos.clear();
        self.pos_dirty = false;
        self.anchor = ScrollAnchor::bottom();
        self.indent_width = 0;
    }

    fn trim(&mut self) {
        if self.max_rows == 0 || self.rows.len() <= self.max_rows {
            return;
        }
        let excess = self.rows.len() - self.max_rows;
        for _ in 0..excess {
            self.rows.pop_front();
        }
        self.index.drain_front(excess);
        self.pos_dirty = true;
    }

    fn alloc_id(&mut self) -> MessageId {
        let id = MessageId(self.next_id);
        self.next_id += 1;
        id
    }

    // ---- geometry ---------------------------------------------------

    /// Change the content width. Invalidates layout without recomputing
    /// it — the whole point of the lazy cache.
    pub fn set_width(&mut self, width: u32) {
        if width == self.params.width {
            return;
        }
        self.params.width = width;
        self.generation.width = width;
        self.index.invalidate_all_measurements();
    }

    pub fn set_font_generation(&mut self, font: u32) {
        if font == self.generation.font {
            return;
        }
        self.generation.font = font;
        self.index.invalidate_all_measurements();
    }

    /// Zoom, in per-mille (1000 = 100%). See scoping §3.7.
    pub fn set_zoom_permille(&mut self, zoom: u32) {
        if zoom == self.generation.zoom_permille {
            return;
        }
        self.generation.zoom_permille = zoom;
        self.index.invalidate_all_measurements();
    }

    pub fn set_word_wrap(&mut self, on: bool) {
        if on == self.params.word_wrap {
            return;
        }
        self.params.word_wrap = on;
        self.invalidate_layout();
    }

    /// Two-column mode.
    pub fn set_indent(&mut self, on: bool) {
        if on == self.params.indent {
            return;
        }
        self.params.indent = on;
        self.indent_width = 0;
        self.invalidate_layout();
    }

    /// Width to reserve for the timestamp column; 0 turns it off.
    pub fn set_stamp_width(&mut self, px: u32) {
        if px == self.params.stamp_width {
            return;
        }
        self.params.stamp_width = px;
        // The gutter has to be re-reconciled from scratch: it may need to
        // grow for a wider stamp, and when the stamp goes away it should
        // shrink back rather than stay padded out.
        self.indent_width = 0;
        self.invalidate_layout();
    }

    pub fn stamp_width(&self) -> u32 {
        self.params.stamp_width
    }

    /// Cap on the gutter width.
    pub fn set_max_indent(&mut self, px: u32) {
        if px == self.params.max_indent {
            return;
        }
        self.params.max_indent = px;
        self.indent_width = self.indent_width.min(px);
        self.invalidate_layout();
    }

    /// Drop every cached layout, keeping heights as estimates.
    ///
    /// The generation key can't express "the params changed" — it tracks
    /// width, font, theme and zoom, and a geometry knob like indent mode
    /// is none of those. Rather than widen the key for two setters that
    /// fire once at construction, clear the caches directly. Heights
    /// survive as estimates, so this is still O(1) work now and
    /// O(visible) on the next draw.
    fn invalidate_layout(&mut self) {
        for r in &mut self.rows {
            r.layout = None;
        }
        self.index.invalidate_all_measurements();
    }

    /// Ensure row `row` has a current layout, computing it if not.
    /// Returns its height.
    pub fn ensure_layout(&mut self, row: usize, measure: &dyn TextMeasure) -> u32 {
        let gen = self.generation;
        let mut params = self.params;
        params.indent_width = self.indent_width;

        let Some(r) = self.rows.get(row) else {
            return 0;
        };
        if let Some(l) = &r.layout {
            if l.generation == gen {
                return l.height;
            }
        }
        let layout = layout_message(&self.rows[row].msg, &params, gen, measure);

        // A wider nick than any seen so far widens the shared gutter, so
        // every *other* row's layout is stale. Rare — it settles within
        // the first few messages — and lazily repaired.
        //
        // The row we just laid out has to be redone rather than merely
        // invalidated: it was measured against the old, narrower gutter,
        // and dropping it would leave `layout_at(row)` returning None
        // immediately after an `ensure_layout(row)` call, which is a
        // contract violation the caller has no way to recover from.
        if layout.natural_indent > self.indent_width {
            self.indent_width = layout.natural_indent;
            for r in &mut self.rows {
                r.layout = None;
            }
            self.index.invalidate_all_measurements();
            params.indent_width = self.indent_width;
            let layout = layout_message(&self.rows[row].msg, &params, gen, measure);
            let height = layout.height;
            self.rows[row].layout = Some(layout);
            self.index.set_height(row, height, true);
            return height;
        }

        let height = layout.height;
        self.rows[row].layout = Some(layout);
        self.index.set_height(row, height, true);
        height
    }

    /// Lay out every row intersecting `[y, y + height)` and return their
    /// positions.
    ///
    /// **Every returned row is guaranteed to have a current layout.**
    /// That guarantee needs defending, because laying a row out can
    /// invalidate the rows already done: meeting a wider nick widens the
    /// shared gutter, which makes every other row's cached layout stale.
    /// Without the retry below, the rows laid out earlier in the pass
    /// come back with `layout_at(row) == None`, the view skips them, and
    /// the first paint of a fresh buffer is visibly shredded — while the
    /// next message, by which time the gutter has settled, looks fine.
    ///
    /// The gutter only ever grows and is capped by `max_indent`, so this
    /// converges fast; the bound is belt-and-braces.
    pub fn ensure_visible(
        &mut self,
        y: u64,
        viewport_height: u32,
        measure: &dyn TextMeasure,
    ) -> Vec<usize> {
        const MAX_SETTLE_PASSES: usize = 4;
        for _ in 0..MAX_SETTLE_PASSES {
            let indent_before = self.indent_width;
            let rows = self.layout_visible_once(y, viewport_height, measure);
            if self.indent_width == indent_before {
                return rows;
            }
        }
        // Didn't settle (shouldn't happen: the gutter is monotonic and
        // capped). Do one final pass so the caller still gets laid-out
        // rows rather than holes.
        self.layout_visible_once(y, viewport_height, measure)
    }

    fn layout_visible_once(
        &mut self,
        y: u64,
        viewport_height: u32,
        measure: &dyn TextMeasure,
    ) -> Vec<usize> {
        let mut out = Vec::new();
        let Some(first) = self.index.locate(y) else {
            return out;
        };
        let mut row = first.row;
        let mut consumed = u64::from(self.index.height_at(row)) - u64::from(first.offset);
        self.ensure_layout(row, measure);
        out.push(row);
        while consumed < u64::from(viewport_height) && row + 1 < self.rows.len() {
            row += 1;
            self.ensure_layout(row, measure);
            out.push(row);
            consumed += u64::from(self.index.height_at(row));
        }
        out
    }

    /// Text of one of a message's sources, for hit-testing and copying.
    pub fn source_text(&self, row: usize, source: LineSource) -> Option<&str> {
        self.source_text_for(&self.rows.get(row)?.msg, source)
    }

    fn source_text_for<'a>(&self, msg: &'a Message, source: LineSource) -> Option<&'a str> {
        match source {
            LineSource::Gutter => msg.gutter.as_ref().map(|g| g.text.as_str()),
            LineSource::Block(bi) => match msg.blocks.get(bi)? {
                Block::Text(p) => Some(p.text.as_str()),
                Block::Quote { content, .. } => Some(content.text.as_str()),
                Block::Code { text, .. } => Some(text.as_str()),
                // An image contributes its alt text, so a selection
                // dragged across a picture copies something meaningful
                // instead of nothing.
                Block::Image { alt, .. } => Some(alt.as_str()),
            },
        }
    }

    /// The link under a caret, as (href, visible text).
    ///
    /// Returned by value rather than by reference because the caller is
    /// the widget, which needs to hold it across a popup.
    pub fn link_at(&self, caret: &Caret) -> Option<(String, String)> {
        let row = self.row_of(caret.message)?;
        let msg = &self.rows.get(row)?.msg;
        let parsed = match caret.source {
            LineSource::Gutter => msg.gutter.as_ref()?,
            LineSource::Block(bi) => match msg.blocks.get(bi)? {
                Block::Text(p) => p,
                Block::Quote { content, .. } => content,
                _ => return None,
            },
        };
        let link = parsed.link_at(caret.offset)?;
        let label = parsed
            .text
            .get(link.range.clone())
            .unwrap_or_default()
            .to_string();
        Some((link.href.clone(), label))
    }

    /// The whitespace-delimited word around a caret.
    ///
    /// Tokenised exactly like xtext's `is_del` macro (xtext.c:239):
    /// space, newline, `<`, `>` and NUL. Matching it byte-for-byte is
    /// the whole point — the existing C handlers in `chat.c` recognise
    /// their targets by *string*, and the angle brackets are what let
    /// `<nick>` split into a bare nick. Tokenise differently and
    /// `inline_media_chat_word_click` stops finding `hxmedia:N`, and the
    /// chat-history sentinel stops matching.
    pub fn word_at(&self, caret: &Caret) -> Option<String> {
        let (start, end) = self.word_bounds(caret)?;
        let row = self.row_of(caret.message)?;
        let text = self.source_text(row, caret.source)?;
        Some(text[start..end].to_string())
    }

    /// Byte bounds of the word around a caret, in its own source.
    ///
    /// Shared by `word_at` (which the `word-click` emission needs as a
    /// string) and double-click word-select, so the two can never
    /// disagree about where a word begins.
    pub fn word_bounds(&self, caret: &Caret) -> Option<(usize, usize)> {
        let row = self.row_of(caret.message)?;
        let text = self.source_text(row, caret.source)?;
        if text.is_empty() {
            return None;
        }
        let is_del = |c: char| c == ' ' || c == '\n' || c == '<' || c == '>' || c == '\0';
        // Clamp to a char boundary before slicing. Carets from
        // `hit_test` are already aligned, but this is reachable from the
        // FFI with an arbitrary offset, and `&text[at..]` off a boundary
        // panics — which unwinds into GTK and aborts.
        let mut at = caret.offset.min(text.len());
        while at > 0 && !text.is_char_boundary(at) {
            at -= 1;
        }

        // A click *on* a delimiter yields no word, which is what xtext
        // does: both its scan loops (xtext.c:2095, 2114) test `is_del`
        // before stepping, so they collapse to an empty span. Returning
        // the preceding word instead would make clicking the gap after a
        // link activate it.
        match text[at..].chars().next() {
            Some(c) if !is_del(c) => {}
            _ => return None,
        }

        let start = text[..at]
            .rfind(is_del)
            .map(|i| i + text[i..].chars().next().map_or(1, |c| c.len_utf8()))
            .unwrap_or(0);
        let end = text[at..]
            .find(is_del)
            .map(|i| at + i)
            .unwrap_or(text.len());
        if start >= end {
            return None;
        }
        Some((start, end))
    }

    /// Double-click: select the word under the caret.
    pub fn select_word(&self, caret: &Caret) -> Option<Selection> {
        let (start, end) = self.word_bounds(caret)?;
        Some(Selection::new(
            Caret {
                message: caret.message,
                source: caret.source,
                offset: start,
            },
            Caret {
                message: caret.message,
                source: caret.source,
                offset: end,
            },
        ))
    }

    /// Triple-click: select the whole row, gutter included.
    ///
    /// The gutter is part of what the user sees on that line, so it is
    /// part of what a "select this line" gesture should give them —
    /// consistent with a whole-row selection dragged from above.
    pub fn select_row(&self, row: usize) -> Option<Selection> {
        let id = self.id_at(row)?;
        let first = self.sources_of(row).first().copied()?;
        let last = self.sources_of(row).last().copied()?;
        let end_len = self.source_text(row, last).map_or(0, |t| t.len());
        Some(Selection::new(
            Caret {
                message: id,
                source: first,
                offset: 0,
            },
            Caret {
                message: id,
                source: last,
                offset: end_len,
            },
        ))
    }

    /// The settled gutter width. 0 when not in indent mode.
    pub fn indent_width(&self) -> u32 {
        self.indent_width
    }

    /// Map a content-space pixel to a document position.
    ///
    /// `x` / `y` are in content coordinates (the view subtracts its own
    /// padding first). `y` is absolute within the buffer, not
    /// viewport-relative.
    ///
    /// Returns `None` only for an empty buffer; a point past the end
    /// clamps to the last row, because a drag that runs off the bottom
    /// should select to the end rather than stop tracking.
    pub fn hit_test(
        &mut self,
        x: i32,
        y: u64,
        measure: &dyn TextMeasure,
    ) -> Option<Caret> {
        let hit = self.index.locate(y)?;
        let row = hit.row;
        self.ensure_layout(row, measure);
        let id = self.id_at(row)?;
        let layout = self.layout_at(row)?;

        // The line whose band contains the point.
        //
        // Several boxes can share a y band — the gutter and the first
        // body line always do — so x has to break the tie, otherwise
        // every click on row 0 resolves to whichever was pushed first
        // and the other becomes unselectable.
        let in_band: Vec<&crate::wrap::LineBox> = layout
            .lines
            .iter()
            .filter(|l| hit.offset >= l.y && hit.offset < l.y + l.height)
            .collect();
        let line = in_band
            .iter()
            .copied()
            .find(|l| {
                let w = self
                    .source_text_for(&self.rows[row].msg, l.source)
                    .and_then(|t| t.get(l.range.clone()))
                    .map(|slice| measure.run_width(slice, Style::default()))
                    .unwrap_or(0);
                x >= l.x as i32 && x < (l.x + w) as i32
            })
            // No box owns the x: fall back to the nearest by distance,
            // so a click in the gap between gutter and body still picks
            // something sensible rather than nothing.
            .or_else(|| {
                in_band.iter().copied().min_by_key(|l| {
                    (x - l.x as i32).abs()
                })
            })
            .or_else(|| {
                if hit.offset < layout.lines.first().map_or(0, |l| l.y) {
                    layout.lines.first()
                } else {
                    layout.lines.last()
                }
            })?;

        let source = line.source;
        let range = line.range.clone();
        let line_x = line.x as i32;
        let text = self.source_text(row, source)?;
        let slice = text.get(range.clone()).unwrap_or("");

        // Within the line, find the byte the x lands on. Left of the
        // line start selects from its beginning; right of the end
        // selects through it, which is what makes dragging past the end
        // of a short line feel right.
        let rel = x - line_x;
        let offset = if rel <= 0 {
            range.start
        } else {
            let (fit, _) = measure.fit_prefix(slice, Style::default(), rel as u32);
            (range.start + fit).min(range.end)
        };

        Some(Caret {
            message: id,
            source,
            offset,
        })
    }

    /// How much of `row` a selection covers.
    pub fn row_selection(&self, row: usize, sel: &Selection) -> RowSelection {
        if sel.is_empty() {
            return RowSelection::None;
        }
        let (start, end) = sel.ordered(|id| self.row_of(id));
        let (Some(sr), Some(er)) = (self.row_of(start.message), self.row_of(end.message)) else {
            return RowSelection::None;
        };
        if row < sr || row > er {
            return RowSelection::None;
        }
        if row > sr && row < er {
            return RowSelection::All;
        }
        // A boundary row. The covered span can begin in one source and
        // end in another (gutter → body), so both endpoints are carried
        // and the caller resolves per source.
        let last = self.last_source(row);
        let (from, to) = if sr == er {
            ((start.source, start.offset), (end.source, end.offset))
        } else if row == sr {
            // Starts here, runs to the end of the row.
            (
                (start.source, start.offset),
                (last, self.source_text(row, last).map_or(0, |t| t.len())),
            )
        } else {
            // Ends here, having begun above.
            ((self.first_source(row), 0), (end.source, end.offset))
        };
        RowSelection::Partial {
            start: from,
            end: to,
        }
    }

    /// The row's sources in visual order: gutter first, then blocks.
    pub fn sources_of(&self, row: usize) -> Vec<LineSource> {
        let mut out = Vec::new();
        let Some(msg) = self.message_at(row) else {
            return out;
        };
        if msg.gutter.as_ref().is_some_and(|g| !g.text.is_empty()) {
            out.push(LineSource::Gutter);
        }
        for i in 0..msg.blocks.len() {
            out.push(LineSource::Block(i));
        }
        out
    }

    fn first_source(&self, row: usize) -> LineSource {
        self.sources_of(row)
            .first()
            .copied()
            .unwrap_or(LineSource::Block(0))
    }

    fn last_source(&self, row: usize) -> LineSource {
        self.sources_of(row)
            .last()
            .copied()
            .unwrap_or(LineSource::Block(0))
    }

    /// The byte range of `source` covered by a row selection, if any.
    ///
    /// The one place that resolves a possibly-cross-source `Partial`
    /// against a single text, so the view and `selected_text` cannot
    /// disagree about what is highlighted versus what gets copied.
    pub fn covered_range(
        &self,
        row: usize,
        source: LineSource,
        sel: &RowSelection,
    ) -> Option<std::ops::Range<usize>> {
        let len = self.source_text(row, source).map_or(0, |t| t.len());
        match sel {
            RowSelection::None => None,
            RowSelection::All => Some(0..len),
            RowSelection::Partial { start, end } => {
                let rank = crate::select::source_rank(source);
                let (sr, so) = *start;
                let (er, eo) = *end;
                let (sr, er) = (crate::select::source_rank(sr), crate::select::source_rank(er));
                if rank < sr || rank > er {
                    return None;
                }
                let from = if rank == sr { so.min(len) } else { 0 };
                let to = if rank == er { eo.min(len) } else { len };
                if from < to {
                    Some(from..to)
                } else {
                    None
                }
            }
        }
    }

    /// The selected text of each covered row, as `(row, text)`.
    ///
    /// Exposed per row because a row's own text may contain hard
    /// newlines — the wrap engine supports them — so a caller cannot
    /// recover row boundaries by splitting the joined string. The
    /// autocopy-timestamp path needs exactly that: it prefixes each
    /// *row* with that row's stamp, and iterating `lines()` over the
    /// joined output drifts onto the wrong rows the moment any selected
    /// message spans more than one line.
    pub fn selected_rows(&self, sel: &Selection) -> Vec<(usize, String)> {
        if sel.is_empty() {
            return Vec::new();
        }
        let (start, end) = sel.ordered(|id| self.row_of(id));
        let (Some(sr), Some(er)) = (self.row_of(start.message), self.row_of(end.message)) else {
            return Vec::new();
        };
        let mut out = Vec::new();
        for row in sr..=er {
            // Walk the row's sources in visual order and take whatever
            // each contributes. One path for every case, so the gutter
            // is copied when it is selected and only then.
            let rsel = self.row_selection(row, sel);
            let mut text = String::new();
            let mut prev: Option<LineSource> = None;
            for source in self.sources_of(row) {
                let Some(range) = self.covered_range(row, source, &rsel) else {
                    continue;
                };
                let Some(t) = self.source_text(row, source) else {
                    continue;
                };
                let Some(slice) = t.get(range) else { continue };
                if slice.is_empty() {
                    continue;
                }
                // Separator matches Message::to_plain_text, which is
                // what whole-row copying used before this path existed:
                // a space between the gutter and the body (they read as
                // one line), a newline between body blocks (they are
                // distinct blocks and collapsing them to spaces would
                // run a code block into the paragraph after it).
                match prev {
                    None => {}
                    Some(LineSource::Gutter) => text.push(' '),
                    Some(LineSource::Block(_)) => text.push('\n'),
                }
                text.push_str(slice);
                prev = Some(source);
            }
            out.push((row, text));
        }
        out
    }

    /// The selected text, rows joined by newlines.
    pub fn selected_text(&self, sel: &Selection) -> String {
        self.selected_rows(sel)
            .into_iter()
            .map(|(_, t)| t)
            .collect::<Vec<_>>()
            .join("\n")
    }

    pub fn layout_at(&self, row: usize) -> Option<&LayoutCache> {
        self.rows.get(row).and_then(|r| r.layout.as_ref())
    }

    pub fn total_height(&mut self) -> u64 {
        self.index.total_height()
    }

    pub fn index_mut(&mut self) -> &mut HeightIndex {
        &mut self.index
    }

    /// The scroll adjustment value the current anchor implies.
    pub fn scroll_offset(&mut self, viewport_height: u32) -> u64 {
        self.reindex();
        let pos = std::mem::take(&mut self.pos);
        let v = AnchorResolver::to_pixels(&self.anchor, &mut self.index, viewport_height, |id| {
            pos.get(&id).copied()
        });
        self.pos = pos;
        v
    }

    /// Re-anchor from a pixel position — a scrollbar drag.
    pub fn scroll_to(&mut self, y: u64, viewport_height: u32, follow_slop: u32) {
        let ids: Vec<MessageId> = self.rows.iter().map(|r| r.id).collect();
        self.anchor = AnchorResolver::from_pixels(
            y,
            &mut self.index,
            viewport_height,
            follow_slop,
            |row| ids.get(row).copied(),
        );
    }

    /// Pin to the bottom and resume following.
    pub fn scroll_to_bottom(&mut self) {
        self.anchor = ScrollAnchor::bottom();
    }

    pub fn is_following(&self) -> bool {
        self.anchor.gravity == Gravity::Bottom
    }
}
