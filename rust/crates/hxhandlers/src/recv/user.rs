//! Users: what the session made of a user arriving, changing or leaving, a
//! user list, what the server says about us, and the replies to creating
//! and joining a private chat, to a user's info, a kick and an account
//! read, on their way to the view.
//!
//! A live change and a list both end in the same roster-apply decision: a
//! new member becomes a `user-create`, an existing one is either a live
//! `user-change` or a silent model refresh. That shared tail is
//! [`hx_user_apply_recv`], called by both paths (`incremental` tells them
//! apart) so the create/change/upsert routing lives in one place. The
//! change-*decision* itself (`user_change::resolve`) lives in `hxproto`.

use std::cell::RefCell;
use std::collections::HashMap;
use std::os::raw::{c_char, c_int, c_void};

use hxsession::{Account, User};

use super::chat::{c_text, hx_chat_subject_emit};

#[cfg(not(test))]
use gtkhx_core::conn::{hx_conn_name, hx_conn_sess};
#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_self_updated, gtkhx_session_emit_user_change,
    gtkhx_session_emit_user_create, gtkhx_session_emit_user_delete, gtkhx_session_emit_user_info,
    gtkhx_session_emit_user_notice, gtkhx_session_get_default,
};

// HxMemberInfo is a type, not one of the shadowed functions, so it is imported
// unconditionally — the #[cfg(test)] doubles below replace the fns only.
use hxmodel::chat_members::HxMemberInfo;
#[cfg(not(test))]
use hxmodel::chat_members::{
    hx_member_model_contains, hx_member_model_count, hx_member_model_get_ignore,
    hx_member_model_get_info, hx_member_model_upsert,
};
#[cfg(not(test))]
use hxmodel::conversation::{
    hx_chat_cid, hx_chat_member_model, hx_chat_set_subject, hx_chat_subject, hx_chat_view,
};

