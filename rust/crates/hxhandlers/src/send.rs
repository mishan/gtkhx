//! Wire-out senders for the domains whose send path has moved to Rust.
//!
//! Built over `hxproto`'s native builders, or `hxrequest`'s. These modules
//! keep the exact C ABI the C senders they replaced exported.

use std::os::raw::c_void;
#[cfg(not(test))]
use std::time::Instant;

use hxsession::Expect;

pub mod chat;
pub mod chat_history;
pub mod files;
pub mod friends;
pub mod icon;
pub mod msg;
pub mod news;
pub mod reactions;
pub mod user;

#[cfg(not(test))]
extern "C" {
    /// session_registry.c — the live connection with this serial, or NULL.
    fn hx_conn_with_serial(serial: u16) -> *mut c_void;
}

#[cfg(not(test))]
thread_local! {
    /// The timers set, by connection serial and what each calls.
    static WAKING: std::cell::RefCell<std::collections::HashSet<(u16, usize)>> =
        std::cell::RefCell::default();
}

/// Call `then` for `htlc` at `at`, by its serial, which outlives it: one
/// timer a connection and caller, which does whatever is due when it fires.
///
/// # Safety
/// `htlc` is a live connection; main thread.
#[cfg(not(test))]
pub(crate) unsafe fn wake(htlc: *mut c_void, at: Instant, then: unsafe fn(*mut c_void, Instant)) {
    let serial = gtkhx_core::conn::hx_conn_serial(htlc.cast());
    if !WAKING.with_borrow_mut(|w| w.insert((serial, then as usize))) {
        return;
    }
    let ms = at.saturating_duration_since(Instant::now()).as_millis() + 1;
    let ms = u32::try_from(ms).unwrap_or(u32::MAX);
    let data = Box::into_raw(Box::new((serial, then))).cast();
    glib::ffi::g_timeout_add(ms, Some(woke), data);
}

#[cfg(not(test))]
unsafe extern "C" fn woke(data: glib::ffi::gpointer) -> glib::ffi::gboolean {
    let (serial, then) = *Box::from_raw(data.cast::<(u16, unsafe fn(*mut c_void, Instant))>());
    WAKING.with_borrow_mut(|w| w.remove(&(serial, then as usize)));
    let htlc = hx_conn_with_serial(serial);
    if !htlc.is_null() {
        then(htlc, Instant::now());
    }
    glib::ffi::G_SOURCE_REMOVE
}

#[cfg(not(test))]
use gtkhx_core::conn::hx_conn_bridge_handle;
#[cfg(not(test))]
use hxnet::ffi::connection_expect;
#[cfg(not(test))]
use hxtask::send::next_trans;

/// The next request on `htlc` goes out with its reply expected: the session
/// turns it into `what`, or a failure, rather than handing the frame on.
/// Said before the request goes, so the reply cannot come first. The trans
/// it goes out on.
///
/// # Safety
/// `htlc` is a live connection; main thread.
pub(crate) unsafe fn expect_next(htlc: *mut c_void, what: Expect) -> u32 {
    let trans = next_trans(htlc.cast());
    connection_expect(hx_conn_bridge_handle(htlc.cast()).cast(), trans, what);
    trans
}

#[cfg(test)]
unsafe fn next_trans(_htlc: *mut c_void) -> u32 {
    expected::SAID.with(|s| s.borrow().len() as u32 + 1)
}
#[cfg(test)]
unsafe fn hx_conn_bridge_handle(_htlc: *const c_void) -> *mut c_void {
    std::ptr::null_mut()
}
#[cfg(test)]
unsafe fn connection_expect(_handle: *mut c_void, trans: u32, what: Expect) {
    expected::SAID.with(|s| s.borrow_mut().push((trans, what)));
}

#[cfg(test)]
pub(crate) mod expected {
    use std::cell::RefCell;

    thread_local! {
        /// Every expectation said, as (trans, what).
        pub static SAID: RefCell<Vec<(u32, hxsession::Expect)>> = const { RefCell::new(Vec::new()) };
    }

    pub fn take() -> Vec<(u32, hxsession::Expect)> {
        SAID.with(|s| std::mem::take(&mut *s.borrow_mut()))
    }
}
