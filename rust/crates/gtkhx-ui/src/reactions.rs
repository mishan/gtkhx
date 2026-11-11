//! Reacting to a lobby line, where the session has reactions (the
//! Reactions draft, capability bit 19): a click on a chip, or the row's
//! menu, with a few emoji to react with at once and GTK's emoji chooser
//! for any other. What the line holds is `chat_output`'s; a refusal is
//! told as a toast.
//!
//! Without reactions no row is anyone's here, and the view's own menu
//! opens as before.

use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxproto::reactions::{INVALID_KEY, TOO_MANY_KEYS, TOO_MANY_OF_YOURS};
use hxproto::structured_chat::{INVALID_TARGET, NOT_ALLOWED, NO_SUCH_TARGET, RATE_LIMITED};
use rotulus::view::RowMark;
use rotulus::RotulusView;
use rotulus_layout::MessageId;
use std::ffi::c_void;

use crate::chat_output::{then, want};
use crate::tr::tr;

/// How many emoji the menu offers at once.
const QUICK: usize = 5;

/// What the user is told of a refusal of their own reaction, by the
/// draft's `reason` code, or else the server's own `error`.
pub(crate) fn refusal(reason: Option<u16>, error: Option<&str>) -> String {
    match reason {
        Some(NO_SUCH_TARGET) => tr("That line can't take reactions any more."),
        Some(NOT_ALLOWED) => tr("You can't react there."),
        Some(INVALID_KEY) => tr("The server doesn't take that emoji as a reaction."),
        Some(TOO_MANY_KEYS) => tr("This line has as many kinds of reaction as it can take."),
        Some(TOO_MANY_OF_YOURS) => tr("You have as many reactions on this line as you can."),
        // TRANSLATORS: the user reacted faster than the server allows.
        Some(RATE_LIMITED) => tr("Slow down a moment."),
        Some(INVALID_TARGET) => tr("That can't take reactions on this server."),
        _ => error.filter(|e| !e.is_empty()).map_or_else(
            || tr("The server didn't take that reaction."),
            str::to_owned,
        ),
    }
}

/// Why a chip for one of the server's own emoji does nothing: without the
/// Server Emoji draft, the server takes none from GtkHx.
pub(crate) fn server_emoji() -> String {
    tr("This is one of the server's own emoji, which this version of GtkHx can't add or remove.")
}

pub(crate) fn toast(text: &str) {
    unsafe { crate::ffi::toolbar_show_toast(crate::cs(text).as_ptr()) };
}

/// The connection `view` is on, where its session has reactions, and the
/// limits its login gave.
fn session(view: &RotulusView) -> Option<(*mut c_void, hxproto::reactions::Limits)> {
    let htlc = crate::dock::htlc_for(crate::chat_view::conn_of(view)?);
    if htlc.is_null() {
        return None;
    }
    // SAFETY: main thread; the connection is live.
    Some((htlc, unsafe { hxhandlers::recv::reactions::limits(htlc)? }))
}

/// The user wants the chip `key` on `row` on, off, or (`None`) the other
/// way round.
fn react(view: &RotulusView, row: MessageId, key: &str, on: Option<bool>) {
    let Some((htlc, limits)) = session(view) else {
        return;
    };
    if let Some(t) = want(view, row, key, on, &limits) {
        // SAFETY: as for `session`.
        unsafe { then(htlc, view, t) };
    }
}

/// React to `row` with `emoji`, picked from the menu or the chooser, which
/// goes first among the menu's after.
fn pick(view: &RotulusView, row: MessageId, emoji: &str) {
    let emoji = hxproto::emoji::qualify(emoji);
    react(view, row, emoji, Some(true));
    let mut quick = vec![emoji.to_owned()];
    quick.extend(self::quick().into_iter().filter(|e| e != emoji));
    quick.truncate(QUICK);
    crate::options::pref_set_string("QUICKREACTIONS", &quick.join(","));
}

fn quick() -> Vec<String> {
    let quick = hxconfig::ffi::with_settings(|s| s.chat.emoji.quick_reactions.clone());
    let mut quick = quick.unwrap_or_default();
    quick.truncate(QUICK);
    quick
}

