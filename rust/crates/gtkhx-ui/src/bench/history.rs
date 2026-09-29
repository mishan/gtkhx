//! The History scenario: a chat-history replay on join, and a "Load older"
//! page on top of it, in the real public-chat view.
//!
//! The replay takes the path a server's GET_CHAT_HISTORY reply takes: a
//! synthetic reply of N entries goes through `rcv_task_chat_history` — the
//! parse, the `chat-history-batch` signal, and `chat.c`'s renderer, which
//! appends the divider, the "Load older" row and every entry — against the
//! unconnected session's own connection and public chat.
//!
//! "Load older" can't take that path: `chat.c` only inserts above the
//! history when its own click handler has just sent a request, and that
//! state is C with no accessor. So the page is N rows inserted one at a
//! time above an anchor row, through the view's insert-above call — the
//! call the renderer makes for each entry of an older page, and the part of
//! it that grows with the scrollback.
//!
//! The scenario refuses a connected session, clears the chat view before
//! and after, and puts back the connection's newest-history cursor, which
//! the replay advances. `chat.c`'s own "Load older" cursor it can't reach,
//! so it only runs when the app exits afterwards (`GTKHX_BENCH_QUIT`).
//!
//! Checks: the replay adds N rows plus its two dividers and "Load older"
//! row, whatever the scrollback cap — history doesn't count against it;
//! the page adds N rows; and a live message arriving after the page keeps
//! the page and drops a live row instead.

use std::ffi::c_void;

use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use gtkhx_core::conn::{
    hx_conn_chat_history_last_msgid, hx_conn_fd, hx_conn_set_chat_history_last_msgid, HtlcConn,
};
use hxhandlers::recv::chat::rcv_task_chat_history;
use rotulus::RotulusView;
use rotulus_layout::{Block, Message, MessageFlags, MessageKind};

use super::{after_paint, warm_up, Report};

extern "C" {
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
}

/// `DATA_HISTORY_ENTRY` and `DATA_HISTORY_HAS_MORE`.
const TAG_ENTRY: u16 = 0x0f05;
const TAG_HAS_MORE: u16 = 0x0f06;
/// The replay's rows besides its entries: the opening divider, the "Load
/// older" row and the closing "live messages" divider.
const REPLAY_EXTRA_ROWS: usize = 3;

const NICKS: [&str; 6] = ["misha", "alice", "bob", "carol", "dave", "eve"];

fn line_of(i: u32) -> String {
    let words = 4 + (i % 19);
    (0..words)
        .map(|w| format!("word{}", (i * 13 + w) % 997))
        .collect::<Vec<_>>()
        .join(" ")
}

/// A GET_CHAT_HISTORY reply of `n` entries, oldest first, with more to come.
fn history_reply(n: u32) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&0u32.to_be_bytes());
    v.extend_from_slice(&[0u8; 18]);
    let mut chunk = |tag: u16, data: &[u8]| {
        v.extend_from_slice(&tag.to_be_bytes());
        v.extend_from_slice(&(data.len() as u16).to_be_bytes());
        v.extend_from_slice(data);
    };
    for i in 0..n {
        let nick = NICKS[i as usize % NICKS.len()];
        let body = hxproto::build::build_history_entry(
            u64::from(i) + 1,
            1_700_000_000 + i64::from(i) * 30,
            0,
            128,
            nick.as_bytes(),
            line_of(i).as_bytes(),
            &[],
        )
        .expect("a short entry fits a chunk");
        chunk(TAG_ENTRY, &body);
    }
    chunk(TAG_HAS_MORE, &[1]);
    v
}

/// A row of an older page: history, as every row `chat.c` draws in the
/// history palette is.
fn row(text: String) -> Message {
    Message {
        kind: MessageKind::History {
            server_message_id: 0,
        },
        timestamp: 0,
        speaker: None,
        gutter: None,
        blocks: vec![Block::text(text)].into(),
        flags: MessageFlags::NONE,
    }
}

pub(super) async fn run(view: &gtk::Widget, n: u32) {
    let Some(chat) = view.downcast_ref::<RotulusView>() else {
        glib::g_warning!("gtkhx", "GTKHX_BENCH history: not a chat view");
        return;
    };
    let sess = unsafe { crate::ffi::hx_active_session() };
    let htlc = if sess.is_null() {
        std::ptr::null_mut()
    } else {
        unsafe { gtkhx_session_htlc(sess) }
    };
    if htlc.is_null() {
        glib::g_warning!("gtkhx", "GTKHX_BENCH history: no connection");
        return;
    }
    let conn = htlc as *mut HtlcConn;
    // The replay renders into the public chat, and the view is cleared
    // after: a real server's history with it.
    if unsafe { hx_conn_fd(conn) } != 0 {
        glib::g_warning!(
            "gtkhx",
            "GTKHX_BENCH history: the session is connected; run with a scratch configuration"
        );
        return;
    }
    // chat.c keeps its own history cursor for "Load older", which the
    // replay leaves on the fake history and the scenario can't reach from
    // here; a real server's "Load older" would then ask for nothing. So the
    // app must exit after the run, as tools/uibench.sh has it do.
    if std::env::var_os("GTKHX_BENCH_QUIT").is_none() {
        glib::g_warning!(
            "gtkhx",
            "GTKHX_BENCH history: needs GTKHX_BENCH_QUIT, since it leaves chat.c's \
             history cursor on the fake replay"
        );
        return;
    }
    let cursor = unsafe { hx_conn_chat_history_last_msgid(conn) };
    chat.clear();

    measure(chat, view, htlc, n).await;

    chat.clear();
    unsafe { hx_conn_set_chat_history_last_msgid(conn, cursor) };
}

