//! The browser: two panels, one of them active, and every operation between
//! them — the header bar's actions, each footer's transfer button, the row
//! menu, drag and drop, and the keyboard.
//!
//! One per connection. It lives as long as its content box, whose destroy is
//! its teardown.

use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::rc::{Rc, Weak};

use glib::translate::from_glib_none;
use gtk::gdk;
use gtk::gio;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::files_entry::HxFileEntry;
use libadwaita as adw;

use super::panel::Panel;
use super::provider::{bytes_c, join_bytes, Provider};
use super::{dialogs, dnd};
use crate::tr::{tr, trc, trn_argv};

extern "C" {
    /// `tasks_bridge.c` — the session a connection belongs to.
    fn hx_sess_from_htlc(htlc: *mut c_void) -> *mut c_void;
    fn hx_htxf_total_pos(p: *const c_void) -> u64;
}

/// `GtkhxConnectionState` (`gtkhx_session.h`).
const GTKHX_CONNECTION_DISCONNECTED: u32 = 0;
const GTKHX_CONNECTION_LOGIN_READY: u32 = 4;

/// Access bit (`hl_access.h`).
pub(super) const HL_ACCESS_MOVE_FILES: i32 = 4;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Side {
    Left,
    Right,
}

impl Side {
    pub fn other(self) -> Side {
        match self {
            Side::Left => Side::Right,
            Side::Right => Side::Left,
        }
    }
}

/// What the Files window hosts: the content, and the action buttons for its
/// header bar.
pub struct Built {
    pub content: gtk::Widget,
    pub header_start: gtk::Widget,
    pub header_end: gtk::Widget,
}

struct TransferButton {
    button: gtk::Button,
    label: gtk::Label,
}

pub struct Browser {
    /// The session this browser was built for. A pane's own connection is its
    /// provider's, which is the one to ask for anything scoped to a pane.
    sess: *mut c_void,
    /// The content box: dialogs parent to it and the shortcuts live on it.
    content: gtk::Box,
    left: Rc<Panel>,
    right: Rc<Panel>,
    active: Cell<Side>,
    toast: adw::ToastOverlay,
    left_xfer: TransferButton,
    right_xfer: TransferButton,
    /// The row menu, one per panel since a popover needs a parent, and the
    /// entry it opened on — the entry rather than its row, which a reload
    /// under the open menu would repoint.
    left_menu: gtk::PopoverMenu,
    right_menu: gtk::PopoverMenu,
    menu_entry: RefCell<Option<HxFileEntry>>,
    session_handlers: RefCell<Vec<glib::SignalHandlerId>>,
    torn_down: Cell<bool>,
}

impl Browser {
    pub fn panel(&self, side: Side) -> &Rc<Panel> {
        match side {
            Side::Left => &self.left,
            Side::Right => &self.right,
        }
    }

    pub fn active(&self) -> Side {
        self.active.get()
    }

    fn active_panel(&self) -> &Rc<Panel> {
        self.panel(self.active.get())
    }

    pub fn side_of(&self, cv: &gtk::ColumnView) -> Option<Side> {
        [Side::Left, Side::Right]
            .into_iter()
            .find(|s| self.panel(*s).column_view() == cv)
    }

    pub fn content(&self) -> &gtk::Box {
        &self.content
    }

    fn set_active(&self, side: Side) {
        self.active.set(side);
        self.left.set_active(side == Side::Left);
        self.right.set_active(side == Side::Right);
    }

    pub fn toast(&self, text: &str) {
        self.toast.add_toast(adw::Toast::new(text));
    }

    fn session_object() -> glib::Object {
        unsafe {
            from_glib_none(
                gtkhx_core::session::gtkhx_session_get_default() as *mut glib::gobject_ffi::GObject
            )
        }
    }

    // ---- Actions on the active panel ----

    fn reload(&self) {
        if let Some(p) = self.active_panel().provider() {
            p.reload();
        }
    }

    fn preview(&self) {
        let panel = self.active_panel();
        let Some(e) = panel.single_selected() else {
            self.toast(&tr("Select a single file to preview."));
            return;
        };
        if e.is_dir() {
            self.toast(&tr("Preview is for files, not folders."));
            return;
        }
        if let Some(p) = panel.provider() {
            p.preview(&e);
        }
    }

