//! Tests for the GTK skin, in two tiers.
//!
//! **Always-on:** the Pango measurer. `pangocairo`'s default font map
//! works without a `GdkDisplay`, so the one part of the widget carrying real
//! logic rather than plumbing is testable anywhere. Geometry correctness
//! proper is covered in `rotulus-layout` against a deterministic
//! measurer; what these check is that the *real* measurer upholds the
//! invariants that engine assumes.
//!
//! **Display-gated:** one smoke test covering class registration and
//! widget construction, at the bottom of this file. Those need a real
//! GTK — gtk4-rs asserts an initialised GTK inside the generated
//! `class_init` itself — which is precisely why every bring-up crash
//! was invisible to `cargo test`. It no-ops without a display and runs
//! on a desktop session.

use crate::measure::PangoMeasure;
use rotulus_layout::{Attrs, Style, TextMeasure};

fn m() -> PangoMeasure {
    PangoMeasure::headless("Monospace 10")
}

/// The palette crosses the C/Rust seam as a bare `GdkRGBA *`: C hands
/// over its `colors[]` array and `ffi.rs` reads `PALETTE_COLS` entries
/// from it. Nothing at the boundary checks the length, so a slot added on
/// one side only would read past the end of the C array in silence. Read
/// the C definitions out of `rotulus.h` and hold the Rust constants to
/// them.
#[test]
fn palette_constants_match_rotulus_h() {
    let header = include_str!("../include/rotulus.h");
    let define = |name: &str| -> usize {
        let prefix = format!("#define {name} ");
        let line = header
            .lines()
            .find(|l| l.starts_with(&prefix))
            .unwrap_or_else(|| panic!("rotulus.h no longer defines {name}"));
        line[prefix.len()..]
            .split_whitespace()
            .next()
            .and_then(|v| v.parse().ok())
            .unwrap_or_else(|| panic!("{name} is not a plain number: {line:?}"))
    };
    for (name, rust) in [
        ("ROTULUS_PAL_COLS", crate::view::PALETTE_COLS),
        ("ROTULUS_PAL_FG", crate::view::PAL_FG),
        ("ROTULUS_PAL_BG", crate::view::PAL_BG),
        ("ROTULUS_PAL_MARK_FG", crate::view::PAL_MARK_FG),
        ("ROTULUS_PAL_MARK_BG", crate::view::PAL_MARK_BG),
        ("ROTULUS_PAL_MUTED", crate::view::PAL_HISTORY_MUTED),
        ("ROTULUS_PAL_MARKER", crate::view::PAL_MARKER),
        ("ROTULUS_PAL_NICK_COLOR0", crate::view::PAL_NICK_COLOR0),
        ("ROTULUS_PAL_TIMESTAMP", crate::view::PAL_TIMESTAMP),
        ("ROTULUS_PAL_RULE", crate::view::PAL_RULE),
    ] {
        assert_eq!(define(name), rust, "{name} differs between C and Rust");
    }
    // The per-nick block is the palette's tail; if it isn't, a slot was
    // added after it and the count above has to account for it.
    assert_eq!(
        define("ROTULUS_PAL_NICK_COLOR0") + define("ROTULUS_PAL_NICK_COLORS"),
        crate::view::PALETTE_COLS,
        "the per-nick colors should end the palette"
    );
}

/// The run attribute bits and row constants cross the seam as bare
/// numbers too. Hold the Rust side to the header's.
#[test]
fn abi_constants_match_rotulus_h() {
    let header = include_str!("../include/rotulus.h");
    let shift = |name: &str| -> u32 {
        let prefix = format!("#define {name} ");
        let line = header
            .lines()
            .find(|l| l.starts_with(&prefix))
            .unwrap_or_else(|| panic!("rotulus.h no longer defines {name}"));
        let v = line[prefix.len()..].trim();
        let n = v
            .trim_start_matches("(1u << ")
            .trim_end_matches(')')
            .parse::<u32>()
            .unwrap_or_else(|_| panic!("{name} is not (1u << n): {v:?}"));
        1 << n
    };
    for (name, rust) in [
        ("ROTULUS_ATTR_BOLD", crate::ffi::ATTR_BOLD),
        ("ROTULUS_ATTR_ITALIC", crate::ffi::ATTR_ITALIC),
        ("ROTULUS_ATTR_UNDERLINE", crate::ffi::ATTR_UNDERLINE),
        ("ROTULUS_ATTR_STRIKETHROUGH", crate::ffi::ATTR_STRIKETHROUGH),
        ("ROTULUS_ATTR_MONOSPACE", crate::ffi::ATTR_MONOSPACE),
        ("ROTULUS_ATTR_REVERSE", crate::ffi::ATTR_REVERSE),
        ("ROTULUS_ATTR_BACKGROUND", crate::ffi::ATTR_BACKGROUND),
        ("ROTULUS_ATTR_RGB", crate::ffi::ATTR_RGB),
        (
            "ROTULUS_ATTR_BACKGROUND_RGB",
            crate::ffi::ATTR_BACKGROUND_RGB,
        ),
    ] {
        assert_eq!(
            shift(name),
            u32::from(rust),
            "{name} differs between C and Rust"
        );
    }
    assert_eq!(shift("ROTULUS_ROW_OUTGOING"), crate::ffi::ROW_OUTGOING);
    assert_eq!(shift("ROTULUS_ROW_ACTION"), crate::ffi::ROW_ACTION);

    // The row kinds are an enum, so their values are their order.
    let kinds: Vec<&str> = header
        .lines()
        .map(str::trim)
        .filter(|l| l.starts_with("ROTULUS_ROW_") && l.ends_with(','))
        .map(|l| l.trim_end_matches(','))
        .collect();
    assert_eq!(
        kinds,
        [
            "ROTULUS_ROW_MESSAGE",
            "ROTULUS_ROW_SYSTEM",
            "ROTULUS_ROW_HISTORY",
            "ROTULUS_ROW_DIVIDER",
            "ROTULUS_ROW_LOAD_OLDER",
            "ROTULUS_ROW_LOAD_NEWER",
        ]
    );
    assert_eq!(
        [
            crate::ffi::ROW_MESSAGE,
            crate::ffi::ROW_SYSTEM,
            crate::ffi::ROW_HISTORY,
            crate::ffi::ROW_DIVIDER,
            crate::ffi::ROW_LOAD_OLDER,
            crate::ffi::ROW_LOAD_NEWER,
        ],
        [0, 1, 2, 3, 4, 5]
    );
}