#[cfg(not(test))]
extern "C" {
    /// Set our own "logged in" flag on the connection (gtkhx-core::conn). SELFINFO is the
    /// canonical login-complete signal; the agreement Agree button reads this.
    fn hx_conn_set_logged_in(htlc: *mut c_void, v: c_int);
    /// Our access bits, from their 8 wire bytes (gtkhx-core::conn).
    fn hx_conn_set_access(htlc: *mut c_void, bytes: *const u8);
    /// `struct chat *chat_with_cid (sess, cid)` — the chat with this id, or NULL.
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    /// `struct chat *chat_new (sess, cid)` — create (and register) a chat.
    fn chat_new(sess: *mut c_void, cid: u32) -> *mut c_void;
    /// `void chat_delete (sess, chat)` — drop a chat (chat.c).
    fn chat_delete(sess: *mut c_void, chat: *mut c_void);
    /// `void reload_news (widget, data)` — kick off the post-login news fetch
    /// (news.c); `data` is the session, `widget` is unused (pass NULL).
    fn reload_news(widget: *mut c_void, data: *mut c_void);
    /// gtkhx-core::conn accessors for our own identity bookkeeping.
    fn hx_conn_uid(htlc: *mut c_void) -> u16;
    fn hx_conn_set_uid(htlc: *mut c_void, v: u16);
    fn hx_conn_icon(htlc: *mut c_void) -> u16;
    fn hx_conn_set_icon(htlc: *mut c_void, v: u16);
    fn hx_conn_set_nick_color(htlc: *mut c_void, v: u32);
    /// Log a pre-formatted line under a debug category (debug.c) — the
    /// non-variadic sibling of debug_log.
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// RGB nick colour sentinel — "no colour, use theme default"
/// (`HX_NICK_COLOR_NONE`, hotline.h).
const HX_NICK_COLOR_NONE: u32 = 0xffff_ffff;

/// `user-notice` signal kinds (must match `HX_USER_NOTICE_*` in gtkhx_session.h).
const HX_USER_NOTICE_JOIN: u32 = 0;
const HX_USER_NOTICE_PART: u32 = 1;
const HX_USER_NOTICE_RENAME: u32 = 2;
const HX_USER_NOTICE_KICKED: u32 = 3;

/// glib TRUE (`gboolean`).
const TRUE: c_int = 1;

/// `gboolean` from a Rust bool.
#[inline]
fn gbool(b: bool) -> c_int {
    b as c_int
}

/// Bytes of a NUL-terminated C string, or `None` for a NULL pointer.
unsafe fn optr_bytes(p: *const c_char) -> Option<Vec<u8>> {
    if p.is_null() {
        None
    } else {
        Some(std::ffi::CStr::from_ptr(p).to_bytes().to_vec())
    }
}

/// Emit a pre-formatted line under `cat` via debug_log_str, stripping any NULs
/// so wire-derived interpolations can't panic CString::new. A debug trace must
/// never be able to crash the client.
///
/// # Safety
/// `debug_log_str` is an FFI call into debug.c.
unsafe fn debug_trace(cat: &std::ffi::CStr, line: String) {
    if let Ok(c) = std::ffi::CString::new(line.replace('\0', "")) {
        debug_log_str(cat.as_ptr(), c.as_ptr());
    }
}

// HxMemberInfo comes from hxmodel::chat_members. This crate used to define a
// fourth hand-synced `#[repr(C)]` mirror of the same C struct; hxmodel's copy
// is the one whose layout is pinned against chat_members.h by a const assert.

/// Result of [`hx_user_apply_recv`] — what (if anything) it did, for the
/// matching join/rename logging.
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
/// which, for the caller's matching notice:
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
/// The caller owns the plan resolution + `is_new` determination, the self-uid
/// bookkeeping, and the join/rename logging keyed on the return.
///
/// # Safety
/// `chat` is the opaque `struct chat *` the signal forwards; `member_model` is a
/// valid `HxMemberModel *` (only read on the silent-upsert path); `name` is a
/// valid C string; `htlc` is opaque.
#[allow(clippy::too_many_arguments)]
unsafe fn hx_user_apply_recv(
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

/// A user arriving in or changing on chat `cid`, or us in a chat we just
/// created, as the server describes them.
///
/// Resolves the chat (creating it if this is the first we've heard of the
/// cid), snapshots the member's pre-change state from the model, and runs
/// the pure change-plan decision (`user_change::resolve`: self-detection
/// incl. the SELFINFO-less uid adoption some 1.9 servers force,
/// new-vs-change, the colour / nick-colour preserve rules, and the
/// rename-notice test). It then routes the apply through the shared
/// [`hx_user_apply_recv`] and emits the matching join / rename notice
/// (both gated behind the showjoin pref in the view), plus the self icon /
/// nick-colour bookkeeping.
///
/// It deliberately does NOT copy the server's name into `htlc` — servers can
/// pin guests to override names (e.g. "Read the agreement") that must show in
/// the user list but must not bleed into the persisted NICK pref.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub unsafe fn changed(htlc: *mut c_void, cid: u32, user: &User) {
    let sess = hx_conn_sess(htlc.cast());
    let mut chat = chat_with_cid(sess, cid);
    if chat.is_null() {
        chat = chat_new(sess, cid);
    }
    let model = hx_chat_member_model(chat.cast());

    // Pre-change snapshot from the authoritative model, taken before the apply
    // updates it — so the preserve rules + rename notice see the old state.
    let mut old = unsafe { std::mem::zeroed::<HxMemberInfo>() };
    let old_exists = hx_member_model_get_info(model, user.uid, &mut old) != 0;
    let old_name_bytes = optr_bytes(old.name.as_ptr());

    let self_name_bytes = optr_bytes(hx_conn_name(htlc.cast()));
    // The name as the model holds it, so it compares with the names the
    // model and our nick hold.
    let name_c = c_text(&user.name);

    let plan = hxproto::user_change::resolve(&hxproto::user_change::ChangeInput {
        uid: user.uid,
        name: name_c.as_bytes(),
        got_color: user.status.is_some(),
        color: user.status.unwrap_or(0),
        got_nick_color: user.color.is_some(),
        nick_color: user.color.unwrap_or(HX_NICK_COLOR_NONE),
        old_exists,
        old_status: old.status,
        old_nick_color: if old_exists {
            old.nick_color
        } else {
            HX_NICK_COLOR_NONE
        },
        old_name: if old_exists {
            old_name_bytes.as_deref()
        } else {
            None
        },
        self_uid: hx_conn_uid(htlc.cast()),
        self_name: self_name_bytes.as_deref(),
    });

    if plan.adopt_self_uid {
        hx_conn_set_uid(htlc, user.uid);
        debug_trace(
            c"login",
            format!(
                "adopted self uid={} from USER_CHANGE broadcast (SELFINFO didn't carry it)",
                user.uid
            ),
        );
    }

    let emitted = hx_user_apply_recv(
        htlc,
        chat,
        model,
        user.uid,
        plan.eff_nick_color,
        name_c.as_ptr(),
        user.icon,
        plan.eff_color,
        gbool(plan.is_new),
        gbool(plan.skip_self_create),
        TRUE, // live broadcast, not the bulk USER_LIST load
    );

    if emitted == HX_USER_CHANGE_SKIPPED {
        // Our own row — the USER_LIST reply creates it in the right spot.
        return;
    } else if emitted == HX_USER_CHANGE_CREATED {
        gtkhx_session_emit_user_notice(
            gtkhx_session_get_default(),
            htlc,
            cid,
            HX_USER_NOTICE_JOIN,
            name_c.as_ptr(),
            std::ptr::null(),
        );
    } else {
        // HX_USER_CHANGE_CHANGED. Bail on ignored users before the notice.
        if hx_member_model_get_ignore(model, user.uid) != 0 {
            return;
        }
        if plan.do_rename_notice {
            // old.name is the pre-change snapshot taken above.
            gtkhx_session_emit_user_notice(
                gtkhx_session_get_default(),
                htlc,
                cid,
                HX_USER_NOTICE_RENAME,
                name_c.as_ptr(),
                old.name.as_ptr(),
            );
        }
    }

    // Self bookkeeping — mirror the just-applied wire/plan values into htlc.
    // (A new-self returned early via SKIPPED, so a self change here is always an
    // existing member.) The name is deliberately not copied back (see above).
    if user.uid != 0 && user.uid == hx_conn_uid(htlc.cast()) {
        let icon = if user.icon != 0 {
            user.icon
        } else if old_exists {
            old.icon
        } else {
            hx_conn_icon(htlc.cast())
        };
        hx_conn_set_icon(htlc, icon);
        if let Some(c) = user.color {
            hx_conn_set_nick_color(htlc, c);
        }
        debug_trace(
            c"name",
            format!(
                "USER_CHANGE for our uid={}: keeping local htlc->name",
                user.uid
            ),
        );
    }
}

/// Emit `user-delete` iff `uid` is a member of the chat (the fan-out removes
/// the model entry itself). Returns 1 when it emitted, 0 otherwise. The
/// caller captures the member's name *before* calling (the emit removes the
/// entry) and logs the "parts" line only when this returns 1.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `chat` is the opaque
/// `struct chat *` the signal forwards; `htlc` is opaque.
unsafe fn hx_user_part_recv(
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

/// User `uid` leaving chat `cid`.
///
/// Resolves the chat, captures the leaving member's name *before* the emit
/// (the `user-delete` fan-out removes the model entry), and delegates the
/// membership-gated emit to [`hx_user_part_recv`]. When that emitted, it
/// emits the "parts: <name>" notice — which the view shows only when the
/// showjoin pref is on.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn left(htlc: *mut c_void, cid: u32, uid: u16) {
    let sess = hx_conn_sess(htlc.cast());
    let chat = chat_with_cid(sess, cid);
    if chat.is_null() {
        return;
    }
    let model = hx_chat_member_model(chat.cast());

    // Snapshot the member before the emit removes it, so we have the name for
    // the "parts" line.
    let mut info = unsafe { std::mem::zeroed::<HxMemberInfo>() };
    let have = hx_member_model_get_info(model, uid, &mut info) != 0;

    if hx_user_part_recv(htlc, chat, model, uid) != 0 && have {
        gtkhx_session_emit_user_notice(
            gtkhx_session_get_default(),
            htlc,
            cid,
            HX_USER_NOTICE_PART,
            info.name.as_ptr(),
            std::ptr::null(),
        );
    }
}

/// What the server says about us: our uid and icon, our access bits and the
/// color it has for our name, each where it said, folded into the
/// connection. The name it has for us is not taken: the one we chose wins,
/// and goes to the server with the agreement. The self-info is the server's
/// word that we are logged in, which the agreement's Agree button reads,
/// and the toolbar refreshes what the access bits allow.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn selfinfo(
    htlc: *mut c_void,
    uid: Option<u16>,
    icon: Option<u16>,
    access: Option<u64>,
    color: Option<u32>,
) {
    if let Some(access) = access {
        hx_conn_set_access(htlc, access.to_be_bytes().as_ptr());
    }
    if let Some(uid) = uid {
        hx_conn_set_uid(htlc, uid);
    }
    if let Some(icon) = icon {
        hx_conn_set_icon(htlc, icon);
    }
    if let Some(color) = color {
        hx_conn_set_nick_color(htlc, color);
    }
    hx_conn_set_logged_in(htlc, 1);
    gtkhx_session_emit_self_updated(gtkhx_session_get_default(), htlc);
}

