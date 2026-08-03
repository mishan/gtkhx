//! Heap-allocated `HxInlineMediaDecoded` returned to the C
//! callback. Layout matches the C struct in
//! `src/inline_media_decode.h` byte-for-byte; the C-side
//! `_Static_assert(sizeof(HxInlineMediaDecoded) == 40, ...)`
//! pins the size at compile time.
//!
//! Ownership:
//! - Rust allocates via `decoded_alloc()` → returns `*mut
//!   HxInlineMediaDecoded`.
//! - The C caller's callback owns the pointer and MUST call
//!   `inline_media_decoded_free(result)`.
//! - On the success path the texture is a strong ref the caller
//!   inherits (`g_object_unref` on free).
//! - On the failure path texture is NULL and error_message is
//!   either NULL or a `'static` literal — caller doesn't free.

use std::ffi::c_void;
use std::ptr;

// Public gtk-rs family (always the workspace 0.21 line) — see crate::compat.
use crate::compat::glib::translate::IntoGlibPtr;
use crate::compat::{gdk, glib};

use crate::decode::CollectedFrame;
use crate::sniff::Format;

/// C-ABI mirror of `HxInlineMediaDecoded`. Matches the C struct
/// in `src/inline_media_decode.h`. Layout pinned `#[repr(C)]`;
/// the C side carries a `_Static_assert` on `sizeof` so any
/// drift trips at compile time.
#[repr(C)]
pub struct HxInlineMediaDecoded {
    /// Strong ref to a `GdkTexture` on success; NULL on failure.
    pub texture: *mut gdk::ffi::GdkTexture,
    /// `'static` C-string literal; caller doesn't free.
    pub canonical_mime: *const std::ffi::c_char,
    /// One of the `INLINE_MEDIA_FORMAT_*` discriminants. Always
    /// set, even on failure.
    pub sniffed_format: u32,
    /// Spec MediaErrorCode (0..=5). 0 on success.
    pub error_code: u16,
    /// 16-bit pad. The C struct carries an explicit
    /// `guint16 _pad0` matching this; `#[repr(C)]` would lay it
    /// out the same way implicitly, but the explicit field
    /// keeps the C/Rust layouts immediately greppable as
    /// equivalent.
    pub _pad0: u16,
    /// `'static` C-string literal; caller doesn't free. NULL
    /// on success.
    pub error_message: *const std::ffi::c_char,
    /// Animation frames (G.3 reservation). NULL in G.2.
    pub frames: *mut c_void,
}

/// Allocate a zero-initialised result on the heap. The C caller
/// receives the raw pointer and must release it via
/// `inline_media_decoded_free`.
pub(crate) fn decoded_alloc() -> *mut HxInlineMediaDecoded {
    let boxed = Box::new(HxInlineMediaDecoded {
        texture: ptr::null_mut(),
        canonical_mime: ptr::null(),
        sniffed_format: Format::Unknown as u32,
        error_code: 0,
        _pad0: 0,
        error_message: ptr::null(),
        frames: ptr::null_mut(),
    });
    Box::into_raw(boxed)
}

/// Populate a result with a successful decode. The texture is
/// consumed (one ref transferred into the C struct); the caller
/// must `g_object_unref` it when freeing the result.
///
/// `sniffed_format` is captured so failed-decoder paths can
/// still report what the sniff caught — useful for telemetry
/// when a future format pinch-point shifts.
pub(crate) fn decoded_set_texture(
    result: *mut HxInlineMediaDecoded,
    texture: gdk::Texture,
    sniffed: Format,
) {
    debug_assert!(!result.is_null());
    let r = unsafe { &mut *result };
    // `into_glib_ptr` transfers ownership: the GObject ref this
    // call adds is what the C side will eventually
    // `g_object_unref`. No double-ref / no leak.
    let raw = texture.into_glib_ptr();
    r.texture = raw;
    r.sniffed_format = sniffed as u32;
    r.canonical_mime = mime_cstr_for(sniffed);
    r.error_code = 0;
    r.error_message = ptr::null();
}

