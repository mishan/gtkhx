//! Audio device enumeration and GStreamer element factories.
//!
//! Phase 8.B builds the audio plumbing the eventual voice pipeline
//! needs without yet connecting it to anything Hotline-shaped:
//!
//! - [`list_input_devices`] / [`list_output_devices`] enumerate the
//!   audio sources / sinks visible to the host's GStreamer device
//!   probes (PipeWire, PulseAudio, ALSA, etc., depending on what's
//!   installed). The voice-settings page (Phase 8.E) calls these to
//!   populate its device combo boxes.
//! - [`make_source`] / [`make_sink`] return configured `gst::Element`s
//!   ready to slot into a pipeline. Both accept an optional explicit
//!   device name; passing `None` falls back to `autoaudiosrc` /
//!   `autoaudiosink` which let GStreamer pick the host default.
//! - [`make_mulaw_encoder`] / [`make_mulaw_decoder`] return the
//!   μ-law codec elements the voice pipeline uses on both legs. PCMU
//!   is the only codec the fogWraith voice spec mandates (RFC 7874,
//!   8 kHz mono, payload type 0), so we lock the caps here.
//!
//! ## Listen-only graceful degradation
//!
//! `autoaudiosrc` succeeds even on hosts with no microphone — it
//! silently emits no buffers. That's the "listen-only participant"
//! shape the voice spec describes for users without a mic: they
//! still receive the room's audio, they just don't contribute a
//! send track. The eventual state machine (Phase 8.C) handles the
//! semantically empty send leg; the runtime layer here doesn't have
//! to do anything special.
//!
//! ## What stays out of this module
//!
//! - `webrtcbin` element construction and per-receive-leg dynamic
//!   pad handling. Phase 8.C.
//! - The pipeline itself (the `gst::Pipeline` that holds these
//!   elements and runs the state machine). Phase 8.C.
//! - `connect_pad_added` callbacks that drive renegotiation. Phase
//!   8.C.

use gstreamer as gst;
use gstreamer::glib;
use gstreamer::prelude::*;

/// Global per-process record of the user's selected capture +
/// playback device names.
///
/// Read by [`VoiceRuntime::new`](crate::runtime::VoiceRuntime::new)
/// at construction time and threaded through to `make_send_bin` /
/// `make_receive_bin` as their `device_name` argument. Written by
/// the C side from preferences on startup, and again whenever the
/// user changes the Settings → Voice pickers.
///
/// `None` means "fall back to system default" — `autoaudiosrc` /
/// `autoaudiosink`. An empty-string set via FFI is also normalised
/// to `None` here so the C side can store "" in `gtkhxrc` as the
/// "use default" sentinel without us having to add an extra
/// "clear" entry point.
///
/// Held behind a single `Mutex` because both the Settings UI and
/// `VoiceRuntime::new` can run from the same main thread, but the
/// runtime construction can also race with a "user changed the
/// picker while a call is being set up" sequence; a Mutex is
/// cheap and avoids the need to reason about that ordering. The
/// reads happen once per call (at construction) and the writes
/// happen on every settings save (rare); contention is negligible.
static DEVICE_PREFS: std::sync::Mutex<DevicePrefs> = std::sync::Mutex::new(DevicePrefs {
    input: None,
    output: None,
});

struct DevicePrefs {
    input: Option<String>,
    output: Option<String>,
}

/// Set the preferred capture device by `gst::Device::name()`.
/// Passing `None` or an empty `&str` clears the preference and
/// falls back to the system default at the next runtime
/// construction.
pub fn set_input_device(name: Option<&str>) {
    let normalised = name.filter(|s| !s.is_empty()).map(str::to_string);
    if let Ok(mut prefs) = DEVICE_PREFS.lock() {
        prefs.input = normalised;
    }
}

/// Set the preferred playback device by `gst::Device::name()`.
/// Same `None` / empty semantics as [`set_input_device`].
pub fn set_output_device(name: Option<&str>) {
    let normalised = name.filter(|s| !s.is_empty()).map(str::to_string);
    if let Ok(mut prefs) = DEVICE_PREFS.lock() {
        prefs.output = normalised;
    }
}

