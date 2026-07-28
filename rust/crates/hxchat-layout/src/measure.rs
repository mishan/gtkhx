//! Text measurement, abstracted.
//!
//! This trait is the reason the rest of the crate has no dependencies.
//! Shaping text genuinely needs a font stack, but everything *around*
//! shaping — wrapping, height indexing, scroll anchoring, the message
//! model — does not, and keeping the font stack behind a trait is what
//! lets the whole engine run under `cargo test` on display-less CI.
//!
//! The view (C2) supplies a Pango-backed implementation. The tests here
//! supply [`FixedMeasure`], where every character is exactly N pixels
//! wide, which makes wrap assertions exact and readable instead of
//! font-dependent and brittle.
//!
//! It is also a hedge against xtext's worst performance bug. Its
//! `find_next_wrap` (xtext.c:3685) called `backend_get_text_width_emph`
//! **per character**, and that did a `pango_layout_set_text` +
//! `pango_layout_get_pixel_size` round trip each time. The trait's unit
//! of work is a run, never a character, so an implementation physically
//! cannot repeat that mistake.

use crate::span::Style;

/// Font metrics the layout engine needs before it measures anything.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct FontMetrics {
    /// Distance from one baseline to the next.
    pub line_height: u32,
    /// Baseline offset from the top of the line box.
    pub ascent: u32,
    /// Width of a space, used for the indent column's minimum.
    pub space_width: u32,
}

impl Default for FontMetrics {
    fn default() -> Self {
        FontMetrics {
            line_height: 16,
            ascent: 12,
            space_width: 8,
        }
    }
}

/// Measures runs of styled text.
///
/// Implementations must be deterministic for a given
/// `(text, style, scale)` — the layout cache assumes that a re-measure
/// at the same [`LayoutGeneration`](crate::LayoutGeneration) yields the
/// same answer.
pub trait TextMeasure {
    /// Metrics at the current scale.
    fn metrics(&self) -> FontMetrics;

    /// Advance width of `text` drawn with `style`, in pixels.
    ///
    /// `text` is a single run: one style, no newlines.
    fn run_width(&self, text: &str, style: Style) -> u32;

    /// The largest prefix of `text` that fits in `max_width`, returned as
    /// a byte offset, together with its width.
    ///
    /// # Contract
    ///
    /// - The returned offset is always on a `char` boundary.
    /// - The returned width is `<= max_width` **except** in the
    ///   minimum-progress case below.
    /// - **Minimum progress:** for non-empty `text` and non-zero
    ///   `max_width`, the offset is always `> 0`, even when not a single
    ///   character fits. This is deliberate and the overflow is
    ///   unavoidable — you cannot render less than one character, and the
    ///   alternative (returning 0) makes the wrap loop in
    ///   [`crate::wrap`] unable to advance. A single over-wide grapheme
    ///   clips; an infinite loop hangs the UI.
    ///
    /// The default implementation is a binary search over
    /// [`TextMeasure::run_width`] on character boundaries — correct for
    /// any monotonic measurer and adequate for the fixed-width test one.
    /// A Pango implementation should override it with a single
    /// `pango_layout_line_index_to_x`-style query rather than paying
    /// `log n` shaping passes.
    fn fit_prefix(&self, text: &str, style: Style, max_width: u32) -> (usize, u32) {
        if text.is_empty() {
            return (0, 0);
        }
        let full = self.run_width(text, style);
        if full <= max_width {
            return (text.len(), full);
        }
        let mut lo = 0usize;
        let mut hi = text.len();
        let mut best = (0usize, 0u32);
        while lo < hi {
            let mut mid = lo + (hi - lo) / 2;
            // Snap to a char boundary; without this the loop can stall
            // on a multi-byte character.
            while mid > lo && !text.is_char_boundary(mid) {
                mid -= 1;
            }
            if mid == lo {
                break;
            }
            let w = self.run_width(&text[..mid], style);
            if w <= max_width {
                best = (mid, w);
                lo = mid;
            } else {
                hi = mid;
            }
        }
        best
    }

    /// Rendered size of an image block, given its intrinsic size and the
    /// width available.
    ///
    /// Default: scale down to fit, never up, preserving aspect ratio —
    /// which is what the Phase 9.E media path already does.
    fn image_size(&self, intrinsic: (u32, u32), max_width: u32) -> (u32, u32) {
        let (w, h) = intrinsic;
        if w == 0 || h == 0 {
            return (0, 0);
        }
        if w <= max_width {
            return (w, h);
        }
        let scaled_h = ((h as u64 * max_width as u64) / w as u64) as u32;
        (max_width, scaled_h.max(1))
    }
}

/// Deterministic measurer for tests: every character is `char_width`
/// pixels wide regardless of style.
///
/// Not a toy — it is what makes the wrap tests assert exact byte offsets
/// rather than "roughly here", and it is the reference implementation
/// the Pango one is checked against for monotonicity.
#[derive(Clone, Copy, Debug)]
pub struct FixedMeasure {
    pub char_width: u32,
    pub metrics: FontMetrics,
}

impl FixedMeasure {
    pub fn new(char_width: u32) -> FixedMeasure {
        FixedMeasure {
            char_width,
            metrics: FontMetrics {
                line_height: 16,
                ascent: 12,
                space_width: char_width,
            },
        }
    }
}

impl Default for FixedMeasure {
    fn default() -> Self {
        FixedMeasure::new(8)
    }
}

impl TextMeasure for FixedMeasure {
    fn metrics(&self) -> FontMetrics {
        self.metrics
    }

    fn run_width(&self, text: &str, _style: Style) -> u32 {
        (text.chars().count() as u32) * self.char_width
    }
}
