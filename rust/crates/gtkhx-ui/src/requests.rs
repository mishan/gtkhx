//! Message requests: the window, one a connection, where what people who
//! are not friends sent waits to be answered. Each sender is listed, oldest
//! first, with their messages, to approve, reply to, befriend, delete or
//! block; the header's Clear… deletes them by when they arrived.
//!
//! Everything in it a stranger wrote is plain text, never markup or a link,
//! with whatever would change how it reads stripped (`hxmodel::friends::
//! plain_name`). It is listed again when it opens and whenever the server
//! says the requests changed while it is open, and closes with its
//! connection. `friends` opens it, from the Friends section's row, which is
//! only there where the server serves requests and some wait.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;

use adw::prelude::*;
use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;

use gtkhx_core::conn::hx_conn_has_cap;
use gtkhx_core::messaging::{with_roster, Sender};
use hxmodel::friends::{plain_name, plain_text};
use hxproto::messaging::{RequestAction, CAP_MESSAGING};

use crate::dock::{self, Bound, ConnKey};
use crate::tr::{tr, tr1, trn_argv};

extern "C" {
    // gtkutil.c — Ctrl+W / Esc close accelerators on a dialog.
    fn gtkhx_dialog_add_close_shortcuts(dialog: *mut gtk::ffi::GtkWidget);
}

struct Window {
    window: adw::Window,
    bound: Bound,
    toast: adw::ToastOverlay,
    stack: gtk::Stack,
    list: gtk::Box,
    dropped: gtk::Label,
    /// What the list was last drawn from, so a listing that changes
    /// nothing leaves its focus where it was.
    drawn: Option<(Vec<Sender>, u32)>,
}

thread_local! {
    static WINDOWS: RefCell<HashMap<ConnKey, Window>> = RefCell::new(HashMap::new());
}

/// Whether `htlc` serves message requests now.
///
/// # Safety
/// `htlc` is a live connection.
unsafe fn served(htlc: *mut c_void) -> bool {
    hx_conn_has_cap(htlc.cast(), CAP_MESSAGING) != 0 && with_roster(htlc, |m| m.requests.served)
}

/// Open `conn`'s requests, or raise them, and list them again.
pub(crate) fn open(conn: ConnKey) {
    let htlc = dock::htlc_for(conn);
    if htlc.is_null() || !unsafe { served(htlc) } {
        return;
    }
    let open = WINDOWS.with_borrow(|w| w.get(&conn).map(|w| w.window.clone()));
    match open {
        Some(window) => window.present(),
        None => build(conn),
    }
    unsafe { hxhandlers::send::friends::requests(htlc) };
    listed(conn);
}

/// `conn` changed state: a window for a connect that is gone, or that no
/// longer serves requests, closes.
pub(crate) fn refresh(conn: ConnKey) {
    let gone = WINDOWS.with_borrow(|w| {
        w.get(&conn)
            .filter(|w| dock::live_htlc(w.bound).is_none_or(|h| !unsafe { served(h) }))
            .map(|w| w.window.clone())
    });
    if let Some(window) = gone {
        window.close();
    }
}

/// The server said the requests changed: list them again, where they are
/// shown.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn changed(htlc: *mut c_void) {
    if WINDOWS.with_borrow(|w| w.contains_key(&dock::conn_key(htlc))) {
        hxhandlers::send::friends::requests(htlc);
    }
}

/// A clearing deleted `requests` requests and declined `friend_requests`
/// friend requests: said in the window, where it is open.
pub(crate) fn cleared(conn: ConnKey, requests: u32, friend_requests: u32) {
    let text = cleared_text(requests, friend_requests);
    let toast = WINDOWS.with_borrow(|w| w.get(&conn).map(|w| w.toast.clone()));
    match toast {
        Some(t) => t.add_toast(adw::Toast::new(&text)),
        None => unsafe { crate::ffi::toolbar_show_toast(crate::cs(&text).as_ptr()) },
    }
}

/// What a clearing did, naming only what it did.
fn cleared_text(requests: u32, friend_requests: u32) -> String {
    let mut said = Vec::new();
    if requests > 0 {
        said.push(trn_argv(
            "Deleted %s message request.",
            "Deleted %s message requests.",
            requests.into(),
            &[&requests.to_string()],
        ));
    }
    if friend_requests > 0 {
        said.push(trn_argv(
            "Declined %s friend request.",
            "Declined %s friend requests.",
            friend_requests.into(),
            &[&friend_requests.to_string()],
        ));
    }
    if said.is_empty() {
        return tr("Nothing was cleared.");
    }
    said.join(" ")
}

