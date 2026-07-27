//! `conversation.rs` — `HxConversation`, the per-chat protocol model that
//! replaces the C `struct chat`. It owns the chat id, the subject buffer, and
//! the authoritative `HxMemberModel` (created here, so a conversation is
//! complete the moment it exists), plus a non-owning pointer to the C view
//! window (`struct gtkhx_chat *`).
//!
//! Allocated as an opaque `Box`: the C side treats `struct chat *` as an opaque
//! handle and reaches every field through the accessors below. This retires the
//! last hand-written C model struct — membership already lives in
//! `HxMemberModel`, history / media / render-cursors are Rust or view-local.

use std::ffi::{c_char, c_void};

use gtk4::glib;
use glib::prelude::*;
use glib::translate::{from_glib_full, IntoGlibPtr};

use hxmodel::member::HxMemberModel;

use gtkhx_boxed::media_table::{hx_media_table_free, hx_media_table_new};

extern "C" {
    // hxchat-model — the Rust InputHistory (input line history), C ABI.
    fn hx_input_history_new() -> *mut c_void;
    fn hx_input_history_free(hist: *mut c_void);
}

/// The per-chat model. Box-allocated, so `subject`'s address is stable for the
/// handle's lifetime (the C side reads a pointer into it).
pub struct HxConversation {
    cid: u32,
    /// Chat topic — a NUL-terminated buffer mirroring the old `char[256]`.
    subject: [c_char; 256],
    /// Authoritative membership. Owned GObject ref (transfer-full); unref'd in
    /// `hx_conversation_free`.
    member_model: *mut c_void,
    /// Input line history (Rust `InputHistory`, hxchat-model). Owned. Created
    /// here so it's conversation-scoped — a pchat's typed history survives
    /// closing + reopening its window (it's chat state, not view state).
    chat_history: *mut c_void,
    /// Inline-media token table (Rust `MediaTable`, gtkhx-boxed). Owned. Tokens
    /// are monotonic, so re-rendering into a rebuilt view never collides.
    media_table: *mut c_void,
    /// The open window/view, or NULL. Non-owning: the C side (`gchat_free`)
    /// owns the `struct gtkhx_chat` and frees it in `chat_free` before us.
    view: *mut c_void,
}

/// `struct chat *hx_conversation_new(guint32 cid)` — a fresh conversation with
/// its own empty membership model. Transfer-full; free with
/// `hx_conversation_free`.
#[no_mangle]
pub extern "C" fn hx_conversation_new(cid: u32) -> *mut HxConversation {
    let mm: *mut glib::gobject_ffi::GObject =
        HxMemberModel::new().upcast::<glib::Object>().into_glib_ptr();
    let member_model = mm as *mut c_void;
    // SAFETY: hx_input_history_new / hx_media_table_new are the C-ABI
    // constructors from hxchat-model / gtkhx-boxed; each returns an owned handle.
    let (chat_history, media_table) =
        unsafe { (hx_input_history_new(), hx_media_table_new()) };
    Box::into_raw(Box::new(HxConversation {
        cid,
        subject: [0; 256],
        member_model,
        chat_history,
        media_table,
        view: std::ptr::null_mut(),
    }))
}

/// `void hx_conversation_free(struct chat *)` — drop the conversation and unref
/// its member model. The view is NOT freed here (the C side owns it and frees
/// it first in `chat_free`).
///
/// # Safety
/// `conv` is NULL or a handle from `hx_conversation_new`, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn hx_conversation_free(conv: *mut HxConversation) {
    if conv.is_null() {
        return;
    }
    let conv = Box::from_raw(conv);
    if !conv.member_model.is_null() {
        let _: glib::Object =
            from_glib_full(conv.member_model as *mut glib::gobject_ffi::GObject);
    }
    if !conv.chat_history.is_null() {
        hx_input_history_free(conv.chat_history);
    }
    if !conv.media_table.is_null() {
        hx_media_table_free(conv.media_table);
    }
}

/// `guint32 hx_chat_cid(struct chat *)`.
///
/// # Safety
/// `conv` is NULL or a live conversation handle.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_cid(conv: *const HxConversation) -> u32 {
    if conv.is_null() {
        0
    } else {
        (*conv).cid
    }
}

/// `const char *hx_chat_subject(struct chat *)` — pointer to the internal
/// NUL-terminated subject buffer (stable for the handle's lifetime; used
/// synchronously by the caller).
///
/// # Safety
/// `conv` is NULL or live.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_subject(conv: *const HxConversation) -> *const c_char {
    if conv.is_null() {
        c"".as_ptr()
    } else {
        (*conv).subject.as_ptr()
    }
}

/// `void hx_chat_set_subject(struct chat *, const char *s, gsize len)` — copy up
/// to 255 bytes of `s` into the subject buffer and NUL-terminate. `s == NULL`
/// (or `len == 0`) clears it.
///
/// # Safety
/// `conv` is NULL or live; `s` is NULL or points at `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_set_subject(
    conv: *mut HxConversation,
    s: *const c_char,
    len: usize,
) {
    if conv.is_null() {
        return;
    }
    let buf = &mut (*conv).subject;
    let n = if s.is_null() { 0 } else { len.min(buf.len() - 1) };
    if n > 0 {
        std::ptr::copy_nonoverlapping(s, buf.as_mut_ptr(), n);
    }
    buf[n] = 0;
}

/// `void *hx_chat_member_model(struct chat *)` — the authoritative
/// `HxMemberModel` (borrowed `GObject *`; do NOT unref).
///
/// # Safety
/// `conv` is NULL or live.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_member_model(conv: *const HxConversation) -> *mut c_void {
    if conv.is_null() {
        std::ptr::null_mut()
    } else {
        (*conv).member_model
    }
}

/// `void *hx_chat_input_history(struct chat *)` — the conversation's
/// `InputHistory` handle (borrowed; owned by the conversation).
///
/// # Safety
/// `conv` is NULL or live.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_input_history(conv: *const HxConversation) -> *mut c_void {
    if conv.is_null() {
        std::ptr::null_mut()
    } else {
        (*conv).chat_history
    }
}

/// `void *hx_chat_media_table(struct chat *)` — the conversation's `MediaTable`
/// handle (borrowed; owned by the conversation).
///
/// # Safety
/// `conv` is NULL or live.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_media_table(conv: *const HxConversation) -> *mut c_void {
    if conv.is_null() {
        std::ptr::null_mut()
    } else {
        (*conv).media_table
    }
}

/// `struct gtkhx_chat *hx_chat_view(struct chat *)` — the open view or NULL.
///
/// # Safety
/// `conv` is NULL or live.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view(conv: *const HxConversation) -> *mut c_void {
    if conv.is_null() {
        std::ptr::null_mut()
    } else {
        (*conv).view
    }
}

/// `void hx_chat_set_view(struct chat *, struct gtkhx_chat *)`.
///
/// # Safety
/// `conv` is NULL or live.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_set_view(conv: *mut HxConversation, view: *mut c_void) {
    if !conv.is_null() {
        (*conv).view = view;
    }
}
