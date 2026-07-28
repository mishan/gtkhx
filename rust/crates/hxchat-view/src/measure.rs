//! Pango-backed [`TextMeasure`].
//!
//! The one part of the engine that needs a font stack. Everything about
//! this implementation is shaped by the thing it must not do: xtext's
//! `find_next_wrap` (xtext.c:3685) called `backend_get_text_width_emph`
//! **per character**, and each of those did a `pango_layout_set_text` +
//! `pango_layout_get_pixel_size` round trip. That is the single hottest
//! path in xtext's append and the reason resizing a large buffer
//! hitches.
//!
//! Here:
//!
//! - the unit of work is a run, because that is all the trait exposes;
//! - one [`pango::Layout`] is reused rather than allocated per call;
//! - [`TextMeasure::fit_prefix`] is a single `xy_to_index` query instead
//!   of the trait's default binary search, so finding a wrap point costs
//!   one shaping pass rather than `log n` of them;
//! - a small cache keyed on `(text, style)` short-circuits the repeat
//!   measurements that wrapping produces for the same run.

use hxchat_layout::{Attrs, FontMetrics, Style, TextMeasure};
use pango::prelude::*;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;

/// How many measured runs to keep. Chat lines repeat heavily (nicks,
/// timestamps, the same words), and a bounded map avoids unbounded
/// growth on a long session.
const CACHE_CAP: usize = 4096;

pub struct PangoMeasure {
    context: pango::Context,
    /// Reused across every measurement; never allocated per call.
    layout: pango::Layout,
    font: pango::FontDescription,
    metrics: FontMetrics,
    /// Zoom, per-mille. Applied to the font size, so the whole row
    /// scales together (scoping §3.7).
    zoom_permille: u32,
    /// Measured run widths, nested attrs → text → width.
    ///
    /// Nested rather than keyed on `(String, u8)` so a lookup can borrow:
    /// `String: Borrow<str>` lets the inner map be probed with a plain
    /// `&str`, whereas a tuple key forces a `to_string()` on *every*
    /// call — including cache hits, which is most of them on the
    /// wrapping hot path, and which would eat most of the benefit the
    /// cache exists for. The outer key is the attribute bits, of which
    /// only a handful ever occur.
    cache: RefCell<HashMap<u8, HashMap<String, u32>>>,
    /// Live entry count across every inner map.
    ///
    /// Tracked incrementally because the capacity check runs on every
    /// cache *miss*, and summing the inner maps there would make
    /// maintenance O(n) per miss on the wrapping hot path — undoing much
    /// of what the cache is for.
    cache_len: Cell<usize>,
}

impl std::fmt::Debug for PangoMeasure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PangoMeasure")
            .field("font", &self.font.to_str())
            .field("metrics", &self.metrics)
            .field("zoom_permille", &self.zoom_permille)
            .finish()
    }
}

impl PangoMeasure {
    /// Build a measurer against a widget's Pango context.
    pub fn new(context: pango::Context, font: pango::FontDescription) -> PangoMeasure {
        let layout = pango::Layout::new(&context);
        let mut m = PangoMeasure {
            context,
            layout,
            font,
            metrics: FontMetrics::default(),
            zoom_permille: 1000,
            cache: RefCell::new(HashMap::new()),
            cache_len: Cell::new(0),
        };
        m.recompute_metrics();
        m
    }

    /// A measurer with no display attached.
    ///
    /// `pangocairo`'s default font map works without a `GdkDisplay`,
    /// which is what lets the C2 widget's geometry be exercised in unit
    /// tests on CI.
    pub fn headless(font: &str) -> PangoMeasure {
        let fm = pangocairo::FontMap::default();
        let ctx = fm.create_context();
        PangoMeasure::new(ctx, pango::FontDescription::from_string(font))
    }

    pub fn set_font(&mut self, font: pango::FontDescription) {
        self.font = font;
        self.clear_cache();
        self.recompute_metrics();
    }

    pub fn set_zoom_permille(&mut self, zoom: u32) {
        if zoom == self.zoom_permille {
            return;
        }
        self.zoom_permille = zoom.clamp(250, 5000);
        self.clear_cache();
        self.recompute_metrics();
    }

    fn clear_cache(&self) {
        self.cache.borrow_mut().clear();
        self.cache_len.set(0);
    }

    pub fn zoom_permille(&self) -> u32 {
        self.zoom_permille
    }

    pub fn context(&self) -> &pango::Context {
        &self.context
    }

    /// The font at the current zoom — what the view must use when it
    /// builds layouts to draw, so drawn text matches measured text.
    pub fn scaled_font(&self) -> pango::FontDescription {
        let mut f = self.font.clone();
        let base = if f.size() > 0 {
            f.size()
        } else {
            10 * pango::SCALE
        };
        let scaled = (i64::from(base) * i64::from(self.zoom_permille) / 1000) as i32;
        f.set_size(scaled.max(pango::SCALE));
        f
    }

