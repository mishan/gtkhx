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
    CallbackBackend, ErrorCallback, MuteChangedCallback, NoopBackend,
    SendWireFrameCallback, SignalCallbacks, SpeakerChangedCallback,
    StateChangedCallback, VoiceRuntime,
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
    pub speaker_changed: Option<SpeakerChangedCallback>,
    pub error: Option<ErrorCallback>,
}

impl From<&GtkhxVoiceRuntimeSignalCallbacks> for SignalCallbacks {
    fn from(c: &GtkhxVoiceRuntimeSignalCallbacks) -> Self {
        SignalCallbacks {
            state_changed: c.state_changed,
            mute_changed: c.mute_changed,
            speaker_changed: c.speaker_changed,
            error: c.error,
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
// Audio-device enumeration + preference setters.
//
// The Settings → Voice page populates input + output device combos
// from `gtkhx_voice_list_input_devices` / `_list_output_devices`,
// stores the user's pick as `gst::Device::name()` in `gtkhxrc`, and
// hands it back via `gtkhx_voice_set_input_device` /
// `_set_output_device`. `VoiceRuntime::new` reads those preferences
// at construction time and threads them through to
// `audio::make_send_bin` / `make_receive_bin`. NULL or "" means
// "use system default" (autoaudiosrc / autoaudiosink).
//
// Device lists are returned as opaque handles with `_len` / `_name`
// / `_display_name` accessors so we don't have to define a stable
// C struct layout for the per-device record. The handle wraps a
// `Vec<AudioDevice>`; the strings inside are kept alive by the
// handle.
// ---------------------------------------------------------------------

/// Opaque list handle returned by `gtkhx_voice_list_*_devices`.
///
/// C-side type is `gtkhx_voice_device_list`. Construct via the
/// listing functions; access entries through `_len` / `_name` /
/// `_display_name`; free with `gtkhx_voice_device_list_free`.
pub struct GtkhxVoiceDeviceList {
    devices: Vec<crate::audio::AudioDevice>,
    /// Per-entry C-string cache. `name_c[i]` and `display_c[i]`
    /// hold owned `CString`s whose `.as_ptr()` the C side
    /// dereferences. Built lazily on first `_name` /
    /// `_display_name` call for that index.
    name_c: Vec<Option<std::ffi::CString>>,
    display_c: Vec<Option<std::ffi::CString>>,
}

fn build_device_list(devices: Vec<crate::audio::AudioDevice>) -> Box<GtkhxVoiceDeviceList> {
    let len = devices.len();
    Box::new(GtkhxVoiceDeviceList {
        devices,
        name_c: (0..len).map(|_| None).collect(),
        display_c: (0..len).map(|_| None).collect(),
    })
}

/// Enumerate available capture devices.
///
/// Returns a heap-allocated, owning handle. The caller must free
/// it with `gtkhx_voice_device_list_free` when done. Never returns
/// NULL — a failed DeviceMonitor scan (typically: GStreamer not
/// initialised; call `gtkhx_voice_init` first) yields a valid
/// list whose `_len` is 0. C callers can branch on length without
/// having to NULL-check.
///
/// # Safety
/// No memory parameters. Caller takes ownership of the returned
/// pointer.
#[no_mangle]
pub extern "C" fn gtkhx_voice_list_input_devices(
) -> *mut GtkhxVoiceDeviceList {
    let devices = crate::audio::list_input_devices();
    Box::into_raw(build_device_list(devices))
}

/// Enumerate available playback devices. Same shape as
/// `gtkhx_voice_list_input_devices`.
#[no_mangle]
pub extern "C" fn gtkhx_voice_list_output_devices(
) -> *mut GtkhxVoiceDeviceList {
    let devices = crate::audio::list_output_devices();
    Box::into_raw(build_device_list(devices))
}

/// Number of entries in a device list. Returns 0 if `list` is NULL.
///
/// # Safety
/// `list` must be a pointer returned by `gtkhx_voice_list_*_devices`
/// and not yet freed.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_device_list_len(
    list: *const GtkhxVoiceDeviceList,
) -> usize {
    if list.is_null() {
        return 0;
    }
    (*list).devices.len()
}

/// Stable `gst::Device::name()` for the entry at `idx`, NUL-terminated.
/// Returns NULL on out-of-range index or NULL list. The returned
/// pointer is valid for the lifetime of the list.
///
/// # Safety
/// Same constraints as `gtkhx_voice_device_list_len`. The returned
/// `*const c_char` must NOT be freed by the caller — it lives as
/// long as the list does.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_device_list_name(
    list: *mut GtkhxVoiceDeviceList,
    idx: usize,
) -> *const c_char {
    if list.is_null() {
        return std::ptr::null();
    }
    let list = &mut *list;
    if idx >= list.devices.len() {
        return std::ptr::null();
    }
    if list.name_c[idx].is_none() {
        list.name_c[idx] = std::ffi::CString::new(
            list.devices[idx].name.as_str(),
        )
        .ok();
    }
    list.name_c[idx]
        .as_ref()
        .map(|c| c.as_ptr())
        .unwrap_or(std::ptr::null())
}

/// User-facing display name for the entry at `idx`, NUL-terminated.
/// Same lifetime + safety contract as `gtkhx_voice_device_list_name`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_device_list_display_name(
    list: *mut GtkhxVoiceDeviceList,
    idx: usize,
) -> *const c_char {
    if list.is_null() {
        return std::ptr::null();
    }
    let list = &mut *list;
    if idx >= list.devices.len() {
        return std::ptr::null();
    }
    if list.display_c[idx].is_none() {
        list.display_c[idx] = std::ffi::CString::new(
            list.devices[idx].display_name.as_str(),
        )
        .ok();
    }
    list.display_c[idx]
        .as_ref()
        .map(|c| c.as_ptr())
        .unwrap_or(std::ptr::null())
}

