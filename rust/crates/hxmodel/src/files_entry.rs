//! `HxFileEntry` — one row in the files browser model, as a Rust
//! glib::subclass GObject.
//!
//! An immutable value object carrying a file or folder's display name, kind
//! label, size, modified time, dir flag, and icon id, held in a `GListStore`
//! that `GtkColumnView` consumes. Both providers (local GIO + remote Hotline)
//! build these rows; the panel widget reads them through the accessors
//! and doesn't care which side produced them.
//!
//! The crate exports the `hx_file_entry_*` C ABI `files_entry.h` declares —
//! `hx_file_entry_get_type`, `hx_file_entry_new`, and the six field getters —
//! for the C providers. The Rust view reads it
//! through the methods on [`HxFileEntry`]; what a row shows is formatted in
//! `gtkhx-ui`'s `files::row`.
//!
//! The entry is immutable once constructed, so the fields need only
//! construction-time writes; there is no signal surface.

mod populate;

pub use populate::gtkhx_files_populate_from_reply;

use std::cell::{Cell, OnceCell, RefCell};
use std::ffi::{c_char, CStr, CString};
use std::os::raw::c_void;

use glib::ffi::{gboolean, GFALSE, GTRUE};
use glib::prelude::*;
use glib::subclass::prelude::*;
use glib::translate::{from_glib_borrow, Borrowed, IntoGlib, IntoGlibPtr};

/// Default icon ids for the "caller didn't classify" case. These MUST
/// stay in lockstep with the `ICON_*` #defines in `src/files.h` (the same
/// two the `hxmodel::files` icon table anchors on).
const ICON_FILE: u16 = 400;
const ICON_FOLDER: u16 = 401;

/// A borrowed empty C string for the NULL-entry accessor fallbacks
/// (mirrors the C `e ? e->name : ""`).
const EMPTY: &CStr = c"";

/// Build a `CString` from a borrowed `&str`, collapsing an interior NUL
/// (not possible from real names, defensive against hostile wire bytes)
/// to an empty string.
fn cstring_of(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new("").unwrap())
}

/// `g_utf8_collate_key` of `s`: byte-comparing two keys orders the strings
/// as `g_utf8_collate` would, without re-deriving the collation each time.
fn collate_key_of(s: &CStr) -> CString {
    unsafe {
        let key = glib::ffi::g_utf8_collate_key(s.as_ptr(), -1);
        let owned = CStr::from_ptr(key).to_owned();
        glib::ffi::g_free(key.cast());
        owned
    }
}

mod imp {
    use super::*;

    pub struct HxFileEntry {
        pub name: RefCell<CString>,
        pub kind: RefCell<CString>,
        pub is_dir: Cell<bool>,
        pub size: Cell<u64>,
        pub modified: Cell<i64>,
        pub icon_id: Cell<u16>,
        pub name_key: OnceCell<CString>,
        pub kind_key: OnceCell<CString>,
    }

    impl Default for HxFileEntry {
        fn default() -> Self {
            HxFileEntry {
                name: RefCell::new(CString::new("").unwrap()),
                kind: RefCell::new(CString::new("").unwrap()),
                is_dir: Cell::new(false),
                size: Cell::new(0),
                modified: Cell::new(0),
                icon_id: Cell::new(0),
                name_key: OnceCell::new(),
                kind_key: OnceCell::new(),
            }
        }
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxFileEntry {
        // The GType name the rest of GLib registers this under. The C side
        // never looks the type up by name (G_DECLARE_FINAL_TYPE's cast /
        // check macros use the get_type() accessor); this is just the
        // canonical registration name, matching the old C type.
        const NAME: &'static str = "HxFileEntry";
        type Type = super::HxFileEntry;
        type ParentType = glib::Object;
    }

