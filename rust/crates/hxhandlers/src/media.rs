//! Inline media: a picture going up to the server, whole or in parts, and one
//! coming down, part by part.
//!
//! Each part is a request on the control connection (`hxrequest::media`),
//! its reply expected by the session, which reads it into a part, the
//! uploaded picture's handle, or a failure with the extension's error code.
//! What is kept here between the parts is the picture's bytes and where it
//! has got to, keyed by connection and the trans of the part in flight. The
//! C ABI is the one the chat window, the attach flow and the picture dialog
//! always called; each hears once, through its callback, how it ended.
//!
//! A connection that closes, or logs in again, lets go of what it had in
//! flight: a download says nothing more, and an upload hands its caller's
//! state to the caller's free function.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};

use glib::ffi::{gboolean, GByteArray, GDestroyNotify};
use hxproto::inline_media::MediaErrorCode;
use hxrequest::{media, Request};
use hxsession::{ChatMedia, Expect, MediaPart};

/// A part's size when the server names none, and the most it may name: it
/// leaves room in the frame for the part's other fields.
const CHUNK_SIZE: usize = 60_000;
/// The spec has an upload's token at 64 bytes at most; this allows for its
/// growing, but no more, as every part carries the token back.
const MAX_TOKEN: usize = 1024;
/// A picture the server gave no type for.
const UNTYPED: &[u8] = b"application/octet-stream";

/// `HxInlineMediaDownloadResult`, mirrored by the picture dialog and the chat
/// view's media rows. Every pointer is borrowed for the callback's duration.
#[repr(C)]
pub struct DownloadResult {
    pub bytes: *mut GByteArray,
    pub canonical_mime: *const c_char,
    pub error_code: u16,
    pub error_message: *const c_char,
    pub error_message_len: usize,
}

/// `HxInlineMediaUploadResult` (inline_media_upload.h). Every pointer is
/// borrowed for the callback's duration.
#[repr(C)]
pub struct UploadResult {
    pub media_id: *const u8,
    pub media_id_len: usize,
    pub media_type: *const c_char,
    pub media_type_len: usize,
    pub width: u32,
    pub height: u32,
    pub bytes: u32,
    pub width_present: gboolean,
    pub height_present: gboolean,
    pub bytes_present: gboolean,
    pub error_code: u16,
    pub error_message: *const c_char,
    pub error_message_len: usize,
}

// Pinned to the C typedefs' LP64 layout, which the callers read.
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<DownloadResult>() == 40);
    assert!(offset_of!(DownloadResult, error_message_len) == 32);
    assert!(size_of::<UploadResult>() == 80);
    assert!(offset_of!(UploadResult, error_code) == 56);
    assert!(offset_of!(UploadResult, error_message_len) == 72);
};

pub type DownloadCallback =
    Option<unsafe extern "C" fn(*mut c_void, *const DownloadResult, *mut c_void)>;
pub type UploadCallback =
    Option<unsafe extern "C" fn(*mut c_void, *const UploadResult, *mut c_void)>;

struct Download {
    /// What the caller cancels it by.
    handle: usize,
    id: Vec<u8>,
    /// The part to ask for next.
    next: u16,
    payload: Vec<u8>,
    mime: Option<Vec<u8>>,
    on_done: DownloadCallback,
    user_data: *mut c_void,
}

struct Upload {
    /// The picture, when it goes in parts; the part to send next, how many
    /// there are, and the token they go on.
    parts: Option<Parts>,
    on_done: UploadCallback,
    user_data: *mut c_void,
    user_data_free: GDestroyNotify,
}

struct Parts {
    payload: Vec<u8>,
    chunk: usize,
    count: u16,
    next: u16,
    token: Option<Vec<u8>>,
}

impl Parts {
    fn slice(&self, index: u16) -> &[u8] {
        let start = (index as usize * self.chunk).min(self.payload.len());
        let end = (start + self.chunk).min(self.payload.len());
        &self.payload[start..end]
    }
}

thread_local! {
    static DOWNLOADS: RefCell<HashMap<(usize, u32), Download>> = RefCell::new(HashMap::new());
    static UPLOADS: RefCell<HashMap<(usize, u32), Upload>> = RefCell::new(HashMap::new());
    static HANDLES: Cell<usize> = const { Cell::new(0) };
}

