//! Golden-image tests: render scenes through a real window and compare
//! them with the pictures in `tests/golden/`.
//!
//! What they pin is the rendering the widget promises — where rows,
//! columns, groups, markers and selections land, and what color they
//! are — not the antialiasing of a particular FreeType. So the scenes are
//! drawn as deterministically as the stack allows (a bundled font with
//! hinting off, the cairo renderer, a fixed DPI, UTC, an explicit
//! palette), and compared at half resolution with a tolerance: a glyph
//! edge a shade different passes, a row a line lower does not.
//!
//! To accept a deliberate change, re-render the goldens:
//!
//! ```sh
//! ROTULUS_UPDATE_GOLDEN=1 tools/isolated-run.sh cargo test -p rotulus --test render
//! ```
//!
//! A failing scene leaves the picture it drew, and a map of where it
//! differs, in cargo's per-test temporary directory; the failure names
//! both.
//!
//! One `#[test]`, because GTK belongs to the first thread that
//! initializes it.

use gtk4 as gtk;
use gtk4::prelude::*;
use rotulus::ffi::{self, RotulusRow, RotulusRun, RotulusSpeaker};
use rotulus::RotulusView;
use std::ffi::{c_int, CString};
use std::path::PathBuf;

const W: i32 = 360;
const H: i32 = 200;

/// Seal the process off from the desktop's fonts and rendering choices.
fn pin_environment() {
    let fonts = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fonts");
    let tmp = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("render-fontconfig");
    std::fs::create_dir_all(&tmp).unwrap();
    let conf = tmp.join("fonts.conf");
    std::fs::write(
        &conf,
        format!(
            r#"<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>{}</dir>
  <cachedir>{}</cachedir>
  <match target="font">
    <edit name="antialias" mode="assign"><bool>true</bool></edit>
    <edit name="hinting" mode="assign"><bool>false</bool></edit>
    <edit name="hintstyle" mode="assign"><const>hintnone</const></edit>
    <edit name="rgba" mode="assign"><const>none</const></edit>
  </match>
</fontconfig>
"#,
            fonts.display(),
            tmp.join("cache").display()
        ),
    )
    .unwrap();
    std::env::set_var("FONTCONFIG_FILE", &conf);
    std::env::set_var("GSK_RENDERER", "cairo");
    std::env::set_var("TZ", "UTC");
    std::env::set_var("GTK_THEME", "Adwaita");
    // No accessibility bus in a sealed session, and nothing here needs one.
    std::env::set_var("GTK_A11Y", "none");
}

/// White background, black text, and the roles the scenes use made
/// explicit, so nothing depends on the theme.
fn palette() -> [gtk::gdk::RGBA; rotulus::view::PALETTE_COLS] {
    let mut p = rotulus::view::default_palette();
    let rgb = |v: u32| {
        gtk::gdk::RGBA::new(
            ((v >> 16) & 0xff) as f32 / 255.0,
            ((v >> 8) & 0xff) as f32 / 255.0,
            (v & 0xff) as f32 / 255.0,
            1.0,
        )
    };
    p[34] = rgb(0x000000); // FG
    p[35] = rgb(0xffffff); // BG
    p[32] = rgb(0xffffff); // MARK_FG
    p[33] = rgb(0x3584e4); // MARK_BG
    p[39] = rgb(0x1c71d8); // NICK
    p[41] = rgb(0x77767b); // NICK_BRACKET
    p[43] = rgb(0x26a269); // SYSTEM
    p[44] = rgb(0x77767b); // SYSTEM_BRACKET
    p[46] = rgb(0xc0bfbc); // RULE
    p
}

fn view() -> RotulusView {
    let v = RotulusView::new();
    v.set_palette(&palette());
    v.set_font_from_string("DejaVu Sans Mono 10");
    v.set_separator(true);
    v
}

/// A run with only the common fields.
fn run(text: &CString, color: i16, attrs: u16) -> RotulusRun {
    RotulusRun {
        text: text.as_ptr(),
        len: -1,
        color,
        attrs,
        background: 0,
        rgb: 0,
        background_rgb: 0,
    }
}

/// Append through the C ABI, so the scenes cover the conversion an
/// application's rows go through: markdown, autolinking, run styles.
fn append(
    view: &RotulusView,
    kind: c_int,
    key: u64,
    nick: Option<&str>,
    body: &[RotulusRun],
    stamp: i64,
) {
    let nick_c = nick.map(|n| CString::new(n).unwrap());
    let parts = nick.map(|_| {
        (
            CString::new("<").unwrap(),
            nick_c.clone().unwrap(),
            CString::new(">").unwrap(),
        )
    });
    let gutter: Vec<RotulusRun> = parts
        .as_ref()
        .map(|(a, n, b)| {
            vec![
                run(a, 41, 0),
                run(n, 47 + (key % 8) as i16, 0),
                run(b, 41, 0),
            ]
        })
        .unwrap_or_default();
    let row = RotulusRow {
        kind,
        flags: 0,
        stamp,
        speaker: RotulusSpeaker {
            key,
            nick: nick_c.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            nick_len: -1,
        },
        gutter: if gutter.is_empty() {
            std::ptr::null()
        } else {
            gutter.as_ptr()
        },
        n_gutter: gutter.len() as c_int,
        body: body.as_ptr(),
        n_body: body.len() as c_int,
    };
    unsafe { ffi::rotulus_view_append(view.upcast_ref::<gtk::Widget>().as_ptr() as *mut _, &row) };
}