/// A popover at `x`, `y` in `view`, hung from its window as the view's own
/// menus are, which gives the view the keyboard back when it closes.
fn popover_at(view: &RotulusView, popover: &impl IsA<gtk::Popover>, x: f64, y: f64) {
    let popover = popover.upcast_ref::<gtk::Popover>();
    let Some(root) = view.root() else {
        return;
    };
    let root: gtk::Widget = root.upcast();
    let at = view.compute_point(&root, &gtk::graphene::Point::new(x as f32, y as f32));
    let at = at.unwrap_or_else(|| gtk::graphene::Point::new(x as f32, y as f32));
    popover.set_has_arrow(false);
    popover.set_halign(gtk::Align::Start);
    popover.set_pointing_to(Some(&gtk::gdk::Rectangle::new(
        at.x() as i32,
        at.y() as i32,
        1,
        1,
    )));
    popover.set_parent(&root);
    let refocus = view.is_focus().then(|| view.downgrade());
    popover.connect_closed(move |p| {
        // After the pick that closed it: GTK before 4.16.6 closes an emoji
        // chooser before it says what was picked.
        let p = p.clone();
        glib::idle_add_local_once(move || p.unparent());
        if let Some(view) = refocus.as_ref().and_then(|v| v.upgrade()) {
            view.grab_focus();
        }
    });
    popover.popup();
}

/// `row`'s menu at `x`, `y`: the quick emoji, "Add Reaction…" for the
/// chooser, and the view's own Copy and Select All. Whether `row` is a line one can react to.
fn menu(view: &RotulusView, row: MessageId, x: f64, y: f64) -> bool {
    let Some((_, limits)) = session(view) else {
        return false;
    };
    if crate::chat_output::line_on(view, row, &limits).is_none() {
        return false;
    }
    let popover = gtk::Popover::new();
    let items = gtk::Box::new(gtk::Orientation::Vertical, 0);
    for set in [
        gtk::Box::set_margin_start,
        gtk::Box::set_margin_end,
        gtk::Box::set_margin_top,
        gtk::Box::set_margin_bottom,
    ] {
        set(&items, 4);
    }
    let quick = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    for emoji in self::quick() {
        let b = gtk::Button::with_label(&emoji);
        b.add_css_class("flat");
        let (view, popover) = (view.downgrade(), popover.downgrade());
        b.connect_clicked(move |_| {
            popover.upgrade().inspect(|p| p.popdown());
            view.upgrade().inspect(|v| pick(v, row, &emoji));
        });
        quick.append(&b);
    }
    items.append(&quick);
    // TRANSLATORS: in a chat line's menu; opens an emoji chooser to react
    // to the line with.
    item(&items, &popover, view, &tr("Add Reaction…"), move |v| {
        chooser(v, row, x, y)
    });
    items.append(&gtk::Separator::new(gtk::Orientation::Horizontal));
    // This menu stands in for the view's own, so it keeps what that offers.
    if view.has_selection() {
        item(&items, &popover, view, &tr("Copy"), |v| {
            v.clipboard().set_text(&v.selected_text())
        });
    }
    item(
        &items,
        &popover,
        view,
        &tr("Select All"),
        RotulusView::select_all,
    );
    popover.set_child(Some(&items));
    popover_at(view, &popover, x, y);
    true
}

/// A menu item in `items` that closes `popover` and does `f` to `view`.
fn item(
    items: &gtk::Box,
    popover: &gtk::Popover,
    view: &RotulusView,
    label: &str,
    f: impl Fn(&RotulusView) + 'static,
) {
    let b = gtk::Button::with_label(label);
    b.add_css_class("flat");
    if let Some(l) = b.child().and_downcast::<gtk::Label>() {
        l.set_xalign(0.0);
    }
    let (view, popover) = (view.downgrade(), popover.downgrade());
    b.connect_clicked(move |_| {
        popover.upgrade().inspect(|p| p.popdown());
        view.upgrade().inspect(|v| f(v));
    });
    items.append(&b);
}

/// GTK's emoji chooser at `x`, `y`, to react to `row` with what it picks.
fn chooser(view: &RotulusView, row: MessageId, x: f64, y: f64) {
    let chooser = gtk::EmojiChooser::new();
    let weak = view.downgrade();
    chooser.connect_emoji_picked(move |_, emoji| {
        weak.upgrade().inspect(|v| pick(v, row, emoji));
    });
    popover_at(view, &chooser, x, y);
}

/// Hear the chips clicked and the rows' menus asked for in `view`, the
/// public chat's.
pub(crate) fn hear(view: &RotulusView) {
    view.connect_closure(
        "reaction-activated",
        false,
        glib::closure_local!(|view: RotulusView, m: RowMark, key: String| {
            react(&view, m.0, &key, None);
        }),
    );
    view.connect_closure(
        "row-menu",
        false,
        glib::closure_local!(|view: RotulusView, m: RowMark, x: f64, y: f64| -> bool {
            menu(&view, m.0, x, y)
        }),
    );
}
