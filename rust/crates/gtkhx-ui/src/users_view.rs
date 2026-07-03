//! `HxUserListView` — Phase R5.9 gtk4-rs port of the user list.
//!
//! The `GtkColumnView`-backed user list used by the standalone Users window
//! and the per-pchat sidebars. Model chain (mirrors the old C):
//!
//! ```text
//! gio::ListStore<HxUserRow>
//!   → GtkSortListModel (column-header sorter)
//!   → GtkSingleSelection
//!   → GtkColumnView (UID + [voice] + Name columns)
//! ```
//!
//! What stays C behind FFI: the custom Name cell (`users_cell.c`
//! `hx_user_cell_name_*`), the voice-indicator column (`users_voice_col.c`,
//! NULL when voice is compiled out), the right-click popover
//! (`user_popup_show`), msg-window opens (`msgwin_with_uid` /
//! `create_msgwin` / `gtkhx_chat_tabs_raise_msg`), the user-list CSS class
//! (`gtkhx_apply_userlist_style`), and the theme singleton
//! (`gtkhx_theme_get_default`). Rows are the Rust `HxUserRow` (R5.8).
//!
//! Exports the same `hx_user_list_view_*` C ABI `users_view.h` declares, so
//! `users.c` / `chat.c` / `users_bridge.c` link unchanged.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::{c_char, c_void};
use std::sync::Once;

use gtk4 as gtk;
use gtk::gio;
use gtk::glib;
use gtk::prelude::*;
use glib::subclass::prelude::*;
use glib::translate::{from_glib_full, from_glib_none, IntoGlib};

use crate::tr::tr;
use crate::user_row::HxUserRow;

/// Opaque C `session *`.
type Session = c_void;

/// Mirrors `HxUserListStyle` (users_view.h).
const STYLE_USERS: i32 = 0;

/// `GTK_INVALID_LIST_POSITION`.
const INVALID: u32 = u32::MAX;

/// qdata key (raw C `g_object_set_data`) planting the bound `GtkListItem`
/// on each cell + its row widget, so the right-click handler can recover
/// the row position. Shared verbatim with `users_voice_col.c`, so both
/// sides must use the raw C qdata API (gtk-rs `set_data` boxes the value
/// and is *not* interchangeable with C `g_object_set_data`).
const LIST_ITEM_KEY: &[u8] = b"user-list-item\0";

// ---------------------------------------------------------------------
// FFI — the C leaves this view drives.
// ---------------------------------------------------------------------

extern "C" {
    // users_cell.c — the custom Name-column cell.
    fn hx_user_cell_name_new(
        text_x_offset: i32,
        themed: glib::ffi::gboolean,
        text_outline: glib::ffi::gboolean,
        row_height: i32,
    ) -> *mut gtk::ffi::GtkWidget;
    fn hx_user_cell_name_set_row(cell: *mut gtk::ffi::GtkWidget, row: *mut c_void);

    // users_voice_col.c — voice column (NULL when HAVE_VOICE unset).
    fn gtkhx_users_voice_column_new(sess: *mut Session) -> *mut gtk::ffi::GtkColumnViewColumn;

    // gtkhx.c — apply the .gtkhx-userlist CSS class + font.
    fn gtkhx_apply_userlist_style(w: *mut gtk::ffi::GtkWidget);

    // users.c — right-click popover + the struct hx_user accessors.
    fn user_popup_show(
        anchor: *mut gtk::ffi::GtkWidget,
        user: *mut c_void,
        sess: *mut Session,
        x: f64,
        y: f64,
    );
    fn hx_user_name(user: *const c_void) -> *const c_char;
    fn hx_user_uid(user: *const c_void) -> u16;

    // msg.c / chat_tabs.c — open or raise the PM window.
    fn msgwin_with_uid(uid: u16) -> *mut c_void;
    fn create_msgwin(uid: u16, name: *mut c_char) -> *mut c_void;
    fn gtkhx_chat_tabs_raise_msg(uid: u16);

    // gtkhx_theme.c — the theming singleton (a GObject) for live rescale.
    fn gtkhx_theme_get_default() -> *mut c_void;
}