/// The structs C builds on the stack. A field added on one side only
/// would shift every field after it, silently.
#[test]
fn abi_struct_layouts_are_what_c_computes() {
    use std::mem::{align_of, size_of};
    let ptr = size_of::<*const u8>();
    // text, len, color, attrs, background (+2 pad), rgb, background_rgb.
    let run = ptr + 4 + 2 + 2 + 4 + 4 + 4;
    assert_eq!(
        size_of::<crate::ffi::RotulusRun>(),
        run.next_multiple_of(align_of::<*const u8>())
    );
    assert_eq!(
        size_of::<crate::ffi::RotulusSpeaker>(),
        (8 + ptr + 4).next_multiple_of(8)
    );
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
    let b = Style {
        fg: rotulus_layout::ColorRef::Palette(4),
        ..Default::default()
    };
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
// Every bring-up crash lived in `class_init` or in widget
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
// exact call sequence an application's setup performs, which is what all three
// crashes died in.

use gtk4::glib::prelude::*;
use gtk4::glib::translate::IntoGlib;
use gtk4::prelude::*;

#[test]
fn gtk_class_and_construction_smoke() {
    // No display is a failure, not a skip.
    //
    // This used to return early with a message, which made it pass on CI
    // without running a line of it — so it read as coverage on the dashboard
    // while covering nothing, and the reference-cycle leak it exists to catch
    // sat undetected until someone ran the suite on a desktop. A skip that
    // looks like a pass is worse than no test at all; the repo has a rule
    // about this and it applies to Rust tests as much as to the C ones.
    //
    // CI now supplies a display (`xvfb-run`, see .github/workflows/tests.yml),
    // so failing here means the harness is misconfigured, which is exactly
    // what we want to hear about.
    assert!(
        gtk4::init().is_ok(),
        "GTK could not be initialised — this test needs a display. \
         Run it under `xvfb-run -a` (with GDK_BACKEND=x11), the way CI does."
    );

    let t = crate::view::RotulusView::static_type();

    // --- class_init ran at all -------------------------------------
    unsafe {
        let c = gtk4::glib::gobject_ffi::g_type_class_ref(t.into_glib());
        assert!(!c.is_null(), "class_init failed for RotulusView");
        gtk4::glib::gobject_ffi::g_type_class_unref(c);
    }

    // --- the GtkScrollable properties are installed ----------------
    //
    // Pins the third bring-up crash: declaring fresh ParamSpecs named
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
                "GtkScrollable property {name:?} is not installed on RotulusView"
            );
        }
        gtk4::glib::gobject_ffi::g_type_class_unref(class as *mut _);
    }

    // --- the typed signals are registered ---------------------------
    //
    // glib-rs's Signal::builder panics on a non-canonical name, and that
    // panic aborts out of class_init; reaching here at all says they
    // registered, and this says they are findable by the names the
    // header documents.
    unsafe {
        for name in [
            c"link-activated",
            c"link-menu",
            c"speaker-activated",
            c"speaker-menu",
            c"load-more",
            c"media-activated",
            c"selection-changed",
        ] {
            let id =
                gtk4::glib::gobject_ffi::g_signal_lookup(name.as_ptr() as *const _, t.into_glib());
            assert_ne!(id, 0, "{name:?} is not registered");
        }
        assert_eq!(
            gtk4::glib::gobject_ffi::g_signal_lookup(
                c"word-click".as_ptr() as *const _,
                t.into_glib()
            ),
            0,
            "word-click is gone"
        );
    }

    // --- a constructed view is a usable widget ---------------------
    //
    // Pins the first bring-up crash (use-after-free on the returned pointer)
    // and re-checks the third: all three produced something that failed
    // exactly this.
    let view = crate::view::RotulusView::new();
    assert!(view.is::<gtk4::Widget>(), "not a GtkWidget");
    assert!(view.is::<gtk4::Scrollable>(), "not a GtkScrollable");

    // --- it survives an application's setup calls ---------------
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
    assert!(
        !view.remove(a),
        "removing a stale mark is a no-op, not a panic"
    );
    view.clear();

    // --- the FFI path, on a floating pointer -----------------------
    //
    // The check the Rust-side construction above cannot make, and the
    // one that would have caught the longest-running bring-up bug.
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
        let raw = crate::ffi::rotulus_view_new();
        assert!(!raw.is_null(), "rotulus_view_new returned NULL");
        let as_obj = raw as *mut gtk4::glib::gobject_ffi::GObject;
        assert_ne!(
            gtk4::glib::gobject_ffi::g_object_is_floating(as_obj),
            0,
            "rotulus_view_new must hand C a floating ref, like a GTK constructor"
        );

        // The entry points an application's setup calls.
        crate::ffi::rotulus_view_set_palette(raw, pal.as_ptr() as *const gtk4::gdk::ffi::GdkRGBA);
        crate::ffi::rotulus_view_set_font(raw, c"Monospace 10".as_ptr());
        crate::ffi::rotulus_view_set_word_wrap(raw, 1);
        crate::ffi::rotulus_view_set_max_lines(raw, 500);
        crate::ffi::rotulus_view_set_indent(raw, 1);
        crate::ffi::rotulus_view_set_show_timestamps(raw, 1);
        crate::ffi::rotulus_view_set_max_indent(raw, 256);
        crate::ffi::rotulus_view_set_link_schemes(
            raw,
            [
                c"https://".as_ptr(),
                c"hotline://".as_ptr(),
                std::ptr::null(),
            ]
            .as_ptr(),
        );
        let _ = crate::ffi::rotulus_view_get_vadjustment(raw);
        let gutter = [plain_run(c"<alice>")];
        let body = [plain_run(c"hello")];
        let row = crate::ffi::RotulusRow {
            kind: crate::ffi::ROW_MESSAGE,
            flags: 0,
            stamp: 0,
            speaker: crate::ffi::RotulusSpeaker {
                key: 7,
                nick: c"alice".as_ptr(),
                nick_len: -1,
            },
            gutter: gutter.as_ptr(),
            n_gutter: 1,
            body: body.as_ptr(),
            n_body: 1,
        };
        let mark = crate::ffi::rotulus_view_append(raw, &row);
        assert!(!mark.is_null(), "append returned no mark");
        assert_eq!(crate::ffi::rotulus_view_get_last(raw), mark);
        assert_ne!(
            crate::ffi::rotulus_view_replace(raw, mark, &row),
            0,
            "a live row can be replaced"
        );
        crate::ffi::rotulus_view_set_marker(raw, mark);

        // Still alive, still a widget, still ours to sink.
        //
        // A count above 1 means something took a reference and never gave it
        // back. The usual cause is a closure that captured a strong clone of
        // the view and was then handed to an event controller the view itself
        // owns — a cycle, so the view can never be finalized and its whole
        // message buffer outlives the window. That is what this caught: the
        // gesture and shortcut handlers installed in `constructed` accounted
        // for seventeen of them, and the adjustment's value-changed handler
        // for one more. Weak captures throughout are the fix.
        assert_eq!(
            (*as_obj).ref_count,
            1,
            "something took a reference to the view and never released it — \
             look for a strong self.clone() captured into a closure the view \
             transitively owns"
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
                crate::ffi::rotulus_view_get_type(),
            ),
            0,
            "the widget was destroyed by an FFI call"
        );

        // Clean up the way a C caller would — and check the view actually dies.
        //
        // The refcount assertion above is a proxy for the property that
        // matters, and a proxy a future cycle could satisfy by accident (drop
        // a reference somewhere else and the arithmetic works out again). So
        // watch the object through a weak pointer, which GLib clears in
        // `finalize`, and require it to have been cleared. That fails on any
        // cycle, whatever the count happened to read.
        let mut watch: *mut std::ffi::c_void = as_obj as *mut _;
        gtk4::glib::gobject_ffi::g_object_add_weak_pointer(as_obj, &mut watch);
        gtk4::glib::gobject_ffi::g_object_ref_sink(as_obj);
        gtk4::glib::gobject_ffi::g_object_unref(as_obj);
        assert!(
            watch.is_null(),
            "the view survived its last unref — something still holds a \
             reference to it, so every chat view ever opened leaks along \
             with its message buffer"
        );
    }

    // --- selection + zoom -------------------------------------------
    let view = crate::view::RotulusView::new();
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
        let caret = rotulus_layout::Caret {
            message: id,
            source: rotulus_layout::LineSource::Block(0),
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

    // --- a following append must land at the true bottom -----------
    //
    // The reported bug: with a multi-line message the view stopped just
    // short of the bottom and the newest row was clipped off the edge.
    //
    // Two independent causes, so this runs twice.
    //
    // `estimate_height` divided a body's byte length by a column count
    // to guess how many lines it wrapped to, which answers the wrong
    // question for a body containing hard newlines: a five-line message
    // was estimated at one. And the view read the scroll offset *before*
    // the frame's layout pass, when following the bottom means
    // `total - viewport` and the layout pass is exactly what corrects
    // the total. Either alone puts the newest row below the bottom edge.
    //
    // The second case is the one no estimator can fix. A proportional
    // font's space is far narrower than its average glyph, so the column
    // count is wildly optimistic and any long line under-counts — and
    // the layout engine deliberately has no font stack to know better.
    //
    // Its own widgets, not the ones above: this asserts on live
    // adjustment state, and reusing a view that earlier checks have
    // zoomed, selected and cleared would make the result depend on what
    // they happened to leave behind.
    for (font, body, case) in [
        (
            "Monospace 10",
            "one\ntwo\nthree\nfour\nfive",
            "hard newlines",
        ),
        ("Sans 10", &"WWWWWWWWWW ".repeat(12), "a proportional font"),
    ] {
        let view = crate::view::RotulusView::new();
        view.set_font_from_string(font);
        view.set_indent(false);
        view.set_word_wrap(true);

        let adj = gtk4::Adjustment::new(0.0, 0.0, 1.0, 1.0, 1.0, 1.0);
        view.set_vadjustment(Some(&adj));

        let holder = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
        holder.append(&view);
        // Drives the real `size_allocate`, which is what tells the
        // buffer its width and first configures the adjustment.
        holder.allocate(400, 200, -1, None);

        // The `snapshot` vfunc, called directly on the private impl
        // rather than through `gtk_widget_snapshot_child`. GTK skips an
        // unmapped widget, and mapping one means a real window and a
        // frame clock — an asynchronous dependency in a test asking a
        // synchronous question. The vfunc is the same code GTK runs.
        //
        // Followed by the layout pass the next frame runs: a paint that
        // corrects the scroll position hands the new extent to the
        // adjustment through `size_allocate`, never from the paint
        // itself, which is what the draw-time check below pins.
        let frame = || {
            use gtk4::subclass::prelude::WidgetImpl;
            let before = (adj.upper(), adj.value(), adj.page_size());
            view.imp_ref().snapshot(&gtk4::Snapshot::new());
            assert_eq!(
                before,
                (adj.upper(), adj.value(), adj.page_size()),
                "the paint reconfigured the adjustment; that re-lays out the \
                 scrollbar mid-frame and GTK draws its slider unallocated"
            );
            holder.allocate(400, 200, -1, None);
        };

        for i in 0..60 {
            view.append(crate::view::plain_message(&format!("line {i}")));
        }
        // One frame, so everything already in the buffer is measured —
        // the state a live window is actually in when a message lands.
        frame();
        assert!(
            view.imp_ref().buffer.borrow().is_following(),
            "the view should still be following the bottom"
        );

        view.append(crate::view::plain_message(body));
        frame();

        let bottom = {
            let mut buf = view.imp_ref().buffer.borrow_mut();
            let last = buf.len() - 1;
            let top = buf.index_mut().offset_of(last);
            top + u64::from(buf.index_mut().height_at(last))
        };
        let viewport_end = (adj.value() + adj.page_size()) as u64;
        assert!(
            bottom <= viewport_end,
            "with {case}: the content ends at {bottom}px but the viewport \
             ends at {viewport_end}px — the newest row hangs {}px below \
             the bottom edge",
            bottom.saturating_sub(viewport_end)
        );
        assert!(
            (adj.value() - (adj.upper() - adj.page_size())).abs() < 1.0,
            "with {case}: following the bottom means value == upper - page; \
             got value {} against upper {} page {}",
            adj.value(),
            adj.upper(),
            adj.page_size()
        );
    }

    check_offscreen_animation_stops();
    check_typed_signals();
    check_replace_and_marker();
}