/// Return the currently-preferred capture device name, or `None`
/// for "system default". The returned owned `String` lives past
/// the lock release; the runtime needs that because it'll thread
/// the value through several borrow-checker hops.
pub fn input_device() -> Option<String> {
    DEVICE_PREFS.lock().ok().and_then(|p| p.input.clone())
}

/// Return the currently-preferred playback device name, or
/// `None` for "system default".
pub fn output_device() -> Option<String> {
    DEVICE_PREFS.lock().ok().and_then(|p| p.output.clone())
}

/// One enumerated audio device, the typed shape the eventual
/// settings UI cares about.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AudioDevice {
    /// Stable handle the rest of the pipeline uses to refer to this
    /// device (`gst::Device::name()`). Persisted into prefs.
    pub name: String,
    /// User-facing display name (`gst::Device::display_name()`).
    /// Falls back to `name` when the platform doesn't supply one.
    pub display_name: String,
    /// `gst::Device::device_class()` (e.g. `"Audio/Source"`,
    /// `"Audio/Sink"`). Surfaced verbatim so the UI can filter or
    /// label without re-deriving the classification.
    pub class: String,
}

impl AudioDevice {
    fn from_gst(d: &gst::Device) -> Self {
        let name = d.name().to_string();
        let display_name = d.display_name().to_string();
        // `gst::Device::display_name()` returns `name` when the
        // backend doesn't provide a separate display name; we don't
        // need a fallback, but keeping the field non-empty for the
        // UI is cheap insurance.
        let display_name = if display_name.is_empty() {
            name.clone()
        } else {
            display_name
        };
        let class = d.device_class().to_string();
        AudioDevice {
            name,
            display_name,
            class,
        }
    }
}

/// Run a one-shot DeviceMonitor scan, filtered to a class string.
///
/// The monitor isn't kept alive — Phase 8.E will wire a live monitor
/// to update the settings combo when devices come and go. For now we
/// just want a snapshot at call time. `gst::DeviceMonitor::start` is
/// required even for the snapshot path: `devices()` returns the
/// caller's accumulated buffer of `device-added` notifications, which
/// the monitor only collects when started.
fn enumerate_class(class_filter: &str) -> Vec<AudioDevice> {
    // Caller is responsible for running `crate::init()` (which calls
    // `gst::init()`) before reaching here — production paths go
    // through the C-side `gtkhx_voice_init` in `main`, and tests
    // call `init()` in their `#[test]` setup. If they didn't, the
    // DeviceMonitor returns an empty list and GStreamer logs the
    // condition to stderr; we just hand the caller a clean empty
    // Vec rather than panic.
    let monitor = gst::DeviceMonitor::new();
    // Empty caps == any caps. The class string is the load-bearing
    // filter ("Audio/Source" or "Audio/Sink").
    monitor.add_filter(Some(class_filter), None);
    if monitor.start().is_err() {
        return Vec::new();
    }
    let devices = monitor
        .devices()
        .iter()
        .map(AudioDevice::from_gst)
        .collect();
    monitor.stop();
    devices
}

/// List all visible audio input devices.
pub fn list_input_devices() -> Vec<AudioDevice> {
    enumerate_class("Audio/Source")
}

/// List all visible audio output devices.
pub fn list_output_devices() -> Vec<AudioDevice> {
    enumerate_class("Audio/Sink")
}