/// `*mut GtkWidget` for a gtk-rs widget.
fn wptr<W: IsA<gtk::Widget>>(w: &W) -> *mut gtk::ffi::GtkWidget {
    w.as_ref().as_ptr()
}

/// Plant the bound `GtkListItem` on `cell` and its row widget via raw C
/// qdata (see LIST_ITEM_KEY). Borrowed pointer, no destroy-notify — matches
/// the old C `stash_list_item`.
fn stash_list_item(cell: &gtk::Widget, item: &gtk::ListItem) {
    // Stash ONLY on `cell` — the widget the factory created (the
    // HxUserCellName / uid Label / voice GtkImage). Do NOT walk up and write
    // qdata onto GtkColumnView's internal cell/row widgets: doing so corrupts
    // its cell lifecycle and frees a live row on the first append, aborting in
    // gtk_list_item_base_update's accessibility update (GTK_IS_ACCESSIBLE
    // assertion). Same lesson the tracker window's POS_KEY stash follows.
    // On right-click, find_list_item walks up from the picked widget to this
    // cell, which fills its column, so coverage is unchanged in practice.
    let key = LIST_ITEM_KEY.as_ptr() as *const c_char;
    let ip = item.as_ptr() as glib::ffi::gpointer;
    unsafe {
        glib::gobject_ffi::g_object_set_data(cell.as_ptr() as *mut _, key, ip);
    }
}

/// Walk up from `from` (exclusive of `top`) to the nearest widget carrying
/// a stashed `GtkListItem`, and return it.
fn find_list_item(from: &gtk::Widget, top: &gtk::Widget) -> Option<gtk::ListItem> {
    let key = LIST_ITEM_KEY.as_ptr() as *const c_char;
    let mut w = Some(from.clone());
    while let Some(cur) = w {
        if &cur == top {
            break;
        }
        let p = unsafe { glib::gobject_ffi::g_object_get_data(cur.as_ptr() as *mut _, key) };
        if !p.is_null() {
            return Some(unsafe { from_glib_none(p as *mut gtk::ffi::GtkListItem) });
        }
        w = cur.parent();
    }
    None
}

// ---------------------------------------------------------------------
// GObject
// ---------------------------------------------------------------------

mod imp {
    use super::*;

    pub struct HxUserListView {
        pub sess: Cell<*mut Session>,
        pub store: RefCell<Option<gio::ListStore>>,
        pub selection: RefCell<Option<gtk::SingleSelection>>,
        pub column_view: RefCell<Option<gtk::ColumnView>>,
        /// O(1) hx_user* → row. Keyed on the borrowed user pointer (raw
        /// pointers hash/compare by address); the row is held with a strong
        /// ref here in addition to the store's.
        pub by_user: RefCell<HashMap<*mut c_void, HxUserRow>>,
        /// Theme singleton + "changed" handler, disconnected on dispose so
        /// a destroyed view leaves no dead handler on the process-lifetime
        /// theme object (matches the old g_signal_connect_object).
        pub theme_conn: RefCell<Option<(glib::Object, glib::SignalHandlerId)>>,
    }

    // Manual Default: the raw `*mut Session` in a Cell can't derive it.
    impl Default for HxUserListView {
        fn default() -> Self {
            Self {
                sess: Cell::new(std::ptr::null_mut()),
                store: RefCell::new(None),
                selection: RefCell::new(None),
                column_view: RefCell::new(None),
                by_user: RefCell::new(HashMap::new()),
                theme_conn: RefCell::new(None),
            }
        }
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxUserListView {
        const NAME: &'static str = "HxUserListView";
        type Type = super::HxUserListView;
        type ParentType = glib::Object;
    }

    impl ObjectImpl for HxUserListView {
        fn dispose(&self) {
            if let Some((theme, id)) = self.theme_conn.borrow_mut().take() {
                theme.disconnect(id);
            }
        }
    }
}

glib::wrapper! {
    /// The user list. A plain GObject wrapping a GtkColumnView; consumers
    /// hold it as an opaque `HxUserListView *`.
    pub struct HxUserListView(ObjectSubclass<imp::HxUserListView>);
}

impl HxUserListView {
    fn session(&self) -> *mut Session {
        self.imp().sess.get()
    }

