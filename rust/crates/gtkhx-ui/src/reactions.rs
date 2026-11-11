//! Reacting to a lobby line, where the session has reactions (the
//! Reactions draft, capability bit 19): a click on a chip, or the row's
//! menu, with a few emoji to react with at once and GTK's emoji chooser
//! for any other, and who reacted with each emoji, from the menu or a
//! chip's own. What the line holds is `chat_output`'s; a refusal is told
//! as a toast.
//!
//! Without reactions no row is anyone's here, and the view's own menu
//! opens as before.

use gtk::prelude::*;
use gtk::{gdk, glib};
use gtk4 as gtk;
use hxproto::reactions::{Key, INVALID_KEY, TOO_MANY_KEYS, TOO_MANY_OF_YOURS};
use hxproto::structured_chat::{INVALID_TARGET, NOT_ALLOWED, NO_SUCH_TARGET, RATE_LIMITED};
use hxproto::target::Target;
use rotulus::view::RowMark;
use rotulus::RotulusView;
use rotulus_layout::MessageId;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;

use crate::chat_output::{emoji_on, line_on, refresh_if_stale, then, want, Then};
use crate::tr::{tr, tr_argv, trn_argv};

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
/// way round. Whether that was refused before any request.
fn react(view: &RotulusView, row: MessageId, key: &str, on: Option<bool>) -> bool {
    let Some((htlc, limits)) = session(view) else {
        return false;
    };
    let t = want(view, row, key, on, &limits);
    let refused = matches!(t, Some(Then::Say(_)));
    if let Some(t) = t {
        // SAFETY: as for `session`.
        unsafe { then(htlc, view, t) };
    }
    refused
}

