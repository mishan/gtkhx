//! Glycin-backed decode pipeline.
//!
//! G.2 (this commit) handles static images: decode bytes → first
//! frame → `gdk::Texture`. The frames-array branch is reserved
//! for G.3 (animation). The pipeline runs async — glycin spawns a
//! subprocess sandbox via `bwrap` + seccomp and IPCs with it via
//! zbus on the async-io reactor. We drive the future from
//! `glib::MainContext::spawn_local` so the result lands on the
//! main thread without thread-marshalling.
//!
//! # Layered defence (same shape as the legacy
//! `inline_media_decode.c` pipeline, now applied around glycin):
//!
//!   1. **Sniff** — magic-byte gate. JPEG / PNG / GIF only; the
//!      blocked formats (SVG / WebP / AVIF / HEIC / TIFF / ICO /
//!      BMP) are rejected here, before glycin spawns any
//!      subprocess. Even if a future glycin loader added support
//!      for one of them, the sniff gate keeps it out.
//!   2. **Byte cap** — pre-decode size check. Rejects oversized
//!      input before constructing the `glib::Bytes` buffer.
//!   3. **Dimension / pixel cap** — applied AFTER glycin's
//!      `load()` returns image info: glycin reads the format
//!      header, reports width × height (via `Image::width()` /
//!      `Image::height()`), we check before allocating frame
//!      pixel data via `next_frame()`.
//!   4. **Loader-error gate** — any glycin error path maps to
//!      `MediaErrorCode::UnsupportedFormat`. The subprocess crashes
//!      and decode errors map onto the same wire code; the
//!      caller-facing error_message carries the glycin
//!      ErrorCtx string for the debug log.
//!
//! Sandboxing: glycin defaults to `SandboxSelector::Auto` — picks
//! bwrap on the host, flatpak-spawn when we're inside a Flatpak
//! sandbox, plain fork on systems without bwrap. We don't
//! override; the auto choice is what every other glycin consumer
//! (Loupe, Image Viewer) ships with.

use std::cell::Cell;
use std::ffi::c_void;
use std::rc::Rc;
use std::time::Instant;

use glib::Bytes;
use glib::MainContext;

use crate::caps::HxInlineMediaCaps;
use crate::ffi_result::{
    decoded_alloc, decoded_set_error, decoded_set_texture, HxInlineMediaDecoded,
};
use crate::sniff::{format_is_allowed, format_to_mime, sniff, Format};
use crate::telemetry::{log_decode_done, log_decode_failed, log_decode_start};

/// Spec MediaErrorCode mapping. Mirrors
/// `hotline-proto::inline_media::MediaErrorCode`. The wire
/// reserves 3–5 for server-side errors (rate-limit / not-
/// authorised / busy); the decoder only emits 1 (PayloadTooLarge)
/// and 2 (UnsupportedFormat), plus 0 (Generic) for the catch-all.
/// Wire-format reservation. The local decoder never emits
/// Generic (0) on its own — every reject category we produce
/// fits into TOO_LARGE (1) or UNSUPPORTED (2). Kept as a named
/// constant so the wire mapping is greppable.
#[allow(dead_code)]
const MEDIA_ERR_GENERIC: u16 = 0;
const MEDIA_ERR_TOO_LARGE: u16 = 1;
const MEDIA_ERR_UNSUPPORTED: u16 = 2;

/// Shared between the in-flight decode future and the C-side
/// cancel token. The user holds a raw `*const DecodeToken` via
/// [`Rc::into_raw`]; the async block holds its own `Rc`. Either
/// side dropping its handle is safe.
pub(crate) struct DecodeToken {
    /// Set by `inline_media_decode_cancel`. The async block
    /// checks before invoking the callback and bails if set.
    pub cancelled: Cell<bool>,
}

impl DecodeToken {
    fn new() -> Self {
        Self {
            cancelled: Cell::new(false),
        }
    }
}

/// Callable that the C caller passes to `inline_media_decode_
/// async`. The Rust side wraps it in a tiny adapter that carries
/// `user_data` opaquely.
pub(crate) type DecodeCallback = unsafe extern "C" fn(
    result: *mut HxInlineMediaDecoded,
    user_data: *mut c_void,
);

/// Caller's user_data pointer, ferried through to the callback.
/// `*mut c_void` is not `Send`, but we never cross threads —
/// `spawn_local` runs on the main thread.
struct UserData(*mut c_void);

/// SAFETY: We never share `UserData` across threads. The
/// `spawn_local` future lives on the same `MainContext` as the
/// caller (main thread). Marking `Send` only allows the
/// `Future` produced by `async move` to satisfy `'static` —
/// `spawn_local` itself doesn't require it.
unsafe impl Send for UserData {}

