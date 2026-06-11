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
use std::sync::atomic::{AtomicU64, Ordering};

use gstreamer::prelude::*;
use gstreamer_webrtc::WebRTCSDPType;

use hxvoice::action::{Action, SignalKind, SignalPayload};
use hxvoice::event::{Event, Timeout};
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
pub type SendWireFrameCallback = unsafe extern "C" fn(
    user_data: *mut core::ffi::c_void,
    opcode: u32,
    body: *const u8,
    body_len: usize,
);

/// Backend implementation that bridges to a C callback. Production
/// uses this with `voice_runtime_send_wire_frame_cb` in voice_panel.c
/// so the state machine's outbound voice opcodes reach `hlwrite_chunks`
/// via the existing `hx_send_voice_*` helpers.
///
/// `emit_signal` and `tear_down` are stubbed out for now — the
/// runtime-to-UI signal path lands in a follow-up that wires
/// `GtkhxSession::emit_*`. Until then, the C side reads runtime
/// state synchronously from the UI click handlers and the
/// optimistic-UI fallback covers the post-click feedback.
///
/// Not `Send` because `user_data` is a raw pointer; the runtime's
/// `Backend` trait doesn't require `Send` and the entire dispatch
/// loop runs main-thread-only.
pub struct CallbackBackend {
    user_data: *mut core::ffi::c_void,
    send_wire_frame_cb: Option<SendWireFrameCallback>,
}

