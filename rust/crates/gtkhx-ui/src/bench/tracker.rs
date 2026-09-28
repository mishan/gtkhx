//! The Tracker scenario: a large listing arriving, then a search typed a
//! key at a time, in the real tracker window.
//!
//! The listing takes the path a fetch's records take once `hxnet` hands
//! them over: `tracker-batch-begin`, then per server an event built by
//! `hx_tracker_server_new_v3`, the `tracker-server-create` signal and the
//! Tasks progress tick — all in one main-loop turn, as the fetch's drain
//! delivers every record that is ready. Nothing touches the network.
//!
//! The search is typed into the window's own search entry, one key at a
//! time. The entry's search delay is held off for the run and each change
//! is signalled directly, so the numbers are the filter's cost rather than
//! the entry's typing pause. Clearing needs no signalling: the entry
//! reports an emptied search at once, whatever its delay.
//!
//! The scenario opens its own tracker window, and refuses to run in one the
//! user already has open.
//!
//! Known-value checks: the window lists N servers; the search shows exactly
//! the servers its regex matches, worked out here independently; clearing
//! it shows all N again.

use std::ffi::{c_char, c_void, CString};

use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use gtkhx_core::boxed::tracker::{hx_tracker_server_free, HxTrackerServer};
use gtkhx_core::session::{
    gtkhx_session_emit_tracker_batch_begin, gtkhx_session_emit_tracker_server_create,
    gtkhx_session_get_default,
};

use super::{after_paint, warm_up, Report, Stats};

extern "C" {
    #[allow(clippy::too_many_arguments)]
    fn hx_tracker_server_new_v3(
        addr_type: u8,
        address: *const u8,
        address_len: usize,
        port: u16,
        nusers: u16,
        name: *const c_char,
        name_len: usize,
        desc: *const c_char,
        desc_len: usize,
        tlv_count: u16,
        tlv_bytes: *const u8,
        tlv_bytes_len: usize,
        total: i32,
    ) -> *mut HxTrackerServer;
    /// `tasks.c` — the Tasks panel's "Listing tracker" progress.
    fn track_prog_update(sess: *mut c_void, url: *mut c_char, num: i32, total: i32);
}

/// `HTRK_V3_ADDR_IPV4` (hotline.h).
const ADDR_IPV4: u8 = 0x04;
const TRACKER_URL: &str = "bench.tracker.invalid";
/// What the search types, one key at a time, and a label for each. What a
/// keystroke costs depends on how much it changes which rows show, so one
/// query narrows the list from its first keys and the other keeps every
/// server until its last few. Both match the same servers whether or not
/// the search is case-sensitive.
const QUERIES: [(&str, &str); 2] = [("narrowing", "retro and"), ("broad", "Server 001")];

/// Words for the descriptions, so they vary without matching the query.
const WORDS: [&str; 8] = [
    "files", "chat", "news", "retro", "mac", "music", "games", "friendly",
];

fn name_of(i: u32) -> String {
    format!("Bench Server {i:05}")
}

fn desc_of(i: u32) -> String {
    let w = |k: u32| WORDS[((i / k) % WORDS.len() as u32) as usize];
    format!("{} and {}, since {}", w(1), w(3), 1995 + i % 30)
}

/// Hand the window `n` servers the way a fetch's drain does.
unsafe fn deliver_listing(sess: *mut c_void, n: u32) {
    let url = CString::new(TRACKER_URL).unwrap();
    let emitter = gtkhx_session_get_default();
    let count = n.min(u16::MAX as u32) as u16;
    gtkhx_session_emit_tracker_batch_begin(emitter, url.as_ptr(), 3, count);
    track_prog_update(sess, url.as_ptr().cast_mut(), 0, n as i32);
    for i in 0..n {
        // 10.x.y.z, a distinct address per server.
        let addr = [10, (i >> 16) as u8, (i >> 8) as u8, i as u8];
        let (name, desc) = (name_of(i), desc_of(i));
        let e = hx_tracker_server_new_v3(
            ADDR_IPV4,
            addr.as_ptr(),
            addr.len(),
            5500,
            (i % 200) as u16,
            name.as_ptr().cast(),
            name.len(),
            desc.as_ptr().cast(),
            desc.len(),
            0,
            std::ptr::null(),
            0,
            n as i32,
        );
        if !e.is_null() {
            gtkhx_session_emit_tracker_server_create(emitter, e.cast());
            hx_tracker_server_free(e);
        }
        // Even for a record that didn't build: the last tick retires the
        // Tasks entry.
        track_prog_update(sess, url.as_ptr().cast_mut(), i as i32 + 1, n as i32);
    }
}

