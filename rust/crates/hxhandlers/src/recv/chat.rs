//! Chat: what the session made of a chat line, an invitation to private
//! chat, a subject, or a page of history, on its way to the view.
//!
//! The session (hx-libs' `hxsession`, driven by `hxnet`) reads these off the
//! wire and decodes their text; what is left here is the per-connection
//! model — whom the user ignores, each chat's subject, the newest history
//! line seen — and the `GtkhxSession` signal each one becomes.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};

use glib::ffi::{g_ptr_array_add, g_ptr_array_new_with_free_func, g_ptr_array_unref, gpointer};
use gtkhx_core::boxed::chat::chat_event_new;
use gtkhx_core::boxed::history::{history_entry_new, hx_history_entry_free, HxHistoryEntry};
use hxsession::{ChatMedia, HistoryEntry};

#[cfg(not(test))]
use gtkhx_core::boxed::chat::hx_chat_event_free;
#[cfg(not(test))]
use gtkhx_core::conn::{
    hx_conn_chat_history_last_msgid, hx_conn_name, hx_conn_sess,
    hx_conn_set_chat_history_last_msgid,
};
#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_chat, gtkhx_session_emit_chat_history_batch,
    gtkhx_session_emit_chat_invitation, gtkhx_session_emit_chat_subject,
    gtkhx_session_emit_chat_subject_notice, gtkhx_session_emit_request_failed,
    gtkhx_session_get_default,
};
#[cfg(not(test))]
use hxmodel::chat_members::hx_member_model_get_ignore;
#[cfg(not(test))]
use hxmodel::conversation::{hx_chat_member_model, hx_chat_set_subject, hx_chat_subject};
#[cfg(not(test))]
use hxtext::gtkhx_text_emoji_shortcodes_enabled;

#[cfg(not(test))]
extern "C" {
    /// Look up a chat by id on a session (`struct chat *`; NULL if absent). cid 0
    /// is the always-present public chat (chat.c).
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    /// Log a pre-formatted line under a debug category (debug.c).
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// The longest subject the chat model holds.
const MAX_SUBJECT: usize = 255;

/// How the session words a reply that was cut short: no refusal of the
/// server's, so nothing to show.
pub(crate) const CUT_SHORT: &str = "the server's reply was cut short";

/// A history request in flight: its chat, and whether it asked for a page
/// older than what the chat shows.
#[derive(Clone, Copy)]
struct HistoryAsked {
    cid: u32,
    older: bool,
}

thread_local! {
    /// The history requests in flight, by connection and trans: a refused
    /// one still has to end its chat's wait.
    static HISTORY_PENDING: RefCell<HashMap<(usize, u32), HistoryAsked>> =
        RefCell::new(HashMap::new());
}

/// A history request for chat `cid` goes out on `trans`; `older` when it
/// asks for a page before what the chat shows.
pub(crate) fn history_requested(htlc: *mut c_void, trans: u32, cid: u32, older: bool) {
    HISTORY_PENDING.with(|p| {
        p.borrow_mut()
            .insert((htlc as usize, trans), HistoryAsked { cid, older })
    });
}

/// Forget what `htlc` asked for before: a new connection numbers its
/// requests afresh, and an old trans would name a new request.
pub(crate) fn history_forget(htlc: *mut c_void) {
    HISTORY_PENDING.with(|p| p.borrow_mut().retain(|(h, _), _| *h != htlc as usize));
}

fn history_answered(htlc: *mut c_void, trans: u32) -> Option<HistoryAsked> {
    HISTORY_PENDING.with(|p| p.borrow_mut().remove(&(htlc as usize, trans)))
}

/// Text as a C string: up to its first NUL, as the C side reads it.
pub(crate) fn c_text(s: &str) -> CString {
    CString::new(s.split('\0').next().unwrap_or_default()).unwrap_or_default()
}

/// A subject as the chat model holds it: up to its first NUL, and no
/// longer than the model's buffer, cut where a character ends.
pub(crate) fn fit_subject(s: &str) -> &str {
    let s = s.split('\0').next().unwrap_or_default();
    let mut end = s.len().min(MAX_SUBJECT);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

/// Emit a pre-formatted line under `cat`.
unsafe fn debug_trace(cat: &std::ffi::CStr, line: String) {
    debug_log_str(cat.as_ptr(), c_text(&line).as_ptr());
}

/// The public chat's member model, where everyone is, for the ignore list;
/// NULL when the session has no public chat.
pub(crate) unsafe fn public_members(htlc: *mut c_void) -> *mut c_void {
    let chat = chat_with_cid(hx_conn_sess(htlc.cast()), 0);
    if chat.is_null() {
        return std::ptr::null_mut();
    }
    hx_chat_member_model(chat.cast())
}

/// A chat line: dropped when its sender is ignored (uid 0 is the server,
/// whom no one ignores), and otherwise the `chat` signal.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn line(
    htlc: *mut c_void,
    cid: u32,
    uid: u16,
    text: &str,
    media: Option<&ChatMedia>,
) {
    let members = public_members(htlc);
    if members.is_null() || (uid != 0 && hx_member_model_get_ignore(members, uid) != 0) {
        return;
    }
    let own = hx_conn_name(htlc.cast());
    let own = if own.is_null() {
        &[][..]
    } else {
        std::ffi::CStr::from_ptr(own).to_bytes()
    };
    if let Some(m) = media {
        debug_trace(
            c"media",
            format!(
                "chat with media: cid={cid} uid={uid} mime={} dims={}x{} bytes={}",
                String::from_utf8_lossy(&m.mime),
                m.width.unwrap_or(0),
                m.height.unwrap_or(0),
                m.bytes.unwrap_or(0)
            ),
        );
    }
    let shortcodes = gtkhx_text_emoji_shortcodes_enabled() != 0;
    let ev = chat_event_new(cid, uid, text, media, own, shortcodes);
    gtkhx_session_emit_chat(gtkhx_session_get_default(), htlc, ev.cast());
    hx_chat_event_free(ev);
}

/// An invitation to private chat `cid`: dropped when the inviter is
/// ignored, and otherwise the `chat-invitation` signal.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn invited(htlc: *mut c_void, cid: u32, uid: u16, name: &str) {
    let members = public_members(htlc);
    if members.is_null() || hx_member_model_get_ignore(members, uid) != 0 {
        return;
    }
    let name = c_text(name);
    gtkhx_session_emit_chat_invitation(gtkhx_session_get_default(), htlc, cid, name.as_ptr());
}