/// A row of runs, the way an application appends one.
fn message_row(key: u64, nick: &str, body: &str) -> rotulus_layout::Message {
    rotulus_layout::Message {
        kind: rotulus_layout::MessageKind::Live,
        timestamp: 1_000,
        speaker: (key != 0).then(|| rotulus_layout::Speaker::new(key, nick)),
        gutter: Some(rotulus_layout::ParsedText::plain(format!("<{nick}>"))),
        blocks: {
            let mut p = rotulus_layout::ParsedText::plain(body);
            crate::links::autolink(&mut p, &rotulus_layout::Linkifier::default());
            vec![rotulus_layout::Block::Text(p)].into()
        },
        flags: rotulus_layout::MessageFlags::NONE,
    }
}

/// Lay a view out at 400x300 and paint it once, so hit-testing has line
/// boxes to find.
fn laid_out(view: &crate::view::RotulusView) -> gtk4::Box {
    use gtk4::subclass::prelude::WidgetImpl;
    let adj = gtk4::Adjustment::new(0.0, 0.0, 1.0, 1.0, 1.0, 1.0);
    view.set_vadjustment(Some(&adj));
    let holder = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
    holder.append(view);
    holder.allocate(400, 300, -1, None);
    view.imp_ref().snapshot(&gtk4::Snapshot::new());
    holder.allocate(400, 300, -1, None);
    holder
}