pub(super) async fn run(n: u32) {
    let n = n.min(u16::MAX as u32);
    let sess = unsafe { crate::ffi::hx_active_session() };
    if sess.is_null() {
        glib::g_warning!("gtkhx", "GTKHX_BENCH tracker: no session");
        return;
    }
    if !crate::tracker::open_without_fetch(sess) {
        glib::g_warning!(
            "gtkhx",
            "GTKHX_BENCH tracker: the tracker window is already open; close it first"
        );
        return;
    }
    let Some((window, entry)) = crate::tracker::bench_view() else {
        glib::g_warning!("gtkhx", "GTKHX_BENCH tracker: the window wasn't built");
        return;
    };
    window.present();
    crate::tracker::tracker_clear();
    let delay = entry.search_delay();
    entry.set_search_delay(u32::MAX);

    measure(n, sess, &window, &entry).await;

    entry.set_text("");
    entry.set_search_delay(delay);
    window.close();
}

async fn measure(n: u32, sess: *mut c_void, window: &gtk::Window, entry: &gtk::SearchEntry) {
    let idle = warm_up(window).await;
    let mut r = Report::new("tracker", idle);
    r.line("servers", &format!("{n:9}"), "");

    // ---- listing --------------------------------------------------------
    let t = glib::monotonic_time();
    unsafe { deliver_listing(sess, n) };
    // The records land in the stores from an idle ahead of the next frame;
    // run it now, so the frozen time is the whole of the work.
    crate::tracker::bench_flush();
    let call = glib::monotonic_time() - t;
    let t = glib::monotonic_time();
    let paint = after_paint(window).await - t;
    r.ms("listing", call, "every record, into the stores, UI frozen");
    r.ms("  first paint", paint, "");
    r.ms("  listing + paint", call + paint, "compare this one");
    let (total, _) = crate::tracker::bench_counts();
    if total != n {
        r.line(
            "  CHECK FAILED",
            &format!("{total:9}"),
            &format!("servers listed, expected {n}"),
        );
    }

    // ---- typing searches ------------------------------------------------
    let caseless = unsafe { crate::ffi::gtkhx_tracker_pref_case() } == 0;
    let mut clear = (0, 0);
    for (label, query) in QUERIES {
        let mut calls = Vec::new();
        let mut painted = Vec::new();
        for ch in query.chars() {
            // Insert at the end, as a key does. set_text would empty the
            // entry first, and emptying it reports a cleared search at once
            // — a full clear before every key.
            let mut pos = entry.text().chars().count() as i32;
            entry.insert_text(ch.encode_utf8(&mut [0; 4]), &mut pos);
            let t = glib::monotonic_time();
            entry.emit_by_name::<()>("search-changed", &[]);
            calls.push(glib::monotonic_time() - t);
            painted.push(after_paint(window).await - t);
        }
        r.line(label, &format!("{query:>9}"), "typed a key at a time");
        r.stats("  keystroke", &Stats::of(&calls));
        r.stats("  until painted", &Stats::of(&painted));
        let want = expected_matches(n, query, caseless);
        let (_, found) = crate::tracker::bench_counts();
        if found != want {
            r.line(
                "  CHECK FAILED",
                &format!("{found:9}"),
                &format!("servers shown, expected {want}"),
            );
        }

        // Emptying the entry reports the search at once, so the clear runs
        // inside set_text.
        let t = glib::monotonic_time();
        entry.set_text("");
        let call = glib::monotonic_time() - t;
        clear = (call, after_paint(window).await - t);
        let (_, found) = crate::tracker::bench_counts();
        if found != n {
            r.line(
                "  CHECK FAILED",
                &format!("{found:9}"),
                &format!("servers shown after clearing, expected {n}"),
            );
        }
    }
    r.ms("clear search", clear.0, "every server back, UI frozen");
    r.ms("  until painted", clear.1, "");

    r.print();
}

/// How many of the `n` servers `query` matches, by the window's own rule —
/// the query is a regex over the name or the description — worked out here
/// without the window.
fn expected_matches(n: u32, query: &str, caseless: bool) -> u32 {
    let flags = if caseless {
        glib::RegexCompileFlags::CASELESS
    } else {
        glib::RegexCompileFlags::empty()
    };
    let Ok(Some(re)) = glib::Regex::new(query, flags, glib::RegexMatchFlags::empty()) else {
        return 0;
    };
    let hit = |s: &str| {
        let gs = glib::GString::from(s);
        re.match_(gs.as_ref(), glib::RegexMatchFlags::empty())
            .map(|m| m.matches())
            .unwrap_or(false)
    };
    (0..n)
        .filter(|&i| hit(&name_of(i)) || hit(&desc_of(i)))
        .count() as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_queries_match_known_handfuls_either_way_round() {
        for caseless in [true, false] {
            // "Bench Server 00100" … "00199".
            assert_eq!(expected_matches(2000, "Server 001", caseless), 100);
            // Descriptions opening "retro and …": one server in eight.
            assert_eq!(expected_matches(2000, "retro and", caseless), 250);
        }
    }

    #[test]
    fn the_narrowing_query_narrows_from_its_first_keys() {
        let shown = |q: &str| expected_matches(2000, q, false);
        assert!(shown("re") < 2000 / 2, "two keys already hide most servers");
        assert!(shown("retro and") < shown("re"));
    }
}
