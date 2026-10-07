//! Our own name, icon and nick color; the user list, asked for once the
//! login settles; a user's info, a kick, an admin's broadcast, and an
//! account read, saved or deleted. The requests are `hxrequest::user`'s;
//! each reply is expected by the session, and a refusal reaches
//! `request-failed`, or, for an account made or saved, the editor that
//! asked.

use std::ffi::CStr;
use std::os::raw::{c_int, c_void};

use gtkhx_core::conn::{
    hx_conn_fd, hx_conn_icon, hx_conn_name, hx_conn_nick_color, hx_conn_nick_color_sent,
    hx_conn_set_nick_color_sent,
};
use hxproto::build::HxChunk;
use hxproto::messages::ClientHdr;
use hxrequest::{user, Request};
use hxsession::{Account, Expect};

use crate::recv::user::{asked, Asked, Done};

#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;

#[cfg(not(test))]
extern "C" {
    // chat_send_bridge.c — whether text goes out as UTF-8.
    fn hx_htlc_text_encoding_cap(htlc: *mut c_void) -> glib::ffi::gboolean;
}

/// `void hx_user_list_get (struct htlc_conn *htlc)` — ask for the user list
/// (USER_GETLIST, no fields). Its reply comes back as the session's
/// `UserList`, which loads the public chat's roster and then the news.
/// Once a login, so what the last connection asked for goes with it.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_get(htlc: *mut c_void) {
    if htlc.is_null() {
        return;
    }
    crate::recv::forget(htlc, true);
    super::expect_next(htlc, Expect::UserList);
    hlwrite_chunks(
        htlc.cast(),
        ClientHdr::UserGetList as u32,
        0,
        std::ptr::null::<HxChunk>(),
        0 as c_int,
    );
}

/// `void hx_change_name_icon (struct htlc_conn *htlc)` — our name, icon and
/// nick color, as the connection holds them (USER_CHANGE, no reply).
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_change_name_icon(htlc: *mut c_void) {
    // Nothing goes out unconnected, so nothing counts as sent.
    if htlc.is_null() || hx_conn_fd(htlc.cast()) == 0 {
        return;
    }
    let h = htlc.cast();
    let color = hx_conn_nick_color(h);
    let Some(req) = user::change(
        CStr::from_ptr(hx_conn_name(h)).to_bytes(),
        hx_htlc_text_encoding_cap(htlc) != glib::ffi::GFALSE,
        hx_conn_icon(h),
        color,
        hx_conn_nick_color_sent(h) != glib::ffi::GFALSE,
    ) else {
        return;
    };
    if color != hxproto::messages::NICK_COLOR_NONE {
        hx_conn_set_nick_color_sent(h, glib::ffi::GTRUE);
    }
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(
            htlc.cast(),
            req.opcode,
            0,
            chunks.as_ptr(),
            chunks.len() as c_int,
        )
    });
}

/// Send `req`, its reply expected as `what`; the trans it went out on.
unsafe fn send(htlc: *mut c_void, req: &Request, what: Expect) -> u32 {
    let trans = super::expect_next(htlc, what);
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(
            htlc.cast(),
            req.opcode,
            0,
            chunks.as_ptr(),
            chunks.len() as c_int,
        )
    });
    trans
}

/// `void hx_get_user_info (struct htlc_conn *htlc, guint16 uid)` — what
/// the server says of `uid`, shown once it answers.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_get_user_info(htlc: *mut c_void, uid: u16) {
    if htlc.is_null() {
        return;
    }
    if let Some(req) = user::info(uid) {
        let trans = send(htlc, &req, Expect::UserInfo);
        asked(htlc, trans, Asked::Info(uid));
    }
}

/// `void hx_kick_user (struct htlc_conn *htlc, guint16 uid, guint16 ban)` —
/// disconnect `uid`, and ban them too when `ban`.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_kick_user(htlc: *mut c_void, uid: u16, ban: u16) {
    if htlc.is_null() {
        return;
    }
    if let Some(req) = user::kick(uid, ban != 0) {
        send(htlc, &req, Expect::Kick);
    }
}

/// `text`, as the user typed it, to everyone on the server.
///
/// # Safety
/// `htlc` is a live connection; main thread.
pub unsafe fn broadcast(htlc: *mut c_void, text: &str) {
    let utf8 = hx_htlc_text_encoding_cap(htlc) != glib::ffi::GFALSE;
    if let Some(req) = user::broadcast(text.as_bytes(), utf8) {
        send(htlc, &req, Expect::Message);
    }
}

/// The account `login` names, into `fill` once the server answers.
///
/// # Safety
/// `htlc` is a live connection; main thread.
pub unsafe fn account_read(htlc: *mut c_void, login: &[u8], fill: impl FnOnce(&Account) + 'static) {
    if let Some(req) = user::account_read(login) {
        let trans = send(htlc, &req, Expect::Account);
        asked(htlc, trans, Asked::Account(Box::new(fill)));
    }
}

