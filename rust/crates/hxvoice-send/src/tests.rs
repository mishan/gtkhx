//! Send-path unit tests, ported from `tests/proto/test_voice.c` (the
//! `/proto/voice/send/*` cases). The native `hxproto::voice` builders
//! run for real; the C send-path primitives are stubbed here (recording the
//! chunks the wrapper handed to `hlwrite_chunks`) so the cargo-test build
//! needs no network.c / tasks.c / rcv.c. The `/proto/voice/parse/*` cases
//! stay in test_voice.c — they exercise hxproto's C ABI directly and
//! don't touch the senders.

use super::*;
use std::cell::{Cell, RefCell};

use hxproto::build::HxChunk;

// Data-chunk tags + opcodes the assertions pin (hotline.h).
const TAG_CHAT_ID: u16 = 0x0072;
const TAG_VOICE_SDP: u16 = 0x01f5;
const TAG_VOICE_ICE: u16 = 0x01f6;
const TAG_VOICE_MUTED: u16 = 0x01f8;

/// One captured `hlwrite_chunks` call: the wire opcode + each chunk's
/// (tag, copied data bytes).
struct Sent {
    ty: u32,
    chunks: Vec<(u16, Vec<u8>)>,
}

thread_local! {
    static CAP: Cell<bool> = const { Cell::new(false) };
    static VIDEO_CAP: Cell<bool> = const { Cell::new(false) };
    static LAST_LABEL: RefCell<Option<String>> = const { RefCell::new(None) };
    static LAST_TAG: Cell<usize> = const { Cell::new(0) };
    static LAST_SEND: RefCell<Option<Sent>> = const { RefCell::new(None) };
}

// ---- C send-path stubs (lib.rs imports these under cfg(test)) -------

pub(crate) unsafe extern "C" fn hx_htlc_voice_cap(_htlc: *mut c_void) -> glib::ffi::gboolean {
    if CAP.with(|c| c.get()) {
        glib::ffi::GTRUE
    } else {
        glib::ffi::GFALSE
    }
}

pub(crate) unsafe extern "C" fn hx_htlc_video_cap(_htlc: *mut c_void) -> glib::ffi::gboolean {
    if VIDEO_CAP.with(|c| c.get()) {
        glib::ffi::GTRUE
    } else {
        glib::ffi::GFALSE
    }
}

pub(crate) unsafe extern "C" fn hlwrite_chunks(
    _htlc: *mut c_void,
    ty: u32,
    _flag: u32,
    chunks: *const HxChunk,
    hc: c_int,
) {
    let mut v = Vec::new();
    for i in 0..hc.max(0) as usize {
        let ch = &*chunks.add(i);
        let data = if ch.data.is_null() || ch.len == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(ch.data, ch.len as usize).to_vec()
        };
        v.push((ch.tag, data));
    }
    LAST_SEND.with(|s| *s.borrow_mut() = Some(Sent { ty, chunks: v }));
}

pub(crate) unsafe extern "C" fn task_new(
    _htlc: *mut c_void,
    _rcv: RcvTaskFn,
    ptr: *mut c_void,
    _data: *mut c_void,
    str_: *const c_char,
) -> *mut c_void {
    LAST_TAG.with(|t| t.set(ptr as usize));
    let label = if str_.is_null() {
        None
    } else {
        Some(
            std::ffi::CStr::from_ptr(str_)
                .to_string_lossy()
                .into_owned(),
        )
    };
    LAST_LABEL.with(|l| *l.borrow_mut() = label);
    std::ptr::null_mut()
}

pub(crate) unsafe extern "C" fn rcv_task_voice_join(
    _h: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    _p: *mut c_void,
    _d: *mut c_void,
) {
}

pub(crate) unsafe extern "C" fn rcv_task_voice_simple_ack(
    _h: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    _p: *mut c_void,
    _d: *mut c_void,
) {
}

// ---- helpers --------------------------------------------------------

fn reset(cap: bool) {
    CAP.with(|c| c.set(cap));
    LAST_SEND.with(|s| *s.borrow_mut() = None);
}

fn last() -> Option<Sent> {
    LAST_SEND.with(|s| s.borrow_mut().take())
}

