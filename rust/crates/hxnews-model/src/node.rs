//! `HxNewsNode` — one GObject per row of the 1.5 news-browser tree
//! (folder / category / post). Ported from the C `HxNewsNode` that lived in
//! `news_browser.c` (Phase R5 N2b).
//!
//! It's a pure data holder — no GTK, no async, no lifecycle beyond GObject
//! refcounting — so it's a clean leaf to move to Rust ahead of the browser's
//! GTK glue. The C browser keeps the `GtkTreeListModel` / factory / fetch
//! code and reaches the node through the `hx_news_node_*` C ABI below.
//!
//! Strings are stored as `CString` so the borrowed `const char *` getters hand
//! C a NUL-terminated pointer straight out of the node (matching the old
//! `node->name` field reads). The `children` `gio::ListStore` is created lazily
//! (`hx_news_node_ensure_children`) and owns the child nodes, exactly as the C
//! struct's field did.

use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_int, CStr, CString};

use gio::prelude::*;
use gio::subclass::prelude::*;
use glib::translate::{from_glib_none, IntoGlib, IntoGlibPtr};

/// `#[repr(C)]` mirror of C's `struct date_time` (`session.h`): a post's
/// timestamp in the parsed 3-field wire form. Copied in/out by value so the C
/// caller keeps passing `struct date_time *`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HxNewsDate {
    pub base_year: u16,
    pub pad: u16,
    pub seconds: u32,
}

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct HxNewsNode {
        /// NB_KIND_FOLDER (1) / NB_KIND_CATEGORY (2) / NB_KIND_POST (3).
        pub kind: Cell<i32>,
        /// Row label. Always set (empty CString when the C caller passed NULL).
        pub name: RefCell<CString>,
        /// Full Hotline path (folders / categories; for posts the containing
        /// category's path). NULL for the never-pathed root case.
        pub path: RefCell<Option<CString>>,
        /// Child rows, created lazily on first expansion. Owns the child nodes.
        pub children: RefCell<Option<gio::ListStore>>,
        /// TRUE once the DIRLIST / CATLIST reply has populated `children` —
        /// guards against a re-fetch on collapse + re-expand.
        pub loaded: Cell<bool>,

        // Post-specific (kind == NB_KIND_POST).
        pub postid: Cell<u32>,
        pub sender: RefCell<Option<CString>>,
        pub mime_type: RefCell<Option<CString>>,
        pub date: Cell<HxNewsDate>,
        /// Cached post body: None = not fetched, Some("") = server returned an
        /// empty body. Populated by the GETTHREAD reply.
        pub body: RefCell<Option<CString>>,
        pub body_fetching: Cell<bool>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxNewsNode {
        // Keep the historical GType name so anything that looked it up by name
        // (and the C `G_DEFINE_FINAL_TYPE` this replaces) stays consistent.
        const NAME: &'static str = "HxNewsNode";
        type Type = super::HxNewsNode;
        type ParentType = glib::Object;
    }

    impl ObjectImpl for HxNewsNode {}
}

glib::wrapper! {
    /// A news-browser tree row (folder / category / post) as a GObject list
    /// item. See the module docs.
    pub struct HxNewsNode(ObjectSubclass<imp::HxNewsNode>);
}

impl HxNewsNode {
    /// Build a node of `kind` with `name` and optional `path` (the shape the C
    /// `hx_news_node_new` constructor produced).
    pub fn new(kind: i32, name: CString, path: Option<CString>) -> Self {
        let obj: Self = glib::Object::new();
        let imp = obj.imp();
        imp.kind.set(kind);
        imp.name.replace(name);
        imp.path.replace(path);
        obj
    }

    /// The lazily-created children store, allocating it (empty) on first call —
    /// creating the store is what makes the row show a `GtkTreeExpander`.
    fn ensure_children(&self) -> gio::ListStore {
        let mut b = self.imp().children.borrow_mut();
        if b.is_none() {
            *b = Some(gio::ListStore::with_type(HxNewsNode::static_type()));
        }
        b.as_ref().unwrap().clone()
    }
}

