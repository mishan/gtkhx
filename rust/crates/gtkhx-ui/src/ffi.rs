//! Shared FFI surface for the UI crate: extern declarations for the C
//! helpers the ported windows call. These resolve at final link against
//! the C binary (the gtkhx_* util helpers, the connect dialog, the
//! `tracker_bridge.c` / `gtkhx_ui_bridge.c` session shims) — the same
//! leaf-up "C resolves Rust's externs" shape the R4 crates use. Window-
//! specific externs (e.g. the tracker boxed payload + its network entry
//! points, or the user-editor wire senders) live in the owning module.

use std::ffi::c_char;
use std::os::raw::{c_int, c_void};

// gtk / glib / gio raw pointer aliases (kept local so callers read
// without importing the whole ffi trees).
pub type GtkWidget = gtk4::ffi::GtkWidget;
pub type GtkWindow = gtk4::ffi::GtkWindow;
pub type GApplication = gio::ffi::GApplication;

/// GtkHx version, embedded from meson's `project(version:)` at build time
/// (rust/meson.build sets `GTKHX_VERSION` in the cargo env). Falls back to
/// "dev" for a bare `cargo build` outside meson.
pub const VERSION: &str = match option_env!("GTKHX_VERSION") {
    Some(v) => v,
    None => "dev",
};

/// `GTKHX_SCALE_WINDOW_BUTTONS` — second value of the `GtkhxScaleArea`
/// enum (`gtkhx_theme.h`); the themable area action buttons use.
pub const GTKHX_SCALE_WINDOW_BUTTONS: c_int = 1;

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
    /// Theme fg/bg/font for a read-only text surface (agreement / news
    /// viewers). See gtkhx_theme.h `.gtkhx-text`.
    pub fn gtkhx_apply_text_style(w: *mut GtkWidget);
    pub fn init_keyaccel(w: *mut GtkWidget);
    /// Dialog-style bail-out shortcuts (Esc / Ctrl+W close, Ctrl+Q quit,
    /// Ctrl+K connect), capture-phase.
    pub fn init_keyaccel_dialog(w: *mut GtkWidget);
    pub fn gtkhx_dialog_add_close_shortcuts(dialog: *mut GtkWidget);
    /// The application-active toplevel (`gtk_application_get_active_window`),
    /// used as a transient-for parent. May be NULL.
    pub fn gtkhx_active_window() -> *mut GtkWindow;
    /// `[host]:port` for IPv6 literals, `host:port` otherwise. Result is
    /// `g_malloc`'d — free with `g_free`.
    pub fn gtkhx_join_host_port(host: *const c_char, port: u16) -> *mut c_char;
    pub fn gtkhx_get_application() -> *mut GApplication;

    // ---- multi-conn M0 session seam (gtkhx.c / session.h) ------------
    /// The currently-focused `session *` (multi-conn M0 seam,
    /// docs/multi-connection.md). Today == `&the_session`; UI
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
    /// `gtkhx_prefs_set_bool(name, value)` — persists + fires the key's
    /// change hook (keeps the Settings switch in lockstep).
    pub fn gtkhx_prefs_set_bool(name: *const c_char, value: c_int);

    // (Bookmark storage moved to the hxbookmarks crate — reached through
    // crate::bookmark_store, no C bookmark API remains.)

    // ---- toolbar (toolbar.c) -----------------------------------------
    /// Rebuild the toolbar Connect-button dropdown after a bookmark
    /// create / rename / delete.
    pub fn toolbar_refresh_bookmarks();

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
