//! Private messages. Each conversation is a tab in its connection's Chat
//! panel, beside the public chat and the private chats.
//!
//! [`hxmodel::pm`] is the model: the open conversations, keyed by connection
//! and peer, and the rules for what a message reads as. This module is the
//! view over it — a tab's widgets, its input, the transcript a message is
//! rendered into, the recipient pane above it — and the C ABI the remaining
//! C reaches it through: the `msg` signal's handler hands it each message
//! and has it notified, `users.c` opens a conversation and tells it of a
//! user's change, and the settings hooks restyle its views.
//!
//! A friend's conversation is the same tab, keyed by their Login rather
//! than a uid (`friends` opens it and hands it what the friend family
//! says): it goes as instant messages, each marked with how far it has got,
//! says when the friend is typing, and tells them what has been read once
//! it is shown.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void, CStr};
use std::rc::{Rc, Weak};

use glib::translate::from_glib_none;
use gtk::gdk;
use gtk::glib;
use gtk::pango;
use gtk::prelude::*;
use gtk4 as gtk;

use gtkhx_core::boxed::msg::HxMsgEvent;
use gtkhx_core::conn::{generation, hx_conn_has_cap, hx_conn_name, hx_conn_sess, hx_conn_uid};
use gtkhx_core::messaging::{with_roster, Delivery, Guid, Im};
use hxmodel::friends::{self, TheirTyping, Typing};
use hxmodel::pm::{self, Conversation, Conversations, Peer};
use hxproto::messaging::CAP_MESSAGING;

use crate::chat_tabs;
use crate::dock::{self, ConnKey};
use crate::tr::{tr, tr1};

/// `ROTULUS_PAL_NICK_BRACKET` and `ROTULUS_PAL_SELF_BRACKET` (rotulus.h,
/// pinned in chat.c): the brackets around someone else's nick and around ours.
const PAL_NICK_BRACKET: i16 = 41;
const PAL_SELF_BRACKET: i16 = 42;
/// The palette's grey, which a message's delivery and our own notices wear.
const PAL_GREY: i16 = 14;
/// `MSG` (sound.h, pinned in chat.c): private messages, friends' among
/// them, and broadcasts.
pub(crate) const SOUND_MSG: std::ffi::c_int = 6;

extern "C" {
    // chat.c — the palette every chat view is drawn with, and a nick's color.
    static mut colors: gdk::ffi::GdkRGBA;
    fn hx_chat_nick_color(nick: *const c_char, len: usize, is_self: glib::ffi::gboolean) -> i16;
    // gtkhx.c — the chat font, and its application to an input.
    static mut gtkhx_font_desc: *mut pango::ffi::PangoFontDescription;
    fn gtkhx_apply_input_font(w: *mut gtk::ffi::GtkWidget);
    fn gtkhx_apply_input_style(w: *mut gtk::ffi::GtkWidget);
    // users.c — a status word's color, or NULL for the theme's foreground.
    fn user_color_gdk(color: u16) -> *const gdk::ffi::GdkRGBA;
    // notify.c / sound.c
    pub(crate) fn gtkhx_notify_send(id: *const c_char, title: *const c_char, body: *const c_char);
    pub(crate) fn play_sound(sound: std::ffi::c_int);
}

/// One conversation's tab. The widgets are held weakly: the tab strip owns
/// them, and a connection's close destroys its Chat page before its
/// conversations are forgotten.
struct MsgWin {
    conn: ConnKey,
    peer: Peer,
    model: RefCell<Conversation>,
    content: glib::WeakRef<gtk::Box>,
    output: glib::WeakRef<gtk::Widget>,
    input: glib::WeakRef<gtk::TextView>,
    info_image: glib::WeakRef<gtk::Image>,
    info_label: glib::WeakRef<gtk::Label>,
    /// A friend's conversation's: who is typing to us.
    typing: glib::WeakRef<gtk::Label>,
    im: RefCell<ImState>,
    /// The window the tab is shown in while it is, and our handler on it.
    shown_in: RefCell<Option<(glib::WeakRef<gtk::Window>, glib::SignalHandlerId)>>,
}

/// What a friend's conversation keeps of its instant messages.
#[derive(Default)]
struct ImState {
    typing: Typing,
    theirs: TheirTyping,
    /// What we sent and is not read yet, by id: its first row, to mark it
    /// again as it gets further.
    sent: HashMap<Guid, Sent>,
    /// What arrived and is not shown yet, to tell its sender once it is.
    unread: Vec<Guid>,
}

struct Sent {
    /// The rotulus mark of the message's first row.
    mark: usize,
    line: String,
    stamp: i64,
    /// Our nick as it was sent under.
    nick: Vec<u8>,
    /// How far it has got, and on which connect of its connection.
    at: Delivery,
    connect: u32,
}

thread_local! {
    static WINDOWS: RefCell<Conversations<Rc<MsgWin>>> = RefCell::new(Conversations::default());
}

fn find(conn: ConnKey, peer: &Peer) -> Option<Rc<MsgWin>> {
    WINDOWS.with_borrow(|t| t.get(conn, peer).cloned())
}

/// `conn`'s conversations, out of the table, so what is done to them can
/// call back in.
fn of_conn(conn: ConnKey) -> Vec<Rc<MsgWin>> {
    WINDOWS.with_borrow(|t| t.of_conn(conn).cloned().collect())
}

/// `*mut GtkWidget` for a gtk-rs widget.
fn wptr<W: IsA<gtk::Widget>>(w: &W) -> *mut gtk::ffi::GtkWidget {
    w.as_ref().as_ptr()
}

/// # Safety
/// `p` is NULL or a valid C string that outlives the borrow.
unsafe fn opt_cstr<'a>(p: *const c_char) -> Option<&'a CStr> {
    (!p.is_null()).then(|| CStr::from_ptr(p))
}

