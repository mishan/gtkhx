//! `HxUserRow` — one row in the users / chat / pchat list, as a Rust
//! `glib::subclass` GObject.
//!
//! Phase R5.8. Transcribed from `users_row.c` (deleted). A `GObject` so it
//! can sit in the `GListStore` that `HxUserListView`'s `GtkColumnView`
//! consumes. It holds a *copy* of the display state the cell renderers read:
//! uid, nick_color, name, icon id, status color, and a cached foreground
//! `GdkRGBA`. Those are copied in from the borrowed `struct hx_user *` at
//! construct / `set_state` (where it's known-valid) — the row does **not**
//! store the pointer (M4b.3b-ii-B), so it can't dangle when `chat->users` is
//! retired. Every state change fires `"changed"` so the view can re-sort +
//! re-snapshot.
//!
//! The row is built + driven from Rust (`HxUserListView`), so the only C ABI
//! left is `hx_user_row_get_type` (the `G_DECLARE_FINAL_TYPE` accessor) + the
//! cell-facing field getters (`get_name` / `_icon` / `_uid` / `_foreground`).
//! Foreground computation calls the pointer-free C `user_nick_color_rgb`, and
//! `uid` / `nick_color` are copied in via the C `hx_user_uid` /
//! `hx_user_nick_color` accessors — all in `users.c` next to `hx_user`.

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
    /// Compute the row foreground from a raw nick_color + status: prefers the
    /// per-user RGB nick color, falls back to the status palette, or NULL for
    /// the regular-user (theme-default) slot. `out` is a caller buffer it may
    /// fill and return. Pointer-free (M4b.3b) so a cached nick_color drives it
    /// without a live `hx_user*`. (users.c)
    fn user_nick_color_rgb(nick_color: u32, status: u16, out: *mut Rgba) -> *const Rgba;
}

/// `HX_NICK_COLOR_NONE` (hotline.h) — the "no custom nick color" sentinel.
const HX_NICK_COLOR_NONE: u32 = 0xFFFF_FFFF;

mod imp {
    use super::*;
    use glib::subclass::Signal;
    use std::sync::OnceLock;

    pub struct HxUserRow {
        /// Cached `hx_user->uid` — the row's identity + the view's map key.
        /// The row no longer holds the `hx_user*` at all (M4b.3b-ii-B): its
        /// fields are copied in at construct / set_state from the borrowed
        /// pointer (known-valid there), and the selection path identifies the
        /// row by this uid.
        pub uid: Cell<u16>,
        /// Cached `hx_user->nick_color` (drives the foreground; refreshed on
        /// each set_state so a recolour is picked up).
        pub nick_color: Cell<u32>,
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
                uid: Cell::new(0),
                nick_color: Cell::new(HX_NICK_COLOR_NONE),
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
    /// The row's uid — the cached `hx_user->uid` (0 for a NULL user), so this
    /// no longer dereferences the borrowed pointer.
    pub(crate) fn uid_of(&self) -> u16 {
        self.imp().uid.get()
    }

    /// Run `f` with a borrowed `const char *` to the row's cached display
    /// name (valid for the closure's duration). Lets the view hand the name
    /// to `create_msgwin` on activate without dereferencing `hx_user*`.
    pub(crate) fn with_name_ptr<R>(&self, f: impl FnOnce(*const c_char) -> R) -> R {
        let name = self.imp().name.borrow();
        f(name.as_ptr())
    }

    /// Case-insensitive name comparison against another row, borrowing both
    /// names in place (no allocation) — the exact ordering the old C
    /// `cmp_name` used (ASCII lowercase, then shorter-first). The
    /// Name-column sorter runs this on every comparison, so it must not
    /// clone the name.
    pub(crate) fn cmp_name_ci(&self, other: &HxUserRow) -> std::cmp::Ordering {
        // Two distinct RefCells (different objects) → no double-borrow.
        let a = self.imp().name.borrow();
        let b = other.imp().name.borrow();
        cmp_ci_bytes(a.to_bytes(), b.to_bytes())
    }

