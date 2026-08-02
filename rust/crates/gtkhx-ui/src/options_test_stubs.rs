//! Test-only definitions for the C symbols the Settings pages call.
//!
//! The pages are Rust but most of the boundary underneath them is not:
//! preferences, themes, icon resources and toasts are C. In the real build
//! those come from `libgtkhx`; a `cargo test` binary links none of it, so the
//! moment a test touches the page table — which holds a function pointer to
//! every page — the linker wants all thirty-odd of them.
//!
//! The voice block is the exception and worth calling out: those symbols are
//! defined in `hxvoice-runtime`, a Rust crate this one already depends on, so
//! they would link without help. They are shadowed deliberately, to keep a
//! `gst::DeviceMonitor` scan out of a CI container with no audio.
//!
//! The list stops where it does because the linker collects unreached code:
//! `gtkhx_active_window`, called only from `present()`, isn't here because no
//! test calls `present()`. Anything that keeps dead code — a coverage build,
//! `-C link-dead-code`, or a future test that opens the window — will want
//! more entries, and the failure will name a symbol that looks unrelated to
//! whatever was just added.
//!
//! These are link-time stubs, not fakes. They exist so the page tree *builds*
//! in a test binary; they deliberately do not simulate behaviour, and a test
//! that depends on what one of them returns is testing this file rather than
//! the client. The values are the blandest ones that keep a page builder on
//! its ordinary path: every preference is absent, there are no themes, no
//! icons and no audio devices.
//!
//! This mirrors what the C test suite already does — `test_theme_scale`
//! "provides its own `gtkhx_prefs` instead of linking the GTK-heavy
//! options.c" — one language over.
//!
//! The cost to keep in mind: a symbol stubbed here is a symbol whose real
//! declaration no longer has to match anything at test-link time. If a C
//! signature changes, the mismatch surfaces in the real build, not here.

#![cfg(test)]
#![allow(clippy::missing_safety_doc)]

use gtk4::glib;
use std::ffi::{c_char, c_int, c_void};

/// `g_strdup("")` — `gtkhx_prefs_get_string` is documented never to return
/// NULL and to hand back a `g_malloc`'d copy the caller frees, so the stub
/// has to allocate the same way rather than return a static.
unsafe fn empty_string() -> *mut c_char {
    glib::ffi::g_strdup(c"".as_ptr())
}

// ---- preferences (options.c's by-name bridge) ---------------------------