    impl ObjectImpl for HxFileEntry {}
}

glib::wrapper! {
    /// One files-browser row (see the module docs). A `GListStore` item.
    pub struct HxFileEntry(ObjectSubclass<imp::HxFileEntry>);
}

impl HxFileEntry {
    /// Construct a row. `icon_id == 0` means "caller didn't classify" and
    /// defaults to the generic file/folder icon per `is_dir` — the same
    /// fallback both providers relied on the C constructor for.
    pub(crate) fn build(
        name: &str,
        is_dir: bool,
        size: u64,
        modified: i64,
        kind: &str,
        icon_id: u16,
    ) -> Self {
        let obj: Self = glib::Object::new();
        let imp = obj.imp();
        imp.name.replace(cstring_of(name));
        imp.kind.replace(cstring_of(kind));
        imp.is_dir.set(is_dir);
        imp.size.set(size);
        imp.modified.set(modified);
        imp.icon_id.set(if icon_id != 0 {
            icon_id
        } else if is_dir {
            ICON_FOLDER
        } else {
            ICON_FILE
        });
        obj
    }

    /// The display name (UTF-8).
    pub fn name(&self) -> String {
        self.imp().name.borrow().to_string_lossy().into_owned()
    }

    /// The name's collation key, for sorting: comparing two keys bytewise
    /// matches `g_utf8_collate` on the names. Derived on first use, since
    /// only a sort needs it, and kept — the entry never changes.
    pub fn name_collate_key(&self) -> &CStr {
        let imp = self.imp();
        imp.name_key
            .get_or_init(|| collate_key_of(&imp.name.borrow()))
    }

    /// The kind's collation key, as [`HxFileEntry::name_collate_key`].
    pub fn kind_collate_key(&self) -> &CStr {
        let imp = self.imp();
        imp.kind_key
            .get_or_init(|| collate_key_of(&imp.kind.borrow()))
    }

    /// The short kind description ("Folder", "MP3 Audio"), empty if unknown.
    pub fn kind(&self) -> String {
        self.imp().kind.borrow().to_string_lossy().into_owned()
    }

    pub fn is_dir(&self) -> bool {
        self.imp().is_dir.get()
    }

    /// Bytes for a file; for a folder, the child count when the server sent one.
    pub fn size(&self) -> u64 {
        self.imp().size.get()
    }

    /// Unix seconds, or 0 when unknown.
    pub fn modified(&self) -> i64 {
        self.imp().modified.get()
    }

    /// The Mac cicn icon ID, already defaulted to the generic file or folder.
    pub fn icon_id(&self) -> u16 {
        self.imp().icon_id.get()
    }
}

// ---- FFI: the hx_file_entry_* C ABI (matches src/files_entry.h) --------

/// Borrow a C-passed `HxFileEntry*` as a typed object **without touching
/// its refcount**. Returns None on NULL.
///
/// The getters below are a hot path — `GtkColumnView` re-snapshots them on
/// every cell bind — so this uses `from_glib_borrow` rather than
/// `from_glib_none`: `Borrowed<HxFileEntry>` is a zero-cost wrapper over
/// the raw pointer (no atomic ref/unref, no runtime GType check) that
/// derefs to `&HxFileEntry`, and cannot be cloned (which would add a ref).
/// The caller owns the reference for the whole call, so any `*const c_char`
/// returned into the object's interior stays valid (the C borrowed-return
/// contract). Same shape as `hxbridge::session_from_ptr` /
/// `HxUserRow`'s `borrow`.
///
/// # Safety
/// `e`, when non-null, must point to a live `HxFileEntry` GObject (as the
/// C ABI guarantees — the store is typed `HX_TYPE_FILE_ENTRY`).
unsafe fn borrow(e: *mut c_void) -> Option<Borrowed<HxFileEntry>> {
    if e.is_null() {
        None
    } else {
        Some(from_glib_borrow::<_, HxFileEntry>(
            e as *mut <HxFileEntry as glib::object::ObjectType>::GlibType,
        ))
    }
}

fn bool_to_gboolean(b: bool) -> gboolean {
    if b {
        GTRUE
    } else {
        GFALSE
    }
}

/// The `G_DECLARE_FINAL_TYPE` accessor. Registers the type on first call.
#[no_mangle]
pub extern "C" fn hx_file_entry_get_type() -> glib::ffi::GType {
    <HxFileEntry as StaticType>::static_type().into_glib()
}

/// Construct a row. Returns a new `HxFileEntry*` carrying one strong ref
/// the caller owns (same as the old `g_object_new`-based constructor).
/// `name` / `kind` are copied; NULL is treated as empty. See
/// [`HxFileEntry::build`] for the `icon_id == 0` fallback.
///
/// # Safety
/// `name` / `kind`, when non-null, must be valid NUL-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn hx_file_entry_new(
    name: *const c_char,
    is_dir: gboolean,
    size: u64,
    modified: i64,
    kind: *const c_char,
    icon_id: u16,
) -> *mut c_void {
    let name = if name.is_null() {
        String::new()
    } else {
        CStr::from_ptr(name).to_string_lossy().into_owned()
    };
    let kind = if kind.is_null() {
        String::new()
    } else {
        CStr::from_ptr(kind).to_string_lossy().into_owned()
    };
    let obj = HxFileEntry::build(&name, is_dir != GFALSE, size, modified, &kind, icon_id);
    // Transfer one strong ref to C (the constructor's owned ref).
    let raw: *mut glib::gobject_ffi::GObject = obj.upcast::<glib::Object>().into_glib_ptr();
    raw as *mut c_void
}

