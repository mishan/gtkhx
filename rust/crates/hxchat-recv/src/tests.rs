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

/// Returns the applied flag; records the emit (if any) in SUBJECT_EMITTED.
fn subject(cid: u32, new: &str, current: &str) -> bool {
    let cnew = CString::new(new).unwrap();
    let ccur = CString::new(current).unwrap();
    let applied = unsafe {
        hx_chat_subject_recv(
            std::ptr::null_mut(),
            cid,
            cnew.as_ptr(),
            new.len(),
            ccur.as_ptr(),
        )
    };
    applied != 0
}

#[test]
fn emits_chat_subject_when_changed() {
    test_env::reset();
    assert!(subject(3, "New Topic", "Old Topic"));
    assert_eq!(
        test_env::SUBJECT_EMITTED.with(|c| c.take()),
        Some((3, b"New Topic".to_vec()))
    );
}

#[test]
fn suppresses_unchanged_subject() {
    test_env::reset();
    assert!(!subject(3, "Same", "Same"));
    assert_eq!(test_env::SUBJECT_EMITTED.with(|c| c.take()), None);
}

#[test]
fn suppresses_empty_subject() {
    test_env::reset();
    // subject_len 0 → no announcement, even against a non-empty current.
    let ccur = CString::new("Existing").unwrap();
    let applied =
        unsafe { hx_chat_subject_recv(std::ptr::null_mut(), 3, c"".as_ptr(), 0, ccur.as_ptr()) };
    assert_eq!(applied, 0);
    assert_eq!(test_env::SUBJECT_EMITTED.with(|c| c.take()), None);
}

/// A sentinel boxed-event pointer (never dereferenced by the crate).
fn fake_event() -> *mut std::os::raw::c_void {
    0xC0FE_usize as *mut std::os::raw::c_void
}

fn chat(uid: u16) -> c_int {
    unsafe { hx_chat_recv(std::ptr::null_mut(), std::ptr::null_mut(), uid, fake_event()) }
}

#[test]
fn emits_chat_when_not_ignored() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(false));
    assert_eq!(chat(42), 1);
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), Some(fake_event()));
}

#[test]
fn drops_chat_from_ignored_user() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    assert_eq!(chat(42), 0);
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), None);
}

#[test]
fn system_line_uid_zero_always_emits() {
    // uid 0 is a server/system line — the ignore list is never consulted.
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    assert_eq!(chat(0), 1);
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), Some(fake_event()));
}
