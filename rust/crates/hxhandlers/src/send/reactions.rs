//! The Reactions draft's requests (capability bit 19), sent only where the
//! session has reactions: Get Reactions, for lines whose reactions the
//! view asks after. What the reply says arrives on the `reactions` signal
//! (`recv::reactions`).

use std::os::raw::{c_int, c_void};

use hxproto::target::Target;
use hxrequest::reactions::{self as req, MAX_TARGETS};

use crate::recv::reactions::{asked, limits};

#[cfg(test)]
use crate::send::friends::tests::{hlwrite_chunks, next_trans};
#[cfg(not(test))]
use hxtask::send::{hlwrite_chunks, next_trans};

/// Ask after the reactions on `targets` of a kind the server takes, in as
/// many Get Reactions as it takes.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub unsafe fn refresh(htlc: *mut c_void, targets: &[Target]) {
    let Some(l) = limits(htlc) else {
        return;
    };
    let taken: Vec<Target> = targets.iter().filter(|t| l.takes(t)).cloned().collect();
    for batch in taken.chunks(MAX_TARGETS) {
        get(htlc, batch);
    }
}

/// One Get Reactions after `targets`.
///
/// # Safety
/// See [`refresh`].
pub(crate) unsafe fn get(htlc: *mut c_void, targets: &[Target]) {
    let Some(r) = req::get(targets, None) else {
        return;
    };
    let trans = next_trans(htlc.cast());
    asked(htlc, trans, targets.to_vec());
    r.with_hx_chunks(|c| hlwrite_chunks(htlc.cast(), r.opcode, 0, c.as_ptr(), c.len() as c_int));
}
