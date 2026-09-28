//! The Users scenario: a large login, a burst of status changes, and a
//! GIF icon for everyone at once, in the main window's real Users panel.
//!
//! It feeds synthetic server frames through the same receive handlers a
//! connection's frames go through — `rcv_task_user_list` for the login's
//! USER_LIST reply, `hx_rcv_user_change` for each USER_CHANGE — against
//! the running session's own connection and public chat, so the member
//! model, the session signals, `users.c` and the list view all do their
//! real work. The GIF icons go in where an ICON_GET reply lands,
//! `gtkhx_avatar_update`, and decode on the real decoder.
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
use hxhandlers::recv::user::{hx_rcv_user_change, rcv_task_user_list};
use hxproto::messages::tag;

use crate::user_row::HxUserRow;

use super::{after_paint, next_frame, warm_up, Report, Stats};

extern "C" {
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    /// `gif_avatar.c` — where an ICON_GET reply's GIF lands.
    fn gtkhx_avatar_update(htlc: *mut c_void, uid: u16, gif: *const u8, len: usize);
    fn gtkhx_avatar_is_animated(htlc: *mut c_void, uid: u16) -> glib::ffi::gboolean;
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

/// A Hotline frame: the 22-byte header (type, then zeroed id, error, sizes
/// and object count — the handlers read none of them) and TLV chunks.
fn frame(msg_type: u32, chunks: &[(u16, &[u8])]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&msg_type.to_be_bytes());
    v.extend_from_slice(&[0u8; 18]);
    for (t, data) in chunks {
        v.extend_from_slice(&t.to_be_bytes());
        v.extend_from_slice(&(data.len() as u16).to_be_bytes());
        v.extend_from_slice(data);
    }
    v
}

/// A USER_LIST reply for `n` users: uid, icon, status and a name each.
fn user_list_reply(n: u32) -> Vec<u8> {
    let records: Vec<Vec<u8>> = (0..n)
        .map(|i| {
            let name = format!("bench user {i:05}");
            let mut r = Vec::with_capacity(8 + name.len());
            r.extend_from_slice(&uid_of(i).to_be_bytes());
            r.extend_from_slice(&(128 + (i % 64) as u16).to_be_bytes());
            r.extend_from_slice(&0u16.to_be_bytes());
            r.extend_from_slice(&(name.len() as u16).to_be_bytes());
            r.extend_from_slice(name.as_bytes());
            r
        })
        .collect();
    let chunks: Vec<(u16, &[u8])> = records
        .iter()
        .map(|r| (tag::USER_LIST, r.as_slice()))
        .collect();
    frame(0, &chunks)
}

/// A USER_CHANGE for user `i`: same name and icon, status `status`.
fn user_change(i: u32, status: u16) -> Vec<u8> {
    let name = format!("bench user {i:05}");
    frame(
        0x0000_012d,
        &[
            (tag::UID, &uid_of(i).to_be_bytes()),
            (tag::ICON, &(128 + (i % 64) as u16).to_be_bytes()),
            (tag::COLOUR, &status.to_be_bytes()),
            (tag::CHAT_ID, &0u32.to_be_bytes()),
            (tag::NAME, name.as_bytes()),
        ],
    )
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
    let reply = user_list_reply(n);
    let t = glib::monotonic_time();
    unsafe {
        rcv_task_user_list(
            htlc,
            reply.as_ptr(),
            reply.len(),
            chat,
            std::ptr::null_mut(),
        )
    };
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
    let frames: Vec<Vec<u8>> = (0..n).map(|i| user_change(i, STATUS_IDLE)).collect();
    let t = glib::monotonic_time();
    for f in &frames {
        unsafe { hx_rcv_user_change(htlc, f.as_ptr(), f.len()) };
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
        unsafe { gtkhx_avatar_update(htlc, uid_of(i), gif.as_ptr(), gif.len()) };
    }
    let call = glib::monotonic_time() - t;
    let decoded = || {
        (0..n)
            .filter(|&i| unsafe { gtkhx_avatar_is_animated(htlc, uid_of(i)) } != 0)
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
    fn synthetic_user_list_parses_back_to_every_user() {
        let buf = user_list_reply(300);
        let recs: Vec<_> = hxproto::wire::ChunkIter::over_message(&buf, buf.len())
            .filter(|c| c.tag == tag::USER_LIST)
            .filter_map(|c| hxproto::parse::parse_user_list_record(c.data, 31))
            .collect();
        assert_eq!(recs.len(), 300);
        assert_eq!(recs[0].uid, FIRST_UID);
        assert_eq!(recs[299].name, b"bench user 00299");
    }

    #[test]
    fn synthetic_user_change_parses_back() {
        let f = user_change(7, STATUS_IDLE);
        let c = hxproto::parse::parse_user_change(&f, f.len(), 31);
        assert_eq!(c.uid, uid_of(7));
        assert_eq!(c.color, STATUS_IDLE);
        assert!(c.got_color);
        assert_eq!(c.name, b"bench user 00007");
    }

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
