//! The icon senders: what goes out, what its reply is expected as, and which
//! refusals stay quiet.

use super::*;
use std::cell::{Cell, RefCell};

use hxproto::build::HxChunk;

use crate::send::expected;

/// A request sent: its opcode and its fields.
type Sent = (u32, Vec<(u16, Vec<u8>)>);

thread_local! {
    static SENT: RefCell<Vec<Sent>> = const { RefCell::new(Vec::new()) };
    static STATE: Cell<c_int> = const { Cell::new(-1) };
    static TIMER: Cell<c_uint> = const { Cell::new(0) };
}

pub(super) unsafe fn hlwrite_chunks(
    _htlc: *mut c_void,
    ty: u32,
    _flag: u32,
    chunks: *const HxChunk,
    hc: c_int,
) {
    let chunks = (0..hc as usize)
        .map(|i| {
            let c = &*chunks.add(i);
            let data = if c.len == 0 {
                Vec::new()
            } else {
                std::slice::from_raw_parts(c.data, c.len as usize).to_vec()
            };
            (c.tag, data)
        })
        .collect();
    SENT.with(|s| s.borrow_mut().push((ty, chunks)));
}
pub(super) unsafe fn hx_conn_gif_icons_state(_h: *const c_void) -> c_int {
    STATE.with(|s| s.get())
}
pub(super) unsafe fn hx_conn_set_gif_icons_state(_h: *mut c_void, v: c_int) {
    STATE.with(|s| s.set(v));
}
pub(super) unsafe fn hx_conn_gif_icons_probe_timer(_h: *const c_void) -> c_uint {
    TIMER.with(|t| t.get())
}
pub(super) unsafe fn hx_conn_set_gif_icons_probe_timer(_h: *mut c_void, v: c_uint) {
    TIMER.with(|t| t.set(v));
}
pub(super) unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}

const HTLC: *mut c_void = 0x30 as *mut _;

fn sent() -> Vec<Sent> {
    SENT.with(|s| std::mem::take(&mut *s.borrow_mut()))
}

/// Whether a refusal of the request on `trans` would stay quiet.
fn quiet(trans: u32) -> bool {
    unsafe { crate::recv::icon::failed(HTLC, trans, None) }
}

#[test]
fn the_probe_asks_for_every_icon_quietly_and_gives_up_in_time() {
    expected::take();
    sent();
    unsafe { hx_icon_probe(HTLC) };
    assert_eq!(sent(), [(1861, vec![])]);
    let said = expected::take();
    assert_eq!(said.len(), 1);
    assert_eq!(said[0].1, Expect::IconList);
    let timer = TIMER.with(|t| t.get());
    assert_ne!(timer, 0, "no watchdog");
    unsafe { glib::ffi::g_source_remove(timer) };

    // Overdue: no GIF icons. Settled already: left as it is.
    for (before, after) in [(GIF_ICONS_UNKNOWN, GIF_ICONS_UNSUPPORTED), (1, 1)] {
        STATE.with(|s| s.set(before));
        unsafe { probe_timeout(HTLC) };
        assert_eq!(STATE.with(|s| s.get()), after);
        assert_eq!(TIMER.with(|t| t.get()), 0);
    }
    assert!(quiet(said[0].0));
}

#[test]
fn an_icon_set_by_hand_is_refused_aloud_and_the_saved_one_quietly() {
    expected::take();
    sent();
    let gif = b"GIF89a....";
    unsafe {
        hx_icon_set(HTLC, gif.as_ptr(), gif.len());
        hx_icon_set_saved(HTLC, gif.as_ptr(), gif.len());
        hx_icon_clear(HTLC);
    }
    let set = (1862, vec![(0x0300, gif.to_vec())]);
    assert_eq!(sent(), [set.clone(), set, (1862, vec![(0x0300, vec![])])]);
    let said = expected::take();
    assert!(said.iter().all(|(_, w)| *w == Expect::IconSet), "{said:?}");
    let quiet: Vec<_> = said.iter().map(|(t, _)| quiet(*t)).collect();
    assert_eq!(quiet, [false, true, false]);
}

#[test]
fn what_is_not_a_gif_is_not_sent() {
    expected::take();
    sent();
    let png = b"\x89PNG\r\n\x1a\n";
    unsafe {
        hx_icon_set(HTLC, png.as_ptr(), png.len());
        hx_icon_set_saved(HTLC, png.as_ptr(), png.len());
    }
    assert!(sent().is_empty());
    assert!(expected::take().is_empty());
}

#[test]
fn a_users_icon_is_asked_for_by_uid() {
    expected::take();
    sent();
    unsafe { hx_icon_get(HTLC, 0x0102) };
    assert_eq!(sent(), [(1863, vec![(0x0067, vec![1, 2])])]);
    assert_eq!(expected::take()[0].1, Expect::Icon);
}
