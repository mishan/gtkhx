//! The video legs of the voice pipeline.
//!
//! Video rides the voice peer connection: the same `webrtcbin`, the same
//! bundle, the same ICE and DTLS. This module builds the elements the
//! runtime hangs off it — a VP8 receive bin per remote stream, a capture
//! bin per local publication — and the frame store the UI reads decoded
//! pictures out of.
//!
//! The crate has no GTK dependency and keeps it that way: a receive bin
//! ends in an `appsink` pulling RGBA, the newest frame per stream is kept
//! in [`FrameStore`], and the runtime posts one main-loop notification per
//! batch of new frames. The UI wraps each frame's bytes in a texture
//! without copying them; see [`VideoFrame::bytes`].

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_app as gst_app;
use hxvoice::VideoKind;

/// The payload type every video section uses, camera and screen alike.
pub const VP8_PAYLOAD_TYPE: i32 = 96;

/// The server's ceiling for one stream kind, from the login reply's
/// `DATA_VIDEO_LIMITS`. A publication must not exceed any of these.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Limits {
    pub max_width: u16,
    pub max_height: u16,
    pub max_fps: u16,
    /// Bits per second.
    pub max_bitrate: u32,
}

impl Limits {
    /// The spec's configuration defaults, used when a server confirmed
    /// the capability but sent no limits field for the kind.
    pub fn spec_default(kind: VideoKind) -> Limits {
        match kind {
            VideoKind::Camera => Limits {
                max_width: 1280,
                max_height: 720,
                max_fps: 30,
                max_bitrate: 1_500_000,
            },
            VideoKind::Screen => Limits {
                max_width: 1920,
                max_height: 1080,
                max_fps: 15,
                max_bitrate: 2_500_000,
            },
        }
    }

    /// What the encoder is configured to: the kind's preferred shape,
    /// clamped to the ceiling. A camera aims for 640×480 at up to 30 fps
    /// on 600 kb/s; a screen share uses the whole ceiling's pixels at
    /// a low frame rate, because a desktop is mostly still and its text
    /// must stay legible.
    pub fn target(&self, kind: VideoKind) -> EncodeTarget {
        let (w, h, fps, bitrate) = match kind {
            VideoKind::Camera => (640, 480, 30, 600_000),
            VideoKind::Screen => (1920, 1080, 15, 2_000_000),
        };
        let (width, height) = fit(w, h, self.max_width as u32, self.max_height as u32);
        EncodeTarget {
            width,
            height,
            fps: fps.min(self.max_fps as u32).max(1),
            bitrate: bitrate.min(self.max_bitrate).max(50_000),
        }
    }
}

/// The encoder settings a publication runs at.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EncodeTarget {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate: u32,
}

/// Scale `w`×`h` down, keeping its aspect, until it fits `max_w`×`max_h`.
/// Even dimensions, because the encoder's 4:2:0 input needs them.
fn fit(w: u32, h: u32, max_w: u32, max_h: u32) -> (u32, u32) {
    let max_w = max_w.max(2);
    let max_h = max_h.max(2);
    let (mut ow, mut oh) = (w, h);
    if ow > max_w {
        oh = oh * max_w / ow;
        ow = max_w;
    }
    if oh > max_h {
        ow = ow * max_h / oh;
        oh = max_h;
    }
    (ow.max(2) & !1, oh.max(2) & !1)
}

/// Which stream a frame belongs to. `user_id` 0 is this client's own
/// preview of the kind: uid 0 is reserved by the base protocol, so it
/// never names a remote publisher.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct StreamKey {
    pub user_id: u16,
    pub kind: VideoKind,
}

/// The preview key for this client's own publication of `kind`.
pub fn self_key(kind: VideoKind) -> StreamKey {
    StreamKey { user_id: 0, kind }
}

/// One decoded picture, RGBA, 8 bits a channel.
#[derive(Debug, Clone)]
pub struct VideoFrame {
    pub width: u32,
    pub height: u32,
    /// Bytes per row.
    pub stride: u32,
    /// The buffer's own memory, mapped and handed over without a copy.
    /// Dropping the last reference unmaps it.
    pub bytes: glib::Bytes,
}

#[derive(Default)]
struct Slot {
    frame: Option<VideoFrame>,
    fresh: bool,
    count: u64,
}

/// The newest frame of every stream, shared between the appsinks'
/// streaming threads and the main thread. A stream keeps one frame; a
/// UI that falls behind skips frames rather than queueing them.
#[derive(Default)]
pub(crate) struct FrameStore {
    slots: Mutex<HashMap<StreamKey, Slot>>,
    pending: AtomicBool,
}

impl FrameStore {
    /// Store `frame` as the newest for `key`. Returns true when the
    /// caller should schedule a main-loop notification — i.e. none is
    /// already on its way.
    pub(crate) fn put(&self, key: StreamKey, frame: VideoFrame) -> bool {
        if let Ok(mut slots) = self.slots.lock() {
            let slot = slots.entry(key).or_default();
            slot.frame = Some(frame);
            slot.fresh = true;
            slot.count += 1;
        }
        !self.pending.swap(true, Ordering::AcqRel)
    }

