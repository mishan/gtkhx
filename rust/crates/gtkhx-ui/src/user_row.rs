//! `HxUserRow` — one row in the users / chat / pchat list, as a Rust
//! `glib::subclass` GObject.
//!
//! Phase R5.8. Transcribed from `users_row.c` (deleted). A `GObject` so it
//! can sit in the `GListStore` that `HxUserListView`'s `GtkColumnView`
//! consumes. It wraps a *borrowed* `struct hx_user *` (still owned by the
//! per-chat `chat->users` hash table — the row never frees it) plus the
//! display state the cell renderers read: name, icon id, status color, and
//! a cached foreground `GdkRGBA`. Every state change fires `"changed"` so
//! the view can re-sort + re-snapshot.
//!
//! The C ABI is preserved exactly — `hx_user_row_get_type` (the
//! `G_DECLARE_FINAL_TYPE` accessor), `hx_user_row_new`,
//! `hx_user_row_set_state` / `_touch`, and the field getters — so
//! `users_view.c` (still C) links unchanged. Foreground computation still
//! calls the C `user_nick_color_gdk`, and the uid comes from the C
//! `hx_user_uid` accessor; both live in `users.c` next to the `hx_user`
//! model they read.

use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_void, CStr, CString};

use glib::prelude::*;
use glib::subclass::prelude::*;
use glib::translate::IntoGlib;

/// A valid empty C string (`""`), for the NULL-row name path.
const EMPTY_CSTR: &[u8] = b"\0";

/// Layout-compatible mirror of `GdkRGBA` (four `f32`s). Used for both the
/// FFI to `user_nick_color_gdk` and the cached foreground the C cell
/// renderer reads back via `hx_user_row_get_foreground`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Rgba {
    red: f32,
    green: f32,
    blue: f32,
    alpha: f32,
}

extern "C" {
    /// Compute the row foreground: prefers the per-user RGB nick color,
    /// falls back to the status palette, or NULL for the regular-user
    /// (theme-default) slot. `out` is a caller buffer it may fill and
    /// return. (users.c)
    fn user_nick_color_gdk(user: *const c_void, status: u16, out: *mut Rgba) -> *const Rgba;
    /// `user->uid`, or 0 if NULL. (users.c) — keeps the `struct hx_user`
    /// layout on the C side rather than pinning its field offsets here.
    fn hx_user_uid(user: *const c_void) -> u16;
}

mod imp {
    use super::*;
    use glib::subclass::Signal;
    use std::sync::OnceLock;

    pub struct HxUserRow {
        /// Borrowed `struct hx_user *` — never freed by the row.
        pub user: Cell<*mut c_void>,
        /// Owned NUL-terminated display name (verbatim bytes).
        pub name: RefCell<CString>,
        pub icon: Cell<u16>,
        pub color: Cell<u16>,
        /// Cached foreground + validity. `as_ptr()` on the `Cell` gives the
        /// C cell renderer a stable interior pointer, matching the old
        /// `&r->fg`.
        pub fg: Cell<Rgba>,
        pub has_fg: Cell<bool>,
    }

    // Manual Default — can't derive because `*mut c_void` (the borrowed
    // user pointer) doesn't implement Default. ObjectSubclass requires the
    // instance struct be Default-constructible.
    impl Default for HxUserRow {
        fn default() -> Self {
            Self {
                user: Cell::new(std::ptr::null_mut()),
                name: RefCell::new(CString::default()),
                icon: Cell::new(0),
                color: Cell::new(0),
                fg: Cell::new(Rgba::default()),
                has_fg: Cell::new(false),
            }
        }
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxUserRow {
        const NAME: &'static str = "HxUserRow";
        type Type = super::HxUserRow;
        type ParentType = glib::Object;
    }

    impl ObjectImpl for HxUserRow {
        fn signals() -> &'static [Signal] {
            // "changed": emitted on every set_state / touch. HxUserListView
            // connects to bump its sort model and re-snapshot the cell.
            static SIGNALS: OnceLock<Vec<Signal>> = OnceLock::new();
            SIGNALS.get_or_init(|| vec![Signal::builder("changed").build()])
        }
    }
}

glib::wrapper! {
    /// A borrowed-`hx_user` row model. See module docs.
    pub struct HxUserRow(ObjectSubclass<imp::HxUserRow>);
}

impl HxUserRow {
    /// Recompute the cached foreground from the row's user + status.
    /// NULL user → no cached fg (theme default), matching the C guard-free
    /// path for the normal non-NULL case while staying safe for the
    /// documented placeholder case.
    fn refresh_fg(&self) {
        let imp = self.imp();
        let user = imp.user.get();
        if user.is_null() {
            imp.has_fg.set(false);
            return;
        }
        let mut tmp = Rgba::default();
        let ret = unsafe { user_nick_color_gdk(user, imp.color.get(), &mut tmp) };
        if ret.is_null() {
            imp.has_fg.set(false);
        } else {
            imp.fg.set(unsafe { *ret });
            imp.has_fg.set(true);
        }
    }

