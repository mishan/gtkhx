//! GtkHx's configuration of the Rotulus chat view.
//!
//! Rotulus knows nothing about Hotline; this is where GtkHx tells it what
//! it needs to know. Every chat, private-chat and private-message output
//! is built by [`gtkhx_chat_view_new`], which sets what never changes —
//! the `hotline://` scheme, the avatar resolver, the link handlers — and
//! then [`gtkhx_chat_view_configure`], which applies the chat
//! preferences and is called again whenever one of them changes.
//!
//! It is also the home of GtkHx's link detector. `gtkurl.c`'s C callers
//! (the news views' URL tagging) ask the same question the chat view does,
//! so they get the same answer from the same scheme list.

use gtk4 as gtk;
use gtk4::glib;
use gtk4::glib::translate::ToGlibPtr;
use gtk4::prelude::*;
use rotulus::RotulusView;
use rotulus_layout::Linkifier;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::LazyLock;

extern "C" {
    /// `chat_avatar.c` — the avatar or icon for a user id.
    fn hx_chat_avatar_for_key(
        view: *mut gtk::ffi::GtkWidget,
        key: u64,
        data: *mut c_void,
    ) -> *mut gtk::gdk::ffi::GdkPaintable;
    /// `gtkurl.c` — the URL menu, with Connect and Save Bookmark for a
    /// `hotline://` link. Its (x, y) are relative to the root.
    fn gtkurl_show_popup(anchor: *mut gtk::ffi::GtkWidget, url: *const c_char, x: f64, y: f64);
    /// `connect.c` — connect to the server a `hotline://` URL names.
    fn connect_open_hotline_url(url: *const c_char) -> glib::ffi::gboolean;
}

/// The schemes GtkHx links: the usual set, plus its own.
static LINKS: LazyLock<Linkifier> = LazyLock::new(|| {
    Linkifier::new(
        rotulus_layout::DEFAULT_SCHEMES
            .iter()
            .copied()
            .chain(["hotline://"]),
    )
});

/// Build a chat output: a Rotulus view, configured for GtkHx and handed
/// to C floating, like any GTK constructor's result.
///
/// # Safety
/// `palette` points to `ROTULUS_PAL_COLS` colours; `font` is a
/// NUL-terminated Pango font description, or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_view_new(
    palette: *const gtk::gdk::ffi::GdkRGBA,
    font: *const c_char,
) -> *mut gtk::ffi::GtkWidget {
    let raw = rotulus::ffi::rotulus_view_new();
    rotulus::ffi::rotulus_view_set_palette(raw, palette);
    if !font.is_null() {
        rotulus::ffi::rotulus_view_set_font(raw, font);
    }
    // A plain reference, not from_glib_none, which would sink the
    // floating one the caller is about to receive.
    let obj: glib::Object =
        glib::translate::from_glib_full(glib::gobject_ffi::g_object_ref(raw as *mut _));
    if let Ok(view) = obj.downcast::<RotulusView>() {
        setup(&view);
        configure(&view);
    }
    raw
}

/// What every GtkHx chat view has, whatever the preferences say.
fn setup(view: &RotulusView) {
    // Typing goes to the input box beside the view, never to the view.
    view.set_can_focus(false);
    view.set_indent(true);
    // Room for the timestamp and a medium-length nick without the column
    // dominating the chat width.
    view.set_max_indent(256);
    view.set_separator(true);
    view.set_group_gap_secs(rotulus_layout::buffer::DEFAULT_GROUP_GAP_SECS);
    let schemes: Vec<&str> = LINKS.schemes().iter().map(String::as_str).collect();
    view.set_link_schemes(&schemes);
    unsafe {
        rotulus::ffi::rotulus_view_set_avatar_func(
            view.upcast_ref::<gtk::Widget>().to_glib_none().0,
            Some(hx_chat_avatar_for_key),
            std::ptr::null_mut(),
            None,
        );
    }

    // A hotline:// link connects; anything else goes to the desktop.
    view.connect_closure(
        "link-activated",
        false,
        glib::closure_local!(|_: RotulusView, href: String| -> bool {
            if !href.to_ascii_lowercase().starts_with("hotline://") {
                return false;
            }
            let Ok(c) = CString::new(href) else {
                return true;
            };
            if unsafe { connect_open_hotline_url(c.as_ptr()) } == 0 {
                let msg = crate::cs(&crate::tr::tr("Couldn't parse hotline:// URL"));
                unsafe { crate::ffi::toolbar_show_toast(msg.as_ptr()) };
            }
            true
        }),
    );
    // Right-click on a link pops the menu every GtkHx surface shares.
    view.connect_closure(
        "link-menu",
        false,
        glib::closure_local!(|view: RotulusView, href: String, x: f64, y: f64| -> bool {
            let Some(root) = view.root() else {
                return false;
            };
            let root: gtk::Widget = root.upcast();
            let p = view
                .compute_point(&root, &gtk::graphene::Point::new(x as f32, y as f32))
                .unwrap_or_else(|| gtk::graphene::Point::new(x as f32, y as f32));
            let Ok(c) = CString::new(href) else {
                return false;
            };
            unsafe {
                gtkurl_show_popup(
                    view.upcast_ref::<gtk::Widget>().to_glib_none().0,
                    c.as_ptr(),
                    f64::from(p.x()),
                    f64::from(p.y()),
                );
            }
            true
        }),
    );
}