/// A subject for chat `cid`. A new one is stored and announced, the
/// `chat-subject` signal for the subject bar and `chat-subject-notice` for
/// the line in the chat; an empty one, or the one the chat already has,
/// is not news.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn subject(htlc: *mut c_void, cid: u32, subject: &str) {
    let subject = fit_subject(subject);
    if subject.is_empty() {
        return;
    }
    let chat = chat_with_cid(hx_conn_sess(htlc.cast()), cid);
    if chat.is_null() {
        return;
    }
    let s = c_text(subject);
    if std::ffi::CStr::from_ptr(hx_chat_subject(chat.cast())) == s.as_c_str() {
        return;
    }
    let sess = gtkhx_session_get_default();
    gtkhx_session_emit_chat_subject(sess, htlc, cid, s.as_ptr());
    hx_chat_set_subject(chat.cast(), s.as_ptr(), s.as_bytes().len());
    gtkhx_session_emit_chat_subject_notice(sess, htlc, cid, hx_chat_subject(chat.cast()));
}

/// The initial-subject-discovery emit (a roster load's subject). Unlike
/// [`subject`], this has no change-gate: the room just came into view and
/// the caller has already set the model, so the subject is always published
/// to refresh the widget (with no "Subject Changed to" log line).
///
/// # Safety
/// `subject` is a NUL-terminated C string; `htlc` is opaque and only forwarded.
pub(crate) unsafe fn hx_chat_subject_emit(htlc: *mut c_void, cid: u32, subject: *const c_char) {
    gtkhx_session_emit_chat_subject(gtkhx_session_get_default(), htlc, cid, subject);
}

/// `GDestroyNotify` shim: the `GPtrArray` frees each entry with the gtkhx-core
/// `hx_history_entry_free` (which is typed `*mut HxHistoryEntry`).
///
/// # Safety
/// `p` is NULL or a valid `HxHistoryEntry*` (the array only ever holds those).
unsafe extern "C" fn destroy_history_entry(p: gpointer) {
    hx_history_entry_free(p as *mut HxHistoryEntry);
}