/// `users` into `chat`'s roster, silently where a member is already there
/// (`hx_user_apply_recv` with `incremental=FALSE` — the join chime is
/// suppressed because these users are already in the room), and
/// `subject`, when there is one, into the chat: published with the
/// initial-subject-discovery emit ([`hx_chat_subject_emit`], no "Subject
/// Changed to" line).
///
/// Two self-bookkeeping gates: the Colored-Nicknames self-mirror (copy a
/// listed colour onto `htlc` when the entry is us), and the self-uid
/// adoption for servers that omit USER_LIST from SELFINFO (the first entry
/// matching our nick+icon claims our uid).
///
/// # Safety
/// Main thread; `htlc` is a live connection and `chat` its `struct chat *`.
pub unsafe fn load(htlc: *mut c_void, chat: *mut c_void, users: &[User], subject: Option<&str>) {
    let model = hx_chat_member_model(chat.cast());
    for user in users {
        let name_c = c_text(&user.name);
        // "not already in this chat's membership" — recomputed per entry so a
        // stale new=1 doesn't spawn spurious creates for later users.
        let is_new = hx_member_model_contains(model, user.uid) == 0;
        if let Some(nc) = user.color {
            if user.uid == hx_conn_uid(htlc) {
                hx_conn_set_nick_color(htlc, nc);
            }
        }
        if hx_conn_uid(htlc) == 0 && user.icon == hx_conn_icon(htlc) {
            if let Some(sn) = optr_bytes(hx_conn_name(htlc.cast())) {
                if name_c.as_bytes() == sn.as_slice() {
                    hx_conn_set_uid(htlc, user.uid);
                }
            }
        }
        hx_user_apply_recv(
            htlc,
            chat,
            model,
            user.uid,
            user.color.unwrap_or(HX_NICK_COLOR_NONE),
            name_c.as_ptr(),
            user.icon,
            user.status.unwrap_or(0),
            gbool(is_new),
            gbool(false), // skip_self_create = FALSE
            gbool(false), // incremental = FALSE
        );
    }
    if let Some(subject) = subject {
        let s = crate::recv::chat::fit_subject(subject);
        hx_chat_set_subject(chat.cast(), s.as_ptr() as *const c_char, s.len());
        hx_chat_subject_emit(htlc, hx_chat_cid(chat.cast()), hx_chat_subject(chat.cast()));
    }
}