/// Free a device list. No-op on NULL.
///
/// # Safety
/// `list` must have been returned by `gtkhx_voice_list_*_devices`
/// and not yet freed.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_device_list_free(
    list: *mut GtkhxVoiceDeviceList,
) {
    if !list.is_null() {
        drop(Box::from_raw(list));
    }
}

/// Set the preferred capture device by `gst::Device::name()`.
///
/// `name` may be NULL or "" to clear the preference (system
/// default, via `autoaudiosrc`). Any non-empty value is stored
/// verbatim and looked up via DeviceMonitor at the next
/// `VoiceRuntime::new` call. If the named device isn't present at
/// runtime construction time, the send chain falls back to
/// autoaudiosrc — the runtime never panics over a missing device.
///
/// # Safety
/// `name` must either be NULL or a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_set_input_device(name: *const c_char) {
    let s = if name.is_null() {
        None
    } else {
        CStr::from_ptr(name).to_str().ok()
    };
    crate::audio::set_input_device(s);
}

/// Set the preferred playback device. Same shape and semantics as
/// `gtkhx_voice_set_input_device` but for the output (sink) side.
///
/// # Safety
/// Same as `gtkhx_voice_set_input_device`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_set_output_device(name: *const c_char) {
    let s = if name.is_null() {
        None
    } else {
        CStr::from_ptr(name).to_str().ok()
    };
    crate::audio::set_output_device(s);
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
            // Same rationale as gtkhx_voice_runtime_new — route
            // through the GStreamer log channel rather than bare
            // stderr so the failure shows up alongside the rest
            // of the voice runtime's diagnostics.
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: VoiceRuntime::new failed: {e}"
            );
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
            // Same rationale as gtkhx_voice_runtime_new — route
            // through the GStreamer log channel rather than bare
            // stderr so the failure shows up alongside the rest
            // of the voice runtime's diagnostics.
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: VoiceRuntime::new failed: {e}"
            );
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

/// Total RTP buffers received across all receive legs since the
/// current pipeline was built. Monotonic within a session; resets to
/// 0 when the runtime rebuilds its pipeline on `Action::TearDown`.
///
/// This is a media-liveness signal for the Tier 3 voice harness: a
/// client that is receiving a remote participant's audio sees this
/// counter advance (~50/s per active sender at PCMU's 20 ms ptime).
/// It needs no audio device — it counts buffers off the receive
/// bin's depay sink, so it advances even for digital-silence PCMU.
/// NULL-safe (returns 0).
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_rtp_buffers_received(
    rt: *mut VoiceRuntime,
) -> u64 {
    match unsafe { rt_from_ptr(rt) } {
        Some(rt) => rt.rtp_buffers_received_for_test(),
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

/// Record the local user's Hotline uid so the send leg's `level` VAD
/// can light the LOCAL user's own speaker indicator when they talk.
/// Call once when joining voice (the uid is stable for the session).
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_set_self_uid(
    rt: *mut VoiceRuntime,
    uid: u16,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    rt.set_self_uid(uid);
}

/// Set the per-listener playback gain for a remote participant —
/// the value behind the user-list right-click volume slider.
///
/// `gain` is a linear multiplier: `0.0` mutes them locally, `1.0` is
/// unity, values above `1.0` boost. It's clamped to `[0.0, 10.0]` and
/// any non-finite value is treated as unity, so a stray FFI value
/// can't feed the GStreamer `volume` element garbage. The gain is
/// session-scoped, keyed by uid, and re-applied when the participant's
/// receive bin is rebuilt on a mid-call rejoin.
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer. No memory params
/// beyond the pointer. Must be called from the main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_set_user_volume(
    rt: *mut VoiceRuntime,
    uid: u16,
    gain: f64,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    rt.set_user_volume(uid, gain);
}

/// Hot-swap the capture (microphone) device on a live runtime. The C
/// side calls this from the Settings input-device change handler AFTER
/// updating the global device preference via
/// [`gtkhx_voice_set_input_device`] — this reads that preference and
/// rebuilds the send bin against it, reusing the existing webrtcbin
/// transceiver (no SDP renegotiation). No-op on a NULL runtime or one
/// without a live pipeline.
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer. Main-thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_reload_input_device(
    rt: *mut VoiceRuntime,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    rt.reload_input_device();
}

/// Hot-swap the playback (speaker) device on a live runtime. Same
/// contract as [`gtkhx_voice_runtime_reload_input_device`] but rebuilds
/// every live receive bin against the current output-device preference.
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer. Main-thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_reload_output_device(
    rt: *mut VoiceRuntime,
) {
    let Some(rt) = (unsafe { rt_from_ptr(rt) }) else {
        return;
    };
    rt.reload_output_device();
}

/// Read the stored per-listener gain for a uid, or `1.0` (unity) when
/// the user never adjusted that uid's slider. Used by the C side to
/// initialise the slider position when it opens the popover.
///
/// # Safety
/// `rt` must be NULL or a valid runtime pointer. Returns `1.0` for a
/// NULL runtime.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_runtime_user_volume(
    rt: *mut VoiceRuntime,
    uid: u16,
) -> f64 {
    match unsafe { rt_from_ptr(rt) } {
        Some(rt) => rt.user_volume(uid),
        None => 1.0,
    }
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
