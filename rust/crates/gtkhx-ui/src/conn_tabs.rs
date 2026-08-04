//! `conn_tabs` — the connection tab strip.
//!
//! One tab per open connection, across the top of the toolbar window. This is
//! the visible half of the tab-switched layout (Model A in
//! `docs/multi-connection.md`): selecting a tab moves the session focus and
//! swaps *every* per-connection panel to that connection's content page in one
//! go, while global panels stay put.
//!
//! Two ideas that look alike and are not:
//!
//! - This strip switches **connections**, and swaps whole panel contents.
//! - `chat_tabs` switches **conversations** within the Chat panel, and belongs
//!   to whichever connection is showing.
//!
//! ## Shape
//!
//! An `AdwTabBar` over an `AdwTabView` that is never drawn. The view is only a
//! model here — the tabs' *content* lives in the dock, not in the view — so
//! each page gets an empty placeholder child and the view itself is hidden.
//! Using the pair rather than a hand-rolled button row buys three things that
//! would otherwise all need writing: the attention badge (`AdwTabPage`'s
//! `needs-attention`, the same property `chat_tabs` already uses), keyboard
//! and overflow handling, and autohide — the strip does not appear at all
//! until there are two connections, so a single-connection session looks
//! exactly as it did before this existed.
//!
//! ## What a switch does not fix
//!
//! `dock::show_page` is a no-op for a panel that has no page for the incoming
//! connection, which leaves the *outgoing* connection's content on screen.
//!
//! That is no longer the normal case — every content module owns its state per
//! connection now, so a connection that has opened a role has a page there.
//! What remains is the ordinary one: a role the incoming connection has never
//! opened, or a panel the user has closed. Both are logged under
//! `GTKHX_DEBUG=dock`, because a panel that doesn't change when you switch
//! reads like a switching bug rather than an absence.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};

use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;
// Re-exports gtk4's prelude, so this is the only one needed.
use libadwaita::prelude::*;

use crate::dock;
use crate::dock::ConnKey;
use crate::tr::{tr, tr1};

extern "C" {
    /// `gtkhx_ui_bridge.c` — the connection a session owns. The module
    /// reaches the *key* through `dock` like everything else; this is for the
    /// one place that needs the connection itself, plus the tests, which use
    /// a connection as its own session.
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    /// `gtkhx-core` — the connection's socket, or 0 when it has none.
    /// Dereferences, so never hand it NULL.
    fn hx_conn_fd(htlc: *const c_void) -> std::os::raw::c_int;
    /// `gtkutil.c` — the server's advertised name, or its address. Owned by
    /// the caller.
    fn hx_session_label(sess: *mut c_void) -> *mut c_char;
    /// `session_registry.c` — move the focus. FALSE if the session isn't in
    /// the collection, which leaves the focus alone.
    fn hx_session_set_active(sess: *mut c_void) -> glib::ffi::gboolean;
    /// `debug.c` — one `GTKHX_DEBUG` category line.
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
    /// `gtkutil.c` — repaint the focus-following chrome for whichever
    /// connection is now selected.
    fn hx_chrome_refresh();
    /// `gtkhx.c` — disconnect a session, destroy its content page in every
    /// per-connection panel, drop it from the registry and free it.
    fn hx_session_close(sess: *mut c_void);
    /// `session_registry.c` — the live session on this connection, or NULL if
    /// it has been closed. The safe way to keep a reference across a turn of
    /// the main loop.
    fn hx_session_with_serial(serial: u16) -> *mut c_void;
}

fn debug(msg: &str) {
    let cat = crate::cs("dock");
    let m = crate::cs(msg);
    unsafe { debug_log_str(cat.as_ptr(), m.as_ptr()) };
}

fn serial_of(sess: *mut c_void) -> ConnKey {
    dock::key_for_session(sess)
}