#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;

#[cfg(not(test))]
extern "C" {
    /// inline_media.c — whether the server agreed to inline media; logs why
    /// not.
    fn inline_media_cap_ok(htlc: *mut c_void) -> gboolean;
}

#[cfg(not(test))]
use gtkhx_core::conn::hx_conn_media_chunk_size;

#[cfg(test)]
use tests::{hlwrite_chunks, hx_conn_media_chunk_size, inline_media_cap_ok};

/// Send `req`, its reply expected as `what`; the trans it went out on.
unsafe fn send(htlc: *mut c_void, req: &Request, what: Expect) -> u32 {
    let trans = crate::send::expect_next(htlc, what);
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(
            htlc.cast(),
            req.opcode,
            0,
            chunks.as_ptr(),
            chunks.len() as c_int,
        )
    });
    trans
}

/// The size of a part: what the server named, or the default, and never
/// more than the default.
unsafe fn chunk_size(htlc: *mut c_void) -> usize {
    match hx_conn_media_chunk_size(htlc.cast()) as usize {
        0 => CHUNK_SIZE,
        n => n.min(CHUNK_SIZE),
    }
}

/// `text` as a C string the callback borrows; `None` for none.
fn c_reason(text: Option<&str>) -> Option<CString> {
    text.filter(|t| !t.is_empty())
        .map(|t| CString::new(t.replace('\0', "")).unwrap_or_default())
}

// ---- Downloads -----------------------------------------------------------

/// `hx_inline_media_download *inline_media_download_start (htlc, handle,
/// handle_len, on_done, user_data)` — fetch the picture `handle` names. The
/// result is what [`inline_media_download_cancel`] takes: never dereferenced,
/// so one that has finished is safe to cancel. NULL when nothing was sent:
/// the server agreed to no inline media, or the handle is empty or too long.
///
/// # Safety
/// `htlc` is NULL or a live connection; `handle` points at `handle_len`
/// bytes. Main thread.
#[no_mangle]
pub unsafe extern "C" fn inline_media_download_start(
    htlc: *mut c_void,
    handle: *const u8,
    handle_len: usize,
    on_done: DownloadCallback,
    user_data: *mut c_void,
) -> *mut c_void {
    if htlc.is_null() || inline_media_cap_ok(htlc) == glib::ffi::GFALSE || handle.is_null() {
        return std::ptr::null_mut();
    }
    let id = std::slice::from_raw_parts(handle, handle_len).to_vec();
    let Some(req) = media::download(&id, None) else {
        return std::ptr::null_mut();
    };
    let trans = send(htlc, &req, Expect::MediaDownload);
    let handle = HANDLES.with(|h| {
        h.set(h.get() + 1);
        h.get()
    });
    let d = Download {
        handle,
        id,
        next: 1,
        payload: Vec::new(),
        mime: None,
        on_done,
        user_data,
    };
    DOWNLOADS.with(|m| m.borrow_mut().insert((htlc as usize, trans), d));
    handle as *mut c_void
}

/// `void inline_media_download_cancel (hx_inline_media_download *dl)` — the
/// download's callback will not run. A NULL or finished one is no-op.
///
/// # Safety
/// Main thread.
#[no_mangle]
pub unsafe extern "C" fn inline_media_download_cancel(dl: *mut c_void) {
    let handle = dl as usize;
    DOWNLOADS.with(|m| m.borrow_mut().retain(|_, d| d.handle != handle));
}

