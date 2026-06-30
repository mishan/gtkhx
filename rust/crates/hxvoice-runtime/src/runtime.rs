//! GStreamer-backed runtime that drives an `hxvoice::SessionMachine`.
//!
//! Built across Phase 8.C steps 2 and 3 of `docs/voice-chat-plan.md`
//! §5.C. The pure state machine lives in `hxvoice`; this module
//! wires it to a `Backend` trait that production fills with FFI
//! calls to the C side, and to a `gst::Pipeline` containing a
//! `webrtcbin` element driven by typed `gstreamer-rs` calls.
//!
//! ## What step 2 implemented
//!
//! - The `Backend` trait — `send_wire_frame` / `emit_signal` /
//!   `tear_down` callbacks the C side hooks up later.
//! - `VoiceRuntime::new()` — constructs the owning
//!   `gst::Pipeline` container.
//! - `handle_event(event)` — the dispatch loop. Pumps the event
//!   through the state machine and walks the returned action
//!   list, dispatching backend-shaped actions
//!   (`SendWireFrame` / `EmitSignal` / `TearDown`) to the
//!   `Backend` and timer-shaped actions (`ArmTimer` /
//!   `CancelTimer`) to the local `armed_timers` set.
//! - Deferred-dispatch queue. Backends that re-enter
//!   `handle_event` from inside a callback (the production shape
//!   when an `EmitSignal` GLib signal handler turns into another
//!   voice transition) enqueue and return; the outer loop
//!   drains.
//!
//! ## What step 3 implements
//!
//! - `webrtcbin` construction in `VoiceRuntime::new()` —
//!   `gst::ElementFactory::make("webrtcbin")` added to the
//!   owning pipeline and stashed in `Inner`.
//! - Dispatch arms for the SDP-shaped actions:
//!   - `Action::SetRemoteDescription` — parses the SDP via
//!     `gstreamer_sdp::SDPMessage`, builds a
//!     `WebRTCSessionDescription` of type `Offer`, emits
//!     `webrtcbin.set-remote-description`.
//!   - `Action::CreateAnswer` — emits
//!     `webrtcbin.create-answer` with a `gst::Promise`. When the
//!     promise fires (typically on a GStreamer worker thread) the
//!     change-func marshals back to the main GLib context via
//!     `glib::MainContext::invoke`, looks the runtime up by
//!     `runtime_id` from the thread-local registry, and re-enters
//!     `handle_event(Event::WebrtcAnswerCreated)`.
//!   - `Action::SetLocalDescription` — same shape as
//!     `SetRemoteDescription` but type `Answer`.
//!
//! ## What step 4 implements
//!
//! - Dispatch arm for `Action::AddRemoteIce` — parses the
//!   `RTCIceCandidateInit` JSON via the shared
//!   `hotline_proto::voice::ice::parse` helper, emits
//!   `webrtcbin.add-ice-candidate(mline_index, candidate)`.
//!   Defensive drops on malformed JSON, missing
//!   `sdpMLineIndex`, and the end-of-candidates marker (empty
//!   `candidate` string).
//! - `webrtcbin.on-ice-candidate` signal wired in
//!   `VoiceRuntime::new` via `connect_on_ice_candidate`. The
//!   callback is `Send + 'static` (webrtcbin may fire it from a
//!   worker thread) — captures only `runtime_id` + the main
//!   context handle, builds the outgoing JSON (resolving
//!   `sdpMid` via the bin's `local-description`), marshals
//!   back through `MainContext::invoke`, and re-enters
//!   `handle_event(Event::WebrtcLocalIceGathered)`. Same
//!   thread-bridge shape as the SDP promise.
//!
//! ## What step 5 implements
//!
//! - Dispatch arms for `Action::StartReceivePipeline` and
//!   `Action::StopReceivePipeline`. StartReceivePipeline pops
//!   the matching pad out of `Inner::pending_pads`, builds the
//!   `rtppcmudepay ! mulawdec ! audioconvert ! audioresample !
//!   autoaudiosink` chain via `audio::make_receive_bin`, adds
//!   it to the pipeline, links the pad to its ghost sink, and
//!   stashes the bin in `Inner::receive_bins` keyed by `mid`.
//!   StopReceivePipeline pops the bin out, sets it to `Null`,
//!   and removes it from the pipeline.
//! - `webrtcbin.pad-added` signal wired in `VoiceRuntime::new`
//!   via `connect_pad_added`. Same thread-bridge shape as
//!   `connect_on_ice_candidate`: filters Src direction pads,
//!   resolves `mid` via the pad's transceiver, marshals to the
//!   main thread, parks the pad in `Inner::pending_pads`, and
//!   re-enters `handle_event(Event::WebrtcPadAdded { mid })`.
//!
//! ## What step 7 implements
//!
//! - Real timer wiring. The state machine has emitted
//!   `Action::ArmTimer { kind, ms }` and `Action::CancelTimer
//!   { kind }` since step 1 (JoinReply / Dtls / IceConnectivity
//!   / Media watchdogs per the spec's §"Session Timeout and
//!   Failure"); the runtime had been bookkeeping them in a
//!   `Vec<Timeout>` without actually firing anything. Step 7
//!   replaces that with a `HashMap<Timeout, Option<glib::SourceId>>`
//!   driven by `glib::timeout_add_local`. ArmTimer cancels any
//!   existing source for the kind (re-arm restarts the
//!   watchdog), then attaches a fresh one-shot timeout whose
//!   callback fires `Event::Timeout { kind }` against the
//!   state machine via the same `with_main_thread_runtime`
//!   registry hop the SDP / ICE / pad callbacks use.
//!   CancelTimer pops the source and removes it from the loop;
//!   Drop on `Inner` removes any still-armed source so glib
//!   doesn't fire a callback against a dropped runtime.
//!   `Option<SourceId>` because the test runner can't always
//!   own the default `MainContext` — see the field doc on
//!   `Inner::armed_timer_sources` for the contention story.
//!
//! ## What step 8 implements
//!
//! - `webrtcbin.notify::connection-state` wired via
//!   `connect_connection_state_notify`. When peer-connection-state
//!   transitions (Connecting → Connected → Failed, etc.), the
//!   callback maps the GStreamer
//!   `WebRTCPeerConnectionState` to the hxvoice `ConnectionState`
//!   via `map_peer_connection_state`, marshals back to the main
//!   thread via `MainContext::invoke`, and re-enters
//!   `handle_event(Event::WebrtcConnectionStateChanged)`. The
//!   state machine then drives Connecting → Connected,
//!   fail() on Failed, etc.
//! - Pipeline bus watch via `attach_pipeline_bus_watch`. An
//!   `add_watch_local` source on `pipeline.bus()` catches
//!   `MessageView::Error` / `MessageView::Warning` and logs the
//!   source element + message + debug string on the GStreamer
//!   warning channel for triage. Doesn't currently translate
//!   bus errors into state-machine events — the matching
//!   connection-state transitions arrive through the
//!   notify::connection-state signal anyway; this watch is
//!   complementary triage logging.
//!
//! ## What's deferred
//!
//! - `Action::SetSendPipelineMute` and the audio-capture +
//!   RTP-send chain it acts on. The send leg lands after the
//!   receive-leg + signaling work is in shape; mute hooks into
//!   it once the send pipeline exists.
//!
//! ## Layering
//!
//! The runtime sits between three peers:
//!
//! - **`hxvoice::SessionMachine`** owned inside `Inner`. Single
//!   source of "what should we do" for every transition.
//! - **`gst::Pipeline` + `gstreamer_webrtc::WebRTCBin`** also owned
//!   inside `Inner`. The runtime dispatches the GStreamer-shaped
//!   Actions (`SetRemoteDescription`, `CreateAnswer`,
//!   `AddRemoteIce`, audio pipeline manipulation) directly here.
//! - **`Backend`** trait, supplied at construction. Captures the
//!   integration points that aren't GStreamer-shaped: wire frames
//!   the C side ships via `hx_send_voice_*`, GtkhxSession signal
//!   emits through `hxbridge`, and teardown. Production wires it
//!   to FFI shims; tests use a recording mock.
//!
//! ## Threading
//!
//! The runtime runs main-thread-only. `Inner` and `Backend` live
//! in `Rc<RefCell<…>>` — `!Send` by construction, which is the
//! point: every state-machine borrow happens on the same thread
//! that owns the GLib main loop.
//!
//! Cross-thread callbacks that GStreamer fires from worker
//! threads (currently `gst::Promise::with_change_func`, and from
//! step 4 onward `on-ice-candidate` / `pad-added` /
//! `notify::peer-connection-state` / pipeline bus messages) bridge
//! back via the [`MAIN_THREAD_RUNTIMES`] thread-local registry +
//! `glib::MainContext::invoke`. The Promise closure must be
//! `Send + 'static`, so it captures the runtime's `u64` id and a
//! `String` payload — both `Send`. On the main thread, the
//! invoked closure looks the runtime back up by id and calls
//! `handle_event` normally. Drop on `Inner` removes the entry,
//! so a closure that fires after teardown becomes a silent
//! no-op rather than a use-after-free.
//!
//! The deferred-dispatch queue from step 2 makes re-entrant
//! calls from `Backend` callbacks safe at any depth — the queue
//! is the same machinery that supports the `webrtcbin`-signal
//! re-entry step 3 introduces.

use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::rc::{Rc, Weak};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use gstreamer::prelude::*;
use gstreamer_webrtc::WebRTCSDPType;

use hxvoice::action::{Action, SignalKind, SignalPayload};
use hxvoice::event::{ConnectionState, Event, Timeout};
use hxvoice::state::{SessionMachine, SessionState};

/// Monotonically increasing id for each `VoiceRuntime` ever built
/// on this process. Used as the key into [`MAIN_THREAD_RUNTIMES`] so
/// `Send`-required closures can refer to a runtime via a `Copy` id
/// rather than carrying a non-`Send` `Rc` clone.
///
/// `Relaxed` ordering is fine: the value is only ever consumed by
/// equality lookup in the registry; we never reason about ordering
/// between ids.
static NEXT_RUNTIME_ID: AtomicU64 = AtomicU64::new(1);

thread_local! {
    /// Per-main-thread registry of live runtimes, keyed by
    /// `runtime_id`. Entries are `Weak` so the registry never
    /// keeps a runtime alive past its last strong reference;
    /// `Drop for Inner` evicts the matching entry when the
    /// state-machine container drops.
    ///
    /// The thread-local form is load-bearing: each `Rc` lives on
    /// exactly one thread (we don't violate `!Send`), and the
    /// `gst::Promise::with_change_func` closures that need to
    /// reach back here run on a GStreamer worker thread first,
    /// then `glib::MainContext::invoke` hops onto the GLib main
    /// thread before touching the registry. The registry on that
    /// thread is the one the runtime was registered with.
    static MAIN_THREAD_RUNTIMES: RefCell<
        HashMap<u64, WeakRuntime>,
    > = RefCell::new(HashMap::new());
}

/// `Weak`-shaped clone of [`VoiceRuntime`], for storage in the
/// thread-local registry without circular ref-count keep-alive.
///
/// `WeakRuntime` itself is `Clone` but not `Send` — same constraint
/// as `Weak<Rc<…>>`. It only exists on the main thread.
#[derive(Clone)]
struct WeakRuntime {
    inner: Weak<RefCell<Inner>>,
    backend: Weak<RefCell<Box<dyn Backend>>>,
}

impl WeakRuntime {
    fn upgrade(&self) -> Option<VoiceRuntime> {
        Some(VoiceRuntime {
            inner: self.inner.upgrade()?,
            backend: self.backend.upgrade()?,
        })
    }
}

/// Look up a runtime by id on the current (main) thread and pass
/// it to the supplied closure. The closure runs only if a live
/// runtime is found — late callbacks (after teardown) become a
/// silent no-op.
///
/// Uses `try_with` rather than `with` so a `MainContext::invoke`
/// callback firing during thread-local destructor teardown (the
/// main thread is shutting down) becomes a clean no-op instead
/// of panicking on `AccessError`. The intended semantics for a
/// late callback are "do nothing"; the registry just happens to
/// be one of the things that disappears in the same teardown
/// sequence.
///
/// Public to the crate so the planned ICE / pad / bus dispatch
/// in step 4+ can share the same registry hop.
pub(crate) fn with_main_thread_runtime<F>(id: u64, f: F)
where
    F: FnOnce(&VoiceRuntime),
{
    let upgraded = MAIN_THREAD_RUNTIMES
        .try_with(|cell| {
            cell.borrow().get(&id).and_then(WeakRuntime::upgrade)
        })
        .ok()
        .flatten();
    if let Some(rt) = upgraded {
        f(&rt);
    }
}

/// Register a freshly-constructed runtime in the thread-local
/// registry under its `runtime_id`. Called by both constructors;
/// dropped by `Drop for Inner`.
fn register(runtime: &VoiceRuntime) {
    let id = runtime.inner.borrow().runtime_id;
    let weak = WeakRuntime {
        inner: Rc::downgrade(&runtime.inner),
        backend: Rc::downgrade(&runtime.backend),
    };
    MAIN_THREAD_RUNTIMES.with(|cell| {
        cell.borrow_mut().insert(id, weak);
    });
}

/// Trait the runtime consults for everything it can't do with
/// GStreamer alone: wire frames over the Hotline TCP control
/// channel (handled C-side by `hx_send_voice_*`), GtkhxSession
/// signal emits (handled C-side by `gtkhx_bridge_emit_*`), and
/// the final teardown notification.
///
/// Implementations need not be `Send` — the runtime calls every
/// method on the main thread.
///
/// The default implementation [`NoopBackend`] makes the runtime
/// fully constructible in environments where C-side callbacks
/// would crash (e.g. early init order errors, headless tests not
/// caring about the C integration).
pub trait Backend {
    /// Emit a Hotline transaction. `opcode` is one of the
    /// `HTLC_HDR_VOICE_*` numeric values; `body` is the chunk
    /// payload the state machine pre-built. C-side dispatch
    /// translates this into a call to `hlwrite_chunks` after
    /// re-packing via `hotline_proto::voice::build_voice_*_chunks`.
    fn send_wire_frame(&mut self, opcode: u32, body: &[u8]);

    /// Emit a GtkhxSession signal so the UI updates. `kind` and
    /// `payload` map to the concrete signal name + boxed payload
    /// the GLib side expects.
    fn emit_signal(&mut self, kind: SignalKind, payload: SignalPayload);

    /// Teardown notification — the state machine has emitted
    /// [`hxvoice::action::Action::TearDown`] for whatever reason
    /// (terminal failure, explicit Leave, mid-session room
    /// switch). The implementor closes any resources it
    /// allocated for the current voice session.
    ///
    /// **`tear_down` does NOT mean "the runtime returns to Idle."**
    /// It describes runtime-resource teardown only; whether the
    /// runtime can be reused depends on the post-step
    /// `SessionMachine` state:
    ///
    /// - When the machine has transitioned to
    ///   [`hxvoice::state::SessionState::Leaving`] (terminal),
    ///   the runtime stays in `Leaving` — the state machine
    ///   treats `Leaving` as terminal and there is no `Leaving →
    ///   Idle` step. Production drops the whole `VoiceRuntime`
    ///   after observing this state and constructs a fresh one
    ///   if voice is rejoined later.
    /// - When `TearDown` accompanied a mid-session room switch
    ///   (the implicit-leave path: `JoinRequested` for a
    ///   different cid while in voice), the machine has walked
    ///   back to `JoinSent` with fresh per-room state in the
    ///   same `step()`. The runtime is expected to rebuild its
    ///   GStreamer-side resources for the new room and keep
    ///   running.
    ///
    /// Implementations that need to know which path triggered
    /// teardown can read the post-step state via the runtime's
    /// own accessor (`VoiceRuntime::state`) after the action
    /// list finishes dispatching.
    fn tear_down(&mut self);
}

/// A backend that discards every call. Useful for headless tests
/// and the FFI-not-wired-yet code path.
#[derive(Default)]
pub struct NoopBackend;

impl Backend for NoopBackend {
    fn send_wire_frame(&mut self, _opcode: u32, _body: &[u8]) {}
    fn emit_signal(&mut self, _kind: SignalKind, _payload: SignalPayload) {}
    fn tear_down(&mut self) {}
}

/// C-callback function pointer type for `Action::SendWireFrame`
/// dispatch. The callback receives the opaque `user_data` it was
/// registered with (production: the `htlc_conn *` for the session),
/// the opcode (one of `HTLC_HDR_VOICE_*`), and a borrowed byte
/// slice describing the action body.
///
/// Body layout matches the encoders in `hxvoice::state::encode_*`:
///
/// - 600 (JOIN), 601 (LEAVE): 4-byte big-endian cid
/// - 603 (SDP_ANSWER): 4-byte BE cid + SDP bytes (not NUL-terminated)
/// - 604 (ICE): 4-byte BE cid + JSON bytes (not NUL-terminated)
/// - 606 (MUTE): 4-byte BE cid + 2-byte BE muted-flag (0 / 1)
///
/// The callback may read the body slice for the duration of the
/// call; the runtime drops the underlying `Vec<u8>` when the
/// dispatch arm returns. Implementations should copy out anything
/// they need to retain past the call.
///
/// **Re-entrancy contract.** The runtime invokes
/// `send_wire_frame` while the backend's `RefCell` is mutably
/// borrowed. A callback that synchronously re-enters
/// `VoiceRuntime::handle_event` (or any other path that lands in
/// `dispatch_inner` and reaches for `self.backend.borrow_mut()`)
/// will panic on the nested borrow_mut. If your callback needs to
/// drive new events into the runtime — including the obvious one
/// of "the wire send failed, so cancel the session" — defer that
/// via `glib::idle_add_local` (or equivalent main-context post)
/// so the outer dispatch loop unwinds first.
pub type SendWireFrameCallback = unsafe extern "C" fn(
    user_data: *mut core::ffi::c_void,
    opcode: u32,
    body: *const u8,
    body_len: usize,
);

/// C-callback for `SignalKind::StateChanged`. The `state` value is
/// the FFI mirror of [`hxvoice::state::SessionState`]; integer
/// values match the C header's `gtkhx_voice_state` enum
/// (`Idle=0` through `Leaving=5`).
pub type StateChangedCallback =
    unsafe extern "C" fn(user_data: *mut core::ffi::c_void, state: u32);

/// C-callback for `SignalKind::MuteChanged`. `muted` is 0 or 1.
pub type MuteChangedCallback =
    unsafe extern "C" fn(user_data: *mut core::ffi::c_void, muted: i32);

/// C-callback for `SignalKind::SpeakerChanged`. `uid` is the
/// Hotline user id whose speaking state just flipped; `is_speaking`
/// is 0 or 1. The runtime invokes this from the periodic
/// voice-activity evaluator (default cadence 200 ms) on the GLib
/// main thread, so the C side does NOT need its own marshalling.
pub type SpeakerChangedCallback = unsafe extern "C" fn(
    user_data: *mut core::ffi::c_void,
    uid: u16,
    is_speaking: i32,
);

/// C-callback for `SignalKind::Error`. `text` is a NUL-terminated
/// UTF-8 string with a user-facing message — typically the body
/// of an AdwToast. The pointer is valid for the duration of the
/// call only (the runtime drops the owning `CString` after the
/// callback returns). Implementations must copy out anything
/// they want to retain past the call.
///
/// Sources of Error signals: spec-defined session timeouts
/// softened to non-fatal in `state.rs` (DTLS, IceConnectivity,
/// Media), and the `ServerTaskError` arm covering all client-
/// initiated voice opcodes (600 / 601 / 603 / 606). 604 is a
/// bidirectional notification with no task reply, so it never
/// surfaces as an Error here.
pub type ErrorCallback = unsafe extern "C" fn(
    user_data: *mut core::ffi::c_void,
    text: *const core::ffi::c_char,
);

/// Bundle of per-`SignalKind` C callbacks. Mirrors
/// `gtkhx_voice_runtime_signal_callbacks` in `src/voice_runtime.h`.
/// Each field is `Option` because the C caller may pass NULL for
/// signals it doesn't care about — the runtime treats `None` as
/// "no subscriber" and drops the corresponding emit silently.
///
/// **ABI note.** Adding fields here is NOT ABI-safe — older C
/// callers built against a smaller struct definition would have
/// the runtime read past the end of their allocation. Whenever a
/// new SignalKind callback slot lands, every consumer must be
/// rebuilt against the new layout. The Meson build that ships
/// this crate as a staticlib is the only consumer in practice,
/// so the rebuild is automatic — but don't be tempted to read
/// this as "older callers silently skip new signals". They
/// don't; they undefined-behave.
#[derive(Clone, Copy)]
pub struct SignalCallbacks {
    pub state_changed: Option<StateChangedCallback>,
    pub mute_changed: Option<MuteChangedCallback>,
    pub speaker_changed: Option<SpeakerChangedCallback>,
    pub error: Option<ErrorCallback>,
}

impl SignalCallbacks {
    /// Empty subscription. The bridge falls back to the
    /// `NoopBackend` behaviour for every signal.
    pub const fn none() -> Self {
        Self {
            state_changed: None,
            mute_changed: None,
            speaker_changed: None,
            error: None,
        }
    }
}

/// Map an `hxvoice::state::SessionState` to the discriminant the
/// C header uses (`gtkhx_voice_state`). Pulled out so the test
/// suite can pin the mapping without dragging in unsafe extern fn
/// noise.
pub(crate) fn session_state_to_ffi(
    state: hxvoice::state::SessionState,
) -> u32 {
    use hxvoice::state::SessionState as S;
    match state {
        S::Idle => 0,
        S::JoinSent => 1,
        S::OfferPending => 2,
        S::Connecting => 3,
        S::Connected => 4,
        S::Leaving => 5,
        // `SessionState` is `#[non_exhaustive]` so a wildcard is
        // mandatory. Future variants should be added to the
        // header's `gtkhx_voice_state` enum and to the explicit
        // arms above; until then, map unknown to Idle (the safe
        // "not in voice" default for the C-side joined-flag
        // computation) and debug_assert so a test run catches
        // the omission.
        _ => {
            debug_assert!(false, "unhandled SessionState variant: {state:?}");
            0
        }
    }
}

/// Backend implementation that bridges to C callbacks. Production
/// uses this with `voice_runtime_send_wire_frame_cb` in
/// voice_panel.c so the state machine's outbound voice opcodes
/// reach `hlwrite_chunks` via the existing `hx_send_voice_*`
/// helpers, and with the matching signal callbacks so the C side
/// reflects authoritative state-machine state instead of running
/// optimistic UI.
///
/// `tear_down` is stubbed — the C side handles teardown via
/// `gtkhx_voice_runtime_free` on disconnect.
///
/// Main-thread-only by convention: `Backend` doesn't require
/// `Send`, and the entire runtime dispatch loop runs on the GLib
/// main thread (the `Inner` cell is `!Send` via `Rc` + `RefCell`,
/// which transitively makes `VoiceRuntime` `!Send`). Raw pointers
/// are auto-`Send`/`Sync` at the Rust level — they don't make this
/// struct `!Send` on their own — so the main-thread-only invariant
/// is enforced by the surrounding runtime, not by this type.
pub struct CallbackBackend {
    user_data: *mut core::ffi::c_void,
    send_wire_frame_cb: Option<SendWireFrameCallback>,
    signal_callbacks: SignalCallbacks,
}

impl CallbackBackend {
    /// Construct a backend that calls `send_wire_frame_cb` for
    /// every `Action::SendWireFrame` action. A `None` callback
    /// makes the backend behave like `NoopBackend` for that
    /// surface. No signal subscription.
    pub fn new(
        user_data: *mut core::ffi::c_void,
        send_wire_frame_cb: Option<SendWireFrameCallback>,
    ) -> Self {
        Self::new_with_signals(
            user_data,
            send_wire_frame_cb,
            SignalCallbacks::none(),
        )
    }

    /// Construct a backend with both wire-frame and signal
    /// callbacks. Each individual callback may still be `None` —
    /// the runtime falls back to NoopBackend behaviour for that
    /// specific surface.
    pub fn new_with_signals(
        user_data: *mut core::ffi::c_void,
        send_wire_frame_cb: Option<SendWireFrameCallback>,
        signal_callbacks: SignalCallbacks,
    ) -> Self {
        Self {
            user_data,
            send_wire_frame_cb,
            signal_callbacks,
        }
    }
}

impl Backend for CallbackBackend {
    fn send_wire_frame(&mut self, opcode: u32, body: &[u8]) {
        let Some(cb) = self.send_wire_frame_cb else {
            return;
        };
        // SAFETY: the C caller registered the callback and
        // user_data together; both stay valid for the lifetime
        // of the backend, which the runtime owns. `body` is a
        // borrowed slice the callback may read for the duration
        // of the call only.
        unsafe { cb(self.user_data, opcode, body.as_ptr(), body.len()) };
    }
    fn emit_signal(&mut self, kind: SignalKind, payload: SignalPayload) {
        match (kind, payload) {
            (
                SignalKind::StateChanged,
                SignalPayload::StateChanged { new_state },
            ) => {
                if let Some(cb) = self.signal_callbacks.state_changed {
                    let ffi_state = session_state_to_ffi(new_state);
                    // SAFETY: same lifetime contract as
                    // send_wire_frame.
                    unsafe { cb(self.user_data, ffi_state) };
                }
            }
            (
                SignalKind::MuteChanged,
                SignalPayload::MuteChanged { muted },
            ) => {
                if let Some(cb) = self.signal_callbacks.mute_changed {
                    // SAFETY: same lifetime contract as
                    // send_wire_frame.
                    unsafe {
                        cb(self.user_data, if muted { 1 } else { 0 })
                    };
                }
            }
            (
                SignalKind::SpeakerChanged,
                SignalPayload::SpeakerChanged { uid, is_speaking },
            ) => {
                if let Some(cb) = self.signal_callbacks.speaker_changed {
                    // SAFETY: same lifetime contract as
                    // send_wire_frame.
                    unsafe {
                        cb(
                            self.user_data,
                            uid,
                            if is_speaking { 1 } else { 0 },
                        )
                    };
                }
            }
            (
                SignalKind::Error,
                SignalPayload::Error { text },
            ) => {
                if let Some(cb) = self.signal_callbacks.error {
                    // Build a NUL-terminated C string the callback
                    // can read for the duration of the call. The
                    // CString drops at scope exit; the callback
                    // must copy out anything it wants to retain
                    // (the C handler typically passes the bytes
                    // into a glib utility like g_strdup before
                    // returning).
                    if let Ok(c) = std::ffi::CString::new(text.as_str()) {
                        // SAFETY: same lifetime contract as
                        // send_wire_frame. CString is owned and
                        // valid until the end of this scope.
                        unsafe { cb(self.user_data, c.as_ptr()) };
                    } else {
                        // Embedded NUL in the toast text — the
                        // state machine builds these from concat!
                        // string literals so this branch is
                        // unreachable in practice. Skip the
                        // callback rather than panic; nothing the
                        // C side can usefully do with a sentinel
                        // text either way.
                        debug_assert!(
                            false,
                            "Error signal payload contained an embedded NUL: {text:?}"
                        );
                    }
                }
            }
            // RoomStatus payload (cid + connection_state) has no C
            // subscriber slot yet; the participant data the user
            // list cares about already flows via the rcv.c path
            // into hx_voice_model_ingest_participants directly,
            // so RoomStatus is informational from this signal's
            // POV. Drop silently.
            _ => {
                debug_assert!(
                    matches!(kind, SignalKind::RoomStatus),
                    "unexpected (SignalKind, SignalPayload) pair: \
                     ({kind:?}, payload-omitted)"
                );
            }
        }
    }
    fn tear_down(&mut self) {
        // Stubbed: the C side handles teardown via
        // gtkhx_voice_runtime_free on disconnect; no need to
        // signal back through this path yet.
    }
}

