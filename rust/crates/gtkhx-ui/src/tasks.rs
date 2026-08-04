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
//!
//! Unlike the other five dock panels this one is **global**: one queue for the
//! whole application, with each row tagged by the connection it belongs to.
//! Switching connection tabs leaves it alone. What that costs here is that a
//! connection after the first still has state to push into a panel it did not
//! build — see the `Done` arm below.

use std::ffi::c_void;

use crate::dock;

/// Opaque C `session *`.
type Session = c_void;

extern "C" {
    /// Build the Tasks panel content (action button bar over the task list
    /// scroller). Returns a still-floating container, or NULL if `sess` is
    /// NULL. Requires `create_tasks` to have run.
    fn gtkhx_tasks_build_content(sess: *mut Session) -> *mut gtk4::ffi::GtkWidget;
    /// Mark the panel open in prefs and push the current task + xfer state
    /// into the freshly-embedded list.
    fn gtkhx_tasks_after_embed(sess: *mut Session);
    /// Push one connection's tasks and transfers into the queue, for a
    /// connection joining a panel that already exists.
    fn gtkhx_tasks_sync_conn(sess: *mut Session);
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

    let sess = data as *mut Session;

    // A second connection opening: either
    // way the queue already exists and must not be rebuilt. It is shared, so
    // what this connection still needs is its own rows put into it — nothing
    // else will, because no page is created for it.
    let page = match dock::open_global(dock::ID_TASKS) {
        dock::Open::Done => {
            gtkhx_tasks_sync_conn(sess);
            return;
        }
        dock::Open::Build(page) => page,
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