    /// Called on the main thread when the notification runs, before
    /// observers are told: frames stored after this schedule another.
    pub(crate) fn clear_pending(&self) {
        self.pending.store(false, Ordering::Release);
    }

    /// The newest frame for `key` if it arrived since the last take.
    pub(crate) fn take(&self, key: StreamKey) -> Option<VideoFrame> {
        let mut slots = self.slots.lock().ok()?;
        let slot = slots.get_mut(&key)?;
        if !slot.fresh {
            return None;
        }
        slot.fresh = false;
        slot.frame.clone()
    }

    /// Frames stored for `key` since it was last removed.
    pub(crate) fn count(&self, key: StreamKey) -> u64 {
        self.slots
            .lock()
            .ok()
            .and_then(|s| s.get(&key).map(|slot| slot.count))
            .unwrap_or(0)
    }

    /// The size of the newest frame for `key`, fresh or not.
    pub(crate) fn size(&self, key: StreamKey) -> Option<(u32, u32)> {
        let slots = self.slots.lock().ok()?;
        let frame = slots.get(&key)?.frame.as_ref()?;
        Some((frame.width, frame.height))
    }

    pub(crate) fn remove(&self, key: StreamKey) {
        if let Ok(mut slots) = self.slots.lock() {
            slots.remove(&key);
        }
    }

    pub(crate) fn clear(&self) {
        if let Ok(mut slots) = self.slots.lock() {
            slots.clear();
        }
    }
}

fn have(factories: &[&str]) -> bool {
    factories
        .iter()
        .all(|f| gst::ElementFactory::find(f).is_some())
}

const DECODE_FACTORIES: &[&str] = &[
    "rtpvp8depay",
    "vp8dec",
    "videoconvert",
    "videoscale",
    "appsink",
];
const ENCODE_FACTORIES: &[&str] = &[
    "videoconvert",
    "videoscale",
    "videorate",
    "vp8enc",
    "rtpvp8pay",
    "tee",
    "queue",
    "appsink",
];

/// Whether this installation can take part in video at all: decode
/// VP8 off the wire and hand pictures to the UI. This is what decides
/// whether the capability bit is advertised; a client that can watch
/// but not publish is a full participant.
pub fn receive_available() -> bool {
    gst::init().is_ok() && have(DECODE_FACTORIES)
}

/// Whether this installation can publish a stream of `kind`: an encoder
/// and payloader, plus a source for the kind.
pub fn publish_available(kind: VideoKind) -> bool {
    if gst::init().is_err() || !have(ENCODE_FACTORIES) {
        return false;
    }
    if std::env::var_os(TEST_SRC_ENV).is_some() {
        return have(&["videotestsrc"]);
    }
    match kind {
        // The UI asks on every button refresh, so this must not start a
        // device monitor: autovideosrc is a factory lookup, and the
        // enumeration is only consulted as it was last seen.
        VideoKind::Camera if camera_via_portal() => {
            gst::ElementFactory::find("pipewiresrc").is_some() && camera_portal_present()
        }
        VideoKind::Camera => gst::ElementFactory::find("autovideosrc").is_some() || cameras_seen(),
        VideoKind::Screen => screen_source_factory().is_some(),
    }
}

/// The RTP caps a video transceiver is pinned to. The three feedback
/// fields put `nack`, `nack pli` and `ccm fir` in the answer, which is
/// what makes the server's keyframe requests legal.
pub(crate) fn vp8_codec_caps() -> gst::Caps {
    gst::Caps::builder("application/x-rtp")
        .field("media", "video")
        .field("encoding-name", "VP8")
        .field("payload", VP8_PAYLOAD_TYPE)
        .field("clock-rate", 90_000i32)
        .field("rtcp-fb-nack", true)
        .field("rtcp-fb-nack-pli", true)
        .field("rtcp-fb-ccm-fir", true)
        .build()
}

fn make(factory: &str) -> Option<gst::Element> {
    gst::ElementFactory::make(factory).build().ok()
}

fn set_if_present(el: &gst::Element, name: &str, value: impl Into<glib::Value>) {
    if el.find_property(name).is_some() {
        el.set_property(name, value.into());
    }
}

/// Turn an appsink sample into a frame, without copying the pixels.
fn frame_from_sample(sample: &gst::Sample) -> Option<VideoFrame> {
    let caps = sample.caps()?;
    let s = caps.structure(0)?;
    let width = u32::try_from(s.get::<i32>("width").ok()?).ok()?;
    let height = u32::try_from(s.get::<i32>("height").ok()?).ok()?;
    if width == 0 || height == 0 {
        return None;
    }
    let buffer = sample.buffer_owned()?;
    let size = buffer.size() as u64;
    // RGBA from videoconvert uses the default stride, a whole number of
    // bytes a row; derive it from the size rather than assuming, and
    // refuse a buffer too small to hold the picture it claims to be.
    let stride = u32::try_from(size / height as u64).ok()?;
    if stride < width * 4 {
        return None;
    }
    let mapped = buffer.into_mapped_buffer_readable().ok()?;
    Some(VideoFrame {
        width,
        height,
        stride,
        bytes: glib::Bytes::from_owned(mapped),
    })
}

