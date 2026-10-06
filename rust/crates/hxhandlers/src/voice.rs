//! Voice and video's requests, and what becomes of their replies.
//!
//! The requests are `hxrequest::voice`'s, behind the `hx_send_voice_*` /
//! `hx_send_video_*` C ABI the voice panel and push-to-talk call. Each that
//! has a reply has it expected by the session: a join's as
//! `Expect::VoiceJoin`, whose offer and room start the call, and the rest
//! as `Expect::Voice`, which is `VoiceDone` once it worked. ICE
//! candidates have no reply.
//!
//! What each request was is kept by connection and trans, for its refusal:
//! the voice runtime reports a refused voice or video request on the voice
//! panel, so the generic toast stays quiet and only the error sound is
//! ours. A refused video start or pause names its kind and the runtime's
//! generation, so the runtime undoes that request and no other. With no
//! runtime on the connection's session the refusal is the generic one.
//! A reply cut short says nothing: it was never answered.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};

use glib::ffi::{gboolean, GFALSE, GTRUE};
use hxproto::messages::ClientHdr;
use hxproto::video::VideoKind;
use hxrequest::{voice, Request};
use hxsession::Expect;

use crate::recv::chat::CUT_SHORT;

/// `ERROR` in sound.h.
const SOUND_ERROR: c_int = 2;

#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;

#[cfg(not(test))]
extern "C" {
    // voice_bridge.c
    fn hx_htlc_voice_cap(htlc: *mut c_void) -> gboolean;
    fn hx_htlc_video_cap(htlc: *mut c_void) -> gboolean;
    fn hx_session_voice_runtime(sess: *mut c_void) -> *mut c_void;
    fn hx_session_voice_model(sess: *mut c_void) -> *mut c_void;
    fn hx_sess_from_htlc(htlc: *mut c_void) -> *mut c_void;
    fn play_sound(sound: c_int);
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
    // hxvoice-runtime and hxvoice-model, in the voice build's link.
    fn gtkhx_voice_runtime_task_error(rt: *mut c_void, origin_opcode: u32, text: *const c_char);
    fn gtkhx_voice_runtime_video_start_failed(
        rt: *mut c_void,
        cid: u32,
        kind: u16,
        gen: u32,
        text: *const c_char,
    );
    fn gtkhx_voice_runtime_video_state_failed(
        rt: *mut c_void,
        cid: u32,
        kind: u16,
        gen: u32,
        text: *const c_char,
    );
    fn gtkhx_voice_runtime_room_status(rt: *mut c_void, cid: u32, blob: *const u8, len: usize);
    fn gtkhx_voice_runtime_sdp_offer(rt: *mut c_void, cid: u32, sdp: *const c_char);
    fn gtkhx_voice_runtime_active_cid(rt: *mut c_void, out_cid: *mut u32) -> i32;
    fn hx_voice_model_ingest_participants(
        model: *mut c_void,
        blob: *const u8,
        len: usize,
        video_cap: gboolean,
    );
}

#[cfg(test)]
use tests::{
    debug_log_str, gtkhx_voice_runtime_active_cid, gtkhx_voice_runtime_room_status,
    gtkhx_voice_runtime_sdp_offer, gtkhx_voice_runtime_task_error,
    gtkhx_voice_runtime_video_start_failed, gtkhx_voice_runtime_video_state_failed, hlwrite_chunks,
    hx_htlc_video_cap, hx_htlc_voice_cap, hx_sess_from_htlc, hx_session_voice_model,
    hx_session_voice_runtime, hx_voice_model_ingest_participants, play_sound,
};

/// A request in flight, as the runtime's refusal path names it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Asked {
    /// Joining, leaving, the answer, muting, stopping video or
    /// subscribing: the opcode, which is all the runtime needs.
    Op(u32),
    /// Starting video of `kind` in room `cid`, the runtime's `gen`.
    VideoStart { cid: u32, kind: u16, gen: u32 },
    /// Pausing or resuming it.
    VideoState { cid: u32, kind: u16, gen: u32 },
}

thread_local! {
    /// The requests in flight, by connection and trans.
    static ASKED: RefCell<HashMap<(usize, u32), Asked>> = RefCell::new(HashMap::new());
}

