//! The tracker fetch's main-thread wakeup (`hxnet_tracker_fetch_watch`):
//! a watcher drains a whole walk on its wakeups alone, and hears nothing
//! once the fetch is closed.
//!
//! One test, because both halves iterate GLib's default main context,
//! which only one thread at a time can own.

use std::cell::Cell;
use std::ffi::{c_void, CString};
use std::net::TcpListener;
use std::time::{Duration, Instant};

use hxnet::ffi::{
    hxnet_tracker_fetch_close, hxnet_tracker_fetch_open, hxnet_tracker_fetch_poll,
    hxnet_tracker_fetch_watch, HxnetTrackerEvent, HxnetTrackerFetch, HXNET_TRK_KIND_ERROR,
    HXNET_TRK_POLL_CLOSED, HXNET_TRK_POLL_EVENT,
};

thread_local! {
    static WAKES: Cell<u32> = const { Cell::new(0) };
    static ERRORS: Cell<u32> = const { Cell::new(0) };
    static CLOSED: Cell<bool> = const { Cell::new(false) };
}

unsafe extern "C" fn drain(user_data: *mut c_void) {
    WAKES.with(|w| w.set(w.get() + 1));
    let handle = user_data as *mut HxnetTrackerFetch;
    loop {
        let mut ev = std::mem::MaybeUninit::<HxnetTrackerEvent>::zeroed();
        match hxnet_tracker_fetch_poll(handle, ev.as_mut_ptr()) {
            HXNET_TRK_POLL_EVENT => {
                if ev.assume_init_ref().kind == HXNET_TRK_KIND_ERROR {
                    ERRORS.with(|e| e.set(e.get() + 1));
                }
            }
            HXNET_TRK_POLL_CLOSED => {
                CLOSED.with(|c| c.set(true));
                return;
            }
            _ => return,
        }
    }
}

unsafe extern "C" fn never(_: *mut c_void) {
    panic!("woken after close");
}

/// A port nothing listens on: bound, then released.
fn dead_port() -> u16 {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    l.local_addr().unwrap().port()
}

fn open(urls: &[CString]) -> *mut HxnetTrackerFetch {
    let ptrs: Vec<_> = urls.iter().map(|u| u.as_ptr()).collect();
    let h = unsafe {
        hxnet_tracker_fetch_open(
            ptrs.as_ptr(),
            ptrs.len(),
            0,
            1_000,
            std::ptr::null(),
            None,
            std::ptr::null_mut(),
        )
    };
    assert!(!h.is_null());
    h
}

fn iterate_for(ctx: &glib::MainContext, d: Duration, done: impl Fn() -> bool) {
    let until = Instant::now() + d;
    while !done() && Instant::now() < until {
        ctx.iteration(false);
        std::thread::sleep(Duration::from_millis(1));
    }
}

#[test]
fn a_watcher_drains_on_wakeups_and_hears_nothing_after_close() {
    let ctx = glib::MainContext::default();
    let _owner = ctx.acquire().unwrap();

    // Two trackers that refuse the connection: an error each, then Done
    // and the close — all of it delivered by wakeups, with no timer.
    let urls = [
        CString::new(format!("127.0.0.1:{}", dead_port())).unwrap(),
        CString::new(format!("127.0.0.1:{}", dead_port())).unwrap(),
    ];
    let h = open(&urls);
    unsafe { hxnet_tracker_fetch_watch(h, Some(drain), h.cast()) };
    iterate_for(&ctx, Duration::from_secs(20), || CLOSED.with(Cell::get));
    assert!(
        CLOSED.with(Cell::get),
        "the drain never saw the fetch close"
    );
    assert_eq!(ERRORS.with(Cell::get), 2);
    // One per tracker, one for Done and one for the close, some coalesced.
    let wakes = WAKES.with(Cell::get);
    assert!((1..=5).contains(&wakes), "{wakes} wakeups");
    unsafe { hxnet_tracker_fetch_close(h) };

    // Closed straight after watching, with a wakeup already queued: it
    // must not be delivered.
    let h = open(&urls);
    unsafe {
        hxnet_tracker_fetch_watch(h, Some(never), std::ptr::null_mut());
        hxnet_tracker_fetch_close(h);
    }
    iterate_for(&ctx, Duration::from_millis(300), || false);
}
