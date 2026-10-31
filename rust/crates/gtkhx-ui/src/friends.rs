//! The friend family of fogWraith's messaging extension, in the windows
//! GtkHx already has: a connection's friends, the requests waiting on an
//! answer, the senders approved and the accounts blocked, listed below its
//! users panel, with the message requests' row; Add Friend and Block in a
//! user's context menu; a request notified as a private message is; our
//! presence following the away state the user list shows of us, and
//! whether our account takes message requests. A friend's conversation is
//! `msg`'s, and the message requests' window `requests`', which this module
//! hands what the `messaging` signal says of them.
//!
//! None of it is there unless the server agreed to the family (capability
//! bit 6): until it has, the panel, the menus and the conversations are
//! exactly what they were. The user list names nobody's Login, so a row of
//! it is never taken for a friend; the friends come from the roster alone
//! ([`hxmodel::friends`]).

use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::ffi::{c_char, c_void, CStr};
use std::rc::Rc;
use std::sync::Once;
use std::time::Instant;

use adw::prelude::*;
use glib::translate::from_glib_none;
use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;

use gtkhx_core::conn::{
    generation, hx_conn_has_cap, hx_conn_logged_in, hx_conn_login, hx_conn_serverhost,
    hx_conn_serverport, hx_conn_uid,
};
use gtkhx_core::messaging::{with_roster, with_roster_mut, Event};
use gtkhx_core::session::{
    connect_messaging, gtkhx_session_emit_request_failed, gtkhx_session_get_default,
};
use hxmodel::chat_members::{hx_member_model_get_info, HxMemberInfo};
use hxmodel::conversation::hx_chat_member_model;
use hxmodel::friends::{self, Hush, Presence, Row, Section};
use hxproto::messages::ClientHdr;
use hxproto::messaging::{PresenceState, ReasonCode, RequestAction, CAP_MESSAGING};

use crate::dock::{self, Bound, ConnKey};
use crate::msg::{gtkhx_notify_send, play_sound, SOUND_MSG};
use crate::tr::{tr, tr1, tr_argv, trn_argv};

extern "C" {
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    // users.c — the bare-popover menu items' style.
    fn hx_popup_item_install_css();
    // gtkutil.c — Ctrl+W / Esc close accelerators on a dialog.
    fn gtkhx_dialog_add_close_shortcuts(dialog: *mut gtk::ffi::GtkWidget);
    // options.c — set a setting by its name, and save it.
    fn gtkhx_prefs_set_string(name: *const c_char, val: *const c_char);
}

/// A connection's users panel, and the friends section it has once the
/// server agreed to the family.
struct Panel {
    content: glib::WeakRef<gtk::Box>,
    section: Option<FriendsList>,
}

struct FriendsList {
    root: gtk::Box,
    /// The section's menu, which only a server serving message requests
    /// has anything in.
    more: gtk::MenuButton,
    list: gtk::ListBox,
    /// The message requests' row, first, while any wait.
    requests: gtk::ListBoxRow,
    count: gtk::Label,
    /// Each entry's row, by Login, kept across changes so that a row's open
    /// menu or focused button outlives a change to another.
    rows: HashMap<Vec<u8>, gtk::ListBoxRow>,
}

thread_local! {
    static PANELS: RefCell<HashMap<ConnKey, Panel>> = RefCell::new(HashMap::new());
    /// Our presence as each connection's server last took it, and the
    /// connect it was for.
    static PRESENCE: RefCell<HashMap<ConnKey, (u32, Presence)>> = RefCell::new(HashMap::new());
    /// The senders each connection just approved.
    static HUSH: RefCell<HashMap<ConnKey, Hush>> = RefCell::new(HashMap::new());
    /// The connect on which GtkHx turned taking requests on by itself, and
    /// the trans of the request while the server is yet to answer it.
    static TURNED_ON: RefCell<HashMap<ConnKey, (u32, Option<u32>)>> =
        RefCell::new(HashMap::new());
}

/// Whether `htlc` speaks the family now.
///
/// # Safety
/// `htlc` is a live connection.
unsafe fn agreed(htlc: *mut c_void) -> bool {
    hx_conn_logged_in(htlc.cast()) != 0 && hx_conn_has_cap(htlc.cast(), CAP_MESSAGING) != 0
}

/// Hear what the family says, and what it follows. Once.
pub(crate) fn install() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        connect_messaging(|htlc, e| unsafe { heard(htlc, e) });
        let session: glib::Object = unsafe {
            from_glib_none(gtkhx_session_get_default() as *mut glib::gobject_ffi::GObject)
        };
        let ptr = |args: &[glib::Value], i: usize| {
            args[i].get::<*mut c_void>().unwrap_or(std::ptr::null_mut())
        };
        session.connect_local("connection-state-changed", false, move |args| {
            let htlc = ptr(args, 1);
            if !htlc.is_null() {
                let conn = dock::conn_key(htlc);
                if unsafe { hx_conn_logged_in(htlc.cast()) } == 0 {
                    HUSH.with_borrow_mut(|h| h.remove(&conn));
                    TURNED_ON.with_borrow_mut(|t| t.remove(&conn));
                }
                refresh(conn);
                crate::requests::refresh(dock::conn_key(htlc));
                unsafe { crate::msg::forget_sent(htlc) };
            }
            None
        });
        session.connect_local("user-change", false, move |args| {
            let htlc = ptr(args, 1);
            let uid = args[3].get::<u32>().unwrap_or(0);
            let color = args[7].get::<u32>().unwrap_or(0);
            if !htlc.is_null() && uid != 0 && uid == u32::from(unsafe { hx_conn_uid(htlc.cast()) })
            {
                unsafe { follow_away(htlc, color & 1 != 0) };
            }
            None
        });
    });
}

