//! Messages: what the session made of a private message, a broadcast, or
//! the server's parting words, on its way to the view.
//!
//! The session (hx-libs' `hxsession`, driven by `hxnet`) reads them off the
//! wire and decodes their text; what is left here is whom the user ignores,
//! what the roster knows of a sender the message leaves unnamed, and the
//! `GtkhxSession` signal each one becomes.

use std::borrow::Cow;
use std::ffi::CStr;
use std::os::raw::c_void;

use gtkhx_core::boxed::msg::msg_event_new;
use hxmodel::chat_members::HxMemberInfo;

use super::chat::{c_text, public_members};

#[cfg(not(test))]
use gtkhx_core::boxed::msg::hx_msg_event_free;
#[cfg(not(test))]
use gtkhx_core::conn::{hx_conn_name, hx_conn_uid};
#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_broadcast, gtkhx_session_emit_msg, gtkhx_session_get_default,
};
#[cfg(not(test))]
use hxmodel::chat_members::{hx_member_model_get_ignore, hx_member_model_get_info};
#[cfg(not(test))]
use hxtext::gtkhx_text_emoji_shortcodes_enabled;

/// Who `uid` is in the public chat, where everyone is, unless ignored:
/// `None` drops what they sent. uid 0 is the server, whom no one ignores.
unsafe fn heard(htlc: *mut c_void, uid: u16) -> Option<Option<HxMemberInfo>> {
    let members = public_members(htlc);
    if members.is_null() || hx_member_model_get_ignore(members, uid) != 0 {
        return None;
    }
    let mut info = std::mem::zeroed::<HxMemberInfo>();
    Some((hx_member_model_get_info(members, uid, &mut info) != 0).then_some(info))
}

/// A private message: dropped when its sender is ignored, and otherwise the
/// `msg` signal. A server can leave the sender's name out (mhxd, on a
/// message to ourselves); it is ours for our own uid, else the roster's, so
/// the window has a name and our own echo reads as ours.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn message(htlc: *mut c_void, uid: u16, from: &str, text: &str) {
    let Some(sender) = heard(htlc, uid) else {
        return;
    };
    let own = hx_conn_name(htlc.cast());
    let own = if own.is_null() {
        &[][..]
    } else {
        CStr::from_ptr(own).to_bytes()
    };
    let from = match sender {
        _ if !from.is_empty() => Cow::Borrowed(from),
        _ if uid == hx_conn_uid(htlc.cast()) && !own.is_empty() => String::from_utf8_lossy(own),
        Some(s) => Cow::Owned(
            CStr::from_ptr(s.name.as_ptr())
                .to_string_lossy()
                .into_owned(),
        ),
        None => Cow::Borrowed(""),
    };
    let shortcodes = gtkhx_text_emoji_shortcodes_enabled() != 0;
    let ev = msg_event_new(uid, &from, text, own, shortcodes);
    gtkhx_session_emit_msg(gtkhx_session_get_default(), htlc, ev.cast());
    hx_msg_event_free(ev);
}

/// A broadcast: dropped when its sender is ignored, and otherwise the
/// `broadcast` signal, with the sender's name when the server gave one and
/// their status from the roster.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn broadcast(htlc: *mut c_void, uid: u16, from: &str, text: &str) {
    let Some(sender) = heard(htlc, uid) else {
        return;
    };
    let status = sender.map_or(0, |s| u32::from(s.status));
    let name = (!from.is_empty()).then(|| c_text(from));
    gtkhx_session_emit_broadcast(
        gtkhx_session_get_default(),
        htlc,
        name.as_ref().map_or(std::ptr::null(), |n| n.as_ptr()),
        status,
        c_text(text).as_ptr(),
        false,
    );
}