/// Display name (UTF-8), borrowed for the object's lifetime. `""` on NULL.
///
/// # Safety
/// `e` must be NULL or a live `HxFileEntry`.
#[no_mangle]
pub unsafe extern "C" fn hx_file_entry_get_name(e: *mut c_void) -> *const c_char {
    match borrow(e) {
        Some(o) => o.imp().name.borrow().as_ptr(),
        None => EMPTY.as_ptr(),
    }
}

/// Short human kind label, borrowed for the object's lifetime. `""` on NULL.
///
/// # Safety
/// `e` must be NULL or a live `HxFileEntry`.
#[no_mangle]
pub unsafe extern "C" fn hx_file_entry_get_kind(e: *mut c_void) -> *const c_char {
    match borrow(e) {
        Some(o) => o.imp().kind.borrow().as_ptr(),
        None => EMPTY.as_ptr(),
    }
}

/// TRUE for directories / Hotline folders. FALSE on NULL.
///
/// # Safety
/// `e` must be NULL or a live `HxFileEntry`.
#[no_mangle]
pub unsafe extern "C" fn hx_file_entry_is_dir(e: *mut c_void) -> gboolean {
    bool_to_gboolean(borrow(e).map(|o| o.imp().is_dir.get()).unwrap_or(false))
}

/// Size in bytes (or Hotline folder child count). 0 on NULL.
///
/// # Safety
/// `e` must be NULL or a live `HxFileEntry`.
#[no_mangle]
pub unsafe extern "C" fn hx_file_entry_get_size(e: *mut c_void) -> u64 {
    borrow(e).map(|o| o.imp().size.get()).unwrap_or(0)
}

/// Unix mtime seconds, or 0 if unknown / NULL.
///
/// # Safety
/// `e` must be NULL or a live `HxFileEntry`.
#[no_mangle]
pub unsafe extern "C" fn hx_file_entry_get_modified(e: *mut c_void) -> i64 {
    borrow(e).map(|o| o.imp().modified.get()).unwrap_or(0)
}

