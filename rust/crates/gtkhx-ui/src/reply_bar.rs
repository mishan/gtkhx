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

use std::cell::RefCell;
use std::rc::Rc;

use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::replies::Quote;
use hxproto::target::Target;
use rotulus::RotulusView;

use crate::tr::{tr, tr_argv};

/// What a bar holds, kept on its view and on its input alike.
struct Bar {
    bar: glib::WeakRef<gtk::Box>,
    label: glib::WeakRef<gtk::Label>,
    input: glib::WeakRef<gtk::Widget>,
    view: glib::WeakRef<RotulusView>,
    /// The line the next one said answers.
    to: RefCell<Option<Target>>,
}

const KEY: &str = "gtkhx-reply-bar";

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
    // The input is C's, and can outlive the bar it was described by.
    let weak = Rc::downgrade(&state);
    bar.connect_destroy(move |_| {
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
    let (Some(bar), Some(label), Some(input)) = (
        state.bar.upgrade(),
        state.label.upgrade(),
        state.input.upgrade(),
    ) else {
        return;
    };
    *state.to.borrow_mut() = Some(target);
    let say = |nick: &str, excerpt: &str| {
        // TRANSLATORS: over the chat input while a reply is being written:
        // who said the line it answers (%1$s), and what (%2$s).
        tr_argv("Replying to %1$s: %2$s", &[nick, excerpt])
    };
    // Escaped whole, so that a translation holding markup's own characters
    // is shown as written; the nick then goes in bold.
    let marked = glib::markup_escape_text(&say("\u{E000}", "\u{E001}"));
    let nick = format!("<b>{}</b>", glib::markup_escape_text(&quote.nick));
    let excerpt = glib::markup_escape_text(&quote.excerpt);
    label.set_markup(
        &marked
            .replace('\u{E000}', &nick)
            .replace('\u{E001}', &excerpt),
    );
    let plain = say(&quote.nick, &quote.excerpt);
    input.update_property(&[gtk::accessible::Property::Description(&plain)]);
    bar.set_visible(true);
    input.grab_focus();
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