/// A non-NULL opaque htlc token (the stubs ignore its contents).
fn htlc() -> *mut c_void {
    std::ptr::dangling_mut::<c_void>()
}

// ---- tests ----------------------------------------------------------

#[test]
fn send_skipped_without_cap() {
    reset(false);
    assert_eq!(unsafe { hx_send_voice_join(htlc(), 0) }, glib::ffi::GFALSE);
    assert!(last().is_none()); // nothing written
}

#[test]
fn send_join_with_cap_writes_chat_id() {
    reset(true);
    assert_eq!(unsafe { hx_send_voice_join(htlc(), 42) }, glib::ffi::GTRUE);
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_VOICE_JOIN);
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[0].1, vec![0, 0, 0, 42]); // big-endian 42
}

#[test]
fn send_leave_emits_chat_id() {
    reset(true);
    assert_eq!(unsafe { hx_send_voice_leave(htlc(), 7) }, glib::ffi::GTRUE);
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_VOICE_LEAVE);
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[0].1[3], 7);
}

#[test]
fn send_sdp_answer_emits_chat_id_and_sdp() {
    reset(true);
    let sdp = b"v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\n";
    assert_eq!(
        unsafe { hx_send_voice_sdp_answer(htlc(), 3, sdp.as_ptr(), sdp.len()) },
        glib::ffi::GTRUE
    );
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_VOICE_SDP_ANSWER);
    assert_eq!(s.chunks.len(), 2);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[0].1.len(), 4);
    assert_eq!(s.chunks[1].0, TAG_VOICE_SDP);
    assert_eq!(s.chunks[1].1, sdp);
}

#[test]
fn send_sdp_answer_rejects_empty() {
    reset(true);
    let empty = b"";
    assert_eq!(
        unsafe { hx_send_voice_sdp_answer(htlc(), 0, empty.as_ptr(), 0) },
        glib::ffi::GFALSE
    );
    assert!(last().is_none());
}

#[test]
fn send_ice_allows_end_of_candidates() {
    reset(true);
    // End-of-candidates marker: NULL / 0-length ICE.
    assert_eq!(
        unsafe { hx_send_voice_ice(htlc(), 9, std::ptr::null(), 0) },
        glib::ffi::GTRUE
    );
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_VOICE_ICE);
    assert_eq!(s.chunks.len(), 2);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[1].0, TAG_VOICE_ICE);
    assert_eq!(s.chunks[1].1.len(), 0); // empty ICE chunk
}

#[test]
fn send_ice_rejects_null_with_nonzero_len() {
    reset(true);
    // A NULL ice pointer is the end-of-candidates marker ONLY with len == 0.
    // (NULL, non-zero) is a caller bug — reject rather than emit a spurious
    // EOC frame that would corrupt negotiation.
    assert_eq!(
        unsafe { hx_send_voice_ice(htlc(), 9, std::ptr::null(), 5) },
        glib::ffi::GFALSE
    );
    assert!(last().is_none());
}

#[test]
fn send_mute_normalises_to_zero_or_one() {
    reset(true);
    // Non-canonical TRUE (42) → the wrapper normalises to wire 1.
    assert_eq!(
        unsafe { hx_send_voice_mute(htlc(), 4, 42) },
        glib::ffi::GTRUE
    );
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_VOICE_MUTE);
    let muted = s
        .chunks
        .iter()
        .find(|(tag, _)| *tag == TAG_VOICE_MUTED)
        .expect("VOICE_MUTED chunk");
    assert_eq!(muted.1, vec![0, 1]); // u16 BE = 1
}

// ---- Video ----

const TAG_VIDEO_KIND: u16 = 0x0220;
const TAG_VIDEO_PAUSED: u16 = 0x0221;
const TAG_VIDEO_SUBSCRIPTIONS: u16 = 0x0225;

fn video_setup(voice: bool, video: bool) {
    CAP.with(|c| c.set(voice));
    VIDEO_CAP.with(|c| c.set(video));
    LAST_SEND.with(|s| *s.borrow_mut() = None);
    LAST_LABEL.with(|l| *l.borrow_mut() = None);
}

/// A captured send: the opcode and each chunk's (tag, bytes).
type SentFrame = (u32, Vec<(u16, Vec<u8>)>);

