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

/// Visual order of a row's texts.
///
/// A row is not one string: it is a gutter followed by body blocks, and
/// a selection boundary can land in any of them. Ranking them is what
/// lets carets in *different* sources be ordered against each other —
/// comparing their byte offsets, which is what the first version did,
/// is meaningless, because the two offsets index unrelated strings.
pub fn source_rank(s: LineSource) -> usize {
    match s {
        LineSource::Gutter => 0,
        LineSource::Block(i) => i + 1,
    }
}

/// How far a selection extends within one row.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RowSelection {
    /// Not selected at all.
    None,
    /// Every source in the row is fully selected.
    All,
    /// Runs from `start` to `end`, which may be in *different* sources.
    ///
    /// This is the shape the first version got wrong: it could name only
    /// one source, so a boundary landing in the gutter left the rest of
    /// that row unselected. Dragging left and then up — which puts the
    /// start of the selection in a gutter — highlighted the nick and not
    /// the message, which reads as the selection being inverted.
    Partial {
        start: (LineSource, usize),
        end: (LineSource, usize),
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
    /// Ordering is on `(row, source rank, offset)`, in that order, and
    /// all three parts matter:
    ///
    /// - **row**, not `MessageId`: ids are allocated in creation order,
    ///   but a chat-history batch inserts *older* messages with *higher*
    ///   ids, so comparing ids inverts the selection after a backfill.
    /// - **source rank** before offset: within a row the gutter precedes
    ///   the body, and their offsets index unrelated strings, so
    ///   comparing offsets across sources is meaningless. Skipping this
    ///   is what made a same-row gutter→body drag select the whole row.
    /// - **offset** last, within one source.
    pub fn ordered(&self, row_of: impl Fn(MessageId) -> Option<usize>) -> (Caret, Caret) {
        let key = |c: &Caret| {
            (
                row_of(c.message).unwrap_or(usize::MAX),
                source_rank(c.source),
                c.offset,
            )
        };
        if key(&self.anchor) <= key(&self.focus) {
            (self.anchor, self.focus)
        } else {
            (self.focus, self.anchor)
        }
    }
}