    /// # Safety
    /// `nam` is NULL or a valid NUL-terminated C string.
    unsafe fn set_state_rs(&self, nam: *const c_char, icon: u16, color: u16) {
        let imp = self.imp();
        imp.name.replace(cstring_from(nam));
        imp.icon.set(icon);
        imp.color.set(color);
        self.refresh_fg();
        self.emit_by_name::<()>("changed", &[]);
    }
}

/// C `char *` → owned `CString`, preserving bytes verbatim (empty on NULL).
///
/// # Safety
/// `p` is NULL or a valid NUL-terminated C string.
unsafe fn cstring_from(p: *const c_char) -> CString {
    if p.is_null() {
        CString::default()
    } else {
        CStr::from_ptr(p).to_owned()
    }
}

/// Borrow a C-passed `HxUserRow *` without touching its refcount.
///
/// # Safety
/// `row` is a valid `HxUserRow *` (non-NULL checked by callers).
unsafe fn borrow(row: *mut c_void) -> glib::translate::Borrowed<HxUserRow> {
    glib::translate::from_glib_borrow::<_, HxUserRow>(
        row as *mut <HxUserRow as glib::object::ObjectType>::GlibType,
    )
}

// ----------------------------------------------------------------------
// C ABI (mirrors users_row.h)
// ----------------------------------------------------------------------

/// The `G_DECLARE_FINAL_TYPE` accessor.
#[no_mangle]
pub extern "C" fn hx_user_row_get_type() -> glib::ffi::GType {
    <HxUserRow as StaticType>::static_type().into_glib()
}

/// Construct a row over borrowed `user`. Returns transfer-full (one owned
/// ref), matching `g_object_new`; `users_view.c` appends it to the store
/// then unrefs.
///
/// # Safety
/// `user` is a valid `struct hx_user *` (or NULL for a placeholder);
/// `nam` is NULL or a valid C string. Main-thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_new(
    user: *mut c_void,
    nam: *const c_char,
    icon: u16,
    color: u16,
) -> *mut c_void {
    let obj = glib::Object::new::<HxUserRow>();
    let imp = obj.imp();
    imp.user.set(user);
    imp.name.replace(cstring_from(nam));
    imp.icon.set(icon);
    imp.color.set(color);
    obj.refresh_fg();
    // Transfer-full: hand our owned ref to C and leak the Rust wrapper so
    // it doesn't unref on drop (mirrors g_object_new's returned ref).
    let raw = obj.as_ptr() as *mut c_void;
    std::mem::forget(obj);
    raw
}

/// In-place mutate + fire "changed".
///
/// # Safety
/// `row` is NULL or a valid `HxUserRow *`; `nam` NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_set_state(
    row: *mut c_void,
    nam: *const c_char,
    icon: u16,
    color: u16,
) {
    if row.is_null() {
        return;
    }
    borrow(row).set_state_rs(nam, icon, color);
}

/// Fire "changed" without mutating state (e.g. a GIF avatar landed).
///
/// # Safety
/// `row` is NULL or a valid `HxUserRow *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_touch(row: *mut c_void) {
    if row.is_null() {
        return;
    }
    borrow(row).emit_by_name::<()>("changed", &[]);
}

/// # Safety
/// `row` is NULL or a valid `HxUserRow *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_get_user(row: *mut c_void) -> *mut c_void {
    if row.is_null() {
        return std::ptr::null_mut();
    }
    borrow(row).imp().user.get()
}

/// Returns a `const char *` owned by the row (valid until the next
/// set_state); caller must not free.
///
/// # Safety
/// `row` is NULL or a valid `HxUserRow *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_get_name(row: *mut c_void) -> *const c_char {
    if row.is_null() {
        return EMPTY_CSTR.as_ptr() as *const c_char;
    }
    // The CString buffer lives in the row's RefCell; its data pointer is
    // stable until name is replaced (next set_state), matching the C
    // g_strdup lifetime.
    borrow(row).imp().name.borrow().as_ptr()
}

/// # Safety
/// `row` is NULL or a valid `HxUserRow *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_get_icon(row: *mut c_void) -> u16 {
    if row.is_null() {
        return 0;
    }
    borrow(row).imp().icon.get()
}

/// # Safety
/// `row` is NULL or a valid `HxUserRow *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_get_color(row: *mut c_void) -> u16 {
    if row.is_null() {
        return 0;
    }
    borrow(row).imp().color.get()
}

/// Returns the cached foreground as `const GdkRGBA *`, or NULL for the
/// regular-user (theme-default) slot.
///
/// # Safety
/// `row` is NULL or a valid `HxUserRow *`. The returned pointer is valid
/// until the row's foreground is recomputed (next set_state) or the row
/// finalizes.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_get_foreground(row: *mut c_void) -> *const c_void {
    if row.is_null() {
        return std::ptr::null();
    }
    let b = borrow(row);
    let imp = b.imp();
    if imp.has_fg.get() {
        imp.fg.as_ptr() as *const c_void
    } else {
        std::ptr::null()
    }
}

/// # Safety
/// `row` is NULL or a valid `HxUserRow *`.
#[no_mangle]
pub unsafe extern "C" fn hx_user_row_get_uid(row: *mut c_void) -> u16 {
    if row.is_null() {
        return 0;
    }
    let user = borrow(row).imp().user.get();
    if user.is_null() {
        0
    } else {
        hx_user_uid(user)
    }
}