/// The whole input, up to any NUL: it goes out as a C string.
fn whole(buf: &gtk::TextBuffer) -> String {
    let text = buf.text(&buf.start_iter(), &buf.end_iter(), false);
    let end = text.find('\0').unwrap_or(text.len());
    text[..end].to_owned()
}

fn set_line(buf: &gtk::TextBuffer, s: &str) {
    buf.set_text(s);
    buf.place_cursor(&buf.end_iter());
}

/// The chat font's description, as `gtkhx_chat_view_new` takes it.
unsafe fn font_name() -> Option<std::ffi::CString> {
    let desc = std::ptr::addr_of!(gtkhx_font_desc).read();
    if desc.is_null() {
        return None;
    }
    let desc: pango::FontDescription = from_glib_none(desc);
    Some(crate::cs(&desc.to_str()))
}

/// Open a conversation with `peer` on `htlc`, as its tab: built, filed,
/// selected, and its input focused.
///
/// # Safety
/// `htlc` is NULL or a live connection. Main thread.
unsafe fn create(htlc: *mut c_void, peer: &Peer, name: &str) -> Option<Rc<MsgWin>> {
    if htlc.is_null() {
        return None;
    }
    crate::ensure_gtk_init();
    let conn = dock::conn_key(htlc);

    let font = font_name();
    let raw = crate::chat_view::gtkhx_chat_view_new(
        std::ptr::addr_of!(colors),
        font.as_ref().map_or(std::ptr::null(), |f| f.as_ptr()),
        conn,
    );
    let output: gtk::Widget = from_glib_none(raw);
    let adjustment: Option<gtk::Adjustment> =
        from_glib_none(rotulus::ffi::rotulus_view_get_vadjustment(raw));
    let vscroll = gtk::Scrollbar::new(gtk::Orientation::Vertical, adjustment.as_ref());

    let input = gtk::TextView::new();
    gtkhx_apply_input_font(wptr(&input));
    gtkhx_apply_input_style(wptr(&input));
    input.set_editable(true);
    input.set_wrap_mode(gtk::WrapMode::Word);
    // Inner margins so the text isn't clipped by the frame's rounded corners.
    input.set_left_margin(6);
    input.set_right_margin(6);
    input.set_top_margin(4);
    input.set_bottom_margin(4);

    // Recipient info pane: icon, and the name over its status.
    let info_image = gtk::Image::new();
    info_image.set_pixel_size(32);
    info_image.set_size_request(32, 32);
    let info_label = gtk::Label::new(None);
    info_label.set_xalign(0.0);
    info_label.set_yalign(0.5);
    info_label.set_use_markup(true);
    info_label.set_ellipsize(pango::EllipsizeMode::End);
    info_label.set_hexpand(true);

    let outer_vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    let typing = matches!(peer, Peer::Login(_)).then(|| {
        let l = gtk::Label::new(None);
        l.set_xalign(0.0);
        l.add_css_class("dim-label");
        l.set_visible(false);
        l
    });

    let win = Rc::new(MsgWin {
        conn,
        peer: peer.clone(),
        model: RefCell::new(Conversation::new(name)),
        content: outer_vbox.downgrade(),
        output: output.downgrade(),
        input: input.downgrade(),
        info_image: info_image.downgrade(),
        info_label: info_label.downgrade(),
        typing: typing.as_ref().map(|l| l.downgrade()).unwrap_or_default(),
        im: RefCell::default(),
        shown_in: RefCell::default(),
    });
    // Before the emoji typeahead attaches its own, so Return reaches this
    // one first.
    attach_keys(&input, Rc::downgrade(&win));

    // Output frame: the transcript and its scrollbar.
    let hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    output.set_hexpand(true);
    output.set_vexpand(true);
    hbox.append(&output);
    hbox.append(&vscroll);
    let outputframe = gtk::Frame::new(None);
    outputframe.set_child(Some(&hbox));

    // Auto-grow input (1-line min, 5-line max), with its emoji and attach
    // buttons alongside.
    let input_scroll = gtk::ScrolledWindow::new();
    input_scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    input_scroll.set_overflow(gtk::Overflow::Visible);
    input_scroll.set_propagate_natural_height(true);
    input_scroll.set_min_content_height(28);
    input_scroll.set_max_content_height(120);
    input_scroll.set_child(Some(&input));

    let input_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    input_scroll.set_hexpand(true);
    input_hbox.append(&input_scroll);
    let emoji_btn: gtk::Widget = from_glib_none(crate::emoji::hx_emoji_button_new(wptr(&input)));
    emoji_btn.set_valign(gtk::Align::End);
    input_hbox.append(&emoji_btn);
    // An instant message carries text alone.
    if let Peer::Uid(uid) = peer {
        let attach = crate::media_attach::message_button(htlc, *uid);
        attach.set_valign(gtk::Align::End);
        input_hbox.append(&attach);
    }
    crate::emoji::hx_emoji_typeahead_attach(wptr(&input));

    let inputframe = gtk::Frame::new(None);
    inputframe.set_child(Some(&input_hbox));

    let vpane = gtk::Box::new(gtk::Orientation::Vertical, 4);
    outputframe.set_vexpand(true);
    inputframe.set_vexpand(false);
    vpane.append(&outputframe);
    if let Some(typing) = &typing {
        vpane.append(typing);
    }
    vpane.append(&inputframe);
    vpane.set_margin_start(5);
    vpane.set_margin_end(5);
    vpane.set_margin_top(5);
    vpane.set_margin_bottom(5);

    let info_box = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    info_box.set_margin_start(10);
    info_box.set_margin_end(10);
    info_box.set_margin_top(6);
    info_box.set_margin_bottom(4);
    info_box.append(&info_image);
    info_box.append(&info_label);

    outer_vbox.append(&info_box);
    outer_vbox.append(&gtk::Separator::new(gtk::Orientation::Horizontal));
    vpane.set_vexpand(true);
    outer_vbox.append(&vpane);

    // With no Chat page to hold it, the tab would be dropped as this returns,
    // and filing it would leave a conversation nothing can show or reopen.
    if !chat_tabs::add_msg(htlc, &outer_vbox, peer, &title(peer, name)) {
        crate::emoji::hx_emoji_typeahead_detach(wptr(&input));
        return None;
    }
    WINDOWS.with_borrow_mut(|t| t.insert(conn, peer.clone(), win.clone()));

    if let Peer::Login(_) = peer {
        let weak = Rc::downgrade(&win);
        input.buffer().connect_changed(move |buf| {
            if let Some(win) = weak.upgrade() {
                typed(&win, buf.char_count() == 0);
            }
        });
        let weak = Rc::downgrade(&win);
        outer_vbox.connect_map(move |w| {
            if let Some(win) = weak.upgrade() {
                watch_window(&win, w.root().and_downcast::<gtk::Window>());
                mark_read(&win);
            }
        });
        let weak = Rc::downgrade(&win);
        outer_vbox.connect_unmap(move |_| {
            if let Some(win) = weak.upgrade() {
                watch_window(&win, None);
            }
        });
        // A friend has no icon to show.
        info_image.set_visible(false);
    }

    refresh_user_info(&win, htlc);
    chat_tabs::raise_msg(htlc, peer);
    crate::ffi::init_keyaccel(wptr(&outer_vbox));
    input.grab_focus();

    Some(win)
}

