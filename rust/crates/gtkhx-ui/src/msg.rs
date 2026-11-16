//! Private messages. Each conversation is a tab in its connection's Chat
//! panel, beside the public chat and the private chats.
//!
//! [`hxmodel::pm`] is the model: the open conversations, keyed by connection
//! and peer, and the rules for what a message reads as. This module is the
//! view over it — a tab's widgets, its input, the transcript a message is
//! rendered into, the recipient pane above it. It hears each message on
//! the `msg` signal, and has it notified; the remaining C reaches it
//! through a C ABI: `users.c` opens a conversation and tells it of a
//! user's change, and the settings hooks restyle its views.
//!
//! A friend's conversation is the same tab, keyed by their Login rather
//! than a uid (`friends` opens it and hands it what the friend family
//! says): it goes as instant messages, each marked with how far it has got,
//! says when the friend is typing, and tells them what has been read once
//! it is shown. Where the session has reactions (the Reactions draft,
//! capability bit 19) and they take an instant message's, each one there
//! takes chips under its last row, reacted to as a chat line is
//! (`reactions`); what the friend reacts with is told change by change,
//! and kept with the conversation, since the server keeps none of it. A
//! private message the server stored takes them likewise, by the reference
//! it named the message by, and what it holds is the server's to say, as a
//! line's is.
//!
//! Where the session has replies (the Replies draft, capability bit 18) and
//! they take an instant message's, or a stored private message's, Reply in
//! its menu shows the bar over the conversation's input (`reply_bar`), and
//! the next message sent answers it. A message that answers another of the
//! conversation's is drawn under a header quoting that one's first row,
//! found by its id or reference alone, or saying it is an earlier message
//! where the conversation no longer holds it; nothing is asked of the
//! server, which keeps no instant message to be read back and no message
//! of another's mailbox. The header goes to the message it quotes.

use std::cell::{Cell, RefCell};
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::ffi::{c_char, c_void, CStr};
use std::hash::Hash;
use std::rc::{Rc, Weak};
use std::time::Instant;

use glib::translate::from_glib_none;
use gtk::gdk;
use gtk::glib;
use gtk::pango;
use gtk::prelude::*;
use gtk4 as gtk;

use gtkhx_core::boxed::msg::HxMsgEvent;
use gtkhx_core::conn::{
    generation, hx_conn_has_cap, hx_conn_login, hx_conn_name, hx_conn_sess, hx_conn_uid,
};
use gtkhx_core::messaging::{with_roster, Delivery, Guid, Im};
use gtkhx_core::session::PmId;
use hxmodel::conversation::{hx_chat_cid, hx_chat_member_model};
use hxmodel::friends::{self, TheirTyping, Typing};
use hxmodel::member::{HxMember, HxMemberModel};
use hxmodel::pm::{self, Conversation, Conversations, Gone, Peer};
use hxmodel::reactions::{ImReactions, LineReactions};
use hxmodel::replies::{Quote, Short};
use hxproto::messaging::CAP_MESSAGING;
use hxproto::target::Target;
use rotulus::RotulusView;
use rotulus_layout::MessageId;

use crate::chat_output::Then;
use crate::chat_tabs;
use crate::dock::{self, ConnKey};
use crate::tr::{tr, tr1};
// The wire send. A test build leaves it out: it would link the whole send
// path, which reaches into C.
#[cfg(not(test))]
use hxhandlers::send::friends::typing as send_typing;
#[cfg(test)]
unsafe fn send_typing(_: *mut c_void, _: &[u8], _: bool) {}

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
    // chat.c
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    // notify.c / sound.c
    pub(crate) fn gtkhx_notify_send(id: *const c_char, title: *const c_char, body: *const c_char);
    pub(crate) fn play_sound(sound: std::ffi::c_int);
}

/// One conversation's tab. The widgets are held weakly: the tab strip owns
/// them, and a connection's close destroys its Chat page before its
/// conversations are forgotten.
struct MsgWin {
    conn: ConnKey,
    /// Who it is with: `Left` while a peer with a uid is offline, and the
    /// uid of whoever the user continues it with after.
    peer: RefCell<Peer>,
    /// The uid its pictures go to: none while its peer is offline, or for
    /// a friend.
    to: Rc<Cell<Option<u16>>>,
    gone: RefCell<Option<Gone>>,
    model: RefCell<Conversation>,
    content: glib::WeakRef<gtk::Box>,
    output: glib::WeakRef<gtk::Widget>,
    input: glib::WeakRef<gtk::TextView>,
    info_image: glib::WeakRef<gtk::Image>,
    info_label: glib::WeakRef<gtk::Label>,
    /// A friend's conversation's: who is typing to us.
    typing: glib::WeakRef<gtk::Label>,
    attach: glib::WeakRef<gtk::Button>,
    /// The bar offering to go on with someone of the nick of a peer who
    /// left: what it says, and its button.
    again: glib::WeakRef<gtk::Label>,
    go_on: glib::WeakRef<gtk::Button>,
    im: RefCell<ImState>,
    ims: RefCell<Drawn<Guid, ImMsg>>,
    /// The private messages the server named, by the reference it gave.
    pms: RefCell<Drawn<PmId, PmMsg>>,
    /// The private messages sent and not yet answered, by trans: their
    /// rows, the connect they went on, and what a reply to them quotes.
    sending: RefCell<HashMap<u32, (Marks, u32, Quote)>>,
    /// The message each reply answers, by the reply's first row.
    answers: RefCell<BTreeMap<u64, Msg>>,
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

/// A conversation's messages, by what names each: where each is drawn,
/// and what is kept of it.
struct Drawn<K, V> {
    rows: HashMap<K, Drawing<V>>,
    /// Oldest first.
    order: VecDeque<K>,
}

impl<K, V> Default for Drawn<K, V> {
    fn default() -> Self {
        Drawn {
            rows: HashMap::new(),
            order: VecDeque::new(),
        }
    }
}

struct Drawing<V> {
    /// The message's first row.
    first: MessageId,
    /// Its last, which its chips go under.
    row: MessageId,
    of: V,
}

impl<K: Copy + Eq + Hash, V> Drawn<K, V> {
    /// Message `key` was drawn on the rows from the first to the last, in a
    /// view that keeps `max_rows`. One drawn again keeps its first rows.
    fn drew(&mut self, key: K, (first, row): Marks, of: V, max_rows: usize) {
        if self.rows.contains_key(&key) {
            return;
        }
        // In row order: a sent message is named only once its answer comes,
        // which can be after a later one was drawn.
        let at = self
            .order
            .partition_point(|k| self.rows[k].first.0 < first.0);
        self.rows.insert(key, Drawing { first, row, of });
        self.order.insert(at, key);
        // Each takes a row at least, so past that many the oldest is gone.
        while max_rows != 0 && self.order.len() > max_rows {
            if let Some(gone) = self.order.pop_front() {
                self.rows.remove(&gone);
            }
        }
    }

    /// The message `row` is one of: a view numbers its rows in the order
    /// they are drawn, and a message's are drawn together.
    fn on(&self, row: MessageId) -> Option<K> {
        let of = |r: &Drawing<V>| (r.first.0..=r.row.0).contains(&row.0);
        self.rows.iter().find(|(_, r)| of(r)).map(|(k, _)| *k)
    }

    fn forget(&mut self, key: &K) {
        self.rows.remove(key);
        self.order.retain(|k| k != key);
    }
}

/// An instant message: who sent it, by Login, the friend or our own
/// account, what a reply to it quotes, and its reactions.
struct ImMsg {
    sender: Vec<u8>,
    said: Quote,
    reactions: ImReactions,
}

/// A private message the server named: what a reply to it quotes, its
/// reactions, and when the server last said what they are, or was asked.
struct PmMsg {
    said: Quote,
    reactions: LineReactions,
    told: Instant,
}

impl PmMsg {
    fn new(said: Quote) -> PmMsg {
        PmMsg {
            said,
            reactions: LineReactions::default(),
            told: Instant::now(),
        }
    }
}

/// What a message shows its chips from.
trait Shows {
    fn shows(&mut self) -> &mut LineReactions;
}

impl Shows for ImMsg {
    fn shows(&mut self) -> &mut LineReactions {
        &mut self.reactions.shown
    }
}

impl Shows for PmMsg {
    fn shows(&mut self) -> &mut LineReactions {
        &mut self.reactions
    }
}

/// A message one may react to: an instant message by its id, or a private
/// message by the reference the server named it by.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Msg {
    Im(Guid),
    Pm(PmId),
}

impl MsgWin {
    fn peer(&self) -> Peer {
        self.peer.borrow().clone()
    }

    /// How the Reactions draft names message `m`, while it is shown.
    fn target(&self, m: Msg) -> Option<Target> {
        match m {
            Msg::Im(guid) => {
                let sender = self.ims.borrow().rows.get(&guid)?.of.sender.clone();
                Some(Target::Im { guid, sender })
            }
            Msg::Pm(pm) => {
                let (stored, reference) = pm;
                (self.pms.borrow().rows.contains_key(&pm))
                    .then_some(Target::Pm { stored, reference })
            }
        }
    }

    fn holds(&self, m: Msg) -> bool {
        match m {
            Msg::Im(guid) => self.ims.borrow().rows.contains_key(&guid),
            Msg::Pm(pm) => self.pms.borrow().rows.contains_key(&pm),
        }
    }

    /// Message `m`'s first row, and what a reply to it quotes, while it is
    /// shown.
    fn drawn(&self, m: Msg) -> Option<(MessageId, Quote)> {
        match m {
            Msg::Im(guid) => {
                (self.ims.borrow().rows.get(&guid)).map(|r| (r.first, r.of.said.clone()))
            }
            Msg::Pm(pm) => (self.pms.borrow().rows.get(&pm)).map(|r| (r.first, r.of.said.clone())),
        }
    }

    /// The message drawn on `row`. A message the server stores is the only
    /// kind of private message GtkHx names: one it does not is named with
    /// whom it is with, which no server takes yet.
    fn on(&self, row: MessageId) -> Option<Msg> {
        let im = self.ims.borrow().on(row).map(Msg::Im);
        let pm = || self.pms.borrow().on(row).filter(|(stored, _)| *stored);
        im.or_else(|| pm().map(Msg::Pm))
    }
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
    /// Sending it let the recipient's request to us go, which a refusal
    /// leaves waiting after all.
    released: bool,
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
    if let Some(view) = output.downcast_ref::<RotulusView>() {
        crate::reactions::hear(view);
        crate::chat_output::hear_replies(view);
    }
    let adjustment: Option<gtk::Adjustment> =
        from_glib_none(rotulus::ffi::rotulus_view_get_vadjustment(raw));
    let vscroll = gtk::Scrollbar::new(gtk::Orientation::Vertical, adjustment.as_ref());

    let input = new_input();
    gtkhx_apply_input_font(wptr(&input));
    gtkhx_apply_input_style(wptr(&input));
    input.set_editable(true);
    input.set_wrap_mode(gtk::WrapMode::Word);
    // Inner margins so the text isn't clipped by the frame's rounded corners.
    input.set_left_margin(6);
    input.set_right_margin(6);
    input.set_top_margin(4);
    input.set_bottom_margin(4);