    /// FILE_GETINFO for the selected remote file; the reply opens the Get Info
    /// dialog. Local files have nothing to ask.
    fn get_info(&self) {
        let panel = self.active_panel();
        let Some(prov) = panel.provider().filter(Provider::is_remote) else {
            self.toast(&tr("Get Info is only available for remote files."));
            return;
        };
        let Some(conn) = prov.conn().filter(|c| c.connected()) else {
            self.toast(&tr("Not connected."));
            return;
        };
        let Some(e) = panel.single_selected() else {
            self.toast(&tr("Select a single file."));
            return;
        };
        let dir = bytes_c(&prov.path());
        let name = e.wire_name();
        unsafe {
            hxhandlers::send::files::hx_file_info(
                conn.ptr(),
                dir.as_ptr(),
                name.as_ptr().cast(),
                name.len(),
            )
        };
    }

    fn rename(self: &Rc<Self>) {
        let side = self.active.get();
        let Some(e) = self.panel(side).single_selected() else {
            self.toast(&tr("Select a single file to rename."));
            return;
        };
        dialogs::rename(self, side, &e);
    }

    fn open_move(self: &Rc<Self>) {
        let side = self.active.get();
        let entries = self.panel(side).selected_entries();
        if entries.is_empty() {
            self.toast(&tr("Select files to move first."));
            return;
        }
        let (Some(sp), Some(dp)) = (
            self.panel(side).provider(),
            self.panel(side.other()).provider(),
        ) else {
            return;
        };
        // MOVEFILE stays on one server and a rename on one filesystem; there's
        // no move across the line, so don't let the user type a destination
        // that wouldn't move anything.
        if sp.is_local() != dp.is_local() {
            self.toast(&tr(
                "Move only works within one side. Use Copy then Delete to move between local \
                 and remote.",
            ));
            return;
        }
        dialogs::move_to(self, side, &entries, &dp);
    }

    fn mkdir(self: &Rc<Self>) {
        dialogs::mkdir(self, self.active.get());
    }

    fn delete(self: &Rc<Self>) {
        let side = self.active.get();
        let entries = self.panel(side).selected_entries();
        if !entries.is_empty() {
            dialogs::delete(self, side, &entries);
        }
    }

    /// F5: the active panel's selection to the other panel's folder.
    fn copy(&self) {
        let side = self.active.get();
        self.transfer(side);
    }

    /// `src`'s selection to the other panel's folder.
    fn transfer(&self, src: Side) {
        let entries = self.panel(src).selected_entries();
        self.copy_entries(src, &entries);
    }

    /// Copy `entries` from `src`'s folder to the other panel's, and say how it
    /// went in one toast.
    pub fn copy_entries(&self, src: Side, entries: &[HxFileEntry]) {
        if entries.is_empty() {
            self.toast(&tr("Select a file to copy first."));
            return;
        }
        let (Some(sp), Some(dp)) = (
            self.panel(src).provider(),
            self.panel(src.other()).provider(),
        ) else {
            return;
        };
        let (mut queued, mut last_err) = (0u64, None);
        for e in entries {
            match sp.copy_to(&dp, e) {
                Ok(()) => queued += 1,
                Err(err) => last_err = Some(err),
            }
        }
        let failed = entries.len() as u64 - queued;
        // The reason leads when anything failed; one is enough, since
        // failures usually share a cause (permission, connection).
        let msg = match last_err {
            None => trn_argv(
                "Transfer queued (%u item).",
                "Transfers queued (%u items).",
                queued,
                &[&queued.to_string()],
            ),
            Some(err) if queued == 0 => err.message(),
            Some(err) => crate::tr::tr_argv(
                "%1$u queued, %2$u failed (%3$s).",
                &[&queued.to_string(), &failed.to_string(), &err.message()],
            ),
        };
        self.toast(&msg);
    }

