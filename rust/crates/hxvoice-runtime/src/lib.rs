//! GStreamer-based voice runtime for GtkHx.
//!
//! Phase 8.B (per `docs/voice-chat-plan.md` §5.B) is the bare-pipeline
//! skeleton: just enough surface to prove the gstreamer-rs crates are
//! wired, the audio devices are reachable, the build works on the
//! Flatpak runtime, and none of the Hotline protocol code from Phase
//! 8.A has moved. There's no state machine, no `webrtcbin`, no SDP
//! parsing, no audio actually flowing to or from the wire. Those land
//! in Phase 8.C.
//!
//! ## What this crate owns
//!
//! - **`gst::init()`** — exposed to the C side via the
//!   [`gtkhx_voice_init`] FFI entry point. Called once from
//!   `gtkhx.c::main` after `gtk_init`. Order matters only weakly:
//!   GStreamer doesn't care about display init, but landing it after
//!   GTK keeps the "init the world in order" reading honest.
//! - **`audio::DeviceMonitor`** — a thin typed wrapper over
//!   `gst::DeviceMonitor` that surfaces the input devices the
//!   eventual settings panel (Phase 8.E) will list. Factory
//!   functions for the `gst::Element`s the pipeline plugs together
//!   live here too.
//! - **Loopback validation** — a Tier 2 test that builds the
//!   `audiotestsrc ! mulawenc ! mulawdec ! fakesink` pipeline and
//!   walks it to `Playing` state. Catches "gstreamer-rs is here and
//!   the wrapped libs actually work" before Phase 8.C drags in the
//!   webrtcbin surface where the same failure mode would be
//!   buried under SDP / ICE noise.
//!
//! ## What lands in Phase 8.C
//!
//! - The `Runtime` struct that owns a `gst::Pipeline` +
//!   `WebRTCBin` + `hxvoice::SessionMachine` per voice session.
//! - `gst::Promise::new_with_change_func` wiring for
//!   `create-offer` / `create-answer`.
//! - Per-receive-leg `connect_pad_added` handling and the
//!   `mid` → user_id mapping that drives the participant UI.
//! - `gstreamer-webrtc` consumption (`webrtcbin`) — listed as a
//!   Cargo dep here so the resolver pins it now; the actual
//!   `use` statement waits for 8.C.
//!
//! ## FFI discipline
//!
//! Same as the rest of the workspace: hand-declared `extern` blocks
//! on the C side, no cbindgen, signature drift surfaces as an
//! undefined symbol at link time. The C-facing header for Phase
//! 8.B is `src/voice_runtime.h` (separate from `src/voice.h` which
//! is the wire-out path Phase 8.A shipped) so the wire layer and
//! the GStreamer layer can evolve independently.

#![allow(unsafe_op_in_unsafe_fn)]

pub mod audio;
mod ffi;

/// Initialise the GStreamer subsystem.
///
/// Safe to call multiple times — `gst::init` itself is idempotent;
/// the underlying `gst_init_check` checks an internal "already
/// initialised" flag and returns success without re-running plugin
/// scan. We don't gate further here.
///
/// Returns `true` on success. On failure (no plugin path available,
/// missing core plugins, etc.) returns `false`; the C caller decides
/// whether to disable voice UI or fail loudly. The detailed error is
/// logged to stderr via GStreamer's own diagnostics.
pub fn init() -> bool {
    gstreamer::init().is_ok()
}
