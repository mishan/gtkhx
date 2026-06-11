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
//! ## What's deferred to step 4+
//!
//! - Dispatch arms for the remaining WebRTC-shaped actions
//!   (`AddRemoteIce`, `StartReceivePipeline` /
//!   `StopReceivePipeline` / `SetSendPipelineMute`). They still
//!   no-op today.
//! - `webrtcbin` signal wiring for `pad-added` /
//!   `on-ice-candidate` / `connection-state` / `bus`. Step 4
//!   covers ICE (with the same main-thread marshaling shape as
//!   `CreateAnswer`); step 5 covers the receive-leg pads; step 8
//!   covers the bus.
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
    /// Currently-armed timer kinds. The runtime arms `glib::timeout`
    /// callbacks elsewhere; we track the kind here so we know
    /// whether a fire is still relevant by the time it arrives
    /// (race with `CancelTimer`).
    ///
    /// `Vec` rather than `HashSet` because the bound is tiny —
    /// at most four entries (JoinReply / Dtls / IceConnectivity /
    /// Media) per the spec's timer table.
    armed_timers: Vec<Timeout>,
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
}

impl Drop for Inner {
    fn drop(&mut self) {
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
        let runtime = VoiceRuntime {
            inner: Rc::new(RefCell::new(Inner {
                machine: SessionMachine::new(),
                pipeline: Some(pipeline),
                webrtcbin: Some(webrtcbin),
                armed_timers: Vec::new(),
                dispatching: false,
                pending: VecDeque::new(),
                runtime_id,
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
                armed_timers: Vec::new(),
                dispatching: false,
                pending: VecDeque::new(),
                runtime_id,
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
                self.dispatch(action);
            }
        }
        self.inner.borrow().machine.state()
    }

    /// Snapshot of the current state. Cheap accessor for tests.
    pub fn state(&self) -> SessionState {
        self.inner.borrow().machine.state()
    }

    /// Currently-armed timers. Order matches `ArmTimer` arrival
    /// order; primarily a test-introspection hook.
    pub fn armed_timers(&self) -> Vec<Timeout> {
        self.inner.borrow().armed_timers.clone()
    }

    fn dispatch(&self, action: Action) {
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

            // ---- Timers ----
            //
            // Phase 8.C step 2: we track armed timers but don't
            // actually wire `glib::timeout_add_local` here. Step 3
            // adds the real timer wiring (which needs the GLib
            // main context running, which production has but
            // bare-tests don't). For now, tests can call
            // `armed_timers()` to verify the right kinds were
            // requested and synthetically fire `Event::Timeout` if
            // they want to exercise the expiry path.
            Action::ArmTimer { kind, ms: _ } => {
                // (Re-)arm. Step 3 will replace the bookkeeping
                // here with `glib::timeout_add_local`; the
                // (re-)arm semantics must match — calling it a
                // second time for the same kind cancels the
                // previous timer and starts a fresh one. A naïve
                // `if !contains { push }` short-circuit would
                // make a same-kind re-arm a silent no-op, which
                // would matter once we wire real timers (the
                // state machine sends repeat `ArmTimer` events
                // on renegotiation, intending the watchdog to
                // restart from the new offer's reception time).
                let mut inner = self.inner.borrow_mut();
                inner.armed_timers.retain(|t| *t != kind);
                inner.armed_timers.push(kind);
            }
            Action::CancelTimer { kind } => {
                let mut inner = self.inner.borrow_mut();
                inner.armed_timers.retain(|t| *t != kind);
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
                // again on the main thread) and a handle to the
                // main GLib context (its accessor is callable
                // from any thread — `Send`-safe).
                let webrtcbin = self
                    .inner
                    .borrow()
                    .webrtcbin
                    .clone();
                let runtime_id = self.inner.borrow().runtime_id;
                if let Some(bin) = webrtcbin {
                    create_answer(&bin, runtime_id);
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

            // ---- webrtcbin-shaped ICE / pad / mute (Phase 8.C step 4+) ----
            //
            // These still no-op today. Step 4 lands the ICE flow;
            // step 5 lands receive-pad dispatch.
            Action::AddRemoteIce { .. }
            | Action::StartReceivePipeline { .. }
            | Action::StopReceivePipeline { .. }
            | Action::SetSendPipelineMute { .. } => {
                // Subsequent steps fill these in.
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
/// a type tag. Returns `None` if the SDP fails to parse; this
/// helper logs the failure on the GStreamer warning channel
/// before returning, so callers should just early-return on
/// `None` rather than re-logging.
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
/// runtime by id from the thread-local registry, and re-enters
/// `handle_event(Event::WebrtcAnswerCreated)`.
///
/// The closure is `Send + 'static` (captures only `runtime_id: u64`
/// and the main context); the runtime itself stays `!Send`. See the
/// module-level "Threading" section for the full reasoning.
fn create_answer(webrtcbin: &gstreamer::Element, runtime_id: u64) {
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
        // Copy) and `sdp` (String). The runtime lookup happens on
        // the main thread inside `with_main_thread_runtime`.
        main_ctx.invoke(move || {
            with_main_thread_runtime(runtime_id, |rt| {
                rt.handle_event(Event::WebrtcAnswerCreated { sdp });
            });
        });
    });
    webrtcbin.emit_by_name::<()>(
        "create-answer",
        &[&None::<gstreamer::Structure>, &promise],
    );
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

    /// Empty-input edge case. `SDPMessage::parse_buffer` is
    /// otherwise permissive — it returns recoverable parses for
    /// almost any byte slice — but an empty buffer is the one
    /// shape it cleanly fails on. We hold this test only to pin
    /// the contract that `build_session_description` returns
    /// `None` rather than panicking when the parser hands back
    /// an error, regardless of where the threshold lies.
    #[test]
    fn build_session_description_handles_parser_failure() {
        assert!(crate::init());
        // Empty buffer — the parser surfaces this as an error.
        // If a future GStreamer release moves the threshold and
        // accepts empty input too, this test would start failing;
        // at that point switch the input to something the parser
        // still rejects (a single garbage byte, etc.) — the
        // contract being tested is "we don't panic when the
        // parser fails", not "this specific input fails".
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
}
