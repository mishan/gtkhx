//! One panel of the browser: the path row, the sortable listing, and the
//! status footer, bound to a provider.
//!
//! The panel shows whatever its provider lists and can be switched between
//! local and remote in place; the browser owns which providers exist and
//! routes every cross-panel operation.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_char;
use std::rc::{Rc, Weak};
use std::time::Duration;

use gtk::gdk;
use gtk::gio;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::files_entry::HxFileEntry;

use super::complete::PathComplete;
use super::provider::Provider;
use super::row;
use crate::tr::{tr, tr_argv, trn_argv};

extern "C" {
    /// The symbolic icon standing in for a chrome icon under the active
    /// theme, or NULL for the classic pixmap (`gtkhx_icon.c`).
    fn gtkhx_icon_symbolic_name(resource: *const c_char) -> *const c_char;
}

/// Access bits (`hl_access.h`) that tell a drop box from a folder that failed.
const HL_ACCESS_UPLOAD_FILES: i32 = 1;
const HL_ACCESS_VIEW_DROP_BOXES: i32 = 30;

/// Clicks on one row further apart than this, and no closer, start an inline
/// rename, and a rename fires this long after the click that armed it. Above
/// GTK's default double-click time, so a double-click never renames.
const RENAME_PAUSE: Duration = Duration::from_millis(350);

/// Column positions.
const COL_KIND: usize = 3;

/// Keys for the data a cell widget carries.
const ITEM_KEY: &str = "hx-list-item";
const OLD_NAME_KEY: &str = "hx-old-name";

/// Record the list item a cell widget is showing, so a hit test can find the
/// row it belongs to. Weak: the column view owns the item.
fn set_item(w: &impl IsA<glib::Object>, item: &gtk::ListItem) {
    // SAFETY: this key only ever holds a WeakRef<ListItem>.
    unsafe { w.set_data(ITEM_KEY, item.downgrade()) };
}

fn item_of(w: &impl IsA<glib::Object>) -> Option<gtk::ListItem> {
    // SAFETY: as set_item.
    unsafe {
        w.data::<glib::WeakRef<gtk::ListItem>>(ITEM_KEY)
            .and_then(|p| p.as_ref().upgrade())
    }
}

/// The name a label showed before editing, which a commit renames from.
fn set_old_name(label: &gtk::EditableLabel, name: Option<String>) {
    // SAFETY: this key only ever holds an Option<String>.
    unsafe { label.set_data(OLD_NAME_KEY, name) };
}

fn old_name(label: &gtk::EditableLabel) -> Option<String> {
    // SAFETY: as set_old_name.
    unsafe {
        label
            .data::<Option<String>>(OLD_NAME_KEY)
            .and_then(|p| p.as_ref().clone())
    }
}

/// Join a name onto a folder path. Both sides use `/`.
pub fn join(dir: &str, name: &str) -> String {
    let dir = if dir.is_empty() { "/" } else { dir };
    if dir.ends_with('/') {
        format!("{dir}{name}")
    } else {
        format!("{dir}/{name}")
    }
}

/// An inline rename waiting out its pause: the label to edit and the name it
/// showed when armed, in case the row was reused for another entry meanwhile.
struct PendingRename {
    source: glib::SourceId,
    label: gtk::EditableLabel,
    name: String,
}

/// What the panel calls when the user switches sides: `true` for Local.
type SwapHandler = Box<dyn Fn(bool)>;

pub struct Panel {
    root: gtk::Box,
    /// Carries the active-panel style.
    frame: gtk::Frame,
    path_entry: gtk::Entry,
    side: Option<(gtk::DropDown, glib::SignalHandlerId)>,
    column_view: gtk::ColumnView,
    selection: gtk::MultiSelection,
    sort_model: gtk::SortListModel,
    status: gtk::Label,
    footer: gtk::Box,