    /// Construct a row from the member's values (M4b.4b-i): uid + display
    /// name + icon + status color + nick_color are passed by the caller — the
    /// row copies them and never sees an `hx_user*`.
    ///
    /// # Safety
    /// `nam` is NULL or a valid C string.
    pub(crate) unsafe fn new_row(
        uid: u16,
        nam: *const c_char,
        icon: u16,
        color: u16,
        nick_color: u32,
    ) -> Self {
        let obj = glib::Object::new::<HxUserRow>();
        let imp = obj.imp();
        imp.uid.set(uid);
        imp.nick_color.set(nick_color);
        imp.name.replace(cstring_from(nam));
        imp.icon.set(icon);
        imp.color.set(color);
        obj.refresh_fg();
        obj
    }

    /// In-place state mutate + fire "changed". Values only (M4b.4b-i); the uid
    /// (the row's identity) is unchanged.
    ///
    /// # Safety
    /// `nam` is NULL or a valid C string.
    pub(crate) unsafe fn set_state_row(
        &self,
        nam: *const c_char,
        icon: u16,
        color: u16,
        nick_color: u32,
    ) {
        self.set_state_rs(nam, icon, color, nick_color);
    }

    /// Fire "changed" without mutating state (avatar refresh).
    pub(crate) fn touch_row(&self) {
        self.emit_by_name::<()>("changed", &[]);
    }

    /// Recompute the cached foreground from the row's user + status.
    /// Always calls `user_nick_color_gdk`, including with a NULL user: it's
    /// NULL-safe and still returns the status-palette color (away/admin) or
    /// NULL for the theme-default slot, so a placeholder row keeps its
    /// status coloring exactly as the deleted C `hx_user_row_refresh_fg`
    /// did. NULL return → no cached fg (fall through to the theme default).
    fn refresh_fg(&self) {
        let imp = self.imp();
        let mut tmp = Rgba::default();
        // Pointer-free (M4b.3b): compute from the cached nick_color, not the
        // borrowed `user`. `recache_ids` keeps `nick_color` current.
        let ret = unsafe { user_nick_color_rgb(imp.nick_color.get(), imp.color.get(), &mut tmp) };
        if ret.is_null() {
            imp.has_fg.set(false);
        } else {
            imp.fg.set(unsafe { *ret });
            imp.has_fg.set(true);
        }
    }

    /// # Safety
    /// `nam` is NULL or a valid NUL-terminated C string.
    unsafe fn set_state_rs(&self, nam: *const c_char, icon: u16, color: u16, nick_color: u32) {
        let imp = self.imp();
        imp.name.replace(cstring_from(nam));
        imp.icon.set(icon);
        imp.color.set(color);
        imp.nick_color.set(nick_color);
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

/// Case-insensitive byte-by-byte compare — the exact ordering the old C
/// `cmp_name` used (ASCII lowercase, then shorter-first).
fn cmp_ci_bytes(a: &[u8], b: &[u8]) -> std::cmp::Ordering {
    use std::cmp::Ordering;
    let n = a.len().min(b.len());
    for i in 0..n {
        match a[i].to_ascii_lowercase().cmp(&b[i].to_ascii_lowercase()) {
            Ordering::Equal => {}
            ord => return ord,
        }
    }
    a.len().cmp(&b.len())
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

// M4b.3b-ii-B: the C-ABI `hx_user_row_new` / `_set_state` / `_touch` /
// `_get_user` are gone — no C caller remained (the Rust `HxUserListView`
// builds rows via `new_row` and drives them via `set_state_row` / `touch_row`),
// and `_get_user` handed back the borrowed `hx_user*` that the row no longer
// holds. The cell-facing getters below (`get_name` / `_icon` / `_uid` /
// `_foreground`) stay.

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
    borrow(row).uid_of()
}