/// Build the source element for the send leg of the voice pipeline.
///
/// `device_name` of `Some` picks a specific device by its
/// `gst::Device::name()`; `None` selects the host default via
/// `autoaudiosrc`. The returned element is ready to link into a
/// pipeline — caller drives state changes.
///
/// Returns `None` if neither path can construct an element (e.g.
/// `autoaudiosrc` plugin missing on a stripped-down build host,
/// vanishingly rare on a desktop runtime).
///
/// **Test hook:** if `GTKHX_VOICE_TEST_AUDIO_SRC` is set in the
/// environment, returns a live `audiotestsrc` instead of any real
/// capture device. The Tier 3 voice media harness sets this so a
/// sender reliably produces RTP on a headless / device-less host —
/// the receiver-side assertions (RTP buffers flowing) then don't
/// depend on a working microphone. Never set in production.
pub fn make_source(device_name: Option<&str>) -> Option<gst::Element> {
    if let Some(val) = std::env::var_os("GTKHX_VOICE_TEST_AUDIO_SRC") {
        // `is-live=true` paces buffers in real time so rtppcmupay
        // emits at the normal ~50 pps, matching a real capture leg.
        //
        // The value selects the waveform. Default is `silence` (PCMU
        // still flows — the RTP-flow harness counts buffers, not
        // loudness — and nothing blasts a tone if the hook is ever set
        // on a desktop). Set it to `sine`/`tone` for a full-scale tone
        // when a test needs the receiver's `level` VAD to fire
        // (e.g. the speaker-indicator harness).
        let wave = match val.to_str() {
            Some("sine") | Some("tone") => "sine",
            _ => "silence",
        };
        return gst::ElementFactory::make("audiotestsrc")
            .property("is-live", true)
            .property_from_str("wave", wave)
            .build()
            .ok();
    }
    if let Some(name) = device_name {
        // Resolve by exact name through the DeviceMonitor first; the
        // GstDevice exposes a `create_element` that hands back a
        // pre-configured source for the picked device.
        let monitor = gst::DeviceMonitor::new();
        monitor.add_filter(Some("Audio/Source"), None);
        if monitor.start().is_ok() {
            let resolved = monitor
                .devices()
                .iter()
                .find(|d| d.name() == name)
                .and_then(|d| d.create_element(None).ok());
            monitor.stop();
            if let Some(element) = resolved {
                return Some(element);
            }
            // Named device wasn't visible — fall through to the
            // default-source path so the user still gets audio
            // instead of silent failure. The state machine surfaces
            // this case via a "configured input device unavailable"
            // toast in Phase 8.E.
        }
    }
    gst::ElementFactory::make("autoaudiosrc").build().ok()
}

/// Build the sink element for the receive leg of the voice pipeline.
///
/// Same shape as [`make_source`]: explicit device by name, or
/// `autoaudiosink` for the host default. The DeviceMonitor lookup
/// here matches sinks (`Audio/Sink` class) for the same fall-back
/// reasons.
pub fn make_sink(device_name: Option<&str>) -> Option<gst::Element> {
    if let Some(name) = device_name {
        let monitor = gst::DeviceMonitor::new();
        monitor.add_filter(Some("Audio/Sink"), None);
        if monitor.start().is_ok() {
            let resolved = monitor
                .devices()
                .iter()
                .find(|d| d.name() == name)
                .and_then(|d| d.create_element(None).ok());
            monitor.stop();
            if let Some(element) = resolved {
                return Some(element);
            }
        }
    }
    gst::ElementFactory::make("autoaudiosink").build().ok()
}

/// Build the μ-law encoder element (`mulawenc`). Phase 8.B's loopback
/// test pairs it with [`make_mulaw_decoder`]; Phase 8.C's voice
/// pipeline puts it between the resampled capture leg and `rtppcmupay`.
pub fn make_mulaw_encoder() -> Option<gst::Element> {
    gst::ElementFactory::make("mulawenc").build().ok()
}

/// Build the μ-law decoder element (`mulawdec`). Paired with
/// `rtppcmudepay` on the receive leg in Phase 8.C.
pub fn make_mulaw_decoder() -> Option<gst::Element> {
    gst::ElementFactory::make("mulawdec").build().ok()
}

/// Interval (nanoseconds) between `level` element bus messages.
///
/// 100 ms is the `level` element's own default; we set it
/// explicitly so the voice-activity-detection cadence is documented
/// at the source rather than inherited silently. At PCMU's 8 kHz
/// that's an 800-sample RMS window per message — long enough to be
/// stable against a single loud sample, short enough that the
/// 200 ms `speaker_tick` evaluator always sees at least one fresh
/// message per window. See `runtime.rs::handle_level_message`.
pub const LEVEL_MESSAGE_INTERVAL_NS: u64 = 100_000_000;