/// An appsink that keeps only the newest RGBA picture and hands each to
/// `on_frame` on the streaming thread.
fn make_frame_sink<F>(on_frame: F) -> Option<gst::Element>
where
    F: Fn(VideoFrame) + Send + Sync + 'static,
{
    let caps = gst::Caps::builder("video/x-raw")
        .field("format", "RGBA")
        .field("pixel-aspect-ratio", gst::Fraction::new(1, 1))
        .build();
    // async=false: a sink added to a running pipeline otherwise holds its
    // bin in PAUSED waiting to preroll, and a capture bin's live source
    // won't produce until it reaches PLAYING — each waits on the other
    // until some unrelated state change happens along.
    let sink = gst_app::AppSink::builder()
        .caps(&caps)
        .sync(false)
        .async_(false)
        .max_buffers(1)
        .drop(true)
        .callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |sink| {
                    let sample = sink.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                    if let Some(frame) = frame_from_sample(&sample) {
                        on_frame(frame);
                    }
                    Ok(gst::FlowSuccess::Ok)
                })
                .build(),
        )
        .build();
    Some(sink.upcast())
}

/// Build the receive bin for one remote video stream:
/// `queue ! rtpvp8depay ! vp8dec ! videoconvert ! videoscale ! appsink`.
///
/// The depayloader asks upstream for a keyframe when it has none and
/// after loss, which rtpbin turns into an RTCP PLI; it also discards
/// everything before the first keyframe, so the decoder never shows the
/// grey smear of a stream joined mid-GOP. The leaky queue decouples
/// decoding from the jitterbuffer's thread, so a slow decode drops video
/// frames rather than stalling the bundle.
pub(crate) fn make_receive_bin<F>(name: &str, on_frame: F) -> Option<gst::Bin>
where
    F: Fn(VideoFrame) + Send + Sync + 'static,
{
    let bin = gst::Bin::builder().name(name).build();
    let queue = gst::ElementFactory::make("queue")
        .property("max-size-buffers", 64u32)
        .property("max-size-bytes", 0u32)
        .property("max-size-time", 0u64)
        .property_from_str("leaky", "downstream")
        .build()
        .ok()?;
    let depay = make("rtpvp8depay")?;
    set_if_present(&depay, "request-keyframe", true);
    set_if_present(&depay, "wait-for-keyframe", true);
    let dec = make("vp8dec")?;
    let convert = make("videoconvert")?;
    let scale = make("videoscale")?;
    let sink = make_frame_sink(on_frame)?;
    let elements = [&queue, &depay, &dec, &convert, &scale, &sink];
    bin.add_many(elements).ok()?;
    gst::Element::link_many(elements).ok()?;
    let target = queue.static_pad("sink")?;
    let ghost = gst::GhostPad::with_target(&target).ok()?;
    ghost.set_active(true).ok()?;
    bin.add_pad(&ghost).ok()?;
    Some(bin)
}

/// A bin that swallows a stream: the landing place for a receive pad
/// whose mid this client doesn't recognize. The spec says such a
/// section is mirrored but never mapped to a user or played; linking it
/// to a sink still matters, because an unlinked pad makes rtpbin report
/// not-linked and can take the bundle down with it.
pub(crate) fn make_discard_bin(name: &str) -> Option<gst::Bin> {
    let bin = gst::Bin::builder().name(name).build();
    let sink = gst::ElementFactory::make("fakesink")
        .property("sync", false)
        .property("async", false)
        .build()
        .ok()?;
    bin.add(&sink).ok()?;
    let target = sink.static_pad("sink")?;
    let ghost = gst::GhostPad::with_target(&target).ok()?;
    ghost.set_active(true).ok()?;
    bin.add_pad(&ghost).ok()?;
    Some(bin)
}

/// Test hook: when set, every capture source is a live `videotestsrc`
/// (the value, if a pattern nick like `ball` or `smpte`, picks the
/// pattern). The Tier 3 video media test sets it so a publisher needs
/// no camera. Never set in production.
pub const TEST_SRC_ENV: &str = "GTKHX_VOICE_TEST_VIDEO_SRC";

static CAMERA_PREF: Mutex<Option<String>> = Mutex::new(None);

/// Set the preferred camera by [`Camera::name`]; `None` or empty means
/// the first camera found.
pub fn set_camera_device(name: Option<&str>) {
    if let Ok(mut p) = CAMERA_PREF.lock() {
        *p = name.filter(|s| !s.is_empty()).map(str::to_string);
    }
}

/// The preferred camera, or `None` for the first one found.
pub fn camera_device() -> Option<String> {
    CAMERA_PREF.lock().ok().and_then(|p| p.clone())
}