/// One part of a download came: the next is asked for, or the picture is
/// whole.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn part(htlc: *mut c_void, trans: u32, part: &MediaPart) {
    let Some(mut d) = DOWNLOADS.with(|m| m.borrow_mut().remove(&(htlc as usize, trans))) else {
        return;
    };
    if d.mime.is_none() && !part.mime.is_empty() {
        d.mime = Some(part.mime.clone());
    }
    d.payload.extend_from_slice(&part.payload);
    if part.last {
        let bytes = glib::ffi::g_byte_array_sized_new(d.payload.len() as u32);
        glib::ffi::g_byte_array_append(bytes, d.payload.as_ptr(), d.payload.len() as u32);
        let mime = CString::new(d.mime.as_deref().unwrap_or(UNTYPED).to_vec())
            .unwrap_or_else(|_| CString::new(UNTYPED).unwrap());
        let r = DownloadResult {
            bytes,
            canonical_mime: mime.as_ptr(),
            error_code: 0,
            error_message: std::ptr::null(),
            error_message_len: 0,
        };
        if let Some(cb) = d.on_done {
            cb(htlc, &r, d.user_data);
        }
        glib::ffi::g_byte_array_unref(bytes);
        return;
    }
    // A server that never says which part is the last would be asked for
    // parts forever.
    let req = (d.next < part.parts)
        .then(|| media::download(&d.id, Some(d.next)))
        .flatten();
    let Some(req) = req else {
        download_failed(htlc, d, MediaErrorCode::Generic, None);
        return;
    };
    let trans = send(htlc, &req, Expect::MediaDownload);
    d.next += 1;
    DOWNLOADS.with(|m| m.borrow_mut().insert((htlc as usize, trans), d));
}

unsafe fn download_failed(
    htlc: *mut c_void,
    d: Download,
    code: MediaErrorCode,
    reason: Option<&str>,
) {
    let reason = c_reason(reason);
    let r = DownloadResult {
        bytes: std::ptr::null_mut(),
        canonical_mime: std::ptr::null(),
        error_code: code.as_u16(),
        error_message: reason.as_ref().map_or(std::ptr::null(), |r| r.as_ptr()),
        error_message_len: reason.as_ref().map_or(0, |r| r.as_bytes().len()),
    };
    if let Some(cb) = d.on_done {
        cb(htlc, &r, d.user_data);
    }
}

// ---- Uploads -------------------------------------------------------------

/// `gboolean hx_send_upload_media (htlc, payload, payload_len,
/// declared_type, declared_type_len, on_done, user_data, user_data_free)` —
/// send a picture, whole when it fits in one part and in parts otherwise;
/// `declared_type` is a hint the server may ignore. FALSE when nothing was
/// sent: the server agreed to no inline media, the picture is empty, or it
/// needs more parts than the wire can count. Otherwise `on_done` hears once
/// how it ended, and `user_data` is the caller's from then on; only if the
/// connection goes first does `user_data_free` get it.
///
/// # Safety
/// `htlc` is NULL or a live connection; `payload` points at `payload_len`
/// bytes and `declared_type` at `declared_type_len`. Main thread.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn hx_send_upload_media(
    htlc: *mut c_void,
    payload: *const u8,
    payload_len: usize,
    declared_type: *const c_char,
    declared_type_len: usize,
    on_done: UploadCallback,
    user_data: *mut c_void,
    user_data_free: GDestroyNotify,
) -> gboolean {
    if htlc.is_null()
        || inline_media_cap_ok(htlc) == glib::ffi::GFALSE
        || payload.is_null()
        || payload_len == 0
    {
        return glib::ffi::GFALSE;
    }
    let payload = std::slice::from_raw_parts(payload, payload_len);
    let mime = (!declared_type.is_null())
        .then(|| std::slice::from_raw_parts(declared_type.cast::<u8>(), declared_type_len));
    let chunk = chunk_size(htlc);
    let (req, parts) = if payload.len() <= chunk {
        (media::upload(payload, mime), None)
    } else {
        let Ok(count) = u16::try_from(payload.len().div_ceil(chunk)) else {
            return glib::ffi::GFALSE;
        };
        let parts = Parts {
            payload: payload.to_vec(),
            chunk,
            count,
            next: 1,
            token: None,
        };
        (
            media::upload_first(parts.slice(0), mime, count),
            Some(parts),
        )
    };
    let Some(req) = req else {
        return glib::ffi::GFALSE;
    };
    let last = parts.is_none();
    let trans = send(htlc, &req, Expect::MediaUpload { last });
    let u = Upload {
        parts,
        on_done,
        user_data,
        user_data_free,
    };
    UPLOADS.with(|m| m.borrow_mut().insert((htlc as usize, trans), u));
    glib::ffi::GTRUE
}

