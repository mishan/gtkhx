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
    /// Build the whole browser + two-panel content for `sess` (registering it
    /// in the C browser table, keyed on the session) and return the content
    /// box, or NULL when there is nothing to embed — this session already has
    /// a browser, or `sess` was NULL, which C logs.
    fn gtkhx_files_build_content(sess: *mut std::ffi::c_void) -> *mut gtk4::ffi::GtkWidget;
}

/// Open (or raise) the Files panel for `sess` — the session whose files it
/// lists. C ABI replacement for the old `files_browser.c::open_files_browser`.
///
/// One browser per connection: `sess` names whose files these are, and a
/// second connection gets its own browser in its own page of the same panel.
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

    // NULL means `sess` was NULL and C refused, or this session somehow
    // already has a browser without a page to show it in — which dock::open
    // above rules out on the normal path.
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
