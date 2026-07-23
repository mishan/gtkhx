//! User-roster receive handlers (ported from `rcv.c`).
//!
//! The live `USER_CHANGE` broadcast and the bulk `USER_LIST` login load both end
//! in the same roster-apply decision: a new member becomes a `user-create`, an
//! existing one is either a live `user-change` or a silent model refresh. That
//! shared tail is [`hx_user_apply_recv`], called by both paths (`incremental`
//! tells them apart) so the create/change/upsert routing lives in one place.
//! [`hx_user_part_recv`] handles the `USER_PART` removal. The change-*decision*
//! itself (`hx_user_change_plan_resolve`) lives in `hotline-proto`; the C side
//! keeps the parse, the plan resolution, the self-uid bookkeeping, and the
//! ignore/rename logging keyed on these functions' return values.

use std::os::raw::{c_char, c_int, c_void};

#[cfg(not(test))]
extern "C" {
    /// The singleton `GtkhxSession` GObject (gtkhx-session).
    fn gtkhx_session_get_default() -> *mut c_void;
    /// `GtkhxSession::user-create (htlc, chat, uid, nick_color, nam, icon, color, incremental)`.
    fn gtkhx_session_emit_user_create(
        self_: *mut c_void,
        htlc: *mut c_void,
        chat: *mut c_void,
        uid: u16,
        nick_color: u32,
        nam: *const c_char,
        icon: u16,
        color: u16,
        incremental: c_int,
    );
    /// `GtkhxSession::user-change (htlc, chat, uid, nick_color, nam, icon, color)`.
    fn gtkhx_session_emit_user_change(
        self_: *mut c_void,
        htlc: *mut c_void,
        chat: *mut c_void,
        uid: u16,
        nick_color: u32,
        nam: *const c_char,
        icon: u16,
        color: u16,
    );
    /// `GtkhxSession::user-delete (htlc, chat, uid, incremental)`.
    fn gtkhx_session_emit_user_delete(
        self_: *mut c_void,
        htlc: *mut c_void,
        chat: *mut c_void,
        uid: u16,
        incremental: c_int,
    );
    /// Whether `uid` is a member of the per-chat model (hxmember-model).
    fn hx_member_model_contains(model: *mut c_void, uid: u16) -> c_int;
    /// Insert-or-update a member's fields in the per-chat model (hxmember-model),
    /// without emitting a view signal.
    fn hx_member_model_upsert(
        model: *mut c_void,
        uid: u16,
        name: *const c_char,
        icon: u16,
        color: u16,
        nick_color: u32,
    );
    /// `GtkhxSession::user-info (uid, nam, info, len)` — a USER_INFO reply.
    fn gtkhx_session_emit_user_info(
        self_: *mut c_void,
        uid: u16,
        nam: *const c_char,
        info: *const c_char,
        len: u16,
    );
    /// `GtkhxSession::self-updated (htlc)` — our own access bits / uid were
    /// (re)parsed from a SELFINFO reply.
    fn gtkhx_session_emit_self_updated(self_: *mut c_void, htlc: *mut c_void);
    /// Parse a SELFINFO frame's chunks into `htlc` (access bits / uid / icon).
    /// Deliberately ignores the server-supplied name — our local prefs nick is
    /// authoritative. C helper in proto_helpers.c.
    fn hx_selfinfo_parse(htlc: *mut c_void, frame: *const u8, frame_len: usize) -> u32;
    /// Set our own "logged in" flag on the connection (hxconn). SELFINFO is the
    /// canonical login-complete signal; the agreement Agree button reads this.
    fn hx_conn_set_logged_in(htlc: *mut c_void, v: c_int);
}

/// Result of [`hx_user_apply_recv`] — tells the C side what (if anything) it
/// did, so it can do the matching join/rename logging.
pub const HX_USER_CHANGE_SKIPPED: c_int = 0;
pub const HX_USER_CHANGE_CREATED: c_int = 1;
pub const HX_USER_CHANGE_CHANGED: c_int = 2;
/// The member already existed and this was a non-incremental (bulk user-list)
/// pass, so its fields were folded into the model silently — no view signal.
pub const HX_USER_CHANGE_UPDATED: c_int = 3;

