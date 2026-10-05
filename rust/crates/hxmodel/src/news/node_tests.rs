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
        assert_eq!(CStr::from_ptr(hx_news_node_body(obj)).to_str().unwrap(), "");
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

// ---- hx_news_build_category_tree_from_articles -----------------------------

use hxsession::{Article, NewsItem};

fn article(id: u32, parent: u32, subject: &str, poster: &str, mime: &str) -> Article {
    Article {
        id,
        parent,
        subject: subject.into(),
        poster: poster.into(),
        year: 0,
        seconds: 0,
        mime: mime.as_bytes().to_vec(),
    }
}

#[test]
fn build_from_articles_threads_and_defaults() {
    // #10 top, #11 reply→#10, #12 top with empty fields + no type → defaults.
    let articles = [
        article(10, 0, "First", "alice", "text/plain"),
        article(11, 10, "Re: First", "bob", "text/html"),
        article(12, 0, "", "", ""),
    ];
    let cat = cs("/news/general");
    let dest = gio::ListStore::with_type(HxNewsNode::static_type());
    unsafe {
        hx_news_build_category_tree_from_articles(dest.as_ptr(), cat.as_ptr(), &articles);
    }

    // #10 and #12 are top-level; #11 is a reply under #10.
    assert_eq!(dest.n_items(), 2);
    let n0 = dest.item(0).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(n0.imp().postid.get(), 10);
    assert_eq!(n0.imp().name.borrow().to_str().unwrap(), "First");
    assert_eq!(
        n0.imp().sender.borrow().as_ref().unwrap().to_str().unwrap(),
        "alice"
    );
    assert_eq!(
        n0.imp().path.borrow().as_ref().unwrap().to_str().unwrap(),
        "/news/general"
    );
    assert_eq!(
        n0.imp()
            .mime_type
            .borrow()
            .as_ref()
            .unwrap()
            .to_str()
            .unwrap(),
        "text/plain"
    );
    let kids = n0.imp().children.borrow();
    let kids = kids.as_ref().expect("parent got a children store");
    assert_eq!(kids.n_items(), 1);
    let reply = kids.item(0).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(reply.imp().postid.get(), 11);
    assert_eq!(
        reply
            .imp()
            .mime_type
            .borrow()
            .as_ref()
            .unwrap()
            .to_str()
            .unwrap(),
        "text/html"
    );

    // #12: empty subject/sender + no type → the defaults.
    let n2 = dest.item(1).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(n2.imp().postid.get(), 12);
    assert_eq!(n2.imp().name.borrow().to_str().unwrap(), "(no subject)");
    assert_eq!(
        n2.imp().sender.borrow().as_ref().unwrap().to_str().unwrap(),
        ""
    );
    assert_eq!(
        n2.imp()
            .mime_type
            .borrow()
            .as_ref()
            .unwrap()
            .to_str()
            .unwrap(),
        "text/plain"
    );
    assert!(n2.imp().children.borrow().is_none());
}

// ---- hx_news_build_dirlist_from_items -------------------------------------

#[test]
fn build_dirlist_labels_by_name_and_paths_by_the_server_s_bytes() {
    let items = [
        NewsItem {
            name: "Docs".into(),
            name_bytes: b"Docs".to_vec(),
            bundle: true,
        },
        // Mac Roman é: shown decoded, named back as sent.
        NewsItem {
            name: "René".into(),
            name_bytes: b"Ren\x8E".to_vec(),
            bundle: false,
        },
    ];
    let parent = cs("/news");
    let dest = gio::ListStore::with_type(HxNewsNode::static_type());
    unsafe {
        hx_news_build_dirlist_from_items(dest.as_ptr(), parent.as_ptr(), &items);
    }
    assert_eq!(dest.n_items(), 2);
    let a = dest.item(0).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(a.imp().kind.get(), 1); // folder
    assert_eq!(a.imp().name.borrow().to_str().unwrap(), "Docs");
    assert_eq!(
        a.imp().path.borrow().as_ref().unwrap().to_str().unwrap(),
        "/news/Docs"
    );
    let b = dest.item(1).unwrap().downcast::<HxNewsNode>().unwrap();
    assert_eq!(b.imp().kind.get(), 2); // category
    assert_eq!(b.imp().name.borrow().to_str().unwrap(), "René");
    assert_eq!(
        b.imp().path.borrow().as_ref().unwrap().as_bytes(),
        b"/news/Ren\x8E"
    );
}