fn answered(htlc: *mut c_void, trans: u32) -> Option<Asked> {
    ASKED.with(|a| a.borrow_mut().remove(&(htlc as usize, trans)))
}

/// Let go of what `htlc` asked for.
pub(crate) fn forget(htlc: *mut c_void) {
    ASKED.with(|a| a.borrow_mut().retain(|(h, _), _| *h != htlc as usize));
}

unsafe fn log(line: &str) {
    if let Ok(c) = CString::new(line) {
        debug_log_str(c"voice".as_ptr(), c.as_ptr());
    }
}

/// Text for C: up to its first NUL, as C would read it.
fn c_text(s: &[u8]) -> CString {
    let s = s.split(|&b| b == 0).next().unwrap_or_default();
    CString::new(s).expect("cut at the first NUL")
}

/// Send `req`; with `asked`, its reply expected and what it was kept.
unsafe fn send(
    htlc: *mut c_void,
    req: Option<Request>,
    asked: Option<(Expect, Asked)>,
) -> gboolean {
    let Some(req) = req else {
        glib::g_debug!("gtkhx", "voice request {:?} not built", asked);
        return GFALSE;
    };
    if let Some((what, a)) = asked {
        let trans = crate::send::expect_next(htlc, what);
        ASKED.with(|m| m.borrow_mut().insert((htlc as usize, trans), a));
    }
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(
            htlc.cast(),
            req.opcode,
            0,
            chunks.as_ptr(),
            chunks.len() as c_int,
        )
    });
    GTRUE
}

/// Every voice request goes only where the server agreed to voice.
unsafe fn voice_ok(htlc: *mut c_void) -> bool {
    if htlc.is_null() {
        return false;
    }
    if hx_htlc_voice_cap(htlc) == GFALSE {
        glib::g_debug!("gtkhx", "skip voice send: server didn't echo CAP_VOICE");
        return false;
    }
    true
}

/// And video's only where it agreed to video too.
unsafe fn video_ok(htlc: *mut c_void) -> bool {
    if !voice_ok(htlc) {
        return false;
    }
    if hx_htlc_video_cap(htlc) == GFALSE {
        glib::g_debug!("gtkhx", "skip video send: server didn't echo CAP_VIDEO");
        return false;
    }
    true
}

fn op(opcode: ClientHdr) -> Option<(Expect, Asked)> {
    Some((Expect::Voice, Asked::Op(opcode as u32)))
}

/// `gboolean hx_send_voice_join (struct htlc_conn *htlc, guint32 cid)`.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_join(htlc: *mut c_void, cid: u32) -> gboolean {
    if !voice_ok(htlc) {
        return GFALSE;
    }
    let asked = (
        Expect::VoiceJoin { cid },
        Asked::Op(ClientHdr::VoiceJoin as u32),
    );
    send(htlc, voice::join(cid), Some(asked))
}

/// `gboolean hx_send_voice_leave (struct htlc_conn *htlc, guint32 cid)`.
///
/// # Safety
/// As [`hx_send_voice_join`].
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_leave(htlc: *mut c_void, cid: u32) -> gboolean {
    if !voice_ok(htlc) {
        return GFALSE;
    }
    send(htlc, voice::leave(cid), op(ClientHdr::VoiceLeave))
}

/// `gboolean hx_send_voice_sdp_answer (struct htlc_conn *htlc, guint32 cid,
/// const guint8 *sdp, gsize sdp_len)`. An empty answer is not sent.
///
/// # Safety
/// `sdp` is NULL or valid for `sdp_len`; `htlc` as above.
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_sdp_answer(
    htlc: *mut c_void,
    cid: u32,
    sdp: *const u8,
    sdp_len: usize,
) -> gboolean {
    if !voice_ok(htlc) {
        return GFALSE;
    }
    if sdp.is_null() || sdp_len == 0 || sdp_len > isize::MAX as usize {
        glib::g_debug!("gtkhx", "VOICE_SDP_ANSWER: empty SDP rejected");
        return GFALSE;
    }
    let sdp = std::slice::from_raw_parts(sdp, sdp_len);
    send(
        htlc,
        voice::sdp_answer(cid, sdp),
        op(ClientHdr::VoiceSdpAnswer),
    )
}

