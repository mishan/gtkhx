//! `chat_tabs` — the Chat panel's internal tab strip (was `chat_tabs.c`).
//!
//! One `AdwTabView` per connection, each with a pinned public-chat tab at
//! position 0, one tab per private chat, and one tab per PM conversation.
//! Under the tab-switched layout every connection has its own Chat content
//! page, so it has its own strip of conversations.
//!
//! The two index maps behind those views are shared, and so stay keyed on
//! `(connection, cid)` / `(connection, uid)`: a cid or a uid is only unique
//! *within* a connection. This module owns the views, the maps, and the
//! close-page dispatch; `chat.c` calls in to add / find / raise / close tabs
//! through the `gtkhx_chat_tabs_*` C ABI, and the `msg` module directly, each
//! passing the connection its tab belongs to.
//!
//! The dock side (flag / raise the Chat dock panel) stays C behind
//! `dock_bridge` — this module only names GTK / libadwaita types. State lives
//! in a `thread_local!` (main-thread only, like every other window module);
//! the close-page handler snapshots what it needs out of that cell and drops
//! the borrow before calling the kind's teardown, so a teardown that calls
//! back in (e.g. `gtkhx_chat_tabs_close_pchat`) can't re-enter the borrow.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};

use glib::translate::from_glib_none;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use libadwaita as adw;

use hxmodel::pm::Peer;

use crate::dock;
use crate::tr::tr;

// Per-page kind, stored as qdata so the close-page dispatch recovers it in
// O(1) (mirrors the C `g_object_set_data` "chat-tab-kind" / "chat-tab-id").
const KIND_PUBLIC: u8 = 1;
const KIND_PCHAT: u8 = 2;
const KIND_MSG: u8 = 3;

/// The connection a tab belongs to, as its serial. Also the key its view is
/// held under, and half of the key its index entry is held under — a cid or a
/// uid is only unique *within* a connection.
type ConnKey = u16;

extern "C" {
    fn hx_conn_serial(h: *const c_void) -> u16;
}

fn conn_key(htlc: *mut c_void) -> ConnKey {
    // NULL reads as 0, which hx_conn_serial reserves for "no connection" —
    // so a connectionless caller cannot collide with a real connection.
    unsafe { hx_conn_serial(htlc) }
}

#[derive(Clone)]
struct TabMeta {
    kind: u8,
    id: u32,
    /// A private message's peer.
    peer: Option<Peer>,
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
    unsafe { page.data::<TabMeta>(META_KEY).map(|p| p.as_ref().clone()) }
}

#[derive(Default)]
struct ChatTabs {
    /// One tab view per connection.
    ///
    /// It used to be one for the whole process, which is what forced the
    /// composite keys below — and what made the Chat panel unbuildable twice,
    /// since a second connection's content build would have re-parented the
    /// first connection's tab view into its own box. Under the tab-switched
    /// layout each connection has its own Chat page, so each has its own strip
    /// of conversations.
    ///
    /// The index maps stay composite-keyed. They are still one table serving
    /// every connection, so a cid or a uid is still not a key on its own; only
    /// the view is per-connection.
    views: HashMap<ConnKey, adw::TabView>,
    pchat: HashMap<(ConnKey, u32), adw::TabPage>,
    msg: HashMap<(ConnKey, Peer), adw::TabPage>,
    on_close_pchat: Option<extern "C" fn(*mut c_void, u32)>,
}

thread_local! {
    static STATE: RefCell<ChatTabs> = RefCell::new(ChatTabs::default());
}

/// Clone `conn`'s tab view out of the cell (a ref bump), holding the borrow
/// only for the clone — so the returned handle can drive adw calls that
/// re-enter our signal handlers without a borrow conflict.
fn view(conn: ConnKey) -> Option<adw::TabView> {
    STATE.with(|s| s.borrow().views.get(&conn).cloned())
}

/// The view a page belongs to, for the handlers that are given a page and no
/// connection.
fn view_of(page: &adw::TabPage) -> Option<adw::TabView> {
    meta(page).and_then(|m| view(m.conn))
}