/// The input's keys. Return sends, Shift+Return is a newline, Up and Down
/// walk what was sent, and Ctrl+K opens Connect.
fn attach_keys(input: &gtk::TextView, win: Weak<MsgWin>) {
    use gdk::{Key, ModifierType};

    let keys = gtk::EventControllerKey::new();
    keys.connect_key_pressed(move |ctrl, key, _code, state| {
        let Some(win) = win.upgrade() else {
            return glib::Propagation::Proceed;
        };
        let Some(view) = ctrl.widget().and_downcast::<gtk::TextView>() else {
            return glib::Propagation::Proceed;
        };
        let buf = view.buffer();
        if state.contains(ModifierType::CONTROL_MASK) {
            if key == Key::k || key == Key::K {
                unsafe {
                    crate::ffi::create_connect_window(
                        std::ptr::null_mut(),
                        crate::ffi::hx_active_session(),
                    )
                };
                return glib::Propagation::Stop;
            }
        } else if state.contains(ModifierType::SHIFT_MASK) && key == Key::Return {
            return glib::Propagation::Proceed;
        } else if key == Key::Return {
            win.model.borrow_mut().history.record(&whole(&buf));
            send(&win, &buf);
            return glib::Propagation::Stop;
        } else if key == Key::Up {
            let shown = win.model.borrow_mut().history.up(&whole(&buf));
            if let Some(line) = shown {
                set_line(&buf, &line);
                return glib::Propagation::Stop;
            }
        } else if key == Key::Down {
            let shown = win.model.borrow_mut().history.down();
            if let Some(line) = shown {
                set_line(&buf, &line);
                return glib::Propagation::Stop;
            }
        }
        glib::Propagation::Proceed
    });
    input.add_controller(keys);
}

/// Send what is typed to the conversation's peer, and show it.
fn send(win: &MsgWin, buf: &gtk::TextBuffer) {
    let text = whole(buf);
    let (mut start, mut end) = (buf.start_iter(), buf.end_iter());
    buf.delete(&mut start, &mut end);
    if text.is_empty() {
        return;
    }
    let htlc = dock::htlc_for(win.conn);
    if htlc.is_null() {
        return;
    }
    match &win.peer {
        Peer::Uid(uid) => {
            let wire = crate::cs(&text.replace('\n', "\r"));
            unsafe {
                echo(htlc, *uid, &crate::cs(&text));
                hxhandlers::send::msg::hx_send_msg(htlc, *uid, wire.as_ptr());
            }
        }
        Peer::Login(login) => unsafe { send_im(win, htlc, login, &text) },
    }
}

/// Whether `login` is a friend `htlc` can send an instant message to.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn reaches(htlc: *mut c_void, login: &[u8]) -> bool {
    hx_conn_has_cap(htlc.cast(), CAP_MESSAGING) != 0
        && with_roster(htlc, |r| friends::is_friend(r, login))
}

/// An instant message to `login`, shown marked as it goes; to someone who is
/// no friend, a note that it did not go, since the server would refuse it.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn send_im(win: &MsgWin, htlc: *mut c_void, login: &[u8], text: &str) {
    if !reaches(htlc, login) {
        let name = win.model.borrow().name.clone();
        notice(
            win,
            &tr1("Not sent: %s is not a friend on this server.", &name),
        );
        return;
    }
    let Some(guid) = hxhandlers::send::friends::im(htlc, login, text) else {
        return;
    };
    let stamp = now();
    let own = opt_cstr(hx_conn_name(htlc.cast()));
    let nick = own.map_or(&b""[..], CStr::to_bytes);
    let Some(view) = win.output.upgrade() else {
        return;
    };
    let mut lines = pm::rows(text);
    let first = lines.next().unwrap_or_default().to_owned();
    let line = Line {
        nick,
        is_self: true,
        outgoing: true,
        speaker: hx_conn_uid(htlc.cast()),
        stamp,
    };
    let mark = line.put(
        &view,
        None,
        &first,
        Some(&delivery_words(Delivery::Sending)),
    );
    for rest in lines {
        rotulus::ffi::rotulus_view_append_text(
            wptr(&view),
            rest.as_ptr().cast(),
            rest.len() as i32,
            stamp,
        );
    }
    let sent = Sent {
        mark: mark as usize,
        line: first,
        stamp,
        nick: nick.to_vec(),
        at: Delivery::Sending,
        connect: generation(htlc.cast()),
    };
    win.im.borrow_mut().sent.insert(guid, sent);
}