/// Build a `level` element configured to post RMS messages on the
/// pipeline bus for client-side voice-activity detection.
///
/// The `level` element measures per-channel RMS / peak power over
/// each [`LEVEL_MESSAGE_INTERVAL_NS`] window and posts it as a
/// `"level"` element message on the pipeline bus. The runtime's bus
/// watch (`runtime.rs::handle_level_message`) reads the RMS, applies
/// a dB threshold, and drives the per-uid speaker indicator off
/// actual loudness instead of mere RTP-packet arrival (PCMU has no
/// silence suppression, so packets flow continuously whether or not
/// the remote is making sound — see the long comment on
/// `voice_model.c::compute_indicator`).
///
/// `level` ships in `gst-plugins-good`, the same package as the
/// `rtppcmudepay` / `mulawdec` elements the receive bin requires, so
/// it's normally present. Returns `None` if the factory call fails;
/// [`make_receive_bin`] treats that as "no VAD" and builds the
/// receive bin without it rather than dropping audio.
pub fn make_level_meter() -> Option<gst::Element> {
    let level = gst::ElementFactory::make("level").build().ok()?;
    // Post a message per interval (the default, set explicitly for
    // clarity) and pin the interval to our documented cadence.
    level.set_property("post-messages", true);
    level.set_property("interval", LEVEL_MESSAGE_INTERVAL_NS);
    Some(level)
}

/// Build a caps filter that pins audio to 8 kHz mono 16-bit signed
/// linear PCM — the codec input PCMU expects. Inserted between the
/// resampler and the encoder in the eventual voice pipeline so
/// `audioconvert` / `audioresample` know which target to convert to.
///
/// `media=audio,rate=8000,channels=1` are the spec-required caps;
/// the format string locks little-endian S16 (the GStreamer
/// canonical 16-bit signed format on every supported host
/// architecture).
pub fn make_pcm8khz_caps_filter() -> Option<gst::Element> {
    let caps = gst::Caps::builder("audio/x-raw")
        .field("format", "S16LE")
        .field("rate", 8000i32)
        .field("channels", 1i32)
        .build();
    let element = gst::ElementFactory::make("capsfilter").build().ok()?;
    element.set_property("caps", &caps);
    Some(element)
}