/// `gboolean hx_send_voice_ice (struct htlc_conn *htlc, guint32 cid,
/// const guint8 *ice, gsize ice_len)`. `(NULL, 0)` is the end of our
/// candidates; NULL with a length is a caller's mistake, not that marker.
///
/// # Safety
/// `ice` is NULL or valid for `ice_len`; `htlc` as above.
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_ice(
    htlc: *mut c_void,
    cid: u32,
    ice: *const u8,
    ice_len: usize,
) -> gboolean {
    if !voice_ok(htlc) {
        return GFALSE;
    }
    if (ice.is_null() && ice_len != 0) || ice_len > isize::MAX as usize {
        glib::g_debug!("gtkhx", "VOICE_ICE: invalid ice ptr/len (len={ice_len})");
        return GFALSE;
    }
    let ice = if ice.is_null() {
        &[][..]
    } else {
        std::slice::from_raw_parts(ice, ice_len)
    };
    send(htlc, voice::ice(cid, ice), None)
}

/// `gboolean hx_send_voice_mute (struct htlc_conn *htlc, guint32 cid,
/// gboolean muted)`.
///
/// # Safety
/// As [`hx_send_voice_join`].
#[no_mangle]
pub unsafe extern "C" fn hx_send_voice_mute(
    htlc: *mut c_void,
    cid: u32,
    muted: gboolean,
) -> gboolean {
    if !voice_ok(htlc) {
        return GFALSE;
    }
    send(
        htlc,
        voice::mute(cid, muted != GFALSE),
        op(ClientHdr::VoiceMute),
    )
}

/// `gboolean hx_send_video_start (struct htlc_conn *htlc, guint32 cid,
/// guint16 kind, guint32 gen)`. `kind` is 1 (camera) or 2 (screen); `gen`
/// is the runtime's number for this start, kept and never sent.
///
/// # Safety
/// As [`hx_send_voice_join`].
#[no_mangle]
pub unsafe extern "C" fn hx_send_video_start(
    htlc: *mut c_void,
    cid: u32,
    kind: u16,
    gen: u32,
) -> gboolean {
    let Some(k) = VideoKind::from_wire(kind) else {
        return GFALSE;
    };
    if !video_ok(htlc) {
        return GFALSE;
    }
    let asked = Asked::VideoStart { cid, kind, gen };
    send(
        htlc,
        voice::video_start(cid, k),
        Some((Expect::Voice, asked)),
    )
}

/// `gboolean hx_send_video_stop (struct htlc_conn *htlc, guint32 cid,
/// guint16 kind)`. `kind` 0 stops every publication in the room.
///
/// # Safety
/// As [`hx_send_voice_join`].
#[no_mangle]
pub unsafe extern "C" fn hx_send_video_stop(htlc: *mut c_void, cid: u32, kind: u16) -> gboolean {
    let kind = match kind {
        0 => None,
        k => match VideoKind::from_wire(k) {
            Some(k) => Some(k),
            None => return GFALSE,
        },
    };
    if !video_ok(htlc) {
        return GFALSE;
    }
    send(htlc, voice::video_stop(cid, kind), op(ClientHdr::VideoStop))
}

/// `gboolean hx_send_video_state (struct htlc_conn *htlc, guint32 cid,
/// guint16 kind, gboolean paused, guint32 gen)`. `gen` as for
/// [`hx_send_video_start`].
///
/// # Safety
/// As [`hx_send_voice_join`].
#[no_mangle]
pub unsafe extern "C" fn hx_send_video_state(
    htlc: *mut c_void,
    cid: u32,
    kind: u16,
    paused: gboolean,
    gen: u32,
) -> gboolean {
    let Some(k) = VideoKind::from_wire(kind) else {
        return GFALSE;
    };
    if !video_ok(htlc) {
        return GFALSE;
    }
    let asked = Asked::VideoState { cid, kind, gen };
    send(
        htlc,
        voice::video_state(cid, k, paused != GFALSE),
        Some((Expect::Voice, asked)),
    )
}