/// Now, in Unix seconds, for a row that is drawn again later with the time
/// it was first drawn at.
fn now() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs() as i64)
}

/// What a message we sent is marked with, for how far it has got.
fn delivery_words(d: Delivery) -> String {
    match d {
        Delivery::Sending => tr("sending…"),
        Delivery::Sent => tr("sent"),
        Delivery::Queued => tr("waiting for them"),
        Delivery::Delivered => tr("delivered"),
        Delivery::Read => tr("read"),
        Delivery::Failed => tr("not delivered"),
    }
}

/// A line of our own in a conversation, about it rather than in it.
fn notice(win: &MsgWin, text: &str) {
    let Some(view) = win.output.upgrade() else {
        return;
    };
    let run = run(text.as_bytes(), PAL_GREY, rotulus::ffi::ATTR_ITALIC);
    let row = rotulus::ffi::RotulusRow {
        kind: rotulus::ffi::ROW_SYSTEM,
        flags: 0,
        stamp: 0,
        speaker: rotulus::ffi::RotulusSpeaker {
            key: 0,
            nick: std::ptr::null(),
            nick_len: -1,
        },
        gutter: std::ptr::null(),
        n_gutter: 0,
        body: &run,
        n_body: 1,
    };
    unsafe { rotulus::ffi::rotulus_view_append(wptr(&view), &row) };
}

fn run(text: &[u8], color: i16, attrs: u16) -> rotulus::ffi::RotulusRun {
    rotulus::ffi::RotulusRun {
        text: text.as_ptr().cast(),
        len: text.len() as i32,
        color,
        attrs,
        background: 0,
        rgb: 0,
        background_rgb: 0,
    }
}

/// The first row of a message: who it is from, and how it is drawn.
struct Line<'a> {
    nick: &'a [u8],
    /// Whose colors the nick wears.
    is_self: bool,
    /// The direction, which `is_self` cannot tell when you message yourself
    /// (see [`pm::speaker`]).
    outgoing: bool,
    /// Whose avatar it shows.
    speaker: u16,
    /// Unix seconds; 0 is now.
    stamp: i64,
}

impl Line<'_> {
    /// Draw `text` as this row, after it any `mark`; in place of the row
    /// `at` marks where there is one, else at the end. The row's mark.
    ///
    /// # Safety
    /// Main thread.
    unsafe fn put(
        &self,
        view: &gtk::Widget,
        at: Option<usize>,
        text: &str,
        mark: Option<&str>,
    ) -> *mut c_void {
        let brackets = if self.is_self {
            PAL_SELF_BRACKET
        } else {
            PAL_NICK_BRACKET
        };
        let nick_color = hx_chat_nick_color(
            self.nick.as_ptr().cast(),
            self.nick.len(),
            self.is_self.into(),
        );
        let gutter = [
            run(b"<", brackets, 0),
            run(self.nick, nick_color, 0),
            run(b">", brackets, 0),
        ];
        let mark = mark.map(|m| format!("  {}", tr1("(%s)", m)));
        let mut body = vec![run(text.as_bytes(), rotulus::ffi::ROTULUS_COLOR_DEFAULT, 0)];
        if let Some(m) = &mark {
            body.push(run(m.as_bytes(), PAL_GREY, rotulus::ffi::ATTR_ITALIC));
        }
        let row = rotulus::ffi::RotulusRow {
            kind: rotulus::ffi::ROW_MESSAGE,
            flags: if self.outgoing {
                rotulus::ffi::ROW_OUTGOING
            } else {
                0
            },
            stamp: self.stamp,
            speaker: rotulus::ffi::RotulusSpeaker {
                key: self.speaker.into(),
                nick: self.nick.as_ptr().cast(),
                nick_len: self.nick.len() as i32,
            },
            gutter: gutter.as_ptr(),
            n_gutter: gutter.len() as i32,
            body: body.as_ptr(),
            n_body: body.len() as i32,
        };
        match at {
            Some(at) => {
                rotulus::ffi::rotulus_view_replace(wptr(view), at as *mut c_void, &row);
                at as *mut c_void
            }
            None => rotulus::ffi::rotulus_view_append(wptr(view), &row),
        }
    }
}

/// The input of a friend's conversation became `empty`, or not: tell them
/// we started or stopped typing, as that changes.
fn typed(win: &MsgWin, empty: bool) {
    let Peer::Login(login) = &win.peer else {
        return;
    };
    let htlc = dock::htlc_for(win.conn);
    if htlc.is_null() || !unsafe { reaches(htlc, login) } {
        return;
    }
    let change = win.im.borrow_mut().typing.edited(empty);
    if let Some(on) = change {
        unsafe { hxhandlers::send::friends::typing(htlc, login, on) };
    }
}

/// Follow whether `window`, the one a friend's conversation is now shown
/// in, has the focus; `None`, it is shown nowhere.
fn watch_window(win: &Rc<MsgWin>, window: Option<gtk::Window>) {
    if let Some((old, id)) = win.shown_in.take() {
        if let Some(old) = old.upgrade() {
            old.disconnect(id);
        }
    }
    let Some(window) = window else {
        return;
    };
    let weak = Rc::downgrade(win);
    let id = window.connect_is_active_notify(move |_| {
        if let Some(win) = weak.upgrade() {
            mark_read(&win);
        }
    });
    *win.shown_in.borrow_mut() = Some((window.downgrade(), id));
}

