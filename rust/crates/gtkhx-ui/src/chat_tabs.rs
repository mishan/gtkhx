//! `chat_tabs` — the Chat panel's internal tab strip (was `chat_tabs.c`).
//!
//! A singleton `AdwTabView` whose tabs are: a pinned public-chat tab at
//! position 0, one tab per private chat keyed on `(connection, cid)`, and one
//! tab per PM conversation keyed on `(connection, uid)`. The connection is
//! half of each key because a cid or a uid is only unique *within* one, and
//! one tab view serves every connection in turn. This module owns the tab
//! view, the two index maps, and the close-page dispatch; `chat.c` / `msg.c`
//! call in to add / find / raise / close tabs through the `gtkhx_chat_tabs_*`
//! C ABI, each passing the connection its tab belongs to.
//!
//! The libpanel side (flag / raise the Chat dock panel) stays C behind
//! `dock_bridge` — this module only names GTK / libadwaita types. State lives
//! in a `thread_local!` (main-thread only, like every other window module);
//! the close-page handler snapshots what it needs out of that cell and drops
//! the borrow before calling the registered C teardown, so a teardown that
//! calls back in (e.g. `gtkhx_chat_tabs_close_pchat`) can't re-enter the borrow.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};

use glib::translate::from_glib_none;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use libadwaita as adw;

use crate::dock;

/// The Chat dock panel id — matches `HX_PANEL_ID_CHAT` (`panel_registry.h`)
/// and what `chat.rs` passes.
const HX_ID_CHAT: &str = "chat";

// Per-page kind, stored as qdata so the close-page dispatch recovers it in
// O(1) (mirrors the C `g_object_set_data` "chat-tab-kind" / "chat-tab-id").
const KIND_PUBLIC: u8 = 1;
const KIND_PCHAT: u8 = 2;
const KIND_MSG: u8 = 3;

/// The connection a tab belongs to, as its serial.
///
/// A cid or a uid is only unique *within* a connection, so neither is a key
/// on its own: two servers can both have a private chat at cid 7 and a user
/// at uid 5. Under the tab-switched layout one tab view serves every
/// connection in turn, so the pair is the key.
type ConnKey = u16;

extern "C" {
    fn hx_conn_serial(h: *const c_void) -> u16;
}

fn conn_key(htlc: *mut c_void) -> ConnKey {
    // NULL reads as 0, which hx_conn_serial reserves for "no connection" —
    // so a connectionless caller cannot collide with a real connection.
    unsafe { hx_conn_serial(htlc) }
}

#[derive(Clone, Copy)]
struct TabMeta {
    kind: u8,
    id: u32,
    /// The connection whose tab this is, as the pointer the close callback
    /// hands straight back to C — which wants a connection to resolve a
    /// session from, not a number it would have to look up.
    htlc: *mut c_void,
    /// The same connection as its index key, computed once when the tab was
    /// added.
    ///
    /// Derivable from `htlc`, and recorded anyway so that the entry a page
    /// comes out of the map under is by construction the one it went in
    /// under — rather than the result of running the same derivation twice
    /// and trusting them to agree. One less FFI hop on the close path, and
    /// one less way for the maps to leak an entry.
    conn: ConnKey,
}

const META_KEY: &str = "chat-tab-meta";

fn set_meta(page: &adw::TabPage, meta: TabMeta) {
    // Safety: single-threaded; the key is module-private and always holds a
    // TabMeta, so the typed read in `meta()` is sound.
    unsafe { page.set_data(META_KEY, meta) };
}

fn meta(page: &adw::TabPage) -> Option<TabMeta> {
    // Safety: matched type with `set_meta`; NonNull points at qdata owned by
    // the page for its lifetime, copied out immediately.
    unsafe { page.data::<TabMeta>(META_KEY).map(|p| *p.as_ref()) }
}

#[derive(Default)]
struct ChatTabs {
    view: Option<adw::TabView>,
    pchat: HashMap<(ConnKey, u32), adw::TabPage>,
    msg: HashMap<(ConnKey, u16), adw::TabPage>,
    on_close_pchat: Option<extern "C" fn(*mut c_void, u32)>,
    on_close_msg: Option<extern "C" fn(*mut c_void, u16)>,
}

thread_local! {
    static STATE: RefCell<ChatTabs> = RefCell::new(ChatTabs::default());
}

/// Clone the tab view out of the cell (a ref bump), holding the borrow only
/// for the clone — so the returned handle can drive adw calls that re-enter
/// our signal handlers without a borrow conflict.
fn view() -> Option<adw::TabView> {
    STATE.with(|s| s.borrow().view.clone())
}