#[derive(Default)]
struct ConnTabs {
    /// The widget the toolbar embeds, holding the bar and the hidden view.
    /// Held here because it is what keeps the tree alive: the C caller gets a
    /// borrowed pointer and adds its own reference, exactly as it does for the
    /// chat tab strip.
    strip: Option<gtk::Box>,
    view: Option<adw::TabView>,
    /// serial → its tab. The other direction — a tab to the session it belongs
    /// to — used to be a second map of raw `session *`, which was safe only
    /// while sessions were immortal. They aren't: closing a tab frees one. So
    /// the session is looked up from the serial through the registry at the
    /// moment it is needed, and a connection that has gone answers NULL
    /// instead of handing back a pointer into freed memory.
    tabs: HashMap<ConnKey, adw::TabPage>,
}

thread_local! {
    static STATE: RefCell<ConnTabs> = RefCell::new(ConnTabs::default());
}

/// Clone the view out of the cell, holding the borrow only for the clone — so
/// the handle can drive adw calls that re-enter our own signal handlers.
fn view() -> Option<adw::TabView> {
    STATE.with(|s| s.borrow().view.clone())
}

fn strip() -> Option<gtk::Box> {
    STATE.with(|s| s.borrow().strip.clone())
}

fn tab_for(conn: ConnKey) -> Option<adw::TabPage> {
    STATE.with(|s| s.borrow().tabs.get(&conn).cloned())
}

/// The serial a tab belongs to. Stored as qdata rather than found by scanning
/// the map, so the selection handler doesn't need a borrow to answer it.
const CONN_KEY: &str = "conn-tab-serial";

fn set_tab_conn(page: &adw::TabPage, conn: ConnKey) {
    // Safety: single-threaded; the key is module-private and always holds a
    // ConnKey, so the typed read below is sound.
    unsafe { page.set_data(CONN_KEY, conn) };
}

fn tab_conn(page: &adw::TabPage) -> Option<ConnKey> {
    // Safety: matched type with `set_tab_conn`; the qdata is owned by the page
    // for its lifetime and copied out immediately.
    unsafe { page.data::<ConnKey>(CONN_KEY).map(|p| *p.as_ref()) }
}

/// Show `conn`'s page in every per-connection panel.
///
/// A panel with no page for this connection is skipped, not blanked — see the
/// module note. The debug line is the only trace of it, and it is worth having
/// because the symptom (a panel still showing the connection you switched away
/// from) reads like a switching bug rather than a missing page.
fn swap_panels(conn: ConnKey) {
    let page = conn.to_string();
    for id in dock::PER_CONNECTION {
        if !dock::show_page(id, &page) {
            // Two causes, worth telling apart: the role's panel isn't in the
            // dock at all (the user closed it), or it is but this connection
            // has no page in it (its content module is still a singleton —
            // see the module note). Only the second is a missing feature.
            let why = if dock::is_embedded(id) {
                "no page for this connection"
            } else {
                "panel not in the dock"
            };
            debug(&format!("connection {conn}: '{id}' left as it was — {why}"));
        }
    }
}

/// Close a connection: disconnect it, destroy its content, drop its tab.
///
/// The order matters. The focus moves off it *first*, because everything the
/// teardown touches — the chrome, the panels — asks which connection is
/// selected, and a session being dismantled is the wrong answer. Then
/// `hx_session_close` does the wire-side teardown and removes the pages, each
/// of which runs its content module's destroy handler. Only then does the tab
/// go.
///
/// A live connection asks first. Closing its tab disconnects it, which is not
/// what a misplaced click on an [X] should do — and the tab strip only appears
/// once there are two connections, so the button is small, adjacent to the one
/// you are looking at, and easy to hit by accident. `close-page` is designed
/// for exactly this: the handler stops the close and finishes it later, so the
/// question can be asked without blocking.
///
/// The last connection can't be closed, and needs no check: `AdwTabBar`
/// autohides below two pages, so there is no close button to press.
fn on_close_page(view: &adw::TabView, page: &adw::TabPage) -> glib::Propagation {
    let Some(conn) = tab_conn(page) else {
        // No serial on the page — nothing to close against. Decline rather
        // than drop a tab whose connection we can't identify.
        view.close_page_finish(page, false);
        return glib::Propagation::Stop;
    };
    let sess = unsafe { hx_session_with_serial(conn) };
    if sess.is_null() {
        // The connection is already gone. Its tab shouldn't be here, but
        // taking it away is closer to right than refusing to.
        STATE.with(|s| {
            s.borrow_mut().tabs.remove(&conn);
        });
        view.close_page_finish(page, true);
        return glib::Propagation::Stop;
    }

    // A connection with a socket is one this would disconnect, so ask. Having
    // a socket rather than being logged in, matching what the Disconnect
    // button acts on: a connection still handshaking is just as much something
    // the user would not expect a stray click to throw away.
    if unsafe { hx_conn_fd(gtkhx_session_htlc(sess)) } != 0 {
        confirm_close(view, page, conn);
        return glib::Propagation::Stop;
    }

    finish_close(view, page, conn, sess);
    glib::Propagation::Stop
}

