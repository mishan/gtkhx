//! `chat_members.rs` — the C ABI over the M2 `HxMemberModel` (membership data)
//! + the M1 nick-completion (`hxchat_model::complete`).
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

use gtk4::gio;
use gtk4::glib;
use gio::prelude::*;
use glib::translate::{from_glib_full, from_glib_none, IntoGlibPtr};

use hxchat_model::{complete_styled, Member};
use hxmember_model::{HxMember, HxMemberModel};

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