    /// Move `entries` between two remote panels' folders: a drag within one
    /// server is a move, as file managers treat a drag within one volume.
    /// MOVEFILE answers later; a refusal arrives as a task error, so the toast
    /// says the move was requested rather than done.
    pub fn move_entries(&self, src: Side, entries: &[HxFileEntry]) {
        if entries.is_empty() {
            return;
        }
        let (Some(sp), Some(dp)) = (
            self.panel(src).provider(),
            self.panel(src.other()).provider(),
        ) else {
            return;
        };
        // Both are remote; the source's connection is where the files are.
        let Some(conn) = sp.conn().filter(|c| c.connected()) else {
            self.toast(&tr("Not connected to a server."));
            return;
        };
        if !conn.access(HL_ACCESS_MOVE_FILES) {
            self.toast(&tr(
                "You don't have permission to move files on the server.",
            ));
            return;
        }
        let (src_dir, dst_dir) = (sp.path(), dp.path());
        for e in entries {
            let name = e.wire_name();
            send_move(
                conn.ptr(),
                &join_bytes(&src_dir, &name),
                &join_bytes(&dst_dir, &name),
            );
        }
        sp.reload();
        dp.reload();
        let n = entries.len() as u64;
        self.toast(&trn_argv(
            "Move requested for %u item.",
            "Move requested for %u items.",
            n,
            &[&n.to_string()],
        ));
    }

    /// Open the entry the menu opened on, wherever it sits now: descend into
    /// a folder or open a file, as a double-click does.
    fn open_menu_entry(&self) {
        let Some(e) = self.menu_entry.borrow().clone() else {
            return;
        };
        let cv = self.active_panel().column_view();
        let Some(model) = cv.model() else {
            return;
        };
        for i in 0..model.n_items() {
            if model.item(i).as_ref() == Some(e.upcast_ref()) {
                cv.emit_by_name::<()>("activate", &[&i]);
                return;
            }
        }
    }

    /// F4: the default action on the single selected file.
    fn open_selected(&self) -> bool {
        let panel = self.active_panel();
        match panel.single_selected() {
            Some(e) if !e.is_dir() => {
                if let Some(p) = panel.provider() {
                    p.activate(&e);
                }
                true
            }
            _ => false,
        }
    }

    // ---- Side swaps ----

    /// A panel's selector asked for the other side. A fresh provider each
    /// time: a provider holds its own current folder, so two panels sharing
    /// one would navigate together.
    fn swap(&self, side: Side, want_local: bool) {
        let prov = if want_local {
            Provider::local()
        } else {
            unsafe { Provider::remote(self.sess) }
        };
        self.panel(side).set_provider(&prov);
        // Both buttons' verbs depend on both sides.
        self.update_transfer_buttons();
    }

    // ---- Transfer buttons ----

    fn xfer(&self, side: Side) -> &TransferButton {
        match side {
            Side::Left => &self.left_xfer,
            Side::Right => &self.right_xfer,
        }
    }

    /// What sending `src`'s selection to the other panel is: a download or an
    /// upload across the local/remote line, a copy within one side.
    pub fn transfer_verb(&self, src: Side) -> String {
        let local = |s: Side| self.panel(s).provider().is_some_and(|p| p.is_local());
        match (local(src), local(src.other())) {
            (false, true) => trc("files transfer", "Download"),
            (true, false) => trc("files transfer", "Upload"),
            _ => trc("files transfer", "Copy"),
        }
    }

    /// Both panels remote: Hotline has no server-side copy.
    pub fn transfer_impossible(&self, src: Side) -> bool {
        let remote = |s: Side| self.panel(s).provider().is_some_and(|p| !p.is_local());
        remote(src) && remote(src.other())
    }

    fn update_transfer_buttons(&self) {
        // Teardown empties the listings, and the models say so.
        if self.torn_down.get() {
            return;
        }
        for side in [Side::Left, Side::Right] {
            let TransferButton { button, label } = self.xfer(side);
            label.set_text(&self.transfer_verb(side));
            if self.transfer_impossible(side) {
                button.set_sensitive(false);
                button.set_tooltip_text(Some(&tr(
                    "Hotline servers can't copy between two remote folders; use Move (F6) instead",
                )));
                continue;
            }
            button.set_tooltip_text(Some(&tr(
                "Send this panel's selection to the other panel's folder",
            )));
            button.set_sensitive(self.panel(side).selection().selection().size() > 0);
        }
    }

    // ---- Row menu ----

