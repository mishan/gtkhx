//! The Hotline icon picker, and icon rendering for previews.
//!
//! Shared by the two places that choose an icon: Settings → Identity, which
//! sets the global default, and Settings → Connections, which sets a
//! per-connection override. It reports a pick through a callback rather than
//! writing anything, because those two callers persist to different places —
//! one to a preference, one to a bookmark.
//!
//! The icon *resources* stay in C. They are Mac resource forks discovered at
//! startup into a global, and rather than mirror that across the boundary the
//! picker asks `icon_enum.h` for the set of IDs and resolves each through the
//! same `load_icon` every other icon consumer uses. See that file for why that
//! matters beyond convenience.

use crate::tr::tr;
use gtk4 as gtk;
use gtk4::gdk_pixbuf;
use gtk4::glib;
use gtk4::prelude::*;
use libadwaita as adw;
use libadwaita::prelude::*;
use std::ffi::c_int;

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

extern "C" {
    // icon_enum.c — the ID snapshot and the by-ID render.
    fn hx_icon_ids_begin() -> c_int;
    fn hx_icon_ids_nth(i: c_int) -> c_int;
    fn hx_icon_ids_end();
    fn hx_icon_pixbuf_for_id(id: c_int, fallback: c_int) -> *mut gdk_pixbuf::ffi::GdkPixbuf;
}

fn icon_pixbuf(id: u16, fallback: bool) -> Option<gdk_pixbuf::Pixbuf> {
    let raw = unsafe { hx_icon_pixbuf_for_id(id as c_int, fallback as c_int) };
    if raw.is_null() {
        return None;
    }
    Some(unsafe { glib::translate::from_glib_full(raw) })
}

/// An icon as a preview texture: uncropped, with the default-icon fallback on.
///
/// This answers "what will other people see", and every display path in the
/// client substitutes the default icon for an unknown ID. A preview that
/// blanked instead would disagree with the user list it is previewing.
pub(crate) fn preview_texture(id: u16) -> Option<gtk::gdk::Texture> {
    icon_pixbuf(id, true).map(|pb| gtk::gdk::Texture::for_pixbuf(&pb))
}

/// Paint `id` into `picture`, clearing it if the icon can't be resolved.
pub(crate) fn set_preview(picture: &gtk::Picture, id: u16) {
    picture.set_paintable(preview_texture(id).as_ref());
}

/// A `GtkPicture` sized and configured the way both callers want their inline
/// preview: square, centred, and scaled to fit rather than cropped.
pub(crate) fn preview_widget(size: i32) -> gtk::Picture {
    let p = gtk::Picture::new();
    p.set_size_request(size, size);
    p.set_valign(gtk::Align::Center);
    p.set_content_fit(gtk::ContentFit::Contain);
    p
}

/// One picker thumbnail, plus whether the icon is a wide banner.
///
/// Nearest-neighbour, because these are 16- and 32-pixel pixel-art sources
/// being enlarged 3.5× and 1.75× — a smoothing filter turns them to mush.
/// Wide banners keep their aspect ratio and scale to the cell *height*, so
/// the art stays readable instead of being squashed into a square.
fn thumbnail(id: u16) -> Option<(gtk::gdk::Texture, bool)> {
    // No fallback here, unlike the preview: the picker is asking "is there an
    // icon with this ID", and a default-icon substitute would put a row of
    // identical placeholders in the grid.
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

/// Fill the grid, a chunk per idle tick.
///
/// Weak on the flowboxes throughout: closing the popup is the normal way out
/// of a long render, and it should stop the walk rather than be something the
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

/// The whole icon catalogue in a scrolled dialog, presented on `anchor`.
///
/// `selected` is pre-selected and scrolled to. `on_pick` fires with the chosen
/// ID and the dialog closes; it does not fire on cancel. The caller decides
/// what a pick *means* — the Identity page writes a preference, the
/// Connections page writes a bookmark override — which is why nothing is
/// persisted here.
///
/// The returned dialog is already presented; callers keep it only to hang a
/// `closed` handler on (re-enabling whatever button opened it).
pub(crate) fn open(
    anchor: &gtk::Widget,
    selected: u16,
    on_pick: impl Fn(u16) + 'static,
) -> adw::Dialog {
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

    let on_pick = std::rc::Rc::new(on_pick);
    let dialog_weak = dialog.downgrade();
    let activated = move |_: &gtk::FlowBox, child: &gtk::FlowBoxChild| {
        let id: u16 = match unsafe { child.data::<u16>("hx-icon-id") } {
            Some(p) => unsafe { *p.as_ref() },
            None => return,
        };
        on_pick(id);
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

    fill_grid(&narrow, &wide, ids, selected);

    dialog.present(Some(anchor));
    dialog
}