/// Ask before disconnecting, then finish or abandon the close.
///
/// Everything is re-resolved in the response handler rather than captured:
/// between asking and answering, the connection can have dropped on its own,
/// or the tab can have gone. The serial is the only thing safe to hold across
/// that gap, which is the same reason the strip indexes on one.
fn confirm_close(view: &adw::TabView, page: &adw::TabPage, conn: ConnKey) {
    let label = unsafe { hx_session_with_serial(conn) };
    let name = if label.is_null() {
        tr("this server")
    } else {
        unsafe {
            let p = hx_session_label(label);
            let s = crate::cstr(p);
            glib::ffi::g_free(p as *mut _);
            s
        }
    };

    let dialog = adw::AlertDialog::new(
        Some(&tr("Disconnect from this server?")),
        Some(&tr1("Closing this tab disconnects from %s.", &name)),
    );
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("close", &tr("_Disconnect"));
    dialog.set_response_appearance("close", adw::ResponseAppearance::Destructive);
    dialog.set_default_response(Some("cancel"));
    dialog.set_close_response("cancel");

    let anchor = view.clone();
    let view = view.clone();
    let page = page.clone();
    dialog.connect_response(None, move |_dlg, response| {
        if response != "close" {
            // Declining has to be reported too: an unfinished close leaves the
            // page stuck, refusing every later attempt to shut it.
            view.close_page_finish(&page, false);
            return;
        }
        // Re-resolve: the connection may have dropped while the question was
        // open, in which case there is nothing to disconnect and the tab
        // should simply go.
        let sess = unsafe { hx_session_with_serial(conn) };
        if sess.is_null() {
            STATE.with(|s| {
                s.borrow_mut().tabs.remove(&conn);
            });
            view.close_page_finish(&page, true);
            return;
        }
        finish_close(&view, &page, conn, sess);
    });

    dialog.present(Some(&anchor));
}

/// Tear the connection down and drop its tab. The second half of
/// [`on_close_page`], reached directly when nothing needed asking.
fn finish_close(view: &adw::TabView, page: &adw::TabPage, conn: ConnKey, sess: *mut c_void) {
    // Move the focus off it if it is the one selected. Selecting a neighbour
    // runs the ordinary switch — focus, panels, chrome — so by the time the
    // teardown starts, nothing is pointing at the session being closed.
    if view.selected_page().as_ref() == Some(page) {
        let n = view.n_pages();
        let pos = view.page_position(page);
        let neighbour = if pos + 1 < n { pos + 1 } else { pos - 1 };
        if neighbour >= 0 && neighbour < n {
            view.set_selected_page(&view.nth_page(neighbour));
        }
    }

    unsafe { hx_session_close(sess) };

    STATE.with(|s| {
        s.borrow_mut().tabs.remove(&conn);
    });
    debug(&format!("closed connection {conn}"));

    view.close_page_finish(page, true);
}

