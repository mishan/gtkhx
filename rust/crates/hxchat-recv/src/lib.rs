//! Chat-domain receive handlers (ported from `rcv.c`).
//!
//! The server-notification handlers in `rcv.c` are thin glue: parse the frame
//! (still C — `proto_helpers` reads `htlc->in`), consult the per-chat model,
//! and emit a `GtkhxSession` signal. This crate owns the model-gate + emit half
//! so the decision (e.g. "drop an invite from an ignored user") is unit-testable
//! headlessly, instead of only against a live server. The C handler keeps the
//! parse + the model lookup and calls in here.

use std::os::raw::{c_char, c_int, c_void};

#[cfg(not(test))]
extern "C" {
    /// The singleton `GtkhxSession` GObject (gtkhx-session).
    fn gtkhx_session_get_default() -> *mut c_void;
    /// Fire `GtkhxSession::chat-invitation (htlc, cid, name)` (gtkhx-session).
    fn gtkhx_session_emit_chat_invitation(
        self_: *mut c_void,
        htlc: *mut c_void,
        cid: u32,
        name: *const c_char,
    );
    /// Fire `GtkhxSession::chat-subject (htlc, cid, subj)` (gtkhx-session).
    fn gtkhx_session_emit_chat_subject(
        self_: *mut c_void,
        htlc: *mut c_void,
        cid: u32,
        subj: *const c_char,
    );
    /// Fire `GtkhxSession::chat (htlc, HxChatEvent* boxed)` (gtkhx-session).
    fn gtkhx_session_emit_chat(self_: *mut c_void, htlc: *mut c_void, event: *mut c_void);
    /// Fire `GtkhxSession::chat-history-batch (htlc, cid, GPtrArray*, has_more)`
    /// (gtkhx-session).
    fn gtkhx_session_emit_chat_history_batch(
        self_: *mut c_void,
        htlc: *mut c_void,
        cid: u32,
        entries: *mut c_void,
        has_more: c_int,
    );
    /// Whether `uid` is on the per-chat ignore list (hxmember-model).
    fn hx_member_model_get_ignore(model: *mut c_void, uid: u16) -> c_int;
}

/// `void hx_chat_invite_recv (htlc, member_model, cid, uid, name)` — the
/// private-chat-invitation receive path: drop the invite if the inviter is on
/// the ignore list, otherwise emit the `chat-invitation` signal (the sound
/// subscriber chimes off it). `member_model` is the public chat's member model
/// (`hx_chat_member_model(chat_with_cid(sess, 0))`); `name` is the inviter's
/// wire name as the parser produced it — `strip_ansi`'d and length-capped, but
/// NOT encoding-converted, so it's the server's raw bytes (often Mac Roman) and
/// is not guaranteed valid UTF-8. It's only forwarded to the signal here; any
/// Mac Roman → UTF-8 conversion happens later on the view side.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `name` is a valid C string;
/// `htlc` is the connection handle (opaque, only forwarded to the signal).
#[no_mangle]
pub unsafe extern "C" fn hx_chat_invite_recv(
    htlc: *mut c_void,
    member_model: *mut c_void,
    cid: u32,
    uid: u16,
    name: *const c_char,
) {
    if hx_member_model_get_ignore(member_model, uid) != 0 {
        return;
    }
    gtkhx_session_emit_chat_invitation(gtkhx_session_get_default(), htlc, cid, name);
}

/// `int hx_chat_subject_recv (htlc, cid, subject, subject_len, current_subject)`
/// — the chat-subject-change receive path. Returns 1 and emits `chat-subject`
/// when the subject is non-empty AND differs from the current one; the C side
/// then updates the model + logs the "Subject Changed to" line. Returns 0 (no
/// emit) for an empty or unchanged subject.
///
/// The emit forwards `subject` verbatim, which is byte-identical to the value
/// the old C read back from the model after setting it; the one subscriber
/// (`output_chat_subject`) uses the signal argument, not the model, so emitting
/// before the C-side set is safe.
///
/// # Safety
/// `subject` / `current_subject` are NUL-terminated C strings (the wire parse
/// and the model getter); `htlc` is opaque and only forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_subject_recv(
    htlc: *mut c_void,
    cid: u32,
    subject: *const c_char,
    subject_len: usize,
    current_subject: *const c_char,
) -> c_int {
    if subject_len == 0 {
        return 0;
    }
    // Unchanged subject → no announcement. (An empty subject was already
    // rejected above; current_subject is the model's "" when unset.)
    if !subject.is_null()
        && !current_subject.is_null()
        && std::ffi::CStr::from_ptr(subject) == std::ffi::CStr::from_ptr(current_subject)
    {
        return 0;
    }
    gtkhx_session_emit_chat_subject(gtkhx_session_get_default(), htlc, cid, subject);
    1
}

