//! `hxnews-model` — pure, unit-tested model logic for the 1.5 threaded-news
//! browser (Phase R5 N2a).
//!
//! The browser window itself (`news_browser.c`) is a large, monolithic
//! GObject/GTK unit — tree model, factory, async reply matching, and dialogs —
//! that can't be verified without a live 1.5 server + a display. This crate
//! carves out the one genuinely subtle, *pure* piece so it can be locked down
//! with tests: the **post-threading layout**.
//!
//! Given a category's flat post list in server (array) order, each post is
//! either top-level (roots directly under the category) or a reply nested
//! under an earlier/later post identified by `parentid`. [`thread_parent_indices`]
//! resolves that into, for each post, the *array index* of its parent (or
//! [`TOP_LEVEL`]). `news_browser.c::catlist_thread_into` then builds the
//! `HxNewsNode` tree from the result — the GObject glue stays C for now.

use std::collections::HashMap;
use std::os::raw::c_int;
use std::slice;

pub mod node;
pub use node::{HxNewsDate, HxNewsNode};

/// A post's identity for threading: its own id and its parent's id (`0` = no
/// parent, i.e. a top-level post).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PostLink {
    pub postid: u32,
    pub parentid: u32,
}

/// Sentinel returned for a top-level post (one that roots directly under the
/// category rather than nesting under another post).
pub const TOP_LEVEL: i32 = -1;

/// For each post (in array order) return its parent's array index, or
/// [`TOP_LEVEL`] when the post is top-level.
///
/// A post is top-level when its `parentid` is `0`, names a `postid` not present
/// in this listing, or resolves to itself (a self-parent guard). When two posts
/// share a `postid`, the later occurrence wins the id — this mirrors the C
/// `g_hash_table_insert` replace semantics the original walker relied on;
/// pathological, but pinned here so a future refactor can't silently change it.
///
/// Array order is preserved: the caller appends the top-level posts to the
/// category in the order they appear, and each reply under its parent's
/// children in the order they appear.
pub fn thread_parent_indices(posts: &[PostLink]) -> Vec<i32> {
    let mut by_postid: HashMap<u32, usize> = HashMap::with_capacity(posts.len());
    for (i, p) in posts.iter().enumerate() {
        // Last occurrence of a given postid wins (replace, not insert-if-absent).
        by_postid.insert(p.postid, i);
    }

    posts
        .iter()
        .enumerate()
        .map(|(i, p)| {
            if p.parentid == 0 {
                return TOP_LEVEL;
            }
            match by_postid.get(&p.parentid) {
                // A post can't be its own parent — that would make it a leaf of
                // itself and drop it from the tree entirely.
                Some(&parent) if parent != i => parent as i32,
                _ => TOP_LEVEL,
            }
        })
        .collect()
}

/// C ABI seam for `news_browser.c::catlist_thread_into`.
///
/// Fills `out_parent[i]` with post `i`'s parent array index (or `-1` /
/// [`TOP_LEVEL`]) for `n` posts whose ids and parent-ids sit in the parallel
/// `postids` / `parentids` arrays. No-op when `n == 0` or any pointer is NULL.
///
/// # Safety
/// `postids`, `parentids`, and `out_parent` must each be valid for `n`
/// elements (or `n` is 0). `out_parent` must not alias the input arrays.
#[no_mangle]
pub unsafe extern "C" fn hx_news_thread_parent_indices(
    postids: *const u32,
    parentids: *const u32,
    n: usize,
    out_parent: *mut c_int,
) {
    if n == 0 || postids.is_null() || parentids.is_null() || out_parent.is_null() {
        return;
    }
    let ids = unsafe { slice::from_raw_parts(postids, n) };
    let pids = unsafe { slice::from_raw_parts(parentids, n) };
    let posts: Vec<PostLink> = (0..n)
        .map(|i| PostLink {
            postid: ids[i],
            parentid: pids[i],
        })
        .collect();
    let parents = thread_parent_indices(&posts);
    let out = unsafe { slice::from_raw_parts_mut(out_parent, n) };
    for (slot, v) in out.iter_mut().zip(parents) {
        *slot = v as c_int;
    }
}

#[cfg(test)]
mod tests;
