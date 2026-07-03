//! Users window — Phase R5 gtk4-rs shell over the C content leaves.
//!
//! The Users panel is docked into the toolbar window's end-area
//! `PanelFrame`. Its *content* is still C: the custom `HxUserListView`
//! (`users_view.c`), the themed action buttons wired to the `view_*_btn`
//! handlers plus the six sensitivity globals `gtkutil.c` toggles, and the
//! optional voice panel. Those leaves are built by `users_bridge.c` and
//! handed back as one still-floating container. This module owns the
//! *window shell*: raise-if-open, dock registration (via `dock_bridge.c`),
//! and the post-embed lifecycle. It replaces the old
//! `users.c::create_users_window`, keeping the same C ABI so the callers
//! (toolbar button, gtkhx.c auto-open) link unchanged.
//!
//! The custom view widget + its button handlers are the next Users
//! increment; they stay C behind the bridge until then.

use std::ffi::c_void;

use crate::dock;

/// Opaque C `session *`.
type Session = c_void;

/// Stable panel id — matches `HX_PANEL_ID_USERS` (`panel_registry.h`).
const HX_ID_USERS: &str = "users";

extern "C" {
    /// Build the Users panel content (button bar over the scrolled view),
    /// stashing the view on `sess->users_view`. Returns a still-floating
    /// container, or NULL if `sess` is NULL.
    fn gtkhx_users_bridge_build_content(sess: *mut Session) -> *mut gtk4::ffi::GtkWidget;
    /// Mark the panel open in prefs and, if connected, sensitize the
    /// buttons + populate the list.
    fn gtkhx_users_bridge_after_embed(sess: *mut Session);
}

/// Open (or raise) the Users panel. C ABI replacement for the old
/// `users.c::create_users_window`. `parent` is vestigial (the panel is a
/// resident of the toolbar dock, not reparented); `data` is the
/// `session *`.
///
/// # Safety
/// `data` is a valid `session *` (or NULL); called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_users_window(_parent: *mut c_void, data: *mut c_void) {
    crate::ensure_gtk_init();

    // A registered panel means the user re-clicked the toolbar button;
    // re-attach + raise instead of rebuilding a second content tree.
    if dock::raise_if_open(HX_ID_USERS) {
        return;
    }

    let sess = data as *mut Session;
    let content = gtkhx_users_bridge_build_content(sess);
    if content.is_null() {
        return;
    }

    // On failure the bridge has already destroyed `content`; skip the
    // post-embed lifecycle so we don't mark a non-existent panel open.
    if dock::embed(
        HX_ID_USERS,
        dock::KIND_SIDEBAR,
        dock::AREA_END,
        "Users",
        "system-users-symbolic",
        content,
    ) {
        gtkhx_users_bridge_after_embed(sess);
    }
}