/// `void hx_chat_subject_emit (htlc, cid, subject)` — the initial-subject-
/// discovery emit (the `rcv_task_user_list` room-load path). Unlike
/// [`hx_chat_subject_recv`], this has no change-gate: the room just came into
/// view and the caller has already set the model, so the subject is always
/// published to refresh the widget (with no "Subject Changed to" log line).
///
/// # Safety
/// `subject` is a NUL-terminated C string; `htlc` is opaque and only forwarded.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_subject_emit(
    htlc: *mut c_void,
    cid: u32,
    subject: *const c_char,
) {
    gtkhx_session_emit_chat_subject(gtkhx_session_get_default(), htlc, cid, subject);
}

/// `void hx_chat_history_recv (htlc, cid, entries, has_more)` — publish a
/// `chat-history-batch` reply. The C handler keeps the chunk walk that builds
/// the `GPtrArray<HxHistoryEntry*>` and advances the newest-msgid cursor; this
/// is the view-notify hop. The array is borrowed for the emit only — the C side
/// still owns and frees it after.
///
/// # Safety
/// `entries` is a valid `GPtrArray *` live for the duration of the call; `htlc`
/// is opaque and only forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_history_recv(
    htlc: *mut c_void,
    cid: u32,
    entries: *mut c_void,
    has_more: c_int,
) {
    gtkhx_session_emit_chat_history_batch(gtkhx_session_get_default(), htlc, cid, entries, has_more);
}

/// `int hx_chat_recv (htlc, member_model, uid, event)` — the public-chat line
/// receive path: drop the line when its sender (`uid`) is on the ignore list,
/// otherwise emit the `chat` signal carrying the boxed `HxChatEvent`. Returns 1
/// when it emitted, 0 when it dropped. A `uid` of 0 is a server/system line
/// (no sender to ignore), so it always emits.
///
/// The C handler owns `event`: it builds the `HxChatEvent` (including any inline
/// -media companion) before calling and frees it after, whether or not this
/// emitted. The emit only borrows it for the duration of the signal.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `event` is a valid boxed
/// `HxChatEvent *`; `htlc` is opaque and only forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_recv(
    htlc: *mut c_void,
    member_model: *mut c_void,
    uid: u16,
    event: *mut c_void,
) -> c_int {
    if uid != 0 && hx_member_model_get_ignore(member_model, uid) != 0 {
        return 0;
    }
    gtkhx_session_emit_chat(gtkhx_session_get_default(), htlc, event);
    1
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::Cell;

    thread_local! {
        /// Configurable return for the stubbed ignore lookup.
        pub static IGNORE: Cell<bool> = const { Cell::new(false) };
        /// Records the last emitted invitation as (cid, name-bytes), or None.
        pub static EMITTED: Cell<Option<(u32, Vec<u8>)>> = const { Cell::new(None) };
        /// Records the last emitted chat-subject as (cid, subj-bytes), or None.
        pub static SUBJECT_EMITTED: Cell<Option<(u32, Vec<u8>)>> = const { Cell::new(None) };
        /// Records the boxed-event pointer of the last emitted `chat`, or None.
        pub static CHAT_EMITTED: Cell<Option<*mut std::os::raw::c_void>> = const { Cell::new(None) };
        /// Records the last emitted chat-history-batch as (cid, entries-ptr,
        /// has_more), or None.
        pub static HISTORY_EMITTED: Cell<Option<(u32, *mut std::os::raw::c_void, bool)>> =
            const { Cell::new(None) };
    }

    pub fn reset() {
        IGNORE.with(|c| c.set(false));
        EMITTED.with(|c| c.set(None));
        SUBJECT_EMITTED.with(|c| c.set(None));
        CHAT_EMITTED.with(|c| c.set(None));
        HISTORY_EMITTED.with(|c| c.set(None));
    }
}

#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat_invitation(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cid: u32,
    name: *const c_char,
) {
    let bytes = if name.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(name).to_bytes().to_vec()
    };
    test_env::EMITTED.with(|c| c.set(Some((cid, bytes))));
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat_subject(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cid: u32,
    subj: *const c_char,
) {
    let bytes = if subj.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(subj).to_bytes().to_vec()
    };
    test_env::SUBJECT_EMITTED.with(|c| c.set(Some((cid, bytes))));
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat(_self_: *mut c_void, _htlc: *mut c_void, event: *mut c_void) {
    test_env::CHAT_EMITTED.with(|c| c.set(Some(event)));
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat_history_batch(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cid: u32,
    entries: *mut c_void,
    has_more: c_int,
) {
    test_env::HISTORY_EMITTED.with(|c| c.set(Some((cid, entries, has_more != 0))));
}

#[cfg(test)]
unsafe fn hx_member_model_get_ignore(_model: *mut c_void, _uid: u16) -> c_int {
    c_int::from(test_env::IGNORE.with(|c| c.get()))
}

#[cfg(test)]
mod tests;
