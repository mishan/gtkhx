//! Swapping an audio device under a running leg.
//!
//! A device change in Settings moves a call in progress onto the new
//! device by replacing just the capture source of the send bin, or just
//! the playback sink of each receive bin. These drive the swap against
//! live data flow, with `audiotestsrc` and `fakesink` standing in for
//! real devices so the tests don't depend on the host's audio setup.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use gstreamer as gst;
use gstreamer::prelude::*;
use hxvoice_runtime::audio;

fn live_test_source() -> gst::Element {
    gst::ElementFactory::make("audiotestsrc")
        .property("is-live", true)
        .property_from_str("wave", "silence")
        .build()
        .expect("audiotestsrc available")
}

fn counting_fakesink() -> (gst::Element, Arc<AtomicU64>) {
    let sink = gst::ElementFactory::make("fakesink")
        .property("sync", false)
        .build()
        .expect("fakesink available");
    let count = Arc::new(AtomicU64::new(0));
    let c = Arc::clone(&count);
    sink.static_pad("sink")
        .expect("fakesink has a sink pad")
        .add_probe(gst::PadProbeType::BUFFER, move |_, _| {
            c.fetch_add(1, Ordering::SeqCst);
            gst::PadProbeReturn::Ok
        });
    (sink, count)
}

fn wait_for(what: &str, cond: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while !cond() {
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn assert_no_bus_errors(pipeline: &gst::Pipeline) {
    let bus = pipeline.bus().expect("pipeline has a bus");
    if let Some(msg) = bus.pop_filtered(&[gst::MessageType::Error]) {
        panic!("pipeline posted an error: {msg:?}");
    }
}

/// The payloader outlives a capture swap: the far end keeps seeing one
/// RTP stream, same SSRC, sequence numbers running on without a gap,
/// and the local mute survives.
#[test]
fn capture_swap_keeps_the_rtp_stream_and_mute() {
    hxvoice_runtime::init();
    let bin = audio::make_send_bin(audio::SEND_BIN_NAME, None).expect("send bin builds");
    assert!(
        audio::replace_source(&bin, live_test_source()),
        "swapping the source of an idle bin"
    );

    // (ssrc, seqnum) of every RTP packet leaving the bin.
    let packets: Arc<Mutex<Vec<(u32, u16)>>> = Arc::default();
    let p = Arc::clone(&packets);
    bin.static_pad("src")
        .expect("send bin has a src ghost pad")
        .add_probe(gst::PadProbeType::BUFFER, move |_, info| {
            if let Some(buf) = info.buffer() {
                let map = buf.map_readable().expect("readable RTP buffer");
                let seq = u16::from_be_bytes([map[2], map[3]]);
                let ssrc = u32::from_be_bytes([map[8], map[9], map[10], map[11]]);
                p.lock().unwrap().push((ssrc, seq));
            }
            gst::PadProbeReturn::Ok
        });
    let (sink, _) = counting_fakesink();
    let pipeline = gst::Pipeline::new();
    pipeline.add_many([bin.upcast_ref(), &sink]).unwrap();
    bin.link(&sink).expect("send bin links to the sink");
    pipeline
        .set_state(gst::State::Playing)
        .expect("pipeline plays");

    wait_for("packets from the first source", || {
        packets.lock().unwrap().len() >= 5
    });
    let volume = bin
        .by_name(audio::SEND_VOLUME_ELEMENT_NAME)
        .expect("send bin has its mute volume");
    volume.set_property("mute", true);

    let old = bin
        .by_name(audio::SEND_SOURCE_ELEMENT_NAME)
        .expect("send bin names its source");
    let replacement = live_test_source();
    assert!(audio::replace_source(&bin, replacement.clone()));
    let before = packets.lock().unwrap().len();

    assert_eq!(
        bin.by_name(audio::SEND_SOURCE_ELEMENT_NAME).as_ref(),
        Some(&replacement)
    );
    assert!(old.parent().is_none(), "the old source leaves the bin");
    assert_eq!(old.current_state(), gst::State::Null);
    wait_for("packets from the new source", || {
        packets.lock().unwrap().len() >= before + 5
    });
    assert!(volume.property::<bool>("mute"), "mute survives the swap");
    assert_eq!(
        bin.by_name(audio::SEND_VOLUME_ELEMENT_NAME).as_ref(),
        Some(&volume)
    );

    let packets = packets.lock().unwrap().clone();
    assert!(
        packets.iter().all(|&(ssrc, _)| ssrc == packets[0].0),
        "one SSRC across the swap"
    );
    for pair in packets.windows(2) {
        assert_eq!(
            pair[1].1,
            pair[0].1.wrapping_add(1),
            "sequence numbers continue across the swap"
        );
    }
    assert_no_bus_errors(&pipeline);
    pipeline.set_state(gst::State::Null).unwrap();
}

/// A bin with no linked source is left alone.
#[test]
fn capture_swap_needs_a_linked_source() {
    hxvoice_runtime::init();
    let bin = gst::Bin::new();
    assert!(!audio::replace_source(&bin, live_test_source()));

    let lone = live_test_source();
    lone.set_property("name", audio::SEND_SOURCE_ELEMENT_NAME);
    bin.add(&lone).unwrap();
    assert!(!audio::replace_source(&bin, live_test_source()));
    assert_eq!(
        bin.by_name(audio::SEND_SOURCE_ELEMENT_NAME).as_ref(),
        Some(&lone)
    );
}

/// Playback moves to the new sink while audio is arriving, and the
/// listener's volume for that participant carries over.
#[test]
fn playback_swap_under_flow_keeps_the_listener_volume() {
    hxvoice_runtime::init();
    let bin = audio::make_receive_bin("hxvoice-recv-test", None).expect("receive bin builds");
    let (first, first_count) = counting_fakesink();
    assert!(audio::replace_sink(&bin, first.clone()));
    assert_eq!(
        bin.by_name(audio::RECV_SINK_ELEMENT_NAME).as_ref(),
        Some(&first),
        "an idle bin swaps on the spot"
    );

    let src = live_test_source();
    let enc = audio::make_mulaw_encoder().expect("mulawenc available");
    let pay = gst::ElementFactory::make("rtppcmupay")
        .build()
        .expect("rtppcmupay available");
    let pipeline = gst::Pipeline::new();
    pipeline
        .add_many([&src, &enc, &pay, bin.upcast_ref()])
        .unwrap();
    gst::Element::link_many([&src, &enc, &pay, bin.upcast_ref()]).expect("chain links");
    pipeline
        .set_state(gst::State::Playing)
        .expect("pipeline plays");
    wait_for("audio at the first sink", || {
        first_count.load(Ordering::SeqCst) >= 5
    });

    let volume = bin
        .by_name(audio::RECV_VOLUME_ELEMENT_NAME)
        .expect("receive bin has its listener volume");
    volume.set_property("volume", 0.25f64);
    let (second, second_count) = counting_fakesink();
    assert!(audio::replace_sink(&bin, second.clone()));
    wait_for("audio at the second sink", || {
        second_count.load(Ordering::SeqCst) >= 5
    });

    assert_eq!(
        bin.by_name(audio::RECV_SINK_ELEMENT_NAME).as_ref(),
        Some(&second)
    );
    assert!(first.parent().is_none(), "the old sink leaves the bin");
    assert_eq!(first.current_state(), gst::State::Null);
    assert_eq!(volume.property::<f64>("volume"), 0.25);
    assert_no_bus_errors(&pipeline);
    pipeline.set_state(gst::State::Null).unwrap();
}

/// A bin without an audio sink — a video or discard bin — is skipped.
#[test]
fn playback_swap_skips_a_bin_without_a_sink() {
    hxvoice_runtime::init();
    let bin = gst::Bin::new();
    let (sink, _) = counting_fakesink();
    assert!(!audio::replace_sink(&bin, sink));
}
