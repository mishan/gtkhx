//! The Identity settings page.
//!
//! Three groups: the display name, the numeric Hotline icon, and the custom
//! GIF avatar. Two of the five rows are the declarative helpers unchanged; the
//! rest is genuinely custom — an icon grid in a popup, two live previews, and
//! an async file chooser.
//!
//! The icon *resources* stay in C. They are Mac resource forks discovered at
//! startup into a global, and rather than mirror that across the boundary the
//! picker asks `icon_enum.h` for the set of IDs and resolves each through the
//! same `load_icon` every other icon consumer uses. See that file for why that
//! matters beyond convenience.

use crate::options::{cfg, entry_row, group, nick_color_row, spin_row, switch_row};
use crate::tr::{tr, tr1, tr_argv};
use gtk4 as gtk;
use gtk4::gdk_pixbuf;
use gtk4::glib;
use gtk4::prelude::*;
use libadwaita as adw;
use libadwaita::prelude::*;
use std::ffi::{c_int, c_void};

/// An icon this wide is a banner rather than a square icon, and the leading
/// `WIDE_CROP` pixels are chrome the picker shouldn't show.
///
/// Both numbers are inherited verbatim from the C picker, which offered no
/// derivation for either. They are *not* the user list's wide-icon constants
/// (`HX_USER_WIDE_ICON_THRESHOLD` / `_LEFT_PAD` in `users_cell.c`), which are
/// different values for a different purpose — don't unify them on the
/// assumption that they mean the same thing.
const WIDE_THRESHOLD: i32 = 400;
const WIDE_CROP: i32 = 198;

/// The grid's cell height, and the width of a square cell.
const THUMB_SIZE: i32 = 56;

/// How many icons to render per idle tick.
///
/// The C version pumped the main loop inline every ten icons and then
/// re-checked that the flowbox still existed, because the user can close the
/// popup mid-render. An idle callback gets the same responsiveness without the
/// reentrancy: the closure holds the grid weakly, so a closed popup simply
/// stops the walk.
const RENDER_CHUNK: usize = 10;

/// A GIF avatar's ceiling, matching `GTKHX_AVATAR_MAX_BYTES`. Checked here
/// only to fail early with a useful message — `hx_icon_save` enforces it
/// again, and that is the one that counts.
const AVATAR_MAX_BYTES: u64 = 32 * 1024;

