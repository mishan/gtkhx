//! `chat_members.rs` — the C ABI over the M2 `HxMemberModel` (membership data)
//! + the M1 nick-completion (`crate::chat::complete`).
//!
//! M2 wire-up (Option A). Every chat (public + private) owns an authoritative
//! `HxMemberModel` (created in `chat_new`, held on `struct chat::member_model`),
//! fed by the `user_create` / `_delete` / `_change` / `users_clear` fan-out in
//! `users.c` — which fires on every wire event regardless of whether a
//! user-list view is open, so the model is reliably populated even when no view
//! exists. `chat.c::tab_nick_comp` reads the model for the chat an input
//! belongs to through [`hx_nick_complete`], retiring ~250 lines of C buffer
//! manipulation in favour of the tested Rust `complete` (and completing
//! private-chat inputs against their own participants, not the public list).
//!
//! The model is a C-owned GObject (like the voice runtime): `new` transfers a
//! ref out, `free` takes it back. The rendered user list keeps its own
//! `HxUserRow` store (the C snapshot cell + selection need `hx_user*`); this
//! model is the *data* source of truth for consumers.

use std::ffi::{c_char, c_void, CStr};

use gio;
use glib;
use gio::prelude::*;
use glib::translate::{from_glib_full, from_glib_none, IntoGlibPtr};

use crate::chat::{complete_styled, InputHistory, Member};
use crate::member::{HxMember, HxMemberModel};

/// `HX_NICK_COLOR_NONE` (hotline.h).
const HX_NICK_COLOR_NONE: u32 = 0xFFFF_FFFF;

/// Borrow the `HxMemberModel` from a C-held `GObject *` (adds + drops a ref for
/// the call). NULL / wrong-type → None.
unsafe fn model_ref(ptr: *mut c_void) -> Option<HxMemberModel> {
    if ptr.is_null() {
        return None;
    }
    let obj: glib::Object = from_glib_none(ptr as *mut glib::gobject_ffi::GObject);
    obj.downcast::<HxMemberModel>().ok()
}

/// Copy `s` into a `g_malloc`'d NUL-terminated buffer (caller `g_free`s).
unsafe fn g_dup(s: &str) -> *mut c_char {
    glib::ffi::g_strndup(s.as_ptr() as *const c_char, s.len())
}

/// `void *hx_member_model_new(void)` — a fresh membership model (transfer full;
/// caller `hx_member_model_free`s).
#[no_mangle]
pub extern "C" fn hx_member_model_new() -> *mut c_void {
    let ptr: *mut glib::gobject_ffi::GObject =
        HxMemberModel::new().upcast::<glib::Object>().into_glib_ptr();
    ptr as *mut c_void
}

/// `void hx_member_model_free(void *model)` — drop the C-held ref.
///
/// # Safety
/// `model` is NULL or a `GObject *` previously returned by `hx_member_model_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_free(model: *mut c_void) {
    if !model.is_null() {
        let _: glib::Object = from_glib_full(model as *mut glib::gobject_ffi::GObject);
    }
}

/// `void hx_member_model_upsert(void *model, guint16 uid, const char *name,
/// guint16 icon, guint16 status, guint32 nick_color)` — insert/update a member.
///
/// # Safety
/// `model` valid or NULL; `name` NULL or a C string.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_upsert(
    model: *mut c_void,
    uid: u16,
    name: *const c_char,
    icon: u16,
    status: u16,
    nick_color: u32,
) {
    let Some(model) = model_ref(model) else {
        return;
    };
    let name = if name.is_null() {
        String::new()
    } else {
        CStr::from_ptr(name).to_string_lossy().into_owned()
    };
    let nick_color = (nick_color != HX_NICK_COLOR_NONE).then_some(nick_color);
    model.upsert(&Member {
        uid,
        icon,
        status,
        nick_color,
        name,
        ignore: false,
    });
}

/// `void hx_member_model_remove(void *model, guint16 uid)`.
///
/// # Safety
/// `model` valid or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_remove(model: *mut c_void, uid: u16) {
    if let Some(model) = model_ref(model) {
        model.remove(uid);
    }
}

/// `void hx_member_model_clear(void *model)`.
///
/// # Safety
/// `model` valid or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_clear(model: *mut c_void) {
    if let Some(model) = model_ref(model) {
        model.clear();
    }
}