/// Recording backend that captures every call into per-method
/// `Vec`s. Used by Tier 2 tests to assert on the action dispatch.
///
/// Exposed publicly so external integration tests can use the
/// same instrument the in-crate tests do.
#[derive(Default)]
pub struct RecordingBackend {
    /// Every `(opcode, body)` pair the runtime asked us to send.
    pub wire_frames: Vec<(u32, Vec<u8>)>,
    /// Every signal emit the runtime asked us to perform.
    pub signals: Vec<(SignalKind, SignalPayload)>,
    /// Number of `tear_down()` calls observed.
    pub tear_downs: u32,
}

impl Backend for RecordingBackend {
    fn send_wire_frame(&mut self, opcode: u32, body: &[u8]) {
        self.wire_frames.push((opcode, body.to_vec()));
    }
    fn emit_signal(&mut self, kind: SignalKind, payload: SignalPayload) {
        self.signals.push((kind, payload));
    }
    fn tear_down(&mut self) {
        self.tear_downs = self.tear_downs.saturating_add(1);
    }
}

/// Per-session owner of the WebRTC pipeline and the state machine
/// it drives. Cheap to clone (it's just two `Rc<RefCell<…>>`s),
/// which lets signal closures hold weak references back without
/// circular ownership.
///
/// The pipeline itself is constructed lazily on `new()`; tests
/// that don't need the GStreamer side can use
/// [`VoiceRuntime::new_without_pipeline`] to skip the
/// `webrtcbin` construction (and the gst-plugins-bad runtime
/// requirement that goes with it).
///
/// ## Re-entrancy
///
/// `Inner` and `Backend` live in SEPARATE `Rc<RefCell<…>>`s
/// deliberately. The backend is the outward-facing surface where
/// re-entrancy is most likely: a GLib signal handler invoked by
/// `emit_signal` may turn around and call `handle_event` on the
/// same runtime (e.g. the UI clicks "Leave" from inside the
/// `voice-error` toast handler), and that nested call needs to
/// borrow `Inner` cleanly. Holding an `Inner` borrow across the
/// backend call would panic on re-entry; splitting the cells
/// makes the borrow scopes independent.
#[derive(Clone)]
pub struct VoiceRuntime {
    inner: Rc<RefCell<Inner>>,
    backend: Rc<RefCell<Box<dyn Backend>>>,
}

struct Inner {
    machine: SessionMachine,
    /// `Some` once the GStreamer pipeline has been constructed.
    /// `None` for the pipeline-less test path. Holds the
    /// `webrtcbin` element as a child (added in `new()`).
    ///
    /// Kept here to extend the pipeline's lifetime to match the
    /// runtime's (the pipeline owns `webrtcbin` and the receive-leg
    /// bins step 5+ will add). The dispatch arms reach `webrtcbin`
    /// directly via the [`Inner::webrtcbin`] handle rather than
    /// going through the pipeline lookup.
    #[allow(dead_code)]
    pipeline: Option<gstreamer::Pipeline>,
    /// The `webrtcbin` element living inside `pipeline`. `Some`
    /// whenever `pipeline` is `Some`; the two move together. Held
    /// directly so dispatch arms don't have to look it up by name
    /// on every action.
    ///
    /// `None` for the pipeline-less test path (the test still
    /// exercises the dispatch loop and the timer / Backend arms,
    /// but the SDP arms early-return when there's no bin to
    /// drive — exactly what production wants if the bin was never
    /// initialised either).
    webrtcbin: Option<gstreamer::Element>,
    /// Currently-armed timer sources keyed by timer kind. The
    /// `Option` carries the `glib::SourceId` when the runtime
    /// owns the GLib default `MainContext` (i.e. production, on
    /// the main thread) — that's the case where the one-shot
    /// `glib::timeout_add_local` actually attached and the
    /// callback will fire `Event::Timeout { kind }` when the
    /// spec-mandated duration elapses.
    ///
    /// `None` is the test fallback: when multiple cargo test
    /// threads race over default-context ownership, the loser
    /// can't call `timeout_add_local` (it panics on
    /// non-ownership). We degrade to bookkeeping only — the
    /// kind is tracked so `armed_timers()` returns it, but no
    /// glib source exists. Tests that need to actually observe
    /// timer firing live in `tests/timer_firing.rs` which runs
    /// in its own process and owns the context outright.
    ///
    /// `HashMap` rather than `Vec` so cancellation by kind
    /// (the common shape — `CancelTimer { kind: Dtls }` when
    /// a `WebRTCConnectionState::Connected` arrives) is O(1)
    /// and so a re-arm of the same kind cleanly supersedes
    /// the previous source. The bound stays tiny: at most
    /// four entries (JoinReply / Dtls / IceConnectivity /
    /// Media) per the spec's timer table.
    ///
    /// Drop on `Inner` walks this map and `remove`s every
    /// `Some` source so glib doesn't fire a callback against a
    /// dropped runtime.
    armed_timer_sources:
        HashMap<Timeout, Option<gstreamer::glib::SourceId>>,
    /// True while `handle_event` is walking the action list for
    /// an event. A re-entrant call (backend dispatches an action
    /// that triggers a Hotline signal which calls back into
    /// `handle_event`) queues onto `pending` instead of running
    /// inline; the outer loop drains the queue **between
    /// events** — after the current event's full action list
    /// finishes dispatching, the loop checks `pending`, pops the
    /// next event, and walks its action list start-to-finish.
    /// Drains happen at the event boundary, not after each
    /// individual action.
    ///
    /// Removes the entire class of "borrow_mut while
    /// dispatching" panics, including the Backend-on-Backend
    /// case the simple two-Rc split doesn't cover on its own.
    dispatching: bool,
    /// Events queued during re-entrant `handle_event` calls. The
    /// outer loop drains this after every step's actions finish
    /// dispatching.
    pending: VecDeque<Event>,
    /// Unique id assigned at construction; the key into
    /// [`MAIN_THREAD_RUNTIMES`]. Captured by `Send`-required
    /// closures (Promise change-funcs, ICE callbacks in step 4+)
    /// so they can find the runtime back on the main thread
    /// without holding a non-`Send` `Rc`.
    runtime_id: u64,
    /// Generation counter incremented on every `Action::CreateAnswer`
    /// dispatch. The `gst::Promise` callback captures the value
    /// at issue time and compares it against the current
    /// generation on the main thread; mismatches mean a newer
    /// offer arrived in the meantime (the state machine's
    /// renegotiation path) and the older answer must be
    /// dropped on the floor — otherwise the stale answer would
    /// re-enter `handle_event(WebrtcAnswerCreated)` and the
    /// state machine would emit a `SendWireFrame(603, …)` for
    /// the wrong offer, blowing up the renegotiated session.
    ///
    /// See `hxvoice/src/state.rs` `(OfferPending, SdpOfferReceived)`
    /// for the matching state-machine assumption that "the
    /// existing CreateAnswer promise will resolve with stale
    /// contents, which the runtime drops on the floor."
    answer_generation: u64,
    /// Pads that `webrtcbin.pad-added` has reported but the
    /// state machine hasn't yet acknowledged with a
    /// `StartReceivePipeline` action. Keyed by `mid`. The
    /// pad-added callback marshals to the main thread, parks
    /// the pad here, and fires `Event::WebrtcPadAdded { mid }`;
    /// the state machine cross-references its mid→user_id
    /// table and returns `StartReceivePipeline { mid, user_id }`,
    /// whose dispatch arm pops the pad out and links it to the
    /// freshly-built receive bin.
    ///
    /// `gst::Pad` is `Send + Sync` (GObject-backed); no special
    /// thread plumbing needed.
    pending_pads: HashMap<String, gstreamer::Pad>,
    /// Receive-leg bins currently linked into the pipeline,
    /// keyed by `mid`. The `Action::StartReceivePipeline`
    /// dispatch arm inserts into this map after a successful
    /// link; `StopReceivePipeline` removes the entry, sets the
    /// bin to `Null`, and lets it drop out of the pipeline.
    receive_bins: HashMap<String, gstreamer::Bin>,
    /// `BusWatchGuard` returned by `attach_pipeline_bus_watch`'s
    /// `add_watch_local`. Dropping this guard removes the watch
    /// source from the main context; we keep it parked here so
    /// the watch survives for the lifetime of the runtime
    /// (otherwise the pipeline's error / warning messages would
    /// stop reaching the logger after construction returns).
    ///
    /// Held for its drop side-effect; never read.
    #[allow(dead_code)]
    bus_watch_guard: Option<gstreamer::bus::BusWatchGuard>,
    /// Always-on RTP buffer counter, incremented by a `BUFFER`
    /// probe on every receive-bin's depay sink as soon as
    /// [`start_receive_bin`] links one in. Read by the wedge
    /// watchdog (see [`WEDGE_WATCHDOG_INTERVAL_MS`]) as a
    /// liveness signal — if the counter doesn't advance for a
    /// full watchdog window while in `Connecting`, the runtime
    /// declares the session wedged and feeds the state machine a
    /// `Timeout::WedgeDeadline`.
    ///
    /// Why `Arc` rather than the `Rc`-shaped fields nearby:
    /// pad probes run on the GStreamer streaming thread, so the
    /// closure they hold needs `Send + Sync` access to the
    /// counter. The increment itself is a single relaxed atomic
    /// add — cheap enough to leave on in production with the
    /// diagnostic `voice-flow` log probes still gated off.
    rtp_buffers_received: Arc<AtomicU64>,
    /// Currently-armed wedge watchdog `glib::SourceId`, if any.
    /// Armed in `handle_event` when the state machine transitions
    /// into `Connecting`; cancelled on the way out (whether to
    /// `Connected`, `Leaving`, or `Idle`). The dispatch arm uses
    /// `glib::timeout_add_local` with [`WEDGE_WATCHDOG_INTERVAL_MS`],
    /// re-arms itself from the callback when RTP is still
    /// flowing, and lets the source expire on a stalled window
    /// after injecting `Timeout::WedgeDeadline`.
    ///
    /// `None` is ambiguous on its own: it means EITHER
    ///   (a) the watchdog is unarmed (the machine is not in
    ///       `Connecting`), OR
    ///   (b) the machine IS in `Connecting` but we degraded to
    ///       the test-fallback path (cargo's parallel runner has
    ///       another thread owning the default `MainContext`, so
    ///       `timeout_add_local` is unavailable and we kept
    ///       bookkeeping only).
    /// Disambiguate by reading `machine.state()` alongside —
    /// that's the canonical "is the watchdog conceptually armed?"
    /// signal and what [`VoiceRuntime::wedge_watchdog_armed`]
    /// returns.
    wedge_watchdog_source: Option<gstreamer::glib::SourceId>,
    /// Last `rtp_buffers_received` value the watchdog observed.
    /// Compared against the live counter on each tick: equality
    /// means no RTP arrived during the window and the runtime
    /// injects `Timeout::WedgeDeadline`; inequality means audio
    /// is flowing despite webrtcbin not having reported
    /// `Connected`, so the watchdog updates the snapshot and
    /// rearms.
    ///
    /// Set to a fresh snapshot of `rtp_buffers_received` at
    /// `arm_wedge_watchdog` time — NOT zeroed. The counter is
    /// process-lifetime (never reset for a new session), so the
    /// "wedged?" check needs deltas, not absolutes. Pre-existing
    /// activity from a previous join is therefore baked into the
    /// snapshot's baseline and ignored by the comparison.
    wedge_watchdog_last_snapshot: u64,
    /// `true` when the wedge watchdog is conceptually armed, i.e.
    /// `arm_wedge_watchdog` ran for the current `Connecting`
    /// entry and `cancel_wedge_watchdog` has not yet fired.
    ///
    /// Pre-Fix-#2 the public accessor [`VoiceRuntime::wedge_watchdog_armed`]
    /// inferred this from `machine.state() == Connecting` — a
    /// reasonable approximation while every Connecting entry
    /// armed the watchdog. With Fix #2's "skip on re-entry after
    /// Connected" behaviour the state alone no longer
    /// distinguishes armed from unarmed, so we track it
    /// explicitly. Tests can still rely on the accessor returning
    /// the same answer in production and in cargo's parallel
    /// runner (where the glib source slot may be `None` even
    /// when the watchdog is conceptually armed).
    wedge_watchdog_armed_flag: bool,
    /// Last `WebRTCPeerConnectionState` we observed via the
    /// `notify::connection-state` watch, translated through
    /// [`map_peer_connection_state`]. `None` until the first
    /// notify fires for this session.
    ///
    /// **Why we track it.** webrtcbin emits `notify::connection-
    /// state` only on actual changes to the property. After the
    /// initial handshake settles, a server-initiated SDP
    /// renegotiation (which the state machine processes as
    /// `Connected → OfferPending → Connecting → Connected`)
    /// produces no fresh `Connected` notify because the property
    /// was already `Connected` when the offer arrived. The state
    /// machine's `(Connecting, Connected)` arm therefore never
    /// fires, and we'd sit in `Connecting` indefinitely — long
    /// enough for the wedge watchdog to tear down a perfectly
    /// healthy session.
    ///
    /// Reset to `None` on every transition into `JoinSent` so a
    /// fresh join starts from a clean slate.
    last_seen_peer_state: Option<ConnectionState>,
    /// `true` once the state machine has reached `Connected` since
    /// the most recent `JoinSent` entry. Tracks "this session has
    /// ever been fully connected" so subsequent re-entries to
    /// `Connecting` (renegotiation cycles) can skip the wedge
    /// watchdog without losing it for genuine never-connected
    /// joins.
    ///
    /// Set on `(Connecting → Connected)`. Reset on every transition
    /// into `JoinSent`. The reset path covers the room-switch case
    /// too (`JoinRequested { cid }` for a different cid walks
    /// through JoinSent) — a fresh room is a fresh session for
    /// wedge purposes.
    has_been_connected_since_join: bool,
    /// Per-user-id voice-activity counters. Each entry counts the
    /// `level`-element RMS windows that cleared the speaking
    /// threshold for that uid; [`handle_level_message`] does the
    /// entry-or-insert + increment from the pipeline bus watch, and
    /// the speaker-activity evaluator ([`speaker_tick`]) reads
    /// deltas. Both run on the GLib main thread, so the [`Mutex`]
    /// only ever guards same-thread access (it stays an
    /// `Arc<Mutex<…>>` for symmetry with the streaming-thread
    /// `rtp_buffers_received` counter and the test hooks; contention
    /// is nil).
    ///
    /// Indexed by Hotline user id (16 bits, parsed from the SDP
    /// `a=mid:user-{uid}` label via the receive bin's name). The
    /// `send` mid is excluded — it's the local-user send leg, which
    /// has no receive bin and posts no remote-side `level` messages.
    ///
    /// Driving this off the `level` element's RMS rather than RTP
    /// packet arrival is the whole point of the VAD work: PCMU has
    /// no silence suppression, so RTP flows at a constant ~50 pps
    /// whether or not the remote is making sound. RMS thresholding
    /// distinguishes actual speech (see [`SPEAKING_RMS_THRESHOLD_DB`]).
    ///
    /// Why `Mutex<HashMap>` rather than `DashMap`: this map
    /// changes at most a handful of times per session
    /// (new participant joins), so contention is non-existent
    /// and the std-only dependency surface is cheaper than
    /// adding `dashmap`.
    per_user_voice_activity:
        Arc<std::sync::Mutex<HashMap<u16, Arc<AtomicU64>>>>,
    /// Previous-tick snapshot of [`per_user_voice_activity`] —
    /// used by [`speaker_tick`] to compute deltas. A uid with a
    /// non-zero delta since the last tick is considered
    /// "speaking"; equality is "silent". Updated at end-of-tick.
    ///
    /// **Lifecycle.** Entries are added on every tick that
    /// observes a new uid in the live map. Neither this snapshot
    /// nor `per_user_voice_activity` is pruned on receive-bin
    /// teardown — there's no code path that removes a uid once
    /// it's been seen. Both maps therefore grow monotonically
    /// across the runtime's lifetime (and are cleared wholesale on
    /// session teardown / rebuild).
    ///
    /// Memory cost: `n_distinct_uids` × (`u16` key + `u64` value +
    /// HashMap overhead) ≈ 10–40 bytes per uid ever seen.
    /// Negligible across realistic session lengths; if the runtime
    /// ever grows a multi-day-session use case worth pruning, the
    /// natural hook is a `LeaveRequested` or `TearDown` dispatch
    /// arm that walks `inner.receive_bins.keys()` and prunes both
    /// maps for any uid no longer represented.
    per_user_activity_prev_snapshot: HashMap<u16, u64>,
    /// Cached "is this uid speaking right now?" state from the
    /// most recent [`speaker_tick`] pass. Drives the
    /// `SignalKind::SpeakerChanged` emit decision: the runtime
    /// only fires the signal when this differs from the value
    /// the new tick computed.
    per_user_speaking: HashMap<u16, bool>,
    /// `glib::SourceId` for the speaker-activity timer if armed,
    /// `None` if the timer is not currently attached. Same
    /// "ownership of the GLib default `MainContext`" caveat as
    /// `wedge_watchdog_source` — tests that race on context
    /// ownership keep `None` here and run the tick manually via
    /// [`VoiceRuntime::speaker_tick_for_test`].
    speaker_timer_source: Option<gstreamer::glib::SourceId>,
}

impl Drop for Inner {
    fn drop(&mut self) {
        // Walk the pipeline back to Null before letting it drop.
        // Production sets it to Playing in VoiceRuntime::new so
        // webrtcbin can do peer-connection work; teardown is the
        // matching reverse. Without this, the pipeline thread
        // may outlive the dispatch loop and a late bus message
        // can fire against the about-to-be-dropped bus watch.
        if let Some(pipeline) = self.pipeline.as_ref() {
            let _ = pipeline.set_state(gstreamer::State::Null);
        }

        // Cancel any still-armed timer sources so glib doesn't
        // fire a callback against a now-dropped runtime. The
        // callback's `with_main_thread_runtime` lookup would
        // fail (we evict from the registry immediately below),
        // making the late fire a silent no-op, but cancelling
        // here is cleaner: glib's main loop doesn't keep
        // wakeups scheduled for a session that's gone.
        //
        // `mem::take` to drain — the SourceId by-value `remove`
        // call wants ownership of each id.
        for (_kind, source) in core::mem::take(&mut self.armed_timer_sources) {
            if let Some(s) = source {
                s.remove();
            }
        }

        // Cancel the wedge watchdog source the same way — same
        // reasoning, just a single `Option` instead of a map.
        if let Some(s) = self.wedge_watchdog_source.take() {
            s.remove();
        }

        // Same for the speaker-activity timer: a recurring source
        // that, left armed, would keep firing `speaker_tick` (and
        // hitting `with_main_thread_runtime`'s lookup miss) every
        // 200 ms until the GLib main loop itself tore down.
        if let Some(s) = self.speaker_timer_source.take() {
            s.remove();
        }

        // Evict our entry from the thread-local registry so a
        // late-firing closure (Promise change-func that races
        // teardown) becomes a silent no-op when it tries to
        // upgrade the Weak.
        //
        // `Inner: !Send` guarantees we drop on the same thread
        // that registered us, so this thread_local lookup hits
        // the correct registry.
        //
        // `try_with` rather than `with` because `Inner` can be
        // dropped during the thread's destructor pass (the
        // runtime's last `Rc` falls out of scope as the GLib
        // main context is being torn down). At that point
        // `MAIN_THREAD_RUNTIMES` may already be destroyed; the
        // registry going away itself makes the eviction
        // unnecessary, so an `AccessError` is just "nothing to
        // do" — ignore it instead of panicking on the way out.
        let _ = MAIN_THREAD_RUNTIMES.try_with(|cell| {
            cell.borrow_mut().remove(&self.runtime_id);
        });
    }
}

impl VoiceRuntime {
    /// Construct a runtime that owns a fresh `gst::Pipeline`
    /// containing a `webrtcbin` element, and will drive the
    /// supplied `Backend`. The bin isn't linked to a send or
    /// receive leg yet — those grow in step 5 (pad-added) and
    /// the audio-pipeline action arms.
    ///
    /// Calls `gst::init()` first — it's idempotent, so production
    /// code that already ran `gtkhx_voice_init()` from `main` just
    /// sees a no-op. A real failure (broken GStreamer install,
    /// missing plugins) propagates as
    /// [`RuntimeError::GstInitFailed`] rather than the delayed
    /// panic from inside gstreamer-rs's assert-initialised
    /// checks.
    ///
    /// `webrtcbin` lives in `gst-plugins-bad`. If the plugin
    /// isn't installed, `ElementFactory::make` fails and we
    /// surface [`RuntimeError::WebrtcbinUnavailable`]; the C
    /// side then disables voice UI for the rest of the session.
    pub fn new(backend: Box<dyn Backend>) -> Result<Self, RuntimeError> {
        gstreamer::init().map_err(RuntimeError::GstInitFailed)?;
        let runtime_id = NEXT_RUNTIME_ID.fetch_add(1, Ordering::Relaxed);
        let bits = build_pipeline_bits(runtime_id)?;
        let runtime = VoiceRuntime {
            inner: Rc::new(RefCell::new(Inner {
                machine: SessionMachine::new(),
                pipeline: Some(bits.pipeline),
                webrtcbin: Some(bits.webrtcbin),
                armed_timer_sources: HashMap::new(),
                dispatching: false,
                pending: VecDeque::new(),
                runtime_id,
                answer_generation: 0,
                pending_pads: HashMap::new(),
                receive_bins: HashMap::new(),
                bus_watch_guard: bits.bus_watch_guard,
                rtp_buffers_received: bits.rtp_buffers_received,
                wedge_watchdog_source: None,
                wedge_watchdog_last_snapshot: 0,
                wedge_watchdog_armed_flag: false,
                last_seen_peer_state: None,
                has_been_connected_since_join: false,
                per_user_voice_activity: bits.per_user_voice_activity,
                per_user_activity_prev_snapshot: HashMap::new(),
                per_user_speaking: HashMap::new(),
                speaker_timer_source: None,
            })),
            backend: Rc::new(RefCell::new(backend)),
        };
        register(&runtime);
        // Arm the periodic speaker-activity evaluator. Idempotent
        // and best-effort — see `arm_speaker_timer` for the
        // test-fallback semantics. Production: starts immediately,
        // emits SpeakerChanged signals every
        // SPEAKER_EVAL_INTERVAL_MS as voice activity flips.
        runtime.arm_speaker_timer();
        Ok(runtime)
    }

    /// Construct a runtime without the GStreamer pipeline. Tests
    /// that exercise the action dispatch don't need `webrtcbin`
    /// alive; using this constructor lets them skip the
    /// `gst::init()` requirement.
    ///
    /// The SDP / ICE / pad dispatch arms early-return when there
    /// is no `webrtcbin` to drive, so the same tests still cover
    /// the Backend + timer paths cleanly.
    pub fn new_without_pipeline(backend: Box<dyn Backend>) -> Self {
        let runtime_id = NEXT_RUNTIME_ID.fetch_add(1, Ordering::Relaxed);
        let runtime = VoiceRuntime {
            inner: Rc::new(RefCell::new(Inner {
                machine: SessionMachine::new(),
                pipeline: None,
                webrtcbin: None,
                armed_timer_sources: HashMap::new(),
                dispatching: false,
                pending: VecDeque::new(),
                runtime_id,
                answer_generation: 0,
                pending_pads: HashMap::new(),
                receive_bins: HashMap::new(),
                bus_watch_guard: None,
                rtp_buffers_received: Arc::new(AtomicU64::new(0)),
                wedge_watchdog_source: None,
                wedge_watchdog_last_snapshot: 0,
                wedge_watchdog_armed_flag: false,
                last_seen_peer_state: None,
                has_been_connected_since_join: false,
                per_user_voice_activity: Arc::new(std::sync::Mutex::new(
                    HashMap::new(),
                )),
                per_user_activity_prev_snapshot: HashMap::new(),
                per_user_speaking: HashMap::new(),
                speaker_timer_source: None,
            })),
            backend: Rc::new(RefCell::new(backend)),
        };
        register(&runtime);
        // Speaker-activity timer left UNARMED in the pipeline-less
        // path. Tests that exercise the evaluator directly drive
        // it via `speaker_tick_for_test`; tests that don't care
        // about per-pad activity wouldn't see anything change
        // either way.
        runtime
    }
}

/// Bundle of the GStreamer-side resources that
/// [`build_pipeline_bits`] produces — kept as a struct so
/// `VoiceRuntime::new` and the `Action::TearDown` rebuild path
/// (in `dispatch_inner`) consume the same shape.
///
/// Each rebuild produces fresh `Arc<AtomicU64>` allocations for the
/// global `rtp_buffers_received` wedge-watchdog counter and the
/// per-user voice-activity map. Reusing the previous session's
/// counters would leak the wedge-watchdog snapshot delta from one
/// session into the next, and the receive-bin RTP probe drops its
/// streaming-thread closure handle when the receive bins drop
/// during the Null transition.
struct PipelineBits {
    pipeline: gstreamer::Pipeline,
    webrtcbin: gstreamer::Element,
    bus_watch_guard: Option<gstreamer::bus::BusWatchGuard>,
    rtp_buffers_received: Arc<AtomicU64>,
    per_user_voice_activity:
        Arc<std::sync::Mutex<HashMap<u16, Arc<AtomicU64>>>>,
}

