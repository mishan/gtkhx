//! The voice and video senders and what becomes of their replies, against
//! doubles of the C send primitive, the voice runtime and the voice model.

use super::*;
use std::cell::{Cell, RefCell};
use std::ffi::CStr;

use hxproto::build::HxChunk;

use crate::send::expected;

type Sent = (u32, Vec<(u16, Vec<u8>)>);

/// What the runtime, the model or the speaker were told.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Told {
    TaskError(u32, Option<String>),
    StartFailed(u32, u16, u32, Option<String>),
    StateFailed(u32, u16, u32, Option<String>),
    RoomStatus(u32, Vec<u8>),
    Offer(u32, String),
    Model(Vec<u8>),
    Sound(c_int),
}

thread_local! {
    static SENT: RefCell<Vec<Sent>> = const { RefCell::new(Vec::new()) };
    static TOLD: RefCell<Vec<Told>> = const { RefCell::new(Vec::new()) };
    static VOICE: Cell<bool> = const { Cell::new(true) };
    static VIDEO: Cell<bool> = const { Cell::new(true) };
    static RUNTIME: Cell<bool> = const { Cell::new(true) };
    static ACTIVE: Cell<Option<u32>> = const { Cell::new(None) };
}

fn tell(t: Told) {
    TOLD.with(|x| x.borrow_mut().push(t));
}

unsafe fn text(p: *const c_char) -> Option<String> {
    (!p.is_null()).then(|| CStr::from_ptr(p).to_string_lossy().into_owned())
}

fn gbool(b: bool) -> gboolean {
    if b {
        GTRUE
    } else {
        GFALSE
    }
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
pub(super) unsafe fn hx_htlc_voice_cap(_htlc: *mut c_void) -> gboolean {
    gbool(VOICE.with(|v| v.get()))
}
pub(super) unsafe fn hx_htlc_video_cap(_htlc: *mut c_void) -> gboolean {
    gbool(VIDEO.with(|v| v.get()))
}
pub(super) unsafe fn hx_sess_from_htlc(htlc: *mut c_void) -> *mut c_void {
    htlc
}
pub(super) unsafe fn hx_session_voice_runtime(_sess: *mut c_void) -> *mut c_void {
    if RUNTIME.with(|r| r.get()) {
        0x40 as *mut c_void
    } else {
        std::ptr::null_mut()
    }
}
pub(super) unsafe fn hx_session_voice_model(_sess: *mut c_void) -> *mut c_void {
    0x50 as *mut c_void
}
pub(super) unsafe fn play_sound(sound: c_int) {
    tell(Told::Sound(sound));
}
pub(super) unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}
pub(super) unsafe fn gtkhx_voice_runtime_task_error(
    _rt: *mut c_void,
    opcode: u32,
    t: *const c_char,
) {
    tell(Told::TaskError(opcode, text(t)));
}
pub(super) unsafe fn gtkhx_voice_runtime_video_start_failed(
    _rt: *mut c_void,
    cid: u32,
    kind: u16,
    gen: u32,
    t: *const c_char,
) {
    tell(Told::StartFailed(cid, kind, gen, text(t)));
}
pub(super) unsafe fn gtkhx_voice_runtime_video_state_failed(
    _rt: *mut c_void,
    cid: u32,
    kind: u16,
    gen: u32,
    t: *const c_char,
) {
    tell(Told::StateFailed(cid, kind, gen, text(t)));
}
pub(super) unsafe fn gtkhx_voice_runtime_room_status(
    _rt: *mut c_void,
    cid: u32,
    blob: *const u8,
    len: usize,
) {
    tell(Told::RoomStatus(
        cid,
        std::slice::from_raw_parts(blob, len).to_vec(),
    ));
}
pub(super) unsafe fn gtkhx_voice_runtime_sdp_offer(_rt: *mut c_void, cid: u32, sdp: *const c_char) {
    tell(Told::Offer(cid, text(sdp).unwrap()));
}
pub(super) unsafe fn gtkhx_voice_runtime_active_cid(_rt: *mut c_void, out: *mut u32) -> i32 {
    match ACTIVE.with(|a| a.get()) {
        Some(cid) => {
            *out = cid;
            1
        }
        None => 0,
    }
}
pub(super) unsafe fn hx_voice_model_ingest_participants(
    _model: *mut c_void,
    blob: *const u8,
    len: usize,
    _video_cap: gboolean,
) {
    tell(Told::Model(std::slice::from_raw_parts(blob, len).to_vec()));
}

const HTLC: *mut c_void = 0x30 as *mut _;

fn reset() {
    VOICE.with(|v| v.set(true));
    VIDEO.with(|v| v.set(true));
    RUNTIME.with(|r| r.set(true));
    ACTIVE.with(|a| a.set(None));
    SENT.with(|s| s.borrow_mut().clear());
    TOLD.with(|t| t.borrow_mut().clear());
    expected::take();
    forget(HTLC);
}

