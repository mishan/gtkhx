//! The inline-media state machines against a scripted server: what each part
//! sends, and what the caller hears.

use super::*;
use std::cell::{Cell, RefCell};

use hxproto::build::HxChunk;

use crate::send::expected;

type Sent = (u32, Vec<(u16, Vec<u8>)>);
/// What a callback heard: Ok(bytes or id, mime), or Err(code, text).
type Heard = Result<(Vec<u8>, String), (u16, String)>;

thread_local! {
    static SENT: RefCell<Vec<Sent>> = const { RefCell::new(Vec::new()) };
    static CHUNK: Cell<u32> = const { Cell::new(0) };
    static HEARD: RefCell<Vec<Heard>> = const { RefCell::new(Vec::new()) };
    static FREED: Cell<usize> = const { Cell::new(0) };
}

pub(super) unsafe fn hlwrite_chunks(
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
pub(super) unsafe fn hx_conn_media_chunk_size(_h: *const c_void) -> u32 {
    CHUNK.with(|c| c.get())
}
pub(super) unsafe fn inline_media_cap_ok(_h: *mut c_void) -> gboolean {
    glib::ffi::GTRUE
}

unsafe fn text(p: *const c_char, len: usize) -> String {
    if p.is_null() {
        return String::new();
    }
    String::from_utf8_lossy(std::slice::from_raw_parts(p.cast::<u8>(), len)).into_owned()
}

unsafe extern "C" fn heard_download(_h: *mut c_void, r: *const DownloadResult, _u: *mut c_void) {
    let r = &*r;
    let got = if r.bytes.is_null() {
        Err((r.error_code, text(r.error_message, r.error_message_len)))
    } else {
        let b = &*r.bytes;
        let bytes = std::slice::from_raw_parts(b.data, b.len as usize).to_vec();
        let mime = std::ffi::CStr::from_ptr(r.canonical_mime).to_string_lossy();
        Ok((bytes, mime.into_owned()))
    };
    HEARD.with(|h| h.borrow_mut().push(got));
}

unsafe extern "C" fn heard_upload(_h: *mut c_void, r: *const UploadResult, _u: *mut c_void) {
    let r = &*r;
    let got = if r.media_id.is_null() {
        Err((r.error_code, text(r.error_message, r.error_message_len)))
    } else {
        let id = std::slice::from_raw_parts(r.media_id, r.media_id_len).to_vec();
        Ok((id, text(r.media_type, r.media_type_len)))
    };
    HEARD.with(|h| h.borrow_mut().push(got));
}

unsafe extern "C" fn freed(_u: *mut c_void) {
    FREED.with(|f| f.set(f.get() + 1));
}

const HTLC: *mut c_void = 0x40 as *mut _;
const USER: *mut c_void = 0x50 as *mut _;

/// A fresh start: nothing sent, expected or heard, and parts of `chunk`.
fn reset(chunk: u32) {
    unsafe { forget(HTLC) };
    expected::take();
    SENT.with(|s| s.borrow_mut().clear());
    HEARD.with(|h| h.borrow_mut().clear());
    FREED.with(|f| f.set(0));
    CHUNK.with(|c| c.set(chunk));
}

fn sent() -> Vec<Sent> {
    SENT.with(|s| std::mem::take(&mut *s.borrow_mut()))
}

fn heard() -> Vec<Heard> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

/// The trans and expectation of the last request sent.
fn last_said() -> (u32, Expect) {
    *expected::SAID
        .with(|s| s.borrow().last().copied())
        .as_ref()
        .unwrap()
}

fn chunk_of(r: &Sent, tag: u16) -> Option<Vec<u8>> {
    r.1.iter().find(|(t, _)| *t == tag).map(|(_, d)| d.clone())
}

const PAYLOAD: u16 = 0x0203;
const TOKEN: u16 = 0x0208;
const INDEX: u16 = 0x0209;

fn uploaded_media() -> ChatMedia {
    ChatMedia {
        id: b"handle".to_vec(),
        mime: b"image/png".to_vec(),
        width: Some(4),
        height: None,
        bytes: None,
    }
}

unsafe fn upload(payload: &[u8]) -> bool {
    let mime = b"image/png";
    hx_send_upload_media(
        HTLC,
        payload.as_ptr(),
        payload.len(),
        mime.as_ptr().cast(),
        mime.len(),
        Some(heard_upload),
        USER,
        Some(freed),
    ) != glib::ffi::GFALSE
}

#[test]
fn a_picture_that_fits_goes_up_whole() {
    reset(0);
    unsafe {
        assert!(upload(b"png"));
        assert_eq!(last_said().1, Expect::MediaUpload { last: true });
        uploaded(HTLC, last_said().0, &uploaded_media());
    }
    assert_eq!(sent().len(), 1);
    assert_eq!(heard(), [Ok((b"handle".to_vec(), "image/png".into()))]);
    assert_eq!(
        FREED.with(|f| f.get()),
        0,
        "the caller's to free once heard"
    );
}

#[test]
fn a_larger_picture_goes_in_parts_on_the_first_reply_s_token() {
    reset(4);
    unsafe {
        assert!(upload(b"0123456789"));
        assert_eq!(last_said().1, Expect::MediaUpload { last: false });
        uploading(HTLC, last_said().0, Some(b"tok"));
        assert_eq!(last_said().1, Expect::MediaUpload { last: false });
        // A later reply need not repeat the token.
        uploading(HTLC, last_said().0, None);
        assert_eq!(last_said().1, Expect::MediaUpload { last: true });
        uploaded(HTLC, last_said().0, &uploaded_media());
    }
    let sent = sent();
    let parts: Vec<_> = sent
        .iter()
        .map(|r| {
            (
                chunk_of(r, PAYLOAD).unwrap(),
                chunk_of(r, INDEX).unwrap(),
                chunk_of(r, TOKEN),
            )
        })
        .collect();
    let tok = Some(b"tok".to_vec());
    assert_eq!(
        parts,
        [
            (b"0123".to_vec(), vec![0, 0], None),
            (b"4567".to_vec(), vec![0, 1], tok.clone()),
            (b"89".to_vec(), vec![0, 2], tok),
        ]
    );
    assert_eq!(heard(), [Ok((b"handle".to_vec(), "image/png".into()))]);
}

#[test]
fn parts_with_no_token_or_one_too_long_fail() {
    for token in [None, Some(vec![b't'; MAX_TOKEN + 1])] {
        reset(4);
        unsafe {
            assert!(upload(b"0123456789"));
            uploading(HTLC, last_said().0, token.as_deref());
        }
        assert_eq!(sent().len(), 1, "{token:?}");
        assert_eq!(heard(), [Err((0, String::new()))], "{token:?}");
    }
}

#[test]
fn a_refusal_says_the_server_s_code_and_reason() {
    reset(0);
    unsafe {
        upload(b"png");
        failed(
            HTLC,
            last_said().0,
            MediaErrorCode::PayloadTooLarge,
            Some("Too big."),
        );
    }
    assert_eq!(heard(), [Err((1, "Too big.".into()))]);
}

#[test]
fn a_closed_connection_hands_an_upload_s_state_back_and_ends_downloads_silently() {
    reset(0);
    let id = b"id";
    unsafe {
        upload(b"png");
        let up = last_said().0;
        inline_media_download_start(HTLC, id.as_ptr(), id.len(), Some(heard_download), USER);
        let down = last_said().0;
        // Another connection's replies are not these.
        uploaded(0x41 as *mut _, up, &uploaded_media());
        forget(HTLC);
        uploaded(HTLC, up, &uploaded_media());
        failed(HTLC, down, MediaErrorCode::Generic, None);
    }
    assert!(heard().is_empty());
    assert_eq!(FREED.with(|f| f.get()), 1);
}

fn part_of(payload: &[u8], parts: u16, last: bool) -> MediaPart {
    MediaPart {
        payload: payload.to_vec(),
        mime: if last {
            Vec::new()
        } else {
            b"image/gif".to_vec()
        },
        parts,
        last,
    }
}

#[test]
fn a_picture_comes_down_part_by_part() {
    reset(0);
    let id = b"id";
    unsafe {
        let dl =
            inline_media_download_start(HTLC, id.as_ptr(), id.len(), Some(heard_download), USER);
        assert!(!dl.is_null());
        assert_eq!(last_said().1, Expect::MediaDownload);
        part(HTLC, last_said().0, &part_of(b"GIF", 2, false));
        part(HTLC, last_said().0, &part_of(b"89a", 2, true));
        // Done: cancelling it now is harmless.
        inline_media_download_cancel(dl);
    }
    let asked: Vec<_> = sent().iter().map(|r| chunk_of(r, INDEX)).collect();
    assert_eq!(asked, [None, Some(vec![0, 1])]);
    // The type is the first part's, the only one to give it.
    assert_eq!(heard(), [Ok((b"GIF89a".to_vec(), "image/gif".into()))]);
}

#[test]
fn a_cancelled_download_says_nothing_and_an_endless_one_fails() {
    reset(0);
    let id = b"id";
    unsafe {
        let dl =
            inline_media_download_start(HTLC, id.as_ptr(), id.len(), Some(heard_download), USER);
        inline_media_download_cancel(dl);
        part(HTLC, last_said().0, &part_of(b"GIF", 1, true));
        assert!(heard().is_empty());

        inline_media_download_start(HTLC, id.as_ptr(), id.len(), Some(heard_download), USER);
        // Two parts said, and the second not the last either.
        part(HTLC, last_said().0, &part_of(b"G", 2, false));
        part(HTLC, last_said().0, &part_of(b"I", 2, false));
    }
    assert_eq!(heard(), [Err((0, String::new()))]);
}

#[test]
fn a_part_is_the_size_the_server_names_up_to_the_default() {
    for (named, used) in [(0, CHUNK_SIZE), (32_000, 32_000), (5 << 20, CHUNK_SIZE)] {
        CHUNK.with(|c| c.set(named));
        assert_eq!(unsafe { chunk_size(HTLC) }, used, "{named}");
    }
}