/// A tab was dragged out of the strip and dropped on the desktop.
///
/// `AdwTabView` asks here for the view to move the page into, and **the answer
/// may not be nothing**: libadwaita logs a critical for a NULL and then leaves
/// the drag half-finished, which crashes inside GTK's crossing-event
/// synthesis when the drop target is resolved against a tab that is being torn
/// down. Not connecting the signal at all has exactly that effect, which is
/// what this used to do.
///
/// There is no way to refuse a detach — no `can-detach` on the page or the
/// bar — so the only answer that keeps the page somewhere valid is the view it
/// is already in. The drop becomes a no-op and the tab stays put.
///
/// That is a placeholder for real detaching, not a decision against it. A
/// detached connection needs its own window with its own dock, and the dock is
/// a process singleton today: one `PanelDock`, four frame globals, and a panel
/// registry keyed on the panel id with no connection dimension, so two windows
/// would collide on every one of the six panel ids. See
/// docs/multi-connection.md — this is the difference between layout Model A,
/// which is what exists, and Model B.
fn on_create_window(view: &adw::TabView) -> Option<adw::TabView> {
    debug("tab dropped outside the strip; detaching is not implemented");
    Some(view.clone())
}

/// Tab selected: move the focus, swap every per-connection panel, and treat
/// the selection as acknowledging whatever the tab was flagged for.
fn on_selected_page_changed(view: &adw::TabView) {
    let Some(page) = view.selected_page() else {
        return;
    };
    let Some(conn) = tab_conn(&page) else {
        return;
    };

    page.set_needs_attention(false);

    let sess = unsafe { hx_session_with_serial(conn) };
    if sess.is_null() {
        debug(&format!(
            "connection {conn}: selected a tab with no session"
        ));
        return;
    }

    // Focus first, then content. `hx_active_session()` is what the panels'
    // own handlers read, so a panel that rebuilds anything during the swap
    // must already see the connection it is being switched to.
    if unsafe { hx_session_set_active(sess) } == glib::ffi::GFALSE {
        debug(&format!(
            "connection {conn}: hx_session_set_active refused — session not in registry"
        ));
        return;
    }
    swap_panels(conn);
    // The status bar, window title, tray flag and toolbar buttons are one set
    // shared by every connection, so they show whichever one is selected —
    // which means a switch has to repaint them. Nothing else will: they are
    // written by connection-state changes, and switching tabs changes which
    // connection they are *about* without any connection changing state.
    unsafe { hx_chrome_refresh() };
}

/// Build the strip once and return the widget the toolbar embeds. Idempotent —
/// a second call returns the same widget rather than a second strip.
///
/// The returned box holds the tab bar and, hidden, the tab view backing it.
/// The view has to be in the tree for the bar to track it, but it is never
/// drawn: it is a model, and the tabs' real content is in the dock.
///
/// Transfer none. This module keeps the owning reference — without one the box
/// would be destroyed as this function returned — and the caller adds its own
/// when it packs the widget, the same contract `gtkhx_chat_tabs_init` has.
///
/// # Safety
/// Called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_conn_tabs_new() -> *mut gtk::ffi::GtkWidget {
    crate::ensure_gtk_init();

    if let Some(existing) = strip() {
        return existing.as_ptr() as *mut gtk::ffi::GtkWidget;
    }

    let view = adw::TabView::new();
    view.connect_selected_page_notify(on_selected_page_changed);
    view.connect_close_page(on_close_page);
    view.connect_create_window(on_create_window);

    let bar = adw::TabBar::new();
    bar.set_view(Some(&view));
    crate::wheel_switches_tabs(&bar, &view);
    // The default, set explicitly because the single-connection appearance
    // depends on it: with one page (and none pinned) the bar hides itself, so
    // a session with one connection looks as it did before this existed.
    bar.set_autohide(true);

    let strip = gtk::Box::new(gtk::Orientation::Vertical, 0);
    strip.append(&bar);
    // In the tree so the bar tracks it, never drawn: the content is in the
    // dock, and these pages' children are placeholders.
    view.set_visible(false);
    strip.append(&view);

    let ptr = strip.as_ptr() as *mut gtk::ffi::GtkWidget;
    STATE.with(|s| {
        let mut st = s.borrow_mut();
        st.view = Some(view);
        st.strip = Some(strip);
    });
    ptr
}

