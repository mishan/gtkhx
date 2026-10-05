//! News requests: flat news's file and posts, and threaded news's listings,
//! articles, posts, deletions and new bundles and categories.
//!
//! Paths are remote `/`-separated paths, in the bytes a listing gave, which
//! name a bundle or category back to the server exactly. Text is UTF-8 as the
//! user typed it, encoded for the wire here — passed through when the
//! connection negotiated UTF-8 (`utf8`), Mac Roman otherwise.
//!
//! Each builder returns `None` for input the wire can't carry: a chunk longer
//! than its u16 length allows.

use hxproto::build::{
    self, HxChunk, NewsDeleteThreadRequest, NewsGetThreadRequest, NewsMakeCategoryRequest,
    NewsMakeDirRequest, NewsPostThreadRequest,
};
use hxproto::messages::ClientHdr;

use crate::path::encode_dir;
use crate::Request;

/// NEWS_GETFILE: the whole of flat news.
pub fn file() -> Request {
    Request {
        opcode: ClientHdr::NewsGetFile as u32,
        chunks: Vec::new(),
    }
}

/// NEWS_POST: `text` added to flat news.
pub fn post(text: &[u8], utf8: bool) -> Option<Request> {
    let body = hxtext::for_wire(text, utf8, true);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_post_chunks(&body, &mut chunks);
    Request::from_built(ClientHdr::NewsPost as u32, &chunks, hc)
}

/// NEWSDIRLIST: what the bundle at `path` holds.
pub fn listing(path: &[u8]) -> Option<Request> {
    let enc = encode_dir(path, false);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_dirlist_chunks(&enc, &mut chunks);
    Request::from_built(ClientHdr::NewsListDir as u32, &chunks, hc)
}

/// NEWSCATLIST: the articles in the category at `path`.
pub fn category(path: &[u8]) -> Option<Request> {
    let enc = encode_dir(path, false);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_catlist_chunks(&enc, &mut chunks);
    Request::from_built(ClientHdr::NewsListCategory as u32, &chunks, hc)
}

/// GETTHREAD: article `id` in the category at `path`, as `mime`, the type
/// its listing gave; text/plain when it gave none.
pub fn article(path: &[u8], id: u32, mime: &[u8]) -> Option<Request> {
    let enc = encode_dir(path, false);
    let req = NewsGetThreadRequest {
        path: &enc,
        threadid: id,
        mime_type: if mime.is_empty() { b"text/plain" } else { mime },
    };
    let mut chunks = [HxChunk::EMPTY; 3];
    let mut scratch = [0u8; 4];
    let hc = build::build_news_getthread_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::GetThread as u32, &chunks, hc)
}

/// POSTTHREAD: a plain-text article in the category at `path`, replying to
/// article `parent`, or 0 to start a thread.
pub fn post_article(
    path: &[u8],
    parent: u32,
    subject: &[u8],
    text: &[u8],
    utf8: bool,
) -> Option<Request> {
    let enc = encode_dir(path, false);
    let subject = hxtext::for_wire(subject, utf8, false);
    let text = hxtext::for_wire(text, utf8, true);
    let req = NewsPostThreadRequest {
        path: &enc,
        // Field 334, the flags, always zero: the parent goes in `thread_id`.
        flags: 0,
        mime_type: b"text/plain",
        subject: &subject,
        text: &text,
        thread_id: parent,
    };
    let mut chunks = [HxChunk::EMPTY; 6];
    let mut scratch = [0u8; 8];
    let hc = build::build_news_post_thread_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::PostThread as u32, &chunks, hc)
}

/// DELETETHREAD: article `id` in the category at `path`.
pub fn delete_article(path: &[u8], id: u32) -> Option<Request> {
    let enc = encode_dir(path, false);
    let req = NewsDeleteThreadRequest {
        path: &enc,
        threadid: id,
    };
    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = [0u8; 4];
    let hc = build::build_news_delete_thread_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::DeleteThread as u32, &chunks, hc)
}

/// DELNEWSDIRCAT: the bundle or category at `path`.
pub fn delete(path: &[u8]) -> Option<Request> {
    let enc = encode_dir(path, false);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_news_delete_chunks(&enc, &mut chunks);
    Request::from_built(ClientHdr::NewsDelete as u32, &chunks, hc)
}

/// MAKECATEGORY: category `name` in the bundle at `path`.
pub fn create_category(path: &[u8], name: &[u8], utf8: bool) -> Option<Request> {
    let enc = encode_dir(path, false);
    let name = hxtext::for_wire(name, utf8, false);
    let req = NewsMakeCategoryRequest {
        path: &enc,
        name: &name,
    };
    let mut chunks = [HxChunk::EMPTY; 2];
    let hc = build::build_news_mkcat_chunks(&req, &mut chunks);
    Request::from_built(ClientHdr::NewsMkCategory as u32, &chunks, hc)
}

/// MAKENEWSDIR: bundle `name` in the bundle at `path`. The new bundle goes as
/// its own name field: the server resolves the path as one that exists.
pub fn create_bundle(path: &[u8], name: &[u8], utf8: bool) -> Option<Request> {
    let enc = encode_dir(path, false);
    let name = hxtext::for_wire(name, utf8, false);
    let req = NewsMakeDirRequest {
        path: &enc,
        name: &name,
    };
    let mut chunks = [HxChunk::EMPTY; 2];
    let hc = build::build_news_mkdir_chunks(&req, &mut chunks);
    Request::from_built(ClientHdr::NewsMkdir as u32, &chunks, hc)
}

#[cfg(test)]
mod tests;