/// One camera, as the device monitor sees it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Camera {
    /// A key stable across runs, for the settings file: the device's path
    /// where the provider gives one, else its display name.
    pub name: String,
    pub display_name: String,
}

/// The stable key for a device. Not `gst::Device::name()`: that is the
/// GstObject name, which some providers (libcamera) leave NULL.
fn device_key(d: &gst::Device) -> String {
    let props = d.properties();
    for key in [
        "api.v4l2.path",
        "device.path",
        "api.libcamera.path",
        "object.path",
    ] {
        if let Some(v) = props
            .as_ref()
            .and_then(|p| p.get::<String>(key).ok())
            .filter(|v| !v.is_empty())
        {
            return v;
        }
    }
    d.display_name().to_string()
}

/// Whether the last enumeration found a camera: 0 never enumerated,
/// 1 none, 2 some. Every enumeration refreshes it.
static CAMERAS_SEEN: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);

/// Whether this machine has a camera, enumerating only the first time.
/// Opening the camera picker or starting a capture enumerates again.
fn cameras_seen() -> bool {
    use std::sync::atomic::Ordering;
    match CAMERAS_SEEN.load(Ordering::Relaxed) {
        0 => !camera_devices().is_empty(),
        v => v == 2,
    }
}

// ---------------------------------------------------------------------
// Cameras through the Camera portal.
// ---------------------------------------------------------------------

/// Test hook: take the Camera portal path outside a sandbox too, so the
/// flow can be exercised on a host session.
pub const CAMERA_PORTAL_ENV: &str = "GTKHX_CAMERA_PORTAL";

/// Whether cameras have to come through the Camera portal: inside the
/// Flatpak sandbox there is no `/dev/video*`, and the portal's PipeWire
/// remote is the only way to a camera. Everywhere else the device
/// monitor sees the cameras directly.
pub fn camera_via_portal() -> bool {
    cfg!(target_os = "linux")
        && (std::path::Path::new("/.flatpak-info").exists()
            || std::env::var_os(CAMERA_PORTAL_ENV).is_some())
}

/// The PipeWire remote the Camera portal opened, once access is granted.
/// Held until a capture through it fails: `pipewiresrc` and the device
/// provider each connect through a duplicate of it, and the portal
/// remembers the grant, so while it works there is nothing to gain by
/// asking again. A failure may be the remote itself gone dead, with the
/// PipeWire service restarted under it, and nothing through it would
/// work again; see [`forget_camera_remote`].
static CAMERA_REMOTE: Mutex<Option<std::os::fd::OwnedFd>> = Mutex::new(None);

/// Hand over the Camera portal's PipeWire remote.
#[cfg(unix)]
pub fn set_camera_remote(fd: std::os::fd::OwnedFd) {
    if let Ok(mut r) = CAMERA_REMOTE.lock() {
        *r = Some(fd);
    }
    // What the remote shows is a fresh answer to "is there a camera".
    CAMERAS_SEEN.store(0, std::sync::atomic::Ordering::Relaxed);
}

/// Let go of the Camera portal's remote after a camera capture failed, so
/// the next start asks the portal for a fresh one. The grant is
/// remembered, so asking again shows nothing. Off the portal path there
/// is no remote and this does nothing.
pub fn forget_camera_remote() {
    if let Ok(mut r) = CAMERA_REMOTE.lock() {
        *r = None;
    }
}

/// Whether the Camera portal has granted access and opened a remote.
pub fn camera_remote_open() -> bool {
    camera_remote_fd().is_some()
}

#[cfg(unix)]
fn camera_remote_fd() -> Option<i32> {
    use std::os::fd::AsRawFd;
    CAMERA_REMOTE
        .lock()
        .ok()
        .and_then(|r| r.as_ref().map(|fd| fd.as_raw_fd()))
}

#[cfg(not(unix))]
fn camera_remote_fd() -> Option<i32> {
    None
}

/// The portal's `IsCameraPresent`: 0 not asked yet, 1 no, 2 yes.
static CAMERA_PORTAL_PRESENT: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);

/// Record the portal's `IsCameraPresent`.
pub fn set_camera_portal_present(present: bool) {
    CAMERA_PORTAL_PRESENT.store(
        if present { 2 } else { 1 },
        std::sync::atomic::Ordering::Relaxed,
    );
}

/// Until the portal has been asked, assume a camera: the button then
/// offers to try, and the attempt says if there is none.
fn camera_portal_present() -> bool {
    CAMERA_PORTAL_PRESENT.load(std::sync::atomic::Ordering::Relaxed) != 1
}