/// # Safety
/// Main thread; `htlc` is the live connection the event is from.
unsafe fn heard(htlc: *mut c_void, e: &Event) {
    if htlc.is_null() {
        return;
    }
    match e {
        Event::Roster => {
            refresh(dock::conn_key(htlc));
            crate::msg::friends_changed(htlc);
            let away = own_status(htlc) & 1 != 0;
            follow_away(htlc, away);
        }
        Event::Changed { login } => {
            // A requester made a friend is handed what waited.
            if with_roster(htlc, |r| friends::is_friend(r, login)) {
                release(htlc, login);
            }
            refresh(dock::conn_key(htlc));
            crate::msg::friend_changed(htlc, login);
        }
        Event::Request { login } => {
            refresh(dock::conn_key(htlc));
            notify_request(htlc, login);
        }
        Event::Im(m) => {
            let conn = dock::conn_key(htlc);
            let quiet = HUSH.with_borrow(|h| {
                h.get(&conn)
                    .is_some_and(|h| h.hushed(&m.from, Instant::now()))
            });
            crate::msg::im(htlc, m, quiet);
        }
        Event::Delivery { to, guid, delivery } => crate::msg::delivered(htlc, to, guid, *delivery),
        Event::Typing { from, typing } => crate::msg::typing(htlc, from, *typing),
        Event::Refused {
            trans,
            request,
            reason,
            text,
        } => {
            if !turn_on_refused(htlc, *trans) {
                refused(htlc, *request, *reason, text.as_deref());
            }
        }
        Event::Requests => {
            refresh(dock::conn_key(htlc));
            turned_on(htlc);
            take_requests_by_default(htlc);
            crate::requests::listed(dock::conn_key(htlc));
        }
        Event::RequestsChanged => {
            refresh(dock::conn_key(htlc));
            crate::requests::changed(htlc);
        }
        Event::Cleared {
            requests,
            friend_requests,
            ..
        } => crate::requests::cleared(dock::conn_key(htlc), *requests, *friend_requests),
        Event::Presence => turned_on(htlc),
        Event::Found { .. } => {}
    }
}

/// `login`'s request on `htlc` no longer waits, and what waited is handed
/// over at once: their messages arrive unannounced for a while, once for
/// each request let go of. Whether one was.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn release(htlc: *mut c_void, login: &[u8]) -> bool {
    let released = with_roster_mut(htlc, |r| r.requests.released(login));
    if released {
        let conn = dock::conn_key(htlc);
        HUSH.with_borrow_mut(|h| h.entry(conn).or_default().approved(login, Instant::now()));
    }
    released
}

/// Our own status word in the public chat's user list, 0 before it lists us.
///
/// # Safety
/// `htlc` is a live connection.
unsafe fn own_status(htlc: *mut c_void) -> u16 {
    let uid = hx_conn_uid(htlc.cast());
    member(gtkhx_core::conn::hx_conn_sess(htlc.cast()).cast(), 0, uid).map_or(0, |m| m.status)
}

/// What chat `cid` of `sess` lists of `uid`.
///
/// # Safety
/// `sess` is NULL or a live session.
pub(crate) unsafe fn member(sess: *mut c_void, cid: u32, uid: u16) -> Option<HxMemberInfo> {
    if sess.is_null() || uid == 0 {
        return None;
    }
    let chat = chat_with_cid(sess, cid);
    let mut mi: HxMemberInfo = std::mem::zeroed();
    (!chat.is_null()
        && hx_member_model_get_info(hx_chat_member_model(chat.cast()), uid, &mut mi)
            != glib::ffi::GFALSE)
        .then_some(mi)
}

/// The user list shows us `away`, or not: tell the server, where that
/// changes what it last took. A new connect starts afresh, present.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn follow_away(htlc: *mut c_void, away: bool) {
    if !agreed(htlc) {
        return;
    }
    let (conn, connect) = (dock::conn_key(htlc), generation(htlc.cast()));
    let change = PRESENCE.with_borrow_mut(|p| {
        let held = p.entry(conn).or_insert((connect, Presence::default()));
        if held.0 != connect {
            *held = (connect, Presence::default());
        }
        held.1.away(away)
    });
    if let Some(state) = change {
        hxhandlers::send::friends::set_presence(htlc, state, None);
    }
}

/// The presence `htlc`'s server was last told on this connect.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn told(htlc: *mut c_void) -> PresenceState {
    let (conn, connect) = (dock::conn_key(htlc), generation(htlc.cast()));
    PRESENCE.with_borrow(|p| {
        p.get(&conn)
            .filter(|held| held.0 == connect)
            .map_or_else(|| Presence::default().told(), |held| held.1.told())
    })
}

/// How the settings name our account on `htlc`'s server.
///
/// # Safety
/// `htlc` is a live connection.
unsafe fn requests_key(htlc: *mut c_void) -> String {
    let host = CStr::from_ptr(hx_conn_serverhost(htlc.cast())).to_string_lossy();
    let login = CStr::from_ptr(hx_conn_login(htlc.cast())).to_bytes();
    friends::requests_set_key(&host, hx_conn_serverport(htlc.cast()), login)
}