/// The first widget-space point on row `row` whose hover target `want`
/// accepts.
fn find_point(
    view: &crate::view::RotulusView,
    row: usize,
    want: impl Fn(&crate::view::HoverTarget) -> bool,
) -> Option<(f64, f64)> {
    let (top, height) = {
        let mut buf = view.imp_ref().buffer.borrow_mut();
        (
            buf.index_mut().offset_of(row),
            buf.index_mut().height_at(row),
        )
    };
    let y0 = f64::from(crate::view::PAD_Y) + top as f64;
    for dy in (1..height).step_by(4) {
        for x in (crate::view::PAD_X..396).step_by(3) {
            let (x, y) = (f64::from(x), y0 + f64::from(dy));
            if view.hover_target_at(x, y).as_ref().is_some_and(&want) {
                return Some((x, y));
            }
        }
    }
    None
}

/// Clicks reach the application as typed signals, not as words to match.
fn check_typed_signals() {
    use crate::view::HoverTarget;
    use std::cell::RefCell;
    use std::rc::Rc;

    let view = crate::view::RotulusView::new();
    view.set_indent(true);
    view.append(message_row(7, "al", "see https://example.com now"));
    view.append(rotulus_layout::Message {
        kind: rotulus_layout::MessageKind::LoadMore(rotulus_layout::LoadMoreDirection::Older),
        timestamp: 1_000,
        speaker: None,
        gutter: None,
        blocks: vec![rotulus_layout::Block::text("load older")].into(),
        flags: rotulus_layout::MessageFlags::NONE,
    });
    view.append(rotulus_layout::Message {
        kind: rotulus_layout::MessageKind::Live,
        timestamp: 1_000,
        speaker: None,
        gutter: None,
        blocks: vec![rotulus_layout::Block::Image {
            token: 9,
            size: None,
            alt: "[image]".into(),
        }]
        .into(),
        flags: rotulus_layout::MessageFlags::NONE,
    });
    let _holder = laid_out(&view);

    let seen: Rc<RefCell<Vec<String>>> = Rc::default();
    {
        let seen = seen.clone();
        // Handled, so the view doesn't go on to launch a browser.
        view.connect_closure(
            "link-activated",
            false,
            gtk4::glib::closure_local!(move |_: crate::view::RotulusView, href: String| -> bool {
                seen.borrow_mut().push(format!("link {href}"));
                true
            }),
        );
    }
    {
        let seen = seen.clone();
        view.connect_closure(
            "speaker-activated",
            false,
            gtk4::glib::closure_local!(move |_: crate::view::RotulusView, key: u64| {
                seen.borrow_mut().push(format!("speaker {key}"));
            }),
        );
    }
    {
        let seen = seen.clone();
        view.connect_closure(
            "load-more",
            false,
            gtk4::glib::closure_local!(
                move |_: crate::view::RotulusView, d: crate::view::LoadDirection| {
                    seen.borrow_mut().push(format!("load {d:?}"));
                }
            ),
        );
    }
    {
        let seen = seen.clone();
        view.connect_closure(
            "media-activated",
            false,
            gtk4::glib::closure_local!(move |_: crate::view::RotulusView, token: u32| {
                seen.borrow_mut().push(format!("media {token}"));
            }),
        );
    }

    let link = find_point(&view, 0, |t| matches!(t, HoverTarget::Link { .. }))
        .expect("the link is hittable");
    view.activate_at(link.0, link.1);
    let nick = find_point(&view, 0, |t| matches!(t, HoverTarget::Nick { key: 7, .. }))
        .expect("the nick is hittable");
    view.activate_at(nick.0, nick.1);
    let more = find_point(&view, 1, |t| matches!(t, HoverTarget::LoadMore { .. }))
        .expect("the load-more row is hittable");
    view.activate_at(more.0, more.1);
    let media = find_point(&view, 2, |t| {
        matches!(t, HoverTarget::Media { token: 9, .. })
    })
    .expect("the image is hittable");
    view.activate_at(media.0, media.1);
    assert_eq!(
        *seen.borrow(),
        [
            "link https://example.com",
            "speaker 7",
            "load Older",
            "media 9"
        ]
    );

    // With activation off, a primary click on the link is just a click.
    view.set_activate_links(false);
    view.activate_at(link.0, link.1);
    assert_eq!(
        seen.borrow().len(),
        4,
        "a link doesn't activate when activation is off"
    );
    view.set_activate_links(true);

    // A link whose label isn't its address opens the menu instead, so the
    // real destination is on screen before anything opens.
    {
        let seen = seen.clone();
        view.connect_closure(
            "link-menu",
            false,
            gtk4::glib::closure_local!(move |_: crate::view::RotulusView,
                                             href: String,
                                             _x: f64,
                                             _y: f64|
                  -> bool {
                seen.borrow_mut().push(format!("menu {href}"));
                true
            }),
        );
    }
    view.append(rotulus_layout::Message {
        kind: rotulus_layout::MessageKind::Live,
        timestamp: 1_000,
        speaker: None,
        gutter: None,
        blocks: rotulus_layout::Block::Text(rotulus_layout::markdown::parse_inline(
            "[the docs](https://elsewhere.example)",
        ))
        .into(),
        flags: rotulus_layout::MessageFlags::NONE,
    });
    let _holder = laid_out(&view);
    let last = view.imp_ref().buffer.borrow().len() - 1;
    let label = find_point(&view, last, |t| {
        matches!(
            t,
            HoverTarget::Link {
                disguised: true,
                ..
            }
        )
    })
    .expect("the labeled link is hittable");
    view.activate_at(label.0, label.1);
    assert_eq!(
        seen.borrow().last().map(String::as_str),
        Some("menu https://elsewhere.example"),
        "a disguised link shows its menu rather than opening"
    );

    // Selection changes are announced, and has-selection follows them.
    let changes = Rc::new(std::cell::Cell::new(0));
    {
        let changes = changes.clone();
        view.connect_closure(
            "selection-changed",
            false,
            gtk4::glib::closure_local!(move |_: crate::view::RotulusView| {
                changes.set(changes.get() + 1);
            }),
        );
    }
    let notified = Rc::new(std::cell::Cell::new(0));
    {
        let notified = notified.clone();
        view.connect_notify_local(Some("has-selection"), move |_, _| {
            notified.set(notified.get() + 1)
        });
    }
    view.select_all();
    assert!(view.property::<bool>("has-selection"));
    view.clear_selection();
    assert!(!view.property::<bool>("has-selection"));
    assert_eq!((changes.get(), notified.get()), (2, 2));
}

