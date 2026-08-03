//! The Identity settings page.
//!
//! Three groups: the display name, the numeric Hotline icon, and the custom
//! GIF avatar. Two of the five rows are the declarative helpers unchanged; the
//! rest is genuinely custom — an icon grid in a popup, two live previews, and
//! an async file chooser.
//!
//! The icon grid itself lives in [`crate::icon_picker`], shared with the
//! Connections page — this page sets the global default, that one sets a
//! per-connection override, and both want the same catalogue.

use crate::icon_picker;
use crate::options::{cfg, entry_row, group, nick_color_row, spin_row, switch_row};
use crate::tr::{tr, tr1, tr_argv};
use gtk4 as gtk;
use gtk4::gdk_pixbuf;
use gtk4::glib;
use gtk4::prelude::*;
use libadwaita as adw;
use libadwaita::prelude::*;
use std::ffi::{c_int, c_void};

/// A GIF avatar's ceiling, matching `GTKHX_AVATAR_MAX_BYTES`. Checked here
/// only to fail early with a useful message — `hx_icon_save` enforces it
/// again, and that is the one that counts.
const AVATAR_MAX_BYTES: u64 = 32 * 1024;

extern "C" {
    // gif_icons.c — avatar persistence and the wire send.
    fn hx_icon_save(gif: *const u8, len: usize) -> glib::ffi::gboolean;
    fn hx_icon_forget() -> glib::ffi::gboolean;
    fn hx_icon_load_saved() -> *mut glib::ffi::GBytes;
    fn hx_icon_set(htlc: *mut c_void, gif: *const u8, len: usize);
    fn hx_icon_clear(htlc: *mut c_void);

    // gtkhx_ui_bridge.c / gtkhx-core — the live connection, and whether the
    // server on it speaks the GIF-icons extension.
    fn gtkhx_active_htlc() -> *mut c_void;
    fn hx_conn_gif_icons_state(htlc: *mut c_void) -> c_int;

    // toolbar.c
    fn toolbar_show_toast(text: *const std::ffi::c_char);
}

/// `GIF_ICONS_SUPPORTED` from `gif_icons.h` — an `ICON_GETLIST`/`GET` reply
/// was seen, so the server understands the extension. The neighbouring values
/// are `UNKNOWN = 0` (not probed) and `UNSUPPORTED = 2` (probe timed out);
/// this must be compared for equality against `SUPPORTED` specifically, since
/// sending `ICON_SET` to a server in either other state means sending an
/// opcode it may reject.
const GIF_ICONS_SUPPORTED: c_int = 1;

fn toast(text: &str) {
    unsafe { toolbar_show_toast(crate::cs(text).as_ptr()) };
}

fn icon_group(page: &adw::PreferencesPage) {
    let grp = group(&tr("Identity icon"));
    let row = spin_row(
        cfg::ICON,
        &tr("Icon ID"),
        Some(&tr("Numeric ID from the loaded icon resource files")),
        0.0,
        65535.0,
        1.0,
    );

    let preview = icon_picker::preview_widget(40);
    icon_picker::set_preview(&preview, row.value().clamp(0.0, u16::MAX as f64) as u16);

    let preview_weak = preview.downgrade();
    row.connect_value_notify(move |row| {
        if let Some(preview) = preview_weak.upgrade() {
            icon_picker::set_preview(&preview, row.value().clamp(0.0, u16::MAX as f64) as u16);
        }
    });
    row.add_prefix(&preview);

    let browse = gtk::Button::with_label(&tr("Browse…"));
    browse.set_valign(gtk::Align::Center);
    let row_weak = row.downgrade();
    browse.connect_clicked(move |btn| {
        let Some(row) = row_weak.upgrade() else {
            return;
        };
        // One picker at a time. A second would orphan the first *and* start a
        // second full-catalogue render alongside it. The C guarded this by
        // re-presenting the existing dialog; making the button insensitive is
        // the same guarantee with the state living on the widget that has it.
        btn.set_sensitive(false);
        let btn_weak = btn.downgrade();
        let selected = row.value().clamp(0.0, u16::MAX as f64) as u16;
        // Writing the spin row's value is what persists the choice — its own
        // notify::value handler is the single write path, so the picker never
        // touches the preference directly.
        let picked_row = row.downgrade();
        let dialog = icon_picker::open(btn.upcast_ref(), selected, move |id| {
            if let Some(row) = picked_row.upgrade() {
                row.set_value(id as f64);
            }
        });
        dialog.connect_closed(move |_| {
            if let Some(btn) = btn_weak.upgrade() {
                btn.set_sensitive(true);
            }
        });
    });
    row.add_suffix(&browse);

    grp.add(&row);
    page.add(&grp);
}