    /// Built per popup: the transfer's verb depends on both sides, and empty
    /// space offers only what makes sense with nothing picked.
    fn row_menu(&self, side: Side, on_row: bool) -> gio::Menu {
        let menu = gio::Menu::new();
        if on_row {
            let top = gio::Menu::new();
            top.append(Some(&tr("Open")), Some("files.open"));
            // Left out rather than offered and refused where it can't work;
            // Move covers two remote panels.
            if !self.transfer_impossible(side) {
                top.append(Some(&self.transfer_verb(side)), Some("files.transfer"));
            }
            top.append(Some(&tr("Preview")), Some("files.preview"));
            top.append(Some(&tr("Get Info")), Some("files.info"));
            let mid = gio::Menu::new();
            mid.append(Some(&tr("Move…")), Some("files.move"));
            mid.append(Some(&tr("Rename…")), Some("files.rename"));
            mid.append(Some(&tr("Delete…")), Some("files.delete"));
            menu.append_section(None, &top);
            menu.append_section(None, &mid);
        }
        let bottom = gio::Menu::new();
        bottom.append(Some(&tr("New Folder…")), Some("files.mkdir"));
        bottom.append(Some(&tr("Reload")), Some("files.reload"));
        menu.append_section(None, &bottom);
        menu
    }

    /// Open the row menu on `side` at `at` (column-view coordinates), for `e`
    /// and its row — selected first — or for empty space.
    fn popup_menu(&self, side: Side, hit: Option<(HxFileEntry, u32)>, at: &gdk::Rectangle) {
        self.set_active(side);
        // A right-click on a row acts on that row: it joins the selection if
        // already in it, and replaces it otherwise, so the menu never acts on
        // rows the user can't see are picked.
        if let Some((_, pos)) = &hit {
            let sel = self.panel(side).selection();
            if !sel.is_selected(*pos) {
                sel.select_item(*pos, true);
            }
        }
        let on_row = hit.is_some();
        *self.menu_entry.borrow_mut() = hit.map(|(e, _)| e);
        let popover = match side {
            Side::Left => &self.left_menu,
            Side::Right => &self.right_menu,
        };
        popover.set_menu_model(Some(&self.row_menu(side, on_row)));
        // The popover hangs off the panel, so the point moves into its space.
        let panel = self.panel(side);
        let at = panel
            .column_view()
            .compute_point(
                panel.widget(),
                &gtk::graphene::Point::new(at.x() as f32, at.y() as f32),
            )
            .map(|p| gdk::Rectangle::new(p.x() as i32, p.y() as i32, 1, 1))
            .unwrap_or(*at);
        popover.set_pointing_to(Some(&at));
        popover.popup();
    }

    // ---- Session events ----

    fn on_connection_state(&self, htlc: *mut c_void, state: u32) {
        // Every browser hears every connection's changes; only its own count.
        if unsafe { hx_sess_from_htlc(htlc) } != self.sess {
            return;
        }
        // Only these two change whether a remote panel can list. A reload
        // before login is ready would put FILE_LIST on the wire before the
        // agreement was accepted, which strict servers disconnect for.
        if state != GTKHX_CONNECTION_DISCONNECTED && state != GTKHX_CONNECTION_LOGIN_READY {
            return;
        }
        if state == GTKHX_CONNECTION_LOGIN_READY {
            // The server has named itself by now.
            unsafe { super::gtkhx_files_window_refresh_title(self.sess) };
        }
        for side in [Side::Left, Side::Right] {
            let Some(prov) = self.panel(side).provider() else {
                continue;
            };
            // Drop what the user can no longer see, and don't carry this
            // server's deep path to the next one.
            if state == GTKHX_CONNECTION_DISCONNECTED {
                prov.reset_to_root();
            }
            prov.emit_unavailable_changed();
        }
    }

    /// A transfer finished on this connection: re-list both panels, so the new
    /// file shows without a reload.
    fn on_file_update(&self, sess: *mut c_void, htxf: *mut c_void) {
        if sess != self.sess || htxf.is_null() {
            return;
        }
        let x = htxf as *const hxnet::xfer_handle::HtxfHandle;
        let total = unsafe { (*x).total_size };
        if total == 0 || unsafe { hx_htxf_total_pos(htxf) } < total {
            return;
        }
        for side in [Side::Left, Side::Right] {
            if let Some(p) = self.panel(side).provider() {
                p.reload();
            }
        }
    }

    fn teardown(&self) {
        self.torn_down.set(true);
        let hub = Self::session_object();
        for id in self.session_handlers.take() {
            hub.disconnect(id);
        }
        self.menu_entry.replace(None);
        self.left.teardown();
        self.right.teardown();
    }
}

