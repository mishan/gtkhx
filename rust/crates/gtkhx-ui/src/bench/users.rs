//! The Users scenario: a large login, a burst of status changes, and a
//! GIF icon for everyone at once, in the main window's real Users panel.
//!
//! It feeds synthetic users through the same receive handlers the session's
//! events go through — `load` for the login's user list, `changed` for each
//! user change — against the running session's own connection and public
//! chat, so the member model, the session signals, `users.c` and the list
//! view all do their real work. The GIF icons go in where an ICON_GET reply
//! lands, `gtkhx_avatar_update`, and decode on the real decoder.
//!
//! Reading the frames is the session's now, outside what is timed, so the
//! login and burst numbers are not comparable with runs from before it was.
//!
//! The session is not connected. The handlers don't need it to be, and it
//! keeps the run from touching any server. The scenario clears the list
//! before and after, through the same `users-clear` signal a disconnect
//! sends.
//!
//! Known-value checks: the list holds N rows after the login; after the
//! burst every row shows the new status; every avatar decodes and
//! animates. A failed check is reported, because the timings would then
//! be timing something other than what they are labeled.

use std::ffi::c_void;

use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use gtkhx_core::session::{gtkhx_session_emit_users_clear, gtkhx_session_get_default};
use hxhandlers::recv::user::{changed, load};
use hxsession::User;

use crate::user_row::HxUserRow;

use super::{after_paint, next_frame, warm_up, Report, Stats};

extern "C" {
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    fn hx_conn_fd(htlc: *const c_void) -> i32;
}

/// Frames sampled for each steady-state measurement.
const SAMPLE_FRAMES: usize = 120;
/// How long decoding may go without finishing another icon before the
/// rest are called lost.
const DECODE_STALL_US: i64 = 5_000_000;
/// The idle bit of a user's status (`color`).
const STATUS_IDLE: u16 = 1;
/// The first uid handed out, clear of 0 (no user) and of our own.
const FIRST_UID: u16 = 100;

fn uid_of(i: u32) -> u16 {
    FIRST_UID + i as u16
}

/// User `i`, with status `status`.
fn user(i: u32, status: u16) -> User {
    User {
        uid: uid_of(i),
        icon: 128 + (i % 64) as u16,
        status: Some(status),
        name: format!("bench user {i:05}"),
        color: None,
    }
}

