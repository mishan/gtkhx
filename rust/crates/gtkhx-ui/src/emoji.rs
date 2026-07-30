//! Emoji picker button + inline `:shortcode:` typeahead (ported from
//! `src/emoji.c`).
//!
//! Two pieces of GTK wiring shared by every chat-style GtkTextView input
//! (public chat, private chat, private message):
//!
//!   * `hx_emoji_button_new` — a flat GtkMenuButton wrapping GTK's built-in
//!     GtkEmojiChooser; inserts the picked glyph at the input's cursor.
//!   * `hx_emoji_typeahead_attach` / `_detach` — a keyboard-driven
//!     `:prefix` popover of matching shortcodes (Up/Down select,
//!     Tab/Enter commit, Esc dismiss). The match list comes from
//!     `hotline_proto::emoji::shortcode_matches` (native Rust); this module
//!     owns only the GTK wiring.
//!
//! The three C ABI entry points are preserved so chat.c / msg.c (and the
//! sibling Rust modules msg.rs / pchat.rs, which extern them) link
//! unchanged.

use std::cell::{Cell, RefCell};
use std::ffi::c_char;
use std::os::raw::c_int;

use glib::translate::{from_glib_borrow, IntoGlibPtr};
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;

use crate::tr::tr;

// Typeahead tuning (mirror emoji.c).
const TA_MIN_PREFIX: i32 = 2;
const TA_MAX_MATCHES: usize = 8;
const TA_MAX_PREFIX_SCAN: i32 = 128;

/// qdata key on the view carrying the boxed `EmojiTypeahead` (raw C qdata so
/// attach/detach can steal it, matching the C).
const TA_KEY: &[u8] = b"hx-emoji-typeahead\0";
/// qdata key on each suggestion row carrying its emoji glyph.
const ROW_EMOJI_KEY: &str = "hx-emoji";

extern "C" {
    /// options.c — the live emoji-typeahead pref (read fresh so a Settings
    /// flip takes effect on open windows).
    fn gtkhx_prefs_get_bool(name: *const c_char) -> c_int;
}

/// Return a freshly-built widget to C with a floating reference (matching a
/// GTK C constructor; the caller's append/set_parent sinks it).
unsafe fn into_floating_ptr<W: IsA<gtk::Widget>>(w: W) -> *mut gtk::ffi::GtkWidget {
    let ptr = w.upcast::<gtk::Widget>().into_glib_ptr();
    glib::gobject_ffi::g_object_force_floating(ptr as *mut glib::gobject_ffi::GObject);
    ptr
}

// ---------------------------------------------------------------------
// Picker button.
// ---------------------------------------------------------------------

fn on_emoji_picked(target: &gtk::TextView, text: &str) {
    if text.is_empty() {
        return;
    }
    target.buffer().insert_at_cursor(text);
    // Re-focus the input so Return-to-send keeps working after a pick.
    target.grab_focus();
}

/// `GtkWidget *hx_emoji_button_new(GtkWidget *target_text_view)`.
///
/// # Safety
/// `target_text_view` is a valid `GtkWidget *`; main thread only. Returns a
/// floating ref (caller packs it).
#[no_mangle]
pub unsafe extern "C" fn hx_emoji_button_new(
    target_text_view: *mut gtk::ffi::GtkWidget,
) -> *mut gtk::ffi::GtkWidget {
    crate::ensure_gtk_init();
    if target_text_view.is_null() {
        return std::ptr::null_mut();
    }
    // Borrow, don't own: `from_glib_none` would sink a floating reference, so a
    // caller passing an unparented view would have it finalized when this
    // wrapper drops. We only inspect it here (retaining via `view.clone()` where
    // needed), so a non-owning borrow is both correct and floating-safe.
    let widget = from_glib_borrow::<_, gtk::Widget>(target_text_view);
    let Some(view) = widget.downcast_ref::<gtk::TextView>() else {
        return std::ptr::null_mut();
    };

    let button = gtk::MenuButton::new();
    button.set_icon_name("face-smile-symbolic");
    button.add_css_class("flat");
    button.set_tooltip_text(Some(&tr("Insert emoji")));

    let chooser = gtk::EmojiChooser::new();
    {
        let view = view.clone();
        chooser.connect_emoji_picked(move |_chooser, text| on_emoji_picked(&view, text));
    }
    button.set_popover(Some(&chooser));

    into_floating_ptr(button)
}

