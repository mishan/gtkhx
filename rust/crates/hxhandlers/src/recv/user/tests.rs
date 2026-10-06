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

// ---- user lists, joins, a new chat -----------------------------------------

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
        unsafe { failed(h, 7, None) }; // not the join's
        assert!(!test_env::CHAT_DELETED.with(|c| c.get()));
        unsafe { failed(h, 8, None) };
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

// ---- what the server says about us, a user's info, a kick, an account -----

#[test]
fn self_info_folds_in_only_what_the_server_said() {
    let cases = [
        (None, None, Some(0x0102), None, (9, 8, 7)),
        (Some(1), Some(2), None, Some(0x112233), (1, 2, 0x112233)),
    ];
    for (uid, icon, access, color, (want_uid, want_icon, want_color)) in cases {
        test_env::reset();
        test_env::SELF_UID.with(|c| c.set(9));
        test_env::SELF_ICON.with(|c| c.set(8));
        test_env::SELF_NICK_COLOR.with(|c| c.set(7));
        unsafe { selfinfo(std::ptr::null_mut(), uid, icon, access, color) };
        assert_eq!(
            test_env::ACCESS.with(|c| c.get()),
            access.map(u64::to_be_bytes)
        );
        assert_eq!(test_env::SELF_UID.with(|c| c.get()), want_uid);
        assert_eq!(test_env::SELF_ICON.with(|c| c.get()), want_icon);
        assert_eq!(test_env::SELF_NICK_COLOR.with(|c| c.get()), want_color);
        // It says we are logged in, and the toolbar hears of it.
        assert_eq!(test_env::LOGGED_IN.with(|c| c.get()), 1);
        assert_eq!(test_env::take(), Some(Emit::SelfUpdated));
    }
}

const CONN_A: *mut c_void = 0x10 as *mut c_void;
const CONN_B: *mut c_void = 0x20 as *mut c_void;

#[test]
fn user_info_goes_to_the_user_asked_of_once_on_its_connection() {
    test_env::reset();
    asked(CONN_A, 7, Asked::Info(11));
    unsafe {
        info(CONN_B, 7, "Alice", "idle");
        info(CONN_A, 8, "Alice", "idle");
    }
    assert_eq!(test_env::take(), None);
    // The text ends at its first NUL, and so does the length it goes with.
    unsafe { info(CONN_A, 7, "Alice", "ab\0cd") };
    assert_eq!(
        test_env::take(),
        Some(Emit::Info {
            uid: 11,
            name: b"Alice".to_vec(),
            info: b"ab".to_vec(),
            len: 2,
        })
    );
    unsafe { info(CONN_A, 7, "Alice", "idle") };
    assert_eq!(test_env::take(), None);
}

#[test]
fn user_info_without_a_name_or_text_shows_nothing() {
    for (name, text) in [("", "idle"), ("Alice", ""), ("\0Alice", "idle")] {
        test_env::reset();
        asked(CONN_A, 1, Asked::Info(11));
        unsafe { info(CONN_A, 1, name, text) };
        assert_eq!(test_env::take(), None, "{name:?} {text:?}");
    }
}

#[test]
fn what_a_connection_asked_goes_when_it_is_forgotten_or_refused() {
    test_env::reset();
    asked(CONN_A, 1, Asked::Info(11));
    asked(CONN_A, 2, Asked::Info(12));
    asked(CONN_B, 1, Asked::Info(13));
    forget(CONN_A);
    unsafe {
        failed(CONN_B, 1, Some("no"));
        info(CONN_A, 1, "Alice", "idle");
        info(CONN_A, 2, "Alice", "idle");
        info(CONN_B, 1, "Alice", "idle");
    }
    assert_eq!(test_env::take(), None);
}

#[test]
fn an_account_fills_the_editor_that_asked_when_it_has_access_bits() {
    let account = |access| Account {
        login: b"ren\x8e".to_vec(),
        name: b"Ren\x8e".to_vec(),
        password: Vec::new(),
        access,
    };
    for access in [None, Some(0x8000_0000_0000_0001)] {
        let filled = std::rc::Rc::new(std::cell::RefCell::new(None));
        let into = filled.clone();
        asked(
            CONN_A,
            3,
            Asked::Account(Box::new(move |a: &Account| {
                *into.borrow_mut() = Some(a.clone())
            })),
        );
        unsafe { super::account(CONN_A, 3, &account(access)) };
        assert_eq!(*filled.borrow(), access.map(|_| account(access)));
        // Answered once.
        unsafe { super::account(CONN_A, 3, &account(Some(0))) };
        assert_eq!(*filled.borrow(), access.map(|_| account(access)));
    }
}

#[test]
fn a_kick_that_worked_is_said_in_the_public_chat() {
    test_env::reset();
    unsafe { kicked(CONN_A) };
    assert_eq!(
        take_notice(),
        Some(test_env::Notice {
            cid: 0,
            kind: HX_USER_NOTICE_KICKED,
            name: Vec::new(),
            old_name: Vec::new(),
        })
    );
}

#[test]
fn an_account_counts_as_made_only_once_the_server_says_so() {
    let made = std::rc::Rc::new(std::cell::Cell::new(0));
    let ask = |trans| {
        let made = made.clone();
        asked(
            CONN_A,
            trans,
            Asked::Made(Box::new(move |ok| {
                if ok {
                    made.set(made.get() + 1)
                }
            })),
        );
    };
    ask(1);
    ask(2);
    unsafe {
        // Refused: the window makes it again on its next Save.
        failed(CONN_A, 1, Some("no"));
        account_changed(CONN_A, 1);
        account_changed(CONN_B, 2);
    }
    assert_eq!(made.get(), 0);
    unsafe {
        account_changed(CONN_A, 2);
        account_changed(CONN_A, 2);
    }
    assert_eq!(made.get(), 1);
}
