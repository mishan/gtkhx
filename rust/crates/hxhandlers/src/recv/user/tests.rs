//! Headless routing tests for the roster receive handlers, driven through the
//! `test_env` recording doubles for the member-model / emit C ABIs.

use super::test_env::Emit;
use super::*;
use crate::recv::chat::test_env::Emitted;
use std::ffi::CString;

/// A live `USER_CHANGE` apply (incremental=1).
#[allow(clippy::too_many_arguments)]
fn change(
    uid: u16,
    nick_color: u32,
    name: &str,
    icon: u16,
    color: u16,
    is_new: bool,
    skip_self: bool,
) -> c_int {
    apply(
        uid, nick_color, name, icon, color, is_new, skip_self, /*incremental=*/ true,
    )
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
    let r = change(
        7, 3, "Alice", 128, 4, /*is_new=*/ true, /*skip_self=*/ false,
    );
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
    let r = change(
        9, 5, "Alice2", 129, 2, /*is_new=*/ false, /*skip_self=*/ false,
    );
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
    let r = change(
        1, 0, "Me", 128, 0, /*is_new=*/ true, /*skip_self=*/ true,
    );
    assert_eq!(r, HX_USER_CHANGE_SKIPPED);
    assert_eq!(test_env::take(), None);
}

#[test]
fn bulk_load_new_user_creates_without_chime() {
    // USER_LIST login load: a new member emits user-create, but incremental=0
    // so the join chime stays silent.
    test_env::reset();
    let r = apply(
        7, 3, "Alice", 128, 4, /*is_new=*/ true, /*skip_self=*/ false,
        /*incremental=*/ false,
    );
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
    let r = apply(
        9, 5, "Alice2", 129, 2, /*is_new=*/ false, /*skip_self=*/ false,
        /*incremental=*/ false,
    );
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

fn user(uid: u16, icon: u16, status: Option<u16>, name: &str, color: Option<u32>) -> User {
    User {
        uid,
        icon,
        status,
        name: name.into(),
        color,
    }
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
    unsafe { left(std::ptr::null_mut(), 3, 42) };
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
    unsafe { left(std::ptr::null_mut(), 3, 42) };
    assert_eq!(test_env::take(), None);
    assert_eq!(take_notice(), None);
}

/// A user change on chat `cid`, as the session reads one: no status or
/// colour, so the member's are kept.
fn rcv_change(cid: u32, uid: u16, name: &str, icon: u16) {
    unsafe {
        changed(
            std::ptr::null_mut(),
            cid,
            &user(uid, icon, None, name, None),
        )
    };
}

#[test]
fn rcv_change_new_member_creates_and_emits_join() {
    test_env::reset();
    // No existing member → create; not us (self uid differs).
    test_env::SELF_UID.with(|c| c.set(99));
    rcv_change(0, 7, "Alice", 128);
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
    rcv_change(0, 7, "Bobby", 128);
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
    rcv_change(0, 7, "Bobby", 128);
    // The view still gets the row update, but no rename notice line.
    assert!(matches!(
        test_env::take(),
        Some(Emit::Change { uid: 7, .. })
    ));
    assert_eq!(take_notice(), None);
}

#[test]
fn a_change_that_keeps_a_non_ascii_name_is_no_rename() {
    // The model holds names decoded, and the session decodes them too: a
    // Mac Roman name compares equal to itself.
    test_env::reset();
    test_env::SELF_UID.with(|c| c.set(99));
    set_member("René");
    unsafe {
        changed(
            std::ptr::null_mut(),
            0,
            &user(7, 128, Some(1), "René", None),
        )
    };
    assert!(matches!(
        test_env::take(),
        Some(Emit::Change {
            uid: 7,
            color: 1,
            ..
        })
    ));
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
    rcv_change(0, 5, "Me", 128);
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
    rcv_change(0, 7, "Me", 200);
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
    unsafe { hx_user_info_recv(std::ptr::null_mut(), 11, name.as_ptr(), info.as_ptr(), 11) };
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

// ---- user lists, joins, a new chat, user info ------------------------------

/// A sentinel chat pointer (the doubles ignore its value).
const FAKE_CHAT_PTR: *mut c_void = 0x2 as *mut c_void;

unsafe fn load_users(users: &[User]) {
    load(std::ptr::null_mut(), FAKE_CHAT_PTR, users, None);
}

#[test]
fn user_list_new_user_creates_without_join_chime() {
    test_env::reset();
    test_env::CONTAINS.with(|c| c.set(false)); // not a member yet → is_new
    unsafe { load_users(&[user(7, 128, Some(4), "Alice", None)]) };
    assert_eq!(
        test_env::take(),
        Some(Emit::Create {
            uid: 7,
            nick_color: HX_NICK_COLOR_NONE,
            name: b"Alice".to_vec(),
            icon: 128,
            color: 4,
            incremental: false, // bulk login load, chime suppressed
        })
    );
}

#[test]
fn user_list_existing_user_upserts_silently() {
    test_env::reset();
    test_env::CONTAINS.with(|c| c.set(true)); // already a member → silent upsert
    unsafe { load_users(&[user(9, 130, Some(2), "Bob", None)]) };
    assert_eq!(
        test_env::take(),
        Some(Emit::Upsert {
            uid: 9,
            nick_color: HX_NICK_COLOR_NONE,
            name: b"Bob".to_vec(),
            icon: 130,
            color: 2,
        })
    );
}

#[test]
fn user_list_colored_nick_mirrors_onto_self() {
    test_env::reset();
    test_env::SELF_UID.with(|c| c.set(5));
    test_env::CONTAINS.with(|c| c.set(true));
    unsafe { load_users(&[user(5, 100, Some(1), "Me", Some(0x0011_2233))]) };
    assert_eq!(test_env::SELF_NICK_COLOR.with(|c| c.get()), 0x0011_2233);
    assert_eq!(
        test_env::take(),
        Some(Emit::Upsert {
            uid: 5,
            nick_color: 0x0011_2233,
            name: b"Me".to_vec(),
            icon: 100,
            color: 1,
        })
    );
}

#[test]
fn user_list_adopts_self_uid_when_unset() {
    test_env::reset();
    test_env::SELF_UID.with(|c| c.set(0)); // no self uid yet
    test_env::SELF_ICON.with(|c| c.set(100));
    test_env::set_self_name("Me");
    test_env::CONTAINS.with(|c| c.set(false));
    unsafe { load_users(&[user(42, 100, Some(1), "Me", None)]) };
    assert_eq!(test_env::SELF_UID.with(|c| c.get()), 42);
}

#[test]
fn the_login_s_user_list_loads_users_then_reloads_news() {
    test_env::reset();
    test_env::CONTAINS.with(|c| c.set(false));
    unsafe {
        listed(
            std::ptr::null_mut(),
            &[user(1, 1, Some(1), "X", None)],
            None,
        )
    };
    assert!(test_env::RELOAD_NEWS.with(|c| c.get()));
    assert!(matches!(
        test_env::take(),
        Some(Emit::Create { uid: 1, .. })
    ));
}

#[test]
fn a_join_makes_its_chat_only_once_answered() {
    for exists in [false, true] {
        test_env::reset();
        test_env::CHAT_EXISTS.with(|c| c.set(exists));
        test_env::CONTAINS.with(|c| c.set(false));
        test_env::CHAT_CID.with(|c| c.set(9));
        let users = [user(3, 1, Some(1), "Y", None)];
        unsafe { joined(std::ptr::null_mut(), 8, 9, &users, Some("Café")) };
        let made: &[u32] = if exists { &[] } else { &[9] };
        assert_eq!(test_env::CHATS_MADE.with(|c| c.take()), made);
        assert!(matches!(
            test_env::take(),
            Some(Emit::Create { uid: 3, .. })
        ));
        assert_eq!(
            crate::recv::chat::test_env::emitted(),
            [Emitted::Subject(9, "Café".into())]
        );
    }
}

#[test]
fn a_join_answered_after_the_user_left_the_chat_is_ignored() {
    test_env::reset();
    test_env::CHAT_EXISTS.with(|c| c.set(false));
    let h = std::ptr::dangling_mut();
    join_requested(h, 8, 9);
    join_parted(h, 9);
    unsafe { joined(h, 8, 9, &[user(3, 1, Some(1), "Y", None)], None) };
    assert_eq!(test_env::CHATS_MADE.with(|c| c.take()), [] as [u32; 0]);
    assert_eq!(test_env::take(), None);
}

#[test]
fn a_refused_join_drops_only_a_chat_nothing_shows() {
    // (window, members, dropped)
    for (view, members, dropped) in [(false, 0, true), (true, 0, false), (false, 2, false)] {
        test_env::reset();
        test_env::VIEW.with(|c| c.set(view));
        test_env::MEMBERS.with(|c| c.set(members));
        let h = std::ptr::dangling_mut();
        join_requested(h, 8, 9);
        unsafe { failed(h, 7) }; // not the join's
        assert!(!test_env::CHAT_DELETED.with(|c| c.get()));
        unsafe { failed(h, 8) };
        assert_eq!(test_env::CHAT_DELETED.with(|c| c.get()), dropped);
    }
}

#[test]
fn us_in_a_new_chat_makes_the_chat_but_not_our_row() {
    test_env::reset();
    test_env::CHAT_EXISTS.with(|c| c.set(false));
    test_env::SELF_UID.with(|c| c.set(5));
    unsafe { changed(std::ptr::null_mut(), 9, &user(5, 128, Some(0), "Me", None)) };
    assert_eq!(test_env::CHATS_MADE.with(|c| c.take()), [9]);
    // Our own row waits for a list, as for any change that is new to us.
    assert_eq!(test_env::take(), None);
}

fn push_chunk(v: &mut Vec<u8>, tag: u16, data: &[u8]) {
    v.extend_from_slice(&tag.to_be_bytes());
    v.extend_from_slice(&(data.len() as u16).to_be_bytes());
    v.extend_from_slice(data);
}

/// A wire frame: 22-byte header (type + zeroed trans/flag/len/len2/hc)
/// followed by TLV chunks — the shape the reply handlers receive.
fn frame(msg_type: u32, chunks: &[(u16, Vec<u8>)]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&msg_type.to_be_bytes());
    v.extend_from_slice(&[0u8; 18]);
    for (tag, data) in chunks {
        push_chunk(&mut v, *tag, data);
    }
    v
}

#[test]
fn user_info_publishes_when_both_present() {
    use hxproto::messages::tag;
    test_env::reset();
    let uid_box = Box::into_raw(Box::new(11u16)) as *mut c_void;
    let f = frame(
        0,
        &[
            (tag::NAME, b"Alice".to_vec()),
            (tag::BODY, b"info text".to_vec()),
        ],
    );
    unsafe {
        rcv_task_user_info(
            std::ptr::null_mut(),
            f.as_ptr(),
            f.len(),
            uid_box,
            std::ptr::null_mut(),
        )
    };
    assert_eq!(
        test_env::take(),
        Some(Emit::Info {
            uid: 11,
            name: b"Alice".to_vec(),
            info: b"info text".to_vec(),
            len: 9,
        })
    );
}

/// A BODY carrying an interior NUL is truncated at the NUL (C-string
/// semantics), and the emitted len must match the truncated buffer — not the
/// full wire length — so a downstream length-aware reader can't run past it.
#[test]
fn user_info_body_interior_nul_truncates_len() {
    use hxproto::messages::tag;
    test_env::reset();
    let uid_box = Box::into_raw(Box::new(11u16)) as *mut c_void;
    let f = frame(
        0,
        &[
            (tag::NAME, b"Alice".to_vec()),
            (tag::BODY, b"ab\0cd".to_vec()),
        ],
    );
    unsafe {
        rcv_task_user_info(
            std::ptr::null_mut(),
            f.as_ptr(),
            f.len(),
            uid_box,
            std::ptr::null_mut(),
        )
    };
    assert_eq!(
        test_env::take(),
        Some(Emit::Info {
            uid: 11,
            name: b"Alice".to_vec(),
            info: b"ab".to_vec(), // truncated at the interior NUL
            len: 2,               // matches info_c, not the full wire length (5)
        })
    );
}

#[test]
fn user_info_dropped_when_info_empty() {
    use hxproto::messages::tag;
    test_env::reset();
    let uid_box = Box::into_raw(Box::new(11u16)) as *mut c_void;
    let f = frame(0, &[(tag::NAME, b"Alice".to_vec())]); // no BODY → gate fails
    unsafe {
        rcv_task_user_info(
            std::ptr::null_mut(),
            f.as_ptr(),
            f.len(),
            uid_box,
            std::ptr::null_mut(),
        )
    };
    assert_eq!(test_env::take(), None);
}

// ---- Mac Roman user-info text ---------------------------------------------

#[test]
fn user_info_text_is_decoded_from_mac_roman() {
    // Hotline text is Mac Roman on the wire; undecoded, it draws mojibake.
    //
    // 0xD5 is a right single quote in Mac Roman and is not valid UTF-8 alone,
    // so it stands in for the whole class.
    let wire = b"Jo\xd5s";
    let got = unsafe { super::cstring_wire_text(wire) };
    let text = got.to_str().expect("decoded text must be valid UTF-8");
    assert_eq!(text, "Jo\u{2019}s");
}

#[test]
fn user_info_text_in_utf8_passes_through_untouched() {
    // Servers that advertise CAP_TEXT_ENCODING send UTF-8, and double-decoding
    // one would be its own mojibake bug.
    let wire = "Jo\u{2019}s".as_bytes();
    let got = unsafe { super::cstring_wire_text(wire) };
    assert_eq!(got.to_str().unwrap(), "Jo\u{2019}s");
}

#[test]
fn user_info_text_still_truncates_at_an_interior_nul() {
    // The old extractor terminated there, and a server that pads a fixed-width
    // field with NULs would otherwise show them as trailing characters.
    let got = unsafe { super::cstring_wire_text(b"bob\0\0\0") };
    assert_eq!(got.to_str().unwrap(), "bob");
}

#[test]
fn empty_user_info_text_is_empty_rather_than_a_failure() {
    let got = unsafe { super::cstring_wire_text(b"") };
    assert_eq!(got.to_str().unwrap(), "");
}