impl CallbackBackend {
    /// Construct a backend that calls `send_wire_frame_cb` for
    /// every `Action::SendWireFrame` action. A `None` callback
    /// makes the backend behave like `NoopBackend` for that
    /// surface.
    pub fn new(
        user_data: *mut core::ffi::c_void,
        send_wire_frame_cb: Option<SendWireFrameCallback>,
    ) -> Self {
        Self {
            user_data,
            send_wire_frame_cb,
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
    fn emit_signal(&mut self, _kind: SignalKind, _payload: SignalPayload) {
        // Stubbed: the runtime-to-UI signal bridge lands in a
        // follow-up. For now the C side reads runtime state
        // synchronously from the UI click handlers.
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
}

impl Drop for Inner {
    fn drop(&mut self) {
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
        let webrtcbin = gstreamer::ElementFactory::make("webrtcbin")
            .name("hxvoice-webrtcbin")
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
        let runtime_id = NEXT_RUNTIME_ID.fetch_add(1, Ordering::Relaxed);
        // Wire the on-ice-candidate signal BEFORE registering.
        // The signal callback only looks the runtime up via
        // `with_main_thread_runtime` (which acquires the registry
        // entry lazily), so the order is safe in either direction;
        // doing it pre-register keeps the construction sequence
        // strictly linear.
        connect_on_ice_candidate(&webrtcbin, runtime_id);
        connect_pad_added(&webrtcbin, runtime_id);
        connect_connection_state_notify(&webrtcbin, runtime_id);
        let bus_watch_guard = attach_pipeline_bus_watch(&pipeline);
        let runtime = VoiceRuntime {
            inner: Rc::new(RefCell::new(Inner {
                machine: SessionMachine::new(),
                pipeline: Some(pipeline),
                webrtcbin: Some(webrtcbin),
                armed_timer_sources: HashMap::new(),
                dispatching: false,
                pending: VecDeque::new(),
                runtime_id,
                answer_generation: 0,
                pending_pads: HashMap::new(),
                receive_bins: HashMap::new(),
                bus_watch_guard,
            })),
            backend: Rc::new(RefCell::new(backend)),
        };
        register(&runtime);
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
            })),
            backend: Rc::new(RefCell::new(backend)),
        };
        register(&runtime);
        runtime
    }

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

            let actions = {
                let mut inner = self.inner.borrow_mut();
                inner.machine.step(event)
            };
            for action in actions {
                self.dispatch_inner(action);
            }
        }
        self.inner.borrow().machine.state()
    }

    /// Snapshot of the current state. Cheap accessor for tests.
    pub fn state(&self) -> SessionState {
        self.inner.borrow().machine.state()
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
                let webrtcbin = self
                    .inner
                    .borrow()
                    .webrtcbin
                    .clone();
                if let Some(bin) = webrtcbin {
                    apply_remote_offer(&bin, &sdp);
                }
            }
            Action::CreateAnswer => {
                // Capture data needed by the (Send) promise
                // closure: the runtime id (to find ourselves
                // again on the main thread), the answer
                // generation (so we can ignore stale
                // resolutions if a later offer arrives before
                // the old promise fires — see below), and a
                // handle to the main GLib context (its accessor
                // is callable from any thread — `Send`-safe).
                //
                // Bump the generation BEFORE handing the
                // promise to webrtcbin. The state machine's
                // OfferPending+SdpOfferReceived arm
                // re-issues CreateAnswer without changing
                // state; without the bump-and-compare the old
                // promise's resolution would still match the
                // current generation and send a stale 603 for
                // the wrong offer.
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
                if let Some(bin) = webrtcbin {
                    create_answer(&bin, runtime_id, generation);
                }
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
                if let Some(bin) = start_receive_bin(&pipeline, &pad, &mid)
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

            // ---- Mute dispatch (Phase 8.C step 6+) ----
            //
            // The send leg lands with step 6 (the audio capture
            // pipeline + RTP send chain); the mute arm needs
            // the send leg's element handles to drop buffers,
            // so it can't usefully land before then.
            Action::SetSendPipelineMute { .. } => {
                // Step 6 fills this in.
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
/// offer SDP and emits `webrtcbin.set-remote-description` with a
/// null promise (we don't currently track when remote-desc has
/// finished applying; the state machine just sequences
/// `CreateAnswer` immediately after).
fn apply_remote_offer(webrtcbin: &gstreamer::Element, sdp: &str) {
    let Some(desc) = build_session_description(sdp, WebRTCSDPType::Offer)
    else {
        return;
    };
    webrtcbin.emit_by_name::<()>(
        "set-remote-description",
        &[&desc, &None::<gstreamer::Promise>],
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
    runtime_id: u64,
) {
    let main_ctx = gstreamer::glib::MainContext::default();
    webrtcbin.connect_pad_added(move |_bin, pad| {
        if pad.direction() != gstreamer::PadDirection::Src {
            // Sink pads (request pads from the send leg) come
            // through this signal too; nothing to do with them.
            return;
        }
        let mid = match lookup_pad_mid(pad) {
            Some(m) => m,
            None => {
                gstreamer::warning!(
                    gstreamer::CAT_RUST,
                    "hxvoice: pad-added with no resolvable mid \
                     (pad={}); dropping",
                    pad.name()
                );
                return;
            }
        };
        // Clone the pad — gst::Pad is Send + Sync (GObject), so
        // we can hand it across the main-thread hop. Capture
        // (mid, pad) in the closure; both are Send.
        let pad = pad.clone();
        let main_ctx = main_ctx.clone();
        main_ctx.invoke(move || {
            with_main_thread_runtime(runtime_id, |rt| {
                rt.inner
                    .borrow_mut()
                    .pending_pads
                    .insert(mid.clone(), pad);
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
                    // The local send leg's transceiver shows up
                    // here with `mid == "send"` and the state
                    // machine intentionally drops it
                    // (hxvoice::state). Don't warn for that —
                    // it's expected on every join.
                    if stale.is_some() && mid != "send" {
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
) -> Option<gstreamer::Bin> {
    let bin_name = format!("hxvoice-recv-{mid}");
    let bin = match crate::audio::make_receive_bin(&bin_name) {
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
    if bin.sync_state_with_parent().is_err() {
        // Not fatal — the pipeline state machine will retry on
        // its own state-change pass. Log so an operator can see
        // it if audio doesn't materialize.
        gstreamer::warning!(
            gstreamer::CAT_RUST,
            "hxvoice: sync_state_with_parent failed on receive bin \
             (mid={mid}); audio may not flow until next state change"
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
        assert_eq!(runtime.state(), SessionState::Leaving);
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
        assert_eq!(runtime.state(), SessionState::Leaving);
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
        // the state machine transitioned all the way to Leaving
        // (JOIN sets active_cid to 42 → LEAVE matches active_cid
        // and walks to Leaving).
        let backend_slot = runtime_slot.borrow();
        let runtime_ref = backend_slot.as_ref().unwrap();
        assert_eq!(runtime_ref.state(), SessionState::Leaving);
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
    /// returns; the bin stays in its prior state.
    #[test]
    fn set_remote_description_with_garbage_sdp_is_a_silent_noop() {
        assert!(crate::init());
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        let before: gstreamer_webrtc::WebRTCSignalingState =
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
        let after: gstreamer_webrtc::WebRTCSignalingState =
            runtime
                .inner
                .borrow()
                .webrtcbin
                .as_ref()
                .unwrap()
                .property("signaling-state");
        assert_eq!(
            before, after,
            "garbage SDP must not transition signaling-state"
        );
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

    /// Regression (Copilot review #3): `Action::CreateAnswer`
    /// must bump `answer_generation` on every dispatch. The
    /// generation is the load-bearing piece of the
    /// renegotiation race guard — promise change-funcs capture
    /// the value at issue time and compare it on the main
    /// thread, so we need each new CreateAnswer to be a fresh
    /// generation to discriminate stale resolutions.
    ///
    /// Driven through `dispatch` rather than the state machine
    /// because we want to exercise the runtime arm directly
    /// without depending on the offer/answer state path.
    #[test]
    fn create_answer_dispatch_bumps_answer_generation() {
        let runtime =
            VoiceRuntime::new_without_pipeline(Box::new(NoopBackend));
        let before = runtime.inner.borrow().answer_generation;
        runtime.dispatch(Action::CreateAnswer);
        let after_one = runtime.inner.borrow().answer_generation;
        assert_eq!(
            after_one,
            before.wrapping_add(1),
            "first CreateAnswer dispatch must bump the generation by 1"
        );
        runtime.dispatch(Action::CreateAnswer);
        let after_two = runtime.inner.borrow().answer_generation;
        assert_eq!(
            after_two,
            before.wrapping_add(2),
            "second CreateAnswer dispatch must bump the generation again"
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
}