/// Build a receive-leg `gst::Bin` for one inbound voice track.
///
/// The shape mirrors the fogWraith voice spec's mandated codec, with
/// an optional `level` meter tapped in for voice-activity detection:
///
/// ```text
/// (ghost sink) -> rtppcmudepay -> mulawdec -> audioconvert
///              -> [level] -> volume -> audioresample -> autoaudiosink
/// ```
///
/// The `volume` element ([`RECV_VOLUME_ELEMENT_NAME`]) is the
/// per-listener playback gain behind the user-list volume slider. It
/// sits *after* the `level` VAD tap on purpose: the speaker indicator
/// should reflect whether the remote is actually talking, independent
/// of how loud this client has chosen to play them back.
///
/// The ghost pad is exposed as the bin's `"sink"` pad, ready for
/// the caller (Phase 8.C step 5's `start_receive_bin`) to link to
/// the matching `webrtcbin` source pad. `autoaudiosink` lets
/// GStreamer pick the host's default output (PipeWire / PulseAudio
/// / ALSA, depending on what's installed); the eventual settings
/// UI (Phase 8.E) will swap that for the user-picked device.
///
/// **VAD is optional.** The `level` element drives the per-uid
/// speaker indicator, but it is *not* required for audio. If the
/// `level` plugin is unavailable, the bin is built without it: voice
/// still works, the speaker indicator just never lights up. Only the
/// audio-path elements (`rtppcmudepay` / `mulawdec` / `audioconvert`
/// / `audioresample` / the sink) are load-bearing.
///
/// Returns `None` only if one of those load-bearing factory calls
/// fails (a missing runtime plugin — `gst-plugins-good` ships
/// `rtppcmudepay` / `mulawdec` / `autoaudiosink`, and the
/// audio-conversion elements are in `gst-plugins-base`). Caller
/// should log and skip the receive leg; the session keeps running,
/// the user just doesn't hear that one remote.
///
/// `name` becomes the bin's element name so pipeline introspection
/// can tell receive legs apart — convention is `"hxvoice-recv-<mid>"`.
pub fn make_receive_bin(name: &str, device_name: Option<&str>) -> Option<gst::Bin> {
    let bin = gst::Bin::builder().name(name).build();
    let depay = gst::ElementFactory::make("rtppcmudepay").build().ok()?;
    let dec = gst::ElementFactory::make("mulawdec").build().ok()?;
    let conv = gst::ElementFactory::make("audioconvert").build().ok()?;
    // Per-listener playback gain. `volume` starts at unity (1.0); the
    // runtime's `set_user_volume` path sets its `volume` property in
    // response to the user-list slider, and re-applies a stored gain
    // when this bin is (re)built on a rejoin. `volume` lives in
    // gst-plugins-base alongside audioconvert / audioresample, so if
    // those resolve so does this — but treat it as load-bearing and
    // fail the bin build if it's missing, matching the send leg.
    let volume = gst::ElementFactory::make("volume")
        .name(RECV_VOLUME_ELEMENT_NAME)
        .build()
        .ok()?;
    let res = gst::ElementFactory::make("audioresample").build().ok()?;
    // Output device — same Some/None pattern as the send leg.
    // None falls back to autoaudiosink; an explicit name resolves
    // via DeviceMonitor against the `Audio/Sink` class.
    let sink = make_sink(device_name)?;
    // Voice-activity meter, tapped on the decoded PCM after
    // audioconvert (normalised raw format) and before audioresample.
    // OPTIONAL: if the `level` plugin isn't installed we drop it from
    // the chain — VAD-driven speaker indicators go dark but audio is
    // unaffected. `level` ships in gst-plugins-good alongside the
    // codec elements above, so the `None` branch is a belt-and-
    // suspenders fallback for an unusual partial install, not an
    // expected path.
    let level = make_level_meter();
    if level.is_none() {
        gst::warning!(
            gst::CAT_RUST,
            "hxvoice: `level` element unavailable — voice-activity \
             speaker indicators are disabled (audio is unaffected). \
             Install gst-plugins-good to enable VAD."
        );
    }
    bin.add_many([&depay, &dec, &conv, &volume, &res, &sink])
        .ok()?;
    if let Some(ref level) = level {
        bin.add(level).ok()?;
        // depay -> dec -> conv -> level -> volume -> res -> sink
        gst::Element::link_many([&depay, &dec, &conv, level, &volume, &res, &sink]).ok()?;
    } else {
        // depay -> dec -> conv -> volume -> res -> sink (no VAD tap)
        gst::Element::link_many([&depay, &dec, &conv, &volume, &res, &sink]).ok()?;
    }
    // Diagnostic: attach pad probes at FOUR points along the
    // receive chain so we can tell exactly where buffers stop
    // flowing. With "receive bin LINKED" already logged but no
    // audible audio and an empty PulseAudio Playback tab, the
    // remaining unknown is whether ANY data is making it through
    // the receive chain. The probe counters answer that without
    // disturbing the data flow.
    //
    // Points instrumented:
    //   1. depay sink — the bin's ingress, immediately downstream
    //      of webrtcbin's `src_0`. If this never fires, the
    //      BUNDLE'd RTP demuxer in webrtcbin isn't routing
    //      packets to the receive transceiver at all.
    //   2. dec src — after mulaw decode. If 1 fires but 2 doesn't,
    //      the RTP payload type or caps negotiation broke
    //      depay→dec.
    //   3. res src — after resample. If 2 fires but 3 doesn't,
    //      the conv/resample stage is rejecting the format.
    //   4. autoaudiosink sink — the final ingress to the
    //      PulseAudio sink. If 3 fires but 4 doesn't, the link
    //      audioresample→autoaudiosink dropped. If 4 fires but
    //      pavucontrol shows nothing, autoaudiosink isn't
    //      opening a real device.
    //
    // Each probe logs the first buffer it sees + every 50th.
    // At rtppcmupay's default 20 ms ptime (50 packets/sec) that's
    // "buffer #50" landing at ~1 s of live audio.
    //
    // Gated on `voice-flow` so the probe (counter increment +
    // modulo per buffer) doesn't run in production where the
    // diagnostic isn't useful.
    if crate::debug::category_enabled("voice-flow") {
        let bin_name = bin.name().to_string();
        attach_buffer_probe(&depay, "sink", &bin_name, "depay.sink");
        attach_buffer_probe(&dec, "src", &bin_name, "dec.src");
        attach_buffer_probe(&res, "src", &bin_name, "res.src");
        attach_buffer_probe(&sink, "sink", &bin_name, "sink.sink");
    }
    // Expose the depayloader's sink as a ghost pad on the bin so
    // the caller can link the webrtcbin source pad to it.
    let depay_sink = depay.static_pad("sink")?;
    let ghost = gst::GhostPad::with_target(&depay_sink).ok()?;
    bin.add_pad(&ghost).ok()?;
    Some(bin)
}