    provider: RefCell<Option<Provider>>,
    provider_handlers: RefCell<Vec<glib::SignalHandlerId>>,
    listing_handler: RefCell<Option<(gio::ListModel, glib::SignalHandlerId)>>,
    /// Called with `true` for Local and `false` for Remote when the user
    /// switches sides; the browser builds the provider and calls
    /// [`Panel::set_provider`].
    swap: RefCell<Option<SwapHandler>>,
    complete: RefCell<Option<PathComplete>>,

    pending_rename: RefCell<Option<PendingRename>>,
    /// The label in edit mode, so a click elsewhere can stop it.
    editing: RefCell<Option<glib::WeakRef<gtk::EditableLabel>>>,
    /// The row and time of the last primary click, for the rename gesture.
    last_click: Cell<(Option<u32>, i64)>,
    /// Set when the user navigates from this panel. A remote listing rebuilds
    /// the rows, which can drop focus onto the other panel, so the reply
    /// takes it back.
    wants_focus: Cell<bool>,
    /// Row pixmaps by icon ID, loaded on first use.
    icons: RefCell<HashMap<u16, Option<gdk::Paintable>>>,
}

impl Panel {
    /// A panel showing `provider`. With `swappable`, it has a Local / Remote
    /// selector; see [`Panel::connect_swap`].
    pub fn new(provider: &Provider, swappable: bool) -> Rc<Panel> {
        let root = gtk::Box::new(gtk::Orientation::Vertical, 0);
        root.set_hexpand(true);
        root.set_vexpand(true);

        // [side selector] [Up] [path]. Styled like an action row but not one:
        // it's navigation, so a pane's Show Action Bar mustn't hide it.
        let path_row = gtk::Box::new(gtk::Orientation::Horizontal, 4);
        path_row.add_css_class("gtkhx-path-row");

        let side = swappable.then(|| {
            let labels = [tr("Local"), tr("Remote")];
            let labels: Vec<&str> = labels.iter().map(String::as_str).collect();
            let dd = gtk::DropDown::from_strings(&labels);
            dd.set_tooltip_text(Some(&tr(
                "Switch this panel between local filesystem and remote server",
            )));
            path_row.append(&dd);
            dd
        });

        let up = gtk::Button::from_icon_name("go-up-symbolic");
        up.set_tooltip_text(Some(&tr("Up one level")));
        path_row.append(&up);

        let path_entry = gtk::Entry::new();
        path_entry.set_hexpand(true);
        path_row.append(&path_entry);
        root.append(&path_row);

        // sort_model → selection → column view. The model under sort_model
        // is the provider's listing, swapped in place when the side changes.
        let sort_model = gtk::SortListModel::new(None::<gio::ListModel>, None::<gtk::Sorter>);
        let selection = gtk::MultiSelection::new(Some(sort_model.clone()));
        let column_view = gtk::ColumnView::new(Some(selection.clone()));
        unsafe {
            crate::ffi::gtkhx_apply_listview_style(column_view.upcast_ref::<gtk::Widget>().as_ptr())
        };
        column_view.set_show_row_separators(false);
        column_view.set_show_column_separators(false);
        column_view.add_css_class("gtkhx-files-list");

        let scrolled = gtk::ScrolledWindow::new();
        scrolled.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
        scrolled.set_vexpand(true);
        scrolled.set_child(Some(&column_view));

        // The frame gives the active-panel style somewhere to draw.
        let frame = gtk::Frame::new(None);
        frame.add_css_class("files-panel");
        frame.set_vexpand(true);
        frame.set_child(Some(&scrolled));
        frame.set_margin_start(6);
        frame.set_margin_end(6);
        root.append(&frame);

        let footer = gtk::Box::new(gtk::Orientation::Horizontal, 6);
        footer.set_margin_start(12);
        footer.set_margin_end(6);
        footer.set_margin_top(4);
        footer.set_margin_bottom(6);
        let status = gtk::Label::new(Some(""));
        status.set_xalign(0.0);
        status.add_css_class("dim-label");
        status.add_css_class("caption");
        status.set_hexpand(true);
        footer.append(&status);
        root.append(&footer);

        let panel = Rc::new_cyclic(|weak: &Weak<Panel>| {
            let side = side.map(|dd| {
                let weak = weak.clone();
                let id = dd.connect_selected_notify(move |dd| {
                    if let Some(p) = weak.upgrade() {
                        p.on_side_changed(dd.selected() == 0);
                    }
                });
                (dd, id)
            });
            Panel {
                root,
                frame,
                path_entry,
                side,
                column_view,
                selection,
                sort_model,
                status,
                footer,
                provider: RefCell::new(None),
                provider_handlers: RefCell::new(Vec::new()),
                listing_handler: RefCell::new(None),
                swap: RefCell::new(None),
                complete: RefCell::new(None),
                pending_rename: RefCell::new(None),
                editing: RefCell::new(None),
                last_click: Cell::new((None, 0)),
                wants_focus: Cell::new(false),
                icons: RefCell::new(HashMap::new()),
            }
        });

        panel.build_columns();
        panel.wire(&up);
        panel.attach(provider);
        panel
    }

