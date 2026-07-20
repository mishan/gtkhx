//! Tests for the `HxNewsNode` GObject + its C ABI. glib-only, headless.

use super::*;
use glib::translate::{from_glib_none, IntoGlib};
use std::ffi::{CStr, CString};

fn cs(s: &str) -> CString {
    CString::new(s).unwrap()
}

#[test]
fn new_sets_kind_name_path() {
    let n = HxNewsNode::new(2, cs("Category"), Some(cs("/a/b")));
    let imp = n.imp();
    assert_eq!(imp.kind.get(), 2);
    assert_eq!(imp.name.borrow().to_str().unwrap(), "Category");
    assert_eq!(
        imp.path.borrow().as_ref().unwrap().to_str().unwrap(),
        "/a/b"
    );
    // Unset post fields default empty.
    assert!(imp.sender.borrow().is_none());
    assert!(imp.body.borrow().is_none());
    assert!(!imp.loaded.get());
}

#[test]
fn ffi_get_type_is_stable_and_named() {
    let t1 = hx_news_node_get_type();
    let t2 = hx_news_node_get_type();
    assert_eq!(t1, t2);
    assert_ne!(t1, 0);
    assert_eq!(HxNewsNode::static_type().into_glib(), t1);
}

#[test]
fn ffi_new_and_scalar_accessors_roundtrip() {
    unsafe {
        let name = cs("Post subject");
        let path = cs("/news/cat");
        let obj = hx_news_node_new(3, name.as_ptr(), path.as_ptr());
        assert!(!obj.is_null());

        assert_eq!(hx_news_node_kind(obj), 3);
        assert_eq!(
            CStr::from_ptr(hx_news_node_name(obj)).to_str().unwrap(),
            "Post subject"
        );
        assert_eq!(
            CStr::from_ptr(hx_news_node_path(obj)).to_str().unwrap(),
            "/news/cat"
        );

        // Scalars.
        hx_news_node_set_postid(obj, 42);
        assert_eq!(hx_news_node_postid(obj), 42);
        assert_eq!(hx_news_node_loaded(obj), glib::ffi::GFALSE);
        hx_news_node_set_loaded(obj, glib::ffi::GTRUE);
        assert_eq!(hx_news_node_loaded(obj), glib::ffi::GTRUE);
        hx_news_node_set_body_fetching(obj, glib::ffi::GTRUE);
        assert_eq!(hx_news_node_body_fetching(obj), glib::ffi::GTRUE);

        glib::gobject_ffi::g_object_unref(obj);
    }
}

#[test]
fn ffi_optional_strings_null_when_unset_and_settable() {
    unsafe {
        let name = cs("x");
        let obj = hx_news_node_new(1, name.as_ptr(), std::ptr::null());
        // Folder: path was NULL → NULL; post fields unset → NULL.
        assert!(hx_news_node_path(obj).is_null());
        assert!(hx_news_node_sender(obj).is_null());
        assert!(hx_news_node_body(obj).is_null());

        let sender = cs("misha");
        hx_news_node_set_sender(obj, sender.as_ptr());
        assert_eq!(
            CStr::from_ptr(hx_news_node_sender(obj)).to_str().unwrap(),
            "misha"
        );

        // Empty body ("" != NULL — server returned an empty post).
        let empty = cs("");
        hx_news_node_set_body(obj, empty.as_ptr());
        assert!(!hx_news_node_body(obj).is_null());
        assert_eq!(
            CStr::from_ptr(hx_news_node_body(obj)).to_str().unwrap(),
            ""
        );
        // NULL clears back to unset.
        hx_news_node_set_body(obj, std::ptr::null());
        assert!(hx_news_node_body(obj).is_null());

        glib::gobject_ffi::g_object_unref(obj);
    }
}

#[test]
fn ffi_date_roundtrip() {
    unsafe {
        let name = cs("p");
        let obj = hx_news_node_new(3, name.as_ptr(), std::ptr::null());
        let d = HxNewsDate {
            base_year: 1904,
            pad: 0,
            seconds: 123456,
        };
        hx_news_node_set_date(obj, &d);
        let mut out = HxNewsDate::default();
        hx_news_node_get_date(obj, &mut out);
        assert_eq!(out.base_year, 1904);
        assert_eq!(out.seconds, 123456);
        glib::gobject_ffi::g_object_unref(obj);
    }
}

