//! Private messages. Each conversation is a tab in its connection's Chat
//! panel, beside the public chat and the private chats.
//!
//! [`hxmodel::pm`] is the model: the open conversations, keyed by connection
//! and peer, and the rules for what a message reads as. This module is the
//! view over it — a tab's widgets, its input, the transcript a message is
//! rendered into, the recipient pane above it — and the C ABI the remaining
//! C reaches it through: the `msg` signal's handler hands it each message,
//! `users.c` opens a conversation and tells it of a user's change, `notify.c`
//! asks for a conversation's tab, and the settings hooks restyle its views.

use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr};
use std::rc::{Rc, Weak};

use glib::translate::from_glib_none;
use gtk::gdk;
use gtk::glib;
use gtk::pango;
use gtk::prelude::*;
use gtk4 as gtk;

use gtkhx_core::boxed::msg::HxMsgEvent;
use gtkhx_core::conn::{hx_conn_name, hx_conn_sess, hx_conn_uid};
use hxmodel::chat_members::{hx_member_model_get_info, HxMemberInfo};
use hxmodel::conversation::hx_chat_member_model;
use hxmodel::pm::{self, Conversation, Conversations, Peer};

use crate::chat_tabs;
use crate::dock::{self, ConnKey};
use crate::tr::{tr, tr1};

/// `ROTULUS_PAL_NICK_BRACKET` and `ROTULUS_PAL_SELF_BRACKET` (rotulus.h,
/// pinned in chat.c): the brackets around someone else's nick and around ours.
const PAL_NICK_BRACKET: i16 = 41;
const PAL_SELF_BRACKET: i16 = 42;

extern "C" {
    // chat.c — the palette every chat view is drawn with, and a nick's color.
    static mut colors: gdk::ffi::GdkRGBA;
    fn hx_chat_nick_color(nick: *const c_char, len: usize, is_self: glib::ffi::gboolean) -> i16;
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    // gtkhx.c — the chat font, and its application to an input.
    static mut gtkhx_font_desc: *mut pango::ffi::PangoFontDescription;
    fn gtkhx_apply_input_font(w: *mut gtk::ffi::GtkWidget);
    fn gtkhx_apply_input_style(w: *mut gtk::ffi::GtkWidget);
    // users.c — a status word's color, or NULL for the theme's foreground.
    fn user_color_gdk(color: u16) -> *const gdk::ffi::GdkRGBA;
}

/// One conversation's tab. The widgets are held weakly: the tab strip owns
/// them, and a connection's close destroys its Chat page before its
/// conversations are forgotten.
struct MsgWin {
    conn: ConnKey,
    uid: u16,
    model: RefCell<Conversation>,
    content: glib::WeakRef<gtk::Box>,
    output: glib::WeakRef<gtk::Widget>,
    input: glib::WeakRef<gtk::TextView>,
    info_image: glib::WeakRef<gtk::Image>,
    info_label: glib::WeakRef<gtk::Label>,
}

thread_local! {
    static WINDOWS: RefCell<Conversations<Rc<MsgWin>>> = RefCell::new(Conversations::default());
}