/// A part of an upload went up: the next follows, on the token the server
/// gave, which it need give only the first time.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn uploading(htlc: *mut c_void, trans: u32, token: Option<&[u8]>) {
    let Some(mut u) = UPLOADS.with(|m| m.borrow_mut().remove(&(htlc as usize, trans))) else {
        return;
    };
    let req = u.parts.as_mut().and_then(|p| {
        if let Some(t) = token.filter(|t| !t.is_empty()) {
            p.token = (t.len() <= MAX_TOKEN).then(|| t.to_vec());
        }
        let last = p.next + 1 >= p.count;
        let req = media::upload_next(p.token.as_deref()?, p.slice(p.next), p.next, last)?;
        Some((req, last))
    });
    let Some((req, last)) = req else {
        upload_failed(htlc, u, MediaErrorCode::Generic, None);
        return;
    };
    let trans = send(htlc, &req, Expect::MediaUpload { last });
    if let Some(p) = u.parts.as_mut() {
        p.next += 1;
    }
    UPLOADS.with(|m| m.borrow_mut().insert((htlc as usize, trans), u));
}

/// An upload is done: the handle the picture goes by, for its caller.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn uploaded(htlc: *mut c_void, trans: u32, m: &ChatMedia) {
    let Some(u) = UPLOADS.with(|map| map.borrow_mut().remove(&(htlc as usize, trans))) else {
        return;
    };
    let mime = CString::new(m.mime.clone()).unwrap_or_default();
    let r = UploadResult {
        media_id: m.id.as_ptr(),
        media_id_len: m.id.len(),
        media_type: mime.as_ptr(),
        media_type_len: mime.as_bytes().len(),
        width: m.width.unwrap_or(0),
        height: m.height.unwrap_or(0),
        bytes: m.bytes.unwrap_or(0),
        width_present: m.width.is_some().into(),
        height_present: m.height.is_some().into(),
        bytes_present: m.bytes.is_some().into(),
        error_code: 0,
        error_message: std::ptr::null(),
        error_message_len: 0,
    };
    if let Some(cb) = u.on_done {
        cb(htlc, &r, u.user_data);
    }
}

unsafe fn upload_failed(htlc: *mut c_void, u: Upload, code: MediaErrorCode, reason: Option<&str>) {
    let reason = c_reason(reason);
    let r = UploadResult {
        media_id: std::ptr::null(),
        media_id_len: 0,
        media_type: std::ptr::null(),
        media_type_len: 0,
        width: 0,
        height: 0,
        bytes: 0,
        width_present: glib::ffi::GFALSE,
        height_present: glib::ffi::GFALSE,
        bytes_present: glib::ffi::GFALSE,
        error_code: code.as_u16(),
        error_message: reason.as_ref().map_or(std::ptr::null(), |r| r.as_ptr()),
        error_message_len: reason.as_ref().map_or(0, |r| r.as_bytes().len()),
    };
    if let Some(cb) = u.on_done {
        cb(htlc, &r, u.user_data);
    }
}

// ---- Both ----------------------------------------------------------------

/// A picture's request on `trans` failed: its download or upload ends, its
/// caller told why.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn failed(
    htlc: *mut c_void,
    trans: u32,
    code: MediaErrorCode,
    reason: Option<&str>,
) {
    let key = (htlc as usize, trans);
    // A reply cut short says nothing the caller can show.
    let reason = reason.filter(|r| *r != crate::recv::chat::CUT_SHORT);
    if let Some(d) = DOWNLOADS.with(|m| m.borrow_mut().remove(&key)) {
        download_failed(htlc, d, code, reason);
    } else if let Some(u) = UPLOADS.with(|m| m.borrow_mut().remove(&key)) {
        upload_failed(htlc, u, code, reason);
    }
}

/// Let go of what `htlc` had in flight. An upload's caller state goes to its
/// free function, once nothing here is borrowed: it may start another.
///
/// # Safety
/// Main thread.
pub(crate) unsafe fn forget(htlc: *mut c_void) {
    let mine = |(h, _): &(usize, u32)| *h == htlc as usize;
    DOWNLOADS.with(|m| m.borrow_mut().retain(|k, _| !mine(k)));
    let gone: Vec<Upload> = UPLOADS.with(|m| {
        let mut m = m.borrow_mut();
        let keys: Vec<_> = m.keys().copied().filter(|k| mine(k)).collect();
        keys.iter().filter_map(|k| m.remove(k)).collect()
    });
    for u in gone {
        if let (Some(free), false) = (u.user_data_free, u.user_data.is_null()) {
            free(u.user_data);
        }
    }
}

#[cfg(test)]
mod tests;