fn requests_set() -> Vec<String> {
    hxconfig::ffi::with_settings(|s| s.messaging.requests_set.clone()).unwrap_or_default()
}

/// Remember that GtkHx has set whether our account on `htlc` takes message
/// requests.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn remember_set(htlc: *mut c_void) {
    let key = requests_key(htlc);
    let mut set = requests_set();
    if set.contains(&key) {
        return;
    }
    set.push(key);
    let name = crate::cs("REQUESTSSET");
    let value = crate::cs(&set.join(","));
    gtkhx_prefs_set_string(name.as_ptr(), value.as_ptr());
}

/// The user set whether our account on `htlc` takes message requests,
/// which GtkHx then never changes by itself.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn set_takes(htlc: *mut c_void, on: bool) {
    if hxhandlers::send::friends::set_takes_requests(htlc, on, Some(told(htlc))).is_some() {
        remember_set(htlc);
    }
}

/// Taking message requests is on by default: GtkHx turns it on the first
/// time it lists an account's requests and finds it off, and never again
/// once the server has taken that, since the user may have turned it off
/// since. It carries no presence, which leaves ours as it is, even an
/// Invisible set from another client, where the server allows that.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn take_requests_by_default(htlc: *mut c_void) {
    let (conn, connect) = (dock::conn_key(htlc), generation(htlc.cast()));
    let asked = TURNED_ON.with_borrow(|t| t.get(&conn).is_some_and(|held| held.0 == connect));
    let takes = with_roster(htlc, |m| m.requests.takes);
    if !friends::turn_requests_on(takes, requests_set().contains(&requests_key(htlc)), asked) {
        return;
    }
    let sent = hxhandlers::send::friends::set_takes_requests(htlc, true, None);
    TURNED_ON.with_borrow_mut(|t| t.insert(conn, (connect, sent)));
}

/// Whether the refusal on `trans` was of GtkHx turning taking requests on
/// by itself, which a server that wants a presence with it refuses: no
/// news to the user, who asked for nothing, and tried again on the next
/// connect.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn turn_on_refused(htlc: *mut c_void, trans: u32) -> bool {
    let (conn, connect) = (dock::conn_key(htlc), generation(htlc.cast()));
    TURNED_ON.with_borrow_mut(|t| match t.get_mut(&conn) {
        Some(held) if *held == (connect, Some(trans)) => {
            held.1 = None;
            true
        }
        _ => false,
    })
}

/// Remember that GtkHx turned taking requests on once the server says it
/// is on: a refusal leaves it to be tried again on the next connect.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn turned_on(htlc: *mut c_void) {
    let (conn, connect) = (dock::conn_key(htlc), generation(htlc.cast()));
    let waiting = TURNED_ON.with_borrow(|t| {
        t.get(&conn)
            .is_some_and(|h| h.0 == connect && h.1.is_some())
    });
    if waiting && with_roster(htlc, |m| m.requests.takes) == Some(true) {
        TURNED_ON.with_borrow_mut(|t| t.insert(conn, (connect, None)));
        remember_set(htlc);
    }
}

/// What a friend's presence and status read as.
pub(crate) fn presence_line(p: PresenceState, status: Option<&str>) -> String {
    let word = match p {
        PresenceState::Online => tr("Online"),
        PresenceState::Away => tr("Away"),
        PresenceState::Busy => tr("Busy"),
        _ => tr("Offline"),
    };
    match status.map(str::trim).filter(|s| !s.is_empty()) {
        // TRANSLATORS: a friend's presence, then the status they set.
        Some(s) => tr_argv("%s: %s", &[&word, s]),
        None => word,
    }
}

/// What a friend request says: its note, or that it asks.
fn request_note(note: Option<&str>) -> String {
    note.map(friends::plain_name)
        .filter(|n| !n.trim().is_empty())
        .map_or_else(|| tr("Asks to be your friend."), |n| n.trim().to_owned())
}

/// Someone asks to be a friend: told as a private message is, by the same
/// chime and under the same setting.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn notify_request(htlc: *mut c_void, login: &[u8]) {
    let Some(f) = with_roster(htlc, |r| r.get(login).cloned()) else {
        return;
    };
    let name = friends::display_name(&f);
    let body = request_note(f.note.as_deref());
    crate::announce::announce(None, &tr1("Friend request from %s", &name), false);
    play_sound(SOUND_MSG);
    if !hxconfig::ffi::with_settings(|s| s.notify.private_message).unwrap_or(false) {
        return;
    }
    let id = crate::cs(&format!(
        "friend-{}-{}",
        dock::conn_key(htlc),
        crate::chat_tabs::hex(login)
    ));
    let title = crate::cs(&tr1("%s (friend request)", &name));
    let body = crate::cs(&body);
    gtkhx_notify_send(id.as_ptr(), title.as_ptr(), body.as_ptr());
}

/// A request was refused: said as every refusal the server explains is.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn refused(htlc: *mut c_void, request: u32, reason: Option<ReasonCode>, text: Option<&str>) {
    let served = with_roster(htlc, |m| m.requests.served);
    let words = reason
        .and_then(|r| reason_words(r, request, served))
        .or_else(|| text.filter(|t| !t.trim().is_empty()).map(str::to_owned))
        .unwrap_or_else(|| tr("The server refused the request."));
    let words = crate::cs(&words);
    gtkhx_session_emit_request_failed(gtkhx_session_get_default(), htlc, words.as_ptr());
}

