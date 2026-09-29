//! Rotulus — a GTK4 scrollback view for text-stream chat.
//!
//! The GTK skin over [`rotulus_layout`]. All the layout logic lives in
//! that crate and is tested headless; this one owns the widget, the
//! Pango font backend, and the C ABI declared in `include/rotulus.h`.
//!
//! What it promises, and what it doesn't, is in docs/chat-view.md.
//!
//! # Translations
//!
//! The widget's few strings (its context menus) are looked up in the
//! `rotulus` gettext domain. An application that ships the catalog binds
//! it the usual way, `bindtextdomain("rotulus", localedir)`; one that
//! doesn't gets English.

mod a11y;
pub mod ffi;
pub mod links;
pub mod measure;
#[cfg(feature = "mirc")]
pub mod mirc_ffi;
pub mod view;

/// The gettext domain the widget's own strings live in.
pub const GETTEXT_DOMAIN: &str = "rotulus";

/// Translate `s` in the widget's own domain, falling back to the msgid
/// on any failure.
pub(crate) fn tr(s: &str) -> String {
    use std::ffi::{c_char, CStr, CString};
    extern "C" {
        fn dgettext(domain: *const c_char, msgid: *const c_char) -> *mut c_char;
    }
    let (Ok(domain), Ok(c)) = (CString::new(GETTEXT_DOMAIN), CString::new(s)) else {
        return s.to_owned();
    };
    unsafe {
        let p = dgettext(domain.as_ptr(), c.as_ptr());
        if p.is_null() {
            return s.to_owned();
        }
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// Tell gtk4-rs that GTK is already initialised.
///
/// An application written in C calls `gtk_init` itself, so gtk4-rs's own
/// init flag is never set — and its widget constructors assert on that
/// flag, aborting across the FFI even though GTK is running. Every C-ABI
/// entry point that constructs a widget calls this first.
pub(crate) fn ensure_gtk_init() {
    unsafe { gtk4::set_initialized() };
}

#[cfg(test)]
mod tests;

pub use measure::PangoMeasure;
pub use view::{AvatarFunc, LoadDirection, RotulusView};
