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

/// `NB_KIND_POST` from the `NB_KIND_*` enum in `news_browser.c` (FOLDER=1,
/// CATEGORY=2, POST=3). Named so the Rust and C meanings of `kind` can't
/// silently drift; only POST is constructed here (the tree builder makes post
/// rows), so it's the only member this crate needs.
pub(crate) const NB_KIND_POST: i32 = 3;

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

// -------------------------------------------------------------------------
// Category tree builder (was news_browser.c::catlist_thread_into).
// -------------------------------------------------------------------------

/// `#[repr(C)]` post record. The C `catlist_thread_into` shim fills an array of
/// these from a `struct news_group`'s posts and hands it to
/// [`hx_news_build_category_tree`]. Layout mirrors the C `struct
/// hx_news_post_data` (news_browser.c); the borrowed `const char *`s are copied
/// into the nodes during the call.
#[repr(C)]
pub struct HxNewsPostData {
    pub postid: u32,
    pub parentid: u32,
    pub subject: *const c_char,
    pub sender: *const c_char,
    pub mime_type: *const c_char,
    pub date: HxNewsDate,
}

/// A borrowed `const char *` → owned `CString`, substituting `default` when the
/// pointer is NULL — and also when it points at `""` if `empty_is_default`.
unsafe fn cstr_or(p: *const c_char, default: &str, empty_is_default: bool) -> CString {
    if p.is_null() {
        return CString::new(default).unwrap();
    }
    let c = CStr::from_ptr(p);
    if empty_is_default && c.to_bytes().is_empty() {
        CString::new(default).unwrap()
    } else {
        c.to_owned()
    }
}

/// `void hx_news_build_category_tree(GListStore *dest, const char *category_path,
/// const struct hx_news_post_data *posts, size_t count)` — build a category's
/// reply-threaded `HxNewsNode` tree and append its top-level posts to `dest`.
///
/// Replaces `news_browser.c::catlist_thread_into`: one `NB_KIND_POST` node per
/// post (subject / sender / date / mime set from `posts`), parent resolution via
/// the tested [`thread_parent_indices`], then replies attached to their parents'
/// children stores and the top-level posts appended to `dest` **last** — so each
/// has its full reply subtree before `GtkTreeListModel` makes its one-shot
/// expandability decision on the first `create_child_model` (otherwise a parent
/// whose children store is still empty becomes a permanent, non-expandable leaf).
///
/// Ownership stays inside Rust: each node is created here (one ref), appended to
/// its owning store (which takes a ref), and the transient `Vec` ref is released
/// when the vector drops — the tree survives, held by the stores.
///
/// # Safety
/// `dest` is a valid `GListStore *`; `posts` points at `count` valid records
/// whose `const char *` fields are NULL or NUL-terminated; main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_news_build_category_tree(
    dest: *mut gio::ffi::GListStore,
    category_path: *const c_char,
    posts: *const HxNewsPostData,
    count: usize,
) {
    if dest.is_null() || posts.is_null() || count == 0 {
        return;
    }
    // Defensive slice-size ceiling: `slice::from_raw_parts` is UB if the total
    // byte size exceeds `isize::MAX`. A corrupt/hostile `count` (e.g. a bogus
    // `group->post_count`) must fail closed here, before the deref — matching
    // the guard on `hx_news_thread_parent_indices`.
    if count > isize::MAX as usize / std::mem::size_of::<HxNewsPostData>() {
        return;
    }
    let dest: gio::ListStore = from_glib_none(dest);
    let path: Option<CString> = if category_path.is_null() {
        None
    } else {
        Some(CStr::from_ptr(category_path).to_owned())
    };
    let data = std::slice::from_raw_parts(posts, count);

    // Pass 1: a node per post + collect (postid, parentid) for threading.
    // subject: NULL/empty → "(no subject)"; sender: NULL → ""; mime: NULL →
    // "text/plain" (the C original's defaults).
    let mut nodes: Vec<HxNewsNode> = Vec::with_capacity(count);
    let mut links: Vec<crate::PostLink> = Vec::with_capacity(count);
    for p in data {
        let subject = cstr_or(p.subject, "(no subject)", true);
        let node = HxNewsNode::new(NB_KIND_POST, subject, path.clone());
        {
            let imp = node.imp();
            imp.postid.set(p.postid);
            imp.sender.replace(Some(cstr_or(p.sender, "", false)));
            imp.mime_type
                .replace(Some(cstr_or(p.mime_type, "text/plain", false)));
            imp.date.set(p.date);
        }
        links.push(crate::PostLink {
            postid: p.postid,
            parentid: p.parentid,
        });
        nodes.push(node);
    }

    // Pass 2: parent resolution (tested).
    let parent_idx = crate::thread_parent_indices(&links);

    // Pass 3: wire replies under their parents; defer top-level posts.
    let mut top: Vec<usize> = Vec::new();
    for (i, &pi) in parent_idx.iter().enumerate() {
        if pi >= 0 && (pi as usize) < count {
            let store = nodes[pi as usize].ensure_children();
            store.append(&nodes[i]);
        } else {
            top.push(i);
        }
    }
    // Pass 4: append top-level posts now that each subtree is fully built.
    for i in top {
        dest.append(&nodes[i]);
    }
}

#[cfg(test)]
#[path = "node_tests.rs"]
mod node_tests;
