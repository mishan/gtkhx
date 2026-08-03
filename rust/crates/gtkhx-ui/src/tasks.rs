//! Tasks window — Phase R5.10 gtk4-rs shell over the C task list.
//!
//! The Tasks panel is docked into the toolbar window's bottom-area
//! `PanelFrame`. Its content — the `GtkListBox` of per-transfer/task rows
//! (`struct gtask`, progress bars, queue badges) and the Stop/Start/Up/Down
//! action buttons wired to the C `task_*` handlers — stays C in `tasks.c`,
//! built by `gtkhx_tasks_build_content`. This module owns the *window
//! shell*: raise-if-open, dock registration (via `dock_bridge.c`), and the
//! post-embed lifecycle. It replaces the old `tasks.c::create_tasks_window`,
//! keeping the same C ABI so the callers (toolbar button, gtkhx.c auto-open)
//! link unchanged.
//!
//! The task list content + its transfer-model coupling (htxf / xfers queue)
//! stay C until the transfers subsystem itself is ported.

use std::ffi::c_void;

use crate::dock;

/// Opaque C `session *`.
type Session = c_void;

extern "C" {
    /// Build the Tasks panel content (action button bar over the task list
    /// scroller). Returns a still-floating container, or NULL if `sess` is
    /// NULL. Requires `create_tasks` to have run (sess->gtask_scroll set).
    fn gtkhx_tasks_build_content(sess: *mut Session) -> *mut gtk4::ffi::GtkWidget;
    /// Mark the panel open in prefs and push the current task + xfer state
    /// into the freshly-embedded list.
    fn gtkhx_tasks_after_embed(sess: *mut Session);
}

/// Open (or raise) the Tasks panel. C ABI replacement for the old
/// `tasks.c::create_tasks_window`. `widget` is vestigial (the panel is a
/// resident of the toolbar dock, not reparented); `data` is the `session *`.
///
/// # Safety
/// `data` is a valid `session *` (or NULL); called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_tasks_window(_widget: *mut c_void, data: *mut c_void) {
    crate::ensure_gtk_init();

    // Re-click of the toolbar button → re-attach + raise, don't rebuild.
    let sess = data as *mut Session;
    // Tasks is per-connection today; M6 turns the queue global, at which
    // point this panel stops having pages at all.
    let dock::Open::Build(page) = dock::open(dock::ID_TASKS, data) else {
        return;
    };

    let content = gtkhx_tasks_build_content(sess);
    if content.is_null() {
        return;
    }

    // On failure the bridge has already destroyed `content`; skip the
    // post-embed lifecycle so we don't mark a non-existent panel open.
    if dock::place(
        dock::ID_TASKS,
        &page,
        dock::KIND_SIDEBAR,
        dock::AREA_BOTTOM,
        "Tasks",
        "view-list-symbolic",
        content,
    ) {
        gtkhx_tasks_after_embed(sess);
    }
}