/// Draw `conn`'s window, where it is open, from what the connection holds.
pub(crate) fn listed(conn: ConnKey) {
    let htlc = dock::htlc_for(conn);
    if htlc.is_null() {
        return;
    }
    let now = with_roster(htlc, |m| (m.requests.senders.clone(), m.requests.dropped));
    WINDOWS.with_borrow_mut(|w| {
        let Some(win) = w.get_mut(&conn) else {
            return;
        };
        if win.drawn.as_ref() == Some(&now) {
            return;
        }
        while let Some(child) = win.list.first_child() {
            win.list.remove(&child);
        }
        for s in &now.0 {
            win.list.append(&sender(conn, s));
        }
        let dropped = now.1;
        win.dropped.set_visible(dropped > 0);
        let n = dropped.to_string();
        win.dropped.set_text(&trn_argv(
            "The server dropped %s request that could not wait any longer.",
            "The server dropped %s requests that could not wait any longer.",
            dropped.into(),
            &[&n],
        ));
        win.stack
            .set_visible_child_name(if now.0.is_empty() { "empty" } else { "list" });
        win.drawn = Some(now);
    });
}

fn build(conn: ConnKey) {
    let window = adw::Window::new();
    window.set_title(Some(&tr("Message Requests")));
    dock::adopt_window(&window, "message-requests", (600, 640));

    let header = adw::HeaderBar::new();
    let session = unsafe { gtkhx_core::conn::hx_conn_sess(dock::htlc_for(conn).cast()) };
    header.set_title_widget(Some(&adw::WindowTitle::new(
        &tr("Message Requests"),
        &dock::session_label(session),
    )));
    header.pack_end(&clear_menu(conn));

    let list = gtk::Box::new(gtk::Orientation::Vertical, 12);
    let dropped = gtk::Label::new(None);
    dropped.set_wrap(true);
    dropped.set_xalign(0.0);
    dropped.add_css_class("dim-label");
    dropped.add_css_class("caption");
    let column = gtk::Box::new(gtk::Orientation::Vertical, 12);
    column.set_margin_start(12);
    column.set_margin_end(12);
    column.set_margin_top(12);
    column.set_margin_bottom(12);
    column.append(&list);
    column.append(&dropped);
    let clamp = adw::Clamp::new();
    clamp.set_child(Some(&column));
    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scroll.set_vexpand(true);
    scroll.set_child(Some(&clamp));

    let empty = adw::StatusPage::new();
    empty.set_icon_name(Some("mail-read-symbolic"));
    empty.set_title(&tr("No Message Requests"));
    empty.set_description(Some(&tr(
        "Messages from people who are not your friends wait here until you answer them.",
    )));
    let stack = gtk::Stack::new();
    stack.add_named(&scroll, Some("list"));
    stack.add_named(&empty, Some("empty"));
    stack.set_visible_child_name("empty");

    let toast = adw::ToastOverlay::new();
    toast.set_child(Some(&stack));
    let view = adw::ToolbarView::new();
    view.add_top_bar(&header);
    view.set_content(Some(&toast));
    window.set_content(Some(&view));

    window.connect_close_request(move |_| {
        WINDOWS.with_borrow_mut(|w| w.remove(&conn));
        glib::Propagation::Proceed
    });
    dock::close_with(conn, &window);
    let win = Window {
        window: window.clone(),
        bound: dock::bind(conn),
        toast,
        stack,
        list,
        dropped,
        drawn: None,
    };
    WINDOWS.with_borrow_mut(|w| w.insert(conn, win));
    window.present();
}

/// A label for what someone else wrote: plain text, wrapped, never a link.
fn plain_label(text: &str) -> gtk::Label {
    let l = gtk::Label::new(None);
    l.set_use_markup(false);
    l.set_text(text);
    l.set_wrap(true);
    l.set_wrap_mode(gtk::pango::WrapMode::WordChar);
    l.set_xalign(0.0);
    l
}

