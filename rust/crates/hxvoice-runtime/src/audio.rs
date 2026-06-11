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
pub fn make_source(device_name: Option<&str>) -> Option<gst::Element> {
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

// glib import kept above for completeness even though no glib symbol
// is referenced directly today — the device-name strings round-trip
// through `glib::GString`, and Phase 8.E will lean on glib types in
// this module when the settings UI lands. Silence the warning until
// then.
#[allow(dead_code)]
fn _glib_keepalive(_: &glib::GString) {}
