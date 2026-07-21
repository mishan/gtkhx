//! Post-login sequencing decision (ported from `rcv.c`'s `rcv_task_login`).
//!
//! Once the LOGIN reply is walked, the client has to decide *when* to fire the
//! post-login fetches (USER_GETLIST, news, files). The rule is version-driven
//! and load-bearing on old servers:
//!
//! - A **1.0 / 1.2** server sends no `HTLS_DATA_VERSION` chunk, so `htlc->version`
//!   stays 0. It doesn't speak the 1.5 agreement flow, so there's no
//!   "after AGREEMENTAGREE" boundary to wait for — deliver NAME + ICON now and
//!   fire the fetches immediately.
//! - A **1.5+** server (`version >= 150`, in practice any non-zero version) takes
//!   the agreement path: `hx_send_agreement_agree` / the Agree click fire the
//!   fetches after the AGREEMENTAGREE round-trip. Here we only arm a 2 s fallback
//!   timer in case the agreement opcode never arrives.
//!
//! `already_fetched` is the single-fire guard (`htlc->flags.post_login_fetched`):
//! if the fetches have already gone out (e.g. via the agreement path racing the
//! LOGIN reply), do nothing.
//!
//! This is the pure decision; `rcv_task_login` performs the resulting side
//! effects (the wire sends, the GLib timer).

use std::os::raw::c_int;

/// What `rcv_task_login` should do after walking the LOGIN reply.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PostLoginAction {
    /// Fetches already fired — do nothing.
    Nothing,
    /// 1.0/1.2 server: send NAME + ICON and fire the post-login fetches now.
    FetchNow,
    /// 1.5+ server: wait for AGREEMENTAGREE; arm the 2 s fallback timer.
    ArmFallback,
}

/// The pure decision — mirrors the `version == 0 ? fetch-now : arm-fallback`
/// (guarded by `!already_fetched`) branch at the tail of `rcv_task_login`.
pub fn post_login_route(version: u16, already_fetched: bool) -> PostLoginAction {
    if already_fetched {
        PostLoginAction::Nothing
    } else if version == 0 {
        PostLoginAction::FetchNow
    } else {
        PostLoginAction::ArmFallback
    }
}

/// C-ABI outcomes for [`hx_post_login_route`].
pub const HX_POST_LOGIN_NOTHING: c_int = 0;
pub const HX_POST_LOGIN_FETCH_NOW: c_int = 1;
pub const HX_POST_LOGIN_ARM_FALLBACK: c_int = 2;

/// `int hx_post_login_route (guint16 version, int already_fetched)` — the C ABI
/// `rcv_task_login` calls to pick its post-login path. See [`post_login_route`].
#[no_mangle]
pub extern "C" fn hx_post_login_route(version: u16, already_fetched: c_int) -> c_int {
    match post_login_route(version, already_fetched != 0) {
        PostLoginAction::Nothing => HX_POST_LOGIN_NOTHING,
        PostLoginAction::FetchNow => HX_POST_LOGIN_FETCH_NOW,
        PostLoginAction::ArmFallback => HX_POST_LOGIN_ARM_FALLBACK,
    }
}

#[cfg(test)]
mod tests {
    use super::PostLoginAction::*;
    use super::*;

    #[test]
    fn versionless_server_fetches_immediately() {
        // 1.0/1.2 (no DATA_VERSION → version 0): fire now, no agreement coming.
        assert_eq!(post_login_route(0, false), FetchNow);
    }

    #[test]
    fn versioned_server_arms_the_fallback() {
        // Any non-zero version is a 1.5+ server that takes the agreement path;
        // we only arm the safety-net timer here.
        assert_eq!(post_login_route(150, false), ArmFallback);
        assert_eq!(post_login_route(190, false), ArmFallback);
        // The boundary is "version present at all", not ">= 150": a server that
        // advertised any version still waits for the agreement flow.
        assert_eq!(post_login_route(1, false), ArmFallback);
    }

    #[test]
    fn already_fetched_does_nothing() {
        // The single-fire guard wins regardless of version.
        assert_eq!(post_login_route(0, true), Nothing);
        assert_eq!(post_login_route(150, true), Nothing);
    }
}
