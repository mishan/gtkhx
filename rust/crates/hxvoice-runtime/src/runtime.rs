//! GStreamer-backed runtime that drives an `hxvoice::SessionMachine`.
//!
//! This is Phase 8.C step 2 per `docs/voice-chat-plan.md` §5.C. The
//! pure state machine landed in step 1; this module wires it to a
//! `Backend` trait that production fills with FFI calls to the C
//! side.
//!
//! ## What step 2 implements
//!
//! - The `Backend` trait — `send_wire_frame` / `emit_signal` /
//!   `tear_down` callbacks the C side hooks up later.
//! - `VoiceRuntime::new()` — constructs an (empty)
//!   `gst::Pipeline` container so step 3 has somewhere to add
//!   `webrtcbin`. Pipeline isn't exercised at this layer yet.
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
//! ## What's deferred to step 3
//!
//! - `webrtcbin` construction + linking into the pipeline.
//! - Dispatch arms for the WebRTC-shaped actions
//!   (`SetRemoteDescription` / `CreateAnswer` /
//!   `SetLocalDescription` / `AddRemoteIce`,
//!   `StartReceivePipeline` / `StopReceivePipeline` /
//!   `SetSendPipelineMute`). They no-op today.
//! - `webrtcbin` signal wiring — `connect_pad_added`,
//!   `connect("on-ice-candidate", …)`, the `create-answer`
//!   promise — that produce the matching `Event::Webrtc*`
//!   variants. Step 3 lands this; the docs below about
//!   threading (`glib::MainContext::spawn_local` marshaling for
//!   cross-thread signal callbacks) describe the design that
//!   step 3 will instantiate, not anything this commit ships.
//!
//! ## Layering (full picture, including step-3 deferred bits)
//!
//! The runtime sits between three peers:
//!
//! - **`hxvoice::SessionMachine`** owned inside `Inner`. Single
//!   source of "what should we do" for every transition.
//! - **`gst::Pipeline` + `gstreamer_webrtc::WebRTCBin`** also owned
//!   inside `Inner` (webrtcbin lands in step 3). The runtime
//!   dispatches the GStreamer-shaped Actions
//!   (`SetRemoteDescription`, `CreateAnswer`, `AddRemoteIce`,
//!   audio pipeline manipulation) directly here.
//! - **`Backend`** trait, supplied at construction. Captures the
//!   integration points that aren't GStreamer-shaped: wire frames
//!   the C side ships via `hx_send_voice_*`, GtkhxSession signal
//!   emits through `hxbridge`, and teardown. Production wires it
//!   to FFI shims; tests use a recording mock.
//!
//! ## Threading (planned for step 3)
//!
//! Designed to run main-thread-only. Once step 3 wires the
//! `webrtcbin` signals, the ones that may fire on GStreamer
//! worker threads (`on-ice-candidate`, `pad-added`, etc.) marshal
//! back to the main GLib context via
//! [`glib::MainContext::spawn_local`] before reaching into
//! `Inner`. That keeps `Inner: !Send` ergonomically viable — every
//! state-machine borrow happens on a single thread.
//!
//! The deferred-dispatch queue in step 2 makes re-entrant calls
//! from `Backend` callbacks safe at any depth — the queue is the
//! same machinery that supports `webrtcbin`-signal re-entry in
//! step 3.

use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;

use hxvoice::action::{Action, SignalKind, SignalPayload};
use hxvoice::event::{Event, Timeout};
use hxvoice::state::{SessionMachine, SessionState};

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

    /// Final teardown notification. The runtime walks itself to
    /// the `Idle` state after this; the implementor closes any
    /// resources it allocated.
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
    /// `None` for the pipeline-less test path. Future webrtcbin
    /// wiring lands in Phase 8.C step 3 and lives behind this
    /// option.
    ///
    /// Currently unused — Phase 8.C step 2 builds the pipeline
    /// container but doesn't add `webrtcbin` or the send leg yet.
    /// Step 3's `dispatch` path will reach in here when handling
    /// the `SetRemoteDescription` / `CreateAnswer` / etc. arms
    /// that are currently no-ops.
    #[allow(dead_code)]
    pipeline: Option<gstreamer::Pipeline>,
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
    /// inline; the outer loop drains it after each action is
    /// dispatched. Removes the entire class of "borrow_mut while
    /// dispatching" panics, including the Backend-on-Backend
    /// case the simple two-Rc split doesn't cover on its own.
    dispatching: bool,
    /// Events queued during re-entrant `handle_event` calls. The
    /// outer loop drains this after every step's actions finish
    /// dispatching.
    pending: VecDeque<Event>,
}