/// Construct a fresh pipeline + webrtcbin + signal wiring for a
/// given `runtime_id`. Called from [`VoiceRuntime::new`] at
/// session-build time and again from the `Action::TearDown`
/// dispatch arm when the runtime needs a clean WebRTC slate
/// after a `fail()` collapse (renegotiation wedge, ICE failure,
/// server task error). The latter path is what makes "click Join
/// Voice again after a wedge" recover cleanly — without
/// rebuilding webrtcbin, the next session inherits the previous
/// session's ICE credentials + DTLS fingerprints, and Janus's
/// fresh session-side credentials don't match.
///
/// Returns the freshly-allocated handles; the caller stores them
/// in `Inner`.
fn build_pipeline_bits(runtime_id: u64) -> Result<PipelineBits, RuntimeError> {
    let pipeline = gstreamer::Pipeline::builder()
        .name("hxvoice-pipeline")
        .build();
    // Log the underlying gstreamer-rs `BoolError` at each
    // failure site before collapsing it into the
    // payload-less `WebrtcbinUnavailable` variant. The error
    // type isn't `std::error::Error` and doesn't carry
    // enough structure to be wrappable cleanly; logging on
    // the way through is the simplest preservation that
    // still helps an operator distinguish "plugin missing"
    // from "pipeline rejected the element."
    //
    // Build webrtcbin with `bundle-policy=max-bundle`. This
        // is critical for the multi-mline case (join-second
        // client receives an SDP offer carrying BOTH a=mid:send
        // for our outgoing leg AND a=mid:user-N for forwarded
        // audio from another participant).
        //
        // Default `bundle-policy` is `none`. Reading
        // gstwebrtcbin.c:6212-6214:
        //
        //   if (webrtc->bundle_policy != GST_WEBRTC_BUNDLE_POLICY_NONE)
        //     if (!_parse_bundle (sdp->sdp, &bundled, error))
        //       goto done;
        //
        // …with `none`, webrtcbin SKIPS parsing the BUNDLE
        // group from the offer entirely. It then creates a
        // SEPARATE transportstream per mline. Janus's offer
        // includes `a=group:BUNDLE 0 1`, but our default-policy
        // answer ignores it, so the receive side ends up on
        // its own transportstream1 with its own session 1 in
        // rtpbin. Incoming RTP arriving on the bundled UDP
        // socket goes through transportstream0/session 0 (the
        // send transceiver's), where rtpbin can't find a
        // matching recvonly transceiver, doesn't fire its
        // pad-added, doesn't create a jitterbuffer, and our
        // webrtcbin.pad-added never gets called. The receive
        // leg dies silently and Janus times the session out.
        //
        // `max-bundle` makes webrtcbin parse + honour the
        // BUNDLE group, share a single transportstream + rtp
        // session across all mlines, and demux incoming RTP
        // via the MID extension to the right transceiver.
        // The fix complements the codec-preferences pin we set
        // in `connect_on_new_transceiver`: without that, the
        // answer mline lacks a codec; without this, the
        // answer lacks a working BUNDLE group. Both have to
        // be in place for the receive side to function.
        let webrtcbin = gstreamer::ElementFactory::make("webrtcbin")
            .name("hxvoice-webrtcbin")
            .property_from_str("bundle-policy", "max-bundle")
            .build()
            .map_err(|e| {
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: failed to build webrtcbin element: {e}"
                );
                RuntimeError::WebrtcbinUnavailable
            })?;
        pipeline
            .add(&webrtcbin)
            .map_err(|e| {
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: failed to add webrtcbin to pipeline: {e}"
                );
                RuntimeError::WebrtcbinUnavailable
            })?;
        // Add the send-leg stub so webrtcbin has something to
        // advertise on the audio mline when it answers. Without
        // a sink pad attached, webrtcbin produces an answer with
        // `a=inactive` for the audio media — Janus then has nothing
        // to route the media stream through, and the ICE
        // connection-state walks new → checking → failed at the
        // ~7 second mark.
        //
        // The send bin captures from `autoaudiosrc` (system-
        // default microphone). Phase 8.E adds settings-driven
        // device override via the DEVICE_PREFS in audio.rs; the
        // encoder + payloader chain is identical either way.
        //
        // Build failure here means the user's GStreamer install
        // is missing `mulawenc` or `rtppcmupay` (both in
        // gst-plugins-good); collapse to WebrtcbinUnavailable
        // since the runtime can't function without the send leg
        // either way.
        let input_device = crate::audio::input_device();
        let send_bin = crate::audio::make_send_bin(
            "hxvoice-send-bin",
            input_device.as_deref(),
        )
            .ok_or_else(|| {
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: failed to build the send bin — check that \
                     mulawenc and rtppcmupay are installed (both ship in \
                     gst-plugins-good)"
                );
                RuntimeError::WebrtcbinUnavailable
            })?;
        pipeline.add(&send_bin).map_err(|e| {
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: failed to add send bin to pipeline: {e}"
            );
            RuntimeError::WebrtcbinUnavailable
        })?;
        // Request a sink pad on webrtcbin and link the send bin's
        // src ghost pad to it. `sink_%u` returns a new sink pad
        // backed by a fresh transceiver — webrtcbin picks the
        // mline index. We DO NOT pre-add a transceiver via
        // `add-transceiver` because that creates a second,
        // unwired transceiver: SDP negotiation matches our
        // pre-added one to mline 0 (a=mid:send) leaving its
        // freshly-created send pad orphaned, then matches the
        // `request_pad_simple` transceiver to mline 1
        // (a=mid:user-N) collapsing it to recvonly. End result:
        // our send chain is wired into a recvonly transceiver,
        // packets get dropped, and the receive pad on the same
        // transceiver never gets exposed via `pad-added` because
        // webrtcbin thinks the output stream is "already
        // connected" from the sink_%u setup. Janus's per-user
        // mlines are the canonical way receive transceivers
        // materialise here: this `sink_%u` request creates one
        // sendonly transceiver for our outgoing audio, and
        // webrtcbin auto-creates a recvonly transceiver (with a
        // src pad) for every `a=mid:user-N` line in the offer.
        //
        // The link must happen BEFORE pipeline.set_state(Playing)
        // so the negotiation sees a populated transceiver
        // direction when create-answer fires later.
        let webrtc_sink = webrtcbin
            .request_pad_simple("sink_%u")
            .ok_or_else(|| {
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: webrtcbin refused to grant a sink_%u pad"
                );
                RuntimeError::WebrtcbinUnavailable
            })?;
        let send_src = send_bin
            .static_pad("src")
            .ok_or_else(|| {
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: send_bin missing its ghost src pad"
                );
                RuntimeError::WebrtcbinUnavailable
            })?;
        send_src.link(&webrtc_sink).map_err(|e| {
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: failed to link send_bin → webrtcbin sink: {e:?}"
            );
            RuntimeError::WebrtcbinUnavailable
        })?;
        // Note: we deliberately do NOT pre-add a Recvonly
        // transceiver here, even though the receive pad-added
        // misfire suggests it might help.
        //
        // We tried that on claude/voice-pre-add-recvonly: SDP
        // matching DID become symmetric (transceiver0 →
        // sendonly for the send mline, transceiver1 → recvonly
        // for the user-N mline) and the test trace showed both
        // transceivers reaching the SDP processor's "creating
        // new receive pad" branch. But:
        //
        //   (a) pad-added STILL didn't fire — the receive
        //       src_0 pad stayed in detached state and the
        //       "stream already connected to rtpbin" short-
        //       circuit kept firing exactly as before. So
        //       pre-adding the transceiver doesn't solve the
        //       underlying gst_element_add_pad gating.
        //   (b) ICE connectivity checks now fail — the peer
        //       connection state goes `new → connecting →
        //       failed` at the 30 s checking timeout, instead
        //       of reaching `connected` as it did with one
        //       transceiver. Something about the second
        //       transceiver's SDP shape or BUNDLE wiring
        //       confuses libnice or Janus.
        //
        // Until we understand the receive pad-add gate
        // (probably needs reading gstwebrtcbin.c source for
        // `_update_transceiver_from_sdp_media` and whatever
        // pad-add path it expects after "creating new receive
        // pad"), the right move is to stay on the single-
        // transceiver setup that at least has working ICE +
        // working send-direction RTP, and pick this back up
        // once we have a clearer picture of the webrtcbin
        // internals.
        //
        // `runtime_id` is taken from the caller — `VoiceRuntime::new`
        // allocates a fresh id on construction; the
        // `Action::TearDown` rebuild path reuses the existing id so
        // the registry entry survives the wedge → rejoin cycle.
        // Allocate the RTP-activity counter UPFRONT so we can both
        // (a) clone an `Arc` into `connect_pad_added`'s streaming-
        // thread closure for the receive-bin liveness probe to
        // increment, and (b) move the same allocation into Inner
        // for the wedge watchdog to read from. Sharing the
        // allocation guarantees the closure and Inner are talking
        // to the same counter — no risk of a "probe writes one
        // counter, watchdog reads a different one" mismatch.
        let rtp_buffers_received = Arc::new(AtomicU64::new(0));
        // Per-user voice-activity counters. Allocated UPFRONT so
        // `Inner` and the `level`-message bus handler share the live
        // collection — `handle_level_message` populates new uids on
        // the fly (main thread); the evaluator reads snapshots on
        // each tick (main thread).
        let per_user_voice_activity = Arc::new(std::sync::Mutex::new(
            HashMap::<u16, Arc<AtomicU64>>::new(),
        ));
        // Wire the on-ice-candidate signal BEFORE registering.
        // The signal callback only looks the runtime up via
        // `with_main_thread_runtime` (which acquires the registry
        // entry lazily), so the order is safe in either direction;
        // doing it pre-register keeps the construction sequence
        // strictly linear.
        connect_on_ice_candidate(&webrtcbin, runtime_id);
        connect_pad_added(
            &webrtcbin,
            &pipeline,
            runtime_id,
            Arc::clone(&rtp_buffers_received),
        );
        connect_connection_state_notify(&webrtcbin, runtime_id);
        connect_on_new_transceiver(&webrtcbin);
        let bus_watch_guard = attach_pipeline_bus_watch(&pipeline, runtime_id);
        if bus_watch_guard.is_none() {
            // No bus watch means three things silently stop working
            // for this session: GStreamer error/warning triage
            // logging, and — since the VAD landed — the `level`
            // element's RMS messages that drive the per-uid speaker
            // indicator (so SPEAKING never lights up). The only way
            // `attach_pipeline_bus_watch` returns `None` in
            // production is a failure to acquire the default
            // `MainContext` (the cargo-parallel-test loser-of-the-
            // race path is expected and harmless; production runs
            // single-threaded on the main thread and shouldn't hit
            // it). Make it loud so a field report is diagnosable
            // rather than a mystery "indicator never moves".
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: could not attach the pipeline bus watch \
                 (default MainContext unavailable). Voice will still \
                 connect, but pipeline error/warning logging and the \
                 `level`-based speaker indicator are disabled for this \
                 session."
            );
        }
        // Transition the pipeline out of Null so webrtcbin's
        // internal peer connection becomes usable. While the
        // pipeline is in Null, webrtcbin reports its peer
        // connection as `closed` and silently aborts every task
        // we hand it — set-remote-description, create-answer,
        // add-ice-candidate, the lot. That manifests as the
        // state machine getting stuck in OfferPending: webrtcbin
        // accepts the call, logs "Peerconnection is closed,
        // aborting execution" at DEBUG level, and never resolves
        // the promise.
        //
        // Playing is the production target — that's the state
        // webrtcbin needs to actually flow media. A failure here
        // (state-change rejected by an element) is fatal to the
        // session, so collapse to WebrtcbinUnavailable rather
        // than soldier on with a half-initialised pipeline.
        // Move the pipeline to Playing. webrtcbin's internal
        // `is_closed` flag mirrors the bin's element state: it's
        // TRUE while in Null, FALSE in any higher state. With
        // is_closed=TRUE every peer-connection task (set-remote-
        // description, create-answer, add-ice-candidate, ...)
        // logs "Peerconnection is closed, aborting execution" at
        // DEBUG level and returns silently — which manifests
        // user-side as the state machine getting stuck in
        // OfferPending forever.
        //
        // The state change is best-effort: in test environments
        // (no audio devices, no GLib main loop driving the bus)
        // rtpbin and other internal elements may refuse to
        // preroll and the call returns StateChangeError. That's
        // fine for the unit tests, which exit the dispatch arms
        // cleanly regardless of peer-connection state. In
        // production the pipeline reaches at least Ready (often
        // Async toward Playing as transceivers are added by the
        // SDP exchange) which is enough to clear `is_closed` so
        // the negotiation can proceed.
        if let Err(e) = pipeline.set_state(gstreamer::State::Playing) {
            // Failure here is usually one of two things:
            //   1. Missing GStreamer nice plugin (libnice).
            //      webrtcbin refuses to leave NULL when nicesink /
            //      nicesrc aren't registered, and silently aborts
            //      every peer-connection task afterwards
            //      ("Peerconnection is closed, aborting execution"
            //      at DEBUG level). On Debian / Ubuntu the plugin
            //      lives in its own package — gst-plugins-bad
            //      doesn't include it because of libnice's split
            //      licensing. Fix: `apt install gstreamer1.0-nice`.
            //      Fedora: gstreamer1-plugins-bad-free-extras or
            //      build gst-plugins-bad with --enable-nice.
            //   2. Test environment with no audio devices and no
            //      GLib main loop driving the bus — rtpbin can't
            //      preroll. Unit tests hit this path deliberately
            //      and don't drive real peer-connection work, so
            //      they exit cleanly even with the pipeline stuck
            //      in NULL.
            // (1) is the user-visible production case and the
            // reason this warning is loud about the package name.
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: pipeline set_state(Playing) returned {e:?}. \
                 If you see 'libnice elements are not available' on the \
                 webrtcbin channel just above, install the GStreamer nice \
                 plugin: `apt install gstreamer1.0-nice` on Debian/Ubuntu, \
                 or gstreamer1-plugins-bad-free-extras on Fedora. \
                 webrtcbin won't leave NULL without it and every SDP / \
                 ICE op will silently no-op."
            );
        }
        Ok(PipelineBits {
            pipeline,
            webrtcbin,
            bus_watch_guard,
            rtp_buffers_received,
            per_user_voice_activity,
        })
    }

/// Drive the existing pipeline back to Null, drop every
/// per-session GStreamer resource, then rebuild fresh handles via
/// [`build_pipeline_bits`] and re-arm the speaker-activity timer.
///
/// Called from the `Action::TearDown` dispatch arm so a subsequent
/// `JoinRequested` builds against a clean webrtcbin instead of
/// renegotiating against the previous session's ICE / DTLS keys.
///
/// No-op when the runtime was constructed via
/// [`VoiceRuntime::new_without_pipeline`] (test path) — the
/// pipeline slot is empty and the early return keeps the test
/// surfaces unchanged.
///
/// On rebuild failure we log loudly and leave `Inner`'s pipeline
/// / webrtcbin slots empty. The runtime stays usable for control-
/// channel-only operations (the SDP / ICE dispatch arms early-
/// return on a missing bin), but voice won't recover until the
/// session is recycled — same behaviour as if the original
/// `VoiceRuntime::new` had failed. The state machine has already
/// walked to `Leaving` by this point; the next `JoinRequested`
/// will surface the rebuild failure (or succeed, if the
/// underlying problem was transient).
fn reset_and_rebuild_pipeline(runtime: &VoiceRuntime) {
    // Snapshot what we need under a short borrow, then drop it
    // before touching GStreamer. The pipeline.set_state(Null) call
    // below may complete synchronously (Success / NoPreroll) or
    // hand the transition off to the streaming thread (Async); in
    // either case it can drive bus-message handlers that re-enter
    // `handle_event`, so releasing the Inner borrow first keeps
    // that path free of borrow_mut panics regardless of which
    // return shape we get. The async return value is handled
    // explicitly below — we do NOT wait for the transition to
    // land before continuing, see the comment at the set_state
    // call.
    let (pipeline, runtime_id) = {
        let mut inner = runtime.inner.borrow_mut();
        // Drop the receive-bin map first so the streaming-thread
        // probes attached to them release their `Arc` clones of
        // the old `rtp_buffers_received`. The bin elements get
        // walked to Null by the parent pipeline transition
        // below; explicit drop here is bookkeeping, not lifecycle.
        inner.receive_bins.clear();
        inner.pending_pads.clear();
        inner.answer_generation = 0;
        inner.last_seen_peer_state = None;
        inner.has_been_connected_since_join = false;
        // Cancel every armed timer source. The state machine's
        // CancelTimer cleanup before TearDown should have done
        // this already, but defense-in-depth: a stray timer
        // callback firing against a torn-down runtime would
        // re-enter the dispatch loop with confusing events.
        for (_kind, source) in core::mem::take(&mut inner.armed_timer_sources) {
            if let Some(s) = source {
                s.remove();
            }
        }
        if let Some(s) = inner.wedge_watchdog_source.take() {
            s.remove();
        }
        inner.wedge_watchdog_armed_flag = false;
        if let Some(s) = inner.speaker_timer_source.take() {
            s.remove();
        }
        // Drop the bus watch BEFORE the pipeline so its watch
        // source releases cleanly. `bus_watch_guard` is held only
        // for its drop side effect anyway.
        inner.bus_watch_guard = None;
        // Per-user voice-activity tracking is per-session — drop it
        // so the next session starts from an empty speaker-evaluator
        // baseline.
        if let Ok(mut map) = inner.per_user_voice_activity.lock() {
            map.clear();
        }
        inner.per_user_activity_prev_snapshot.clear();
        inner.per_user_speaking.clear();
        // Pull the webrtcbin slot now so the late bus-message
        // handlers that read it during the Null transition see
        // `None` and skip cleanly. The element itself stays
        // alive until the pipeline's drop walks its children.
        let _webrtcbin = inner.webrtcbin.take();
        // Move the pipeline out before issuing the synchronous
        // state change. Owning the value rather than borrowing it
        // lets the function consume `pipeline` on drop after the
        // Null transition lands.
        let pipeline = inner.pipeline.take();
        let runtime_id = inner.runtime_id;
        (pipeline, runtime_id)
    };

    let Some(pipeline) = pipeline else {
        // Pipeline-less runtime (test path). Nothing to walk
        // down or rebuild — leave Inner's pipeline / webrtcbin
        // slots empty as they were.
        return;
    };

    // Walk back to Null. `Async` is acceptable here:
    // gstreamer-rs returns `Success`, `Async`, or `NoPreroll`
    // from `set_state`, and only `Failure` indicates a problem
    // we should log.
    //
    // We deliberately do NOT block on Async with
    // `pipeline.state(ClockTime::NONE)` here. Production runs
    // this function on the GLib main thread (the dispatch loop is
    // main-thread-only) and webrtcbin's internal state-change
    // worker posts bus messages back onto that same main
    // context to finish the transition. A blocking wait on the
    // main thread would deadlock the message drain it depends on.
    // The same reasoning applies in test environments where the
    // main loop isn't running at all.
    //
    // The "settle before rebuild" guarantee we DO need is on UDP
    // socket / ICE-agent ownership, not on bin state. The
    // `drop(pipeline)` immediately below releases the strong
    // reference to the bin; gstreamer-rs's GObject Drop machinery
    // walks element children, releasing nicesrc / nicesink
    // resources and unbinding their sockets BEFORE
    // `build_pipeline_bits` constructs the replacement. That
    // socket release is what makes the rebuild's fresh ICE agent
    // safe to bind on the same port; the Null-state transition
    // is a precursor to the drop, not a substitute for it.
    if let Err(e) = pipeline.set_state(gstreamer::State::Null) {
        gstreamer::warning!(
            gstreamer::CAT_RUST,
            "hxvoice: pipeline.set_state(Null) on teardown returned {e:?}"
        );
    }
    // Explicitly drop the pipeline before rebuilding. The drop
    // releases the strong ref, which walks the element children
    // and releases UDP socket / ICE agent ownership; without that
    // release, the rebuilt webrtcbin's nice elements would race
    // for the same UDP port the old one still holds.
    drop(pipeline);

    // Rebuild. Failure here means the next session can't drive
    // voice, but the state machine is already in Leaving and
    // the user will see the error toast that fail() emitted.
    let bits = match build_pipeline_bits(runtime_id) {
        Ok(b) => b,
        Err(e) => {
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: rebuild_pipeline failed: {e:?} — voice unavailable \
                 until the runtime is recycled"
            );
            return;
        }
    };
    {
        let mut inner = runtime.inner.borrow_mut();
        inner.pipeline = Some(bits.pipeline);
        inner.webrtcbin = Some(bits.webrtcbin);
        inner.bus_watch_guard = bits.bus_watch_guard;
        inner.rtp_buffers_received = bits.rtp_buffers_received;
        inner.wedge_watchdog_last_snapshot = 0;
        inner.per_user_voice_activity = bits.per_user_voice_activity;
    }
    // Re-arm the periodic speaker-activity evaluator against the
    // fresh `per_user_voice_activity` allocation. The old timer was
    // cancelled above; this restores production cadence so the
    // next session emits SpeakerChanged signals at the same
    // 200 ms interval.
    runtime.arm_speaker_timer();
}

impl VoiceRuntime {
    /// Drive one transition. Pumps `event` through the state
    /// machine, walks the returned action list, dispatches each
    /// effect.
    ///
    /// ## Return value semantics
    ///
    /// On a top-level (non-re-entrant) call, returns the
    /// [`SessionState`] the machine has entered after this event
    /// and any subsequently-queued events are fully drained.
    ///
    /// On a **re-entrant** call (a backend invocation made from
    /// inside another `handle_event`'s dispatch loop), the
    /// returned `SessionState` is the **current** state at the
    /// time of enqueueing — not the post-event state, because
    /// the queued event hasn't been processed yet. Callers that
    /// need the eventual state should consult [`Self::state`]
    /// after the outer dispatch unwinds (or, more typically,
    /// listen for the `StateChanged` signal via their `Backend`).
    ///
    /// Re-entrant calls are supported: a backend action that
    /// triggers a signal handler that synchronously calls
    /// `handle_event` again on the same runtime enqueues the
    /// nested event and the outer dispatch loop drains it after
    /// the current step's actions finish. This is the only way
    /// to avoid the "borrow_mut while we're already dispatching"
    /// panic class when production wires `EmitSignal` to a GLib
    /// signal that the UI's voice button reacts to.
    pub fn handle_event(&self, event: Event) -> SessionState {
        // Enqueue the event. If we're already dispatching (this is
        // a nested call from inside a Backend invocation), just
        // queue and return — the outer loop will drain it.
        {
            let mut inner = self.inner.borrow_mut();
            inner.pending.push_back(event);
            if inner.dispatching {
                return inner.machine.state();
            }
            inner.dispatching = true;
        }

        // Outer loop — drains pending until empty.
        loop {
            let next = {
                let mut inner = self.inner.borrow_mut();
                match inner.pending.pop_front() {
                    Some(e) => Some(e),
                    None => {
                        inner.dispatching = false;
                        None
                    }
                }
            };
            let Some(event) = next else {
                break;
            };

            // Mirror the WebRTC connection-state observation onto
            // `Inner::last_seen_peer_state` so the post-step diff
            // below can detect "renegotiation while already
            // Connected" regardless of whether the event came
            // from the real webrtcbin notify or from a test
            // injection. (The notify callback also writes this
            // field before invoking `handle_event`; the duplicate
            // write here is idempotent for that path and
            // load-bearing for the test-injection path.)
            if let Event::WebrtcConnectionStateChanged { state } = event {
                self.inner.borrow_mut().last_seen_peer_state = Some(state);
            }

            // Sample state BEFORE step() so we can compare against
            // the post-step state and detect whether this event
            // caused an entry into or exit from `Connecting`.
            // `SessionMachine::step` matches one (state, event) arm
            // per call and performs at most one transition, so the
            // before-vs-after pair is a complete diff of what this
            // event did to the state.
            let before = self.inner.borrow().machine.state();
            let actions = {
                let mut inner = self.inner.borrow_mut();
                inner.machine.step(event)
            };
            for action in actions {
                self.dispatch_inner(action);
            }
            let after = self.inner.borrow().machine.state();
            match (before, after) {
                // ---- Fresh join (Idle / Leaving → JoinSent) ----
                //
                // Reset the per-session wedge-watchdog tracking so a
                // new session starts from a clean slate. The
                // mid-session room-switch path (JoinRequested for a
                // different cid) also walks through JoinSent, and we
                // want it treated as a fresh session for wedge
                // purposes — switching rooms means new webrtcbin
                // negotiation, new ICE handshake, new "have we
                // reached Connected yet?" timeline.
                (b, SessionState::JoinSent)
                    if b != SessionState::JoinSent =>
                {
                    let mut inner = self.inner.borrow_mut();
                    inner.has_been_connected_since_join = false;
                    inner.last_seen_peer_state = None;
                }

                // ---- Entry into Connecting ----
                //
                // Three sub-cases, in order:
                //
                //   1. Renegotiation while webrtcbin is already
                //      Connected. The state machine bounced
                //      Connected → OfferPending → Connecting, but
                //      webrtcbin won't fire a fresh `Connected`
                //      notify because its property never changed.
                //      Synthesize one so the (Connecting, Connected)
                //      arm advances us back to Connected without
                //      waiting on a notify that will never come.
                //
                //   2. Re-entry to Connecting after we've already
                //      been Connected at least once in this session.
                //      Even if last_seen_peer_state isn't Connected
                //      yet (race: notify is racing the SDP path), we
                //      know the network leg is healthy enough that
                //      arming the wedge watchdog would only ever
                //      tear down a working session in the "everyone
                //      is muted" common case. Skip the watchdog.
                //
                //   3. First entry to Connecting — the genuine
                //      never-connected case the wedge watchdog
                //      was designed for. Arm it.
                (b, SessionState::Connecting)
                    if b != SessionState::Connecting =>
                {
                    let (already_connected, has_been_connected) = {
                        let inner = self.inner.borrow();
                        (
                            matches!(
                                inner.last_seen_peer_state,
                                Some(ConnectionState::Connected),
                            ),
                            inner.has_been_connected_since_join,
                        )
                    };
                    if already_connected {
                        // Case 1: synthesize. The state machine will
                        // pick it up on the next loop iteration and
                        // transition (Connecting → Connected). No
                        // need to arm/cancel the wedge — the
                        // synthetic step will land before any timer
                        // could fire anyway, and the
                        // (Connecting, Connected) arm below cancels
                        // along its normal path.
                        self.inner.borrow_mut().pending.push_back(
                            Event::WebrtcConnectionStateChanged {
                                state: ConnectionState::Connected,
                            },
                        );
                    } else if !has_been_connected {
                        // Case 3: first entry. Arm the wedge.
                        arm_wedge_watchdog(self);
                    }
                    // Case 2 (re-entry after Connected, but
                    // last_seen isn't Connected yet — racy notify):
                    // fall through, no wedge armed.
                }

                // ---- Reached Connected ----
                //
                // Mark "session has been Connected at least once"
                // so subsequent renegotiation cycles skip the
                // wedge. Cancel any wedge that was armed by the
                // initial Connecting entry. Same cancellation
                // semantics as the legacy code, plus the new
                // bookkeeping bit.
                (SessionState::Connecting, SessionState::Connected) => {
                    self.inner.borrow_mut().has_been_connected_since_join =
                        true;
                    cancel_wedge_watchdog(self);
                }

                // ---- Left Connecting for any other state ----
                //
                // The wedge watchdog is conceptually armed only
                // while the machine is in Connecting; clean it
                // up on every exit just like the legacy code did.
                (SessionState::Connecting, _) => {
                    cancel_wedge_watchdog(self);
                }

                _ => {}
            }
        }
        self.inner.borrow().machine.state()
    }

    /// Snapshot of the current state. Cheap accessor for tests.
    pub fn state(&self) -> SessionState {
        self.inner.borrow().machine.state()
    }

    /// Currently-active cid (the room the state machine is
    /// joining / joined to). `None` in `SessionState::Idle` and
    /// `SessionState::Leaving`. Production callers use this from
    /// inside signal callbacks to figure out which voice panel to
    /// update; the state machine owns the canonical answer so we
    /// just delegate.
    pub fn active_cid(&self) -> Option<u32> {
        self.inner.borrow().machine.active_cid()
    }

    /// Test-only accessor for the underlying `gst::Pipeline`.
    /// Phase 8.C step 8's bus-watch integration test uses this
    /// to post synthetic messages onto the bus and verify the
    /// watch closure handles them without panicking. Production
    /// code has no business reaching past the dispatch arms.
    #[doc(hidden)]
    pub fn pipeline_for_test(&self) -> Option<gstreamer::Pipeline> {
        self.inner.borrow().pipeline.clone()
    }

    /// `true` while the wedge watchdog is conceptually armed
    /// (the runtime transitioned into `Connecting` and hasn't
    /// left it yet). Test-introspection hook so the wedge-
    /// watchdog tests don't have to inspect private fields.
    ///
    /// Implementation note: we deliberately *don't* require
    /// `wedge_watchdog_source.is_some()`. Under cargo's parallel
    /// test runner the source slot is `None` even when the
    /// machine is in `Connecting` — `arm_wedge_watchdog`
    /// degrades to bookkeeping-only when another thread owns
    /// the default `MainContext`. Checking only the state means
    /// production (where `Some(source_id)` is set) and tests
    /// (where it stays `None`) both report the same answer.
    pub fn wedge_watchdog_armed(&self) -> bool {
        self.inner.borrow().wedge_watchdog_armed_flag
    }

    /// Number of RTP buffers the receive-side liveness probe has
    /// observed since the runtime was constructed. Test-only
    /// hook for the wedge-watchdog flow tests; production callers
    /// have no reason to read this. Exposes the underlying
    /// `Arc<AtomicU64>` by snapshot so callers don't have to
    /// reason about atomic ordering.
    #[doc(hidden)]
    pub fn rtp_buffers_received_for_test(&self) -> u64 {
        self.inner.borrow().rtp_buffers_received.load(Ordering::Relaxed)
    }

    /// Test-only mutator: bump the RTP-activity counter so the
    /// wedge-watchdog tick observes "audio is flowing" without
    /// having to set up a full receive bin and feed real
    /// buffers through it. Pairs with
    /// [`rtp_buffers_received_for_test`].
    #[doc(hidden)]
    pub fn bump_rtp_buffers_received_for_test(&self) {
        self.inner
            .borrow()
            .rtp_buffers_received
            .fetch_add(1, Ordering::Relaxed);
    }