/// When a waiting message was sent, as the desktop writes a date and time.
fn sent_at(secs: u64) -> String {
    i64::try_from(secs)
        .ok()
        .and_then(|s| glib::DateTime::from_unix_local(s).ok())
        .and_then(|d| d.format("%c").ok())
        .map_or_else(String::new, |s| s.to_string())
}

/// One sender's group: their name, their messages, and what can be done
/// with them.
fn sender(conn: ConnKey, s: &Sender) -> gtk::Box {
    let name = plain_name(&hxproto::text::to_utf8(&s.login));
    let group = gtk::Box::new(gtk::Orientation::Vertical, 6);
    group.add_css_class("card");
    let inner = gtk::Box::new(gtk::Orientation::Vertical, 6);
    inner.set_margin_start(12);
    inner.set_margin_end(12);
    inner.set_margin_top(12);
    inner.set_margin_bottom(12);
    group.append(&inner);

    let title = plain_label(&name);
    title.add_css_class("heading");
    inner.append(&title);
    for m in &s.messages {
        let when = gtk::Label::new(Some(&sent_at(m.sent_at)));
        when.set_xalign(0.0);
        when.add_css_class("caption");
        when.add_css_class("dim-label");
        inner.append(&when);
        inner.append(&plain_label(&plain_text(&m.body)));
    }

    let buttons = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    buttons.set_margin_top(6);
    let answers = [
        (
            tr("Approve"),
            tr1("Approve the requests of %s", &name),
            Do::Approve,
        ),
        (tr("Reply"), tr1("Reply to %s", &name), Do::Reply),
    ];
    for (label, named, what) in answers {
        let b = gtk::Button::with_label(&label);
        b.update_property(&[gtk::accessible::Property::Label(&named)]);
        if what == Do::Approve {
            b.add_css_class("suggested-action");
        }
        let login = s.login.clone();
        b.connect_clicked(move |b| act(conn, &login, what, b.upcast_ref()));
        buttons.append(&b);
    }
    let more = gtk::MenuButton::new();
    more.set_icon_name("view-more-symbolic");
    crate::set_name_tip(&more, &tr1("More for %s", &name));
    let rest = [
        (tr("Add Friend…"), Do::Add),
        (tr("Delete…"), Do::Delete),
        (tr("Block…"), Do::Block),
    ];
    let login = s.login.clone();
    popup_menu(
        &more,
        rest.into_iter().map(|(label, what)| {
            let login = login.clone();
            let f: Box<dyn Fn(&gtk::Widget)> =
                Box::new(move |parent| act(conn, &login, what, parent));
            (label, f)
        }),
    );
    buttons.append(&more);
    inner.append(&buttons);
    group
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Do {
    Approve,
    Reply,
    Add,
    Delete,
    Block,
}

fn act(conn: ConnKey, login: &[u8], what: Do, parent: &gtk::Widget) {
    let Some(bound) = WINDOWS.with_borrow(|w| w.get(&conn).map(|w| w.bound)) else {
        return;
    };
    let Some(htlc) = dock::live_htlc(bound).filter(|h| unsafe { served(*h) }) else {
        return;
    };
    use hxhandlers::send::friends as send;
    match what {
        Do::Approve => unsafe {
            if send::answer_request(htlc, login, RequestAction::Approve).is_some() {
                crate::friends::hush(conn, login);
                crate::msg::open_friend(htlc, login);
            }
        },
        Do::Reply => unsafe { crate::msg::open_friend(htlc, login) },
        Do::Add => crate::friends::add_friend(conn, login, parent),
        Do::Block => crate::friends::block(conn, login, parent),
        Do::Delete => {
            let name = plain_name(&hxproto::text::to_utf8(login));
            let body = tr1("Delete the message requests of %s?", &name);
            confirm(
                bound,
                &tr("Delete Requests"),
                &body,
                &tr("_Delete"),
                parent,
                {
                    let login = login.to_vec();
                    move |htlc| unsafe {
                        send::answer_request(htlc, &login, RequestAction::Delete);
                    }
                },
            );
        }
    }
}

/// Ask first, and do `f` on the connection once the user agrees, where it
/// is still the one asked about.
fn confirm(
    bound: Bound,
    heading: &str,
    body: &str,
    verb: &str,
    parent: &gtk::Widget,
    f: impl Fn(*mut c_void) + 'static,
) {
    let dialog = adw::AlertDialog::new(Some(heading), Some(body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("ok", verb);
    dialog.set_response_appearance("ok", adw::ResponseAppearance::Destructive);
    dialog.set_close_response("cancel");
    unsafe { gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget) };
    dialog.connect_response(Some("ok"), move |_, _| {
        if let Some(htlc) = dock::live_htlc(bound).filter(|h| unsafe { served(*h) }) {
            f(htlc);
        }
    });
    dialog.present(Some(parent));
}

/// The spans Clear… offers: how far back each reaches, in seconds, `None`
/// for everything.
#[derive(Debug, Clone, Copy)]
enum Span {
    Hour,
    Day,
    All,
}

impl Span {
    fn secs(self) -> Option<u64> {
        match self {
            Span::Hour => Some(60 * 60),
            Span::Day => Some(24 * 60 * 60),
            Span::All => None,
        }
    }
}

/// The header's Clear… menu.
fn clear_menu(conn: ConnKey) -> gtk::MenuButton {
    let button = gtk::MenuButton::new();
    button.set_label(&tr("Clear…"));
    let spans = [
        (tr("From the Last Hour"), Span::Hour),
        (tr("From the Last Day"), Span::Day),
        (tr("All"), Span::All),
    ];
    popup_menu(
        &button,
        spans.into_iter().map(|(label, span)| {
            let f: Box<dyn Fn(&gtk::Widget)> = Box::new(move |parent| clear(conn, span, parent));
            (label, f)
        }),
    );
    button
}

/// Give `button` a menu of `items`, each handed the button, as the parent
/// of what it asks, once its menu has closed.
fn popup_menu(
    button: &gtk::MenuButton,
    items: impl IntoIterator<Item = (String, Box<dyn Fn(&gtk::Widget)>)>,
) {
    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    vbox.set_margin_start(4);
    vbox.set_margin_end(4);
    vbox.set_margin_top(4);
    vbox.set_margin_bottom(4);
    let popover = gtk::Popover::new();
    for (label, f) in items {
        let item = gtk::Button::with_label(&label);
        item.add_css_class("flat");
        if let Some(l) = item.child().and_downcast::<gtk::Label>() {
            l.set_xalign(0.0);
        }
        let (popover, button) = (popover.downgrade(), button.downgrade());
        item.connect_clicked(move |_| {
            if let Some(p) = popover.upgrade() {
                p.popdown();
            }
            if let Some(b) = button.upgrade() {
                f(b.upcast_ref());
            }
        });
        vbox.append(&item);
    }
    popover.set_child(Some(&vbox));
    button.set_popover(Some(&popover));
}

/// Clear what arrived within `span`, asking first. The span is reckoned by
/// this computer's clock, which the server's may not agree with, so it
/// says "about".
fn clear(conn: ConnKey, span: Span, parent: &gtk::Widget) {
    let Some(bound) = WINDOWS.with_borrow(|w| w.get(&conn).map(|w| w.bound)) else {
        return;
    };
    let body = match span {
        Span::Hour => tr(
            "Delete the message requests from about the last hour? Friend requests from then are declined too.",
        ),
        Span::Day => tr(
            "Delete the message requests from about the last day? Friend requests from then are declined too.",
        ),
        Span::All => tr("Delete every message request? Every friend request waiting is declined too."),
    };
    confirm(
        bound,
        &tr("Clear Requests"),
        &body,
        &tr("_Clear"),
        parent,
        move |htlc| {
            let now = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_or(0, |d| d.as_secs());
            let since = span.secs().map(|s| now.saturating_sub(s));
            unsafe { hxhandlers::send::friends::clear_requests(htlc, since, None) };
        },
    );
}

#[cfg(test)]
mod tests {
    use super::cleared_text;

    #[test]
    fn a_clearing_names_only_what_it_did() {
        let cases = [
            (0, 0, "Nothing was cleared."),
            (2, 0, "Deleted 2 message requests."),
            (0, 1, "Declined 1 friend request."),
            (
                1,
                3,
                "Deleted 1 message request. Declined 3 friend requests.",
            ),
        ];
        for (requests, friend_requests, want) in cases {
            assert_eq!(cleared_text(requests, friend_requests), want);
        }
    }
}
