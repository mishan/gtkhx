//! Sandbox / decode telemetry.
//!
//! Routes through the C-side `debug_log("media", ...)`
//! infrastructure so a single `GTKHX_DEBUG=media` env-var gate
//! controls both the Rust-side decode trace and the existing C
//! consumer-side log lines (download progress, autofetch
//! lifecycle, dialog state transitions).
//!
//! Format mirrors the C `debug_log` shape so a grep across the
//! combined trace reads cleanly. Two events:
//!
//!   `decode-start fmt=<f> bytes=<n>`
//!   `decode-done fmt=<f> bytes=<n> dims=<w>x<h> ms=<d>`
//!
//! plus a `decode-failed` row with the glycin error category
//! (the detailed message isn't 'static so doesn't make it across
//! the FFI; the category is what's actionable).

use std::ffi::CString;
use std::time::Duration;

use crate::sniff::Format;

// Bridge function on the C side, defined in
// `src/inline_media_decode.c`. Threads through the existing
// `debug_log("media", ...)` infrastructure so it inherits the
// `GTKHX_DEBUG=media` env-var gate.
extern "C" {
    fn hx_image_decode_log(msg: *const std::ffi::c_char);
}

fn emit(msg: String) {
    // CString::new fails on interior NUL; our format strings
    // don't produce those, but if they ever did we'd rather drop
    // the log line than panic on a debug call.
    let Ok(cs) = CString::new(msg) else {
        return;
    };
    unsafe { hx_image_decode_log(cs.as_ptr()) };
}

fn fmt_label(f: Format) -> &'static str {
    match f {
        Format::Jpeg => "JPEG",
        Format::Png => "PNG",
        Format::Gif => "GIF",
        Format::Svg => "SVG",
        Format::Webp => "WEBP",
        Format::Avif => "AVIF",
        Format::Heic => "HEIC",
        Format::Tiff => "TIFF",
        Format::Ico => "ICO",
        Format::Bmp => "BMP",
        Format::Unknown => "?",
    }
}

pub(crate) fn log_decode_start(format: Format, encoded_bytes: usize) {
    emit(format!(
        "decode-start fmt={} bytes={}",
        fmt_label(format),
        encoded_bytes
    ));
}

pub(crate) fn log_decode_done(
    format: Format,
    width: i32,
    height: i32,
    elapsed: Duration,
    encoded_bytes: usize,
) {
    emit(format!(
        "decode-done fmt={} bytes={} dims={}x{} ms={}",
        fmt_label(format),
        encoded_bytes,
        width,
        height,
        elapsed.as_millis()
    ));
}

pub(crate) fn log_decode_failed(format: Format, category: &str, elapsed: Duration) {
    emit(format!(
        "decode-failed fmt={} reason={:?} ms={}",
        fmt_label(format),
        category,
        elapsed.as_millis()
    ));
}
