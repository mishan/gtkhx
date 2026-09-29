//! Rotulus against the widgets a GTK4 chat client would otherwise use.
//!
//! - `rotulus`: this crate.
//! - `textview`: a `GtkTextView`, one line per message, the nick in a
//!   colored tag. What Polari and many small clients do.
//! - `listview`: a `GtkListView` of wrapping `GtkLabel`s, one per message,
//!   the nick in Pango markup. The widget-per-message shape Fractal-style
//!   clients use.
//!
//! Each run shows one widget in an 800×600 window and times, through the
//! real frame clock:
//!
//! - **ingest**: appending N messages, one call each, as a chat does;
//! - **first paint**: from the end of ingest to the first frame drawn at
//!   the bottom;
//! - **settled**: from the end of ingest until the main loop has nothing
//!   left to do — a text view keeps validating lines in the background
//!   after it paints;
//! - **resize**: the frames after the window narrows to 600 px, and how
//!   long until it settles again;
//! - **scroll**: frames paging from the top, a third of a page each;
//! - **memory**: resident set growth from before ingest to settled.
//!
//! Run each widget in its own process, so their memory doesn't mix:
//!
//! ```sh
//! for w in rotulus textview listview; do
//!     tools/isolated-run.sh cargo run --release -p rotulus --example compare -- $w 20000
//! done
//! ```
//!
//! Frame times include GTK's own work and the display; compare runs on
//! one machine, and read the idle interval first — no frame can be
//! shorter than it.

use gtk4 as gtk;
use gtk4::glib;
use gtk4::prelude::*;
use rotulus_layout::{
    Block, ColorRef, Message, MessageFlags, MessageKind, ParsedText, Span, Speaker, Style,
};
use std::cell::Cell;
use std::rc::Rc;

const NICKS: &[&str] = &["al", "misha", "hx_fan_1999", "zed", "somebody_longer"];
const WORDS: &[&str] = &[
    "hotline", "server", "anyone", "around", "tonight", "the", "files", "are", "up", "again", "ok",
    "lol", "mac", "classic", "news", "brb", "somewhat", "longer", "words", "here",
];

/// Message `i`: a nick and a body of 3–19 words, the same every run.
fn message(i: u32) -> (&'static str, String) {
    let mut seed = u64::from(i).wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1;
    let mut next = || {
        seed = seed
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (seed >> 33) as usize
    };
    let n = 3 + next() % 17;
    let body: Vec<&str> = (0..n).map(|_| WORDS[next() % WORDS.len()]).collect();
    (NICKS[i as usize % NICKS.len()], body.join(" "))
}

fn now() -> i64 {
    glib::monotonic_time()
}

fn ms(us: i64) -> String {
    format!("{:.1} ms", us as f64 / 1000.0)
}

/// Resident set size, in bytes.
fn rss() -> usize {
    let statm = std::fs::read_to_string("/proc/self/statm").unwrap_or_default();
    let pages: usize = statm
        .split_whitespace()
        .nth(1)
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    pages * 4096
}

/// Pump the main loop until the frame clock finishes a paint; returns
/// when it did.
fn next_frame(w: &impl IsA<gtk::Widget>) -> i64 {
    let clock = w.frame_clock().expect("the widget is realized");
    let done: Rc<Cell<Option<i64>>> = Rc::default();
    let d = done.clone();
    let id = clock.connect_after_paint(move |_| {
        if d.get().is_none() {
            d.set(Some(now()));
        }
    });
    w.queue_draw();
    let ctx = glib::MainContext::default();
    while done.get().is_none() {
        ctx.iteration(true);
    }
    clock.disconnect(id);
    done.get().unwrap()
}

/// Pump the main loop until nothing but idle work below everything GTK
/// schedules is left; returns when that was.
fn settle() -> i64 {
    let done = Rc::new(Cell::new(None));
    let d = done.clone();
    glib::idle_add_local_full(glib::Priority::from(glib::ffi::G_PRIORITY_LOW), move || {
        d.set(Some(now()));
        glib::ControlFlow::Break
    });
    let ctx = glib::MainContext::default();
    while done.get().is_none() {
        ctx.iteration(true);
    }
    done.get().unwrap()
}

/// Worst and mean of a run of frame intervals.
fn frames(label: &str, t: &[i64]) {
    let worst = t.iter().copied().max().unwrap_or(0);
    let mean = t.iter().sum::<i64>() / t.len().max(1) as i64;
    println!(
        "  {label:<14} worst {:>10}  mean {:>10}  ({} frames)",
        ms(worst),
        ms(mean),
        t.len()
    );
}

enum Subject {
    Rotulus(rotulus::RotulusView),
    Text(gtk::TextView),
    List(gtk::StringList),
}

impl Subject {
    fn new(kind: &str) -> (Subject, gtk::Widget) {
        match kind {
            "rotulus" => {
                let v = rotulus::RotulusView::new();
                v.set_font_from_string("Monospace 10");
                v.set_indent(true);
                (Subject::Rotulus(v.clone()), v.upcast())
            }
            "textview" => {
                let v = gtk::TextView::new();
                v.set_editable(false);
                v.set_cursor_visible(false);
                v.set_wrap_mode(gtk::WrapMode::WordChar);
                v.set_monospace(true);
                v.buffer().create_tag(
                    Some("nick"),
                    &[("foreground", &"#1c71d8"), ("weight", &700i32)],
                );
                (Subject::Text(v.clone()), v.upcast())
            }
            "listview" => {
                let model = gtk::StringList::new(&[]);
                let factory = gtk::SignalListItemFactory::new();
                factory.connect_setup(|_, item| {
                    let label = gtk::Label::new(None);
                    label.set_wrap(true);
                    label.set_xalign(0.0);
                    label.set_selectable(false);
                    item.downcast_ref::<gtk::ListItem>()
                        .unwrap()
                        .set_child(Some(&label));
                });
                factory.connect_bind(|_, item| {
                    let item = item.downcast_ref::<gtk::ListItem>().unwrap();
                    let s = item.item().and_downcast::<gtk::StringObject>().unwrap();
                    item.child()
                        .and_downcast::<gtk::Label>()
                        .unwrap()
                        .set_markup(&s.string());
                });
                let v = gtk::ListView::new(
                    Some(gtk::NoSelection::new(Some(model.clone()))),
                    Some(factory),
                );
                (Subject::List(model), v.upcast())
            }
            other => panic!("no widget called {other:?}: rotulus, textview or listview"),
        }
    }