/// The user list asked for once logged in: the public chat's roster and
/// subject, then the news fetch.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn listed(htlc: *mut c_void, users: &[User], subject: Option<&str>) {
    let sess = hx_conn_sess(htlc.cast());
    let chat = chat_with_cid(sess, 0);
    if !chat.is_null() {
        load(htlc, chat, users, subject);
    }
    reload_news(std::ptr::null_mut(), sess);
}

/// A join in flight: its chat, and whether the user has left that chat
/// since asking.
#[derive(Clone, Copy)]
struct Join {
    cid: u32,
    parted: bool,
}

thread_local! {
    /// The joins in flight, by connection and trans.
    static JOINS: RefCell<HashMap<(usize, u32), Join>> = RefCell::new(HashMap::new());
}

/// A join of chat `cid` goes out on `trans`.
pub(crate) fn join_requested(htlc: *mut c_void, trans: u32, cid: u32) {
    JOINS.with(|j| {
        j.borrow_mut()
            .insert((htlc as usize, trans), Join { cid, parted: false })
    });
}

/// The user leaves chat `cid`: a join of it still in flight is answered
/// for a chat that is gone.
pub(crate) fn join_parted(htlc: *mut c_void, cid: u32) {
    JOINS.with(|j| {
        for (_, join) in j
            .borrow_mut()
            .iter_mut()
            .filter(|((h, _), join)| *h == htlc as usize && join.cid == cid)
        {
            join.parted = true;
        }
    });
}