/// Add a tab for `sess`, titled `title`, and make it the selected one.
///
/// Selecting it is what makes a new connection the one you are looking at,
/// which is also what every panel's content build assumes — they resolve
/// through `hx_active_session()` for anything the caller didn't hand them.
///
/// # Safety
/// `sess` is a live `session *`; `title` is NULL or a valid C string. Called
/// on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_conn_tabs_add(sess: *mut c_void, title: *const c_char) {
    let Some(view) = view() else { return };
    let conn = serial_of(sess);

    if let Some(existing) = tab_for(conn) {
        view.set_selected_page(&existing);
        return;
    }

    // The view is a model: its pages' children are never drawn, so an empty
    // box is the honest placeholder. The content is in the dock.
    let page = view.append(&gtk::Box::new(gtk::Orientation::Vertical, 0));
    let t = crate::cstr(title);
    let fallback = tr("Connection");
    page.set_title(if t.is_empty() { &fallback } else { &t });
    set_tab_conn(&page, conn);

    STATE.with(|s| {
        s.borrow_mut().tabs.insert(conn, page.clone());
    });

    // `append` selects the page itself when the view was empty, and it does
    // so *before* the two lines above have run — so the notify handler saw a
    // page with no serial on it and bailed, and this `set_selected_page` is a
    // no-op because the page is already selected. The result was that the
    // first tab silently skipped the focus move and the panel swap.
    //
    // So: select, then run the handler by hand if selecting didn't. Doing it
    // unconditionally would double-run it for every tab after the first.
    let was_selected = view.selected_page().as_ref() == Some(&page);
    view.set_selected_page(&page);
    if was_selected {
        on_selected_page_changed(&view);
    }
}

/// Select `sess`'s tab, as if the user had clicked it — which moves the focus
/// and swaps every per-connection panel, through the same handler.
///
/// # Safety
/// `sess` is a live `session *`. Called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_conn_tabs_select(sess: *mut c_void) {
    if let (Some(view), Some(page)) = (view(), tab_for(serial_of(sess))) {
        view.set_selected_page(&page);
    }
}

/// Close `sess`'s tab as if the user had clicked its close button — the same
/// path, including the disconnect and the panel teardown.
///
/// # Safety
/// `sess` is a live `session *`. Called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_conn_tabs_close(sess: *mut c_void) {
    if let (Some(view), Some(page)) = (view(), tab_for(serial_of(sess))) {
        view.close_page(&page);
    }
}

/// Retitle a connection's tab.
///
/// A tab is created before there is anything to call it, so this is how it
/// gets a real name. Today the only caller is the connection-state handler in
/// `gtkhx.c`, which uses the host being connected to; a server's advertised
/// name would be better and is not available at any point the client can rely
/// on.
///
/// # Safety
/// `htlc` is a live connection; `title` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_conn_tabs_set_title(htlc: *mut c_void, title: *const c_char) {
    if let Some(page) = tab_for(dock::conn_key(htlc)) {
        let t = crate::cstr(title);
        if !t.is_empty() {
            page.set_title(&t);
        }
    }
}

/// Flag or clear a connection's tab.
///
/// Setting is ignored for the connection already selected: its tab is the one
/// the user is looking at, so a badge on it would say "look here" about the
/// thing they are already looking at. Clearing always applies, since the
/// caller may be acknowledging something on the selected tab.
///
/// # Safety
/// `htlc` is NULL or a live connection. Called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_conn_tabs_set_attention(
    htlc: *mut c_void,
    state: glib::ffi::gboolean,
) {
    let on = state != glib::ffi::GFALSE;
    let Some(page) = tab_for(dock::conn_key(htlc)) else {
        return;
    };
    if on && view().and_then(|v| v.selected_page()).as_ref() == Some(&page) {
        return;
    }
    page.set_needs_attention(on);
}

