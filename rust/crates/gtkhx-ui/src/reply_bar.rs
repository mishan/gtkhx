//! The "Replying to …" bar over a chat's input, where the session has
//! replies (the Replies draft, capability bit 18): Reply in a row's menu
//! shows it with who said the line and what, and the next line typed and
//! sent as chat names that line. Its button or Escape in the input cancels
//! it; the view cleared (at a connect, or by `/clear`), the window closing,
//! and the line sent let go of it. A line
//! the scrollback lets go of meanwhile is still the one answered: the
//! reply names it, not its row.
//!
//! While it shows, the input's accessible description is its text, so a
//! screen reader says what is being answered on each arrival in the input.

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use gtk::glib;
use gtk::glib::translate::from_glib_none;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::replies::Quote;
use hxproto::target::Target;
use libadwaita as adw;
use rotulus::RotulusView;

use crate::name_shadow;
use crate::tr::{tr, tr_argv};

/// What a bar holds, kept on its view and on its input alike.
struct Bar {
    bar: glib::WeakRef<gtk::Box>,
    label: glib::WeakRef<gtk::Label>,
    input: glib::WeakRef<gtk::Widget>,
    view: glib::WeakRef<RotulusView>,
    /// The line the next one said answers.
    to: RefCell<Option<Target>>,
    /// What the bar says of it.
    quote: RefCell<Option<Quote>>,
}

const KEY: &str = "gtkhx-reply-bar";

extern "C" {
    // gtkhx_theme.c
    fn gtkhx_theme_get_default() -> *mut std::ffi::c_void;
}

fn of(w: &impl IsA<glib::Object>) -> Option<Rc<Bar>> {
    // SAFETY: the key is only ever set to an Rc<Bar>, on the main thread.
    let bar = unsafe { w.data::<Rc<Bar>>(KEY) }.map(|b| unsafe { b.as_ref() }.clone())?;
    bar.bar.upgrade().is_some().then_some(bar)
}

/// The bar for `view`'s replies, sent from `input`: hidden until Reply is
/// chosen, and packed above the input by the caller.
pub(crate) fn build(view: &RotulusView, input: &gtk::Widget) -> gtk::Box {
    let bar = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    bar.set_visible(false);
    let label = gtk::Label::new(None);
    label.set_xalign(0.0);
    label.set_hexpand(true);
    label.set_single_line_mode(true);
    label.set_ellipsize(gtk::pango::EllipsizeMode::End);
    label.set_margin_start(6);
    bar.append(&label);
    // TRANSLATORS: the button beside "Replying to …" over the chat input,
    // which sends the next line as an ordinary one.
    let cancel = crate::icon_button("window-close-symbolic", &tr("Cancel reply"));
    cancel.add_css_class("flat");
    bar.append(&cancel);

    let state = Rc::new(Bar {
        bar: bar.downgrade(),
        label: label.downgrade(),
        input: input.downgrade(),
        view: view.downgrade(),
        to: RefCell::new(None),
        quote: RefCell::new(None),
    });
    // SAFETY: the key is only ever set to an Rc<Bar>, on the main thread.
    unsafe {
        view.set_data(KEY, state.clone());
        input.set_data(KEY, state.clone());
    }
    let weak = Rc::downgrade(&state);
    cancel.connect_clicked(move |_| {
        if let Some(state) = weak.upgrade() {
            hide(&state);
            if let Some(input) = state.input.upgrade() {
                input.grab_focus();
            }
        }
    });
    // The palette, the window's background and the contrast asked on it
    // can change while the bar shows; drawn again once the theme's own
    // handlers have applied the change. A switch to or from high contrast
    // is the theme's "changed" too.
    let weak = Rc::downgrade(&state);
    let changed = move || {
        let weak = weak.clone();
        glib::idle_add_local_once(move || {
            if let Some(state) = weak.upgrade().filter(|s| s.to.borrow().is_some()) {
                draw(&state);
            }
        });
    };
    let style: glib::Object = adw::StyleManager::default().upcast();
    // SAFETY: the theme is a singleton that lives as long as the process.
    let theme: glib::Object = unsafe { from_glib_none(gtkhx_theme_get_default().cast()) };
    let notified = Cell::new(Some([
        (
            style.clone(),
            style.connect_notify_local(Some("dark"), {
                let changed = changed.clone();
                move |_, _| changed()
            }),
        ),
        (
            theme.clone(),
            theme.connect_local("changed", false, move |_| {
                changed();
                None
            }),
        ),
    ]));
    // The input is C's, and can outlive the bar it was described by.
    let weak = Rc::downgrade(&state);
    bar.connect_destroy(move |_| {
        for (object, id) in notified.take().into_iter().flatten() {
            object.disconnect(id);
        }
        if let Some(input) = weak.upgrade().and_then(|s| s.input.upgrade()) {
            input.reset_property(gtk::AccessibleProperty::Description);
        }
    });
    bar
}

