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
    pub fn set_image_size(
        &mut self,
        id: MessageId,
        token: u32,
        size: crate::message::ImageSize,
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
                    *s = Some(size);
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
    pub fn ensure_visible(
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