/// A new account `login`; `done` is told whether the server made it. One
/// that may read accounts (`can_read`) reads the
/// login first, and makes nothing if it is there, running `exists`
/// instead: a server may replace an account that is there with a new one.
/// One that may not, makes it.
///
/// # Safety
/// `htlc` is a live connection; main thread.
#[allow(clippy::too_many_arguments)]
pub unsafe fn account_create(
    htlc: *mut c_void,
    login: &[u8],
    password: &[u8],
    name: &[u8],
    access: [u8; 8],
    can_read: bool,
    exists: impl FnOnce() + 'static,
    done: impl FnOnce(Result<(), Option<String>>) + 'static,
) {
    let Some(req) = user::account_create(login, password, name, access) else {
        return;
    };
    let done = Box::new(done);
    if !can_read {
        create(htlc, &req, done);
        return;
    }
    if let Some(read) = user::account_read(login) {
        let trans = send(htlc, &read, Expect::Account);
        let exists = Box::new(exists);
        asked(
            htlc,
            trans,
            Asked::Check {
                create: req,
                exists,
                done,
            },
        );
    }
}

/// Send `req`, a new account, `done` to be told whether the server made it.
///
/// # Safety
/// `htlc` is a live connection; main thread.
pub(crate) unsafe fn create(htlc: *mut c_void, req: &Request, done: Done) {
    let trans = send(htlc, req, Expect::AccountChange);
    asked(htlc, trans, Asked::Changed(done));
}

/// Replace what the account `login` names holds; `done` is told whether
/// the server did.
///
/// # Safety
/// `htlc` is a live connection; main thread.
pub unsafe fn account_save(
    htlc: *mut c_void,
    login: &[u8],
    password: &[u8],
    name: &[u8],
    access: [u8; 8],
    done: impl FnOnce(Result<(), Option<String>>) + 'static,
) {
    if let Some(req) = user::account_save(login, password, name, access) {
        let trans = send(htlc, &req, Expect::AccountChange);
        asked(htlc, trans, Asked::Changed(Box::new(done)));
    }
}

/// # Safety
/// `htlc` is a live connection; main thread.
pub unsafe fn account_delete(htlc: *mut c_void, login: &[u8]) {
    if let Some(req) = user::account_delete(login) {
        send(htlc, &req, Expect::AccountChange);
    }
}