// ------------------------------------------------------------ the avatar --

/// Decode a GIF into the preview, scaled down to something a row can hold.
fn avatar_preview(picture: &gtk::Picture, gif: &[u8]) {
    let loader = gdk_pixbuf::PixbufLoader::new();
    loader.connect_size_prepared(|loader, w, h| {
        // Clamp the decode itself rather than the result: a hostile or merely
        // enormous GIF should not be fully decoded just to be shrunk.
        let longest = w.max(h);
        if longest > 512 {
            let scale = 512.0 / longest as f64;
            // Floor to at least 1: an extreme aspect ratio rounds the short
            // side to zero, and a zero dimension aborts the decode.
            loader.set_size(
                ((w as f64 * scale) as i32).max(1),
                ((h as f64 * scale) as i32).max(1),
            );
        }
    });
    // close() must run exactly once however write() fared, so sequence the
    // two rather than short-circuiting.
    let wrote = loader.write(gif).is_ok();
    let closed = loader.close().is_ok();
    let tex = if wrote && closed {
        loader.pixbuf().map(|pb| gtk::gdk::Texture::for_pixbuf(&pb))
    } else {
        None
    };
    // Set unconditionally, including to nothing: a failed decode has to clear
    // the previous avatar, or the preview goes on showing an image that is no
    // longer what we saved.
    picture.set_paintable(tex.as_ref());
}

fn load_saved_avatar() -> Option<glib::Bytes> {
    let raw = unsafe { hx_icon_load_saved() };
    if raw.is_null() {
        return None;
    }
    Some(unsafe { glib::translate::from_glib_full(raw) })
}

/// Store a chosen avatar and, if the server understands the extension, send
/// it. Saving is unconditional: the picker is deliberately not gated on the
/// connection, so a choice made offline is sent on the next connect.
fn accept_avatar(preview: &gtk::Picture, gif: &[u8]) {
    if unsafe { hx_icon_save(gif.as_ptr(), gif.len()) } == 0 {
        // Size and GIF signature are both checked before we get here, so the
        // only way to fail now is the write itself.
        toast(&tr(
            "Couldn't save the avatar — check the permissions on your GtkHx \
             configuration directory.",
        ));
        return;
    }
    avatar_preview(preview, gif);

    let htlc = unsafe { gtkhx_active_htlc() };
    if !htlc.is_null() && unsafe { hx_conn_gif_icons_state(htlc) } == GIF_ICONS_SUPPORTED {
        unsafe { hx_icon_set(htlc, gif.as_ptr(), gif.len()) };
        toast(&tr("Avatar updated."));
    } else {
        toast(&tr(
            "Avatar saved — it'll be sent when you connect to a server that \
             supports GIF icons.",
        ));
    }
}

/// "That file is 91.4 KB — the limit is 32 KB."
///
/// Both sizes are in the message because "too big" alone doesn't tell anyone
/// how much smaller to go.
fn too_big(bytes: u64) -> String {
    tr_argv(
        "That file is %s KB — the limit is %s KB. Pick a smaller one.",
        &[
            &format!("{:.1}", bytes as f64 / 1024.0),
            &(AVATAR_MAX_BYTES / 1024).to_string(),
        ],
    )
}

