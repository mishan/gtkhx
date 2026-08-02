//! The Voice settings page.
//!
//! Two audio-device combos and the push-to-talk group. The devices come from a
//! GStreamer `DeviceMonitor` scan in `hxvoice-runtime`; the key capture is
//! bespoke because the row's content (subtitle = the current bind) and its
//! interaction (click → capture dialog, Escape → cancel, a valid key → write
//! the canonical spec back) don't fit any of the declarative row helpers.
//!
//! The whole module is behind the crate's `voice` feature, matching the page
//! itself — the *preferences* are registered unconditionally so a build
//! without voice doesn't discard someone's saved picks, and only the UI that
//! edits them is gated.

use crate::options::{cfg, combo_row, group, pref_get_string, pref_set_string, switch_row};
use crate::tr::tr;
use gtk4 as gtk;
use gtk4::glib::translate::IntoGlib;
use gtk4::prelude::*;
use libadwaita as adw;
use libadwaita::prelude::*;
use std::ffi::{c_char, c_void, CStr};

extern "C" {
    // hxvoice-runtime — a gst::DeviceMonitor scan, as an opaque list.
    fn gtkhx_voice_list_input_devices() -> *mut c_void;
    fn gtkhx_voice_list_output_devices() -> *mut c_void;
    fn gtkhx_voice_device_list_len(list: *mut c_void) -> usize;
    fn gtkhx_voice_device_list_name(list: *mut c_void, i: usize) -> *const c_char;
    fn gtkhx_voice_device_list_display_name(list: *mut c_void, i: usize) -> *const c_char;
    fn gtkhx_voice_device_list_free(list: *mut c_void);

    // voice_ptt_keyspec.c — the bind vocabulary. Pure, and with its own unit
    // test, so it stays C.
    fn hx_voice_ptt_keyspec_parse(
        spec: *const c_char,
        out_keyval: *mut u32,
        out_state: *mut u32,
    ) -> gtk::glib::ffi::gboolean;
    fn hx_voice_ptt_keyspec_allowed(keyval: u32, state: u32) -> gtk::glib::ffi::gboolean;
    fn hx_voice_ptt_keyspec_canonicalize(keyval: u32, state: u32) -> *mut c_char;
    fn g_free(p: *mut c_void);
}

/// One device list, as `(value, label)` pairs with the "System default" entry
/// in front.
///
/// The value is a stable `gst::Device::name()`; the empty string means "follow
/// the desktop's audio configuration", which is what the runtime resolves
/// through `autoaudiosrc` / `autoaudiosink`.
///
/// Scanned once per page build and copied out immediately, so the opaque list
/// can be freed here rather than living as long as the page — the C version
/// stashed it on the page widget with a destroy notify because its combo
/// helper borrowed the pointers.
fn devices(list: *mut c_void) -> Vec<(String, String)> {
    let mut out = vec![(String::new(), tr("System default"))];
    if list.is_null() {
        return out;
    }
    unsafe {
        for i in 0..gtkhx_voice_device_list_len(list) {
            let name = gtkhx_voice_device_list_name(list, i);
            let display = gtkhx_voice_device_list_display_name(list, i);
            if name.is_null() {
                continue;
            }
            let name = CStr::from_ptr(name).to_string_lossy().into_owned();
            let display = if display.is_null() {
                name.clone()
            } else {
                CStr::from_ptr(display).to_string_lossy().into_owned()
            };
            out.push((name, display));
        }
        gtkhx_voice_device_list_free(list);
    }
    out
}

fn device_group(page: &adw::PreferencesPage) {
    let grp = group(&tr("Audio Devices"));
    grp.set_description(Some(&tr(
        "Capture and playback devices for voice chat. \"System default\" \
         follows your desktop's audio configuration. Changes take effect the \
         next time you join a voice room.",
    )));

    for (key, title, list) in [
        (cfg::VOICE_INPUT_DEVICE, tr("Input (microphone)"), unsafe {
            gtkhx_voice_list_input_devices()
        }),
        (cfg::VOICE_OUTPUT_DEVICE, tr("Output (speakers)"), unsafe {
            gtkhx_voice_list_output_devices()
        }),
    ] {
        let pairs = devices(list);
        let values: Vec<&str> = pairs.iter().map(|(v, _)| v.as_str()).collect();
        let labels: Vec<&str> = pairs.iter().map(|(_, l)| l.as_str()).collect();
        grp.add(&combo_row(key, &title, &values, &labels));
    }

    page.add(&grp);
}

// ------------------------------------------------------------ push-to-talk --

/// The row's subtitle: the current bind, or an invitation to set one.
///
/// Read back through the parser rather than printed raw, so a value that a
/// hand-edited settings file left unparseable reads as "not set" instead of as
/// a binding that doesn't work.
fn refresh_subtitle(row: &adw::ActionRow) {
    let spec = pref_get_string(cfg::VOICE_PTT_KEY);
    let valid = !spec.is_empty() && {
        let c = crate::cs(&spec);
        unsafe {
            hx_voice_ptt_keyspec_parse(c.as_ptr(), std::ptr::null_mut(), std::ptr::null_mut()) != 0
        }
    };
    if valid {
        row.set_subtitle(&spec);
    } else {
        row.set_subtitle(&tr("Not set — click to capture"));
    }
}

