//! Shared FFI surface for the UI crate: extern declarations for the C
//! helpers the ported windows call, plus the `HxBookmark` `#[repr(C)]`
//! mirror. These resolve at final link against the C binary (the
//! gtkhx_* util helpers, the bookmark API, the connect dialog, the
//! `tracker_bridge.c` session/prefs shim) — the same leaf-up "C resolves
//! Rust's externs" shape the R4 crates use. Window-specific externs
//! (e.g. the tracker boxed payload + its network entry points) live in
//! the owning module (`tracker::ffi`).

use std::ffi::c_char;
use std::os::raw::{c_int, c_void};

// gtk / glib / gio raw pointer aliases (kept local so callers read
// without importing the whole ffi trees).
pub type GtkWidget = gtk4::ffi::GtkWidget;
pub type GApplication = gio::ffi::GApplication;
pub type GError = glib::ffi::GError;

/// `GTKHX_SCALE_WINDOW_BUTTONS` — second value of the `GtkhxScaleArea`
/// enum (`gtkhx_theme.h`); the themable area action buttons use.
pub const GTKHX_SCALE_WINDOW_BUTTONS: c_int = 1;

// ---------------------------------------------------------------------
// HxBookmark — the fixed on-disk bookmark record (bookmarks.h).
// ---------------------------------------------------------------------

/// `#[repr(C)]` mirror of `HxBookmark` (`bookmarks.h`). `name` is a heap
/// `char*` owned by the record; the rest are inline fixed buffers.
#[repr(C)]
pub struct HxBookmark {
    pub name: *mut c_char,
    pub server: [c_char; 128],
    pub port: [c_char; 8],
    pub login: [c_char; 33],
    pub pass: [c_char; 33],
    pub secure: c_char,
    pub compress: c_char,
    pub cipher: c_char,
    pub tls: c_char,
}

const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<HxBookmark>() == 216);
    assert!(offset_of!(HxBookmark, server) == 8);
    assert!(offset_of!(HxBookmark, port) == 136);
    assert!(offset_of!(HxBookmark, secure) == 210);
    assert!(offset_of!(HxBookmark, tls) == 213);
};

extern "C" {
    // ---- gtkhx util helpers (gtkutil.c / gtkhx.c / host_port.c) ------
    /// Themed pixel-art GResource button. Pass `cb = NULL` / `data =
    /// NULL` and wire the "clicked" signal from Rust via gtk4-rs.
    pub fn gtkhx_pixmap_button(
        resource_name: *const c_char,
        tooltip: *const c_char,
        area: c_int,
        cb: *const c_void,
        user_data: *mut c_void,
    ) -> *mut GtkWidget;
    pub fn gtkhx_apply_listview_style(w: *mut GtkWidget);
    pub fn init_keyaccel(w: *mut GtkWidget);
    pub fn gtkhx_dialog_add_close_shortcuts(dialog: *mut GtkWidget);
    /// `[host]:port` for IPv6 literals, `host:port` otherwise. Result is
    /// `g_malloc`'d — free with `g_free`.
    pub fn gtkhx_join_host_port(host: *const c_char, port: u16) -> *mut c_char;
    pub fn gtkhx_get_application() -> *mut GApplication;

    // ---- multi-conn M0 session seam (gtkhx.c / session.h) ------------
    /// The currently-focused `session *` (multi-conn M0 seam,
    /// docs/multi-connection-scoping.md). Today == `&the_session`; UI
    /// actions route through it so they act on the focused connection.
    /// Held opaquely as `*mut c_void` (a `session *`).
    pub fn hx_active_session() -> *mut c_void;

    // ---- connect dialog (connect.c) ----------------------------------
    pub fn create_connect_window(btn: *mut GtkWidget, data: *mut c_void);
    pub fn set_the_entries(
        address: *mut c_char,
        login: *mut c_char,
        password: *mut c_char,
        port: *mut c_char,
        secure: c_char,
        compress: c_char,
        cipher: c_char,
        tls: c_char,
    );

    // ---- toolbar toast (toolbar.c) -----------------------------------
    pub fn toolbar_show_toast(text: *const c_char);

    // ---- prefs setter (options.c) ------------------------------------
    /// `gtkhx_prefs_set_bool(name, value)` — persists + fires the cfgvar
    /// change hook (keeps the Settings switch in lockstep).
    pub fn gtkhx_prefs_set_bool(name: *const c_char, value: c_int);

    // ---- bookmark API (bookmarks.c / bookmark_cipher.c) --------------
    pub fn hx_bookmark_new() -> *mut HxBookmark;
    pub fn hx_bookmark_free(bm: *mut HxBookmark);
    pub fn hx_bookmark_load(name: *const c_char) -> *mut HxBookmark;
    pub fn hx_bookmark_save(bm: *const HxBookmark, err: *mut *mut GError) -> glib::ffi::gboolean;
    pub fn hx_bookmark_safe_filename(name: *const c_char) -> *mut c_char;
    pub fn bookmark_cipher_name(byte: u8) -> *const c_char;

    // ---- tracker_bridge.c (new, permanent session/prefs shim) --------
    // Narrow accessors into not-yet-ported global C session/prefs state.
    // Named for the tracker (first consumer); connect_apply / log_info
    // are generic and future windows reuse them.
    /// Read `gtkhx_prefs.track_case` (1 = match case).
    pub fn gtkhx_tracker_pref_case() -> c_int;
    /// Direct double-click connect: reset htlc cipher/compress alg, set
    /// cipheralg to `cipher_name` (NULL = leave empty), call `hx_connect`
    /// on `the_session.htlc`.
    pub fn gtkhx_tracker_connect_apply(
        address: *const c_char,
        port: u16,
        secure: c_char,
        tls: c_char,
        cipher_name: *const c_char,
    );
    /// `hx_printf_prefix(&the_session.htlc, 0, INFOPREFIX, "%s", msg)`.
    pub fn gtkhx_tracker_log_info(msg: *const c_char);
}

/// Stable bookmark cipher-byte vocabulary (`bookmark_cipher.h`).
pub const BOOKMARK_CIPHER_BYTE_NONE: u8 = 0;
pub const BOOKMARK_CIPHER_BYTE_BLOWFISH: u8 = 2;
pub const BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305: u8 = 3;