#[test]
fn ffi_children_lazy_and_typed() {
    unsafe {
        let name = cs("folder");
        let obj = hx_news_node_new(1, name.as_ptr(), std::ptr::null());
        // Not created yet.
        assert!(hx_news_node_children(obj).is_null());

        let store_ptr = hx_news_node_ensure_children(obj);
        assert!(!store_ptr.is_null());
        // Same store on a second call (no re-alloc).
        assert_eq!(hx_news_node_children(obj), store_ptr);

        // It's a ListStore of HxNewsNode; appending a child works + persists.
        let store: gio::ListStore = from_glib_none(store_ptr);
        assert_eq!(store.item_type(), HxNewsNode::static_type());
        let child = HxNewsNode::new(3, cs("reply"), None);
        store.append(&child);
        assert_eq!(store.n_items(), 1);

        glib::gobject_ffi::g_object_unref(obj);
    }
}

#[test]
fn ffi_null_ptr_is_safe() {
    unsafe {
        assert_eq!(hx_news_node_kind(std::ptr::null_mut()), 0);
        assert!(hx_news_node_name(std::ptr::null_mut()).is_null());
        assert!(hx_news_node_children(std::ptr::null_mut()).is_null());
        // setters no-op
        hx_news_node_set_loaded(std::ptr::null_mut(), glib::ffi::GTRUE);
    }
}

// ---- hx_news_build_category_tree ----------------------------------------

fn post(postid: u32, parentid: u32, subject: &CStr, sender: &CStr, mime: &CStr) -> HxNewsPostData {
    HxNewsPostData {
        postid,
        parentid,
        subject: subject.as_ptr(),
        sender: sender.as_ptr(),
        mime_type: mime.as_ptr(),
        date: HxNewsDate::default(),
    }
}

#[test]
fn build_category_tree_threads_replies_under_parents() {
    // Array (server) order: #10 top, #11 reply→#10, #12 top.
    let (s0, s1, s2) = (cs("First"), cs("Re: First"), cs("Second"));
    let (a, b, c) = (cs("alice"), cs("bob"), cs("carol"));
    let mime = cs("text/plain");
    let cat = cs("/news/general");
    let posts = [
        post(10, 0, &s0, &a, &mime),
        post(11, 10, &s1, &b, &mime),
        post(12, 0, &s2, &c, &mime),
    ];
    let dest = gio::ListStore::with_type(HxNewsNode::static_type());
    unsafe {
        hx_news_build_category_tree(dest.as_ptr(), cat.as_ptr(), posts.as_ptr(), posts.len());
    }

    // Two top-level posts, in order.
    assert_eq!(dest.n_items(), 2);
    let n0 = dest.item(0).unwrap().downcast::<HxNewsNode>().unwrap();
    let n1 = dest.item(1).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(n0.imp().postid.get(), 10);
    assert_eq!(n0.imp().name.borrow().to_str().unwrap(), "First");
    assert_eq!(n0.imp().sender.borrow().as_ref().unwrap().to_str().unwrap(), "alice");
    assert_eq!(n0.imp().path.borrow().as_ref().unwrap().to_str().unwrap(), "/news/general");
    assert_eq!(n1.imp().postid.get(), 12);

    // #10 has one reply (#11) in its children store; #12 is a leaf.
    let kids = n0.imp().children.borrow();
    let kids = kids.as_ref().expect("parent got a children store");
    assert_eq!(kids.n_items(), 1);
    let reply = kids.item(0).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(reply.imp().postid.get(), 11);
    assert_eq!(reply.imp().name.borrow().to_str().unwrap(), "Re: First");
    // #12 never had ensure_children called → no store (renders as a leaf).
    assert!(n1.imp().children.borrow().is_none());
}

