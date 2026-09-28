//! Files browser window.
//!
//! The two-panel browser is a window of its own, one per connection, rather
//! than a dock panel: a two-panel file manager wants more width than a dock
//! frame gives it, and it is used in bursts — browse, queue transfers, leave —
//! while the transfers carry on in the Tasks panel. Closing the window closes
//! the browser.
//!
//! This module owns the window: one per connection, raised if already open,
//! sized from the last one the user left, and closed when its connection goes
//! away. Inside it, `browser` holds the two panels and every operation between
//! them, `panel` is one panel, `dialogs` and `dnd` are the browser's dialogs
//! and drag and drop, `row` is what a row shows, and `complete` is the path
//! entries' completion. The providers underneath are still C, reached through
//! `provider`; `local` is the local one's folder listing.

mod browser;
pub mod complete;
mod dialogs;
mod dnd;
mod local;
pub(crate) mod panel;
pub(crate) mod provider;
mod row;

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void};

use glib::translate::from_glib_none;
use gtk::gio;
use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;
use libadwaita::prelude::*;

use crate::dock::{self, ConnKey};
use crate::tr::tr;

extern "C" {
    /// `gtkutil.c` — what to call a session: the server's name once it has
    /// sent one, else the address. Newly allocated.
    fn hx_session_label(sess: *mut c_void) -> *mut c_char;
    fn gtkhx_get_application() -> *mut gio::ffi::GApplication;
    /// `dock_layout.c` — the last size a named window was left at; FALSE
    /// when none was saved.
    fn dock_layout_get_window_size(
        name: *const c_char,
        w: *mut c_int,
        h: *mut c_int,
    ) -> glib::ffi::gboolean;
    fn dock_layout_set_window_size(name: *const c_char, w: c_int, h: c_int);
}

/// The size a Files window opens at when none has been saved: enough for two
/// panels to show name, size and date side by side.
const DEFAULT_W: i32 = 1000;
const DEFAULT_H: i32 = 640;

thread_local! {
    /// The open window for each connection. Weak, so the map never keeps a
    /// closed window alive; an entry whose window is gone reads as absent.
    static WINDOWS: RefCell<HashMap<ConnKey, glib::WeakRef<adw::Window>>> =
        RefCell::new(HashMap::new());
}

fn window_for(key: ConnKey) -> Option<adw::Window> {
    WINDOWS.with_borrow(|m| m.get(&key).and_then(|w| w.upgrade()))
}

fn window_title(sess: *mut c_void) -> String {
    let label = unsafe {
        let p = hx_session_label(sess);
        if p.is_null() {
            String::new()
        } else {
            let s = std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned();
            glib::ffi::g_free(p as *mut c_void);
            s
        }
    };
    if label.is_empty() {
        tr("Files")
    } else {
        // TRANSLATORS: The Files window's title. %s is the server's name,
        // or its address before it has sent one.
        crate::tr::tr1("Files — %s", &label)
    }
}

/// Open (or raise) the Files window for `sess` — the session whose files it
/// lists.
///
/// # Safety
/// Called on the GTK main thread; `sess` is a `session *` or NULL.
#[no_mangle]
pub unsafe extern "C" fn open_files_browser(sess: *mut c_void) {
    crate::ensure_gtk_init();

    let key = dock::key_for_session(sess);
    if let Some(win) = window_for(key) {
        // The server may have named itself since the window opened.
        win.set_title(Some(&window_title(sess)));
        win.present();
        return;
    }

    // Every operation routes through the session's connection, and a NULL one
    // has no meaning to fall back on: disconnected is a session whose socket
    // is closed.
    if sess.is_null() {
        glib::g_critical!("gtkhx", "open_files_browser: no session");
        return;
    }
    let built = browser::build(sess);

    let win = adw::Window::new();
    win.set_title(Some(&window_title(sess)));
    let name = crate::cs("files");
    let (mut w, mut h) = (0, 0);
    if dock_layout_get_window_size(name.as_ptr(), &mut w, &mut h) != glib::ffi::GFALSE {
        win.set_default_size(w, h);
    } else {
        win.set_default_size(DEFAULT_W, DEFAULT_H);
    }

    let header = adw::HeaderBar::new();
    header.pack_start(&built.header_start);
    header.pack_end(&built.header_end);
    let view = adw::ToolbarView::new();
    view.add_top_bar(&header);
    view.set_content(Some(&built.content));
    win.set_content(Some(&view));

    // Registered with the application so it counts as one of its windows
    // (dialogs parent to it, and the app doesn't consider itself idle).
    let app = gtkhx_get_application();
    if !app.is_null() {
        let app: gio::Application = from_glib_none(app);
        if let Ok(app) = app.downcast::<gtk::Application>() {
            app.add_window(&win);
        }
    }

    // The app-wide accelerators (Ctrl+W close, Ctrl+Q quit, …) on the window
    // itself: Ctrl+W acts on a GtkWindow, and the content box can't be one.
    crate::ffi::init_keyaccel(win.as_ptr() as *mut crate::ffi::GtkWidget);

    // Remember the size as the user changes it, for the next one. On every
    // change rather than at close: a connection closing destroys the window
    // without a close-request, and quitting destroys nothing at all. The
    // layout file's save is debounced, so a drag is one write.
    let save_size = |win: &adw::Window| {
        let (w, h) = win.default_size();
        if w > 0 && h > 0 {
            let name = crate::cs("files");
            unsafe { dock_layout_set_window_size(name.as_ptr(), w, h) };
        }
    };
    win.connect_default_width_notify(save_size);
    win.connect_default_height_notify(save_size);

    // Forget this window, and only this one: a destroy that lands late must
    // not remove a newer window's entry for the same connection.
    let weak = win.downgrade();
    win.connect_destroy(move |_| {
        WINDOWS.with_borrow_mut(|m| {
            let ours = m
                .get(&key)
                .is_some_and(|w| w.upgrade().is_none() || w.upgrade() == weak.upgrade());
            if ours {
                m.remove(&key);
            }
        });
    });

    WINDOWS.with_borrow_mut(|m| {
        m.insert(key, win.downgrade());
    });
    win.present();
}

/// Retitle `sess`'s Files window from what the session is now called — the
/// server's name, once login has delivered it. No-op without a window.
///
/// # Safety
/// Called on the GTK main thread; `sess` is a `session *` or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_window_refresh_title(sess: *mut c_void) {
    if let Some(win) = window_for(dock::key_for_session(sess)) {
        win.set_title(Some(&window_title(sess)));
    }
}

/// Close this connection's Files window, if it has one. The connection is
/// going away, and the browser's teardown (on its content's destroy) has to
/// run while the session is still intact.
pub(crate) fn close_for_session(sess: *mut c_void) {
    if let Some(win) = window_for(dock::key_for_session(sess)) {
        win.destroy();
    }
}