/// FILE_MOVE (and a rename where the name changes) from `src` to `dst`.
pub(super) fn send_move(htlc: *mut c_void, src: &[u8], dst: &[u8]) {
    let (s, d) = (bytes_c(src), bytes_c(dst));
    unsafe {
        hxhandlers::send::files::hx_file_move(htlc, s.as_ptr().cast_mut(), d.as_ptr().cast_mut())
    };
}

/// Build the browser for `sess`.
///
/// # Safety
/// `sess` is a live session that outlives the returned content.
pub unsafe fn build(sess: *mut c_void) -> Built {
    install_css();

    let local = Provider::local();
    let remote = Provider::remote(sess);
    let left = Panel::new(&local, true);
    let right = Panel::new(&remote, true);

    // Two panels, level to start with and resizing together.
    let paned = gtk::Paned::new(gtk::Orientation::Horizontal);
    paned.set_resize_start_child(true);
    paned.set_resize_end_child(true);
    paned.set_shrink_start_child(false);
    paned.set_shrink_end_child(false);
    paned.set_start_child(Some(left.widget()));
    paned.set_end_child(Some(right.widget()));
    settle_paned(&paned);

    let toast = adw::ToastOverlay::new();
    toast.set_child(Some(&paned));
    toast.set_vexpand(true);
    let content = gtk::Box::new(gtk::Orientation::Vertical, 0);
    content.append(&toast);

    let br = Rc::new(Browser {
        sess,
        content: content.clone(),
        left_xfer: transfer_button(Side::Left),
        right_xfer: transfer_button(Side::Right),
        left_menu: menu_popover(left.widget()),
        right_menu: menu_popover(right.widget()),
        left,
        right,
        active: Cell::new(Side::Left),
        toast,
        menu_entry: RefCell::new(None),
        session_handlers: RefCell::new(Vec::new()),
        torn_down: Cell::new(false),
    });

    let (header_start, header_end) = header_buttons(&br);

    for side in [Side::Left, Side::Right] {
        let panel = br.panel(side);
        let weak = Rc::downgrade(&br);
        panel.connect_swap(move |want_local| {
            if let Some(br) = weak.upgrade() {
                br.swap(side, want_local);
            }
        });

        let xfer = &br.xfer(side).button;
        panel.footer().append(xfer);
        let weak = Rc::downgrade(&br);
        xfer.connect_clicked(move |_| {
            if let Some(br) = weak.upgrade() {
                br.transfer(side);
            }
        });
        // A new listing replaces the items without a selection change, and
        // can empty the selection with it.
        let weak = Rc::downgrade(&br);
        panel.selection().connect_selection_changed(move |_, _, _| {
            if let Some(br) = weak.upgrade() {
                br.update_transfer_buttons();
            }
        });
        let weak = Rc::downgrade(&br);
        panel.selection().connect_items_changed(move |_, _, _, _| {
            if let Some(br) = weak.upgrade() {
                br.update_transfer_buttons();
            }
        });

        track_focus(&br, side);
        dnd::attach(&br, side);
        attach_menu(&br, side);
    }
    br.update_transfer_buttons();

    content.insert_action_group("files", Some(&menu_actions(&br)));
    install_shortcuts(&br);

    let hub = Browser::session_object();
    let weak = Rc::downgrade(&br);
    let state_id = hub.connect_local("connection-state-changed", false, move |args| {
        let htlc = args.get(1).and_then(|v| v.get::<*mut c_void>().ok());
        let state = args.get(2).and_then(|v| v.get::<u32>().ok());
        if let (Some(br), Some(htlc), Some(state)) = (weak.upgrade(), htlc, state) {
            br.on_connection_state(htlc, state);
        }
        None
    });
    let weak = Rc::downgrade(&br);
    let update_id = hub.connect_local("file-update", false, move |args| {
        let sess = args.get(1).and_then(|v| v.get::<*mut c_void>().ok());
        let htxf = args.get(2).and_then(|v| v.get::<*mut c_void>().ok());
        if let (Some(br), Some(sess), Some(htxf)) = (weak.upgrade(), sess, htxf) {
            br.on_file_update(sess, htxf);
        }
        None
    });
    *br.session_handlers.borrow_mut() = vec![state_id, update_id];

    // Closing the window destroys the content, and so does the connection
    // going away. The browser lives until then.
    let held = RefCell::new(Some(br.clone()));
    content.connect_destroy(move |_| {
        if let Some(br) = held.take() {
            br.teardown();
        }
    });

    br.set_active(Side::Left);
    br.left.column_view().grab_focus();

    Built {
        content: content.upcast(),
        header_start: header_start.upcast(),
        header_end: header_end.upcast(),
    }
}