#[cfg(test)]
use tests::{hlwrite_chunks, hx_htlc_text_encoding_cap};

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    thread_local! {
        static SENT: RefCell<Vec<(u32, c_int)>> = const { RefCell::new(Vec::new()) };
    }

    pub(super) unsafe fn hlwrite_chunks(
        _htlc: *mut c_void,
        ty: u32,
        _flag: u32,
        _chunks: *const HxChunk,
        hc: c_int,
    ) {
        SENT.with(|s| s.borrow_mut().push((ty, hc)));
    }

    pub(super) unsafe fn hx_htlc_text_encoding_cap(_htlc: *mut c_void) -> glib::ffi::gboolean {
        glib::ffi::GTRUE
    }

    /// A new account is made after a read that says its login is free,
    /// when we may read accounts; a login that is there is left alone.
    #[test]
    fn a_new_account_is_made_only_where_its_login_is_free() {
        use crate::recv::user::{account, failed};
        use std::cell::Cell;
        use std::rc::Rc;

        let taken = hxsession::Account {
            login: b"bob".to_vec(),
            name: b"Bob".to_vec(),
            password: Vec::new(),
            access: None,
        };
        // (may read, what answers the read) -> sent after the read, whether
        // `exists` ran, whether the refusal is kept from the user, what
        // `done` was told.
        type Answer = Option<Result<(), Option<&'static str>>>;
        type Told = Option<Result<(), Option<String>>>;
        type Case = (bool, Answer, Vec<u32>, bool, bool, Told);
        let cases: [Case; 5] = [
            (false, None, vec![350], false, false, None),
            (true, Some(Ok(())), vec![], true, false, None),
            // Refused, with the server's reason or without one.
            (
                true,
                Some(Err(Some("Account does not exist."))),
                vec![350],
                false,
                true,
                None,
            ),
            (true, Some(Err(None)), vec![350], false, true, None),
            (
                true,
                Some(Err(Some(crate::recv::chat::CUT_SHORT))),
                vec![],
                false,
                true,
                Some(Err(None)),
            ),
        ];
        for (can_read, answer, after, want_exists, want_quiet, want_made) in cases {
            crate::send::expected::take();
            SENT.with(|s| s.take());
            let htlc = std::ptr::dangling_mut();
            let exists = Rc::new(Cell::new(false));
            let made = Rc::new(RefCell::new(None));
            let (seen, told) = (exists.clone(), made.clone());
            unsafe {
                account_create(
                    htlc,
                    b"bob",
                    b"pw",
                    b"Bob",
                    [0; 8],
                    can_read,
                    move || seen.set(true),
                    move |r| *told.borrow_mut() = Some(r),
                )
            };
            let mut quiet = false;
            if can_read {
                let sent: Vec<u32> = SENT.with(|s| s.take()).iter().map(|(t, _)| *t).collect();
                assert_eq!(sent, [352]);
                let [(trans, Expect::Account)] = crate::send::expected::take()[..] else {
                    panic!("the read's reply is not expected as an account");
                };
                match answer {
                    Some(Ok(())) => unsafe { account(htlc, trans, &taken) },
                    Some(Err(reason)) => quiet = unsafe { failed(htlc, trans, reason) },
                    None => {}
                }
            }
            let sent: Vec<u32> = SENT.with(|s| s.take()).iter().map(|(t, _)| *t).collect();
            assert_eq!(sent, after, "{can_read} {answer:?}");
            // A create goes with its reply expected.
            let expected: Vec<_> = crate::send::expected::take()
                .into_iter()
                .map(|(_, what)| what)
                .collect();
            assert_eq!(
                expected,
                after
                    .iter()
                    .map(|_| Expect::AccountChange)
                    .collect::<Vec<_>>()
            );
            assert_eq!(exists.get(), want_exists, "{can_read} {answer:?}");
            assert_eq!(quiet, want_quiet, "{can_read} {answer:?}");
            assert_eq!(made.take(), want_made, "{can_read} {answer:?}");
            unsafe { crate::recv::forget(htlc, false) };
        }
    }

    /// A made or saved account the server refuses is the editor's to
    /// show, with the server's reason or none: the caller shows nothing of
    /// it.
    #[test]
    fn a_refused_account_change_goes_to_the_editor_with_its_reason() {
        use crate::recv::user::failed;
        use std::rc::Rc;

        type Send = fn(*mut c_void, Box<dyn FnOnce(Result<(), Option<String>>)>);
        let sends: [Send; 2] = [
            |h, d| unsafe { account_create(h, b"bob", b"", b"Bob", [0; 8], false, || {}, d) },
            |h, d| unsafe { account_save(h, b"bob", b"", b"Bob", [0; 8], d) },
        ];
        for (send, reason) in sends
            .into_iter()
            .flat_map(|s| [(s, Some("No.")), (s, None)])
        {
            crate::send::expected::take();
            let htlc = std::ptr::dangling_mut();
            let told = Rc::new(RefCell::new(None));
            let t = told.clone();
            send(htlc, Box::new(move |r| *t.borrow_mut() = Some(r)));
            let [(trans, Expect::AccountChange)] = crate::send::expected::take()[..] else {
                panic!("the change's reply is not expected");
            };
            assert!(unsafe { failed(htlc, trans, reason) });
            let said = reason.unwrap_or_default().to_owned();
            assert_eq!(told.take(), Some(Err(Some(said))), "{reason:?}");
            unsafe { crate::recv::forget(htlc, false) };
        }
    }

    /// A connection says nothing of a color until one has gone out on it,
    /// and says it is cleared once one has.
    #[test]
    fn a_nick_color_is_cleared_only_once_one_went() {
        use gtkhx_core::conn::{hx_conn_free, hx_conn_new, hx_conn_set_fd, hx_conn_set_nick_color};
        use hxproto::messages::NICK_COLOR_NONE as NONE;

        // (connected, our color) -> chunks sent, if anything was.
        let steps = [
            (true, NONE, Some(2)),
            (false, 0x0012_3456, None),
            (true, NONE, Some(2)),
            (true, 0x0012_3456, Some(3)),
            (true, NONE, Some(3)),
            (true, NONE, Some(3)),
        ];
        let h = hx_conn_new();
        for (i, (connected, color, want)) in steps.into_iter().enumerate() {
            SENT.with(|s| s.take());
            unsafe {
                hx_conn_set_fd(h, c_int::from(connected));
                hx_conn_set_nick_color(h, color);
                hx_change_name_icon(h.cast());
            }
            let sent = SENT.with(|s| s.take());
            assert_eq!(
                sent,
                want.map(|hc| (304, hc)).into_iter().collect::<Vec<_>>(),
                "step {i}"
            );
        }
        unsafe { hx_conn_free(h) };
    }

    #[test]
    fn each_request_goes_with_its_reply_expected() {
        crate::send::expected::take();
        let htlc = std::ptr::dangling_mut();
        unsafe {
            hx_user_list_get(htlc);
            hx_get_user_info(htlc, 5);
            hx_kick_user(htlc, 5, 1);
            broadcast(htlc, "hi");
            account_read(htlc, b"bob", |_| {});
            account_create(htlc, b"bob", b"", b"Bob", [0; 8], false, || {}, |_| {});
            account_save(htlc, b"bob", b"", b"Bob", [0; 8], |_| {});
            account_delete(htlc, b"bob");
        }
        assert_eq!(
            crate::send::expected::take(),
            [
                (1, Expect::UserList),
                (2, Expect::UserInfo),
                (3, Expect::Kick),
                (4, Expect::Message),
                (5, Expect::Account),
                (6, Expect::AccountChange),
                (7, Expect::AccountChange),
                (8, Expect::AccountChange),
            ]
        );
        assert_eq!(
            SENT.with(|s| s.take()),
            [
                (300, 0),
                (303, 1),
                (110, 2),
                (355, 1),
                (352, 1),
                (350, 4),
                (353, 4),
                (351, 1)
            ]
        );
        unsafe { crate::recv::forget(htlc, false) };
    }
}