fn say(view: &RotulusView, key: u64, nick: &str, body: &str, stamp: i64) {
    let b = CString::new(body).unwrap();
    append(
        view,
        ffi::ROW_MESSAGE,
        key,
        Some(nick),
        &[run(&b, -1, 0)],
        stamp,
    );
}

const T: i64 = 1_700_000_000; // 2023-11-14 22:13:20 UTC

/// Two columns: stamps and nicks in their own column, grouping, markdown,
/// a link, a status line.
fn two_column() -> RotulusView {
    let v = view();
    v.set_time_stamp(true);
    v.set_stamp_format("%H:%M ");
    say(&v, 1, "alice", "hello there", T);
    say(
        &v,
        1,
        "alice",
        "a second line, grouped under the first",
        T + 20,
    );
    say(
        &v,
        2,
        "bob",
        "see **bold**, `code` and https://example.com",
        T + 40,
    );
    say(&v, 3, "carol", "> a quote\nand a reply to it", T + 60);
    let (tag, lb, rb, body) = (
        CString::new("hx").unwrap(),
        CString::new("[").unwrap(),
        CString::new("]").unwrap(),
        CString::new("connected").unwrap(),
    );
    let gutter = [run(&lb, 44, 0), run(&tag, 43, 0), run(&rb, 44, 0)];
    let b = [run(&body, -1, 0)];
    let row = RotulusRow {
        kind: ffi::ROW_SYSTEM,
        flags: 0,
        stamp: T + 80,
        speaker: RotulusSpeaker {
            key: 0,
            nick: std::ptr::null(),
            nick_len: -1,
        },
        gutter: gutter.as_ptr(),
        n_gutter: 3,
        body: b.as_ptr(),
        n_body: 1,
    };
    unsafe { ffi::rotulus_view_append(v.upcast_ref::<gtk::Widget>().as_ptr() as *mut _, &row) };
    v
}

/// One column: the nick at the start of the body's first line, wrapping
/// to the left edge, and IRC formatting.
fn single_column() -> RotulusView {
    let v = view();
    v.set_indent(false);
    v.set_separator(false);
    v.set_time_stamp(true);
    v.set_stamp_format("%H:%M ");
    say(
        &v,
        1,
        "alice",
        "a message long enough that it has to wrap onto a second line",
        T,
    );
    let text = CString::new(
        "\x02bold\x02 \x0304red\x03 \x0300,02white on blue\x03 \x16reversed\x16 \x1funder\x1f",
    )
    .unwrap();
    let mut n = 0;
    let runs = unsafe { rotulus::mirc_ffi::rotulus_mirc_parse(text.as_ptr(), -1, &mut n) };
    let body = unsafe { std::slice::from_raw_parts(runs, n as usize) };
    append(&v, ffi::ROW_MESSAGE, 2, Some("bob"), body, T + 30);
    unsafe { gtk::glib::ffi::g_free(runs as *mut _) };
    say(&v, 3, "carol", "short", T + 60);
    v
}

/// History framing, a load-more row, and the last-read marker.
fn history_and_marker() -> RotulusView {
    let v = view();
    let muted = |t: &str| CString::new(t).unwrap();
    let (more, div, h1, h2, live) = (
        muted("─── ↑ Load older messages ───"),
        muted("─── chat history (2 messages) ───"),
        muted("an old message"),
        muted("another old one"),
        muted("─── live messages ───"),
    );
    append(&v, ffi::ROW_LOAD_OLDER, 0, None, &[run(&more, 37, 0)], T);
    append(&v, ffi::ROW_DIVIDER, 0, None, &[run(&div, 37, 0)], T);
    append(&v, ffi::ROW_HISTORY, 0, None, &[run(&h1, 37, 0)], T);
    append(&v, ffi::ROW_HISTORY, 0, None, &[run(&h2, 37, 0)], T);
    append(&v, ffi::ROW_DIVIDER, 0, None, &[run(&live, 37, 0)], T);
    say(&v, 1, "alice", "read up to here", T + 10);
    v.set_marker(v.last());
    say(&v, 2, "bob", "and this is new", T + 20);
    v
}

/// A selection across rows.
fn selection() -> RotulusView {
    let v = view();
    say(&v, 1, "alice", "one", T);
    say(&v, 2, "bob", "two", T + 10);
    v.select_all();
    v
}