/// Apply the chat preferences.
fn configure(view: &RotulusView) {
    let Some(chat) = hxconfig::ffi::with_settings(|s| s.chat.clone()) else {
        return;
    };
    view.set_word_wrap(chat.word_wrap);
    view.set_max_rows(chat.scrollback_lines.min(i32::MAX as u32) as i32);
    view.set_time_stamp(chat.timestamp);
    view.set_stamp_format(&chat.timestamp_format);
    view.set_avatar_size(if chat.avatars { AVATAR_SIZE } else { 0 });
    view.set_markdown(chat.markdown);
    view.set_activate_links(chat.single_click_links);
    view.set_autocopy(chat.autocopy.text);
    view.set_copy_timestamps(chat.autocopy.timestamp);
}

/// `ROTULUS_AVATAR_SIZE_DEFAULT`.
const AVATAR_SIZE: u32 = 32;

/// Re-apply the chat preferences to a view `gtkhx_chat_view_new` built.
///
/// # Safety
/// `w` is NULL or a `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_view_configure(w: *mut gtk::ffi::GtkWidget) {
    if w.is_null() {
        return;
    }
    // A plain reference, not from_glib_none: that sinks a floating one.
    let obj: glib::Object =
        glib::translate::from_glib_full(glib::gobject_ffi::g_object_ref(w as *mut _));
    if let Ok(view) = obj.downcast::<RotulusView>() {
        configure(&view);
    }
}

// ---- gtkurl.h's detection half ---------------------------------------

/// # Safety
/// `p` is NULL or NUL-terminated.
unsafe fn word(p: *const c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    Some(CStr::from_ptr(p).to_string_lossy().into_owned())
}

/// # Safety
/// `w` is NULL or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn gtkurl_word_has_url_scheme(w: *const c_char) -> glib::ffi::gboolean {
    word(w).is_some_and(|w| LINKS.has_scheme(&w)).into()
}

/// # Safety
/// `w` is NULL or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn gtkurl_is_url(w: *const c_char) -> glib::ffi::gboolean {
    word(w).is_some_and(|w| LINKS.is_url(&w)).into()
}

/// # Safety
/// `w` is NULL or NUL-terminated. The result is freed with `g_free`.
#[no_mangle]
pub unsafe extern "C" fn gtkurl_normalize(w: *const c_char) -> *mut c_char {
    let out = word(w).map(|w| LINKS.normalize(&w)).unwrap_or_default();
    out.as_str().to_glib_full()
}

/// `gtkurl_match_cb`.
type MatchCb = unsafe extern "C" fn(*const c_char, c_int, c_int, *mut c_void);

/// # Safety
/// `text` points to `length` readable bytes (NUL-terminated when `length`
/// is negative); `cb` is called with `text`, a byte range, and `user`.
#[no_mangle]
pub unsafe extern "C" fn gtkurl_scan(
    text: *const c_char,
    length: isize,
    cb: Option<MatchCb>,
    user: *mut c_void,
) {
    let (false, Some(cb)) = (text.is_null(), cb) else {
        return;
    };
    let bytes = if length < 0 {
        CStr::from_ptr(text).to_bytes()
    } else {
        std::slice::from_raw_parts(text as *const u8, length as usize)
    };
    // Scan the valid prefix; a byte range into invalid UTF-8 would be a
    // range into text the caller can't display as a link anyway.
    let valid = match std::str::from_utf8(bytes) {
        Ok(s) => s,
        Err(e) => std::str::from_utf8(&bytes[..e.valid_up_to()]).unwrap_or(""),
    };
    for r in LINKS.scan(valid) {
        cb(text, r.start as c_int, r.end as c_int, user);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "C" fn collect(_: *const c_char, a: c_int, b: c_int, user: *mut c_void) {
        (*(user as *mut Vec<(c_int, c_int)>)).push((a, b));
    }

    #[test]
    fn gtkhx_links_hotline_urls_and_the_usual_ones() {
        let text = c"see hotline://hx.example and https://example.com.";
        let mut found: Vec<(c_int, c_int)> = Vec::new();
        unsafe {
            gtkurl_scan(
                text.as_ptr(),
                -1,
                Some(collect),
                &mut found as *mut _ as *mut c_void,
            )
        };
        let s = text.to_str().unwrap();
        let words: Vec<&str> = found
            .iter()
            .map(|&(a, b)| &s[a as usize..b as usize])
            .collect();
        assert_eq!(words, ["hotline://hx.example", "https://example.com"]);
    }

    #[test]
    fn words_classify_as_before() {
        unsafe {
            assert_ne!(gtkurl_is_url(c"hotline://hx.example".as_ptr()), 0);
            assert_ne!(gtkurl_is_url(c"someone@example.com".as_ptr()), 0);
            assert_eq!(
                gtkurl_word_has_url_scheme(c"someone@example.com".as_ptr()),
                0
            );
            assert_eq!(gtkurl_is_url(std::ptr::null()), 0);
            let n = gtkurl_normalize(c"www.example.com".as_ptr());
            assert_eq!(
                CStr::from_ptr(n).to_str().unwrap(),
                "https://www.example.com"
            );
            glib::ffi::g_free(n as *mut c_void);
        }
    }
}
