//! Files browser window shell — Phase R5.16 gtk4-rs shell.
//!
//! The two-panel files browser is docked into the toolbar window's CENTER
//! area. Its content — the two `files_panel` column views, the headerbar +
//! center-column action buttons, DnD between panels, the provider/transfer
//! integration, and the whole shortcut set — stays C in `files_browser.c`,
//! built by `gtkhx_files_build_content`. This module owns the *window shell*:
//! raise-if-open + dock registration (via `dock_bridge.c`). It replaces the old
//! `files_browser.c::open_files_browser`, keeping the same C ABI so the toolbar
//! GAction links unchanged.
//!
//! Nothing needs the dock panel back after embedding (unlike the news browser
//! there's no `presented` hook, and `br->window` points at the content box),
//! so there's no post-embed step.

use crate::dock;

/// Stable panel id — matches `HX_PANEL_ID_FILES` (`panel_registry.h`).
const HX_ID_FILES: &str = "files";

extern "C" {
    /// Build the whole browser + two-panel content (stashing it in the C
    /// `the_browser` global) and return the content box, or NULL if the
    /// browser already exists.
    fn gtkhx_files_build_content() -> *mut gtk4::ffi::GtkWidget;
}

/// Open (or raise) the Files panel. C ABI replacement for the old
/// `files_browser.c::open_files_browser` (action-style, no args).
///
/// # Safety
/// Called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn open_files_browser() {
    crate::ensure_gtk_init();

    // Re-open of an existing panel → re-attach + raise, don't rebuild.
    if dock::raise_if_open(HX_ID_FILES) {
        return;
    }

    // NULL means the browser already exists but its panel wasn't registered
    // (should-never-happen) — nothing to embed.
    let content = gtkhx_files_build_content();
    if content.is_null() {
        return;
    }

    dock::embed(
        HX_ID_FILES,
        dock::KIND_CENTER,
        dock::AREA_CENTER,
        "Files",
        "folder-symbolic",
        content,
    );
}