/// The close-page dispatcher. Removes the page's index entry, fires the
/// teardown for its kind, then confirms the close.
fn on_close_page(view: &adw::TabView, page: &adw::TabPage) -> glib::Propagation {
    let m = meta(page);

    // Public chat is pinned; the user can't close it through the strip.
    // Defensive: decline if some path ever triggers it.
    if m.as_ref().map(|m| m.kind) == Some(KIND_PUBLIC) {
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
                    if let Some(peer) = m.peer {
                        st.msg.remove(&(m.conn, peer.clone()));
                        cb = Some(Box::new(move || crate::msg::tab_closed(m.conn, &peer)));
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
///
/// The *connection* tab is not cleared here, and that asymmetry is deliberate:
/// this strip only ever shows the focused connection's conversations, so
/// reading one of them says nothing about what another server has been up to.
/// The connection tab clears when the user selects it.
fn on_selected_page_changed(view: &adw::TabView) {
    if let Some(page) = view.selected_page() {
        page.set_needs_attention(false);
    }
    dock::set_needs_attention(dock::ID_CHAT, false);
}

/// Build (once per connection) and return `htlc`'s tab view as a borrowed
/// `GtkWidget*` (transfer none — the module keeps the owning ref). Idempotent
/// for a given connection.
#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_init(htlc: *mut c_void) -> *mut gtk::ffi::GtkWidget {
    crate::ensure_gtk_init();

    let conn = conn_key(htlc);
    if let Some(v) = view(conn) {
        return v.as_ptr() as *mut gtk::ffi::GtkWidget;
    }

    let view = adw::TabView::new();
    view.connect_close_page(on_close_page);
    view.connect_selected_page_notify(on_selected_page_changed);
    // A drop must not be answered with nothing; see conn_tabs' on_create_window.
    view.connect_create_window(|view| Some(view.clone()));

    let ptr = view.as_ptr() as *mut gtk::ffi::GtkWidget;
    STATE.with(|s| {
        s.borrow_mut().views.insert(conn, view);
    });
    ptr
}

/// Forget `htlc`'s view and every tab indexed under it.
///
/// Called when a connection's Chat page is destroyed. Without it
/// `gtkhx_chat_tabs_init` would hand a rebuilt page the *old* view — which
/// still holds the old pinned public tab, so `add_public` would append a
/// second one and the panel would show two "Chat" tabs, the first empty.
///
/// # Safety
/// `htlc` is NULL or a live connection. GTK main thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_tabs_forget(htlc: *mut c_void) {
    forget_conn(conn_key(htlc));
}

/// [`gtkhx_chat_tabs_forget`] for a caller that already holds the serial.
///
/// The form a destroy handler wants: it may run after the connection has been
/// freed, so it must not be holding one to resolve.
pub(crate) fn forget_conn(conn: ConnKey) {
    STATE.with(|s| {
        let mut st = s.borrow_mut();
        st.views.remove(&conn);
        st.pchat.retain(|(c, _), _| *c != conn);
        st.msg.retain(|(c, _), _| *c != conn);
    });
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_set_close_pchat_handler(
    func: Option<extern "C" fn(*mut c_void, u32)>,
) {
    STATE.with(|s| s.borrow_mut().on_close_pchat = func);
}

/// # Safety
/// `content` is a valid `GtkWidget*`; `title` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_tabs_add_public(
    htlc: *mut c_void,
    content: *mut gtk::ffi::GtkWidget,
    title: *const c_char,
) {
    let conn = conn_key(htlc);
    let Some(view) = view(conn) else { return };
    let w: gtk::Widget = from_glib_none(content);

    let page = view.append_pinned(&w);
    let t = crate::cstr(title);
    page.set_title(&if t.is_empty() { tr("Chat") } else { t });
    // A pinned tab shows its icon and nothing else.
    page.set_icon(Some(&gtk::gio::ThemedIcon::new("system-users-symbolic")));
    set_meta(
        &page,
        TabMeta {
            kind: KIND_PUBLIC,
            id: 0,
            peer: None,
            // Never indexed and never closed through the strip, so the
            // pointer is only here for `view_of` to find the right view.
            htlc,
            conn,
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
    let conn = conn_key(htlc);
    let Some(view) = view(conn) else {
        return std::ptr::null_mut();
    };
    let w: gtk::Widget = from_glib_none(content);

    let page = view.append(&w);
    let t = crate::cstr(title);
    page.set_title(&if t.is_empty() { tr("Private Chat") } else { t });

    set_meta(
        &page,
        TabMeta {
            kind: KIND_PCHAT,
            id: cid,
            peer: None,
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

/// A private message's tab title, or the generic one when it has none.
fn msg_title(title: &str) -> String {
    if title.is_empty() {
        tr("PM")
    } else {
        title.to_owned()
    }
}

/// Add a private-message tab for `peer` on `htlc`. False when that
/// connection has no tab view to add it to: its Chat page isn't built.
pub(crate) fn add_msg(
    htlc: *mut c_void,
    content: &impl IsA<gtk::Widget>,
    peer: &Peer,
    title: &str,
) -> bool {
    let conn = conn_key(htlc);
    let Some(view) = view(conn) else {
        return false;
    };

    let page = view.append(content);
    page.set_title(&msg_title(title));

    set_meta(
        &page,
        TabMeta {
            kind: KIND_MSG,
            id: 0,
            peer: Some(peer.clone()),
            htlc,
            conn,
        },
    );

    STATE.with(|s| {
        s.borrow_mut().msg.insert((conn, peer.clone()), page);
    });
    true
}

fn find_pchat(htlc: *mut c_void, cid: u32) -> Option<adw::TabPage> {
    STATE.with(|s| s.borrow().pchat.get(&(conn_key(htlc), cid)).cloned())
}

fn find_msg(htlc: *mut c_void, peer: &Peer) -> Option<adw::TabPage> {
    STATE.with(|s| s.borrow().msg.get(&(conn_key(htlc), peer.clone())).cloned())
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

/// Raise the Chat dock panel (re-attach + focus via the bridge) then select
/// `page`. Both calls run with no `STATE` borrow held.
fn raise_and_select(page: Option<adw::TabPage>) {
    let Some(page) = page else { return };
    // Show the tab's *connection* page first. Each connection has its own view
    // now, so raising the Chat panel alone can leave the selected tab inside a
    // page the dock isn't showing — which is what an incoming PM on a
    // background connection would do: raise Chat onto the foreground
    // connection and select an invisible tab.
    if let Some(m) = meta(&page) {
        dock::show_page(dock::ID_CHAT, &m.conn.to_string());
    }
    dock::raise_if_open(dock::ID_CHAT);
    if let Some(view) = view_of(&page) {
        view.set_selected_page(&page);
    }
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_raise_pchat(htlc: *mut c_void, cid: u32) {
    raise_and_select(find_pchat(htlc, cid));
}

pub(crate) fn raise_msg(htlc: *mut c_void, peer: &Peer) {
    raise_and_select(find_msg(htlc, peer));
}

/// The id a notification of a message from `peer` goes by, which
/// [`open_notified`] reads back: `msg-` with the connection's serial and
/// the uid, or `im-` with it and the Login in hex.
pub(crate) fn notify_id(conn: ConnKey, peer: &Peer) -> String {
    match peer {
        Peer::Uid(uid) => format!("msg-{conn}-{uid}"),
        Peer::Login(login) => format!("im-{conn}-{}", hex(login)),
    }
}

/// A Login in hex, for a notification's id.
pub(crate) fn hex(login: &[u8]) -> String {
    login.iter().map(|b| format!("{b:02x}")).collect()
}

/// A Login back out of the hex [`notify_id`] wrote.
fn unhex(s: &str) -> Option<Vec<u8>> {
    if !s.len().is_multiple_of(2) {
        return None;
    }
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok())
        .collect()
}

/// `app.open-notified`: clicking a notification opens the conversation it was
/// about, and brings GtkHx forward either way. The target is the
/// notification's own id (notify.c, or [`notify_id`]): `chat-`, `pchat-`,
/// `msg-` or `im-` with the connection's serial and the chat, user or
/// account, which opens nothing once that conversation has closed; anything
/// else (news, transfers, broadcasts, invitations, friend requests) only
/// brings the window forward.
fn open_notified(app: &gtk::Application, id: &str) {
    let mut it = id.splitn(3, '-');
    let (kind, conn, n) = (
        it.next(),
        it.next().and_then(|s| s.parse::<ConnKey>().ok()),
        it.next(),
    );
    let page = match (kind, conn) {
        (Some("chat"), Some(conn)) => view(conn)
            .filter(|v| v.n_pages() > 0)
            .map(|v| v.nth_page(0)),
        (Some("pchat"), Some(conn)) => n
            .and_then(|n| n.parse::<u32>().ok())
            .and_then(|cid| STATE.with(|s| s.borrow().pchat.get(&(conn, cid)).cloned())),
        (Some("msg"), Some(conn)) => n
            .and_then(|n| n.parse::<u16>().ok())
            .and_then(|uid| STATE.with(|s| s.borrow().msg.get(&(conn, Peer::Uid(uid))).cloned())),
        (Some("im"), Some(conn)) => n.and_then(unhex).and_then(|login| {
            STATE.with(|s| s.borrow().msg.get(&(conn, Peer::Login(login))).cloned())
        }),
        _ => None,
    };
    let child = page.as_ref().map(|p| p.child());
    raise_and_select(page);
    // After the raise, which can move the Chat panel to another window.
    let window = child
        .and_then(|c| c.root())
        .and_downcast::<gtk::Window>()
        .or_else(|| app.active_window())
        .or_else(|| app.windows().into_iter().next());
    if let Some(w) = window {
        w.present();
    }
}

/// `void gtkhx_notify_install_actions (GtkApplication *app)`.
///
/// # Safety
/// `app` is a valid `GtkApplication *`. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_notify_install_actions(app: *mut gtk::ffi::GtkApplication) {
    let app: gtk::Application = from_glib_none(app);
    let action = gtk::gio::SimpleAction::new("open-notified", Some(glib::VariantTy::STRING));
    let weak = app.downgrade();
    action.connect_activate(move |_, target| {
        if let (Some(app), Some(id)) = (weak.upgrade(), target.and_then(|t| t.str())) {
            open_notified(&app, id);
        }
    });
    app.add_action(&action);
    // Once, at startup, before any connection can hear from a friend.
    crate::friends::install();
}

/// Raise the public-chat tab of the connection the user is looking at. The
/// pinned tab is always at position 0.
#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_raise_public() {
    let page = view(dock::active_key()).map(|v| v.nth_page(0));
    raise_and_select(page);
}

/// Set / clear needs-attention on `page`, and mirror it up the two levels
/// that sit above the tab: the Chat panel, and `htlc`'s connection tab.
///
/// Three levels, because a background conversation can be hidden three ways
/// and the user has to be able to find it from wherever they are: behind
/// another chat tab, behind another dock panel, or behind another connection.
/// Each mirror is a no-op when its level isn't there — no connection tab
/// before there are two connections, no panel flag if Chat isn't docked.
///
/// The connection tab is only ever *set* from here, never cleared, and the
/// asymmetry is the point. This function knows one conversation was
/// acknowledged; it does not know whether that connection has others still
/// waiting, and clearing on the strength of one of them would drop the badge
/// while a second unread chat on the same server was still outstanding. The
/// connection tab clears when the user selects it, which is the moment they
/// have actually looked.
///
/// A set is in turn ignored for the connection already selected — see
/// `gtkhx_conn_tabs_set_attention` — so this doesn't badge the server the
/// user is looking at because one of its own background chats spoke.
fn set_page_attention(htlc: *mut c_void, page: Option<adw::TabPage>, state: bool) {
    let Some(page) = page else { return };
    page.set_needs_attention(state);
    dock::set_needs_attention(dock::ID_CHAT, state);
    if state {
        // Safety: `htlc` came in from C as the connection this tab belongs to
        // and is live for the call; the callee only reads its serial.
        unsafe { crate::conn_tabs::gtkhx_conn_tabs_set_attention(htlc, glib::ffi::GTRUE) };
    }
}

#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_set_attention_pchat(
    htlc: *mut c_void,
    cid: u32,
    state: glib::ffi::gboolean,
) {
    set_page_attention(htlc, find_pchat(htlc, cid), state != glib::ffi::GFALSE);
}

pub(crate) fn set_attention_msg(htlc: *mut c_void, peer: &Peer, state: bool) {
    set_page_attention(htlc, find_msg(htlc, peer), state);
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
        page.set_title(&if t.is_empty() { tr("Private Chat") } else { t });
    }
}

pub(crate) fn set_title_msg(htlc: *mut c_void, peer: &Peer, title: &str) {
    if let Some(page) = find_msg(htlc, peer) {
        page.set_title(&msg_title(title));
    }
}

/// Programmatic close (server tore down the chat, kick, etc.). Fires
/// close-page → `on_close_page`, which removes the index entry and runs the
/// registered teardown.
#[no_mangle]
pub extern "C" fn gtkhx_chat_tabs_close_pchat(htlc: *mut c_void, cid: u32) {
    if let (Some(view), Some(page)) = (view(conn_key(htlc)), find_pchat(htlc, cid)) {
        view.close_page(&page);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_login_comes_back_out_of_its_notification() {
        let logins: [&[u8]; 4] = [b"alice", b"friend-a", b"\x00\xff\x8a", b""];
        for login in logins {
            let id = notify_id(7, &Peer::Login(login.to_vec()));
            let hex = id.strip_prefix("im-7-").expect(&id);
            assert_eq!(unhex(hex).as_deref(), Some(login), "{id}");
        }
        assert_eq!(unhex("abc"), None);
        assert_eq!(unhex("zz"), None);
    }
}