/// `void hx_member_model_set_ignore(void *model, guint16 uid, gboolean ignore)`
/// — set the client-local ignore flag on `uid` (M4b.4a; the model is now the
/// authoritative store for it, not `hx_user::ignore`). No-op if absent.
///
/// # Safety
/// `model` valid or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_set_ignore(
    model: *mut c_void,
    uid: u16,
    ignore: glib::ffi::gboolean,
) {
    if let Some(model) = model_ref(model) {
        model.set_ignore(uid, ignore != glib::ffi::GFALSE);
    }
}

/// `gboolean hx_member_model_get_ignore(void *model, guint16 uid)` — the
/// ignore flag for `uid`, or FALSE when absent / NULL model.
///
/// # Safety
/// `model` valid or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_get_ignore(model: *mut c_void, uid: u16) -> glib::ffi::gboolean {
    match model_ref(model) {
        Some(model) if model.get_ignore(uid) => glib::ffi::GTRUE,
        _ => glib::ffi::GFALSE,
    }
}

/// `gboolean hx_member_model_toggle_ignore(void *model, guint16 uid)` — flip
/// `uid`'s ignore flag and return the new state (FALSE and no change if
/// absent / NULL model).
///
/// # Safety
/// `model` valid or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_toggle_ignore(
    model: *mut c_void,
    uid: u16,
) -> glib::ffi::gboolean {
    match model_ref(model) {
        Some(model) if model.toggle_ignore(uid) => glib::ffi::GTRUE,
        _ => glib::ffi::GFALSE,
    }
}

/// `#[repr(C)]` fill-out struct for [`hx_member_model_get_info`] — mirrors
/// `struct hx_member_info` in `chat_members.h`. `status` is the Admin/Guest/
/// Away bitmap (the field C called `hx_user::color`); `name` is NUL-terminated
/// (32-byte buffer, matching the old `hx_user::name[32]`).
#[repr(C)]
pub struct HxMemberInfo {
    pub uid: u16,
    pub icon: u16,
    pub status: u16,
    pub nick_color: u32,
    pub name: [c_char; 32],
}

// Pin the layout against the C `struct hx_member_info` (chat_members.h) —
// Rust writes it, C reads it, so a drift would be a silent memory bug.
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(offset_of!(HxMemberInfo, uid) == 0);
    assert!(offset_of!(HxMemberInfo, icon) == 2);
    assert!(offset_of!(HxMemberInfo, status) == 4);
    assert!(offset_of!(HxMemberInfo, nick_color) == 8);
    assert!(offset_of!(HxMemberInfo, name) == 12);
    assert!(size_of::<HxMemberInfo>() == 44);
};

/// Copy an `HxMember`'s display fields into a C `HxMemberInfo` (name truncated
/// + NUL-terminated into the fixed 32-byte buffer, matching the old
/// `hx_user::name[32]`).
fn fill_member_info(m: &HxMember, o: &mut HxMemberInfo) {
    o.uid = m.uid();
    o.icon = m.icon();
    o.status = m.status();
    o.nick_color = m.nick_color().unwrap_or(HX_NICK_COLOR_NONE);
    m.with_name(|n| {
        let bytes = n.as_bytes();
        let cap = o.name.len() - 1; // reserve the NUL
        let k = bytes.len().min(cap);
        for (i, &b) in bytes[..k].iter().enumerate() {
            o.name[i] = b as c_char;
        }
        o.name[k] = 0;
    });
}

/// `gboolean hx_member_model_get_info(void *model, guint16 uid,
/// struct hx_member_info *out)` — copy the member's display fields into `out`.
/// Returns FALSE (out untouched) when the member is absent / args NULL. The
/// C read path that used to `hx_user_with_uid(...)->field` (M4b.4b-ii).
///
/// # Safety
/// `model` valid or NULL; `out` NULL or a valid `struct hx_member_info *`.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_get_info(
    model: *mut c_void,
    uid: u16,
    out: *mut HxMemberInfo,
) -> glib::ffi::gboolean {
    if out.is_null() {
        return glib::ffi::GFALSE;
    }
    let Some(model) = model_ref(model) else {
        return glib::ffi::GFALSE;
    };
    let Some(m) = model.get(uid) else {
        return glib::ffi::GFALSE;
    };
    fill_member_info(&m, &mut *out);
    glib::ffi::GTRUE
}

