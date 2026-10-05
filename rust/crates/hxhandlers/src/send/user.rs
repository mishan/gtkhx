//! The user list, asked for once the login settles.

use std::os::raw::{c_int, c_void};

use hxproto::build::HxChunk;
use hxproto::messages::ClientHdr;
use hxsession::Expect;

#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;

/// `void hx_user_list_get (struct htlc_conn *htlc)` — ask for the user list
/// (USER_GETLIST, no fields). Its reply comes back as the session's
/// `UserList`, which loads the public chat's roster and then the news.
/// Once a login, so what the last connection asked for goes with it.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_get(htlc: *mut c_void) {
    if htlc.is_null() {
        return;
    }
    crate::recv::user::joins_forget(htlc);
    super::expect_next(htlc, Expect::UserList);
    hlwrite_chunks(
        htlc.cast(),
        ClientHdr::UserGetList as u32,
        0,
        std::ptr::null::<HxChunk>(),
        0 as c_int,
    );
}

#[cfg(test)]
use tests::hlwrite_chunks;

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    thread_local! {
        static SENT: RefCell<Vec<(u32, c_int)>> = const { RefCell::new(Vec::new()) };
    }

    pub(super) unsafe fn hlwrite_chunks(
        _htlc: *mut c_void,
        ty: u32,
        _flag: u32,
        _chunks: *const HxChunk,
        hc: c_int,
    ) {
        SENT.with(|s| s.borrow_mut().push((ty, hc)));
    }

    #[test]
    fn the_user_list_s_reply_is_expected() {
        crate::send::expected::take();
        unsafe { hx_user_list_get(std::ptr::dangling_mut()) };
        assert_eq!(crate::send::expected::take(), [(1, Expect::UserList)]);
        assert_eq!(SENT.with(|s| s.take()), [(300, 0)]);
    }
}
