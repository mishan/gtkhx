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
    let b = Style {
        fg: hxchat_layout::ColorRef::Palette(4),
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
        let raw = crate::ffi::hx_chat_view_new(
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
        crate::ffi::hx_chat_view_set_font(raw, c"Monospace 10".as_ptr());
        crate::ffi::hx_chat_view_set_word_wrap(raw, 1);
        crate::ffi::hx_chat_view_set_max_lines(raw, 500);
        crate::ffi::hx_chat_view_set_indent(raw, 1);
        crate::ffi::hx_chat_view_set_time_stamp(raw, 1);
        crate::ffi::hx_chat_view_set_max_indent(raw, 256);
        let _ = crate::ffi::hx_chat_view_get_vadjustment(raw);
        let mark = crate::ffi::hx_chat_view_append_indent(
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
                crate::ffi::hx_chat_view_get_type(),
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

// ---- run-based append (C6) ------------------------------------------

/// Build a C run array from Rust and hand it to the FFI converter.
///
/// Headless: `runs_to_text` touches no GTK, which is the point of
/// keeping the conversion separate from the widget.
fn to_text(runs: &[(&str, i16, u16)]) -> hxchat_layout::ParsedText {
    let cstrings: Vec<std::ffi::CString> = runs
        .iter()
        .map(|(t, _, _)| std::ffi::CString::new(*t).unwrap())
        .collect();
    let c_runs: Vec<crate::ffi::HxChatRun> = runs
        .iter()
        .zip(&cstrings)
        .map(|((t, color, attrs), cs)| crate::ffi::HxChatRun {
            text: cs.as_ptr(),
            len: t.len() as std::ffi::c_int,
            color: *color,
            attrs: *attrs,
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
    assert_eq!(p.spans[0].style.fg, hxchat_layout::ColorRef::Palette(5));
    assert_eq!(p.spans[1].style.fg, hxchat_layout::ColorRef::Palette(5));
}

#[test]
fn run_ranges_survive_multibyte_text() {
    // Ranges are byte offsets into the joined string, so a multi-byte
    // run before a styled one must not shift it.
    let p = to_text(&[("héllo ", -1, 0), ("wörld", 3, 1)]);
    assert_eq!(p.text, "héllo wörld");
    assert_eq!(p.spans.len(), 1);
    assert_eq!(&p.text[p.spans[0].range.clone()], "wörld");
    assert!(p.spans[0].style.attrs.contains(hxchat_layout::Attrs::BOLD));
}

#[test]
fn empty_runs_are_skipped_not_recorded() {
    let p = to_text(&[("", 4, 0), ("text", -1, 0), ("", 4, 1)]);
    assert_eq!(p.text, "text");
    assert!(p.spans.is_empty());
}

#[test]
fn a_run_with_len_minus_one_uses_strlen() {
    // chat_view.h documents len as "bytes, or -1 for strlen". cslice
    // treats anything <= 0 as empty, so -1 silently dropped the run —
    // an ABI the header promised and the implementation didn't keep.
    let cs = std::ffi::CString::new("hello").unwrap();
    let runs = [crate::ffi::HxChatRun {
        text: cs.as_ptr(),
        len: -1,
        color: -1,
        attrs: 0,
    }];
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
    let sp = crate::ffi::HxChatSpeaker {
        uid: 7,
        nick: line.as_ptr(),
        nick_len: 5,
        outgoing: 0,
    };
    let got = unsafe { crate::ffi::speaker_of(&sp) }.expect("uid 7 is known");
    assert_eq!(got.nick, "misha");
    assert_eq!(got.uid, 7);

    // -1 still means "this really is a C string".
    let whole = crate::ffi::HxChatSpeaker {
        uid: 7,
        nick: line.as_ptr(),
        nick_len: -1,
        outgoing: 0,
    };
    let got = unsafe { crate::ffi::speaker_of(&whole) }.unwrap();
    assert_eq!(got.nick, "misha:  hello world");
}

// ---- markdown rendering ---------------------------------------------

/// Build a message body from one plain run, the way live chat does.
fn body_of(text: &str, markdown: bool) -> Vec<hxchat_layout::Block> {
    let cs = std::ffi::CString::new(text).unwrap();
    let runs = [crate::ffi::HxChatRun {
        text: cs.as_ptr(),
        len: text.len() as std::ffi::c_int,
        color: -1,
        attrs: 0,
    }];
    unsafe { crate::ffi::body_blocks(runs.as_ptr(), 1, markdown) }
}

fn text_of(b: &hxchat_layout::Block) -> &hxchat_layout::ParsedText {
    match b {
        hxchat_layout::Block::Text(p) | hxchat_layout::Block::Quote { content: p, .. } => p,
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
            ("this", hxchat_layout::Attrs::BOLD),
            ("that", hxchat_layout::Attrs::ITALIC),
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
        hxchat_layout::Block::Code { text, .. } => {
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
        hxchat_layout::Block::Quote { content, depth } => {
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
    let runs = [crate::ffi::HxChatRun {
        text: cs.as_ptr(),
        len: text.len() as std::ffi::c_int,
        color: 37, // HX_CHAT_PAL_HISTORY_MUTED
        attrs: 0,
    }];
    let blocks = unsafe { crate::ffi::body_blocks(runs.as_ptr(), 1, true) };
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
            s.range.start, at,
            "gap or overlap before {:?} — the row would draw unmuted there",
            &p.text[s.range.clone()]
        );
        assert_eq!(
            s.style.fg,
            hxchat_layout::ColorRef::Palette(37),
            "{:?} lost the row colour",
            &p.text[s.range.clone()]
        );
        at = s.range.end;
    }
    assert_eq!(at, p.text.len(), "the tail of the row is uncovered");

    // ...and the emphasis is still there on top.
    assert!(p
        .spans
        .iter()
        .any(|s| &p.text[s.range.clone()] == "bold"
            && s.style.attrs.contains(hxchat_layout::Attrs::BOLD)));
}

#[test]
fn the_base_colour_tiles_across_multibyte_text() {
    // The tiling above is only interesting if it survives text where a
    // character is several bytes: `under` splices around span
    // boundaries, and getting that wrong on a multi-byte boundary is
    // both a wrong render and a potential panic.
    let text = "héllo **wörld** ☃";
    let cs = std::ffi::CString::new(text).unwrap();
    let runs = [crate::ffi::HxChatRun {
        text: cs.as_ptr(),
        len: text.len() as std::ffi::c_int,
        color: 37,
        attrs: 0,
    }];
    let blocks = unsafe { crate::ffi::body_blocks(runs.as_ptr(), 1, true) };
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
    let runs = [
        crate::ffi::HxChatRun { text: a.as_ptr(), len: 6, color: -1, attrs: 0 },
        crate::ffi::HxChatRun { text: b.as_ptr(), len: 8, color: 4, attrs: 0 },
    ];
    let blocks = unsafe { crate::ffi::body_blocks(runs.as_ptr(), 2, true) };
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
        hxchat_layout::Block::Code { text, .. } => assert_eq!(text, "hello world"),
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
            && s.style.attrs.contains(hxchat_layout::Attrs::CODE)),
        "the code span must be marked: {:?}",
        p.spans
    );
}
