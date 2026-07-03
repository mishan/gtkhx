//! News browser (1.5) window shell — Phase R5.12 gtk4-rs shell.
//!
//! The 1.5 threaded-news browser is docked into the toolbar window's CENTER
//! area. Its content — the whole `gnews_browser` (tree view, breadcrumb,
//! action buttons, post view, compose/create dialogs) and its fetch/RPC path
//! — stays C in `news_browser.c`, built by `gtkhx_news_browser_build_content`.
//! This module owns the *window shell*: raise-if-open, dock registration (via
//! `dock_bridge.c`), and the post-embed `presented`-hook wiring.
//!
//! Unlike the sidebar windows, the browser integrates the panel as its window
//! object, so it needs the panel back after embedding — `after_embed` recovers
//! it from the registry (C side) to connect the one panel-level signal. The
//! entry point `open_news_browser` (still C) orchestrates the keyaccel +
//! fetch-on-open around this shell.

use std::ffi::c_void;

use crate::dock;

/// Stable panel id — matches `HX_PANEL_ID_NEWS15` (`panel_registry.h`).
const HX_ID_NEWS15: &str = "news15";

extern "C" {
    /// Build the whole browser + content tree (stashing it in the C
    /// `the_browser` global) and return the content box (still floating).
    fn gtkhx_news_browser_build_content() -> *mut gtk4::ffi::GtkWidget;
    /// Wire the PanelWidget::presented hook once the panel exists.
    fn gtkhx_news_browser_after_embed();
}

/// Open (or raise) the News-browser panel. C ABI entry the `open_news_browser`
/// orchestrator calls. `widget` / `sess` are vestigial here — the browser is a
/// process singleton (`the_browser`) built without per-call state.
///
/// # Safety
/// Called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_news_browser_window(
    _widget: *mut c_void,
    _sess: *mut c_void,
) {
    crate::ensure_gtk_init();

    // Re-open of an existing panel → re-attach + raise, don't rebuild.
    if dock::raise_if_open(HX_ID_NEWS15) {
        return;
    }

    let content = gtkhx_news_browser_build_content();
    if content.is_null() {
        return;
    }

    // On failure the bridge has already destroyed `content`; skip the
    // presented-hook wiring.
    if dock::embed(
        HX_ID_NEWS15,
        dock::KIND_CENTER,
        dock::AREA_CENTER,
        "News (1.5+)",
        "text-x-generic-symbolic",
        content,
    ) {
        gtkhx_news_browser_after_embed();
    }
}