    // Recipient info pane: icon, and the name over its status, which says
    // whose icon it is.
    let info_image = gtk::Image::builder()
        .accessible_role(gtk::AccessibleRole::Presentation)
        .build();
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
    let to = Rc::new(Cell::new(match peer {
        Peer::Uid(uid) => Some(*uid),
        Peer::Login(_) | Peer::Left(_) => None,
    }));
    // An instant message carries text alone.
    let attach =
        matches!(peer, Peer::Uid(_)).then(|| crate::media_attach::message_button(htlc, to.clone()));
    let (again_bar, again, go_on) = again_bar();

    let win = Rc::new(MsgWin {
        conn,
        peer: RefCell::new(peer.clone()),
        to,
        gone: RefCell::default(),
        model: RefCell::new(Conversation::new(name)),
        content: outer_vbox.downgrade(),
        output: output.downgrade(),
        input: input.downgrade(),
        info_image: info_image.downgrade(),
        info_label: info_label.downgrade(),
        typing: typing.as_ref().map(|l| l.downgrade()).unwrap_or_default(),
        attach: attach.as_ref().map(|b| b.downgrade()).unwrap_or_default(),
        again: again.downgrade(),
        go_on: go_on.downgrade(),
        im: RefCell::default(),
        ims: RefCell::default(),
        pms: RefCell::default(),
        sending: RefCell::default(),
        answers: RefCell::default(),
        shown_in: RefCell::default(),
    });
    // Before the emoji typeahead attaches its own, so Return reaches this
    // one first.
    attach_keys(&input, Rc::downgrade(&win));
    go_on_when_asked(&win);

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
    if let Some(attach) = &attach {
        attach.set_valign(gtk::Align::End);
        input_hbox.append(attach);
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
    vpane.append(&again_bar);
    if let Some(view) = output.downcast_ref::<RotulusView>() {
        vpane.append(&crate::reply_bar::build(view, input.upcast_ref()));
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
    if let Peer::Uid(uid) = peer {
        still_here(htlc, *uid);
    }
    chat_tabs::raise_msg(htlc, &win.peer());
    crate::ffi::init_keyaccel(wptr(&outer_vbox));
    input.grab_focus();

    Some(win)
}

/// A conversation's input, which Tab and Shift+Tab leave, as they do a
/// form's text field, rather than typing a tab into the message.
fn new_input() -> gtk::TextView {
    gtk::TextView::builder().accepts_tab(false).build()
}

/// The input's keys. Return sends, Shift+Return is a newline, Up and Down
/// walk what was sent, Escape cancels a reply and only then is taken, and
/// Ctrl+K opens Connect.
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
            send(&win, &buf);
            return glib::Propagation::Stop;
        } else if key == Key::Escape && crate::reply_bar::escape(&view) {
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
    let Some((to, text)) = take(win, buf) else {
        return;
    };
    let htlc = dock::htlc_for(win.conn);
    if htlc.is_null() {
        return;
    }
    match &to {
        Peer::Uid(uid) => {
            let wire = crate::cs(&text.replace('\n', "\r"));
            unsafe {
                let reply = reply(win, htlc);
                let echoed = echo(htlc, *uid, &crate::cs(&text));
                if let Some((win, rows, _)) = &echoed {
                    head(win, rows.0, reply.as_ref());
                }
                let trans = hxhandlers::send::msg::message(htlc, *uid, &wire, reply.as_ref());
                sending(htlc, echoed, trans);
            }
        }
        Peer::Login(login) => unsafe { send_im(win, htlc, login, &text) },
        Peer::Left(_) => {}
    }
}

/// What is typed, taken out of the input to send and kept for Up, and
/// whom it goes to: nothing where it is empty, nor to a peer who has left
/// or whom the server would refuse an instant message, which is said
/// instead, the text kept for when it can go.
fn take(win: &MsgWin, buf: &gtk::TextBuffer) -> Option<(Peer, String)> {
    let text = whole(buf);
    let htlc = dock::htlc_for(win.conn);
    let name = || win.model.borrow().name.clone();
    let peer = win.peer();
    let refused = match &peer {
        Peer::Uid(uid) if !htlc.is_null() && !unsafe { still_here(htlc, *uid) } => {
            Some(tr1("Not sent: %s is offline.", &name()))
        }
        Peer::Left(_) => Some(tr1("Not sent: %s is offline.", &name())),
        Peer::Login(login) if !htlc.is_null() && !unsafe { reaches(htlc, login) } => {
            Some(tr1("Not sent: %s is not a friend on this server.", &name()))
        }
        _ => None,
    };
    if let Some(refused) = refused {
        if !text.is_empty() {
            notice(win, &refused);
        }
        return None;
    }
    let (mut start, mut end) = (buf.start_iter(), buf.end_iter());
    buf.delete(&mut start, &mut end);
    if text.is_empty() {
        return None;
    }
    win.model.borrow_mut().history.record(&text);
    Some((peer, text))
}

/// Whether `htlc` can send `login` an instant message: a friend, or anyone
/// where the server serves message requests, which holds one to someone
/// else as a request.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn reaches(htlc: *mut c_void, login: &[u8]) -> bool {
    hx_conn_has_cap(htlc.cast(), CAP_MESSAGING) != 0
        && with_roster(htlc, |r| r.requests.served || friends::is_friend(r, login))
}

/// The message the reply bar holds, which it lets go of, where `htlc`'s
/// login takes a reply to it.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn reply(win: &MsgWin, htlc: *mut c_void) -> Option<Target> {
    let targets = hxhandlers::recv::replies::targets(htlc);
    let reply = win.input.upgrade().and_then(|i| crate::reply_bar::take(&i));
    // A target the wire cannot carry would refuse the message whole.
    reply.filter(|t| targets.is_some_and(|k| k.takes(t)) && t.pack().is_some())
}

