//! Wire-out senders for the domains whose send path has moved to Rust.
//!
//! Built over `hxproto`'s native builders. The remaining C senders
//! (`hx_kick_user`, `hx_get_user_info`, …) are unaffected — these modules keep
//! the exact C ABI their former crates exported.

use std::os::raw::c_void;

use hxsession::Expect;

pub mod chat;
pub mod chat_history;
pub mod files;
pub mod news;

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
