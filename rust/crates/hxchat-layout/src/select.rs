//! Selection: hit-testing, the selection range, and text extraction.
//!
//! Selection is a *document* position pair, not a pixel pair. A
//! [`Caret`] names `(message, source-within-message, byte offset)`, so
//! the selection survives everything that moves pixels around — a
//! resize re-wraps, a zoom rescales, an image finishes decoding and
//! grows its row, a history batch prepends above. xtext stored
//! `mark_start` / `mark_end` as byte offsets on each `textentry` plus
//! `select_start_x/y` in widget coordinates, which is why its selection
//! behaves oddly across a resize mid-drag.
//!
//! The engine deliberately owns none of the *interaction* — no notion of
//! button state, drag, or double-click. It answers three questions and
//! the widget does the rest:
//!
//! - where in the document is this pixel? ([`ChatBuffer::hit_test`])
//! - what does the region between two carets look like? ([`Selection`])
//! - what text does that region contain? ([`ChatBuffer::selected_text`])

use crate::message::MessageId;
use crate::wrap::LineSource;

/// A document position.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Caret {
    pub message: MessageId,
    /// Which of the message's texts the offset indexes.
    pub source: LineSource,
    /// Byte offset into that text. Always on a `char` boundary.
    pub offset: usize,
}

/// An ordered selection range.
///
/// `anchor` is where the drag started and `focus` where it is now;
/// either may be the earlier one in document order, which is what makes
/// dragging upwards work.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Selection {
    pub anchor: Caret,
    pub focus: Caret,
}

/// How far a selection extends within one row.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RowSelection {
    /// Not selected at all.
    None,
    /// Every source in the row is fully selected.
    All,
    /// Part of one source is selected, `start..end` bytes.
    Partial {
        source: LineSource,
        start: usize,
        end: usize,
    },
}

impl Selection {
    pub fn new(anchor: Caret, focus: Caret) -> Selection {
        Selection { anchor, focus }
    }

    pub fn is_empty(&self) -> bool {
        self.anchor == self.focus
    }

    /// `(earlier, later)` in document order, given a row resolver.
    ///
    /// Ordering needs row positions because `MessageId`s are allocated
    /// in creation order, not document order — a chat-history batch
    /// inserts *older* messages with *higher* ids. Comparing ids
    /// directly would invert the selection across a backfill, which is
    /// the kind of bug that only shows up after someone clicks "load
    /// older".
    pub fn ordered(&self, row_of: impl Fn(MessageId) -> Option<usize>) -> (Caret, Caret) {
        let a = row_of(self.anchor.message);
        let f = row_of(self.focus.message);
        let anchor_first = match (a, f) {
            (Some(ar), Some(fr)) if ar != fr => ar < fr,
            _ => self.anchor.offset <= self.focus.offset,
        };
        if anchor_first {
            (self.anchor, self.focus)
        } else {
            (self.focus, self.anchor)
        }
    }
}