    /// Test-only hook: drive one wedge-watchdog tick on demand,
    /// bypassing the 60-second one-shot. The tick is the same
    /// function the real timer callback runs — it samples the
    /// RTP counter, decides advance-vs-stall, and either rearms
    /// or injects `Timeout::WedgeDeadline`.
    ///
    /// Cancels any real `glib::timeout_add_local` source first.
    /// Without that, the source-id parked in
    /// `Inner::wedge_watchdog_source` would survive the manual
    /// tick (the tick clears the bookkeeping field but doesn't
    /// `remove()` the glib source — it's normally clearing
    /// bookkeeping because the source is about to expire
    /// naturally), and the real timer would still fire 60 s
    /// later with no way to cancel it. Tests can drive the tick
    /// repeatedly without leaving live glib sources behind.
    #[doc(hidden)]
    pub fn fire_wedge_watchdog_for_test(&self) {
        let prev = self.inner.borrow_mut().wedge_watchdog_source.take();
        if let Some(s) = prev {
            s.remove();
        }
        wedge_watchdog_tick(self);
    }

    /// Arm the speaker-activity evaluator. Exposed primarily so
    /// the production constructor can call it; tests that exercise
    /// the evaluator drive ticks manually via
    /// [`Self::speaker_tick_for_test`] and skip the timer setup.
    fn arm_speaker_timer(&self) {
        arm_speaker_timer(self);
    }

    /// Drive one tick of the speaker-activity evaluator. Test-only
    /// hook — production drives ticks from the recurring
    /// `glib::timeout_add_local` that `arm_speaker_timer` attaches.
    #[doc(hidden)]
    pub fn speaker_tick_for_test(&self) {
        speaker_tick(self);
    }

    /// Bump a uid's voice-activity counter, allocating a fresh
    /// entry at zero if the uid hasn't been seen before. Test-only
    /// hook used by the speaker_tick unit tests to simulate an
    /// above-threshold `level` RMS window without a real receive
    /// bin or pipeline (mirrors what [`handle_level_message`] does).
    #[doc(hidden)]
    pub fn bump_per_user_activity_for_test(&self, uid: u16) {
        let map_arc = self.inner.borrow().per_user_voice_activity.clone();
        let mut guard = match map_arc.lock() {
            Ok(g) => g,
            Err(_) => return,
        };
        guard
            .entry(uid)
            .or_insert_with(|| Arc::new(AtomicU64::new(0)))
            .fetch_add(1, Ordering::Relaxed);
    }

    /// Whether the speaker-activity timer is currently armed.
    /// `Some` source means a real glib timer is running; `None`
    /// stays `false` even after `arm_speaker_timer` finished
    /// (test-fallback path). Tests that need to assert "we did
    /// schedule a timer" should run inside a process that owns
    /// the default `MainContext`.
    #[doc(hidden)]
    pub fn speaker_timer_armed(&self) -> bool {
        self.inner.borrow().speaker_timer_source.is_some()
    }

    /// Currently-armed timer kinds. Order is unspecified
    /// (HashMap-backed); primarily a test-introspection hook.
    /// Use `.contains(&kind)` rather than `assert_eq!` against
    /// a literal `Vec`.
    pub fn armed_timers(&self) -> Vec<Timeout> {
        self.inner
            .borrow()
            .armed_timer_sources
            .keys()
            .copied()
            .collect()
    }

    /// Drive a single `Action` through the dispatch loop.
    ///
    /// Internal entrypoint shared by `handle_event`'s walk of
    /// the state machine's action list, and (with the
    /// `test-utils` Cargo feature) exposed to out-of-crate test
    /// binaries that want to inject surgical actions without
    /// running them through the state machine. In-crate unit
    /// tests in `runtime::tests` reach it via the implicit
    /// `cfg(test)` gate alone.
    ///
    /// **Not for production callers.** Direct dispatch bypasses
    /// `handle_event`'s re-entrancy queue: a backend callback
    /// invoked here that turns around and calls back into the
    /// runtime would panic on the nested `borrow_mut`. The
    /// queue/drain in `handle_event` is what makes that path
    /// safe; jump over it at your own risk.
    ///
    /// The dispatch arms themselves have no preconditions beyond
    /// "we're on the main thread" — actions are safe to drive in
    /// any order, dispatch arms handle their own pipeline-less /
    /// missing-bin / unknown-mid fallbacks.
    #[cfg(any(test, feature = "test-utils"))]
    pub fn dispatch(&self, action: Action) {
        self.dispatch_inner(action);
    }

    /// Private (always-compiled) wrapper around the dispatch
    /// match. Lets `handle_event` reach the same code path
    /// without conditionally compiling the inner logic.
    fn dispatch_inner(&self, action: Action) {
        match action {
            // ---- C-side integration points (delegated to Backend) ----
            //
            // Backend calls deliberately do NOT hold an `Inner`
            // borrow. A backend implementation that turns around
            // and re-enters `handle_event` (e.g. via a GLib signal
            // handler) needs the Inner cell free; holding it here
            // would panic on the nested borrow_mut. The Backend's
            // own cell still has a re-entrancy hazard if the
            // backend recursively calls back through `self`, but
            // that's a constraint the backend implementor manages.
            Action::SendWireFrame { opcode, body } => {
                self.backend.borrow_mut().send_wire_frame(opcode, &body.0);
            }
            Action::EmitSignal { kind, payload } => {
                self.backend.borrow_mut().emit_signal(kind, payload);
            }
            Action::TearDown => {
                self.backend.borrow_mut().tear_down();
                // Synchronously reset the GStreamer side of the
                // session: walk the existing pipeline back to Null
                // (which tears down webrtcbin's ICE agent + DTLS
                // keys + transceivers + UDP sockets), drop the
                // receive-bin map + pending pad slots, cancel
                // every armed timer source, and rebuild fresh
                // pipeline-bits so the next `JoinRequested`
                // negotiation flows through a clean webrtcbin.
                //
                // Why: pre-fix, fail() emitted TearDown and the
                // backend's `tear_down` was a stub. The pipeline
                // kept running with the previous session's ICE
                // credentials + DTLS fingerprint; on the next
                // join, Janus issued a fresh SDP offer with new
                // server-side credentials and the stale webrtcbin
                // tried to consummate it against the old keys,
                // landing the ICE handshake on `failed`. The
                // visible symptom was "rejoin after wedge
                // succeeds briefly, then 30 seconds later the
                // connection fails outright."
                //
                // `new_without_pipeline` runtimes never built a
                // pipeline in the first place, so the rebuild is
                // a no-op on the test path; the early-return on
                // `pipeline.is_none()` keeps that pathway alive.
                reset_and_rebuild_pipeline(self);
            }

            // ---- Timers (Phase 8.C step 7) ----
            //
            // ArmTimer: cancel any existing timer of this kind
            // (re-arm semantics — the state machine sends repeat
            // ArmTimers on renegotiation, intending the watchdog
            // to restart from the new event's reception time),
            // then arm a fresh one-shot `glib::timeout_add_local`
            // for `ms` milliseconds. The callback fires
            // `Event::Timeout { kind }` back at the state machine
            // via the same `with_main_thread_runtime` registry
            // hop the SDP / ICE callbacks use.
            //
            // We're already on the main thread (the dispatch
            // loop runs main-thread-only), which is the only
            // place `timeout_add_local` is callable.
            Action::ArmTimer { kind, ms } => {
                arm_timer(self, kind, ms);
            }

            // CancelTimer: pop the source out of our map and
            // call `remove()` if we have a live SourceId. The
            // source goes away from the main loop; no callback
            // fires. `None` entries (test-fallback bookkeeping)
            // just drop.
            Action::CancelTimer { kind } => {
                let source = self
                    .inner
                    .borrow_mut()
                    .armed_timer_sources
                    .remove(&kind);
                if let Some(Some(s)) = source {
                    s.remove();
                }
            }

            // ---- webrtcbin-shaped SDP actions (Phase 8.C step 3) ----
            //
            // Each arm reads the `webrtcbin` element out of `Inner`
            // and emits the matching signal. The borrow scope is
            // tight: we clone the element handle (cheap — it's
            // refcounted in glib) and drop the `Inner` borrow
            // before calling into GStreamer, so a synchronous
            // GStreamer callback can re-enter `handle_event`
            // cleanly.
            //
            // Silent no-op if the pipeline-less constructor was
            // used or the bin slot is empty — production always
            // has the bin (it's built by `VoiceRuntime::new`),
            // and the only paths that exercise these arms
            // without it are dispatch-loop tests that
            // deliberately use `new_without_pipeline` to avoid
            // the `gst::init()` requirement. The SDP actions are
            // emitted by the state machine unconditionally; the
            // runtime is the layer that decides whether to
            // actually drive GStreamer.
            Action::SetRemoteDescription { sdp } => {
                // set-remote-description on webrtcbin is async.
                // `create-answer` requires remote desc to be
                // applied first — issuing it synchronously after
                // (the previous bug) made webrtcbin error the
                // promise, the state machine never saw
                // `WebrtcAnswerCreated`, and the session stalled
                // in `OfferPending` until the JoinReply timer
                // walked it to Failed.
                //
                // Now: attach a promise to set-remote-description
                // and chain `create-answer` from inside its
                // resolution. The state machine's separate
                // `Action::CreateAnswer` becomes a no-op (see
                // below) — the runtime has already taken
                // responsibility for issuing it at the right
                // point.
                //
                // Bump `answer_generation` BEFORE handing the
                // promise to webrtcbin so a renegotiation offer
                // arriving while this chain is in flight
                // produces a fresh generation, and the in-flight
                // promise's eventual resolution is correctly
                // dropped as stale.
                let (webrtcbin, runtime_id, generation) = {
                    let mut inner = self.inner.borrow_mut();
                    inner.answer_generation =
                        inner.answer_generation.wrapping_add(1);
                    (
                        inner.webrtcbin.clone(),
                        inner.runtime_id,
                        inner.answer_generation,
                    )
                };
                // Loud dump of every `a=mid:` line in the offer
                // so we can see what Janus is actually labelling
                // its media lines with. cache_offer_mids in the
                // state machine only learns user_id from
                // `a=mid:user-N`; anything else (audio0, a
                // numeric mid, or Janus's reused `send` for
                // forwarded streams) still gets a receive bin
                // built — the WebrtcPadAdded handler falls back
                // to user_id = 0 ("anonymous receive") and audio
                // still plays, the UI just can't attribute it to
                // a specific speaker until we extend the mid
                // cache.
                for line in sdp.lines() {
                    let trimmed = line.trim_end_matches('\r');
                    if let Some(rest) = trimmed.strip_prefix("a=mid:") {
                        crate::debug::log!(
                            "voice-pipe",
                            "SDP offer carries a=mid:{rest}"
                        );
                    }
                }
                if let Some(bin) = webrtcbin {
                    apply_remote_offer_and_chain_answer(
                        &bin, &sdp, runtime_id, generation,
                    );
                }
            }
            Action::CreateAnswer => {
                // No-op: `Action::SetRemoteDescription`'s promise
                // chain calls `create_answer` once webrtcbin has
                // actually applied the remote offer. The state
                // machine still emits this action because the
                // chain isn't its concern; we silently absorb it
                // here so we don't fire `create-answer` twice
                // (once stale, once correctly chained).
            }
            Action::SetLocalDescription { sdp } => {
                let webrtcbin = self
                    .inner
                    .borrow()
                    .webrtcbin
                    .clone();
                if let Some(bin) = webrtcbin {
                    apply_local_answer(&bin, &sdp);
                }
            }

            // ---- webrtcbin-shaped ICE dispatch (Phase 8.C step 4) ----
            //
            // Same shape as the SDP arms: clone the bin handle,
            // drop the `Inner` borrow, hand the parsed bits to
            // GStreamer. Parse via hotline-proto::voice::ice (the
            // same parser the C-side wire layer uses, so the JSON
            // shape definition lives in one place) and hand
            // webrtcbin a (mlineindex, candidate) pair through
            // its `add-ice-candidate` signal.
            Action::AddRemoteIce { candidate_json } => {
                let webrtcbin = self
                    .inner
                    .borrow()
                    .webrtcbin
                    .clone();
                if let Some(bin) = webrtcbin {
                    apply_remote_ice(&bin, &candidate_json);
                }
            }

            // ---- Receive-leg dispatch (Phase 8.C step 5) ----
            //
            // StartReceivePipeline: the state machine has
            // cross-referenced `mid` against its mid→user_id
            // table and decided this pad should produce audio.
            // Pop the pad out of `pending_pads` (parked by
            // `connect_pad_added`), build the
            // `rtppcmudepay ! mulawdec ! audioconvert !
            // audioresample ! autoaudiosink` chain via
            // `audio::make_receive_bin`, add it to the
            // pipeline, link the pad to its ghost sink, and
            // stash the bin in `receive_bins` keyed by mid.
            // `user_id` is captured by the state machine for
            // UI use (speaker indicator); the runtime doesn't
            // need it after the link is established.
            Action::StartReceivePipeline { mid, user_id: _ } => {
                // Pop the new pad first; the existing-bin
                // teardown only fires when we actually have a
                // pad to replace it with. Otherwise this would
                // orphan a still-playing receive leg on every
                // speculative StartReceivePipeline that the
                // state machine emits without a matching
                // pad-added event.
                let (pipeline, pad) = {
                    let mut inner = self.inner.borrow_mut();
                    (
                        inner.pipeline.clone(),
                        inner.pending_pads.remove(&mid),
                    )
                };
                let (Some(pipeline), Some(pad)) = (pipeline, pad) else {
                    // Pipeline-less runtime (test) or no pending
                    // pad for this mid (state machine drove past
                    // pad-added) — silent no-op. Don't touch
                    // receive_bins; the previously-linked bin
                    // for this mid (if any) stays installed.
                    return;
                };
                // We have a fresh pad. Tear down any pre-existing
                // receive bin for this mid before adding a new
                // one — renegotiation can bring a second
                // pad-added for the same mid (a recycled slot, a
                // re-offer that re-declares the same media line);
                // without this teardown, start_receive_bin's
                // `pipeline.add(&bin)` would fail on the
                // duplicate "hxvoice-recv-{mid}" element name and
                // the OLD bin would stay linked, wedging
                // playback.
                if let Some(existing) =
                    self.inner.borrow_mut().receive_bins.remove(&mid)
                {
                    stop_receive_bin(&pipeline, &existing);
                }
                let counter = {
                    let inner = self.inner.borrow();
                    Arc::clone(&inner.rtp_buffers_received)
                };
                // The per-uid speaker-activity counter is driven by
                // the `level` element's RMS bus messages
                // (`handle_level_message`), not from here, so this
                // path only needs the global wedge-watchdog counter.
                if let Some(bin) =
                    start_receive_bin(&pipeline, &pad, &mid, &counter)
                {
                    self.inner
                        .borrow_mut()
                        .receive_bins
                        .insert(mid, bin);
                }
            }

            // StopReceivePipeline: the matching mid's receive
            // leg goes away (participant left, mid recycled
            // through renegotiation). Pop the bin, set it to
            // Null, remove it from the pipeline so it drops.
            Action::StopReceivePipeline { mid } => {
                let (pipeline, bin) = {
                    let mut inner = self.inner.borrow_mut();
                    (
                        inner.pipeline.clone(),
                        inner.receive_bins.remove(&mid),
                    )
                };
                if let (Some(pipeline), Some(bin)) = (pipeline, bin) {
                    stop_receive_bin(&pipeline, &bin);
                }
            }

            // ---- Mute dispatch ----
            //
            // Apply the local mute to the send leg by toggling the
            // `volume` element's `mute` property — when set, the
            // captured microphone audio is replaced with digital
            // silence before it reaches the encoder, so a "muted"
            // user genuinely stops transmitting their mic (not just
            // server-side enforcement via the 606 flag). The element
            // is looked up by name; `Bin::by_name` recurses into the
            // send bin. If the pipeline or element is absent (e.g.
            // the pipeline-less test runtime, or a teardown race),
            // the action is a no-op — the next mute action after a
            // rebuild re-applies the state.
            Action::SetSendPipelineMute { muted } => {
                let pipeline = self.inner.borrow().pipeline.clone();
                if let Some(pipeline) = pipeline {
                    match pipeline.by_name(crate::audio::SEND_VOLUME_ELEMENT_NAME)
                    {
                        Some(volume) => {
                            volume.set_property("mute", muted);
                            crate::debug::log!(
                                "voice-pipe",
                                "send-leg mute set to {muted} via `{}`",
                                crate::audio::SEND_VOLUME_ELEMENT_NAME
                            );
                        }
                        None => {
                            gstreamer::warning!(
                                gstreamer::CAT_RUST,
                                "hxvoice: SetSendPipelineMute({muted}) could \
                                 not find the send `volume` element \
                                 `{}` — local mic mute not applied (the \
                                 server-side 606 flag still gates audio).",
                                crate::audio::SEND_VOLUME_ELEMENT_NAME
                            );
                        }
                    }
                }
            }

            // hxvoice::Action is #[non_exhaustive] — the wildcard
            // is required to compile, but silently dropping a new
            // variant would be a very subtle correctness bug. In
            // debug builds (including the test suite), trip a
            // debug_assert so a missing dispatch arm fails CI
            // loudly at the test that first walks the unhandled
            // variant. Release builds keep the no-op so a
            // partially-rolled-out client doesn't crash against a
            // newer hxvoice that emits an unknown action.
            other => {
                debug_assert!(
                    false,
                    "hxvoice::Action variant not handled by \
                     hxvoice_runtime::VoiceRuntime::dispatch: {other:?}"
                );
                let _ = other;
            }
        }
    }
}

/// Build a `WebRTCSessionDescription` from a raw SDP string and
/// a type tag. Returns `None` if the SDP can't be parsed.
///
/// Logging discipline: the **parser-error** path (a non-empty
/// buffer that `gst_sdp_message_parse_buffer` rejects) logs the
/// underlying message on the GStreamer warning channel before
/// returning `None`. The **empty-input** path early-returns
/// silently — the parser would otherwise trip a
/// `g_returns_if_fail(size != 0)` on the GStreamer-SDP
/// critical channel, and an empty buffer is unambiguously
/// "caller has nothing to parse" rather than a recoverable
/// shape worth surfacing. Callers should early-return on `None`
/// without re-logging.
///
/// gstreamer-rs's `SDPMessage::parse_buffer` is the typed entry
/// point: it parses the SDP into an owned `SDPMessage`, which we
/// then wrap in a `WebRTCSessionDescription` of the requested
/// type. Both calls are pure parsing — they don't touch the
/// pipeline or webrtcbin yet.
fn build_session_description(
    sdp: &str,
    type_: WebRTCSDPType,
) -> Option<gstreamer_webrtc::WebRTCSessionDescription> {
    // Pre-flight empty check. `gst_sdp_message_parse_buffer`
    // g_returns_if_fail's on size==0, which lights up the
    // GStreamer-SDP critical channel — caller intent is "we
    // couldn't parse it" either way, just less noisily.
    if sdp.is_empty() {
        return None;
    }
    let message = match gstreamer_sdp::SDPMessage::parse_buffer(sdp.as_bytes())
    {
        Ok(m) => m,
        Err(e) => {
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: failed to parse SDP ({} bytes): {e}",
                sdp.len()
            );
            return None;
        }
    };
    Some(gstreamer_webrtc::WebRTCSessionDescription::new(
        type_, message,
    ))
}

/// Dispatch arm for `Action::SetRemoteDescription`. Parses the
/// offer SDP, emits `webrtcbin.set-remote-description` with a
/// promise, and chains `create-answer` from the promise's
/// resolution.
///
/// The chain matters: webrtcbin's `set-remote-description` is
/// asynchronous, and `create-answer` requires remote desc to be
/// applied before it can produce a usable answer. Issuing them
/// back-to-back synchronously (the previous behaviour) left
/// webrtcbin without a real remote-desc when `create-answer`
/// fired, the promise either errored or returned an empty
/// structure, the state machine never saw
/// `Event::WebrtcAnswerCreated`, and the session stalled in
/// `OfferPending` until the JoinReply timer walked it to Failed.
///
/// Renegotiation race guard: `answer_generation` was bumped by
/// the dispatch arm before calling us, and the captured value is
/// checked inside `create_answer`'s promise too. If a fresh
/// `SetRemoteDescription` runs before this chain's `create-answer`
/// promise resolves, the generation mismatch drops the stale
/// answer.
fn apply_remote_offer_and_chain_answer(
    webrtcbin: &gstreamer::Element,
    sdp: &str,
    runtime_id: u64,
    generation: u64,
) {
    let Some(desc) = build_session_description(sdp, WebRTCSDPType::Offer)
    else {
        return;
    };
    let main_ctx = gstreamer::glib::MainContext::default();
    let promise = gstreamer::Promise::with_change_func(move |reply| {
        // `set-remote-description` populates the promise's reply
        // structure with at most a debug hint; we only care that
        // it resolved (whether Ok or Err). On Err the webrtcbin
        // state hasn't actually advanced — issuing create-answer
        // anyway would just produce the same stall as before.
        // Log and bail.
        if let Err(e) = reply {
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: set-remote-description promise resolved with \
                 error for runtime {runtime_id}: {e:?} — skipping chained \
                 create-answer; JoinReply timer will walk the session to \
                 Failed"
            );
            return;
        }
        // Marshal back to the main thread. The closure must be
        // `Send + 'static`; we capture only `runtime_id` and
        // `generation` (both `u64: Copy`) plus the main-context
        // handle. `create_answer` does its own generation re-
        // check inside its promise resolution.
        main_ctx.invoke(move || {
            with_main_thread_runtime(runtime_id, |rt| {
                if rt.inner.borrow().answer_generation != generation {
                    return;
                }
                let webrtcbin =
                    rt.inner.borrow().webrtcbin.clone();
                if let Some(bin) = webrtcbin {
                    create_answer(&bin, runtime_id, generation);
                }
            });
        });
    });
    webrtcbin.emit_by_name::<()>(
        "set-remote-description",
        &[&desc, &promise],
    );
}

/// Dispatch arm for `Action::SetLocalDescription`. Parses the
/// answer SDP we just received from `create-answer` and emits
/// `webrtcbin.set-local-description`. The state machine emits
/// the matching `SendWireFrame(603)` separately so the server
/// gets the answer.
fn apply_local_answer(webrtcbin: &gstreamer::Element, sdp: &str) {
    let Some(desc) = build_session_description(sdp, WebRTCSDPType::Answer)
    else {
        return;
    };
    webrtcbin.emit_by_name::<()>(
        "set-local-description",
        &[&desc, &None::<gstreamer::Promise>],
    );
}

/// Dispatch arm for `Action::CreateAnswer`. Emits
/// `webrtcbin.create-answer` with a `gst::Promise` whose change-func
/// fires (typically on a GStreamer worker thread) when the answer
/// is ready, marshals back to the GLib main context, looks up the
/// runtime by id from the thread-local registry, compares the
/// captured generation against the current `answer_generation` (so
/// resolutions from a superseded offer are silently dropped), and
/// re-enters `handle_event(Event::WebrtcAnswerCreated)` only if
/// the answer is for the still-current offer.
///
/// The closure is `Send + 'static` (captures only `runtime_id: u64`
/// and `generation: u64` and the main context); the runtime itself
/// stays `!Send`. See the module-level "Threading" section for the
/// full reasoning.
fn create_answer(
    webrtcbin: &gstreamer::Element,
    runtime_id: u64,
    generation: u64,
) {
    // Snapshot the main context up front so the closure carries
    // a `Send` handle into the worker thread without having to
    // re-acquire it there. `MainContext::default()` returns the
    // global default context — it's callable from any thread,
    // we just want the handle in scope before we move into the
    // promise's closure so the worker-side code stays linear.
    let main_ctx = gstreamer::glib::MainContext::default();
    let promise = gstreamer::Promise::with_change_func(move |reply| {
        let Ok(Some(reply)) = reply else {
            // create-answer either errored or completed with no
            // structure — both mean the state machine won't see
            // an answer event, the JoinReply / Dtls timers will
            // eventually fire, and the session walks to Failed.
            // Nothing to do here.
            return;
        };
        let Ok(desc) = reply
            .get::<gstreamer_webrtc::WebRTCSessionDescription>("answer")
        else {
            return;
        };
        // Serialize the answer SDP. `as_text()` returns
        // `Result<String, BoolError>` — failure here means
        // something went wrong in gstreamer-sdp's printer,
        // never an expected condition. Bailing out is the
        // right move: the state machine would otherwise see
        // an empty SDP string, emit a `SendWireFrame(603,
        // empty body)`, and ship an invalid 603 to the
        // server — the worst possible recovery. The JoinReply
        // / Dtls timers will fire and walk the session to
        // Failed naturally.
        let sdp = match desc.sdp().as_text() {
            Ok(s) => s,
            Err(e) => {
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: failed to serialize answer SDP for \
                     runtime {runtime_id}: {e} — letting the JoinReply \
                     timer drive the session to Failed"
                );
                return;
            }
        };
        // Marshal back to the main thread. The closure must be
        // `Send + 'static`; we capture only `runtime_id` (u64,
        // Copy), `generation` (u64, Copy), and `sdp` (String).
        // The runtime lookup happens on the main thread inside
        // `with_main_thread_runtime`; the generation check
        // happens there too because `answer_generation` lives
        // in `Inner` which is `!Send`.
        main_ctx.invoke(move || {
            with_main_thread_runtime(runtime_id, |rt| {
                // Renegotiation race guard: if a newer
                // CreateAnswer was issued while we were
                // waiting for this promise to resolve (the
                // OfferPending+SdpOfferReceived state-machine
                // arm bumps the generation without changing
                // state), the resolution we're holding is for
                // a now-stale offer. Drop it — the state
                // machine would otherwise emit
                // SendWireFrame(603) carrying the wrong
                // answer.
                if rt.inner.borrow().answer_generation != generation {
                    return;
                }
                rt.handle_event(Event::WebrtcAnswerCreated { sdp });
            });
        });
    });
    webrtcbin.emit_by_name::<()>(
        "create-answer",
        &[&None::<gstreamer::Structure>, &promise],
    );
}

/// Dispatch arm for `Action::AddRemoteIce`. Parses the JSON via
/// the shared hotline-proto::voice::ice parser, then emits
/// `webrtcbin.add-ice-candidate` with the extracted mlineindex +
/// candidate string.
///
/// Handles three malformed-input shapes without panicking:
///
/// 1. JSON that fails to parse → log + return; the state machine
///    will not see the candidate, the missing entry just means
///    one fewer path for ICE to converge on. ICE failure
///    timers (Phase 8.C step 7) drive the session to Failed
///    if no candidates ever land.
/// 2. End-of-candidates marker (empty `candidate` string) →
///    silent no-op at the bin level. webrtcbin doesn't need an
///    explicit EOC signal; gathering completes when the remote
///    stops sending. The state machine's
///    `EndOfRemoteCandidates` event is informational only.
/// 3. Missing `sdpMLineIndex` → log + return. The bin signal
///    requires the index; without it we'd have to guess, which
///    is worse than dropping the candidate.
fn apply_remote_ice(webrtcbin: &gstreamer::Element, candidate_json: &str) {
    let parsed = match hotline_proto::voice::ice::parse(
        candidate_json.as_bytes(),
    ) {
        Some(p) => p,
        None => {
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: failed to parse ICE candidate JSON \
                 ({} bytes); dropping",
                candidate_json.len()
            );
            return;
        }
    };
    if parsed.is_end_of_candidates() {
        // EOC marker — the state machine routes this through
        // EndOfRemoteCandidates instead of AddRemoteIce, but
        // we belt-and-brace here in case a path slips through.
        return;
    }
    let Some(candidate) = parsed.candidate.as_deref() else {
        // Parser guarantees `candidate` is Some on a successful
        // parse (it's a required field; absence is the
        // None-return above). Defensive double-check so the
        // compiler can use `Option<String>` properties on the
        // `IceCandidate` struct for other callsites too.
        return;
    };
    let Some(mline_index) = parsed.sdp_mline_index else {
        gstreamer::warning!(
            gstreamer::CAT_RUST,
            "hxvoice: ICE candidate JSON missing sdpMLineIndex; \
             dropping (webrtcbin requires it)"
        );
        return;
    };
    webrtcbin.emit_by_name::<()>(
        "add-ice-candidate",
        &[&mline_index, &candidate],
    );
}

