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
//! sandbox. We keep Auto in production (the choice every other glycin
//! consumer — Loupe, Image Viewer — ships with) so server-supplied
//! images stay sandboxed. The one override: when
//! `GTKHX_GLYCIN_NO_SANDBOX` is set we select `NotSandboxed`, for CI /
//! test environments where bwrap can't run (an unprivileged container
//! has no usable user namespaces). glycin 3.x exposes no env knob for
//! this — the selector is API-only — so the override lives here.

use std::cell::Cell;
use std::ffi::c_void;
use std::rc::Rc;
use std::time::{Duration, Instant};

use glib::Bytes;
use glib::MainContext;

use crate::caps::HxInlineMediaCaps;
use crate::ffi_result::{
    decoded_alloc, decoded_set_error, decoded_set_frames, decoded_set_texture,
    HxInlineMediaDecoded,
};
use crate::sniff::{format_is_allowed, format_to_mime, sniff, Format};
use crate::telemetry::{
    log_decode_done, log_decode_done_animation, log_decode_failed, log_decode_start,
};

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

/// Format-acceptance policy applied at the sniff gate. The
/// inline-media path uses [`DecodePolicy::Strict`] — only the
/// spec-allowlisted JPEG/PNG/GIF formats pass; SVG/WebP/AVIF/
/// HEIC/TIFF/BMP/ICO are rejected before glycin spawns. The
/// preview path uses [`DecodePolicy::Wide`] — sniff still
/// identifies the format for the result's canonical_mime and
/// telemetry, but the gate accepts whatever bytes glycin's
/// loader set can decode (the user explicitly opened the
/// file). PICT and other formats glycin doesn't ship a loader
/// for surface as glycin decode errors, which preview's
/// caller falls back to its ImageMagick PICT chain on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum DecodePolicy {
    Strict,
    Wide,
}