/// The cameras on the portal's remote, through PipeWire's device
/// provider. Its elements are `pipewiresrc`s that connect through the
/// same fd, so each capture reaches its camera without device access.
fn portal_camera_devices() -> Vec<gst::Device> {
    let Some(fd) = camera_remote_fd() else {
        return Vec::new();
    };
    let Some(provider) =
        gst::DeviceProviderFactory::find("pipewiredeviceprovider").and_then(|f| f.get())
    else {
        return Vec::new();
    };
    if provider.find_property("fd").is_none() {
        return Vec::new();
    }
    provider.set_property("fd", fd);
    if provider.start().is_err() {
        return Vec::new();
    }
    let devices = provider
        .devices()
        .into_iter()
        .filter(|d| d.has_classes("Video/Source"))
        .collect();
    provider.stop();
    devices
}

fn camera_devices() -> Vec<gst::Device> {
    let devices = if camera_via_portal() {
        portal_camera_devices()
    } else {
        let monitor = gst::DeviceMonitor::new();
        monitor.add_filter(Some("Video/Source"), None);
        if monitor.start().is_err() {
            CAMERAS_SEEN.store(1, std::sync::atomic::Ordering::Relaxed);
            return Vec::new();
        }
        let devices = monitor.devices().into_iter().collect::<Vec<_>>();
        monitor.stop();
        devices
    };
    let devices = devices
        .into_iter()
        // PipeWire lists screen-cast nodes as video sources too; a
        // camera picker must not offer somebody's desktop.
        .filter(|d| {
            d.properties()
                .and_then(|p| p.get::<String>("media.role").ok())
                .is_none_or(|role| role != "Screen")
        })
        .collect::<Vec<_>>();
    // The same camera turns up once per provider (v4l2, libcamera,
    // PipeWire); keep the first of each name.
    let mut seen = std::collections::HashSet::new();
    let devices: Vec<_> = devices
        .into_iter()
        .filter(|d| seen.insert(d.display_name().to_string()))
        .collect();
    CAMERAS_SEEN.store(
        if devices.is_empty() { 1 } else { 2 },
        std::sync::atomic::Ordering::Relaxed,
    );
    devices
}

/// The cameras this machine has, for the settings picker.
pub fn list_cameras() -> Vec<Camera> {
    if gst::init().is_err() {
        return Vec::new();
    }
    camera_devices()
        .iter()
        .map(|d| Camera {
            name: device_key(d),
            display_name: d.display_name().to_string(),
        })
        .collect()
}

fn test_source() -> Option<gst::Element> {
    let src = gst::ElementFactory::make("videotestsrc")
        .property("is-live", true)
        .build()
        .ok()?;
    // The hook's value picks the pattern when it names one.
    let pattern = std::env::var(TEST_SRC_ENV).unwrap_or_default();
    if let Some(pspec) = src.find_property("pattern") {
        if let Some(class) = glib::EnumClass::with_type(pspec.value_type()) {
            if let Some(v) = class.value_by_nick(&pattern) {
                src.set_property_from_value("pattern", &v.to_value(&class));
            }
        }
    }
    Some(src)
}

/// The element factory a screen share captures with on this platform,
/// or `None` where there is none. Linux goes through the ScreenCast
/// portal and `pipewiresrc`, which is what works on Wayland, on X11
/// and inside the Flatpak sandbox alike.
fn screen_source_factory() -> Option<&'static str> {
    let candidates: &[&str] = if cfg!(target_os = "linux") {
        &["pipewiresrc"]
    } else if cfg!(target_os = "macos") {
        &["avfvideosrc"]
    } else if cfg!(target_os = "windows") {
        &["d3d11screencapturesrc"]
    } else {
        &[]
    };
    candidates
        .iter()
        .copied()
        .find(|f| gst::ElementFactory::find(f).is_some())
}

/// Where a screen share's pictures come from, once the user has picked
/// what to share. On Linux that is a PipeWire stream the ScreenCast
/// portal opened; elsewhere the platform source captures the main
/// display directly.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ScreenSource {
    /// A portal session's PipeWire remote and the node to read.
    PipeWire { fd: i32, node: u32 },
    /// The platform's own screen capture element, main display.
    Platform,
}

fn make_camera_source() -> Option<gst::Element> {
    if std::env::var_os(TEST_SRC_ENV).is_some() {
        return test_source();
    }
    let wanted = camera_device();
    let devices = camera_devices();
    let pick = match &wanted {
        Some(name) => devices
            .iter()
            .find(|d| device_key(d) == *name)
            .or(devices.first()),
        None => devices.first(),
    };
    if let Some(el) = pick.and_then(|d| d.create_element(None).ok()) {
        return Some(el);
    }
    if camera_via_portal() {
        // No device provider to list with: the remote's default camera.
        // And no remote, no camera — autovideosrc would only find the
        // sandbox's empty /dev.
        let fd = camera_remote_fd()?;
        return gst::ElementFactory::make("pipewiresrc")
            .property("fd", fd)
            .build()
            .ok();
    }
    make("autovideosrc")
}