/// A page of chat `cid`'s history, the reply to the request on `trans`:
/// the `chat-history-batch` signal, borrowing a `GPtrArray` of
/// `HxHistoryEntry` for the call. The newest line seen moves the cursor a
/// reconnect to the same server catches up from.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub unsafe fn history(
    htlc: *mut c_void,
    trans: u32,
    cid: u32,
    entries: &[HistoryEntry],
    has_more: bool,
) {
    history_answered(htlc, trans);
    let array = g_ptr_array_new_with_free_func(Some(destroy_history_entry));
    for e in entries {
        debug_trace(
            c"chat-history",
            format!(
                "entry msgid={} ts={} flags=0x{:04x} nick={:?} msg={:?}",
                e.message_id, e.timestamp, e.flags, e.nick, e.text
            ),
        );
        g_ptr_array_add(array, history_entry_new(e) as gpointer);
    }
    let newest = entries.iter().map(|e| e.message_id).max().unwrap_or(0);
    if newest > hx_conn_chat_history_last_msgid(htlc.cast()) {
        hx_conn_set_chat_history_last_msgid(htlc.cast(), newest);
    }
    debug_trace(
        c"chat-history",
        format!(
            "received batch: cid={cid} entries={} has_more={}",
            entries.len(),
            has_more as i32
        ),
    );
    gtkhx_session_emit_chat_history_batch(
        gtkhx_session_get_default(),
        htlc,
        cid,
        array as *mut c_void,
        c_int::from(has_more),
    );
    g_ptr_array_unref(array);
}

/// A request the session said failed. One for history ends its chat's
/// wait with an empty page: a refused "Load older" keeps its row, to be
/// tried again, and the fetch at login has nothing to show. The server's
/// reason, when it gave one, is `request-failed`, for the view to show.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn failed(htlc: *mut c_void, trans: u32, reason: Option<&str>) {
    if let Some(asked) = history_answered(htlc, trans) {
        history(htlc, trans, asked.cid, &[], asked.older);
    }
    if let Some(reason) = reason.filter(|r| *r != CUT_SHORT) {
        let reason = c_text(reason);
        gtkhx_session_emit_request_failed(gtkhx_session_get_default(), htlc, reason.as_ptr());
    }
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::{Cell, RefCell};

    /// A signal as the doubles record it.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub enum Emitted {
        Chat {
            cid: u32,
            uid: u16,
            line: String,
            is_self: bool,
            media: bool,
        },
        Invitation(u32, String),
        Subject(u32, String),
        SubjectNotice(u32, String),
        RequestFailed(String),
        History {
            cid: u32,
            ids: Vec<u64>,
            has_more: bool,
        },
    }

    thread_local! {
        pub static IGNORE: Cell<bool> = const { Cell::new(false) };
        pub static SHORTCODES: Cell<bool> = const { Cell::new(true) };
        pub static OWN_NAME: RefCell<String> = const { RefCell::new(String::new()) };
        pub static SUBJECT: RefCell<String> = const { RefCell::new(String::new()) };
        pub static EMITTED: RefCell<Vec<Emitted>> = const { RefCell::new(Vec::new()) };
        pub static CURSOR: Cell<u64> = const { Cell::new(0) };
    }

    pub fn reset() {
        IGNORE.with(|c| c.set(false));
        SHORTCODES.with(|c| c.set(true));
        OWN_NAME.with(|c| c.borrow_mut().clear());
        SUBJECT.with(|c| c.borrow_mut().clear());
        EMITTED.with(|c| c.borrow_mut().clear());
        CURSOR.with(|c| c.set(0));
    }

    pub fn emitted() -> Vec<Emitted> {
        EMITTED.with(|c| std::mem::take(&mut *c.borrow_mut()))
    }

    pub fn emit(e: Emitted) {
        EMITTED.with(|c| c.borrow_mut().push(e));
    }
}

#[cfg(test)]
use doubles::*;

#[cfg(test)]
mod doubles {
    use super::test_env::{self, Emitted};
    use gtkhx_core::boxed::chat::HxChatEvent;
    use gtkhx_core::boxed::history::HxHistoryEntry;
    use std::ffi::CStr;
    use std::os::raw::{c_char, c_int, c_void};

    unsafe fn text(p: *const c_char) -> String {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }

    pub unsafe fn gtkhx_session_get_default() -> *mut c_void {
        std::ptr::null_mut()
    }
    pub unsafe fn gtkhx_session_emit_chat(_s: *mut c_void, _h: *mut c_void, ev: *mut c_void) {
        let e = &*(ev as *const HxChatEvent);
        test_env::emit(Emitted::Chat {
            cid: e.cid,
            uid: e.uid,
            line: String::from_utf8_lossy(std::slice::from_raw_parts(
                e.line as *const u8,
                e.line_len,
            ))
            .into_owned(),
            is_self: e.is_self != 0,
            media: !e.media.is_null(),
        });
    }
    pub unsafe fn gtkhx_session_emit_chat_invitation(
        _s: *mut c_void,
        _h: *mut c_void,
        cid: u32,
        name: *const c_char,
    ) {
        test_env::emit(Emitted::Invitation(cid, text(name)));
    }
    pub unsafe fn gtkhx_session_emit_chat_subject(
        _s: *mut c_void,
        _h: *mut c_void,
        cid: u32,
        subject: *const c_char,
    ) {
        test_env::emit(Emitted::Subject(cid, text(subject)));
    }
    pub unsafe fn gtkhx_session_emit_chat_subject_notice(
        _s: *mut c_void,
        _h: *mut c_void,
        cid: u32,
        subject: *const c_char,
    ) {
        test_env::emit(Emitted::SubjectNotice(cid, text(subject)));
    }
    pub unsafe fn gtkhx_session_emit_chat_history_batch(
        _s: *mut c_void,
        _h: *mut c_void,
        cid: u32,
        entries: *mut c_void,
        has_more: c_int,
    ) {
        let array = &*(entries as *const glib::ffi::GPtrArray);
        let ids = (0..array.len as usize)
            .map(|i| (*(*array.pdata.add(i) as *const HxHistoryEntry)).message_id)
            .collect();
        test_env::emit(Emitted::History {
            cid,
            ids,
            has_more: has_more != 0,
        });
    }
    pub unsafe fn gtkhx_session_emit_request_failed(
        _s: *mut c_void,
        _h: *mut c_void,
        reason: *const c_char,
    ) {
        test_env::emit(Emitted::RequestFailed(text(reason)));
    }
    pub unsafe fn hx_member_model_get_ignore(_m: *mut c_void, _uid: u16) -> c_int {
        c_int::from(test_env::IGNORE.with(|c| c.get()))
    }
    pub unsafe fn hx_conn_sess(_h: *const c_void) -> *mut c_void {
        std::ptr::dangling_mut::<c_void>()
    }
    pub unsafe fn chat_with_cid(_sess: *mut c_void, _cid: u32) -> *mut c_void {
        std::ptr::dangling_mut::<c_void>()
    }
    pub unsafe fn hx_chat_member_model(_chat: *mut c_void) -> *mut c_void {
        std::ptr::dangling_mut::<c_void>()
    }
    pub unsafe fn hx_chat_subject(_chat: *mut c_void) -> *const c_char {
        thread_local! {
            static HELD: std::cell::RefCell<std::ffi::CString> = std::cell::RefCell::default();
        }
        let s = test_env::SUBJECT.with(|c| c.borrow().clone());
        HELD.with(|h| {
            *h.borrow_mut() = std::ffi::CString::new(s).unwrap();
            h.borrow().as_ptr()
        })
    }
    pub unsafe fn hx_chat_set_subject(_chat: *mut c_void, s: *const c_char, _len: usize) {
        test_env::SUBJECT.with(|c| *c.borrow_mut() = text(s));
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
        glib::ffi::gboolean::from(test_env::SHORTCODES.with(|c| c.get()))
    }
    pub unsafe fn hx_chat_event_free(e: *mut HxChatEvent) {
        gtkhx_core::boxed::chat::hx_chat_event_free(e)
    }
    pub unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}
    pub unsafe fn hx_conn_chat_history_last_msgid(_h: *const c_void) -> u64 {
        test_env::CURSOR.with(|c| c.get())
    }
    pub unsafe fn hx_conn_set_chat_history_last_msgid(_h: *mut c_void, v: u64) {
        test_env::CURSOR.with(|c| c.set(v));
    }
}

#[cfg(test)]
mod tests;
