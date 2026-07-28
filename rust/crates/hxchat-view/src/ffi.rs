//! The C ABI.
//!
//! Mirrors `src/chat_view.h`, one `hx_chat_view_impl_*` per
//! `hx_chat_view_*`. The C side (`chat_view.c`) is the dispatcher: it
//! decides at construction which backend a view is, then routes every
//! later call by widget type. That is why these are `_impl_`-prefixed
//! rather than simply defining the public names — both backends have to
//! coexist in one binary until C5.
//!
//! **Marks.** `chat_view.h`'s `HxChatMark *` is an opaque handle, and
//! here it is a [`MessageId`] rather than a pointer, encoded the way
//! GLib encodes integer handles (`GSIZE_TO_POINTER`). Ids start at 1, so
//! `NULL` is unambiguously "no mark"; on a 32-bit host the id space caps
//! at 4 billion rows in one session, which a chat scrollback will not
//! reach. Nothing dereferences it — that is the entire point of the
//! opaque type, and the reason a stale mark here is inert where xtext's
//! raw `textentry *` would have dangled.

use crate::view::{HxChatView, PALETTE_COLS};
use gtk4::glib::translate::{FromGlibPtrNone, IntoGlib, ToGlibPtr};
use gtk4::prelude::*;
use hxchat_layout::{mirc, Block, Message, MessageId, MessageKind, ParsedText};
use std::ffi::{c_char, c_int, c_void, CStr};

type CGtkWidget = *mut gtk4::ffi::GtkWidget;

/// # Safety
/// `p` is NULL or a valid NUL-terminated C string.
unsafe fn cstr(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// # Safety
/// `p` points to `len` readable bytes, or is NULL.
unsafe fn cslice(p: *const c_char, len: c_int) -> String {
    if p.is_null() || len <= 0 {
        return String::new();
    }
    let bytes = std::slice::from_raw_parts(p as *const u8, len as usize);
    String::from_utf8_lossy(bytes).into_owned()
}

fn mark_to_ptr(id: MessageId) -> *mut c_void {
    id.0 as usize as *mut c_void
}

fn ptr_to_mark(p: *mut c_void) -> Option<MessageId> {
    let v = p as usize as u64;
    if v == 0 {
        None
    } else {
        Some(MessageId(v))
    }
}

/// # Safety
/// `w` is a valid `HxChatView *`.
unsafe fn view_of(w: CGtkWidget) -> Option<HxChatView> {
    if w.is_null() {
        return None;
    }
    let widget = gtk4::Widget::from_glib_none(w);
    widget.downcast::<HxChatView>().ok()
}

macro_rules! with_view {
    ($w:expr, $v:ident, $body:expr) => {{
        match view_of($w) {
            Some($v) => $body,
            None => Default::default(),
        }
    }};
}

// ---- construction / configuration ---------------------------------

/// # Safety
/// `palette` points to `HX_CHAT_PAL_COLS` `GdkRGBA`s, or is NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_new(
    palette: *const gtk4::gdk::ffi::GdkRGBA,
    separator: c_int,
) -> CGtkWidget {
    crate::ensure_gtk_init();
    let view = HxChatView::new();
    view.set_separator(separator != 0);
    if !palette.is_null() {
        let mut pal = [gtk4::gdk::RGBA::BLACK; PALETTE_COLS];
        let src = std::slice::from_raw_parts(palette, PALETTE_COLS);
        for (dst, s) in pal.iter_mut().zip(src.iter()) {
            *dst = gtk4::gdk::RGBA::new(s.red, s.green, s.blue, s.alpha);
        }
        view.set_palette(&pal);
    }
    // The caller owns the floating ref, exactly as gtk_xtext_new leaves
    // it, so the two backends hand back the same ownership.
    view.upcast::<gtk4::Widget>().to_glib_none().0
}

/// # Safety
/// See module docs; `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_get_type() -> gtk4::glib::ffi::GType {
    crate::ensure_gtk_init();
    use gtk4::glib::prelude::StaticType;
    HxChatView::static_type().into_glib()
}

/// # Safety
/// `w` is a valid `HxChatView *`; `font` a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_font(w: CGtkWidget, font: *const c_char) {
    with_view!(w, v, {
        let f = cstr(font);
        if !f.is_empty() {
            v.set_font_from_string(&f);
        }
    })
}