/// cicn icon id (see `files.h` ICON_*). 0 on NULL.
///
/// # Safety
/// `e` must be NULL or a live `HxFileEntry`.
#[no_mangle]
pub unsafe extern "C" fn hx_file_entry_get_icon_id(e: *mut c_void) -> u16 {
    borrow(e).map(|o| o.imp().icon_id.get()).unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cstr(s: &str) -> CString {
        CString::new(s).unwrap()
    }

    #[test]
    fn build_and_read_back() {
        let name = cstr("report.txt");
        let kind = cstr("Plain Text");
        unsafe {
            let e = hx_file_entry_new(name.as_ptr(), GFALSE, 1234, 42, kind.as_ptr(), 404);
            assert!(!e.is_null());
            assert_eq!(
                CStr::from_ptr(hx_file_entry_get_name(e)).to_str().unwrap(),
                "report.txt"
            );
            assert_eq!(
                CStr::from_ptr(hx_file_entry_get_kind(e)).to_str().unwrap(),
                "Plain Text"
            );
            assert_eq!(hx_file_entry_is_dir(e), GFALSE);
            assert_eq!(hx_file_entry_get_size(e), 1234);
            assert_eq!(hx_file_entry_get_modified(e), 42);
            assert_eq!(hx_file_entry_get_icon_id(e), 404);
            drop_entry(e);
        }
    }

    #[test]
    fn icon_fallback_file_and_folder() {
        unsafe {
            // icon_id 0, not a dir -> ICON_FILE
            let f = hx_file_entry_new(core::ptr::null(), GFALSE, 0, 0, core::ptr::null(), 0);
            assert_eq!(hx_file_entry_get_icon_id(f), ICON_FILE);
            // icon_id 0, dir -> ICON_FOLDER
            let d = hx_file_entry_new(core::ptr::null(), GTRUE, 0, 0, core::ptr::null(), 0);
            assert_eq!(hx_file_entry_get_icon_id(d), ICON_FOLDER);
            assert_eq!(hx_file_entry_is_dir(d), GTRUE);
            drop_entry(f);
            drop_entry(d);
        }
    }

    #[test]
    fn null_name_and_kind_read_empty() {
        unsafe {
            let e = hx_file_entry_new(core::ptr::null(), GTRUE, 7, 0, core::ptr::null(), 0);
            assert_eq!(
                CStr::from_ptr(hx_file_entry_get_name(e)).to_str().unwrap(),
                ""
            );
            assert_eq!(
                CStr::from_ptr(hx_file_entry_get_kind(e)).to_str().unwrap(),
                ""
            );
            drop_entry(e);
        }
    }

    #[test]
    fn null_entry_accessors_return_defaults() {
        unsafe {
            let n = core::ptr::null_mut();
            assert_eq!(CStr::from_ptr(hx_file_entry_get_name(n)).to_bytes(), b"");
            assert_eq!(CStr::from_ptr(hx_file_entry_get_kind(n)).to_bytes(), b"");
            assert_eq!(hx_file_entry_is_dir(n), GFALSE);
            assert_eq!(hx_file_entry_get_size(n), 0);
            assert_eq!(hx_file_entry_get_modified(n), 0);
            assert_eq!(hx_file_entry_get_icon_id(n), 0);
        }
    }

    #[test]
    fn collate_keys_order_as_collate() {
        let names = ["b", "A", "a", "Ä", "z", "10", "9", "", "émile", "Zed"];
        let entries: Vec<HxFileEntry> = names
            .iter()
            .map(|n| HxFileEntry::build(n, false, 0, 0, n, 0))
            .collect();
        for a in &entries {
            for b in &entries {
                let (an, bn) = (a.imp().name.borrow(), b.imp().name.borrow());
                let want = unsafe { glib::ffi::g_utf8_collate(an.as_ptr(), bn.as_ptr()) }.cmp(&0);
                assert_eq!(a.name_collate_key().cmp(b.name_collate_key()), want);
                assert_eq!(a.kind_collate_key().cmp(b.kind_collate_key()), want);
            }
        }
    }

    #[test]
    fn get_type_is_stable() {
        let a = hx_file_entry_get_type();
        let b = hx_file_entry_get_type();
        assert_eq!(a, b);
        assert_ne!(a, glib::Type::INVALID.into_glib());
    }

    /// Release a ref taken by `hx_file_entry_new` (the C caller's
    /// `g_object_unref`).
    unsafe fn drop_entry(e: *mut c_void) {
        let obj: glib::Object =
            glib::translate::from_glib_full(e as *mut glib::gobject_ffi::GObject);
        drop(obj);
    }
}
