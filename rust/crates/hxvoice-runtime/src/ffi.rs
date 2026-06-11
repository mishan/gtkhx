//! `#[no_mangle] extern "C"` entry points the C side calls.
//!
//! Phase 8.B exposed [`gtkhx_voice_init`] — the Rust-side wrapper
//! over `gst::init()` — and that's still the lifecycle entry point
//! the C dispatcher hits from `src/gtkhx.c::main`.
//!
//! Phase 8.D's "runtime wiring" follow-up adds the per-session
//! opaque-handle surface: a `gtkhx_voice_runtime` (opaque pointer
//! on the C side) that wraps a Rust `VoiceRuntime`, plus a set of
//! event-injection shims (`_join` / `_leave` / `_mute` /
//! `_sdp_offer` / `_ice_candidate` / `_room_status` /
//! `_task_error`) that translate from C-flavoured parameters into
//! the typed `hxvoice::Event` variants and feed them through
//! `VoiceRuntime::handle_event`.
//!
//! All entry points are documented for safety at the function
//! level. The mirror prototypes live in `src/voice_runtime.h`.

use core::slice;
use std::ffi::{c_char, CStr};

use hxvoice::event::Event;
use hxvoice::event::ServerError;

use crate::runtime::{
    CallbackBackend, MuteChangedCallback, NoopBackend, SendWireFrameCallback,
    SignalCallbacks, StateChangedCallback, VoiceRuntime,
};

/// FFI mirror of [`crate::runtime::SignalCallbacks`]. The C header
/// declares this as `gtkhx_voice_runtime_signal_callbacks`;
/// the field layout (and any future appended fields) must stay in
/// lock-step.
///
/// `#[repr(C)]` because the C caller writes the field offsets
/// directly. Layout is "pointer-sized fn-pointer slot per signal",
/// matching the header exactly.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GtkhxVoiceRuntimeSignalCallbacks {
    pub state_changed: Option<StateChangedCallback>,
    pub mute_changed: Option<MuteChangedCallback>,
}

impl From<&GtkhxVoiceRuntimeSignalCallbacks> for SignalCallbacks {
    fn from(c: &GtkhxVoiceRuntimeSignalCallbacks) -> Self {
        SignalCallbacks {
            state_changed: c.state_changed,
            mute_changed: c.mute_changed,
        }
    }
}

/// Initialise the GStreamer subsystem.
///
/// Idempotent — `gst::init` checks an internal "already initialised"
/// flag, so repeat calls are cheap no-ops. Returns `1` (true) on
/// success or `0` (false) on failure (no plugin path configured,
/// missing core plugins, broken GStreamer install). On failure the
/// detailed error goes to stderr via GStreamer's own diagnostics;
/// the C caller decides whether to disable voice UI or fail loudly.
///
/// Returning `int` rather than `bool` because the C header declares
/// it `int` for consistency with the rest of the FFI surface
/// (Hotline-proto's `gtkhx_proto_parse_*` family uses `bool` because
/// stdbool.h is already included; the voice runtime header is
/// deliberately leaner and just uses `int` for the small handful
/// of present-and-future entry points).
///
/// # Safety
/// No memory parameters. Safe to call from any thread, but the
/// expected call site is the main thread during application
/// startup.
#[no_mangle]
pub extern "C" fn gtkhx_voice_init() -> i32 {
    if crate::init() {
        1
    } else {
        0
    }
}

// ---------------------------------------------------------------------
// Per-session runtime handle.
//
// The C side holds the runtime as an opaque `gtkhx_voice_runtime *`.
// On the Rust side it's a `Box<VoiceRuntime>` that we leak across
// the FFI boundary on `new` and recover + drop on `free`.
//
// The runtime currently uses `NoopBackend` for outgoing wire frames
// — the C side already calls `hx_send_voice_join` / `_leave` /
// `_mute` directly from the UI click handlers, so wiring the state
// machine's `SendWireFrame` actions back to the wire would
// double-send. A subsequent follow-up will swap to a bridge
// Backend that:
//
//   - send_wire_frame: routes through `hlwrite_chunks` so the C
//     side stops doing direct sends and the state machine becomes
//     the single source of truth for outgoing voice opcodes.
//   - emit_signal: drives `gtkhx_session_emit_voice_*` so the UI
//     reflects state-machine state instead of optimistic UI.
//   - tear_down: closes webrtcbin + receive bins.
//
// For now the runtime is wired in receive-direction only: rcv.c
// hands it SDP offer / ICE / ROOM_STATUS / TASK errors, the state
// machine + GStreamer plumbing process them, and the resulting
// outbound 603 / 604 / 606 frames would be NoopBackend'd — which
// is fine because the C side hasn't started routing wire-frame
// emissions through here yet.
// ---------------------------------------------------------------------

