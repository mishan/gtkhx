//! The Video panel's tile layout.
//!
//! Tiles are laid out the way a call window lays out faces: as a gallery,
//! at whatever column count makes them largest in the space there is, each
//! at its stream's shape so the picture fills it. With a tile in focus, that
//! one takes the stage at the top and the rest share a strip below it, as a
//! smaller gallery of their own.
//!
//! The panel scrolls, so the grid asks for no more height than its tiles
//! need at their smallest; the scrolled window hands it the whole view when
//! that is more, and the tiles grow into it. Only when they can't all fit
//! at their smallest does the panel scroll.
//!
//! The arithmetic is plain functions of sizes, apart from the widget, so it
//! is tested without a display.

use std::cell::{Cell, RefCell};

use gtk4 as gtk;
use gtk4::glib;
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;

use hxvoice_runtime::hxvoice::VideoKind;
use hxvoice_runtime::video::StreamKey;

/// Between tiles, both ways.
const SPACING: f64 = 6.0;
/// The narrowest a tile gets before the panel scrolls instead.
const MIN_TILE_W: f64 = 160.0;
/// The shortest a gallery row gets before the panel scrolls instead:
/// `MIN_TILE_W` at 4:3.
const MIN_TILE_H: f64 = 120.0;
/// The shortest the stage gets, focus on.
const MIN_STAGE_H: f64 = 180.0;
/// The share of the height the strip under the stage gets at least.
const STRIP_SHARE: f64 = 0.25;
/// The shape a gallery takes when its tiles differ.
const MIXED_ASPECT: f64 = 4.0 / 3.0;
/// The narrowest and widest shapes a tile takes. A stream's shape comes
/// from the frames a peer sends, so a sliver of a picture mustn't make a
/// sliver of a tile.
const MIN_ASPECT: f64 = 0.25;
const MAX_ASPECT: f64 = 4.0;

/// The shape a stream's tile has before its first frame says otherwise.
pub(crate) fn default_aspect(kind: VideoKind) -> f64 {
    match kind {
        VideoKind::Camera => 4.0 / 3.0,
        VideoKind::Screen => 16.0 / 9.0,
    }
}

/// A tile's place, in the grid's own coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct Rect {
    pub(crate) x: f64,
    pub(crate) y: f64,
    pub(crate) w: f64,
    pub(crate) h: f64,
}

/// The one shape a gallery of tiles shaped `aspects` shares: theirs when
/// they agree, a camera's when they don't.
fn common_aspect(aspects: &[f64]) -> f64 {
    match aspects.split_first() {
        Some((first, rest)) if rest.iter().all(|a| (a - first).abs() < 0.01) => *first,
        _ => MIXED_ASPECT,
    }
}

/// How many columns a gallery of `n` at its smallest takes in width `w`.
fn min_columns(n: usize, w: f64) -> usize {
    let fit = ((w + SPACING) / (MIN_TILE_W + SPACING)).floor() as usize;
    fit.clamp(1, n.max(1))
}

/// The height a gallery of `n` shaped `aspect` needs in width `w` with its
/// tiles at their smallest: `MIN_TILE_W` wide, and rows no shorter than
/// `MIN_TILE_H`.
fn gallery_min_height(n: usize, aspect: f64, w: f64) -> f64 {
    if n == 0 {
        return 0.0;
    }
    let rows = n.div_ceil(min_columns(n, w)) as f64;
    let row_h = MIN_TILE_H.max(MIN_TILE_W / aspect);
    rows * row_h + (rows - 1.0) * SPACING
}

/// `n` tiles shaped `aspect` in the box at (`x`, `y`), `w` by `h`: the
/// column count that makes them largest, the block centered in the box
/// and a short last row centered under the others.
fn gallery(n: usize, aspect: f64, x: f64, y: f64, w: f64, h: f64) -> Vec<Rect> {
    if n == 0 {
        return Vec::new();
    }
    let size = |cols: usize| {
        let rows = n.div_ceil(cols);
        let cell_w = (w - SPACING * (cols - 1) as f64) / cols as f64;
        let cell_h = (h - SPACING * (rows - 1) as f64) / rows as f64;
        cell_w.min(cell_h * aspect)
    };
    // The first column count to reach the largest tile, so a tie keeps the
    // tiles in fewer, longer rows.
    let mut cols = 1;
    for c in 2..=n {
        if size(c) > size(cols) {
            cols = c;
        }
    }
    let tile_w = size(cols).floor().max(1.0);
    let tile_h = (tile_w / aspect).floor().max(1.0);
    let rows = n.div_ceil(cols);
    let block_h = rows as f64 * tile_h + (rows - 1) as f64 * SPACING;
    let top = y + ((h - block_h) / 2.0).max(0.0).floor();
    (0..n)
        .map(|i| {
            let (row, col) = (i / cols, i % cols);
            let in_row = cols.min(n - row * cols);
            let row_w = in_row as f64 * tile_w + (in_row - 1) as f64 * SPACING;
            let left = x + ((w - row_w) / 2.0).max(0.0).floor();
            Rect {
                x: left + col as f64 * (tile_w + SPACING),
                y: top + row as f64 * (tile_h + SPACING),
                w: tile_w,
                h: tile_h,
            }
        })
        .collect()
}