fn find(conn: ConnKey, uid: u16) -> Option<Rc<MsgWin>> {
    WINDOWS.with_borrow(|t| t.get(conn, &Peer::Uid(uid)).cloned())
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

/// Open a conversation with `uid` on `htlc`, as its tab: built, filed,
/// selected, and its input focused.
///
/// # Safety
/// `htlc` is NULL or a live connection. Main thread.
unsafe fn create(htlc: *mut c_void, uid: u16, name: &CStr) -> Option<Rc<MsgWin>> {
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

    let win = Rc::new(MsgWin {
        conn,
        uid,
        model: RefCell::new(Conversation::new(name.to_string_lossy())),
        content: outer_vbox.downgrade(),
        output: output.downgrade(),
        input: input.downgrade(),
        info_image: info_image.downgrade(),
        info_label: info_label.downgrade(),
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
    let attach = crate::media_attach::message_button(htlc, uid);
    attach.set_valign(gtk::Align::End);
    input_hbox.append(&attach);
    crate::emoji::hx_emoji_typeahead_attach(wptr(&input));

    let inputframe = gtk::Frame::new(None);
    inputframe.set_child(Some(&input_hbox));

    let vpane = gtk::Box::new(gtk::Orientation::Vertical, 4);
    outputframe.set_vexpand(true);
    inputframe.set_vexpand(false);
    vpane.append(&outputframe);
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
    if !chat_tabs::add_msg(
        htlc,
        &outer_vbox,
        uid,
        &pm::title(&name.to_string_lossy(), uid),
    ) {
        crate::emoji::hx_emoji_typeahead_detach(wptr(&input));
        return None;
    }
    WINDOWS.with_borrow_mut(|t| t.insert(conn, Peer::Uid(uid), win.clone()));

    refresh_user_info(&win, htlc);
    chat_tabs::raise_msg(htlc, uid);
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
    let wire = crate::cs(&text.replace('\n', "\r"));
    unsafe {
        echo(htlc, win.uid, &crate::cs(&text));
        hxhandlers::send::msg::hx_send_msg(htlc, win.uid, wire.as_ptr());
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
    let conn = dock::conn_key(htlc);
    let win = match find(conn, uid) {
        Some(win) => win,
        None => create(htlc, uid, name.unwrap_or(c""))?,
    };

    let brackets = if is_self {
        PAL_SELF_BRACKET
    } else {
        PAL_NICK_BRACKET
    };
    // Pango asserts UTF-8, and a vintage server's text is Mac Roman.
    let body = hxproto::text::to_utf8(body.unwrap_or_default());
    let nick = name.map_or(&b""[..], CStr::to_bytes);

    if let Some(view) = win.output.upgrade() {
        let view = wptr(&view);
        for (i, line) in pm::rows(&body).enumerate() {
            if i > 0 {
                rotulus::ffi::rotulus_view_append_text(
                    view,
                    line.as_ptr().cast(),
                    line.len() as i32,
                    0,
                );
                continue;
            }
            let run = |text: &[u8], color: i16| rotulus::ffi::RotulusRun {
                text: text.as_ptr().cast(),
                len: text.len() as i32,
                color,
                attrs: 0,
                background: 0,
                rgb: 0,
                background_rgb: 0,
            };
            let nick_color = hx_chat_nick_color(nick.as_ptr().cast(), nick.len(), is_self.into());
            let gutter = [
                run(b"<", brackets),
                run(nick, nick_color),
                run(b">", brackets),
            ];
            let text = run(line.as_bytes(), rotulus::ffi::ROTULUS_COLOR_DEFAULT);
            let speaker = pm::speaker(outgoing, hx_conn_uid(htlc.cast()), uid);
            let row = rotulus::ffi::RotulusRow {
                kind: rotulus::ffi::ROW_MESSAGE,
                flags: if outgoing {
                    rotulus::ffi::ROW_OUTGOING
                } else {
                    0
                },
                stamp: 0,
                speaker: rotulus::ffi::RotulusSpeaker {
                    key: speaker.into(),
                    nick: nick.as_ptr().cast(),
                    nick_len: nick.len() as i32,
                },
                gutter: gutter.as_ptr(),
                n_gutter: gutter.len() as i32,
                body: &text,
                n_body: 1,
            };
            rotulus::ffi::rotulus_view_append(view, &row);
        }
    }

    if !is_self {
        chat_tabs::set_attention_msg(htlc, uid, true);
    }
    Some(win)
}

/// Draw the info pane from the public chat's entry for the peer, or from
/// the name the conversation was opened with when it has left.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
unsafe fn refresh_user_info(win: &MsgWin, htlc: *mut c_void) {
    let chat = chat_with_cid(hx_conn_sess(htlc.cast()), 0);
    let mut mi: HxMemberInfo = std::mem::zeroed();
    if !chat.is_null()
        && hx_member_model_get_info(hx_chat_member_model(chat.cast()), win.uid, &mut mi)
            != glib::ffi::GFALSE
    {
        let name = CStr::from_ptr(mi.name.as_ptr()).to_string_lossy();
        apply_user_view(win, htlc, Some(&name), mi.icon, mi.status, true);
    } else {
        apply_user_view(win, htlc, None, 0, 0, false);
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
        info_markup(&display, win.uid, Some((icon, &words, hex.as_deref())))
    } else {
        info_markup(&display, win.uid, None)
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
    chat_tabs::set_title_msg(htlc, win.uid, &pm::title(nick, win.uid));
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
    if find(dock::conn_key(htlc), uid).is_some() {
        chat_tabs::raise_msg(htlc, uid);
    } else {
        create(htlc, uid, name);
    }
}

/// The tab strip closed a conversation's tab.
pub(crate) fn tab_closed(conn: ConnKey, uid: u16) {
    let Some(win) = WINDOWS.with_borrow_mut(|t| t.remove(conn, &Peer::Uid(uid))) else {
        return;
    };
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
    if let Some(win) = find(conn, uid) {
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

/// `GtkWidget *hx_msg_window (session *, guint16 uid)` — the conversation's
/// tab content, or NULL when none is open.
///
/// # Safety
/// `sess` is NULL or a live session. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_msg_window(sess: *mut c_void, uid: u16) -> *mut gtk::ffi::GtkWidget {
    find(dock::key_for_session(sess), uid)
        .and_then(|w| w.content.upgrade())
        .map_or(std::ptr::null_mut(), |c| wptr(&c))
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
    use super::info_markup;

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