/// `gboolean hx_send_video_subscribe (struct htlc_conn *htlc, guint32 cid,
/// const guint8 *streams, gsize len)`. `streams` is the packed four-byte
/// `uid | kind` array, the complete set wanted; empty is no video at all.
///
/// # Safety
/// `streams` is NULL or valid for `len`; `htlc` as above.
#[no_mangle]
pub unsafe extern "C" fn hx_send_video_subscribe(
    htlc: *mut c_void,
    cid: u32,
    streams: *const u8,
    len: usize,
) -> gboolean {
    if !video_ok(htlc) {
        return GFALSE;
    }
    let streams = if streams.is_null() || len == 0 || len > isize::MAX as usize {
        &[][..]
    } else {
        std::slice::from_raw_parts(streams, len)
    };
    send(
        htlc,
        voice::video_subscribe(cid, streams),
        op(ClientHdr::VideoSubscribe),
    )
}

/// The reply to joining voice in room `cid`: who is in it, for the runtime's
/// map of media to users and, for the room this client is in, the voice
/// model's first list; and the server's offer, which starts the answer.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn joined(
    htlc: *mut c_void,
    trans: u32,
    cid: u32,
    sdp: &[u8],
    codec: &[u8],
    participants: &[u8],
) {
    answered(htlc, trans);
    let sum = hxproto::voice::sdp::summarize(sdp);
    let users: Vec<_> = hxproto::voice::parse_voice_participants(participants).collect();
    log(&format!(
        "← VOICE_JOIN reply cid={cid} codec={} sdp_len={} mids={} has_pcmu={} participants={}",
        String::from_utf8_lossy(codec),
        sdp.len(),
        sum.mids.len() + sum.unknown_mids.len(),
        sum.has_pcmu as i32,
        users.len()
    ));
    for u in &users {
        log(&format!(
            "    uid={} flags=0x{:04x} codec={}{}",
            u.user_id,
            u.flags,
            u.codec_id,
            if u.is_muted() { " MUTED" } else { "" }
        ));
    }
    let sess = hx_sess_from_htlc(htlc);
    let rt = hx_session_voice_runtime(sess);
    if rt.is_null() {
        return;
    }
    gtkhx_voice_runtime_room_status(rt, cid, participants.as_ptr(), participants.len());
    if !sdp.is_empty() {
        gtkhx_voice_runtime_sdp_offer(rt, cid, c_text(sdp).as_ptr());
    }
    // A reply for a room already switched away from would otherwise become
    // the next room's baseline.
    let model = hx_session_voice_model(sess);
    let mut active = 0;
    if !model.is_null() && gtkhx_voice_runtime_active_cid(rt, &mut active) != 0 && active == cid {
        hx_voice_model_ingest_participants(
            model,
            participants.as_ptr(),
            participants.len(),
            hx_htlc_video_cap(htlc),
        );
    }
}

/// A request on `trans` went through, with nothing more to say.
pub(crate) fn done(htlc: *mut c_void, trans: u32) {
    answered(htlc, trans);
}

/// A request on `trans` was refused, or its reply cut short. Whether the
/// user has heard of it already: a voice or video refusal goes to the
/// runtime, with the error sound, when the connection's session has one.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn failed(htlc: *mut c_void, trans: u32, reason: Option<&str>) -> bool {
    let Some(asked) = answered(htlc, trans) else {
        return false;
    };
    if reason == Some(CUT_SHORT) {
        return true;
    }
    let rt = hx_session_voice_runtime(hx_sess_from_htlc(htlc));
    if rt.is_null() {
        return false;
    }
    let text = reason
        .filter(|r| !r.is_empty())
        .map(|r| c_text(r.as_bytes()));
    let text = text.as_ref().map_or(std::ptr::null(), |t| t.as_ptr());
    match asked {
        Asked::Op(opcode) => gtkhx_voice_runtime_task_error(rt, opcode, text),
        Asked::VideoStart { cid, kind, gen } => {
            gtkhx_voice_runtime_video_start_failed(rt, cid, kind, gen, text)
        }
        Asked::VideoState { cid, kind, gen } => {
            gtkhx_voice_runtime_video_state_failed(rt, cid, kind, gen, text)
        }
    }
    play_sound(SOUND_ERROR);
    true
}

#[cfg(test)]
mod tests;