fn last_video() -> Option<SentFrame> {
    last().map(|x| (x.ty, x.chunks))
}

fn label() -> Option<String> {
    LAST_LABEL.with(|l| l.borrow().clone())
}

/// The last task's ptr slot: the generation, for 607 and 609.
fn tag() -> usize {
    LAST_TAG.with(|t| t.get())
}

#[test]
fn video_sends_need_both_caps() {
    unsafe {
        video_setup(true, false);
        assert_eq!(hx_send_video_start(htlc(), 1, 1, 1), glib::ffi::GFALSE);
        video_setup(false, true);
        assert_eq!(hx_send_video_start(htlc(), 1, 1, 1), glib::ffi::GFALSE);
        assert!(last_video().is_none());
    }
}

#[test]
fn video_start_carries_the_kind_in_frame_and_label() {
    unsafe {
        video_setup(true, true);
        assert_eq!(hx_send_video_start(htlc(), 9, 2, 41), glib::ffi::GTRUE);
        let (ty, chunks) = last_video().unwrap();
        assert_eq!(ty, 607);
        assert_eq!(chunks.len(), 2, "the generation stays off the wire");
        assert_eq!(chunks[0], (TAG_CHAT_ID, 9u32.to_be_bytes().to_vec()));
        assert_eq!(chunks[1], (TAG_VIDEO_KIND, vec![0, 2]));
        assert_eq!(label().as_deref(), Some("video-start-screen"));
        assert_eq!(tag(), 41, "the task keeps the generation");
        // Kind 0 and the reserved kinds never reach the wire.
        video_setup(true, true);
        assert_eq!(hx_send_video_start(htlc(), 9, 0, 1), glib::ffi::GFALSE);
        assert_eq!(hx_send_video_start(htlc(), 9, 3, 1), glib::ffi::GFALSE);
        assert!(last_video().is_none());
    }
}

#[test]
fn video_stop_with_and_without_kind() {
    unsafe {
        video_setup(true, true);
        hx_send_video_stop(htlc(), 4, 0);
        let (ty, chunks) = last_video().unwrap();
        assert_eq!(ty, 608);
        assert_eq!(chunks.len(), 1, "kind 0 omits the field: stop everything");
        hx_send_video_stop(htlc(), 4, 1);
        let (_, chunks) = last_video().unwrap();
        assert_eq!(chunks[1], (TAG_VIDEO_KIND, vec![0, 1]));
    }
}

#[test]
fn video_state_normalizes_paused() {
    unsafe {
        video_setup(true, true);
        hx_send_video_state(htlc(), 4, 1, 7, 12);
        let (ty, chunks) = last_video().unwrap();
        assert_eq!(ty, 609);
        assert_eq!(chunks.len(), 3, "the generation stays off the wire");
        assert_eq!(chunks[2], (TAG_VIDEO_PAUSED, vec![0, 1]));
        assert_eq!(label().as_deref(), Some("video-state-camera"));
        assert_eq!(tag(), 12);
        hx_send_video_state(htlc(), 4, 2, 0, 13);
        let (_, chunks) = last_video().unwrap();
        assert_eq!(chunks[2], (TAG_VIDEO_PAUSED, vec![0, 0]));
        assert_eq!(label().as_deref(), Some("video-state-screen"));
    }
}

#[test]
fn video_subscribe_drops_undefined_kinds() {
    unsafe {
        video_setup(true, true);
        // uid 5 camera, uid 6 screen-audio (reserved), uid 7 screen.
        let blob = [0u8, 5, 0, 1, 0, 6, 0, 3, 0, 7, 0, 2];
        hx_send_video_subscribe(htlc(), 3, blob.as_ptr(), blob.len());
        let (ty, chunks) = last_video().unwrap();
        assert_eq!(ty, 610);
        assert_eq!(
            chunks[1],
            (TAG_VIDEO_SUBSCRIPTIONS, vec![0, 5, 0, 1, 0, 7, 0, 2])
        );
        // The empty set is sent, as an empty field.
        hx_send_video_subscribe(htlc(), 3, std::ptr::null(), 0);
        let (_, chunks) = last_video().unwrap();
        assert_eq!(chunks[1], (TAG_VIDEO_SUBSCRIPTIONS, vec![]));
    }
}