fn make_screen_source(src: Option<&ScreenSource>) -> Option<gst::Element> {
    if std::env::var_os(TEST_SRC_ENV).is_some() {
        return test_source();
    }
    match src? {
        ScreenSource::PipeWire { fd, node } => {
            let el = gst::ElementFactory::make("pipewiresrc")
                .property("fd", *fd)
                .property("path", node.to_string())
                .build()
                .ok()?;
            // The portal's stream renegotiates its size when the user
            // resizes a shared window; keep the last frame instead of
            // stalling while it does. Older plugins lack the property.
            set_if_present(&el, "keepalive-time", 1000i32);
            Some(el)
        }
        ScreenSource::Platform => {
            let factory = screen_source_factory()?;
            let el = make(factory)?;
            set_if_present(&el, "capture-screen", true);
            set_if_present(&el, "capture-screen-cursor", true);
            set_if_present(&el, "show-cursor", true);
            Some(el)
        }
    }
}

/// Build the capture bin for a publication of `kind`:
///
/// ```text
/// source ! videoconvert ! videoscale ! videorate ! caps(w×h) ! tee
///   tee. ! queue ! vp8enc ! rtpvp8pay ssrc=… ! caps(VP8/96, ssrc) → src
///   tee. ! queue ! videoconvert ! appsink (self-preview)
/// ```
///
/// The payloader's SSRC is set explicitly and repeated in the caps
/// handed to `webrtcbin`, so the `a=ssrc` in the answer is the one on
/// the wire: the spec gives the server no other way to tell a camera
/// from a screen, both being VP8 at PT 96, and it must reject a video
/// send section without one.
///
/// The encoder runs realtime with error resilience and a long keyframe
/// interval: the far side asks for keyframes by PLI when it needs one,
/// and rtpbin turns that into the force-key-unit event `vp8enc`
/// honors, so periodic keyframes would only spend bits.
pub(crate) fn make_send_bin<F>(
    name: &str,
    kind: VideoKind,
    screen: Option<&ScreenSource>,
    target: EncodeTarget,
    rtp: RtpContinuity,
    on_preview: F,
) -> Option<gst::Bin>
where
    F: Fn(VideoFrame) + Send + Sync + 'static,
{
    let bin = gst::Bin::builder().name(name).build();
    let source = match kind {
        VideoKind::Camera => make_camera_source()?,
        VideoKind::Screen => make_screen_source(screen)?,
    };
    let convert = make("videoconvert")?;
    let scale = gst::ElementFactory::make("videoscale")
        .property("add-borders", true)
        .build()
        .ok()?;
    let rate = gst::ElementFactory::make("videorate")
        .property("max-rate", target.fps as i32)
        .build()
        .ok()?;
    // Cap the rate, never make frames up. PipeWire cameras stamp their
    // first buffer far from the segment start, and a videorate free to
    // duplicate fills that gap at the source's rate — hundreds of
    // thousands of copies ahead of the first real frame, which is a
    // camera that takes ages to appear.
    set_if_present(&rate, "drop-only", true);
    set_if_present(&rate, "skip-to-first", true);
    let raw_caps = gst::ElementFactory::make("capsfilter")
        .property(
            "caps",
            gst::Caps::builder("video/x-raw")
                .field("width", target.width as i32)
                .field("height", target.height as i32)
                .field("pixel-aspect-ratio", gst::Fraction::new(1, 1))
                .build(),
        )
        .build()
        .ok()?;
    let tee = make("tee")?;
    let enc_queue = gst::ElementFactory::make("queue")
        .property("max-size-buffers", 2u32)
        .property("max-size-bytes", 0u32)
        .property("max-size-time", 0u64)
        .property_from_str("leaky", "downstream")
        .build()
        .ok()?;
    let enc = make("vp8enc")?;
    enc.set_property("deadline", 1i64);
    enc.set_property("target-bitrate", target.bitrate as i32);
    enc.set_property_from_str("end-usage", "cbr");
    enc.set_property_from_str("error-resilient", "default");
    enc.set_property("keyframe-max-dist", 3000i32);
    enc.set_property(
        "cpu-used",
        match kind {
            VideoKind::Camera => 4i32,
            VideoKind::Screen => 8i32,
        },
    );
    set_if_present(&enc, "threads", 2i32);
    if kind == VideoKind::Screen {
        // A desktop is mostly unchanged from one frame to the next; let
        // the encoder skip the macroblocks that are.
        set_if_present(&enc, "static-threshold", 100i32);
    }
    let pay = gst::ElementFactory::make("rtpvp8pay")
        .name(PAYLOADER_NAME)
        .build()
        .ok()?;
    pay.set_property("pt", VP8_PAYLOAD_TYPE as u32);
    pay.set_property("ssrc", rtp.ssrc);
    pay.set_property("timestamp-offset", rtp.timestamp_base);
    if let Some(seq) = rtp.next_seqnum {
        pay.set_property("seqnum-offset", seq as i32);
    }
    pay.set_property_from_str("picture-id-mode", "15-bit");
    let ssrc = rtp.ssrc;
    let rtp_caps = gst::ElementFactory::make("capsfilter")
        .property(
            "caps",
            gst::Caps::builder("application/x-rtp")
                .field("media", "video")
                .field("encoding-name", "VP8")
                .field("payload", VP8_PAYLOAD_TYPE)
                .field("clock-rate", 90_000i32)
                .field("ssrc", ssrc)
                .build(),
        )
        .build()
        .ok()?;
    let preview_queue = gst::ElementFactory::make("queue")
        .property("max-size-buffers", 1u32)
        .property("max-size-bytes", 0u32)
        .property("max-size-time", 0u64)
        .property_from_str("leaky", "downstream")
        .build()
        .ok()?;
    let preview_convert = make("videoconvert")?;
    let preview_sink = make_frame_sink(on_preview)?;

    bin.add_many([
        &source,
        &convert,
        &scale,
        &rate,
        &raw_caps,
        &tee,
        &enc_queue,
        &enc,
        &pay,
        &rtp_caps,
        &preview_queue,
        &preview_convert,
        &preview_sink,
    ])
    .ok()?;
    gst::Element::link_many([&source, &convert, &scale, &rate, &raw_caps, &tee]).ok()?;
    gst::Element::link_many([&tee, &enc_queue, &enc, &pay, &rtp_caps]).ok()?;
    gst::Element::link_many([&tee, &preview_queue, &preview_convert, &preview_sink]).ok()?;

    let target_pad = rtp_caps.static_pad("src")?;
    let ghost = gst::GhostPad::with_target(&target_pad).ok()?;
    ghost.set_active(true).ok()?;
    bin.add_pad(&ghost).ok()?;
    Some(bin)
}