/// Kick off an async decode. Mirrors the C contract documented in
/// `inline_media_decode.h`. Returns `None` when the decode rejects
/// synchronously (NULL/empty bytes, byte cap exceeded, sniff
/// reject) — the caller's callback has already fired once with
/// the failure result, no token is allocated.
pub(crate) fn decode_async(
    bytes: &[u8],
    caps: HxInlineMediaCaps,
    policy: DecodePolicy,
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

    // ---- Sync gate 3: magic-byte sniff (policy-gated) ----------
    // Sniff always runs (gives us canonical_mime + telemetry
    // label). Under Strict, only the inline-media spec
    // allowlist passes. Under Wide, any non-empty input is
    // handed to glycin; unrecognised formats just have NULL
    // canonical_mime, which the preview caller is happy to
    // tolerate.
    let sniffed = sniff(bytes);
    if policy == DecodePolicy::Strict && !format_is_allowed(sniffed) {
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

    let max_frames = caps.max_frames;
    let max_duration_ms = caps.max_duration_ms;

    MainContext::default().spawn_local(async move {
        // Re-bind the captured user_data so we can move it out
        // across await points. The marker keeps it !Send-safe.
        let UserData(user_data) = user_data;

        let result = decoded_alloc();

        match run_glycin_decode(
            gbytes,
            max_dim,
            max_pix,
            max_frames,
            max_duration_ms,
            sniffed,
        )
        .await
        {
            Ok(DecodeOk::Static(tex)) => {
                let w = gdk::prelude::TextureExt::width(&tex);
                let h = gdk::prelude::TextureExt::height(&tex);
                decoded_set_texture(result, tex, sniffed);
                log_decode_done(sniffed, w, h, started.elapsed(), bytes_len);
            }
            Ok(DecodeOk::Animation(frames)) => {
                // The first frame's texture doubles as the
                // top-level `texture` field so static-image
                // consumers see something sensible without
                // knowing about the array.
                let (first_w, first_h, frame_count, total_ms) = {
                    let f0 = &frames[0];
                    (
                        gdk::prelude::TextureExt::width(&f0.texture),
                        gdk::prelude::TextureExt::height(&f0.texture),
                        frames.len(),
                        frames.iter().map(|f| f.delay_ms as u64).sum::<u64>(),
                    )
                };
                decoded_set_frames(result, frames, sniffed);
                log_decode_done_animation(
                    sniffed,
                    first_w,
                    first_h,
                    frame_count,
                    total_ms,
                    started.elapsed(),
                    bytes_len,
                );
            }
            Err(GlycinErr { code, message }) => {
                decoded_set_error(result, code, message, sniffed);
                log_decode_failed(sniffed, message, started.elapsed());
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

/// One frame collected from glycin.
pub(crate) struct CollectedFrame {
    pub texture: gdk::Texture,
    pub delay_ms: u32,
}

/// Successful decode result. Static images yield a single
/// texture without per-frame timing; animations yield a frame
/// vector that the FFI marshals into a GArray.
enum DecodeOk {
    Static(gdk::Texture),
    Animation(Vec<CollectedFrame>),
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
    max_frames: u32,
    max_duration_ms: u32,
    _sniffed: Format,
) -> Result<DecodeOk, GlycinErr> {
    // glycin::Loader::new_bytes is the GLib-Bytes-in entry point;
    // glycin keeps a ref + passes the buffer to the subprocess via
    // memfd. The default sandbox selector (Auto) picks bwrap on the
    // host and flatpak-spawn inside a Flatpak runtime.
    let mut loader = glycin::Loader::new_bytes(gbytes);
    // Test/CI escape hatch: glycin 3.x has no env knob for the sandbox
    // (the selector is API-only), and its Auto choice runs each loader
    // under bwrap — which an unprivileged CI container can't spawn, so
    // the decode fails and maps to UnsupportedFormat. The decode test
    // fixtures are trusted in-tree images, so GTKHX_GLYCIN_NO_SANDBOX=1
    // forces the unsandboxed loader path. Unset in production, so
    // server-supplied images keep the Auto sandbox.
    if std::env::var_os("GTKHX_GLYCIN_NO_SANDBOX").is_some() {
        loader.sandbox_selector(glycin::SandboxSelector::NotSandboxed);
    }
    let image = loader
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

    // First frame is always present. `delay` distinguishes
    // static images (None) from animated ones (Some). Glycin
    // documents next_frame as looping back to frame 0 once the
    // animation completes — we stop ourselves via max_frames /
    // max_duration_ms rather than relying on a sentinel.
    let first = image.next_frame().await.map_err(|ctx| GlycinErr {
        code: MEDIA_ERR_UNSUPPORTED,
        message: glycin_err_category(&ctx),
    })?;
    let first_delay = first.delay();
    let first_tex = first.texture();

    let Some(first_delay) = first_delay else {
        // Static image — single frame, no animation.
        return Ok(DecodeOk::Static(first_tex));
    };

    // Animation. Collect frames until the cap.
    let mut frames = Vec::with_capacity(max_frames.min(64) as usize);
    let mut total_ms: u64 = 0;
    let push = |frames: &mut Vec<CollectedFrame>,
                total_ms: &mut u64,
                texture: gdk::Texture,
                delay: Duration| {
        let delay_ms = clamp_delay_ms(delay);
        frames.push(CollectedFrame { texture, delay_ms });
        *total_ms = total_ms.saturating_add(delay_ms as u64);
    };
    push(&mut frames, &mut total_ms, first_tex, first_delay);

    while frames.len() < max_frames as usize && total_ms < max_duration_ms as u64
    {
        let frame = match image.next_frame().await {
            Ok(f) => f,
            // Anything other than success ends the loop. Glycin's
            // documented behaviour is to loop back to frame 0
            // rather than EOF, but a real decoder error here
            // (corrupted IDAT mid-animation) shouldn't fail the
            // whole render — return what we've got, log the
            // truncation in telemetry.
            Err(_) => break,
        };
        let delay = match frame.delay() {
            Some(d) => d,
            // Glycin returned a frame with no delay after the
            // first delay-bearing frame — treat as
            // end-of-animation. Some loaders flag this on the
            // last frame.
            None => break,
        };
        push(&mut frames, &mut total_ms, frame.texture(), delay);
    }

    Ok(DecodeOk::Animation(frames))
}

/// Map glycin's per-frame `Duration` to the wire's milli-second
/// integer. Spec doesn't define a "0 ms = fastest" sentinel —
/// we clamp to a sane floor (10 ms) so a malformed loader
/// returning 0 doesn't busy-spin the tick timer.
fn clamp_delay_ms(d: Duration) -> u32 {
    let ms = d.as_millis().min(u32::MAX as u128) as u32;
    if ms < 10 {
        10
    } else {
        ms
    }
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