    /// Owned handle clones (GObject ref bumps) — avoid holding a `Ref` into
    /// the imp across the borrowed-view temporary.
    fn store(&self) -> Option<gio::ListStore> {
        self.imp().store.borrow().clone()
    }
    fn selection(&self) -> Option<gtk::SingleSelection> {
        self.imp().selection.borrow().clone()
    }
    fn column_view(&self) -> Option<gtk::ColumnView> {
        self.imp().column_view.borrow().clone()
    }

    /// Look up the row bound to `user`, if any.
    fn row_for(&self, user: *mut c_void) -> Option<HxUserRow> {
        self.imp().by_user.borrow().get(&user).cloned()
    }

    /// Build the whole widget tree + model chain for `style`.
    fn build(&self, sess: *mut Session, style: i32) {
        let imp = self.imp();
        imp.sess.set(sess);

        // Style parameters. STYLE_USERS = standalone window (themed, taller
        // rows, text outline); anything else = compact chat sidebar.
        let (row_height, themed, text_outline, text_x_offset) = if style == STYLE_USERS {
            (26, true, true, 36)
        } else {
            (18, false, false, 36)
        };
        let col_uid_width = 35;

        // Model chain.
        let store = gio::ListStore::new::<HxUserRow>();
        let sort_model =
            gtk::SortListModel::new(Some(store.clone()), None::<gtk::Sorter>);
        sort_model.set_incremental(false);
        let selection = gtk::SingleSelection::new(Some(sort_model.clone()));
        selection.set_autoselect(false);
        selection.set_can_unselect(true);
        selection.set_selected(INVALID);

        let column_view = gtk::ColumnView::new(Some(selection.clone()));
        column_view.set_show_column_separators(false);
        column_view.set_show_row_separators(false);

        // UID column — right-aligned numeric label.
        let col_uid = make_uid_column(col_uid_width);
        column_view.append_column(&col_uid);

        // Voice column (C leaf) — appended between UID and Name when built
        // with voice; NULL otherwise.
        let voice_ptr = unsafe { gtkhx_users_voice_column_new(sess) };
        if !voice_ptr.is_null() {
            let col_voice: gtk::ColumnViewColumn = unsafe { from_glib_full(voice_ptr) };
            column_view.append_column(&col_voice);
        }

        // Name column — the custom C cell.
        let col_name = make_name_column(text_x_offset, themed, text_outline, row_height);
        column_view.append_column(&col_name);

        // Header sorter → sort model, default UID ascending.
        if let Some(sorter) = column_view.sorter() {
            sort_model.set_sorter(Some(&sorter));
        }
        column_view.sort_by_column(Some(&col_uid), gtk::SortType::Ascending);

        // Apply the .gtkhx-userlist CSS class (font/fg/bg tracking).
        unsafe { gtkhx_apply_userlist_style(wptr(&column_view)) };
        install_padding_css();

        imp.store.replace(Some(store));
        imp.selection.replace(Some(selection));
        imp.column_view.replace(Some(column_view.clone()));

        // Double-click / Enter → open the PM window. Capture a WEAK self:
        // the view holds a strong ref to column_view, which owns this
        // closure, so a strong self clone here would form a
        // view → column_view → closure → view reference cycle and leak the
        // whole graph when C unrefs the view.
        {
            let weak = self.downgrade();
            column_view.connect_activate(move |_, pos| {
                if let Some(this) = weak.upgrade() {
                    this.on_activate(pos);
                }
            });
        }

        // Right-click → user popover. Capture phase, same as the C gesture.
        // Weak self again — the gesture controller is owned by column_view,
        // so a strong clone would form the same cycle as connect_activate.
        if !sess.is_null() {
            let rclick = gtk::GestureClick::new();
            rclick.set_button(gtk::gdk::BUTTON_SECONDARY);
            rclick.set_propagation_phase(gtk::PropagationPhase::Capture);
            let weak = self.downgrade();
            rclick.connect_pressed(move |g, _n, x, y| {
                if let Some(this) = weak.upgrade() {
                    this.on_secondary_press(g, x, y);
                }
            });
            column_view.add_controller(rclick);
        }

        // Live rescale on theme changes (themed views only).
        if themed {
            let theme: glib::Object = unsafe {
                from_glib_none(gtkhx_theme_get_default() as *mut glib::gobject_ffi::GObject)
            };
            let cv_weak = column_view.downgrade();
            let id = theme.connect_local("changed", false, move |_| {
                if let Some(cv) = cv_weak.upgrade() {
                    cv.queue_resize();
                    cv.queue_draw();
                }
                None
            });
            imp.theme_conn.replace(Some((theme, id)));
        }
    }

