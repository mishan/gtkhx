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
use crate::tr::{n_, tr, tr_argv};

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
/// either how fast and how long to go or that it is paused or failed, `paused` being
/// `xfer_paused`'s answer. `run` is what its connection has brought, over
/// `elapsed` seconds. A newly allocated string, for the caller to `g_free`.
#[no_mangle]
pub extern "C" fn gtkhx_tasks_subtitle(
    pos: u64,
    size: u64,
    run: u64,
    elapsed: i64,
    paused: c_int,
) -> *mut c_char {
    let text = subtitle(pos, size, run, elapsed, paused, human);
    let text = CString::new(text).unwrap_or_default();
    unsafe { gtk4::glib::ffi::g_strdup(text.as_ptr()) }
}

/// `void gtkhx_tasks_failed(int upload, const char *name, int why, int err)`
/// — tell the person a transfer of `name` failed, as a refused request is
/// told: a toast and the error sound. `why` is 0 for a connection that
/// couldn't be opened, 1 for a server that stopped responding, 2 for one that
/// failed partway, 3 for the local file's OS error `err`, and 4 for a file,
/// 5 for a folder, that the server already holds a larger file of.
///
/// # Safety
/// `name` is NULL or a NUL-terminated string. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_tasks_failed(
    upload: c_int,
    name: *const c_char,
    why: c_int,
    err: c_int,
) {
    let name = if name.is_null() {
        Default::default()
    } else {
        CStr::from_ptr(name).to_string_lossy()
    };
    let reason = reason(why).unwrap_or_else(|| {
        CStr::from_ptr(gtk4::glib::ffi::g_strerror(err))
            .to_string_lossy()
            .into_owned()
    });
    let msgid = if upload != 0 {
        n_("Couldn't upload %1$s: %2$s")
    } else {
        n_("Couldn't download %1$s: %2$s")
    };
    let text = crate::cs(&tr_argv(msgid, &[&name, &reason]));
    crate::ffi::toolbar_show_toast(text.as_ptr());
    crate::msg::play_sound(SOUND_ERROR);
}

/// Why a transfer failed, as [`gtkhx_tasks_failed`] is told it, but for
/// the local file's error.
fn reason(why: c_int) -> Option<String> {
    Some(match why {
        0 => tr("the connection to the server couldn't be opened"),
        1 => tr("the server stopped responding"),
        2 => tr("the connection to the server failed"),
        4 => tr("the server already has a larger file with that name"),
        5 => tr("the server already has a larger file with the name of a file in the folder"),
        _ => return None,
    })
}

/// sound.h's `ERROR`.
const SOUND_ERROR: c_int = 2;

/// [`gtkhx_tasks_subtitle`]'s text, with sizes written by `human`.
fn subtitle(
    pos: u64,
    size: u64,
    run: u64,
    elapsed: i64,
    paused: c_int,
    human: impl Fn(u64) -> String,
) -> String {
    let (p, s) = (human(pos), human(size));
    if size <= 1 && pos == 0 && paused == 0 {
        // A size of 1 is the stand-in for one not known yet (an upload the
        // server hasn't answered): nothing to tell.
        String::new()
    } else if paused == 2 {
        tr_argv("%1$s of %2$s · Paused: the file has changed", &[&p, &s])
    } else if paused == 3 {
        tr_argv("%1$s of %2$s · Paused: a file in it has changed", &[&p, &s])
    } else if paused == 4 {
        tr_argv(
            "%1$s of %2$s · Paused: the folder on the server has changed",
            &[&p, &s],
        )
    } else if paused >= 5 {
        match reason(paused - 5) {
            Some(r) => tr_argv("%1$s of %2$s · Failed: %3$s", &[&p, &s, &r]),
            None => tr_argv("%1$s of %2$s · Failed", &[&p, &s]),
        }
    } else if paused != 0 {
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
    }
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

#[cfg(test)]
mod tests {
    use super::subtitle;

    #[test]
    fn subtitle_tells_what_the_row_knows() {
        // (pos, size, run, elapsed, paused, the line)
        for (pos, size, run, elapsed, paused, want) in [
            (0, 1, 0, 0, 0, ""),              // size not known yet
            (0, 5_000, 0, 3, 0, "0 of 5000"), // waiting its turn
            (1_200, 5_000, 0, 0, 1, "1200 of 5000 · Paused"),
            (
                1_200,
                5_000,
                0,
                0,
                2,
                "1200 of 5000 · Paused: the file has changed",
            ),
            (
                1_200,
                5_000,
                0,
                0,
                3,
                "1200 of 5000 · Paused: a file in it has changed",
            ),
            (
                1_200,
                5_000,
                0,
                0,
                4,
                "1200 of 5000 · Paused: the folder on the server has changed",
            ),
            (
                1_200,
                5_000,
                0,
                0,
                6,
                "1200 of 5000 · Failed: the server stopped responding",
            ),
            (1_200, 5_000, 0, 0, 8, "1200 of 5000 · Failed"), // a local error
            (1_000, 5_000, 1_000, 2, 0, "1000 of 5000 · 500/s · ETA 0:08"),
            (
                0,
                4_000_000,
                100,
                1,
                0,
                "0 of 4000000 · 100/s · ETA 11:06:40",
            ),
        ] {
            let got = subtitle(pos, size, run, elapsed, paused, |n| n.to_string());
            assert_eq!(got, want, "pos {pos}, size {size}, run {run}");
        }
    }
}
