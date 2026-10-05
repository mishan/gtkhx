//! What the session's news events become, through recording doubles of the
//! signal emits.

use std::cell::RefCell;
use std::ffi::{c_char, CStr};

use glib::prelude::*;
use hxmodel::news::node::{
    hx_news_node_body_fetching, hx_news_node_new, hx_news_node_set_body_fetching,
};

use super::carrier::{
    gnews_catalog_articles, gnews_catalog_free, gnews_catalog_new, gnews_folder_free,
    gnews_folder_items, gnews_folder_new, news_post_body, news_post_free, news_post_target,
};
use super::*;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum Emitted {
    File(usize, String),
    Post(usize, String),
    /// A folder carrier, and the names it holds.
    Folder(usize, Vec<String>),
    /// A catalog carrier, and the ids of the articles it holds.
    Catalog(usize, Vec<u32>),
    /// An article's text, for its node.
    Thread(usize, String),
}

thread_local! {
    static EMITTED: RefCell<Vec<Emitted>> = const { RefCell::new(Vec::new()) };
}

fn emitted() -> Vec<Emitted> {
    EMITTED.with(|e| std::mem::take(&mut *e.borrow_mut()))
}

fn emit(e: Emitted) {
    EMITTED.with(|v| v.borrow_mut().push(e));
}

pub(super) unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

unsafe fn text(p: *const c_char, len: u32) -> String {
    String::from_utf8(std::slice::from_raw_parts(p.cast::<u8>(), len as usize).to_vec()).unwrap()
}

pub(super) unsafe fn gtkhx_session_emit_news_file(
    _s: *mut c_void,
    h: *mut c_void,
    news: *const c_char,
    len: u32,
) {
    emit(Emitted::File(h as usize, text(news, len)));
}

pub(super) unsafe fn gtkhx_session_emit_news_post(
    _s: *mut c_void,
    h: *mut c_void,
    news: *const c_char,
    len: u32,
) {
    emit(Emitted::Post(h as usize, text(news, len)));
}

pub(super) unsafe fn gtkhx_session_emit_news_folder(
    _s: *mut c_void,
    h: *mut c_void,
    g: *mut c_void,
) {
    let names = gnews_folder_items(g)
        .iter()
        .map(|i| i.name.clone())
        .collect();
    gnews_folder_free(g);
    emit(Emitted::Folder(h as usize, names));
}

pub(super) unsafe fn gtkhx_session_emit_news_catalog(
    _s: *mut c_void,
    h: *mut c_void,
    g: *mut c_void,
) {
    let ids = gnews_catalog_articles(g).iter().map(|a| a.id).collect();
    gnews_catalog_free(g);
    emit(Emitted::Catalog(h as usize, ids));
}

pub(super) unsafe fn gtkhx_session_emit_news_thread(
    _s: *mut c_void,
    h: *mut c_void,
    post: *mut c_void,
) {
    let body = CStr::from_ptr(news_post_body(post))
        .to_str()
        .unwrap()
        .to_owned();
    let target = news_post_target(post);
    news_post_free(post);
    glib::gobject_ffi::g_object_unref(target.cast());
    emit(Emitted::Thread(h as usize, body));
}

/// Two connections, as sentinel pointers never dereferenced.
const A: usize = 0xA0;
const B: usize = 0xB0;

fn conn(h: usize) -> *mut c_void {
    h as *mut c_void
}

fn item(name: &str) -> NewsItem {
    NewsItem {
        name: name.into(),
        name_bytes: name.as_bytes().to_vec(),
        bundle: false,
    }
}

fn an_article(id: u32) -> Article {
    Article {
        id,
        parent: 0,
        subject: "s".into(),
        poster: "p".into(),
        year: 2026,
        seconds: 0,
        mime: b"text/plain".to_vec(),
    }
}

/// A post node, with the ref a fetch hands over.
fn node() -> *mut c_void {
    let n = unsafe { hx_news_node_new(3, c"post".as_ptr(), c"/c".as_ptr()) };
    unsafe { hx_news_node_set_body_fetching(n, 1) };
    n.cast()
}

#[test]
fn flat_news_is_its_signals_on_its_connection() {
    unsafe {
        posted(conn(A), "café\nnews");
        file(conn(B), "all of it");
    }
    assert_eq!(
        emitted(),
        [
            Emitted::Post(A, "café\nnews".into()),
            Emitted::File(B, "all of it".into())
        ]
    );
}

/// A reply goes to what asked for it on its own connection: the same trans
/// on another connection is another request.
#[test]
fn a_reply_fills_what_asked_on_its_connection_and_trans() {
    unsafe {
        let (fa, fb) = (
            gnews_folder_new(c"/".as_ptr()),
            gnews_folder_new(c"/".as_ptr()),
        );
        let cat = gnews_catalog_new(c"/c".as_ptr());
        asked(conn(A), 5, Asked::Folder(fa));
        asked(conn(B), 5, Asked::Folder(fb));
        asked(conn(A), 6, Asked::Catalog(cat));
        // A reply of another kind is not the folder's, which still waits.
        category(conn(B), 5, &[an_article(1)]);
        listing(conn(B), 5, &[item("bee")]);
        listing(conn(A), 5, &[item("ay"), item("ex")]);
        category(conn(A), 6, &[an_article(7), an_article(8)]);
        // Answered already, or never asked: nothing.
        listing(conn(A), 5, &[item("again")]);
        category(conn(B), 6, &[an_article(9)]);
    }
    assert_eq!(
        emitted(),
        [
            Emitted::Folder(B, vec!["bee".into()]),
            Emitted::Folder(A, vec!["ay".into(), "ex".into()]),
            Emitted::Catalog(A, vec![7, 8]),
        ]
    );
}

#[test]
fn an_article_s_text_reaches_its_node() {
    let n = node();
    unsafe {
        asked(conn(A), 3, Asked::Article(n));
        article(conn(A), 3, "one\ntwo");
    }
    assert_eq!(emitted(), [Emitted::Thread(A, "one\ntwo".into())]);
}

/// A refused or forgotten fetch still lets the browser go of what waits for
/// it: a listing comes empty, and an article's node may be fetched again.
#[test]
fn a_fetch_with_no_answer_lets_go() {
    let n = node();
    let keep: glib::Object =
        unsafe { glib::translate::from_glib_none(n.cast::<glib::gobject_ffi::GObject>()) };
    unsafe {
        asked(conn(A), 1, Asked::Folder(gnews_folder_new(c"/".as_ptr())));
        asked(conn(A), 2, Asked::Article(n));
        asked(
            conn(B),
            1,
            Asked::Catalog(gnews_catalog_new(c"/c".as_ptr())),
        );
        failed(conn(A), 1);
        failed(conn(A), 2);
        forget(conn(B));
        // Forgotten, so its late reply is nothing.
        category(conn(B), 1, &[an_article(1)]);
        assert_eq!(hx_news_node_body_fetching(keep.as_ptr()), 0);
    }
    assert_eq!(
        emitted(),
        [Emitted::Folder(A, vec![]), Emitted::Catalog(B, vec![])]
    );
}