/// The header bar's single-panel actions: Reload, New Folder, Preview and Get
/// Info at the start, Rename and Delete at the end.
fn header_buttons(br: &Rc<Browser>) -> (gtk::Box, gtk::Box) {
    let make = |resource: &str, tip: String, f: fn(&Rc<Browser>)| {
        let res = crate::cs(resource);
        let btn: gtk::Widget = unsafe {
            from_glib_none(crate::ffi::gtkhx_pixmap_button(
                res.as_ptr(),
                std::ptr::null(),
                crate::ffi::GTKHX_SCALE_WINDOW_BUTTONS,
                std::ptr::null(),
                std::ptr::null_mut(),
            ))
        };
        btn.set_tooltip_text(Some(&tip));
        if let Some(b) = btn.downcast_ref::<gtk::Button>() {
            let weak = Rc::downgrade(br);
            b.connect_clicked(move |_| {
                if let Some(br) = weak.upgrade() {
                    f(&br);
                }
            });
        }
        btn
    };
    let start = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    start.append(&make(
        "/com/nasledov/gtkhx/pixmaps/refresh.png",
        tr("Reload active panel (Ctrl+R)"),
        |b| b.reload(),
    ));
    start.append(&make(
        "/com/nasledov/gtkhx/pixmaps/mkdir.png",
        tr("New folder in active panel (F7, Ctrl+N)"),
        |b| b.mkdir(),
    ));
    start.append(&make(
        "/com/nasledov/gtkhx/pixmaps/preview.png",
        tr("Preview selected file (F3, Ctrl+P)"),
        |b| b.preview(),
    ));
    start.append(&make(
        "/com/nasledov/gtkhx/pixmaps/info.png",
        tr("Get Info for selected file (Ctrl+I)"),
        |b| b.get_info(),
    ));
    let end = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    // A pencil for "edit the name", as in the news browser.
    end.append(&make(
        "/com/nasledov/gtkhx/pixmaps/pencil.png",
        tr("Rename selected file (F2)"),
        |b| b.rename(),
    ));
    end.append(&make(
        "/com/nasledov/gtkhx/pixmaps/trash.png",
        tr("Delete selection in active panel (F8, Delete, Ctrl+D)"),
        |b| b.delete(),
    ));
    (start, end)
}

/// A footer's transfer button: it says in words what it does to that panel's
/// selection, and its arrow points at the other panel.
fn transfer_button(side: Side) -> TransferButton {
    let button = gtk::Button::new();
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    let label = gtk::Label::new(None);
    let arrow = gtk::Image::from_icon_name(match side {
        Side::Left => "go-next-symbolic",
        Side::Right => "go-previous-symbolic",
    });
    match side {
        Side::Left => {
            row.append(&label);
            row.append(&arrow);
        }
        Side::Right => {
            row.append(&arrow);
            row.append(&label);
        }
    }
    button.set_child(Some(&row));
    button.add_css_class("gtkhx-files-transfer");
    TransferButton { button, label }
}

/// The row menu's popover, parented to the panel rather than its column view:
/// under the column view it came out cut to a fixed height, dropping the last
/// items. It has to be given back before its parent is finalized, and the
/// browser's teardown can come too late for that, so the parent's own destroy
/// does it.
fn menu_popover(parent: &gtk::Box) -> gtk::PopoverMenu {
    let popover = gtk::PopoverMenu::from_model(None::<&gio::MenuModel>);
    popover.set_parent(parent);
    popover.set_has_arrow(false);
    popover.set_halign(gtk::Align::Start);
    let p = popover.clone();
    parent.connect_destroy(move |_| p.unparent());
    popover
}