/// Why the request of opcode `request` was refused, in our words, where
/// the extension says; `served`, the server serves message requests.
fn reason_words(r: ReasonCode, request: u32, served: bool) -> Option<String> {
    Some(match r {
        // Where an answer names no request that waits.
        ReasonCode::AccountNotFound if request == ClientHdr::AnswerMessageRequest as u32 => {
            tr("That request is no longer waiting.")
        }
        ReasonCode::AccountNotFound => tr("No account has that login."),
        ReasonCode::NotMessagingEnabled => tr("That account does not take part in messaging."),
        ReasonCode::Blocked => tr("That account has blocked you."),
        ReasonCode::AlreadyFriends => tr("You are already friends."),
        ReasonCode::RequestPending => tr("A friend request is already waiting."),
        ReasonCode::NotFriends if served => tr("They don't accept message requests."),
        ReasonCode::NotFriends => tr("Only a friend can be sent an instant message."),
        ReasonCode::QueueFull => tr("Too many messages are waiting for them."),
        ReasonCode::RateLimited => tr("Too many requests. Try again later."),
        ReasonCode::RosterFull => tr("The friends list is full."),
        ReasonCode::MessageTooLong => tr("The message is too long."),
        _ => return None,
    })
}

// ---- the users panel ---------------------------------------------------

/// `sess`'s users panel was built as `content`: list its friends below it,
/// whenever its server speaks the family.
///
/// # Safety
/// `sess` is a live session; `content` its panel's content box. Main thread.
pub(crate) unsafe fn attach(sess: *mut c_void, content: *mut gtk::ffi::GtkWidget) {
    let Ok(content) = from_glib_none::<_, gtk::Widget>(content).downcast::<gtk::Box>() else {
        return;
    };
    let conn = dock::key_for_session(sess);
    // A panel built again for the connection has taken this one's place.
    content.connect_destroy(move |gone| {
        PANELS.with_borrow_mut(|p| {
            let ours = p
                .get(&conn)
                .is_some_and(|e| e.content.upgrade().is_none_or(|c| c == *gone));
            if ours {
                p.remove(&conn);
            }
        });
    });
    let panel = Panel {
        content: content.downgrade(),
        section: None,
    };
    PANELS.with_borrow_mut(|p| p.insert(conn, panel));
    refresh(conn);
}

/// Bring `conn`'s friends section in line with its roster: built where the
/// server speaks the family, and gone where it does not.
fn refresh(conn: ConnKey) {
    let htlc = dock::htlc_for(conn);
    let want = !htlc.is_null() && unsafe { agreed(htlc) };
    let Some(content) = PANELS.with_borrow(|p| p.get(&conn).and_then(|p| p.content.upgrade()))
    else {
        return;
    };
    let (rows, served, waiting) = if want {
        with_roster(htlc, |r| {
            (
                friends::rows(r),
                r.requests.served,
                friends::requests_waiting(r),
            )
        })
    } else {
        (Vec::new(), false, None)
    };
    PANELS.with_borrow_mut(|p| {
        let Some(panel) = p.get_mut(&conn) else {
            return;
        };
        if !want {
            if let Some(section) = panel.section.take() {
                content.remove(&section.root);
            }
            return;
        }
        let section = panel.section.get_or_insert_with(|| {
            let s = build_section(conn);
            content.append(&s.root);
            s
        });
        section.more.set_visible(served);
        section.requests.set_visible(waiting.is_some());
        if let Some(count) = waiting {
            let n = count.to_string();
            section.count.set_text(&n);
            let label = trn_argv(
                "Message requests, %s waiting",
                "Message requests, %s waiting",
                count.into(),
                &[&n],
            );
            section
                .requests
                .update_property(&[gtk::accessible::Property::Label(&label)]);
        }
        fill(section, conn, &rows);
    });
}

fn build_section(conn: ConnKey) -> FriendsList {
    let root = gtk::Box::new(gtk::Orientation::Vertical, 0);
    root.append(&gtk::Separator::new(gtk::Orientation::Horizontal));

    let header = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    header.add_css_class("gtkhx-panel-actions");
    let title = gtk::Label::new(Some(&tr("Friends")));
    title.add_css_class("heading");
    title.set_xalign(0.0);
    title.set_hexpand(true);
    title.set_margin_start(6);
    header.append(&title);
    let add = crate::icon_button("list-add-symbolic", &tr("Add Friend…"));
    add.add_css_class("flat");
    add.connect_clicked(move |b| prompt(conn, Act::Add, None, b.upcast_ref()));
    header.append(&add);
    let more = section_menu(conn);
    header.append(&more);
    root.append(&header);

    let requests = gtk::ListBoxRow::new();
    let line = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    line.set_margin_start(6);
    line.set_margin_end(6);
    line.set_margin_top(4);
    line.set_margin_bottom(4);
    let title = gtk::Label::new(Some(&tr("Message Requests")));
    title.set_xalign(0.0);
    title.set_hexpand(true);
    line.append(&title);
    let count = gtk::Label::new(None);
    count.add_css_class("numeric");
    count.add_css_class("dim-label");
    line.append(&count);
    requests.set_child(Some(&line));
    requests.set_visible(false);
    // Safety: read back only by `order`, as the same type.
    unsafe { requests.set_data(ORDER_KEY, 0u32) };

    let list = gtk::ListBox::new();
    list.set_selection_mode(gtk::SelectionMode::None);
    list.update_property(&[gtk::accessible::Property::Label(&tr("Friends"))]);
    list.set_sort_func(|a, b| order(a).cmp(&order(b)).into());
    list.append(&requests);
    let requests_row = requests.clone();
    list.connect_row_activated(move |_, row| {
        if *row == requests_row {
            crate::requests::open(conn);
            return;
        }
        if let Some(r) = entry_of(row).filter(|r| talks(r.section)) {
            let htlc = dock::htlc_for(conn);
            if !htlc.is_null() {
                unsafe { crate::msg::open_friend(htlc, &r.login) };
            }
        }
    });
    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scroll.set_propagate_natural_height(true);
    scroll.set_max_content_height(240);
    scroll.set_child(Some(&list));
    root.append(&scroll);
    FriendsList {
        root,
        more,
        list,
        requests,
        count,
        rows: HashMap::new(),
    }
}

