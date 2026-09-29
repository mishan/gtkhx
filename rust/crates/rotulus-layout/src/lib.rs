//! The layout engine behind the Rotulus chat view.
//!
//! Everything between "a message arrives" and "the view knows what
//! pixels to put where", with no GTK, no GLib and no Pango, so all of it
//! runs under `cargo test` on display-less CI. The widget that consumes
//! it is `rotulus`; see docs/chat-view.md.
//!
//! ```text
//!   Message  ──parse──▶  ParsedText (text + Spans)
//!      │                        │
//!      │                   layout_message ──▶ LayoutCache (height, LineBoxes)
//!      ▼                        │
//!   ChatBuffer ◀────────────────┘
//!      │  rows + HeightIndex + ScrollAnchor
//!      ▼
//!   view: "which rows are visible, and how tall is everything"
//! ```
//!
//! # What this replaces, and why
//!
//! HexChat's xtext widget lays out on a uniform grid: every row is
//! `fontsize × subline_count`, the scroll adjustment's unit is fractional
//! *text lines*, and hit-testing is `y / fontsize`. That assumption is
//! wired through the whole widget, and every feature added to it late
//! had to work around it — inline media reserves
//! `ceil(image_height / fontsize)` blank text rows and paints a texture
//! across them, which is why selection drags through empty space and the
//! marker line draws above an image rather than across it.
//!
//! Four things here are deliberate departures:
//!
//! - **Parse once.** Styling is resolved at append into [`span::Span`]s.
//!   xtext re-ran its escape state machine over every visible byte on
//!   every render pass *and* separately built a run list at append that
//!   the render path then ignored.
//! - **Pixels, not lines.** [`index::HeightIndex`] gives O(log n)
//!   pixel↔row in both directions over genuinely variable heights.
//! - **Anchor, not offset.** [`anchor::ScrollAnchor`] pins the viewport
//!   to a row. Resize, zoom, history backfill, scrollback trim and a
//!   late-arriving image decode then all preserve reading position for
//!   free, rather than each needing its own compensation.
//! - **Lazy layout.** A width or font change invalidates without
//!   recomputing, so resize costs O(visible). xtext's
//!   `gtk_xtext_calc_lines` re-wraps the entire scrollback.
//!
//! # Measurement
//!
//! [`measure::TextMeasure`] is the seam. The view supplies a Pango
//! implementation; tests supply [`measure::FixedMeasure`], where every
//! character is N pixels wide, so wrap assertions are exact rather than
//! font-dependent. The trait's unit of work is a *run*, never a
//! character — xtext measured per character through a full Pango round
//! trip each time, and the trait shape makes repeating that impossible.
//!
//! # Input vocabularies
//!
//! Style arrives as runs from the application, already resolved, so the
//! engine decodes no in-band escapes and nothing a remote user sends can
//! restyle a row. [`markdown`] is the one vocabulary it parses itself: an
//! inline subset, rendered on display and transmitted literally.
//! [`linkify`] finds URLs, against a scheme list the application owns.

#![forbid(unsafe_code)]
#![warn(missing_debug_implementations)]

pub mod anchor;
pub mod buffer;
pub mod index;
pub mod linkify;
pub mod markdown;
pub mod measure;
pub mod message;
pub mod search;
pub mod select;
pub mod span;
pub mod wrap;

#[cfg(test)]
mod tests;

pub use anchor::{Gravity, ScrollAnchor};
pub use buffer::{ChatBuffer, MIN_INDENT};
pub use index::HeightIndex;
pub use linkify::{Linkifier, DEFAULT_SCHEMES};
pub use markdown::{scan_delims, SourceSpan};
pub use measure::{FixedMeasure, FontMetrics, TextMeasure};
pub use message::{
    Block, Blocks, GroupKey, ImageSize, LoadMoreDirection, Message, MessageFlags, MessageId,
    MessageKind, Speaker,
};
pub use search::{find_all, Match, SearchState};
pub use select::{Caret, RowSelection, Selection};
pub use span::{Attrs, ColorRef, Link, LinkId, ParsedText, Span, Style};
pub use wrap::{AvatarBox, LayoutCache, LayoutGeneration, LayoutParams, LineBox, LineSource};