/// A 32×16 GIF of two frames, 100 ms each, looping forever — a typical
/// animated user icon.
///
/// The image data uses the uncompressed LZW form: with a 7-bit minimum
/// code size every code is 8 bits, one byte per pixel, and a clear code
/// every 100 pixels keeps the table from growing into 9-bit codes.
fn animated_icon() -> Vec<u8> {
    const W: u16 = 32;
    const H: u16 = 16;
    let mut g = Vec::new();
    g.extend_from_slice(b"GIF89a");
    g.extend_from_slice(&W.to_le_bytes());
    g.extend_from_slice(&H.to_le_bytes());
    // Global color table, 128 entries (2^(6+1)).
    g.extend_from_slice(&[0xf6, 0, 0]);
    for c in 0..128u8 {
        g.extend_from_slice(&[c.wrapping_mul(2), 255 - c.wrapping_mul(2), c]);
    }
    // Loop forever.
    g.extend_from_slice(b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00");
    for f in 0..2u8 {
        // Graphic control: 10 centiseconds.
        g.extend_from_slice(&[0x21, 0xf9, 0x04, 0x00, 10, 0, 0, 0]);
        g.push(0x2c);
        g.extend_from_slice(&[0, 0, 0, 0]);
        g.extend_from_slice(&W.to_le_bytes());
        g.extend_from_slice(&H.to_le_bytes());
        g.push(0);
        // Image data: minimum code size, then the codes in sub-blocks.
        const CLEAR: u8 = 128;
        const END: u8 = 129;
        let mut codes = Vec::new();
        for p in 0..(W as usize * H as usize) {
            if p % 100 == 0 {
                codes.push(CLEAR);
            }
            let (x, y) = (p % W as usize, p / W as usize);
            codes.push((((x / 4 + y / 4) as u8 + f * 3) % 64) + 1);
        }
        codes.push(END);
        g.push(7);
        for block in codes.chunks(255) {
            g.push(block.len() as u8);
            g.extend_from_slice(block);
        }
        g.push(0);
    }
    g.push(0x3b);
    g
}

pub(super) async fn run(n: u32) {
    // Every uid must fit in a u16 above FIRST_UID.
    let n = n.min(u32::from(u16::MAX - FIRST_UID));
    let sess = unsafe { crate::ffi::hx_active_session() };
    if sess.is_null() {
        glib::g_warning!("gtkhx", "GTKHX_BENCH users: no session");
        return;
    }
    let (htlc, chat) = unsafe { (gtkhx_session_htlc(sess), chat_with_cid(sess, 0)) };
    if htlc.is_null() || chat.is_null() {
        glib::g_warning!("gtkhx", "GTKHX_BENCH users: no connection or public chat");
        return;
    }
    // The scenario writes fake users into the public chat and clears it
    // after — a real server's roster, subject and icons with it.
    if unsafe { hx_conn_fd(htlc) } != 0 {
        glib::g_warning!(
            "gtkhx",
            "GTKHX_BENCH users: the session is connected; run with a scratch configuration"
        );
        return;
    }
    // Open the Users panel if the layout hasn't.
    unsafe { crate::users::create_users_window(std::ptr::null_mut(), sess) };
    let Some((cv, store)) = crate::users_view::public_list(sess.cast()) else {
        glib::g_warning!("gtkhx", "GTKHX_BENCH users: the Users panel wasn't built");
        return;
    };
    let clear =
        || unsafe { gtkhx_session_emit_users_clear(gtkhx_session_get_default(), htlc, chat) };
    clear();

    measure(n, sess, htlc, chat, &cv, &store).await;

    clear();
}

async fn measure(
    n: u32,
    sess: *mut c_void,
    htlc: *mut c_void,
    chat: *mut c_void,
    cv: &gtk::ColumnView,
    store: &gtk::gio::ListStore,
) {
    let idle = warm_up(cv).await;
    let mut r = Report::new("users", idle);
    r.line("users", &format!("{n:9}"), "");

    // ---- login ----------------------------------------------------------
    let users: Vec<User> = (0..n).map(|i| user(i, 0)).collect();
    let t = glib::monotonic_time();
    unsafe { load(htlc, chat, &users, None) };
    // The rows land in the store from an idle ahead of the next frame;
    // run it now, so the frozen time is the whole of the work.
    crate::users_view::flush_public(sess.cast());
    let call = glib::monotonic_time() - t;
    let t = glib::monotonic_time();
    let paint = after_paint(cv).await - t;
    r.ms("login", call, "USER_LIST reply into the store, UI frozen");
    r.ms("  first paint", paint, "");
    r.ms("  login + paint", call + paint, "compare this one");
    let rows = store.n_items();
    if rows != n {
        r.line(
            "  CHECK FAILED",
            &format!("{rows:9}"),
            &format!("rows shown, expected {n}"),
        );
    }

    // ---- USER_CHANGE burst -----------------------------------------------
    // Everyone goes idle at once, as when a server sweeps its idle timer.
    let changes: Vec<User> = (0..n).map(|i| user(i, STATUS_IDLE)).collect();
    let t = glib::monotonic_time();
    for u in &changes {
        unsafe { changed(htlc, 0, u) };
    }
    let call = glib::monotonic_time() - t;
    let t = glib::monotonic_time();
    let paint = after_paint(cv).await - t;
    r.ms(
        "change burst",
        call,
        &format!("{n} USER_CHANGEs, UI frozen"),
    );
    r.ms("  first paint", paint, "");
    let stale = (0..store.n_items())
        .filter_map(|i| store.item(i).and_downcast::<HxUserRow>())
        .filter(|row| row.status_of() != STATUS_IDLE)
        .count();
    if stale != 0 || store.n_items() != n {
        r.line(
            "  CHECK FAILED",
            &format!("{stale:9}"),
            "rows not showing the new status",
        );
    }

    // ---- every GIF icon at once ------------------------------------------
    let gif = animated_icon();
    let t = glib::monotonic_time();
    for i in 0..n {
        unsafe { crate::avatar::gtkhx_avatar_update(htlc, uid_of(i), gif.as_ptr(), gif.len()) };
    }
    let call = glib::monotonic_time() - t;
    let decoded = || {
        (0..n)
            .filter(|&i| unsafe { crate::avatar::gtkhx_avatar_is_animated(htlc, uid_of(i)) } != 0)
            .count() as u32
    };
    let start = glib::monotonic_time();
    let mut last = start;
    let mut longest = 0;
    let (mut count, mut progress_at) = (0, start);
    while count < n && last - progress_at < DECODE_STALL_US {
        // Frames have stopped: every wait returns at once, so this loop
        // would spin without letting a decode land. The report says why.
        if super::STALLED.get() {
            break;
        }
        let now = next_frame(cv).await;
        longest = longest.max(now - last);
        last = now;
        let c = decoded();
        if c > count {
            (count, progress_at) = (c, now);
        }
    }
    let until = progress_at - start;
    r.ms("gif icons", call, "handing them in, UI frozen");
    r.ms("  until decoded", call + until, "the last one to finish");
    r.ms("  longest frame", longest, "while decoding");
    if count != n {
        r.line(
            "  CHECK FAILED",
            &format!("{count:9}"),
            &format!("icons decoded and animating, expected {n}"),
        );
    }

    // ---- steady state with every icon animating ---------------------------
    let mut still = Vec::with_capacity(SAMPLE_FRAMES);
    let mut last = next_frame(cv).await;
    for _ in 0..SAMPLE_FRAMES {
        let now = next_frame(cv).await;
        still.push(now - last);
        last = now;
    }
    r.stats("animating", &Stats::of(&still));

    // ---- what the animating icons cost at rest ---------------------------
    // With nothing else going on: with the list on screen, where only the
    // rows in view need to move, and with it hidden, where none do.
    // The first row is in view, so its avatar moves; hidden, it holds.
    let scroller = cv.parent();
    let first = || crate::avatar::steps(htlc, uid_of(0));
    if let Some(root) = cv.root().map(|w| w.upcast::<gtk::Widget>()) {
        let before = first();
        if let Some(rates) = super::media::sample(&root).await {
            super::media::report(&mut r, "at rest, in view", &rates);
        }
        if first() == before {
            r.line("  CHECK FAILED", "", "an avatar in view never moved");
        }
        if let Some(s) = &scroller {
            s.set_visible(false);
            // Let the tick that was due land before taking the frame.
            glib::timeout_future(std::time::Duration::from_millis(300)).await;
            let before = first();
            if let Some(rates) = super::media::sample(&root).await {
                super::media::report(&mut r, "at rest, hidden", &rates);
            }
            if first() != before {
                r.line("  CHECK FAILED", "", "a hidden avatar kept moving");
            }
            s.set_visible(true);
        }
    }

    let adj = cv
        .parent()
        .and_downcast::<gtk::ScrolledWindow>()
        .map(|sw| sw.vadjustment());
    let mut scroll = Vec::with_capacity(SAMPLE_FRAMES);
    let mut last = next_frame(cv).await;
    for _ in 0..SAMPLE_FRAMES {
        if let Some(adj) = &adj {
            let page = adj.page_size();
            let mut v = adj.value() + page / 3.0;
            if v > adj.upper() - page {
                v = 0.0;
            }
            adj.set_value(v);
        }
        let now = next_frame(cv).await;
        scroll.push(now - last);
        last = now;
    }
    r.stats("scroll", &Stats::of(&scroll));

    r.print();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_icon_is_an_animated_gif() {
        let bytes = animated_icon();
        let loader = gtk::gdk_pixbuf::PixbufLoader::with_type("gif").unwrap();
        loader.write(&bytes).unwrap();
        loader.close().unwrap();
        let anim = loader.animation().unwrap();
        assert!(!anim.is_static_image());
        assert_eq!((anim.width(), anim.height()), (32, 16));
    }
}
