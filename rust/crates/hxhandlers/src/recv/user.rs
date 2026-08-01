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
    hx_member_model_contains, hx_member_model_get_ignore, hx_member_model_get_info,
    hx_member_model_upsert,
};
#[cfg(not(test))]
use hxmodel::conversation::{
    hx_chat_cid, hx_chat_member_model, hx_chat_set_subject, hx_chat_subject,
};

// Native reply parsers — pure Rust, identical in test and production. The old C
// rcv_task_user_list / _user_info round-tripped through the
// gtkhx_proto_parse_user_* C ABI; here we call the native parsers directly.
use hotline_proto::parse::{parse_user_info, parse_user_list_record};
use hotline_proto::wire::ChunkIter;

/// Wire chunk types carried in a USER_LIST reply (hotline.h).
const HTLS_DATA_USER_LIST: u16 = 0x012c;
const HTLS_DATA_CHAT_SUBJECT: u16 = 0x0073;

#[cfg(not(test))]
extern "C" {
    /// Parse a SELFINFO frame's chunks into `htlc` (access bits / uid / icon).
    /// Deliberately ignores the server-supplied name — our local prefs nick is
    /// authoritative. C helper in proto_helpers.c.
    fn hx_selfinfo_parse(htlc: *mut c_void, frame: *const u8, frame_len: usize) -> u32;
    /// Set our own "logged in" flag on the connection (gtkhx-core::conn). SELFINFO is the
    /// canonical login-complete signal; the agreement Agree button reads this.
    fn hx_conn_set_logged_in(htlc: *mut c_void, v: c_int);
    /// `struct chat *chat_with_cid (sess, cid)` — the chat with this id, or NULL.
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    /// `int task_inerror (htlc, frame, frame_len)` — TRUE if the frame is a
    /// task-error reply we should bail on (protocol.h).
    fn task_inerror(htlc: *mut c_void, frame: *const u8, frame_len: usize) -> c_int;
    /// `struct chat *chat_new (sess, cid)` — create (and register) a chat.
    fn chat_new(sess: *mut c_void, cid: u32) -> *mut c_void;
    /// `void chat_delete (sess, chat)` — drop a chat (chat.c). Used when a join's
    /// USER_LIST reply comes back a task error.
    fn chat_delete(sess: *mut c_void, chat: *mut c_void);
    /// `void reload_news (widget, data)` — kick off the post-login news fetch
    /// (news.c); `data` is the session, `widget` is unused (pass NULL).
    fn reload_news(widget: *mut c_void, data: *mut c_void);
    /// The initial-subject-discovery emit (recv/chat.rs): publish a chat subject
    /// with no "Subject Changed to" log line.
    fn hx_chat_subject_emit(htlc: *mut c_void, cid: u32, subject: *const c_char);
    /// GLib `g_free` — release the `guint16 *` uid task parameter.
    fn g_free(p: *mut c_void);
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

/// glib TRUE (`gboolean`).
const TRUE: c_int = 1;

/// `gboolean` from a Rust bool.
#[inline]
fn gbool(b: bool) -> c_int {
    b as c_int
}

/// A `CString` from wire bytes, truncated at the first interior NUL — mirroring
/// how the old C `char*` extractor buffer terminated. Infallible (the truncated
/// slice has no interior NUL).
unsafe fn cstring_first_nul(bytes: &[u8]) -> std::ffi::CString {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    std::ffi::CString::new(&bytes[..end]).unwrap_or_default()
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

/// `void hx_rcv_user_change (htlc, frame, frame_len)` — the live USER_CHANGE
/// (`HTLS_HDR_USER_CHANGE`) broadcast handler.
///
/// Parses the frame natively (`parse_user_change`), resolves the chat (creating
/// it if this is the first we've heard of the cid), snapshots the member's
/// pre-change state from the model, and runs the pure change-plan decision
/// natively (`user_change::resolve` — the same code the retired C
/// `hx_user_change_plan_resolve` wrapped: self-detection incl. the SELFINFO-less
/// uid adoption some 1.9 servers force, new-vs-change, the colour / nick-colour
/// preserve rules, and the rename-notice test). It then routes the apply through
/// the shared [`hx_user_apply_recv`] and does the matching join / rename logging
/// (both gated behind the showjoin pref inside the C shims), plus the self
/// icon / nick-colour bookkeeping.
///
/// It deliberately does NOT copy the server's name into `htlc` — servers can
/// pin guests to override names (e.g. "Read the agreement") that must show in
/// the user list but must not bleed into the persisted NICK pref.
///
/// # Safety
/// `frame` is valid for `frame_len` bytes; `htlc` is the opaque connection.
#[no_mangle]
pub unsafe extern "C" fn hx_rcv_user_change(htlc: *mut c_void, frame: *const u8, frame_len: usize) {
    if frame.is_null() || task_inerror(htlc, frame, frame_len) != 0 {
        return;
    }
    let buf = std::slice::from_raw_parts(frame, frame_len);
    let uc = hotline_proto::parse::parse_user_change(buf, frame_len, 31);

    let sess = hx_conn_sess(htlc.cast());
    let mut chat = chat_with_cid(sess, uc.cid);
    if chat.is_null() {
        chat = chat_new(sess, uc.cid);
    }
    let model = hx_chat_member_model(chat.cast());

    // Pre-change snapshot from the authoritative model, taken before the apply
    // updates it — so the preserve rules + rename notice see the old state.
    let mut old = unsafe { std::mem::zeroed::<HxMemberInfo>() };
    let old_exists = hx_member_model_get_info(model, uc.uid, &mut old) != 0;
    let old_name_bytes = optr_bytes(old.name.as_ptr());

    let self_name_bytes = optr_bytes(hx_conn_name(htlc.cast()));

    let plan = hotline_proto::user_change::resolve(&hotline_proto::user_change::ChangeInput {
        uid: uc.uid,
        name: &uc.name,
        got_color: uc.got_color,
        color: uc.color,
        got_nick_color: uc.got_nick_color,
        nick_color: uc.nick_color,
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
        hx_conn_set_uid(htlc, uc.uid);
        debug_trace(
            c"login",
            format!(
                "adopted self uid={} from USER_CHANGE broadcast (SELFINFO didn't carry it)",
                uc.uid
            ),
        );
    }

    // uc.name as a C string (first-NUL truncated, matching the old extractor).
    let name_c = cstring_first_nul(&uc.name);

    let emitted = hx_user_apply_recv(
        htlc,
        chat,
        model,
        uc.uid,
        plan.eff_nick_color,
        name_c.as_ptr(),
        uc.icon,
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
            uc.cid,
            HX_USER_NOTICE_JOIN,
            name_c.as_ptr(),
            std::ptr::null(),
        );
    } else {
        // HX_USER_CHANGE_CHANGED. Bail on ignored users before the notice.
        if hx_member_model_get_ignore(model, uc.uid) != 0 {
            return;
        }
        if plan.do_rename_notice {
            // old.name is the pre-change snapshot taken above.
            gtkhx_session_emit_user_notice(
                gtkhx_session_get_default(),
                htlc,
                uc.cid,
                HX_USER_NOTICE_RENAME,
                name_c.as_ptr(),
                old.name.as_ptr(),
            );
        }
    }

    // Self bookkeeping — mirror the just-applied wire/plan values into htlc.
    // (A new-self returned early via SKIPPED, so a self change here is always an
    // existing member.) The name is deliberately not copied back (see above).
    if uc.uid != 0 && uc.uid == hx_conn_uid(htlc.cast()) {
        let icon = if uc.icon != 0 {
            uc.icon
        } else if old_exists {
            old.icon
        } else {
            hx_conn_icon(htlc.cast())
        };
        hx_conn_set_icon(htlc, icon);
        if uc.got_nick_color {
            hx_conn_set_nick_color(htlc, uc.nick_color);
        }
        debug_trace(
            c"name",
            format!(
                "USER_CHANGE for our uid={}: keeping local htlc->name",
                uc.uid
            ),
        );
    }
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

/// `void hx_rcv_user_part (htlc, frame, frame_len)` — the USER_PART
/// (`HTLS_HDR_USER_PART`) receive handler.
///
/// Parses the frame natively (`parse_user_part` → uid + cid), resolves the chat,
/// captures the leaving member's name *before* the emit (the `user-delete`
/// fan-out removes the model entry), and delegates the membership-gated emit to
/// [`hx_user_part_recv`]. When that emitted, it logs the "parts: <name>" line —
/// which the C shim suppresses unless the showjoin pref is on.
///
/// # Safety
/// `frame` is valid for `frame_len` bytes; `htlc` is the opaque connection.
#[no_mangle]
pub unsafe extern "C" fn hx_rcv_user_part(htlc: *mut c_void, frame: *const u8, frame_len: usize) {
    if frame.is_null() {
        return;
    }
    let buf = std::slice::from_raw_parts(frame, frame_len);
    let pm = hotline_proto::parse::parse_user_part(buf, frame_len);

    let sess = hx_conn_sess(htlc.cast());
    let chat = chat_with_cid(sess, pm.cid);
    if chat.is_null() {
        return;
    }
    let model = hx_chat_member_model(chat.cast());

    // Snapshot the member before the emit removes it, so we have the name for
    // the "parts" line.
    let mut info = unsafe { std::mem::zeroed::<HxMemberInfo>() };
    let have = hx_member_model_get_info(model, pm.uid, &mut info) != 0;

    if hx_user_part_recv(htlc, chat, model, pm.uid) != 0 && have {
        gtkhx_session_emit_user_notice(
            gtkhx_session_get_default(),
            htlc,
            pm.cid,
            HX_USER_NOTICE_PART,
            info.name.as_ptr(),
            std::ptr::null(),
        );
    }
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
    if frame.is_null() {
        return;
    }
    hx_selfinfo_parse(htlc, frame, frame_len);
    hx_conn_set_logged_in(htlc, 1);
    hx_selfinfo_recv(htlc);
}

/// `void rcv_task_user_list (htlc, frame, frame_len, chat, text)` — the bulk
/// USER_LIST login-load reply (was `rcv.c`). Walks the reply's chunks natively:
/// each `HTLS_DATA_USER_LIST` record parses via [`parse_user_list_record`]
/// (8-byte fixed header, two-stage nlen clamp, `strip_ansi`, the
/// Colored-Nicknames trailer) and folds into the roster through the shared
/// [`hx_user_apply_recv`] with `incremental=FALSE` — the join chime is
/// suppressed because these users are already in the room at login. A
/// `HTLS_DATA_CHAT_SUBJECT` chunk seeds the chat's subject and publishes it via
/// the initial-subject-discovery emit ([`hx_chat_subject_emit`], no "Subject
/// Changed to" line).
///
/// Two self-bookkeeping gates from the old C move here: the Colored-Nicknames
/// self-mirror (copy the trailer colour onto `htlc` when the record is us), and
/// the self-uid adoption for servers that omit USER_LIST from SELFINFO (the first
/// record matching our nick+icon claims our uid). `text` is vestigial.
///
/// # Safety
/// C-ABI reply callback (`hx_rcv_task`, main thread). `frame` is valid for
/// `frame_len` bytes; `chat` is the reply's target `struct chat *` (task ptr).
#[no_mangle]
pub unsafe extern "C" fn rcv_task_user_list(
    htlc: *mut c_void,
    frame: *const u8,
    frame_len: usize,
    chat: *mut c_void,
    _text: *mut c_void,
) {
    if frame.is_null() {
        return;
    }
    let buf = std::slice::from_raw_parts(frame, frame_len);
    let model = hx_chat_member_model(chat.cast());
    for chunk in ChunkIter::over_message(buf, frame_len) {
        match chunk.tag {
            HTLS_DATA_USER_LIST => {
                let Some(rec) = parse_user_list_record(chunk.data, 31) else {
                    continue;
                };
                let name_c = cstring_first_nul(&rec.name);
                // "not already in this chat's membership" — recomputed per record
                // so a stale new=1 doesn't spawn spurious creates for later users.
                let is_new = hx_member_model_contains(model, rec.uid) == 0;
                // Colored-Nicknames: mirror the trailer colour onto htlc when this
                // record is us (absent trailer => leave htlc's colour alone).
                if let Some(nc) = rec.nick_color {
                    if rec.uid == hx_conn_uid(htlc) {
                        hx_conn_set_nick_color(htlc, nc);
                    }
                }
                // Self-adoption for servers that omit USER_LIST from SELFINFO: the
                // first record matching our nick+icon claims our uid.
                if hx_conn_uid(htlc) == 0 && rec.icon == hx_conn_icon(htlc) {
                    if let Some(sn) = optr_bytes(hx_conn_name(htlc.cast())) {
                        if name_c.as_bytes() == sn.as_slice() {
                            hx_conn_set_uid(htlc, rec.uid);
                        }
                    }
                }
                hx_user_apply_recv(
                    htlc,
                    chat,
                    model,
                    rec.uid,
                    rec.nick_color.unwrap_or(HX_NICK_COLOR_NONE),
                    name_c.as_ptr(),
                    rec.icon,
                    rec.color,
                    gbool(is_new),
                    gbool(false), // skip_self_create = FALSE
                    gbool(false), // incremental = FALSE (bulk login load)
                );
            }
            HTLS_DATA_CHAT_SUBJECT => {
                let slen = chunk.data.len().min(255);
                hx_chat_set_subject(chat.cast(), chunk.data.as_ptr() as *const c_char, slen);
                hx_chat_subject_emit(htlc, hx_chat_cid(chat.cast()), hx_chat_subject(chat.cast()));
            }
            _ => {}
        }
    }
}

/// `void rcv_task_user_list_switch (htlc, frame, frame_len, chat, data)` — the
/// USER_LIST reply for a *join* (channel switch). On a task error the join
/// failed, so drop the half-created chat; otherwise it's a normal user-list load.
///
/// # Safety
/// See [`rcv_task_user_list`]. `chat` is the joined `struct chat *`.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_user_list_switch(
    htlc: *mut c_void,
    frame: *const u8,
    frame_len: usize,
    chat: *mut c_void,
    _data: *mut c_void,
) {
    if task_inerror(htlc, frame, frame_len) != 0 {
        chat_delete(hx_conn_sess(htlc.cast()), chat);
        return;
    }
    rcv_task_user_list(htlc, frame, frame_len, chat, std::ptr::null_mut());
}

/// `void rcv_task_news_users (htlc, frame, frame_len, chat, text)` — the
/// post-login USER_GETLIST reply: load the user list, then kick off the news
/// fetch. Login-path only.
///
/// # Safety
/// See [`rcv_task_user_list`].
#[no_mangle]
pub unsafe extern "C" fn rcv_task_news_users(
    htlc: *mut c_void,
    frame: *const u8,
    frame_len: usize,
    chat: *mut c_void,
    text: *mut c_void,
) {
    rcv_task_user_list(htlc, frame, frame_len, chat, text);
    reload_news(std::ptr::null_mut(), hx_conn_sess(htlc.cast()));
}

/// `void rcv_task_user_info (htlc, frame, frame_len, uid_ptr, text)` — the
/// USER_GETINFO reply. Parses the (name, info) pair natively ([`parse_user_info`],
/// 31 / 4096 caps, `strip_ansi` + CR2LF), then — when both are non-empty (the
/// `nlen && ilen` gate that filters unanswered server frames) — publishes via
/// [`hx_user_info_recv`]. `uid_ptr` is a `g_malloc`'d `guint16` (the request's
/// uid, which the reply doesn't echo); it's freed here.
///
/// # Safety
/// C-ABI reply callback. `frame` is valid for `frame_len` bytes; `uid_ptr` is a
/// live `guint16 *` from the send wrapper (freed here).
#[no_mangle]
pub unsafe extern "C" fn rcv_task_user_info(
    _htlc: *mut c_void,
    frame: *const u8,
    frame_len: usize,
    uid_ptr: *mut c_void,
    _text: *mut c_void,
) {
    // uid is the request's `g_malloc`'d task parameter (the reply doesn't echo
    // it). A NULL here would be a caller bug — return without emitting rather
    // than publish a bogus uid=0 user-info event. Nothing to free on that path.
    if uid_ptr.is_null() {
        return;
    }
    let uid = *(uid_ptr as *const u16);
    g_free(uid_ptr);
    if frame.is_null() {
        return;
    }
    let buf = std::slice::from_raw_parts(frame, frame_len);
    let ui = parse_user_info(buf, frame_len, 31, 4096);
    if ui.name.is_empty() || ui.info.is_empty() {
        return;
    }
    let name_c = cstring_first_nul(&ui.name);
    let info_c = cstring_first_nul(&ui.info);
    // Pass the *truncated* length, not `ui.info.len()`: `cstring_first_nul`
    // truncates the body at the first interior NUL, so the emitted len must match
    // `info_c`'s buffer. Passing the full wire length would let a downstream
    // length-aware reader run past the shorter allocation.
    let info_len = info_c.as_bytes().len() as u16;
    hx_user_info_recv(uid, name_c.as_ptr(), info_c.as_ptr(), info_len);
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
        /// get_info: the member snapshot to return (name + fields), or None
        /// (absent → get_info returns FALSE).
        pub static MEMBER: RefCell<Option<MemberSnap>> = const { RefCell::new(None) };
        /// The last emitted user-notice, as (cid, kind, name, old_name).
        pub static NOTICE: RefCell<Option<Notice>> = const { RefCell::new(None) };
        /// task_inerror return.
        pub static TASK_ERROR: Cell<bool> = const { Cell::new(false) };
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
        /// The last (cid, subject) the initial-subject-discovery emit saw.
        pub static SUBJECT_EMITTED: RefCell<Option<(u32, Vec<u8>)>> = const { RefCell::new(None) };
        /// The cid hx_chat_cid returns.
        pub static CHAT_CID: Cell<u32> = const { Cell::new(0) };
        /// True once reload_news fired (rcv_task_news_users).
        pub static RELOAD_NEWS: Cell<bool> = const { Cell::new(false) };
        /// True once chat_delete fired (rcv_task_user_list_switch error path).
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
        SELFINFO_PARSED.with(|c| c.set(false));
        LOGGED_IN.with(|c| c.set(-1));
        MEMBER.with(|c| *c.borrow_mut() = None);
        NOTICE.with(|c| *c.borrow_mut() = None);
        TASK_ERROR.with(|c| c.set(false));
        SELF_UID.with(|c| c.set(0));
        SELF_ICON.with(|c| c.set(0));
        SELF_NICK_COLOR.with(|c| c.set(0));
        IGNORE.with(|c| c.set(false));
        SELF_NAME.with(|c| *c.borrow_mut() = std::ffi::CString::new("").unwrap());
        SUBJECT_STORE.with(|c| *c.borrow_mut() = std::ffi::CString::new("").unwrap());
        SUBJECT_EMITTED.with(|c| *c.borrow_mut() = None);
        CHAT_CID.with(|c| c.set(0));
        RELOAD_NEWS.with(|c| c.set(false));
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
    FAKE_CHAT
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
unsafe fn task_inerror(_htlc: *mut c_void, _frame: *const u8, _frame_len: usize) -> c_int {
    c_int::from(test_env::TASK_ERROR.with(|c| c.get()))
}

#[cfg(test)]
unsafe fn chat_new(_sess: *mut c_void, _cid: u32) -> *mut c_void {
    FAKE_CHAT
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
unsafe fn hx_chat_subject_emit(_htlc: *mut c_void, cid: u32, subject: *const c_char) {
    let b = cstr_bytes(subject);
    test_env::SUBJECT_EMITTED.with(|c| *c.borrow_mut() = Some((cid, b)));
}

#[cfg(test)]
unsafe fn chat_delete(_sess: *mut c_void, _chat: *mut c_void) {
    test_env::CHAT_DELETED.with(|c| c.set(true));
}

#[cfg(test)]
unsafe fn reload_news(_widget: *mut c_void, _data: *mut c_void) {
    test_env::RELOAD_NEWS.with(|c| c.set(true));
}

#[cfg(test)]
unsafe fn g_free(_p: *mut c_void) {}

#[cfg(test)]
mod tests;