/// The section's own menu: a message to anyone, by Login, and whether our
/// account takes message requests, as its server holds it.
fn section_menu(conn: ConnKey) -> gtk::MenuButton {
    unsafe { hx_popup_item_install_css() };
    let more = gtk::MenuButton::new();
    more.set_icon_name("view-more-symbolic");
    more.add_css_class("flat");
    crate::set_name_tip(&more, &tr("Friends Menu"));
    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    vbox.set_margin_start(4);
    vbox.set_margin_end(4);
    vbox.set_margin_top(4);
    vbox.set_margin_bottom(4);
    menu_item(&vbox, &tr("Message…"), move |anchor| {
        if let Some(anchor) = anchor {
            prompt(conn, Act::Message, None, &anchor);
        }
    });
    let takes = gtk::CheckButton::with_label(&tr("Accept Message Requests"));
    takes.set_margin_start(6);
    takes.set_tooltip_text(Some(&tr(
        "Let people who are not your friends send you messages, which wait until you answer them",
    )));
    vbox.append(&takes);
    let popover = gtk::Popover::new();
    popover.set_has_arrow(false);
    popover.set_child(Some(&vbox));
    more.set_popover(Some(&popover));

    // Shown as the server last said, which is no change to send.
    let showing = Rc::new(Cell::new(false));
    {
        let (takes, showing) = (takes.clone(), showing.clone());
        popover.connect_show(move |_| {
            let htlc = dock::htlc_for(conn);
            let on = !htlc.is_null() && with_roster(htlc, |m| m.requests.takes == Some(true));
            showing.set(true);
            takes.set_active(on);
            showing.set(false);
        });
    }
    takes.connect_toggled(move |b| {
        let htlc = dock::htlc_for(conn);
        if !showing.get() && !htlc.is_null() && unsafe { agreed(htlc) } {
            unsafe { set_takes(htlc, b.is_active()) };
        }
    });
    more
}

/// Whether a row's entry is someone to talk to: activating it opens the
/// conversation.
fn talks(section: Section) -> bool {
    matches!(section, Section::Friend | Section::Approved)
}

const ENTRY_KEY: &str = "friend-entry";
const ORDER_KEY: &str = "friend-order";

/// The entry a row was last drawn from.
fn entry_of(row: &gtk::ListBoxRow) -> Option<Row> {
    // Safety: only `dress` sets the key, and always to a Row.
    unsafe { row.data::<Row>(ENTRY_KEY).map(|p| p.as_ref().clone()) }
}

fn order(row: &gtk::ListBoxRow) -> u32 {
    // Safety: only `fill` sets the key, and always to a u32.
    unsafe { row.data::<u32>(ORDER_KEY).map_or(u32::MAX, |p| *p.as_ref()) }
}

/// What a row says under its name.
fn subtitle(r: &Row) -> String {
    match r.section {
        Section::Request => request_note(r.note.as_deref()),
        Section::Friend => presence_line(r.presence, r.status.as_deref()),
        Section::Approved => tr("Approved"),
        Section::Sent => tr("Friend request sent"),
        Section::Blocked => tr("Blocked"),
    }
}

/// Make the section's rows `rows`, in their order, drawing again only the
/// rows whose entries changed.
fn fill(s: &mut FriendsList, conn: ConnKey, rows: &[Row]) {
    let wanted: HashSet<&[u8]> = rows.iter().map(|r| r.login.as_slice()).collect();
    let list = &s.list;
    s.rows.retain(|login, row| {
        let keep = wanted.contains(login.as_slice());
        if !keep {
            list.remove(row);
        }
        keep
    });
    for (i, r) in rows.iter().enumerate() {
        let row = s.rows.entry(r.login.clone()).or_insert_with(|| {
            let row = new_row(conn, list);
            list.append(&row);
            row
        });
        // Safety: read back only by `order`, as the same type. The
        // requests' row is first.
        unsafe { row.set_data(ORDER_KEY, i as u32 + 1) };
        if entry_of(row).as_ref() != Some(r) {
            dress(row, conn, r);
        }
    }
    list.invalidate_sort();
}

