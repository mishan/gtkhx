//! `#[no_mangle] extern "C"` entry points the C side calls.
//!
//! Phase 8.B exposes just one symbol: [`gtkhx_voice_init`], the
//! Rust-side wrapper over `gst::init()`. The C dispatcher in
//! `src/gtkhx.c::main` calls this once after `gtk_init` so the
//! voice subsystem is ready before any window construction reaches
//! a code path that touches voice elements (none today — but Phase
//! 8.D's voice toolbar will, and putting the init call in `main`
//! keeps the lifecycle reading honest).
//!
//! Future phases will add per-session opaque handles
//! (`gtkhx_voice_runtime_new` / `_free` / `_handle_join` /
//! `_handle_leave` / `_handle_mute_toggle` / `_handle_rcv_task`),
//! all of which sit alongside this entry point. The header that
//! mirrors these prototypes is `src/voice_runtime.h`.

/// Initialise the GStreamer subsystem.
///
/// Idempotent — `gst::init` checks an internal "already initialised"
/// flag, so repeat calls are cheap no-ops. Returns `1` (true) on
/// success or `0` (false) on failure (no plugin path configured,
/// missing core plugins, broken GStreamer install). On failure the
/// detailed error goes to stderr via GStreamer's own diagnostics;
/// the C caller decides whether to disable voice UI or fail loudly.
///
/// Returning `int` rather than `bool` because the C header declares
/// it `int` for consistency with the rest of the FFI surface
/// (Hotline-proto's `gtkhx_proto_parse_*` family uses `bool` because
/// stdbool.h is already included; the voice runtime header is
/// deliberately leaner and just uses `int` for the small handful
/// of present-and-future entry points).
///
/// # Safety
/// No memory parameters. Safe to call from any thread, but the
/// expected call site is the main thread during application
/// startup.
#[no_mangle]
pub extern "C" fn gtkhx_voice_init() -> i32 {
    if crate::init() {
        1
    } else {
        0
    }
}