fn attach_menu(br: &Rc<Browser>, side: Side) {
    let click = gtk::GestureClick::new();
    click.set_button(gdk::BUTTON_SECONDARY);
    let weak = Rc::downgrade(br);
    click.connect_pressed(move |g, _, x, y| {
        g.set_state(gtk::EventSequenceState::Claimed);
        if let Some(br) = weak.upgrade() {
            let hit = br.panel(side).entry_at(x, y);
            br.popup_menu(side, hit, &gdk::Rectangle::new(x as i32, y as i32, 1, 1));
        }
    });
    br.panel(side).column_view().add_controller(click);
}

/// An action on the browser, for the menu's action table.
type BrowserAction = fn(&Rc<Browser>);

fn menu_actions(br: &Rc<Browser>) -> gio::SimpleActionGroup {
    let group = gio::SimpleActionGroup::new();
    let actions: [(&str, BrowserAction); 9] = [
        ("open", |b| b.open_menu_entry()),
        ("transfer", |b| b.transfer(b.active())),
        ("move", |b| b.open_move()),
        ("preview", |b| b.preview()),
        ("info", |b| b.get_info()),
        ("rename", |b| b.rename()),
        ("delete", |b| b.delete()),
        ("mkdir", |b| b.mkdir()),
        ("reload", |b| b.reload()),
    ];
    for (name, f) in actions {
        let action = gio::SimpleAction::new(name, None);
        let weak = Rc::downgrade(br);
        action.connect_activate(move |_, _| {
            if let Some(br) = weak.upgrade() {
                f(&br);
            }
        });
        group.add_action(&action);
    }
    group
}

/// The active panel follows focus: a focus controller on each panel's root
/// fires on focus entering it anywhere inside. A click is a second way in, for
/// the places that don't take focus. It runs in the bubble phase: in the
/// capture phase the column view took the first click of a double-click on
/// the other panel for a plain selection and waited for another pair, so the
/// first double-click there did nothing.
fn track_focus(br: &Rc<Browser>, side: Side) {
    let root = br.panel(side).widget();
    let focus = gtk::EventControllerFocus::new();
    let weak = Rc::downgrade(br);
    focus.connect_enter(move |_| {
        if let Some(br) = weak.upgrade() {
            br.set_active(side);
        }
    });
    root.add_controller(focus);
    let click = gtk::GestureClick::new();
    click.set_propagation_phase(gtk::PropagationPhase::Bubble);
    let weak = Rc::downgrade(br);
    click.connect_pressed(move |_, _, _, _| {
        if let Some(br) = weak.upgrade() {
            br.set_active(side);
        }
    });
    root.add_controller(click);
}

/// Split the panels evenly, once the window's width has settled: a window's
/// first allocations come in steps (natural size, then default size), and
/// halving the first one halves the wrong width. So wait for the same width
/// two frames running, split once, and leave the divider to the user. Half the
/// width rather than of max-position, which is the width less the end panel's
/// minimum and would land left of center.
fn settle_paned(paned: &gtk::Paned) {
    let last = Cell::new(0);
    paned.add_tick_callback(move |paned, _| {
        let width = paned.width();
        if width <= 0 || width != last.replace(width) {
            return glib::ControlFlow::Continue;
        }
        paned.set_position(width / 2);
        glib::ControlFlow::Break
    });
}

/// Whether focus is in something editable, where Tab and Backspace belong to
/// the text. A GtkEntry hands focus to its internal GtkText, and both are
/// editables.
fn focus_is_editable(br: &Browser) -> bool {
    br.content
        .root()
        .and_then(|r| r.focus())
        .is_some_and(|f| f.is::<gtk::Editable>())
}