fn sent() -> Vec<Sent> {
    SENT.with(|s| std::mem::take(&mut *s.borrow_mut()))
}

fn told() -> Vec<Told> {
    TOLD.with(|t| std::mem::take(&mut *t.borrow_mut()))
}

#[test]
fn each_request_goes_out_with_its_reply_expected() {
    reset();
    let streams = [0u8, 5, 0, 1];
    let sdp = b"v=0\r\n";
    let ok = unsafe {
        [
            hx_send_voice_join(HTLC, 2),
            hx_send_voice_leave(HTLC, 2),
            hx_send_voice_sdp_answer(HTLC, 2, sdp.as_ptr(), sdp.len()),
            hx_send_voice_ice(HTLC, 2, std::ptr::null(), 0),
            hx_send_voice_mute(HTLC, 2, 42),
            hx_send_video_start(HTLC, 2, 1, 7),
            hx_send_video_stop(HTLC, 2, 0),
            hx_send_video_state(HTLC, 2, 2, GTRUE, 8),
            hx_send_video_subscribe(HTLC, 2, streams.as_ptr(), streams.len()),
        ]
    };
    assert!(ok.iter().all(|&b| b == GTRUE));
    let room = (0x0072, vec![0, 0, 0, 2]);
    let opcodes: Vec<u32> = sent().iter().map(|s| s.0).collect();
    assert_eq!(opcodes, [600, 601, 603, 604, 606, 607, 608, 609, 610]);
    // Every request but the ICE candidate has its reply expected.
    let said: Vec<Expect> = expected::take().into_iter().map(|(_, e)| e).collect();
    let mut want = vec![Expect::VoiceJoin { cid: 2 }];
    want.extend([Expect::Voice; 7]);
    assert_eq!(said, want);
    // Each request's bytes are hxrequest's, pinned there.
    unsafe { hx_send_voice_mute(HTLC, 2, 42) };
    assert_eq!(
        sent(),
        [(606, vec![room, (0x01f8, vec![0, 1])])],
        "any true mutes"
    );
}

#[test]
fn nothing_goes_out_that_the_server_did_not_agree_to_or_cannot_be_sent() {
    reset();
    let streams = [0u8, 5, 0, 1];
    unsafe {
        VOICE.with(|v| v.set(false));
        assert_eq!(hx_send_voice_join(HTLC, 0), GFALSE);
        assert_eq!(hx_send_voice_ice(HTLC, 0, std::ptr::null(), 0), GFALSE);
        assert_eq!(hx_send_video_stop(HTLC, 0, 0), GFALSE);
        VOICE.with(|v| v.set(true));
        VIDEO.with(|v| v.set(false));
        assert_eq!(hx_send_video_start(HTLC, 0, 1, 1), GFALSE);
        assert_eq!(
            hx_send_video_subscribe(HTLC, 0, streams.as_ptr(), 4),
            GFALSE
        );
        VIDEO.with(|v| v.set(true));
        // No answer, a candidate pointer with no bytes behind it, and kinds
        // the extension doesn't define.
        assert_eq!(
            hx_send_voice_sdp_answer(HTLC, 0, std::ptr::null(), 0),
            GFALSE
        );
        assert_eq!(hx_send_voice_ice(HTLC, 0, std::ptr::null(), 5), GFALSE);
        assert_eq!(hx_send_video_start(HTLC, 0, 0, 1), GFALSE);
        assert_eq!(hx_send_video_start(HTLC, 0, 3, 1), GFALSE);
        assert_eq!(hx_send_video_state(HTLC, 0, 3, GFALSE, 1), GFALSE);
        assert_eq!(hx_send_video_stop(HTLC, 0, 3), GFALSE);
        assert_eq!(hx_send_voice_join(std::ptr::null_mut(), 0), GFALSE);
    }
    assert!(sent().is_empty());
    assert!(expected::take().is_empty());
}

/// Send one request and hand back the trans it went out on.
fn sent_on(send: impl FnOnce()) -> u32 {
    expected::take();
    send();
    sent();
    let said = expected::take();
    assert_eq!(said.len(), 1);
    said[0].0
}