    fn build_columns(self: &Rc<Self>) {
        let cv = &self.column_view;
        let name = self.add_column(&tr("Name"), self.name_factory(), row::cmp_name, None, true);
        let size = self.add_column(&tr("Size"), size_factory(), row::cmp_size, Some(84), false);
        let modified = self.add_column(
            &tr("Modified"),
            text_factory(1.0, |lbl, e| {
                lbl.set_text(&row::modified_text(e.modified()))
            }),
            row::cmp_modified,
            Some(110),
            false,
        );
        // Kind starts hidden: the icon already says folder or file, and the
        // column mostly repeated it. Any header's right-click brings it back.
        let kind = self.add_column(
            &tr("Kind"),
            text_factory(0.0, |lbl, e| lbl.set_text(&e.kind())),
            row::cmp_kind,
            Some(110),
            false,
        );
        kind.set_visible(false);
        cv.sort_by_column(Some(&name), gtk::SortType::Ascending);

        let show_kind = gio::SimpleAction::new_stateful("show-kind", None, &false.to_variant());
        let cols = [name, size, modified, kind];
        let kind_col = cols[COL_KIND].clone();
        show_kind.connect_change_state(move |a, v| {
            if let Some(v) = v {
                a.set_state(v);
                kind_col.set_visible(v.get::<bool>().unwrap_or(false));
            }
        });
        let group = gio::SimpleActionGroup::new();
        group.add_action(&show_kind);
        cv.insert_action_group("fpanel", Some(&group));
        let menu = gio::Menu::new();
        menu.append(Some(&tr("Show Kind Column")), Some("fpanel.show-kind"));
        for c in &cols {
            c.set_header_menu(Some(&menu));
        }

        // Header clicks sort the model the selection sits on.
        self.sort_model.set_sorter(cv.sorter().as_ref());
    }

    fn add_column(
        &self,
        title: &str,
        factory: gtk::SignalListItemFactory,
        cmp: fn(&HxFileEntry, &HxFileEntry) -> std::cmp::Ordering,
        width: Option<i32>,
        expand: bool,
    ) -> gtk::ColumnViewColumn {
        let col = gtk::ColumnViewColumn::new(Some(title), Some(factory));
        if let Some(w) = width {
            col.set_fixed_width(w);
        }
        col.set_expand(expand);
        col.set_resizable(true);
        col.set_sorter(Some(&gtk::CustomSorter::new(move |a, b| {
            match (
                a.downcast_ref::<HxFileEntry>(),
                b.downcast_ref::<HxFileEntry>(),
            ) {
                (Some(a), Some(b)) => cmp(a, b).into(),
                _ => gtk::Ordering::Equal,
            }
        })));
        self.column_view.append_column(&col);
        col
    }

