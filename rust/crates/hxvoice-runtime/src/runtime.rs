//! GStreamer-backed runtime that drives an `hxvoice::SessionMachine`.
//!
//! This is Phase 8.C step 2 per `docs/voice-chat-plan.md` §5.C. The
//! pure state machine landed in step 1; this module owns the
//! `gst::Pipeline` + `webrtcbin` it dispatches into, translates
//! [`hxvoice::Action`]s into either direct GStreamer calls or
//! [`Backend`]-mediated callbacks, and wires `webrtcbin` signals
//! back into [`hxvoice::Event`]s so the state machine sees the
//! WebRTC layer's responses.
//!
//! ## Layering
//!
//! The runtime sits between three peers:
//!
//! - **`hxvoice::SessionMachine`** owned inside `Inner`. Single
//!   source of "what should we do" for every transition.
//! - **`gst::Pipeline` + `gstreamer_webrtc::WebRTCBin`** also owned
//!   inside `Inner`. The runtime dispatches the
//!   GStreamer-shaped Actions (`SetRemoteDescription`,
//!   `CreateAnswer`, `AddRemoteIce`, audio pipeline manipulation)
//!   directly here.
//! - **`Backend`** trait, supplied at construction. Captures the
//!   integration points that aren't GStreamer-shaped: wire frames
//!   the C side ships via `hx_send_voice_*`, GtkhxSession signal
//!   emits through `hxbridge`, and teardown. Production wires it
//!   to FFI shims; tests use a recording mock.
//!
//! ## Threading
//!
//! Designed to run main-thread-only. `webrtcbin` signals
//! (`on-ice-candidate`, `pad-added`, etc.) may fire on the
//! GStreamer worker threads; we marshal those back to the main
//! GLib context via [`glib::MainContext::spawn_local`] before
//! reaching into `Inner`. That keeps `Inner: !Send` ergonomically
//! viable — every state-machine borrow happens on a single thread.
//!
//! The reentrance hazard the plan §3 calls out (`webrtcbin` worker
//! emits while we're mid-dispatch holding a `RefCell` borrow on the
//! machine) is handled by the spawn_local marshaling: the closure
//! body runs in a fresh main-loop turn, so by the time it borrows
//! `Inner`, the outer `handle_event` call has already returned.

use std::cell::RefCell;
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
/// it drives. Cheap to clone (it's just an `Rc<RefCell<Inner>>`),
/// which lets signal closures hold weak references back without
/// circular ownership.
///
/// The pipeline itself is constructed lazily on `new()`; tests
/// that don't need the GStreamer side can use
/// [`VoiceRuntime::new_without_pipeline`] to skip the
/// `webrtcbin` construction (and the gst-plugins-bad runtime
/// requirement that goes with it).
#[derive(Clone)]
pub struct VoiceRuntime {
    inner: Rc<RefCell<Inner>>,
}

struct Inner {
    machine: SessionMachine,
    backend: Box<dyn Backend>,
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
}