/// Whether `view` has a bar to reply from.
pub(crate) fn offered(view: &RotulusView) -> bool {
    of(view).is_some()
}

/// Reply to the line `target` names, which `quote` quotes: the bar shows
/// it, in place of any other, and the input takes the keyboard.
pub(crate) fn start(view: &RotulusView, target: Target, quote: &Quote) {
    let Some(state) = of(view) else {
        return;
    };
    let (Some(bar), Some(input)) = (state.bar.upgrade(), state.input.upgrade()) else {
        return;
    };
    *state.to.borrow_mut() = Some(target);
    *state.quote.borrow_mut() = Some(quote.clone());
    draw(&state);
    let plain = say(&quote.nick, &quote.excerpt);
    input.update_property(&[gtk::accessible::Property::Description(&plain)]);
    bar.set_visible(true);
    input.grab_focus();
}

fn say(nick: &str, excerpt: &str) -> String {
    // TRANSLATORS: over the chat input while a reply is being written:
    // who said the line it answers (%1$s), and what (%2$s).
    tr_argv("Replying to %1$s: %2$s", &[nick, excerpt])
}

/// The bar's label, for the quote it holds, in the colors drawn in now.
fn draw(state: &Bar) {
    let (Some(view), Some(label)) = (state.view.upgrade(), state.label.upgrade()) else {
        return;
    };
    let quote = state.quote.borrow();
    let Some(quote) = quote.as_ref() else {
        return;
    };
    // Escaped whole, so that a translation holding markup's own characters
    // is shown as written; the nick then goes in bold, in the color the
    // view draws it in.
    let marked = glib::markup_escape_text(&say("\u{E000}", "\u{E001}"));
    let c = view.text_color(crate::chat_output::nick_color(&view, &quote.nick));
    let nick = glib::markup_escape_text(&quote.nick);
    // Rotulus makes the color read on the view's background, but the bar
    // sits on the window's: one that does not read there is left the bar's.
    let ink = name_shadow::rel_luminance(c.red().into(), c.green().into(), c.blue().into());
    let nick = if name_shadow::contrast(ink, name_shadow::window_luminance()) >= min_contrast() {
        let [r, g, b] = [c.red(), c.green(), c.blue()].map(|v| (v * 255.0).round() as u8);
        format!("<b><span foreground=\"#{r:02x}{g:02x}{b:02x}\">{nick}</span></b>")
    } else {
        format!("<b>{nick}</b>")
    };
    let excerpt = glib::markup_escape_text(&quote.excerpt);
    label.set_markup(
        &marked
            .replace('\u{E000}', &nick)
            .replace('\u{E001}', &excerpt),
    );
}

/// The contrast the nick must reach, as Rotulus asks of its text: more
/// when the desktop asks for more, through GTK 4.20's setting or, before
/// it, a high-contrast theme; and where libadwaita is in high contrast,
/// which before GTK 4.20 sets neither.
fn min_contrast() -> f64 {
    let Some(settings) = gtk::Settings::default() else {
        return 4.5;
    };
    let high = adw::StyleManager::default().is_high_contrast();
    let asked = (settings.find_property("gtk-interface-contrast")).is_some_and(|_| {
        let v = settings.property_value("gtk-interface-contrast");
        glib::EnumValue::from_value(&v).is_some_and(|(_, e)| e.nick() == "more")
    });
    let theme = (settings.gtk_theme_name())
        .is_some_and(|n| n.to_ascii_lowercase().contains("highcontrast"));
    if asked || theme || high {
        7.0
    } else {
        4.5
    }
}

fn hide(state: &Bar) -> bool {
    let was = state.to.take().is_some();
    if let Some(bar) = state.bar.upgrade() {
        bar.set_visible(false);
    }
    if let Some(input) = state.input.upgrade() {
        input.reset_property(gtk::AccessibleProperty::Description);
    }
    was
}

/// Let go of the reply `view`'s bar holds, if any.
pub(crate) fn cancel(view: &RotulusView) {
    if let Some(state) = of(view) {
        hide(&state);
    }
}

/// Escape in `input`: whether there was a reply to cancel.
pub(crate) fn escape(input: &impl IsA<gtk::Widget>) -> bool {
    of(input.upcast_ref::<gtk::Widget>()).is_some_and(|state| hide(&state))
}

/// The line the next chat line from `input` answers, if any, which the bar
/// then lets go of, as the view does the line a jump marked.
pub(crate) fn take(input: &impl IsA<gtk::Widget>) -> Option<Target> {
    let state = of(input.upcast_ref::<gtk::Widget>())?;
    let to = state.to.borrow_mut().take();
    hide(&state);
    if let Some(view) = state.view.upgrade() {
        crate::chat_output::unspot(&view);
    }
    to
}
