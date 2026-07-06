//! Unit tests for the post-threading layout. These carry the verification
//! the display-less `news_browser.c` GTK glue can't — every subtle parenting
//! rule (top-level, nested, out-of-order, missing/self parent, duplicate ids)
//! is pinned here.

use super::*;

fn links(pairs: &[(u32, u32)]) -> Vec<PostLink> {
    pairs
        .iter()
        .map(|&(postid, parentid)| PostLink { postid, parentid })
        .collect()
}

#[test]
fn empty_list() {
    assert!(thread_parent_indices(&[]).is_empty());
}

#[test]
fn all_top_level_when_no_parents() {
    let p = links(&[(1, 0), (2, 0), (3, 0)]);
    assert_eq!(thread_parent_indices(&p), vec![-1, -1, -1]);
}

#[test]
fn reply_points_at_parent_index() {
    // post 10 at idx 0; reply 11 -> 10 at idx 1.
    let p = links(&[(10, 0), (11, 10)]);
    assert_eq!(thread_parent_indices(&p), vec![-1, 0]);
}

#[test]
fn parent_after_child_in_array_still_resolves() {
    // The reply appears before its parent in array order — the two-pass build
    // must still resolve it (server ordering isn't guaranteed parents-first).
    let p = links(&[(11, 10), (10, 0)]);
    assert_eq!(thread_parent_indices(&p), vec![1, -1]);
}

#[test]
fn nested_replies_chain() {
    // 1 root, 2 -> 1, 3 -> 2.
    let p = links(&[(1, 0), (2, 1), (3, 2)]);
    assert_eq!(thread_parent_indices(&p), vec![-1, 0, 1]);
}

#[test]
fn multiple_replies_same_parent_preserve_order() {
    let p = links(&[(1, 0), (2, 1), (3, 1)]);
    assert_eq!(thread_parent_indices(&p), vec![-1, 0, 0]);
}

#[test]
fn missing_parent_is_top_level() {
    // parentid names a post not in this listing → top-level, not dropped.
    let p = links(&[(11, 999)]);
    assert_eq!(thread_parent_indices(&p), vec![-1]);
}

#[test]
fn self_parent_is_top_level() {
    // parentid == own postid must not attach the node under itself.
    let p = links(&[(11, 11)]);
    assert_eq!(thread_parent_indices(&p), vec![-1]);
}

#[test]
fn duplicate_postid_last_wins() {
    // Two posts share postid 5; a reply to 5 resolves to the LATER index
    // (mirrors g_hash_table_insert replace semantics).
    let p = links(&[(5, 0), (5, 0), (9, 5)]);
    assert_eq!(thread_parent_indices(&p), vec![-1, -1, 1]);
}

#[test]
fn ffi_matches_pure_fn() {
    let posts = links(&[(1, 0), (2, 1), (3, 999), (4, 2)]);
    let ids: Vec<u32> = posts.iter().map(|p| p.postid).collect();
    let pids: Vec<u32> = posts.iter().map(|p| p.parentid).collect();
    let mut out = vec![0i32; posts.len()];
    unsafe {
        hx_news_thread_parent_indices(
            ids.as_ptr(),
            pids.as_ptr(),
            posts.len(),
            out.as_mut_ptr(),
        );
    }
    assert_eq!(out, thread_parent_indices(&posts));
}

#[test]
fn ffi_oversized_n_is_noop() {
    // A bogus/hostile huge `n` must fail closed (the isize::MAX ceiling) before
    // any slice deref — the out buffer stays untouched.
    let ids = [1u32];
    let pids = [0u32];
    let mut out = [7i32; 1];
    unsafe {
        hx_news_thread_parent_indices(ids.as_ptr(), pids.as_ptr(), usize::MAX, out.as_mut_ptr());
    }
    assert_eq!(out, [7]);
}

#[test]
fn ffi_zero_len_is_noop() {
    // n == 0 must not touch out_parent (or deref the arrays).
    let mut out = [7i32; 1];
    unsafe {
        hx_news_thread_parent_indices(
            std::ptr::null(),
            std::ptr::null(),
            0,
            out.as_mut_ptr(),
        );
    }
    assert_eq!(out, [7]);
}
