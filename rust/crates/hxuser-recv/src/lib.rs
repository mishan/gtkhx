//! User-roster receive handlers (ported from `rcv.c`).
//!
//! The `USER_CHANGE` / `USER_PART` broadcasts are the most entangled of the
//! receive handlers — they read the per-chat member model, mutate `htlc` self
//! state, and log. The pure change-*decision* (`hx_user_change_plan_resolve`)
//! already lives in `proto_helpers.c` with its own Tier-2 tests. This crate
//! takes the next cleanly-extractable slice: the emit *routing* — which
//! `GtkhxSession` signal fires for which outcome — leaving the parse, the plan,
//! the model reads, the ignore/rename logging, and the self-bookkeeping in C.

use std::os::raw::{c_char, c_int, c_void};

#[cfg(not(test))]
extern "C" {
    /// The singleton `GtkhxSession` GObject (gtkhx-session).
    fn gtkhx_session_get_default() -> *mut c_void;
    /// `GtkhxSession::user-create (htlc, chat, user, nam, icon, color, incremental)`.
    fn gtkhx_session_emit_user_create(
        self_: *mut c_void,
        htlc: *mut c_void,
        chat: *mut c_void,
        user: *mut c_void,
        nam: *const c_char,
        icon: u16,
        color: u16,
        incremental: c_int,
    );
    /// `GtkhxSession::user-change (htlc, chat, user, nam, icon, color)`.
    fn gtkhx_session_emit_user_change(
        self_: *mut c_void,
        htlc: *mut c_void,
        chat: *mut c_void,
        user: *mut c_void,
        nam: *const c_char,
        icon: u16,
        color: u16,
    );
    /// `GtkhxSession::user-delete (htlc, chat, user, incremental)`.
    fn gtkhx_session_emit_user_delete(
        self_: *mut c_void,
        htlc: *mut c_void,
        chat: *mut c_void,
        user: *mut c_void,
        incremental: c_int,
    );
    /// Whether `uid` is a member of the per-chat model (hxmember-model).
    fn hx_member_model_contains(model: *mut c_void, uid: u16) -> c_int;
}

/// Result of [`hx_user_change_recv`] — tells the C side what (if anything) it
/// emitted, so it can do the matching join/rename logging.
pub const HX_USER_CHANGE_SKIPPED: c_int = 0;
pub const HX_USER_CHANGE_CREATED: c_int = 1;
pub const HX_USER_CHANGE_CHANGED: c_int = 2;

/// `int hx_user_change_recv (htlc, chat, carrier, name, icon, color, is_new,
/// skip_self_create, incremental)` — route a resolved `USER_CHANGE` to the right
/// roster signal. Returns [`HX_USER_CHANGE_SKIPPED`] (nothing emitted — our own
/// join, deferred to the USER_LIST reply), [`HX_USER_CHANGE_CREATED`]
/// (user-create), or [`HX_USER_CHANGE_CHANGED`] (user-change). The C side owns
/// the plan resolution, the model reads, and the join/rename logging keyed on
/// this return.
///
/// # Safety
/// `chat` / `carrier` are the opaque `struct chat *` / `struct hx_user *` the
/// signal forwards; `name` is a valid C string; `htlc` is opaque.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn hx_user_change_recv(
    htlc: *mut c_void,
    chat: *mut c_void,
    carrier: *mut c_void,
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
            carrier,
            name,
            icon,
            color,
            incremental,
        );
        return HX_USER_CHANGE_CREATED;
    }
    gtkhx_session_emit_user_change(
        gtkhx_session_get_default(),
        htlc,
        chat,
        carrier,
        name,
        icon,
        color,
    );
    HX_USER_CHANGE_CHANGED
}

/// `int hx_user_part_recv (htlc, chat, member_model, carrier, uid)` — emit
/// `user-delete` iff `uid` is a member of the chat (the fan-out removes the
/// model entry itself). Returns 1 when it emitted, 0 otherwise. The C side
/// captures the member's name *before* calling (the emit removes the entry) and
/// logs the "parts" line only when this returns 1.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `chat` / `carrier` are the
/// opaque `struct chat *` / `struct hx_user *` the signal forwards; `htlc` is
/// opaque.
#[no_mangle]
pub unsafe extern "C" fn hx_user_part_recv(
    htlc: *mut c_void,
    chat: *mut c_void,
    member_model: *mut c_void,
    carrier: *mut c_void,
    uid: u16,
) -> c_int {
    if hx_member_model_contains(member_model, uid) == 0 {
        return 0;
    }
    gtkhx_session_emit_user_delete(gtkhx_session_get_default(), htlc, chat, carrier, 1);
    1
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::{Cell, RefCell};

    #[derive(Debug, PartialEq, Eq, Clone)]
    pub enum Emit {
        Create {
            name: Vec<u8>,
            icon: u16,
            color: u16,
            incremental: bool,
        },
        Change {
            name: Vec<u8>,
            icon: u16,
            color: u16,
        },
        Delete {
            incremental: bool,
        },
    }

    thread_local! {
        /// Drives the stubbed member-model membership check.
        pub static CONTAINS: Cell<bool> = const { Cell::new(true) };
        /// Records the last emitted roster signal, or None.
        pub static EMIT: RefCell<Option<Emit>> = const { RefCell::new(None) };
    }

    pub fn reset() {
        CONTAINS.with(|c| c.set(true));
        EMIT.with(|c| *c.borrow_mut() = None);
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
    _user: *mut c_void,
    nam: *const c_char,
    icon: u16,
    color: u16,
    incremental: c_int,
) {
    test_env::record(test_env::Emit::Create {
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
    _user: *mut c_void,
    nam: *const c_char,
    icon: u16,
    color: u16,
) {
    test_env::record(test_env::Emit::Change {
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
    _user: *mut c_void,
    incremental: c_int,
) {
    test_env::record(test_env::Emit::Delete {
        incremental: incremental != 0,
    });
}

#[cfg(test)]
unsafe fn hx_member_model_contains(_model: *mut c_void, _uid: u16) -> c_int {
    c_int::from(test_env::CONTAINS.with(|c| c.get()))
}

#[cfg(test)]
mod tests;