/// The close-page dispatcher. Removes the page's index entry, fires the
/// registered C teardown for its kind, then confirms the close.
fn on_close_page(view: &adw::TabView, page: &adw::TabPage) -> glib::Propagation {
    let m = meta(page);

    // Public chat is pinned; the user can't close it through the strip.
    // Defensive: decline if some path ever triggers it.
    if m.map(|m| m.kind) == Some(KIND_PUBLIC) {
        view.close_page_finish(page, false);
        return glib::Propagation::Stop;
    }

    // Remove our index entry and snapshot the teardown handler, then drop the
    // borrow before calling out — the handler may call back into this module.
    let mut cb: Option<Box<dyn FnOnce()>> = None;
    if let Some(m) = m {
        STATE.with(|s| {
            let mut st = s.borrow_mut();
            match m.kind {
                KIND_PCHAT => {
                    st.pchat.remove(&(m.conn, m.id));
                    if let Some(f) = st.on_close_pchat {
                        let htlc = m.htlc;
                        cb = Some(Box::new(move || f(htlc, m.id)));
                    }
                }
                KIND_MSG => {
                    st.msg.remove(&(m.conn, m.id as u16));
                    if let Some(f) = st.on_close_msg {
                        let (htlc, uid) = (m.htlc, m.id as u16);
                        cb = Some(Box::new(move || f(htlc, uid)));
                    }
                }
                _ => {}
            }
        });
    }
    if let Some(cb) = cb {
        cb();
    }

    view.close_page_finish(page, true);
    glib::Propagation::Stop
}

/// Selecting any tab acknowledges attention: clear the new page's flag and the
/// Chat panel's flag. A slight over-clear in the multi-flagged case, but the
/// alternative (scan every tab on each selection) would re-fire constantly.
fn on_selected_page_changed(view: &adw::TabView) {
    if let Some(page) = view.selected_page() {
        page.set_needs_attention(false);
    }
    dock::set_needs_attention(HX_ID_CHAT, false);
}

/// Initialize (once) and return the shared tab view as a borrowed
/// `GtkWidget*` (transfer none — the module keeps the owning ref). Idempotent.
#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_init() -> *mut gtk::ffi::GtkWidget {
    crate::ensure_gtk_init();

    if let Some(v) = view() {
        return v.as_ptr() as *mut gtk::ffi::GtkWidget;
    }

    let view = adw::TabView::new();
    view.connect_close_page(on_close_page);
    view.connect_selected_page_notify(on_selected_page_changed);

    let ptr = view.as_ptr() as *mut gtk::ffi::GtkWidget;
    STATE.with(|s| s.borrow_mut().view = Some(view));
    ptr
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_set_close_pchat_handler(
    func: Option<extern "C" fn(*mut c_void, u32)>,
) {
    STATE.with(|s| s.borrow_mut().on_close_pchat = func);
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_set_close_msg_handler(
    func: Option<extern "C" fn(*mut c_void, u16)>,
) {
    STATE.with(|s| s.borrow_mut().on_close_msg = func);
}

/// # Safety
/// `content` is a valid `GtkWidget*`; `title` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_tabs_add_public(
    content: *mut gtk::ffi::GtkWidget,
    title: *const c_char,
) {
    let Some(view) = view() else { return };
    let w: gtk::Widget = from_glib_none(content);

    let page = view.append_pinned(&w);
    let t = crate::cstr(title);
    page.set_title(if t.is_empty() { "Chat" } else { &t });
    set_meta(
        &page,
        TabMeta {
            kind: KIND_PUBLIC,
            id: 0,
            // The public tab is pinned, never indexed and never closed
            // through the strip, so it needs no connection of its own. 0 is
            // the serial no connection is ever assigned.
            htlc: std::ptr::null_mut(),
            conn: 0,
        },
    );
    view.set_selected_page(&page);
}

/// # Safety
/// `content` is a valid `GtkWidget*`; `title` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_tabs_add_pchat(
    htlc: *mut c_void,
    content: *mut gtk::ffi::GtkWidget,
    cid: u32,
    title: *const c_char,
) -> *mut adw::ffi::AdwTabPage {
    let Some(view) = view() else {
        return std::ptr::null_mut();
    };
    let w: gtk::Widget = from_glib_none(content);

    let page = view.append(&w);
    let t = crate::cstr(title);
    page.set_title(if t.is_empty() { "Private Chat" } else { &t });

    let conn = conn_key(htlc);
    set_meta(
        &page,
        TabMeta {
            kind: KIND_PCHAT,
            id: cid,
            htlc,
            conn,
        },
    );

    let ptr = page.as_ptr();
    STATE.with(|s| {
        s.borrow_mut().pchat.insert((conn, cid), page);
    });
    ptr
}