// -------------------------------------------------------------------------
// C ABI (news_browser.c). `HxNewsNode *` crosses as a `GObject *`.
// -------------------------------------------------------------------------

/// Borrow a `HxNewsNode` from a C `GObject *` without changing its refcount
/// net (the temporary wrapper's ref is dropped at the end of `f`; the C
/// caller's own ref keeps it alive, and any borrowed pointer we return points
/// into node-owned storage that outlives the call).
unsafe fn with_node<R>(
    ptr: *mut glib::gobject_ffi::GObject,
    f: impl FnOnce(&HxNewsNode) -> R,
) -> Option<R> {
    if ptr.is_null() {
        return None;
    }
    let obj: glib::Object = from_glib_none(ptr);
    let node = obj.downcast::<HxNewsNode>().ok()?;
    Some(f(&node))
}

/// `GType hx_news_node_get_type(void)` — registers the type on first call.
#[no_mangle]
pub extern "C" fn hx_news_node_get_type() -> glib::ffi::GType {
    HxNewsNode::static_type().into_glib()
}

/// `HxNewsNode *hx_news_node_new(int kind, const char *name, const char *path)`
/// — returns a new node with one (transfer-full) reference, like the C
/// `g_object_new` constructor it replaces.
///
/// # Safety
/// `name` / `path` are NULL or valid C strings.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_new(
    kind: c_int,
    name: *const c_char,
    path: *const c_char,
) -> *mut glib::gobject_ffi::GObject {
    let name_c = if name.is_null() {
        CString::default()
    } else {
        CStr::from_ptr(name).to_owned()
    };
    let path_c = if path.is_null() {
        None
    } else {
        Some(CStr::from_ptr(path).to_owned())
    };
    let node = HxNewsNode::new(kind, name_c, path_c);
    node.upcast::<glib::Object>().into_glib_ptr()
}

// ---- scalar getters / setters -------------------------------------------

macro_rules! node_cell_get {
    ($fn:ident, $field:ident, $ret:ty, $null:expr) => {
        /// # Safety
        /// `ptr` is NULL or a valid `HxNewsNode *`.
        #[no_mangle]
        pub unsafe extern "C" fn $fn(ptr: *mut glib::gobject_ffi::GObject) -> $ret {
            with_node(ptr, |n| n.imp().$field.get() as $ret).unwrap_or($null)
        }
    };
}

node_cell_get!(hx_news_node_kind, kind, c_int, 0);
node_cell_get!(hx_news_node_postid, postid, u32, 0);

/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_loaded(ptr: *mut glib::gobject_ffi::GObject) -> glib::ffi::gboolean {
    with_node(ptr, |n| n.imp().loaded.get())
        .unwrap_or(false)
        .into_glib()
}

/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_body_fetching(
    ptr: *mut glib::gobject_ffi::GObject,
) -> glib::ffi::gboolean {
    with_node(ptr, |n| n.imp().body_fetching.get())
        .unwrap_or(false)
        .into_glib()
}

/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_set_loaded(
    ptr: *mut glib::gobject_ffi::GObject,
    loaded: glib::ffi::gboolean,
) {
    with_node(ptr, |n| n.imp().loaded.set(loaded != glib::ffi::GFALSE));
}

/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_set_body_fetching(
    ptr: *mut glib::gobject_ffi::GObject,
    fetching: glib::ffi::gboolean,
) {
    with_node(ptr, |n| {
        n.imp().body_fetching.set(fetching != glib::ffi::GFALSE)
    });
}

/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_set_postid(ptr: *mut glib::gobject_ffi::GObject, postid: u32) {
    with_node(ptr, |n| n.imp().postid.set(postid));
}

// ---- string getters / setters -------------------------------------------