/// An instant message to `login`, answering the message the reply bar holds
/// where the login takes it, shown marked as it goes.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn send_im(win: &MsgWin, htlc: *mut c_void, login: &[u8], text: &str) {
    let reply = reply(win, htlc);
    let reply = reply.as_ref();
    let Some(guid) = hxhandlers::send::friends::im(htlc, login, text, reply) else {
        return;
    };
    // hxd-ng takes a reply to someone waiting with a request as approving
    // it. It is let go of here rather than at the answer, which can come
    // after what the approval hands over.
    let released = crate::friends::release(htlc, login);
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
    let mut last = mark;
    for rest in lines {
        last = rotulus::ffi::rotulus_view_append_text(
            wptr(&view),
            rest.as_ptr().cast(),
            rest.len() as i32,
            stamp,
        );
    }
    head(win, mark, reply);
    let own = CStr::from_ptr(hx_conn_login(htlc.cast())).to_bytes();
    let said = Quote::new(&String::from_utf8_lossy(nick), &first);
    drew_im(win, guid, own, said, (mark, last));
    let sent = Sent {
        mark: mark as usize,
        line: first,
        stamp,
        nick: nick.to_vec(),
        at: Delivery::Sending,
        connect: generation(htlc.cast()),
        released,
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
        Delivery::Requested => tr("waiting for approval"),
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
/// we started or stopped typing, as that changes. Someone who is not a
/// friend is told nothing until they are one.
fn typed(win: &MsgWin, empty: bool) {
    let Peer::Login(login) = &win.peer() else {
        return;
    };
    let htlc = dock::htlc_for(win.conn);
    if htlc.is_null() || !with_roster(htlc, |r| friends::is_friend(r, login)) {
        return;
    }
    let change = win.im.borrow_mut().typing.edited(empty);
    if let Some(on) = change {
        unsafe { send_typing(htlc, login, on) };
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
    let Peer::Login(login) = &win.peer() else {
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
/// its recipient only — and hand back where it went.
///
/// # Safety
/// `htlc` is NULL or a live connection. Main thread.
unsafe fn echo(htlc: *mut c_void, uid: u16, body: &CStr) -> Option<Rendered> {
    if htlc.is_null() {
        return None;
    }
    let own = opt_cstr(hx_conn_name(htlc.cast()));
    let own_bytes = own.map_or(&b""[..], CStr::to_bytes);
    let is_self = pm::is_self(own.map(CStr::to_bytes), own_bytes);
    render(htlc, own, uid, Some(body.to_bytes()), is_self, true)
}

/// Show the picture `media` as sent by us to `uid` on `trans`, as
/// [`echo`] shows text.
///
/// # Safety
/// `htlc` is NULL or a live connection; `media` is valid. Main thread.
pub(crate) unsafe fn echo_picture(
    htlc: *mut c_void,
    uid: u16,
    trans: Option<u32>,
    media: &gtkhx_core::boxed::chat::HxChatMedia,
) {
    let echoed = echo(htlc, uid, c"").map(|(win, rows, said)| {
        let rows = picture(&win, htlc, media).map_or(rows, |last| (rows.0, last));
        (win, rows, said)
    });
    sending(htlc, echoed, trans);
}

/// The first and last rows a message was drawn on.
type Rows = (*mut c_void, *mut c_void);

/// A private message drawn: its conversation, its rows, and what a reply to
/// it quotes.
type Rendered = (Rc<MsgWin>, Rows, Quote);

/// As [`Rows`], as the view's marks.
type Marks = (MessageId, MessageId);

/// Append `media`'s row to `win`: the row, where it was drawn.
///
/// # Safety
/// `htlc` is a live connection; `media` is valid. Main thread.
unsafe fn picture(
    win: &MsgWin,
    htlc: *mut c_void,
    media: *const gtkhx_core::boxed::chat::HxChatMedia,
) -> Option<*mut c_void> {
    let output = win.output.upgrade()?;
    let row = crate::inline_media_row::append(wptr(&output), std::ptr::null_mut(), media, htlc);
    (!row.is_null()).then_some(row)
}

/// What we sent to the conversation `echoed` drew it in went on `trans`,
/// whose answer names it.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn sending(htlc: *mut c_void, echoed: Option<Rendered>, trans: Option<u32>) {
    let (Some((win, rows, said)), Some(trans)) = (echoed, trans) else {
        return;
    };
    if let Some(rows) = marks(rows) {
        let connect = generation(htlc.cast());
        win.sending
            .borrow_mut()
            .insert(trans, (rows, connect, said));
    }
}

/// `rows` as the view's marks, where both are rows.
fn marks((first, last): Rows) -> Option<Marks> {
    (!first.is_null() && !last.is_null())
        .then_some((MessageId(first as u64), MessageId(last as u64)))
}

/// The private message sent on `trans` on `htlc` was answered, the server
/// naming it `pm_id`, where it stored it.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn answered(htlc: *mut c_void, trans: u32, pm_id: Option<PmId>) {
    let connect = generation(htlc.cast());
    for win in of_conn(dock::conn_key(htlc)) {
        if answered_in(&win, trans, pm_id, connect) {
            return;
        }
    }
}

/// Whether what `trans` answers on `connect` was sent from `win` on it;
/// named `pm_id` there, if so.
fn answered_in(win: &MsgWin, trans: u32, pm_id: Option<PmId>, connect: u32) -> bool {
    let mut sending = win.sending.borrow_mut();
    if !sending.get(&trans).is_some_and(|s| s.1 == connect) {
        return false;
    }
    let Some((rows, _, said)) = sending.remove(&trans) else {
        return false;
    };
    drop(sending);
    if let Some(pm) = pm_id {
        drew_pm(win, pm, rows, said);
    }
    true
}

/// Private message `pm`, which a reply quotes as `said`, was drawn in `win`
/// on `rows`.
fn drew_pm(win: &MsgWin, pm: PmId, rows: Marks, said: Quote) {
    if let Some(view) = win.output.upgrade().and_downcast::<RotulusView>() {
        (win.pms.borrow_mut()).drew(pm, rows, PmMsg::new(said), view.max_rows());
    }
}

/// Render a message into its conversation, opening it if it isn't, and flag
/// the tab when it isn't ours; where it went.
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
) -> Option<Rendered> {
    if htlc.is_null() || hx_conn_sess(htlc.cast()).is_null() {
        return None;
    }
    let peer = Peer::Uid(uid);
    let win = match find(dock::conn_key(htlc), &peer) {
        Some(win) => win,
        None => create(
            htlc,
            &peer,
            &hxproto::text::to_utf8(name.map_or(&[][..], CStr::to_bytes)),
        )?,
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
    let rows = draw(&win, &line, &body);
    if !is_self {
        chat_tabs::set_attention_msg(htlc, &win.peer(), true);
    }
    let first = pm::rows(&body).next().unwrap_or_default();
    let said = Quote::new(&String::from_utf8_lossy(line.nick), first);
    Some((win, rows, said))
}

/// Draw a message into `win`: its first row as `line`, the rest as text.
/// Its first and last rows' marks.
///
/// # Safety
/// Main thread.
unsafe fn draw(win: &MsgWin, line: &Line<'_>, body: &str) -> Rows {
    let (mut first, mut last) = (std::ptr::null_mut(), std::ptr::null_mut());
    let Some(view) = win.output.upgrade() else {
        return (first, last);
    };
    for (i, text) in pm::rows(body).enumerate() {
        last = if i == 0 {
            first = line.put(&view, None, text, None);
            first
        } else {
            rotulus::ffi::rotulus_view_append_text(
                wptr(&view),
                text.as_ptr().cast(),
                text.len() as i32,
                line.stamp,
            )
        };
    }
    (first, last)
}

/// Instant message `guid`, `sender`'s, which a reply quotes as `said`, was
/// drawn in `win` on `rows`.
fn drew_im(win: &MsgWin, guid: Guid, sender: &[u8], said: Quote, rows: Rows) {
    let view = win.output.upgrade().and_downcast::<RotulusView>();
    if let (Some(view), Some(rows)) = (view, marks(rows)) {
        let of = ImMsg {
            sender: sender.to_vec(),
            said,
            reactions: ImReactions::default(),
        };
        win.ims.borrow_mut().drew(guid, rows, of, view.max_rows());
    }
}

/// Head the message whose first row is `row` with the instant message or
/// private message `to` names, where it names one: quoted from that
/// message's first row where the conversation holds it, by its id or
/// reference alone, so a Login spelled otherwise does not lose it; and
/// otherwise as an earlier message, which nothing can fetch.
fn head(win: &MsgWin, row: *mut c_void, to: Option<&Target>) {
    let m = match to {
        Some(&Target::Im { guid, .. }) => Msg::Im(guid),
        Some(&Target::Pm { stored, reference }) => Msg::Pm((stored, reference)),
        _ => return,
    };
    let Some(view) = win.output.upgrade().and_downcast::<RotulusView>() else {
        return;
    };
    let reply = match win.drawn(m) {
        Some((_, q)) => crate::chat_output::quoting(&view, &q),
        None => crate::chat_output::unquoted(None),
    };
    view.set_reply(MessageId(row as u64), Some(reply));
    let mut answers = win.answers.borrow_mut();
    answers.insert(row as u64, m);
    // Each reply takes a row at least, so past that many the oldest is gone.
    while view.max_rows() != 0 && answers.len() > view.max_rows() {
        answers.pop_first();
    }
}

/// How a reply names the message drawn on `row` of `view`, and what it
/// quotes, where `targets` take a reply to it.
pub(crate) fn reply_on(
    view: &RotulusView,
    row: MessageId,
    targets: &hxproto::replies::Targets,
) -> Option<(Target, Quote)> {
    let win = shown_in(view)?;
    reply_in(&win, row, targets)
}

fn reply_in(
    win: &MsgWin,
    row: MessageId,
    targets: &hxproto::replies::Targets,
) -> Option<(Target, Quote)> {
    let m = win.on(row)?;
    let target = win.target(m).filter(|t| targets.takes(t))?;
    Some((target, win.drawn(m)?.1))
}

/// The header of the reply on `row` of `view` was activated, from the
/// keyboard or not: go to the message it answers, where `view` is a
/// conversation's. Whether it is.
pub(crate) fn jump(view: &RotulusView, row: MessageId, keyboard: bool) -> bool {
    let Some(win) = shown_in(view) else {
        return false;
    };
    jump_in(&win, view, row, keyboard);
    true
}

/// [`jump`] in `win`: to the message the reply on `row` answers where the
/// conversation holds it, and otherwise told that it isn't there.
fn jump_in(win: &MsgWin, view: &RotulusView, row: MessageId, keyboard: bool) {
    let Some(&re) = win.answers.borrow().get(&row.0) else {
        return;
    };
    match win.drawn(re) {
        Some((to, _)) => crate::chat_output::reveal(view, to, keyboard),
        None => crate::reactions::toast(&crate::chat_output::short(Short::Unavailable)),
    }
}

/// An instant message `htlc` received: shown in its sender's conversation,
/// opened if need be, notified as a private message is unless `quiet`, and
/// read if it is on screen.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn im(htlc: *mut c_void, m: &Im, quiet: bool) {
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
    // Whoever sent it, no bidi override or invisible character of theirs
    // reaches the transcript or the desktop.
    let body = friends::plain_text(&m.body);
    let rows = draw(&win, &line, &body);
    head(&win, rows.0, m.reply_to.as_ref());
    let said = Quote::new(&name, pm::rows(&body).next().unwrap_or_default());
    drew_im(&win, m.guid, &m.from, said, rows);
    show_typing(&win, false);
    chat_tabs::set_attention_msg(htlc, &peer, true);
    win.im.borrow_mut().unread.push(m.guid);
    mark_read(&win);
    if !quiet {
        notify(htlc, &peer, &name, &body);
        play_sound(SOUND_MSG);
    }
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
    let relist = d == Delivery::Failed && sent.released;
    if matches!(d, Delivery::Read | Delivery::Failed) {
        im.sent.remove(guid);
    }
    drop(im);
    if relist {
        hxhandlers::send::friends::requests(htlc);
    }
}

/// The conversation `view` shows.
fn shown_in(view: &RotulusView) -> Option<Rc<MsgWin>> {
    let conn = crate::chat_view::conn_of(view)?;
    let shows = |w: &Rc<MsgWin>| {
        w.output
            .upgrade()
            .is_some_and(|o| &o == view.upcast_ref::<gtk::Widget>())
    };
    of_conn(conn).into_iter().find(shows)
}

/// The message drawn on `row` of `view`, where `limits` take reactions to
/// it.
pub(crate) fn on(
    view: &RotulusView,
    row: MessageId,
    limits: &hxproto::reactions::Limits,
) -> Option<Msg> {
    let win = shown_in(view)?;
    on_in(&win, row, limits)
}

fn on_in(win: &MsgWin, row: MessageId, limits: &hxproto::reactions::Limits) -> Option<Msg> {
    let m = win.on(row)?;
    limits.takes(&win.target(m)?).then_some(m)
}

/// As `chat_output::want`, for message `m` in `view`.
pub(crate) fn want(
    view: &RotulusView,
    m: Msg,
    key: &str,
    on: Option<bool>,
    limits: &hxproto::reactions::Limits,
) -> Option<Then<Msg>> {
    let win = shown_in(view)?;
    want_in(&win, m, key, on, limits)
}

fn want_in(
    win: &MsgWin,
    m: Msg,
    key: &str,
    on: Option<bool>,
    limits: &hxproto::reactions::Limits,
) -> Option<Then<Msg>> {
    let key = match crate::chat_output::chip_key(key) {
        Ok(key) => key,
        Err(say) => return Some(say),
    };
    // The login's caps are a stored message's and a line's: a server keeps
    // nothing of an instant message's to hold them to.
    let limits = match m {
        Msg::Im(_) => hxproto::reactions::Limits {
            max_keys: None,
            max_per_user: None,
            ..*limits
        },
        Msg::Pm(_) => *limits,
    };
    redraw(win, m, |r| {
        crate::chat_output::wanted(r, m, key, on, &limits)
    })?
}

/// Draw message `m`'s chips again, after `change` to what they show; what
/// it returns. One whose row has gone is let go of.
fn redraw<T>(win: &MsgWin, m: Msg, change: impl FnOnce(&mut LineReactions) -> T) -> Option<T> {
    match m {
        Msg::Im(guid) => redraw_in(win, &win.ims, &guid, |v| change(v.shows())),
        Msg::Pm(pm) => redraw_in(win, &win.pms, &pm, |v| change(v.shows())),
    }
}

fn redraw_in<K: Copy + Eq + Hash, V: Shows, T>(
    win: &MsgWin,
    drawn: &RefCell<Drawn<K, V>>,
    key: &K,
    change: impl FnOnce(&mut V) -> T,
) -> Option<T> {
    let view = win.output.upgrade()?.downcast::<RotulusView>().ok()?;
    let (row, chips, done) = {
        let mut drawn = drawn.borrow_mut();
        let r = drawn.rows.get_mut(key)?;
        let done = change(&mut r.of);
        let chips = crate::chat_output::chips(&r.of.shows().shown());
        (r.row, chips, done)
    };
    if !view.set_reactions(row, chips) {
        drawn.borrow_mut().forget(key);
    }
    Some(done)
}

/// Stored message `pm` in `win` was just told of, or asked after.
fn told(win: &MsgWin, pm: &PmId) {
    if let Some(r) = win.pms.borrow_mut().rows.get_mut(pm) {
        r.of.told = Instant::now();
    }
}

/// Do what [`want`] left to do in `view`, on `htlc`.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn then(htlc: *mut c_void, view: &RotulusView, t: Then<Msg>) {
    if let Some(win) = shown_in(view) {
        act(htlc, &win, t);
    }
}

/// # Safety
/// See [`then`].
unsafe fn act(htlc: *mut c_void, win: &MsgWin, t: Then<Msg>) {
    use hxhandlers::send::reactions as send;
    let (m, key) = match &t {
        Then::Send(m, k, _) | Then::Withdraw(m, k) => (*m, k.clone()),
        Then::Say(text) => return crate::reactions::toast(text),
    };
    let Some(target) = win.target(m) else {
        return;
    };
    let gone = match (t, m, &win.peer()) {
        (Then::Send(_, _, add), Msg::Im(_), Peer::Login(with)) => {
            refused_unless(send::react_im(htlc, target, with, key.clone(), add))
        }
        (Then::Send(_, _, add), Msg::Pm(pm), _) => {
            // A reply to it may come before the server applies it, so
            // asking after it now could take back a reaction it took.
            told(win, &pm);
            refused_unless(send::react(htlc, target, key.clone(), add))
        }
        (Then::Send(..), ..) => true,
        _ => send::withdraw(htlc, &target, &key),
    };
    if gone {
        redraw(win, m, |r| r.refused(&key));
    }
}

