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
//! That is every panel, today. All six content modules still keep their state
//! in one process-global slot, so `dock::claim_singleton` hands each role to
//! the first connection that asks and refuses the rest — which means a second
//! connection has no pages to switch to and the dock does not visibly change
//! when you select its tab. Undoing that, module by module, is all of M4g. The
//! switch logs each refusal under `GTKHX_DEBUG=dock` rather than papering over
//! it.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};

use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use libadwaita as adw;

use crate::dock;
use crate::tr::tr;

extern "C" {
    /// `gtkhx_ui_bridge.c` — the connection a session owns.
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    /// `gtkhx-core` — that connection's process-unique serial. NULL reads as 0.
    fn hx_conn_serial(htlc: *const c_void) -> u16;
    /// `session_registry.c` — move the focus. FALSE if the session isn't in
    /// the collection, which leaves the focus alone.
    fn hx_session_set_active(sess: *mut c_void) -> glib::ffi::gboolean;
    /// `debug.c` — one `GTKHX_DEBUG` category line.
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

fn debug(msg: &str) {
    let cat = crate::cs("dock");
    let m = crate::cs(msg);
    unsafe { debug_log_str(cat.as_ptr(), m.as_ptr()) };
}

/// A connection's serial — the key everything here is indexed by, and the same
/// one the dock uses for its page names.
type ConnKey = u16;

fn serial_of(sess: *mut c_void) -> ConnKey {
    unsafe { hx_conn_serial(gtkhx_session_htlc(sess)) }
}

#[derive(Default)]
struct ConnTabs {
    /// The widget the toolbar embeds, holding the bar and the hidden view.
    /// Held here because it is what keeps the tree alive: the C caller gets a
    /// borrowed pointer and adds its own reference, exactly as it does for the
    /// chat tab strip.
    strip: Option<gtk::Box>,
    view: Option<adw::TabView>,
    /// serial → its tab, and serial → its session. Two maps rather than one of
    /// pairs because the lookups go in both directions: a badge arrives with a
    /// connection and wants the tab, a selection arrives with a tab and wants
    /// the session.
    ///
    /// The session pointers are raw and unowned, and nothing here can tell
    /// whether one is still good. That is safe *only* because sessions are
    /// immortal: `session_registry.c` has no remove, and `on_close_page`
    /// refuses to close a tab, so an entry once written is never stale. M6 is
    /// what breaks that — the moment a connection can be closed, this needs an
    /// invalidation path, and the tab removal is where it goes.
    tabs: HashMap<ConnKey, adw::TabPage>,
    sessions: HashMap<ConnKey, *mut c_void>,
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

/// Refuse to close a connection tab.
///
/// The button is there because `AdwTabView` puts one on every page, and there
/// is nothing behind it yet: closing a connection means disconnecting it,
/// tearing down its panels' pages and dropping its session from the registry,
/// and none of that exists — the registry has no remove, and a page removed
/// from a panel takes the content module's teardown with it. Declining is the
/// only honest answer until M6 gives a connection a close path; silently
/// removing the tab would strand a live session with no way back to it.
fn on_close_page(view: &adw::TabView, page: &adw::TabPage) -> glib::Propagation {
    debug("connection tabs can't be closed yet — no disconnect path (M6)");
    view.close_page_finish(page, false);
    glib::Propagation::Stop
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

    // Drop the borrow before anything that can re-enter: hx_session_set_active
    // is C, and swap_panels goes through the dock bridge into libpanel.
    let sess = STATE.with(|s| s.borrow().sessions.get(&conn).copied());
    let Some(sess) = sess else {
        return;
    };

    // Focus first, then content. `hx_active_session()` is what the panels'
    // own handlers read, so a panel that rebuilds anything during the swap
    // must already see the connection it is being switched to.
    unsafe { hx_session_set_active(sess) };
    swap_panels(conn);
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

    let bar = adw::TabBar::new();
    bar.set_view(Some(&view));
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
        let mut st = s.borrow_mut();
        st.tabs.insert(conn, page.clone());
        st.sessions.insert(conn, sess);
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
    if let Some(page) = tab_for(hx_conn_serial(htlc)) {
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
    let Some(page) = tab_for(hx_conn_serial(htlc)) else {
        return;
    };
    if on && view().and_then(|v| v.selected_page()).as_ref() == Some(&page) {
        return;
    }
    page.set_needs_attention(on);
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
    }
}