/// The `name` (never NULL — empty string when unset), borrowed for the node's
/// lifetime.
///
/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_name(ptr: *mut glib::gobject_ffi::GObject) -> *const c_char {
    with_node(ptr, |n| n.imp().name.borrow().as_ptr()).unwrap_or(std::ptr::null())
}

macro_rules! node_opt_str_get {
    ($fn:ident, $field:ident) => {
        /// The field, or NULL when unset — borrowed for the node's lifetime.
        ///
        /// # Safety
        /// `ptr` is NULL or a valid `HxNewsNode *`.
        #[no_mangle]
        pub unsafe extern "C" fn $fn(ptr: *mut glib::gobject_ffi::GObject) -> *const c_char {
            with_node(ptr, |n| match &*n.imp().$field.borrow() {
                Some(cs) => cs.as_ptr(),
                None => std::ptr::null(),
            })
            .unwrap_or(std::ptr::null())
        }
    };
}

node_opt_str_get!(hx_news_node_path, path);
node_opt_str_get!(hx_news_node_sender, sender);
node_opt_str_get!(hx_news_node_mime_type, mime_type);
node_opt_str_get!(hx_news_node_body, body);

macro_rules! node_opt_str_set {
    ($fn:ident, $field:ident) => {
        /// Set the field (copies `s`; NULL clears it back to unset).
        ///
        /// # Safety
        /// `ptr` is NULL or a valid `HxNewsNode *`; `s` is NULL or a valid C string.
        #[no_mangle]
        pub unsafe extern "C" fn $fn(ptr: *mut glib::gobject_ffi::GObject, s: *const c_char) {
            with_node(ptr, |n| {
                let v = if s.is_null() {
                    None
                } else {
                    Some(CStr::from_ptr(s).to_owned())
                };
                n.imp().$field.replace(v);
            });
        }
    };
}

node_opt_str_set!(hx_news_node_set_sender, sender);
node_opt_str_set!(hx_news_node_set_mime_type, mime_type);
node_opt_str_set!(hx_news_node_set_body, body);

// ---- date ----------------------------------------------------------------

/// Copy the post date into `*out` (zero-filled when the node has none).
///
/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`; `out` is a valid `struct date_time *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_get_date(
    ptr: *mut glib::gobject_ffi::GObject,
    out: *mut HxNewsDate,
) {
    if out.is_null() {
        return;
    }
    let d = with_node(ptr, |n| n.imp().date.get()).unwrap_or_default();
    *out = d;
}

/// Set the post date from `*date` (no-op on NULL).
///
/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`; `date` is NULL or a valid
/// `struct date_time *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_set_date(
    ptr: *mut glib::gobject_ffi::GObject,
    date: *const HxNewsDate,
) {
    if date.is_null() {
        return;
    }
    with_node(ptr, |n| n.imp().date.set(*date));
}

// ---- children store ------------------------------------------------------

/// The children `GListStore`, or NULL when it hasn't been created yet.
/// Borrowed (no ref added).
///
/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_children(
    ptr: *mut glib::gobject_ffi::GObject,
) -> *mut gio::ffi::GListStore {
    with_node(ptr, |n| match &*n.imp().children.borrow() {
        Some(store) => store.as_ptr(),
        None => std::ptr::null_mut(),
    })
    .unwrap_or(std::ptr::null_mut())
}

/// The children `GListStore`, creating an empty one on first call. Borrowed
/// (no ref added).
///
/// # Safety
/// `ptr` is NULL or a valid `HxNewsNode *`.
#[no_mangle]
pub unsafe extern "C" fn hx_news_node_ensure_children(
    ptr: *mut glib::gobject_ffi::GObject,
) -> *mut gio::ffi::GListStore {
    with_node(ptr, |n| n.ensure_children().as_ptr()).unwrap_or(std::ptr::null_mut())
}

#[cfg(test)]
#[path = "node_tests.rs"]
mod node_tests;