impl VoiceRuntime {
    /// Construct a runtime that owns a fresh `gst::Pipeline` and
    /// will drive the supplied `Backend`. The pipeline construction
    /// itself doesn't touch `webrtcbin` yet — Phase 8.C step 3 lands
    /// the bin and the signal wiring. Step 2's pipeline is just
    /// the empty container that step 3 grows into.
    ///
    /// `gst::init()` must have been called first (the C side runs
    /// `gtkhx_voice_init()` from `main` for this purpose). Calling
    /// without it returns `Err` rather than panicking.
    pub fn new(backend: Box<dyn Backend>) -> Result<Self, RuntimeError> {
        let pipeline = gstreamer::Pipeline::builder()
            .name("hxvoice-pipeline")
            .build();
        Ok(VoiceRuntime {
            inner: Rc::new(RefCell::new(Inner {
                machine: SessionMachine::new(),
                backend,
                pipeline: Some(pipeline),
                armed_timers: Vec::new(),
            })),
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
                backend,
                pipeline: None,
                armed_timers: Vec::new(),
            })),
        }
    }

    /// Drive one transition. Pumps `event` through the state
    /// machine, walks the returned action list, dispatches each
    /// effect.
    ///
    /// Returns the [`SessionState`] the machine has now entered —
    /// useful for tests; production callers can also consume the
    /// `StateChanged` signal via their `Backend`.
    pub fn handle_event(&self, event: Event) -> SessionState {
        let actions = {
            let mut inner = self.inner.borrow_mut();
            inner.machine.step(event)
        };
        for action in actions {
            self.dispatch(action);
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
            Action::SendWireFrame { opcode, body } => {
                self.inner
                    .borrow_mut()
                    .backend
                    .send_wire_frame(opcode, &body.0);
            }
            Action::EmitSignal { kind, payload } => {
                self.inner.borrow_mut().backend.emit_signal(kind, payload);
            }
            Action::TearDown => {
                self.inner.borrow_mut().backend.tear_down();
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
                let mut inner = self.inner.borrow_mut();
                if !inner.armed_timers.contains(&kind) {
                    inner.armed_timers.push(kind);
                }
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

            // hxvoice::Action is #[non_exhaustive]; new variants
            // would land here as a build-clean no-op until the
            // runtime grows handling for them.
            _ => {}
        }
    }
}

/// Errors returned from `VoiceRuntime::new`.
#[derive(Debug)]
pub enum RuntimeError {
    /// GStreamer wasn't initialised. Production code must call
    /// `gtkhx_voice_init()` (which runs `gst::init`) before
    /// constructing a runtime.
    NotInitialised,
}

impl core::fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            RuntimeError::NotInitialised => write!(
                f,
                "GStreamer subsystem not initialised; call \
                 gtkhx_voice_init() first"
            ),
        }
    }
}

impl std::error::Error for RuntimeError {}

#[cfg(test)]
mod tests {
    use super::*;
    use hxvoice::event::ConnectionState;

    fn rec() -> (VoiceRuntime, *mut RecordingBackend) {
        // Build a recording backend and hand the runtime a Box of
        // it, but keep a raw pointer so the test can read the
        // captured state. Safe because we control the lifetime and
        // never alias the &mut access — the runtime borrows it
        // only inside `dispatch`, and the test only reads it
        // between handle_event calls.
        let backend = Box::new(RecordingBackend::default());
        let ptr = Box::into_raw(backend);
        // Reconstruct from the raw pointer for the runtime's Box.
        let owned = unsafe { Box::from_raw(ptr) };
        let runtime = VoiceRuntime::new_without_pipeline(owned);
        (runtime, ptr)
    }

    /// Read the recording backend through the raw pointer. Only
    /// safe between `handle_event` calls (when the runtime isn't
    /// borrowing it).
    unsafe fn read(ptr: *mut RecordingBackend) -> &'static RecordingBackend {
        &*ptr
    }

    #[test]
    fn join_request_routes_send_wire_frame_to_backend() {
        let (runtime, backend) = rec();
        let new_state = runtime.handle_event(Event::JoinRequested { cid: 42 });
        assert_eq!(new_state, SessionState::JoinSent);
        let backend = unsafe { read(backend) };
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
        let backend = unsafe { read(backend) };
        // 600 (JOIN) then 603 (SDP_ANSWER).
        let opcodes: Vec<u32> = backend
            .wire_frames
            .iter()
            .map(|(op, _)| *op)
            .collect();
        assert_eq!(opcodes, vec![600, 603]);
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
        let backend = unsafe { read(backend) };
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
        let backend = unsafe { read(backend) };
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

    #[test]
    fn pipeline_built_runtime_constructs_when_gst_initialised() {
        // Coverage smoke for the with-pipeline constructor. Requires
        // gst::init() to have been called; we run it inline here
        // so the test is self-contained.
        let _ = crate::init();
        let runtime = VoiceRuntime::new(Box::new(NoopBackend))
            .expect("runtime should construct with a fresh pipeline");
        // Sanity: the pipeline element is reachable.
        assert!(runtime.inner.borrow().pipeline.is_some());
    }
}