/// Mark `conn`'s tab as the one holding the microphone, clearing the mark from
/// every other tab. `None` clears it everywhere.
///
/// Deliberately not voice-aware: this module knows nothing about rooms or
/// runtimes, only that one connection is flagged at a time. The voice arbiter
/// is what decides which, and calls this when the answer changes — so a build
/// without voice simply never calls it.
///
/// The mark matters because voice is exclusive. Without it, "why did my
/// microphone stop?" has no answer on screen, and finding your way back to the
/// conversation you were in means clicking through every tab.
pub fn set_voice_indicator(conn: Option<ConnKey>) {
    // Snapshot before touching any of them, like every other accessor here:
    // the adw setters below emit property notifies, and a handler that reached
    // back into this module would find the cell already borrowed.
    let tabs: Vec<(ConnKey, adw::TabPage)> = STATE.with(|s| {
        s.borrow()
            .tabs
            .iter()
            .map(|(k, p)| (*k, p.clone()))
            .collect()
    });
    if tabs.is_empty() {
        // Nothing to mark, and nothing GLib-flavoured attempted — which is
        // what lets the arbiter call this unconditionally, including from a
        // unit test with no display and no type system initialised.
        return;
    }

    let icon = gtk::gio::ThemedIcon::new("audio-input-microphone-symbolic");
    for (key, page) in tabs {
        if Some(key) == conn {
            page.set_indicator_icon(Some(&icon));
            page.set_indicator_tooltip(&tr("Voice chat is active on this connection"));
        } else {
            page.set_indicator_icon(gtk::gio::Icon::NONE);
            page.set_indicator_tooltip("");
        }
    }
}

