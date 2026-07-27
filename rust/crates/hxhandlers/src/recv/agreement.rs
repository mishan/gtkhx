//! Agreement receive path (ported from `rcv.c`).
//!
//! `hx_rcv_agreement_file` parses the server's login agreement, then takes one
//! of two paths: if the server sent an actual agreement, show it (emit the
//! `agreement` signal so the view pops the Agree window); if it didn't (no-
//! agreement config, or a malformed payload), there's nothing for the user to
//! click, so the client sends `AGREEMENTAGREE` itself to complete login. This
//! crate owns that decision + the emit; the C handler keeps the wire parse and
//! — on the auto path — the `AGREEMENTAGREE` wire send (`hx_send_agreement_agree`).

use std::os::raw::{c_char, c_int, c_void};

// The singleton `GtkhxSession` GObject and the `agreement` signal emit come
// from the gtkhx-session crate as a normal Cargo dependency, so rustc checks
// these signatures. They used to be a hand-written `extern "C"` block, which
// only linked — a signature drift surfaced as a corrupt call at runtime, or
// not at all. The `#[cfg(test)]` stubs below still shadow these in test builds.
#[cfg(not(test))]
use gtkhx_session::{gtkhx_session_emit_agreement, gtkhx_session_get_default};

/// The server sent no agreement (or a malformed one): the C side should send
/// `AGREEMENTAGREE` itself to finish login.
pub const HX_AGREEMENT_ACT_AUTO_AGREE: c_int = 0;
/// The agreement was shown — the `agreement` signal fired and the view pops the
/// Agree window; nothing more for the C side to do.
pub const HX_AGREEMENT_ACT_SHOWN: c_int = 1;

/// `int hx_agreement_recv (sess, has_agreement, buf, len)` — route a parsed login
/// agreement. When `has_agreement` is set (the wire carried a real agreement),
/// emit the `agreement` signal and return [`HX_AGREEMENT_ACT_SHOWN`]. Otherwise
/// return [`HX_AGREEMENT_ACT_AUTO_AGREE`] so the C handler sends `AGREEMENTAGREE`
/// itself (no-agreement servers complete login inside that round-trip, and it's
/// what triggers the banner on banner-configured servers).
///
/// The C side does the classification (`result == HX_AGREEMENT_OK`) and owns the
/// `AGREEMENTAGREE` wire send; this crate owns the show-vs-auto decision + emit.
///
/// # Safety
/// `sess` is the opaque `session *` the signal forwards; `buf` is a valid C
/// string of the sanitised agreement body (only read when `has_agreement`).
#[no_mangle]
pub unsafe extern "C" fn hx_agreement_recv(
    sess: *mut c_void,
    has_agreement: c_int,
    buf: *const c_char,
    len: u16,
) -> c_int {
    if has_agreement != 0 {
        gtkhx_session_emit_agreement(gtkhx_session_get_default(), sess, buf, len);
        HX_AGREEMENT_ACT_SHOWN
    } else {
        HX_AGREEMENT_ACT_AUTO_AGREE
    }
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::Cell;

    thread_local! {
        /// Records the last emitted agreement as (body-bytes, len), or None.
        pub static EMITTED: Cell<Option<(Vec<u8>, u16)>> = const { Cell::new(None) };
    }

    pub fn reset() {
        EMITTED.with(|c| c.set(None));
    }
}

#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_agreement(
    _self_: *mut c_void,
    _sess: *mut c_void,
    agreement: *const c_char,
    len: u16,
) {
    let bytes = if agreement.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(agreement).to_bytes().to_vec()
    };
    test_env::EMITTED.with(|c| c.set(Some((bytes, len))));
}

#[cfg(test)]
mod tests;