/// `int hx_user_apply_recv (htlc, chat, member_model, uid, nick_color, name,
/// icon, color, is_new, skip_self_create, incremental)` — the one roster-apply
/// routine shared by the live `USER_CHANGE` broadcast and the bulk `USER_LIST`
/// load. It routes a member's resolved state to the right outcome and returns
/// which, so the C side does the matching logging:
///
/// - **new + `skip_self_create`** → [`HX_USER_CHANGE_SKIPPED`]: our own live
///   join; the USER_LIST reply creates the row in the right spot.
/// - **new** → [`HX_USER_CHANGE_CREATED`]: emit `user-create` (the view inserts
///   the row and seeds the model). `incremental` gates the join chime.
/// - **existing + `incremental`** (live change) → [`HX_USER_CHANGE_CHANGED`]:
///   emit `user-change` (the view updates the row in place).
/// - **existing + not `incremental`** (bulk re-load) → [`HX_USER_CHANGE_UPDATED`]:
///   fold the fields into the model silently, no view churn — matches the old
///   quiet field update for a re-sent list.
///
/// The C side owns the plan resolution + `is_new` determination, the self-uid
/// bookkeeping, and the join/rename logging keyed on the return.
///
/// # Safety
/// `chat` is the opaque `struct chat *` the signal forwards; `member_model` is a
/// valid `HxMemberModel *` (only read on the silent-upsert path); `name` is a
/// valid C string; `htlc` is opaque.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn hx_user_apply_recv(
    htlc: *mut c_void,
    chat: *mut c_void,
    member_model: *mut c_void,
    uid: u16,
    nick_color: u32,
    name: *const c_char,
    icon: u16,
    color: u16,
    is_new: c_int,
    skip_self_create: c_int,
    incremental: c_int,
) -> c_int {
    if is_new != 0 {
        if skip_self_create != 0 {
            // Our own row — the USER_LIST reply creates it in the right spot.
            return HX_USER_CHANGE_SKIPPED;
        }
        gtkhx_session_emit_user_create(
            gtkhx_session_get_default(),
            htlc,
            chat,
            uid,
            nick_color,
            name,
            icon,
            color,
            incremental,
        );
        return HX_USER_CHANGE_CREATED;
    }
    if incremental != 0 {
        gtkhx_session_emit_user_change(
            gtkhx_session_get_default(),
            htlc,
            chat,
            uid,
            nick_color,
            name,
            icon,
            color,
        );
        return HX_USER_CHANGE_CHANGED;
    }
    // Existing member seen during the bulk user-list load: keep the model
    // current without churning the view.
    hx_member_model_upsert(member_model, uid, name, icon, color, nick_color);
    HX_USER_CHANGE_UPDATED
}

/// `int hx_user_part_recv (htlc, chat, member_model, uid)` — emit `user-delete`
/// iff `uid` is a member of the chat (the fan-out removes the model entry
/// itself). Returns 1 when it emitted, 0 otherwise. The C side captures the
/// member's name *before* calling (the emit removes the entry) and logs the
/// "parts" line only when this returns 1.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `chat` is the opaque
/// `struct chat *` the signal forwards; `htlc` is opaque.
#[no_mangle]
pub unsafe extern "C" fn hx_user_part_recv(
    htlc: *mut c_void,
    chat: *mut c_void,
    member_model: *mut c_void,
    uid: u16,
) -> c_int {
    if hx_member_model_contains(member_model, uid) == 0 {
        return 0;
    }
    gtkhx_session_emit_user_delete(gtkhx_session_get_default(), htlc, chat, uid, 1);
    1
}

/// `void hx_user_info_recv (uid, name, info, len)` — emit the `user-info`
/// signal for a USER_INFO reply. The C handler keeps the parse (already Rust,
/// via `gtkhx_proto_parse_user_info`) and the `name_len && info_len` gate that
/// filters unanswered server frames; this publishes the parsed pair.
///
/// # Safety
/// `name` / `info` are valid C strings (`info` valid for at least `len` bytes).
#[no_mangle]
pub unsafe extern "C" fn hx_user_info_recv(
    uid: u16,
    name: *const c_char,
    info: *const c_char,
    len: u16,
) {
    gtkhx_session_emit_user_info(gtkhx_session_get_default(), uid, name, info, len);
}

/// `void hx_selfinfo_recv (htlc)` — emit the `self-updated` signal after a
/// SELFINFO reply re-parsed our own access bits / uid. The C handler keeps the
/// chunk parse and the `logged_in` flag; this is the view-notify hop.
///
/// # Safety
/// `htlc` is only forwarded to the signal (never dereferenced here).
#[no_mangle]
pub unsafe extern "C" fn hx_selfinfo_recv(htlc: *mut c_void) {
    gtkhx_session_emit_self_updated(gtkhx_session_get_default(), htlc);
}