/// Replacing keeps a row's place and id; the marker follows its row out.
fn check_replace_and_marker() {
    let view = crate::view::RotulusView::new();
    let a = view.append(message_row(1, "al", "draft"));
    let b = view.append(message_row(2, "bo", "reply"));
    assert!(view.replace(a, message_row(1, "al", "final text")));
    assert_eq!(view.last(), Some(b), "a replace doesn't move the row");
    {
        let buf = view.imp_ref().buffer.borrow();
        assert_eq!(buf.row_of(a), Some(0));
        assert_eq!(buf.message(a).unwrap().to_plain_text(), "final text");
    }
    view.set_marker(Some(a));
    assert_eq!(view.marker(), Some(a));
    assert!(view.remove(a));
    assert_eq!(view.marker(), None, "the marker goes with its row");
    assert!(
        !view.replace(a, message_row(1, "al", "late")),
        "a stale mark replaces nothing"
    );

    // Properties round-trip, including the ones the setters wrap.
    view.set_link_schemes(&["hotline://"]);
    assert_eq!(view.property::<Vec<String>>("link-schemes"), ["hotline://"]);
    view.set_link_schemes(&[]);
    assert!(view
        .property::<Vec<String>>("link-schemes")
        .contains(&"https://".to_string()));
    view.set_zoom_permille(1250);
    assert!((view.property::<f64>("zoom") - 1.25).abs() < 1e-9);
    view.set_stamp_format("");
    assert_eq!(
        view.property::<String>("timestamp-format"),
        crate::view::DEFAULT_STAMP_FORMAT
    );

    #[cfg(feature = "v4_14")]
    {
        let text = view
            .with_a11y(|m, _| Some(m.contents(0, u32::MAX).to_string()))
            .expect("the accessible text builds");
        assert_eq!(text, "<bo> reply");
        view.append(message_row(3, "cy", "new"));
        let text = view
            .with_a11y(|m, _| Some(m.contents(0, u32::MAX).to_string()))
            .unwrap();
        assert_eq!(
            text, "<bo> reply\n<cy> new",
            "an append extends the accessible text"
        );
    }
}

