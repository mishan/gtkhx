//! Transfers panel — Phase R5.10 gtk4-rs shell over the C task list.
//!
//! The Transfers panel is docked into the toolbar window's bottom-area
//! `PanelFrame`. It keeps its old `tasks` id, so saved layouts still place
//! it. Its content — the `GtkListBox` of per-transfer rows
//! (`struct gtask`, progress bars, queue badges) and the Stop/Pause/Start/Up/Down
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

use std::ffi::{c_char, c_int, c_void, CStr, CString};

use crate::dock;
use crate::tr::tr_argv;

/// Opaque C `session *`.
type Session = c_void;

extern "C" {
    /// Build the Transfers panel content (action button bar over the task list
    /// scroller). Returns a still-floating container, or NULL if `sess` is
    /// NULL. Requires `create_tasks` to have run.
    fn gtkhx_tasks_build_content(sess: *mut Session) -> *mut gtk4::ffi::GtkWidget;
    /// Mark the panel open in prefs and push the current transfers into the
    /// freshly-embedded list.
    fn gtkhx_tasks_after_embed(sess: *mut Session);
    /// Push one connection's transfers into the queue, for a
    /// connection joining a panel that already exists.
    fn gtkhx_tasks_sync_conn(sess: *mut Session);
    /// human_readable.c — a byte count as "1.2M", into `buf`.
    fn human_size(buf: *mut c_char, size: u64) -> *mut c_char;
}

fn human(size: u64) -> String {
    let mut buf = [0 as c_char; 64];
    unsafe { CStr::from_ptr(human_size(buf.as_mut_ptr(), size)) }
        .to_string_lossy()
        .into_owned()
}

/// The line under a transfer's name: how far it has got of how much, and
/// either how fast and how long to go or that it is paused. `run` is what
/// its connection has brought, over `elapsed` seconds. A newly allocated
/// string, for the caller to `g_free`.
#[no_mangle]
pub extern "C" fn gtkhx_tasks_subtitle(
    pos: u64,
    size: u64,
    run: u64,
    elapsed: i64,
    paused: c_int,
) -> *mut c_char {
    let (p, s) = (human(pos), human(size));
    let text = if paused != 0 {
        tr_argv("%1$s of %2$s · Paused", &[&p, &s])
    } else if run == 0 {
        // No speed to tell yet: waiting on the server, or queued by it.
        tr_argv("%1$s of %2$s", &[&p, &s])
    } else {
        let bps = (run / elapsed.max(1) as u64).max(1);
        // Rounded up, so the countdown ends at 0 rather than skipping it.
        let eta = size.saturating_sub(pos).div_ceil(bps);
        let (h, m, sec) = (eta / 3600, eta % 3600 / 60, format!("{:02}", eta % 60));
        let rate = human(bps);
        if h > 0 {
            tr_argv(
                "%1$s of %2$s · %3$s/s · ETA %4$d:%5$02d:%6$02d",
                &[&p, &s, &rate, &h.to_string(), &format!("{m:02}"), &sec],
            )
        } else {
            tr_argv(
                "%1$s of %2$s · %3$s/s · ETA %4$d:%5$02d",
                &[&p, &s, &rate, &m.to_string(), &sec],
            )
        }
    };
    let text = CString::new(text).unwrap_or_default();
    unsafe { gtk4::glib::ffi::g_strdup(text.as_ptr()) }
}

/// Open (or raise) the Transfers panel. C ABI replacement for the old
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
        "Transfers",
        "mail-send-receive-symbolic",
        content,
    ) {
        gtkhx_tasks_after_embed(sess);
    }
}