/// Construct a new `VoiceRuntime` for a session. Returns an opaque
/// pointer on success, NULL on failure (typically: GStreamer
/// runtime not initialised correctly, or `webrtcbin` plugin not
/// installed). The caller frees with [`gtkhx_voice_runtime_free`].
///
/// # Safety
/// No memory parameters. Must be called from the main thread (the
/// runtime's `Inner` is `!Send` and tracks its registry entry
/// against the calling thread's TLS).
#[no_mangle]
pub extern "C" fn gtkhx_voice_runtime_new() -> *mut VoiceRuntime {
    match VoiceRuntime::new(Box::new(NoopBackend)) {
        Ok(rt) => Box::into_raw(Box::new(rt)),
        Err(e) => {
            // Route through the GStreamer log channel so the
            // failure shows up alongside the rest of the voice
            // runtime's diagnostics (and not as bare stderr
            // noise on top of any GUI/test runner output).
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: VoiceRuntime::new failed: {e}"
            );
            core::ptr::null_mut()
        }
    }
}

/// Construct a runtime with a C callback registered for
/// `Action::SendWireFrame` dispatch. Production uses this so the
/// state machine's outbound voice opcodes (especially 603
/// SDP_ANSWER and 604 ICE, which originate from webrtcbin events
/// and have no other path to the wire) reach the C side's
/// `hx_send_voice_*` helpers.
///
/// `send_wire_frame_cb` may be NULL — the bridge then behaves like
/// `NoopBackend` for that surface. `user_data` is opaque to the
/// runtime; in production it's the `htlc_conn *` for the session,
/// which the callback unpacks to drive `hlwrite_chunks`.
///
/// See the [`crate::runtime::SendWireFrameCallback`] type alias
/// for the body-layout contract.
///
/// # Safety
/// `user_data` + `send_wire_frame_cb` must remain valid for the
/// lifetime of the returned runtime — typically: tied to the
/// `htlc_conn`'s lifetime, freed in lockstep via
/// `gtkhx_voice_runtime_free` from the same disconnect path.
/// Must be called from the main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_new_with_callbacks(
    user_data: *mut core::ffi::c_void,
    send_wire_frame_cb: Option<SendWireFrameCallback>,
) -> *mut VoiceRuntime {
    let backend =
        Box::new(CallbackBackend::new(user_data, send_wire_frame_cb));
    match VoiceRuntime::new(backend) {
        Ok(rt) => Box::into_raw(Box::new(rt)),
        Err(e) => {
            eprintln!("hxvoice: VoiceRuntime::new failed: {e}");
            core::ptr::null_mut()
        }
    }
}

/// Construct a runtime with both wire-frame and state-machine
/// signal callbacks registered. Production uses this so the C
/// side's `voice_panel.c` reflects authoritative state-machine
/// state (joined / muted) instead of running optimistic UI.
///
/// `send_wire_frame_cb` and every individual `signals.*` field
/// follow the same NULL semantics as
/// [`gtkhx_voice_runtime_new_with_callbacks`] — NULL means "no
/// subscriber for this surface". A NULL `signals` pointer is
/// equivalent to passing a struct with all-NULL fields.
///
/// # Safety
/// `user_data` and every non-NULL callback must remain valid for
/// the lifetime of the returned runtime. The `signals` pointer
/// itself is read once at construction; the caller may free or
/// reuse the struct as soon as this function returns.
/// Must be called from the main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_new_v2(
    user_data: *mut core::ffi::c_void,
    send_wire_frame_cb: Option<SendWireFrameCallback>,
    signals: *const GtkhxVoiceRuntimeSignalCallbacks,
) -> *mut VoiceRuntime {
    let signal_callbacks = if signals.is_null() {
        SignalCallbacks::none()
    } else {
        // SAFETY: caller contract — non-NULL `signals` must
        // point at a valid GtkhxVoiceRuntimeSignalCallbacks for
        // the duration of this call. We copy out of it before
        // returning, so its lifetime ends here.
        SignalCallbacks::from(unsafe { &*signals })
    };
    let backend = Box::new(CallbackBackend::new_with_signals(
        user_data,
        send_wire_frame_cb,
        signal_callbacks,
    ));
    match VoiceRuntime::new(backend) {
        Ok(rt) => Box::into_raw(Box::new(rt)),
        Err(e) => {
            eprintln!("hxvoice: VoiceRuntime::new failed: {e}");
            core::ptr::null_mut()
        }
    }
}

