//! Tests for the GTK skin, in two tiers.
//!
//! **Always-on:** the Pango measurer. `pangocairo`'s default font map
//! works without a `GdkDisplay`, so the one part of C2 carrying real
//! logic rather than plumbing is testable anywhere. Geometry correctness
//! proper is covered in `hxchat-layout` against a deterministic
//! measurer; what these check is that the *real* measurer upholds the
//! invariants that engine assumes.
//!
//! **Display-gated:** one smoke test covering class registration and
//! widget construction, at the bottom of this file. Those need a real
//! GTK — gtk4-rs asserts an initialised GTK inside the generated
//! `class_init` itself — which is precisely why every C2 bring-up crash
//! was invisible to `cargo test`. It no-ops without a display and runs
//! on a desktop session.

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

// ---- the GTK smoke test (requires a display) ------------------------
//
// Every C2 bring-up crash lived in `class_init` or in widget
// construction, and none was visible to `cargo test`: gtk4-rs asserts an
// initialised GTK inside the generated `class_init` itself
// (gtk4-0.10.3/src/subclass/widget.rs:563), so even registering the type
// needs a display.
//
// **This is deliberately ONE test, not several.** `gtk4::init()` records
// the calling thread as *the* GTK thread, and libtest runs each test on
// its own spawned thread — so a second test that touches GTK trips
// "GTK may only be used from the main thread", which aborts rather than
// failing, taking the whole run with it. (That is not hypothetical: the
// first version of this file was three tests behind a shared `OnceLock`
// gate, and the OnceLock made it worse by telling the second thread GTK
// was ready.) Keeping all GTK work in a single test function means only
// one thread ever touches it, whatever the harness does.
//
// On display-less CI it no-ops. On a developer machine it exercises the
// exact call sequence `create_chat` performs, which is what all three
// crashes died in.

use gtk4::glib::prelude::*;
use gtk4::glib::translate::IntoGlib;