/// Wire `webrtcbin.on-ice-candidate` so locally-gathered candidates
/// flow back into the state machine as `Event::WebrtcLocalIceGathered`.
///
/// The signal callback must be `Send + 'static` (webrtcbin may fire
/// it on a GStreamer worker thread); we follow the same shape as
/// `create_answer`'s promise — capture the runtime id (u64, Copy)
/// and the main context handle (Send), build the outgoing
/// `RTCIceCandidateInit` JSON in the callback, marshal back to the
/// main thread via `MainContext::invoke`, and re-enter
/// `handle_event` after a registry lookup.
///
/// The JSON carries all three of `candidate`, `sdpMid`, and
/// `sdpMLineIndex`. `candidate` and `sdpMLineIndex` come directly
/// from the signal payload; `sdpMid` isn't surfaced by the signal,
/// so we resolve it from the bin's `local-description` SDP by
/// looking up the `a=mid:…` attribute on the media line at the
/// signal's index. The fogWraith voice extension's wire format
/// treats `sdpMid` as a required key — `hotline_proto::voice::ice`
/// rejects payloads missing it — so shipping a candidate without
/// it would break trickle ICE downstream.
///
/// If the `sdpMid` lookup fails (local-description not yet set,
/// no `mid` attribute on the indexed media), the candidate is
/// dropped with a GStreamer warning. ICE failure timers (Phase
/// 8.C step 7) drive the session to Failed if no usable
/// candidates ever land.
///
/// Called from `VoiceRuntime::new` once the bin is in the pipeline,
/// so production gets ICE wiring for every fresh runtime
/// automatically. The pipeline-less constructor skips this — there's
/// no bin to attach to.
fn connect_on_ice_candidate(
    webrtcbin: &gstreamer::Element,
    runtime_id: u64,
) {
    let main_ctx = gstreamer::glib::MainContext::default();
    webrtcbin.connect("on-ice-candidate", false, move |values| {
        // The signal-emit returns no value (we return None at the
        // end), but the callback receives a slice of `glib::Value`s
        // — by signature `[webrtcbin, mlineindex: u32,
        // candidate: &str]`.
        let bin = match values
            .first()
            .and_then(|v| v.get::<gstreamer::Element>().ok())
        {
            Some(b) => b,
            None => return None,
        };
        let mline_index = match values.get(1).and_then(|v| v.get::<u32>().ok()) {
            Some(v) => v,
            None => return None,
        };
        let candidate = match values.get(2).and_then(|v| v.get::<String>().ok()) {
            Some(s) => s,
            None => return None,
        };
        // Resolve sdpMid by walking the bin's local-description.
        // Dropping the candidate when the lookup fails is the
        // correct move — voice::ice::parse on the wire side
        // rejects payloads missing sdpMid, so we'd just produce
        // a JSON the server can't accept.
        let sdp_mid = match lookup_local_sdp_mid(&bin, mline_index) {
            Some(mid) => mid,
            None => {
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: local-description doesn't have an \
                     a=mid:… for mline {mline_index}; dropping \
                     ICE candidate (would have produced JSON \
                     rejected by voice::ice::parse)"
                );
                return None;
            }
        };
        let candidate_json = hotline_proto::voice::ice::build(
            &hotline_proto::voice::ice::IceCandidate {
                candidate: Some(candidate),
                sdp_mid: Some(sdp_mid),
                sdp_mline_index: Some(mline_index),
                username_fragment: None,
            },
        );
        let main_ctx = main_ctx.clone();
        main_ctx.invoke(move || {
            with_main_thread_runtime(runtime_id, |rt| {
                rt.handle_event(Event::WebrtcLocalIceGathered {
                    candidate_json,
                });
            });
        });
        None
    });
}

/// Look up the `a=mid:…` attribute on the media line at the
/// given index in `webrtcbin`'s `local-description` SDP. Returns
/// `None` if local-description isn't set yet, the index is out of
/// range, or the indexed media has no `mid` attribute.
///
/// Pulled into its own helper so the on-ice-candidate callback
/// stays linear and the lookup logic can be reused by the
/// step-5 pad-added path when it lands.
fn lookup_local_sdp_mid(
    webrtcbin: &gstreamer::Element,
    mline_index: u32,
) -> Option<String> {
    let desc: gstreamer_webrtc::WebRTCSessionDescription = webrtcbin
        .property_value("local-description")
        .get()
        .ok()?;
    let sdp = desc.sdp();
    let media = sdp.media(mline_index)?;
    media.attribute_val("mid").map(|s| s.to_string())
}

/// Wire `webrtcbin.pad-added` so each freshly-arrived receive pad
/// becomes a `Event::WebrtcPadAdded { mid }` against the state
/// machine. The runtime parks the pad in `Inner::pending_pads` so
/// the matching `Action::StartReceivePipeline` arm can pop it back
/// out and link it to the receive-leg bin.
///
/// Skips non-`Src` pads — webrtcbin also exposes sink pads (request
/// pads from the send leg) and we shouldn't try to bind a receive
/// bin to those. The `mid` is resolved through the pad's
/// `transceiver` property's `mid`; a pad without a transceiver
/// (rare; data-channel pads don't have one) or one whose
/// transceiver doesn't have a `mid` yet is dropped with a warning.
///
/// Same `Send + 'static` callback shape as `connect_on_ice_candidate`:
/// captures only the runtime id + the main context handle, marshals
/// back to the GLib main thread via `MainContext::invoke`, looks
/// the runtime up by id in [`MAIN_THREAD_RUNTIMES`], and re-enters
/// `handle_event` after parking the pad.
fn connect_pad_added(
    webrtcbin: &gstreamer::Element,
    pipeline: &gstreamer::Pipeline,
    runtime_id: u64,
    rtp_buffers_received: Arc<AtomicU64>,
) {
    let main_ctx = gstreamer::glib::MainContext::default();
    let pipeline = pipeline.clone();
    webrtcbin.connect_pad_added(move |_bin, pad| {
        if pad.direction() != gstreamer::PadDirection::Src {
            // Sink pads (request pads from the send leg) come
            // through this signal too; nothing to do with them.
            return;
        }
        let mid = match lookup_pad_mid(pad) {
            Some(m) => m,
            None => {
                crate::debug::log!(
                    "voice-pipe",
                    "pad-added with no resolvable mid (pad={}); dropping",
                    pad.name()
                );
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: pad-added with no resolvable mid \
                     (pad={}); dropping",
                    pad.name()
                );
                return;
            }
        };
        // Log the resolved mid so we can see exactly what mid
        // webrtcbin assigned to the receive pad. If this doesn't
        // match what the state machine cached from the SDP
        // offer's a=mid: lines, StartReceivePipeline never fires
        // and audio never reaches the speakers — the
        // silent-failure that bit us in the multi-client test.
        crate::debug::log!(
            "voice-pipe",
            "pad-added pad={} mid={} dir={:?}",
            pad.name(),
            mid,
            pad.direction()
        );

        // SYNCHRONOUS receive-bin build + link, BEFORE we return
        // from this signal handler.
        //
        // This signal fires from webrtcbin's worker thread the
        // moment `on_rtpbin_pad_added` calls `gst_element_add_pad`
        // (gstwebrtcbin.c:7724). rtpbin starts pushing the first
        // buffer downstream into the bin's internal target
        // immediately after, on its streaming thread. If we
        // marshal to the main GLib loop to build the receive bin
        // and link the pad (the old path), there's a ~milliseconds-
        // to-tens-of-milliseconds gap during which buffers get
        // pushed to a pad with no peer, return GST_FLOW_NOT_LINKED,
        // and rtpjitterbuffer / rtpbin can interpret the consumer
        // as gone — leading to the "one buffer at webrtcbin src_0,
        // then silence" pattern we observed in voice.log /
        // voice-2.log even after codec-preferences + bundle-policy
        // were correct.
        //
        // gst::Pipeline::add, gst::Element::link, and
        // sync_state_with_parent are all thread-safe (GObject +
        // PADD lock); we don't need the main thread to do them.
        // The main-thread hop later is only for bookkeeping
        // (recording the bin in receive_bins so LeaveRequested
        // can tear it down).
        //
        // The per-uid speaker-activity counter is no longer driven
        // from here. It's fed by the `level` element's RMS bus
        // messages (`handle_level_message`), which run on the main
        // thread and own the `per_user_voice_activity` map directly,
        // so `start_receive_bin` only needs the global wedge-watchdog
        // counter on this path.
        let recv_bin =
            start_receive_bin(&pipeline, pad, &mid, &rtp_buffers_received);
        let pad = pad.clone();
        let main_ctx = main_ctx.clone();
        main_ctx.invoke(move || {
            with_main_thread_runtime(runtime_id, |rt| {
                // If the synchronous link succeeded, store the
                // bin in receive_bins so LeaveRequested's
                // teardown can find it later — BUT ONLY when the
                // state machine is in a state that will accept
                // `Event::WebrtcPadAdded` and own the bin's
                // lifecycle. Otherwise we'd leak the bin: e.g.
                // a late pad-added arriving after `Leaving` has
                // already torn down everything else won't reach
                // an arm that returns
                // `Action::StopReceivePipeline`, so without
                // this gate the bin would keep pulling RTP
                // forever until the entire session is dropped.
                //
                // Active states are the same three the
                // state machine's WebrtcPadAdded arm accepts:
                // OfferPending (renegotiation drain),
                // Connecting (between SDP answer and ICE
                // complete), Connected (steady-state). Anything
                // else (Idle, JoinSent, Leaving) — tear the
                // bin back down right here.
                if let Some(ref bin) = recv_bin {
                    let state = rt.state();
                    let active = matches!(
                        state,
                        SessionState::OfferPending
                            | SessionState::Connecting
                            | SessionState::Connected,
                    );
                    if active {
                        rt.inner
                            .borrow_mut()
                            .receive_bins
                            .insert(mid.clone(), bin.clone());
                    } else {
                        // Salvage the pipeline handle, then tear
                        // the bin down. The pipeline handle is
                        // cloned (GObject ref); `stop_receive_bin`
                        // calls `pipeline.remove(bin)` and sets
                        // the bin to Null, dropping our local
                        // reference releases the last refcount.
                        let pipeline =
                            rt.inner.borrow().pipeline.clone();
                        if let Some(pipeline) = pipeline {
                            stop_receive_bin(&pipeline, bin);
                        }
                    }
                }
                // Park the pad too, mostly for backwards-compat
                // with the existing pending_pads cleanup logic.
                // The state machine still emits
                // StartReceivePipeline for this mid, but the
                // dispatch handler's `pending_pads.remove`
                // returns the pad we put here — and the
                // existing-bin teardown branch will tear down the
                // bin we just linked, which we DON'T want. So we
                // skip parking the pad if the sync link
                // succeeded. The state machine's
                // StartReceivePipeline will hit the
                // "no pending pad" early return and leave our
                // pre-linked bin alone.
                if recv_bin.is_none() {
                    rt.inner
                        .borrow_mut()
                        .pending_pads
                        .insert(mid.clone(), pad);
                }
                rt.handle_event(Event::WebrtcPadAdded {
                    mid: mid.clone(),
                });
            });
            // The "did the state machine consume the parked pad?"
            // cleanup runs on the NEXT main-loop tick, not
            // inline. handle_event has a re-entrancy guard
            // (inner.dispatching == true): if a backend callback
            // is still draining the outer dispatch loop, the
            // WebrtcPadAdded event we just submitted is only
            // ENQUEUED, not processed. Inlining the
            // pending_pads.remove(&mid) below would yank the pad
            // before the outer dispatch loop gets to it, and the
            // eventual Action::StartReceivePipeline would find
            // nothing to link.
            //
            // Deferring to glib::idle_add_local lets the outer
            // loop unwind first. By the time idle runs, the
            // event has been dispatched and a successful
            // StartReceivePipeline has already migrated the pad
            // out of pending_pads into receive_bins. If the
            // entry is still in pending_pads at idle time, it
            // really is stale and we drop it.
            let mid = mid.clone();
            gstreamer::glib::idle_add_local_once(move || {
                with_main_thread_runtime(runtime_id, |rt| {
                    let stale =
                        rt.inner.borrow_mut().pending_pads.remove(&mid);
                    // A stale entry at idle time means the state
                    // machine didn't consume the pad — either it
                    // wasn't in a state that accepts
                    // `WebrtcPadAdded`, or the event was queued
                    // but dropped during a tear-down. The state
                    // machine no longer ignores any specific mid
                    // label (the old `mid == "send"` guard was
                    // removed once we observed Janus reusing
                    // `send` for forwarded receive mlines), so
                    // any stale parking is worth surfacing.
                    if stale.is_some() {
                        crate::debug::log!(
                            "voice-pipe",
                            "pad mid={mid} produced no \
                             StartReceivePipeline — state machine \
                             rejected WebrtcPadAdded in current state"
                        );
                        gstreamer::warning!(
                            gstreamer::CAT_RUST,
                            "hxvoice: pad-added for mid={mid} produced \
                             no StartReceivePipeline; dropping the \
                             parked pad"
                        );
                    }
                });
            });
        });
    });
}

/// Resolve the `mid` (SDP media identifier) for a webrtcbin pad.
///
/// webrtcbin tags every transceiver-backed pad with a
/// `transceiver` property pointing at the matching
/// `WebRTCRTPTransceiver`. The transceiver's `mid` property
/// matches the `a=mid:…` attribute in the SDP, which is what we
/// need to cross-reference against the state machine's
/// `mid_to_user` cache.
///
/// Returns `None` if the pad doesn't have a transceiver (data
/// channels) or the transceiver hasn't been assigned a `mid` yet
/// (the bin is still negotiating).
fn lookup_pad_mid(pad: &gstreamer::Pad) -> Option<String> {
    let trans: gstreamer_webrtc::WebRTCRTPTransceiver = pad
        .property_value("transceiver")
        .get()
        .ok()?;
    let mid: Option<String> =
        trans.property_value("mid").get().ok().flatten();
    mid
}

/// Wire `webrtcbin::on-new-transceiver` so we can set
/// `codec-preferences` on every transceiver the SDP processor
/// creates for us.
///
/// ## Why this matters
///
/// When Janus's initial SDP offer carries both `a=mid:send`
/// (our outgoing leg) and `a=mid:user-N` (audio forwarded from
/// another participant), webrtcbin's
/// `_create_and_associate_transceivers_from_sdp`
/// (gstwebrtcbin.c:6601) auto-creates a recvonly transceiver
/// for the user-N mline. The auto-created transceiver has no
/// `codec-preferences` set.
///
/// `_create_sdp_task` then runs immediately after to build the
/// answer SDP. For each transceiver it calls
/// `_find_codec_preferences` (gstwebrtcbin.c:2058). That
/// function reads `rtp_trans->codec_preferences` (the property
/// we're setting here). If unset, it tries to find a pad to
/// query caps from — but the receive-side pad doesn't exist
/// yet, since pad-added is gated on rtpbin demuxing data
/// (which can't happen until the answer is shipped and Janus
/// starts forwarding). So `_find_codec_preferences` returns
/// NULL and logs "Could not find caps for mline N".
///
/// The resulting answer SDP has no valid codec on that mline.
/// Janus parses our answer, sees the mline as effectively
/// rejected, and never forwards real audio for that user. No
/// audio → no rtpbin demux → no `_add_pad` → no pad-added
/// signal → no receive bin → no audio playback. The receive
/// leg dies silently.
///
/// The renegotiation path (when a participant joins AFTER we
/// joined an empty room) doesn't hit this because by the time
/// the user-N mline is added, the existing session already
/// has caps context wired up.
///
/// ## What this handler does
///
/// `on-new-transceiver` (gstwebrtcbin.c:9549) fires
/// synchronously from `_create_and_associate_transceivers_from_sdp`
/// with `PC_LOCK` released (lines 6607-6610). The signal is
/// emitted in the SAME synchronous task that goes on to call
/// `_find_codec_preferences`, so anything we set on the
/// transceiver is visible to the answer generation moments
/// later.
///
/// We set `codec-preferences` to PCMU 8 kHz mono — the only
/// codec Hotline voice supports. This applies to every
/// transceiver webrtcbin creates: the one from our
/// `request_pad_simple` (called once at startup; harmless to
/// also pin its preferences), the auto-created recvonly ones
/// (the actual fix target), and any explicit `add-transceiver`
/// calls.
///
/// The handler runs on whatever thread is processing the SDP
/// task (typically a webrtcbin internal worker). Setting a
/// GObject property is thread-safe. We don't hop to the main
/// thread because the answer-creation code path is
/// synchronous after this signal and would race with a
/// deferred property set.
fn connect_on_new_transceiver(webrtcbin: &gstreamer::Element) {
    // Build the caps once and hand a clone to the closure.
    let pcmu_caps = gstreamer::Caps::builder("application/x-rtp")
        .field("media", "audio")
        .field("encoding-name", "PCMU")
        .field("payload", 0i32)
        .field("clock-rate", 8000i32)
        .build();
    // Use `connect_closure` with the cross-thread `closure!`
    // macro from glib. We can't use `closure_local!` (the
    // single-thread variant) because the signal fires from
    // webrtcbin's internal SDP-task worker, NOT the main GLib
    // loop. `closure_local!` enforces same-thread access via
    // glib's ThreadGuard and panics if invoked off-thread.
    //
    // `closure!` requires captures to be Send + Sync. `gst::Caps`
    // is — it's a refcounted GObject — so the move is safe.
    use gstreamer::glib;
    webrtcbin.connect_closure(
        "on-new-transceiver",
        false,
        glib::closure!(
            move |_bin: &gstreamer::Element,
                  transceiver: &gstreamer_webrtc::WebRTCRTPTransceiver| {
                transceiver.set_property("codec-preferences", &pcmu_caps);
                let dir: gstreamer_webrtc::WebRTCRTPTransceiverDirection =
                    transceiver.property("direction");
                let mid: Option<String> = transceiver.property("mid");
                crate::debug::log!(
                    "voice-pipe",
                    "on-new-transceiver: pinned PCMU codec-preferences \
                     on transceiver direction={dir:?} mid={mid:?}"
                );
            }
        ),
    );
}

/// Wire `webrtcbin.notify::connection-state` so peer-connection-state
/// transitions flow into the state machine as
/// `Event::WebrtcConnectionStateChanged`.
///
/// Uses `connect_notify` (NOT the `_local` variant): notify signals
/// fire on the thread that called the property setter, which for
/// webrtcbin's `connection-state` is typically a GStreamer worker.
/// The Send closure captures only the runtime id + main context
/// handle (both `Send`) and marshals the actual `handle_event` call
/// back to the GLib main thread via `MainContext::invoke`, same
/// shape as `connect_pad_added` and `connect_on_ice_candidate`.
///
/// The `Send + 'static` callback is satisfied because we capture
/// only the `runtime_id` (`u64`, `Copy`) and the main context
/// handle; the per-fire `peer-connection-state` lookup reads from
/// the bin handed back through the signal args.
fn connect_connection_state_notify(
    webrtcbin: &gstreamer::Element,
    runtime_id: u64,
) {
    let main_ctx = gstreamer::glib::MainContext::default();
    webrtcbin.connect_notify(
        Some("connection-state"),
        move |bin, _pspec| {
            let state: gstreamer_webrtc::WebRTCPeerConnectionState =
                bin.property("connection-state");
            let mapped = map_peer_connection_state(state);
            let main_ctx = main_ctx.clone();
            main_ctx.invoke(move || {
                with_main_thread_runtime(runtime_id, |rt| {
                    // Mirror the post-translate state onto Inner
                    // BEFORE firing the event. The state machine's
                    // step() runs synchronously inside handle_event,
                    // and the post-step state diff (in handle_event)
                    // reads `last_seen_peer_state` to decide whether
                    // to synthesize a Connected event for a stuck
                    // renegotiation. Updating before keeps the post-
                    // step diff strictly post-mortem on the latest
                    // notify.
                    rt.inner.borrow_mut().last_seen_peer_state = Some(mapped);
                    rt.handle_event(
                        Event::WebrtcConnectionStateChanged { state: mapped },
                    );
                });
            });
        },
    );
}

/// Translate a `gstreamer_webrtc::WebRTCPeerConnectionState` into the
/// `hxvoice::event::ConnectionState` the state machine understands.
///
/// The fogWraith voice extension's state machine only cares about the
/// four peer-connection-state values that change observable
/// behaviour. `New` and `Closed` map to `Closed` because the state
/// machine treats both as "not connected, not transitioning" — the
/// runtime is either pre-join (New) or terminal (Closed); neither
/// triggers a state-machine transition. Anything unrecognised is
/// `Disconnected` as a safe default that doesn't trigger fail().
fn map_peer_connection_state(
    state: gstreamer_webrtc::WebRTCPeerConnectionState,
) -> hxvoice::event::ConnectionState {
    use gstreamer_webrtc::WebRTCPeerConnectionState as Src;
    use hxvoice::event::ConnectionState as Dst;
    match state {
        Src::New | Src::Closed => Dst::Closed,
        Src::Connecting => Dst::Connecting,
        Src::Connected => Dst::Connected,
        Src::Disconnected => Dst::Disconnected,
        Src::Failed => Dst::Failed,
        _ => Dst::Disconnected,
    }
}

/// RMS power (dB, relative to full scale) at or above which a
/// receive leg is considered "actively speaking" for the user-list
/// indicator.
///
/// The `level` element reports per-window RMS as a non-positive
/// decibel value: 0 dB is full scale, `-inf` is digital silence,
/// and normal speech on an open mic lands roughly in the -30 to
/// -45 dB range with the noise floor below -55 dB. -50 dB sits in
/// the gap — high enough to reject ambient hiss / line noise, low
/// enough to catch quiet talkers. It's within the -45..-50 dB band
/// the voice-chat plan (§12 step 4a) scoped, biased to the
/// sensitive end so we don't drop soft speech. Tunable in one place
/// if real-world rooms want it tighter.
const SPEAKING_RMS_THRESHOLD_DB: f64 = -50.0;

/// Name prefix every receive bin carries (`hxvoice-recv-<mid>`).
/// Shared between [`start_receive_bin`], which builds the name, and
/// [`uid_from_recv_bin_name`], which parses the uid back out of a
/// `level` message's source element parent — so the two can't drift.
const RECV_BIN_PREFIX: &str = "hxvoice-recv-";

/// `true` when an RMS reading clears the speaking threshold. Pure
/// so the threshold decision is unit-testable without a pipeline.
/// `f64::NEG_INFINITY` (the `level` element's silence sentinel)
/// compares below every finite threshold, so digital silence is
/// correctly "not speaking".
fn rms_db_indicates_speaking(db: f64) -> bool {
    db >= SPEAKING_RMS_THRESHOLD_DB
}

/// Recover the Hotline user id from a receive bin's element name.
///
/// Receive bins are named `hxvoice-recv-<mid>` where `<mid>` is the
/// SDP mid label (`user-<uid>` for a remote leg, `send` for the
/// local send leg). Returns `Some(uid)` only for `user-<uid>`
/// labels; the send leg and any unparseable name yield `None`.
fn uid_from_recv_bin_name(name: &str) -> Option<u16> {
    let mid = name.strip_prefix(RECV_BIN_PREFIX)?;
    match hotline_proto::voice::parse_voice_mid_label(mid.as_bytes()) {
        Some(hotline_proto::voice::MidLabel::User(uid)) => Some(uid),
        _ => None,
    }
}

/// Pull the loudest per-channel RMS (dB) out of a `level` element
/// message structure. The `rms` field is a `glib::ValueArray` of
/// one `f64` per channel; voice is mono so there's normally a
/// single entry, but we take the max defensively in case a future
/// pipeline carries stereo. Returns `None` if the field is absent
/// or empty.
fn level_message_max_rms_db(s: &gstreamer::StructureRef) -> Option<f64> {
    let rms = s.get::<gstreamer::glib::ValueArray>("rms").ok()?;
    let mut max: Option<f64> = None;
    for v in rms.iter() {
        if let Ok(db) = v.get::<f64>() {
            max = Some(max.map_or(db, |m| m.max(db)));
        }
    }
    max
}

/// Handle one `"level"` element bus message: map the posting
/// element back to its receive bin's uid, threshold the RMS, and
/// bump that uid's voice-activity counter when it clears the
/// threshold. The periodic [`speaker_tick`] turns those bumps into
/// `SignalKind::SpeakerChanged` flips with a ~one-tick hangover.
///
/// Runs on the main thread (the bus watch is `add_watch_local`), so
/// it reaches the runtime via `with_main_thread_runtime` and mutates
/// the activity map under its mutex directly. Below-threshold
/// windows are simply not counted — a uid that goes quiet stops
/// advancing and `speaker_tick` flips it back to not-speaking.
fn handle_level_message(
    runtime_id: u64,
    src: Option<&gstreamer::Object>,
    s: &gstreamer::StructureRef,
) {
    // The message source is the `level` element; its parent is the
    // receive bin whose name encodes the uid.
    let Some(uid) = src
        .and_then(|o| o.parent())
        .and_then(|bin| uid_from_recv_bin_name(bin.name().as_str()))
    else {
        return;
    };
    let Some(rms_db) = level_message_max_rms_db(s) else {
        return;
    };
    if !rms_db_indicates_speaking(rms_db) {
        return;
    }
    with_main_thread_runtime(runtime_id, |rt| {
        let map_arc = rt.inner.borrow().per_user_voice_activity.clone();
        let Ok(mut guard) = map_arc.lock() else {
            return;
        };
        guard
            .entry(uid)
            .or_insert_with(|| Arc::new(AtomicU64::new(0)))
            .fetch_add(1, Ordering::Relaxed);
    });
}

/// Attach a main-loop bus watch to the pipeline so GStreamer
/// errors and warnings surface in our logs with element context.
/// Doesn't (yet) translate bus errors into state-machine events
/// — most pipeline-level errors are fatal regardless, and the
/// matching connection-state transitions arrive through
/// `connect_connection_state_notify` above; this watch is for
/// triage logging only.
///
/// `add_watch_local` is main-thread-only; that's what we want
/// (the runtime runs there). Returns the `BusWatchGuard` so the
/// caller (the runtime constructor) can park it in `Inner`. The
/// guard drops the watch when it goes out of scope, so dropping
/// it inline here would yank the watch right back off the bus.
/// Returns `None` if the bus is unreachable or the default
/// context can't be acquired.
fn attach_pipeline_bus_watch(
    pipeline: &gstreamer::Pipeline,
    runtime_id: u64,
) -> Option<gstreamer::bus::BusWatchGuard> {
    let bus = pipeline.bus()?;
    // `add_watch_local` attaches a source to the default
    // `MainContext` — same ownership requirement as
    // `glib::timeout_add_local` in `arm_timer`. In production
    // the main thread owns the context and this is fine; in
    // cargo's parallel test runner multiple threads race for
    // it. Defensively try-acquire, fall back to no-watch on
    // contention; the loopback/parser tests that don't depend
    // on bus messages don't care, and the Tier 3 voice tests
    // run with a real main loop that owns the context outright.
    //
    // The acquire guard MUST stay live across the
    // `add_watch_local` call — `ctx.acquire().is_err()` would
    // drop the guard at the end of the boolean expression,
    // releasing the context before we attach. Bind it to a
    // named local instead (mirrors the same fix `arm_timer`
    // applied).
    let ctx = gstreamer::glib::MainContext::default();
    let _acquire_guard = if ctx.is_owner() {
        None
    } else {
        match ctx.acquire() {
            Ok(g) => Some(g),
            Err(_) => return None,
        }
    };
    let watch = bus
        .add_watch_local(move |_bus, msg| {
            use gstreamer::MessageView;
            match msg.view() {
                MessageView::Error(err) => {
                    let src = err
                        .src()
                        .map(|o| o.path_string().to_string())
                        .unwrap_or_else(|| "<unknown>".into());
                    gstreamer::warning!(
                        gstreamer::CAT_RUST,
                        "hxvoice: pipeline error from {src}: {} ({:?})",
                        err.error(),
                        err.debug()
                    );
                }
                MessageView::Warning(w) => {
                    let src = w
                        .src()
                        .map(|o| o.path_string().to_string())
                        .unwrap_or_else(|| "<unknown>".into());
                    gstreamer::warning!(
                        gstreamer::CAT_RUST,
                        "hxvoice: pipeline warning from {src}: {} ({:?})",
                        w.error(),
                        w.debug()
                    );
                }
                MessageView::Element(e) => {
                    // The `level` element in each receive bin posts a
                    // `"level"` element message per RMS window. Route
                    // it to the per-uid voice-activity counter that
                    // drives the speaker indicator.
                    if let Some(s) = e.structure() {
                        if s.name() == "level" {
                            handle_level_message(runtime_id, e.src(), s);
                        }
                    }
                }
                _ => {}
            }
            gstreamer::glib::ControlFlow::Continue
        })
        .ok()?;
    Some(watch)
}