/// A friend's conversation is in front of the user, its tab shown in the
/// window that has the focus: what it holds unread has been read.
/// Whether the conversation is on screen in the window that has the focus.
fn is_focused(win: &MsgWin) -> bool {
    win.content.upgrade().is_some_and(|c| {
        c.is_mapped()
            && c.root()
                .and_downcast::<gtk::Window>()
                .is_some_and(|w| w.is_active())
    })
}

fn mark_read(win: &MsgWin) {
    let Peer::Login(login) = &win.peer else {
        return;
    };
    if !is_focused(win) {
        return;
    }
    let htlc = dock::htlc_for(win.conn);
    if htlc.is_null() {
        return;
    }
    let unread = std::mem::take(&mut win.im.borrow_mut().unread);
    for guid in unread {
        unsafe { hxhandlers::send::friends::read(htlc, &guid, login) };
    }
}

/// Show `body` as sent by us to `uid` — a server sends a private message to
/// its recipient only — and hand back the transcript it went into.
///
/// # Safety
/// `htlc` is NULL or a live connection. Main thread.
pub(crate) unsafe fn echo(htlc: *mut c_void, uid: u16, body: &CStr) -> Option<gtk::Widget> {
    if htlc.is_null() {
        return None;
    }
    let own = opt_cstr(hx_conn_name(htlc.cast()));
    let own_bytes = own.map_or(&b""[..], CStr::to_bytes);
    let is_self = pm::is_self(own.map(CStr::to_bytes), own_bytes);
    render(htlc, own, uid, Some(body.to_bytes()), is_self, true).and_then(|w| w.output.upgrade())
}

/// Render a message into its conversation, opening it if it isn't, and flag
/// the tab when it isn't ours.
///
/// `is_self` picks whose colors the nick wears; `outgoing` is the direction,
/// which `is_self` cannot tell when you message yourself (see
/// [`pm::speaker`]).
///
/// # Safety
/// `htlc` is NULL or a live connection. Main thread.
unsafe fn render(
    htlc: *mut c_void,
    name: Option<&CStr>,
    uid: u16,
    body: Option<&[u8]>,
    is_self: bool,
    outgoing: bool,
) -> Option<Rc<MsgWin>> {
    if htlc.is_null() || hx_conn_sess(htlc.cast()).is_null() {
        return None;
    }
    let peer = Peer::Uid(uid);
    let win = match find(dock::conn_key(htlc), &peer) {
        Some(win) => win,
        None => create(htlc, &peer, &name.unwrap_or(c"").to_string_lossy())?,
    };
    // Pango asserts UTF-8, and a vintage server's text is Mac Roman.
    let body = hxproto::text::to_utf8(body.unwrap_or_default());
    let line = Line {
        nick: name.map_or(&b""[..], CStr::to_bytes),
        is_self,
        outgoing,
        speaker: pm::speaker(outgoing, hx_conn_uid(htlc.cast()), uid),
        stamp: 0,
    };
    draw(&win, &line, &body);
    if !is_self {
        chat_tabs::set_attention_msg(htlc, &peer, true);
    }
    Some(win)
}

/// Draw a message into `win`: its first row as `line`, the rest as text.
///
/// # Safety
/// Main thread.
unsafe fn draw(win: &MsgWin, line: &Line<'_>, body: &str) {
    let Some(view) = win.output.upgrade() else {
        return;
    };
    for (i, text) in pm::rows(body).enumerate() {
        if i == 0 {
            line.put(&view, None, text, None);
        } else {
            rotulus::ffi::rotulus_view_append_text(
                wptr(&view),
                text.as_ptr().cast(),
                text.len() as i32,
                line.stamp,
            );
        }
    }
}

/// An instant message `htlc` received: shown in its sender's conversation,
/// opened if need be, notified as a private message is, and read if it is
/// on screen.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn im(htlc: *mut c_void, m: &Im) {
    if hx_conn_sess(htlc.cast()).is_null() {
        return;
    }
    let peer = Peer::Login(m.from.clone());
    let name = with_roster(htlc, |r| friends::name_of(r, &m.from));
    let win = match find(dock::conn_key(htlc), &peer) {
        Some(win) => win,
        None => match create(htlc, &peer, &name) {
            Some(win) => win,
            None => return,
        },
    };
    let line = Line {
        nick: name.as_bytes(),
        is_self: false,
        outgoing: false,
        speaker: 0,
        stamp: i64::try_from(m.sent_at).unwrap_or(0),
    };
    draw(&win, &line, &m.body);
    show_typing(&win, false);
    chat_tabs::set_attention_msg(htlc, &peer, true);
    win.im.borrow_mut().unread.push(m.guid);
    mark_read(&win);
    notify(htlc, &peer, &name, &m.body);
    play_sound(SOUND_MSG);
}

/// A message we sent to `to` got further: mark it so, and once it can get
/// no further, let go of it.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn delivered(htlc: *mut c_void, to: &[u8], guid: &Guid, d: Delivery) {
    let Some(win) = find(dock::conn_key(htlc), &Peer::Login(to.to_vec())) else {
        return;
    };
    let Some(view) = win.output.upgrade() else {
        return;
    };
    let mut im = win.im.borrow_mut();
    let Some(sent) = im.sent.get_mut(guid) else {
        return;
    };
    sent.at = d;
    let line = Line {
        nick: &sent.nick,
        is_self: true,
        outgoing: true,
        speaker: hx_conn_uid(htlc.cast()),
        stamp: sent.stamp,
    };
    line.put(&view, Some(sent.mark), &sent.line, Some(&delivery_words(d)));
    if matches!(d, Delivery::Read | Delivery::Failed) {
        im.sent.remove(guid);
    }
}

/// `htlc`'s connection changed state: what was sent on an earlier connect
/// will hear no more, but for what was still being sent, which the
/// roster's own forgetting fails.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn forget_sent(htlc: *mut c_void) {
    let connect = generation(htlc.cast());
    for win in of_conn(dock::conn_key(htlc)) {
        win.im
            .borrow_mut()
            .sent
            .retain(|_, s| s.connect == connect || s.at == Delivery::Sending);
    }
}