    /// The Name column: icon plus an editable label that renames in place.
    ///
    /// The label is neither editable nor a pointer target until a rename
    /// starts. GtkEditableLabel would otherwise start editing on its own click,
    /// and its inner label takes presses for text selection, which keeps them
    /// from the row's selection, double-click and drag. The rename gesture sits
    /// on the row box instead, in the capture phase so it sees the click before
    /// the column view claims it, and never claims it.
    fn name_factory(self: &Rc<Self>) -> gtk::SignalListItemFactory {
        let factory = gtk::SignalListItemFactory::new();
        let weak = Rc::downgrade(self);
        factory.connect_setup(move |_, item| {
            let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
            let row = gtk::Box::new(gtk::Orientation::Horizontal, 6);
            // The icons are 16px pixel art: crisp at their own size, and the
            // rows stay dense.
            let icon = gtk::Image::new();
            icon.set_pixel_size(16);
            let label = gtk::EditableLabel::new("");
            label.set_editable(false);
            label.set_can_target(false);
            label.set_hexpand(true);
            label.set_halign(gtk::Align::Start);
            label.set_valign(gtk::Align::Center);
            row.append(&icon);
            row.append(&label);
            item.set_child(Some(&row));

            let w = weak.clone();
            label.connect_editing_notify(move |label| {
                if let Some(p) = w.upgrade() {
                    p.on_editing_changed(label);
                }
            });

            let click = gtk::GestureClick::new();
            click.set_button(gdk::BUTTON_PRIMARY);
            click.set_propagation_phase(gtk::PropagationPhase::Capture);
            let w = weak.clone();
            click.connect_pressed(move |g, n, _, _| {
                if let Some(p) = w.upgrade() {
                    p.on_name_pressed(g, n);
                }
            });
            row.add_controller(click);
        });
        let weak = Rc::downgrade(self);
        factory.connect_bind(move |_, item| {
            let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
            let Some(row) = item.child() else {
                return;
            };
            set_item(&row, item);
            let (Some(icon), Some(label)) = (
                row.first_child().and_downcast::<gtk::Image>(),
                row.last_child().and_downcast::<gtk::EditableLabel>(),
            ) else {
                return;
            };
            let Some(e) = item.item().and_downcast::<HxFileEntry>() else {
                icon.clear();
                label.set_text("");
                set_old_name(&label, None);
                return;
            };
            if let Some(p) = weak.upgrade() {
                p.set_row_icon(&icon, e.icon_id());
            }
            // A rebind mid-edit would commit whatever was typed against the
            // new entry; drop the edit first.
            if label.is_editing() {
                label.stop_editing(false);
            }
            let name = e.name();
            label.set_text(&name);
            set_old_name(&label, Some(name));
        });
        factory
    }

    /// The symbolic icon when the theme uses them — asked per bind, so a theme
    /// change reaches rows as they rebind — else the pixmap.
    fn set_row_icon(&self, icon: &gtk::Image, icon_id: u16) {
        let resource = row::icon_resource(icon_id)
            .or_else(|| row::icon_resource(row::ICON_FILE))
            .unwrap_or_default();
        let res = crate::cs(resource);
        let symbolic = unsafe { gtkhx_icon_symbolic_name(res.as_ptr()) };
        if !symbolic.is_null() {
            icon.set_icon_name(Some(&unsafe { crate::cstr(symbolic) }));
            return;
        }
        let id = if row::icon_resource(icon_id).is_some() {
            icon_id
        } else {
            row::ICON_FILE
        };
        let paintable = self
            .icons
            .borrow_mut()
            .entry(id)
            .or_insert_with(|| crate::news_browser::load_icon(resource))
            .clone();
        match paintable {
            Some(p) => icon.set_paintable(Some(&p)),
            None => icon.clear(),
        }
    }