/// Read the runtime's currently-active cid. Returns `1` and
/// writes the cid through `out_cid` when the state machine has an
/// active room (any state except `Idle` / `Leaving`); returns `0`
/// and leaves `out_cid` untouched otherwise. NULL-safe on both
/// `rt` and `out_cid` (returns `0`).
///
/// Production uses this from the signal callbacks to figure out
/// which voice panel to update — the StateChanged / MuteChanged
/// payloads don't carry cid, but the C side may have multiple
/// chat panels each tracking a different cid.
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer. `out_cid`, if
/// non-NULL, must be writable for `sizeof(uint32_t)`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_active_cid(
    rt: *mut VoiceRuntime,
    out_cid: *mut u32,
) -> i32 {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return 0;
    };
    if out_cid.is_null() {
        return 0;
    }
    match rt.active_cid() {
        Some(cid) => {
            // SAFETY: caller guaranteed out_cid is writable.
            unsafe { *out_cid = cid };
            1
        }
        None => 0,
    }
}

/// Free a runtime created with [`gtkhx_voice_runtime_new`].
///
/// # Safety
/// `rt` must be a pointer returned by `gtkhx_voice_runtime_new`
/// and not previously freed. Safe to call with NULL (no-op). The
/// caller must not use the pointer after this call returns.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_free(rt: *mut VoiceRuntime) {
    if rt.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(rt) });
}

/// Helper: borrow a runtime pointer as a `&VoiceRuntime`. Returns
/// `None` on NULL. Callers should treat `None` as a soft failure
/// (event dropped, no panic).
///
/// # Safety
/// `rt` must be a pointer returned by `gtkhx_voice_runtime_new` or
/// NULL. Concurrent calls against the same runtime are not safe —
/// the dispatch loop is main-thread-only.
unsafe fn rt_from_ptr<'a>(rt: *mut VoiceRuntime) -> Option<&'a VoiceRuntime> {
    if rt.is_null() {
        return None;
    }
    Some(unsafe { &*rt })
}

// ---- Event-injection shims ------------------------------------------
//
// Each shim translates from C-flavoured parameters into the typed
// `hxvoice::Event` and feeds it through `VoiceRuntime::handle_event`.
// All are NULL-safe on the runtime pointer (drop the event silently
// — the C side may have torn the runtime down on disconnect just
// before a late wire frame arrived). String parameters are owned by
// the caller for the duration of the call; we copy into a Rust
// `String` before handing to the state machine.

/// Fire `Event::JoinRequested { cid }`. Called from the Join Voice
/// click handler.
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer. No memory params
/// beyond the pointer.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_join(
    rt: *mut VoiceRuntime,
    cid: u32,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    rt.handle_event(Event::JoinRequested { cid });
}

/// Fire `Event::LeaveRequested { cid }`. Called from the Leave
/// Voice click handler.
///
/// # Safety
/// Same as `gtkhx_voice_runtime_join`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_leave(
    rt: *mut VoiceRuntime,
    cid: u32,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    rt.handle_event(Event::LeaveRequested { cid });
}

/// Fire `Event::MuteToggleRequested { muted }`. Called from the
/// Mute toggle handler. The state machine ignores redundant
/// toggles (e.g. already-muted + muted=1), so the C side doesn't
/// need to dedupe.
///
/// # Safety
/// Same as `gtkhx_voice_runtime_join`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_mute(
    rt: *mut VoiceRuntime,
    muted: i32,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    rt.handle_event(Event::MuteToggleRequested {
        muted: muted != 0,
    });
}

/// Fire `Event::SdpOfferReceived { cid, sdp }`. Called from
/// `rcv_task_voice_join` / `hx_rcv_voice_sdp_offer` after the
/// server's 602 reply lands and the SDP has been extracted.
///
/// `sdp` is a C string (NUL-terminated). NULL is treated as empty
/// (and dropped by the state machine via the wrong-shape guard
/// implicit in webrtcbin's parser).
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer. `sdp` must be NULL
/// or a NUL-terminated C string valid for reads up to and including
/// the NUL terminator. The Rust copy is taken before this function
/// returns; the caller's buffer can be freed immediately after.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_sdp_offer(
    rt: *mut VoiceRuntime,
    cid: u32,
    sdp: *const c_char,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    let sdp = unsafe { cstr_to_string(sdp) };
    rt.handle_event(Event::SdpOfferReceived { cid, sdp });
}

/// Fire `Event::IceCandidateReceived { cid, candidate_json }`.
/// Called from `hx_rcv_voice_ice` after the JSON has been extracted
/// from the DATA_VOICE_ICE chunk.
///
/// # Safety
/// Same shape as `gtkhx_voice_runtime_sdp_offer`. The empty-string
/// end-of-candidates marker is handled inside the state machine.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_ice_candidate(
    rt: *mut VoiceRuntime,
    cid: u32,
    candidate_json: *const c_char,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    let candidate_json = unsafe { cstr_to_string(candidate_json) };
    rt.handle_event(Event::IceCandidateReceived {
        cid,
        candidate_json,
    });
}