/// Show `view` in a window and render what it draws.
fn render(view: &RotulusView) -> gtk::gdk::Texture {
    let window = gtk::Window::new();
    window.set_decorated(false);
    window.set_default_size(W, H);
    view.set_size_request(W, H);
    window.set_child(Some(view));
    window.present();
    let ctx = gtk::glib::MainContext::default();
    for _ in 0..500 {
        while ctx.iteration(false) {}
        if view.is_mapped() && view.width() == W && view.height() == H {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    assert!(view.is_mapped(), "the window never mapped");
    // Two passes: the first paint settles heights the estimate got wrong.
    for _ in 0..2 {
        view.queue_draw();
        while ctx.iteration(false) {}
    }
    let snapshot = gtk::Snapshot::new();
    window.snapshot_child(view, &snapshot);
    let node = snapshot.to_node().expect("the view drew nothing");
    let renderer = window.renderer().expect("the window has a renderer");
    let texture = renderer.render_texture(
        &node,
        Some(&gtk::graphene::Rect::new(0.0, 0.0, W as f32, H as f32)),
    );
    window.destroy();
    texture
}

/// RGBA bytes, averaged over 2x2 blocks.
fn half(t: &gtk::gdk::Texture) -> (usize, usize, Vec<u8>) {
    let mut d = gtk::gdk::TextureDownloader::new(t);
    d.set_format(gtk::gdk::MemoryFormat::R8g8b8a8);
    let (bytes, stride) = d.download_bytes();
    let (w, h) = (t.width() as usize, t.height() as usize);
    let (hw, hh) = (w / 2, h / 2);
    let mut out = vec![0u8; hw * hh * 4];
    for y in 0..hh {
        for x in 0..hw {
            for c in 0..4 {
                let px = |dx: usize, dy: usize| {
                    u32::from(bytes[(2 * y + dy) * stride + (2 * x + dx) * 4 + c])
                };
                out[(y * hw + x) * 4 + c] = ((px(0, 0) + px(1, 0) + px(0, 1) + px(1, 1)) / 4) as u8;
            }
        }
    }
    (hw, hh, out)
}

/// Compare with the golden, or write it. `None` when they match.
fn check(name: &str, texture: &gtk::gdk::Texture) -> Option<String> {
    let golden = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(format!("tests/golden/{name}.png"));
    if std::env::var_os("ROTULUS_UPDATE_GOLDEN").is_some() {
        texture.save_to_png(&golden).unwrap();
        return None;
    }
    let out = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("{name}.png"));
    texture.save_to_png(&out).unwrap();
    let Ok(want) = gtk::gdk::Texture::from_filename(&golden) else {
        return Some(format!(
            "{name}: no golden at {}; drew {}",
            golden.display(),
            out.display()
        ));
    };
    if (want.width(), want.height()) != (texture.width(), texture.height()) {
        return Some(format!(
            "{name}: drew {}x{}, golden is {}x{}",
            texture.width(),
            texture.height(),
            want.width(),
            want.height()
        ));
    }
    let (w, h, got) = half(texture);
    let (_, _, exp) = half(&want);
    // A pixel differs when any channel is off by more than this; the scene
    // fails when more than this share of its pixels differ.
    const CHANNEL: i32 = 48;
    const SHARE: f64 = 0.01;
    let mut diff = vec![255u8; got.len()];
    let mut off = 0usize;
    for i in 0..w * h {
        let far =
            (0..4).any(|c| (i32::from(got[i * 4 + c]) - i32::from(exp[i * 4 + c])).abs() > CHANNEL);
        if far {
            off += 1;
            diff[i * 4..i * 4 + 4].copy_from_slice(&[255, 0, 0, 255]);
        }
    }
    let share = off as f64 / (w * h) as f64;
    if share <= SHARE {
        return None;
    }
    let map = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("{name}-diff.png"));
    let bytes = gtk::glib::Bytes::from_owned(diff);
    gtk::gdk::MemoryTexture::new(
        w as i32,
        h as i32,
        gtk::gdk::MemoryFormat::R8g8b8a8,
        &bytes,
        w * 4,
    )
    .save_to_png(&map)
    .unwrap();
    Some(format!(
        "{name}: {:.1}% of pixels differ from the golden; drew {}, differences in {}",
        share * 100.0,
        out.display(),
        map.display()
    ))
}

#[test]
fn scenes_match_their_goldens() {
    pin_environment();
    assert!(
        gtk::init().is_ok(),
        "GTK could not be initialized — run under tools/isolated-run.sh, as CI does"
    );
    if let Some(s) = gtk::Settings::default() {
        s.set_gtk_xft_dpi(96 * 1024);
        s.set_gtk_xft_antialias(1);
        s.set_gtk_xft_hinting(0);
        s.set_gtk_xft_hintstyle(Some("hintnone"));
        s.set_gtk_xft_rgba(Some("none"));
    }

    let failures: Vec<String> = [
        ("two-column", two_column as fn() -> RotulusView),
        ("single-column", single_column),
        ("history-and-marker", history_and_marker),
        ("selection", selection),
    ]
    .into_iter()
    .filter_map(|(name, scene)| check(name, &render(&scene())))
    .collect();
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}
