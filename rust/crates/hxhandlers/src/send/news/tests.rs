//! The news senders' plumbing: which request goes, with which reply
//! expected, and what waits for it. The requests' bytes are
//! `hxrequest::news`'s, pinned there; the C send-path primitives and the
//! carrier path accessors are stubbed here so the cargo-test build links no
//! C.

use super::*;
use std::cell::{Cell, RefCell};

/// A request as written: its opcode, and each chunk's tag and data.
type Sent = (u32, Vec<(u16, Vec<u8>)>);

thread_local! {
    static UTF8: Cell<bool> = const { Cell::new(false) };
    // When set, the path accessors return NULL (a node cleared during
    // refresh), exercising the senders' NULL-path guard.
    static PATH_NULL: Cell<bool> = const { Cell::new(false) };
    static SENT: RefCell<Vec<Sent>> = const { RefCell::new(Vec::new()) };
}

pub(crate) unsafe fn hx_htlc_text_encoding_cap(_htlc: *mut c_void) -> glib::ffi::gboolean {
    UTF8.with(|c| c.get()).into()
}

pub(crate) unsafe fn hlwrite_chunks(
    _htlc: *mut c_void,
    ty: u32,
    _flag: u32,
    chunks: *const HxChunk,
    hc: c_int,
) {
    let chunks = (0..hc as usize)
        .map(|i| {
            let c = &*chunks.add(i);
            let data = if c.len == 0 {
                Vec::new()
            } else {
                std::slice::from_raw_parts(c.data, c.len as usize).to_vec()
            };
            (c.tag, data)
        })
        .collect();
    SENT.with(|s| s.borrow_mut().push((ty, chunks)));
}

pub(crate) unsafe fn gnews_catalog_path(_g: *mut c_void) -> *const c_char {
    path()
}
pub(crate) unsafe fn gnews_folder_path(_g: *mut c_void) -> *const c_char {
    path()
}

fn path() -> *const c_char {
    if PATH_NULL.with(|c| c.get()) {
        std::ptr::null()
    } else {
        c"/Caf\x8e".as_ptr()
    }
}

fn reset() {
    UTF8.with(|c| c.set(false));
    PATH_NULL.with(|c| c.set(false));
    SENT.with(|s| s.borrow_mut().clear());
    crate::send::expected::take();
}

fn sent() -> Vec<Sent> {
    SENT.with(|s| std::mem::take(&mut *s.borrow_mut()))
}

fn as_sent(req: Option<Request>) -> Sent {
    let req = req.unwrap();
    (req.opcode, req.chunks)
}

/// What was expected of each reply, in order.
fn expected() -> Vec<Expect> {
    crate::send::expected::take()
        .into_iter()
        .map(|(_, what)| what)
        .collect()
}

fn htlc() -> *mut c_void {
    std::ptr::dangling_mut::<c_void>()
}
fn tok() -> *mut c_void {
    0xABCDusize as *mut c_void
}

const CAFE: &[u8] = b"/Caf\x8e";

#[test]
fn a_fetch_goes_with_its_reply_expected_and_waits_with_what_asked() {
    reset();
    unsafe {
        hx_news15_fldr_list(htlc(), tok());
        hx_news15_cat_list(htlc(), tok());
        hx_news15_get_post(
            htlc(),
            c"/Caf\x8e".as_ptr(),
            7,
            c"text/html".as_ptr(),
            tok(),
        );
        hx_get_news(htlc());
    }
    assert_eq!(
        sent(),
        [
            as_sent(news::listing(CAFE)),
            as_sent(news::category(CAFE)),
            as_sent(news::article(CAFE, 7, b"text/html")),
            as_sent(Some(news::file())),
        ]
    );
    assert_eq!(
        expected(),
        [
            Expect::NewsListing,
            Expect::NewsCategory,
            Expect::NewsArticle,
            Expect::NewsFile
        ]
    );
    let waiting: Vec<_> = (1..=3)
        .map(|t| crate::recv::news::answered(htlc(), t, |_| true))
        .collect();
    assert_eq!(
        waiting,
        [
            Some(Asked::Folder(tok())),
            Some(Asked::Catalog(tok())),
            Some(Asked::Article(tok())),
        ]
    );
}

/// A change goes in the connection's encoding, with nothing to read in its
/// reply once it worked.
#[test]
fn a_change_goes_in_the_connection_s_encoding() {
    reset();
    UTF8.with(|c| c.set(true));
    unsafe {
        hx_news15_post_thread(
            htlc(),
            c"/Caf\x8e".as_ptr(),
            c"Thé".as_ptr(),
            0x2a,
            c"a\nb".as_ptr(),
        );
        hx_news15_delete_thread(htlc(), c"/Caf\x8e".as_ptr(), 7);
        hx_news15_delete(htlc(), c"/Caf\x8e".as_ptr());
        hx_news15_mkcat(htlc(), c"/".as_ptr(), c"Thé".as_ptr());
        hx_news15_mkdir(htlc(), c"/".as_ptr(), c"new".as_ptr());
        // Only the first `len` bytes are the post.
        hx_post_news(htlc(), c"hello".as_ptr(), 3);
    }
    assert_eq!(
        sent(),
        [
            as_sent(news::post_article(
                CAFE,
                0x2a,
                "Thé".as_bytes(),
                b"a\nb",
                true
            )),
            as_sent(news::delete_article(CAFE, 7)),
            as_sent(news::delete(CAFE)),
            as_sent(news::create_category(b"/", "Thé".as_bytes(), true)),
            as_sent(news::create_bundle(b"/", b"new", true)),
            as_sent(news::post(b"hel", true)),
        ]
    );
    assert_eq!(expected(), [Expect::NewsChange; 6]);
}

#[test]
fn nothing_goes_without_a_connection_or_a_path() {
    reset();
    let name = c"n".as_ptr();
    unsafe {
        hx_get_news(std::ptr::null_mut());
        hx_post_news(std::ptr::null_mut(), name, 1);
        // NULL target so the transfer-full release is a no-op (not an unref).
        hx_news15_get_post(std::ptr::null_mut(), name, 0, name, std::ptr::null_mut());
        hx_news15_get_post(htlc(), std::ptr::null(), 0, name, std::ptr::null_mut());
        hx_news15_cat_list(std::ptr::null_mut(), tok());
        hx_news15_fldr_list(htlc(), std::ptr::null_mut());
        hx_news15_post_thread(htlc(), std::ptr::null(), name, 0, name);
        hx_news15_delete_thread(std::ptr::null_mut(), name, 1);
        hx_news15_delete(htlc(), std::ptr::null());
        hx_news15_mkcat(htlc(), std::ptr::null(), name);
        hx_news15_mkdir(std::ptr::null_mut(), name, name);
        // A node cleared during refresh has no path to ask for.
        PATH_NULL.with(|c| c.set(true));
        hx_news15_cat_list(htlc(), tok());
        hx_news15_fldr_list(htlc(), tok());
    }
    assert!(sent().is_empty());
    assert!(expected().is_empty());
}