/// The server's parting words before it hangs up: the `broadcast` signal,
/// marked as parting.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn parting(htlc: *mut c_void, text: &str) {
    gtkhx_session_emit_broadcast(
        gtkhx_session_get_default(),
        htlc,
        std::ptr::null(),
        0,
        c_text(text).as_ptr(),
        true,
    );
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::{Cell, RefCell};

    /// A signal as the doubles record it.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub enum Emitted {
        Msg {
            htlc: usize,
            uid: u16,
            name: String,
            body: String,
            is_self: bool,
        },
        Broadcast {
            name: Option<String>,
            status: u32,
            text: String,
            parting: bool,
        },
    }

    thread_local! {
        pub static IGNORE: Cell<bool> = const { Cell::new(false) };
        /// The roster's entry for any uid asked about: (name, status).
        pub static MEMBER: RefCell<Option<(String, u16)>> = const { RefCell::new(None) };
        pub static OWN_UID: Cell<u16> = const { Cell::new(0) };
        pub static OWN_NAME: RefCell<String> = const { RefCell::new(String::new()) };
        pub static EMITTED: RefCell<Vec<Emitted>> = const { RefCell::new(Vec::new()) };
    }

    pub fn reset() {
        IGNORE.with(|c| c.set(false));
        MEMBER.with(|c| c.borrow_mut().take());
        OWN_UID.with(|c| c.set(0));
        OWN_NAME.with(|c| c.borrow_mut().clear());
        EMITTED.with(|c| c.borrow_mut().clear());
    }

    pub fn emitted() -> Vec<Emitted> {
        EMITTED.with(|c| std::mem::take(&mut *c.borrow_mut()))
    }
}

#[cfg(test)]
use doubles::*;

#[cfg(test)]
mod doubles {
    use super::test_env::{self, Emitted};
    use gtkhx_core::boxed::msg::HxMsgEvent;
    use hxmodel::chat_members::HxMemberInfo;
    use std::ffi::CStr;
    use std::os::raw::{c_char, c_int, c_void};

    unsafe fn text(p: *const c_char) -> String {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }

    pub unsafe fn gtkhx_session_get_default() -> *mut c_void {
        std::ptr::null_mut()
    }
    pub unsafe fn gtkhx_session_emit_msg(_s: *mut c_void, h: *mut c_void, ev: *mut c_void) {
        let e = &*(ev as *const HxMsgEvent);
        test_env::EMITTED.with(|c| {
            c.borrow_mut().push(Emitted::Msg {
                htlc: h as usize,
                uid: e.uid,
                name: text(e.name),
                body: text(e.body),
                is_self: e.is_self != 0,
            })
        });
    }
    pub unsafe fn gtkhx_session_emit_broadcast(
        _s: *mut c_void,
        _h: *mut c_void,
        name: *const c_char,
        status: u32,
        body: *const c_char,
        parting: bool,
    ) {
        test_env::EMITTED.with(|c| {
            c.borrow_mut().push(Emitted::Broadcast {
                name: (!name.is_null()).then(|| text(name)),
                status,
                text: text(body),
                parting,
            })
        });
    }
    pub unsafe fn hx_msg_event_free(e: *mut HxMsgEvent) {
        gtkhx_core::boxed::msg::hx_msg_event_free(e)
    }
    pub unsafe fn hx_member_model_get_ignore(_m: *mut c_void, _uid: u16) -> c_int {
        c_int::from(test_env::IGNORE.with(|c| c.get()))
    }
    pub unsafe fn hx_member_model_get_info(
        _m: *mut c_void,
        uid: u16,
        out: *mut HxMemberInfo,
    ) -> c_int {
        let Some((name, status)) = test_env::MEMBER.with(|c| c.borrow().clone()) else {
            return 0;
        };
        (*out).uid = uid;
        (*out).status = status;
        for (i, b) in name.bytes().enumerate() {
            (*out).name[i] = b as c_char;
        }
        (*out).name[name.len()] = 0;
        1
    }
    pub unsafe fn hx_conn_uid(_h: *const c_void) -> u16 {
        test_env::OWN_UID.with(|c| c.get())
    }
    pub unsafe fn hx_conn_name(_h: *const c_void) -> *const c_char {
        thread_local! {
            static HELD: std::cell::RefCell<std::ffi::CString> = std::cell::RefCell::default();
        }
        let s = test_env::OWN_NAME.with(|c| c.borrow().clone());
        HELD.with(|h| {
            *h.borrow_mut() = std::ffi::CString::new(s).unwrap();
            h.borrow().as_ptr()
        })
    }
    pub fn gtkhx_text_emoji_shortcodes_enabled() -> glib::ffi::gboolean {
        glib::ffi::GTRUE
    }
}

#[cfg(test)]
mod tests;