extern "C" {
    // icon_enum.c — the ID snapshot and the by-ID render.
    fn hx_icon_ids_begin() -> c_int;
    fn hx_icon_ids_nth(i: c_int) -> c_int;
    fn hx_icon_ids_end();
    fn hx_icon_pixbuf_for_id(id: c_int, fallback: c_int) -> *mut gdk_pixbuf::ffi::GdkPixbuf;

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

// -------------------------------------------------------------- the icon --

fn icon_pixbuf(id: u16, fallback: bool) -> Option<gdk_pixbuf::Pixbuf> {
    let raw = unsafe { hx_icon_pixbuf_for_id(id as c_int, fallback as c_int) };
    if raw.is_null() {
        return None;
    }
    Some(unsafe { glib::translate::from_glib_full(raw) })
}

/// One picker thumbnail, plus whether the icon is a wide banner.
///
/// Nearest-neighbour, because these are 16- and 32-pixel pixel-art sources
/// being enlarged 3.5× and 1.75× — a smoothing filter turns them to mush.
/// Wide banners keep their aspect ratio and scale to the cell *height*, so
/// the art stays readable instead of being squashed into a square.
fn thumbnail(id: u16) -> Option<(gtk::gdk::Texture, bool)> {
    let pixbuf = icon_pixbuf(id, false)?;

    let wide = pixbuf.width() > WIDE_THRESHOLD;
    let pixbuf = if wide {
        pixbuf.new_subpixbuf(WIDE_CROP, 0, pixbuf.width() - WIDE_CROP, pixbuf.height())
    } else {
        pixbuf
    };

    let (w, h) = if wide {
        let scaled = pixbuf.width() * THUMB_SIZE / pixbuf.height().max(1);
        (scaled.max(1), THUMB_SIZE)
    } else {
        (THUMB_SIZE, THUMB_SIZE)
    };
    let scaled = pixbuf.scale_simple(w, h, gdk_pixbuf::InterpType::Nearest)?;
    Some((gtk::gdk::Texture::for_pixbuf(&scaled), wide))
}

/// The inline preview beside the ID field.
///
/// Uncropped and with the default-icon fallback on, unlike the picker: this
/// answers "what will other people see", and every display path in the client
/// substitutes the default icon for an unknown ID. A preview that blanked
/// instead would disagree with the user list it is previewing.
fn set_preview(picture: &gtk::Picture, id: u16) {
    let tex = icon_pixbuf(id, true).map(|pb| gtk::gdk::Texture::for_pixbuf(&pb));
    picture.set_paintable(tex.as_ref());
}

/// Fill the grid, a chunk per idle tick.
///
/// Weak on the flowbox throughout: closing the popup is the normal way out of
/// a long render, and it should stop the walk rather than be something the
/// walk has to notice.
fn fill_grid(narrow: &gtk::FlowBox, wide: &gtk::FlowBox, ids: Vec<u16>, selected: u16) {
    let narrow_weak = narrow.downgrade();
    let wide_weak = wide.downgrade();
    let mut next = 0usize;

    glib::idle_add_local(move || {
        let (Some(narrow), Some(wide)) = (narrow_weak.upgrade(), wide_weak.upgrade()) else {
            return glib::ControlFlow::Break;
        };
        let end = (next + RENDER_CHUNK).min(ids.len());
        for &id in &ids[next..end] {
            let Some((texture, is_wide)) = thumbnail(id) else {
                continue;
            };
            let cell = gtk::Box::new(gtk::Orientation::Vertical, 4);
            cell.set_margin_top(6);
            cell.set_margin_bottom(6);
            cell.set_margin_start(6);
            cell.set_margin_end(6);
            // GtkPicture with can_shrink off, not GtkImage: Adwaita's
            // stylesheet clamps GtkImage to icon-button dimensions whatever
            // the paintable's natural size, so the cell would grow while the
            // icon stayed tiny inside it. The texture is already scaled to
            // the exact size we want drawn, so pinning to natural size is
            // also what keeps the nearest-neighbour scaling intact.
            let image = gtk::Picture::for_paintable(&texture);
            image.set_can_shrink(false);
            let label = gtk::Label::new(Some(&id.to_string()));
            label.add_css_class("caption");
            cell.append(&image);
            cell.append(&label);

            let child = gtk::FlowBoxChild::new();
            child.set_child(Some(&cell));
            // The ID rides on the child so the activate handler doesn't need a
            // parallel table.
            unsafe { child.set_data("hx-icon-id", id) };

            let grid = if is_wide { &wide } else { &narrow };
            grid.append(&child);

            if id == selected {
                grid.select_child(&child);
                // Focus as well as selection: a focused child inside a
                // GtkScrolledWindow is scrolled into view, which is the only
                // way the user finds their current icon in a grid of
                // hundreds. Appending below a focused child doesn't move the
                // adjustment, so doing this mid-fill is safe.
                child.grab_focus();
            }
        }
        next = end;
        if next >= ids.len() {
            glib::ControlFlow::Break
        } else {
            glib::ControlFlow::Continue
        }
    });
}

/// One of the picker's two flowboxes. `wide` selects the one-per-row banner
/// strip over the multi-column square grid — banners are several times wider
/// than they are tall, and sharing a homogeneous grid with 56×56 icons would
/// shrink them to an unreadable smear.
fn picker_flowbox(wide: bool) -> gtk::FlowBox {
    let fb = gtk::FlowBox::new();
    fb.set_selection_mode(gtk::SelectionMode::Single);
    fb.set_homogeneous(!wide);
    fb.set_min_children_per_line(if wide { 1 } else { 2 });
    fb.set_max_children_per_line(if wide { 1 } else { 4 });
    fb.set_row_spacing(4);
    fb.set_column_spacing(4);
    fb.set_valign(gtk::Align::Start);
    fb
}

/// The Browse… popup: the whole icon grid in a scrolled dialog.
fn open_icon_picker(anchor: &gtk::Widget, row: &adw::SpinRow) -> adw::Dialog {
    let count = unsafe { hx_icon_ids_begin() };
    let mut ids = Vec::with_capacity(count.max(0) as usize);
    for i in 0..count {
        let id = unsafe { hx_icon_ids_nth(i) };
        if (0..=u16::MAX as c_int).contains(&id) {
            ids.push(id as u16);
        }
    }
    unsafe { hx_icon_ids_end() };

    let dialog = adw::Dialog::new();
    dialog.set_title(&tr("Choose Icon"));
    dialog.set_content_width(420);
    dialog.set_content_height(520);
    dialog.set_size_request(300, 360);
    unsafe { crate::ffi::gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut _) };

    let narrow = picker_flowbox(false);
    let wide = picker_flowbox(true);

    let row_weak = row.downgrade();
    let dialog_weak = dialog.downgrade();
    let activated = move |_: &gtk::FlowBox, child: &gtk::FlowBoxChild| {
        let id: u16 = match unsafe { child.data::<u16>("hx-icon-id") } {
            Some(p) => unsafe { *p.as_ref() },
            None => return,
        };
        if let Some(row) = row_weak.upgrade() {
            // Writing the spin row's value is what persists the choice — its
            // own notify::value handler is the single write path, so the
            // picker never touches the preference directly.
            row.set_value(id as f64);
        }
        if let Some(dialog) = dialog_weak.upgrade() {
            dialog.close();
        }
    };
    narrow.connect_child_activated(activated.clone());
    wide.connect_child_activated(activated);

    let picker_box = gtk::Box::new(gtk::Orientation::Vertical, 4);
    picker_box.set_margin_top(6);
    picker_box.set_margin_bottom(6);
    picker_box.set_margin_start(6);
    picker_box.set_margin_end(6);
    picker_box.append(&narrow);
    picker_box.append(&wide);

    let scroller = gtk::ScrolledWindow::new();
    scroller.set_vexpand(true);
    scroller.set_child(Some(&picker_box));

    let view = adw::ToolbarView::new();
    view.add_top_bar(&adw::HeaderBar::new());
    view.set_content(Some(&scroller));
    dialog.set_child(Some(&view));

    let selected = row.value().clamp(0.0, u16::MAX as f64) as u16;
    fill_grid(&narrow, &wide, ids, selected);

    dialog.present(Some(anchor));
    dialog
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

    let preview = gtk::Picture::new();
    preview.set_size_request(40, 40);
    preview.set_valign(gtk::Align::Center);
    preview.set_content_fit(gtk::ContentFit::Contain);
    set_preview(&preview, row.value().clamp(0.0, u16::MAX as f64) as u16);

    let preview_weak = preview.downgrade();
    row.connect_value_notify(move |row| {
        if let Some(preview) = preview_weak.upgrade() {
            set_preview(&preview, row.value().clamp(0.0, u16::MAX as f64) as u16);
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
        open_icon_picker(btn.upcast_ref(), &row).connect_closed(move |_| {
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