/// The keyboard. F-keys follow the Norton layout, each with a Ctrl form for
/// desktops that take the F-keys for media: F2 rename, F3 / Ctrl+P preview,
/// F4 open, F5 copy, F6 move, F7 / Ctrl+N new folder, F8 / Delete / Ctrl+D
/// delete, Ctrl+I Get Info, Ctrl+R reload, Shift+F10 / Menu the row menu,
/// Tab the other panel, Backspace up. F5 has no Ctrl form, since Ctrl+C is the
/// clipboard everywhere, and F6 none, since Ctrl+M is Return to terminals and
/// Ctrl+I is taken.
///
/// In the capture phase: the column view would otherwise spend Tab on moving
/// between its columns before a shortcut saw it.
fn install_shortcuts(br: &Rc<Browser>) {
    let ctl = gtk::ShortcutController::new();
    ctl.set_propagation_phase(gtk::PropagationPhase::Capture);
    ctl.set_scope(gtk::ShortcutScope::Global);

    let add = |trigger: gtk::ShortcutTrigger, f: fn(&Rc<Browser>) -> bool| {
        let weak: Weak<Browser> = Rc::downgrade(br);
        let action = gtk::CallbackAction::new(move |_, _| match weak.upgrade() {
            Some(br) if f(&br) => glib::Propagation::Stop,
            _ => glib::Propagation::Proceed,
        });
        ctl.add_shortcut(gtk::Shortcut::new(Some(trigger), Some(action)));
    };
    let key = |k: gdk::Key| gtk::KeyvalTrigger::new(k, gdk::ModifierType::empty()).upcast();
    let ctrl = |k: gdk::Key| gtk::KeyvalTrigger::new(k, gdk::ModifierType::CONTROL_MASK).upcast();

    add(key(gdk::Key::Tab), |b| {
        if focus_is_editable(b) {
            return false;
        }
        b.panel(b.active().other()).column_view().grab_focus();
        true
    });
    add(key(gdk::Key::BackSpace), |b| {
        if focus_is_editable(b) {
            return false;
        }
        if let Some(p) = b.active_panel().provider() {
            p.navigate_up();
        }
        true
    });
    add(key(gdk::Key::F4), |b| b.open_selected());
    add(key(gdk::Key::F2), |b| {
        b.rename();
        true
    });
    add(
        gtk::AlternativeTrigger::new(
            gtk::KeyvalTrigger::new(gdk::Key::F10, gdk::ModifierType::SHIFT_MASK),
            gtk::KeyvalTrigger::new(gdk::Key::Menu, gdk::ModifierType::empty()),
        )
        .upcast(),
        |b| {
            let side = b.active();
            let (hit, at) = match b.panel(side).focused_entry() {
                Some((e, pos, at)) => (Some((e, pos)), at),
                None => (None, gdk::Rectangle::new(0, 0, 1, 1)),
            };
            b.popup_menu(side, hit, &at);
            true
        },
    );
    add(key(gdk::Key::F6), |b| {
        b.open_move();
        true
    });
    add(ctrl(gdk::Key::i), |b| {
        b.get_info();
        true
    });
    for trigger in [key(gdk::Key::F3), ctrl(gdk::Key::p)] {
        add(trigger, |b| {
            b.preview();
            true
        });
    }
    add(key(gdk::Key::F5), |b| {
        b.copy();
        true
    });
    for trigger in [key(gdk::Key::F7), ctrl(gdk::Key::n)] {
        add(trigger, |b| {
            b.mkdir();
            true
        });
    }
    for trigger in [key(gdk::Key::F8), key(gdk::Key::Delete), ctrl(gdk::Key::d)] {
        add(trigger, |b| {
            b.delete();
            true
        });
    }
    add(ctrl(gdk::Key::r), |b| {
        b.reload();
        true
    });

    br.content.add_controller(ctl);
}

/// The active panel's outline, and the rows' and transfer buttons' spacing.
/// The outline is an inset box shadow, not a border: a border takes layout
/// space, and the reflow when it moved was enough for the column view to drop
/// a click sequence in progress.
const CSS: &str = "\
.files-panel {
  border-radius: 6px;
}
.files-panel-active {
  box-shadow: inset 0 0 0 1px alpha(@accent_color, 0.8);
  border-radius: 6px;
}
columnview.gtkhx-files-list > listview > row > cell {
  padding-top: 3px;
  padding-bottom: 3px;
}
button.gtkhx-files-transfer {
  padding: 2px 10px;
  min-height: 24px;
}
";

/// Once per process: the style is display-wide and the same for every browser.
fn install_css() {
    thread_local! {
        static INSTALLED: Cell<bool> = const { Cell::new(false) };
    }
    if INSTALLED.replace(true) {
        return;
    }
    let Some(display) = gdk::Display::default() else {
        return;
    };
    let css = gtk::CssProvider::new();
    // load_from_string is GTK 4.12, above the bindings' floor. Deprecated
    // only when a newer binding feature is on, as the chat view's
    // accessibility turns on.
    #[allow(deprecated)]
    css.load_from_data(CSS);
    gtk::style_context_add_provider_for_display(
        &display,
        &css,
        gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );
}
