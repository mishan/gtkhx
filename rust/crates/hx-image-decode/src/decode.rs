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

// gtk-rs family + glycin major are version-selected — see crate::compat.
use crate::compat::glib::{Bytes, MainContext};
use crate::compat::gdk;
// glycin is Linux-only; the non-Linux backend below uses the `image` crate.
#[cfg(target_os = "linux")]
use crate::compat::glycin;

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

        match run_decode(
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
            Err(DecodeErr {
                code,
                message,
                detail,
            }) => {
                decoded_set_error(result, code, message, sniffed);
                // Telemetry takes glycin's full error text when present
                // (its "Used config" dump names the loader dirs + API
                // version it searched — the diagnosis for a "no loader
                // for this format" failure); else the static category.
                let reason = detail.as_deref().unwrap_or(message);
                log_decode_failed(sniffed, reason, started.elapsed());
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

/// Outcome handed to [`decode_first_frame_async`]'s closure. Success carries the
/// decoded first (or only) frame's texture; failure carries the spec
/// MediaErrorCode (`1` too-large, `2` unsupported, `0` generic) plus a
/// human-readable reason.
pub enum ImageDecodeOutcome {
    Ok(gdk::Texture),
    Err { code: u16, message: String },
}

/// Cancel handle for an in-flight [`decode_first_frame_async`]. Call
/// [`cancel`](Self::cancel) to suppress the pending closure (e.g. the banner was
/// cleared before the decode landed); the decode future still runs to
/// completion, but drops its outcome + the closure instead of invoking it.
/// Dropping the handle does *not* cancel — hold it for as long as cancellation
/// matters.
pub struct ImageDecodeHandle {
    token: Rc<DecodeToken>,
}

impl ImageDecodeHandle {
    /// Suppress the pending completion closure.
    pub fn cancel(&self) {
        self.token.cancelled.set(true);
    }
}

/// Closure-based, all-Rust sibling of the C-ABI [`inline_media_decode_async`]:
/// decode `bytes` under the inline-media Strict allowlist (JPEG/PNG/GIF) and
/// hand the first frame's [`gdk::Texture`] — or a typed failure — to `on_done`
/// on the GTK main thread. Returns `None` on a synchronous reject (empty /
/// over-cap / disallowed format), in which case `on_done` has already fired once
/// with the failure. No C callback trampoline and no raw result struct: the
/// caller captures its state in the closure and reads a `gdk::Texture` directly.
///
/// The heavy lifting (the glycin sandbox decode + cap enforcement) is shared
/// with [`inline_media_decode_async`] via [`run_decode`]; only the delivery
/// differs (a Rust closure vs. a C callback + `HxInlineMediaDecoded`).
///
/// [`inline_media_decode_async`]: crate::ffi::inline_media_decode_async
pub fn decode_first_frame_async(
    bytes: &[u8],
    caps: HxInlineMediaCaps,
    on_done: impl FnOnce(ImageDecodeOutcome) + 'static,
) -> Option<ImageDecodeHandle> {
    let started = Instant::now();
    // Any zero field falls back to the spec default, same as the C caps path.
    let caps = caps.with_defaults();

    // Sync gates mirror decode_async's Strict path — reject before glycin spawns.
    if bytes.is_empty() {
        on_done(ImageDecodeOutcome::Err {
            code: MEDIA_ERR_UNSUPPORTED,
            message: "empty payload".to_string(),
        });
        return None;
    }
    if bytes.len() as u64 > caps.max_bytes as u64 {
        on_done(ImageDecodeOutcome::Err {
            code: MEDIA_ERR_TOO_LARGE,
            message: "encoded payload exceeds size cap".to_string(),
        });
        return None;
    }
    let sniffed = sniff(bytes);
    if !format_is_allowed(sniffed) {
        let msg = if format_to_mime(sniffed).is_some() {
            "format rejected by inline-media allowlist"
        } else {
            "unrecognised image magic bytes"
        };
        on_done(ImageDecodeOutcome::Err {
            code: MEDIA_ERR_UNSUPPORTED,
            message: msg.to_string(),
        });
        return None;
    }

    log_decode_start(sniffed, bytes.len());

    let token = Rc::new(DecodeToken::new());
    let token_for_future = Rc::clone(&token);

    let gbytes = Bytes::from(bytes);
    let bytes_len = bytes.len();
    let max_dim = caps.max_dimension;
    let max_pix = caps.max_pixels;
    let max_frames = caps.max_frames;
    let max_duration_ms = caps.max_duration_ms;

    MainContext::default().spawn_local(async move {
        let outcome = match run_decode(
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
                log_decode_done(sniffed, w, h, started.elapsed(), bytes_len);
                ImageDecodeOutcome::Ok(tex)
            }
            Ok(DecodeOk::Animation(mut frames)) => {
                // Every current native caller (the banner) renders a single
                // frame — take the first, drop the rest.
                let first = frames.remove(0);
                let w = gdk::prelude::TextureExt::width(&first.texture);
                let h = gdk::prelude::TextureExt::height(&first.texture);
                log_decode_done(sniffed, w, h, started.elapsed(), bytes_len);
                ImageDecodeOutcome::Ok(first.texture)
            }
            Err(DecodeErr {
                code,
                message,
                detail,
            }) => {
                let reason = detail.as_deref().unwrap_or(message);
                log_decode_failed(sniffed, reason, started.elapsed());
                // Hand back glycin's detailed reason when it has one (e.g. its
                // "no loader for this format" text), falling back to the static
                // category — so native callers can surface / re-log something
                // useful, matching this fn's "human-readable reason" contract.
                ImageDecodeOutcome::Err {
                    code,
                    message: detail.unwrap_or_else(|| message.to_string()),
                }
            }
        };

        if token_for_future.cancelled.get() {
            return; // caller cancelled — drop the outcome + closure on the floor
        }
        on_done(outcome);
    });

    Some(ImageDecodeHandle { token })
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
struct DecodeErr {
    code: u16,
    message: &'static str,
    /// Glycin's own error text (its `ErrorCtx` Display), when the
    /// failure came from glycin rather than our own cap checks. Carries
    /// the detail the `'static` `message` can't — including glycin's
    /// "Used config" dump (the loader dirs + API version it searched),
    /// which is exactly what's needed to diagnose a "no loader for this
    /// format" failure. Routed to the telemetry / debug log only; the
    /// wire-facing `message` stays the static category.
    detail: Option<String>,
}

#[cfg(target_os = "linux")]
async fn run_decode(
    gbytes: Bytes,
    max_dimension: u32,
    max_pixels: u32,
    max_frames: u32,
    max_duration_ms: u32,
    _sniffed: Format,
) -> Result<DecodeOk, DecodeErr> {
    // Backend construction differs. glycin 3.x has `Loader::new_bytes`,
    // a GLib-Bytes-in entry point — glycin keeps a ref + passes the
    // buffer to the subprocess via memfd. glycin 2.x has no bytes
    // constructor, so the v2 path stages the payload into a private
    // temp file and hands glycin a `gio::File`; glycin streams the
    // file *content* over a socket to the loader (the sandbox never
    // sees the path), and `_tmp_guard`'s Drop unlinks it once the
    // decode future is done reading. Either way the default sandbox
    // selector (Auto) picks bwrap on the host and flatpak-spawn inside
    // a Flatpak runtime.
    #[cfg(feature = "glycin-v3")]
    let (mut loader, _tmp_guard) = (glycin::Loader::new_bytes(gbytes), ());
    #[cfg(all(target_os = "linux", feature = "glycin-v2"))]
    let (mut loader, _tmp_guard) = {
        let guard = TempImageFile::create(&gbytes).map_err(|e| DecodeErr {
            code: MEDIA_ERR_UNSUPPORTED,
            message: "glycin decode failed",
            detail: Some(format!("temp-file decode staging failed: {e}")),
        })?;
        let loader = glycin::Loader::new(guard.gfile());
        (loader, guard)
    };
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
        .map_err(|ctx| DecodeErr {
            code: MEDIA_ERR_UNSUPPORTED,
            // The category is what matters at the wire level; glycin's
            // full ErrorCtx (descriptive, but not 'static) rides the
            // `detail` field to the telemetry / debug log.
            message: glycin_err_category(&ctx),
            detail: Some(format!("{ctx}")),
        })?;

    // Dimension cap: glycin parsed the header during load(). Reject
    // before the pixel-data step of next_frame() runs — keeps the
    // loader from allocating a multi-megabyte frame buffer for a
    // payload we're about to throw away. The accessor differs by
    // backend: glycin 3.x exposes `Image::details()` (ImageDetails
    // width/height methods), glycin 2.x exposes `Image::info()`
    // (ImageInfo width/height fields).
    #[cfg(feature = "glycin-v3")]
    let (w, h) = {
        let details = image.details();
        (details.width(), details.height())
    };
    #[cfg(all(target_os = "linux", feature = "glycin-v2"))]
    let (w, h) = {
        let info = image.info();
        (info.width, info.height)
    };
    if w == 0 || h == 0 {
        return Err(DecodeErr {
            code: MEDIA_ERR_UNSUPPORTED,
            message: "decoder reported zero-dimension image",
            detail: None,
        });
    }
    if w > max_dimension || h > max_dimension {
        return Err(DecodeErr {
            code: MEDIA_ERR_TOO_LARGE,
            message: "image dimension exceeds cap",
            detail: None,
        });
    }
    if (w as u64) * (h as u64) > max_pixels as u64 {
        return Err(DecodeErr {
            code: MEDIA_ERR_TOO_LARGE,
            message: "image pixel count exceeds cap",
            detail: None,
        });
    }

    // First frame is always present. `delay` distinguishes
    // static images (None) from animated ones (Some). Glycin
    // documents next_frame as looping back to frame 0 once the
    // animation completes — we stop ourselves via max_frames /
    // max_duration_ms rather than relying on a sentinel.
    let first = image.next_frame().await.map_err(|ctx| DecodeErr {
        code: MEDIA_ERR_UNSUPPORTED,
        message: glycin_err_category(&ctx),
        detail: Some(format!("{ctx}")),
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

// ---- Non-Linux backend: pure-Rust `image`-crate decode -------------------
//
// glycin (sandboxed subprocess loaders + memfd IPC) is Linux-only, so off-Linux
// we decode in-process with the `image` crate and build the same gdk::Texture /
// frame vector the FFI expects. CPU-bound but bounded by the caller's byte cap
// (256 KiB default for inline media), so the inline decode on the main-context
// task is a sub-millisecond hit. Format coverage is the pure-Rust codec set
// (JPEG/PNG/GIF + the WIDE-policy preview formats BMP/ICO/TIFF/WebP); animated
// GIF and APNG collect frames, everything else yields one static texture.

#[cfg(not(target_os = "linux"))]
async fn run_decode(
    gbytes: Bytes,
    max_dimension: u32,
    max_pixels: u32,
    max_frames: u32,
    max_duration_ms: u32,
    sniffed: Format,
) -> Result<DecodeOk, DecodeErr> {
    let bytes: &[u8] = &gbytes;

    // Cheap dimension probe for the cap gate before decoding pixel data.
    let (w, h) = image::ImageReader::new(std::io::Cursor::new(bytes))
        .with_guessed_format()
        .map_err(|e| img_err(&e))?
        .into_dimensions()
        .map_err(|e| img_err(&e))?;
    check_dims(w, h, max_dimension, max_pixels)?;

    // Animated GIF / APNG collect frames; everything else is a single texture.
    match sniffed {
        Format::Gif => decode_gif_frames(bytes, max_frames, max_duration_ms),
        Format::Png => decode_png_maybe_apng(bytes, max_frames, max_duration_ms),
        _ => decode_static(bytes),
    }
}

/// Build a gdk::Texture from straight (non-premultiplied) RGBA8 pixels.
#[cfg(not(target_os = "linux"))]
fn texture_from_rgba(w: u32, h: u32, rgba: &[u8]) -> gdk::Texture {
    use crate::compat::glib::prelude::Cast;
    let gbytes = Bytes::from(rgba);
    let stride = (w as usize) * 4;
    gdk::MemoryTexture::new(
        w as i32,
        h as i32,
        gdk::MemoryFormat::R8g8b8a8,
        &gbytes,
        stride,
    )
    .upcast()
}

/// Cap gate shared by the static + frame paths (mirrors the glycin backend's
/// post-`load()` dimension / pixel checks).
#[cfg(not(target_os = "linux"))]
fn check_dims(w: u32, h: u32, max_dimension: u32, max_pixels: u32) -> Result<(), DecodeErr> {
    if w == 0 || h == 0 {
        return Err(DecodeErr {
            code: MEDIA_ERR_UNSUPPORTED,
            message: "decoder reported zero-dimension image",
            detail: None,
        });
    }
    if w > max_dimension || h > max_dimension {
        return Err(DecodeErr {
            code: MEDIA_ERR_TOO_LARGE,
            message: "image dimension exceeds cap",
            detail: None,
        });
    }
    if (w as u64) * (h as u64) > max_pixels as u64 {
        return Err(DecodeErr {
            code: MEDIA_ERR_TOO_LARGE,
            message: "image pixel count exceeds cap",
            detail: None,
        });
    }
    Ok(())
}

/// Map any `image` / IO error onto the wire's UnsupportedFormat category; the
/// `Display` text rides `detail` to the telemetry log, like glycin's ErrorCtx.
#[cfg(not(target_os = "linux"))]
fn img_err(e: &dyn std::fmt::Display) -> DecodeErr {
    DecodeErr {
        code: MEDIA_ERR_UNSUPPORTED,
        message: "image decode failed",
        detail: Some(e.to_string()),
    }
}

/// Decode a single image → one static texture.
#[cfg(not(target_os = "linux"))]
fn decode_static(bytes: &[u8]) -> Result<DecodeOk, DecodeErr> {
    let img = image::load_from_memory(bytes).map_err(|e| img_err(&e))?;
    let rgba = img.to_rgba8();
    let (w, h) = rgba.dimensions();
    if w == 0 || h == 0 {
        return Err(DecodeErr {
            code: MEDIA_ERR_UNSUPPORTED,
            message: "decoder reported zero-dimension image",
            detail: None,
        });
    }
    Ok(DecodeOk::Static(texture_from_rgba(w, h, rgba.as_raw())))
}

#[cfg(not(target_os = "linux"))]
fn decode_gif_frames(
    bytes: &[u8],
    max_frames: u32,
    max_duration_ms: u32,
) -> Result<DecodeOk, DecodeErr> {
    use image::AnimationDecoder;
    let dec = image::codecs::gif::GifDecoder::new(std::io::Cursor::new(bytes))
        .map_err(|e| img_err(&e))?;
    collect_frames(dec.into_frames(), max_frames, max_duration_ms)
}

#[cfg(not(target_os = "linux"))]
fn decode_png_maybe_apng(
    bytes: &[u8],
    max_frames: u32,
    max_duration_ms: u32,
) -> Result<DecodeOk, DecodeErr> {
    use image::AnimationDecoder;
    let dec = image::codecs::png::PngDecoder::new(std::io::Cursor::new(bytes))
        .map_err(|e| img_err(&e))?;
    if dec.is_apng().map_err(|e| img_err(&e))? {
        return collect_frames(
            dec.apng().map_err(|e| img_err(&e))?.into_frames(),
            max_frames,
            max_duration_ms,
        );
    }
    // A non-animated PNG → decode the whole image as one texture.
    decode_static(bytes)
}

/// Collect `image` animation frames into our `CollectedFrame` vec, honoring the
/// frame-count + total-duration caps. A single collected frame collapses to a
/// static texture (matching glycin's "first frame has no delay → static").
#[cfg(not(target_os = "linux"))]
fn collect_frames(
    frames: image::Frames<'_>,
    max_frames: u32,
    max_duration_ms: u32,
) -> Result<DecodeOk, DecodeErr> {
    let mut out: Vec<CollectedFrame> = Vec::new();
    let mut total_ms: u64 = 0;
    for frame in frames {
        let frame = frame.map_err(|e| img_err(&e))?;
        let (numer, denom) = frame.delay().numer_denom_ms();
        let ms = if denom == 0 { 0 } else { numer / denom };
        let delay_ms = clamp_delay_ms(Duration::from_millis(ms as u64));
        let buf = frame.into_buffer();
        let (w, h) = buf.dimensions();
        out.push(CollectedFrame {
            texture: texture_from_rgba(w, h, buf.as_raw()),
            delay_ms,
        });
        total_ms = total_ms.saturating_add(delay_ms as u64);
        if out.len() >= max_frames as usize || total_ms >= max_duration_ms as u64 {
            break;
        }
    }
    match out.len() {
        0 => Err(DecodeErr {
            code: MEDIA_ERR_UNSUPPORTED,
            message: "image decode failed",
            detail: None,
        }),
        // One frame → static, so single-frame GIFs render like plain images.
        1 => Ok(DecodeOk::Static(out.pop().unwrap().texture)),
        _ => Ok(DecodeOk::Animation(out)),
    }
}

/// Map a per-frame `Duration` to the wire's millisecond integer. The spec
/// defines no "0 ms = fastest" sentinel, so clamp to a sane floor (10 ms) so a
/// malformed loader returning 0 doesn't busy-spin the tick timer.
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
#[cfg(target_os = "linux")]
fn glycin_err_category(_ctx: &glycin::ErrorCtx) -> &'static str {
    // The glycin::Error variants are non_exhaustive — we
    // collapse to a single category here. Future refinement
    // (e.g. "loader unsupported" vs "loader crashed") can move
    // off this to match-on-Error if it pays off.
    "glycin decode failed"
}

/// v2-only: glycin 2.x's `Loader` only accepts a `gio::File`, so the
/// in-memory payload is staged to a private temp file for the duration
/// of one decode. glycin reads the file's *content* in our process and
/// streams it to the loader subprocess over a socket — the bwrap/
/// flatpak-spawn sandbox never sees this path (only SVG's
/// `ExposeBaseDir` mounts the directory, and SVG is rejected at the
/// sniff gate long before this). The guard's Drop unlinks the file once
/// the decode future has finished reading it.
#[cfg(all(target_os = "linux", feature = "glycin-v2"))]
struct TempImageFile {
    path: std::path::PathBuf,
}

#[cfg(all(target_os = "linux", feature = "glycin-v2"))]
impl TempImageFile {
    fn create(bytes: &[u8]) -> std::io::Result<Self> {
        use std::io::Write;

        let dir = std::env::temp_dir();
        // The name carries a fresh random UUID (122 bits of entropy)
        // rather than a predictable pid+counter, and `create_new` makes
        // the open atomic. Together that stops another local user on a
        // shared temp dir from pre-creating the path to force decodes to
        // fail; on the astronomically unlikely collision we just retry
        // with new entropy. `0600` keeps the staged bytes unreadable by
        // other users while they exist.
        let mut last_err = None;
        for _ in 0..16 {
            let name = format!(
                "gtkhx-imgdec-{}-{}.bin",
                std::process::id(),
                crate::compat::glib::uuid_string_random()
            );
            let path = dir.join(name);

            let mut opts = std::fs::OpenOptions::new();
            opts.write(true).create_new(true);
            #[cfg(unix)]
            {
                use std::os::unix::fs::OpenOptionsExt;
                opts.mode(0o600);
            }
            match opts.open(&path) {
                Ok(mut f) => {
                    // Bind the guard before the writes so a write/flush
                    // failure still unlinks the file via Drop on the `?`.
                    let me = Self { path };
                    f.write_all(bytes)?;
                    f.flush()?;
                    return Ok(me);
                }
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                    last_err = Some(e);
                    continue;
                }
                Err(e) => return Err(e),
            }
        }
        Err(last_err.unwrap_or_else(|| {
            std::io::Error::new(
                std::io::ErrorKind::AlreadyExists,
                "temp decode-staging name kept colliding",
            )
        }))
    }

    fn gfile(&self) -> crate::compat::gio::File {
        crate::compat::gio::File::for_path(&self.path)
    }
}

#[cfg(all(target_os = "linux", feature = "glycin-v2"))]
impl Drop for TempImageFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

#[cfg(test)]
mod tests {
    // The decode path requires a GLib main loop + a running
    // glycin subprocess sandbox; both are awkward to bring up
    // inside a unit-test runner. G.6 adds the Rust-side
    // integration tests (lazy_init a MainContext + run
    // pollster::block_on against fixtures). Sniff-layer tests
    // sit in `sniff::tests` and exercise the pure path.

    // The v2 temp-file staging helper, on the other hand, is pure
    // filesystem plumbing — exercise it directly so the glycin-v2
    // backend (which the default-feature build never compiles) has
    // unit coverage in the CI v2 leg.
    #[cfg(all(target_os = "linux", feature = "glycin-v2"))]
    #[test]
    fn temp_image_file_round_trips_and_cleans_up() {
        use super::TempImageFile;

        let payload = b"\xFF\xD8\xFF\x00 not really a jpeg, just bytes";
        let guard = TempImageFile::create(payload).expect("staging should succeed");
        let path = guard.path.clone();

        // Exists while the guard is alive, holds exactly what we wrote,
        // and (on unix) is private to us.
        assert!(path.exists(), "staged file should exist");
        assert_eq!(std::fs::read(&path).unwrap(), payload);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(&path).unwrap().permissions().mode();
            assert_eq!(mode & 0o777, 0o600, "staged file must be 0600");
        }

        // The gio::File handle points at the same path. (`path()` is a
        // GFile trait method, so the prelude has to be in scope.)
        use crate::compat::gio::prelude::FileExt;
        assert_eq!(guard.gfile().path().as_deref(), Some(path.as_path()));

        // Drop unlinks it.
        drop(guard);
        assert!(!path.exists(), "Drop should remove the staged file");
    }

    #[cfg(all(target_os = "linux", feature = "glycin-v2"))]
    #[test]
    fn temp_image_file_names_are_unique() {
        use super::TempImageFile;
        let a = TempImageFile::create(b"a").unwrap();
        let b = TempImageFile::create(b"b").unwrap();
        assert_ne!(a.path, b.path, "fresh UUID per staging");
    }
}