async fn measure(chat: &RotulusView, view: &gtk::Widget, htlc: *mut c_void, n: u32) {
    let idle = warm_up(view).await;
    let mut r = Report::new("chat history", idle);
    r.line("entries", &format!("{n:9}"), "");
    r.line(
        "scrollback cap",
        &format!("{:9}", chat.max_rows()),
        "rows; 0 is no limit",
    );

    // ---- replay on join ---------------------------------------------------
    let reply = history_reply(n);
    let before = chat.len();
    let t = glib::monotonic_time();
    unsafe {
        rcv_task_chat_history(
            htlc,
            reply.as_ptr(),
            reply.len(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    let call = glib::monotonic_time() - t;
    let t = glib::monotonic_time();
    let paint = after_paint(view).await - t;
    r.ms("replay", call, "parse + signal + render, UI frozen");
    r.ms("  first paint", paint, "");
    r.ms("  replay + paint", call + paint, "compare this one");
    let added = chat.len() - before;
    // History doesn't count against the scrollback cap: all of it stays.
    let want = n as usize + REPLAY_EXTRA_ROWS;
    if added != want {
        r.line(
            "  CHECK FAILED",
            &format!("{added:9}"),
            &format!("rows added, expected {want}"),
        );
    }

    // ---- a "Load older" page ------------------------------------------------
    // Above an anchor at the top of what's there, one row at a time,
    // oldest first, as the renderer inserts an older page.
    chat.scroll_to_extreme(false);
    after_paint(view).await;
    let anchor = chat.insert_before(None, row("── chat history ──".into()));
    let before = chat.len();
    let t = glib::monotonic_time();
    for i in 0..n {
        let nick = NICKS[i as usize % NICKS.len()];
        chat.insert_before(Some(anchor), row(format!("<{nick}> {}", line_of(i))));
    }
    let call = glib::monotonic_time() - t;
    let t = glib::monotonic_time();
    let paint = after_paint(view).await - t;
    r.ms("load older", call, "inserted above, UI frozen");
    r.ms("  first paint", paint, "");
    r.ms("  load older + paint", call + paint, "compare this one");
    let added = chat.len() - before;
    if added != n as usize {
        r.line(
            "  CHECK FAILED",
            &format!("{added:9}"),
            &format!("rows added, expected {n}"),
        );
    }

    // A live message on a full scrollback must not take the page with it.
    // Fill the cap first, then time messages that each trim: with history
    // at the top, the live row they drop is below it, not at the front.
    let cap = chat.max_rows();
    if cap == 0 {
        r.line("live message", "", "no scrollback cap; nothing to trim");
        r.print();
        return;
    }
    let live = |i: usize| Message {
        kind: MessageKind::Live,
        ..row(format!("live {i}"))
    };
    for i in 0..cap {
        chat.append(live(i));
    }
    let traffic = cap;
    let t = glib::monotonic_time();
    for i in 0..traffic {
        chat.append(live(cap + i));
    }
    let per = (glib::monotonic_time() - t) as f64 / traffic as f64;
    r.line(
        "live message",
        &format!("{per:9.1} µs"),
        "at the cap, trimming under the history, each",
    );
    let history = chat.len() - cap;
    let want = n as usize + 1 + REPLAY_EXTRA_ROWS + n as usize;
    if history != want {
        r.line(
            "  CHECK FAILED",
            &format!("{history:9}"),
            &format!("history rows after live traffic, expected {want}"),
        );
    }

    r.print();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_synthetic_reply_parses_back_to_every_entry() {
        let buf = history_reply(120);
        let mut entries = 0;
        let mut has_more = false;
        for c in hxproto::wire::ChunkIter::over_message(&buf, buf.len()) {
            match c.tag {
                TAG_ENTRY => {
                    let e = hxproto::parse::parse_history_entry(c.data).expect("entry");
                    entries += 1;
                    assert_eq!(e.message_id, entries);
                }
                TAG_HAS_MORE => has_more = c.data == [1],
                _ => {}
            }
        }
        assert_eq!(entries, 120);
        assert!(has_more);
    }
}