/// What keeps one publication a single RTP stream across the capture bins
/// built for it. A pause tears the capture down so the camera turns off,
/// and a resume builds a new one; were its payloader to start from a
/// random sequence number and timestamp base on the same SSRC, a
/// receiver's jitterbuffer would see a stream that jumped backwards and
/// discard it. So the SSRC and timestamp base are fixed per publication,
/// and each new payloader carries on from the last one's sequence number.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct RtpContinuity {
    pub ssrc: u32,
    pub timestamp_base: u32,
    /// The sequence number to start at; `None` for the first capture.
    pub next_seqnum: Option<u16>,
}

/// The payloader's element name inside a capture bin, for reading its
/// last sequence number back before the bin is torn down.
pub(crate) const PAYLOADER_NAME: &str = "pay";

/// The name of the capture bin for `kind`. The bus watch matches an
/// error's source against it to tell a failed capture, which ends one
/// publication, from a failure of the call.
pub(crate) fn send_bin_name(kind: VideoKind) -> &'static str {
    match kind {
        VideoKind::Camera => "hxvoice-video-send-camera",
        VideoKind::Screen => "hxvoice-video-send-screen",
    }
}

/// Which capture bin, if any, an element path belongs to.
pub(crate) fn kind_of_send_path(path: &str) -> Option<VideoKind> {
    VideoKind::ALL
        .into_iter()
        .find(|k| path.split('/').any(|seg| seg == send_bin_name(*k)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fit_keeps_aspect_and_even_sizes() {
        assert_eq!(fit(640, 480, 1280, 720), (640, 480));
        assert_eq!(fit(1920, 1080, 1280, 720), (1280, 720));
        assert_eq!(fit(640, 480, 320, 240), (320, 240));
        // A ceiling narrower than the aspect allows: width first, then
        // height, rounded down to even.
        assert_eq!(fit(1920, 1080, 1000, 1000), (1000, 562));
        assert_eq!(fit(640, 480, 0, 0), (2, 2));
    }

    #[test]
    fn targets_never_exceed_the_ceiling() {
        let tight = Limits {
            max_width: 320,
            max_height: 180,
            max_fps: 10,
            max_bitrate: 200_000,
        };
        for kind in VideoKind::ALL {
            let t = tight.target(kind);
            assert!(t.width <= 320 && t.height <= 180, "{kind:?} {t:?}");
            assert!(t.fps <= 10);
            assert!(t.bitrate <= 200_000);
        }
        let cam = Limits::spec_default(VideoKind::Camera).target(VideoKind::Camera);
        assert_eq!((cam.width, cam.height, cam.fps), (640, 480, 30));
        let scr = Limits::spec_default(VideoKind::Screen).target(VideoKind::Screen);
        assert_eq!((scr.width, scr.height, scr.fps), (1920, 1080, 15));
    }

    #[test]
    fn frame_store_keeps_the_newest_and_notifies_once() {
        let store = FrameStore::default();
        let key = StreamKey {
            user_id: 5,
            kind: VideoKind::Camera,
        };
        let frame = |w| VideoFrame {
            width: w,
            height: 1,
            stride: w * 4,
            bytes: glib::Bytes::from_owned(vec![0u8; (w * 4) as usize]),
        };
        assert!(store.put(key, frame(1)), "first frame schedules");
        assert!(!store.put(key, frame(2)), "one notification in flight");
        assert_eq!(store.take(key).map(|f| f.width), Some(2));
        assert!(store.take(key).is_none(), "nothing new since");
        store.clear_pending();
        assert!(store.put(key, frame(3)));
        assert_eq!(store.count(key), 3);
        store.remove(key);
        assert_eq!(store.count(key), 0);
    }

    #[test]
    fn send_paths_map_to_their_kind() {
        assert_eq!(
            kind_of_send_path("/hxvoice-pipeline/hxvoice-video-send-camera/vp8enc0"),
            Some(VideoKind::Camera)
        );
        assert_eq!(
            kind_of_send_path("/hxvoice-pipeline/hxvoice-video-send-screen/pipewiresrc0"),
            Some(VideoKind::Screen)
        );
        assert_eq!(
            kind_of_send_path("/hxvoice-pipeline/hxvoice-webrtcbin"),
            None
        );
    }
}

#[cfg(test)]
mod pipeline_tests {
    use super::*;

    #[test]
    fn send_bin_builds_with_the_test_source() {
        std::env::set_var(TEST_SRC_ENV, "ball");
        gst::init().unwrap();
        let t = Limits::spec_default(VideoKind::Camera).target(VideoKind::Camera);
        let rtp = RtpContinuity {
            ssrc: 1234,
            timestamp_base: 99,
            next_seqnum: Some(7),
        };
        let bin = make_send_bin("t", VideoKind::Camera, None, t, rtp, |_| {}).unwrap();
        let pay = bin.by_name(PAYLOADER_NAME).unwrap();
        assert_eq!(pay.property::<u32>("ssrc"), 1234);
        assert_eq!(pay.property::<u32>("timestamp-offset"), 99);
        assert_eq!(pay.property::<i32>("seqnum-offset"), 7);
    }

    /// Push a capture bin to Playing into a fakesink and count what comes
    /// out of its payloader. Both kinds must produce RTP.
    fn rtp_out_of(kind: VideoKind) -> u64 {
        std::env::set_var(TEST_SRC_ENV, "ball");
        gst::init().unwrap();
        let t = Limits::spec_default(kind).target(kind);
        let rtp = RtpContinuity {
            ssrc: 1,
            timestamp_base: 1,
            next_seqnum: None,
        };
        let bin = make_send_bin("t", kind, None, t, rtp, |_| {}).unwrap();
        let pipeline = gst::Pipeline::new();
        let sink = gst::ElementFactory::make("fakesink")
            .property("sync", false)
            .build()
            .unwrap();
        pipeline
            .add_many([bin.upcast_ref::<gst::Element>(), &sink])
            .unwrap();
        bin.link(&sink).unwrap();
        let count = std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0));
        let c = count.clone();
        sink.static_pad("sink")
            .unwrap()
            .add_probe(gst::PadProbeType::BUFFER, move |_, _| {
                c.fetch_add(1, Ordering::Relaxed);
                gst::PadProbeReturn::Ok
            });
        pipeline.set_state(gst::State::Playing).unwrap();
        std::thread::sleep(std::time::Duration::from_secs(2));
        let bus = pipeline.bus().unwrap();
        while let Some(msg) = bus.pop() {
            if let gst::MessageView::Error(e) = msg.view() {
                panic!("{kind:?} capture error: {} {:?}", e.error(), e.debug());
            }
        }
        pipeline.set_state(gst::State::Null).unwrap();
        count.load(Ordering::Relaxed)
    }

    #[test]
    fn both_kinds_produce_rtp() {
        assert!(rtp_out_of(VideoKind::Camera) > 10);
        assert!(rtp_out_of(VideoKind::Screen) > 10);
    }

    /// The state machine's mid scanner is a no_std copy of hxproto's
    /// parser. They must answer alike, or the machine and the runtime
    /// would disagree about which user a section belongs to.
    #[test]
    fn the_machine_and_hxproto_read_mids_alike() {
        use hxproto::voice::{parse_voice_mid_label, MidLabel};
        use hxvoice::video::{parse_mid, Track};
        let corpus = [
            "send",
            "user-1",
            "user-65535",
            "user-65536",
            "user-0",
            "user-01",
            "user-",
            "cam-send",
            "scr-send",
            "cam-user-7",
            "scr-user-7",
            "cam-user-0",
            "scr-user-099",
            "sca-send",
            "sca-user-3",
            "screen-user-5",
            "0",
            "",
            "send ",
            "user-12x",
            "cam-user-12345678",
            "scr-user-65535",
            "audio",
        ];
        for mid in corpus {
            let theirs = parse_voice_mid_label(mid.as_bytes()).map(|m| match m {
                MidLabel::Send => Track::Mic,
                MidLabel::User(u) => Track::Audio(u),
                MidLabel::CamSend => Track::VideoSend(VideoKind::Camera),
                MidLabel::ScrSend => Track::VideoSend(VideoKind::Screen),
                MidLabel::CamUser(u) => Track::Video(u, VideoKind::Camera),
                MidLabel::ScrUser(u) => Track::Video(u, VideoKind::Screen),
            });
            assert_eq!(parse_mid(mid), theirs, "{mid:?}");
        }
    }
}
