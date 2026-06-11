//! Tier 2 loopback test for the Phase 8.B GStreamer wiring.
//!
//! Builds the minimal `audiotestsrc ! mulawenc ! mulawdec ! fakesink`
//! pipeline and walks it to `Playing` state for ~100 ms. The test
//! catches three classes of failure that would otherwise surface
//! much later, with much worse signal-to-noise:
//!
//! 1. **gstreamer-rs isn't linked correctly.** The Rust crate
//!    family compiles fine on hosts with the system libs present,
//!    but a botched `gstreamer-sys` build script (wrong pkg-config
//!    path, missing dev headers) wouldn't surface until runtime —
//!    here, at `Element::make("audiotestsrc")`.
//! 2. **gst-plugins-base isn't installed.** `audiotestsrc`,
//!    `mulawenc`, `mulawdec`, and `fakesink` all live in
//!    gst-plugins-base; a missing package shows up as
//!    `ElementFactory::make` returning an error. The pkg-config
//!    probe in `meson.build` catches the headers, but the runtime
//!    plugins (`.so` files under `/usr/lib/.../gstreamer-1.0/`)
//!    are a separate package on every distro.
//! 3. **The pipeline can't reach `Playing` state.** Any number of
//!    config errors (caps mismatch, missing PTS, deadlocked
//!    state-change negotiation) manifest as the state-change
//!    polling timing out.
//!
//! Why `audiotestsrc`, not `autoaudiosrc`: CI containers have no
//! audio device. `audiotestsrc` synthesises a 440 Hz sine
//! regardless of host capability, so the same test passes
//! everywhere. The eventual production pipeline uses
//! `autoaudiosrc`, which is what Phase 8.C wires; the production
//! shape is tested against a real Janus server, not here.

use std::time::Duration;

use gstreamer as gst;
use gstreamer::prelude::*;

/// Build the four-element loopback pipeline, drive it to `Playing`,
/// hold for 100 ms, then tear down. Asserts that every step
/// succeeded — anything else fails the test loudly.
#[test]
fn loopback_audiotestsrc_to_fakesink_reaches_playing() {
    // gst::init is idempotent — safe to call from every test even
    // when the binary runs a single test in isolation.
    hxvoice_runtime::init();

    // Resolve every element up front so a missing plugin produces a
    // single clear failure rather than a partial pipeline.
    let src = gst::ElementFactory::make("audiotestsrc")
        .property("is-live", false)
        .build()
        .expect("audiotestsrc plugin available");
    let enc = hxvoice_runtime::audio::make_mulaw_encoder()
        .expect("mulawenc plugin available (gst-plugins-good or -bad)");
    let dec = hxvoice_runtime::audio::make_mulaw_decoder()
        .expect("mulawdec plugin available");
    let sink = gst::ElementFactory::make("fakesink")
        .property("sync", false)
        .build()
        .expect("fakesink plugin available (core)");

    let pipeline = gst::Pipeline::new();
    pipeline
        .add_many([&src, &enc, &dec, &sink])
        .expect("add elements to pipeline");
    gst::Element::link_many([&src, &enc, &dec, &sink])
        .expect("link pipeline elements");

    // Drive to Playing. `set_state` returns a typed
    // StateChangeSuccess; `Async` means the state change is in
    // flight on a worker thread, which is normal — we wait below.
    let res = pipeline.set_state(gst::State::Playing);
    assert!(
        res.is_ok(),
        "set_state(Playing) returned: {res:?}"
    );

    // Block until the pipeline reaches Playing or 2 s elapse.
    // `get_state` with a non-None timeout blocks the calling thread;
    // we explicitly bound it so a hung state-change can't pin CI.
    let (status, current, _pending) =
        pipeline.state(Some(gst::ClockTime::from_seconds(2)));
    assert!(
        status.is_ok(),
        "state(Playing) returned status {status:?} current={current:?}"
    );
    assert_eq!(
        current,
        gst::State::Playing,
        "pipeline reached {current:?}, expected Playing"
    );

    // Let it run briefly. We don't assert on buffer counts here —
    // the loopback test exists to catch state-change failures, not
    // to measure throughput. A 100 ms hold is enough to confirm the
    // mulaw codec elements aren't dying on the first buffer.
    std::thread::sleep(Duration::from_millis(100));

    // Tear down cleanly. `set_state(Null)` is synchronous on a
    // well-behaved pipeline.
    pipeline
        .set_state(gst::State::Null)
        .expect("set_state(Null) on teardown");
}

