//! `hxtext` — text encoding + emoji-shortcode helpers (the Rust port of
//! `src/text_util.c`).
//!
//! Three jobs, all delegating to hotline-proto's **native** primitives (no
//! C-ABI `gtkhx_proto_*` shim, no glib `g_convert`):
//!
//!   * `gtkhx_text_to_utf8` — decode wire bytes to UTF-8 (fast-path
//!     already-valid UTF-8; otherwise Mac Roman via `text::to_utf8`).
//!   * `gtkhx_text_for_wire` — encode UTF-8 for the wire: UTF-8 mode passes
//!     through; legacy mode rewrites emoji to `:shortcode:` text, encodes to
//!     Mac Roman (`text::from_utf8`, `?` fallback), and normalises LF→CR for
//!     body fields.
//!   * the emoji-shortcode toggle (`_set_/_emoji_shortcodes_enabled`).
//!
//! The 4 entry points keep the exact C ABI (returns are `g_malloc`'d so the
//! ~16 C callers `g_free` them unchanged), and the whole thing is pure enough
//! to unit-test under `cargo test` — `test_text_util.c`'s cases moved to the
//! tests module.

use std::cell::Cell;
use std::os::raw::c_char;

/// Hard cap on `gtkhx_text_to_utf8`'s `len` (mirrors
/// `GTKHX_TEXT_TO_UTF8_MAX_LEN` in text_util.h): `(G_MAXSSIZE - 1) / 3`,
/// the tightest of the validate / overflow / isize-slice constraints.
const TO_UTF8_MAX_LEN: usize = (isize::MAX as usize - 1) / 3;

/// Length cap for `gtkhx_text_for_wire`: `(G_MAXSSIZE - 64) / 2`, bounding the
/// legacy shortcode-rewrite buffer + the encode length under isize::MAX. Gates
/// BOTH modes (checked before any `from_raw_parts`, whose own precondition is
/// `len <= isize::MAX`); the legacy `*2` bound is the tightest, and no real
/// message approaches it, so UTF-8 pass-through shares it harmlessly.
const FOR_WIRE_MAX_LEN: usize = (isize::MAX as usize - 64) / 2;

thread_local! {
    /// Emoji ↔ `:shortcode:` toggle (Settings → Chat). Default ON. This is
    /// main-thread-only state (Settings sets it, the send path reads it — both
    /// on the GTK main thread), mirroring the plain `static` the C used; a
    /// `thread_local` keeps that single-thread contract and, as a bonus, lets
    /// the parallel `cargo test` harness toggle it per-test without racing.
    static EMOJI_SHORTCODES: Cell<bool> = const { Cell::new(true) };
}

#[inline]
fn emoji_shortcodes_on() -> bool {
    EMOJI_SHORTCODES.with(|c| c.get())
}

/// Copy `bytes` into a fresh `g_malloc`'d, NUL-terminated buffer (the C ABI's
/// ownership contract — caller `g_free`s). Interior NULs are preserved; the
/// length is reported separately via `out_len`.
unsafe fn g_dup(bytes: &[u8]) -> *mut c_char {
    let n = bytes.len();
    let buf = glib::ffi::g_malloc(n + 1) as *mut u8;
    if n > 0 {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf, n);
    }
    *buf.add(n) = 0;
    buf as *mut c_char
}

/// `void gtkhx_text_set_emoji_shortcodes_enabled(gboolean enabled)`.
#[no_mangle]
pub extern "C" fn gtkhx_text_set_emoji_shortcodes_enabled(enabled: glib::ffi::gboolean) {
    EMOJI_SHORTCODES.with(|c| c.set(enabled != glib::ffi::GFALSE));
}

/// `gboolean gtkhx_text_emoji_shortcodes_enabled(void)`.
#[no_mangle]
pub extern "C" fn gtkhx_text_emoji_shortcodes_enabled() -> glib::ffi::gboolean {
    if emoji_shortcodes_on() {
        glib::ffi::GTRUE
    } else {
        glib::ffi::GFALSE
    }
}

/// `char *gtkhx_text_to_utf8(const char *bytes, gsize len, gsize *out_len)` —
/// decode wire bytes to UTF-8. Already-valid UTF-8 passes through verbatim
/// (embedded NULs preserved); otherwise every byte decodes through the Mac
/// Roman table (always succeeds → always valid UTF-8, never NULL).
///
/// # Safety
/// `bytes` is NULL or valid for `len`; `out_len` is NULL or a valid `gsize *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_text_to_utf8(
    bytes: *const c_char,
    len: usize,
    out_len: *mut usize,
) -> *mut c_char {
    if bytes.is_null() || len > TO_UTF8_MAX_LEN {
        if !out_len.is_null() {
            *out_len = 0;
        }
        return g_dup(b"");
    }
    let input = std::slice::from_raw_parts(bytes as *const u8, len);

    // Fast path: already-valid UTF-8 (the common case on a modern server).
    if std::str::from_utf8(input).is_ok() {
        if !out_len.is_null() {
            *out_len = len;
        }
        return g_dup(input);
    }

    // Slow path: native Mac Roman decode (every byte maps to a codepoint).
    let decoded = hotline_proto::text::to_utf8(input);
    let out = decoded.as_bytes();
    if !out_len.is_null() {
        *out_len = out.len();
    }
    g_dup(out)
}

/// `char *gtkhx_text_for_wire(const char *utf8, gsize utf8_len, gboolean
/// utf8_mode, gboolean is_body, gsize *out_len)` — encode UTF-8 for the wire.
///
/// # Safety
/// `utf8` is NULL or valid for `utf8_len`; `out_len` is NULL or valid.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_text_for_wire(
    utf8: *const c_char,
    utf8_len: usize,
    utf8_mode: glib::ffi::gboolean,
    is_body: glib::ffi::gboolean,
    out_len: *mut usize,
) -> *mut c_char {
    // Pathological-length guard BEFORE constructing any slice: `from_raw_parts`
    // requires `len <= isize::MAX`, so a huge `utf8_len` would be UB even in the
    // UTF-8 pass-through branch. Gate both modes here, up front (also covers the
    // encode-buffer overflow the legacy path would hit). See text_util.h.
    if utf8.is_null() || utf8_len > FOR_WIRE_MAX_LEN {
        if !out_len.is_null() {
            *out_len = 0;
        }
        return g_dup(b"");
    }
    let input = std::slice::from_raw_parts(utf8 as *const u8, utf8_len);

    // UTF-8 negotiated — pass through verbatim (no conversion, no LF→CR).
    if utf8_mode != glib::ffi::GFALSE {
        if !out_len.is_null() {
            *out_len = utf8_len;
        }
        return g_dup(input);
    }

    let text = String::from_utf8_lossy(input);
    // Rewrite emoji → `:shortcode:` so they survive Mac Roman as readable
    // text instead of the `?` fallback (unless the toggle is off / empty).
    let sc: String = if emoji_shortcodes_on() && utf8_len > 0 {
        hotline_proto::emoji::emoji_to_shortcodes(&text)
    } else {
        text.into_owned()
    };

    // Encode the shortcoded UTF-8 to Mac Roman (`?` for out-of-repertoire).
    let mut wire = hotline_proto::text::from_utf8(&sc);

    // Body fields: LF → CR for legacy clients (spec: legacy servers expect
    // CR-terminated lines on the wire).
    if is_body != glib::ffi::GFALSE {
        for b in wire.iter_mut() {
            if *b == 0x0a {
                *b = 0x0d;
            }
        }
    }

    if !out_len.is_null() {
        *out_len = wire.len();
    }
    g_dup(&wire)
}

#[cfg(test)]
mod tests;
