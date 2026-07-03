//! Chat window — Phase R5.13 gtk4-rs shell over the C chat content.
//!
//! The public-chat panel is docked into the toolbar window's CENTER area. Its
//! content — the subject entry, the xtext output, the input text view with
//! emoji/inline-media buttons, and the `AdwTabView` that hosts the
//! per-conversation *private chat* / *private message* tabs — stays C in
//! `chat.c`, built by `gtkhx_chat_build_content`. This module owns the
//! *window shell*: raise-if-open, dock registration (via `dock_bridge.c`), and
//! the post-embed lifecycle. It replaces the old `chat.c::create_chat_window`,
//! keeping the same C ABI so the toolbar + gtkhx.c auto-open link unchanged.
//!
//! Private chats and private messages are tabs *inside* this one panel
//! (`chat_tabs.c` / AdwTabView), not separate dock panels — so there is a
//! single dock registration and the dynamic-panel path isn't needed here.

use std::ffi::c_void;

use crate::dock;

/// Opaque C `session *`.
type Session = c_void;

/// Stable panel id — matches `HX_PANEL_ID_CHAT` (`panel_registry.h`).
const HX_ID_CHAT: &str = "chat";

extern "C" {
    /// Build the public-chat content + the pchat/PM tab view. Returns a
    /// still-floating container, or NULL if `sess` is NULL / has no
    /// public-chat gchat yet.
    fn gtkhx_chat_build_content(sess: *mut Session) -> *mut gtk4::ffi::GtkWidget;
    /// Carry the session onto the dock panel, mark it open, focus the input.
    fn gtkhx_chat_after_embed(sess: *mut Session);
}

/// Open (or raise) the Chat panel. C ABI replacement for the old
/// `chat.c::create_chat_window`. `parent` is vestigial (the panel is a
/// resident of the toolbar dock, not reparented); `data` is the `session *`.
///
/// # Safety
/// `data` is a valid `session *` (or NULL); called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_chat_window(_parent: *mut c_void, data: *mut c_void) {
    crate::ensure_gtk_init();

    // Re-open → re-attach + raise the existing panel, don't rebuild.
    if dock::raise_if_open(HX_ID_CHAT) {
        return;
    }

    let sess = data as *mut Session;
    let content = gtkhx_chat_build_content(sess);
    if content.is_null() {
        return;
    }

    // On failure the bridge has already destroyed `content`; skip the
    // post-embed lifecycle.
    if dock::embed(
        HX_ID_CHAT,
        dock::KIND_CENTER,
        dock::AREA_CENTER,
        "Chat",
        "user-available-symbolic",
        content,
    ) {
        gtkhx_chat_after_embed(sess);
    }
}
