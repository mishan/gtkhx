//! C-ABI mirror of `HxInlineMediaCaps`.
//!
//! Field defaults live in `src/hotline.h::HX_MEDIA_DEFAULT_*`;
//! the FFI shim resolves zero / NULL to those defaults before
//! handing to the decode pipeline so the Rust core can assume
//! every cap is non-zero. The C side documents this in
//! `inline_media_decode.h`.

/// Per-request cap envelope. Mirrors the C
/// `HxInlineMediaCaps` definition byte-for-byte (five `guint32`
/// fields, 20 bytes total). `#[repr(C)]` is load-bearing — the
/// FFI shim passes a `*const HxInlineMediaCaps` directly.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
#[derive(Default)]
pub struct HxInlineMediaCaps {
    pub max_bytes: u32,
    pub max_dimension: u32,
    pub max_pixels: u32,
    pub max_frames: u32,
    pub max_duration_ms: u32,
}

// Spec defaults match HX_MEDIA_DEFAULT_* in src/hotline.h. They
// don't change at the Rust layer's discretion — kept here so the
// FFI shim can fall back when the caller passes NULL.
const SPEC_MAX_BYTES: u32 = 256 * 1024; // 256 KiB encoded
const SPEC_MAX_DIMENSION: u32 = 2048; // each axis
const SPEC_MAX_PIXELS: u32 = 4 * 1024 * 1024; // ~4 megapixels
const SPEC_MAX_FRAMES: u32 = 150;
const SPEC_MAX_DURATION_MS: u32 = 15000;

impl HxInlineMediaCaps {
    /// Spec defaults (used when the caller passes NULL).
    pub const SPEC: Self = Self {
        max_bytes: SPEC_MAX_BYTES,
        max_dimension: SPEC_MAX_DIMENSION,
        max_pixels: SPEC_MAX_PIXELS,
        max_frames: SPEC_MAX_FRAMES,
        max_duration_ms: SPEC_MAX_DURATION_MS,
    };

    /// Replace any zero field with the spec default. Matches the
    /// legacy C resolution logic in `inline_media_decode.c`
    /// before the migration: a caller passing all-zeros gets
    /// the spec floor rather than "no image is ever small
    /// enough."
    pub fn with_defaults(mut self) -> Self {
        if self.max_bytes == 0 {
            self.max_bytes = SPEC_MAX_BYTES;
        }
        if self.max_dimension == 0 {
            self.max_dimension = SPEC_MAX_DIMENSION;
        }
        if self.max_pixels == 0 {
            self.max_pixels = SPEC_MAX_PIXELS;
        }
        if self.max_frames == 0 {
            self.max_frames = SPEC_MAX_FRAMES;
        }
        if self.max_duration_ms == 0 {
            self.max_duration_ms = SPEC_MAX_DURATION_MS;
        }
        self
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_replace_zero_fields() {
        let zero = HxInlineMediaCaps {
            max_bytes: 0,
            max_dimension: 0,
            max_pixels: 0,
            max_frames: 0,
            max_duration_ms: 0,
        };
        let r = zero.with_defaults();
        assert_eq!(r.max_bytes, SPEC_MAX_BYTES);
        assert_eq!(r.max_dimension, SPEC_MAX_DIMENSION);
        assert_eq!(r.max_pixels, SPEC_MAX_PIXELS);
        assert_eq!(r.max_frames, SPEC_MAX_FRAMES);
        assert_eq!(r.max_duration_ms, SPEC_MAX_DURATION_MS);
    }

    #[test]
    fn defaults_leave_nonzero_alone() {
        let custom = HxInlineMediaCaps {
            max_bytes: 1024,
            max_dimension: 256,
            max_pixels: 65536,
            max_frames: 12,
            max_duration_ms: 5000,
        };
        let resolved = custom.with_defaults();
        assert_eq!(resolved.max_bytes, 1024);
        assert_eq!(resolved.max_dimension, 256);
        assert_eq!(resolved.max_pixels, 65536);
        assert_eq!(resolved.max_frames, 12);
        assert_eq!(resolved.max_duration_ms, 5000);
    }
}
