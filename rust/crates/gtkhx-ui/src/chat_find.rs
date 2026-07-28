//! The chat find bar: Ctrl+F over a chat output view.
//!
//! Deliberately shaped like `news.rs`'s find bar — same `GtkSearchBar` +
//! `GtkSearchEntry` + count label + prev/next layout, same signals, same
//! "Esc closes and clears" behaviour — because two find bars in one app
//! that behave differently is a papercut. The difference is where the
//! matching happens: news searches a `GtkTextBuffer` from Rust, whereas
//! here the whole engine lives in `hxchat-layout` behind the
//! `hx_chat_view_search*` C API, so this file only drives it.
//!
//! Built unconditionally since C5. During the A/B this checked
//! `hx_chat_view_can_search` and skipped building the bar on xtext,
//! which could not search; there is one backend now and it can.

use gtk4 as gtk;
use gtk::gdk;
use gtk::glib;
use gtk::prelude::*;
use std::cell::Cell;
use std::rc::Rc;

use crate::tr::tr;

extern "C" {
    fn hx_chat_view_search(
        view: *mut gtk::ffi::GtkWidget,
        needle: *const std::ffi::c_char,
        case_sensitive: glib::ffi::gboolean,
        n_matches: *mut u32,
        current: *mut u32,
    );
    fn hx_chat_view_search_step(
        view: *mut gtk::ffi::GtkWidget,
        dir: std::ffi::c_int,
        n_matches: *mut u32,
        current: *mut u32,
    );
    fn hx_chat_view_search_clear(view: *mut gtk::ffi::GtkWidget);
}

struct FindCtx {
    view: gtk::Widget,
    entry: gtk::SearchEntry,
    count: gtk::Label,
    prev_btn: gtk::Button,
    next_btn: gtk::Button,
    /// Debounce source for the in-flight keystroke, if any.
    pending: Cell<Option<glib::SourceId>>,
}

/// How long to wait after a keystroke before searching.
///
/// A search is O(scrollback) — it walks every message, by design, so
/// that matches in never-laid-out rows are found. Running that on each
/// keystroke of a fast typist in a 20k-line buffer is the one way this
/// becomes noticeable, and a debounce is cheaper than a smarter index.
const DEBOUNCE_MS: u32 = 120;

impl FindCtx {
    fn cptr(&self) -> *mut gtk::ffi::GtkWidget {
        self.view.as_ptr()
    }

    /// Run the query now and update the readout.
    fn run(&self) {
        let needle = self.entry.text().to_string();
        let (mut n, mut cur) = (0u32, 0u32);
        let c = glib::GString::from(needle.as_str());
        unsafe {
            hx_chat_view_search(self.cptr(), c.as_ptr(), 0, &mut n, &mut cur);
        }
        self.update_readout(needle.is_empty(), n, cur);
    }

    fn step(&self, dir: i32) {
        // Flush a pending debounce first, or Enter-before-the-timer
        // steps through the *previous* query's results.
        self.flush();
        let (mut n, mut cur) = (0u32, 0u32);
        unsafe {
            hx_chat_view_search_step(self.cptr(), dir, &mut n, &mut cur);
        }
        self.update_readout(self.entry.text().is_empty(), n, cur);
    }

    fn update_readout(&self, empty_query: bool, n: u32, cur: u32) {
        if empty_query {
            self.count.set_text("");
        } else if n == 0 {
            self.count.set_text(&tr("No results"));
        } else {
            self.count.set_text(&format!("{cur} / {n}"));
        }
        // add/remove, not set_css_classes: the latter replaces the whole
        // list, which would drop whatever classes GtkSearchEntry itself
        // relies on for its default styling. We only own "error" here.
        self.set_error(!empty_query && n == 0);
        let has = n > 0;
        self.prev_btn.set_sensitive(has);
        self.next_btn.set_sensitive(has);
    }

    /// Toggle the "no results" styling without touching any other class.
    fn set_error(&self, on: bool) {
        if on {
            self.entry.add_css_class("error");
        } else {
            self.entry.remove_css_class("error");
        }
    }

    /// Run any pending debounced search immediately.
    fn flush(&self) {
        if let Some(id) = self.pending.take() {
            id.remove();
            self.run();
        }
    }

    fn schedule(self: &Rc<Self>) {
        if let Some(id) = self.pending.take() {
            id.remove();
        }
        let this = self.clone();
        let id = glib::timeout_add_local_once(
            std::time::Duration::from_millis(DEBOUNCE_MS as u64),
            move || {
                this.pending.set(None);
                this.run();
            },
        );
        self.pending.set(Some(id));
    }

    fn reset(&self) {
        if let Some(id) = self.pending.take() {
            id.remove();
        }
        unsafe { hx_chat_view_search_clear(self.cptr()) };
        self.count.set_text("");
        self.set_error(false);
        self.prev_btn.set_sensitive(false);
        self.next_btn.set_sensitive(false);
    }
}

