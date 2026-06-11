//! Phase 8.C step 8 — exercise the pipeline bus watch end-to-end.
//!
//! Lives in its own integration-test binary so cargo runs it in a
//! dedicated process; the bus watch + connection-state notify both
//! attach to the GLib default `MainContext`, and only the
//! single-process integration shape can reliably own that without
//! racing the other test binaries.
//!
//! The unit tests in `src/runtime.rs` cover the
//! `map_peer_connection_state` translation and confirm that
//! construction completes with the bus watch attached. This test
//! covers the dynamic shape: a bus message posted after construction
//! actually reaches the watch closure (verified indirectly by
//! pumping the main loop and asserting no panic / no crash).

use core::cell::RefCell;
use std::rc::Rc;
use std::time::Duration;

use hxvoice::action::{SignalKind, SignalPayload};
use hxvoice_runtime::runtime::{Backend, RecordingBackend, VoiceRuntime};

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

#[test]
fn pipeline_bus_watch_handles_posted_messages_without_panic() {
    assert!(hxvoice_runtime::init());

    // Single owner of the default main context for this whole
    // process — same pattern as `timer_firing.rs`.
    let ctx = gstreamer::glib::MainContext::default();
    let _guard = ctx
        .acquire()
        .expect("acquire default main context — sole test in this binary");

    let backend = Rc::new(RefCell::new(RecordingBackend::default()));
    let runtime = VoiceRuntime::new(Box::new(SharedRec(backend.clone())))
        .expect("with-pipeline construction");

    let pipeline = runtime
        .pipeline_for_test()
        .expect("pipeline must be present after construction");
    let bus = pipeline.bus().expect("pipeline bus must be present");

    // Post a synthetic Warning message. The bus watch should
    // log it (Warning arm) and return Continue. We can't observe
    // the log output from a unit test, but reaching the
    // post-iteration assertion means the watch callback didn't
    // panic or abort.
    use gstreamer::prelude::*;
    let msg = gstreamer::message::Warning::builder(
        gstreamer::CoreError::Failed,
        "synthetic bus warning for test",
    )
    .build();
    let _ = bus.post(msg);

    // Drain the main loop briefly so the watch fires.
    let start = std::time::Instant::now();
    while start.elapsed() < Duration::from_millis(100) {
        while ctx.iteration(false) {}
        std::thread::sleep(Duration::from_millis(1));
    }

    // Reaching this assertion at all is the test. The runtime
    // shouldn't have been torn down by a synthetic warning.
    assert_eq!(backend.borrow().tear_downs, 0);
}
