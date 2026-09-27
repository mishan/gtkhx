//! Drag and drop between the panels, and out of a local one.
//!
//! A drag carries the source panel and the entries selected when it started.
//! Dropped on the other panel it copies, or moves when both panels are
//! remote — a drag within one server is a move, as file managers treat a drag
//! within one volume. Dropped on its own panel it does nothing.
//!
//! A local drag also offers the files themselves, so other applications can
//! take them. A remote one doesn't: the file isn't here yet, and promising it
//! needs the file transfer portal. Starting a download when the drag started
//! was tried and dropped — it downloaded every file picked up, drop or not.

use std::rc::Rc;

use gtk::gdk;
use gtk::gio;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::files_entry::HxFileEntry;

use super::browser::{Browser, Side};
use super::panel::join;

/// A drag between panels: where it came from and what it carries.
#[derive(Clone, glib::Boxed)]
#[boxed_type(name = "HxFilesDrag")]
struct FilesDrag {
    source: glib::WeakRef<gtk::ColumnView>,
    entries: Vec<HxFileEntry>,
}

pub fn attach(br: &Rc<Browser>, side: Side) {
    let cv = br.panel(side).column_view().clone();

    // Copy only: a move isn't offered to other applications, and a link maps
    // onto neither side.
    let source = gtk::DragSource::new();
    source.set_actions(gdk::DragAction::COPY);
    let weak = Rc::downgrade(br);
    source.connect_prepare(move |_, _, _| {
        let br = weak.upgrade()?;
        prepare(&br, side)
    });
    cv.add_controller(source);

    // The drop target highlights the view while a drag it takes hovers.
    let target = gtk::DropTarget::new(FilesDrag::static_type(), gdk::DragAction::COPY);
    let weak = Rc::downgrade(br);
    target.connect_drop(move |_, value, _, _| {
        let (Some(br), Ok(drag)) = (weak.upgrade(), value.get::<FilesDrag>()) else {
            return false;
        };
        drop_on(&br, side, drag)
    });
    cv.add_controller(target);
}

fn prepare(br: &Browser, side: Side) -> Option<gdk::ContentProvider> {
    let panel = br.panel(side);
    let entries = panel.selected_entries();
    if entries.is_empty() {
        return None;
    }
    let prov = panel.provider()?;
    let files = prov.is_local().then(|| {
        let dir = prov.current_path();
        let files: Vec<gio::File> = entries
            .iter()
            .map(|e| gio::File::for_path(join(&dir, &e.name())))
            .collect();
        gdk::ContentProvider::for_value(&gdk::FileList::from_array(&files).to_value())
    });
    let ours = gdk::ContentProvider::for_value(
        &FilesDrag {
            source: panel.column_view().downgrade(),
            entries,
        }
        .to_value(),
    );
    Some(match files {
        Some(files) => gdk::ContentProvider::new_union(&[ours, files]),
        None => ours,
    })
}

fn drop_on(br: &Browser, dst: Side, drag: FilesDrag) -> bool {
    let Some(src_view) = drag.source.upgrade() else {
        return false;
    };
    // Only from one of this browser's panels.
    let Some(src) = br.side_of(&src_view) else {
        return false;
    };
    // Accepted, so it doesn't animate back as refused, but nothing to do.
    if src == dst {
        return true;
    }
    let remote = |s: Side| br.panel(s).provider().is_some_and(|p| p.is_remote());
    if remote(src) && remote(dst) {
        br.move_entries(src, &drag.entries);
    } else {
        br.copy_entries(src, &drag.entries);
    }
    true
}