    fn on_activate(&self, pos: u32) {
        let Some(sel) = self.selection() else {
            return;
        };
        let Some(row) = sel.item(pos).and_downcast::<HxUserRow>() else {
            return;
        };
        let user = row.user_ptr();
        if user.is_null() {
            return;
        }
        let uid = unsafe { hx_user_uid(user as *const c_void) };
        if unsafe { msgwin_with_uid(uid) }.is_null() {
            // hx_user_name returns a borrowed const char*; create_msgwin
            // takes char* (it copies the name), so cast for the FFI call.
            let name = unsafe { hx_user_name(user as *const c_void) };
            unsafe { create_msgwin(uid, name as *mut c_char) };
        } else {
            unsafe { gtkhx_chat_tabs_raise_msg(uid) };
        }
    }

    fn on_secondary_press(&self, gesture: &gtk::GestureClick, x: f64, y: f64) {
        let sess = self.imp().sess.get();
        if sess.is_null() {
            return;
        }
        let Some(cv) = self.column_view() else {
            return;
        };
        let cv_widget: gtk::Widget = cv.clone().upcast();

        // Which row was clicked: pick the deepest widget, walk up to a
        // stashed GtkListItem, take its live position.
        let Some(picked) = cv.pick(x, y, gtk::PickFlags::DEFAULT) else {
            return;
        };
        let Some(item) = find_list_item(&picked, &cv_widget) else {
            return;
        };
        let pos = item.position();
        if pos == INVALID {
            return;
        }
        let _ = gesture;

        let Some(sel) = self.selection() else {
            return;
        };
        // Select first so the toolbar buttons + menu see the right row.
        sel.set_selected(pos);
        let Some(row) = sel.item(pos).and_downcast::<HxUserRow>() else {
            return;
        };
        let user = row.user_ptr();
        if user.is_null() {
            return;
        }
        unsafe { user_popup_show(wptr(&cv_widget), user, sess, x, y) };
    }
}

// ---------------------------------------------------------------------
// Column factories.
// ---------------------------------------------------------------------

fn make_uid_column(fixed_width: i32) -> gtk::ColumnViewColumn {
    let factory = gtk::SignalListItemFactory::new();
    factory.connect_setup(|_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        let lbl = gtk::Label::new(None);
        lbl.set_xalign(1.0);
        lbl.set_margin_start(6);
        lbl.set_margin_end(6);
        item.set_child(Some(&lbl));
    });
    factory.connect_bind(|_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        let Some(lbl) = item.child().and_downcast::<gtk::Label>() else {
            return;
        };
        stash_list_item(lbl.upcast_ref(), item);
        match item.item().and_downcast::<HxUserRow>() {
            Some(row) => lbl.set_text(&row.uid_of().to_string()),
            None => lbl.set_text(""),
        }
    });

    let col = gtk::ColumnViewColumn::new(Some(&tr("UID")), Some(factory));
    col.set_fixed_width(fixed_width);
    col.set_resizable(true);

    let sorter = gtk::CustomSorter::new(|a, b| {
        let ua = a.downcast_ref::<HxUserRow>().map(|r| r.uid_of()).unwrap_or(0);
        let ub = b.downcast_ref::<HxUserRow>().map(|r| r.uid_of()).unwrap_or(0);
        ua.cmp(&ub).into()
    });
    col.set_sorter(Some(&sorter));
    col
}