/// The shapes in `aspects` other than the one at `f`.
fn others(aspects: &[f64], f: usize) -> Vec<f64> {
    (0..aspects.len())
        .filter(|&i| i != f)
        .map(|i| aspects[i])
        .collect()
}

/// The height tiles shaped `aspects`, `focused` on the stage if any, need
/// in width `w` at their smallest.
pub(crate) fn min_height(aspects: &[f64], focused: Option<usize>, w: f64) -> f64 {
    match focused {
        Some(f) if aspects.len() > 1 => {
            let stage = MIN_STAGE_H.min(w / aspects[f]);
            stage
                + SPACING
                + gallery_min_height(aspects.len() - 1, common_aspect(&others(aspects, f)), w)
        }
        _ => gallery_min_height(aspects.len(), common_aspect(aspects), w),
    }
}

/// Where each of the tiles shaped `aspects` goes in `w` by `h`, in order.
/// With none `focused`, one gallery. With one, it takes the top at its own
/// shape — as tall as the width lets it be, less the strip — and the
/// others share what is left below as a gallery.
pub(crate) fn arrange(aspects: &[f64], focused: Option<usize>, w: f64, h: f64) -> Vec<Rect> {
    let Some(f) = focused.filter(|_| aspects.len() > 1) else {
        return gallery(aspects.len(), common_aspect(aspects), 0.0, 0.0, w, h);
    };
    let others = others(aspects, f);
    let strip = (h * STRIP_SHARE).max(gallery_min_height(others.len(), common_aspect(&others), w));
    let stage_h = (h - strip - SPACING)
        .min(w / aspects[f])
        .max(MIN_STAGE_H.min(w / aspects[f]));
    let stage = gallery(1, aspects[f], 0.0, 0.0, w, stage_h);
    let below = stage_h + SPACING;
    let mut strip = gallery(
        others.len(),
        common_aspect(&others),
        0.0,
        below,
        w,
        h - below,
    )
    .into_iter();
    (0..aspects.len())
        .map(|i| {
            if i == f {
                stage[0]
            } else {
                strip.next().expect("a place for every other tile")
            }
        })
        .collect()
}

// ---------------------------------------------------------------------
// The widget.
// ---------------------------------------------------------------------

struct Entry {
    key: StreamKey,
    widget: gtk::Widget,
    aspect: f64,
}