/// Build the receive bin for `mid`, add it to the pipeline, link
/// the supplied webrtcbin source pad to its ghost sink, and sync
/// state with the parent so audio flows immediately.
///
/// Returns the linked bin on success so the dispatch arm can park
/// it in `Inner::receive_bins`. On any failure, the bin is removed
/// from the pipeline (if it got that far) and `None` is returned —
/// the session keeps running, the user just doesn't hear this one
/// remote. Each failure mode logs on the GStreamer warning channel
/// with the mid for triage.
fn start_receive_bin(
    pipeline: &gstreamer::Pipeline,
    src_pad: &gstreamer::Pad,
    mid: &str,
    rtp_buffers_received: &Arc<AtomicU64>,
) -> Option<gstreamer::Bin> {
    // Diagnostic: probe webrtcbin's src_0 BEFORE we link the
    // depay bin to it. If this probe never fires while the
    // downstream depay.sink probe in audio.rs is also silent,
    // webrtcbin isn't pushing data and the failure is in
    // webrtcbin / rtpbin / jitterbuffer (upstream of us). If
    // this probe fires but depay.sink doesn't, the link or
    // sync_state_with_parent is the problem.
    //
    // Gated on `voice-flow` so the per-RTP-packet counter
    // increment doesn't run in production.
    if crate::debug::category_enabled("voice-flow") {
        let counter = std::sync::atomic::AtomicU64::new(0);
        let mid_owned = mid.to_string();
        src_pad.add_probe(gstreamer::PadProbeType::BUFFER, move |_pad, _info| {
            let n = counter
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed)
                + 1;
            if n == 1 || n % 50 == 0 {
                crate::debug::log!(
                    "voice-flow",
                    "webrtcbin src_0 (mid={mid_owned}): buffer #{n}"
                );
            }
            gstreamer::PadProbeReturn::Ok
        });
    }
    let bin_name = format!("{RECV_BIN_PREFIX}{mid}");
    let output_device = crate::audio::output_device();
    let bin = match crate::audio::make_receive_bin(
        &bin_name,
        output_device.as_deref(),
    ) {
        Some(b) => b,
        None => {
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: failed to construct receive bin for mid={mid} \
                 — check that gst-plugins-good (rtppcmudepay, mulawdec, \
                 autoaudiosink) and gst-plugins-base (audioconvert, \
                 audioresample) are installed"
            );
            return None;
        }
    };
    if pipeline.add(&bin).is_err() {
        gstreamer::warning!(
            gstreamer::CAT_RUST,
            "hxvoice: failed to add receive bin to pipeline (mid={mid})"
        );
        return None;
    }
    let sink_pad = match bin.static_pad("sink") {
        Some(p) => p,
        None => {
            gstreamer::warning!(
                gstreamer::CAT_RUST,
                "hxvoice: receive bin has no ghost sink pad (mid={mid})"
            );
            let _ = pipeline.remove(&bin);
            return None;
        }
    };
    if src_pad.link(&sink_pad).is_err() {
        gstreamer::warning!(
            gstreamer::CAT_RUST,
            "hxvoice: failed to link webrtcbin src pad to receive bin sink \
             (mid={mid})"
        );
        let _ = pipeline.remove(&bin);
        return None;
    }
    // Always-on liveness probe — increments the runtime's
    // `rtp_buffers_received` Arc on every buffer that reaches
    // the depay's sink (i.e. the first place inside our receive
    // bin that sees RTP after webrtcbin's `src_0` hands a packet
    // off). The wedge watchdog reads this counter as its "is
    // media actually flowing?" signal, distinguishing genuinely-
    // stuck sessions from ones where webrtcbin never reported
    // `Connected` because of the
    // `_collate_peer_connection_states: Undefined situation`
    // FIXME but RTP is still arriving fine.
    //
    // Unlike the gated diagnostic probes in audio.rs, this one
    // runs in production. The per-buffer cost is a single
    // relaxed atomic add — cheaper than the modulo+log probes,
    // and load-bearing rather than diagnostic. Attached after
    // the link so we're observing the path the buffers
    // actually flow through.
    let counter = Arc::clone(rtp_buffers_received);
    sink_pad.add_probe(gstreamer::PadProbeType::BUFFER, move |_pad, _info| {
        counter.fetch_add(1, Ordering::Relaxed);
        gstreamer::PadProbeReturn::Ok
    });
    if bin.sync_state_with_parent().is_err() {
        // Not fatal — the pipeline state machine will retry on
        // its own state-change pass. Log so an operator can see
        // it if audio doesn't materialize.
        crate::debug::log!(
            "voice-pipe",
            "sync_state_with_parent FAILED for receive bin mid={mid}"
        );
        gstreamer::warning!(
            gstreamer::CAT_RUST,
            "hxvoice: sync_state_with_parent failed on receive bin \
             (mid={mid}); audio may not flow until next state change"
        );
    } else {
        // Success log: if you see this and still no audio, the
        // failure is downstream — autoaudiosink can't open a
        // device, or PulseAudio is rejecting the connection. If
        // you DON'T see this on a remote join, the path from
        // pad-added to here is broken.
        crate::debug::log!(
            "voice-pipe",
            "receive bin LINKED for mid={mid} — audio should be flowing"
        );
    }
    Some(bin)
}

/// Tear down a receive bin: unlink the ghost sink from its peer
/// (the webrtcbin src pad), set to `Null`, remove from the
/// pipeline. The caller has already popped the bin handle from
/// `Inner::receive_bins`, so dropping the local reference on
/// return is what releases the last refcount.
///
/// The unlink step matters: `pipeline.remove` doesn't break pad
/// links — the webrtcbin src pad keeps the ghost sink alive via a
/// peer ref. A subsequent `StartReceivePipeline` for the same mid
/// would then try to link the (still-linked) src pad and fail with
/// "already linked", leaving the new bin orphaned in the pipeline.
fn stop_receive_bin(
    pipeline: &gstreamer::Pipeline,
    bin: &gstreamer::Bin,
) {
    if let Some(sink_pad) = bin.static_pad("sink") {
        if let Some(peer) = sink_pad.peer() {
            // Unlink the src→sink direction. The src pad's peer
            // ref is what kept the link alive across remove().
            let _ = peer.unlink(&sink_pad);
        }
    }
    let _ = bin.set_state(gstreamer::State::Null);
    let _ = pipeline.remove(bin);
}

/// Helper for the `Action::ArmTimer` dispatch arm — cancels any
/// existing timer of the same kind, then arms a fresh one-shot
/// `glib::timeout_add_local` for `ms` milliseconds whose callback
/// fires `Event::Timeout { kind }` against the state machine.
///
/// Captures `runtime_id` (u64, Copy) and `kind` (Timeout, Copy)
/// in the callback closure — no Rc clones, so the callback
/// doesn't artificially extend the runtime's lifetime past its
/// last user-facing strong reference.
///
/// Inside the callback we pop our own bookkeeping entry before
/// firing `handle_event` (the source is auto-removed by glib
/// because we return `ControlFlow::Break`; popping just keeps
/// our `armed_timer_sources` map honest). The state machine may
/// emit a fresh `Action::ArmTimer` for the same kind in
/// response to the `Event::Timeout`; that lands as a clean
/// insert after our pop.
fn arm_timer(runtime: &VoiceRuntime, kind: Timeout, ms: u32) {
    // Cancel any existing timer of this kind first — the
    // re-arm semantics on `ArmTimer` are "restart the
    // watchdog from now", not "no-op if already armed".
    let prev = runtime
        .inner
        .borrow_mut()
        .armed_timer_sources
        .remove(&kind);
    if let Some(Some(s)) = prev {
        s.remove();
    }

    let runtime_id = runtime.inner.borrow().runtime_id;
    // `glib::timeout_add_local` panics if the GLib default
    // `MainContext` isn't owned by the current thread. In
    // production the main thread owns it (acquired in `main`
    // by `gtk_application_run`). In cargo's parallel test
    // runner, multiple test threads race for ownership; the
    // losers can't arm real timers. We try-acquire first; on
    // failure we degrade to bookkeeping-only — the kind is
    // still tracked in `armed_timer_sources` (as `None`), so
    // `armed_timers()` returns the right thing for the
    // dispatch-loop tests that just check what was requested.
    //
    // The real timer-firing path is exercised by the
    // `tests/timer_firing.rs` integration test, which runs in
    // its own process and owns the context outright.
    let ctx = gstreamer::glib::MainContext::default();
    // If we don't already own the context, acquire it and hold
    // the guard across the timeout_add_local call. Naive
    // `ctx.acquire().is_ok()` drops the guard at the end of the
    // expression — between that drop and timeout_add_local
    // running, another thread can grab ownership and we'd panic
    // inside the timeout_add_local internal `assert!(is_owner)`.
    // Binding the guard to a named local keeps it alive through
    // the whole arming sequence; the guard drops at the end of
    // the if-let, releasing ownership cleanly.
    let _acquire_guard = if ctx.is_owner() {
        None
    } else {
        match ctx.acquire() {
            Ok(g) => Some(g),
            // Some other thread holds the context — degrade to
            // bookkeeping-only. The kind is tracked in
            // armed_timer_sources as None; the real timer-firing
            // path runs in `tests/timer_firing.rs` which has the
            // process to itself.
            Err(_) => {
                runtime
                    .inner
                    .borrow_mut()
                    .armed_timer_sources
                    .insert(kind, None);
                return;
            }
        }
    };
    let source_id = Some(gstreamer::glib::timeout_add_local(
        core::time::Duration::from_millis(ms as u64),
        move || {
            with_main_thread_runtime(runtime_id, |rt| {
                // Drop our bookkeeping entry first. The source
                // will be auto-removed by glib when we return
                // Break below; we just want the SourceId out of
                // our map so any subsequent CancelTimer for the
                // same kind doesn't try to `remove` an already-
                // gone source.
                rt.inner.borrow_mut().armed_timer_sources.remove(&kind);
                // Now fire the event. The state machine may
                // emit fresh ArmTimers in response; those land
                // through the normal dispatch path.
                rt.handle_event(Event::Timeout { kind });
            });
            gstreamer::glib::ControlFlow::Break
        },
    ));
    runtime
        .inner
        .borrow_mut()
        .armed_timer_sources
        .insert(kind, source_id);
}

/// Wedge-watchdog one-shot window in milliseconds. Each tick the
/// runtime samples [`Inner::rtp_buffers_received`]; if it hasn't
/// changed since the previous tick, the runtime feeds the state
/// machine `Timeout::WedgeDeadline` and lets `fail()` tear the
/// session down. If it HAS changed, the runtime updates the
/// snapshot and rearms — sessions where webrtcbin never reports
/// `Connected` but RTP is flowing fine stay alive indefinitely.
///
/// 60 seconds is roughly double the spec's `Media` watchdog
/// (30 s, see `hxvoice::event::Timeout::Media`). That deliberate
/// extra slack is the whole point of the wedge layer: the spec's
/// ICE-connectivity, DTLS, and Media watchdogs each have their
/// own 10–30 s budget and either still fire as fatal in their
/// respective states (Media) or got softened to non-fatal toasts
/// (IceConnectivity, Dtls) because of the webrtcbin
/// `_collate_peer_connection_states: Undefined situation` FIXME.
/// The wedge watchdog is the last-ditch safety net for the soft
/// cases — we want it long enough that real sessions whose
/// negotiation just took longer than the soft timers aren't
/// torn down (~30 s of slack on top), but short enough that a
/// truly stuck session doesn't sit in `Connecting` forever.
const WEDGE_WATCHDOG_INTERVAL_MS: u32 = 60_000;

/// Arm the wedge watchdog timer. Called when the state machine
/// transitions into `Connecting`.
///
/// Snapshots [`Inner::rtp_buffers_received`] into
/// [`Inner::wedge_watchdog_last_snapshot`] so the next tick can
/// compare apples to apples. Cancels any previously-armed
/// watchdog first so a state-change burst that flips Connecting
/// off and back on doesn't end up with two scheduled checks
/// running against the same counter.
///
/// Same MainContext-acquire pattern as `arm_timer` — try-acquire
/// for the cargo-parallel-tests fallback, store `None` in
/// `wedge_watchdog_source` to mark "armed but no real glib
/// source" so test introspection still sees the watchdog as
/// active.
fn arm_wedge_watchdog(runtime: &VoiceRuntime) {
    {
        let mut inner = runtime.inner.borrow_mut();
        if let Some(s) = inner.wedge_watchdog_source.take() {
            s.remove();
        }
        inner.wedge_watchdog_last_snapshot =
            inner.rtp_buffers_received.load(Ordering::Relaxed);
        inner.wedge_watchdog_armed_flag = true;
    }

    let runtime_id = runtime.inner.borrow().runtime_id;
    let ctx = gstreamer::glib::MainContext::default();
    let _acquire_guard = if ctx.is_owner() {
        None
    } else {
        match ctx.acquire() {
            Ok(g) => Some(g),
            Err(_) => {
                // Test fallback: bookkeeping only. The real
                // wedge-fires test runs in tests/wedge_watchdog.rs
                // which owns the process default-context outright.
                runtime.inner.borrow_mut().wedge_watchdog_source = None;
                return;
            }
        }
    };
    let source_id = gstreamer::glib::timeout_add_local(
        core::time::Duration::from_millis(WEDGE_WATCHDOG_INTERVAL_MS as u64),
        move || {
            with_main_thread_runtime(runtime_id, |rt| {
                wedge_watchdog_tick(rt);
            });
            // We rearm explicitly inside the callback (with a
            // fresh snapshot), so this one-shot returns Break.
            gstreamer::glib::ControlFlow::Break
        },
    );
    runtime.inner.borrow_mut().wedge_watchdog_source = Some(source_id);
}

/// Cancel the wedge watchdog, if armed. Called when the state
/// machine leaves `Connecting` (whether to `Connected`,
/// `Leaving`, or `Idle`).
fn cancel_wedge_watchdog(runtime: &VoiceRuntime) {
    let prev = {
        let mut inner = runtime.inner.borrow_mut();
        inner.wedge_watchdog_armed_flag = false;
        inner.wedge_watchdog_source.take()
    };
    if let Some(s) = prev {
        s.remove();
    }
}

/// Wedge-watchdog tick callback. Read the live RTP counter,
/// compare against the snapshot at arm time:
///
/// - If counter advanced: RTP is flowing despite webrtcbin not
///   reporting `Connected`. This is the soft-stuck case we
///   deliberately don't want to tear down. Update the snapshot
///   and rearm for another window.
/// - If counter unchanged: no RTP arrived in the entire window
///   while we're still in `Connecting`. This is the genuinely-
///   wedged case. Inject `Timeout::WedgeDeadline` into the state
///   machine — its `(Connecting, WedgeDeadline)` arm calls
///   `fail()` and tears the session down.
///
/// Also bails (without rearming) if the state machine already
/// left `Connecting` since the arm — the `handle_event` machinery
/// is meant to cancel us via `cancel_wedge_watchdog` in that
/// case, so reaching the tick from a non-Connecting state means
/// a race between cancel and fire that we treat as "cancel
/// won."
fn wedge_watchdog_tick(runtime: &VoiceRuntime) {
    let (state, current, snapshot) = {
        let inner = runtime.inner.borrow();
        (
            inner.machine.state(),
            inner.rtp_buffers_received.load(Ordering::Relaxed),
            inner.wedge_watchdog_last_snapshot,
        )
    };
    // Already removed by glib when the one-shot fired; nothing
    // to remove here. Clear our bookkeeping entry so a stale
    // SourceId doesn't linger.
    runtime.inner.borrow_mut().wedge_watchdog_source = None;

    if state != SessionState::Connecting {
        return;
    }
    if current > snapshot {
        // Audio is flowing — keep the session alive and rearm.
        arm_wedge_watchdog(runtime);
        return;
    }
    // No RTP in the window. Inject the timeout; state.rs's
    // `(Connecting, WedgeDeadline)` arm calls `fail()`.
    runtime.handle_event(Event::Timeout {
        kind: Timeout::WedgeDeadline,
    });
}

/// Tick interval for the voice-activity evaluator.
/// 200 ms is the same cadence the Phase 8 plan called out: short
/// enough that the speaker indicator feels live (Discord's
/// pulse-on-speak runs at ~100 ms, Zoom around 150 ms), long
/// enough that the per-tick overhead — one HashMap snapshot, one
/// HashMap diff, at most a couple of signal callbacks — is
/// negligible. It's also a clean multiple of the `level` element's
/// 100 ms RMS window ([`crate::audio::LEVEL_MESSAGE_INTERVAL_NS`]),
/// so each tick sees at least one (usually two) fresh RMS messages
/// per uid and the resulting one-tick hangover smooths over the
/// short gaps between words within a single utterance.
const SPEAKER_EVAL_INTERVAL_MS: u32 = 200;

/// Arm the periodic speaker-activity evaluator. Idempotent.
///
/// The timer is a recurring `glib::timeout_add_local` rather than
/// a one-shot. Re-entry is safe: the existing source (if any) is
/// removed before scheduling a new one.
///
/// MainContext-acquire fallback: if some other thread owns the
/// default context (the cargo-parallel test runner's loser-of-
/// the-race case), `arm_speaker_timer` leaves `speaker_timer_source`
/// as `None` and returns without scheduling anything. No glib
/// source is attached, no production ticks fire, and
/// `speaker_timer_armed()` reads `false`. Tests that exercise the
/// evaluator drive ticks manually via `speaker_tick_for_test` and
/// don't need the timer to be live.
fn arm_speaker_timer(runtime: &VoiceRuntime) {
    {
        let mut inner = runtime.inner.borrow_mut();
        if let Some(s) = inner.speaker_timer_source.take() {
            s.remove();
        }
    }

    let runtime_id = runtime.inner.borrow().runtime_id;
    let ctx = gstreamer::glib::MainContext::default();
    let _acquire_guard = if ctx.is_owner() {
        None
    } else {
        match ctx.acquire() {
            Ok(g) => Some(g),
            Err(_) => {
                runtime.inner.borrow_mut().speaker_timer_source = None;
                return;
            }
        }
    };
    let source_id = gstreamer::glib::timeout_add_local(
        core::time::Duration::from_millis(
            SPEAKER_EVAL_INTERVAL_MS as u64,
        ),
        move || {
            with_main_thread_runtime(runtime_id, |rt| {
                speaker_tick(rt);
            });
            gstreamer::glib::ControlFlow::Continue
        },
    );
    runtime.inner.borrow_mut().speaker_timer_source = Some(source_id);
}

/// Stop the speaker-activity evaluator. Called from
/// `VoiceRuntime::drop` so the timer doesn't fire against a freed
/// runtime; otherwise the evaluator runs for the runtime's whole
/// lifetime (including across Idle / JoinSent gaps — the per-pad
/// counters live across rejoins).
#[allow(dead_code)]
fn cancel_speaker_timer(runtime: &VoiceRuntime) {
    let prev = runtime.inner.borrow_mut().speaker_timer_source.take();
    if let Some(s) = prev {
        s.remove();
    }
}

/// Per-tick evaluator: snapshot `per_user_voice_activity`, diff
/// against the prev snapshot, transition each uid to speaking /
/// silent, emit `SignalKind::SpeakerChanged` for any uid whose
/// state flipped.
///
/// A uid whose counter advanced since the previous tick had at
/// least one above-threshold RMS window in this interval, so it
/// reads as speaking; an unchanged counter reads as silent. The
/// counter is bumped by [`handle_level_message`] off the `level`
/// element's RMS, so this is true voice activity, not mere RTP
/// arrival.
///
/// Why a separate `prev` map rather than just clearing the live
/// counters: both the writer ([`handle_level_message`]) and this
/// reader run on the main thread, so a lost bump isn't the worry —
/// keeping a frozen prev snapshot avoids mutating the shared map
/// here at all, so the brief lock is read-only and the diff math
/// stays trivially correct.
fn speaker_tick(runtime: &VoiceRuntime) {
    // Snapshot the live counter values + collect the uid list
    // under the mutex, then drop the lock before the diff /
    // emit pass. This keeps the lock window microscopic and
    // avoids holding it across the `backend.borrow_mut()` below.
    let snapshot: Vec<(u16, u64)> = {
        let map_arc = runtime.inner.borrow().per_user_voice_activity.clone();
        let Ok(guard) = map_arc.lock() else {
            return;
        };
        guard
            .iter()
            .map(|(uid, counter)| (*uid, counter.load(Ordering::Relaxed)))
            .collect()
    };

    // Compute the diff + the new per_user_speaking state.
    // Collected into a separate Vec so we can swap maps under
    // borrow_mut without holding it across the backend callback.
    let mut flips: Vec<(u16, bool)> = Vec::new();
    {
        let mut inner = runtime.inner.borrow_mut();
        for (uid, current) in &snapshot {
            let prev = inner
                .per_user_activity_prev_snapshot
                .get(uid)
                .copied()
                .unwrap_or(0);
            let speaking = *current > prev;
            let was_speaking =
                inner.per_user_speaking.get(uid).copied().unwrap_or(false);
            if speaking != was_speaking {
                flips.push((*uid, speaking));
                inner.per_user_speaking.insert(*uid, speaking);
            }
            inner.per_user_activity_prev_snapshot.insert(*uid, *current);
        }
    }

    for (uid, is_speaking) in flips {
        runtime.backend.borrow_mut().emit_signal(
            SignalKind::SpeakerChanged,
            SignalPayload::SpeakerChanged { uid, is_speaking },
        );
    }
}

/// Errors returned from `VoiceRuntime::new`.
#[derive(Debug)]
pub enum RuntimeError {
    /// `gst::init()` failed. The constructor calls
    /// `gstreamer::init()` itself (idempotent — repeats are a
    /// no-op), so this variant means the underlying call
    /// returned an error: the GStreamer runtime itself failed
    /// to initialise. Causes vary (broken or missing GStreamer
    /// install, mis-set `GST_PLUGIN_PATH` / `GST_REGISTRY` env,
    /// running under a sandbox that can't reach the system
    /// registry); the wrapped `glib::Error` carries the
    /// gstreamer-rs message verbatim, which is the most
    /// reliable signal for triage.
    ///
    /// Production has typically already run
    /// `gtkhx_voice_init()` from `main` which surfaces the
    /// failure in the C-side log; constructing a runtime
    /// afterwards then sees the re-init no-op and never reaches
    /// this branch. Step 2 doesn't need the WebRTC plugin set
    /// yet — that requirement is local to specific dispatch
    /// arms in step 3+ (which surface plugin-load failures
    /// separately).
    GstInitFailed(gstreamer::glib::Error),
    /// `webrtcbin` couldn't be wired into the runtime's pipeline.
    /// Covers both `ElementFactory::make("webrtcbin").build()`
    /// failures (typical case: `gst-plugins-bad` isn't installed
    /// at runtime so the factory lookup misses) AND
    /// `Pipeline::add` failures (rare — a misconfigured pipeline,
    /// a registry race). The underlying `BoolError` is logged on
    /// the GStreamer warning channel at the failure site; the
    /// variant itself is payload-less because the error type
    /// doesn't implement `std::error::Error` cleanly.
    /// The C side disables voice UI for the rest of the session
    /// in either case.
    WebrtcbinUnavailable,
}

impl core::fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            RuntimeError::GstInitFailed(err) => write!(
                f,
                "gst::init() failed: {err} — the GStreamer runtime \
                 couldn't initialise; check the GStreamer install \
                 and the GST_PLUGIN_PATH / GST_REGISTRY environment"
            ),
            RuntimeError::WebrtcbinUnavailable => write!(
                f,
                "couldn't wire `webrtcbin` into the runtime \
                 pipeline; the most common cause is that \
                 `gst-plugins-bad` isn't installed (the package \
                 that ships webrtcbin), but a misconfigured \
                 pipeline can produce this too — the underlying \
                 GStreamer error is logged at the failure site"
            ),
        }
    }
}

