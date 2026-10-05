//! News, as the session reads it: what is added to 1.2 flat news, and the
//! replies to the requests `send::news` makes, each matched by its trans to
//! what asked for it.
//!
//! A threaded-news reply reaches the browser as it always did: the folder or
//! catalog carrier the browser made, now holding what the session read, or
//! the post with its text, on `news-folder`, `news-catalog` and
//! `news-thread`; the browser's `gnews_browser_handle_*` reads and frees
//! them.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;

use hxsession::{Article, NewsItem};

pub mod carrier;
use carrier::{
    gnews_catalog_set_articles, gnews_folder_set_items, news_post_fetch_failed, news_post_new,
};

#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_news_catalog, gtkhx_session_emit_news_file, gtkhx_session_emit_news_folder,
    gtkhx_session_emit_news_post, gtkhx_session_emit_news_thread, gtkhx_session_get_default,
};
#[cfg(test)]
use tests::{
    gtkhx_session_emit_news_catalog, gtkhx_session_emit_news_file, gtkhx_session_emit_news_folder,
    gtkhx_session_emit_news_post, gtkhx_session_emit_news_thread, gtkhx_session_get_default,
};

/// What a threaded-news fetch in flight is answered into.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Asked {
    /// A bundle's listing, into its `gnews_folder` carrier.
    Folder(*mut c_void),
    /// A category's articles, into its `gnews_catalog` carrier.
    Catalog(*mut c_void),
    /// An article's text, for the `HxNewsNode` whose ref it holds.
    Article(*mut c_void),
}

thread_local! {
    /// The fetches in flight, by connection and trans.
    static ASKED: RefCell<HashMap<(usize, u32), Asked>> = RefCell::new(HashMap::new());
}

/// A fetch goes out on `trans`, its reply to go into `what`.
pub(crate) fn asked(htlc: *mut c_void, trans: u32, what: Asked) {
    ASKED.with(|a| a.borrow_mut().insert((htlc as usize, trans), what));
}

/// What asked on `trans`, when `is` says the reply is for it; left waiting
/// otherwise, for the reply that is.
pub(crate) fn answered(htlc: *mut c_void, trans: u32, is: fn(&Asked) -> bool) -> Option<Asked> {
    ASKED.with(|a| {
        let mut a = a.borrow_mut();
        let key = (htlc as usize, trans);
        a.get(&key).filter(|w| is(w))?;
        a.remove(&key)
    })
}

/// Let go of what `htlc` asked for before: a new connection numbers its
/// requests afresh, and those replies are never coming.
///
/// # Safety
/// Main thread.
pub(crate) unsafe fn forget(htlc: *mut c_void) {
    let gone: Vec<Asked> = ASKED.with(|a| {
        let mut a = a.borrow_mut();
        let keys: Vec<_> = a
            .keys()
            .filter(|(h, _)| *h == htlc as usize)
            .copied()
            .collect();
        keys.into_iter().filter_map(|k| a.remove(&k)).collect()
    });
    for what in gone {
        unanswered(htlc, what);
    }
}

/// A fetch with no answer: the browser still hears of a listing, empty, so
/// it lets go of the carrier and of the node waiting for it; an article's
/// node is free to be fetched again.
unsafe fn unanswered(htlc: *mut c_void, what: Asked) {
    match what {
        Asked::Folder(g) => gtkhx_session_emit_news_folder(gtkhx_session_get_default(), htlc, g),
        Asked::Catalog(g) => gtkhx_session_emit_news_catalog(gtkhx_session_get_default(), htlc, g),
        Asked::Article(target) => news_post_fetch_failed(target),
    }
}

/// The emit's length: the signals carry it as a `guint`.
fn emit_len(text: &str) -> u32 {
    u32::try_from(text.len()).unwrap_or(u32::MAX)
}

/// What is added to flat news, as the server announces it.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn posted(htlc: *mut c_void, text: &str) {
    gtkhx_session_emit_news_post(
        gtkhx_session_get_default(),
        htlc,
        text.as_ptr().cast(),
        emit_len(text),
    );
}

/// The whole of flat news.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn file(htlc: *mut c_void, text: &str) {
    gtkhx_session_emit_news_file(
        gtkhx_session_get_default(),
        htlc,
        text.as_ptr().cast(),
        emit_len(text),
    );
}

/// What a bundle holds, for the folder carrier that asked.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn listing(htlc: *mut c_void, trans: u32, items: &[NewsItem]) {
    if let Some(Asked::Folder(g)) = answered(htlc, trans, |a| matches!(a, Asked::Folder(_))) {
        gnews_folder_set_items(g, items.to_vec());
        gtkhx_session_emit_news_folder(gtkhx_session_get_default(), htlc, g);
    }
}

/// The articles in a category, for the catalog carrier that asked.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn category(htlc: *mut c_void, trans: u32, articles: &[Article]) {
    if let Some(Asked::Catalog(g)) = answered(htlc, trans, |a| matches!(a, Asked::Catalog(_))) {
        gnews_catalog_set_articles(g, articles.to_vec());
        gtkhx_session_emit_news_catalog(gtkhx_session_get_default(), htlc, g);
    }
}

/// An article's text, for the node that asked.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn article(htlc: *mut c_void, trans: u32, text: &str) {
    if let Some(Asked::Article(target)) = answered(htlc, trans, |a| matches!(a, Asked::Article(_)))
    {
        let post = news_post_new(target, text.as_ptr(), text.len());
        gtkhx_session_emit_news_thread(gtkhx_session_get_default(), htlc, post);
    }
}

/// A request on `trans` was refused, or its reply cut short. The reason, if
/// any, is `request-failed`'s to show.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn failed(htlc: *mut c_void, trans: u32) {
    if let Some(what) = answered(htlc, trans, |_| true) {
        unanswered(htlc, what);
    }
}

#[cfg(test)]
mod tests;