fn make_name_column(
    text_x_offset: i32,
    themed: bool,
    text_outline: bool,
    row_height: i32,
) -> gtk::ColumnViewColumn {
    let factory = gtk::SignalListItemFactory::new();
    factory.connect_setup(move |_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        // The custom C cell. hx_user_cell_name_new returns a floating
        // widget; from_glib_none refs it (leaving it floating) and
        // set_child sinks it — the list item then owns it.
        let ptr = unsafe {
            hx_user_cell_name_new(
                text_x_offset,
                themed.into_glib(),
                text_outline.into_glib(),
                row_height,
            )
        };
        let cell: gtk::Widget = unsafe { from_glib_none(ptr) };
        item.set_child(Some(&cell));
    });
    factory.connect_bind(|_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        let Some(cell) = item.child() else {
            return;
        };
        stash_list_item(&cell, item);
        let row_ptr = item
            .item()
            .and_downcast::<HxUserRow>()
            .map(|r| r.as_ptr() as *mut c_void)
            .unwrap_or(std::ptr::null_mut());
        unsafe { hx_user_cell_name_set_row(wptr(&cell), row_ptr) };
    });
    factory.connect_unbind(|_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        if let Some(cell) = item.child() {
            unsafe { hx_user_cell_name_set_row(wptr(&cell), std::ptr::null_mut()) };
        }
    });

    let col = gtk::ColumnViewColumn::new(Some(&tr("Name")), Some(factory));
    col.set_expand(true);
    col.set_resizable(true);

    // Compare borrowed names in place — no per-comparison allocation.
    let sorter = gtk::CustomSorter::new(|a, b| {
        match (a.downcast_ref::<HxUserRow>(), b.downcast_ref::<HxUserRow>()) {
            (Some(ra), Some(rb)) => ra.cmp_name_ci(rb).into(),
            _ => gtk::Ordering::Equal,
        }
    });
    col.set_sorter(Some(&sorter));
    col
}

/// One-shot global CSS provider stripping Adwaita's columnview row/cell
/// padding on the .gtkhx-userlist class so measure-returned row heights
/// take effect. Matches the C static provider.
fn install_padding_css() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let p = gtk::CssProvider::new();
        // load_from_string is gtk 4.12; the crate targets v4_10, so use the
        // (deprecated-in-4.12 but present) load_from_data.
        #[allow(deprecated)]
        p.load_from_data(
            ".gtkhx-userlist > listview > row,\
             .gtkhx-userlist > listview > row > cell {\
               min-height: 0;\
               padding-top: 0;\
               padding-bottom: 0;\
             }",
        );
        if let Some(display) = gtk::gdk::Display::default() {
            gtk::style_context_add_provider_for_display(
                &display,
                &p,
                gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
            );
        }
    });
}

// ---------------------------------------------------------------------
// Helpers for the C ABI.
// ---------------------------------------------------------------------

/// Borrow a C-passed `HxUserListView *` without touching its refcount.
///
/// # Safety
/// `v` is a valid `HxUserListView *`.
unsafe fn borrow(v: *mut c_void) -> glib::translate::Borrowed<HxUserListView> {
    glib::translate::from_glib_borrow::<_, HxUserListView>(
        v as *mut <HxUserListView as glib::object::ObjectType>::GlibType,
    )
}

// ---------------------------------------------------------------------
// C ABI (mirrors users_view.h).
// ---------------------------------------------------------------------

/// The `G_DECLARE_FINAL_TYPE` accessor.
#[no_mangle]
pub extern "C" fn hx_user_list_view_get_type() -> glib::ffi::GType {
    <HxUserListView as StaticType>::static_type().into_glib()
}

/// Construct a fresh view over borrowed `sess`. Returns transfer-full.
///
/// # Safety
/// `sess` is a valid `session *` (or NULL). Main-thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_new(sess: *mut c_void, style: i32) -> *mut c_void {
    crate::ensure_gtk_init();
    let obj = glib::Object::new::<HxUserListView>();
    obj.build(sess, style);
    // Transfer-full: hand our owned ref to C, leak the Rust wrapper.
    let raw = obj.as_ptr() as *mut c_void;
    std::mem::forget(obj);
    raw
}