/// # Safety
/// `palette` points to `HX_CHAT_PAL_COLS` `GdkRGBA`s.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_palette(
    w: CGtkWidget,
    palette: *const gtk4::gdk::ffi::GdkRGBA,
) {
    if palette.is_null() {
        return;
    }
    with_view!(w, v, {
        let mut pal = [gtk4::gdk::RGBA::BLACK; PALETTE_COLS];
        let src = std::slice::from_raw_parts(palette, PALETTE_COLS);
        for (dst, s) in pal.iter_mut().zip(src.iter()) {
            *dst = gtk4::gdk::RGBA::new(s.red, s.green, s.blue, s.alpha);
        }
        v.set_palette(&pal);
    })
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_word_wrap(w: CGtkWidget, on: c_int) {
    with_view!(w, v, v.set_word_wrap(on != 0))
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_max_lines(w: CGtkWidget, n: c_int) {
    with_view!(w, v, v.set_max_rows(n))
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_indent(w: CGtkWidget, on: c_int) {
    with_view!(w, v, v.set_indent(on != 0))
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_max_indent(w: CGtkWidget, px: c_int) {
    with_view!(w, v, v.set_max_indent(px))
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_time_stamp(w: CGtkWidget, _on: c_int) {
    // C4: the timestamp column is part of the gutter rework that lands
    // with the chat-history row kinds. Accepting and ignoring keeps the
    // dispatcher uniform; PM windows (C2's only surface) render the same
    // either way because chat.c puts the stamp in the gutter text.
    let _ = w;
}

/// # Safety
/// `w` is a valid `HxChatView *` or NULL; `fmt` a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_stamp_format(
    w: CGtkWidget,
    fmt: *const c_char,
) {
    let _ = (w, fmt); // C4, with the timestamp column.
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_urlcheck_function(
    w: CGtkWidget,
    f: *mut c_void,
) {
    // C3: link activation is part of the interaction phase, and it will
    // arrive as a typed `link-activated` signal rather than a
    // word-classifier callback (scoping §3.6). Accepted and dropped so
    // the dispatcher stays uniform.
    let _ = (w, f);
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_get_vadjustment(
    w: CGtkWidget,
) -> *mut gtk4::ffi::GtkAdjustment {
    match view_of(w) {
        Some(v) => {
            // Create one on demand if the view isn't in a
            // GtkScrolledWindow — chat.c packs a bare GtkScrollbar and
            // hands it this adjustment, exactly as it did with xtext.
            let adj = match v.vadjustment() {
                Some(a) => a,
                None => {
                    let a = gtk4::Adjustment::new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
                    v.set_vadjustment(Some(&a));
                    a
                }
            };
            adj.to_glib_none().0
        }
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_refresh(w: CGtkWidget) {
    with_view!(w, v, v.queue_draw())
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_clear(w: CGtkWidget) {
    with_view!(w, v, v.clear())
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_set_zoom_permille(w: CGtkWidget, zoom: c_int) {
    with_view!(w, v, v.set_zoom_permille(zoom.max(0) as u32))
}

// ---- appending -----------------------------------------------------

/// Build a message from the compat path's two mIRC strings.
///
/// The left column keeps its own styling via `Message::gutter` — see the
/// field's docs for why a bare `Speaker { nick }` can't reproduce what
/// `chat.c` emits.
fn compat_message(left: &str, right: &str, stamp: i64) -> Message {
    let gutter = if left.is_empty() {
        None
    } else {
        Some(mirc::parse(left))
    };
    Message {
        kind: MessageKind::Live,
        timestamp: stamp,
        speaker: None,
        gutter,
        blocks: vec![Block::Text(mirc::parse(right))],
        flags: hxchat_layout::MessageFlags::NONE,
    }
}

/// # Safety
/// `text` points to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_append(
    w: CGtkWidget,
    text: *const c_char,
    len: c_int,
    stamp: i64,
) {
    with_view!(w, v, {
        let body = cslice(text, len);
        v.append(Message {
            kind: MessageKind::Live,
            timestamp: stamp,
            speaker: None,
            gutter: None,
            blocks: vec![Block::Text(mirc::parse(&body))],
            flags: hxchat_layout::MessageFlags::NONE,
        });
    })
}

/// # Safety
/// `left` / `right` point to their respective readable byte counts.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_append_indent(
    w: CGtkWidget,
    left: *const c_char,
    left_len: c_int,
    right: *const c_char,
    right_len: c_int,
    stamp: i64,
) -> *mut c_void {
    match view_of(w) {
        Some(v) => {
            let l = cslice(left, left_len);
            let r = cslice(right, right_len);
            mark_to_ptr(v.append(compat_message(&l, &r, stamp)))
        }
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `left` / `right` point to their respective readable byte counts.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_insert_before(
    w: CGtkWidget,
    anchor: *mut c_void,
    left: *const c_char,
    left_len: c_int,
    right: *const c_char,
    right_len: c_int,
    stamp: i64,
) -> *mut c_void {
    match view_of(w) {
        Some(v) => {
            let l = cslice(left, left_len);
            let r = cslice(right, right_len);
            mark_to_ptr(v.insert_before(ptr_to_mark(anchor), compat_message(&l, &r, stamp)))
        }
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_remove(w: CGtkWidget, mark: *mut c_void) -> c_int {
    match (view_of(w), ptr_to_mark(mark)) {
        (Some(v), Some(id)) => c_int::from(v.remove(id)),
        _ => 0,
    }
}

// ---- inline media (C4) ---------------------------------------------

/// # Safety
/// `alt` is a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_append_media(
    w: CGtkWidget,
    _texture: *mut c_void,
    alt: *const c_char,
    token: u32,
    stamp: i64,
) {
    // C2 is text-only: the row appends as its placeholder, which is
    // precisely the Phase 9.D behaviour and is spec-conformant on its
    // own. C4 attaches the decoded size and it becomes a real
    // variable-height image block.
    with_view!(w, v, {
        let alt = cstr(alt);
        v.append(Message {
            kind: MessageKind::Live,
            timestamp: stamp,
            speaker: None,
            gutter: None,
            blocks: vec![Block::Image {
                token,
                size: None,
                alt: mirc::strip(&alt),
            }],
            flags: hxchat_layout::MessageFlags::NONE,
        });
    })
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_media_mark(w: CGtkWidget, token: u32) -> *mut c_void {
    match view_of(w) {
        Some(v) => v.find_image(token).map_or(std::ptr::null_mut(), mark_to_ptr),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_media_set_texture(
    w: CGtkWidget,
    mark: *mut c_void,
    texture: *mut c_void,
) {
    let _ = (w, mark, texture); // C4.
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_impl_media_set_animation(
    w: CGtkWidget,
    mark: *mut c_void,
    frames: *mut c_void,
) {
    let _ = (w, mark, frames); // C4.
}

/// Unused today; keeps `ParsedText` reachable for the C4 work without a
/// dead-import warning in the meantime.
#[allow(dead_code)]
fn _keep(_: ParsedText) {}
