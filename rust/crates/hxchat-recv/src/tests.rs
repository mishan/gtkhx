//! Headless signal-behaviour tests for the chat receive handlers, driven
//! through the `test_env` recording doubles for the member-model / emit C ABIs.

use super::*;
use std::ffi::CString;

fn invite(cid: u32, uid: u16, name: &str) {
    let cname = CString::new(name).unwrap();
    // htlc / member_model are opaque and unused by the doubles.
    unsafe {
        hx_chat_invite_recv(
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            cid,
            uid,
            cname.as_ptr(),
        );
    }
}

#[test]
fn emits_chat_invitation_when_not_ignored() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(false));

    invite(7, 42, "Inviter");

    let got = test_env::EMITTED.with(|c| c.take());
    assert_eq!(got, Some((7, b"Inviter".to_vec())));
}

#[test]
fn drops_invitation_from_ignored_user() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));

    invite(7, 42, "Blocked");

    // Ignored inviter → no chat-invitation signal.
    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}