/// An animated image drawn on screen runs the frame tick; scrolled out of
/// view, the next paint stops it, and scrolled back, the paint after that
/// starts it again. Unmapping stops it too. Part of the one display-backed
/// test above.
fn check_offscreen_animation_stops() {
    use gtk4::prelude::*;
    use gtk4::subclass::prelude::WidgetImpl;

    let view = crate::view::RotulusView::new();
    let adj = gtk4::Adjustment::new(0.0, 0.0, 1.0, 1.0, 1.0, 1.0);
    view.set_vadjustment(Some(&adj));
    let holder = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
    holder.append(&view);
    holder.allocate(400, 200, -1, None);
    let frame = || {
        view.imp_ref().snapshot(&gtk4::Snapshot::new());
        holder.allocate(400, 200, -1, None);
    };
    let ticking = || view.imp_ref().anim_tick.borrow().is_some();

    const TOKEN: u32 = 7;
    view.append(rotulus_layout::Message {
        kind: rotulus_layout::MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: None,
        blocks: vec![rotulus_layout::Block::Image {
            token: TOKEN,
            size: None,
            alt: "gif".into(),
        }]
        .into(),
        flags: rotulus_layout::MessageFlags::NONE,
    });
    let texture = |v: u8| -> gtk4::gdk::Texture {
        gtk4::gdk::MemoryTexture::new(
            8,
            8,
            gtk4::gdk::MemoryFormat::R8g8b8a8,
            &gtk4::glib::Bytes::from_owned(vec![v; 8 * 8 * 4]),
            8 * 4,
        )
        .upcast()
    };
    view.set_media_frames(TOKEN, vec![(texture(10), 100), (texture(200), 100)]);
    frame();
    assert!(ticking(), "an animated image on screen should run the tick");

    for i in 0..200 {
        view.append(crate::view::plain_message(&format!("line {i}")));
    }
    frame();
    frame();
    assert!(
        !view.imp_ref().drawn_media.borrow().contains(&TOKEN),
        "the image should be out of view by now"
    );
    assert!(
        !ticking(),
        "an animation scrolled out of view must not keep the tick running"
    );

    view.scroll_to_extreme(false);
    frame();
    frame();
    assert!(
        ticking(),
        "scrolled back into view, it should animate again"
    );

    // Hidden — a tab switched away — no snapshot runs to say the image
    // left the screen; unmapping has to.
    view.imp_ref().unmap();
    assert!(
        !ticking(),
        "an unmapped view must not keep the tick running"
    );
    frame();
    assert!(ticking(), "the next paint should start it again");
}

// ---- run-based append ------------------------------------------------

/// Build a C run array from Rust and hand it to the FFI converter.
///
/// Headless: `runs_to_text` touches no GTK, which is the point of
/// keeping the conversion separate from the widget.
/// A run with only the four common fields set, as C writes them.
fn run_of(
    text: *const std::ffi::c_char,
    len: std::ffi::c_int,
    color: i16,
    attrs: u16,
) -> crate::ffi::RotulusRun {
    crate::ffi::RotulusRun {
        text,
        len,
        color,
        attrs,
        background: 0,
        rgb: 0,
        background_rgb: 0,
    }
}

fn plain_run(t: &'static std::ffi::CStr) -> crate::ffi::RotulusRun {
    run_of(t.as_ptr(), -1, -1, 0)
}

fn to_text(runs: &[(&str, i16, u16)]) -> rotulus_layout::ParsedText {
    let cstrings: Vec<std::ffi::CString> = runs
        .iter()
        .map(|(t, _, _)| std::ffi::CString::new(*t).unwrap())
        .collect();
    let c_runs: Vec<crate::ffi::RotulusRun> = runs
        .iter()
        .zip(&cstrings)
        .map(|((t, color, attrs), cs)| {
            run_of(cs.as_ptr(), t.len() as std::ffi::c_int, *color, *attrs)
        })
        .collect();
    unsafe { crate::ffi::runs_to_text(c_runs.as_ptr(), c_runs.len() as std::ffi::c_int) }
}

#[test]
fn plain_runs_produce_no_spans() {
    // An unstyled row must come out span-free, not one-span-per-run.
    // Otherwise every ordinary chat line carries a span list the
    // renderer then has to walk, and `draw_runs` splits text it has no
    // reason to split.
    let p = to_text(&[("hello ", -1, 0), ("world", -1, 0)]);
    assert_eq!(p.text, "hello world");
    assert!(p.spans.is_empty(), "plain runs must not create spans");
}

#[test]
fn styled_runs_carry_byte_ranges_into_the_joined_text() {
    // The nick-bracket shape: bracket coloured, name default, bracket
    // coloured. This is what used to be
    // "\003NN<\003name\003NN>\003" and had to be re-parsed to find
    // where the name began.
    let p = to_text(&[("<", 5, 0), ("alice", -1, 0), (">", 5, 0)]);
    assert_eq!(p.text, "<alice>");
    assert_eq!(p.spans.len(), 2, "only the styled runs get spans");
    assert_eq!(&p.text[p.spans[0].range.clone()], "<");
    assert_eq!(&p.text[p.spans[1].range.clone()], ">");
    assert_eq!(p.spans[0].style.fg, rotulus_layout::ColorRef::Palette(5));
    assert_eq!(p.spans[1].style.fg, rotulus_layout::ColorRef::Palette(5));
}

#[test]
fn run_ranges_survive_multibyte_text() {
    // Ranges are byte offsets into the joined string, so a multi-byte
    // run before a styled one must not shift it.
    let p = to_text(&[("héllo ", -1, 0), ("wörld", 3, 1)]);
    assert_eq!(p.text, "héllo wörld");
    assert_eq!(p.spans.len(), 1);
    assert_eq!(&p.text[p.spans[0].range.clone()], "wörld");
    assert!(p.spans[0].style.attrs.contains(rotulus_layout::Attrs::BOLD));
}

#[test]
fn empty_runs_are_skipped_not_recorded() {
    let p = to_text(&[("", 4, 0), ("text", -1, 0), ("", 4, 1)]);
    assert_eq!(p.text, "text");
    assert!(p.spans.is_empty());
}

#[test]
fn a_run_with_len_minus_one_uses_strlen() {
    // rotulus.h documents len as "bytes, or -1 for strlen". cslice
    // treats anything <= 0 as empty, so -1 silently dropped the run —
    // an ABI the header promised and the implementation didn't keep.
    let cs = std::ffi::CString::new("hello").unwrap();
    let runs = [run_of(cs.as_ptr(), -1, -1, 0)];
    let p = unsafe { crate::ffi::runs_to_text(runs.as_ptr(), 1) };
    assert_eq!(p.text, "hello");
}