/// Build a find bar for `view`.
///
/// `capture` is the subtree Ctrl+F and type-ahead are captured from —
/// normally the chat column, so typing in the message input is not
/// hijacked into the find entry.
pub fn build(view: &gtk::Widget, capture: &impl IsA<gtk::Widget>) -> gtk::SearchBar {
    let ctx = Rc::new(FindCtx {
        view: view.clone(),
        entry: gtk::SearchEntry::new(),
        count: gtk::Label::new(None),
        prev_btn: gtk::Button::from_icon_name("go-up-symbolic"),
        next_btn: gtk::Button::from_icon_name("go-down-symbolic"),
        pending: Cell::new(None),
    });

    ctx.entry.set_placeholder_text(Some(&tr("Find in chat")));
    ctx.entry.set_hexpand(true);
    ctx.count.add_css_class("dim-label");
    ctx.count.set_margin_start(6);
    ctx.count.set_margin_end(6);
    for (b, tip) in [
        (&ctx.prev_btn, tr("Previous match (Ctrl+Shift+G, Shift+F3)")),
        (&ctx.next_btn, tr("Next match (Ctrl+G, F3)")),
    ] {
        b.set_tooltip_text(Some(&tip));
        b.add_css_class("flat");
        b.set_sensitive(false);
    }

    let hbox = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    hbox.append(&ctx.entry);
    hbox.append(&ctx.count);
    hbox.append(&ctx.prev_btn);
    hbox.append(&ctx.next_btn);

    let bar = gtk::SearchBar::new();
    bar.set_child(Some(&hbox));
    bar.connect_entry(&ctx.entry);

    ctx.entry.connect_search_changed({
        let c = ctx.clone();
        move |_| c.schedule()
    });
    // Enter / Shift+Enter and the entry's own next/prev bindings all step.
    ctx.entry.connect_activate({
        let c = ctx.clone();
        move |_| c.step(1)
    });
    ctx.entry.connect_next_match({
        let c = ctx.clone();
        move |_| c.step(1)
    });
    ctx.entry.connect_previous_match({
        let c = ctx.clone();
        move |_| c.step(-1)
    });
    ctx.next_btn.connect_clicked({
        let c = ctx.clone();
        move |_| c.step(1)
    });
    ctx.prev_btn.connect_clicked({
        let c = ctx.clone();
        move |_| c.step(-1)
    });

    // Shift+Enter steps backwards. GtkSearchEntry emits `activate` for
    // plain Enter only, so the modifier case needs its own handler, and
    // it has to run before the entry's default binding.
    {
        let key = gtk::EventControllerKey::new();
        key.set_propagation_phase(gtk::PropagationPhase::Capture);
        let c = ctx.clone();
        key.connect_key_pressed(move |_, keyval, _, state| {
            let shift = state.contains(gdk::ModifierType::SHIFT_MASK);
            let enter = matches!(keyval, gdk::Key::Return | gdk::Key::KP_Enter);
            if shift && enter {
                c.step(-1);
                return glib::Propagation::Stop;
            }
            glib::Propagation::Proceed
        });
        ctx.entry.add_controller(key);
    }

    // Esc closes the bar; closing clears the query and the highlights so
    // reopening doesn't show stale bands over the chat.
    bar.connect_search_mode_enabled_notify({
        let c = ctx.clone();
        move |bar| {
            if !bar.is_search_mode() {
                c.reset();
                c.entry.set_text("");
            }
        }
    });

    // Keyboard: open, and step. Capture phase throughout, because the
    // message input holds focus and would otherwise see the keys first.
    {
        let sc = gtk::ShortcutController::new();
        sc.set_propagation_phase(gtk::PropagationPhase::Capture);

        // Ctrl+F opens and focuses — and, when the bar is *already* open
        // with a query and the entry focused, advances instead.
        //
        // That second case is the one people actually reach for: you hit
        // Ctrl+F, type, then keep hitting Ctrl+F. Gating it on the entry
        // already having focus is what keeps the ordinary "reopen and
        // retype" flow intact — coming from the chat or the message box,
        // Ctrl+F still selects the old query so typing replaces it.
        {
            let b = bar.clone();
            let c = ctx.clone();
            let action = gtk::CallbackAction::new(move |_, _| {
                let reopening = !b.is_search_mode();
                let advancing = !reopening
                    && c.entry.has_focus()
                    && !c.entry.text().is_empty();
                if advancing {
                    c.step(1);
                } else {
                    b.set_search_mode(true);
                    c.entry.grab_focus();
                    // grab_focus selects the whole query, so typing
                    // replaces it — the reason not to advance here.
                    c.entry.select_region(0, -1);
                }
                glib::Propagation::Stop
            });
            let trigger =
                gtk::KeyvalTrigger::new(gdk::Key::f, gdk::ModifierType::CONTROL_MASK);
            sc.add_shortcut(gtk::Shortcut::new(Some(trigger), Some(action)));
        }

        // The conventional next/prev accelerators, both families, since
        // which one is muscle memory depends entirely on where someone
        // came from: Ctrl+G / Ctrl+Shift+G (GNOME, macOS) and F3 /
        // Shift+F3 (Windows, most editors). They cost two shortcuts each
        // and save the user guessing.
        //
        // Unlike Ctrl+F these do nothing when the bar is closed: they are
        // "again", and there is no again without a first time.
        for (key, mods, dir) in [
            (gdk::Key::g, gdk::ModifierType::CONTROL_MASK, 1),
            (
                gdk::Key::G,
                gdk::ModifierType::CONTROL_MASK | gdk::ModifierType::SHIFT_MASK,
                -1,
            ),
            (gdk::Key::F3, gdk::ModifierType::empty(), 1),
            (gdk::Key::F3, gdk::ModifierType::SHIFT_MASK, -1),
        ] {
            let b = bar.clone();
            let c = ctx.clone();
            let action = gtk::CallbackAction::new(move |_, _| {
                if !b.is_search_mode() || c.entry.text().is_empty() {
                    return glib::Propagation::Proceed;
                }
                c.step(dir);
                glib::Propagation::Stop
            });
            let trigger = gtk::KeyvalTrigger::new(key, mods);
            sc.add_shortcut(gtk::Shortcut::new(Some(trigger), Some(action)));
        }

        capture.as_ref().add_controller(sc);
    }
    // Deliberately *not* set_key_capture_widget: news can do that
    // because its panel has no text input, but here a bare keystroke
    // belongs to the message box. The shortcuts above are the only way
    // in.

    bar
}