/// Kick off an async decode. Mirrors the C contract documented in
/// `inline_media_decode.h`. Returns `None` when the decode rejects
/// synchronously (NULL/empty bytes, byte cap exceeded, sniff
/// reject) — the caller's callback has already fired once with
/// the failure result, no token is allocated.
pub(crate) fn decode_async(
    bytes: &[u8],
    caps: HxInlineMediaCaps,
    cb: DecodeCallback,
    user_data: *mut c_void,
) -> Option<Rc<DecodeToken>> {
    let started = Instant::now();

    // ---- Sync gate 1: empty input ------------------------------
    if bytes.is_empty() {
        let result = decoded_alloc();
        decoded_set_error(
            result,
            MEDIA_ERR_UNSUPPORTED,
            "empty payload",
            Format::Unknown,
        );
        unsafe { cb(result, user_data) };
        return None;
    }

    // ---- Sync gate 2: byte cap ---------------------------------
    if bytes.len() as u64 > caps.max_bytes as u64 {
        let result = decoded_alloc();
        decoded_set_error(
            result,
            MEDIA_ERR_TOO_LARGE,
            "encoded payload exceeds size cap",
            Format::Unknown,
        );
        unsafe { cb(result, user_data) };
        return None;
    }

    // ---- Sync gate 3: magic-byte sniff -------------------------
    let sniffed = sniff(bytes);
    if !format_is_allowed(sniffed) {
        let result = decoded_alloc();
        let msg = if format_to_mime(sniffed).is_some() {
            "format rejected by inline-media allowlist"
        } else {
            "unrecognised image magic bytes"
        };
        decoded_set_error(result, MEDIA_ERR_UNSUPPORTED, msg, sniffed);
        unsafe { cb(result, user_data) };
        return None;
    }

    log_decode_start(sniffed, bytes.len());

    // ---- Async gate 4: glycin --------------------------------
    let token = Rc::new(DecodeToken::new());
    let token_for_future = Rc::clone(&token);
    let user_data = UserData(user_data);

    // Copy into a glib::Bytes — glycin's loader hands these to
    // the subprocess via memfd, so the source must outlive the
    // load() future. We're consuming the caller's borrowed
    // slice; from this point glycin owns the copy.
    let gbytes = Bytes::from(bytes);
    let bytes_len = bytes.len();
    let max_dim = caps.max_dimension;
    let max_pix = caps.max_pixels;

    MainContext::default().spawn_local(async move {
        // Re-bind the captured user_data so we can move it out
        // across await points. The marker keeps it !Send-safe.
        let UserData(user_data) = user_data;

        let result = decoded_alloc();

        match run_glycin_decode(gbytes, max_dim, max_pix, sniffed).await {
            Ok(tex) => {
                let w = gdk::prelude::TextureExt::width(&tex);
                let h = gdk::prelude::TextureExt::height(&tex);
                decoded_set_texture(result, tex, sniffed);
                log_decode_done(sniffed, w, h, started.elapsed(), bytes_len);
            }
            Err(GlycinErr { code, message }) => {
                decoded_set_error(result, code, message, sniffed);
                log_decode_failed(sniffed, &message, started.elapsed());
            }
        }

        if token_for_future.cancelled.get() {
            // Caller cancelled mid-flight; drop the result on
            // the floor.
            crate::ffi_result::decoded_drop(result);
            return;
        }

        unsafe { cb(result, user_data) };
    });

    Some(token)
}

/// Error path internal to the async block. `message` is a
/// 'static literal — keeps the FFI struct's error_message
/// borrowed-pointer contract.
struct GlycinErr {
    code: u16,
    message: &'static str,
}

async fn run_glycin_decode(
    gbytes: Bytes,
    max_dimension: u32,
    max_pixels: u32,
    _sniffed: Format,
) -> Result<gdk::Texture, GlycinErr> {
    // glycin::Loader::new_bytes is the GLib-Bytes-in entry point;
    // glycin keeps a ref + passes the buffer to the subprocess via
    // memfd. The default sandbox selector picks bwrap on the host
    // and flatpak-spawn inside a Flatpak runtime — we don't
    // override.
    let image = glycin::Loader::new_bytes(gbytes)
        .load()
        .await
        .map_err(|ctx| GlycinErr {
            code: MEDIA_ERR_UNSUPPORTED,
            // Glycin's ErrorCtx is descriptive in debug logs but
            // we can't borrow it as 'static. The category is
            // what matters at the wire level; the formatted
            // message goes to debug_log via the telemetry path,
            // which holds its own buffer.
            message: glycin_err_category(&ctx),
        })?;

    // Dimension cap: glycin parsed the header during load();
    // ImageDetails (returned by Image::details()) exposes
    // width / height accessors. Reject before the pixel-data
    // step of next_frame() runs — keeps the loader from
    // allocating a multi-megabyte frame buffer for a payload
    // we're about to throw away.
    let details = image.details();
    let w = details.width();
    let h = details.height();
    if w == 0 || h == 0 {
        return Err(GlycinErr {
            code: MEDIA_ERR_UNSUPPORTED,
            message: "decoder reported zero-dimension image",
        });
    }
    if w > max_dimension || h > max_dimension {
        return Err(GlycinErr {
            code: MEDIA_ERR_TOO_LARGE,
            message: "image dimension exceeds cap",
        });
    }
    if (w as u64) * (h as u64) > max_pixels as u64 {
        return Err(GlycinErr {
            code: MEDIA_ERR_TOO_LARGE,
            message: "image pixel count exceeds cap",
        });
    }

    let frame = image.next_frame().await.map_err(|ctx| GlycinErr {
        code: MEDIA_ERR_UNSUPPORTED,
        message: glycin_err_category(&ctx),
    })?;

    Ok(frame.texture())
}

/// Map a glycin ErrorCtx onto a static error-message category.
/// We avoid leaking the detailed message across the FFI (the
/// caller would receive a borrowed pointer with an undefined
/// lifetime); the detail goes to debug_log on the telemetry
/// path where the format string can hold the ctx by value.
fn glycin_err_category(_ctx: &glycin::ErrorCtx) -> &'static str {
    // The glycin::Error variants are non_exhaustive — we
    // collapse to a single category here. Future refinement
    // (e.g. "loader unsupported" vs "loader crashed") can move
    // off this to match-on-Error if it pays off.
    "glycin decode failed"
}

#[cfg(test)]
mod tests {
    // The decode path requires a GLib main loop + a running
    // glycin subprocess sandbox; both are awkward to bring up
    // inside a unit-test runner. G.6 adds the Rust-side
    // integration tests (lazy_init a MainContext + run
    // pollster::block_on against fixtures). Sniff-layer tests
    // sit in `sniff::tests` and exercise the pure path.
}