/// FFI mirror of `HxInlineMediaFrame`. Layout pinned
/// `#[repr(C)]` to match the C struct (`GdkTexture *` +
/// `guint32 delay_ms` + 4-byte pad = 16 bytes on every Linux
/// ABI we ship for).
#[repr(C)]
pub struct HxInlineMediaFrame {
    pub texture: *mut gdk::ffi::GdkTexture,
    pub delay_ms: u32,
    /// Explicit pad so Rust + C agree on the trailing slot.
    /// `#[repr(C)]` would lay this out implicitly on 64-bit
    /// targets, but writing it makes the layouts greppably
    /// equivalent.
    pub _pad0: u32,
}

/// Populate a result with an animation. `frames` is a Vec of
/// glycin-decoded per-frame textures + delays; we marshal them
/// into a `GArray` of `HxInlineMediaFrame` whose clear_func
/// drops each per-element texture ref on free. The first
/// frame's texture is *also* installed at `result->texture` so
/// static-image consumers (callers that don't know about
/// animation yet) keep working as before.
pub(crate) fn decoded_set_frames(
    result: *mut HxInlineMediaDecoded,
    frames: Vec<CollectedFrame>,
    sniffed: Format,
) {
    debug_assert!(!result.is_null());
    debug_assert!(!frames.is_empty(), "animation must have at least one frame");
    let r = unsafe { &mut *result };

    // Build the GArray<HxInlineMediaFrame>. clear_func unrefs
    // each per-frame texture on g_array_free; clear_func is
    // mandatory here because we install raw GdkTexture * via
    // into_glib_ptr (transfer-full into the array).
    let element_size = std::mem::size_of::<HxInlineMediaFrame>() as u32;
    let arr = unsafe {
        glib::ffi::g_array_sized_new(
            glib::ffi::GFALSE,
            glib::ffi::GFALSE,
            element_size,
            frames.len() as u32,
        )
    };
    unsafe {
        glib::ffi::g_array_set_clear_func(arr, Some(frame_clear_cb));
    }

    let mut first_texture_ptr: *mut gdk::ffi::GdkTexture = ptr::null_mut();
    for (i, f) in frames.into_iter().enumerate() {
        let raw_tex = f.texture.into_glib_ptr();
        let elem = HxInlineMediaFrame {
            texture: raw_tex,
            delay_ms: f.delay_ms,
            _pad0: 0,
        };
        unsafe {
            glib::ffi::g_array_append_vals(arr, &elem as *const _ as glib::ffi::gconstpointer, 1);
        }
        if i == 0 {
            // Keep the first frame's texture available at the
            // result's `texture` slot — but with an extra ref,
            // because the array also holds a strong ref and
            // will drop it on its own.
            unsafe {
                glib::gobject_ffi::g_object_ref(raw_tex as *mut _);
            }
            first_texture_ptr = raw_tex;
        }
    }

    r.texture = first_texture_ptr;
    r.frames = arr as *mut c_void;
    r.canonical_mime = mime_cstr_for(sniffed);
    r.sniffed_format = sniffed as u32;
    r.error_code = 0;
    r.error_message = ptr::null();
}

/// GDestroyNotify-shaped callback that the GArray clear_func
/// invokes on each element when the array is freed. Element
/// pointer is a `HxInlineMediaFrame *`; we drop the texture
/// ref and let the GArray free the element storage.
extern "C" fn frame_clear_cb(elem: glib::ffi::gpointer) {
    if elem.is_null() {
        return;
    }
    let frame = unsafe { &mut *(elem as *mut HxInlineMediaFrame) };
    if !frame.texture.is_null() {
        unsafe {
            glib::gobject_ffi::g_object_unref(frame.texture as *mut _);
        }
        frame.texture = ptr::null_mut();
    }
}

/// Populate a result with a failure. The error_message must
/// be a `'static` literal; the C side reads it as a borrowed
/// pointer and won't free.
pub(crate) fn decoded_set_error(
    result: *mut HxInlineMediaDecoded,
    code: u16,
    message: &'static str,
    sniffed: Format,
) {
    debug_assert!(!result.is_null());
    let r = unsafe { &mut *result };
    r.texture = ptr::null_mut();
    r.canonical_mime = mime_cstr_for(sniffed);
    r.sniffed_format = sniffed as u32;
    r.error_code = code;
    r.error_message = static_cstr(message);
}

