//! Tests for the GTK skin.
//!
//! Deliberately limited to what can run without a display. The widget
//! itself can't be constructed on display-less CI (gtk4-rs asserts an
//! initialised GTK on every widget constructor), so what is tested here
//! is the piece that *can* be: the Pango measurer, which is the only
//! part of C2 carrying real logic rather than plumbing.
//!
//! `pangocairo`'s default font map works without a `GdkDisplay`, which
//! is what makes even this much possible. Geometry correctness proper is
//! covered in `hxchat-layout` against a deterministic measurer; what
//! matters here is that the *real* measurer upholds the invariants that
//! engine assumes.

use crate::measure::PangoMeasure;
use hxchat_layout::{Attrs, Style, TextMeasure};

fn m() -> PangoMeasure {
    PangoMeasure::headless("Monospace 10")
}

#[test]
fn metrics_are_sane() {
    let m = m();
    let met = m.metrics();
    assert!(met.line_height > 0, "line height must be positive");
    assert!(met.space_width > 0, "space width must be positive");
    assert!(
        met.ascent <= met.line_height,
        "ascent {} exceeds line height {}",
        met.ascent,
        met.line_height
    );
}

#[test]
fn empty_run_is_zero_width() {
    assert_eq!(m().run_width("", Style::default()), 0);
}

#[test]
fn width_is_monotonic_in_length() {
    // The wrap algorithm's binary-search fallback and the break-point
    // walk both assume longer means wider. A measurer that violated this
    // would produce silently wrong wrap points rather than an error.
    let m = m();
    let s = "the quick brown fox jumps over the lazy dog";
    let mut prev = 0;
    for i in (1..s.len()).step_by(3) {
        let w = m.run_width(&s[..i], Style::default());
        assert!(w >= prev, "width shrank between {} and {i}", i - 3);
        prev = w;
    }
}

#[test]
fn bold_is_never_narrower_than_regular() {
    // Not strictly guaranteed by every font, but any font where bold is
    // *narrower* would make the style-aware break-point search
    // pessimistic rather than wrong. Assert the assumption so a font
    // stack change that breaks it is visible.
    let m = m();
    let s = "measurement";
    let plain = m.run_width(s, Style::default());
    let bold = m.run_width(s, Style::default().with_attrs(Attrs::BOLD));
    assert!(bold >= plain, "bold {bold} < regular {plain}");
}

#[test]
fn fit_prefix_honours_its_contract() {
    // The property the whole wrap algorithm rests on — stated as
    // TextMeasure::fit_prefix documents it, including the
    // minimum-progress exemption. A budget too small for even one
    // character must still consume one: you cannot draw less than a
    // character, and returning 0 would leave the wrap loop unable to
    // advance. One clipped grapheme beats a hung UI.
    let m = m();
    let s = "the quick brown fox jumps over the lazy dog";
    let one_char = m.run_width(&s[..1], Style::default());

    for budget in [1u32, 5, 10, 25, 50, 100, 200, 10_000] {
        let (n, w) = m.fit_prefix(s, Style::default(), budget);
        assert!(n <= s.len());
        assert!(n > 0, "must make progress at budget {budget}");
        assert!(s.is_char_boundary(n), "prefix {n} splits a character");
        if n < s.len() {
            assert!(
                w <= budget || w <= one_char,
                "prefix of {n} bytes measures {w}, over budget {budget} \
                 and wider than a single character ({one_char})"
            );
        }
    }
}

#[test]
fn fit_prefix_makes_progress_on_a_tiny_budget() {
    // Returning 0 for a non-empty run would hang the wrap loop. The
    // engine guards against it too, but the measurer must not rely on
    // that.
    let m = m();
    let (n, _) = m.fit_prefix("hello", Style::default(), 1);
    assert!(n > 0, "must consume at least one character");
}

#[test]
fn fit_prefix_handles_multibyte() {
    let m = m();
    let s = "héllo → wörld 😀 more text here";
    for budget in [1u32, 7, 13, 40, 90] {
        let (n, _) = m.fit_prefix(s, Style::default(), budget);
        assert!(s.is_char_boundary(n), "prefix {n} split a codepoint");
    }
}

#[test]
fn zoom_scales_measurements() {
    // Zoom is a view scale, so everything must grow together (§3.7).
    let mut m = m();
    let s = "hello world";
    let base = m.run_width(s, Style::default());
    let base_h = m.metrics().line_height;

    m.set_zoom_permille(2000);
    let big = m.run_width(s, Style::default());
    let big_h = m.metrics().line_height;

    assert!(big > base, "text should widen with zoom ({base} -> {big})");
    assert!(
        big_h > base_h,
        "line height should grow with zoom ({base_h} -> {big_h})"
    );

    m.set_zoom_permille(1000);
    assert_eq!(
        m.run_width(s, Style::default()),
        base,
        "returning to 100% must restore the original measurement"
    );
}

#[test]
fn zoom_is_clamped() {
    let mut m = m();
    m.set_zoom_permille(0);
    assert!(m.zoom_permille() >= 250);
    m.set_zoom_permille(u32::MAX);
    assert!(m.zoom_permille() <= 5000);
}

#[test]
fn cache_returns_consistent_widths() {
    // The cache keys on (text, attrs) and ignores colour, since colour
    // cannot affect advance width. Verify that assumption holds rather
    // than trusting it.
    let m = m();
    let s = "cached";
    let a = Style::default();
    let mut b = Style::default();
    b.fg = hxchat_layout::ColorRef::Palette(4);
    assert_eq!(m.run_width(s, a), m.run_width(s, b));
    // And a repeat hit agrees with the first.
    assert_eq!(m.run_width(s, a), m.run_width(s, a));
}

#[test]
fn font_change_invalidates_the_cache() {
    let mut m = m();
    let s = "the quick brown fox";
    let small = m.run_width(s, Style::default());
    m.set_font(pango::FontDescription::from_string("Monospace 30"));
    let large = m.run_width(s, Style::default());
    assert!(
        large > small,
        "a larger font must measure wider ({small} -> {large}); a stale \
         cache would return the old value"
    );
}