/// Whether a reaction the wire could not carry, `!sent`, is told so.
fn refused_unless(sent: bool) -> bool {
    if !sent {
        let invalid = Some(hxproto::reactions::INVALID_KEY);
        crate::reactions::toast(&crate::reactions::refusal(invalid, None));
    }
    !sent
}

/// What the `reactions` signal said of message `m` on `htlc`.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn reacted(htlc: *mut c_void, m: Msg, e: &gtkhx_core::reactions::Event) {
    use gtkhx_core::reactions::Event;
    // A change is the other party's, so in the conversation with them.
    let win = match e {
        Event::Changed { from, .. } => find(dock::conn_key(htlc), &Peer::Login(from.clone())),
        _ => of_conn(dock::conn_key(htlc))
            .into_iter()
            .find(|w| w.holds(m)),
    };
    // The user hears why their own act was refused even once the
    // conversation it was in has closed.
    if let Event::Refused {
        key, reason, error, ..
    } = e
    {
        if let (Some(win), Some(key)) = (&win, key) {
            redraw(win, m, |r| r.refused(key));
        }
        return crate::reactions::toast(&crate::reactions::refusal(*reason, error.as_deref()));
    }
    let Some(win) = win else {
        return;
    };
    match e {
        Event::PutOff { target, key, add } => {
            if redraw(&win, m, |r| r.put_off(key)) != Some(true) {
                return;
            }
            let (target, key) = (target.clone(), key.clone());
            match (m, &win.peer()) {
                (Msg::Im(_), Peer::Login(with)) => {
                    hxhandlers::send::reactions::retry_im(htlc, target, with, key, *add);
                }
                _ => {
                    hxhandlers::send::reactions::retry(htlc, target, key, *add);
                }
            }
        }
        _ => {
            if let Some(t) = heard(&win, m, e) {
                act(htlc, &win, t);
            }
        }
    }
}

/// What a change of message `m`'s in `win`, or the answer to our own,
/// leaves to do: ask for what the user has wanted since.
fn heard(win: &MsgWin, m: Msg, e: &gtkhx_core::reactions::Event) -> Option<Then<Msg>> {
    use gtkhx_core::reactions::Event;
    match (e, m) {
        (Event::Changed { key, add, from, .. }, Msg::Im(guid)) => {
            redraw_in(win, &win.ims, &guid, |v| v.reactions.told(key, *add, from));
            None
        }
        (Event::Now { reactions, .. }, Msg::Pm(pm)) => {
            let (summary, mine) = reactions.clone().unwrap_or_default();
            redraw(win, m, |r| r.replace(summary, &mine));
            told(win, &pm);
            None
        }
        (
            Event::Acted {
                key, add, count, ..
            },
            _,
        ) => {
            let again = redraw(win, m, |r| r.answered(key, *add, *count))??;
            Some(Then::Send(m, key.clone(), again))
        }
        _ => None,
    }
}

/// Ask after what stored message `m` in `view` holds, on `htlc`, where
/// what the server last said of it may be out of date.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn refresh_if_stale(view: &RotulusView, htlc: *mut c_void, m: Msg) {
    let Some(win) = shown_in(view) else {
        return;
    };
    if let Some(target) = due(&win, m, Instant::now()) {
        hxhandlers::send::reactions::refresh(htlc, &[target]);
    }
}

/// What to ask after for message `m` in `win` at `now`: a stored message
/// told of or asked after over a while ago. Asking counts as told, so a
/// menu opened again before the answer asks no more.
fn due(win: &MsgWin, m: Msg, now: Instant) -> Option<Target> {
    let Msg::Pm(pm) = m else {
        return None;
    };
    {
        let mut pms = win.pms.borrow_mut();
        let r = pms.rows.get_mut(&pm)?;
        if now.saturating_duration_since(r.of.told) <= crate::chat_output::FRESH {
            return None;
        }
        r.of.told = now;
    }
    win.target(m)
}

/// `htlc`'s connection changed state: what was sent on an earlier connect
/// will hear no more, but for what was still being sent, which the
/// roster's own forgetting fails; and a reply pending is let go of, as a
/// chat's is.
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
        win.sending.borrow_mut().retain(|_, s| s.1 == connect);
        unanswered(&win);
        if let Some(view) = win.output.upgrade().and_downcast::<RotulusView>() {
            crate::reply_bar::cancel(&view);
        }
    }
}

/// The reactions asked for in `win` and not answered never will be: the
/// connection's requests went with it.
fn unanswered(win: &MsgWin) {
    let mut held: Vec<Msg> = win.ims.borrow().rows.keys().copied().map(Msg::Im).collect();
    held.extend(win.pms.borrow().rows.keys().copied().map(Msg::Pm));
    for m in held {
        redraw(win, m, |r| r.settle());
    }
}

/// The stored messages `htlc`'s conversations show, asked after once it
/// has logged in again: what they hold may have changed while it was
/// away, and nothing tells it so. One Get Reactions' worth, each
/// conversation's newest first: a login has spent most of what hxd-ng
/// lets a session spend in a few seconds before it bans it, and the rest
/// are asked after as their menus open.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
unsafe fn ask_again(htlc: *mut c_void) {
    let mut targets = Vec::new();
    for win in of_conn(dock::conn_key(htlc)) {
        let pms: Vec<PmId> = win.pms.borrow().order.iter().rev().copied().collect();
        for pm in pms.into_iter().filter(|(stored, _)| *stored) {
            if targets.len() == hxrequest::reactions::MAX_TARGETS {
                break;
            }
            told(&win, &pm);
            targets.extend(win.target(Msg::Pm(pm)));
        }
    }
    hxhandlers::send::reactions::refresh(htlc, &targets);
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
        if let Peer::Login(login) = &win.peer() {
            friend_changed(htlc, login);
        }
    }
}

/// The bar over a conversation's input that offers, once its peer has
/// left, to go on with someone of their nick who joined since: what it
/// says, and its button.
fn again_bar() -> (gtk::Box, gtk::Label, gtk::Button) {
    let bar = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    bar.set_visible(false);
    let label = gtk::Label::new(None);
    label.set_xalign(0.0);
    label.set_hexpand(true);
    label.set_ellipsize(pango::EllipsizeMode::End);
    label.set_margin_start(6);
    bar.append(&label);
    // TRANSLATORS: beside "<nick> is online again" over a private
    // message's input: messages go to the one who joined from then on.
    let go_on = gtk::Button::with_label(&tr("Continue"));
    bar.append(&go_on);
    (bar, label, go_on)
}

fn go_on_when_asked(win: &Rc<MsgWin>) {
    let weak = Rc::downgrade(win);
    if let Some(go_on) = win.go_on.upgrade() {
        go_on.connect_clicked(move |_| {
            if let Some(win) = weak.upgrade() {
                unsafe { carry_on(&win) };
            }
        });
    }
}

/// Show the bar where the conversation has someone to go on with, said
/// aloud as it appears, and hide it where it has nobody.
fn show_again(win: &MsgWin) {
    let (Some(label), Some(go_on)) = (win.again.upgrade(), win.go_on.upgrade()) else {
        return;
    };
    let Some(bar) = label.parent() else {
        return;
    };
    let gone = win.gone.borrow();
    let Some(gone) = gone.as_ref().filter(|g| g.offered().is_some()) else {
        return bar.set_visible(false);
    };
    // TRANSLATORS: over a private message's input once someone of the
    // nick of the person it was with, who left the server, joins it.
    let text = tr1("%s is online again", &gone.nick);
    label.set_text(&text);
    // The button alone says nothing of whom it goes on with.
    go_on.update_property(&[gtk::accessible::Property::Description(&text)]);
    if !bar.is_visible() {
        bar.set_visible(true);
        crate::announce::announce(Some(label.upcast_ref()), &text, false);
    }
}

/// The public chat's members: everyone on `htlc`'s server. Read whole,
/// as `HxMemberInfo` cuts a name to C's buffer, and that much of a nick
/// may be someone else's.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn public_model(htlc: *mut c_void) -> Option<HxMemberModel> {
    let chat = chat_with_cid(hx_conn_sess(htlc.cast()).cast(), 0);
    if chat.is_null() {
        return None;
    }
    let model = hx_chat_member_model(chat.cast());
    if model.is_null() {
        return None;
    }
    let model: glib::Object = from_glib_none(model as *mut glib::gobject_ffi::GObject);
    model.downcast().ok()
}

/// Everyone on `htlc`'s server, by uid and nick, in the order they
/// joined.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn public_members(htlc: *mut c_void) -> Vec<(u16, String)> {
    let members = public_model(htlc).map(|m| m.iter::<HxMember>().flatten().collect());
    let members: Vec<HxMember> = members.unwrap_or_default();
    members.iter().map(|m| (m.uid(), m.name())).collect()
}

/// Whether `uid` is still on `htlc`'s server as its user list has it: one
/// the list no longer holds has left, though no part was heard, and is
/// let go of as a part lets go. An empty list has not been read yet, and
/// says nothing. A conversation opened, or a message sent, before the list
/// was read is checked as anything goes out.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
pub(crate) unsafe fn still_here(htlc: *mut c_void, uid: u16) -> bool {
    let members = public_members(htlc);
    let gone = !members.is_empty() && members.iter().all(|&(u, _)| u != uid);
    if gone {
        left(htlc, uid);
    }
    !gone
}

/// File `win` as the conversation with `peer`, and its tab with it. A
/// notification of it as it was would open nothing, or someone else's.
fn refile(win: &Rc<MsgWin>, htlc: *mut c_void, peer: Peer) {
    let old = win.peer.replace(peer.clone());
    withdraw(&chat_tabs::notify_id(win.conn, &old));
    WINDOWS.with_borrow_mut(|t| {
        t.remove(win.conn, &old);
        t.insert(win.conn, peer.clone(), win.clone());
    });
    chat_tabs::rekey_msg(htlc, &old, &peer);
}

/// The conversation's peer left `htlc`'s server, or `htlc` its: it stays
/// open, says they are offline, and is filed where nothing for its old
/// uid, which may be anyone's by now, finds it.
fn go_offline(win: &Rc<MsgWin>, htlc: *mut c_void) {
    thread_local! {
        static LEFT: Cell<u32> = const { Cell::new(0) };
    }
    if let Peer::Uid(_) = win.peer() {
        let n = LEFT.with(|n| n.replace(n.get().wrapping_add(1)));
        refile(win, htlc, Peer::Left(n));
    }
    win.to.set(None);
    let nick = win.model.borrow().name.clone();
    // Someone of the nick may be on already: a quick reconnect can join
    // before its old session is let go.
    let mut gone = Gone::new(nick.clone());
    let own = unsafe { hx_conn_uid(htlc.cast()) };
    for (uid, name) in unsafe { public_members(htlc) } {
        if uid != own {
            gone.seen(uid, &name);
        }
    }
    *win.gone.borrow_mut() = Some(gone);
    if let Some(attach) = win.attach.upgrade() {
        attach.set_sensitive(false);
    }
    if let Some(label) = win.info_label.upgrade() {
        label.set_markup(&format!(
            "<b>{}</b>\n<small>{}</small>",
            glib::markup_escape_text(&nick),
            glib::markup_escape_text(&tr("Offline"))
        ));
    }
    let peer = win.peer();
    chat_tabs::set_title_msg(htlc, &peer, &title(&peer, &nick));
    show_again(win);
}