/// React to `row` with `emoji`, picked from the menu or the chooser, which
/// goes first among the menu's after unless it was refused.
fn pick(view: &RotulusView, row: MessageId, emoji: &str) {
    let emoji = hxproto::emoji::qualify(emoji);
    if react(view, row, emoji, Some(true)) {
        return;
    }
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

/// A menu's column of items.
fn items() -> gtk::Box {
    let items = gtk::Box::new(gtk::Orientation::Vertical, 0);
    for set in [
        gtk::Box::set_margin_start,
        gtk::Box::set_margin_end,
        gtk::Box::set_margin_top,
        gtk::Box::set_margin_bottom,
    ] {
        set(&items, 4);
    }
    items
}

/// `row`'s menu at `x`, `y`: the quick emoji, "Add Reaction…" for the
/// chooser, "Who Reacted…" where it has emoji chips, and the view's own
/// Copy and Select All. Whether `row` is a line one can react to.
fn menu(view: &RotulusView, row: MessageId, x: f64, y: f64) -> bool {
    let Some((htlc, limits)) = session(view) else {
        return false;
    };
    let Some(id) = line_on(view, row, &limits) else {
        return false;
    };
    // SAFETY: as for `session`.
    unsafe { refresh_if_stale(view, htlc, id) };
    let popover = gtk::Popover::new();
    let items = items();
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
    let emoji = emoji_on(view, id);
    if !emoji.is_empty() {
        // TRANSLATORS: in a chat line's menu; lists the emoji the line has
        // been reacted with, to see who reacted with one.
        item(&items, &popover, view, &tr("Who Reacted…"), move |v| {
            reacted_with(v, row, &emoji, x, y)
        });
    }
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

/// The emoji `row` has been reacted with, at `x`, `y`, each to see who
/// reacted with it, or who reacted with the one there is.
fn reacted_with(view: &RotulusView, row: MessageId, emoji: &[String], x: f64, y: f64) {
    if let [one] = emoji {
        who(view, row, one, x, y);
        return;
    }
    let popover = gtk::Popover::new();
    let items = items();
    for e in emoji {
        let key = e.clone();
        item(&items, &popover, view, e, move |v| {
            who(v, row, &key, x, y);
        });
    }
    popover.set_child(Some(&items));
    popover_at(view, &popover, x, y);
}

thread_local! {
    /// The lists of who reacted still waiting on their answers, by what
    /// each asked.
    static ASKING: RefCell<HashMap<u64, glib::WeakRef<gtk::ListBox>>> =
        RefCell::new(HashMap::new());
}

/// Who reacted to `row` with `emoji`, at `x`, `y`, as the server says.
/// Whether `row` holds that emoji, on a line one can react to.
fn who(view: &RotulusView, row: MessageId, emoji: &str, x: f64, y: f64) -> bool {
    let Some((htlc, limits)) = session(view) else {
        return false;
    };
    let Some(id) = line_on(view, row, &limits) else {
        return false;
    };
    // A server emoji's placeholder is left to the row's menu: GtkHx sends
    // no such key, and a server without the Server Emoji draft takes none.
    if !emoji_on(view, id).iter().any(|e| e == emoji) {
        return false;
    }
    let line = Target::Line { channel: 0, id };
    // SAFETY: as for `session`.
    let ask = unsafe { hxhandlers::send::reactions::who(htlc, line, Key::Emoji(emoji.into())) };
    // A server names up to a hundred.
    let names = gtk::ScrolledWindow::new();
    names.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Never);
    names.set_propagate_natural_height(true);
    names.set_max_content_height(NAMES_HEIGHT);
    names.set_child(Some(&asking(ask, emoji)));
    let popover = gtk::Popover::new();
    popover.set_child(Some(&names));
    popover.connect_closed(move |_| {
        if let Some(ask) = ask {
            ASKING.with_borrow_mut(|a| a.remove(&ask));
        }
    });
    popover_at(view, &popover, x, y);
    true
}

/// How tall the list of who reacted grows before it scrolls.
const NAMES_HEIGHT: i32 = 320;

/// The list of who reacted with `emoji`, loading until [`heard`] fills it
/// for `ask`, or with nobody to show if nothing was asked.
fn asking(ask: Option<u64>, emoji: &str) -> gtk::ListBox {
    let list = gtk::ListBox::new();
    list.set_selection_mode(gtk::SelectionMode::None);
    // TRANSLATORS: names the list of who reacted to a chat line with %s,
    // an emoji.
    let name = tr_argv("Who reacted with %s", &[emoji]);
    list.update_property(&[gtk::accessible::Property::Label(&name)]);
    match ask {
        Some(ask) => {
            // TRANSLATORS: while the list of who reacted is on its way.
            rows(&list, &[(tr("Loading…"), None)]);
            ASKING.with_borrow_mut(|a| a.insert(ask, list.downgrade()));
        }
        None => fill(&list, &[], 0),
    }
    list
}

/// A reactor's nick, and the icon of whoever online goes by it.
pub(crate) type Who = (String, Option<gdk::Paintable>);

/// What answered `ask`: who holds its emoji, newest first, of `count`.
pub(crate) fn heard(ask: u64, who: &[Who], count: u32) {
    let list = ASKING.with_borrow_mut(|a| a.remove(&ask));
    if let Some(list) = list.and_then(|l| l.upgrade()) {
        fill(&list, who, count);
    }
}

fn fill(list: &gtk::ListBox, who: &[Who], count: u32) {
    let more = u64::from(count).saturating_sub(who.len() as u64);
    let mut lines = who.to_vec();
    if who.is_empty() {
        lines.push((tr("Nobody can be shown for this reaction."), None));
    } else if more > 0 {
        // TRANSLATORS: after the names of who reacted, how many more did.
        let more = trn_argv("and %u more", "and %u more", more, &[&more.to_string()]);
        lines.push((more, None));
    }
    rows(list, &lines);
}

/// `list` holds `lines`, a row each, with its icon where it has one. A
/// row already there is changed rather than replaced, so the keyboard
/// stays on it.
fn rows(list: &gtk::ListBox, lines: &[Who]) {
    for (i, (text, icon)) in lines.iter().enumerate() {
        let row = list.row_at_index(i as i32).and_then(|r| r.child());
        let (picture, label) = match row.and_downcast::<gtk::Box>() {
            Some(b) => (
                b.first_child().and_downcast::<gtk::Picture>().unwrap(),
                b.last_child().and_downcast::<gtk::Label>().unwrap(),
            ),
            None => {
                let b = gtk::Box::new(gtk::Orientation::Horizontal, 6);
                for set in [
                    gtk::Box::set_margin_start,
                    gtk::Box::set_margin_end,
                    gtk::Box::set_margin_top,
                    gtk::Box::set_margin_bottom,
                ] {
                    set(&b, 4);
                }
                // The name beside it says whose it is.
                let picture = gtk::Picture::builder()
                    .accessible_role(gtk::AccessibleRole::Presentation)
                    .can_shrink(false)
                    .build();
                let label = gtk::Label::new(None);
                label.set_xalign(0.0);
                b.append(&picture);
                b.append(&label);
                list.append(&b);
                (picture, label)
            }
        };
        picture.set_paintable(icon.as_ref());
        picture.set_visible(icon.is_some());
        label.set_text(text);
    }
    // A scrollbar that may show is room kept for it, however short the list.
    if let Some(sw) = list
        .ancestor(gtk::ScrolledWindow::static_type())
        .and_downcast::<gtk::ScrolledWindow>()
    {
        let (_, height, _, _) = list.measure(gtk::Orientation::Vertical, -1);
        let scroll = if height > NAMES_HEIGHT {
            gtk::PolicyType::Automatic
        } else {
            gtk::PolicyType::Never
        };
        sw.set_vscrollbar_policy(scroll);
    }
    // Hung from the window, whose allocation knows nothing of it, an open
    // popover is laid out again only when presented, and closes otherwise.
    let popover = list.ancestor(gtk::Popover::static_type());
    if let Some(p) = popover.and_downcast::<gtk::Popover>() {
        p.present();
    }
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
        "reaction-menu",
        false,
        glib::closure_local!(|view: RotulusView,
                              m: RowMark,
                              key: String,
                              x: f64,
                              y: f64|
         -> bool { who(&view, m.0, &key, x, y) }),
    );
    view.connect_closure(
        "row-menu",
        false,
        glib::closure_local!(|view: RotulusView, m: RowMark, x: f64, y: f64| -> bool {
            menu(&view, m.0, x, y)
        }),
    );
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    fn shown(list: &gtk::ListBox) -> Vec<String> {
        let mut rows = Vec::new();
        while let Some(row) = list.row_at_index(rows.len() as i32) {
            let b = row.child().unwrap();
            let label = b.last_child().and_downcast::<gtk::Label>().unwrap();
            rows.push(label.text().to_string());
        }
        rows
    }

    /// Which rows of `list` show an icon.
    fn iconed(list: &gtk::ListBox) -> Vec<bool> {
        let mut rows = Vec::new();
        while let Some(row) = list.row_at_index(rows.len() as i32) {
            let b = row.child().unwrap();
            let picture = b.first_child().and_downcast::<gtk::Picture>().unwrap();
            rows.push(picture.is_visible() && picture.paintable().is_some());
        }
        rows
    }

    /// Who reacted says it is loading until its answer, then who, newest
    /// first, and how many more the server did not name, or that nobody
    /// can be shown; a list gone before its answer is left alone, and one
    /// open stays open.
    pub(crate) fn check_who_reacted_says_who_once_answered() {
        let nobody = "Nobody can be shown for this reaction.";
        let ask = u64::MAX;
        let list = asking(Some(ask), "\u{1F44D}");
        assert_eq!(shown(&list), ["Loading\u{2026}"]);
        let names = |n: &[&str]| {
            n.iter()
                .map(|n| (n.to_string(), None))
                .collect::<Vec<Who>>()
        };
        let icon = gdk::Paintable::new_empty(32, 16);
        heard(ask, &[("b".into(), Some(icon)), ("a".into(), None)], 5);
        assert_eq!(shown(&list), ["b", "a", "and 3 more"]);
        assert_eq!(iconed(&list), [true, false, false], "b's icon alone");
        heard(ask, &[], 0);
        assert_eq!(shown(&list), ["b", "a", "and 3 more"], "answered twice");

        for (case, nicks, count, want) in [
            ("everyone named", &["b", "a"][..], 2, &["b", "a"][..]),
            ("an empty answer", &[], 0, &[nobody]),
            ("a count without names", &[], 3, &[nobody]),
        ] {
            let list = asking(Some(ask), "\u{1F44D}");
            heard(ask, &names(nicks), count);
            assert_eq!(shown(&list), want, "{case}");
        }
        assert_eq!(shown(&asking(None, "\u{1F44D}")), [nobody], "not asked");

        // Short, it keeps no room for a scrollbar; past its height, it
        // scrolls.
        for (case, n, want) in [
            ("short", 2, gtk::PolicyType::Never),
            ("long", 100, gtk::PolicyType::Automatic),
        ] {
            let scrolled = gtk::ScrolledWindow::new();
            scrolled.set_child(Some(&asking(Some(ask), "\u{1F44D}")));
            let nicks: Vec<String> = (0..n).map(|i| i.to_string()).collect();
            let nicks: Vec<&str> = nicks.iter().map(String::as_str).collect();
            heard(ask, &names(&nicks), n);
            assert_eq!(scrolled.vscrollbar_policy(), want, "{case}");
        }

        drop(asking(Some(ask), "\u{1F44D}"));
        heard(ask, &names(&["a"]), 1);
        assert!(ASKING.with_borrow(|a| a.is_empty()));

        // Hung from a window, as the view's are, it stays open once filled.
        let window = gtk::Window::new();
        window.present();
        let popover = gtk::Popover::new();
        popover.set_child(Some(&asking(Some(ask), "\u{1F44D}")));
        popover.set_parent(&window);
        popover.popup();
        let settle = || {
            let ctx = glib::MainContext::default();
            let until = std::time::Instant::now() + std::time::Duration::from_millis(300);
            while std::time::Instant::now() < until {
                ctx.iteration(false);
            }
        };
        settle();
        heard(ask, &names(&["b", "a"]), 2);
        settle();
        assert!(popover.is_visible(), "closed once filled");
        popover.unparent();
        window.destroy();
    }
}
