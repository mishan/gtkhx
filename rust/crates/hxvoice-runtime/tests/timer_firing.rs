//! Phase 8.C step 7 — exercise the real `glib::timeout_add_local`
//! path end-to-end.
//!
//! Lives in its own integration-test binary so cargo runs it in a
//! dedicated process: no other test thread can hold the GLib default
//! `MainContext` outside this file.
//!
//! All three scenarios live in a single `#[test]` function because
//! the default `MainContext` can be acquired by exactly one thread
//! at a time — splitting them into separate `#[test]`s lets cargo's
//! parallel runner schedule them concurrently, and the loser of the
//! `acquire()` race panics. One serialised entrypoint sidesteps that
//! without pulling in a `serial_test` dep.
//!
//! The unit tests in `src/runtime.rs` cover the bookkeeping path
//! (arm → cancel, re-arm semantics, etc.). This file covers what
//! they can't: that an armed timer actually fires when the spec-
//! mandated duration elapses, that `CancelTimer` removes the
//! source from the loop, and that Drop cleans up without leaving a
//! dangling callback.

use core::cell::RefCell;
use std::rc::Rc;
use std::time::Duration;

use hxvoice::action::{Action, SignalKind, SignalPayload};
use hxvoice::event::{Event, Timeout};
use hxvoice::state::SessionState;
use hxvoice_runtime::runtime::{Backend, RecordingBackend, VoiceRuntime};

/// Backend that re-publishes every call into a shared
/// `RecordingBackend` so the test can inspect what the runtime
/// dispatched after the timer fired.
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

/// Drive the supplied `MainContext` until `predicate` returns true
/// or the budget elapses. Returns whether the predicate was
/// satisfied.
fn run_until<F: Fn() -> bool>(
    ctx: &gstreamer::glib::MainContext,
    budget: Duration,
    predicate: F,
) -> bool {
    let start = std::time::Instant::now();
    while start.elapsed() < budget {
        if predicate() {
            return true;
        }
        while ctx.iteration(false) {}
        std::thread::sleep(Duration::from_millis(1));
    }
    predicate()
}

#[test]
fn timer_firing_scenarios() {
    assert!(hxvoice_runtime::init());

    // Single context-acquire guard for all three scenarios. The
    // dispatch arms reach for `MainContext::default()` from the
    // same thread, so all three see the same owned context.
    let ctx = gstreamer::glib::MainContext::default();
    let _guard = ctx.acquire().expect(
        "acquire default main context — no other thread in this \
         test process should hold it",
    );

    // -- Scenario 1: armed timer fires Event::Timeout, walks
    //    state machine to Leaving via fail().
    {
        let backend = Rc::new(RefCell::new(RecordingBackend::default()));
        let runtime = VoiceRuntime::new_without_pipeline(Box::new(
            SharedRec(backend.clone()),
        ));

        runtime.handle_event(Event::JoinRequested { cid: 7 });
        assert_eq!(runtime.state(), SessionState::JoinSent);
        assert!(runtime.armed_timers().contains(&Timeout::JoinReply));

        // Short-circuit the 10 s production JoinReply watchdog
        // (JOIN_REPLY_TIMEOUT_MS in hxvoice::state) with a 10 ms
        // one so the test runs in a reasonable wallclock.
        runtime.dispatch(Action::ArmTimer {
            kind: Timeout::JoinReply,
            ms: 10,
        });

        let fired = run_until(&ctx, Duration::from_millis(250), || {
            runtime.state() == SessionState::Leaving
        });
        assert!(
            fired,
            "10 ms JoinReply timer should have fired and walked the state \
             machine to Leaving; ended at {:?} with armed_timers={:?}",
            runtime.state(),
            runtime.armed_timers()
        );
        let recordings = backend.borrow();
        assert_eq!(
            recordings.tear_downs, 1,
            "fail() must drive tear_down through the Backend"
        );
    }

    // -- Scenario 2: CancelTimer pulls the source off the loop,
    //    timer never fires.
    {
        let backend = Rc::new(RefCell::new(RecordingBackend::default()));
        let runtime = VoiceRuntime::new_without_pipeline(Box::new(
            SharedRec(backend.clone()),
        ));
        runtime.dispatch(Action::ArmTimer {
            kind: Timeout::Dtls,
            ms: 10,
        });
        assert!(runtime.armed_timers().contains(&Timeout::Dtls));

        runtime.dispatch(Action::CancelTimer { kind: Timeout::Dtls });
        assert!(!runtime.armed_timers().contains(&Timeout::Dtls));

        let start = std::time::Instant::now();
        while start.elapsed() < Duration::from_millis(50) {
            while ctx.iteration(false) {}
            std::thread::sleep(Duration::from_millis(1));
        }

        assert_eq!(
            runtime.state(),
            SessionState::Idle,
            "no event was driven; state machine should stay in Idle"
        );
        assert_eq!(
            backend.borrow().tear_downs,
            0,
            "cancelled timer must not fire and reach the Backend"
        );
    }

    // -- Scenario 3: Drop with a pending timer doesn't panic.
    //    The Drop impl on Inner cancels the source so glib's main
    //    loop doesn't fire a callback against a dropped runtime.
    {
        let backend = Rc::new(RefCell::new(RecordingBackend::default()));
        let runtime = VoiceRuntime::new_without_pipeline(Box::new(
            SharedRec(backend.clone()),
        ));
        runtime.dispatch(Action::ArmTimer {
            kind: Timeout::Media,
            ms: 10,
        });

        drop(runtime);

        let start = std::time::Instant::now();
        while start.elapsed() < Duration::from_millis(50) {
            while ctx.iteration(false) {}
            std::thread::sleep(Duration::from_millis(1));
        }

        // Reaching this line ⇒ no panic, no abort. Backend
        // didn't see any callback either.
        assert_eq!(backend.borrow().tear_downs, 0);
    }
}