/// `from` started or stopped typing to us.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn typing(htlc: *mut c_void, from: &[u8], on: bool) {
    if let Some(win) = find(dock::conn_key(htlc), &Peer::Login(from.to_vec())) {
        show_typing(&win, on);
    }
}

/// Show, or stop showing, that the friend is typing; said aloud once as it
/// starts, and let lapse unless they say it again.
fn show_typing(win: &Rc<MsgWin>, on: bool) {
    let Some(label) = win.typing.upgrade() else {
        return;
    };
    let now = std::time::Instant::now();
    let news = {
        let mut im = win.im.borrow_mut();
        if on {
            im.theirs.started(now)
        } else {
            im.theirs.stopped();
            false
        }
    };
    let text = tr1("%s is typing…", &win.model.borrow().name);
    label.set_text(&text);
    label.set_visible(on);
    if news {
        crate::announce::announce(Some(label.upcast_ref()), &text, false);
    }
    if on {
        let weak = Rc::downgrade(win);
        glib::timeout_add_local_once(friends::TYPING_LAPSE, move || {
            let Some(win) = weak.upgrade() else {
                return;
            };
            let on = win.im.borrow().theirs.is_on(std::time::Instant::now());
            if let (false, Some(label)) = (on, win.typing.upgrade()) {
                label.set_visible(false);
            }
        });
    }
}

/// `login`'s entry changed: redraw its conversation's pane, and a friend
/// who has gone is typing no more.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn friend_changed(htlc: *mut c_void, login: &[u8]) {
    let Some(win) = find(dock::conn_key(htlc), &Peer::Login(login.to_vec())) else {
        return;
    };
    let present = with_roster(htlc, |r| {
        r.get(login).map(|f| friends::is_present(f.presence))
    });
    if present != Some(true) {
        show_typing(&win, false);
    }
    refresh_user_info(&win, htlc);
}

/// Every conversation of `htlc` with a friend: their entries were read
/// again.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn friends_changed(htlc: *mut c_void) {
    for win in of_conn(dock::conn_key(htlc)) {
        if let Peer::Login(login) = &win.peer {
            friend_changed(htlc, login);
        }
    }
}

/// Tell the desktop of a message from `peer`, as of every private message:
/// announced, and notified where the settings ask for it.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn notify(htlc: *mut c_void, peer: &Peer, name: &str, body: &str) {
    let (cname, cbody) = (crate::cs(name), crate::cs(body));
    crate::announce::gtkhx_announce_line(cname.as_ptr(), cbody.as_ptr(), glib::ffi::GTRUE);
    if !hxconfig::ffi::with_settings(|s| s.notify.private_message).unwrap_or(false) {
        return;
    }
    if hxconfig::ffi::with_settings(|s| s.notify.omit_focused).unwrap_or(false)
        && find(dock::conn_key(htlc), peer).is_some_and(|win| is_focused(&win))
    {
        return;
    }
    let id = crate::cs(&chat_tabs::notify_id(dock::conn_key(htlc), peer));
    let shown = if name.is_empty() { "?" } else { name };
    let title = crate::cs(&tr1("%s (private message)", shown));
    gtkhx_notify_send(id.as_ptr(), title.as_ptr(), cbody.as_ptr());
}

/// `void gtkhx_notify_msg (struct htlc_conn *, HxMsgEvent *)` — tell the
/// desktop of a private message `htlc` received; not of one of our own.
///
/// # Safety
/// `htlc` is a live connection; `event` is NULL or valid. Main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_notify_msg(htlc: *mut c_void, event: *const HxMsgEvent) {
    let Some(e) = event.as_ref() else {
        return;
    };
    if e.is_self != 0 {
        return;
    }
    // In the server's encoding, as the transcript reads them.
    let text = |p| hxproto::text::to_utf8(opt_cstr(p).map_or(&[][..], CStr::to_bytes));
    notify(htlc, &Peer::Uid(e.uid), &text(e.name), &text(e.body));
}

/// Draw the info pane from the public chat's entry for the peer, or from
/// the name the conversation was opened with when it has left.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn refresh_user_info(win: &MsgWin, htlc: *mut c_void) {
    let uid = match &win.peer {
        Peer::Uid(uid) => *uid,
        Peer::Login(login) => {
            friend_view(win, htlc, login);
            return;
        }
    };
    if let Some(mi) = crate::friends::member(hx_conn_sess(htlc.cast()).cast(), 0, uid) {
        let name = CStr::from_ptr(mi.name.as_ptr()).to_string_lossy();
        apply_user_view(win, htlc, Some(&name), mi.icon, mi.status, true);
    } else {
        apply_user_view(win, htlc, None, 0, 0, false);
    }
}

/// Paint a friend's conversation's pane: their name over their Login and
/// presence, as their entry says, or the name it was opened with once there
/// is no entry.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn friend_view(win: &MsgWin, htlc: *mut c_void, login: &[u8]) {
    let Some(label) = win.info_label.upgrade() else {
        return;
    };
    let entry = with_roster(htlc, |r| r.get(login).cloned());
    let name = entry
        .as_ref()
        .map_or_else(|| win.model.borrow().name.clone(), friends::display_name);
    let login = hxproto::text::to_utf8(login);
    let status = entry
        .as_ref()
        .map(|f| crate::friends::presence_line(f.presence, f.status.as_deref()));
    label.set_markup(&friend_markup(&name, &login, status.as_deref()));
    chat_tabs::set_title_msg(htlc, &win.peer, &pm::login_title(&name, &login));
    name_inputs(win, &name);
}