    fn wire(self: &Rc<Self>, up: &gtk::Button) {
        let weak = Rc::downgrade(self);
        up.connect_clicked(move |_| {
            if let Some(p) = weak.upgrade() {
                p.wants_focus.set(true);
                if let Some(prov) = p.provider() {
                    prov.navigate_up();
                }
            }
        });
        let weak = Rc::downgrade(self);
        self.path_entry.connect_activate(move |entry| {
            let text = entry.text();
            if text.is_empty() {
                return;
            }
            if let Some(p) = weak.upgrade() {
                p.wants_focus.set(true);
                if let Some(prov) = p.provider() {
                    prov.navigate(&text);
                }
            }
        });
        let weak = Rc::downgrade(self);
        self.column_view.connect_activate(move |_, pos| {
            if let Some(p) = weak.upgrade() {
                p.on_row_activated(pos);
            }
        });
        let weak = Rc::downgrade(self);
        self.selection.connect_selection_changed(move |_, _, _| {
            if let Some(p) = weak.upgrade() {
                p.update_status();
            }
        });
    }

    // ---- Provider ----

    pub fn provider(&self) -> Option<Provider> {
        self.provider.borrow().clone()
    }

    /// Show `provider` instead, keeping the widgets. The selection goes; the
    /// path completion follows the side.
    pub fn set_provider(self: &Rc<Self>, provider: &Provider) {
        if self.provider.borrow().as_ref() == Some(provider) {
            return;
        }
        self.detach();
        self.attach(provider);
    }