/// How many connections the strip is showing. For the tests, and for callers
/// that want to know whether the strip is doing anything at all.
#[no_mangle]
pub extern "C" fn gtkhx_conn_tabs_count() -> u32 {
    STATE.with(|s| s.borrow().tabs.len() as u32)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    extern "C" {
        /// `gtkhx-core` — a fresh connection, with the next serial.
        fn hx_conn_new() -> *mut c_void;
    }

    /// How many times the selection handler has reached `hx_session_set_active`.
    fn calls() -> u32 {
        crate::options_test_stubs::SET_ACTIVE_CALLS.load(std::sync::atomic::Ordering::Relaxed)
    }

    /// Build the strip, add two connections, and check the three properties
    /// the tab-switched layout rests on: the strip hides itself at one
    /// connection, each connection gets exactly one tab, and a tab carries
    /// the serial the dock names pages after.
    ///
    /// Driven by `crate::gtk_tests` rather than being a `#[test]` — see there.
    ///
    /// The selection handler *does* run — appending a tab selects it — so
    /// `hx_session_set_active` and the dock bridge are stubbed to reach it.
    /// What the assertions stay away from is its *effects*, which are entirely
    /// those stubs: whether the focus moved and whether the panels swapped are
    /// questions only the real registry and a real dock can answer. Asserting
    /// on them here would be asserting on `options_test_stubs`. So this checks
    /// the half that is this module's own — the index, the keys, and the
    /// widget wiring — and leaves the C side to the integration surface.
    pub(crate) fn check_strip_indexes_connections() {
        let strip: gtk::Widget = unsafe { glib::translate::from_glib_none(gtkhx_conn_tabs_new()) };
        let bar = strip
            .first_child()
            .and_downcast::<adw::TabBar>()
            .expect("tab bar is the strip's first child");

        // Idempotent: a second call is the same strip, not a second one.
        let again: gtk::Widget = unsafe { glib::translate::from_glib_none(gtkhx_conn_tabs_new()) };
        assert_eq!(strip, again, "gtkhx_conn_tabs_new built a second strip");

        let view = view().expect("view built");
        assert!(
            bar.is_autohide(),
            "autohide off — the strip would show at one"
        );

        // Two connections, real ones, from the real allocator — so the
        // serials the strip keys on are the ones production would hand it.
        // They stand in for sessions too: the stubbed `gtkhx_session_htlc` is
        // an identity, so a connection is its own session here. Leaked
        // deliberately; the strip holds the pointers for the process's life
        // and there is nothing to free them for.
        let (a, b) = unsafe { (hx_conn_new(), hx_conn_new()) };
        assert_ne!(serial_of(a), serial_of(b), "two connections, one serial");
        // Into the stand-in registry, because the strip finds a session by
        // asking for it rather than by keeping a pointer.
        crate::options_test_stubs::register_session(a);
        crate::options_test_stubs::register_session(b);
        unsafe {
            gtkhx_conn_tabs_add(a, c"Server A".as_ptr());
            assert_eq!(gtkhx_conn_tabs_count(), 1);
            assert!(!bar.is_tabs_revealed(), "strip visible at one connection");

            gtkhx_conn_tabs_add(b, c"Server B".as_ptr());
        }

        // The selection handler has to have run for *both* tabs, and the
        // first is the one that catches a regression: `append` selects a page
        // itself when the view was empty, and it does so before the tab has
        // been given its serial or put in the map — so the handler sees a
        // blank page and returns, and the explicit `set_selected_page` after
        // it is a no-op because the page is already selected. The symptom is
        // that the first connection silently never gets its focus move or its
        // panel swap.
        //
        // Asserting on final state would not catch that: the serial is on the
        // page either way by the time this runs. Only the call count can tell
        // whether the handler fired.
        assert_eq!(
            calls(),
            2,
            "the selection handler didn't run once per tab — the first tab's \
             focus move and panel swap were skipped"
        );
        assert_eq!(
            tab_conn(&view.nth_page(0)),
            Some(serial_of(a)),
            "first tab has no serial"
        );
        assert_eq!(gtkhx_conn_tabs_count(), 2);
        assert_eq!(view.n_pages(), 2);

        // Adding the same session again selects its tab instead of doubling it.
        unsafe { gtkhx_conn_tabs_add(a, c"Server A".as_ptr()) };
        assert_eq!(gtkhx_conn_tabs_count(), 2, "duplicate session added a tab");
        assert_eq!(
            view.selected_page().map(|p| p.title()),
            Some("Server A".into()),
            "re-adding a session didn't select its tab"
        );

        // The serial on the tab is what swap_panels turns into a page name.
        let tab_a = tab_for(serial_of(a)).expect("A indexed");
        assert_eq!(tab_conn(&tab_a), Some(serial_of(a)));

        // Retitle after login.
        unsafe { gtkhx_conn_tabs_set_title(gtkhx_session_htlc(b), c"Real Name".as_ptr()) };
        assert_eq!(
            tab_for(serial_of(b)).map(|p| p.title()),
            Some("Real Name".into())
        );

        // Badges: not on the selected tab, yes on the other, cleared either way.
        let (sel, other) = (serial_of(a), serial_of(b));
        unsafe {
            gtkhx_conn_tabs_set_attention(gtkhx_session_htlc(a), glib::ffi::GTRUE);
            gtkhx_conn_tabs_set_attention(gtkhx_session_htlc(b), glib::ffi::GTRUE);
        }
        assert!(
            !tab_for(sel).unwrap().needs_attention(),
            "badged the connection the user is already looking at"
        );
        assert!(
            tab_for(other).unwrap().needs_attention(),
            "background connection went unbadged"
        );
        unsafe { gtkhx_conn_tabs_set_attention(gtkhx_session_htlc(b), glib::ffi::GFALSE) };
        assert!(!tab_for(other).unwrap().needs_attention());

        // A flagged tab's title is bold. The flag is a libadwaita property and
        // the weight comes from a CSS rule naming libadwaita's own classes, so
        // this asserts on what is actually rendered — a selector that stopped
        // matching would leave `needs_attention` perfectly true and nothing
        // visibly different.
        unsafe { gtkhx_conn_tabs_set_attention(gtkhx_session_htlc(b), glib::ffi::GTRUE) };
        fn title_weight(w: &gtk::Widget, want_attention: bool) -> Option<String> {
            if w.type_().name() == "AdwTab" && w.has_css_class("needs-attention") == want_attention
            {
                let mut c = w.first_child();
                while let Some(ch) = c {
                    if ch.has_css_class("tab-title") {
                        // The name of the weight, not its number: the
                        // comparison only needs the two to differ, and
                        // `Bold` says what it is asserting.
                        return Some(format!(
                            "{:?}",
                            ch.pango_context().font_description()?.weight()
                        ));
                    }
                    c = ch.next_sibling();
                }
            }
            let mut c = w.first_child();
            while let Some(ch) = c {
                if let Some(v) = title_weight(&ch, want_attention) {
                    return Some(v);
                }
                c = ch.next_sibling();
            }
            None
        }
        let root = bar.upcast_ref::<gtk::Widget>();
        let flagged = title_weight(root, true).expect("no flagged tab title");
        let plain = title_weight(root, false).expect("no unflagged tab title");
        assert_ne!(
            flagged, plain,
            "a tab needing attention renders no differently from one that \
             doesn't — the CSS selector has stopped matching"
        );
        assert_eq!(flagged, "Bold", "flagged tab title is not bold");
        unsafe { gtkhx_conn_tabs_set_attention(gtkhx_session_htlc(b), glib::ffi::GFALSE) };

        // A tab dragged onto the desktop. libadwaita asks for a view to move
        // the page into and will not take nothing for an answer — a NULL
        // leaves the drag half-finished and crashes in GTK. Answering with
        // the view the page is already in makes the drop a no-op, and what
        // has to survive that round trip is the page's connection key: it is
        // qdata, and the strip's entire tab-to-connection mapping is built on
        // it. Driven through `transfer_page` because that is what libadwaita
        // does with the returned view.
        let dropped = tab_for(other).unwrap();
        let same = on_create_window(&view).unwrap();
        assert_eq!(same, view, "detach handler pointed somewhere else");
        view.transfer_page(&dropped, &same, view.page_position(&dropped));
        assert_eq!(gtkhx_conn_tabs_count(), 2, "the no-op detach lost a tab");
        assert_eq!(
            tab_conn(&dropped),
            Some(other),
            "the page lost its connection key crossing the transfer"
        );
        assert_eq!(tab_for(other).as_ref(), Some(&dropped), "index went stale");

        // Closing. Three things have to happen together: the tab goes, the
        // session is told, and the connection stops resolving. The first two
        // are the pair that was broken — closing was refused outright, so the
        // button looked live and did nothing. The third is what makes the
        // strip safe now that a close *frees* the session: it keeps serials
        // rather than pointers precisely so a closed connection answers
        // nothing instead of answering with freed memory.
        let closes = crate::options_test_stubs::SESSION_CLOSE_CALLS
            .load(std::sync::atomic::Ordering::Relaxed);
        unsafe { gtkhx_conn_tabs_close(b) };
        assert_eq!(gtkhx_conn_tabs_count(), 1, "closing left the tab behind");
        assert_eq!(view.n_pages(), 1);
        assert!(tab_for(other).is_none(), "closed connection still indexed");
        assert_eq!(
            crate::options_test_stubs::SESSION_CLOSE_CALLS
                .load(std::sync::atomic::Ordering::Relaxed),
            closes + 1,
            "the tab went but the connection was never torn down"
        );
        assert!(
            unsafe { crate::options_test_stubs::hx_session_with_serial(other) }.is_null(),
            "a closed connection still resolves to a session"
        );
        // Back below two, so the strip hides itself again.
        assert!(!bar.is_tabs_revealed(), "strip visible at one connection");
        // And the survivor is what the user is left looking at.
        assert_eq!(tab_conn(&view.selected_page().unwrap()), Some(serial_of(a)));
    }
}
