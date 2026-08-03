//! Files browser window shell — Phase R5.16 gtk4-rs shell.
//!
//! The two-panel files browser is docked into the toolbar window's CENTER
//! area. Its content — the two `files_panel` column views, the headerbar +
//! center-column action buttons, DnD between panels, the provider/transfer
//! integration, and the whole shortcut set — stays C in `files_browser.c`,
//! built by `gtkhx_files_build_content`. This module owns the *window shell*:
//! raise-if-open + dock registration (via `dock_bridge.c`). It replaces the old
//! `files_browser.c::open_files_browser`; the one caller is
//! `create_toolbar_window`.
//!
//! Nothing needs the dock panel back after embedding (unlike the news browser
//! there's no `presented` hook, and `br->window` points at the content box),
//! so there's no post-embed step.

use crate::dock;

extern "C" {
    /// Build the whole browser + two-panel content for `sess` (stashing it in
    /// the C `the_browser` global) and return the content box, or NULL when
    /// there is nothing to embed — a browser already exists, or `sess` was
    /// NULL, which C logs.
    fn gtkhx_files_build_content(sess: *mut std::ffi::c_void) -> *mut gtk4::ffi::GtkWidget;
}

/// Open (or raise) the Files panel for `sess` — the session whose files it
/// lists. C ABI replacement for the old `files_browser.c::open_files_browser`.
///
/// `sess` binds the browser on the *first* open only: the browser is still a
/// singleton, so a later call raises the existing panel and never sees the
/// session it was passed. De-singletonising it is M4g.
///
/// # Safety
/// Called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn open_files_browser(sess: *mut std::ffi::c_void) {
    crate::ensure_gtk_init();

    // Re-open of an existing panel → re-attach + raise, don't rebuild.
    let dock::Open::Build(page) = dock::open(dock::ID_FILES, sess) else {
        return;
    };

    // NULL means `sess` was NULL and C refused. The other historical cause —
    // a browser already existing — is now caught a step earlier by
    // dock::open's singleton claim, which is where every role handles it the
    // same way instead of each one improvising.
    let content = gtkhx_files_build_content(sess);
    if content.is_null() {
        return;
    }

    dock::place(
        dock::ID_FILES,
        &page,
        dock::KIND_CENTER,
        dock::AREA_CENTER,
        "Files",
        "folder-symbolic",
        content,
    );
}