/// `void hx_rcv_user_selfinfo (htlc, frame, frame_len)` — the SELFINFO
/// (`HTLS_HDR_USER_SELFINFO`) receive handler.
///
/// SELFINFO carries our own access bitmap + uid + icon. The chunk parse stays in
/// `hx_selfinfo_parse` (proto_helpers.c) so the Tier-2 unit tests can drive it
/// headless; it folds the fields into `htlc` and deliberately ignores the
/// server-supplied name (our local prefs nick is authoritative and we push it
/// back at agreement time). SELFINFO is the canonical "login complete" signal,
/// so we set the `logged_in` flag — the agreement Agree button reads it to
/// decide whether to send AGREEMENTAGREE. The view then refreshes toolbar
/// sensitivity (kick/ban gate on the access bits) off the `self-updated` emit.
///
/// This is NOT where post-login fetches fire: in the 1.5 flow SELFINFO arrives
/// before the agreement, so USER_GETLIST / news are sent from
/// `hx_send_agreement_agree`, after AGREEMENTAGREE is on the wire.
///
/// # Safety
/// `frame` is valid for `frame_len` bytes; `htlc` is the opaque connection.
#[no_mangle]
pub unsafe extern "C" fn hx_rcv_user_selfinfo(
    htlc: *mut c_void,
    frame: *const u8,
    frame_len: usize,
) {
    hx_selfinfo_parse(htlc, frame, frame_len);
    hx_conn_set_logged_in(htlc, 1);
    hx_selfinfo_recv(htlc);
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::{Cell, RefCell};
    use std::os::raw::c_int;

    #[derive(Debug, PartialEq, Eq, Clone)]
    pub enum Emit {
        Create {
            uid: u16,
            nick_color: u32,
            name: Vec<u8>,
            icon: u16,
            color: u16,
            incremental: bool,
        },
        Change {
            uid: u16,
            nick_color: u32,
            name: Vec<u8>,
            icon: u16,
            color: u16,
        },
        Delete {
            uid: u16,
            incremental: bool,
        },
        /// The silent-upsert path (existing member during a bulk load): no
        /// view signal fired, the model was updated directly.
        Upsert {
            uid: u16,
            nick_color: u32,
            name: Vec<u8>,
            icon: u16,
            color: u16,
        },
        /// A USER_INFO reply was published.
        Info {
            uid: u16,
            name: Vec<u8>,
            info: Vec<u8>,
            len: u16,
        },
        /// A SELFINFO reply refreshed our own access/uid.
        SelfUpdated,
    }

    thread_local! {
        /// Drives the stubbed member-model membership check.
        pub static CONTAINS: Cell<bool> = const { Cell::new(true) };
        /// Records the last emitted roster signal, or None.
        pub static EMIT: RefCell<Option<Emit>> = const { RefCell::new(None) };
        /// SELFINFO handler: did it call the chunk parse?
        pub static SELFINFO_PARSED: Cell<bool> = const { Cell::new(false) };
        /// SELFINFO handler: value passed to hx_conn_set_logged_in (or -1).
        pub static LOGGED_IN: Cell<c_int> = const { Cell::new(-1) };
    }

    pub fn reset() {
        CONTAINS.with(|c| c.set(true));
        EMIT.with(|c| *c.borrow_mut() = None);
        SELFINFO_PARSED.with(|c| c.set(false));
        LOGGED_IN.with(|c| c.set(-1));
    }
    pub fn take() -> Option<Emit> {
        EMIT.with(|c| c.borrow_mut().take())
    }
    pub fn record(e: Emit) {
        EMIT.with(|c| *c.borrow_mut() = Some(e));
    }
}

#[cfg(test)]
unsafe fn cbytes(p: *const c_char) -> Vec<u8> {
    if p.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(p).to_bytes().to_vec()
    }
}

#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
#[allow(clippy::too_many_arguments)]
unsafe fn gtkhx_session_emit_user_create(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    _chat: *mut c_void,
    uid: u16,
    nick_color: u32,
    nam: *const c_char,
    icon: u16,
    color: u16,
    incremental: c_int,
) {
    test_env::record(test_env::Emit::Create {
        uid,
        nick_color,
        name: cbytes(nam),
        icon,
        color,
        incremental: incremental != 0,
    });
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_user_change(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    _chat: *mut c_void,
    uid: u16,
    nick_color: u32,
    nam: *const c_char,
    icon: u16,
    color: u16,
) {
    test_env::record(test_env::Emit::Change {
        uid,
        nick_color,
        name: cbytes(nam),
        icon,
        color,
    });
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_user_delete(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    _chat: *mut c_void,
    uid: u16,
    incremental: c_int,
) {
    test_env::record(test_env::Emit::Delete {
        uid,
        incremental: incremental != 0,
    });
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_user_info(
    _self_: *mut c_void,
    uid: u16,
    nam: *const c_char,
    info: *const c_char,
    len: u16,
) {
    test_env::record(test_env::Emit::Info {
        uid,
        name: cbytes(nam),
        info: cbytes(info),
        len,
    });
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_self_updated(_self_: *mut c_void, _htlc: *mut c_void) {
    test_env::record(test_env::Emit::SelfUpdated);
}

#[cfg(test)]
unsafe fn hx_selfinfo_parse(_htlc: *mut c_void, _frame: *const u8, _frame_len: usize) -> u32 {
    test_env::SELFINFO_PARSED.with(|c| c.set(true));
    0
}

#[cfg(test)]
unsafe fn hx_conn_set_logged_in(_htlc: *mut c_void, v: c_int) {
    test_env::LOGGED_IN.with(|c| c.set(v));
}

#[cfg(test)]
unsafe fn hx_member_model_contains(_model: *mut c_void, _uid: u16) -> c_int {
    c_int::from(test_env::CONTAINS.with(|c| c.get()))
}

#[cfg(test)]
unsafe fn hx_member_model_upsert(
    _model: *mut c_void,
    uid: u16,
    name: *const c_char,
    icon: u16,
    color: u16,
    nick_color: u32,
) {
    test_env::record(test_env::Emit::Upsert {
        uid,
        nick_color,
        name: cbytes(name),
        icon,
        color,
    });
}

#[cfg(test)]
mod tests;