/// A row, with its menu, which reads the row's entry when it opens.
fn new_row(conn: ConnKey, list: &gtk::ListBox) -> gtk::ListBoxRow {
    let row = gtk::ListBoxRow::new();
    let click = gtk::GestureClick::new();
    click.set_button(gtk::gdk::BUTTON_SECONDARY);
    let weak = list.downgrade();
    click.connect_pressed(move |g, _, x, y| {
        if let (Some(row), Some(list)) =
            (g.widget().and_downcast::<gtk::ListBoxRow>(), weak.upgrade())
        {
            menu(&list, &row, conn, x, y);
        }
    });
    row.add_controller(click);

    let keys = gtk::EventControllerKey::new();
    let weak = list.downgrade();
    keys.connect_key_pressed(move |k, key, _, state| {
        use gtk::gdk::{Key, ModifierType};
        let menu_key =
            key == Key::Menu || (key == Key::F10 && state.contains(ModifierType::SHIFT_MASK));
        match (k.widget().and_downcast::<gtk::ListBoxRow>(), weak.upgrade()) {
            (Some(row), Some(list)) if menu_key => {
                menu(&list, &row, conn, 0.0, 0.0);
                glib::Propagation::Stop
            }
            _ => glib::Propagation::Proceed,
        }
    });
    row.add_controller(keys);
    row
}

/// Draw `row` as entry `r`.
fn dress(row: &gtk::ListBoxRow, conn: ConnKey, r: &Row) {
    let name = gtk::Label::new(Some(&r.name));
    name.set_xalign(0.0);
    name.set_ellipsize(gtk::pango::EllipsizeMode::End);
    let sub = gtk::Label::new(Some(&subtitle(r)));
    sub.set_xalign(0.0);
    sub.set_ellipsize(gtk::pango::EllipsizeMode::End);
    sub.add_css_class("caption");
    let text = gtk::Box::new(gtk::Orientation::Vertical, 0);
    text.set_hexpand(true);
    text.append(&name);
    text.append(&sub);
    if r.dimmed {
        text.add_css_class("dim-label");
    }

    let line = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    line.set_margin_start(6);
    line.set_margin_end(6);
    line.set_margin_top(2);
    line.set_margin_bottom(2);
    line.append(&text);
    if r.section == Section::Request {
        let answers = [
            (
                tr("Accept"),
                tr1("Accept the friend request from %s", &r.name),
                true,
            ),
            (
                tr("Decline"),
                tr1("Decline the friend request from %s", &r.name),
                false,
            ),
        ];
        for (label, named, accept) in answers {
            let b = gtk::Button::with_label(&label);
            b.update_property(&[gtk::accessible::Property::Label(&named)]);
            b.set_valign(gtk::Align::Center);
            if accept {
                b.add_css_class("suggested-action");
            }
            let login = r.login.clone();
            b.connect_clicked(move |b| act(conn, &login, Do::Respond(accept), b.upcast_ref()));
            line.append(&b);
        }
    }

    row.set_child(Some(&line));
    row.set_activatable(talks(r.section));
    // TRANSLATORS: a friends list row read aloud: the name, then its state.
    let label = tr_argv("%s, %s", &[&r.name, &subtitle(r)]);
    row.update_property(&[gtk::accessible::Property::Label(&label)]);
    // Safety: read back only by `entry_of`, as the same type.
    unsafe { row.set_data(ENTRY_KEY, r.clone()) };
}

/// What the user asks of an entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Do {
    Message,
    Respond(bool),
    Remove,
    Block,
    Unblock,
    EndApproval,
}

/// What can be done with an entry in `section`, as its menu offers it.
fn choices(section: Section) -> Vec<(String, Do)> {
    match section {
        Section::Friend => vec![
            (tr("Message"), Do::Message),
            (tr("Remove Friend…"), Do::Remove),
            (tr("Block…"), Do::Block),
        ],
        Section::Request => vec![
            (tr("Accept"), Do::Respond(true)),
            (tr("Decline"), Do::Respond(false)),
            (tr("Block…"), Do::Block),
        ],
        Section::Sent => vec![
            (tr("Cancel Request"), Do::Remove),
            (tr("Block…"), Do::Block),
        ],
        Section::Approved => vec![
            (tr("Message"), Do::Message),
            (tr("End Approval…"), Do::EndApproval),
            (tr("Block…"), Do::Block),
        ],
        Section::Blocked => vec![(tr("Unblock"), Do::Unblock)],
    }
}

/// Do `what` to `login`, asking first where it ends a friendship.
fn act(conn: ConnKey, login: &[u8], what: Do, parent: &gtk::Widget) {
    let htlc = dock::htlc_for(conn);
    if htlc.is_null() || !unsafe { agreed(htlc) } {
        return;
    }
    let entry = with_roster(htlc, |r| r.get(login).cloned());
    let friend =
        entry.as_ref().and_then(|f| f.state) == Some(hxproto::messaging::RosterState::Accepted);
    let name = with_roster(htlc, |r| friends::name_of(r, login));
    let asked = match what {
        Do::Block => Some((
            tr("Block"),
            tr1("Block %s? They will no longer be able to reach you.", &name),
            tr("_Block"),
        )),
        Do::Remove if friend => Some((
            tr("Remove Friend"),
            tr1("Remove %s from your friends?", &name),
            tr("_Remove"),
        )),
        Do::EndApproval => Some((
            tr("End Approval"),
            tr1(
                "End the approval of %s? What they send will wait as a message request again.",
                &name,
            ),
            tr("_End Approval"),
        )),
        _ => None,
    };
    match asked {
        Some((heading, body, verb)) => confirm(conn, login, what, &heading, &body, &verb, parent),
        None => unsafe { send(htlc, login, what) },
    }
}