    /// Called when the user picks a side: `true` for Local.
    pub fn connect_swap<F: Fn(bool) + 'static>(&self, f: F) {
        *self.swap.borrow_mut() = Some(Box::new(f));
    }

    fn attach(self: &Rc<Self>, provider: &Provider) {
        *self.provider.borrow_mut() = Some(provider.clone());
        let listing = provider.listing();
        self.sort_model.set_model(Some(&listing));
        let weak = Rc::downgrade(self);
        let id = listing.connect_items_changed(move |_, _, _, _| {
            if let Some(p) = weak.upgrade() {
                // The row the rename was armed on may be gone or reused.
                p.cancel_rename();
                p.update_status();
            }
        });
        *self.listing_handler.borrow_mut() = Some((listing, id));

        self.path_entry.set_text(&provider.current_path());

        // Completion only for a local folder: a remote one can't be listed
        // without a round trip per keystroke.
        self.complete.replace(None);
        if provider.is_local() {
            *self.complete.borrow_mut() = Some(PathComplete::attach(&self.path_entry));
        }

        // The selector follows the provider rather than driving it.
        if let Some((dd, id)) = &self.side {
            dd.block_signal(id);
            dd.set_selected(if provider.is_local() { 0 } else { 1 });
            dd.unblock_signal(id);
        }

        let weak = Rc::downgrade(self);
        let navigated = provider.connect_navigated(move |path| {
            if let Some(p) = weak.upgrade() {
                p.on_navigated(path);
            }
        });
        let weak = Rc::downgrade(self);
        let unavailable = provider.connect_unavailable_changed(move || {
            if let Some(p) = weak.upgrade() {
                if let Some(prov) = p.provider() {
                    // Now reachable: list what's really there.
                    if prov.unavailable_reason().is_none() {
                        prov.reload();
                    }
                }
                p.update_status();
            }
        });
        *self.provider_handlers.borrow_mut() = vec![navigated, unavailable];

        // A remote provider doesn't send anything until it's logged in; the
        // unavailable-changed handler catches up then.
        provider.reload();
        self.update_status();
    }

    fn detach(&self) {
        self.cancel_rename();
        let Some(provider) = self.provider.borrow_mut().take() else {
            return;
        };
        for id in self.provider_handlers.take() {
            provider.disconnect(id);
        }
        if let Some((listing, id)) = self.listing_handler.take() {
            listing.disconnect(id);
        }
    }

    fn on_side_changed(&self, want_local: bool) {
        let Some(current) = self.provider() else {
            return;
        };
        if current.is_local() == want_local {
            return;
        }
        if let Some(f) = self.swap.borrow().as_ref() {
            f(want_local);
        }
    }

    // ---- Navigation and status ----

    fn on_navigated(&self, path: &str) {
        self.path_entry.set_text(path);
        self.update_status();
        // Only after a navigation the user made here; a reload on connecting
        // mustn't pull focus from wherever the user is working.
        if self.wants_focus.replace(false) {
            self.column_view.grab_focus();
        }
    }

    fn on_row_activated(&self, pos: u32) {
        // The first press of the double-click may have armed a rename.
        self.cancel_rename();
        let Some(e) = self
            .column_view
            .model()
            .and_then(|m| m.item(pos))
            .and_downcast::<HxFileEntry>()
        else {
            return;
        };
        let Some(prov) = self.provider() else {
            return;
        };
        if e.is_dir() {
            self.wants_focus.set(true);
            prov.navigate(&join(&prov.current_path(), &e.name()));
        } else {
            prov.activate(&e);
        }
    }

    fn update_status(&self) {
        let prov = self.provider();
        let n_total = self.selection.n_items();
        let n_sel = self.selection.selection().size() as u32;

        // Before login and after a disconnect: say why the list is empty,
        // rather than "0 items".
        if let Some(reason) = prov.as_ref().and_then(Provider::unavailable_reason) {
            self.status.add_css_class("warning");
            self.status.set_text(&reason);
            return;
        }
        if let Some(prov) = prov.as_ref().filter(|p| p.has_listing_error()) {
            // An account that can upload but not see drop boxes has almost
            // certainly opened one. The bits are the listed server's.
            let drop_box = prov.conn().is_some_and(|c| {
                c.connected()
                    && c.access(HL_ACCESS_UPLOAD_FILES)
                    && !c.access(HL_ACCESS_VIEW_DROP_BOXES)
            });
            self.status.add_css_class("warning");
            self.status.set_text(&if drop_box {
                tr("Folder is upload-only — drop files here to upload")
            } else {
                tr("Can't list this folder.")
            });
            return;
        }
        self.status.remove_css_class("warning");
        let n = n_total.to_string();
        self.status.set_text(&if n_sel == 0 {
            trn_argv("%u item", "%u items", n_total.into(), &[&n])
        } else {
            tr_argv("%1$u of %2$u selected", &[&n_sel.to_string(), &n])
        });
    }

    // ---- Inline rename ----

    fn cancel_rename(&self) {
        if let Some(pending) = self.pending_rename.take() {
            pending.source.remove();
        }
    }

    /// A primary press on a row. A second click on the same row, with a pause
    /// that rules out a double-click, starts renaming it — the Finder and
    /// GNOME Files gesture. The clicks are tracked here rather than read off
    /// the selection, which the column view updates in an order no observer
    /// can rely on.
    fn on_name_pressed(self: &Rc<Self>, gesture: &gtk::GestureClick, n_press: i32) {
        // The second press of a double-click belongs to activation.
        if n_press != 1 {
            self.cancel_rename();
            return;
        }
        let Some(row) = gesture.widget() else {
            return;
        };
        let Some(item) = item_of(&row) else {
            return;
        };
        let pos = item.position();

        // Make sure the row highlights as the first click of the gesture.
        // Not with Ctrl or Shift held: those toggle and extend, and the column
        // view's own gesture handles them.
        let mods = gesture.current_event_state();
        if !mods.intersects(gdk::ModifierType::CONTROL_MASK | gdk::ModifierType::SHIFT_MASK) {
            self.selection.select_item(pos, true);
        }

        let label = row.last_child().and_downcast::<gtk::EditableLabel>();
        let editing = self.editing.borrow().as_ref().and_then(|w| w.upgrade());
        if editing.is_some() && editing != label {
            self.stop_inline_edit();
        }

        let Some(e) = item.item().and_downcast::<HxFileEntry>() else {
            self.cancel_rename();
            return;
        };
        let Some(label) = label else {
            return;
        };
        // A click inside the open editor places the cursor.
        if label.is_editing() {
            self.cancel_rename();
            return;
        }

        let now = glib::monotonic_time();
        let (prev, then) = self.last_click.replace((Some(pos), now));
        let paused = now - then >= RENAME_PAUSE.as_micros() as i64;
        if prev != Some(pos) || !paused {
            self.cancel_rename();
            return;
        }

        self.cancel_rename();
        let weak = Rc::downgrade(self);
        let source = glib::timeout_add_local_once(RENAME_PAUSE, move || {
            if let Some(p) = weak.upgrade() {
                p.fire_rename();
            }
        });
        *self.pending_rename.borrow_mut() = Some(PendingRename {
            source,
            label,
            name: e.name(),
        });
    }

    fn fire_rename(&self) {
        // The source has fired, so it isn't removed.
        let Some(PendingRename { label, name, .. }) = self.pending_rename.take() else {
            return;
        };
        // The row may have been reused for another entry since.
        if old_name(&label).as_deref() != Some(name.as_str()) || label.is_editing() {
            return;
        }
        // Editable and a pointer target while editing, so the entry takes
        // clicks for the cursor; the end of the edit turns both back off.
        label.set_editable(true);
        label.set_can_target(true);
        label.start_editing();
        label.select_region(0, -1);
    }

    /// Leave an edit without renaming. The stop comes before moving focus:
    /// losing focus first sends GtkEditableLabel down its own commit path,
    /// which can leave the editor showing.
    fn stop_inline_edit(&self) {
        let Some(label) = self.editing.take().and_then(|w| w.upgrade()) else {
            return;
        };
        label.stop_editing(false);
        self.column_view.grab_focus();
        label.set_editable(false);
        label.set_can_target(false);
    }

    fn on_editing_changed(&self, label: &gtk::EditableLabel) {
        if label.is_editing() {
            *self.editing.borrow_mut() = Some(label.downgrade());
            label.select_region(0, -1);
            return;
        }
        label.set_editable(false);
        label.set_can_target(false);
        let was_this = self
            .editing
            .borrow()
            .as_ref()
            .and_then(|w| w.upgrade())
            .is_some_and(|l| &l == label);
        if was_this {
            self.editing.replace(None);
        }

        let Some(old) = old_name(label) else {
            return;
        };
        let new = label.text();
        if new.is_empty() || new == old {
            label.set_text(&old);
            return;
        }
        let Some(prov) = self.provider() else {
            return;
        };
        match prov.rename(&old, &new) {
            Ok(()) => set_old_name(label, Some(new.into())),
            Err(e) => {
                glib::g_warning!("gtkhx", "files: inline rename {old} -> {new} failed: {e}");
                label.set_text(&old);
            }
        }
    }

    // ---- For the browser ----

    pub fn widget(&self) -> &gtk::Box {
        &self.root
    }

    pub fn column_view(&self) -> &gtk::ColumnView {
        &self.column_view
    }

    pub fn selection(&self) -> &gtk::MultiSelection {
        &self.selection
    }

    /// The status row; the browser adds its transfer button to it.
    pub fn footer(&self) -> &gtk::Box {
        &self.footer
    }

    pub fn set_active(&self, active: bool) {
        if active {
            self.frame.add_css_class("files-panel-active");
        } else {
            self.frame.remove_css_class("files-panel-active");
        }
    }

    /// The selected entry, when exactly one is selected.
    pub fn single_selected(&self) -> Option<HxFileEntry> {
        let sel = self.selection.selection();
        if sel.size() != 1 {
            return None;
        }
        self.selection.item(sel.minimum()).and_downcast()
    }

    /// The selected entries, in row order.
    pub fn selected_entries(&self) -> Vec<HxFileEntry> {
        let sel = self.selection.selection();
        let mut out = Vec::new();
        if let Some((iter, first)) = gtk::BitsetIter::init_first(&sel) {
            for pos in std::iter::once(first).chain(iter) {
                if let Some(e) = self.selection.item(pos).and_downcast() {
                    out.push(e);
                }
            }
        }
        out
    }

    /// The entry under (`x`, `y`) in the column view's coordinates, and its
    /// row, or `None` for empty space or the header. Every cell's bind records
    /// its list item on the cell's child, so walking up from the hit finds it;
    /// a hit in a cell's padding lands on the cell, above that child, so each
    /// step looks one level down too.
    pub fn entry_at(&self, x: f64, y: f64) -> Option<(HxFileEntry, u32)> {
        let cv = self.column_view.upcast_ref::<gtk::Widget>();
        let mut w = cv.pick(x, y, gtk::PickFlags::DEFAULT);
        while let Some(widget) = w {
            if &widget == cv {
                break;
            }
            for probe in [Some(widget.clone()), widget.first_child()]
                .into_iter()
                .flatten()
            {
                if let Some(item) = item_of(&probe) {
                    if let Some(e) = item.item().and_downcast::<HxFileEntry>() {
                        return Some((e, item.position()));
                    }
                }
            }
            w = widget.parent();
        }
        None
    }

    /// The entry on the focused row, its row, and a point just under it to
    /// hang a menu from — or `None` when focus isn't on a row here.
    pub fn focused_entry(&self) -> Option<(HxFileEntry, u32, gdk::Rectangle)> {
        let cv = self.column_view.upcast_ref::<gtk::Widget>();
        let focus = cv.root()?.focus()?;
        if !focus.is_ancestor(cv) {
            return None;
        }
        let item = find_item(&focus)?;
        let e = item.item().and_downcast::<HxFileEntry>()?;
        let rect = focus
            .compute_bounds(cv)
            .map(|b| gdk::Rectangle::new(b.x() as i32 + 24, (b.y() + b.height()) as i32, 1, 1))
            .unwrap_or_else(|| gdk::Rectangle::new(0, 0, 1, 1));
        Some((e, item.position(), rect))
    }

    /// Stop timers and let go of the provider. The browser calls this as its
    /// content is destroyed.
    pub fn teardown(&self) {
        self.complete.replace(None);
        self.detach();
    }
}