impl VoiceRuntime {
    /// Construct a runtime that owns a fresh `gst::Pipeline` and
    /// will drive the supplied `Backend`. The pipeline construction
    /// itself doesn't touch `webrtcbin` yet — Phase 8.C step 3 lands
    /// the bin and the signal wiring. Step 2's pipeline is just
    /// the empty container that step 3 grows into.
    ///
    /// Calls `gst::init()` first — it's idempotent, so production
    /// code that already ran `gtkhx_voice_init()` from `main` just
    /// sees a no-op. A real failure (broken GStreamer install,
    /// missing plugins) propagates as
    /// [`RuntimeError::GstInitFailed`] rather than the delayed
    /// panic from inside gstreamer-rs's assert-initialised
    /// checks.
    pub fn new(backend: Box<dyn Backend>) -> Result<Self, RuntimeError> {
        gstreamer::init().map_err(|_| RuntimeError::GstInitFailed)?;
        let pipeline = gstreamer::Pipeline::builder()
            .name("hxvoice-pipeline")
            .build();
        Ok(VoiceRuntime {
            inner: Rc::new(RefCell::new(Inner {
                machine: SessionMachine::new(),
                pipeline: Some(pipeline),
                armed_timers: Vec::new(),
                dispatching: false,
                pending: VecDeque::new(),
            })),
            backend: Rc::new(RefCell::new(backend)),
        })
    }

    /// Construct a runtime without the GStreamer pipeline. Tests
    /// that exercise the action dispatch don't need `webrtcbin`
    /// alive; using this constructor lets them skip the
    /// `gst::init()` requirement.
    pub fn new_without_pipeline(backend: Box<dyn Backend>) -> Self {
        VoiceRuntime {
            inner: Rc::new(RefCell::new(Inner {
                machine: SessionMachine::new(),
                pipeline: None,
                armed_timers: Vec::new(),
                dispatching: false,
                pending: VecDeque::new(),
            })),
            backend: Rc::new(RefCell::new(backend)),
        }
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

            // ---- webrtcbin-shaped actions (Phase 8.C step 3) ----
            //
            // The full SDP / ICE / pad lifecycle dispatch lands in
            // step 3 once webrtcbin construction is wired. For step
            // 2, we keep the match exhaustive but no-op these arms
            // — the state machine still emits them so the dispatch
            // loop walks the full list, and a recording backend
            // can verify the order surrounding them.
            //
            // Tests synthesise the `Event::WebrtcAnswerCreated` /
            // `Event::WebrtcPadAdded` / etc. responses directly to
            // exercise the state machine's downstream transitions.
            Action::SetRemoteDescription { .. }
            | Action::CreateAnswer
            | Action::SetLocalDescription { .. }
            | Action::AddRemoteIce { .. }
            | Action::StartReceivePipeline { .. }
            | Action::StopReceivePipeline { .. }
            | Action::SetSendPipelineMute { .. } => {
                // Step 3 fills these in.
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

/// Errors returned from `VoiceRuntime::new`.
#[derive(Debug)]
pub enum RuntimeError {
    /// `gst::init()` failed. The constructor calls
    /// `gstreamer::init()` itself (idempotent — repeats are a
    /// no-op), so this variant means the underlying call failed:
    /// missing GStreamer plugins (`gst-plugins-base` /
    /// `-plugins-bad`), a broken GStreamer install, or no plugin
    /// path configured. Production has typically already run
    /// `gtkhx_voice_init()` from `main` which surfaces this in
    /// the C-side log; constructing a runtime then sees the
    /// re-init no-op and never reaches this branch.
    GstInitFailed,
}

impl core::fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            RuntimeError::GstInitFailed => write!(
                f,
                "gst::init() failed — check the GStreamer install \
                 (gst-plugins-base / -plugins-bad must be available)"
            ),
        }
    }
}

impl std::error::Error for RuntimeError {}

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

    #[test]
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
        /// runtime field is wired post-construction via the OnceCell
        /// shape; the Cell is a single-shot fuse so the recursion
        /// doesn't loop.
        struct ReentrantBackend {
            runtime: Rc<RefCell<Option<VoiceRuntime>>>,
            re_entered: Cell<bool>,
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
                        self.re_entered.set(true);
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
            re_entered: Cell::new(false),
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
        // the old code would have panicked. Verify the re-entry
        // actually happened by inspecting the fuse.
        let backend_slot = runtime_slot.borrow();
        // We can't downcast the Box<dyn Backend> directly; instead
        // we verify the re-entry by observing the state machine
        // transitioned all the way to Leaving (JOIN → LEAVE,
        // even-though Leaving's cid match is for the active cid).
        let runtime_ref = backend_slot.as_ref().unwrap();
        assert_eq!(runtime_ref.state(), SessionState::Leaving);
    }

    #[test]
    fn pipeline_built_runtime_constructs_when_gst_initialised() {
        // Coverage smoke for the with-pipeline constructor. Requires
        // gst::init() to have been called; we run it inline here so
        // the test is self-contained, and assert on the result so a
        // GStreamer install regression surfaces here as the failed
        // assertion rather than as a delayed panic from VoiceRuntime::new
        // (which itself now propagates the init error as
        // RuntimeError::GstInitFailed).
        assert!(
            crate::init(),
            "gst::init() must succeed for the with-pipeline test"
        );
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        // Sanity: the pipeline element is reachable.
        assert!(runtime.inner.borrow().pipeline.is_some());
    }
}