/// `uid` joined `htlc`'s server as `nick`, or was renamed to it: offered
/// to each conversation whose peer left under that nick, and to no other.
fn seen(htlc: *mut c_void, uid: u16, nick: &str) {
    // Our own row is heard too, on a list's load or a rename of ours.
    if uid == unsafe { hx_conn_uid(htlc.cast()) } {
        return;
    }
    for win in of_conn(dock::conn_key(htlc)) {
        if let Some(gone) = win.gone.borrow_mut().as_mut() {
            gone.seen(uid, nick);
        }
        show_again(&win);
    }
}

/// `uid` left `htlc`'s server: its conversation is offline, and it is
/// offered to no other.
fn left(htlc: *mut c_void, uid: u16) {
    let conn = dock::conn_key(htlc);
    if let Some(win) = find(conn, &Peer::Uid(uid)) {
        go_offline(&win, htlc);
    }
    for win in of_conn(conn) {
        if let Some(gone) = win.gone.borrow_mut().as_mut() {
            gone.left(uid);
        }
        show_again(&win);
    }
}

/// `htlc`'s user list was emptied: everyone it had a conversation with by
/// uid is offline, and offered to none, until they join again.
fn cleared(htlc: *mut c_void) {
    for win in of_conn(dock::conn_key(htlc)) {
        if !matches!(win.peer(), Peer::Login(_)) {
            go_offline(&win, htlc);
        }
    }
}

/// Continue: the conversation goes on with the one its bar offers, whose
/// uid it is filed and sent to from now on; where that one has a
/// conversation of their own already, it is shown instead, the two kept
/// apart.
unsafe fn carry_on(win: &Rc<MsgWin>) {
    let Some(uid) = win.gone.borrow().as_ref().and_then(Gone::offered) else {
        return;
    };
    let htlc = dock::htlc_for(win.conn);
    if htlc.is_null() {
        return;
    }
    let new = Peer::Uid(uid);
    if find(win.conn, &new).is_some() {
        return chat_tabs::raise_msg(htlc, &new);
    }
    refile(win, htlc, new);
    *win.gone.borrow_mut() = None;
    win.to.set(Some(uid));
    if let Some(attach) = win.attach.upgrade() {
        attach.set_sensitive(true);
    }
    show_again(win);
    refresh_user_info(win, htlc);
    if let Some(input) = win.input.upgrade() {
        input.grab_focus();
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

/// Tell the desktop of a private message `htlc` received from `peer`, the
/// conversation it was shown in; not of one of our own.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn notify_msg(htlc: *mut c_void, e: &HxMsgEvent, peer: &Peer) {
    if e.is_self != 0 {
        return;
    }
    // In the server's encoding, as the transcript reads them.
    let text = |p| hxproto::text::to_utf8(opt_cstr(p).map_or(&[][..], CStr::to_bytes));
    notify(htlc, peer, &text(e.name), &text(e.body));
}

#[cfg(test)]
use tests::withdraw;

/// Take back the desktop's notification `id`.
#[cfg(not(test))]
fn withdraw(id: &str) {
    if let Some(app) = gtk::gio::Application::default() {
        app.withdraw_notification(id);
    }
}

/// Draw the info pane from the public chat's entry for the peer, or from
/// the name the conversation was opened with when it has left.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn refresh_user_info(win: &MsgWin, htlc: *mut c_void) {
    let uid = match &win.peer() {
        Peer::Uid(uid) => *uid,
        Peer::Login(login) => {
            friend_view(win, htlc, login);
            return;
        }
        Peer::Left(_) => return,
    };
    match public_model(htlc).and_then(|m| m.get(uid)) {
        Some(m) => apply_user_view(win, htlc, Some(&m.name()), m.icon(), m.status(), true),
        None => apply_user_view(win, htlc, None, 0, 0, false),
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
    let login = friends::plain_name(&hxproto::text::to_utf8(login));
    let status = entry
        .as_ref()
        .map(|f| crate::friends::presence_line(f.presence, f.status.as_deref()));
    label.set_markup(&friend_markup(&name, &login, status.as_deref()));
    chat_tabs::set_title_msg(htlc, &win.peer(), &pm::login_title(&name, &login));
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
    let Peer::Uid(uid) = win.peer() else {
        return;
    };
    // Kept, as the nick someone joining is known by should the peer leave.
    let display = match display {
        Some(d) if !d.is_empty() => {
            win.model.borrow_mut().name = d.to_owned();
            d.to_owned()
        }
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
    let peer = win.peer();
    chat_tabs::set_title_msg(htlc, &peer, &title(&peer, nick));
    name_inputs(win, nick);
}

/// A conversation's tab title.
fn title(peer: &Peer, name: &str) -> String {
    match peer {
        Peer::Uid(uid) => pm::title(name, *uid),
        // TRANSLATORS: a private message's tab once the person it is with
        // has left the server.
        Peer::Left(_) => tr1("%s (offline)", name),
        Peer::Login(login) => {
            pm::login_title(name, &friends::plain_name(&hxproto::text::to_utf8(login)))
        }
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

/// Open the conversation with the account `login` on `htlc`, a friend or
/// not, or select it if it is open.
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

/// A private message `htlc` received, the server naming it `pm_id`, which
/// answers `reply_to`: shown, opening its conversation if need be, and told
/// to the desktop.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn received(
    htlc: *mut c_void,
    e: &HxMsgEvent,
    pm_id: Option<PmId>,
    reply_to: Option<&Target>,
) {
    // Shown and told together: a notification of a message whose
    // connection has no tab would point at a conversation nobody can open.
    if hx_conn_sess(htlc.cast()).is_null() {
        return;
    }
    let has_media = !e.media.is_null();
    let body = pm::shown_body(opt_cstr(e.body).map(CStr::to_bytes), has_media);
    let shown = render(
        htlc,
        opt_cstr(e.name),
        e.uid,
        Some(body),
        e.is_self != 0,
        false,
    );
    // A message from a uid already gone is shown offline, and told as such.
    let peer = shown
        .as_ref()
        .map_or(Peer::Uid(e.uid), |(win, ..)| win.peer());
    if let Some((win, (first, last), said)) = shown {
        head(&win, first, reply_to);
        let last = match has_media {
            true => picture(&win, htlc, e.media).unwrap_or(last),
            false => last,
        };
        if let (Some(pm), Some(rows)) = (pm_id, marks((first, last))) {
            drew_pm(&win, pm, rows, said);
        }
    }
    notify_msg(htlc, e, &peer);
}

/// Hear the private messages the session hands over and the answers to
/// those sent, and ask after the stored ones again at each login. Once.
pub(crate) fn install() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        gtkhx_core::session::connect_msg(|htlc, e, pm_id, reply_to| unsafe {
            if !htlc.is_null() {
                received(htlc, e, pm_id, reply_to);
            }
        });
        gtkhx_core::session::connect_msg_answered(|htlc, trans, pm_id| unsafe {
            if !htlc.is_null() {
                answered(htlc, trans, pm_id);
            }
        });
        let session: glib::Object = unsafe {
            from_glib_none(
                gtkhx_core::session::gtkhx_session_get_default() as *mut glib::gobject_ffi::GObject
            )
        };
        session.connect_local("logged-in", false, |args| {
            let htlc = args[1].get::<*mut c_void>().unwrap_or(std::ptr::null_mut());
            if !htlc.is_null() {
                unsafe { ask_again(htlc) };
            }
            None
        });
        hear_roster();
    });
}