/// The capture dialog: an `AdwAlertDialog` with no buttons. A key or Escape is
/// the only way out, which is the whole interaction.
fn open_capture_dialog(row: &adw::ActionRow) {
    let dlg = adw::AlertDialog::new(
        Some(&tr("Capture push-to-talk key")),
        Some(&tr(
            "Press the key (or modifier+key combination) you want to use as \
             your push-to-talk binding.\n\n\
             Accepted: F1–F24, Pause, Scroll Lock, Insert, Print, Menu, or any \
             Ctrl/Alt/Super combination with another key. Plain letters and \
             digits are rejected so they don't conflict with chat input.\n\n\
             Press Escape to cancel.",
        )),
    );

    // Hidden rather than empty, so the body doesn't reserve a blank row under
    // the prompt before anything has been rejected. AdwAlertDialog doesn't
    // expose its body label, hence the extra-child slot.
    let err = gtk::Label::new(None);
    err.add_css_class("error");
    err.set_wrap(true);
    err.set_xalign(0.0);
    err.set_visible(false);
    dlg.set_extra_child(Some(&err));

    let key = gtk::EventControllerKey::new();
    // Capture phase: the dialog has to see every key before anything else
    // does, or a binding attempt fires an app shortcut instead.
    key.set_propagation_phase(gtk::PropagationPhase::Capture);

    let dlg_weak = dlg.downgrade();
    let row_weak = row.downgrade();
    let err_weak = err.downgrade();
    key.connect_key_pressed(move |_, keyval, _, state| {
        let Some(dlg) = dlg_weak.upgrade() else {
            return gtk::glib::Propagation::Proceed;
        };

        // Escape cancels, unless it's held with Ctrl/Alt/Super — those are
        // legitimate binds and have to reach the capture below.
        //
        // Deliberately only those three: they are exactly the modifiers
        // `hx_voice_ptt_keyspec_allowed` accepts as a bind prefix. Shift,
        // Lock and the NumLock-style mode bits therefore fall through to the
        // cancel, which is right — Shift+Escape can't be bound, so treating
        // it as anything but Escape would leave the dialog with no way out.
        let modified = state.intersects(
            gtk::gdk::ModifierType::CONTROL_MASK
                | gtk::gdk::ModifierType::ALT_MASK
                | gtk::gdk::ModifierType::SUPER_MASK,
        );
        if keyval == gtk::gdk::Key::Escape && !modified {
            dlg.close();
            return gtk::glib::Propagation::Stop;
        }

        let raw_state = state.bits();
        if unsafe { hx_voice_ptt_keyspec_allowed(keyval.into_glib(), raw_state) } == 0 {
            if let Some(err) = err_weak.upgrade() {
                // Lists the same three modifiers the dialog body does, which
                // are the three `hx_voice_ptt_keyspec_allowed` accepts —
                // naming a narrower set here sends people hunting for a
                // combination that would in fact have worked.
                err.set_text(&tr(
                    "That key would conflict with chat typing. Try a function \
                     key (F1–F24), Pause, or a Ctrl/Alt/Super-modified \
                     combination.",
                ));
                err.set_visible(true);
            }
            // Consumed anyway: a rejected key must not reach the app either.
            return gtk::glib::Propagation::Stop;
        }

        let spec = unsafe { hx_voice_ptt_keyspec_canonicalize(keyval.into_glib(), raw_state) };
        if spec.is_null() {
            return gtk::glib::Propagation::Stop;
        }
        let owned = unsafe { CStr::from_ptr(spec) }
            .to_string_lossy()
            .into_owned();
        unsafe { g_free(spec as *mut c_void) };

        pref_set_string(cfg::VOICE_PTT_KEY, &owned);
        if let Some(row) = row_weak.upgrade() {
            refresh_subtitle(&row);
        }
        dlg.close();
        gtk::glib::Propagation::Stop
    });
    dlg.add_controller(key);

    dlg.present(Some(row));
}

fn ptt_group(page: &adw::PreferencesPage) {
    let grp = group(&tr("Push-to-Talk"));
    grp.set_description(Some(&tr(
        "When enabled, you start muted and unmute by holding the captured \
         key. Works from any focused widget in the GtkHx window.",
    )));

    grp.add(&switch_row(
        cfg::VOICE_PTT_ENABLED,
        &tr("Enable push-to-talk"),
        None,
    ));

    let key_row = adw::ActionRow::new();
    key_row.set_title(&tr("PTT key"));
    // AdwActionRow inherits GtkListBoxRow; this is what makes click and Enter
    // both emit "activated".
    key_row.set_activatable(true);
    key_row.connect_activated(open_capture_dialog);

    let clear = gtk::Button::from_icon_name("edit-clear-symbolic");
    clear.set_valign(gtk::Align::Center);
    clear.set_tooltip_text(Some(&tr("Clear PTT key")));
    clear.add_css_class("flat");
    let row_weak = key_row.downgrade();
    clear.connect_clicked(move |_| {
        pref_set_string(cfg::VOICE_PTT_KEY, "");
        if let Some(row) = row_weak.upgrade() {
            refresh_subtitle(&row);
        }
    });
    key_row.add_suffix(&clear);

    refresh_subtitle(&key_row);
    grp.add(&key_row);
    page.add(&grp);
}

pub(crate) fn build(page: &adw::PreferencesPage) {
    device_group(page);
    ptt_group(page);
}