/// The first list item recorded on `root` or below it.
fn find_item(root: &gtk::Widget) -> Option<gtk::ListItem> {
    if let Some(item) = item_of(root) {
        return Some(item);
    }
    let mut c = root.first_child();
    while let Some(child) = c {
        if let Some(item) = find_item(&child) {
            return Some(item);
        }
        c = child.next_sibling();
    }
    None
}

/// A label column: `xalign` 1 for Size and Modified, 0 for Kind.
fn text_factory(
    xalign: f32,
    bind: impl Fn(&gtk::Label, &HxFileEntry) + 'static,
) -> gtk::SignalListItemFactory {
    let factory = gtk::SignalListItemFactory::new();
    factory.connect_setup(move |_, item| {
        let lbl = gtk::Label::new(None);
        lbl.set_xalign(xalign);
        lbl.set_ellipsize(gtk::pango::EllipsizeMode::End);
        item.downcast_ref::<gtk::ListItem>()
            .expect("list item")
            .set_child(Some(&lbl));
    });
    factory.connect_bind(move |_, item| {
        let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
        let Some(lbl) = item.child().and_downcast::<gtk::Label>() else {
            return;
        };
        // For the context menu's hit test (Panel::entry_at).
        set_item(&lbl, item);
        match item.item().and_downcast::<HxFileEntry>() {
            Some(e) => bind(&lbl, &e),
            None => {
                lbl.set_text("");
                lbl.set_tooltip_text(None);
            }
        }
    });
    factory
}

/// The Size column: rounded, with the exact byte count a hover away.
fn size_factory() -> gtk::SignalListItemFactory {
    text_factory(1.0, |lbl, e| {
        lbl.set_text(&row::size_text(e.is_dir(), e.size()));
        let tip = (!e.is_dir()).then(|| row::exact_size(e.size()));
        lbl.set_tooltip_text(tip.as_deref());
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn joins_with_one_slash() {
        assert_eq!(join("/", "a"), "/a");
        assert_eq!(join("", "a"), "/a");
        assert_eq!(join("/x", "a"), "/x/a");
        assert_eq!(join("/x/", "a"), "/x/a");
    }
}
