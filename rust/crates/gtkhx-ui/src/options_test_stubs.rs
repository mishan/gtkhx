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

// ---- session identity + the dock bridge ----------------------------------
//
// For the connection tab strip, which keys everything on a connection's
// serial, reaches it through the session, and asks the dock to swap pages.
//
// `hx_conn_serial` is deliberately *not* here: it lives in gtkhx-core, a Rust
// crate this one already depends on, so stubbing it would be a duplicate
// symbol — and a lie besides, since the serial is the one value the strip's
// correctness rests on. The test allocates real connections instead, and
// `gtkhx_session_htlc` below is an identity so that a connection can stand in
// for the session that owns it. That keeps the real serial allocator in the
// loop rather than substituting a fake one that could quietly hand every tab
// the same key.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void {
    sess
}
/// Counted, unlike its neighbours, because for one assertion the *call* is the
/// thing under test rather than the answer: the tab strip's selection handler
/// has no other observable effect inside a test binary, and whether it ran at
/// all for the first tab is a real bug this once had.
pub static SET_ACTIVE_CALLS: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

#[no_mangle]
pub unsafe extern "C" fn hx_session_set_active(_sess: *mut c_void) -> glib::ffi::gboolean {
    SET_ACTIVE_CALLS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    glib::ffi::GTRUE
}
/// Counted for the same reason as its neighbour above: tearing a connection
/// down has no observable effect inside a test binary — the session it is
/// handed is a bare connection with no registry behind it — so whether the
/// close button reaches it at all is the only thing a test can see, and "the
/// close button did nothing" is exactly the bug this once had.
pub static SESSION_CLOSE_CALLS: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

#[no_mangle]
pub unsafe extern "C" fn hx_session_close(_sess: *mut c_void) {
    SESSION_CLOSE_CALLS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
}
/// The focus-following chrome (status bar, titles, tray, banner) — all of it C
/// or behind C state a test binary doesn't link. Inert.
#[no_mangle]
pub unsafe extern "C" fn hx_chrome_refresh() {}
#[no_mangle]
pub unsafe extern "C" fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}

// The dock bridge (dock_bridge.c). Inert: the strip's selection handler runs
// during the test — appending a tab selects it — and walks every
// per-connection panel, so these have to exist. Answering FALSE throughout
// means "no panel has a page for this connection", which is both true of a
// test binary with no dock and the branch worth having run.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_dock_raise_if_open(_id: *const c_char) -> glib::ffi::gboolean {
    glib::ffi::GFALSE
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_dock_is_embedded(_id: *const c_char) -> glib::ffi::gboolean {
    glib::ffi::GFALSE
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_dock_set_needs_attention(
    _id: *const c_char,
    _state: glib::ffi::gboolean,
) {
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_dock_has_page(
    _id: *const c_char,
    _page: *const c_char,
) -> glib::ffi::gboolean {
    glib::ffi::GFALSE
}
#[no_mangle]
pub unsafe extern "C" fn gtkhx_dock_show_page(
    _id: *const c_char,
    _page: *const c_char,
) -> glib::ffi::gboolean {
    glib::ffi::GFALSE
}

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