/// Drop a result allocated by `decoded_alloc`. Releases the
/// texture ref (if any) and frees the box. Safe with NULL.
///
/// This is what `inline_media_decoded_free` from the C side
/// delegates to.
pub(crate) fn decoded_drop(result: *mut HxInlineMediaDecoded) {
    if result.is_null() {
        return;
    }
    let boxed = unsafe { Box::from_raw(result) };
    if !boxed.texture.is_null() {
        unsafe {
            glib::gobject_ffi::g_object_unref(boxed.texture as *mut _);
        }
    }
    // Frame array — g_array_unref with TRUE for the clear_func
    // sweep at the same time it deallocates the buffer. The
    // clear_func unrefs each frame's texture; the array storage
    // gets freed alongside.
    if !boxed.frames.is_null() {
        unsafe {
            glib::ffi::g_array_unref(boxed.frames as *mut glib::ffi::GArray);
        }
    }
    // canonical_mime + error_message point at 'static literals;
    // nothing to free.
    drop(boxed);
}

/// Map a `Format` to its NUL-terminated MIME string. NULL for
/// `Unknown`.
fn mime_cstr_for(f: Format) -> *const std::ffi::c_char {
    let s: &[u8] = match f {
        Format::Jpeg => b"image/jpeg\0",
        Format::Png => b"image/png\0",
        Format::Gif => b"image/gif\0",
        Format::Svg => b"image/svg+xml\0",
        Format::Webp => b"image/webp\0",
        Format::Avif => b"image/avif\0",
        Format::Heic => b"image/heic\0",
        Format::Tiff => b"image/tiff\0",
        Format::Ico => b"image/x-icon\0",
        Format::Bmp => b"image/bmp\0",
        Format::Unknown => return ptr::null(),
    };
    s.as_ptr() as *const std::ffi::c_char
}

/// Convert a Rust `'static str` to a `'static` NUL-terminated C
/// string pointer using a small lookup. All error messages we
/// emit are literal — this gates them through a table so the
/// pointer is stable for the program's lifetime.
fn static_cstr(s: &'static str) -> *const std::ffi::c_char {
    // Compile-time table. New error messages get added here;
    // matching the literal at runtime catches accidental
    // dynamic strings that wouldn't be 'static-safe.
    macro_rules! literal {
        ($lit:literal) => {
            if s == $lit {
                return concat!($lit, "\0").as_ptr() as *const _;
            }
        };
    }
    literal!("empty payload");
    literal!("encoded payload exceeds size cap");
    literal!("format rejected by inline-media allowlist");
    literal!("unrecognised image magic bytes");
    literal!("glycin decode failed");
    literal!("image decode failed");
    literal!("decoder reported zero-dimension image");
    literal!("image dimension exceeds cap");
    literal!("image pixel count exceeds cap");
    // Fallthrough: unknown literal. Returning NULL drops the
    // detail in the C debug log but doesn't UAF. The
    // debug_assert below catches in test builds.
    debug_assert!(false, "static_cstr: unknown literal {:?}", s);
    ptr::null()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alloc_then_drop_no_texture() {
        let r = decoded_alloc();
        decoded_set_error(r, 1, "empty payload", Format::Unknown);
        unsafe {
            assert_eq!((*r).error_code, 1);
            assert!(!(*r).error_message.is_null());
        }
        decoded_drop(r);
    }

    #[test]
    fn drop_is_null_safe() {
        decoded_drop(ptr::null_mut());
    }

    #[test]
    fn static_cstr_recognises_known() {
        let p = static_cstr("empty payload");
        assert!(!p.is_null());
        // Read NUL-terminated.
        let s = unsafe { std::ffi::CStr::from_ptr(p) };
        assert_eq!(s.to_str().unwrap(), "empty payload");
    }
}
