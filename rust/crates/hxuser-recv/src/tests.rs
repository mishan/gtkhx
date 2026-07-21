//! Headless routing tests for the roster receive handlers, driven through the
//! `test_env` recording doubles for the member-model / emit C ABIs.

use super::test_env::Emit;
use super::*;
use std::ffi::CString;

#[allow(clippy::too_many_arguments)]
fn change(uid: u16, nick_color: u32, name: &str, icon: u16, color: u16, is_new: bool, skip_self: bool) -> c_int {
    let cname = CString::new(name).unwrap();
    unsafe {
        hx_user_change_recv(
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            uid,
            nick_color,
            cname.as_ptr(),
            icon,
            color,
            c_int::from(is_new),
            c_int::from(skip_self),
            /*incremental=*/ 1,
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