#[test]
fn gtk_class_and_construction_smoke() {
    if gtk4::init().is_err() {
        eprintln!(
            "skipped: no display, GTK could not be initialised \
             (run this on a desktop session to exercise it)"
        );
        return;
    }

    let t = crate::view::HxChatView::static_type();

    // --- class_init ran at all -------------------------------------
    unsafe {
        let c = gtk4::glib::gobject_ffi::g_type_class_ref(t.into_glib());
        assert!(!c.is_null(), "class_init failed for HxChatView");
        gtk4::glib::gobject_ffi::g_type_class_unref(c);
    }

    // --- the GtkScrollable properties are installed ----------------
    //
    // Pins the third C2 crash: declaring fresh ParamSpecs named
    // "hadjustment" etc. collides with the interface's, GLib refuses to
    // install them, and g_object_new yields an object that fails
    // GTK_IS_WIDGET. ParamSpecOverride::for_interface is the fix.
    unsafe {
        let class = gtk4::glib::gobject_ffi::g_type_class_ref(t.into_glib())
            as *mut gtk4::glib::gobject_ffi::GObjectClass;
        for name in [
            c"hadjustment",
            c"vadjustment",
            c"hscroll-policy",
            c"vscroll-policy",
        ] {
            let p = gtk4::glib::gobject_ffi::g_object_class_find_property(
                class,
                name.as_ptr() as *const _,
            );
            assert!(
                !p.is_null(),
                "GtkScrollable property {name:?} is not installed on HxChatView"
            );
        }
        gtk4::glib::gobject_ffi::g_type_class_unref(class as *mut _);
    }

    // --- word_click resolves under both spellings ------------------
    //
    // Pins the second C2 crash: glib-rs's Signal::builder panics on a
    // non-canonical name, and that panic aborts out of class_init. The C
    // callers all spell it "word_click".
    unsafe {
        let hyphen = gtk4::glib::gobject_ffi::g_signal_lookup(
            c"word-click".as_ptr() as *const _,
            t.into_glib(),
        );
        let underscore = gtk4::glib::gobject_ffi::g_signal_lookup(
            c"word_click".as_ptr() as *const _,
            t.into_glib(),
        );
        assert_ne!(hyphen, 0, "word-click is not registered");
        assert_eq!(
            hyphen, underscore,
            "chat.c and msg.c connect \"word_click\"; GLib must resolve it \
             to the same signal as \"word-click\""
        );
    }

    // --- a constructed view is a usable widget ---------------------
    //
    // Pins the first C2 crash (use-after-free on the returned pointer)
    // and re-checks the third: all three produced something that failed
    // exactly this.
    let view = crate::view::HxChatView::new();
    assert!(view.is::<gtk4::Widget>(), "not a GtkWidget");
    assert!(view.is::<gtk4::Scrollable>(), "not a GtkScrollable");

    // --- it survives create_chat's call sequence -------------------
    view.set_font_from_string("Monospace 10");
    view.set_word_wrap(true);
    view.set_max_rows(500);
    view.set_indent(true);
    view.set_max_indent(256);
    view.set_zoom_permille(1000);

    // Appending must not panic on a RefCell re-entrancy, which is the
    // remaining untested hazard in the adjustment plumbing.
    let a = view.append(crate::view::plain_message("hello"));
    let b = view.append(crate::view::plain_message("world"));
    assert_ne!(a, b, "marks must be distinct");
    assert!(view.remove(a), "removing a live mark should succeed");
    assert!(!view.remove(a), "removing a stale mark is a no-op, not a panic");
    view.clear();

    // --- the FFI path, on a floating pointer -----------------------
    //
    // The check the Rust-side construction above cannot make, and the
    // one that would have caught the longest-running C2 bug.
    //
    // C receives the widget *floating* with refcount 1. glib-rs's
    // `from_glib_none` sinks floating references, so wrapping the
    // incoming pointer with it handed ownership to a temporary Rust
    // wrapper that dropped at the end of the call and destroyed the
    // widget — on the *first* FFI call after construction. Constructing
    // in Rust never sees this, because the wrapper holds a real
    // reference and nothing is floating.
    //
    // So: build it the way C does, call through the C ABI, and assert
    // it is still alive and still floating afterwards.
    unsafe {
        let pal = [gtk4::gdk::RGBA::BLACK; crate::view::PALETTE_COLS];
        let raw = crate::ffi::hx_chat_view_impl_new(
            pal.as_ptr() as *const gtk4::gdk::ffi::GdkRGBA,
            1,
        );
        assert!(!raw.is_null(), "impl_new returned NULL");
        let as_obj = raw as *mut gtk4::glib::gobject_ffi::GObject;
        assert_ne!(
            gtk4::glib::gobject_ffi::g_object_is_floating(as_obj),
            0,
            "impl_new must hand C a floating ref, like gtk_xtext_new"
        );

        // Every entry point create_chat calls, in order.
        crate::ffi::hx_chat_view_impl_set_font(raw, c"Monospace 10".as_ptr());
        crate::ffi::hx_chat_view_impl_set_word_wrap(raw, 1);
        crate::ffi::hx_chat_view_impl_set_max_lines(raw, 500);
        crate::ffi::hx_chat_view_impl_set_indent(raw, 1);
        crate::ffi::hx_chat_view_impl_set_time_stamp(raw, 1);
        crate::ffi::hx_chat_view_impl_set_max_indent(raw, 256);
        let _ = crate::ffi::hx_chat_view_impl_get_vadjustment(raw);
        let mark = crate::ffi::hx_chat_view_impl_append_indent(
            raw,
            c"<alice>".as_ptr(),
            7,
            c"hello".as_ptr(),
            5,
            0,
        );
        assert!(!mark.is_null(), "append_indent returned no mark");

        // Still alive, still a widget, still ours to sink.
        assert_eq!(
            (*as_obj).ref_count,
            1,
            "an FFI call leaked or dropped a reference"
        );
        assert_ne!(
            gtk4::glib::gobject_ffi::g_object_is_floating(as_obj),
            0,
            "an FFI call sank the caller's floating reference — \
             from_glib_none does this; use g_object_ref instead"
        );
        assert_ne!(
            gtk4::glib::gobject_ffi::g_type_check_instance_is_a(
                raw as *mut gtk4::glib::gobject_ffi::GTypeInstance,
                crate::ffi::hx_chat_view_impl_get_type(),
            ),
            0,
            "the widget was destroyed by an FFI call"
        );

        // Clean up the way chat.c would.
        gtk4::glib::gobject_ffi::g_object_ref_sink(as_obj);
        gtk4::glib::gobject_ffi::g_object_unref(as_obj);
    }

    // --- selection + zoom (C3) -------------------------------------
    let view = crate::view::HxChatView::new();
    view.set_font_from_string("Monospace 10");
    view.set_indent(false);
    view.append(crate::view::plain_message("alpha"));
    view.append(crate::view::plain_message("bravo"));

    assert!(!view.has_selection());
    assert_eq!(view.selected_text(), "");
    view.clear_selection(); // no-op, must not panic

    // Word and line select, through the view's own buffer.
    {
        let buf = view.imp_ref().buffer.borrow();
        let id = buf.id_at(0).expect("row 0");
        let caret = hxchat_layout::Caret {
            message: id,
            source: hxchat_layout::LineSource::Block(0),
            offset: 1,
        };
        let word = buf.select_word(&caret).expect("word select");
        assert_eq!(buf.selected_text(&word), "alpha");
        let line = buf.select_row(0).expect("row select");
        assert_eq!(buf.selected_text(&line), "alpha");
    }

    // Zoom walks a fixed ladder and returns to exactly 100%.
    assert_eq!(view.zoom_permille(), 1000);
    view.zoom_step(1);
    let zoomed = view.zoom_permille();
    assert!(zoomed > 1000, "zoom in should raise the level");
    view.zoom_step(-1);
    assert_eq!(
        view.zoom_permille(),
        1000,
        "in then out must land back on exactly 100%, not drift"
    );
    // Select All covers the buffer; Copy is a no-op with nothing
    // selected rather than a panic.
    view.select_all();
    assert!(view.has_selection(), "select_all should select something");
    assert!(view.selected_text().contains("alpha"));
    assert!(view.selected_text().contains("bravo"));
    view.clear_selection();
    assert!(!view.has_selection());

    // Clamps at both ends rather than running off the ladder.
    for _ in 0..40 {
        view.zoom_step(1);
    }
    assert!(view.zoom_permille() <= 4000);
    for _ in 0..80 {
        view.zoom_step(-1);
    }
    assert!(view.zoom_permille() >= 500);
}
