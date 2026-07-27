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

fn push_chunk(v: &mut Vec<u8>, tag: u16, data: &[u8]) {
    v.extend_from_slice(&tag.to_be_bytes());
    v.extend_from_slice(&(data.len() as u16).to_be_bytes());
    v.extend_from_slice(data);
}

/// Build a wire frame: 22-byte header (type + zeroed trans/flag/len/len2/hc)
/// followed by TLV chunks — the shape the recv handlers receive.
fn frame(msg_type: u32, chunks: &[(u16, Vec<u8>)]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&msg_type.to_be_bytes());
    v.extend_from_slice(&[0u8; 18]);
    for (tag, data) in chunks {
        push_chunk(&mut v, *tag, data);
    }
    v
}

fn part_frame(uid: u16, cid: u32) -> Vec<u8> {
    use hotline_proto::messages::tag;
    frame(
        0x0000_0077, // HTLS_HDR_USER_PART (value irrelevant to parse; walks chunks)
        &[
            (tag::UID, uid.to_be_bytes().to_vec()),
            (tag::CHAT_ID, cid.to_be_bytes().to_vec()),
        ],
    )
}

fn rcv_part(f: &[u8]) {
    unsafe { hx_rcv_user_part(std::ptr::null_mut(), f.as_ptr(), f.len()) };
}

fn set_member(name: &str) {
    test_env::MEMBER.with(|c| {
        *c.borrow_mut() = Some(test_env::MemberSnap {
            icon: 0,
            status: 0,
            nick_color: 0,
            name: name.as_bytes().to_vec(),
        })
    });
}

fn take_notice() -> Option<test_env::Notice> {
    test_env::NOTICE.with(|c| c.borrow_mut().take())
}

#[test]
fn rcv_part_of_member_deletes_and_emits_notice() {
    test_env::reset();
    test_env::CONTAINS.with(|c| c.set(true));
    set_member("Bob");
    rcv_part(&part_frame(42, 3));
    assert_eq!(
        test_env::take(),
        Some(Emit::Delete {
            uid: 42,
            incremental: true
        })
    );
    assert_eq!(
        take_notice(),
        Some(test_env::Notice {
            cid: 3,
            kind: HX_USER_NOTICE_PART,
            name: b"Bob".to_vec(),
            old_name: Vec::new(),
        })
    );
}

#[test]
fn rcv_part_of_non_member_no_delete_no_notice() {
    test_env::reset();
    test_env::CONTAINS.with(|c| c.set(false));
    // MEMBER stays None → get_info returns FALSE.
    rcv_part(&part_frame(42, 3));
    assert_eq!(test_env::take(), None);
    assert_eq!(take_notice(), None);
}

fn change_frame(uid: u16, cid: u32, name: &str, icon: u16) -> Vec<u8> {
    use hotline_proto::messages::tag;
    frame(
        0x0000_0076, // HTLS_HDR_USER_CHANGE (value irrelevant to parse)
        &[
            (tag::UID, uid.to_be_bytes().to_vec()),
            (tag::ICON, icon.to_be_bytes().to_vec()),
            (tag::CHAT_ID, cid.to_be_bytes().to_vec()),
            (tag::NAME, name.as_bytes().to_vec()),
        ],
    )
}

fn rcv_change(f: &[u8]) {
    unsafe { hx_rcv_user_change(std::ptr::null_mut(), f.as_ptr(), f.len()) };
}

#[test]
fn rcv_change_new_member_creates_and_emits_join() {
    test_env::reset();
    // No existing member → create; not us (self uid differs).
    test_env::SELF_UID.with(|c| c.set(99));
    rcv_change(&change_frame(7, 0, "Alice", 128));
    assert_eq!(
        test_env::take(),
        Some(Emit::Create {
            uid: 7,
            nick_color: HX_NICK_COLOR_NONE,
            name: b"Alice".to_vec(),
            icon: 128,
            color: 0,
            incremental: true,
        })
    );
    assert_eq!(
        take_notice(),
        Some(test_env::Notice {
            cid: 0,
            kind: HX_USER_NOTICE_JOIN,
            name: b"Alice".to_vec(),
            old_name: Vec::new(),
        })
    );
}

#[test]
fn rcv_change_existing_rename_emits_rename() {
    test_env::reset();
    test_env::SELF_UID.with(|c| c.set(99));
    set_member("Bob"); // old name
    rcv_change(&change_frame(7, 0, "Bobby", 128));
    assert_eq!(
        test_env::take(),
        Some(Emit::Change {
            uid: 7,
            nick_color: 0, // preserved from old snapshot (no wire nick colour)
            name: b"Bobby".to_vec(),
            icon: 128,
            color: 0,
        })
    );
    assert_eq!(
        take_notice(),
        Some(test_env::Notice {
            cid: 0,
            kind: HX_USER_NOTICE_RENAME,
            name: b"Bobby".to_vec(),
            old_name: b"Bob".to_vec(),
        })
    );
}

#[test]
fn rcv_change_ignored_user_emits_change_but_no_notice() {
    test_env::reset();
    test_env::SELF_UID.with(|c| c.set(99));
    test_env::IGNORE.with(|c| c.set(true));
    set_member("Bob");
    rcv_change(&change_frame(7, 0, "Bobby", 128));
    // The view still gets the row update, but no rename notice line.
    assert!(matches!(test_env::take(), Some(Emit::Change { uid: 7, .. })));
    assert_eq!(take_notice(), None);
}

#[test]
fn rcv_change_task_error_bails() {
    test_env::reset();
    test_env::TASK_ERROR.with(|c| c.set(true));
    rcv_change(&change_frame(7, 0, "Alice", 128));
    assert_eq!(test_env::take(), None);
    assert_eq!(take_notice(), None);
}

#[test]
fn rcv_change_adopts_self_uid_when_selfinfo_omitted_it() {
    test_env::reset();
    // SELFINFO-less 1.9 server: self uid still 0, but the first USER_CHANGE
    // echoes our own nick with our freshly-assigned uid → adopt it, and skip
    // creating our own row.
    test_env::SELF_UID.with(|c| c.set(0));
    test_env::set_self_name("Me");
    rcv_change(&change_frame(5, 0, "Me", 128));
    assert_eq!(test_env::SELF_UID.with(|c| c.get()), 5); // adopted
    assert_eq!(test_env::take(), None); // skip-self-create, no emit
    assert_eq!(take_notice(), None);
}

#[test]
fn rcv_change_self_updates_bookkeeping() {
    test_env::reset();
    // We are uid 7 and already a member (existing) → CHANGED, self bookkeeping.
    test_env::SELF_UID.with(|c| c.set(7));
    set_member("Me");
    rcv_change(&change_frame(7, 0, "Me", 200));
    // icon mirrored into htlc.
    assert_eq!(test_env::SELF_ICON.with(|c| c.get()), 200);
    // self rename notice is never emitted for our own change.
    assert_eq!(take_notice(), None);
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