/// The one stub that is not a blank: it delegates to the real schema.
///
/// Every row builder starts by checking the key's kind and returning an
/// insensitive placeholder if it doesn't match the row's shape. A stub
/// answering "no such key" would send all of them down that three-line path,
/// and `every_page_builds` would be asserting that almost nothing runs. The
/// real lookup is `path_of` + `kind_of` with no loaded config behind it, so
/// borrowing it costs nothing and makes the pages build for real.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_prefs_type(name: *const c_char) -> c_int {
    hxconfig::ffi::hxconfig_type(name)
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_prefs_get_bool(_name: *const c_char) -> c_int {
    0
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_prefs_set_bool(_name: *const c_char, _value: c_int) {}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_prefs_get_int(_name: *const c_char) -> c_int {
    0
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_prefs_set_int(_name: *const c_char, _value: c_int) {}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_prefs_get_string(_name: *const c_char) -> *mut c_char {
    empty_string()
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_prefs_set_string(_name: *const c_char, _value: *const c_char) {}

// ---- themes (gtkhx_theme.c) ---------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn gtkhx_theme_names_begin() -> c_int {
    0
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_theme_names_name(_i: c_int) -> *const c_char {
    std::ptr::null()
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_theme_names_display(_i: c_int) -> *const c_char {
    std::ptr::null()
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_theme_names_end() {}

// ---- chrome (gtkutil.c, toolbar.c) --------------------------------------

#[no_mangle]
pub unsafe extern "C" fn gtkhx_dialog_add_close_shortcuts(_dialog: *mut c_void) {}
#[no_mangle]
pub unsafe extern "C" fn toolbar_show_toast(_text: *const c_char) {}
#[no_mangle]
pub unsafe extern "C" fn toolbar_refresh_bookmarks() {}

// ---- the config directory (gtkhx.c) -------------------------------------

/// A scratch directory, **not** the developer's real config.
///
/// This one has to lie rather than be blank. The Connections page reads the
/// connection collection while it builds, and the collection is a file
/// resolved from here — pointing a test at the real one would have it
/// bootstrap, and potentially rewrite, whatever connections the person
/// running the suite actually has.
///
/// Per-process so a parallel run can't collide, created eagerly so the store
/// takes its ordinary path rather than its can't-write one, and leaked
/// deliberately: the C contract is a borrowed pointer that stays valid, and
/// there is nowhere to hang a lifetime.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_config_dir() -> *const c_char {
    use std::sync::OnceLock;
    static DIR: OnceLock<std::ffi::CString> = OnceLock::new();
    DIR.get_or_init(|| {
        let path = std::env::temp_dir().join(format!("gtkhx-ui-test-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&path);
        std::ffi::CString::new(path.to_string_lossy().as_ref()).expect("temp path has no NUL")
    })
    .as_ptr()
}

// ---- the live connection (gtkhx_ui_bridge.c) ----------------------------

/// NULL means "not connected", which is the state every avatar path already
/// has to handle.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_active_htlc() -> *mut c_void {
    std::ptr::null_mut()
}

// ---- icons and avatars (icon_enum.c, gif_icons.c) -----------------------

#[no_mangle]
pub unsafe extern "C" fn hx_icon_ids_begin() -> c_int {
    0
}
#[no_mangle]
pub unsafe extern "C" fn hx_icon_ids_nth(_i: c_int) -> c_int {
    -1
}
#[no_mangle]
pub unsafe extern "C" fn hx_icon_ids_end() {}
#[no_mangle]
pub unsafe extern "C" fn hx_icon_pixbuf_for_id(_id: c_int, _fallback: c_int) -> *mut c_void {
    std::ptr::null_mut()
}
#[no_mangle]
pub unsafe extern "C" fn hx_icon_save(_gif: *const u8, _len: usize) -> c_int {
    0
}
#[no_mangle]
pub unsafe extern "C" fn hx_icon_forget() -> c_int {
    0
}
#[no_mangle]
pub unsafe extern "C" fn hx_icon_load_saved() -> *mut c_void {
    std::ptr::null_mut()
}
#[no_mangle]
pub unsafe extern "C" fn hx_icon_set(_htlc: *mut c_void, _gif: *const u8, _len: usize) {}
#[no_mangle]
pub unsafe extern "C" fn hx_icon_clear(_htlc: *mut c_void) {}

// ---- voice (hxvoice-runtime, voice_ptt_keyspec.c) -----------------------
//
// Only referenced in a voice build, and gated to match so the stub set
// doesn't drift out of step with the feature it shadows.

#[cfg(feature = "voice")]
mod voice {
    use super::*;

    #[no_mangle]
    pub unsafe extern "C" fn gtkhx_voice_list_input_devices() -> *mut c_void {
        std::ptr::null_mut()
    }
    #[no_mangle]
    pub unsafe extern "C" fn gtkhx_voice_list_output_devices() -> *mut c_void {
        std::ptr::null_mut()
    }
    #[no_mangle]
    pub unsafe extern "C" fn gtkhx_voice_device_list_len(_list: *mut c_void) -> usize {
        0
    }
    #[no_mangle]
    pub unsafe extern "C" fn gtkhx_voice_device_list_name(
        _list: *mut c_void,
        _i: usize,
    ) -> *const c_char {
        std::ptr::null()
    }
    #[no_mangle]
    pub unsafe extern "C" fn gtkhx_voice_device_list_display_name(
        _list: *mut c_void,
        _i: usize,
    ) -> *const c_char {
        std::ptr::null()
    }
    #[no_mangle]
    pub unsafe extern "C" fn gtkhx_voice_device_list_free(_list: *mut c_void) {}

    #[no_mangle]
    pub unsafe extern "C" fn hx_voice_ptt_keyspec_parse(
        _spec: *const c_char,
        _out_keyval: *mut u32,
        _out_state: *mut u32,
    ) -> c_int {
        0
    }
    #[no_mangle]
    pub unsafe extern "C" fn hx_voice_ptt_keyspec_allowed(_keyval: u32, _state: u32) -> c_int {
        0
    }
    #[no_mangle]
    pub unsafe extern "C" fn hx_voice_ptt_keyspec_canonicalize(
        _keyval: u32,
        _state: u32,
    ) -> *mut c_char {
        std::ptr::null_mut()
    }
}