/// # Safety
/// `content` is a valid `GtkWidget*`; `title` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_tabs_add_msg(
    htlc: *mut c_void,
    content: *mut gtk::ffi::GtkWidget,
    uid: u16,
    title: *const c_char,
) -> *mut adw::ffi::AdwTabPage {
    let Some(view) = view() else {
        return std::ptr::null_mut();
    };
    let w: gtk::Widget = from_glib_none(content);

    let page = view.append(&w);
    let t = crate::cstr(title);
    page.set_title(if t.is_empty() { "PM" } else { &t });

    let conn = conn_key(htlc);
    set_meta(
        &page,
        TabMeta {
            kind: KIND_MSG,
            id: uid as u32,
            htlc,
            conn,
        },
    );

    let ptr = page.as_ptr();
    STATE.with(|s| {
        s.borrow_mut().msg.insert((conn, uid), page);
    });
    ptr
}

fn find_pchat(htlc: *mut c_void, cid: u32) -> Option<adw::TabPage> {
    STATE.with(|s| s.borrow().pchat.get(&(conn_key(htlc), cid)).cloned())
}

fn find_msg(htlc: *mut c_void, uid: u16) -> Option<adw::TabPage> {
    STATE.with(|s| s.borrow().msg.get(&(conn_key(htlc), uid)).cloned())
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_find_pchat(
    htlc: *mut c_void,
    cid: u32,
) -> *mut adw::ffi::AdwTabPage {
    find_pchat(htlc, cid)
        .map(|p| p.as_ptr())
        .unwrap_or(std::ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_find_msg(
    htlc: *mut c_void,
    uid: u16,
) -> *mut adw::ffi::AdwTabPage {
    find_msg(htlc, uid)
        .map(|p| p.as_ptr())
        .unwrap_or(std::ptr::null_mut())
}

/// Raise the Chat dock panel (re-attach + focus via the bridge) then select
/// `page`. Both calls run with no `STATE` borrow held.
fn raise_and_select(page: Option<adw::TabPage>) {
    let Some(page) = page else { return };
    dock::raise_if_open(HX_ID_CHAT);
    if let Some(view) = view() {
        view.set_selected_page(&page);
    }
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_raise_pchat(htlc: *mut c_void, cid: u32) {
    raise_and_select(find_pchat(htlc, cid));
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_raise_msg(htlc: *mut c_void, uid: u16) {
    raise_and_select(find_msg(htlc, uid));
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_raise_public() {
    let page = view().map(|v| v.nth_page(0));
    raise_and_select(page);
}

/// Set / clear needs-attention on `page` and mirror it onto the Chat panel.
fn set_page_attention(page: Option<adw::TabPage>, state: bool) {
    let Some(page) = page else { return };
    page.set_needs_attention(state);
    dock::set_needs_attention(HX_ID_CHAT, state);
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_set_attention_pchat(
    htlc: *mut c_void,
    cid: u32,
    state: glib::ffi::gboolean,
) {
    set_page_attention(find_pchat(htlc, cid), state != glib::ffi::GFALSE);
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_set_attention_msg(
    htlc: *mut c_void,
    uid: u16,
    state: glib::ffi::gboolean,
) {
    set_page_attention(find_msg(htlc, uid), state != glib::ffi::GFALSE);
}

/// # Safety
/// `title` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_tabs_set_title_pchat(
    htlc: *mut c_void,
    cid: u32,
    title: *const c_char,
) {
    if let Some(page) = find_pchat(htlc, cid) {
        let t = crate::cstr(title);
        page.set_title(if t.is_empty() { "Private Chat" } else { &t });
    }
}

/// # Safety
/// `title` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_tabs_set_title_msg(
    htlc: *mut c_void,
    uid: u16,
    title: *const c_char,
) {
    if let Some(page) = find_msg(htlc, uid) {
        let t = crate::cstr(title);
        page.set_title(if t.is_empty() { "PM" } else { &t });
    }
}

/// Programmatic close (server tore down the chat, kick, etc.). Fires
/// close-page → `on_close_page`, which removes the index entry and runs the
/// registered teardown.
#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_close_pchat(htlc: *mut c_void, cid: u32) {
    if let (Some(view), Some(page)) = (view(), find_pchat(htlc, cid)) {
        view.close_page(&page);
    }
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_close_msg(htlc: *mut c_void, uid: u16) {
    if let (Some(view), Some(page)) = (view(), find_msg(htlc, uid)) {
        view.close_page(&page);
    }
}