/// `guint16 hx_member_model_find_by_name(void *model, const char *name)` — the
/// uid of the member whose name matches `name` exactly, or 0 if none (the C
/// `hx_user_with_name` linear scan; one caller, `/msg <name>`).
///
/// # Safety
/// `model` valid or NULL; `name` NULL or a C string.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_find_by_name(
    model: *mut c_void,
    name: *const c_char,
) -> u16 {
    if name.is_null() {
        return 0;
    }
    let Some(model) = model_ref(model) else {
        return 0;
    };
    let target = CStr::from_ptr(name).to_string_lossy();
    let n = model.n_items();
    for i in 0..n {
        if let Some(m) = model.item(i).and_then(|o| o.downcast::<HxMember>().ok()) {
            if m.with_name(|nm| nm == target) {
                return m.uid();
            }
        }
    }
    0
}

/// `guint hx_member_model_count(void *model)` — the member count (0 if NULL).
///
/// # Safety
/// `model` valid or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_count(model: *mut c_void) -> u32 {
    model_ref(model).map(|m| m.n_items()).unwrap_or(0)
}

/// `gboolean hx_member_model_contains(void *model, guint16 uid)` — whether
/// `uid` is a member (FALSE if absent / NULL).
///
/// # Safety
/// `model` valid or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_contains(model: *mut c_void, uid: u16) -> glib::ffi::gboolean {
    match model_ref(model) {
        Some(model) if model.get(uid).is_some() => glib::ffi::GTRUE,
        _ => glib::ffi::GFALSE,
    }
}

/// `gboolean hx_member_model_get_at(void *model, guint index,
/// struct hx_member_info *out)` — copy the member at insertion `index` into
/// `out` (for a full walk, `count` + `get_at`; the `user_list` repopulate).
/// FALSE if out-of-range / NULL args.
///
/// # Safety
/// `model` valid or NULL; `out` NULL or a valid `struct hx_member_info *`.
#[no_mangle]
pub unsafe extern "C" fn hx_member_model_get_at(
    model: *mut c_void,
    index: u32,
    out: *mut HxMemberInfo,
) -> glib::ffi::gboolean {
    if out.is_null() {
        return glib::ffi::GFALSE;
    }
    let Some(model) = model_ref(model) else {
        return glib::ffi::GFALSE;
    };
    let Some(m) = model.item(index).and_then(|o| o.downcast::<HxMember>().ok()) else {
        return glib::ffi::GFALSE;
    };
    fill_member_info(&m, &mut *out);
    glib::ffi::GTRUE
}

/// `gboolean hx_nick_complete(void *model, const char *input, gsize cursor,
/// gboolean reverse, gunichar suffix, char **out_text, int *out_cursor,
/// char **out_info)` — the tested M1 nick completion over the model's members.
///
/// On a result, returns TRUE and fills: `out_text` (a `g_malloc`'d new buffer
/// for the whole input), `out_cursor` (char offset, or -1 for end), and
/// `out_info` (a `g_malloc`'d space-joined candidate list to echo, or NULL).
/// Caller `g_free`s `*out_text` / `*out_info`. Returns FALSE (outputs
/// untouched) when there's nothing to complete.
///
/// # Safety
/// `model` valid or NULL; `input` a C string; the out-pointers non-NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_nick_complete(
    model: *mut c_void,
    input: *const c_char,
    cursor: usize,
    reverse: glib::ffi::gboolean,
    suffix: u32,
    old_style: glib::ffi::gboolean,
    out_text: *mut *mut c_char,
    out_cursor: *mut i32,
    out_info: *mut *mut c_char,
) -> glib::ffi::gboolean {
    // Every success writes through all three out-pointers — refuse a caller
    // that passed a NULL one rather than segfaulting inside Rust.
    if out_text.is_null() || out_cursor.is_null() || out_info.is_null() {
        return glib::ffi::GFALSE;
    }
    let (Some(model), false) = (model_ref(model), input.is_null()) else {
        return glib::ffi::GFALSE;
    };
    let input = CStr::from_ptr(input).to_string_lossy();

    // Collect member names (case-insensitively sorted — the completion's
    // deterministic walk order, matching MemberList::names_sorted).
    let n = model.n_items();
    let mut names: Vec<String> = Vec::with_capacity(n as usize);
    for i in 0..n {
        if let Some(m) = model.item(i).and_then(|o| o.downcast::<HxMember>().ok()) {
            names.push(m.name());
        }
    }
    // sort_by_cached_key lowercases each name once (vs. sort_by_key, which
    // re-allocates the key on every comparison) — cheaper for large lists.
    names.sort_by_cached_key(|s| s.to_ascii_lowercase());
    let name_refs: Vec<&str> = names.iter().map(|s| s.as_str()).collect();

    let suffix = char::from_u32(suffix).unwrap_or(':');
    match complete_styled(
        &name_refs,
        &input,
        cursor,
        reverse != glib::ffi::GFALSE,
        suffix,
        old_style != glib::ffi::GFALSE,
    ) {
        Some(c) => {
            *out_text = g_dup(&c.text);
            // Clamp before the narrowing cast: a raw `as i32` on a >2^31 char
            // offset would wrap to a bogus (possibly negative) value. Real
            // inputs never approach this, but keep the FFI offset well-defined.
            *out_cursor = c
                .cursor
                .map(|x| x.min(i32::MAX as usize) as i32)
                .unwrap_or(-1);
            *out_info = if c.info.is_empty() {
                std::ptr::null_mut()
            } else {
                g_dup(&c.info.join(" "))
            };
            glib::ffi::GTRUE
        }
        None => glib::ffi::GFALSE,
    }
}