/// Attach a buffer-counting `gst::Pad` probe to a named pad on an
/// element and `eprintln` the first observed buffer plus every 50th.
///
/// `where_` is a label like `"depay.sink"` so the diagnostic line
/// makes clear which stage of the receive chain the count belongs
/// to. The probe is `BUFFER` only (not `BUFFER_LIST`), since the
/// PCMU receive chain uses one-buffer-per-RTP-packet flow.
fn attach_buffer_probe(
    element: &gst::Element,
    pad_name: &str,
    bin_name: &str,
    where_: &'static str,
) {
    let Some(pad) = element.static_pad(pad_name) else {
        crate::debug::log!(
            "voice-flow",
            "{bin_name} {where_}: could not get pad to probe"
        );
        return;
    };
    let bin_name = bin_name.to_string();
    let counter = std::sync::atomic::AtomicU64::new(0);
    pad.add_probe(gst::PadProbeType::BUFFER, move |_pad, _info| {
        let n = counter.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
        if n == 1 || n % 50 == 0 {
            crate::debug::log!("voice-flow", "{bin_name} {where_}: buffer #{n}");
        }
        gst::PadProbeReturn::Ok
    });
}

/// Element name of the `volume` element in the send bin, used as the
/// local mute control. The `SetSendPipelineMute` dispatch arm in
/// `runtime.rs` looks it up by this name via `pipeline.by_name(…)` to
/// toggle the mic, so the producer here and the consumer there must
/// agree on the string.
pub const SEND_VOLUME_ELEMENT_NAME: &str = "hxvoice-send-volume";

/// Element name of the per-listener `volume` element in every receive
/// bin — the playback-gain control behind the user-list right-click
/// volume slider. The runtime's `set_user_volume` path resolves a
/// uid's receive bin(s) and looks this element up **within that bin**
/// via `bin.by_name(…)` to set its `volume` property (0.0 = silence,
/// 1.0 = unity, up to 10.0 = boost). Producer here and consumer in
/// `runtime.rs` must agree on the string.
///
/// GStreamer element names only need to be unique within their
/// immediate parent bin, so every receive bin can reuse this same
/// name without collision — the lookup is always scoped to one bin,
/// never `pipeline.by_name` (which would return an arbitrary match).
pub const RECV_VOLUME_ELEMENT_NAME: &str = "hxvoice-recv-volume";

/// Name of the send `gst::Bin`. The runtime passes this to
/// [`make_send_bin`] and matches on it in `handle_level_message` to
/// route the send leg's `level` RMS windows to the LOCAL user's speaker
/// indicator (outgoing VAD) instead of a remote uid.
pub const SEND_BIN_NAME: &str = "hxvoice-send-bin";