/// Tiles in user order, this client's own first: the order publications
/// reach the client varies, and tiles shouldn't swap places between
/// sessions. A user's camera comes before their screen.
fn order(key: &StreamKey) -> (u16, VideoKind) {
    (key.user_id, key.kind)
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct VideoGrid {
        pub(super) entries: RefCell<Vec<Entry>>,
        pub(super) focused: Cell<Option<StreamKey>>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for VideoGrid {
        const NAME: &'static str = "HxVideoGrid";
        type Type = super::VideoGrid;
        type ParentType = gtk::Widget;
    }

    impl ObjectImpl for VideoGrid {
        fn dispose(&self) {
            for e in self.entries.borrow_mut().drain(..) {
                e.widget.unparent();
            }
        }
    }

    impl WidgetImpl for VideoGrid {
        fn request_mode(&self) -> gtk::SizeRequestMode {
            gtk::SizeRequestMode::HeightForWidth
        }

        fn measure(&self, orientation: gtk::Orientation, for_size: i32) -> (i32, i32, i32, i32) {
            if orientation == gtk::Orientation::Horizontal {
                let min = MIN_TILE_W as i32;
                return (min, 2 * min + SPACING as i32, -1, -1);
            }
            let w = if for_size < 0 {
                MIN_TILE_W
            } else {
                f64::from(for_size)
            };
            let (aspects, focused) = self.shape();
            let h = min_height(&aspects, focused, w).ceil() as i32;
            (h, h, -1, -1)
        }

        fn size_allocate(&self, width: i32, height: i32, _baseline: i32) {
            let (aspects, focused) = self.shape();
            let places = arrange(&aspects, focused, f64::from(width), f64::from(height));
            for (e, r) in self.entries.borrow().iter().zip(places) {
                // Every child is measured before it is allocated, and never
                // given less than its minimum.
                let (min_w, ..) = e.widget.measure(gtk::Orientation::Horizontal, -1);
                let w = (r.w as i32).max(min_w);
                let (min_h, ..) = e.widget.measure(gtk::Orientation::Vertical, w);
                let h = (r.h as i32).max(min_h);
                e.widget
                    .size_allocate(&gtk::Allocation::new(r.x as i32, r.y as i32, w, h), -1);
            }
        }
    }

    impl VideoGrid {
        /// Each tile's shape, in order, and which is in focus.
        fn shape(&self) -> (Vec<f64>, Option<usize>) {
            let entries = self.entries.borrow();
            let focused = self.focused.get();
            (
                entries.iter().map(|e| e.aspect).collect(),
                entries.iter().position(|e| Some(e.key) == focused),
            )
        }
    }
}

glib::wrapper! {
    /// The container the Video panel's tiles sit in. See module docs.
    pub struct VideoGrid(ObjectSubclass<imp::VideoGrid>)
        @extends gtk::Widget,
        @implements gtk::Accessible, gtk::Buildable, gtk::ConstraintTarget;
}

impl VideoGrid {
    pub(crate) fn new() -> VideoGrid {
        glib::Object::new()
    }

    /// Add `widget` as `key`'s tile, in its place in the order.
    pub(crate) fn insert(&self, key: StreamKey, widget: &impl IsA<gtk::Widget>) {
        let mut entries = self.imp().entries.borrow_mut();
        let at = entries.partition_point(|e| order(&e.key) < order(&key));
        // Children in the same order as the tiles, so keyboard focus moves
        // through them the way they read.
        let next = entries.get(at).map(|e| e.widget.clone());
        widget.insert_before(self, next.as_ref());
        entries.insert(
            at,
            Entry {
                key,
                widget: widget.clone().upcast(),
                aspect: default_aspect(key.kind),
            },
        );
        drop(entries);
        self.queue_resize();
    }

    /// Take `key`'s tile out, if it is here.
    pub(crate) fn remove(&self, key: StreamKey) {
        let mut entries = self.imp().entries.borrow_mut();
        if let Some(at) = entries.iter().position(|e| e.key == key) {
            entries.remove(at).widget.unparent();
            drop(entries);
            self.queue_resize();
        }
    }

    /// Put `key`'s tile on the stage, or none.
    pub(crate) fn set_focused(&self, key: Option<StreamKey>) {
        if self.imp().focused.replace(key) != key {
            self.queue_resize();
        }
    }

    #[cfg(test)]
    pub(crate) fn focused(&self) -> Option<StreamKey> {
        self.imp().focused.get()
    }

    /// `key`'s stream turned out to be `aspect` (width over height).
    pub(crate) fn set_aspect(&self, key: StreamKey, aspect: f64) {
        if !aspect.is_finite() || aspect <= 0.0 {
            return;
        }
        let aspect = aspect.clamp(MIN_ASPECT, MAX_ASPECT);
        let mut entries = self.imp().entries.borrow_mut();
        if let Some(e) = entries.iter_mut().find(|e| e.key == key) {
            if (e.aspect - aspect).abs() >= 0.01 {
                e.aspect = aspect;
                drop(entries);
                self.queue_resize();
            }
        }
    }

    /// The tiles' keys, in order.
    #[cfg(test)]
    pub(crate) fn keys(&self) -> Vec<StreamKey> {
        self.imp().entries.borrow().iter().map(|e| e.key).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const CAM: f64 = 4.0 / 3.0;
    const SCREEN: f64 = 16.0 / 9.0;

    fn inside(r: &Rect, w: f64, h: f64) -> bool {
        r.x >= 0.0 && r.y >= 0.0 && r.x + r.w <= w + 0.5 && r.y + r.h <= h + 0.5
    }

    fn overlap(a: &Rect, b: &Rect) -> bool {
        a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h
    }

    fn check(places: &[Rect], w: f64, h: f64) {
        for (i, a) in places.iter().enumerate() {
            assert!(inside(a, w, h), "{a:?} leaves {w}x{h}");
            for b in &places[i + 1..] {
                assert!(!overlap(a, b), "{a:?} overlaps {b:?}");
            }
        }
    }

    /// One camera fills the width of a tall panel, at its own shape.
    #[test]
    fn one_tile_takes_the_width() {
        let p = arrange(&[CAM], None, 400.0, 900.0);
        assert_eq!(p[0].w, 400.0);
        assert_eq!(p[0].h, 300.0);
        // Centered in the height.
        assert_eq!(p[0].y, 300.0);
    }

    /// Four cameras go two by two in a square-ish panel, and side by side
    /// in a wide one: whichever makes them largest.
    #[test]
    fn gallery_picks_the_largest_columns() {
        let square = arrange(&[CAM; 4], None, 800.0, 600.0);
        check(&square, 800.0, 600.0);
        let cols = square.iter().filter(|r| r.y == square[0].y).count();
        assert_eq!(cols, 2);

        let wide = arrange(&[CAM; 4], None, 1600.0, 300.0);
        check(&wide, 1600.0, 300.0);
        assert!(wide.iter().all(|r| r.y == wide[0].y), "one row");

        let tall = arrange(&[CAM; 4], None, 300.0, 1200.0);
        check(&tall, 300.0, 1200.0);
        assert!(tall.iter().all(|r| r.x == tall[0].x), "one column");
    }

    /// Three in two columns: the last row's lone tile is centered.
    #[test]
    fn a_short_last_row_is_centered() {
        let p = arrange(&[CAM; 3], None, 606.0, 456.0);
        check(&p, 606.0, 456.0);
        assert_eq!(p[0].y, p[1].y);
        assert!(p[2].y > p[0].y);
        let mid = |r: &Rect| r.x + r.w / 2.0;
        assert!((mid(&p[2]) - 303.0).abs() <= 1.0, "{:?}", p[2]);
    }

    /// With one in focus it gets the stage, far larger than the others,
    /// and they all sit below it.
    #[test]
    fn focus_takes_the_stage() {
        let aspects = [CAM, CAM, SCREEN, CAM];
        let p = arrange(&aspects, Some(2), 800.0, 800.0);
        check(&p, 800.0, 800.0);
        let stage = p[2];
        assert_eq!(stage.y, 0.0);
        assert!((stage.w / stage.h - SCREEN).abs() < 0.02);
        for (i, r) in p.iter().enumerate() {
            if i != 2 {
                assert!(r.y >= stage.y + stage.h, "{r:?} is below the stage");
                assert!(r.w * r.h * 4.0 < stage.w * stage.h, "{r:?} is smaller");
            }
        }
    }

    /// A narrow panel doesn't leave the stage half empty: the stage is only
    /// as tall as the focused tile at full width, and the strip gets the
    /// rest.
    #[test]
    fn a_narrow_stage_gives_the_strip_the_rest() {
        let p = arrange(&[CAM; 3], Some(0), 300.0, 900.0);
        check(&p, 300.0, 900.0);
        assert_eq!((p[0].w, p[0].h), (300.0, 225.0));
        assert_eq!(p[1].w, 300.0, "the strip has the width to itself");
    }

    /// Focus on the only tile is no different from none.
    #[test]
    fn focus_on_the_only_tile_is_a_gallery() {
        assert_eq!(
            arrange(&[CAM], Some(0), 500.0, 500.0),
            arrange(&[CAM], None, 500.0, 500.0)
        );
    }

    /// The height asked for is enough for the tiles at their smallest, and
    /// laid out in it they are no smaller than that.
    #[test]
    fn min_height_fits_the_smallest_tiles() {
        for n in 1..=20 {
            for w in [160.0, 250.0, 333.0, 700.0] {
                let aspects = vec![CAM; n];
                let h = min_height(&aspects, None, w);
                let p = arrange(&aspects, None, w, h);
                check(&p, w, h);
                assert!(p.iter().all(|r| r.w >= MIN_TILE_W), "n={n} w={w}");

                let h = min_height(&aspects, Some(0), w);
                let p = arrange(&aspects, Some(0), w, h);
                check(&p, w, h);
                assert!(p[1..].iter().all(|r| r.w >= MIN_TILE_W), "n={n} w={w}");
            }
        }
    }

    /// Portrait streams, a phone's camera, are no narrower than any other
    /// tile at their smallest: the rows grow instead.
    #[test]
    fn portrait_tiles_keep_their_width() {
        const PHONE: f64 = 9.0 / 16.0;
        for n in 1..=8 {
            for w in [160.0, 300.0, 700.0] {
                let aspects = vec![PHONE; n];
                let h = min_height(&aspects, None, w);
                let p = arrange(&aspects, None, w, h);
                check(&p, w, h);
                assert!(
                    p.iter().all(|r| r.w >= MIN_TILE_W - 1.0),
                    "n={n} w={w}: {p:?}"
                );

                let mut mixed = vec![PHONE; n];
                mixed.insert(0, SCREEN);
                let h = min_height(&mixed, Some(0), w);
                let p = arrange(&mixed, Some(0), w, h);
                check(&p, w, h);
                assert!(
                    p[1..].iter().all(|r| r.w >= MIN_TILE_W - 1.0),
                    "n={n} w={w}: {p:?}"
                );
            }
        }
    }

    /// Mixed shapes share a camera's; a gallery of screens keeps theirs.
    #[test]
    fn a_gallery_shares_one_shape() {
        assert_eq!(common_aspect(&[SCREEN, SCREEN]), SCREEN);
        assert_eq!(common_aspect(&[SCREEN, CAM]), MIXED_ASPECT);
        assert_eq!(common_aspect(&[]), MIXED_ASPECT);
    }
}