/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn send(htlc: *mut c_void, login: &[u8], what: Do) {
    use hxhandlers::send::friends as send;
    let _ = match what {
        Do::Message => {
            crate::msg::open_friend(htlc, login);
            None
        }
        Do::Respond(accept) => send::respond(htlc, login, accept),
        Do::Remove => send::remove(htlc, login),
        Do::Block => send::block(htlc, login),
        Do::Unblock => send::unblock(htlc, login),
        Do::EndApproval => send::answer_request(htlc, login, RequestAction::EndApproval),
    };
}

/// Block `login`, asking first: for the requests' window, where the sender
/// has no entry.
pub(crate) fn block(conn: ConnKey, login: &[u8], parent: &gtk::Widget) {
    act(conn, login, Do::Block, parent);
}

fn confirm(
    conn: ConnKey,
    login: &[u8],
    what: Do,
    heading: &str,
    body: &str,
    verb: &str,
    parent: &gtk::Widget,
) {
    let bound = dock::bind(conn);
    let dialog = adw::AlertDialog::new(Some(heading), Some(body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("ok", verb);
    dialog.set_response_appearance("ok", adw::ResponseAppearance::Destructive);
    dialog.set_close_response("cancel");
    unsafe { gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget) };
    let login = login.to_vec();
    dialog.connect_response(Some("ok"), move |_, _| {
        if let Some(htlc) = dock::live_htlc(bound).filter(|h| unsafe { agreed(*h) }) {
            unsafe { send(htlc, &login, what) };
        }
    });
    dialog.present(Some(parent));
}

/// A menu item, drawn as the user list's own menu draws its items. Clicked,
/// it closes its menu and hands `f` what the menu was opened on.
fn menu_item(menu: &gtk::Box, label: &str, f: impl Fn(Option<gtk::Widget>) + 'static) {
    let btn = gtk::Button::with_label(label);
    btn.add_css_class("flat");
    btn.add_css_class("gtkhx-popup-item");
    btn.set_has_frame(false);
    if let Some(l) = btn.child().and_downcast::<gtk::Label>() {
        l.set_xalign(0.0);
        l.set_hexpand(true);
    }
    btn.connect_clicked(move |b| {
        let popover = b.ancestor(gtk::Popover::static_type());
        let anchor = popover.as_ref().and_then(|p| p.parent());
        if let Some(p) = popover.and_downcast::<gtk::Popover>() {
            p.popdown();
        }
        f(anchor);
    });
    menu.append(&btn);
}

/// `row`'s menu, at `x`, `y` in it. The menu hangs from the list rather than
/// the row, which a change to its entry can draw again under it.
fn menu(list: &gtk::ListBox, row: &gtk::ListBoxRow, conn: ConnKey, x: f64, y: f64) {
    let Some(entry) = entry_of(row) else {
        return;
    };
    let at = row
        .compute_point(list, &gtk::graphene::Point::new(x as f32, y as f32))
        .unwrap_or_else(|| gtk::graphene::Point::new(0.0, 0.0));
    unsafe { hx_popup_item_install_css() };
    let popover = gtk::Popover::new();
    popover.set_has_arrow(false);
    popover.set_halign(gtk::Align::Start);
    popover.set_pointing_to(Some(&gtk::gdk::Rectangle::new(
        at.x() as i32,
        at.y() as i32,
        1,
        1,
    )));
    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    vbox.set_margin_start(4);
    vbox.set_margin_end(4);
    vbox.set_margin_top(4);
    vbox.set_margin_bottom(4);
    for (label, what) in choices(entry.section) {
        let login = entry.login.clone();
        menu_item(&vbox, &label, move |anchor| {
            if let Some(anchor) = anchor {
                act(conn, &login, what, &anchor);
            }
        });
    }
    popover.set_child(Some(&vbox));
    popover.set_parent(list);
    popover.connect_closed(|p| {
        let p = p.clone();
        // After the click that closed it has been handled.
        glib::idle_add_local_once(move || p.unparent());
    });
    popover.popup();
}

// ---- asking for a Login ------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Act {
    Add,
    Block,
    Message,
}