#[test]
fn a_refusal_reaches_the_runtime_once_with_the_error_sound() {
    let sdp = b"v=0\r\n";
    type Send = Box<dyn Fn()>;
    let cases: Vec<(Send, Told)> = vec![
        (
            Box::new(|| unsafe {
                hx_send_voice_join(HTLC, 2);
            }),
            Told::TaskError(600, Some("No.".into())),
        ),
        (
            Box::new(|| unsafe {
                hx_send_voice_leave(HTLC, 2);
            }),
            Told::TaskError(601, Some("No.".into())),
        ),
        (
            Box::new(move || unsafe {
                hx_send_voice_sdp_answer(HTLC, 2, sdp.as_ptr(), sdp.len());
            }),
            Told::TaskError(603, Some("No.".into())),
        ),
        (
            Box::new(|| unsafe {
                hx_send_voice_mute(HTLC, 2, GTRUE);
            }),
            Told::TaskError(606, Some("No.".into())),
        ),
        (
            Box::new(|| unsafe {
                hx_send_video_start(HTLC, 2, 2, 41);
            }),
            Told::StartFailed(2, 2, 41, Some("No.".into())),
        ),
        (
            Box::new(|| unsafe {
                hx_send_video_stop(HTLC, 2, 1);
            }),
            Told::TaskError(608, Some("No.".into())),
        ),
        (
            Box::new(|| unsafe {
                hx_send_video_state(HTLC, 2, 1, GTRUE, 12);
            }),
            Told::StateFailed(2, 1, 12, Some("No.".into())),
        ),
        (
            Box::new(|| unsafe {
                hx_send_video_subscribe(HTLC, 2, std::ptr::null(), 0);
            }),
            Told::TaskError(610, Some("No.".into())),
        ),
    ];
    for (send, want) in cases {
        reset();
        let t = sent_on(send);
        assert!(unsafe { failed(HTLC, t, Some("No.")) });
        assert_eq!(told(), [want.clone(), Told::Sound(SOUND_ERROR)]);
        // Answered: a second refusal on the trans is someone else's.
        assert!(!unsafe { failed(HTLC, t, Some("No.")) });
        assert!(told().is_empty());
    }
}

#[test]
fn a_refusal_without_a_runtime_or_with_nothing_to_say() {
    reset();
    let join = || unsafe {
        hx_send_voice_join(HTLC, 2);
    };
    // With no runtime, the refusal is the generic toast's.
    RUNTIME.with(|r| r.set(false));
    let t = sent_on(join);
    assert!(!unsafe { failed(HTLC, t, Some("No.")) });
    RUNTIME.with(|r| r.set(true));
    // No reason, or an empty one, reaches the runtime as none.
    for reason in [None, Some("")] {
        let t = sent_on(join);
        assert!(unsafe { failed(HTLC, t, reason) });
        assert_eq!(
            told(),
            [Told::TaskError(600, None), Told::Sound(SOUND_ERROR)]
        );
    }
    // Cut short: never answered, so nothing to tell anyone.
    let t = sent_on(join);
    assert!(unsafe { failed(HTLC, t, Some(CUT_SHORT)) });
    // Not ours, or done with: it went through, or the connection closed.
    assert!(!unsafe { failed(HTLC, 999, Some("No.")) });
    let t = sent_on(|| unsafe {
        hx_send_voice_mute(HTLC, 2, GTRUE);
    });
    done(HTLC, t);
    assert!(!unsafe { failed(HTLC, t, Some("No.")) });
    assert_eq!(
        ASKED.with(|a| a.borrow().len()),
        0,
        "a success left its entry"
    );
    let t = sent_on(join);
    forget(HTLC);
    assert!(!unsafe { failed(HTLC, t, Some("No.")) });
    assert!(told().is_empty());
}

#[test]
fn each_connection_has_its_own_requests() {
    reset();
    const OTHER: *mut c_void = 0x31 as *mut _;
    forget(OTHER);
    let mute = |h: *mut c_void| {
        sent_on(|| unsafe {
            hx_send_voice_mute(h, 2, GTRUE);
        })
    };
    // Both connections number their own requests, so the same trans may
    // be in flight on each.
    let (a, b) = (mute(HTLC), mute(OTHER));
    // A refusal on one is not the other's, and closing one keeps the
    // other's in flight.
    assert!(!unsafe { failed(OTHER, a + b + 1, Some("No.")) });
    forget(HTLC);
    assert!(!unsafe { failed(HTLC, a, Some("No.")) });
    assert!(unsafe { failed(OTHER, b, Some("No.")) });
    assert_eq!(
        told(),
        [
            Told::TaskError(606, Some("No.".into())),
            Told::Sound(SOUND_ERROR)
        ]
    );
}

#[test]
fn a_join_starts_the_answer_and_fills_the_room_it_is_for() {
    let blob = vec![0u8, 3, 0, 1, 0, 0];
    // The model takes the room this client is in, and no other.
    for (active, model) in [(Some(2), true), (Some(5), false), (None, false)] {
        reset();
        ACTIVE.with(|a| a.set(active));
        let t = sent_on(|| unsafe {
            hx_send_voice_join(HTLC, 2);
        });
        unsafe { joined(HTLC, t, 2, b"v=0\r\n", b"PCMU", &blob) };
        let mut want = vec![
            Told::RoomStatus(2, blob.clone()),
            Told::Offer(2, "v=0\r\n".into()),
        ];
        if model {
            want.push(Told::Model(blob.clone()));
        }
        assert_eq!(told(), want, "active room {active:?}");
        assert!(!unsafe { failed(HTLC, t, Some("No.")) }, "still asked");
    }
    // No runtime: nothing to start.
    reset();
    RUNTIME.with(|r| r.set(false));
    unsafe { joined(HTLC, 1, 2, b"v=0\r\n", b"PCMU", &blob) };
    assert!(told().is_empty());
}