/// Fire `Event::ParticipantsUpdated { cid, entries }`. Called from
/// `hx_rcv_voice_room_status` after the binary DATA_VOICE_PARTICIPANTS
/// blob has been parsed.
///
/// `blob` + `len` describe the 6-byte-per-entry packed binary the
/// `hotline_proto::voice::parse_voice_participants` iterator
/// consumes. We re-parse here on the Rust side rather than asking
/// the C side to construct `hxvoice::Participant` (which lives
/// inside hxvoice's `no_std`-friendly typed surface and isn't
/// directly C-shaped).
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer. `blob` must be
/// valid for reads of `len` bytes, or NULL with `len == 0`. The
/// Rust copy is taken before return.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_room_status(
    rt: *mut VoiceRuntime,
    cid: u32,
    blob: *const u8,
    len: usize,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    // FFI safety: `slice::from_raw_parts` requires the total
    // byte length to be ≤ isize::MAX (so the size in bytes fits
    // in a `ssize_t`-shaped integer). A C caller passing an
    // out-of-range `size_t` would otherwise trigger UB. The
    // hotline-proto FFI shims established the convention of
    // treating len > isize::MAX as an empty slice; mirror that
    // here. Combined with the NULL / zero-length guard, the
    // result is "treat malformed input as a zero-participant
    // update" — the state machine's wrong-cid + empty-blob
    // paths handle that cleanly.
    let bytes: &[u8] =
        if blob.is_null() || len == 0 || len > isize::MAX as usize {
            &[]
        } else {
            unsafe { slice::from_raw_parts(blob, len) }
        };
    // Cap participants to a sane ceiling. The on-wire u16 length
    // would let a server (or a corrupt frame) ship up to ~10k
    // entries — collecting that uncapped into a Vec + handing it
    // to the state machine's HashMap is a DoS vector against an
    // untrusted network input. Real voice rooms are
    // small-group calls; 256 simultaneous participants is well
    // past the spec's design point. Log when we truncate so an
    // operator can see it.
    const MAX_PARTICIPANTS: usize = 256;
    let mut entries: Vec<hxvoice::event::Participant> =
        hotline_proto::voice::parse_voice_participants(bytes)
            .take(MAX_PARTICIPANTS + 1)
            .map(|p| hxvoice::event::Participant {
                user_id: p.user_id,
                codec_id: p.codec_id,
                muted: p.is_muted(),
            })
            .collect();
    if entries.len() > MAX_PARTICIPANTS {
        gstreamer::warning!(
            gstreamer::CAT_RUST,
            "hxvoice: room_status blob has >{MAX_PARTICIPANTS} participants \
             (cid={cid}, blob len={}); truncating",
            bytes.len()
        );
        entries.truncate(MAX_PARTICIPANTS);
    }
    rt.handle_event(Event::ParticipantsUpdated { cid, entries });
}

/// Fire `Event::ServerTaskError { origin_opcode, text }`. Called
/// from the `HTLS_HDR_TASK` error dispatch when the task's
/// originating opcode was one of the voice opcodes that registers
/// a TASK: 600 (JOIN), 601 (LEAVE), 603 (SDP_ANSWER), 606 (MUTE).
/// 604 (ICE) is a bidirectional notification with no task reply,
/// so it never reaches this entry point. The state machine decides
/// whether to tear down (JOIN / SDP_ANSWER errors → fail) or
/// surface as a toast only (LEAVE / MUTE errors).
///
/// # Safety
/// `text` shape matches `gtkhx_voice_runtime_sdp_offer`. NULL is
/// treated as an empty error message.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_task_error(
    rt: *mut VoiceRuntime,
    origin_opcode: u32,
    text: *const c_char,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    let text = unsafe { cstr_to_string(text) };
    rt.handle_event(Event::ServerTaskError(ServerError {
        origin_opcode,
        text,
    }));
}

/// Copy a C string into a Rust `String`. NULL → empty. Invalid
/// UTF-8 → replaced via `String::from_utf8_lossy`.
///
/// # Safety
/// `s` must be NULL or a NUL-terminated C string valid for reads
/// up to and including the NUL terminator.
unsafe fn cstr_to_string(s: *const c_char) -> String {
    if s.is_null() {
        return String::new();
    }
    let cstr = unsafe { CStr::from_ptr(s) };
    String::from_utf8_lossy(cstr.to_bytes()).into_owned()
}