/// Build a send-leg `gst::Bin` that captures audio from the system
/// microphone, encodes to μ-law, payloads as RTP/PCMU, and exposes
/// a single source pad ready to link to `webrtcbin`'s sink request
/// pad.
///
/// Chain:
///
/// ```text
/// autoaudiosrc -> audioconvert -> volume -> audioresample
///              -> capsfilter(PCM 8 kHz mono) -> mulawenc
///              -> rtppcmupay -> (ghost src)
/// ```
///
/// The `volume` element ([`SEND_VOLUME_ELEMENT_NAME`]) is the local
/// mute control — see [`make_send_bin`]'s body and the
/// `SetSendPipelineMute` dispatch in `runtime.rs`.
///
/// `autoaudiosrc` is GStreamer's auto-plugger that picks the host
/// default capture device — `pulsesrc` on a PulseAudio /
/// PipeWire-PA-shim setup, `pipewiresrc` if `gst-plugins-rs`'s
/// pipewire element is registered and PA isn't, `alsasrc` on bare
/// ALSA, etc. The capture device shows up under pavucontrol's
/// "Recording" tab while the call is active, just like any
/// pulse-aware audio app (zoom, discord, etc.).
///
/// A user-pickable device selection comes in a follow-up phase
/// (settings UI for input device — see voice spec §8.E); for now,
/// system-default microphone is the right behavior.
///
/// Returns `None` on any factory failure. The eventual missing
/// element is whichever one the user's GStreamer install doesn't
/// ship; `autoaudiosrc` lives in `gst-plugins-good`, `mulawenc`
/// and `rtppcmupay` in `gst-plugins-good` as well — production
/// already has all of those via the audio runtime install.
///
/// Linking note: the chain ends in a ghost src pad, NOT a direct
/// link to webrtcbin. The caller asks webrtcbin for a sink request
/// pad (`sink_%u`) and links the ghost src to it. That gives
/// webrtcbin the chance to advertise the right SDP media
/// description (PCMU caps, sendrecv direction) when answering an
/// incoming offer — without a send-leg attached, webrtcbin answers
/// with `a=inactive` and the peer connection never carries media.
pub fn make_send_bin(name: &str, device_name: Option<&str>) -> Option<gst::Bin> {
    let bin = gst::Bin::builder().name(name).build();
    // Capture device. `device_name == None` falls back to
    // autoaudiosrc (system default); a `Some(name)` value is
    // looked up via DeviceMonitor with the `Audio/Source` filter
    // and the resolved device's `create_element` returns a
    // pre-configured source. The fallback path is intentional:
    // if the user picked a specific device that's no longer
    // present (USB mic unplugged, profile changed), they still
    // get audio through the default source rather than a silent
    // failure. The Settings UI surfaces this case as a
    // "configured input device unavailable" toast in Phase
    // 8.E follow-ups.
    let src = make_source(device_name)?;
    let conv = gst::ElementFactory::make("audioconvert").build().ok()?;
    // Local mute control. A `volume` element whose `mute` property,
    // when set TRUE, replaces the captured microphone audio with
    // digital silence *before* it reaches the encoder. This is the
    // real, client-side mute: with it engaged, no microphone content
    // is encoded, payloaded, or sent — the 606 wire flag is only the
    // server-side enforcement, this is the local guarantee that a
    // "muted" user is genuinely not transmitting their mic.
    //
    // We mute via the `mute` property (silence) rather than dropping
    // the stream (e.g. a `valve`): silent PCMU keeps flowing at the
    // normal ~50 pps, which keeps the RTP / NAT path warm. Going
    // fully silent on the wire would let an idle NAT binding lapse —
    // exactly the kind of idle-transport fragility we want to avoid.
    // `volume` lives in gst-plugins-base alongside audioconvert /
    // audioresample, so if those resolve, so does this.
    let volume = gst::ElementFactory::make("volume")
        .name(SEND_VOLUME_ELEMENT_NAME)
        .build()
        .ok()?;
    let res = gst::ElementFactory::make("audioresample").build().ok()?;
    let caps = make_pcm8khz_caps_filter()?;
    let enc = make_mulaw_encoder()?;
    let pay = gst::ElementFactory::make("rtppcmupay").build().ok()?;
    // Outgoing VAD: a `level` meter right AFTER the mute `volume`, so it
    // measures what we actually transmit — muting (volume silence) reads
    // as not-speaking, unmuted speech reads as speaking. Its RMS windows
    // post `level` bus messages that `handle_level_message` routes to the
    // LOCAL user's speaker indicator (this bin is `SEND_BIN_NAME`). The
    // meter is optional: if the `level` element is unavailable we build
    // the send bin without it (no outgoing VAD, audio still flows) — the
    // same graceful degradation the receive bin uses.
    let level = make_level_meter();
    if let Some(level) = &level {
        bin.add_many([&src, &conv, &volume, level, &res, &caps, &enc, &pay])
            .ok()?;
        gst::Element::link_many([&src, &conv, &volume, level, &res, &caps, &enc, &pay]).ok()?;
    } else {
        bin.add_many([&src, &conv, &volume, &res, &caps, &enc, &pay])
            .ok()?;
        gst::Element::link_many([&src, &conv, &volume, &res, &caps, &enc, &pay]).ok()?;
    }
    // Diagnostic probe: count buffers as they exit the
    // payloader. If both peers receive exactly one packet over
    // a working WebRTC session and then go silent, the question
    // is whether OUR send chain is actually pushing packets
    // continuously past the first one or stalling somewhere.
    // The probe fires once per RTP-packet pushed downstream
    // (autoaudiosrc → rtppcmupay default ptime = 20 ms → 50
    // packets/sec, so "buffer #50" lands at ~1 s of live audio).
    //
    // If the send probe ticks continuously while the remote
    // peer sees only one packet, the failure is downstream of
    // us (webrtcbin / DTLS-SRTP encrypt / Janus forwarding).
    // If the send probe also stalls after #1, our send
    // pipeline is the problem (mic feed stalled, negotiated
    // payload-type mismatch, etc.).
    //
    // Gated on `voice-flow` to keep the per-buffer counter
    // increment out of production.
    if crate::debug::category_enabled("voice-flow") {
        let bin_name = bin.name().to_string();
        attach_send_buffer_probe(&pay, "src", &bin_name);
    }
    // Expose the payloader's src as a ghost pad on the bin so the
    // caller can link it to webrtcbin's sink request pad.
    let pay_src = pay.static_pad("src")?;
    let ghost = gst::GhostPad::with_target(&pay_src).ok()?;
    bin.add_pad(&ghost).ok()?;
    Some(bin)
}

/// Attach a buffer-counting probe to the send chain's payloader
/// src pad. Same shape as `attach_buffer_probe` above but with a
/// distinct log prefix so it's easy to tell apart from the
/// receive-side counters.
fn attach_send_buffer_probe(element: &gst::Element, pad_name: &str, bin_name: &str) {
    let Some(pad) = element.static_pad(pad_name) else {
        crate::debug::log!("voice-flow", "send {bin_name}: could not get pad to probe");
        return;
    };
    let bin_name = bin_name.to_string();
    let counter = std::sync::atomic::AtomicU64::new(0);
    pad.add_probe(gst::PadProbeType::BUFFER, move |_pad, _info| {
        let n = counter.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
        if n == 1 || n % 50 == 0 {
            crate::debug::log!("voice-flow", "{bin_name} pay.src: buffer #{n}");
        }
        gst::PadProbeReturn::Ok
    });
}

// glib import kept above for completeness even though no glib symbol
// is referenced directly today — the device-name strings round-trip
// through `glib::GString`, and Phase 8.E will lean on glib types in
// this module when the settings UI lands. Silence the warning until
// then.
#[allow(dead_code)]
fn _glib_keepalive(_: &glib::GString) {}