/// Ask for the Login to add, block or write to. The user list does not say
/// who a user logs in as, so a user picked from it is named in the question
/// and their Login asked for, never guessed.
fn prompt(conn: ConnKey, act: Act, nick: Option<&str>, parent: &gtk::Widget) {
    let bound = dock::bind(conn);
    let (heading, verb) = match act {
        Act::Add => (tr("Add Friend"), tr("_Add")),
        Act::Block => (tr("Block"), tr("_Block")),
        Act::Message => (tr("Message"), tr("_Message")),
    };
    let body = match nick {
        Some(n) => tr1(
            "The user list does not say which account %s logs in with. Enter its login.",
            n,
        ),
        None => tr("Enter the login of the account."),
    };
    let dialog = adw::AlertDialog::new(Some(&heading), Some(&body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("ok", &verb);
    dialog.set_response_appearance(
        "ok",
        match act {
            Act::Add | Act::Message => adw::ResponseAppearance::Suggested,
            Act::Block => adw::ResponseAppearance::Destructive,
        },
    );
    dialog.set_default_response(Some("ok"));
    dialog.set_close_response("cancel");

    let group = adw::PreferencesGroup::new();
    let login = adw::EntryRow::new();
    login.set_title(&tr("Login"));
    group.add(&login);
    dialog.set_response_enabled("ok", false);
    {
        let dialog = dialog.downgrade();
        login.connect_changed(move |e| {
            if let Some(d) = dialog.upgrade() {
                d.set_response_enabled("ok", !e.text().trim().is_empty());
            }
        });
    }
    let note = (act == Act::Add).then(|| {
        let note = adw::EntryRow::new();
        note.set_title(&tr("Note (optional)"));
        group.add(&note);
        note
    });
    dialog.set_extra_child(Some(&group));
    unsafe { gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget) };

    let done = move |login: &str, note: Option<String>| ask(bound, act, login, note.as_deref());
    {
        let (login, note) = (login.clone(), note.clone());
        dialog.connect_response(Some("ok"), move |_, _| {
            done(&login.text(), note.as_ref().map(|n| n.text().to_string()));
        });
    }
    {
        let dialog = dialog.downgrade();
        let note2 = note.clone();
        login.connect_entry_activated(move |login| {
            if login.text().trim().is_empty() {
                return;
            }
            done(&login.text(), note2.as_ref().map(|n| n.text().to_string()));
            if let Some(d) = dialog.upgrade() {
                d.close();
            }
        });
    }
    dialog.present(Some(parent));
}

/// Ask `login`, whose Login is known, to be a friend, with the note asked
/// for first: for the requests' window.
pub(crate) fn add_friend(conn: ConnKey, login: &[u8], parent: &gtk::Widget) {
    let bound = dock::bind(conn);
    let name = friends::plain_name(&hxproto::text::to_utf8(login));
    let body = tr1("Ask %s to be your friend?", &name);
    let dialog = adw::AlertDialog::new(Some(&tr("Add Friend")), Some(&body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("ok", &tr("_Add"));
    dialog.set_response_appearance("ok", adw::ResponseAppearance::Suggested);
    dialog.set_default_response(Some("ok"));
    dialog.set_close_response("cancel");
    let group = adw::PreferencesGroup::new();
    let note = adw::EntryRow::new();
    note.set_title(&tr("Note (optional)"));
    note.set_activates_default(true);
    group.add(&note);
    dialog.set_extra_child(Some(&group));
    unsafe { gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget) };
    let login = login.to_vec();
    dialog.connect_response(Some("ok"), move |_, _| {
        let text = note.text();
        let note = Some(text.trim()).filter(|n| !n.is_empty());
        if let Some(htlc) = dock::live_htlc(bound).filter(|h| unsafe { agreed(*h) }) {
            unsafe { hxhandlers::send::friends::add(htlc, &login, note) };
        }
    });
    dialog.present(Some(parent));
}

fn ask(bound: Bound, act: Act, typed: &str, note: Option<&str>) {
    let typed = typed.trim();
    let Some(htlc) = dock::live_htlc(bound).filter(|h| !typed.is_empty() && unsafe { agreed(*h) })
    else {
        return;
    };
    let note = note.map(str::trim).filter(|n| !n.is_empty());
    use hxhandlers::send::friends as send;
    unsafe {
        let login = send::login(htlc, typed);
        match act {
            Act::Add => send::add(htlc, &login, note),
            Act::Block => send::block(htlc, &login),
            Act::Message => {
                crate::msg::open_friend(htlc, &login);
                None
            }
        };
    }
}

/// `void hx_friends_popup_append (GtkBox *menu, session *sess, guint32 cid,
/// guint16 uid)` — Add Friend and Block in the context menu of user `uid`
/// of chat `cid`, where the server speaks the family; nothing otherwise.
///
/// # Safety
/// `menu` is the live menu's box, inside its popover; `sess` is NULL or a
/// live session. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_friends_popup_append(
    menu: *mut gtk::ffi::GtkBox,
    sess: *mut c_void,
    cid: u32,
    uid: u16,
) {
    if menu.is_null() || sess.is_null() {
        return;
    }
    let htlc = gtkhx_session_htlc(sess);
    if htlc.is_null() || !agreed(htlc) {
        return;
    }
    let Some(mi) = member(sess, cid, uid) else {
        return;
    };
    let nick = CStr::from_ptr(mi.name.as_ptr())
        .to_string_lossy()
        .into_owned();
    // The box, unlike its popover, is no longer floating: taking the
    // popover here would sink its floating reference and free it on drop.
    let menu: gtk::Box = from_glib_none(menu);
    let conn = dock::conn_key(htlc);
    menu.append(&gtk::Separator::new(gtk::Orientation::Horizontal));
    for (label, act) in [(tr("Add Friend…"), Act::Add), (tr("Block…"), Act::Block)] {
        let nick = nick.clone();
        menu_item(&menu, &label, move |anchor| {
            if let Some(anchor) = &anchor {
                prompt(conn, act, Some(&nick), anchor);
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::reason_words;
    use hxproto::messages::ClientHdr;
    use hxproto::messaging::ReasonCode;

    #[test]
    fn a_refusal_reads_as_what_was_asked_and_where() {
        let answer = ClientHdr::AnswerMessageRequest as u32;
        let im = ClientHdr::ImSend as u32;
        let cases = [
            (
                ReasonCode::AccountNotFound,
                answer,
                true,
                "That request is no longer waiting.",
            ),
            (
                ReasonCode::AccountNotFound,
                im,
                true,
                "No account has that login.",
            ),
            (
                ReasonCode::NotFriends,
                im,
                true,
                "They don't accept message requests.",
            ),
            (
                ReasonCode::NotFriends,
                im,
                false,
                "Only a friend can be sent an instant message.",
            ),
        ];
        for (reason, request, served, want) in cases {
            assert_eq!(
                reason_words(reason, request, served).as_deref(),
                Some(want),
                "{reason:?} {request} {served}"
            );
        }
    }
}