#[test]
fn build_category_tree_defaults_empty_fields() {
    // NULL subject → "(no subject)"; NULL sender → ""; NULL mime → "text/plain".
    let posts = [HxNewsPostData {
        postid: 1,
        parentid: 0,
        subject: std::ptr::null(),
        sender: std::ptr::null(),
        mime_type: std::ptr::null(),
        date: HxNewsDate::default(),
    }];
    let dest = gio::ListStore::with_type(HxNewsNode::static_type());
    unsafe {
        hx_news_build_category_tree(dest.as_ptr(), std::ptr::null(), posts.as_ptr(), 1);
    }
    assert_eq!(dest.n_items(), 1);
    let n = dest.item(0).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(n.imp().name.borrow().to_str().unwrap(), "(no subject)");
    assert_eq!(n.imp().sender.borrow().as_ref().unwrap().to_str().unwrap(), "");
    assert_eq!(n.imp().mime_type.borrow().as_ref().unwrap().to_str().unwrap(), "text/plain");
}

#[test]
fn build_category_tree_empty_or_null_is_no_op() {
    let dest = gio::ListStore::with_type(HxNewsNode::static_type());
    // Bind the CStrings to locals so `post()`'s stored pointers stay valid for
    // the call. A `&cs("x")` temporary dangles at the end of the statement —
    // harmless only because count==0 returns before any deref, but fragile.
    let (subj, sndr, mime) = (cs("x"), cs("y"), cs("text/plain"));
    let posts = [post(1, 0, &subj, &sndr, &mime)];
    unsafe {
        hx_news_build_category_tree(dest.as_ptr(), std::ptr::null(), posts.as_ptr(), 0); // count 0
        hx_news_build_category_tree(dest.as_ptr(), std::ptr::null(), std::ptr::null(), 3); // null posts
    }
    assert_eq!(dest.n_items(), 0);
}

// ---- hx_news_build_dirlist_into ----------------------------------------

#[test]
fn build_dirlist_appends_folder_and_category_nodes() {
    // type==1 → folder (kind 1); anything else → category (kind 2).
    let (n0, n1) = (cs("Docs"), cs("Announcements"));
    let items = [
        HxNewsDirItem { item_type: 1, name: n0.as_ptr() },
        HxNewsDirItem { item_type: 0, name: n1.as_ptr() },
    ];
    let parent = cs("/news");
    let dest = gio::ListStore::with_type(HxNewsNode::static_type());
    unsafe {
        hx_news_build_dirlist_into(dest.as_ptr(), parent.as_ptr(), items.as_ptr(), items.len());
    }
    assert_eq!(dest.n_items(), 2);
    let a = dest.item(0).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(a.imp().kind.get(), 1);
    assert_eq!(a.imp().name.borrow().to_str().unwrap(), "Docs");
    assert_eq!(a.imp().path.borrow().as_ref().unwrap().to_str().unwrap(), "/news/Docs");
    let b = dest.item(1).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(b.imp().kind.get(), 2);
    assert_eq!(b.imp().path.borrow().as_ref().unwrap().to_str().unwrap(), "/news/Announcements");
}

#[test]
fn build_dirlist_null_parent_and_null_name() {
    // NULL parent → root "/"; NULL name → empty label; root + "" → "/".
    let items = [HxNewsDirItem { item_type: 0, name: std::ptr::null() }];
    let dest = gio::ListStore::with_type(HxNewsNode::static_type());
    unsafe {
        hx_news_build_dirlist_into(dest.as_ptr(), std::ptr::null(), items.as_ptr(), 1);
    }
    assert_eq!(dest.n_items(), 1);
    let n = dest.item(0).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(n.imp().name.borrow().to_str().unwrap(), "");
    assert_eq!(n.imp().path.borrow().as_ref().unwrap().to_str().unwrap(), "/");
}

#[test]
fn build_dirlist_empty_or_null_is_no_op() {
    let dest = gio::ListStore::with_type(HxNewsNode::static_type());
    let name = cs("x");
    let items = [HxNewsDirItem { item_type: 1, name: name.as_ptr() }];
    unsafe {
        hx_news_build_dirlist_into(dest.as_ptr(), std::ptr::null(), items.as_ptr(), 0); // count 0
        hx_news_build_dirlist_into(dest.as_ptr(), std::ptr::null(), std::ptr::null(), 3); // null items
    }
    assert_eq!(dest.n_items(), 0);
}