/// A friend's pane's markup: the name over the Login and, where there is an
/// entry, the presence.
fn friend_markup(name: &str, login: &str, status: Option<&str>) -> String {
    let name = glib::markup_escape_text(name);
    let login = glib::markup_escape_text(login);
    match status {
        Some(s) => format!(
            "<b>{name}</b>\n<small>{login} \u{b7} {}</small>",
            glib::markup_escape_text(s)
        ),
        None => format!("<b>{name}</b>\n<small>{login}</small>"),
    }
}

/// The info pane's markup: the name, colored when its status has a color,
/// over the uid and, when the user is known, their icon and status.
fn info_markup(name: &str, uid: u16, known: Option<(u16, &str, Option<&str>)>) -> String {
    let name = glib::markup_escape_text(name);
    match known {
        Some((icon, status, Some(hex))) => format!(
            "<span foreground=\"{hex}\"><b>{name}</b></span>\n\
             <small>UID {uid} \u{b7} Icon {icon} \u{b7} {status}</small>"
        ),
        Some((icon, status, None)) => {
            format!("<b>{name}</b>\n<small>UID {uid} \u{b7} Icon {icon} \u{b7} {status}</small>")
        }
        None => format!("<b>{name}</b>\n<small>UID {uid}</small>"),
    }
}

/// `#rrggbb` for a status word's color, or None for the theme's foreground.
unsafe fn status_hex(color: u16) -> Option<String> {
    let c = user_color_gdk(color).as_ref()?;
    let byte = |v: f32| (v as f64 * 255.0 + 0.5) as i32;
    Some(format!(
        "#{:02x}{:02x}{:02x}",
        byte(c.red),
        byte(c.green),
        byte(c.blue)
    ))
}

/// Paint the info pane, and name the tab, input and transcript, for
/// `display` (or the opening name when that is empty). The icon is always
/// reloaded, since a user can change theirs mid-conversation.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn apply_user_view(
    win: &MsgWin,
    htlc: *mut c_void,
    display: Option<&str>,
    icon: u16,
    color: u16,
    known: bool,
) {
    let (Some(label), Some(image)) = (win.info_label.upgrade(), win.info_image.upgrade()) else {
        return;
    };
    let Peer::Uid(uid) = win.peer else {
        return;
    };
    let display = match display {
        Some(d) if !d.is_empty() => d.to_owned(),
        _ => win.model.borrow().name.clone(),
    };
    name_widgets(win, htlc, &display);

    let markup = if known {
        let status = pm::Status::of(color);
        let words = format!(
            "{}{}",
            if status.admin {
                tr("Admin")
            } else {
                tr("Guest")
            },
            if status.away {
                tr(" (Away)")
            } else {
                String::new()
            }
        );
        let hex = status_hex(color);
        info_markup(&display, uid, Some((icon, &words, hex.as_deref())))
    } else {
        info_markup(&display, uid, None)
    };
    label.set_markup(&markup);

    match crate::icon_picker::preview_texture(icon) {
        Some(texture) => {
            image.set_paintable(Some(&texture));
            image.set_pixel_size(32);
        }
        None => image.clear(),
    }
}

/// Name a conversation's tab, input and transcript for `nick`.
fn name_widgets(win: &MsgWin, htlc: *mut c_void, nick: &str) {
    chat_tabs::set_title_msg(htlc, &win.peer, &title(&win.peer, nick));
    name_inputs(win, nick);
}

/// A conversation's tab title.
fn title(peer: &Peer, name: &str) -> String {
    match peer {
        Peer::Uid(uid) => pm::title(name, *uid),
        Peer::Login(login) => pm::login_title(name, &hxproto::text::to_utf8(login)),
    }
}

/// Name a conversation's input and transcript for `nick`.
fn name_inputs(win: &MsgWin, nick: &str) {
    if let Some(input) = win.input.upgrade() {
        input.update_property(&[gtk::accessible::Property::Label(&tr1(
            "Message to %s",
            nick,
        ))]);
    }
    if let Some(output) = win.output.upgrade() {
        output.update_property(&[gtk::accessible::Property::Label(&tr1(
            "Messages with %s",
            nick,
        ))]);
    }
}

/// Open the conversation with `uid` on `htlc`, or select it if it is open.
///
/// # Safety
/// `htlc` is NULL or a live connection. Main thread.
pub(crate) unsafe fn open(htlc: *mut c_void, uid: u16, name: &CStr) {
    open_peer(htlc, &Peer::Uid(uid), &name.to_string_lossy());
}

/// Open the conversation with the friend `login` on `htlc`, or select it if
/// it is open.
///
/// # Safety
/// `htlc` is NULL or a live connection. Main thread.
pub(crate) unsafe fn open_friend(htlc: *mut c_void, login: &[u8]) {
    let name = with_roster(htlc, |r| friends::name_of(r, login));
    open_peer(htlc, &Peer::Login(login.to_vec()), &name);
}

unsafe fn open_peer(htlc: *mut c_void, peer: &Peer, name: &str) {
    if find(dock::conn_key(htlc), peer).is_some() {
        chat_tabs::raise_msg(htlc, peer);
    } else {
        create(htlc, peer, name);
    }
}

/// The tab strip closed a conversation's tab.
pub(crate) fn tab_closed(conn: ConnKey, peer: &Peer) {
    let Some(win) = WINDOWS.with_borrow_mut(|t| t.remove(conn, peer)) else {
        return;
    };
    // An emptied input says we stopped, where we had said we started.
    typed(&win, true);
    // Unparent the emoji typeahead's popover before the tab view disposes
    // the input, or GtkTextView's dispose spins on the foreign child.
    if let Some(input) = win.input.upgrade() {
        unsafe { crate::emoji::hx_emoji_typeahead_detach(wptr(&input)) };
    }
}

