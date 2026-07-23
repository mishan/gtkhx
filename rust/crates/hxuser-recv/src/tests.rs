//! Headless routing tests for the roster receive handlers, driven through the
//! `test_env` recording doubles for the member-model / emit C ABIs.

use super::test_env::Emit;
use super::*;
use std::ffi::CString;

/// A live `USER_CHANGE` apply (incremental=1).
#[allow(clippy::too_many_arguments)]
fn change(uid: u16, nick_color: u32, name: &str, icon: u16, color: u16, is_new: bool, skip_self: bool) -> c_int {
    apply(uid, nick_color, name, icon, color, is_new, skip_self, /*incremental=*/ true)
}

/// The unified roster-apply — mirrors both callers (USER_CHANGE = incremental,
/// USER_LIST = not).
#[allow(clippy::too_many_arguments)]
fn apply(
    uid: u16,
    nick_color: u32,
    name: &str,
    icon: u16,
    color: u16,
    is_new: bool,
    skip_self: bool,
    incremental: bool,
) -> c_int {
    let cname = CString::new(name).unwrap();
    unsafe {
        hx_user_apply_recv(
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            uid,
            nick_color,
            cname.as_ptr(),
            icon,
            color,
            c_int::from(is_new),
            c_int::from(skip_self),
            c_int::from(incremental),
        )
    }
}

#[test]
fn new_user_routes_to_create() {
    test_env::reset();
    let r = change(7, 3, "Alice", 128, 4, /*is_new=*/ true, /*skip_self=*/ false);
    assert_eq!(r, HX_USER_CHANGE_CREATED);
    assert_eq!(
        test_env::take(),
        Some(Emit::Create {
            uid: 7,
            nick_color: 3,
            name: b"Alice".to_vec(),
            icon: 128,
            color: 4,
            incremental: true,
        })
    );
}

#[test]
fn existing_user_routes_to_change() {
    test_env::reset();
    let r = change(9, 5, "Alice2", 129, 2, /*is_new=*/ false, /*skip_self=*/ false);
    assert_eq!(r, HX_USER_CHANGE_CHANGED);
    assert_eq!(
        test_env::take(),
        Some(Emit::Change {
            uid: 9,
            nick_color: 5,
            name: b"Alice2".to_vec(),
            icon: 129,
            color: 2,
        })
    );
}

#[test]
fn self_join_is_skipped_without_emit() {
    test_env::reset();
    let r = change(1, 0, "Me", 128, 0, /*is_new=*/ true, /*skip_self=*/ true);
    assert_eq!(r, HX_USER_CHANGE_SKIPPED);
    assert_eq!(test_env::take(), None);
}

#[test]
fn bulk_load_new_user_creates_without_chime() {
    // USER_LIST login load: a new member emits user-create, but incremental=0
    // so the join chime stays silent.
    test_env::reset();
    let r = apply(7, 3, "Alice", 128, 4, /*is_new=*/ true, /*skip_self=*/ false, /*incremental=*/ false);
    assert_eq!(r, HX_USER_CHANGE_CREATED);
    assert_eq!(
        test_env::take(),
        Some(Emit::Create {
            uid: 7,
            nick_color: 3,
            name: b"Alice".to_vec(),
            icon: 128,
            color: 4,
            incremental: false,
        })
    );
}

#[test]
fn bulk_load_existing_user_upserts_silently() {
    // USER_LIST re-load of a member already in the room: fold the fields into
    // the model directly, no view signal.
    test_env::reset();
    let r = apply(9, 5, "Alice2", 129, 2, /*is_new=*/ false, /*skip_self=*/ false, /*incremental=*/ false);
    assert_eq!(r, HX_USER_CHANGE_UPDATED);
    assert_eq!(
        test_env::take(),
        Some(Emit::Upsert {
            uid: 9,
            nick_color: 5,
            name: b"Alice2".to_vec(),
            icon: 129,
            color: 2,
        })
    );
}

fn part(uid: u16) -> c_int {
    unsafe {
        hx_user_part_recv(
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            uid,
        )
    }
}

#[test]
fn part_of_member_emits_delete() {
    test_env::reset();
    test_env::CONTAINS.with(|c| c.set(true));
    assert_eq!(part(42), 1);
    assert_eq!(
        test_env::take(),
        Some(Emit::Delete {
            uid: 42,
            incremental: true
        })
    );
}

#[test]
fn part_of_non_member_is_ignored() {
    test_env::reset();
    test_env::CONTAINS.with(|c| c.set(false));
    assert_eq!(part(42), 0);
    assert_eq!(test_env::take(), None);
}

#[test]
fn user_info_publishes_the_pair() {
    test_env::reset();
    let name = CString::new("Bob").unwrap();
    let info = CString::new("hello there").unwrap();
    unsafe { hx_user_info_recv(11, name.as_ptr(), info.as_ptr(), 11) };
    assert_eq!(
        test_env::take(),
        Some(Emit::Info {
            uid: 11,
            name: b"Bob".to_vec(),
            info: b"hello there".to_vec(),
            len: 11,
        })
    );
}

#[test]
fn selfinfo_emits_self_updated() {
    test_env::reset();
    unsafe { hx_selfinfo_recv(std::ptr::null_mut()) };
    assert_eq!(test_env::take(), Some(Emit::SelfUpdated));
}

#[test]
fn rcv_selfinfo_parses_marks_logged_in_then_emits() {
    // The full SELFINFO handler: parse the frame, flip logged-in, emit.
    test_env::reset();
    let frame = [0u8; 8];
    unsafe { hx_rcv_user_selfinfo(std::ptr::null_mut(), frame.as_ptr(), frame.len()) };
    assert!(test_env::SELFINFO_PARSED.with(|c| c.get()));
    assert_eq!(test_env::LOGGED_IN.with(|c| c.get()), 1);
    assert_eq!(test_env::take(), Some(Emit::SelfUpdated));
}