    fn recompute_metrics(&mut self) {
        let font = self.scaled_font();
        self.context.set_font_description(Some(&font));
        let m = self.context.metrics(Some(&font), None);
        let ascent = m.ascent() / pango::SCALE;
        let descent = m.descent() / pango::SCALE;
        let line_height = (ascent + descent).max(1);
        self.layout.set_font_description(Some(&font));
        self.layout.set_text(" ");
        let (space_width, _) = self.layout.pixel_size();
        self.metrics = FontMetrics {
            line_height: line_height as u32,
            ascent: ascent.max(0) as u32,
            space_width: space_width.max(1) as u32,
        };
    }

    /// Pango attributes for a style.
    ///
    /// Public because the view's snapshot pass needs the *same*
    /// attributes it measured with — if drawing and measuring disagree
    /// about what bold means, text drifts out of its line box.
    pub fn attrs_for(style: Style) -> pango::AttrList {
        let list = pango::AttrList::new();
        if style.attrs.contains(Attrs::BOLD) {
            list.insert(pango::AttrInt::new_weight(pango::Weight::Bold));
        }
        if style.attrs.contains(Attrs::ITALIC) {
            list.insert(pango::AttrInt::new_style(pango::Style::Italic));
        }
        if style.attrs.contains(Attrs::UNDERLINE) {
            list.insert(pango::AttrInt::new_underline(pango::Underline::Single));
        }
        if style.attrs.contains(Attrs::STRIKETHROUGH) {
            list.insert(pango::AttrInt::new_strikethrough(true));
        }
        if style.attrs.contains(Attrs::CODE) {
            list.insert(pango::AttrFontDesc::new(
                &pango::FontDescription::from_string("Monospace"),
            ));
        }
        list
    }

    /// Configure the shared layout for one run and return it.
    fn prepare(&self, text: &str, style: Style) -> &pango::Layout {
        self.layout.set_font_description(Some(&self.scaled_font()));
        self.layout.set_attributes(Some(&Self::attrs_for(style)));
        self.layout.set_text(text);
        &self.layout
    }
}

impl TextMeasure for PangoMeasure {
    fn metrics(&self) -> FontMetrics {
        self.metrics
    }

    fn run_width(&self, text: &str, style: Style) -> u32 {
        if text.is_empty() {
            return 0;
        }
        // Only the attribute bits affect width; colour does not, so the
        // key ignores it and gets far better hit rates.
        let attrs = style.attrs.0;
        if let Some(w) = self
            .cache
            .borrow()
            .get(&attrs)
            .and_then(|inner| inner.get(text))
        {
            return *w;
        }
        let layout = self.prepare(text, style);
        let (w, _) = layout.pixel_size();
        let w = w.max(0) as u32;
        {
            let mut c = self.cache.borrow_mut();
            if self.cache_len.get() >= CACHE_CAP {
                c.clear();
                self.cache_len.set(0);
            }
            // The one allocation, on insert only.
            if c.entry(attrs)
                .or_default()
                .insert(text.to_string(), w)
                .is_none()
            {
                self.cache_len.set(self.cache_len.get() + 1);
            }
        }
        w
    }

    /// One shaping pass, not the trait default's binary search.
    fn fit_prefix(&self, text: &str, style: Style, max_width: u32) -> (usize, u32) {
        if text.is_empty() || max_width == 0 {
            return (0, 0);
        }
        let layout = self.prepare(text, style);
        let (full, _) = layout.pixel_size();
        if full.max(0) as u32 <= max_width {
            return (text.len(), full.max(0) as u32);
        }
        let line = match layout.line_readonly(0) {
            Some(l) => l,
            // Pango should always give us a line for non-empty text, but
            // returning (0, 0) here would break the trait's
            // minimum-progress guarantee and hang the wrap loop — the
            // exact failure mode TextMeasure::fit_prefix documents.
            // Degrade to one character instead.
            None => return self.min_progress(text, style),
        };
        let hit = line.x_to_index((max_width as i32) * pango::SCALE);
        // `is_inside` false means the x fell past the end of the line, so
        // everything fits up to that point. Otherwise take the reported
        // byte index; the trailing offset counts into the glyph, and we
        // deliberately do *not* add it — overshooting the width is worse
        // than undershooting by one character, because it produces a line
        // that draws outside its own box.
        let mut idx = if hit.is_inside() {
            (hit.index() as usize).min(text.len())
        } else {
            text.len()
        };
        while idx > 0 && !text.is_char_boundary(idx) {
            idx -= 1;
        }
        if idx == 0 {
            return self.min_progress(text, style);
        }
        (idx, self.run_width(&text[..idx], style))
    }
}

impl PangoMeasure {
    /// The trait's minimum-progress case: one character, whatever it
    /// measures.
    ///
    /// Every path in [`TextMeasure::fit_prefix`] that would otherwise
    /// return a zero-length prefix routes here instead, because a zero
    /// prefix leaves [`hxchat_layout::wrap`]'s loop unable to advance.
    /// The returned width may exceed the caller's budget; that is the
    /// documented exemption, and one clipped grapheme beats a hung UI.
    fn min_progress(&self, text: &str, style: Style) -> (usize, u32) {
        if text.is_empty() {
            return (0, 0);
        }
        let mut i = 1;
        while i < text.len() && !text.is_char_boundary(i) {
            i += 1;
        }
        (i, self.run_width(&text[..i], style))
    }
}