#[test]
fn a_speakers_nick_is_length_delimited_not_nul_delimited() {
    // The chat path hands over a slice into the middle of the received
    // line: the name is bytes [0,5) of "misha:  hello world". Reading to
    // the NUL would take the colon and the body too — and that string
    // feeds the gutter-width estimate in wrap.rs, so it would have
    // reserved a column wide enough for the whole message.
    let line = std::ffi::CString::new("misha:  hello world").unwrap();
    let sp = crate::ffi::RotulusSpeaker {
        key: 7,
        nick: line.as_ptr(),
        nick_len: 5,
    };
    let got = unsafe { crate::ffi::speaker_of(&sp) }.expect("key 7 is known");
    assert_eq!(got.nick, "misha");
    assert_eq!(got.key, 7);

    // -1 still means "this really is a C string".
    let whole = crate::ffi::RotulusSpeaker {
        key: 7,
        nick: line.as_ptr(),
        nick_len: -1,
    };
    let got = unsafe { crate::ffi::speaker_of(&whole) }.unwrap();
    assert_eq!(got.nick, "misha:  hello world");
}

// ---- markdown rendering ---------------------------------------------

/// Build a message body from one plain run, the way live chat does.
fn body_of(text: &str, markdown: bool) -> Vec<rotulus_layout::Block> {
    let cs = std::ffi::CString::new(text).unwrap();
    let runs = [run_of(cs.as_ptr(), text.len() as std::ffi::c_int, -1, 0)];
    unsafe {
        crate::ffi::body_blocks(
            runs.as_ptr(),
            1,
            markdown,
            &rotulus_layout::Linkifier::default(),
        )
    }
}

fn text_of(b: &rotulus_layout::Block) -> &rotulus_layout::ParsedText {
    match b {
        rotulus_layout::Block::Text(p) | rotulus_layout::Block::Quote { content: p, .. } => p,
        _ => panic!("not a text block"),
    }
}

#[test]
fn markdown_renders_inline_emphasis() {
    let blocks = body_of("look at **this** and *that*", true);
    assert_eq!(blocks.len(), 1);
    let p = text_of(&blocks[0]);
    assert_eq!(p.text, "look at this and that");
    let styled: Vec<_> = p
        .spans
        .iter()
        .map(|s| (&p.text[s.range.clone()], s.style.attrs))
        .collect();
    assert_eq!(
        styled,
        vec![
            ("this", rotulus_layout::Attrs::BOLD),
            ("that", rotulus_layout::Attrs::ITALIC),
        ]
    );
}

#[test]
fn markdown_off_leaves_the_delimiters_alone() {
    let blocks = body_of("look at **this**", false);
    let p = text_of(&blocks[0]);
    assert_eq!(p.text, "look at **this**", "text is untouched");
    assert!(p.spans.is_empty());
}

#[test]
fn a_fenced_block_becomes_an_inert_code_block() {
    let blocks = body_of("see:\n```\nlet x = **not bold**;\n```\ndone", true);
    assert_eq!(blocks.len(), 3, "paragraph, code, paragraph");
    match &blocks[1] {
        rotulus_layout::Block::Code { text, .. } => {
            assert!(
                text.contains("**not bold**"),
                "code contents stay literal: {text:?}"
            );
        }
        other => panic!("expected a code block, got {other:?}"),
    }
}

#[test]
fn a_quote_becomes_a_quote_block_with_its_markers_gone() {
    let blocks = body_of("> quoted **bold**", true);
    match &blocks[0] {
        rotulus_layout::Block::Quote { content, depth } => {
            assert_eq!(content.text, "quoted bold");
            assert_eq!(*depth, 1);
        }
        other => panic!("expected a quote, got {other:?}"),
    }
}

#[test]
fn a_styled_body_keeps_its_colour_under_the_markdown() {
    // History rows arrive muted. The renderer treats a gap between spans
    // as *default* style, so without laying the base colour under the
    // parse, a muted line would come back with only its bold words muted
    // and everything else at full contrast.
    let text = "muted **bold** tail";
    let cs = std::ffi::CString::new(text).unwrap();
    let runs = [run_of(cs.as_ptr(), text.len() as std::ffi::c_int, 37, 0)]; // ROTULUS_PAL_MUTED
    let blocks = unsafe {
        crate::ffi::body_blocks(
            runs.as_ptr(),
            1,
            true,
            &rotulus_layout::Linkifier::default(),
        )
    };
    let p = text_of(&blocks[0]);
    assert_eq!(p.text, "muted bold tail");

    // The spans must *tile* the text: start at 0, be contiguous, and
    // reach the end, every one of them carrying the muted colour.
    //
    // Tiling is the property that matters, and asserting it directly is
    // better than sampling positions. The renderer draws a gap between
    // spans in the *default* style, so a single uncovered byte is a
    // visibly unmuted stretch of a muted row — and a per-character loop
    // (which this was) cannot see a gap that falls inside a character
    // anyway.
    let mut spans: Vec<_> = p.spans.iter().collect();
    spans.sort_by_key(|s| s.range.start);
    let mut at = 0usize;
    for s in &spans {
        assert_eq!(
            s.range.start,
            at,
            "gap or overlap before {:?} — the row would draw unmuted there",
            &p.text[s.range.clone()]
        );
        assert_eq!(
            s.style.fg,
            rotulus_layout::ColorRef::Palette(37),
            "{:?} lost the row colour",
            &p.text[s.range.clone()]
        );
        at = s.range.end;
    }
    assert_eq!(at, p.text.len(), "the tail of the row is uncovered");

    // ...and the emphasis is still there on top.
    assert!(p.spans.iter().any(|s| &p.text[s.range.clone()] == "bold"
        && s.style.attrs.contains(rotulus_layout::Attrs::BOLD)));
}