/// The GtkColumnView widget to pack (borrowed).
///
/// # Safety
/// `v` is NULL or a valid `HxUserListView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_get_widget(v: *mut c_void) -> *mut gtk::ffi::GtkWidget {
    if v.is_null() {
        return std::ptr::null_mut();
    }
    match borrow(v).column_view() {
        Some(cv) => wptr(&cv),
        None => std::ptr::null_mut(),
    }
}

/// Add (or refresh-if-present) a row for `user`.
///
/// # Safety
/// `v` NULL or valid; `user` a borrowed `hx_user *`; `nam` NULL or a C string.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_add(
    v: *mut c_void,
    user: *mut c_void,
    nam: *const c_char,
    icon: u16,
    color: u16,
) {
    if v.is_null() || user.is_null() {
        return;
    }
    let view = borrow(v);
    // Already present → treat as an in-place refresh (bug-shaped double add).
    if let Some(row) = view.row_for(user) {
        row.set_state_row(nam, icon, color);
        return;
    }
    let row = HxUserRow::new_row(user, nam, icon, color);
    view.imp().by_user.borrow_mut().insert(user, row.clone());
    if let Some(store) = view.store() {
        store.append(&row);
    }
}

/// Remove the row for `user`.
///
/// # Safety
/// `v` NULL or valid; `user` a borrowed `hx_user *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_remove(v: *mut c_void, user: *mut c_void) {
    if v.is_null() || user.is_null() {
        return;
    }
    let view = borrow(v);
    let removed = view.imp().by_user.borrow_mut().remove(&user);
    let Some(row) = removed else {
        return;
    };
    if let Some(store) = view.store() {
        if let Some(pos) = store.find(&row) {
            store.remove(pos);
        }
    }
}

/// Update `user`'s row state (name / icon / color).
///
/// # Safety
/// `v` NULL or valid; `user` a borrowed `hx_user *`; `nam` NULL or a C string.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_update(
    v: *mut c_void,
    user: *mut c_void,
    nam: *const c_char,
    icon: u16,
    color: u16,
) {
    if v.is_null() || user.is_null() {
        return;
    }
    let view = borrow(v);
    match view.row_for(user) {
        Some(row) => row.set_state_row(nam, icon, color),
        // Update for an unknown user → treat as add (missed create race).
        None => hx_user_list_view_add(v, user, nam, icon, color),
    }
}

/// Re-fire the row's "changed" so its cell re-resolves the GIF avatar.
///
/// # Safety
/// `v` NULL or valid; `user` a borrowed `hx_user *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_refresh_avatar(v: *mut c_void, user: *mut c_void) {
    if v.is_null() || user.is_null() {
        return;
    }
    if let Some(row) = borrow(v).row_for(user) {
        row.touch_row();
    }
}

/// Drop all rows.
///
/// # Safety
/// `v` NULL or a valid `HxUserListView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_clear(v: *mut c_void) {
    if v.is_null() {
        return;
    }
    let view = borrow(v);
    if let Some(store) = view.store() {
        store.remove_all();
    }
    view.imp().by_user.borrow_mut().clear();
}

/// The `struct hx_user *` under the live selection, or NULL.
///
/// # Safety
/// `v` NULL or a valid `HxUserListView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_get_selected_user(v: *mut c_void) -> *mut c_void {
    if v.is_null() {
        return std::ptr::null_mut();
    }
    let Some(sel) = borrow(v).selection() else {
        return std::ptr::null_mut();
    };
    let pos = sel.selected();
    if pos == INVALID {
        return std::ptr::null_mut();
    }
    match sel.item(pos).and_downcast::<HxUserRow>() {
        Some(row) => row.user_ptr(),
        None => std::ptr::null_mut(),
    }
}

/// The borrowed session this view was built against.
///
/// # Safety
/// `v` NULL or a valid `HxUserListView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_get_session(v: *mut c_void) -> *mut c_void {
    if v.is_null() {
        return std::ptr::null_mut();
    }
    borrow(v).session()
}

/// Queue a redraw so cells re-run with the current user-list font.
///
/// # Safety
/// `v` NULL or a valid `HxUserListView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_list_view_refresh_font(v: *mut c_void) {
    if v.is_null() {
        return;
    }
    if let Some(cv) = borrow(v).column_view() {
        cv.queue_draw();
    }
}