fn choose_avatar(button: &gtk::Button, preview: &gtk::Picture) {
    let filter = gtk::FileFilter::new();
    filter.set_name(Some(&tr("GIF images")));
    filter.add_mime_type("image/gif");
    let filters = gtk::gio::ListStore::new::<gtk::FileFilter>();
    filters.append(&filter);

    let dialog = gtk::FileDialog::new();
    dialog.set_title(&tr("Choose an avatar"));
    dialog.set_filters(Some(&filters));

    let window = button.root().and_downcast::<gtk::Window>();
    // Strong, unlike every other capture on this page: the chooser is async
    // and there is no other owner keeping the preview alive across it. If the
    // Settings window closes first the widget is unparented but still valid,
    // so the late `set_paintable` is a harmless no-op rather than a use of
    // freed memory — which is exactly why the C reffed it here too.
    let preview = preview.clone();
    dialog.open(
        window.as_ref(),
        gtk::gio::Cancellable::NONE,
        move |result| {
            let file = match result {
                Ok(file) => file,
                Err(err) => {
                    // Dismissing the chooser is the ordinary way out and says
                    // nothing worth reporting. A portal that actually broke
                    // does — silently doing nothing looks like a client bug.
                    if !err.matches(gtk::DialogError::Dismissed) {
                        toast(&tr1("File picker failed: %s", err.message()));
                    }
                    return;
                }
            };
            // Size first, from the stat rather than the read: a GIF far over the
            // limit shouldn't be pulled into memory only to be rejected.
            if let Ok(info) = file.query_info(
                "standard::size",
                gtk::gio::FileQueryInfoFlags::NONE,
                gtk::gio::Cancellable::NONE,
            ) {
                if info.size() as u64 > AVATAR_MAX_BYTES {
                    // Checked ahead of the GIF signature, so a large non-GIF
                    // lands here too — keep the wording neutral.
                    toast(&too_big(info.size() as u64));
                    return;
                }
            }
            let bytes = match file.load_contents(gtk::gio::Cancellable::NONE) {
                Ok((bytes, _)) => bytes,
                Err(err) => {
                    toast(&tr1("Couldn't read the file: %s", err.message()));
                    return;
                }
            };
            if bytes.is_empty() {
                toast(&tr("That file is empty."));
                return;
            }
            if bytes.len() as u64 > AVATAR_MAX_BYTES {
                // The stat can lie, or be unavailable; the read is the truth.
                toast(&too_big(bytes.len() as u64));
                return;
            }
            if !hotline_proto::gif_icons::is_gif(&bytes) {
                toast(&tr("That file is not a GIF."));
                return;
            }
            accept_avatar(&preview, &bytes);
        },
    );
}

fn avatar_group(page: &adw::PreferencesPage) {
    let grp = group(&tr("Custom GIF avatar"));
    grp.set_description(Some(&tr(
        "A GIF other users see in place of your icon. Best authored at icon \
         size (or wide-banner size); max 32 KB. Sent automatically when you \
         connect to a server that supports it.",
    )));

    let row = adw::ActionRow::new();
    row.set_title(&tr("Avatar"));

    let preview = gtk::Picture::new();
    preview.set_size_request(48, 48);
    preview.set_valign(gtk::Align::Center);
    preview.set_content_fit(gtk::ContentFit::Contain);
    // Seeded from what was saved, not from the live per-session cache, so it
    // shows before connecting.
    if let Some(saved) = load_saved_avatar() {
        avatar_preview(&preview, &saved);
    }
    row.add_prefix(&preview);

    let choose = gtk::Button::with_label(&tr("Choose…"));
    choose.set_valign(gtk::Align::Center);
    let preview_weak = preview.downgrade();
    choose.connect_clicked(move |btn| {
        if let Some(preview) = preview_weak.upgrade() {
            choose_avatar(btn, &preview);
        }
    });

    let clear = gtk::Button::with_label(&tr("Clear"));
    clear.set_valign(gtk::Align::Center);
    let preview_weak = preview.downgrade();
    clear.connect_clicked(move |_| {
        let removed = unsafe { hx_icon_forget() } != 0;
        if let Some(preview) = preview_weak.upgrade() {
            preview.set_paintable(gtk::gdk::Texture::NONE);
        }
        let htlc = unsafe { gtkhx_active_htlc() };
        if !htlc.is_null() && unsafe { hx_conn_gif_icons_state(htlc) } == GIF_ICONS_SUPPORTED {
            unsafe { hx_icon_clear(htlc) };
        }
        // Don't claim a clear that didn't persist: a file that survived
        // deletion reloads and re-sends on the next start, which would make
        // the toast a lie the user only discovers a session later.
        toast(&if removed {
            tr("Avatar cleared.")
        } else {
            tr("Avatar cleared for now, but the saved file could not be deleted.")
        });
    });

    row.add_suffix(&choose);
    row.add_suffix(&clear);
    grp.add(&row);

    grp.add(&switch_row(
        cfg::ANIMATE_AVATARS,
        &tr("Animate GIF avatars"),
        Some(&tr(
            "Play animated avatars in the user list. Turn off to show a still \
             frame.",
        )),
    ));

    page.add(&grp);
}

pub(crate) fn build(page: &adw::PreferencesPage) {
    let name = group(&tr("Display name"));
    name.add(&entry_row(cfg::NICK, &tr("Your name")));
    name.add(&nick_color_row(cfg::NICK_COLOR));
    page.add(&name);

    icon_group(page);
    avatar_group(page);
}