/// Hear who joins and leaves the server, and its user list emptied.
fn hear_roster() {
    let session: glib::Object = unsafe {
        from_glib_none(
            gtkhx_core::session::gtkhx_session_get_default() as *mut glib::gobject_ffi::GObject
        )
    };
    let ptr = |args: &[glib::Value], i: usize| {
        args[i].get::<*mut c_void>().unwrap_or(std::ptr::null_mut())
    };
    // The public chat's members are everyone on the server.
    let public = move |args: &[glib::Value]| {
        let (htlc, chat) = (ptr(args, 1), ptr(args, 2));
        let cid = (!chat.is_null()).then(|| unsafe { hx_chat_cid(chat.cast()) });
        (!htlc.is_null() && cid == Some(0)).then_some(htlc)
    };
    let uid = |args: &[glib::Value]| args[3].get::<u32>().unwrap_or(0) as u16;
    // A rename comes as a change.
    for signal in ["user-create", "user-change"] {
        session.connect_local(signal, true, move |args| {
            if let Some(htlc) = public(args) {
                let nick = unsafe { opt_cstr(ptr(args, 5).cast()) }.map(CStr::to_string_lossy);
                seen(htlc, uid(args), nick.as_deref().unwrap_or(""));
            }
            None
        });
    }
    session.connect_local("user-delete", true, move |args| {
        if let Some(htlc) = public(args) {
            left(htlc, uid(args));
        }
        None
    });
    session.connect_local("users-clear", true, move |args| {
        if let Some(htlc) = public(args) {
            cleared(htlc);
        }
        None
    });
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
pub(crate) mod tests {
    use super::*;
    use super::{friend_markup, info_markup};

    /// Tab and Shift+Tab move the focus out of a conversation's input either
    /// way, typing nothing.
    pub(crate) fn check_tab_leaves_the_input() {
        use gdk::{Key, ModifierType as M};
        use glib::translate::IntoGlib;
        let input = new_input();
        let (before, after) = (gtk::Button::new(), gtk::Button::new());
        let column = gtk::Box::new(gtk::Orientation::Vertical, 0);
        column.append(&before);
        column.append(&input);
        column.append(&after);
        let window = gtk::Window::new();
        window.set_child(Some(&column));
        window.present();
        // A key as the input's controllers have it, ours and GTK's own.
        let press = |key: Key, state: M| {
            (input.observe_controllers().iter::<glib::Object>())
                .filter_map(|c| c.ok()?.downcast::<gtk::EventControllerKey>().ok())
                .any(|c| c.emit_by_name::<bool>("key-pressed", &[&key.into_glib(), &0u32, &state]))
        };
        for (key, state, lands) in [
            (Key::Tab, M::empty(), &after),
            (Key::ISO_Left_Tab, M::SHIFT_MASK, &before),
        ] {
            input.grab_focus();
            press(key, state);
            let focus = gtk::prelude::GtkWindowExt::focus(&window);
            assert_eq!(focus.as_ref(), Some(lands.upcast_ref()), "{key:?}");
        }
        assert_eq!(whole(&input.buffer()), "");
        window.destroy();
    }

    thread_local! {
        static WITHDRAWN: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    }

    /// Take back notification `id`, as the desktop is asked to.
    pub(crate) fn withdraw(id: &str) {
        WITHDRAWN.with_borrow_mut(|w| w.push(id.to_owned()));
    }

    /// An instant message the server would refuse, to someone who is not a
    /// friend where nobody takes requests, is not sent and stays in the
    /// input, kept from Up; one the server would take goes.
    pub(crate) fn check_a_refused_instant_message_keeps_its_draft() {
        use gtkhx_core::conn::{hx_conn_new, hx_conn_set_caps, hx_conn_set_sess};
        use gtkhx_core::messaging::with_roster_mut;
        // SAFETY: the connection stands in for its own session, as in
        // the checks above, and lives for the rest of the run.
        unsafe {
            let conn = hx_conn_new();
            let htlc: *mut c_void = conn.cast();
            hx_conn_set_sess(conn, htlc);
            crate::options_test_stubs::register_session(htlc);
            let view = RotulusView::new();
            let input = new_input();
            let win = MsgWin {
                conn: dock::conn_key(htlc),
                input: input.downgrade(),
                ..bobs(&view)
            };
            let say = |text: &str| {
                let buf = input.buffer();
                buf.set_text(text);
                (take(&win, &buf), whole(&buf))
            };
            let said = || crate::chat_output::tests::spoken(&view);
            let refused = "Not sent: bob is not a friend on this server.";
            let bob = |text: &str| Some((Peer::Login(b"bob".to_vec()), text.to_owned()));

            assert_eq!(say("hi"), (None, "hi".to_owned()), "no messaging");
            assert!(said().ends_with(refused), "{}", said());
            hx_conn_set_caps(htlc.cast(), CAP_MESSAGING);
            assert_eq!(say("hi"), (None, "hi".to_owned()), "not a friend");
            assert_eq!(
                win.model.borrow_mut().history.up(""),
                None,
                "refusals not kept"
            );
            with_roster_mut(htlc, |r| r.requests.served = true);
            assert_eq!(say("hi"), (bob("hi"), String::new()), "a request");
        }
    }

    /// A conversation whose peer leaves the server, or whose connection's
    /// user list is emptied, stays open saying they are offline, and sends
    /// nothing. Once someone of their nick joins, a bar offers to continue
    /// with the latest of them still there, and goes once none is; nothing
    /// goes to them until Continue, which files the conversation under
    /// their uid and sends to it from then on, or, where they have a
    /// conversation already, shows that one and leaves the two apart.
    pub(crate) fn check_a_conversation_whose_peer_left_goes_on_when_asked() {
        use gtkhx_core::conn::{hx_conn_new, hx_conn_set_sess};
        use gtkhx_core::session::{
            gtkhx_session_emit_user_change, gtkhx_session_emit_user_create,
            gtkhx_session_emit_user_delete, gtkhx_session_emit_users_clear,
            gtkhx_session_get_default,
        };
        use hxmodel::chat_members::{
            hx_member_model_clear, hx_member_model_remove, hx_member_model_upsert,
        };
        use hxmodel::conversation::{hx_chat_member_model, hx_conversation_new, HxConversation};
        type Conn = (*mut c_void, *mut HxConversation);
        hear_roster();
        // SAFETY: each connection stands in for its own session, as in
        // pchat's, and it and its public chat live for the rest of the run.
        unsafe {
            let session = gtkhx_session_get_default();
            let connect = || {
                let htlc = hx_conn_new();
                let sess: *mut c_void = htlc.cast();
                hx_conn_set_sess(htlc, sess);
                crate::options_test_stubs::register_session(sess);
                let lobby = hx_conversation_new(0);
                let chats = crate::options_test_stubs::chats();
                (chats.lock().unwrap()).push((sess as usize, 0, lobby as usize));
                crate::chat_tabs::tests::bare_view(sess);
                (sess, lobby) as Conn
            };
            // The members change first, as users.c's handlers, ahead of
            // msg's, change them.
            let join = |(htlc, lobby): Conn, uid, nick: &CStr| {
                hx_member_model_upsert(hx_chat_member_model(lobby), uid, nick.as_ptr(), 0, 0, 0);
                let nick = nick.as_ptr();
                gtkhx_session_emit_user_create(session, htlc, lobby.cast(), uid, 0, nick, 0, 0, 1);
            };
            let rename = |(htlc, lobby): Conn, uid, nick: &CStr| {
                hx_member_model_upsert(hx_chat_member_model(lobby), uid, nick.as_ptr(), 0, 0, 0);
                let nick = nick.as_ptr();
                gtkhx_session_emit_user_change(session, htlc, lobby.cast(), uid, 0, nick, 0, 0);
            };
            let leave = |(htlc, lobby): Conn, uid| {
                hx_member_model_remove(hx_chat_member_model(lobby), uid);
                gtkhx_session_emit_user_delete(session, htlc, lobby.cast(), uid, 1);
            };
            let clear = |(htlc, lobby): Conn| {
                hx_member_model_clear(hx_chat_member_model(lobby));
                gtkhx_session_emit_users_clear(session, htlc, lobby.cast());
            };
            let tab = |htlc, uid| crate::chat_tabs::find_msg(htlc, &Peer::Uid(uid));
            let title = |htlc, uid| tab(htlc, uid).map(|p| p.title().to_string());
            // Whatever the conversation is filed under.
            let shown = |htlc, win: &MsgWin| {
                let page = crate::chat_tabs::find_msg(htlc, &win.peer());
                page.map(|p| p.title().to_string())
            };
            let header = |win: &MsgWin| win.info_label.upgrade().unwrap().text().to_string();
            // What Return would send, and what it leaves in the input.
            let say = |win: &MsgWin, text: &str| {
                let buf = win.input.upgrade().unwrap().buffer();
                buf.set_text(text);
                (take(win, &buf), whole(&buf))
            };
            let to = |uid, text: &str| Some((Peer::Uid(uid), text.to_owned()));
            let offered = |win: &MsgWin| {
                let label = win.again.upgrade().unwrap();
                (label.parent().unwrap().is_visible()).then(|| label.text().to_string())
            };
            let said = |win: &MsgWin| {
                let view = win.output.upgrade().and_downcast::<RotulusView>().unwrap();
                crate::chat_output::tests::spoken(&view)
            };
            let again = Some("bob is online again".to_owned());
            let withdrawn = || WITHDRAWN.with_borrow_mut(std::mem::take);
            let go_on = |win: &MsgWin| win.go_on.upgrade().unwrap().emit_clicked();

            for (case, cleared) in [("left", false), ("cleared", true)] {
                let c = connect();
                let (htlc, conn) = (c.0, dock::conn_key(c.0));
                join(c, 5, c"bob");
                let win = filed(htlc, 5, "bob");
                assert_eq!(title(htlc, 5).as_deref(), Some("bob (5)"), "{case}");
                assert_eq!(say(&win, "hi"), (to(5, "hi"), String::new()), "{case}");
                let view = win.output.upgrade().and_downcast::<RotulusView>().unwrap();
                let answering = Target::Pm {
                    stored: true,
                    reference: [1; 16],
                };
                crate::reply_bar::start(&view, answering.clone(), &Quote::new("bob", "hi"));
                withdrawn();

                if cleared {
                    clear(c);
                } else {
                    leave(c, 5);
                }
                assert_eq!(
                    shown(htlc, &win).as_deref(),
                    Some("bob (offline)"),
                    "{case}"
                );
                assert_eq!(header(&win), "bob\nOffline", "{case}");
                assert!(!win.attach.upgrade().unwrap().is_sensitive(), "{case}");
                assert_eq!(win.to.get(), None, "{case}: the attachment's");
                let kept = "still there?".to_owned();
                assert_eq!(say(&win, &kept), (None, kept.clone()), "{case}");
                assert_eq!(say(&win, &kept).0, None, "{case}");
                let mut history = std::mem::take(&mut win.model.borrow_mut().history);
                assert_eq!(history.up(""), Some("hi".to_owned()), "{case}: kept for Up");
                assert_eq!(history.up(""), None, "{case}: refusals not kept");
                assert!(said(&win).ends_with("Not sent: bob is offline."), "{case}");
                let bobs_note = crate::chat_tabs::notify_id(conn, &Peer::Uid(5));
                assert_eq!(withdrawn(), [bobs_note], "{case}");

                // Bob's uid is someone else's now: what a message from it,
                // or opening it from the user list, would find is not his,
                // so it opens a conversation of its own, and its entry and
                // notifications are that one's.
                assert!(find(conn, &Peer::Uid(5)).is_none(), "{case}: 5 finds bob's");
                join(c, 5, c"eve");
                let eve = filed(htlc, 5, "eve");
                hx_msg_user_changed(htlc, 5, c"eve".as_ptr(), 0, 0);
                let fives = find(conn, &Peer::Uid(5)).map(|w| Rc::ptr_eq(&w, &eve));
                assert_eq!(fives, Some(true), "{case}");
                assert_eq!(title(htlc, 5).as_deref(), Some("eve (5)"), "{case}");
                let bobs_tab = crate::chat_tabs::find_msg(htlc, &win.peer());
                assert_ne!(tab(htlc, 5), bobs_tab, "{case}");
                let bobs = (shown(htlc, &win), header(&win));
                let untouched = (Some("bob (offline)".to_owned()), "bob\nOffline".to_owned());
                assert_eq!(bobs, untouched, "{case}");

                join(c, 9, c"carol");
                assert_eq!(offered(&win), None, "{case}: another nick");
                join(c, 8, c"bob");
                join(c, 12, c"bob");
                assert_eq!(offered(&win), again, "{case}");
                assert_eq!(say(&win, "hi").0, None, "{case}: not before Continue");
                leave(c, 12);
                assert_eq!(offered(&win), again, "{case}: one is still here");
                leave(c, 8);
                assert_eq!(offered(&win), None, "{case}: none is");

                join(c, 8, c"bob");
                join(c, 12, c"bob");
                let offline_note = crate::chat_tabs::notify_id(conn, &win.peer());
                go_on(&win);
                assert_eq!(withdrawn(), [offline_note], "{case}");
                let filed_as = |uid| find(conn, &Peer::Uid(uid)).map(|w| Rc::ptr_eq(&w, &win));
                assert_eq!(
                    (filed_as(5), filed_as(12)),
                    (Some(false), Some(true)),
                    "{case}"
                );
                let titles = (title(htlc, 5), title(htlc, 12));
                let want = (Some("eve (5)".to_owned()), Some("bob (12)".to_owned()));
                assert_eq!(titles, want, "{case}");
                let closes_as = tab(htlc, 12).and_then(|p| crate::chat_tabs::tests::peer_of(&p));
                assert_eq!(closes_as, Some(Peer::Uid(12)), "{case}");
                assert_eq!(offered(&win), None, "{case}");
                assert!(win.attach.upgrade().unwrap().is_sensitive(), "{case}");
                assert_eq!(win.to.get(), Some(12), "{case}: the attachment's");
                assert_eq!(say(&win, "back"), (to(12, "back"), String::new()), "{case}");
                let input = win.input.upgrade().unwrap();
                let reply = crate::reply_bar::take(&input);
                assert_eq!(reply, Some(answering), "{case}: the reply kept");
            }

            // A quick reconnect: the new session joins before the old one
            // leaves, and is offered at once.
            let c = connect();
            let conn = dock::conn_key(c.0);
            join(c, 5, c"bob");
            let win = filed(c.0, 5, "bob");
            join(c, 12, c"bob");
            leave(c, 5);
            assert_eq!(offered(&win), again, "joined first");
            go_on(&win);
            let filed_as_12 = find(conn, &Peer::Uid(12)).map(|w| Rc::ptr_eq(&w, &win));
            assert_eq!(filed_as_12, Some(true), "joined first");

            // Our own row, as a list's load or a rename of ours tells it, is
            // never offered.
            for (case, renamed) in [("our row joins", false), ("our rename", true)] {
                let c = connect();
                gtkhx_core::conn::hx_conn_set_uid(c.0.cast(), 2);
                join(c, 5, c"bob");
                let win = filed(c.0, 5, "bob");
                if renamed {
                    join(c, 2, c"guest");
                }
                leave(c, 5);
                if renamed {
                    rename(c, 2, c"bob");
                } else {
                    join(c, 2, c"bob");
                }
                assert_eq!(offered(&win), None, "{case}");
            }

            // A rename away from the nick is offered no more, and one to it
            // is, as a join.
            let c = connect();
            let conn = dock::conn_key(c.0);
            join(c, 5, c"bob");
            let win = filed(c.0, 5, "bob");
            leave(c, 5);
            join(c, 8, c"bob");
            join(c, 9, c"carol");
            assert_eq!(offered(&win), again, "renames");
            rename(c, 8, c"mallory");
            assert_eq!(offered(&win), None, "renamed away");
            rename(c, 9, c"bob");
            assert_eq!(offered(&win), again, "renamed to it");
            go_on(&win);
            let filed_as_9 = find(conn, &Peer::Uid(9)).map(|w| Rc::ptr_eq(&w, &win));
            assert_eq!(filed_as_9, Some(true), "renamed to it");

            // One opened with a uid the user list no longer holds starts
            // offline; a listed one, or one before the list is read, not.
            for (case, listed, uid, offline) in [
                ("gone", true, 7, true),
                ("listed", true, 3, false),
                ("no list yet", false, 7, false),
            ] {
                let c = connect();
                if listed {
                    join(c, 3, c"zoe");
                }
                let win = filed(c.0, uid, "dave");
                still_here(c.0, uid);
                let filed_as_uid = find(dock::conn_key(c.0), &Peer::Uid(uid)).is_some();
                assert_eq!(filed_as_uid, !offline, "{case}");
                assert_eq!(say(&win, "hi").0.is_none(), offline, "{case}");
            }

            // One opened before the list was read, which then holds no such
            // uid, goes offline as anything goes out.
            let c = connect();
            let win = filed(c.0, 7, "dave");
            assert_eq!(say(&win, "hi").0, to(7, "hi"), "before the list");
            join(c, 3, c"zoe");
            let kept = "again".to_owned();
            assert_eq!(say(&win, &kept), (None, kept), "the list read");
            assert!(matches!(win.peer(), Peer::Left(_)), "the list read");
            assert_eq!(shown(c.0, &win).as_deref(), Some("dave (offline)"));

            // A nick longer than C's buffer once in UTF-8 is matched whole,
            // not by the part of it that fits.
            let long = "\u{e9}".repeat(17);
            let (whole_nick, part) = (crate::cs(&long), crate::cs(&long[..30]));
            let c = connect();
            join(c, 5, &whole_nick);
            let win = filed(c.0, 5, "bob");
            refresh_user_info(&win, c.0);
            leave(c, 5);
            join(c, 8, &part);
            assert_eq!(offered(&win), None, "its first part");
            join(c, 9, &whole_nick);
            assert!(offered(&win).is_some(), "the whole nick");

            // The new bob messaged first: Continue shows his conversation.
            let c = connect();
            let (htlc, conn) = (c.0, dock::conn_key(c.0));
            join(c, 5, c"bob");
            let old = filed(htlc, 5, "bob");
            leave(c, 5);
            join(c, 8, c"bob");
            let new = filed(htlc, 8, "bob");
            crate::chat_tabs::raise_msg(htlc, &old.peer());
            old.go_on.upgrade().unwrap().emit_clicked();
            assert_eq!(crate::chat_tabs::tests::selected(htlc), tab(htlc, 8));
            for win in [&old, &new] {
                let filed = find(conn, &win.peer());
                assert!(filed.is_some_and(|w| Rc::ptr_eq(&w, win)));
            }
            assert_ne!(old.peer(), Peer::Uid(8));
            assert_eq!(shown(htlc, &old).as_deref(), Some("bob (offline)"));
            assert_eq!(say(&old, "hi").0, None);

            // Its tab closes as the offline conversation it is.
            let page = crate::chat_tabs::find_msg(htlc, &old.peer()).unwrap();
            assert_eq!(crate::chat_tabs::tests::peer_of(&page), Some(old.peer()));
            crate::chat_tabs::tests::close_msg(htlc, &old.peer());
            assert!(find(conn, &old.peer()).is_none(), "closed");
            assert!(of_conn(conn).iter().all(|w| !Rc::ptr_eq(w, &old)), "closed");
        }
    }

    /// A conversation with `uid` on `htlc` as `create` files one, holding
    /// the widgets these checks read.
    unsafe fn filed(htlc: *mut c_void, uid: u16, name: &str) -> Rc<MsgWin> {
        let view = RotulusView::new();
        let peer = Peer::Uid(uid);
        let (bar, again, go_on) = again_bar();
        let (input, info_label, info_image) =
            (new_input(), gtk::Label::new(None), gtk::Image::new());
        let attach = gtk::Button::new();
        let replying = crate::reply_bar::build(&view, input.upcast_ref());
        let content = gtk::Box::new(gtk::Orientation::Vertical, 0);
        for w in [
            view.upcast_ref::<gtk::Widget>(),
            info_image.upcast_ref(),
            info_label.upcast_ref(),
            bar.upcast_ref(),
            replying.upcast_ref(),
            input.upcast_ref(),
            attach.upcast_ref(),
        ] {
            content.append(w);
        }
        let win = Rc::new(MsgWin {
            conn: dock::conn_key(htlc),
            to: Rc::new(Cell::new(Some(uid))),
            model: RefCell::new(Conversation::new(name)),
            content: content.downgrade(),
            input: input.downgrade(),
            info_image: info_image.downgrade(),
            info_label: info_label.downgrade(),
            attach: attach.downgrade(),
            again: again.downgrade(),
            go_on: go_on.downgrade(),
            ..with(&view, peer.clone())
        });
        go_on_when_asked(&win);
        assert!(chat_tabs::add_msg(
            htlc,
            &content,
            &peer,
            &title(&peer, name)
        ));
        WINDOWS.with_borrow_mut(|t| t.insert(win.conn, peer, win.clone()));
        win
    }

    /// A friend's conversation with bob in `view`, as `create` files one.
    fn bobs(view: &RotulusView) -> MsgWin {
        with(view, Peer::Login(b"bob".to_vec()))
    }

    fn with(view: &RotulusView, peer: Peer) -> MsgWin {
        MsgWin {
            conn: 0,
            peer: RefCell::new(peer),
            to: Rc::default(),
            gone: RefCell::default(),
            model: RefCell::new(Conversation::new("bob")),
            content: Default::default(),
            output: view.upcast_ref::<gtk::Widget>().downgrade(),
            input: Default::default(),
            info_image: Default::default(),
            info_label: Default::default(),
            typing: Default::default(),
            attach: Default::default(),
            again: Default::default(),
            go_on: Default::default(),
            im: RefCell::default(),
            ims: RefCell::default(),
            pms: RefCell::default(),
            sending: RefCell::default(),
            answers: RefCell::default(),
            shown_in: RefCell::default(),
        }
    }

    /// Draw `text` in `view` as a row: its mark.
    fn row(view: &RotulusView, text: &str) -> MessageId {
        let w = view.upcast_ref::<gtk::Widget>().as_ptr();
        // SAFETY: a live view.
        let p = unsafe {
            rotulus::ffi::rotulus_view_append_text(w, text.as_ptr().cast(), text.len() as i32, 0)
        };
        MessageId(p as u64)
    }

    /// An instant message's chips go under its last row: what the friend
    /// reacts with as each change says, and ours at once as asked, held to
    /// no cap of a line's, kept once answered, and let go of where the
    /// connection went before the answer; a placeholder is none to ask
    /// for. Any of its rows is the message's; a row of no message, and a
    /// session that takes no instant message's reactions, is none of this.
    /// Each message is named by who sent it.
    pub(crate) fn check_an_instant_messages_chips_are_its_parties() {
        use gtkhx_core::reactions::Event;
        use hxproto::reactions::{Key, Limits, TARGETS_IMS, TARGETS_LINES};
        let up = "\u{1F44D}";
        let key = Key::Emoji(up.into());
        let takes = |targets| Limits {
            targets,
            max_keys: None,
            max_per_user: None,
        };
        let view = RotulusView::new();
        let win = bobs(&view);
        let (theirs, ours) = ([1; 16], [2; 16]);
        let mark = |row: MessageId| row.0 as *mut c_void;
        let first = row(&view, "hi");
        let last = row(&view, "there");
        let said = |text| Quote::new("bob", text);
        drew_im(&win, theirs, b"bob", said("hi"), (mark(first), mark(last)));
        let mine = row(&view, "hello");
        drew_im(
            &win,
            ours,
            b"alice",
            said("hello"),
            (mark(mine), mark(mine)),
        );
        let notice = row(&view, "no message");
        let shown = |row| {
            (view.reactions_of(row).into_iter())
                .map(|r| (r.key, r.count, r.mine))
                .collect::<Vec<_>>()
        };
        let ups = |n, mine| vec![(up.to_owned(), n, mine)];
        let changed = |add| Event::Changed {
            target: win.target(Msg::Im(theirs)).unwrap(),
            key: key.clone(),
            add,
            from: b"bob".to_vec(),
        };

        let (theirs_m, ours_m) = (Msg::Im(theirs), Msg::Im(ours));
        assert_eq!(heard(&win, theirs_m, &changed(true)), None);
        assert_eq!((shown(last), shown(first)), (ups(1, false), vec![]));
        let ims = takes(TARGETS_IMS);
        // Held to no cap of a line's.
        let capped = Limits {
            max_keys: Some(0),
            max_per_user: Some(0),
            ..ims
        };
        let send = Some(Then::Send(theirs_m, key.clone(), true));
        assert_eq!(want_in(&win, theirs_m, up, None, &capped), send);
        assert_eq!(shown(last), ups(2, true));
        // A connection's requests go with it, so this one's answer never
        // comes, and the next click asks again.
        unanswered(&win);
        assert_eq!(shown(last), ups(1, false));
        assert_eq!(want_in(&win, theirs_m, up, None, &ims), send);
        assert_eq!(shown(last), ups(2, true));
        let acted = Event::Acted {
            target: win.target(Msg::Im(theirs)).unwrap(),
            key: key.clone(),
            add: true,
            count: None,
        };
        assert_eq!(heard(&win, theirs_m, &acted), None);
        assert_eq!(heard(&win, theirs_m, &changed(false)), None);
        assert_eq!(shown(last), ups(1, true));
        assert_eq!(
            want_in(&win, theirs_m, "server-emoji:9", None, &ims),
            Some(Then::Say(crate::reactions::server_emoji()))
        );

        for (row, m) in [(first, theirs_m), (last, theirs_m), (mine, ours_m)] {
            assert_eq!(on_in(&win, row, &ims), Some(m));
        }
        for (case, row, limits) in [
            ("no message's row", notice, &ims),
            ("lines alone", last, &takes(TARGETS_LINES)),
        ] {
            assert_eq!(on_in(&win, row, limits), None, "{case}");
        }
        let named = |m| win.target(m);
        assert_eq!(
            (named(theirs_m), named(ours_m)),
            (
                Some(Target::Im {
                    guid: theirs,
                    sender: b"bob".to_vec()
                }),
                Some(Target::Im {
                    guid: ours,
                    sender: b"alice".to_vec()
                })
            )
        );
    }

    /// A message that answers another of the conversation's is headed with
    /// that one's first row, found by its id or reference, an instant
    /// message's whatever its Login is spelled, and as an earlier message
    /// where the conversation does not hold it; a target of no message
    /// heads nothing. Its header goes to the message it answers, or says it
    /// isn't there. Any row of a message offers a reply to it, where the
    /// login takes its kind; a private message only where the server
    /// stores it, ours once the server has named it.
    pub(crate) fn check_a_messages_reply_is_quoted_from_its_conversation() {
        use hxproto::replies::Targets;
        let mark = |row: MessageId| row.0 as *mut c_void;
        let im = |guid, sender: &[u8]| Target::Im {
            guid,
            sender: sender.to_vec(),
        };
        let pm = |stored, reference| Target::Pm { stored, reference };
        let drew = |win: &MsgWin, t: &Target, said: &Quote, (first, last)| match t {
            Target::Im { guid, sender } => {
                drew_im(win, *guid, sender, said.clone(), (mark(first), mark(last)))
            }
            &Target::Pm { stored, reference } => {
                drew_pm(win, (stored, reference), (first, last), said.clone())
            }
            _ => unreachable!(),
        };
        let said = Quote::new("bob", "hi");
        for (peer, kind, theirs, spelled, gone) in [
            (
                Peer::Login(b"bob".to_vec()),
                2,
                im([1; 16], b"bob"),
                im([1; 16], b"BOB"),
                im([9; 16], b"bob"),
            ),
            (
                Peer::Uid(5),
                5,
                pm(true, [1; 16]),
                pm(true, [1; 16]),
                pm(true, [9; 16]),
            ),
        ] {
            let view = RotulusView::new();
            let win = with(&view, peer);
            let (first, last) = (row(&view, "hi"), row(&view, "there"));
            drew(&win, &theirs, &said, (first, last));
            let replies = [spelled, gone, Target::Line { channel: 0, id: 1 }].map(|target| {
                let r = row(&view, "re");
                head(&win, mark(r), Some(&target));
                r
            });
            assert_eq!(
                crate::chat_output::tests::headers(&view),
                [
                    "replying to bob: hi",
                    "replying to a message: earlier message"
                ],
                "kind {kind}"
            );

            let toasts = || crate::options_test_stubs::TOASTS.with_borrow_mut(std::mem::take);
            toasts();
            jump_in(&win, &view, replies[0], false);
            assert_eq!((view.spotlight(), toasts()), (Some(first), vec![]));
            view.set_spotlight(None);
            jump_in(&win, &view, replies[1], false);
            assert_eq!(
                (view.spotlight(), toasts()),
                (None, vec!["That message can't be shown.".to_owned()]),
                "kind {kind}"
            );

            let takes = Targets(1 << kind);
            for at in [first, last] {
                assert_eq!(
                    reply_in(&win, at, &takes),
                    Some((theirs.clone(), said.clone())),
                    "kind {kind}"
                );
            }
            assert_eq!(reply_in(&win, first, &Targets(1 << 1)), None, "lines alone");
            assert_eq!(
                reply_in(&win, row(&view, "notice"), &takes),
                None,
                "no message's row"
            );
        }

        let view = RotulusView::new();
        let win = with(&view, Peer::Uid(5));
        let pms = Targets(1 << 5);
        let unstored = row(&view, "not kept");
        drew(&win, &pm(false, [3; 16]), &said, (unstored, unstored));
        assert_eq!(reply_in(&win, unstored, &pms), None, "not stored");
        let ours = row(&view, "ours");
        let mine = Quote::new("alice", "ours");
        (win.sending.borrow_mut()).insert(7, ((ours, ours), 1, mine.clone()));
        assert_eq!(reply_in(&win, ours, &pms), None, "not named yet");
        answered_in(&win, 7, Some((true, [2; 16])), 1);
        assert_eq!(
            reply_in(&win, ours, &pms),
            Some((pm(true, [2; 16]), mine)),
            "named"
        );
    }

    /// A stored private message's chips go under its last row: what the
    /// server says it holds, and ours over it at once as asked, held to the
    /// login's caps as a line is; an answer, which has no count, keeps ours
    /// until the server says otherwise, and a request still unanswered when
    /// the connection went is let go of. One we sent is named by the answer
    /// to its request, on the connect it went on, and nothing by a refusal.
    /// A message the server does not store, and a session that takes no
    /// stored message's reactions, is none of this. One is asked after
    /// once a while has passed since it was told of or asked after, and
    /// not again until another has.
    pub(crate) fn check_a_stored_messages_chips_are_the_servers() {
        use gtkhx_core::reactions::Event;
        use hxproto::reactions::{
            Key, Limits, Mine, Summary, TARGETS_LINES, TARGETS_STORED_PMS, TARGETS_UNSTORED_PMS,
        };
        let up = "\u{1F44D}";
        let key = Key::Emoji(up.into());
        let takes = |targets| Limits {
            targets,
            max_keys: None,
            max_per_user: None,
        };
        let view = RotulusView::new();
        let win = with(&view, Peer::Uid(5));
        let (theirs, unstored) = ((true, [1; 16]), (false, [3; 16]));
        let first = row(&view, "hi");
        let last = row(&view, "there");
        let said = Quote::new("bob", "hi");
        drew_pm(&win, theirs, (first, last), said.clone());
        let plain = row(&view, "not kept");
        drew_pm(&win, unstored, (plain, plain), said.clone());
        let shown = |row| {
            (view.reactions_of(row).into_iter())
                .map(|r| (r.key, r.count, r.mine))
                .collect::<Vec<_>>()
        };
        let ups = |n, mine| vec![(up.to_owned(), n, mine)];
        let m = Msg::Pm(theirs);
        let now = |n, mine: &[u8]| Event::Now {
            target: win.target(m).unwrap(),
            reactions: Some((
                Summary {
                    entries: vec![(key.clone(), n)],
                },
                Mine(mine.to_vec()),
            )),
        };

        assert_eq!(heard(&win, m, &now(1, &[])), None);
        assert_eq!((shown(last), shown(first)), (ups(1, false), vec![]));
        let stored = takes(TARGETS_STORED_PMS);
        let one_kind = Limits {
            max_keys: Some(1),
            ..stored
        };
        assert_eq!(
            want_in(&win, m, "\u{1F389}", None, &one_kind),
            Some(Then::Say(crate::reactions::refusal(
                Some(hxproto::reactions::TOO_MANY_KEYS),
                None
            )))
        );
        let send = Some(Then::Send(m, key.clone(), true));
        assert_eq!(want_in(&win, m, up, None, &stored), send);
        assert_eq!(shown(last), ups(2, true));
        let acted = Event::Acted {
            target: win.target(m).unwrap(),
            key: key.clone(),
            add: true,
            count: None,
        };
        assert_eq!(heard(&win, m, &acted), None);
        assert_eq!(shown(last), ups(2, true), "answered with no count");
        let one_of_ours = Limits {
            max_per_user: Some(1),
            ..stored
        };
        assert_eq!(
            want_in(&win, m, "\u{1F389}", None, &one_of_ours),
            Some(Then::Say(crate::reactions::refusal(
                Some(hxproto::reactions::TOO_MANY_OF_YOURS),
                None
            )))
        );
        assert_eq!(heard(&win, m, &now(1, &[])), None);
        assert_eq!(shown(last), ups(1, false), "the server dropped it");
        assert_eq!(want_in(&win, m, up, None, &stored), send);
        unanswered(&win);
        assert_eq!(shown(last), ups(1, false));

        for (row, want) in [(first, Some(m)), (last, Some(m)), (plain, None)] {
            assert_eq!(on_in(&win, row, &stored), want);
        }
        let all = takes(TARGETS_STORED_PMS | TARGETS_UNSTORED_PMS);
        assert_eq!(on_in(&win, plain, &all), None, "not stored");
        assert_eq!(
            on_in(&win, last, &takes(TARGETS_LINES)),
            None,
            "lines alone"
        );
        assert_eq!(
            win.target(m),
            Some(Target::Pm {
                stored: true,
                reference: [1; 16]
            })
        );

        let (sent, refused, earlier) = (row(&view, "ours"), row(&view, "no"), row(&view, "old"));
        for (trans, at, connect) in [(7, sent, 2), (8, refused, 2), (9, earlier, 1)] {
            (win.sending.borrow_mut()).insert(trans, ((at, at), connect, said.clone()));
        }
        let ours = (true, [2; 16]);
        assert!(answered_in(&win, 7, Some(ours), 2));
        assert!(answered_in(&win, 8, None, 2));
        assert!(
            !answered_in(&win, 9, Some((true, [4; 16])), 2),
            "another connect's"
        );
        assert!(!answered_in(&win, 7, Some(ours), 2), "answered twice");
        let named = [sent, refused, earlier].map(|r| on_in(&win, r, &stored));
        assert_eq!(named, [Some(Msg::Pm(ours)), None, None]);

        let start = win.pms.borrow().rows[&theirs].of.told;
        let later = start + crate::chat_output::FRESH + std::time::Duration::from_secs(1);
        assert_eq!(due(&win, m, start), None, "just told");
        assert_eq!(due(&win, m, later), win.target(m));
        assert_eq!(due(&win, m, later), None, "asked already");
        assert_eq!(due(&win, Msg::Im([1; 16]), later), None);
    }

    /// A conversation keeps each message's rows once, its first, and lets
    /// the oldest go once there are more than the view keeps rows.
    #[test]
    fn a_conversation_keeps_what_its_view_can_still_show() {
        let mut drawn = Drawn::<u8, ()>::default();
        let row = |n| (MessageId(n), MessageId(n));
        drawn.drew(1, row(1), (), 2);
        drawn.drew(1, row(9), (), 2);
        assert_eq!(drawn.on(MessageId(9)), None, "drawn again");
        drawn.drew(2, row(2), (), 2);
        drawn.drew(3, row(3), (), 2);
        let held = |d: &Drawn<u8, ()>| [1, 2, 3].map(|n| d.on(MessageId(n)).is_some());
        assert_eq!(held(&drawn), [false, true, true]);
        let mut all = Drawn::<u8, ()>::default();
        for n in 1..=3 {
            all.drew(n, row(u64::from(n)), (), 0);
        }
        assert_eq!(held(&all), [true; 3], "no limit");
        let mut late = Drawn::<u8, ()>::default();
        late.drew(2, row(2), (), 2);
        late.drew(1, row(1), (), 2);
        late.drew(3, row(3), (), 2);
        assert_eq!(held(&late), [false, true, true], "named after a later one");
    }

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
