//! A branch linked into the runtime's pipeline after it is playing — as
//! every send bin is — gets its latency only when the bus watch
//! recalculates it. Without that, rtpbin can't time an RTCP report for
//! it, and RTCP, PLIs included, goes missing.
//!
//! Its own binary, because the bus watch needs the default main
//! context to itself.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use gstreamer as gst;
use gstreamer::prelude::*;
use hxvoice_runtime::runtime::{RecordingBackend, VoiceRuntime};

#[test]
fn a_branch_linked_while_playing_is_given_its_latency() {
    assert!(hxvoice_runtime::init());
    let ctx = glib::MainContext::default();
    let _guard = ctx
        .acquire()
        .expect("acquire default main context — sole test in this binary");

    let runtime = VoiceRuntime::new(Box::new(RecordingBackend::default()))
        .expect("with-pipeline construction");
    let pipeline = runtime.pipeline_for_test().expect("pipeline");
    pipeline.set_state(gst::State::Playing).expect("playing");

    let src = gst::ElementFactory::make("audiotestsrc")
        .property("is-live", true)
        .build()
        .expect("audiotestsrc");
    let sink = gst::ElementFactory::make("fakesink")
        .property("async", false)
        .build()
        .expect("fakesink");
    let latency_events = Arc::new(AtomicUsize::new(0));
    let seen = latency_events.clone();
    sink.static_pad("sink").unwrap().add_probe(
        gst::PadProbeType::EVENT_UPSTREAM,
        move |_, info| {
            if let Some(gst::PadProbeData::Event(e)) = &info.data {
                if e.type_() == gst::EventType::Latency {
                    seen.fetch_add(1, Ordering::SeqCst);
                }
            }
            gst::PadProbeReturn::Ok
        },
    );
    pipeline.add_many([&src, &sink]).unwrap();
    src.link(&sink).unwrap();
    src.sync_state_with_parent().unwrap();
    sink.sync_state_with_parent().unwrap();

    let _ = pipeline
        .bus()
        .unwrap()
        .post(gst::message::Latency::builder().src(&sink).build());

    let start = Instant::now();
    while latency_events.load(Ordering::SeqCst) == 0 && start.elapsed() < Duration::from_secs(5) {
        while ctx.iteration(false) {}
        std::thread::sleep(Duration::from_millis(5));
    }
    let _ = pipeline.set_state(gst::State::Null);
    assert!(
        latency_events.load(Ordering::SeqCst) > 0,
        "the branch never received a latency event"
    );
}
