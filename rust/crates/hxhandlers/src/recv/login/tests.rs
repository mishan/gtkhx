use std::cell::RefCell;
use std::ffi::CStr;

use gtkhx_core::conn::{
    hx_conn_caps, hx_conn_free, hx_conn_history_max_days, hx_conn_history_max_msgs,
    hx_conn_logged_in, hx_conn_media_chunk_size, hx_conn_media_max_bytes, hx_conn_media_max_pixels,
    hx_conn_new, hx_conn_uid, hx_conn_version, hx_conn_video_limits, VideoLimits,
};
use hxproto::inline_media::LimitsAdvertisement;
use hxproto::video::{Limits, VideoKind};

use super::*;

#[derive(Debug, PartialEq)]
enum Emitted {
    LoggedIn(Option<String>),
    RequestFailed(String),
}

thread_local! {
    static EMITTED: RefCell<Vec<Emitted>> = const { RefCell::new(Vec::new()) };
}

fn text(p: *const c_char) -> Option<String> {
    (!p.is_null()).then(|| unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
}

pub(super) unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}
pub(super) unsafe fn gtkhx_session_emit_logged_in(
    _s: *mut c_void,
    _h: *mut c_void,
    name: *const c_char,
) {
    EMITTED.with(|e| e.borrow_mut().push(Emitted::LoggedIn(text(name))));
}
pub(super) unsafe fn gtkhx_session_emit_request_failed(
    _s: *mut c_void,
    _h: *mut c_void,
    reason: *const c_char,
) {
    EMITTED.with(|e| {
        e.borrow_mut()
            .push(Emitted::RequestFailed(text(reason).unwrap()))
    });
}
pub(super) unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}

fn emitted() -> Vec<Emitted> {
    EMITTED.with(|e| std::mem::take(&mut *e.borrow_mut()))
}

const CAMERA: Limits = Limits {
    kind: VideoKind::Camera,
    max_width: 1280,
    max_height: 720,
    max_fps: 30,
    max_bitrate: 1_500_000,
    max_per_room: 8,
};

fn video(h: *mut gtkhx_core::conn::HtlcConn, kind: u16) -> Option<(u16, u32)> {
    let mut l = VideoLimits::default();
    (unsafe { hx_conn_video_limits(h, kind, &mut l) } != 0).then_some((l.max_width, l.max_bitrate))
}

/// A server that said everything, then one that said nothing, on the same
/// connection: nothing of the first outlives the second's reply but our
/// uid, which only a server that names it changes.
#[test]
fn the_reply_lands_on_the_connection_and_tells_the_view() {
    let h = hx_conn_new();
    let full = ServerInfo {
        version: 200,
        name: Some("Janus".into()),
        uid: Some(42),
        caps: cap::TEXT_ENCODING | cap::INLINE_MEDIA,
        media: LimitsAdvertisement {
            max_bytes: Some(262_144),
            chunk_size: Some(60_000),
            ..LimitsAdvertisement::empty()
        },
        history_max_msgs: Some(500),
        history_max_days: Some(7),
        video: vec![CAMERA],
    };
    let bare = ServerInfo {
        version: 0,
        name: None,
        uid: None,
        caps: 0,
        media: LimitsAdvertisement::empty(),
        history_max_msgs: None,
        history_max_days: None,
        video: vec![],
    };
    unsafe {
        logged_in(h.cast(), &full);
        assert_eq!(hx_conn_logged_in(h), 1);
        assert_eq!(hx_conn_uid(h), 42);
        assert_eq!(hx_conn_version(h), 200);
        assert_eq!(hx_conn_caps(h), u64::from(full.caps));
        assert_eq!(hx_conn_media_max_bytes(h), 262_144);
        assert_eq!(hx_conn_media_chunk_size(h), 60_000);
        assert_eq!(hx_conn_media_max_pixels(h), 0);
        assert_eq!(hx_conn_history_max_msgs(h), 500);
        assert_eq!(hx_conn_history_max_days(h), 7);
        assert_eq!(video(h, 1), Some((1280, 1_500_000)));
        assert_eq!(video(h, 2), None);
        assert_eq!(emitted(), [Emitted::LoggedIn(Some("Janus".into()))]);

        logged_in(h.cast(), &bare);
        assert_eq!(hx_conn_uid(h), 42);
        assert_eq!(hx_conn_version(h), 0);
        assert_eq!(hx_conn_caps(h), 0);
        assert_eq!(hx_conn_media_max_bytes(h), 0);
        assert_eq!(hx_conn_media_chunk_size(h), 0);
        assert_eq!(hx_conn_history_max_msgs(h), 0);
        assert_eq!(hx_conn_history_max_days(h), 0);
        assert_eq!(video(h, 1), None);
        assert_eq!(emitted(), [Emitted::LoggedIn(None)]);
        hx_conn_free(h);
    }
}

#[test]
fn a_refusal_shows_the_servers_reason() {
    unsafe { refused(std::ptr::null_mut(), "Incorrect login.") };
    assert_eq!(
        emitted(),
        [Emitted::RequestFailed("Incorrect login.".into())]
    );
}