/// Forget what `htlc` asked for before: a new connection numbers its
/// requests afresh.
pub(crate) fn joins_forget(htlc: *mut c_void) {
    JOINS.with(|j| j.borrow_mut().retain(|(h, _), _| *h != htlc as usize));
}

fn join_answered(htlc: *mut c_void, trans: u32) -> Option<Join> {
    JOINS.with(|j| j.borrow_mut().remove(&(htlc as usize, trans)))
}

/// The reply to the join of private chat `cid` on `trans`: the chat, made
/// now that we are in it, with who is there and its subject. Not if the
/// user left it while the join was on its way.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn joined(
    htlc: *mut c_void,
    trans: u32,
    cid: u32,
    users: &[User],
    subject: Option<&str>,
) {
    if join_answered(htlc, trans).is_some_and(|j| j.parted) {
        return;
    }
    let sess = hx_conn_sess(htlc.cast());
    let mut chat = chat_with_cid(sess, cid);
    if chat.is_null() {
        chat = chat_new(sess, cid);
    }
    load(htlc, chat, users, subject);
}

/// A request the session said failed: what asked is let go. A refused
/// check for a new account means there is none, and the account is made;
/// that refusal is no news, and the caller shows nothing of it (true). A
/// refused join drops its chat when nothing shows it: one that a new chat's
/// reply made, with no window and no one in it. A chat already open stays.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn failed(htlc: *mut c_void, trans: u32, reason: Option<&str>) -> bool {
    match answered(htlc, trans) {
        // A refusal says it is not there, whether or not the server said
        // why; a reply cut short says nothing either way.
        Some(Asked::Check { create, made, .. }) if reason != Some(super::chat::CUT_SHORT) => {
            crate::send::user::create(htlc, &create, made);
            return true;
        }
        Some(Asked::Check { made, .. }) | Some(Asked::Made(made)) => made(false),
        _ => {}
    }
    let Some(join) = join_answered(htlc, trans) else {
        return false;
    };
    let sess = hx_conn_sess(htlc.cast());
    let chat = chat_with_cid(sess, join.cid);
    if !chat.is_null()
        && hx_chat_view(chat.cast()).is_null()
        && hx_member_model_count(hx_chat_member_model(chat.cast())) == 0
    {
        chat_delete(sess, chat);
    }
    false
}

/// What a request in flight is answered into.
pub(crate) enum Asked {
    /// A user's info, for the user it was asked of: the reply does not say.
    Info(u16),
    /// An account read, for the editor that asked.
    Account(Box<dyn FnOnce(&Account)>),
    /// An account made, for the editor that made it: told whether the
    /// server made it.
    Made(Box<dyn FnOnce(bool)>),
    /// A read of a new account's login, to tell whether it is taken: read,
    /// it is, and `exists` runs; refused, it isn't, and `create` goes.
    Check {
        create: hxrequest::Request,
        exists: Box<dyn FnOnce()>,
        made: Box<dyn FnOnce(bool)>,
    },
}

thread_local! {
    /// The requests in flight, by connection and trans.
    static ASKED: RefCell<HashMap<(usize, u32), Asked>> = RefCell::new(HashMap::new());
}

/// A request goes out on `trans`, its reply to go into `what`.
pub(crate) fn asked(htlc: *mut c_void, trans: u32, what: Asked) {
    ASKED.with(|a| a.borrow_mut().insert((htlc as usize, trans), what));
}

fn answered(htlc: *mut c_void, trans: u32) -> Option<Asked> {
    ASKED.with(|a| a.borrow_mut().remove(&(htlc as usize, trans)))
}

