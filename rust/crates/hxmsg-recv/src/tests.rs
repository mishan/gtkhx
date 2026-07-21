//! Headless routing tests for the private-message receive handler, driven
//! through the `test_env` recording doubles for the member-model / emit C ABIs.

use super::*;

/// A sentinel boxed-event pointer (never dereferenced by the crate).
fn fake_event() -> *mut std::os::raw::c_void {
    0x5A5A_usize as *mut std::os::raw::c_void
}

fn recv(uid: u16, is_pm: bool, event: *mut std::os::raw::c_void) -> c_int {
    unsafe { hx_msg_recv(std::ptr::null_mut(), uid, c_int::from(is_pm), event) }
}

#[test]
fn private_message_emits() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(false));
    assert_eq!(recv(42, /*is_pm=*/ true, fake_event()), HX_MSG_EMITTED);
    assert_eq!(test_env::EMITTED.with(|c| c.take()), Some(fake_event()));
}

#[test]
fn ignored_private_message_is_dropped() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    assert_eq!(recv(42, /*is_pm=*/ true, fake_event()), HX_MSG_DROPPED);
    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}

#[test]
fn broadcast_reports_broadcast_without_emitting() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(false));
    // Broadcast branch: no boxed event, C renders it via broadcastmsg.
    assert_eq!(recv(7, /*is_pm=*/ false, std::ptr::null_mut()), HX_MSG_BROADCAST);
    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}

#[test]
fn ignored_broadcast_is_dropped() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    assert_eq!(recv(7, /*is_pm=*/ false, std::ptr::null_mut()), HX_MSG_DROPPED);
    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}