impl std::error::Error for RuntimeError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            RuntimeError::GstInitFailed(err) => Some(err),
            RuntimeError::WebrtcbinUnavailable => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use hxvoice::event::ConnectionState;
    // Note re: GStreamer extension-trait methods (pipeline.by_name,
    // bin.factory, property::<T>, etc.) used by the SDP-dispatch
    // tests below: these come from `gstreamer::prelude::*`, which
    // the parent module imports at the top. Trait-method resolution
    // sees parent-module `use` imports inside child modules, so we
    // don't need to re-import the prelude here.

    /// Shared-state recorder. The runtime owns a `Box<dyn Backend>`
    /// wrapping one clone of the cell; the test keeps another clone
    /// for inspection. This replaces the previous raw-pointer hack
    /// — proper lifetimes, no unsafe — and pairs naturally with the
    /// `Rc<RefCell<Box<dyn Backend>>>` slot the runtime exposes.
    struct SharedRec(Rc<RefCell<RecordingBackend>>);

    impl Backend for SharedRec {
        fn send_wire_frame(&mut self, opcode: u32, body: &[u8]) {
            self.0.borrow_mut().send_wire_frame(opcode, body);
        }
        fn emit_signal(&mut self, kind: SignalKind, payload: SignalPayload) {
            self.0.borrow_mut().emit_signal(kind, payload);
        }
        fn tear_down(&mut self) {
            self.0.borrow_mut().tear_down();
        }
    }

    /// Returns a runtime and a shared handle the test can read.
    fn rec() -> (VoiceRuntime, Rc<RefCell<RecordingBackend>>) {
        let shared = Rc::new(RefCell::new(RecordingBackend::default()));
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(SharedRec(shared.clone())));
        (runtime, shared)
    }

    #[test]
    fn join_request_routes_send_wire_frame_to_backend() {
        let (runtime, backend) = rec();
        let new_state = runtime.handle_event(Event::JoinRequested { cid: 42 });
        assert_eq!(new_state, SessionState::JoinSent);
        let backend = backend.borrow();
        assert_eq!(backend.wire_frames.len(), 1);
        assert_eq!(backend.wire_frames[0].0, 600);
        // The state machine encoded cid 42 as BE.
        assert_eq!(backend.wire_frames[0].1, vec![0, 0, 0, 42]);
    }

    #[test]
    fn join_request_arms_join_reply_timer() {
        let (runtime, _) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        assert_eq!(runtime.armed_timers(), vec![Timeout::JoinReply]);
    }

    /// On entering `Connecting`, the wedge watchdog must be
    /// armed. `wedge_watchdog_armed()` reports the conceptual
    /// armed state (state == Connecting) rather than the
    /// SourceId presence, so production and the cargo-parallel
    /// test-fallback path both return `true` here. The real
    /// glib-source firing path is covered by the manual-tick
    /// tests below (`wedge_tick_*`).
    #[test]
    fn entering_connecting_arms_wedge_watchdog() {
        let (runtime, _) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        assert_eq!(runtime.state(), SessionState::Connecting);
        assert!(
            runtime.wedge_watchdog_armed(),
            "wedge watchdog must be armed on entering Connecting"
        );
    }

    /// Wedge tick with the RTP counter unchanged from arm time
    /// injects `Timeout::WedgeDeadline`. Under cargo parallel
    /// tests we drive the tick manually via
    /// `fire_wedge_watchdog_for_test` rather than waiting on
    /// the 60-second glib source.
    #[test]
    fn wedge_tick_with_no_rtp_activity_tears_session_down() {
        let (runtime, backend) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        assert_eq!(runtime.state(), SessionState::Connecting);
        // Snapshot is 0 (initial); counter is 0 (no probe ever
        // fired). Tick fires WedgeDeadline → state.rs walks the
        // session to Leaving and emits TearDown.
        runtime.fire_wedge_watchdog_for_test();
        assert_eq!(runtime.state(), SessionState::Leaving);
        assert!(
            backend.borrow().tear_downs > 0,
            "tear_down should have been called by the WedgeDeadline injection"
        );
    }

    /// Wedge tick with the RTP counter advanced since arm time
    /// rearms instead of injecting WedgeDeadline. The session
    /// stays in Connecting; we verify by asserting state.
    #[test]
    fn wedge_tick_with_rtp_activity_keeps_session_alive() {
        let (runtime, backend) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        assert_eq!(runtime.state(), SessionState::Connecting);
        // Simulate RTP arriving between arm and tick — the
        // production path increments via a pad probe; the test
        // hook bumps the same Arc<AtomicU64>.
        runtime.bump_rtp_buffers_received_for_test();
        runtime.fire_wedge_watchdog_for_test();
        assert_eq!(runtime.state(), SessionState::Connecting);
        assert_eq!(
            backend.borrow().tear_downs,
            0,
            "tear_down must NOT be called when RTP is flowing"
        );
    }

    /// Leaving Connecting (e.g. via WebrtcConnectionStateChanged
    /// → Connected) cancels the wedge watchdog. The next tick
    /// races at most once with the cancel — and if it does fire,
    /// the catch-all in `wedge_watchdog_tick` bails because the
    /// machine is no longer in Connecting. Either way: the
    /// session is NOT torn down by a late wedge tick.
    #[test]
    fn wedge_tick_after_connected_does_not_tear_down() {
        let (runtime, backend) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        // Walk to Connected — the watchdog must cancel.
        runtime.handle_event(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(runtime.state(), SessionState::Connected);
        // Manually fire a stale tick. The tick observes state !=
        // Connecting and returns without injecting anything.
        runtime.fire_wedge_watchdog_for_test();
        assert_eq!(runtime.state(), SessionState::Connected);
        assert_eq!(backend.borrow().tear_downs, 0);
    }

    /// Regression: a server-initiated SDP renegotiation while the
    /// peer connection is already `Connected` used to leave the
    /// state machine stuck in `Connecting`. The webrtcbin's
    /// `notify::connection-state` only fires on actual property
    /// changes, and the property was already `Connected` when the
    /// new offer arrived, so the `(Connecting, Connected)` arm
    /// never ran and the wedge watchdog eventually tore down a
    /// perfectly healthy session.
    ///
    /// Fix: when the runtime observes the state machine entering
    /// `Connecting` after `last_seen_peer_state` has been
    /// `Connected`, it synthesises a fresh
    /// `WebrtcConnectionStateChanged { Connected }` event so the
    /// state machine advances back to `Connected` immediately.
    /// This test pins that path.
    #[test]
    fn renegotiation_while_already_connected_returns_to_connected() {
        let (runtime, _backend) = rec();
        // Walk through the initial join → Connected sequence.
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(runtime.state(), SessionState::Connected);

        // Server sends a renegotiation offer. The state machine
        // walks Connected → OfferPending → (after answer creation)
        // Connecting. In production webrtcbin would NOT emit a
        // fresh `connection-state == Connected` notify here, so we
        // do not feed one in below — the runtime must synthesise
        // it on its own.
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\nrenegotiated\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\nanswer-2\n".into(),
        });
        assert_eq!(
            runtime.state(),
            SessionState::Connected,
            "renegotiation must land back at Connected even without a fresh \
             webrtcbin connection-state notify — the runtime synthesises one \
             from `last_seen_peer_state`"
        );
    }

    /// Companion to
    /// `renegotiation_while_already_connected_returns_to_connected`:
    /// even if the synthesis didn't run for some reason, the wedge
    /// watchdog must not be armed on a re-entry to Connecting that
    /// follows a prior Connected. Pin that with a fresh runtime
    /// where we deliberately suppress the `last_seen_peer_state`
    /// view by walking through the state machine without a
    /// `Connected` notify between offers.
    ///
    /// In this test the WebrtcConnectionStateChanged events are
    /// supplied explicitly so `last_seen_peer_state` is tracked
    /// honestly. The assertion target is the watchdog status, NOT
    /// the post-renegotiation SessionState.
    #[test]
    fn wedge_not_armed_on_renegotiation_reentry() {
        let (runtime, _backend) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        // Initial Connecting entry: wedge armed.
        assert!(runtime.wedge_watchdog_armed());
        runtime.handle_event(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        // Connected: wedge cancelled.
        assert!(!runtime.wedge_watchdog_armed());

        // Renegotiation: drives Connected → OfferPending →
        // (answer) → Connecting. The synthesis push-back from
        // Fix #1 immediately drains us back to Connected, but
        // even if it didn't, the `has_been_connected_since_join`
        // gate must prevent the wedge from arming on re-entry.
        // We assert the watchdog is NOT armed once dispatch
        // settles.
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\nrenegotiated\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\nanswer-2\n".into(),
        });
        assert!(
            !runtime.wedge_watchdog_armed(),
            "renegotiation re-entry to Connecting must not re-arm the wedge \
             watchdog — a never-Connected first-join is what the wedge is \
             for, not a renegotiation cycle"
        );
    }

    // ---- Speaker-activity evaluator ------------------------------
    //
    // The evaluator runs `speaker_tick` periodically (200ms in
    // production via glib::timeout_add_local); these tests drive
    // it manually via `speaker_tick_for_test`, mutate the per-user
    // counters via `bump_per_user_activity_for_test`, and assert on the
    // SpeakerChanged signal emissions captured by RecordingBackend.

    fn speaker_emits(backend: &Rc<RefCell<RecordingBackend>>) -> Vec<(u16, bool)> {
        backend
            .borrow()
            .signals
            .iter()
            .filter_map(|(kind, payload)| match (kind, payload) {
                (
                    SignalKind::SpeakerChanged,
                    SignalPayload::SpeakerChanged { uid, is_speaking },
                ) => Some((*uid, *is_speaking)),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn speaker_tick_with_no_activity_emits_nothing() {
        let (runtime, backend) = rec();
        runtime.speaker_tick_for_test();
        assert!(speaker_emits(&backend).is_empty());
    }

    #[test]
    fn speaker_tick_after_activity_emits_speaking_true() {
        let (runtime, backend) = rec();
        // Streaming-thread analogue: probe bumped uid 7's counter.
        runtime.bump_per_user_activity_for_test(7);
        runtime.speaker_tick_for_test();
        let emits = speaker_emits(&backend);
        assert_eq!(emits, vec![(7, true)]);
    }

    #[test]
    fn speaker_tick_steady_state_does_not_re_emit() {
        let (runtime, backend) = rec();
        runtime.bump_per_user_activity_for_test(7);
        runtime.speaker_tick_for_test();
        // Activity stops — counter doesn't advance between ticks.
        runtime.speaker_tick_for_test();
        runtime.speaker_tick_for_test();
        let emits = speaker_emits(&backend);
        // First tick → (7, true). Second → (7, false) once it
        // observes no advance. Third → no change.
        assert_eq!(emits, vec![(7, true), (7, false)]);
    }

    #[test]
    fn speaker_tick_resume_emits_speaking_true_again() {
        let (runtime, backend) = rec();
        // Tick 1: activity → speaking.
        runtime.bump_per_user_activity_for_test(7);
        runtime.speaker_tick_for_test();
        // Tick 2: no new activity → silent.
        runtime.speaker_tick_for_test();
        // Tick 3: activity resumed → speaking again.
        runtime.bump_per_user_activity_for_test(7);
        runtime.speaker_tick_for_test();
        let emits = speaker_emits(&backend);
        assert_eq!(emits, vec![(7, true), (7, false), (7, true)]);
    }

    #[test]
    fn speaker_tick_handles_multiple_uids_independently() {
        let (runtime, backend) = rec();
        runtime.bump_per_user_activity_for_test(3);
        runtime.bump_per_user_activity_for_test(5);
        runtime.speaker_tick_for_test();
        // Only uid 5 stays active across the second tick.
        runtime.bump_per_user_activity_for_test(5);
        runtime.speaker_tick_for_test();
        let emits = speaker_emits(&backend);
        // Order within a single tick is unspecified (HashMap
        // iteration), so check membership rather than positional.
        assert!(emits.contains(&(3, true)));
        assert!(emits.contains(&(5, true)));
        assert!(emits.contains(&(3, false)));
        assert!(!emits.contains(&(5, false)));
    }

    // ---- Client-side VAD (level element) ----

    #[test]
    fn rms_db_threshold_is_inclusive_and_rejects_silence() {
        // At or above the threshold reads as speaking; the boundary
        // is inclusive.
        assert!(rms_db_indicates_speaking(0.0));
        assert!(rms_db_indicates_speaking(-4.9));
        assert!(rms_db_indicates_speaking(SPEAKING_RMS_THRESHOLD_DB));
        // Below the threshold is silent, including the `level`
        // element's `-inf` digital-silence sentinel.
        assert!(!rms_db_indicates_speaking(SPEAKING_RMS_THRESHOLD_DB - 0.1));
        assert!(!rms_db_indicates_speaking(-60.0));
        assert!(!rms_db_indicates_speaking(f64::NEG_INFINITY));
    }

    #[test]
    fn uid_recovered_from_receive_bin_name() {
        assert_eq!(uid_from_recv_bin_name("hxvoice-recv-user-5"), Some(5));
        assert_eq!(
            uid_from_recv_bin_name("hxvoice-recv-user-65535"),
            Some(65535)
        );
        // The local send leg carries no remote uid.
        assert_eq!(uid_from_recv_bin_name("hxvoice-recv-send"), None);
        // Names without a parseable user-<uid> mid don't resolve.
        assert_eq!(uid_from_recv_bin_name("hxvoice-recv-bogus"), None);
        assert_eq!(uid_from_recv_bin_name("some-other-element"), None);
        // The name `start_receive_bin` actually builds must round-trip
        // back to the uid — pins the prefix shared between the two.
        let name = format!("{RECV_BIN_PREFIX}user-9");
        assert_eq!(uid_from_recv_bin_name(&name), Some(9));
    }

    /// End-to-end against the host's real `level` element: build a
    /// loud-sine pipeline, capture a `"level"` bus message, and prove
    /// `level_message_max_rms_db` pulls a finite RMS out of the
    /// `GValueArray` that reads as speaking. This is the host-side
    /// guard on the GValueArray field-type assumption — a silent
    /// regression in gstreamer-rs's representation of `rms` would
    /// turn the extraction into a permanent `None` (speaker indicator
    /// stuck off) that the pure-logic tests above can't catch.
    #[test]
    fn level_message_rms_extracted_from_real_pipeline() {
        assert!(crate::init(), "gst::init() must succeed");
        let pipeline = gstreamer::Pipeline::new();
        let src = gstreamer::ElementFactory::make("audiotestsrc")
            .property("is-live", false)
            .build()
            .expect("audiotestsrc plugin available");
        let conv = gstreamer::ElementFactory::make("audioconvert")
            .build()
            .expect("audioconvert plugin available");
        let caps = crate::audio::make_pcm8khz_caps_filter()
            .expect("capsfilter element");
        let level =
            crate::audio::make_level_meter().expect("level plugin available");
        // Shorter interval than production so a message lands fast.
        level.set_property("interval", 50_000_000u64);
        let sink = gstreamer::ElementFactory::make("fakesink")
            .property("sync", false)
            .build()
            .expect("fakesink plugin available");
        pipeline
            .add_many([&src, &conv, &caps, &level, &sink])
            .expect("add elements");
        gstreamer::Element::link_many([&src, &conv, &caps, &level, &sink])
            .expect("link elements");
        pipeline
            .set_state(gstreamer::State::Playing)
            .expect("pipeline reaches Playing");

        // Poll the bus synchronously — no main loop needed, so this
        // is robust under cargo's parallel test runner where the
        // default MainContext may be owned by another thread.
        let bus = pipeline.bus().expect("pipeline has a bus");
        let deadline =
            std::time::Instant::now() + std::time::Duration::from_secs(5);
        let mut got: Option<f64> = None;
        while std::time::Instant::now() < deadline {
            let Some(msg) = bus.timed_pop_filtered(
                Some(gstreamer::ClockTime::from_mseconds(200)),
                &[gstreamer::MessageType::Element],
            ) else {
                continue;
            };
            if let gstreamer::MessageView::Element(e) = msg.view() {
                if let Some(s) = e.structure() {
                    if s.name() == "level" {
                        got = level_message_max_rms_db(s);
                        break;
                    }
                }
            }
        }
        let _ = pipeline.set_state(gstreamer::State::Null);

        let db = got.expect("a 'level' message carrying rms should arrive");
        assert!(db.is_finite(), "loud sine RMS should be finite, got {db}");
        assert!(
            rms_db_indicates_speaking(db),
            "a full-volume sine should read as speaking, got {db} dB"
        );
    }

    #[test]
    fn speaker_signal_routes_to_callback_backend() {
        // Exercise the FFI dispatch arm specifically — the
        // CallbackBackend's emit_signal arm for SignalKind::
        // SpeakerChanged. We hand it a fake callback that records
        // arguments into a global AtomicU64-packed (uid, flag)
        // tuple.
        use core::sync::atomic::AtomicU64;
        static SPEAKER_PAYLOAD: AtomicU64 = AtomicU64::new(0);
        unsafe extern "C" fn cb(
            _ud: *mut core::ffi::c_void,
            uid: u16,
            is_speaking: i32,
        ) {
            // Pack as (uid << 32) | is_speaking so the test can
            // distinguish multiple emits.
            SPEAKER_PAYLOAD.store(
                ((uid as u64) << 32) | (is_speaking as u64),
                Ordering::SeqCst,
            );
        }
        let signals = SignalCallbacks {
            state_changed: None,
            mute_changed: None,
            speaker_changed: Some(cb),
            error: None,
        };
        let mut backend = CallbackBackend::new_with_signals(
            core::ptr::null_mut(),
            None,
            signals,
        );
        backend.emit_signal(
            SignalKind::SpeakerChanged,
            SignalPayload::SpeakerChanged {
                uid: 42,
                is_speaking: true,
            },
        );
        assert_eq!(
            SPEAKER_PAYLOAD.load(Ordering::SeqCst),
            (42u64 << 32) | 1
        );
        backend.emit_signal(
            SignalKind::SpeakerChanged,
            SignalPayload::SpeakerChanged {
                uid: 42,
                is_speaking: false,
            },
        );
        assert_eq!(
            SPEAKER_PAYLOAD.load(Ordering::SeqCst),
            (42u64 << 32) | 0
        );
    }

    #[test]
    fn answer_created_walks_to_connecting_with_send_603() {
        let (runtime, backend) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "a=mid:user-2\na=mid:send\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        assert_eq!(runtime.state(), SessionState::Connecting);
        let backend = backend.borrow();
        // 600 (JOIN) then 603 (SDP_ANSWER).
        let opcodes: Vec<u32> = backend
            .wire_frames
            .iter()
            .map(|(op, _)| *op)
            .collect();
        assert_eq!(opcodes, vec![600, 603]);
    }

    /// Regression (Copilot review): ArmTimer used to no-op when
    /// the same kind was already armed. That's wrong semantics
    /// for a (re-)arm — the state machine sends repeat ArmTimer
    /// events on renegotiation, expecting the watchdog to
    /// restart. The fix removes the old entry and pushes a fresh
    /// one; we verify by counting occurrences (must stay at 1
    /// across a re-arm).
    #[test]
    fn rearming_same_timer_kind_is_an_idempotent_reset() {
        let (runtime, _) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        // The state machine emits an ArmTimer(JoinReply) on this
        // event; pump another that re-arms it (we drive it via
        // dispatch directly, since the state machine wouldn't
        // emit a redundant ArmTimer naturally).
        runtime.dispatch(Action::ArmTimer {
            kind: Timeout::JoinReply,
            ms: 5000,
        });
        let armed = runtime.armed_timers();
        let count = armed.iter().filter(|t| **t == Timeout::JoinReply).count();
        assert_eq!(count, 1, "JoinReply should appear exactly once");
    }

    #[test]
    fn timer_cancel_clears_the_armed_set() {
        let (runtime, _) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        assert!(runtime.armed_timers().contains(&Timeout::JoinReply));
        // The SDP offer cancels the JoinReply timer.
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        assert!(!runtime.armed_timers().contains(&Timeout::JoinReply));
    }

    #[test]
    fn webrtc_connected_arms_media_timer_and_cancels_dtls_ice() {
        let (runtime, _) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        // Connecting state: DTLS and ICE timers are armed.
        let armed = runtime.armed_timers();
        assert!(armed.contains(&Timeout::Dtls));
        assert!(armed.contains(&Timeout::IceConnectivity));

        runtime.handle_event(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(runtime.state(), SessionState::Connected);
        let armed = runtime.armed_timers();
        assert!(!armed.contains(&Timeout::Dtls));
        assert!(!armed.contains(&Timeout::IceConnectivity));
        assert!(armed.contains(&Timeout::Media));
    }

    #[test]
    fn leave_from_connected_tears_down_and_sends_601() {
        let (runtime, backend) = rec();
        // Walk to Connected.
        runtime.handle_event(Event::JoinRequested { cid: 7 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 7,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });

        runtime.handle_event(Event::LeaveRequested { cid: 7 });
        // LeaveRequested walks directly to Idle so the user can
        // rejoin without the machine wedging in Leaving. See
        // hxvoice::state::tests::rejoin_after_leave_walks_idle_to_join_sent
        // for the canonical pin.
        assert_eq!(runtime.state(), SessionState::Idle);
        let backend = backend.borrow();
        let opcodes: Vec<u32> = backend
            .wire_frames
            .iter()
            .map(|(op, _)| *op)
            .collect();
        assert!(opcodes.contains(&601));
        assert_eq!(backend.tear_downs, 1);
    }

    #[test]
    fn failed_connection_triggers_error_signal_and_tear_down() {
        let (runtime, backend) = rec();
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Failed,
        });
        let backend = backend.borrow();
        assert_eq!(backend.tear_downs, 1);
        // At least one Error signal should have fired.
        let saw_error = backend
            .signals
            .iter()
            .any(|(k, _)| matches!(k, SignalKind::Error));
        assert!(saw_error);
    }

    #[test]
    fn noop_backend_lets_runtime_run_without_assertions() {
        // Coverage smoke for the NoopBackend — confirms the
        // production-not-yet-wired path doesn't panic during a
        // full join → leave walk.
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(NoopBackend));
        runtime.handle_event(Event::JoinRequested { cid: 1 });
        runtime.handle_event(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        runtime.handle_event(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        runtime.handle_event(Event::LeaveRequested { cid: 1 });
        // User-driven Leave walks directly to Idle (see
        // hxvoice::state::tests::rejoin_after_leave_walks_idle_to_join_sent).
        assert_eq!(runtime.state(), SessionState::Idle);
    }

    /// Regression (Copilot review): dispatch() used to hold an
    /// `Inner` borrow_mut while calling into the Backend. A backend
    /// implementation that turns around and re-enters
    /// `handle_event` (the typical shape when a UI signal handler
    /// triggers another voice transition — e.g. the "Leave" button
    /// inside the voice-error toast) would panic on the nested
    /// `Inner` borrow. Splitting backend into its own `Rc<RefCell>`
    /// makes the borrow scopes independent.
    #[test]
    fn backend_can_reenter_handle_event_without_panicking() {
        use core::cell::Cell;

        /// Backend that fires LeaveRequested back into the runtime
        /// the first time it sees a JOIN wire frame go out. The
        /// runtime field is wired post-construction via the
        /// `RefCell<Option<…>>` shape; `fired` is a single-shot
        /// recursion guard so the LEAVE doesn't fire a second
        /// JOIN→LEAVE loop.
        struct ReentrantBackend {
            runtime: Rc<RefCell<Option<VoiceRuntime>>>,
            fired: Cell<bool>,
        }

        impl Backend for ReentrantBackend {
            fn send_wire_frame(&mut self, opcode: u32, _body: &[u8]) {
                // First JOIN wire frame: turn around and request
                // Leave. Without the Backend-split fix this would
                // panic on borrow_mut(Inner) because the dispatch
                // loop is still holding it.
                if opcode == 600 && !self.fired.get() {
                    self.fired.set(true);
                    if let Some(rt) = self.runtime.borrow().as_ref() {
                        // The state machine only accepts a LEAVE
                        // for the active cid (42 here); spurious
                        // ones are dropped silently, which is
                        // exactly what we want for the re-entry
                        // probe.
                        rt.handle_event(Event::LeaveRequested { cid: 42 });
                    }
                }
            }
            fn emit_signal(&mut self, _: SignalKind, _: SignalPayload) {}
            fn tear_down(&mut self) {}
        }

        let runtime_slot: Rc<RefCell<Option<VoiceRuntime>>> =
            Rc::new(RefCell::new(None));
        let backend = Box::new(ReentrantBackend {
            runtime: runtime_slot.clone(),
            fired: Cell::new(false),
        });
        // Build the runtime, then plug it into the backend so the
        // re-entry can find it.
        let runtime = VoiceRuntime::new_without_pipeline(backend);
        *runtime_slot.borrow_mut() = Some(runtime.clone());

        // Drive the JOIN — backend will re-enter with LEAVE before
        // we return.
        runtime.handle_event(Event::JoinRequested { cid: 42 });

        // The fact that we reached this line at all is the test —
        // the old code would have panicked on the re-entry.
        // We verify the re-entry actually happened by observing
        // the state machine transitioned all the way back to Idle
        // (JOIN sets active_cid to 42, LEAVE matches active_cid
        // and walks Idle now that LeaveRequested no longer parks
        // at Leaving).
        let backend_slot = runtime_slot.borrow();
        let runtime_ref = backend_slot.as_ref().unwrap();
        assert_eq!(runtime_ref.state(), SessionState::Idle);
    }

    #[test]
    fn pipeline_built_runtime_constructs_when_gst_initialised() {
        // Coverage smoke for the with-pipeline constructor.
        // `VoiceRuntime::new` runs `gstreamer::init()` itself, but
        // we call `crate::init()` first as an explicit
        // sanity-check on the test environment: if GStreamer is
        // misconfigured in this CI container, the standalone
        // `init()` check surfaces it here as a clear test
        // failure, before `VoiceRuntime::new` would otherwise
        // hand back `RuntimeError::GstInitFailed`. With the
        // standalone init passing, `new` should succeed cleanly.
        assert!(
            crate::init(),
            "gst::init() must succeed for the with-pipeline test"
        );
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        // Sanity: the pipeline element is reachable.
        assert!(runtime.inner.borrow().pipeline.is_some());
    }

    // ------------------------------------------------------------------
    // Phase 8.C step 3 — SDP dispatch arms.
    // ------------------------------------------------------------------

    /// Minimal but valid offer SDP for `webrtcbin`. Needs the
    /// session-level headers (`v= o= s= t=`) AND at least one media
    /// description (`m=`) — without media `webrtcbin` keeps
    /// `signaling-state == Stable` even after set-remote-description
    /// completes. PCMU/8000 mono is the spec-required codec for
    /// the Hotline voice extension, so we mirror that here.
    const MIN_OFFER_SDP: &str = concat!(
        "v=0\r\n",
        "o=- 1 1 IN IP4 127.0.0.1\r\n",
        "s=-\r\n",
        "t=0 0\r\n",
        "m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n",
        "c=IN IP4 0.0.0.0\r\n",
        "a=rtcp-mux\r\n",
        "a=sendrecv\r\n",
        "a=mid:audio0\r\n",
        "a=rtpmap:0 PCMU/8000\r\n",
        "a=setup:actpass\r\n",
        "a=ice-ufrag:abcd\r\n",
        "a=ice-pwd:0123456789abcdef0123456789\r\n",
        "a=fingerprint:sha-256 ",
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:",
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n",
    );

    /// Pipeline-built constructor adds a `webrtcbin` element. The
    /// element should be visible by name on the pipeline so the
    /// step-4 ICE wiring can find it.
    #[test]
    fn pipeline_built_runtime_contains_webrtcbin() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        let inner = runtime.inner.borrow();
        let bin = inner
            .webrtcbin
            .as_ref()
            .expect("webrtcbin must be present");
        assert_eq!(bin.factory().unwrap().name(), "webrtcbin");
        // And it should live inside the pipeline.
        let pipeline = inner.pipeline.as_ref().unwrap();
        assert!(
            pipeline.by_name("hxvoice-webrtcbin").is_some(),
            "webrtcbin must be a child of the hxvoice pipeline"
        );
    }

    /// End-to-end: a `MuteToggleRequested` event drives the
    /// `SetSendPipelineMute` dispatch arm, which flips the send
    /// `volume` element's `mute` property — the real client-side
    /// mic cut. Starts unmuted, mutes, unmutes. This is the
    /// regression guard for the previously-no-op mute arm.
    #[test]
    fn mute_toggle_sets_send_volume_mute_property() {
        assert!(crate::init(), "gst::init() must succeed");
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");

        // The send bin + its volume element exist from construction.
        let pipeline = runtime
            .inner
            .borrow()
            .pipeline
            .clone()
            .expect("with-pipeline runtime has a pipeline");
        let vol = pipeline
            .by_name(crate::audio::SEND_VOLUME_ELEMENT_NAME)
            .expect("send volume element present in the pipeline");
        assert!(
            !vol.property::<bool>("mute"),
            "send volume starts unmuted"
        );

        // MuteToggleRequested is only honoured in an active-room
        // state, so join first (Idle → JoinSent).
        runtime.handle_event(Event::JoinRequested { cid: 0 });

        runtime.handle_event(Event::MuteToggleRequested { muted: true });
        assert!(
            vol.property::<bool>("mute"),
            "mute=true must reach the send volume element"
        );

        runtime.handle_event(Event::MuteToggleRequested { muted: false });
        assert!(
            !vol.property::<bool>("mute"),
            "unmute must reach the send volume element"
        );
    }

    /// SetRemoteDescription dispatch reaches the underlying
    /// `webrtcbin.emit("set-remote-description", …)` call without
    /// panicking when handed a syntactically-valid offer SDP. The
    /// signal emit itself is fire-and-forget at the bin's NULL
    /// state — webrtcbin enqueues the task on its main thread and
    /// only actually transitions `signaling-state` once the
    /// pipeline is at `Playing` (which requires real codec
    /// elements wired up, deferred to step 5+). Asserting on the
    /// post-state property would be testing webrtcbin, not our
    /// dispatch.
    ///
    /// What we DO verify: the dispatch arm completes, the bin is
    /// still alive (no GStreamer crash), and the SDP parsed
    /// (otherwise `build_session_description` would have returned
    /// `None` and the emit would be skipped).
    #[test]
    fn set_remote_description_with_valid_sdp_dispatches_without_panic() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        runtime.dispatch(Action::SetRemoteDescription {
            sdp: MIN_OFFER_SDP.into(),
        });
        // The bin survived the emit.
        assert!(runtime.inner.borrow().webrtcbin.is_some());
    }

    /// Direct unit cover for the SDP parser. `MIN_OFFER_SDP` must
    /// successfully build a `WebRTCSessionDescription`; without
    /// this guarantee the higher-level dispatch tests silently
    /// no-op via the `None` early-return.
    #[test]
    fn build_session_description_parses_valid_offer_sdp() {
        assert!(crate::init());
        let desc = build_session_description(MIN_OFFER_SDP, WebRTCSDPType::Offer)
            .expect("MIN_OFFER_SDP must parse cleanly");
        assert_eq!(desc.type_(), WebRTCSDPType::Offer);
    }

    /// Empty-input early-return. Exercises the `sdp.is_empty()`
    /// guard that sits in front of
    /// `gstreamer_sdp::SDPMessage::parse_buffer`.
    ///
    /// Note: the parser-error branch BELOW the empty-input guard
    /// (the `match parse_buffer { Err(e) => warning! + return
    /// None }` arm) is effectively unreachable from external
    /// input — `gst_sdp_message_parse_buffer` is permissive
    /// enough to accept literally any non-empty byte slice
    /// (probed manually: NUL bytes, single ASCII control, raw
    /// 0xFF bytes — all return Ok). The arm exists for forward
    /// compatibility if a future GStreamer release tightens
    /// parsing; until then, no fixture-driven test of the
    /// logging path is possible. The contract this test pins is
    /// the only externally-reachable failure path:
    /// empty input → silent `None`, no GStreamer-SDP critical,
    /// no panic.
    #[test]
    fn build_session_description_empty_input_returns_none() {
        assert!(crate::init());
        let desc = build_session_description("", WebRTCSDPType::Offer);
        assert!(
            desc.is_none(),
            "expected build_session_description to return None for empty input"
        );
    }

    /// Malformed SDP must not panic. The dispatch arm logs and
    /// returns; the test's load-bearing assertion is "we didn't
    /// crash" — webrtcbin'd signaling-state behaviour on a fed
    /// garbage SDP is gst-version-sensitive (gst 1.26 silently
    /// rejects the message and leaves state at Stable; gst 1.28+
    /// constructs an empty SDP message even from random bytes
    /// because `gst_sdp_message_parse_buffer` is extremely
    /// lenient, and webrtcbin then accepts that empty message and
    /// transitions to HaveRemoteOffer). The previous revision of
    /// this test asserted Stable-after-Stable, which CI's Fedora
    /// 42 / gst 1.26 satisfied but newer Debian / gst 1.28 broke;
    /// pinning the state check to a specific gst version is more
    /// brittle than the actual contract under test.
    #[test]
    fn set_remote_description_with_garbage_sdp_is_a_silent_noop() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        // Read signaling-state to prove the bin is alive before
        // we hand it garbage. The actual value is not asserted on
        // (see above).
        let _before: gstreamer_webrtc::WebRTCSignalingState =
            runtime
                .inner
                .borrow()
                .webrtcbin
                .as_ref()
                .unwrap()
                .property("signaling-state");
        runtime.dispatch(Action::SetRemoteDescription {
            sdp: "not an sdp".into(),
        });
        // Re-read to confirm the bin is still alive — a panic
        // inside dispatch would have unwound here. Any value is
        // acceptable; we just verify the property query succeeds.
        let _after: gstreamer_webrtc::WebRTCSignalingState =
            runtime
                .inner
                .borrow()
                .webrtcbin
                .as_ref()
                .unwrap()
                .property("signaling-state");
    }

    /// Pipeline-less runtime: SDP dispatch arms early-return cleanly.
    /// This is the configuration tests that exercise the
    /// state-machine + Backend paths use; SDP arms shouldn't
    /// crash just because there's no bin.
    #[test]
    fn pipeline_less_runtime_no_ops_sdp_dispatch() {
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(NoopBackend));
        // None of these should panic.
        runtime.dispatch(Action::SetRemoteDescription {
            sdp: MIN_OFFER_SDP.into(),
        });
        runtime.dispatch(Action::SetLocalDescription {
            sdp: MIN_OFFER_SDP.into(),
        });
        runtime.dispatch(Action::CreateAnswer);
        // Sanity: state machine is still Idle.
        assert_eq!(runtime.state(), SessionState::Idle);
    }

    /// Drop on `Inner` evicts the registry entry. We register on
    /// construction; the registry holds a `Weak`, so the runtime
    /// dropping reduces the Weak's strong count to zero; Drop
    /// removes the dead entry so a late closure looking up by
    /// id sees None.
    #[test]
    fn dropping_runtime_evicts_thread_local_registry_entry() {
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(NoopBackend));
        let id = runtime.inner.borrow().runtime_id;
        // Confirm registered.
        let present = MAIN_THREAD_RUNTIMES
            .with(|cell| cell.borrow().contains_key(&id));
        assert!(present, "runtime should be registered post-construction");

        drop(runtime);

        let still_present = MAIN_THREAD_RUNTIMES
            .with(|cell| cell.borrow().contains_key(&id));
        assert!(
            !still_present,
            "Drop for Inner should evict the registry entry"
        );
    }

    /// Late callbacks (Promise fires after the runtime has been
    /// dropped) become a silent no-op via the upgrade-from-Weak
    /// machinery. The `with_main_thread_runtime` helper handles
    /// the absent case without panicking.
    #[test]
    fn with_main_thread_runtime_returns_silently_for_unknown_id() {
        let mut ran = false;
        // Use a deliberately-bogus id — far above NEXT_RUNTIME_ID's
        // monotonic floor for the test.
        with_main_thread_runtime(u64::MAX - 1, |_| ran = true);
        assert!(
            !ran,
            "closure must not run when the runtime id is unknown"
        );
    }

    /// SetLocalDescription on the answerer path: after accepting
    /// the remote offer, applying a local answer takes the
    /// signaling state to `Stable`. (`HaveRemoteOffer` → `Stable`
    /// is the answerer's local-answer transition per RFC 8829
    /// §3.4.)
    ///
    /// We can't actually generate a real answer SDP without
    /// running webrtcbin's `create-answer` (which needs a
    /// configured pipeline with media), so we cheat by re-using
    /// the offer SDP shape. webrtcbin accepts it as a
    /// syntactically-valid answer and transitions accordingly —
    /// good enough to verify the dispatch arm reaches the bin.
    /// SetLocalDescription dispatch reaches the underlying
    /// `webrtcbin.emit("set-local-description", …)` call without
    /// panicking. Same scope-caveat as the SetRemoteDescription
    /// test above.
    #[test]
    fn set_local_description_with_valid_sdp_dispatches_without_panic() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        runtime.dispatch(Action::SetLocalDescription {
            sdp: MIN_OFFER_SDP.into(),
        });
        assert!(runtime.inner.borrow().webrtcbin.is_some());
    }

    /// CreateAnswer dispatch emits the `create-answer` signal with
    /// a real `gst::Promise`. The promise never fires in this test
    /// (the bin isn't in `Playing` and has no codecs configured),
    /// but the dispatch arm must complete without panicking.
    #[test]
    fn create_answer_dispatches_without_panic() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        runtime.dispatch(Action::CreateAnswer);
        assert!(runtime.inner.borrow().webrtcbin.is_some());
    }

    /// Regression: `answer_generation` must bump on every
    /// `Action::SetRemoteDescription` dispatch. The generation is
    /// the load-bearing piece of the renegotiation race guard —
    /// `set-remote-description`'s promise chain captures the
    /// value at issue time, and so does the chained
    /// `create-answer`'s promise. A renegotiation `SetRemote` in
    /// flight must invalidate any older chain's eventual
    /// `WebrtcAnswerCreated` dispatch.
    ///
    /// Originally pinned to `Action::CreateAnswer`; moved to
    /// `SetRemoteDescription` when create-answer became a chained
    /// continuation of set-remote-description (see
    /// `apply_remote_offer_and_chain_answer`). `Action::CreateAnswer`
    /// is now a no-op and must NOT bump the generation — a bump
    /// there would corrupt the generation captured by the in-flight
    /// chain's promise.
    #[test]
    fn set_remote_description_dispatch_bumps_answer_generation() {
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(NoopBackend));
        let before = runtime.inner.borrow().answer_generation;
        runtime.dispatch(Action::SetRemoteDescription {
            sdp: "v=0\r\n".into(),
        });
        let after_one = runtime.inner.borrow().answer_generation;
        assert_eq!(
            after_one,
            before.wrapping_add(1),
            "first SetRemoteDescription dispatch must bump the generation by 1"
        );
        runtime.dispatch(Action::SetRemoteDescription {
            sdp: "v=0\r\n".into(),
        });
        let after_two = runtime.inner.borrow().answer_generation;
        assert_eq!(
            after_two,
            before.wrapping_add(2),
            "second SetRemoteDescription dispatch must bump the generation again"
        );
    }

    /// `Action::CreateAnswer` is a no-op after the chain refactor —
    /// the `set-remote-description` promise drives `create-answer`
    /// on its resolution. Verify the runtime doesn't bump the
    /// generation a second time when the state machine emits both
    /// actions back-to-back.
    #[test]
    fn create_answer_dispatch_is_now_a_noop_on_generation() {
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(NoopBackend));
        let before = runtime.inner.borrow().answer_generation;
        runtime.dispatch(Action::CreateAnswer);
        let after = runtime.inner.borrow().answer_generation;
        assert_eq!(
            after, before,
            "Action::CreateAnswer is a no-op; the chain handles it"
        );
    }

    // ------------------------------------------------------------------
    // Phase 8.C step 4 — ICE dispatch + on-ice-candidate plumbing.
    // ------------------------------------------------------------------

    /// Well-formed ICE candidate JSON for the dispatch tests.
    /// Mirrors the shape webrtcbin emits + Janus serves: a
    /// candidate string + sdpMLineIndex + sdpMid. The exact
    /// candidate bytes don't matter — they just need to round-trip
    /// through `voice::ice::parse` cleanly.
    const VALID_ICE_JSON: &str = concat!(
        r#"{"candidate":"candidate:1 1 UDP 2113937151 192.0.2.1 12345 typ host","#,
        r#""sdpMid":"audio0","#,
        r#""sdpMLineIndex":0}"#,
    );

    /// AddRemoteIce dispatch with a valid candidate JSON reaches
    /// `webrtcbin.emit("add-ice-candidate", …)` without panicking.
    /// We can't observe the bin's reaction (the bin needs the
    /// pipeline running + a remote DTLS state to actually accept
    /// the candidate), so the assertion is structural: the
    /// dispatch completes and the bin is still alive.
    #[test]
    fn add_remote_ice_with_valid_json_dispatches_without_panic() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        runtime.dispatch(Action::AddRemoteIce {
            candidate_json: VALID_ICE_JSON.into(),
        });
        assert!(runtime.inner.borrow().webrtcbin.is_some());
    }

    /// Malformed JSON early-returns through
    /// `voice::ice::parse` without panicking. The dispatch arm
    /// logs the failure on the GStreamer warning channel and
    /// aborts; the bin stays alive for subsequent candidates.
    #[test]
    fn add_remote_ice_with_garbage_json_logs_and_drops_candidate() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        runtime.dispatch(Action::AddRemoteIce {
            candidate_json: "not actually json".into(),
        });
        assert!(runtime.inner.borrow().webrtcbin.is_some());
    }

    /// End-of-candidates marker (empty `candidate` string) is
    /// dropped at the bin level — webrtcbin doesn't need an
    /// explicit signal, gathering finishes when the remote
    /// stops sending. The dispatch must not panic on the empty
    /// candidate, which is the failure mode of a naïve
    /// `unwrap()` on `candidate.as_deref()`.
    #[test]
    fn add_remote_ice_end_of_candidates_marker_is_dropped() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        let eoc_json = r#"{"candidate":"","sdpMid":"audio0","sdpMLineIndex":0}"#;
        runtime.dispatch(Action::AddRemoteIce {
            candidate_json: eoc_json.into(),
        });
        assert!(runtime.inner.borrow().webrtcbin.is_some());
    }

    /// Missing `sdpMLineIndex` is a defensive drop — the bin
    /// signal requires the mline index. We can't synthesise it
    /// from `sdpMid` reliably, so we log and skip the candidate.
    #[test]
    fn add_remote_ice_missing_mline_index_is_dropped() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        let no_mline = concat!(
            r#"{"candidate":"candidate:1 1 UDP 100 192.0.2.1 12345 typ host","#,
            r#""sdpMid":"audio0"}"#,
        );
        runtime.dispatch(Action::AddRemoteIce {
            candidate_json: no_mline.into(),
        });
        assert!(runtime.inner.borrow().webrtcbin.is_some());
    }

    /// Pipeline-less runtime: AddRemoteIce dispatch early-returns
    /// cleanly when there's no bin to feed. Same shape as the
    /// existing pipeline-less SDP test.
    #[test]
    fn pipeline_less_runtime_no_ops_add_remote_ice() {
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(NoopBackend));
        runtime.dispatch(Action::AddRemoteIce {
            candidate_json: VALID_ICE_JSON.into(),
        });
        runtime.dispatch(Action::AddRemoteIce {
            candidate_json: "garbage".into(),
        });
        assert_eq!(runtime.state(), SessionState::Idle);
    }

    /// `lookup_local_sdp_mid` returns `None` when the bin's
    /// `local-description` hasn't been set yet — the on-ice-candidate
    /// callback's path for "candidate fires before
    /// set-local-description has landed". Without this graceful
    /// return the callback would unwrap on `None` and panic the
    /// worker thread.
    ///
    /// Driven against a fresh, unconfigured `webrtcbin` element
    /// because that's the cleanest way to put it in the "no
    /// local-description" state from a unit test. We don't try
    /// to assert the happy path here — that requires a full
    /// SDP round-trip, which lives in the Tier 3 voice
    /// integration test (Phase 8.F).
    #[test]
    fn lookup_local_sdp_mid_returns_none_without_local_description() {
        assert!(crate::init());
        let bin = gstreamer::ElementFactory::make("webrtcbin")
            .build()
            .expect("webrtcbin element must be constructable");
        assert_eq!(lookup_local_sdp_mid(&bin, 0), None);
        assert_eq!(lookup_local_sdp_mid(&bin, 42), None);
    }

    // ------------------------------------------------------------------
    // Phase 8.C step 5 — receive-pad dispatch.
    // ------------------------------------------------------------------

    /// Pipeline-less runtime: StartReceivePipeline dispatch is a
    /// silent no-op — there's no pipeline to add the bin to and
    /// no pending pad to link. The state machine emits these
    /// actions unconditionally; the runtime is the layer that
    /// decides whether to drive GStreamer.
    #[test]
    fn pipeline_less_runtime_no_ops_start_receive_pipeline() {
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(NoopBackend));
        runtime.dispatch(Action::StartReceivePipeline {
            mid: "audio0".into(),
            user_id: 42,
        });
        // Bookkeeping confirmation: nothing got stashed in
        // receive_bins.
        assert!(runtime.inner.borrow().receive_bins.is_empty());
    }

    /// StartReceivePipeline with no matching pad in
    /// `pending_pads` is a silent no-op. Happens when the state
    /// machine emits the action speculatively or when pad-added
    /// fired for a mid we've already processed; either way we
    /// shouldn't crash.
    #[test]
    fn start_receive_pipeline_without_pending_pad_is_a_noop() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        runtime.dispatch(Action::StartReceivePipeline {
            mid: "unknown-mid".into(),
            user_id: 7,
        });
        assert!(runtime.inner.borrow().receive_bins.is_empty());
    }

    /// Regression: a speculative StartReceivePipeline (no pending
    /// pad for the mid) must NOT remove an already-installed
    /// receive bin for the same mid. The earlier dispatch removed
    /// `receive_bins[&mid]` up front, then early-returned when
    /// pad was None — orphaning a still-playing leg on every
    /// no-pad dispatch.
    #[test]
    fn start_receive_pipeline_without_pad_leaves_existing_bin_intact() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        // Pre-seed receive_bins with a sentinel bin to stand in
        // for an already-installed receive leg. Use a bare
        // `gst::Bin` — it's not linked into the pipeline, which
        // is exactly the property we care about preserving (the
        // dispatch must not yank it out of the bookkeeping map).
        let sentinel = gstreamer::Bin::with_name("sentinel-leg");
        runtime
            .inner
            .borrow_mut()
            .receive_bins
            .insert("audio0".into(), sentinel.clone());

        // Dispatch with no matching pad in pending_pads. The
        // dispatch's no-op path used to remove the bin from
        // receive_bins.
        runtime.dispatch(Action::StartReceivePipeline {
            mid: "audio0".into(),
            user_id: 42,
        });

        let inner = runtime.inner.borrow();
        let surviving = inner
            .receive_bins
            .get("audio0")
            .expect("existing bin must still be in receive_bins");
        // Same Bin instance, not a freshly-allocated replacement.
        assert!(
            sentinel.as_ptr() == surviving.as_ptr(),
            "the pre-seeded bin must be the one still parked under audio0"
        );
    }

    /// StopReceivePipeline with no matching bin in
    /// `receive_bins` is a silent no-op. Symmetric to the
    /// no-pending-pad shape above.
    #[test]
    fn stop_receive_pipeline_without_matching_bin_is_a_noop() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        runtime.dispatch(Action::StopReceivePipeline {
            mid: "never-linked".into(),
        });
        assert!(runtime.inner.borrow().receive_bins.is_empty());
    }

    // ------------------------------------------------------------------
    // Phase 8.C step 8 — pipeline bus + connection-state notify.
    // ------------------------------------------------------------------

    /// Map every `WebRTCPeerConnectionState` variant to the matching
    /// `hxvoice::ConnectionState`. Pins the contract that downstream
    /// consumers (`(Connecting, WebrtcConnectionStateChanged
    /// { state: Connected }) => Connected`, etc.) rely on.
    #[test]
    fn map_peer_connection_state_covers_every_variant() {
        use gstreamer_webrtc::WebRTCPeerConnectionState as Src;
        use hxvoice::event::ConnectionState as Dst;
        // The state machine treats "New" and "Closed" identically:
        // both mean "no live peer connection", neither triggers a
        // transition. Map both to the same Dst::Closed for the
        // mapper's external contract.
        assert!(matches!(map_peer_connection_state(Src::New), Dst::Closed));
        assert!(matches!(
            map_peer_connection_state(Src::Closed),
            Dst::Closed
        ));
        assert!(matches!(
            map_peer_connection_state(Src::Connecting),
            Dst::Connecting
        ));
        assert!(matches!(
            map_peer_connection_state(Src::Connected),
            Dst::Connected
        ));
        assert!(matches!(
            map_peer_connection_state(Src::Disconnected),
            Dst::Disconnected
        ));
        assert!(matches!(map_peer_connection_state(Src::Failed), Dst::Failed));
    }

    /// Constructing a runtime with a real pipeline runs
    /// `attach_pipeline_bus_watch` against the pipeline's bus
    /// without panicking. We can't easily observe "the watch
    /// callback fires" from a unit test (it needs the main loop
    /// running and a real GStreamer-source message to trigger),
    /// but we CAN pin that the construction sequence completes
    /// cleanly even though step 8 adds a new wiring step. The
    /// full bus-message + callback path is a Tier 3 concern
    /// (Phase 8.F, real Janus session emits real bus errors on
    /// real failure modes).
    #[test]
    fn pipeline_built_runtime_construction_includes_bus_watch() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        // The pipeline has a reachable bus and construction
        // completed without panicking. That's all this assertion
        // proves — it does NOT verify the watch closure was
        // actually installed by `attach_pipeline_bus_watch`
        // (that requires observing a side effect of the closure
        // running, which the watch's log-only behaviour
        // doesn't surface). The full bus-message + callback
        // path is a Tier 3 concern (Phase 8.F, real Janus
        // session emits real bus errors on real failure modes).
        assert!(runtime
            .inner
            .borrow()
            .pipeline
            .as_ref()
            .expect("pipeline must be present")
            .bus()
            .is_some());
    }

    /// Capture site for `CallbackBackend` unit tests. `user_data`
    /// points at one of these; `extern "C" fn callback_capture`
    /// reads it back. RefCell-not-Mutex because the runtime is
    /// !Send and these tests stay on one thread.
    struct CapturedFrames(RefCell<Vec<(u32, Vec<u8>)>>);

    unsafe extern "C" fn callback_capture(
        user_data: *mut core::ffi::c_void,
        opcode: u32,
        body: *const u8,
        body_len: usize,
    ) {
        // SAFETY: tests below pass a `&CapturedFrames` cast to
        // `*mut c_void` and keep it alive across the callback
        // invocation. `body` is a slice the backend owns for
        // the call duration.
        let captured = unsafe { &*(user_data as *const CapturedFrames) };
        let bytes = if body.is_null() || body_len == 0 {
            Vec::new()
        } else {
            unsafe { core::slice::from_raw_parts(body, body_len) }.to_vec()
        };
        captured.0.borrow_mut().push((opcode, bytes));
    }

    #[test]
    fn callback_backend_forwards_opcode_and_body() {
        let captured = CapturedFrames(RefCell::new(Vec::new()));
        let user_data =
            &captured as *const CapturedFrames as *mut core::ffi::c_void;
        let mut backend = CallbackBackend::new(user_data, Some(callback_capture));

        backend.send_wire_frame(603, b"v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\n");
        backend.send_wire_frame(604, b"{\"candidate\":\"\",\"sdpMid\":\"0\"}");

        let frames = captured.0.borrow();
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0].0, 603);
        assert_eq!(&frames[0].1[..3], b"v=0");
        assert_eq!(frames[1].0, 604);
        assert!(frames[1].1.starts_with(b"{\"candidate"));
    }

    #[test]
    fn callback_backend_with_no_callback_drops_silently() {
        // A backend with `send_wire_frame_cb == None` must NOT
        // attempt to dispatch. Pass a NULL user_data so a buggy
        // implementation that does dispatch on None segfaults
        // loudly instead of writing to junk memory.
        let mut backend = CallbackBackend::new(core::ptr::null_mut(), None);
        backend.send_wire_frame(603, b"unused");
        // No assertion beyond "didn't crash" — the !Some branch is
        // a one-line early-return and unit-testing the absence of
        // a call is what we have here.
    }

    #[test]
    fn callback_backend_forwards_empty_body() {
        // 600 JOIN / 601 LEAVE are 4-byte BE cid, but Action's
        // body slice may be any length the encoder produced.
        // Pin that an empty slice routes through cleanly.
        let captured = CapturedFrames(RefCell::new(Vec::new()));
        let user_data =
            &captured as *const CapturedFrames as *mut core::ffi::c_void;
        let mut backend = CallbackBackend::new(user_data, Some(callback_capture));

        backend.send_wire_frame(600, &[]);

        let frames = captured.0.borrow();
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].0, 600);
        assert!(frames[0].1.is_empty());
    }

    // ---- Signal-bridge unit tests ----

    /// Capture site for state-changed signals.
    struct CapturedStates(RefCell<Vec<u32>>);
    /// Capture site for mute-changed signals.
    struct CapturedMutes(RefCell<Vec<i32>>);

    unsafe extern "C" fn state_capture(
        user_data: *mut core::ffi::c_void,
        state: u32,
    ) {
        let captured = unsafe { &*(user_data as *const CapturedStates) };
        captured.0.borrow_mut().push(state);
    }

    unsafe extern "C" fn mute_capture(
        user_data: *mut core::ffi::c_void,
        muted: i32,
    ) {
        let captured = unsafe { &*(user_data as *const CapturedMutes) };
        captured.0.borrow_mut().push(muted);
    }

    #[test]
    fn session_state_to_ffi_covers_every_variant() {
        use hxvoice::state::SessionState as S;
        // Pin every Rust state's discriminant to its C-side
        // mirror. A new SessionState variant added without
        // updating session_state_to_ffi would otherwise silently
        // route through the wildcard's debug_assert.
        assert_eq!(session_state_to_ffi(S::Idle), 0);
        assert_eq!(session_state_to_ffi(S::JoinSent), 1);
        assert_eq!(session_state_to_ffi(S::OfferPending), 2);
        assert_eq!(session_state_to_ffi(S::Connecting), 3);
        assert_eq!(session_state_to_ffi(S::Connected), 4);
        assert_eq!(session_state_to_ffi(S::Leaving), 5);
    }

    #[test]
    fn callback_backend_forwards_state_changed_signal() {
        use hxvoice::state::SessionState;
        let captured = CapturedStates(RefCell::new(Vec::new()));
        let user_data =
            &captured as *const CapturedStates as *mut core::ffi::c_void;
        let signals = SignalCallbacks {
            state_changed: Some(state_capture),
            mute_changed: None,
            speaker_changed: None,
            error: None,
        };
        let mut backend = CallbackBackend::new_with_signals(
            user_data,
            None,
            signals,
        );

        backend.emit_signal(
            SignalKind::StateChanged,
            SignalPayload::StateChanged {
                new_state: SessionState::JoinSent,
            },
        );
        backend.emit_signal(
            SignalKind::StateChanged,
            SignalPayload::StateChanged {
                new_state: SessionState::Connected,
            },
        );
        backend.emit_signal(
            SignalKind::StateChanged,
            SignalPayload::StateChanged {
                new_state: SessionState::Leaving,
            },
        );

        assert_eq!(captured.0.borrow().as_slice(), &[1, 4, 5]);
    }

    #[test]
    fn callback_backend_forwards_mute_changed_signal() {
        let captured = CapturedMutes(RefCell::new(Vec::new()));
        let user_data =
            &captured as *const CapturedMutes as *mut core::ffi::c_void;
        let signals = SignalCallbacks {
            state_changed: None,
            mute_changed: Some(mute_capture),
            speaker_changed: None,
            error: None,
        };
        let mut backend = CallbackBackend::new_with_signals(
            user_data,
            None,
            signals,
        );

        backend.emit_signal(
            SignalKind::MuteChanged,
            SignalPayload::MuteChanged { muted: true },
        );
        backend.emit_signal(
            SignalKind::MuteChanged,
            SignalPayload::MuteChanged { muted: false },
        );

        assert_eq!(captured.0.borrow().as_slice(), &[1, 0]);
    }

    #[test]
    fn callback_backend_drops_signal_with_no_subscriber() {
        // None callback in SignalCallbacks must not be invoked.
        // Pair with NULL user_data so a wayward invocation would
        // segfault.
        let mut backend = CallbackBackend::new_with_signals(
            core::ptr::null_mut(),
            None,
            SignalCallbacks::none(),
        );
        backend.emit_signal(
            SignalKind::StateChanged,
            SignalPayload::StateChanged {
                new_state: hxvoice::state::SessionState::Connected,
            },
        );
        backend.emit_signal(
            SignalKind::MuteChanged,
            SignalPayload::MuteChanged { muted: true },
        );
        // No assertion beyond "didn't crash".
    }

    use core::sync::atomic::{AtomicBool, Ordering};

    /// Static flags the must-not-fire callbacks flip on invocation.
    /// The callbacks deliberately do NOT dereference `user_data` —
    /// a regression that invoked them would otherwise UB-read
    /// through a type-confused pointer and turn the test failure
    /// into nondeterministic flake instead of a clean assertion.
    static MUTE_FIRED: AtomicBool = AtomicBool::new(false);
    static STATE_FIRED: AtomicBool = AtomicBool::new(false);

    unsafe extern "C" fn mute_must_not_fire(
        _user_data: *mut core::ffi::c_void,
        _muted: i32,
    ) {
        MUTE_FIRED.store(true, Ordering::SeqCst);
    }

    unsafe extern "C" fn state_must_not_fire(
        _user_data: *mut core::ffi::c_void,
        _state: u32,
    ) {
        STATE_FIRED.store(true, Ordering::SeqCst);
    }

    #[test]
    fn callback_backend_ignores_unsubscribed_signal_kinds() {
        // RoomStatus + Error don't have C-side subscriber slots
        // yet; emitting them with subscribed state/mute callbacks
        // must not call them either. Use flag-only callbacks so
        // the assertion fails deterministically on a regression
        // rather than UB-reading a type-confused user_data
        // pointer.
        STATE_FIRED.store(false, Ordering::SeqCst);
        MUTE_FIRED.store(false, Ordering::SeqCst);

        let signals = SignalCallbacks {
            state_changed: Some(state_must_not_fire),
            mute_changed: Some(mute_must_not_fire),
            speaker_changed: None,
            error: None,
        };
        let mut backend = CallbackBackend::new_with_signals(
            core::ptr::null_mut(),
            None,
            signals,
        );
        backend.emit_signal(
            SignalKind::RoomStatus,
            SignalPayload::RoomStatus {
                cid: 42,
                connection_state:
                    hxvoice::event::ConnectionState::Connected,
            },
        );
        backend.emit_signal(
            SignalKind::Error,
            SignalPayload::Error { text: "oops".into() },
        );
        assert!(
            !STATE_FIRED.load(Ordering::SeqCst),
            "state_changed must not fire for RoomStatus / Error"
        );
        assert!(
            !MUTE_FIRED.load(Ordering::SeqCst),
            "mute_changed must not fire for RoomStatus / Error"
        );
    }
}