#[test]
fn the_base_colour_tiles_across_multibyte_text() {
    // The tiling above is only interesting if it survives text where a
    // character is several bytes: `under` splices around span
    // boundaries, and getting that wrong on a multi-byte boundary is
    // both a wrong render and a potential panic.
    let text = "héllo **wörld** ☃";
    let cs = std::ffi::CString::new(text).unwrap();
    let runs = [run_of(cs.as_ptr(), text.len() as std::ffi::c_int, 37, 0)];
    let blocks = unsafe {
        crate::ffi::body_blocks(
            runs.as_ptr(),
            1,
            true,
            &rotulus_layout::Linkifier::default(),
        )
    };
    let p = text_of(&blocks[0]);
    assert_eq!(p.text, "héllo wörld ☃");

    let mut spans: Vec<_> = p.spans.iter().collect();
    spans.sort_by_key(|s| s.range.start);
    let mut at = 0usize;
    for s in &spans {
        assert_eq!(s.range.start, at);
        assert!(p.text.is_char_boundary(s.range.start));
        assert!(p.text.is_char_boundary(s.range.end));
        at = s.range.end;
    }
    assert_eq!(at, p.text.len());
}

#[test]
fn a_body_the_caller_styled_run_by_run_is_left_alone() {
    // Chrome — a divider, a "[hx]" line — is styled deliberately by the
    // caller. Re-parsing it would fight that, so a non-uniform body opts
    // out of markdown entirely.
    let a = std::ffi::CString::new("plain ").unwrap();
    let b = std::ffi::CString::new("**loud**").unwrap();
    let runs = [run_of(a.as_ptr(), 6, -1, 0), run_of(b.as_ptr(), 8, 4, 0)];
    let blocks = unsafe {
        crate::ffi::body_blocks(
            runs.as_ptr(),
            2,
            true,
            &rotulus_layout::Linkifier::default(),
        )
    };
    assert_eq!(blocks.len(), 1);
    assert_eq!(text_of(&blocks[0]).text, "plain **loud**");
}

#[test]
fn a_one_line_fence_reaches_the_view_as_a_code_block() {
    // The reported bug end-to-end: "```hello world```" rendered as a
    // blank row, because the scanner read it as an unterminated fence
    // and produced an empty block.
    let blocks = body_of("```hello world```", true);
    assert_eq!(blocks.len(), 1);
    match &blocks[0] {
        rotulus_layout::Block::Code { text, .. } => assert_eq!(text, "hello world"),
        other => panic!("expected code, got {other:?}"),
    }
}

#[test]
fn inline_code_carries_the_code_attr_for_the_renderer_to_tint() {
    // `code` used to be indistinguishable from plain text, because the
    // chat font is already monospace and CODE only set the family. The
    // parse has to at least *mark* it so the draw path can tint it.
    let blocks = body_of("try `ls -l` now", true);
    let p = text_of(&blocks[0]);
    assert_eq!(p.text, "try ls -l now");
    assert!(
        p.spans.iter().any(|s| &p.text[s.range.clone()] == "ls -l"
            && s.style.attrs.contains(rotulus_layout::Attrs::CODE)),
        "the code span must be marked: {:?}",
        p.spans
    );
}

#[test]
fn extended_run_fields_take_effect_only_under_their_bits() {
    let cs = std::ffi::CString::new("x").unwrap();
    let mut r = run_of(cs.as_ptr(), 1, 3, 0);
    r.background = 5;
    r.rgb = 0x123456;
    let plain = crate::ffi::run_style(&r);
    assert_eq!(plain.fg, rotulus_layout::ColorRef::Palette(3));
    assert_eq!(
        plain.bg,
        rotulus_layout::ColorRef::Default,
        "no BACKGROUND bit, no background"
    );

    r.attrs = crate::ffi::ATTR_BACKGROUND | crate::ffi::ATTR_RGB | crate::ffi::ATTR_REVERSE;
    let styled = crate::ffi::run_style(&r);
    assert_eq!(styled.fg, rotulus_layout::ColorRef::Rgb(0x123456));
    assert_eq!(styled.bg, rotulus_layout::ColorRef::Palette(5));
    assert!(styled.attrs.contains(rotulus_layout::Attrs::REVERSE));
}

#[test]
fn a_view_links_its_own_schemes() {
    let only = rotulus_layout::Linkifier::new(["hotline://"]);
    let text = "hotline://example.org and https://example.com";
    let cs = std::ffi::CString::new(text).unwrap();
    let runs = [run_of(cs.as_ptr(), text.len() as std::ffi::c_int, -1, 0)];
    let blocks = unsafe { crate::ffi::body_blocks(runs.as_ptr(), 1, true, &only) };
    let p = text_of(&blocks[0]);
    let hrefs: Vec<_> = p.links.iter().map(|l| l.href.as_str()).collect();
    assert_eq!(hrefs, ["hotline://example.org"]);
}

#[cfg(feature = "mirc")]
#[test]
fn mirc_runs_point_into_the_callers_text() {
    let text = "a \x02bold\x02 \x0304red";
    let cs = std::ffi::CString::new(text).unwrap();
    let mut n = 0;
    let runs = unsafe { crate::mirc_ffi::rotulus_mirc_parse(cs.as_ptr(), -1, &mut n) };
    assert_eq!(n, 4);
    let joined = unsafe { crate::ffi::runs_to_text(runs, n) };
    assert_eq!(joined.text, "a bold red");
    let styles: Vec<_> = joined
        .spans
        .iter()
        .map(|s| (&joined.text[s.range.clone()], s.style))
        .collect();
    assert!(styles
        .iter()
        .any(|(t, s)| *t == "bold" && s.attrs.contains(rotulus_layout::Attrs::BOLD)));
    assert!(styles
        .iter()
        .any(|(t, s)| *t == "red" && s.fg == rotulus_layout::ColorRef::Palette(4)));
    unsafe { gtk4::glib::ffi::g_free(runs as *mut _) };
}