    fn append(&self, i: u32) {
        let (nick, body) = message(i);
        match self {
            Subject::Rotulus(v) => {
                let gutter = format!("<{nick}>");
                let n = gutter.len();
                let mut g = ParsedText::plain(gutter);
                g.spans.push(Span {
                    range: 1..n - 1,
                    style: Style::default().with_fg(ColorRef::Rgb(0x1c71d8)),
                });
                v.append(Message {
                    kind: MessageKind::Live,
                    timestamp: 0,
                    speaker: Some(Speaker::new(u64::from(i % 64) + 1, nick)),
                    gutter: Some(g),
                    blocks: vec![Block::text(body)].into(),
                    flags: MessageFlags::NONE,
                });
            }
            Subject::Text(v) => {
                let buf = v.buffer();
                let mut end = buf.end_iter();
                buf.insert_with_tags_by_name(&mut end, &format!("<{nick}> "), &["nick"]);
                buf.insert(&mut end, &body);
                buf.insert(&mut end, "\n");
            }
            Subject::List(model) => {
                model.append(&format!(
                    "<span foreground=\"#1c71d8\" weight=\"bold\">&lt;{nick}&gt;</span> {}",
                    glib::markup_escape_text(&body)
                ));
            }
        }
    }

    /// Follow the newest message, as a chat does.
    fn to_bottom(&self, scroller: &gtk::ScrolledWindow) {
        match self {
            // Follows the bottom by itself.
            Subject::Rotulus(_) => {}
            Subject::Text(v) => {
                let buf = v.buffer();
                let mark = buf.create_mark(None, &buf.end_iter(), false);
                v.scroll_to_mark(&mark, 0.0, false, 0.0, 1.0);
            }
            Subject::List(_) => {
                let adj = scroller.vadjustment();
                adj.set_value(adj.upper() - adj.page_size());
            }
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let kind = args.get(1).map(String::as_str).unwrap_or("rotulus");
    let n: u32 = args.get(2).and_then(|v| v.parse().ok()).unwrap_or(20_000);

    gtk::init().expect("GTK needs a display: run under tools/isolated-run.sh");
    let (subject, widget) = Subject::new(kind);
    let scroller = gtk::ScrolledWindow::new();
    scroller.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Always);
    scroller.set_child(Some(&widget));
    let window = gtk::Window::new();
    window.set_default_size(800, 600);
    window.set_child(Some(&scroller));
    window.present();

    // Warm up: the first frames pay for CSS, fonts and the renderer.
    let mut last = next_frame(&widget);
    let mut idle = Vec::new();
    for _ in 0..30 {
        let t = next_frame(&widget);
        idle.push(t - last);
        last = t;
    }
    idle.sort_unstable();
    println!("{kind}, {n} messages");
    println!("  {:<14} {}", "idle frame", ms(idle[idle.len() / 2]));

    let rss0 = rss();
    let t = now();
    for i in 0..n {
        subject.append(i);
    }
    let ingested = now();
    subject.to_bottom(&scroller);
    let painted = next_frame(&widget);
    subject.to_bottom(&scroller);
    let settled = settle();
    let rss1 = rss();
    println!(
        "  {:<14} {:>10}  ({:.0} msgs/s)",
        "ingest",
        ms(ingested - t),
        f64::from(n) / ((ingested - t) as f64 / 1e6)
    );
    println!("  {:<14} {:>10}", "first paint", ms(painted - ingested));
    println!("  {:<14} {:>10}", "settled", ms(settled - ingested));
    println!(
        "  {:<14} {:>7.1} MB  ({} B/msg)",
        "memory",
        (rss1.saturating_sub(rss0)) as f64 / 1e6,
        rss1.saturating_sub(rss0) / n as usize
    );

    // Relayout: narrow the window, so every line may re-wrap. First how
    // long until the widget has nothing left to do, then — widening it
    // again — what the frames cost while it works.
    next_frame(&widget);
    let t = now();
    window.set_default_size(600, 600);
    let first = next_frame(&widget);
    let settled = settle();
    println!(
        "  {:<14} {:>10}  (first frame {})",
        "resize settled",
        ms(settled - t),
        ms(first - t)
    );
    let mut last = next_frame(&widget);
    window.set_default_size(800, 600);
    let mut resize = Vec::new();
    for _ in 0..10 {
        let f = next_frame(&widget);
        resize.push(f - last);
        last = f;
    }
    frames("resize frames", &resize);

    // Scroll: from the top, a third of a page a frame.
    let adj = scroller.vadjustment();
    adj.set_value(0.0);
    let mut last = next_frame(&widget);
    let mut scroll = Vec::new();
    for _ in 0..120 {
        let v = adj.value() + adj.page_size() / 3.0;
        adj.set_value(if v > adj.upper() - adj.page_size() {
            0.0
        } else {
            v
        });
        let f = next_frame(&widget);
        scroll.push(f - last);
        last = f;
    }
    frames("scroll", &scroll);
    window.destroy();
}