// ---------------------------------------------------------------------
// Typeahead.
// ---------------------------------------------------------------------

struct EmojiTypeahead {
    view: gtk::TextView,
    /// Option so detach can `unparent` + drop it before the view disposes.
    popover: RefCell<Option<gtk::Popover>>,
    listbox: gtk::ListBox,
    open: Cell<bool>,
    /// Guards re-entrancy while we edit the buffer in commit.
    suppress: Cell<bool>,
    /// Char offset of the ':' that opened the current token.
    tok_start_off: Cell<i32>,
    buffer: gtk::TextBuffer,
    changed_id: RefCell<Option<glib::SignalHandlerId>>,
    cursor_id: RefCell<Option<glib::SignalHandlerId>>,
    key_ctrl: RefCell<Option<gtk::EventControllerKey>>,
}

fn ta_is_name_char(c: char) -> bool {
    (c as u32) < 128
        && (c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '+' || c == '-')
}

/// If the cursor sits inside an open `:prefix` token, return the colon's char
/// offset + the prefix text.
fn ta_current_token(view: &gtk::TextView) -> Option<(i32, String)> {
    let buf = view.buffer();
    let cur = buf.iter_at_mark(&buf.get_insert());
    let mut it = cur;

    let mut steps = 0i32;
    while it.backward_char() {
        let c = it.char();
        if ta_is_name_char(c) {
            steps += 1;
            if steps > TA_MAX_PREFIX_SCAN {
                return None; // runaway — not a shortcode
            }
            continue;
        }
        if c == ':' {
            let mut before = it;
            let at_start = !before.backward_char();
            let ok_prev = at_start || before.char().is_whitespace();
            if !ok_prev || steps < TA_MIN_PREFIX {
                return None;
            }
            let mut pstart = it;
            pstart.forward_char(); // skip the colon
            let prefix = buf.text(&pstart, &cur, false).to_string();
            return Some((it.offset(), prefix));
        }
        return None; // some other char ends the run with no opener
    }
    None
}

fn ta_hide(ta: &EmojiTypeahead) {
    if ta.open.get() {
        ta.open.set(false);
        if let Some(pop) = ta.popover.borrow().as_ref() {
            pop.popdown();
        }
    }
}

fn ta_clear_rows(ta: &EmojiTypeahead) {
    while let Some(child) = ta.listbox.first_child() {
        ta.listbox.remove(&child);
    }
}

/// Build a selectable row per (shortcode name, emoji glyph) match.
fn ta_populate(ta: &EmojiTypeahead, matches: &[(&str, &str)]) {
    ta_clear_rows(ta);

    for &(name, emoji) in matches {
        let row = gtk::ListBoxRow::new();
        let hbox = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        hbox.set_margin_start(6);
        hbox.set_margin_end(6);
        hbox.set_margin_top(2);
        hbox.set_margin_bottom(2);
        hbox.append(&gtk::Label::new(Some(emoji)));
        let nlab = gtk::Label::new(Some(&format!(":{}:", name)));
        nlab.set_halign(gtk::Align::Start);
        hbox.append(&nlab);
        row.set_child(Some(&hbox));

        // The row owns the emoji glyph; commit reads it back.
        unsafe { row.set_data(ROW_EMOJI_KEY, emoji.to_string()) };
        ta.listbox.append(&row);
    }

    if let Some(first) = ta.listbox.row_at_index(0) {
        ta.listbox.select_row(Some(&first));
    }
}

/// Anchor the popover at the caret.
fn ta_position(ta: &EmojiTypeahead) {
    let buf = ta.view.buffer();
    let cur = buf.iter_at_mark(&buf.get_insert());
    let loc = ta.view.iter_location(&cur);
    let (wx, wy) = ta
        .view
        .buffer_to_window_coords(gtk::TextWindowType::Widget, loc.x(), loc.y());
    let r = gtk::gdk::Rectangle::new(wx, wy, 1, if loc.height() > 0 { loc.height() } else { 1 });
    if let Some(pop) = ta.popover.borrow().as_ref() {
        pop.set_pointing_to(Some(&r));
    }
}