// ---- chat input line history (InputHistory) ------------------------------
//
// The Rust replacement for gchat->chat_history (a GNU-readline HISTORY) +
// chat_history_draft. Owned by C as an opaque Box<InputHistory> pointer:
// `new` boxes it, `free` drops it. Drives chat_input_key_pressed's
// Return / Up / Down.

/// `void *hx_input_history_new(void)`.
#[no_mangle]
pub extern "C" fn hx_input_history_new() -> *mut c_void {
    Box::into_raw(Box::new(InputHistory::new())) as *mut c_void
}

/// `void hx_input_history_free(void *hist)`.
///
/// # Safety
/// `hist` is NULL or a pointer from `hx_input_history_new`.
#[no_mangle]
pub unsafe extern "C" fn hx_input_history_free(hist: *mut c_void) {
    if !hist.is_null() {
        drop(Box::from_raw(hist as *mut InputHistory));
    }
}

/// `void hx_input_history_record(void *hist, const char *line)` — record a
/// just-sent line and reset navigation to the bottom.
///
/// # Safety
/// `hist` valid or NULL; `line` a C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_input_history_record(hist: *mut c_void, line: *const c_char) {
    if hist.is_null() {
        return;
    }
    let hist = &mut *(hist as *mut InputHistory);
    let line = if line.is_null() {
        std::borrow::Cow::Borrowed("")
    } else {
        CStr::from_ptr(line).to_string_lossy()
    };
    hist.record(&line);
}

/// `gboolean hx_input_history_up(void *hist, const char *current, char **out)`
/// — Up arrow. `current` is the live buffer text (snapshotted as the draft on
/// the first press). On a change returns TRUE + `*out` (a `g_malloc`'d line to
/// show; caller `g_free`s); FALSE (out untouched) when there's nothing older.
///
/// # Safety
/// `hist` valid or NULL; `current` a C string or NULL; `out` non-NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_input_history_up(
    hist: *mut c_void,
    current: *const c_char,
    out: *mut *mut c_char,
) -> glib::ffi::gboolean {
    if hist.is_null() || out.is_null() {
        return glib::ffi::GFALSE;
    }
    let hist = &mut *(hist as *mut InputHistory);
    let current = if current.is_null() {
        std::borrow::Cow::Borrowed("")
    } else {
        CStr::from_ptr(current).to_string_lossy()
    };
    match hist.up(&current) {
        Some(text) => {
            *out = g_dup(&text);
            glib::ffi::GTRUE
        }
        None => glib::ffi::GFALSE,
    }
}

/// `gboolean hx_input_history_down(void *hist, char **out)` — Down arrow. On a
/// change returns TRUE + `*out` (the line to show, possibly the restored
/// draft); FALSE when already at the draft.
///
/// # Safety
/// `hist` valid or NULL; `out` non-NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_input_history_down(
    hist: *mut c_void,
    out: *mut *mut c_char,
) -> glib::ffi::gboolean {
    if hist.is_null() || out.is_null() {
        return glib::ffi::GFALSE;
    }
    let hist = &mut *(hist as *mut InputHistory);
    match hist.down() {
        Some(text) => {
            *out = g_dup(&text);
            glib::ffi::GTRUE
        }
        None => glib::ffi::GFALSE,
    }
}
