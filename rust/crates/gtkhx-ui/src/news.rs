//! News window — Phase R5.11 gtk4-rs shell over the C news content.
//!
//! The 1.0/1.2 flat-news panel is docked into the toolbar window's start
//! (sidebar) area. Its content — the Post/Reload/Find buttons, the
//! read-only `GtkTextView`, and the Find search bar with its Ctrl+F
//! shortcut — stays C in `news.c`, built by `gtkhx_news_build_content`. This
//! module owns the *window shell*: raise-if-open, dock registration (via
//! `dock_bridge.c`), and the post-embed lifecycle. It replaces the old
//! `news.c::create_news_window`, keeping the same C ABI so `open_news` and
//! the toolbar link unchanged.
//!
//! The news search-ctx (previously hung off the dock panel) now lives on
//! `sess->news_text`, so the C content build never needs the panel the
//! shell owns; the fetch/post RPC path stays C.

use std::ffi::c_void;

use crate::dock;

/// Opaque C `session *`.
type Session = c_void;

/// Stable panel id — matches `HX_PANEL_ID_NEWS` (`panel_registry.h`).
const HX_ID_NEWS: &str = "news";

extern "C" {
    /// Build the News panel content (button bar + search bar + read-only
    /// text view), stashing news_text/postButton/reloadButton on the
    /// session. Returns a still-floating container, or NULL if `sess` is
    /// NULL.
    fn gtkhx_news_build_content(sess: *mut Session) -> *mut gtk4::ffi::GtkWidget;
    /// Mark the panel open in prefs and, if connected, sensitize the Post +
    /// Reload buttons.
    fn gtkhx_news_after_embed(sess: *mut Session);
}

/// Open (or raise) the News panel. C ABI replacement for the old
/// `news.c::create_news_window`. `toolbar_window` is vestigial (the panel is
/// a resident of the toolbar dock, not reparented); `sess` is the
/// `session *`.
///
/// # Safety
/// `sess` is a valid `session *` (or NULL); called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_news_window(_toolbar_window: *mut c_void, sess: *mut c_void) {
    crate::ensure_gtk_init();

    // Re-click of the toolbar button → re-attach + raise, don't rebuild.
    if dock::raise_if_open(HX_ID_NEWS) {
        return;
    }

    let sess = sess as *mut Session;
    let content = gtkhx_news_build_content(sess);
    if content.is_null() {
        return;
    }

    // On failure the bridge has already destroyed `content`; skip the
    // post-embed lifecycle so we don't mark a non-existent panel open.
    if dock::embed(
        HX_ID_NEWS,
        dock::KIND_SIDEBAR,
        dock::AREA_START,
        "News",
        "text-x-generic-symbolic",
        content,
    ) {
        gtkhx_news_after_embed(sess);
    }
}
