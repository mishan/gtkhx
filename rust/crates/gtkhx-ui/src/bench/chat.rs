//! The chat-view scenario: ingest, first paint, relayout and scroll, in
//! the real public-chat view.
//!
//! This is the harness that decided the xtext replacement, moved here from
//! C when the harness became general. docs/chat-view-benchmark.md is its
//! record; the phases are the same, so its numbers stay comparable.
//!
//! **Ingest and first paint are summed.** The view stores a message on
//! append and lays it out in the frame that needs it, so ingest alone
//! measures bookkeeping. Their sum is the honest cost of getting N
//! messages on screen.
//!
//! **Relayout changes the font, not the width.** The first version of the
//! C harness shrank the view's size request and timed one tick, which
//! measured nothing: the view is `hexpand`, so its allocation never
//! changed and nothing re-wrapped. A font change is client-side and
//! invalidates every wrap point, and the phase samples ten frames so a
//! whole-scrollback re-wrap cannot land just after the frame being timed.
//!
//! **There is no memory metric.** An RSS delta was tried and reported
//! ~0.1 MB for 20,000 messages, which is impossible; see the benchmark
//! record.

use gtk::prelude::*;
use gtk4 as gtk;
use hxchat_view::HxChatView;

use super::{after_paint, next_frame, warm_up, Report, Stats};

/// Frames sampled after the relayout trigger.
const RELAYOUT_FRAMES: usize = 10;
/// Frames to let the first font land before timing the second.
const RELAYOUT_PREP_FRAMES: usize = 3;
/// Frames sampled while scrolling.
const SCROLL_FRAMES: usize = 120;

/// Different sizes, so every cached width and wrap point is invalid.
const FONT_A: &str = "Monospace 10";
const FONT_B: &str = "Monospace 12";

/// One synthetic line, through the same compat append path `chat.c` uses.
/// Lengths vary so wrapping is exercised; nick widths cycle so the gutter
/// settles early, as in a real room.
fn append_one(view: &gtk::Widget, i: u32) {
    const NAMES: [&str; 5] = ["misha", "alice", "bob", "carol", "dave-with-a-long-name"];
    let nick = format!("<{}>", NAMES[i as usize % NAMES.len()]);
    let words = 3 + (i % 17);
    let body: Vec<String> = (0..words)
        .map(|w| format!("word{}", (i * 7 + w) % 1000))
        .collect();
    let body = body.join(" ");
    unsafe {
        hxchat_view::ffi::hx_chat_view_append_indent(
            view.as_ptr() as *mut _,
            nick.as_ptr().cast(),
            nick.len() as i32,
            body.as_ptr().cast(),
            body.len() as i32,
            0,
        );
    }
}

pub(super) async fn run(view: &gtk::Widget, n: u32) {
    let Some(chat) = view.downcast_ref::<HxChatView>() else {
        gtk::glib::g_warning!("gtkhx", "GTKHX_BENCH chat: not a chat view");
        return;
    };
    let idle = warm_up(view).await;

    // ---- ingest + first paint -----------------------------------------
    let t = gtk::glib::monotonic_time();
    for i in 0..n {
        append_one(view, i);
    }
    let ingest = gtk::glib::monotonic_time() - t;
    view.queue_resize();
    let t = gtk::glib::monotonic_time();
    let first_paint = after_paint(view).await - t;

    // ---- relayout -------------------------------------------------------
    chat.set_font_from_string(FONT_A);
    for _ in 0..RELAYOUT_PREP_FRAMES {
        next_frame(view).await;
    }
    let mut last = gtk::glib::monotonic_time();
    chat.set_font_from_string(FONT_B);
    view.queue_resize();
    let mut relayout = Vec::with_capacity(RELAYOUT_FRAMES);
    for _ in 0..RELAYOUT_FRAMES {
        let now = next_frame(view).await;
        relayout.push(now - last);
        last = now;
        view.queue_draw();
    }

    // ---- scroll ---------------------------------------------------------
    // From the top down, a third of a page per frame, so each frame lands
    // on rows not laid out before.
    let adj = chat.vadjustment();
    let mut scroll = Vec::with_capacity(SCROLL_FRAMES);
    let mut last = next_frame(view).await;
    for _ in 0..SCROLL_FRAMES {
        if let Some(adj) = &adj {
            let page = adj.page_size();
            let mut v = adj.value() + page / 3.0;
            if v > adj.upper() - page {
                v = 0.0;
            }
            adj.set_value(v);
        }
        view.queue_draw();
        let now = next_frame(view).await;
        scroll.push(now - last);
        last = now;
    }

    let relayout = Stats::of(&relayout);
    let mut r = Report::new("chat view", idle);
    r.line("messages", &format!("{n:9}"), "");
    let rate = if ingest > 0 {
        n as f64 / (ingest as f64 / 1e6)
    } else {
        0.0
    };
    r.ms("ingest", ingest, &format!("{rate:.0} msgs/s"));
    r.ms("first paint", first_paint, "");
    r.ms("ingest + paint", ingest + first_paint, "compare this one");
    r.ms(
        "relayout total",
        relayout.total,
        &format!("{} frames after a font change", relayout.n),
    );
    r.ms(
        "relayout worst",
        relayout.worst,
        "a whole-scrollback re-wrap shows here",
    );
    r.stats("scroll", &Stats::of(&scroll));
    r.print();
}
