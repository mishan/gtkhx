//! C ABI for `hxproto::login`, the post-login sequencing decision.

use std::os::raw::c_int;

use hxproto::login::{post_login_route, PostLoginAction};

/// C-ABI outcomes for [`hx_post_login_route`].
pub const HX_POST_LOGIN_NOTHING: c_int = 0;
pub const HX_POST_LOGIN_FETCH_NOW: c_int = 1;
pub const HX_POST_LOGIN_ARM_FALLBACK: c_int = 2;

/// `int hx_post_login_route (guint16 version, int already_fetched)` — the C ABI
/// `rcv_task_login` calls to pick its post-login path. See
/// `hxproto::login::post_login_route`.
#[no_mangle]
pub extern "C" fn hx_post_login_route(version: u16, already_fetched: c_int) -> c_int {
    match post_login_route(version, already_fetched != 0) {
        PostLoginAction::Nothing => HX_POST_LOGIN_NOTHING,
        PostLoginAction::FetchNow => HX_POST_LOGIN_FETCH_NOW,
        PostLoginAction::ArmFallback => HX_POST_LOGIN_ARM_FALLBACK,
    }
}