/// Recompute the popup against the current cursor context.
fn ta_update(ta: &EmojiTypeahead) {
    if ta.suppress.get() {
        return;
    }
    // Read the pref live so a Settings flip takes effect immediately.
    if unsafe { gtkhx_prefs_get_bool(crate::cs("EMOJITYPEAHEAD").as_ptr()) } == 0 {
        ta_hide(ta);
        return;
    }

    let Some((colon_off, prefix)) = ta_current_token(&ta.view) else {
        ta_hide(ta);
        return;
    };

    let matches = hotline_proto::emoji::shortcode_matches(&prefix, TA_MAX_MATCHES);
    if matches.is_empty() {
        ta_hide(ta);
        return;
    }

    ta.tok_start_off.set(colon_off);
    ta_populate(ta, &matches);
    ta_position(ta);
    if !ta.open.get() {
        ta.open.set(true);
        if let Some(pop) = ta.popover.borrow().as_ref() {
            pop.popup();
        }
    }
}

/// Replace the `:prefix` token with the selected emoji glyph.
fn ta_commit(ta: &EmojiTypeahead) {
    let emoji = ta.listbox.selected_row().and_then(|row| unsafe {
        row.data::<String>(ROW_EMOJI_KEY)
            .map(|p| p.as_ref().clone())
    });
    let Some(emoji) = emoji else {
        ta_hide(ta);
        return;
    };

    let buf = ta.view.buffer();
    let mut start = buf.iter_at_offset(ta.tok_start_off.get());
    let mut cur = buf.iter_at_mark(&buf.get_insert());

    ta.suppress.set(true);
    buf.begin_user_action();
    buf.delete(&mut start, &mut cur); // drop ":prefix"
    buf.insert(&mut start, &emoji); // insert the glyph
    buf.end_user_action();
    ta.suppress.set(false);

    ta_hide(ta);
    ta.view.grab_focus();
}

fn ta_move(ta: &EmojiTypeahead, delta: i32) {
    let idx = ta.listbox.selected_row().map(|r| r.index()).unwrap_or(-1);
    if let Some(next) = ta.listbox.row_at_index(idx + delta) {
        ta.listbox.select_row(Some(&next));
    }
}

/// Fallback free (qdata destroy-notify): drop the box if the view finalizes
/// without an explicit detach (the never-disposed public-chat input).
unsafe extern "C" fn ta_free_notify(data: glib::ffi::gpointer) {
    drop(Box::from_raw(data as *mut EmojiTypeahead));
}