/// Let go of what `htlc` asked for before: a closed connection gets no more
/// replies, and a new one numbers its requests afresh.
pub(crate) fn forget(htlc: *mut c_void) {
    // Dropped once ASKED is released: an editor's callback may be what goes.
    let _gone: Vec<_> = ASKED.with(|a| {
        let mut a = a.borrow_mut();
        let keys: Vec<_> = a
            .keys()
            .filter(|(h, _)| *h == htlc as usize)
            .copied()
            .collect();
        keys.into_iter().filter_map(|k| a.remove(&k)).collect()
    });
    joins_forget(htlc);
}

/// What the server says of a user, for the user it was asked of. A reply
/// that leaves the name or the text out shows nothing.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn info(htlc: *mut c_void, trans: u32, name: &str, info: &str) {
    let Some(Asked::Info(uid)) = answered(htlc, trans) else {
        return;
    };
    let (name, info) = (c_text(name), c_text(info));
    if name.is_empty() || info.is_empty() {
        return;
    }
    let len = info.as_bytes().len() as u16;
    gtkhx_session_emit_user_info(
        gtkhx_session_get_default(),
        htlc,
        uid,
        name.as_ptr(),
        info.as_ptr(),
        len,
    );
}

/// A kick went through: said in the public chat.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn kicked(htlc: *mut c_void) {
    gtkhx_session_emit_user_notice(
        gtkhx_session_get_default(),
        htlc,
        0,
        HX_USER_NOTICE_KICKED,
        std::ptr::null(),
        std::ptr::null(),
    );
}

/// An account change went through: an account made tells the editor that
/// made it.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn account_changed(htlc: *mut c_void, trans: u32) {
    if let Some(Asked::Made(made)) = answered(htlc, trans) {
        made(true);
    }
}