/// Smoke-test the DeviceMonitor enumeration entry point.
///
/// We don't assert on the device count — CI containers have no
/// audio device, so a clean run can return an empty Vec. We
/// assert only that the call doesn't panic and that the returned
/// devices, if any, have non-empty class strings (sanity-check on
/// the wrapper code).
#[test]
fn device_enumeration_does_not_panic() {
    hxvoice_runtime::init();

    let inputs = hxvoice_runtime::audio::list_input_devices();
    let outputs = hxvoice_runtime::audio::list_output_devices();

    for d in inputs.iter().chain(outputs.iter()) {
        assert!(!d.class.is_empty(), "device class string should be non-empty");
        assert!(!d.name.is_empty(), "device name should be non-empty");
    }
}

/// `make_source` / `make_sink` with `None` fall back to
/// `autoaudiosrc` / `autoaudiosink`. These factories exist on every
/// supported host (they're part of gst-plugins-good) and should
/// always return `Some`.
#[test]
fn auto_source_and_sink_factories_succeed() {
    hxvoice_runtime::init();

    assert!(
        hxvoice_runtime::audio::make_source(None).is_some(),
        "autoaudiosrc fallback element should be constructable"
    );
    assert!(
        hxvoice_runtime::audio::make_sink(None).is_some(),
        "autoaudiosink fallback element should be constructable"
    );
}

/// The PCM caps filter element builds successfully and carries the
/// canonical 8 kHz mono S16LE caps the eventual voice pipeline
/// pins between the audio resampler and the μ-law encoder.
#[test]
fn pcm_caps_filter_pins_8khz_mono_s16le() {
    hxvoice_runtime::init();

    let filter = hxvoice_runtime::audio::make_pcm8khz_caps_filter()
        .expect("capsfilter element");
    let caps: gst::Caps = filter.property("caps");
    let s = caps.structure(0).expect("caps has at least one structure");

    assert_eq!(s.name(), "audio/x-raw");
    assert_eq!(s.get::<i32>("rate").expect("rate field"), 8000);
    assert_eq!(s.get::<i32>("channels").expect("channels field"), 1);
    assert_eq!(
        s.get::<&str>("format").expect("format field"),
        "S16LE"
    );
}

/// Phase 8.C step 5's receive-leg bin builds end-to-end against
/// the host's installed GStreamer plugins. Pins that
/// `rtppcmudepay` (gst-plugins-good), `mulawdec` (-good),
/// `audioconvert` / `audioresample` (gst-plugins-base), and
/// `autoaudiosink` (-good) are all present and link cleanly.
///
/// Also verifies the ghost "sink" pad is exposed — the dispatch
/// arm's `bin.static_pad("sink")` lookup depends on this name.
#[test]
fn receive_bin_builds_and_exposes_ghost_sink_pad() {
    hxvoice_runtime::init();

    let bin = hxvoice_runtime::audio::make_receive_bin("hxvoice-recv-test")
        .expect(
            "receive bin must build — check gst-plugins-good \
             (rtppcmudepay / mulawdec / autoaudiosink) + \
             gst-plugins-base (audioconvert / audioresample)",
        );
    use gst::prelude::*;
    assert_eq!(bin.name(), "hxvoice-recv-test");
    let sink_pad = bin
        .static_pad("sink")
        .expect("receive bin must expose a ghost sink pad");
    assert_eq!(sink_pad.direction(), gst::PadDirection::Sink);
}