/// `void hx_emoji_typeahead_attach(GtkWidget *target_text_view)`.
///
/// # Safety
/// `target_text_view` is a valid `GtkWidget *`; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_emoji_typeahead_attach(target_text_view: *mut gtk::ffi::GtkWidget) {
    crate::ensure_gtk_init();
    if target_text_view.is_null() {
        return;
    }
    // Borrow, don't own: `from_glib_none` would sink a floating reference, so a
    // caller passing an unparented view would have it finalized when this
    // wrapper drops. We only inspect it here (retaining via `view.clone()` where
    // needed), so a non-owning borrow is both correct and floating-safe.
    let widget = from_glib_borrow::<_, gtk::Widget>(target_text_view);
    let Some(view) = widget.downcast_ref::<gtk::TextView>() else {
        return;
    };

    // Idempotent: a second attach would leak the first's live handlers.
    let key = TA_KEY.as_ptr() as *const c_char;
    if !glib::gobject_ffi::g_object_get_data(view.as_ptr() as *mut _, key).is_null() {
        return;
    }

    let popover = gtk::Popover::new();
    popover.set_parent(view);
    // Don't steal focus — the user keeps typing; we drive selection ourselves.
    popover.set_autohide(false);
    popover.set_has_arrow(false);
    popover.set_position(gtk::PositionType::Bottom);

    let scroller = gtk::ScrolledWindow::new();
    scroller.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scroller.set_max_content_height(220);
    scroller.set_propagate_natural_height(true);

    let listbox = gtk::ListBox::new();
    listbox.set_selection_mode(gtk::SelectionMode::Browse);
    listbox.add_css_class("menu");
    scroller.set_child(Some(&listbox));
    popover.set_child(Some(&scroller));

    let buffer = view.buffer();

    let ptr = Box::into_raw(Box::new(EmojiTypeahead {
        view: view.clone(),
        popover: RefCell::new(Some(popover.clone())),
        listbox: listbox.clone(),
        open: Cell::new(false),
        suppress: Cell::new(false),
        tok_start_off: Cell::new(0),
        buffer: buffer.clone(),
        changed_id: RefCell::new(None),
        cursor_id: RefCell::new(None),
        key_ctrl: RefCell::new(None),
    }));

    // Signal closures deref the (box-stable) raw pointer; detach disconnects
    // them all before dropping the box, so none fire against freed state.
    listbox.connect_row_activated(move |lb, row| {
        let ta = unsafe { &*ptr };
        lb.select_row(Some(row));
        ta_commit(ta);
    });
    let changed_id = buffer.connect_changed(move |_| ta_update(unsafe { &*ptr }));
    let cursor_id = buffer.connect_notify_local(Some("cursor-position"), move |_, _| {
        ta_update(unsafe { &*ptr })
    });
    (*ptr).changed_id.replace(Some(changed_id));
    (*ptr).cursor_id.replace(Some(cursor_id));

    // Capture phase so nav/commit/dismiss keys are seen before the chat
    // input's bubble-phase handler — but only consumed while open.
    let kc = gtk::EventControllerKey::new();
    kc.set_propagation_phase(gtk::PropagationPhase::Capture);
    kc.connect_key_pressed(move |_, keyval, _, _| {
        let ta = unsafe { &*ptr };
        if !ta.open.get() {
            return glib::Propagation::Proceed; // dormant — let the input handle it
        }
        match keyval {
            gtk::gdk::Key::Down => {
                ta_move(ta, 1);
                glib::Propagation::Stop
            }
            gtk::gdk::Key::Up => {
                ta_move(ta, -1);
                glib::Propagation::Stop
            }
            gtk::gdk::Key::Tab | gtk::gdk::Key::Return | gtk::gdk::Key::KP_Enter => {
                ta_commit(ta);
                glib::Propagation::Stop
            }
            gtk::gdk::Key::Escape => {
                ta_hide(ta);
                glib::Propagation::Stop
            }
            _ => glib::Propagation::Proceed,
        }
    });
    view.add_controller(kc.clone());
    (*ptr).key_ctrl.replace(Some(kc));

    // The view owns the state via qdata; ta_free_notify reclaims it at
    // finalize (fallback for the never-detached public-chat input).
    glib::gobject_ffi::g_object_set_data_full(
        view.as_ptr() as *mut _,
        key,
        ptr as glib::ffi::gpointer,
        Some(ta_free_notify),
    );
}

/// `void hx_emoji_typeahead_detach(GtkWidget *target_text_view)` — unwire +
/// unparent the typeahead BEFORE the view disposes (required for runtime-
/// destroyed inputs: the parented popover would otherwise spin GtkTextView's
/// dispose loop forever). Idempotent; no-op if never attached.
///
/// # Safety
/// `target_text_view` is NULL or a valid `GtkWidget *`; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_emoji_typeahead_detach(target_text_view: *mut gtk::ffi::GtkWidget) {
    if target_text_view.is_null() {
        return;
    }
    // Borrow, don't own: `from_glib_none` would sink a floating reference, so a
    // caller passing an unparented view would have it finalized when this
    // wrapper drops. We only inspect it here (retaining via `view.clone()` where
    // needed), so a non-owning borrow is both correct and floating-safe.
    let widget = from_glib_borrow::<_, gtk::Widget>(target_text_view);
    let Some(view) = widget.downcast_ref::<gtk::TextView>() else {
        return;
    };

    // Steal (not get): take over teardown from the destroy-notify so it
    // doesn't also free the struct.
    let key = TA_KEY.as_ptr() as *const c_char;
    let ptr =
        glib::gobject_ffi::g_object_steal_data(view.as_ptr() as *mut _, key) as *mut EmojiTypeahead;
    if ptr.is_null() {
        return;
    }
    let ta = Box::from_raw(ptr);

    if let Some(id) = ta.changed_id.borrow_mut().take() {
        ta.buffer.disconnect(id);
    }
    if let Some(id) = ta.cursor_id.borrow_mut().take() {
        ta.buffer.disconnect(id);
    }
    if let Some(kc) = ta.key_ctrl.borrow_mut().take() {
        view.remove_controller(&kc);
    }
    if let Some(pop) = ta.popover.borrow_mut().take() {
        // Two refs at this point: the parent's, and the strong ref we just
        // took out of `ta.popover` into `pop`. unparent() drops the parent's
        // (while the view is still alive, pre-dispose); `pop` dropping at the
        // end of this block releases the last one and frees the popover.
        pop.unparent();
    }
    drop(ta); // frees the box + releases its widget refs
}
