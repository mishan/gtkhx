//! Scroll position, expressed as a row rather than a pixel.
//!
//! The single most load-bearing design decision in the engine. A raw
//! pixel scroll value is only meaningful relative to a particular set of
//! row heights, and every interesting thing that happens to a chat
//! buffer changes those heights: a resize re-wraps, a zoom rescales, an
//! image finishes decoding and a 16-pixel placeholder becomes 240
//! pixels, a history batch prepends 50 rows above the viewport, a
//! scrollback trim drops rows off the top.
//!
//! Anchoring to `(row, offset within it)` makes all of those free.
//! Compare xtext, which hand-patches each case separately:
//! `gtk_xtext_insert_indent_before` (xtext.c:5720) bumps `pagetop_line`,
//! `last_pixel_pos`, `old_value` and the adjustment by the inserted
//! row's subline count to approximate "don't jump"; the trim path does
//! the mirror-image decrement; and a font change just accepts the jump.
//!
//! It is also what makes estimated heights ([`crate::index`]) tolerable:
//! when an estimate is replaced by a real measurement the total height
//! changes, so the scrollbar thumb moves — but the anchored row does
//! not, so the *text the user is reading* stays put. Thumb drift is
//! survivable; content jumping is not.

use crate::index::HeightIndex;
use crate::message::MessageId;

/// What the viewport is pinned to.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Gravity {
    /// Follow new messages. The common case: the user is at the bottom
    /// reading live chat, and appends should scroll.
    Bottom,
    /// Hold the anchored row still. The user has scrolled up.
    Free,
}

/// The viewport's position.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ScrollAnchor {
    /// The row the viewport's top edge sits in. `None` with
    /// [`Gravity::Bottom`] means "pinned to the very bottom", which is
    /// the initial state and the state after every append while
    /// following.
    pub message: Option<MessageId>,
    /// Pixels from the top of that row down to the viewport's top edge.
    pub offset: u32,
    pub gravity: Gravity,
}

impl Default for ScrollAnchor {
    fn default() -> Self {
        ScrollAnchor {
            message: None,
            offset: 0,
            gravity: Gravity::Bottom,
        }
    }
}

impl ScrollAnchor {
    /// Pinned to the bottom, following new messages.
    pub fn bottom() -> ScrollAnchor {
        ScrollAnchor::default()
    }

    pub fn is_following(&self) -> bool {
        self.gravity == Gravity::Bottom
    }
}

/// Resolves anchors against a live index. Stateless — the buffer calls
/// these as associated functions.
#[derive(Debug)]
pub struct AnchorResolver;

impl AnchorResolver {
    /// The pixel value the scroll adjustment should report.
    ///
    /// `row_of` maps a [`MessageId`] to its current row position, which
    /// only the buffer knows. Returns a value already clamped to
    /// `[0, total - viewport]`.
    pub fn to_pixels(
        anchor: &ScrollAnchor,
        index: &mut HeightIndex,
        viewport_height: u32,
        row_of: impl Fn(MessageId) -> Option<usize>,
    ) -> u64 {
        let total = index.total_height();
        let max = total.saturating_sub(u64::from(viewport_height));

        match anchor.message {
            // Following: always the bottom.
            None => max,
            Some(id) => match row_of(id) {
                Some(row) => {
                    let base = index.offset_of(row);
                    let y = base + u64::from(anchor.offset);
                    y.min(max)
                }
                // The anchored row was trimmed or cleared out from under
                // us. Falling back to the bottom is right: a trim only
                // ever removes the *oldest* rows, so the content the user
                // was looking at is gone and the least surprising place
                // to be is where new messages arrive.
                None => max,
            },
        }
    }

    /// Build an anchor from a pixel position — what a scrollbar drag
    /// produces.
    ///
    /// Snapping to `Gravity::Bottom` when the position is within
    /// `follow_slop` of the end is what makes "scroll to the bottom and
    /// it starts following again" work, and it is why the slop exists at
    /// all: landing one pixel short of the end should not silently stop
    /// following.
    pub fn from_pixels(
        y: u64,
        index: &mut HeightIndex,
        viewport_height: u32,
        follow_slop: u32,
        id_of_row: impl Fn(usize) -> Option<MessageId>,
    ) -> ScrollAnchor {
        let total = index.total_height();
        let max = total.saturating_sub(u64::from(viewport_height));
        if y + u64::from(follow_slop) >= max {
            return ScrollAnchor::bottom();
        }
        match index.locate(y) {
            Some(hit) => ScrollAnchor {
                message: id_of_row(hit.row),
                offset: hit.offset,
                gravity: Gravity::Free,
            },
            None => ScrollAnchor::bottom(),
        }
    }
}