/// Forget a closed connection's conversations. The typeahead holds its
/// input, so each input is kept alive until it is detached.
pub(crate) fn forget_conn(conn: ConnKey) {
    for win in WINDOWS.with_borrow_mut(|t| t.forget_conn(conn)) {
        if let Some(input) = win.input.upgrade() {
            unsafe { crate::emoji::hx_emoji_typeahead_detach(wptr(&input)) };
        }
    }
}

/// `void msg_output_from_event (struct htlc_conn *, HxMsgEvent *)` — show a
/// private message `htlc` received, opening its conversation if need be.
///
/// # Safety
/// `htlc` is NULL or a live connection; `event` is NULL or valid. Main
/// thread.
#[no_mangle]
pub unsafe extern "C" fn msg_output_from_event(htlc: *mut c_void, event: *const HxMsgEvent) {
    let Some(e) = event.as_ref() else {
        return;
    };
    let has_media = !e.media.is_null();
    let body = pm::shown_body(opt_cstr(e.body).map(CStr::to_bytes), has_media);
    let win = render(
        htlc,
        opt_cstr(e.name),
        e.uid,
        Some(body),
        e.is_self != 0,
        false,
    );
    if has_media {
        let output = win.and_then(|w| w.output.upgrade());
        crate::inline_media_row::hx_inline_media_row_append(
            output.as_ref().map_or(std::ptr::null_mut(), wptr),
            std::ptr::null_mut(),
            e.media,
            htlc,
        );
    }
}

/// `void hx_msg_open (session *, guint16 uid, const char *name)` — open the
/// conversation with `uid`, or select it if it is open.
///
/// # Safety
/// `sess` is NULL or a live session; `name` is NULL or a C string. Main
/// thread.
#[no_mangle]
pub unsafe extern "C" fn hx_msg_open(sess: *mut c_void, uid: u16, name: *const c_char) {
    let htlc = dock::htlc_for(dock::key_for_session(sess));
    if !htlc.is_null() {
        open(htlc, uid, opt_cstr(name).unwrap_or(c""));
    }
}

/// `void hx_msg_user_changed (session *, guint16 uid, const char *name,
/// guint16 icon, guint16 color)` — redraw the conversation with `uid`, if
/// one is open, for what the user just changed to. The values are passed
/// because the user list has not taken them yet.
///
/// # Safety
/// `sess` is NULL or a live session; `name` is NULL or a C string. Main
/// thread.
#[no_mangle]
pub unsafe extern "C" fn hx_msg_user_changed(
    sess: *mut c_void,
    uid: u16,
    name: *const c_char,
    icon: u16,
    color: u16,
) {
    let conn = dock::key_for_session(sess);
    if let Some(win) = find(conn, &Peer::Uid(uid)) {
        let name = opt_cstr(name).map(CStr::to_string_lossy);
        apply_user_view(
            &win,
            dock::htlc_for(conn),
            name.as_deref(),
            icon,
            color,
            true,
        );
    }
}

/// `void hx_msg_set_palette (session *, const GdkRGBA *palette)`.
///
/// # Safety
/// `sess` is NULL or a live session; `palette` holds `ROTULUS_PAL_COLS`
/// colors. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_msg_set_palette(sess: *mut c_void, palette: *const gdk::ffi::GdkRGBA) {
    for win in of_conn(dock::key_for_session(sess)) {
        if let Some(output) = win.output.upgrade() {
            rotulus::ffi::rotulus_view_set_palette(wptr(&output), palette);
        }
    }
}

/// `void hx_msg_set_font (session *, const char *font)` — the chat font, on
/// each conversation's transcript and input.
///
/// # Safety
/// `sess` is NULL or a live session; `font` is a Pango font description.
/// Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_msg_set_font(sess: *mut c_void, font: *const c_char) {
    for win in of_conn(dock::key_for_session(sess)) {
        if let Some(output) = win.output.upgrade() {
            rotulus::ffi::rotulus_view_set_font(wptr(&output), font);
        }
        if let Some(input) = win.input.upgrade() {
            gtkhx_apply_input_font(wptr(&input));
        }
    }
}

/// `void hx_msg_configure (session *)` — the chat preferences, on each
/// conversation's transcript.
///
/// # Safety
/// `sess` is NULL or a live session. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_msg_configure(sess: *mut c_void) {
    for win in of_conn(dock::key_for_session(sess)) {
        if let Some(output) = win.output.upgrade() {
            crate::chat_view::gtkhx_chat_view_configure(wptr(&output));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{friend_markup, info_markup};

    #[test]
    fn a_friends_pane_says_what_their_entry_does() {
        let cases = [
            (
                "Al",
                "alice",
                Some("Away: lunch"),
                "<b>Al</b>\n<small>alice \u{b7} Away: lunch</small>",
            ),
            (
                "<b>",
                "b&b",
                None,
                "<b>&lt;b&gt;</b>\n<small>b&amp;b</small>",
            ),
        ];
        for (name, login, status, want) in cases {
            assert_eq!(friend_markup(name, login, status), want, "{name}");
        }
    }

    #[test]
    fn info_pane_says_what_is_known() {
        let cases = [
            (
                "alice",
                Some((128, "Admin (Away)", Some("#ff0000"))),
                "<span foreground=\"#ff0000\"><b>alice</b></span>\n\
                 <small>UID 7 \u{b7} Icon 128 \u{b7} Admin (Away)</small>",
            ),
            (
                "bob",
                Some((0, "Guest", None)),
                "<b>bob</b>\n<small>UID 7 \u{b7} Icon 0 \u{b7} Guest</small>",
            ),
            (
                "<carol>",
                None,
                "<b>&lt;carol&gt;</b>\n<small>UID 7</small>",
            ),
        ];
        for (name, known, want) in cases {
            assert_eq!(info_markup(name, 7, known), want, "{name}");
        }
    }
}