/// An account, for the editor that asked; one the server sent no access
/// bits for fills nothing. For a new account's check, any answer means the
/// login is taken.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn account(htlc: *mut c_void, trans: u32, account: &Account) {
    match answered(htlc, trans) {
        Some(Asked::Account(fill)) if account.access.is_some() => fill(account),
        Some(Asked::Check { exists, .. }) => exists(),
        _ => {}
    }
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
        /// Our access bits' wire bytes (hx_conn_set_access), or None.
        pub static ACCESS: Cell<Option<[u8; 8]>> = const { Cell::new(None) };
        /// SELFINFO handler: value passed to hx_conn_set_logged_in (or -1).
        pub static LOGGED_IN: Cell<c_int> = const { Cell::new(-1) };
        /// get_info: the member snapshot to return (name + fields), or None
        /// (absent → get_info returns FALSE).
        pub static MEMBER: RefCell<Option<MemberSnap>> = const { RefCell::new(None) };
        /// The last emitted user-notice, as (cid, kind, name, old_name).
        pub static NOTICE: RefCell<Option<Notice>> = const { RefCell::new(None) };
        /// Our own uid (hx_conn_uid / set_uid).
        pub static SELF_UID: Cell<u16> = const { Cell::new(0) };
        /// Our own icon (hx_conn_icon / set_icon).
        pub static SELF_ICON: Cell<u16> = const { Cell::new(0) };
        /// Our own nick colour (hx_conn_set_nick_color).
        pub static SELF_NICK_COLOR: Cell<u32> = const { Cell::new(0) };
        /// get_ignore return.
        pub static IGNORE: Cell<bool> = const { Cell::new(false) };
        /// Our own display name (hx_conn_name returns a pointer into this).
        pub static SELF_NAME: RefCell<std::ffi::CString> =
            RefCell::new(std::ffi::CString::new("").unwrap());
        /// The chat subject the set-subject double stored (hx_chat_subject
        /// returns a pointer into it).
        pub static SUBJECT_STORE: RefCell<std::ffi::CString> =
            RefCell::new(std::ffi::CString::new("").unwrap());
        /// The cid hx_chat_cid returns.
        pub static CHAT_CID: Cell<u32> = const { Cell::new(0) };
        /// True once reload_news fired (the login's user list).
        pub static RELOAD_NEWS: Cell<bool> = const { Cell::new(false) };
        /// The cids chat_new made.
        pub static CHATS_MADE: RefCell<Vec<u32>> = const { RefCell::new(Vec::new()) };
        /// Whether chat_with_cid finds a chat.
        pub static CHAT_EXISTS: Cell<bool> = const { Cell::new(true) };
        /// Whether the chat has a window (hx_chat_view).
        pub static VIEW: Cell<bool> = const { Cell::new(false) };
        /// How many are in the chat (hx_member_model_count).
        pub static MEMBERS: Cell<u32> = const { Cell::new(0) };
        /// True once chat_delete fired.
        pub static CHAT_DELETED: Cell<bool> = const { Cell::new(false) };
    }

    /// A member snapshot the get_info double hands back.
    #[derive(Clone)]
    pub struct MemberSnap {
        pub icon: u16,
        pub status: u16,
        pub nick_color: u32,
        pub name: Vec<u8>,
    }

    /// A recorded user-notice emit.
    #[derive(Debug, PartialEq, Eq, Clone)]
    pub struct Notice {
        pub cid: u32,
        pub kind: u32,
        pub name: Vec<u8>,
        /// Empty when the emit passed NULL (everything but a rename).
        pub old_name: Vec<u8>,
    }

    pub fn reset() {
        CONTAINS.with(|c| c.set(true));
        EMIT.with(|c| *c.borrow_mut() = None);
        ACCESS.with(|c| c.set(None));
        LOGGED_IN.with(|c| c.set(-1));
        MEMBER.with(|c| *c.borrow_mut() = None);
        NOTICE.with(|c| *c.borrow_mut() = None);
        SELF_UID.with(|c| c.set(0));
        SELF_ICON.with(|c| c.set(0));
        SELF_NICK_COLOR.with(|c| c.set(0));
        IGNORE.with(|c| c.set(false));
        SELF_NAME.with(|c| *c.borrow_mut() = std::ffi::CString::new("").unwrap());
        SUBJECT_STORE.with(|c| *c.borrow_mut() = std::ffi::CString::new("").unwrap());
        CHAT_CID.with(|c| c.set(0));
        RELOAD_NEWS.with(|c| c.set(false));
        CHATS_MADE.with(|c| c.borrow_mut().clear());
        CHAT_EXISTS.with(|c| c.set(true));
        VIEW.with(|c| c.set(false));
        MEMBERS.with(|c| c.set(0));
        CHAT_DELETED.with(|c| c.set(false));
    }

    /// Set the self display name the hx_conn_name double returns.
    pub fn set_self_name(name: &str) {
        SELF_NAME.with(|c| *c.borrow_mut() = std::ffi::CString::new(name).unwrap());
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

// Test double that must match the production `extern "C"` emit signature
// (same argument count) so the handler under test calls it unchanged.
#[cfg(test)]
#[allow(clippy::too_many_arguments)]
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
    _htlc: *mut c_void,
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
unsafe fn hx_conn_set_access(_htlc: *mut c_void, bytes: *const u8) {
    let b = std::slice::from_raw_parts(bytes, 8).try_into().unwrap();
    test_env::ACCESS.with(|c| c.set(Some(b)));
}

#[cfg(test)]
unsafe fn hx_conn_set_logged_in(_htlc: *mut c_void, v: c_int) {
    test_env::LOGGED_IN.with(|c| c.set(v));
}

// Non-null sentinels so the handlers' null-guards pass in tests.
#[cfg(test)]
const FAKE_CHAT: *mut c_void = 0xC0FE_usize as *mut c_void;
#[cfg(test)]
const FAKE_MODEL: *mut c_void = 0xB0B0_usize as *mut c_void;

#[cfg(test)]
unsafe fn hx_conn_sess(_htlc: *mut c_void) -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
unsafe fn chat_with_cid(_sess: *mut c_void, _cid: u32) -> *mut c_void {
    if test_env::CHAT_EXISTS.with(|c| c.get()) {
        FAKE_CHAT
    } else {
        std::ptr::null_mut()
    }
}

#[cfg(test)]
unsafe fn hx_chat_member_model(_chat: *mut c_void) -> *mut c_void {
    FAKE_MODEL
}

#[cfg(test)]
unsafe fn hx_member_model_get_info(
    _model: *mut c_void,
    _uid: u16,
    out: *mut HxMemberInfo,
) -> c_int {
    test_env::MEMBER.with(|c| match &*c.borrow() {
        Some(snap) => {
            let o = &mut *out;
            o.uid = _uid;
            o.icon = snap.icon;
            o.status = snap.status;
            o.nick_color = snap.nick_color;
            o.name = [0; 32];
            let n = snap.name.len().min(31);
            for i in 0..n {
                o.name[i] = snap.name[i] as c_char;
            }
            1
        }
        None => 0,
    })
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_user_notice(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cid: u32,
    kind: u32,
    name: *const c_char,
    old_name: *const c_char,
) {
    test_env::NOTICE.with(|c| {
        *c.borrow_mut() = Some(test_env::Notice {
            cid,
            kind,
            name: cstr_bytes(name),
            old_name: cstr_bytes(old_name),
        })
    });
}

#[cfg(test)]
unsafe fn cstr_bytes(p: *const c_char) -> Vec<u8> {
    if p.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(p).to_bytes().to_vec()
    }
}

#[cfg(test)]
unsafe fn chat_new(_sess: *mut c_void, cid: u32) -> *mut c_void {
    test_env::CHATS_MADE.with(|c| c.borrow_mut().push(cid));
    FAKE_CHAT
}

#[cfg(test)]
unsafe fn chat_delete(_sess: *mut c_void, _chat: *mut c_void) {
    test_env::CHAT_DELETED.with(|c| c.set(true));
}

#[cfg(test)]
unsafe fn hx_chat_view(_chat: *const c_void) -> *mut c_void {
    if test_env::VIEW.with(|c| c.get()) {
        FAKE_CHAT
    } else {
        std::ptr::null_mut()
    }
}

#[cfg(test)]
unsafe fn hx_member_model_count(_model: *mut c_void) -> u32 {
    test_env::MEMBERS.with(|c| c.get())
}

#[cfg(test)]
unsafe fn hx_member_model_get_ignore(_model: *mut c_void, _uid: u16) -> c_int {
    c_int::from(test_env::IGNORE.with(|c| c.get()))
}

#[cfg(test)]
unsafe fn hx_conn_uid(_htlc: *mut c_void) -> u16 {
    test_env::SELF_UID.with(|c| c.get())
}

#[cfg(test)]
unsafe fn hx_conn_set_uid(_htlc: *mut c_void, v: u16) {
    test_env::SELF_UID.with(|c| c.set(v));
}

#[cfg(test)]
unsafe fn hx_conn_icon(_htlc: *mut c_void) -> u16 {
    test_env::SELF_ICON.with(|c| c.get())
}

#[cfg(test)]
unsafe fn hx_conn_set_icon(_htlc: *mut c_void, v: u16) {
    test_env::SELF_ICON.with(|c| c.set(v));
}

#[cfg(test)]
unsafe fn hx_conn_set_nick_color(_htlc: *mut c_void, v: u32) {
    test_env::SELF_NICK_COLOR.with(|c| c.set(v));
}

#[cfg(test)]
unsafe fn hx_conn_name(_htlc: *mut c_void) -> *const c_char {
    test_env::SELF_NAME.with(|c| c.borrow().as_ptr())
}

#[cfg(test)]
unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}

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
unsafe fn hx_chat_set_subject(_chat: *mut c_void, s: *const c_char, len: usize) {
    let bytes = std::slice::from_raw_parts(s as *const u8, len);
    // NUL-truncate like the real model stores it.
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    test_env::SUBJECT_STORE
        .with(|c| *c.borrow_mut() = std::ffi::CString::new(&bytes[..end]).unwrap_or_default());
}

#[cfg(test)]
unsafe fn hx_chat_cid(_chat: *const c_void) -> u32 {
    test_env::CHAT_CID.with(|c| c.get())
}

#[cfg(test)]
unsafe fn hx_chat_subject(_chat: *const c_void) -> *const c_char {
    // The stored CString lives in the thread-local and isn't mutated during the
    // emit, so the borrowed pointer stays valid for the caller.
    test_env::SUBJECT_STORE.with(|c| c.borrow().as_ptr())
}

#[cfg(test)]
unsafe fn reload_news(_widget: *mut c_void, _data: *mut c_void) {
    test_env::RELOAD_NEWS.with(|c| c.set(true));
}

#[cfg(test)]
mod tests;
